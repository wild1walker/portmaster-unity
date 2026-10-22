// prof.c check (UA_PROF): a thread named UnityMain spends its frames in three places, a fake
// "render thread" waits for it, and the profile written must say so:
//   - spin_inner() called from spin_outer() (C code: named by dladdr, the caller on the stack),
//   - a loop in anonymous executable memory, named by a fake Mono as M:Fake.Jit:Loop,
//   - sleeping (blocked samples).
// Scenarios (argv[1]): on (UA_PROF_START), req (<dir>/prof.req), off (no UA_PROF: no thread, no file);
// spin <hz>: the cost of sampling, as the work a thread with a 12 KB deep stack gets done in 3 s
// while it is sampled at <hz> (0: not), in iterations per second.
// Usage: test_prof <scenario> <dir> [hz]. Built with -rdynamic (dladdr names the test's functions); run
// under docker --platform linux/arm/v7 (QEMU), natively on arm64, or on a handheld
// (runtime/host/test/prof_test.sh).
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "../prof.h"

// ---- what prof.c needs from the loader, the fake JNI and Mono ------------------------------
static _Atomic int g_finishing;
int up_is_finishing(void) { return g_finishing; }
const char *so_module_at(const void *addr, uintptr_t *bias) { (void)addr; (void)bias; return NULL; }
// the loader's exidx lookup, for this program and glibc's libraries (32-bit ARM)
#include <link.h>
struct xq { uintptr_t pc, exidx; int count; };
static int xcb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
#if defined(__arm__)
    struct xq *q = data;
    uintptr_t exidx = 0; int count = 0, in = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t a = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_LOAD && q->pc >= a && q->pc < a + ph->p_memsz) in = 1;
        if (ph->p_type == PT_ARM_EXIDX) { exidx = a; count = (int)(ph->p_memsz / 8); }
    }
    if (in) { q->exidx = exidx; q->count = count; return 1; }
#else
    (void)info; (void)data;
#endif
    return 0;
}
uintptr_t so_find_exidx(uintptr_t pc, int *pcount)
{
    struct xq q = { pc, 0, 0 };
    dl_iterate_phdr(xcb, &q);
    *pcount = q.count;
    return q.exidx;
}

static uint8_t *g_jit;                  // the "JIT code": a countdown loop
static int g_domain, g_ji, g_method, g_thread;
static void *f_root(void) { return &g_domain; }
static void *f_attach(void *d) { return d == &g_domain ? &g_thread : NULL; }
static void f_detach(void *t) { (void)t; }
static void *f_find(void *d, void *a) { return d == &g_domain && (uint8_t *)a >= g_jit && (uint8_t *)a < g_jit + 64 ? &g_ji : NULL; }
static void *f_method(void *ji) { return ji == &g_ji ? &g_method : NULL; }
static void *f_start(void *ji) { (void)ji; return g_jit; }
static int f_size(void *ji) { (void)ji; return 64; }
static char *f_name(void *m, int sig) { (void)sig; return m == &g_method ? strdup("Fake.Jit:Loop (int)") : NULL; }
static int g_lib;
void *mono_runtime_lib(void) { return &g_lib; }
void *so_dlsym(void *h, const char *s)
{
    if (h != &g_lib) return NULL;
    static const struct { const char *n; void *f; } t[] = {
        { "mono_get_root_domain", (void *)f_root }, { "mono_thread_attach", (void *)f_attach }, { "mono_thread_detach", (void *)f_detach },
        { "mono_jit_info_table_find", (void *)f_find }, { "mono_jit_info_get_method", (void *)f_method },
        { "mono_jit_info_get_code_start", (void *)f_start }, { "mono_jit_info_get_code_size", (void *)f_size },
        { "mono_method_full_name", (void *)f_name }, { "mono_free", (void *)free } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(s, t[i].n)) return t[i].f;
    return NULL;
}

// ---- the "game" --------------------------------------------------------------------------------
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static volatile double g_sink;
static double g_per_iter;               // seconds per spin_inner iteration (calibrated)
// a leaf with no calls in its loop (so the pc is in it, not in clock_gettime); its caller's
// return address is in lr, spin_outer's (into game_thread) on the stack
__attribute__((noinline)) void spin_inner(long n) { double x = 0; for (long i = 0; i < n; i++) x += (double)i * 0.5; g_sink += x; }
__attribute__((noinline)) void spin_outer(double secs) { spin_inner((long)(secs / g_per_iter)); g_sink += 1; }
static int (*g_loop)(int);
static _Atomic int g_stop;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_frame;

void *game_thread(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "UnityMain");
    while (!g_stop) {
        spin_outer(0.006);
        double t = now();
        while (now() - t < 0.003) g_loop(20000);
        struct timespec s = { 0, 4000000 };
        nanosleep(&s, NULL);
        prof_main_frame();
        pthread_mutex_lock(&g_mu); g_frame++; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

static void *render(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "UnityGfxDeviceW");
    int seen = 0;
    while (!g_stop) {
        pthread_mutex_lock(&g_mu);
        while (g_frame == seen && !g_stop) {
            struct timespec d;
            clock_gettime(CLOCK_REALTIME, &d);
            d.tv_nsec += 50000000; if (d.tv_nsec >= 1000000000) { d.tv_sec++; d.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mu, &d);
        }
        seen = g_frame;
        pthread_mutex_unlock(&g_mu);
        spin_inner((long)(0.001 / g_per_iter));
        prof_swap();
    }
    return NULL;
}

static void *other(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "Job.Worker 0");
    while (!g_stop) { struct timespec s = { 0, 10000000 }; nanosleep(&s, NULL); }
    return NULL;
}

static void make_jit(void)
{
    g_jit = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_jit == MAP_FAILED) { perror("mmap"); exit(1); }
#if defined(__aarch64__)
    static const uint32_t code[] = { 0xF1000400, 0x54FFFFE1, 0xD65F03C0 };   // subs x0,x0,#1; b.ne .-4; ret
#else
    static const uint32_t code[] = { 0xE2500001, 0x1AFFFFFD, 0xE12FFF1E };   // subs r0,r0,#1; bne .-4; bx lr (ARM)
#endif
    memcpy(g_jit, code, sizeof code);
    __builtin___clear_cache((char *)g_jit, (char *)g_jit + sizeof code);
    g_loop = (int (*)(int))(void *)g_jit;
}

// ---- spin: the sampler's cost to the sampled thread ----------------------------------------
static _Atomic long g_iters;
__attribute__((noinline)) long deep(int n, volatile char *pad)
{
    volatile char frame[192];
    frame[0] = pad ? pad[0] : 0;
    if (n > 0) return deep(n - 1, frame) + frame[0];
    long k = 0;
    double t = now();
    while (now() - t < 3.0) { spin_inner(2000); k++; }
    g_iters = k;
    return k;
}
void *spin_thread(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "UnityMain");
    deep(64, NULL);
    return NULL;
}

static int fail(const char *what) { printf("test_prof: FAILED: %s\n", what); return 1; }

static int has_ua_prof_thread(void)
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    int found = 0;
    while (d && (e = readdir(d))) {
        char p[300], c[32] = "";
        snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(c, sizeof c, f)) c[strcspn(c, "\n")] = 0; fclose(f); }
        if (!strcmp(c, "ua_prof")) found = 1;
    }
    if (d) closedir(d);
    return found;
}

// the profile, checked line by line (tools/analysis/profsum.py reads the same format)
static int check(const char *path, double want_secs)
{
    FILE *f = fopen(path, "r");
    if (!f) return fail("no profile written");
    static char line[1 << 16];
    unsigned long jit_lo = 0, jit_hi = 0;
    unsigned long inner[4096], outer[4096], gamet[4096];
    int ninner = 0, nouter = 0, ngame = 0, header = 0, main_idx = -1, cpu_main = 0, end = 0, swaps = 0, mains = 0, seqs = 0;
    long main_samples = 0, r = 0, s_blocked = 0, in_inner = 0, with_outer = 0, with_game = 0, in_jit = 0, unwound = 0;
    double secs = 0;
    // pass 1: names
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b, c;
        char n[256];
        if (!strncmp(line, "ua_prof 3", 9)) header = 1;
        if (sscanf(line, "secs %lf", &secs) == 1) continue;
        if (sscanf(line, "jit %lx %lx %255s", &a, &b, n) == 3 && !strcmp(n, "M:Fake.Jit:Loop")) { jit_lo = a; jit_hi = a + b; }
        if (sscanf(line, "sym %lx %255s %lx", &a, n, &c) == 3) {
            if (!strcmp(n, "spin_inner") && ninner < 4096) inner[ninner++] = a;
            if (!strcmp(n, "spin_outer") && nouter < 4096) outer[nouter++] = a;
            if (!strcmp(n, "game_thread") && ngame < 4096) gamet[ngame++] = a;
        }
        int i, tid; unsigned smp, drop;
        if (sscanf(line, "thread %d %d %255s %u %u", &i, &tid, n, &smp, &drop) == 5 && !strcmp(n, "UnityMain")) { main_idx = i; main_samples = smp; }
        if (sscanf(line, "cpu %d %255s", &tid, n) == 2 && !strcmp(n, "UnityMain")) cpu_main = 1;
        if (!strncmp(line, "end", 3)) end = 1;
    }
    rewind(f);
    // pass 2: the stacks of UnityMain
    while (fgets(line, sizeof line, f)) {
        int id, th; unsigned cnt, exact; char st; unsigned sys; unsigned long pc, lr;
        int used;
        if (!strncmp(line, "swap ", 5)) { for (char *p = line + 5; *p; p++) if (*p == ' ') swaps++; swaps++; continue; }
        if (!strncmp(line, "main ", 5)) { for (char *p = line + 5; *p; p++) if (*p == ' ') mains++; mains++; continue; }
        if (!strncmp(line, "seq ", 4) && atoi(line + 4) == main_idx) { for (char *p = line + 4; *p; p++) if (*p == ':') seqs++; continue; }
        if (sscanf(line, "stack %d %u %d %c %x %lx %lx %u%n", &id, &cnt, &th, &st, &sys, &pc, &lr, &exact, &used) != 8 || th != main_idx) continue;
        if (st == 'R') r += cnt; else if (st == 'S' || st == 'D') s_blocked += cnt;
        if (st != 'R') continue;
        int is_inner = 0, is_outer = 0, is_game = 0;
        for (int k = 0; k < ninner; k++) if (inner[k] == pc) is_inner = 1;
        if (pc >= jit_lo && pc < jit_hi) in_jit += cnt;
        if (!is_inner) continue;
        in_inner += cnt;
        for (int k = 0; k < nouter; k++) if (outer[k] == lr) is_outer = 1;
        for (char *p = line + used; *p; ) {
            char *e;
            while (*p == ' ' || *p == '~') p++;             // (~: found by scanning)
            unsigned long v = strtoul(p, &e, 16);
            if (e == p) break;
            for (int k = 0; k < nouter; k++) if (outer[k] == v) is_outer = 1;
            for (int k = 0; k < ngame; k++) if (gamet[k] == v) is_game = 1;
            p = e;
        }
        if (is_outer) with_outer += cnt;
        if (is_game) with_game += cnt;
        if (exact >= 2) unwound += cnt;
    }
    fclose(f);
    printf("test_prof: %.2f s, UnityMain %ld samples: R %ld (spin_inner %ld, called from spin_outer %ld, game_thread on the stack %ld, unwound %ld; JIT %ld), blocked %ld; frames %d swap / %d main\n",
           secs, main_samples, r, in_inner, with_outer, with_game, unwound, in_jit, s_blocked, swaps, mains);
    if (!header || !end) return fail("header or end line missing");
    if (secs < want_secs * 0.8 || secs > want_secs * 3) return fail("recording length");
    if (main_idx < 0 || main_samples < 40) return fail("too few UnityMain samples");
    if (seqs != main_samples) return fail("seq entries != samples");
    if (!cpu_main) return fail("no cpu line for UnityMain");
    if (!jit_lo) return fail("the JIT method was not named");
    if (in_inner * 10 < r * 2) return fail("spin_inner under 20% of the running samples");
    if (with_outer * 2 < in_inner) return fail("spin_outer (lr or stack) missing from most spin_inner samples");
    if (with_game * 2 < in_inner) return fail("game_thread missing from the stack of most spin_inner samples");
#if defined(__arm__)
    if (unwound * 2 < in_inner) return fail("most spin_inner samples were not unwound (EHABI)");
#elif defined(__aarch64__)
    if (unwound * 2 < in_inner) return fail("most spin_inner samples were not unwound (frame pointers)");
#endif
    if (in_jit * 20 < r) return fail("the JIT loop under 5% of the running samples");
    if (s_blocked * 20 < main_samples) return fail("no blocked samples (UnityMain sleeps ~30%)");
    if (swaps < 3 || mains < 3) return fail("frame times missing");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_prof on|req|off <dir>\n"); return 2; }
    const char *mode = argv[1], *dir = argv[2];
    if (!strcmp(mode, "spin")) {
        int hz = argc > 3 ? atoi(argv[3]) : 0;
        double c0 = now();
        spin_inner(2000000);
        g_per_iter = (now() - c0) / 2000000;
        char b[16];
        snprintf(b, sizeof b, "%d", hz);
        if (hz > 0) { setenv("UA_PROF", b, 1); setenv("UA_PROF_SECS", "3", 1); setenv("UA_PROF_START", "0", 1); }
        prof_init(dir);
        prof_swap();                    // the first frame starts the recording
        pthread_t th;
        pthread_create(&th, NULL, spin_thread, NULL);
        pthread_join(th, NULL);
        printf("test_prof spin %d Hz: %.0f iterations/s\n", hz, g_iters / 3.0);
        g_finishing = 1;
        return 0;
    }
    char path[600], req[600];
    snprintf(path, sizeof path, "%s/prof-1.txt", dir);
    snprintf(req, sizeof req, "%s/prof.req", dir);
    unlink(path);
    make_jit();
    double c0 = now();
    spin_inner(2000000);
    g_per_iter = (now() - c0) / 2000000;
    if (!strcmp(mode, "on")) { setenv("UA_PROF", "500", 1); setenv("UA_PROF_SECS", "1", 1); setenv("UA_PROF_START", "0.3", 1); }
    else if (!strcmp(mode, "req")) { setenv("UA_PROF", "400", 1); unsetenv("UA_PROF_START"); }
    else unsetenv("UA_PROF");
    prof_init(dir);
    pthread_t g, rt, o;
    pthread_create(&g, NULL, game_thread, NULL);
    pthread_create(&rt, NULL, render, NULL);
    pthread_create(&o, NULL, other, NULL);
    if (!strcmp(mode, "req")) {
        struct timespec s = { 0, 500000000 }; nanosleep(&s, NULL);
        FILE *f = fopen(req, "w"); if (f) { fputs("0.8\n", f); fclose(f); }
    }
    double t0 = now(), limit = !strcmp(mode, "off") ? 2 : 60;
    while (now() - t0 < limit && access(path, F_OK) != 0) { struct timespec s = { 0, 50000000 }; nanosleep(&s, NULL); }
    int ua_prof = has_ua_prof_thread();
    g_stop = 1;
    pthread_join(g, NULL); pthread_join(rt, NULL); pthread_join(o, NULL);
    g_finishing = 1;
    if (!strcmp(mode, "off")) {
        if (ua_prof) return fail("off: a sampler thread runs");
        if (access(path, F_OK) == 0) return fail("off: a profile was written");
        puts("test_prof off: ok");
        return 0;
    }
    if (!ua_prof) return fail("no sampler thread");
    if (!strcmp(mode, "req") && access(req, F_OK) == 0) return fail("prof.req not taken");
    int rc = check(path, !strcmp(mode, "req") ? 0.8 : 1.0);
    if (!rc) printf("test_prof %s: ok\n", mode);
    return rc;
}
