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
//                       GC_free_space_divisor, which no setting reaches; the Mono route's libmonobdwgc the
//                       same, 16 MB at a time), and a heap never gives memory back;
//                       collected first, the next growth waits for the memory the game keeps. Each one is a
//                       pause inside the load ("[host] gc grow" with its length, the heap and the used heap
//                       before and after); frames at play speed are never paused by it.
//   UA_GC_TIGHT=<n>     (default 0 = off) during a load (UnityMain inside a frame of over 1 s, or within 3 s after a
//                       frame of over 2.5 s or two of over 1 s within 5 s), Boehm's free-space divisor at <n> (GC_set_free_space_divisor: a collection after heap/<n>
//                       bytes allocated, where it would otherwise grow the heap 16 MB at a time); its own value, 3, again
//                       once frames come at play speed, so play keeps the collector's own pace and pauses. The same on
//                       every device (no memory line); at the switch back, if a collection ran meanwhile, one full collection, so that play's
//                       allowance before the next one is computed with the collector's own divisor (Boehm computes it
//                       once per collection: the load's last left play with the load's small one). Collectors that export the setting: the Mono route's libmonobdwgc
//                       (and Unity's Mono 2017+ players); Mono 2.6's has none ("[host] gc tight: off").
//
// A census of the live managed heap (a measurement; Unity's Boehm collector, Mono players and the Mono route):
//   UA_GC_CENSUS=<n>    after each of the first <n> collections of UA_GC_BUDGET, and whenever a file gccensus.req
//                       appears in the game's folder (looked for every 30 frames, removed; then a full collection
//                       first), what the last collection kept: Boehm's reachable objects (its mark bits, read
//                       under its allocation lock) counted by the collector's kind (pointer-free, normal,
//                       uncollectable), by size and, for the objects, by class (the vtable each starts with,
//                       named only when its domain word is the root domain). "[census] ..." lines; nothing is
//                       changed. What a 64-bit build's heap holds against a 32-bit one's (docs/mono-arm64.md).
#define _GNU_SOURCE
#include "gcbudget.h"
#include "hitches.h"
#include "frames.h"
#include "../loader/so_loader.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <sys/uio.h>
#include <sys/syscall.h>

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
static int64_t (*m_heap)(void);             // the collector's heap size (the log line only)
static int64_t g_base = -1;                 // used heap after the last collection (-1: not read yet)
static int g_count;

static int g_grow_mb;                       // UA_GC_GROW (0: off)
static int g_tight;                         // UA_GC_TIGHT: Boehm's free-space divisor under UA_GC_GROW's line (0: off)
static volatile double g_frame_t;           // when UnityMain last finished a frame
static volatile double g_long_t = -1e9;     // when UnityMain last finished a load's frame (see UA_GC_TIGHT)
static volatile double g_long1 = -1e9;      // when UnityMain last finished a frame of over 1 s
static int g_grow_started;
// UA_GC_TIGHT's state, shared by the grow thread and UnityMain (the game's load flag, UA_GC_LOAD_FLAG)
static pthread_mutex_t t_mx = PTHREAD_MUTEX_INITIALIZER;
static void (*t_set_div)(unsigned long);
static unsigned long t_div0;
static int (*t_count)(int);
static void (*t_collect)(int);
static int (*t_max_gen)(void);
static int64_t (*t_heap)(void);
static volatile int t_ready, t_on, t_n, t_logged;
static volatile int g_flag_known, g_flag;   // UA_GC_LOAD_FLAG: the game's own "loading" bool, read each frame
static int g_census_on, g_census_left, g_census_frames;   // UA_GC_CENSUS

void gcbudget_init(void)
{
    const char *e = getenv("UA_GC_BUDGET");
    if (e && *e) g_budget = (long long)atoi(e) << 20;
    e = getenv("UA_GC_GROW");
    if (e && *e) g_grow_mb = atoi(e);
    e = getenv("UA_GC_TIGHT");
    if (e && *e) g_tight = atoi(e);
    e = getenv("UA_GC_CENSUS");
    if (e && *e) { g_census_on = 1; g_census_left = atoi(e); }
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

static void tight_enter(const char *why)
{
    pthread_mutex_lock(&t_mx);
    if (t_ready && !t_on) {
        t_set_div((unsigned long)g_tight); t_on = 1; t_n = t_count(0);
        if (t_logged++ < 200) fprintf(stderr, "[host] gc tight: a load (%s): free-space divisor %lu -> %d (heap %lld MB)\n", why, t_div0, g_tight, (long long)(t_heap() >> 20));
    }
    pthread_mutex_unlock(&t_mx);
}

// back to play's divisor; if a collection ran with the tight one, one more now (Boehm computes the allocation that
// triggers its next collection once per collection, with the divisor of that moment: the load's last one would
// leave play with the load's small allowance, a full collection every ~16 MB in a fight). attach: called off a
// Mono thread (the grow thread); UnityMain is one already
static void tight_exit(const char *why, void *(*root)(void), void *(*attach)(void *), void (*detach)(void *))
{
    pthread_mutex_lock(&t_mx);
    if (t_ready && t_on) {
        t_set_div(t_div0); t_on = 0;
        if (t_count(0) == t_n) {
            if (t_logged++ < 200) fprintf(stderr, "[host] gc tight: play (%s): free-space divisor back to %lu (no collection ran meanwhile)\n", why, t_div0);
        } else {
            static int shot = -1;
            if (shot < 0) { const char *e = getenv("UA_GC_TIGHT_SHOT"); shot = e && *e && strcmp(e, "0"); }
            if (shot) frames_shot_next();       // (a measurement) what the screen shows around this collection
            double tc = now();
            void *d = root ? root() : NULL, *th = d && attach ? attach(d) : NULL;
            if (d || !root) t_collect(t_max_gen());
            if (th) detach(th);
            if (t_logged++ < 200) fprintf(stderr, "[host] gc tight: play (%s): free-space divisor back to %lu (heap %lld MB), a collection to set play's allowance in %.0f ms\n",
                                          why, t_div0, (long long)(t_heap() >> 20), (now() - tc) * 1000);
        }
    }
    pthread_mutex_unlock(&t_mx);
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
    if (g_grow_mb > 0)
        fprintf(stderr, "[host] gc grow: on (a collection when the heap grows during a load with under %d MB available)\n", g_grow_mb);
    // UA_GC_TIGHT: during a load (a frame of over 1 s; 3 s after a frame of over 2.5 s or two of over 1 s) Boehm's free-space
    // divisor at this value: it collects after heap/<n> bytes allocated instead of growing the heap; its own value again
    // once frames come at play speed, so play keeps the collector's own pace
    void (*set_div)(unsigned long) = (void (*)(unsigned long))so_dlsym(h, "GC_set_free_space_divisor");
    unsigned long (*get_div)(void) = (unsigned long (*)(void))so_dlsym(h, "GC_get_free_space_divisor");
    unsigned long div0 = get_div ? get_div() : 0;
    int tick = 0;
    if (g_tight > 0 && (!set_div || !div0)) fprintf(stderr, "[host] gc tight: this collector has no free-space divisor setting: off\n");
    if (g_tight > 0 && set_div && div0) {
        pthread_mutex_lock(&t_mx);
        t_set_div = set_div; t_div0 = div0; t_count = count; t_collect = collect; t_max_gen = max_gen; t_heap = heap; t_ready = 1;
        pthread_mutex_unlock(&t_mx);
    }
    int64_t last = heap(); int last_n = count(0), logged = 0;
    for (;;) {
        usleep(10000);
        if (t_ready && ++tick % 5 == 0) {
            double t = now();
            double in = t - g_frame_t;
            // from the first frame of over 1 s (a floor's load can grow the heap by 100 MB in its first one); then
            // while the game's own load flag is set (UA_GC_LOAD_FLAG: UnityMain enters and leaves at its edges), or
            // without one until 3 s after a load's frame (g_long_t: over 2.5 s, or two of over 1 s within 5 s)
            // (the flag's own edges are UnityMain's: load_flag_frame)
            // with the game's flag known only the flag decides (UnityMain's edges): a long frame in play (the arrival,
            // a stall) must not set the tight divisor, or Boehm computes play's allowance with it (once per collection,
            // at the first check after it) and the fight collects after a few MB (the Keep, G350: 507 ms)
            int loading = g_frame_t > 0 && !g_flag_known && (in > 1.0 || t - g_long_t < 3.0);
            if (loading) tight_enter(in > 1.0 ? "a long frame" : "after a long frame");
            else if (t_on && !(g_flag_known && g_flag)) { tight_exit("frames at play speed", root, attach, detach); last = heap(); last_n = count(0); }
        }
        if (g_grow_mb <= 0) continue;
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


// ---- UA_GC_CENSUS: the live heap by kind, size and class ----
#define CS_SLOTS 65536
struct cs_slot { uintptr_t vt; uint32_t kind, n; uint64_t bytes; };
static struct cs_slot *g_cs;
static uint64_t g_cs_kind_n[8], g_cs_kind_b[8], g_cs_size_n[8][16], g_cs_size_b[8][16], g_cs_full;
static int (*c_kind)(const void *, size_t *);
// the strings by value: how many live strings hold the same characters (one entry per value, its first 40 characters)
#define ST_SLOTS 131072
struct st_slot { uint32_t h, n, len; uint64_t bytes; char text[40]; };
static struct st_slot *g_st;
static uintptr_t g_string_vt;
static uint64_t g_st_full;

static int cs_bucket(size_t b)
{
    static const size_t lim[] = { 16, 24, 32, 48, 64, 96, 128, 256, 512, 1024, 4096, 16384, 65536, 262144, 1048576 };
    for (int i = 0; i < 15; i++) if (b <= lim[i]) return i;
    return 15;
}

static void cs_proc(void *obj, size_t bytes, void *ud)
{
    (void)ud;
    size_t sz; int k = c_kind(obj, &sz);
    if (k < 0 || k > 7) k = 7;
    g_cs_kind_n[k]++; g_cs_kind_b[k] += bytes;
    int b = cs_bucket(bytes); g_cs_size_n[k][b]++; g_cs_size_b[k][b] += bytes;
    uintptr_t vt = *(uintptr_t *)obj & ~(uintptr_t)3;
    if (vt == g_string_vt && g_st && bytes >= 2 * sizeof(void *) + 4) {
        int32_t len = *(int32_t *)((char *)obj + 2 * sizeof(void *));
        const uint16_t *c = (const uint16_t *)((char *)obj + 2 * sizeof(void *) + 4);
        if (len >= 0 && (size_t)len * 2 + 2 * sizeof(void *) + 4 <= bytes) {
            uint32_t hh = 2166136261u ^ (uint32_t)len;
            for (int32_t i = 0; i < len; i++) hh = (hh ^ c[i]) * 16777619u;
            if (!hh) hh = 1;
            uint32_t j = hh & (ST_SLOTS - 1);
            for (int t = 0; t < 32; t++, j = (j + 1) & (ST_SLOTS - 1)) {
                struct st_slot *e = &g_st[j];
                if (e->h == hh && e->len == (uint32_t)len) { e->n++; e->bytes += bytes; break; }
                if (!e->h) {
                    e->h = hh; e->len = len; e->n = 1; e->bytes = bytes;
                    for (int32_t i = 0; i < len && i < 39; i++) e->text[i] = c[i] >= 32 && c[i] < 127 ? (char)c[i] : '?';
                    break;
                }
                if (t == 31) g_st_full += bytes;
            }
        }
    }
    uint32_t h = (uint32_t)((vt >> 3) * 2654435761u) & (CS_SLOTS - 1);
    for (int i = 0; i < 64; i++, h = (h + 1) & (CS_SLOTS - 1)) {
        struct cs_slot *e = &g_cs[h];
        if (e->vt == vt && e->kind == (uint32_t)k) { e->n++; e->bytes += bytes; return; }
        if (!e->vt && !e->n) { e->vt = vt; e->kind = k; e->n = 1; e->bytes = bytes; return; }
    }
    g_cs_full += bytes;
}

static void *cs_walk(void *fn) { ((void (*)(void (*)(void *, size_t, void *), void *))fn)(cs_proc, NULL); return NULL; }

static void *cs_sym(const char *n)
{
    void *h = mono_runtime_lib(), *p = h ? so_dlsym(h, n) : NULL;
    return p ? p : dlsym(RTLD_DEFAULT, n);
}

// a read that cannot fault (the vtable word of a block that holds no object)
static int cs_read(const void *p, void *out, size_t n)
{
    struct iovec l = { out, n }, r = { (void *)p, n };
    return syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == (long)n;
}

static int st_cmp(const void *a, const void *b)
{
    const struct st_slot *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static int cs_cmp(const void *a, const void *b)
{
    const struct cs_slot *x = a, *y = b;
    return x->bytes < y->bytes ? 1 : x->bytes > y->bytes ? -1 : 0;
}

// Mono's own counters of its memory (mono_counters_foreach): the ones in bytes, and the mempools' and tables' sizes
static const char *(*k_name)(void *);
static int (*k_type)(void *), (*k_sample)(void *, void *, int);
static int cs_counter(void *c, void *ud)
{
    (void)ud;
    const char *n = k_name(c);
    int t = k_type(c);
    if (!n || ((t & 0x0f000000) != 0x01000000 && !strstr(n, "size") && !strstr(n, "ytes"))) return 1;
    union { int32_t i; int64_t l; double d; char pad[64]; } v; memset(&v, 0, sizeof v);
    int sz = k_sample(c, &v, sizeof v);
    long long x = (t & 0xf) == 2 ? (sizeof(void *) == 8 ? v.l : v.i) : (t & 0xf) >= 3 && (t & 0xf) <= 4 ? v.l : (t & 0xf) <= 1 ? v.i : -1;
    if (sz > 0 && x > 0) fprintf(stderr, "[census] mono counter %s: %lld (%.2f MB)\n", n, x, x / 1048576.0);
    return 1;
}

static void census(const char *why)
{
    void *(*with_lock)(void *(*)(void *), void *) = cs_sym("GC_call_with_alloc_lock");
    void *walk = cs_sym("GC_enumerate_reachable_objects_inner");
    c_kind = cs_sym("GC_get_kind_and_size");
    void *(*root)(void) = cs_sym("mono_get_root_domain");
    const char *(*cname)(void *) = cs_sym("mono_class_get_name");
    const char *(*cns)(void *) = cs_sym("mono_class_get_namespace");
    int64_t (*heap)(void) = cs_sym("mono_gc_get_heap_size");
    if (!heap) heap = cs_sym("il2cpp_gc_get_heap_size");
    if (!with_lock || !walk || !c_kind || !root || !cname || !cns) {
        fprintf(stderr, "[census] this collector lacks the calls it needs (GC_enumerate_reachable_objects_inner ...): off\n");
        g_census_on = g_census_left = 0; return;
    }
    if (!g_cs && !(g_cs = calloc(CS_SLOTS, sizeof *g_cs))) return;
    void *(*strclass)(void) = cs_sym("mono_get_string_class");
    void *(*vtable)(void *, void *) = cs_sym("mono_class_vtable");
    g_string_vt = strclass && vtable ? (uintptr_t)vtable(root(), strclass()) : 0;
    if (g_string_vt && !g_st) g_st = calloc(ST_SLOTS, sizeof *g_st);
    if (g_st) memset(g_st, 0, ST_SLOTS * sizeof *g_st);
    g_st_full = 0;
    memset(g_cs, 0, CS_SLOTS * sizeof *g_cs);
    memset(g_cs_kind_n, 0, sizeof g_cs_kind_n); memset(g_cs_kind_b, 0, sizeof g_cs_kind_b);
    memset(g_cs_size_n, 0, sizeof g_cs_size_n); memset(g_cs_size_b, 0, sizeof g_cs_size_b); g_cs_full = 0;
    double t = now();
    with_lock(cs_walk, walk);
    double ms = (now() - t) * 1000;
    uint64_t tn = 0, tb = 0;
    for (int k = 0; k < 8; k++) { tn += g_cs_kind_n[k]; tb += g_cs_kind_b[k]; }
    fprintf(stderr, "[census] %s: %llu reachable objects, %.2f MB (heap %.1f MB), walked in %.0f ms, %d-bit\n", why,
            (unsigned long long)tn, tb / 1048576.0, heap ? heap() / 1048576.0 : 0, ms, (int)sizeof(void *) * 8);
    static const char *kn[] = { "pointer-free", "normal", "uncollectable", "atomic-uncollectable", "kind 4", "kind 5", "kind 6", "other" };
    static const char *bn[] = { "<=16", "<=24", "<=32", "<=48", "<=64", "<=96", "<=128", "<=256", "<=512", "<=1K", "<=4K", "<=16K", "<=64K", "<=256K", "<=1M", ">1M" };
    for (int k = 0; k < 8; k++) {
        if (!g_cs_kind_n[k]) continue;
        char line[1024]; int o = 0;
        for (int b = 0; b < 16; b++)
            if (g_cs_size_n[k][b]) o += snprintf(line + o, sizeof line - o, " %s:%llu/%.2fMB", bn[b], (unsigned long long)g_cs_size_n[k][b], g_cs_size_b[k][b] / 1048576.0);
        fprintf(stderr, "[census] kind %s: %llu objects, %.2f MB;%s\n", kn[k], (unsigned long long)g_cs_kind_n[k], g_cs_kind_b[k] / 1048576.0, line);
    }
    if (g_cs_full) fprintf(stderr, "[census] (%.2f MB not counted by class: table full)\n", g_cs_full / 1048576.0);
    // by class: merge the slots whose word is a vtable of the root domain, by vtable
    static const char *kind_tag[] = { " pointer-free", "", "", "", " kind 4", " kind 5", " kind 6", " other kind" };
    void *dom = root();
    int m = 0;
    for (int i = 0; i < CS_SLOTS; i++) if (g_cs[i].n) g_cs[m++] = g_cs[i];
    qsort(g_cs, m, sizeof *g_cs, cs_cmp);
    uint64_t named = 0, unnamed = 0, shown = 0;
    int lines = 0;
    for (int i = 0; i < m; i++) {
        struct cs_slot *e = &g_cs[i];
        uintptr_t w[3];
        if (e->kind == 2 || e->kind == 3 || !e->vt || !cs_read((void *)e->vt, w, sizeof w) || (void *)w[2] != dom || !w[0]) { unnamed += e->bytes; continue; }
        named += e->bytes;
        if (lines < 60 || e->bytes >= (256u << 10)) {
            uintptr_t k2; if (!cs_read((void *)w[0], &k2, sizeof k2)) continue;
            const char *ns = cns((void *)w[0]), *nm = cname((void *)w[0]);
            fprintf(stderr, "[census] class %s%s%s: %u objects, %.3f MB (%.0f bytes each)%s\n", ns && *ns ? ns : "", ns && *ns ? "." : "",
                    nm ? nm : "?", e->n, e->bytes / 1048576.0, (double)e->bytes / e->n, kind_tag[e->kind & 7]);
            shown += e->bytes; lines++;
        }
    }
    int (*each)(int (*)(void *, void *), void *) = cs_sym("mono_counters_foreach");
    k_name = cs_sym("mono_counter_get_name"); k_type = cs_sym("mono_counter_get_type"); k_sample = cs_sym("mono_counters_sample");
    if (each && k_name && k_type && k_sample) each(cs_counter, NULL);
    if (g_st) {
        int sm = 0; uint64_t sn = 0, sb = 0, dn = 0, db = 0;
        for (int i = 0; i < ST_SLOTS; i++) if (g_st[i].n) {
            sn += g_st[i].n; sb += g_st[i].bytes;
            if (g_st[i].n > 1) { dn += g_st[i].n - 1; db += g_st[i].bytes - g_st[i].bytes / g_st[i].n; }
            g_st[sm++] = g_st[i];
        }
        qsort(g_st, sm, sizeof *g_st, st_cmp);
        fprintf(stderr, "[census] strings: %llu, %.2f MB, %d values; %llu copies of a value another live string holds, %.2f MB%s\n",
                (unsigned long long)sn, sb / 1048576.0, sm, (unsigned long long)dn, db / 1048576.0, g_st_full ? " (table full: some not counted)" : "");
        for (int i = 0; i < sm && i < 40; i++)
            fprintf(stderr, "[census] string x%u (%u chars, %.3f MB): \"%.40s\"\n", g_st[i].n, g_st[i].len, g_st[i].bytes / 1048576.0, g_st[i].text);
    }
    fprintf(stderr, "[census] objects of the root domain %.2f MB (%.2f MB shown), the rest (other words, uncollectable) %.2f MB\n",
            named / 1048576.0, shown / 1048576.0, unnamed / 1048576.0);
}

static int resolve_collector(void)
{
    if (m_collect) return 1;
    {
        void *h = mono_runtime_lib();
        if (h) {
            m_collect = (void (*)(int))so_dlsym(h, "mono_gc_collect");
            m_max_gen = (int (*)(void))so_dlsym(h, "mono_gc_max_generation");
            m_used = (int64_t (*)(void))so_dlsym(h, "mono_gc_get_used_size");
            m_heap = (int64_t (*)(void))so_dlsym(h, "mono_gc_get_heap_size");
        } else if ((h = so_dlopen("libil2cpp.so", 4 /* RTLD_NOLOAD */))) {
            // IL2CPP (its Boehm collector): il2cpp_gc_collect collects everything whatever the
            // generation it is given; UnityMain, where this runs, is attached to the runtime
            m_collect = (void (*)(int))so_dlsym(h, "il2cpp_gc_collect");
            m_used = (int64_t (*)(void))so_dlsym(h, "il2cpp_gc_get_used_size");
            m_heap = (int64_t (*)(void))so_dlsym(h, "il2cpp_gc_get_heap_size");
            m_max_gen = il2cpp_max_gen;
        }
        if (!m_collect || !m_max_gen || !m_used) { m_collect = NULL; return 0; }
    }
    return 1;
}

static void census_frame(void)
{
    if (!g_census_on || ++g_census_frames % 30) return;
    if (access("gccensus.req", F_OK) != 0) return;
    unlink("gccensus.req");
    if (!m_collect) return;
    int off = g_mode_known && g_mode == GC_DISABLED && r_set_mode;
    if (off) r_set_mode(GC_ENABLED);
    m_collect(m_max_gen());
    if (off) r_set_mode(GC_DISABLED);
    if (m_used) g_base = m_used();
    census("gccensus.req");
}

// UA_GC_LOAD_FLAG=<assembly>:<[namespace.]class>:<static field holding the instance>:<bool field of it> (with UA_GC_TIGHT;
// Enter the Gungeon: Assembly-CSharp:GameManager:mr_manager:m_loadingLevel, its IsLoadingLevel): the game's own "a level
// is loading" bool, read at the end of every frame on UnityMain (the static field from its class's static data, which
// mono_class_vtable makes without running the class's static constructor: nothing of the game runs or changes). Tight
// from the frame it turns true; the frame it turns false (Gungeon: the load's fade is black, the level not yet shown)
// play's divisor again and, if a collection ran meanwhile, play's collection there and then, on UnityMain
static struct { void *static_data; uint32_t s_off, i_off; volatile unsigned char *end_flag; } g_lf;
static int g_lf_state;        // 0: not resolved yet, 1: resolved, -1: off
static void load_flag_frame(void)
{
    static const char *spec; static int tries;
    if (g_lf_state < 0 || g_tight <= 0) return;
    if (g_lf_state == 0) {
        if (!spec) { spec = getenv("UA_GC_LOAD_FLAG"); if (!spec || !*spec) { g_lf_state = -1; return; } }
        if (++tries % 60) return;
        char a[128], c[128], sf[128], inf[128];
        if (sscanf(spec, "%127[^:]:%127[^:]:%127[^:]:%127s", a, c, sf, inf) != 4) { fprintf(stderr, "[host] gc tight: UA_GC_LOAD_FLAG=%s: not <assembly>:<class>:<static field>:<field>\n", spec); g_lf_state = -1; return; }
        void *h = mono_runtime_lib();
        if (!h) return;
        void *(*img_loaded)(const char *) = so_dlsym(h, "mono_image_loaded");
        void *(*from_name)(void *, const char *, const char *) = so_dlsym(h, "mono_class_from_name");
        void *(*field)(void *, const char *) = so_dlsym(h, "mono_class_get_field_from_name");
        uint32_t (*offset)(void *) = so_dlsym(h, "mono_field_get_offset");
        void *(*vtable)(void *, void *) = so_dlsym(h, "mono_class_vtable");
        void *(*sdata)(void *) = so_dlsym(h, "mono_vtable_get_static_field_data");
        void *(*root)(void) = so_dlsym(h, "mono_get_root_domain");
        if (!img_loaded || !from_name || !field || !offset || !vtable || !sdata || !root) {
            fprintf(stderr, "[host] gc tight: this Mono lacks the calls UA_GC_LOAD_FLAG needs: the frame-time rule only\n"); g_lf_state = -1; return;
        }
        void *img = img_loaded(a);
        if (!img || !root()) { if (tries > 60 * 600) g_lf_state = -1; return; }   // not loaded yet (gives up after ~10 min)
        char ns[128] = "", *dot = strrchr(c, '.'), *cn = c;
        if (dot) { *dot = 0; snprintf(ns, sizeof ns, "%s", c); cn = dot + 1; }
        void *k = from_name(img, ns, cn), *fs = k ? field(k, sf) : NULL, *fi = k ? field(k, inf) : NULL;
        void *vt = fs && fi ? vtable(root(), k) : NULL, *data = vt ? sdata(vt) : NULL;
        if (!data) { fprintf(stderr, "[host] gc tight: UA_GC_LOAD_FLAG=%s: not found: the frame-time rule only\n", spec); g_lf_state = -1; return; }
        g_lf.static_data = data; g_lf.s_off = offset(fs); g_lf.i_off = offset(fi);
        g_lf_state = 1; g_flag_known = 1;
        fprintf(stderr, "[host] gc tight: the game's load flag %s (static +%u, field +%u)\n", spec, g_lf.s_off, g_lf.i_off);
        // UA_GC_LOAD_END=<assembly>:<class>:<static bool field>: set by the game (Gungeon: gungeonpatch's load-black,
        // GameManager.UaLoadBlack) on a frame whose picture is the load's black fade: play's collection there
        const char *ee = getenv("UA_GC_LOAD_END");
        char ea[128], ec[128], ef[128];
        if (ee && *ee && sscanf(ee, "%127[^:]:%127[^:]:%127s", ea, ec, ef) == 3) {
            void *eimg = img_loaded(ea);
            char ens[128] = "", *edot = strrchr(ec, '.'), *ecn = ec;
            if (edot) { *edot = 0; snprintf(ens, sizeof ens, "%s", ec); ecn = edot + 1; }
            void *ek = eimg ? from_name(eimg, ens, ecn) : NULL, *ff = ek ? field(ek, ef) : NULL;
            void *evt = ff ? vtable(root(), ek) : NULL, *edata = evt ? sdata(evt) : NULL;
            if (edata) { g_lf.end_flag = (volatile unsigned char *)edata + offset(ff); fprintf(stderr, "[host] gc tight: the load's black frame %s\n", ee); }
            else fprintf(stderr, "[host] gc tight: UA_GC_LOAD_END=%s: not found (an unpatched game): the load flag's end only\n", ee);
        }
    }
    if (g_lf.end_flag && *g_lf.end_flag) { *g_lf.end_flag = 0; tight_exit("the load's fade is black", NULL, NULL, NULL); }
    void *obj = *(void **)((char *)g_lf.static_data + g_lf.s_off);
    int f = obj ? *((volatile unsigned char *)obj + g_lf.i_off) != 0 : 0;
    if (f && !g_flag) tight_enter("the game's load flag");
    else if (!f && g_flag) tight_exit("the game's load ended", NULL, NULL, NULL);
    g_flag = f;
}

// UA_STALL_TRACE=<ms> (a measurement): a thread samples UnityMain every 20 ms (/proc/self/task/<tid>/stat, wchan, io);
// once a frame has lasted <ms>, until it ends, and then logs one line: the samples' states (R running, S sleeping,
// D waiting on the disk), the kernel functions it waited in (wchan, with counts), its page faults (minor, major), the
// bytes it read and its voluntary / involuntary context switches over the frame: a page fault or read, a lock, the GPU
// driver or plain CPU (the JIT, the game) tell apart. Nothing changes in the game.
static volatile pid_t g_main_tid;
static double g_t_start;
static int g_stall_ms;
static int st_read(pid_t tid, char *state, unsigned long *minf, unsigned long *majf)
{
    char p[64], b[1024]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", (int)tid);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char *c = strrchr(b, ')'); if (!c) return 0;
    // after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt
    unsigned long v[8]; char stc;
    if (sscanf(c + 2, "%c %lu %lu %lu %lu %lu %lu %lu %*s %lu", &stc, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], minf, majf) < 9) return 0;
    *state = stc; return 1;
}
static void st_wchan(pid_t tid, char *w, size_t n)
{
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/wchan", (int)tid);
    FILE *f = fopen(p, "r"); w[0] = 0; if (!f) return;
    size_t k = fread(w, 1, n - 1, f); fclose(f); w[k] = 0;
}
static void st_ctx(pid_t tid, unsigned long *vol, unsigned long *invol, unsigned long *rb)
{
    char p[64], l[160]; *vol = *invol = *rb = 0;
    snprintf(p, sizeof p, "/proc/self/task/%d/status", (int)tid);
    FILE *f = fopen(p, "r");
    if (f) { while (fgets(l, sizeof l, f)) { sscanf(l, "voluntary_ctxt_switches: %lu", vol); sscanf(l, "nonvoluntary_ctxt_switches: %lu", invol); } fclose(f); }
    snprintf(p, sizeof p, "/proc/self/task/%d/io", (int)tid);
    f = fopen(p, "r");
    if (f) { while (fgets(l, sizeof l, f)) sscanf(l, "read_bytes: %lu", rb); fclose(f); }
}
static void *stall_main(void *arg)
{
    (void)arg;
    struct { char w[48]; int n; } wc[16]; int nwc = 0, sr = 0, ss = 0, sd = 0, so = 0, in_stall = 0;
    unsigned long mi0 = 0, ma0 = 0, v0 = 0, iv0 = 0, rb0 = 0; double t0 = 0;
    for (;;) {
        usleep(20000);
        // the game's own thread, by name (UnityMain: the frame hook's thread waits for it)
        static pid_t um; static int looked;
        if (!um && looked++ < 500) {
            DIR *d = opendir("/proc/self/task"); struct dirent *de;
            while (d && (de = readdir(d))) {
                char pp[64], nm[32] = ""; snprintf(pp, sizeof pp, "/proc/self/task/%s/comm", de->d_name);
                FILE *f = fopen(pp, "r"); if (!f) continue;
                if (fgets(nm, sizeof nm, f) && !strncmp(nm, "UnityMain", 9)) um = (pid_t)atoi(de->d_name);
                fclose(f);
            }
            if (d) closedir(d);
        }
        pid_t tid = um ? um : g_main_tid; if (!tid) continue;
        double ft = g_frame_t, t = now();
        if (ft <= 0) continue;
        if (!in_stall) {
            if ((t - ft) * 1000 < g_stall_ms) continue;
            in_stall = 1; nwc = sr = ss = sd = so = 0; t0 = ft;
            char c; if (!st_read(tid, &c, &mi0, &ma0)) { in_stall = 0; continue; }
            st_ctx(tid, &v0, &iv0, &rb0);
        }
        if (g_frame_t != t0) {      // the frame ended: one line
            char c; unsigned long mi, ma, v, iv, rb; st_read(tid, &c, &mi, &ma); st_ctx(tid, &v, &iv, &rb);
            char wl[400] = ""; size_t o = 0;
            for (int i = 0; i < nwc && o < sizeof wl - 60; i++) o += snprintf(wl + o, sizeof wl - o, " %s x%d", wc[i].w, wc[i].n);
            fprintf(stderr, "[stall] frame of %.0f ms at %.1f s: the game thread's samples R %d, S %d, D %d, other %d; waits:%s; page faults %lu minor, %lu major; read %lu KB; switches %lu voluntary, %lu involuntary\n",
                    (g_frame_t - t0) * 1000, t0 - g_t_start, sr, ss, sd, so, nwc ? wl : " none", mi - mi0, ma - ma0, (rb - rb0) >> 10, v - v0, iv - iv0);
            in_stall = 0; continue;
        }
        char c; unsigned long mi, ma;
        if (!st_read(tid, &c, &mi, &ma)) continue;
        if (c == 'R') sr++; else if (c == 'S') ss++; else if (c == 'D') sd++; else so++;
        if (c != 'R') {
            // who runs while UnityMain waits: the other threads in R, by name
            DIR *d = opendir("/proc/self/task"); struct dirent *de;
            while (d && (de = readdir(d))) {
                pid_t o = (pid_t)atoi(de->d_name); if (o <= 0 || o == tid) continue;
                char pp[64], bb[256]; snprintf(pp, sizeof pp, "/proc/self/task/%d/stat", (int)o);
                FILE *f = fopen(pp, "r"); if (!f) continue;
                size_t nn = fread(bb, 1, sizeof bb - 1, f); fclose(f); bb[nn] = 0;
                char *l = strchr(bb, '('), *r2 = strrchr(bb, ')'); if (!l || !r2 || r2[2] != 'R') continue;
                char nm[48]; snprintf(nm, sizeof nm, "run:%.*s", (int)(r2 - l - 1), l + 1);
                int i; for (i = 0; i < nwc && strcmp(wc[i].w, nm); i++) {}
                if (i < nwc) wc[i].n++; else if (nwc < 16) { snprintf(wc[nwc].w, sizeof wc[nwc].w, "%s", nm); wc[nwc++].n = 1; }
            }
            if (d) closedir(d);
            char w[48]; st_wchan(tid, w, sizeof w);
            if (!w[0] || !strcmp(w, "0")) continue;
            int i; for (i = 0; i < nwc && strcmp(wc[i].w, w); i++) {}
            if (i < nwc) wc[i].n++; else if (nwc < 16) { snprintf(wc[nwc].w, sizeof wc[nwc].w, "%s", w); wc[nwc++].n = 1; }
        }
    }
    return NULL;
}

void gcbudget_frame(void *user)
{
    (void)user;
    if (!g_main_tid) {
        g_main_tid = (pid_t)syscall(SYS_gettid); g_t_start = now();
        const char *e = getenv("UA_STALL_TRACE");
        if (e && atoi(e) > 0) { g_stall_ms = atoi(e); pthread_t t; if (pthread_create(&t, NULL, stall_main, NULL) == 0) pthread_detach(t);
                                fprintf(stderr, "[stall] tracing frames over %d ms\n", g_stall_ms); }
    }
    load_flag_frame();
    if (g_grow_mb > 0 || g_tight > 0) {
        double t = now();
        // a load's frame: over 2.5 s, or over 1 s within 5 s of another such (a floor's load has several; a single
        // stall in play, e.g. a shader program linked on first use, is not a load)
        if (g_frame_t > 0 && t - g_frame_t > 1.0) {
            if (t - g_frame_t > 2.5 || t - g_long1 < 5.0) g_long_t = t;
            g_long1 = t;
        }
        g_frame_t = t;
        // Mono 2.6's Boehm (Unity's 32-bit Mono players) and the Mono route's (Unity's Mono for aarch64,
        // libmonobdwgc-2.0.so): both grow their heap during a load rather than collect
        if (!g_grow_started && mono_runtime_lib()) {
            pthread_t t;
            g_grow_started = 1;
            if (pthread_create(&t, NULL, grow_main, NULL) == 0) pthread_detach(t);
        }
    }
    // the census (UA_GC_CENSUS) also where the game keeps its collector on
    if (g_census_on && resolve_collector()) census_frame();
    if (g_budget <= 0 || !g_mode_known || g_mode != GC_DISABLED || !r_set_mode) return;
    if (!resolve_collector()) { g_budget = 0; return; }
    int64_t used = m_used();
    if (g_base < 0) { g_base = used; return; }
    if (used - g_base < g_budget) return;
    double t = now();
    r_set_mode(GC_ENABLED);
    m_collect(m_max_gen());
    r_set_mode(GC_DISABLED);
    int64_t after = m_used();
    if (g_count++ < 50)
        fprintf(stderr, "[host] gc budget: the game keeps the collector off; heap %lld -> %lld MB used of %lld MB, collected in %.0f ms (UA_GC_BUDGET=%lld)\n",
                (long long)(used >> 20), (long long)(after >> 20), (long long)(m_heap ? m_heap() >> 20 : -1), (now() - t) * 1000, g_budget >> 20);
    g_base = after;
    if (g_census_left > 0) { g_census_left--; census("gc budget"); }
}
