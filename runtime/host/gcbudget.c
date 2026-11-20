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
#include "../loader/so_loader.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
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
static volatile double g_frame_t;           // when UnityMain last finished a frame
static int g_grow_started;
static int g_census_on, g_census_left, g_census_frames;   // UA_GC_CENSUS

void gcbudget_init(void)
{
    const char *e = getenv("UA_GC_BUDGET");
    if (e && *e) g_budget = (long long)atoi(e) << 20;
    e = getenv("UA_GC_GROW");
    if (e && *e) g_grow_mb = atoi(e);
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
            m_heap = (int64_t (*)(void))so_dlsym(h, "mono_gc_get_heap_size");
        } else if ((h = so_dlopen("libil2cpp.so", 4 /* RTLD_NOLOAD */))) {
            // IL2CPP (its Boehm collector): il2cpp_gc_collect collects everything whatever the
            // generation it is given; UnityMain, where this runs, is attached to the runtime
            m_collect = (void (*)(int))so_dlsym(h, "il2cpp_gc_collect");
            m_used = (int64_t (*)(void))so_dlsym(h, "il2cpp_gc_get_used_size");
            m_heap = (int64_t (*)(void))so_dlsym(h, "il2cpp_gc_get_heap_size");
            m_max_gen = il2cpp_max_gen;
        }
        if (!m_collect || !m_max_gen || !m_used) { m_collect = NULL; g_budget = 0; return; }
    }
    census_frame();
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
