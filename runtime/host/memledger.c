// memledger.c: which library mapped which memory (a measurement, UA_MEM_LEDGER=1).
//
// The game's Android libraries (libunity, Mono, the plugins) get their mmap, mmap64, __mmap2, munmap and mremap
// through these wrappers when UA_MEM_LEDGER=1: each mapping is noted with the library that asked for it (the return
// address, so_dladdr). memledger_report() (the census, UA_DUMP_MEMORY / UA_DUMP_TEXTURES, monodump.c) logs per library
// its mappings, their size and how much of them is resident (mincore), anonymous and file mappings apart, then glibc's
// malloc arenas (malloc_stats: every library's malloc, the GPU driver's too) and mallinfo's totals: what the
// process's anonymous memory in /proc/<pid>/smaps is made of. UA_MEM_LEDGER=2: their malloc, calloc, realloc, free,
// memalign and posix_memalign too (live bytes by library: a pointer table, a lock around each call; a measurement).
// Off, the table is empty and nothing is wrapped.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../loader/so_loader.h"
#include "memledger.h"

int so_dladdr(const void *addr, Dl_info *info);   /* the loader: Android modules first, then the host */

enum { MAXR = 16384, MAXM = 16 };
struct range { uintptr_t a, b; unsigned char mod, anon; };
static struct range g_r[MAXR];
static int g_n, g_on = -1, g_dropped;
static char g_mods[MAXM][48];
static int g_nmods;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int on(void)
{
    if (g_on < 0) { const char *e = getenv("UA_MEM_LEDGER"); g_on = e && atoi(e) > 0; }
    return g_on;
}

static int module_of(void *ret)            // (under g_mu)
{
    Dl_info di;
    const char *n = "other";
    if (so_dladdr(ret, &di) && di.dli_fname) { n = strrchr(di.dli_fname, '/'); n = n ? n + 1 : di.dli_fname; }
    for (int i = 0; i < g_nmods; i++) if (!strcmp(g_mods[i], n)) return i;
    if (g_nmods == MAXM) return MAXM - 1;
    snprintf(g_mods[g_nmods], sizeof g_mods[0], "%s", n);
    return g_nmods++;
}

static void forget(uintptr_t a, uintptr_t b)   // [a, b) unmapped: the noted ranges trimmed or split (under g_mu)
{
    for (int i = 0; i < g_n; i++) {
        struct range *r = &g_r[i];
        if (r->b <= a || r->a >= b) continue;
        if (r->a >= a && r->b <= b) { *r = g_r[--g_n]; i--; continue; }
        if (r->a < a && r->b > b) {                 // a hole in the middle: two ranges
            if (g_n < MAXR) { g_r[g_n] = *r; g_r[g_n].a = b; g_n++; }
            r->b = a;
            continue;
        }
        if (r->a < a) r->b = a; else r->a = b;
    }
}

static void note(void *p, size_t len, int flags, void *ret)
{
    if (p == MAP_FAILED || !len) return;
    uintptr_t a = (uintptr_t)p, b = a + ((len + 4095) & ~(size_t)4095);
    pthread_mutex_lock(&g_mu);
    forget(a, b);                                   // (MAP_FIXED over an old mapping)
    if (g_n < MAXR) g_r[g_n++] = (struct range){ a, b, (unsigned char)module_of(ret), (unsigned char)((flags & MAP_ANONYMOUS) != 0) };
    else g_dropped++;
    pthread_mutex_unlock(&g_mu);
}

static void *w_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    void *p = mmap(addr, len, prot, flags, fd, off);
    note(p, len, flags, __builtin_return_address(0));
    return p;
}
static void *w_mmap64(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    void *p = mmap64(addr, len, prot, flags, fd, off);
    note(p, len, flags, __builtin_return_address(0));
    return p;
}
static void *w_mmap2(void *addr, size_t len, int prot, int flags, int fd, size_t pgoff)
{
#ifdef SYS_mmap2
    void *p = (void *)syscall(SYS_mmap2, addr, len, prot, flags, fd, pgoff);
#else
    void *p = mmap(addr, len, prot, flags, fd, (off_t)pgoff * 4096);
#endif
    note(p, len, flags, __builtin_return_address(0));
    return p;
}
static int w_munmap(void *addr, size_t len)
{
    int r = munmap(addr, len);
    if (!r) {
        pthread_mutex_lock(&g_mu);
        forget((uintptr_t)addr, (uintptr_t)addr + ((len + 4095) & ~(size_t)4095));
        pthread_mutex_unlock(&g_mu);
    }
    return r;
}
static void *w_mremap(void *old, size_t olen, size_t nlen, int flags, void *naddr)
{
    void *p = mremap(old, olen, nlen, flags, naddr);
    if (p != MAP_FAILED) {
        pthread_mutex_lock(&g_mu);
        int mod = -1, anon = 1;
        for (int i = 0; i < g_n; i++) if (g_r[i].a <= (uintptr_t)old && g_r[i].b > (uintptr_t)old) { mod = g_r[i].mod; anon = g_r[i].anon; break; }
        forget((uintptr_t)old, (uintptr_t)old + ((olen + 4095) & ~(size_t)4095));
        if (mod < 0) mod = module_of(__builtin_return_address(0));
        uintptr_t a = (uintptr_t)p;
        if (g_n < MAXR) g_r[g_n++] = (struct range){ a, a + ((nlen + 4095) & ~(size_t)4095), (unsigned char)mod, (unsigned char)anon };
        pthread_mutex_unlock(&g_mu);
    }
    return p;
}

// UA_MEM_LEDGER=2: the Android libraries' malloc family too: each block they allocate noted in a pointer table (linear
// probing, 2^20 slots, 8 MB) with its library and usable size, taken out when freed, live bytes by library in the report.
// A measurement: a lock around every allocation and free of those libraries.
enum { MSLOTS = 1 << 20 };
struct mslot { uintptr_t p; unsigned size : 24; unsigned mod : 8; };
static struct mslot *g_ms;
static unsigned g_mused, g_mfull;
static long long g_mlive[MAXM];
static unsigned g_mcount[MAXM];
static pthread_mutex_t g_mmu = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t g_mbase[MAXM];                  // a library's lowest address seen (so_dladdr's base), by module index

static int mod_of_fast(void *ret)                // (under g_mmu) the module whose base is the highest at or below ret
{
    uintptr_t a = (uintptr_t)ret;
    int best = -1;
    for (int i = 0; i < g_nmods; i++)
        if (g_mbase[i] && g_mbase[i] <= a && (best < 0 || g_mbase[i] > g_mbase[best])) best = i;
    if (best >= 0 && a - g_mbase[best] < (64u << 20)) return best;
    Dl_info di;
    pthread_mutex_lock(&g_mu);
    int m = module_of(ret);
    if (so_dladdr(ret, &di) && di.dli_fbase && (!g_mbase[m] || (uintptr_t)di.dli_fbase < g_mbase[m])) g_mbase[m] = (uintptr_t)di.dli_fbase;
    pthread_mutex_unlock(&g_mu);
    return m;
}
static unsigned slot_of(uintptr_t p) { return (unsigned)((p >> 3) * 2654435761u) & (MSLOTS - 1); }
static void mtrack(void *p, void *ret)
{
    if (!p || !g_ms) return;
    size_t n = malloc_usable_size(p);
    pthread_mutex_lock(&g_mmu);
    int m = mod_of_fast(ret);
    if (g_mused * 4 >= MSLOTS * 3) { g_mfull++; pthread_mutex_unlock(&g_mmu); return; }
    unsigned i = slot_of((uintptr_t)p);
    while (g_ms[i].p) i = (i + 1) & (MSLOTS - 1);
    g_ms[i].p = (uintptr_t)p; g_ms[i].size = n > 0xffffff ? 0xffffff : (unsigned)n; g_ms[i].mod = (unsigned)m;
    g_mused++;
    g_mlive[m] += n; g_mcount[m]++;
    pthread_mutex_unlock(&g_mmu);
}
static void muntrack(void *p)
{
    if (!p || !g_ms) return;
    pthread_mutex_lock(&g_mmu);
    unsigned i = slot_of((uintptr_t)p);
    while (g_ms[i].p && g_ms[i].p != (uintptr_t)p) i = (i + 1) & (MSLOTS - 1);
    if (g_ms[i].p) {
        size_t n = g_ms[i].size;
        if (n == 0xffffff) n = malloc_usable_size(p);
        g_mlive[g_ms[i].mod] -= n; g_mcount[g_ms[i].mod]--;
        // backward-shift deletion: the run after the hole moved up where its entries belong
        unsigned j = i;
        g_ms[i].p = 0; g_mused--;
        for (;;) {
            j = (j + 1) & (MSLOTS - 1);
            if (!g_ms[j].p) break;
            unsigned k = slot_of(g_ms[j].p);
            if ((i <= j) ? (i < k && k <= j) : (i < k || k <= j)) continue;
            g_ms[i] = g_ms[j]; g_ms[j].p = 0; i = j;
        }
    }
    pthread_mutex_unlock(&g_mmu);
}
static void *w_malloc(size_t n) { void *p = malloc(n); mtrack(p, __builtin_return_address(0)); return p; }
static void *w_calloc(size_t a, size_t b) { void *p = calloc(a, b); mtrack(p, __builtin_return_address(0)); return p; }
static void *w_realloc(void *o, size_t n)
{
    muntrack(o);
    void *p = realloc(o, n);
    if (p) mtrack(p, __builtin_return_address(0));
    else if (o && n) mtrack(o, __builtin_return_address(0));   // (the old block stays when realloc fails)
    return p;
}
static void w_free(void *p) { muntrack(p); free(p); }
static void *w_memalign(size_t al, size_t n) { void *p = memalign(al, n); mtrack(p, __builtin_return_address(0)); return p; }
static int w_posix_memalign(void **pp, size_t al, size_t n)
{
    int r = posix_memalign(pp, al, n);
    if (!r) mtrack(*pp, __builtin_return_address(0));
    return r;
}

static const so_shim g_mshims[] = {
    { "mmap", (void *)w_mmap }, { "mmap64", (void *)w_mmap64 }, { "__mmap2", (void *)w_mmap2 },
    { "munmap", (void *)w_munmap }, { "mremap", (void *)w_mremap },
    { "malloc", (void *)w_malloc }, { "calloc", (void *)w_calloc }, { "realloc", (void *)w_realloc }, { "free", (void *)w_free },
    { "memalign", (void *)w_memalign }, { "posix_memalign", (void *)w_posix_memalign }, { NULL, NULL }
};

static const so_shim g_shims[] = {
    { "mmap", (void *)w_mmap }, { "mmap64", (void *)w_mmap64 }, { "__mmap2", (void *)w_mmap2 },
    { "munmap", (void *)w_munmap }, { "mremap", (void *)w_mremap }, { NULL, NULL }
};
static const so_shim g_none[] = { { NULL, NULL } };

const so_shim *memledger_table(void)
{
    if (!on()) return g_none;
    if (atoi(getenv("UA_MEM_LEDGER")) >= 2) {
        g_ms = calloc(MSLOTS, sizeof *g_ms);
        if (g_ms) return g_mshims;
    }
    return g_shims;
}

void memledger_report(void)
{
    if (on()) {
        static unsigned char vec[(64u << 20) / 4096];     // mincore's answer for up to 64 MB a call
        long long virt[MAXM][2] = { { 0 } }, res[MAXM][2] = { { 0 } };
        int cnt[MAXM][2] = { { 0 } };
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_n; i++) {
            struct range *r = &g_r[i];
            virt[r->mod][r->anon] += r->b - r->a;
            cnt[r->mod][r->anon]++;
            for (uintptr_t a = r->a; a < r->b; a += sizeof vec * 4096) {
                size_t len = r->b - a < sizeof vec * 4096 ? r->b - a : sizeof vec * 4096;
                if (mincore((void *)a, len, vec) != 0) break;
                for (size_t k = 0; k < len / 4096; k++) res[r->mod][r->anon] += (vec[k] & 1) * 4096;
            }
        }
        for (int m = 0; m < g_nmods; m++)
            for (int an = 1; an >= 0; an--)
                if (cnt[m][an])
                    fprintf(stderr, "[mem] %s: %d %s mappings, %.1f MB, %.1f MB resident\n", g_mods[m], cnt[m][an],
                            an ? "anonymous" : "file", virt[m][an] / 1048576.0, res[m][an] / 1048576.0);
        if (g_dropped) fprintf(stderr, "[mem] (%d mappings not noted: the table was full)\n", g_dropped);
        pthread_mutex_unlock(&g_mu);
        if (g_ms) {
            pthread_mutex_lock(&g_mmu);
            for (int m = 0; m < g_nmods; m++)
                if (g_mcount[m]) fprintf(stderr, "[mem] %s: %u blocks from malloc, %.1f MB live\n", g_mods[m], g_mcount[m], g_mlive[m] / 1048576.0);
            if (g_mfull) fprintf(stderr, "[mem] (%u allocations not noted: the pointer table was full)\n", g_mfull);
            pthread_mutex_unlock(&g_mmu);
        }
    }
    // glibc's malloc: every library's malloc (Unity's, Mono's, the GPU driver's, ours), by arena
    struct mallinfo mi = mallinfo();
    fprintf(stderr, "[mem] malloc: %.1f MB in use, %.1f MB free in the arenas, %.1f MB in mmapped chunks (%d), arenas %.1f MB\n",
            (unsigned)mi.uordblks / 1048576.0, (unsigned)mi.fordblks / 1048576.0, (unsigned)mi.hblkhd / 1048576.0, mi.hblks,
            (unsigned)mi.arena / 1048576.0);
    fflush(stderr);
    malloc_stats();                                 // (stderr: each arena's system and in-use bytes)
}
