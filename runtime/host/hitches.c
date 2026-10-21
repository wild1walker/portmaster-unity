// Hitch sampler: where the game is while a frame takes too long.
//
//   UA_HITCH_TRACE=<ms>        once a frame has taken longer than <ms>, the threads below are
//                              sampled every UA_HITCH_EVERY ms (default 20) until the frame ends
//   UA_HITCH_THREADS=a,b       threads to sample, by name (default UnityMain)
//   UA_PROF_EVERY=<ms>         profiling: sample those threads every <ms> all the time, logged as
//                              "[prof] <thread> pc ... | ..." (host/hitchsum.py --prof sums them per boss state)
//
// A sample stops the thread in a signal handler while a watchdog thread copies its stack, then
// logs one line: "[hitch-sample] +<ms into the frame> <thread> pc <module+off> lr <module+off> |
// <return addresses>". The return addresses are the stack words that point just past a call
// instruction (ARM/Thumb BL/BLX, A64 BL/BLR) in a mapped module (so: real calls, a few stale
// ones). host/sym.py names libunity.so offsets. When the frame ends: "[hitch-sample] frame of
// <ms> ms, <n> samples". Frames in Mono's JIT code (anonymous executable memory) are named by
// Mono itself, as "M:<Namespace.Class:Method>" (the watchdog thread attaches to Mono's root
// domain for that).
//
// Always on: kill -USR2 <pid> logs every thread's stack once ("[dump] <thread> pc ... | ..."), e.g. to
// see where a hung process waits. Mono 4.x (libmonobdwgc: 2018.4+ .NET 4.x and every 2019.4+ Mono game)
// takes SIGUSR1/SIGUSR2 for its own thread suspension once it starts (SIGUSR2 resumes a suspended
// thread; on a thread Mono does not know it aborts: "may have been prematurely finalized"): for those,
// kill -s RTMIN+7 <pid> (41 with glibc) dumps the same.
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "hitches.h"
#include "arch.h"
#include "../jni/unityplayer.h"
#include "../loader/so_loader.h"

#define MAX_THREADS 8
#define STACK_WORDS 2048        // pointer-sized words: 8 KB of stack on ARM, 16 KB on AArch64
#define MAX_FRAMES 28

static int g_thresh_ms, g_every_ms = 20, g_prof_ms;
static char g_names[MAX_THREADS][16];
static int g_nnames;
static atomic_llong g_last_swap;        // ns, CLOCK_MONOTONIC
static int g_sig;

// handshake with the handler: 1 = registers stored, 2 = stack copied (handler may return)
static atomic_int g_state;
static volatile uintptr_t g_pc, g_lr, g_sp;

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ll + t.tv_nsec;
}

static void on_sample(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    g_pc = UC_PC(uc);
    g_lr = UC_LR(uc);
    g_sp = UC_SP(uc);
    atomic_store(&g_state, 1);
    long long until = now_ns() + 200000000ll;      // never hold the thread longer than 200 ms
    while (atomic_load(&g_state) != 2 && now_ns() < until) { struct timespec t = { 0, 200000 }; nanosleep(&t, NULL); }
    atomic_store(&g_state, 0);
}

typedef struct { uintptr_t lo, hi, base; char name[48]; } map_ent;
static map_ent g_maps[256];
static int g_nmaps;

static void read_maps(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    g_nmaps = 0;
    if (!f) return;
    struct { char path[256]; uintptr_t base; } firsts[64];
    int nf = 0;
    while (fgets(line, sizeof line, f) && g_nmaps < 256) {
        unsigned long lo, hi, off;
        char perms[8], path[256] = "";
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %255[^\n]", &lo, &hi, perms, &off, path) < 4 || path[0] == '[') continue;
        if (!path[0]) {                  // anonymous executable memory: Mono's JIT code, trampolines
            if (!strchr(perms, 'x')) continue;
            map_ent *m = &g_maps[g_nmaps++];
            m->lo = lo; m->hi = hi; m->base = lo; m->name[0] = 0;
            continue;
        }
        int k;
        for (k = 0; k < nf && strcmp(firsts[k].path, path); k++) {}
        if (k == nf && nf < 64 && off == 0) { snprintf(firsts[nf].path, sizeof firsts[nf].path, "%s", path); firsts[nf].base = lo; nf++; }
        if (!strchr(perms, 'x')) continue;
        map_ent *m = &g_maps[g_nmaps++];
        m->lo = lo; m->hi = hi; m->base = lo - off;
        for (k = 0; k < nf; k++) if (!strcmp(firsts[k].path, path)) m->base = firsts[k].base;
        const char *b = strrchr(path, '/');
        snprintf(m->name, sizeof m->name, "%.47s", b ? b + 1 : path);
    }
    fclose(f);
}

// end of the mapping (any permissions) that holds address v, 0 if none
static uintptr_t mapping_end(uintptr_t v)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    uintptr_t end = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 && v >= lo && v < hi) { if (perms[0] == 'r') end = hi; break; }
    }
    fclose(f);
    return end;
}

static const map_ent *find_map(uintptr_t v)
{
    for (int i = 0; i < g_nmaps; i++) if (v >= g_maps[i].lo && v < g_maps[i].hi) return &g_maps[i];
    return NULL;
}

#if defined(__aarch64__)
// v is a return address: the instruction before it is a BL or BLR (or the pointer-authenticating
// BLRAA/BLRAB forms)
static int after_call(uintptr_t v, const map_ent *m)
{
    if (!code_aligned(v) || v < m->lo + 4) return 0;
    uint32_t i = *(const uint32_t *)(v - 4);
    return (i & 0xFC000000) == 0x94000000          // BL imm26
        || (i & 0xFFFFFC1F) == 0xD63F0000          // BLR Xn
        || (i & 0xFEFFF800) == 0xD63F0800;         // BLRAA/BLRAAZ/BLRAB/BLRABZ
}
#else
// v is a return address: the instruction before it is a BL/BLX (ARM or Thumb)
static int after_call(uintptr_t v, const map_ent *m)
{
    if (v & 1) {
        uintptr_t a = v - 1;
        if (a < m->lo + 4) return 0;
        uint16_t h1 = *(const uint16_t *)(a - 4), h2 = *(const uint16_t *)(a - 2);
        if ((h1 & 0xF800) == 0xF000 && (h2 & 0xC000) == 0xC000) return 1;      // BL / BLX imm
        return (h2 & 0xFF87) == 0x4780;                                        // BLX reg
    }
    if ((v & 3) || v < m->lo + 4) return 0;
    uint32_t i = *(const uint32_t *)(v - 4);
    return (i & 0x0F000000) == 0x0B000000 || (i & 0xFE000000) == 0xFA000000 || (i & 0x0FFFFFF0) == 0x012FFF30;
}
#endif

// Mono, for JIT code: loaded lazily from the Android libmono.so (pointer arguments only: no softfp issue)
static void *(*mono_root)(void);
static void *(*mono_attach)(void *);
static void (*mono_detach)(void *);
static void *(*mono_ji_find)(void *, char *);
static void *(*mono_ji_method)(void *);
static char *(*mono_name)(void *, int);
static void (*mono_free_)(void *);
static void *g_domain;

static void mono_setup(void)
{
    if (g_domain) return;
    if (!mono_root) {
        void *h = so_dlopen("libmono.so", 4 /* RTLD_NOLOAD */);
        if (!h) return;
        mono_root = so_dlsym(h, "mono_get_root_domain"); mono_attach = so_dlsym(h, "mono_thread_attach");
        mono_detach = so_dlsym(h, "mono_thread_detach");
        mono_ji_find = so_dlsym(h, "mono_jit_info_table_find"); mono_ji_method = so_dlsym(h, "mono_jit_info_get_method");
        mono_name = so_dlsym(h, "mono_method_full_name"); mono_free_ = so_dlsym(h, "mono_free");
        if (!mono_root || !mono_attach || !mono_ji_find || !mono_ji_method || !mono_name || !mono_free_) { mono_root = NULL; return; }
    }
    void *d = mono_root();
    if (d) g_domain = d;
}

// Attached to Mono only while naming frames: Mono's shutdown waits for every attached thread.
static void *g_mthread;
static void mono_enter(void) { if (g_domain && !g_mthread) g_mthread = mono_attach(g_domain); }
static void mono_leave(void) { if (g_mthread && mono_detach) { mono_detach(g_mthread); g_mthread = NULL; } }

static int fmt(char *out, size_t n, uintptr_t v)
{
    const map_ent *m = find_map(v);
    if (m && !m->name[0]) {
        void *ji = g_domain ? mono_ji_find(g_domain, (char *)(uintptr_t)v) : NULL;
        char *mn = ji ? mono_name(mono_ji_method(ji), 1) : NULL;
        if (mn) {
            int k = snprintf(out, n, "M:");
            const char *c = mn;
            if (!strncmp(c, "(wrapper ", 9) && strchr(c, ')')) { c = strchr(c, ')') + 1; while (*c == ' ') c++; k = snprintf(out, n, "W:"); }
            for (; *c && *c != '(' && k < (int)n - 2; c++) out[k++] = *c == ' ' ? '_' : *c;
            while (k > 2 && out[k - 1] == '_') k--;
            out[k] = 0;
            mono_free_(mn);
            return k;
        }
        return snprintf(out, n, "anon:0x%lx", (unsigned long)v);
    }
    // Android libraries by the loader's own load bias (ELF addresses, as the symbol tools want):
    // /proc/self/maps cannot tell their images from other mappings of the same file
    uintptr_t bias;
    const char *path = m ? so_module_at((void *)v, &bias) : NULL;
    if (path) {
        const char *b = strrchr(path, '/');
        return snprintf(out, n, "%s+0x%lx", b ? b + 1 : path, (unsigned long)(v - bias));
    }
    return m ? snprintf(out, n, "%s+0x%lx", m->name, (unsigned long)(v - m->base)) : snprintf(out, n, "0x%lx", (unsigned long)v);
}

static int find_threads_all(pid_t *tids, int max)
{
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d)) && n < max) if (e->d_name[0] != '.') tids[n++] = atoi(e->d_name);
    closedir(d);
    return n;
}

static int find_threads(pid_t *tids)
{
    int n = 0;
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d)) && n < MAX_THREADS) {
        if (e->d_name[0] == '.') continue;
        char p[300], name[32] = "";
        snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
        for (int k = 0; k < g_nnames; k++) if (!strcmp(name, g_names[k])) { tids[n++] = atoi(e->d_name); break; }
    }
    closedir(d);
    return n;
}

static void sample(pid_t tid, double ms, int prof)
{
    static uintptr_t stack[STACK_WORDS];
    char name[32] = "?", p[64], line[2048];
    snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    FILE *f = fopen(p, "r");
    if (f) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0; fclose(f); }
    atomic_store(&g_state, 0);
    if (syscall(SYS_tgkill, getpid(), tid, g_sig) != 0) return;
    long long until = now_ns() + 100000000ll;
    while (atomic_load(&g_state) != 1 && now_ns() < until) { struct timespec t = { 0, 100000 }; nanosleep(&t, NULL); }
    if (atomic_load(&g_state) != 1) { fprintf(stderr, "[hitch-sample] +%.0f %s: no answer\n", ms, name); return; }
    uintptr_t pc = g_pc, lr = g_lr, sp = g_sp;
    // the thread waits in the handler: its stack above sp stays as it was; copy it up to the end
    // of the mapping that holds sp
    uintptr_t end = mapping_end(sp);
    long got = end > sp ? (long)(end - sp < sizeof stack ? end - sp : sizeof stack) : 0;
    if (got > 0) memcpy(stack, (const void *)sp, (size_t)got);
    atomic_store(&g_state, 2);           // the thread goes on
    mono_enter();
    int len = prof == 2 ? snprintf(line, sizeof line, "[dump] %d %s pc ", tid, name) : prof ? snprintf(line, sizeof line, "[prof] %s pc ", name)
            : snprintf(line, sizeof line, "[hitch-sample] +%.0f %s pc ", ms, name);
    len += fmt(line + len, sizeof line - len, pc);
    len += snprintf(line + len, sizeof line - len, " lr ");
    len += fmt(line + len, sizeof line - len, lr);
    len += snprintf(line + len, sizeof line - len, " |");
    int frames = 0;
    for (long i = 0; got > 0 && i < got / (long)sizeof stack[0] && frames < MAX_FRAMES && len < (int)sizeof line - 64; i++) {
        const map_ent *m = find_map(stack[i]);
        if (!m || !after_call(stack[i], m)) continue;
        line[len++] = ' ';
        len += fmt(line + len, sizeof line - len, stack[i]);
        frames++;
    }
    fprintf(stderr, "%s\n", line);
    mono_leave();
}

static void *watchdog(void *arg)
{
    (void)arg;
    pid_t tids[MAX_THREADS];
    int ntids = 0, samples = 0;
    long long sampled_frame = 0, next = 0, found_at = 0;
    long long prof_next = 0, prof_found = 0;
    pid_t ptids[MAX_THREADS];
    int nptids = 0;
    for (;;) {
        struct timespec t = { 0, (g_prof_ms ? 2 : 5) * 1000000 };
        nanosleep(&t, NULL);
        long long last = atomic_load(&g_last_swap), now = now_ns();
        if (!last) continue;
        if (up_is_finishing()) return NULL;     // Unity is shutting down (Mono too): hands off
        if (g_prof_ms && now >= prof_next) {
            prof_next = now + g_prof_ms * 1000000ll;
            if (!nptids || now - prof_found > 5000000000ll) { mono_setup(); read_maps(); nptids = find_threads(ptids); prof_found = now; }
            for (int i = 0; i < nptids; i++) sample(ptids[i], 0, 1);
        }
        if (!g_thresh_ms) continue;
        if (sampled_frame && sampled_frame != last) {
            fprintf(stderr, "[hitch-sample] frame of %.0f ms, %d samples\n", (last - sampled_frame) / 1e6, samples);
            sampled_frame = 0; samples = 0;
        }
        if (now - last < g_thresh_ms * 1000000ll || now < next || samples >= 200) continue;
        if (!sampled_frame) {
            sampled_frame = last;
            mono_setup();
            read_maps();
            if (!ntids || now - found_at > 5000000000ll) { ntids = find_threads(tids); found_at = now; }
        }
        for (int i = 0; i < ntids; i++) sample(tids[i], (now_ns() - last) / 1e6, 0);
        samples++;
        next = now + g_every_ms * 1000000ll;
    }
    return NULL;
}

static int g_dump_pipe[2] = { -1, -1 };
static void on_dump(int sig) { (void)sig; char c = 1; if (write(g_dump_pipe[1], &c, 1) < 0) {} }

static void *dumper(void *arg)
{
    (void)arg;
    char c;
    while (read(g_dump_pipe[0], &c, 1) == 1) {
        pid_t tids[128];
        pid_t self = (pid_t)syscall(SYS_gettid);
        mono_setup();
        read_maps();
        int n = find_threads_all(tids, 128);
        fprintf(stderr, "[dump] %d threads\n", n);
        for (int i = 0; i < n; i++) if (tids[i] != self) sample(tids[i], 0, 2);
        fprintf(stderr, "[dump] done\n");
    }
    return NULL;
}

static void install_sampler(void)
{
    static int done;
    if (done) return;
    done = 1;
    g_sig = SIGRTMIN + 6;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(g_sig, &sa, NULL);
}

void hitches_frame(void)
{
    if (g_thresh_ms > 0 || g_prof_ms > 0) atomic_store(&g_last_swap, now_ns());
}

void hitches_init(void)
{
    install_sampler();
    if (pipe(g_dump_pipe) == 0) {
        struct sigaction da;
        memset(&da, 0, sizeof da);
        da.sa_handler = on_dump;
        da.sa_flags = SA_RESTART;
        sigemptyset(&da.sa_mask);
        sigaction(SIGUSR2, &da, NULL);
        sigaction(SIGRTMIN + 7, &da, NULL);     // the one Mono 4.x leaves alone (above)
        pthread_t dt;
        pthread_create(&dt, NULL, dumper, NULL);
    }
    const char *e = getenv("UA_HITCH_TRACE"), *pe = getenv("UA_PROF_EVERY");
    g_thresh_ms = e ? atoi(e) : 0;
    g_prof_ms = pe ? atoi(pe) : 0;
    if (g_thresh_ms <= 0 && g_prof_ms <= 0) { g_thresh_ms = g_prof_ms = 0; return; }
    if (getenv("UA_HITCH_EVERY") && atoi(getenv("UA_HITCH_EVERY")) > 0) g_every_ms = atoi(getenv("UA_HITCH_EVERY"));
    const char *names = getenv("UA_HITCH_THREADS") ? getenv("UA_HITCH_THREADS") : "UnityMain";
    char buf[256];
    snprintf(buf, sizeof buf, "%s", names);
    for (char *s = strtok(buf, ","); s && g_nnames < MAX_THREADS; s = strtok(NULL, ","))
        snprintf(g_names[g_nnames++], sizeof g_names[0], "%.15s", s);
    pthread_t th;
    pthread_create(&th, NULL, watchdog, NULL);
    if (g_thresh_ms) fprintf(stderr, "[hitch-sample] sampling %s in frames over %d ms, every %d ms\n", names, g_thresh_ms, g_every_ms);
    if (g_prof_ms) fprintf(stderr, "[prof] sampling %s every %d ms\n", names, g_prof_ms);
}
