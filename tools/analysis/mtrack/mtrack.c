// mtrack.so (LD_PRELOAD, a measurement): every malloc of the process noted with the library its caller is in (by
// /proc/self/maps, read with raw syscalls: no dladdr inside malloc), through glibc's __libc_malloc and friends. When
// <cwd>/mtrack.req appears (a thread looks every 2 s) it writes to stderr the live bytes by library
// ("[mtrack] libmali.so: 3944 blocks, 13.9 MB live") and the 30 call sites in ua_host holding the most, as offsets
// to symbolize with ua_host.debug (arm-linux-gnueabihf-addr2line -f -i -e out/ua_host.debug <offset - 1>).
// UA_MEM_LEDGER=2 (docs/runtime.md) names the Android libraries' blocks the same way; this adds the GPU driver's,
// ua_host's and the system libraries'. A 16 MB table and a lock around every allocation: not for a frame-rate run.
// Build (the device's 32-bit userland), in runtime/host's ua-host-armhf image:
//   arm-linux-gnueabihf-gcc -O2 -shared -fPIC -o mtrack.so mtrack.c -lpthread
// Use: LD_PRELOAD=<dir>/mtrack.so for ua_host only (a bench's environment file), then touch mtrack.req in the
// game's folder (docs/porting.md, where a level's memory goes).
#define _GNU_SOURCE
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void *__libc_malloc(size_t), *__libc_calloc(size_t, size_t), *__libc_realloc(void *, size_t), *__libc_memalign(size_t, size_t);
extern void __libc_free(void *);

enum { MS = 1 << 21, MAXM = 120, MAXR = 4096 };
struct slot { uint32_t p; uint32_t size : 24, mod : 8; };
static struct slot g_s[MS];
static uint32_t g_caller[MS];
static uintptr_t g_base[MAXM];
static unsigned g_used, g_full;
static long long g_live[MAXM]; static unsigned g_cnt[MAXM];
static char g_names[MAXM][64]; static int g_nmods = 1;   // 0 = unknown
static struct { uintptr_t a, b; int m; } g_r[MAXR]; static int g_nr;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_buf[512 << 10];
static unsigned g_miss;

static void read_maps(void)
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    size_t n = 0; ssize_t k;
    while (n < sizeof g_buf - 1 && (k = read(fd, g_buf + n, sizeof g_buf - 1 - n)) > 0) n += k;
    close(fd); g_buf[n] = 0;
    g_nr = 0;
    for (char *l = g_buf; *l && g_nr < MAXR; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        unsigned long a, b, fo = 0; char perm[8]; int off = 0;
        if (sscanf(l, "%lx-%lx %7s %lx %*s %*s %n", &a, &b, perm, &fo, &off) >= 4 && perm[2] == 'x' && off && l[off] == '/') {
            const char *path = l + off, *base = strrchr(path, '/'); base = base ? base + 1 : path;
            int m = 0;
            for (int i = 1; i < g_nmods; i++) if (!strcmp(g_names[i], base)) { m = i; break; }
            if (!m && g_nmods < MAXM) { m = g_nmods++; snprintf(g_names[m], sizeof g_names[m], "%s", base); }
            g_r[g_nr].a = a; g_r[g_nr].b = b; g_r[g_nr].m = m; g_nr++;
            if (m && (!g_base[m] || a - fo < g_base[m])) g_base[m] = a - fo;
        }
        if (!e) break;
        l = e + 1;
    }
}
static int mod_of(uintptr_t a)
{
    for (int i = 0; i < g_nr; i++) if (a >= g_r[i].a && a < g_r[i].b) return g_r[i].m;
    if ((g_miss++ & 1023) == 0) { read_maps(); for (int i = 0; i < g_nr; i++) if (a >= g_r[i].a && a < g_r[i].b) return g_r[i].m; }
    return 0;
}
static unsigned slot_of(uintptr_t p) { return (unsigned)((p >> 3) * 2654435761u) & (MS - 1); }
static __thread int t_in;
static void track(void *p, void *ret)
{
    if (!p || t_in) return;
    t_in = 1;
    size_t n = malloc_usable_size(p);
    pthread_mutex_lock(&g_mu);
    int m = mod_of((uintptr_t)ret);
    if (g_used * 4 >= MS * 3) g_full++;
    else {
        unsigned i = slot_of((uintptr_t)p);
        while (g_s[i].p) i = (i + 1) & (MS - 1);
        g_s[i].p = (uint32_t)(uintptr_t)p; g_s[i].size = n > 0xffffff ? 0xffffff : n; g_s[i].mod = m; g_caller[i] = (uint32_t)(uintptr_t)ret;
        g_used++; g_live[m] += n; g_cnt[m]++;
    }
    pthread_mutex_unlock(&g_mu);
    t_in = 0;
}
static void untrack(void *p)
{
    if (!p || t_in) return;
    pthread_mutex_lock(&g_mu);
    unsigned i = slot_of((uintptr_t)p);
    while (g_s[i].p && g_s[i].p != (uint32_t)(uintptr_t)p) i = (i + 1) & (MS - 1);
    if (g_s[i].p) {
        size_t n = g_s[i].size; if (n == 0xffffff) n = malloc_usable_size(p);
        g_live[g_s[i].mod] -= n; g_cnt[g_s[i].mod]--;
        unsigned j = i; g_s[i].p = 0; g_used--;
        for (;;) {
            j = (j + 1) & (MS - 1);
            if (!g_s[j].p) break;
            unsigned k = slot_of(g_s[j].p);
            if ((i <= j) ? (i < k && k <= j) : (i < k || k <= j)) continue;
            g_s[i] = g_s[j]; g_caller[i] = g_caller[j]; g_s[j].p = 0; i = j;
        }
    }
    pthread_mutex_unlock(&g_mu);
}
void *malloc(size_t n) { void *p = __libc_malloc(n); track(p, __builtin_return_address(0)); return p; }
void *calloc(size_t a, size_t b) { void *p = __libc_calloc(a, b); track(p, __builtin_return_address(0)); return p; }
void *realloc(void *o, size_t n)
{
    if (o) untrack(o);
    void *p = __libc_realloc(o, n);
    if (p) track(p, __builtin_return_address(0)); else if (o && n) track(o, __builtin_return_address(0));
    return p;
}
void free(void *p) { untrack(p); __libc_free(p); }
void *memalign(size_t a, size_t n) { void *p = __libc_memalign(a, n); track(p, __builtin_return_address(0)); return p; }
int posix_memalign(void **r, size_t a, size_t n) { void *p = __libc_memalign(a, n); if (!p) return 12; *r = p; track(p, __builtin_return_address(0)); return 0; }
void *aligned_alloc(size_t a, size_t n) { void *p = __libc_memalign(a, n); track(p, __builtin_return_address(0)); return p; }

static void report(void)
{
    char line[256];
    pthread_mutex_lock(&g_mu);
    for (int m = 0; m < g_nmods; m++)
        if (g_cnt[m]) {
            int k = snprintf(line, sizeof line, "[mtrack] %s: %u blocks, %.1f MB live\n", m ? g_names[m] : "(unknown caller)", g_cnt[m], g_live[m] / 1048576.0);
            write(2, line, k);
        }
    {
        static struct { uint32_t c; long long b; unsigned n; } top[4096]; int nt = 0;
        int hm = 0; for (int m = 1; m < g_nmods; m++) if (!strcmp(g_names[m], "ua_host")) hm = m;
        for (unsigned i = 0; hm && i < MS; i++) {
            if (!g_s[i].p || g_s[i].mod != hm) continue;
            size_t n = g_s[i].size == 0xffffff ? malloc_usable_size((void *)(uintptr_t)g_s[i].p) : g_s[i].size;
            int j = 0; while (j < nt && top[j].c != g_caller[i]) j++;
            if (j == nt) { if (nt == 4096) continue; top[nt].c = g_caller[i]; top[nt].b = 0; top[nt].n = 0; nt++; }
            top[j].b += n; top[j].n++;
        }
        for (int r = 0; r < 30 && r < nt; r++) {
            int b = r; for (int j = r + 1; j < nt; j++) if (top[j].b > top[b].b) b = j;
            __typeof__(top[0]) t = top[r]; top[r] = top[b]; top[b] = t;
            int k = snprintf(line, sizeof line, "[mtrack] ua_host caller +0x%lx: %u blocks, %.2f MB\n", (unsigned long)(top[r].c - g_base[hm]), top[r].n, top[r].b / 1048576.0);
            write(2, line, k);
        }
    }
    int k = snprintf(line, sizeof line, "[mtrack] %u blocks noted, %u not (table full)\n", g_used, g_full);
    write(2, line, k);
    pthread_mutex_unlock(&g_mu);
}
static void *watch(void *x)
{
    (void)x;
    for (;;) {
        sleep(2);
        if (access("mtrack.req", F_OK) == 0) { unlink("mtrack.req"); report(); }
    }
    return NULL;
}
__attribute__((constructor)) static void init(void)
{
    pthread_mutex_lock(&g_mu); read_maps(); pthread_mutex_unlock(&g_mu);
    pthread_t t; pthread_create(&t, NULL, watch, NULL); pthread_detach(t);
}
