// A heap budget for games that switch Unity's garbage collector off. Unity 2018.3+ lets a game set
// GarbageCollector.GCMode = Disabled and collect only when it chooses (Hollow Knight's GCManager does
// so, collecting at scene changes). On a desktop the heap then just grows; on a 1 GB handheld a long
// stay in one scene runs out of memory (Hollow Knight: Mono's heap 64 -> 192 MB in 28 minutes of
// King's Pass, with 4 collections). While the game has the collector off, the host collects once
// whenever the used heap has grown by the budget since the last collection, the way the game's own
// forced collections do it (mode Enabled, a full collection, mode Disabled again), with Mono's
// collector or IL2CPP's:
//   UA_GC_BUDGET=<MB>   the budget (default 32; 0 = off). Games that keep the collector on, and
//                       players without the GarbageCollector API (before 2018.3), are not affected.
// Each collection is one pause (logged with its length); the game sees its mode unchanged.
//
// A collection before the heap grows again, for 1 GB handhelds without swap (Mono's Boehm collector):
//   UA_GC_GROW=<MB>     (default 0 = off) when Mono's heap has grown during a long frame (a load: UnityMain
//                       inside one frame for over half a second) without a collection since, while the system
//                       has less than <MB> MB available (MemAvailable), a thread of the host runs one full
//                       collection right away. Boehm grows its heap whenever the garbage since its last
//                       collection is small next to the heap (a third of the live data: Mono 2.6's
//                       GC_free_space_divisor, which no setting reaches), and a heap never gives memory back;
//                       collected first, the next growth waits for the memory the game keeps. Each one is a
//                       pause inside the load ("[host] gc grow" with its length, the heap and the used heap
//                       before and after); frames at play speed are never paused by it.
#include "gcbudget.h"
#include "hitches.h"
#include "../loader/so_loader.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*add_icall_fn)(const char *name, const void *fn);
typedef void (*set_mode_fn)(int);

enum { GC_DISABLED = 0, GC_ENABLED = 1 };
static long long g_budget = 32LL << 20;
static add_icall_fn r_mono_add, r_il2cpp_add;
static set_mode_fn r_set_mode;
static int g_mode = GC_ENABLED, g_mode_known;
static void (*m_collect)(int);
static int (*m_max_gen)(void);
static int64_t (*m_used)(void);
static int64_t g_base = -1;                 // used heap after the last collection (-1: not read yet)
static int g_count;

static int g_grow_mb;                       // UA_GC_GROW (0: off)
static volatile double g_frame_t;           // when UnityMain last finished a frame
static int g_grow_started;

void gcbudget_init(void)
{
    const char *e = getenv("UA_GC_BUDGET");
    if (e && *e) g_budget = (long long)atoi(e) << 20;
    e = getenv("UA_GC_GROW");
    if (e && *e) g_grow_mb = atoi(e);
}

static void w_set_mode(int mode)
{
    g_mode = mode; g_mode_known = 1;
    if (mode == GC_DISABLED) g_base = -1;   // measured from the next frame on
    r_set_mode(mode);
}

static int name_is(const char *name, const char *want)
{
    size_t n = strlen(want);
    return !strncmp(name, want, n) && (name[n] == 0 || name[n] == '(');
}

static const void *wrap(const char *name, const void *fn)
{
    if (name && fn && name_is(name, "UnityEngine.Scripting.GarbageCollector::SetMode"))
    { r_set_mode = (set_mode_fn)fn; return (const void *)w_set_mode; }
    return fn;
}

static void w_mono_add(const char *name, const void *fn) { r_mono_add(name, wrap(name, fn)); }
static void w_il2cpp_add(const char *name, const void *fn) { r_il2cpp_add(name, wrap(name, fn)); }

void *gcbudget_dlsym_hook(const char *symbol, void *found)
{
    if (g_budget <= 0 || !found) return found;
    if (!strcmp(symbol, "mono_add_internal_call") && found != (void *)w_mono_add)
    { r_mono_add = (add_icall_fn)found; return (void *)w_mono_add; }
    if (!strcmp(symbol, "il2cpp_add_internal_call") && found != (void *)w_il2cpp_add)
    { r_il2cpp_add = (add_icall_fn)found; return (void *)w_il2cpp_add; }
    return found;
}

static int il2cpp_max_gen(void) { return 0; }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static long mem_available_mb(void)
{
    char line[128]; long kb = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb < 0 ? -1 : kb >> 10;
}

// UA_GC_GROW's thread: polls the heap's size (Boehm's GC_heapsize and GC_gc_no, read without its lock)
static void *grow_main(void *arg)
{
    (void)arg;
    void *h = mono_runtime_lib();
    int64_t (*heap)(void) = (int64_t (*)(void))so_dlsym(h, "mono_gc_get_heap_size");
    int64_t (*used)(void) = (int64_t (*)(void))so_dlsym(h, "mono_gc_get_used_size");
    int (*count)(int) = (int (*)(int))so_dlsym(h, "mono_gc_collection_count");
    void (*collect)(int) = (void (*)(int))so_dlsym(h, "mono_gc_collect");
    int (*max_gen)(void) = (int (*)(void))so_dlsym(h, "mono_gc_max_generation");
    void *(*root)(void) = (void *(*)(void))so_dlsym(h, "mono_get_root_domain");
    void *(*attach)(void *) = (void *(*)(void *))so_dlsym(h, "mono_thread_attach");
    void (*detach)(void *) = (void (*)(void *))so_dlsym(h, "mono_thread_detach");
    if (!heap || !used || !count || !collect || !max_gen || !root || !attach || !detach) {
        fprintf(stderr, "[host] gc grow: this Mono lacks the calls it needs: off\n");
        return NULL;
    }
    fprintf(stderr, "[host] gc grow: on (a collection when the heap grows during a load with under %d MB available)\n", g_grow_mb);
    int64_t last = heap(); int last_n = count(0), logged = 0;
    for (;;) {
        usleep(10000);
        int64_t hs = heap(); int n = count(0);
        int grew = hs > last, collected = n != last_n;
        last = hs; last_n = n;
        if (!grew || collected || now() - g_frame_t < 0.5) continue;
        long avail = mem_available_mb();
        if (avail < 0 || avail >= g_grow_mb) continue;
        void *d = root();
        if (!d) continue;
        void *th = attach(d);
        double t = now();
        int64_t u0 = used();
        collect(max_gen());
        int64_t u1 = used();
        if (th) detach(th);
        if (logged++ < 100)
            fprintf(stderr, "[host] gc grow: heap %lld MB with %ld MB available: collected in %.0f ms, used %lld -> %lld MB (UA_GC_GROW=%d)\n",
                    (long long)(hs >> 20), avail, (now() - t) * 1000, (long long)(u0 >> 20), (long long)(u1 >> 20), g_grow_mb);
        last = heap(); last_n = count(0);
    }
    return NULL;
}

void gcbudget_frame(void *user)
{
    (void)user;
    if (g_grow_mb > 0) {
        g_frame_t = now();
        if (!g_grow_started && so_dlopen("libmono.so", 4 /* RTLD_NOLOAD: Mono 2.6's Boehm only */)) {
            pthread_t t;
            g_grow_started = 1;
            if (pthread_create(&t, NULL, grow_main, NULL) == 0) pthread_detach(t);
        }
    }
    if (g_budget <= 0 || !g_mode_known || g_mode != GC_DISABLED || !r_set_mode) return;
    if (!m_collect) {
        void *h = mono_runtime_lib();
        if (h) {
            m_collect = (void (*)(int))so_dlsym(h, "mono_gc_collect");
            m_max_gen = (int (*)(void))so_dlsym(h, "mono_gc_max_generation");
            m_used = (int64_t (*)(void))so_dlsym(h, "mono_gc_get_used_size");
        } else if ((h = so_dlopen("libil2cpp.so", 4 /* RTLD_NOLOAD */))) {
            // IL2CPP (its Boehm collector): il2cpp_gc_collect collects everything whatever the
            // generation it is given; UnityMain, where this runs, is attached to the runtime
            m_collect = (void (*)(int))so_dlsym(h, "il2cpp_gc_collect");
            m_used = (int64_t (*)(void))so_dlsym(h, "il2cpp_gc_get_used_size");
            m_max_gen = il2cpp_max_gen;
        }
        if (!m_collect || !m_max_gen || !m_used) { m_collect = NULL; g_budget = 0; return; }
    }
    int64_t used = m_used();
    if (g_base < 0) { g_base = used; return; }
    if (used - g_base < g_budget) return;
    double t = now();
    r_set_mode(GC_ENABLED);
    m_collect(m_max_gen());
    r_set_mode(GC_DISABLED);
    int64_t after = m_used();
    if (g_count++ < 50)
        fprintf(stderr, "[host] gc budget: the game keeps the collector off; heap %lld -> %lld MB used, collected in %.0f ms (UA_GC_BUDGET=%lld)\n",
                (long long)(used >> 20), (long long)(after >> 20), (now() - t) * 1000, g_budget >> 20);
    g_base = after;
}
