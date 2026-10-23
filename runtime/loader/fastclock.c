/*
 * fastclock.c - clock_gettime/gettimeofday for the Android libraries without a system call, on
 * 32-bit ARM kernels that give 32-bit processes no vDSO.
 *
 * Android's kernels give 32-bit apps on a 64-bit kernel a vDSO (Android's arm64 kernels carried
 * the compat vDSO long before Linux 5.3 had it): clock_gettime reads the ARM generic timer's
 * virtual counter (CNTVCT) and scales it, with no system call. A 64-bit Linux kernel before 5.3
 * has no vDSO for 32-bit processes (the RG34XX-SP's 4.9 kernel: AT_SYSINFO_EHDR is 0), so every
 * clock_gettime of a 32-bit player is a system call: about 1.07 us on its Cortex-A53, against
 * 15 ns for a counter read. Unity, Mono and FMOD ask the time a few hundred times a frame.
 *
 * Here, as in the vDSO: each clock is a line through the counter, ns = ns0 + (c - c0) * mult,
 * published under a sequence count. Its slope is the kernel's own rate: every period (0.25 s) the
 * first caller after it asks the kernel (one system call, with the counter read just before and
 * after: a pair more than 50 us apart is not used) and starts a new line at the value the old one
 * gives then, with the slope that meets the kernel's clock by the end of the next period. The
 * clock stays continuous and monotonic, and within a few microseconds of the kernel's (it follows
 * NTP's slewing; the RG34XX-SP's counter and CLOCK_MONOTONIC differ by about 2 ppm). A clock found
 * more than 2 ms off the kernel's (CLOCK_REALTIME set, CLOCK_BOOTTIME after a suspend) is set to
 * it at once: CLOCK_REALTIME may step at any time anyway, and the monotonic clocks only step
 * forward that way. CLOCK_MONOTONIC, _RAW, _COARSE, CLOCK_BOOTTIME, CLOCK_REALTIME and its
 * _COARSE go through the counter (a coarse clock may be more precise than a tick); every other
 * clock (CPU time clocks, ...) is the system call it was.
 *
 * Off by default: on the RG34XX-SP it removed about 6,000 system calls a second from Hollow Knight
 * without a frame rate gain that could be measured (docs/profiling.md). UA_FAST_CLOCK=1 uses it when:
 * 32-bit ARM, no vDSO, the counter readable from user space (a probe under a SIGILL handler) and its
 * frequency plausible, and the counter keeps pace with the kernel's clock over a short check;
 * =force also where there is a vDSO (tests). UA_CLOCK_STATS=<s> logs the calls per clock every <s> s.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fastclock.h"

/* bionic's 32-bit timespec/timeval: two longs */
struct b_timespec { long tv_sec; long tv_nsec; };
struct b_timeval { long tv_sec; long tv_usec; };

#define NCLK 8            /* clock ids 0..7 */
#define SHIFT 26          /* mult: ns per tick << SHIFT (24 MHz: 41.67 ns -> 2.8e9, fits 32 bits) */

struct slot {
    atomic_uint seq;      /* odd while the writer fills it */
    uint32_t mult;
    uint64_t c0;
    int64_t sec0;         /* the value at c0, as seconds and nanoseconds (no 64-bit division when read) */
    uint32_t nsec0;
};

/* two slots: the writer fills the one readers are not told to use, then switches them over, so
   a reader never waits for a writer (which may be preempted at any point) */
struct line {
    struct slot slot[2];
    atomic_uint cur;      /* the slot readers use; 0 until the first line */
    atomic_flag busy;     /* a writer */
    int64_t k_ns;         /* the kernel's value at the last resync, and the counter then */
    uint64_t k_c;
    int valid;
};

static struct line lines[NCLK];
static int g_on = -1;               /* -1: not decided, 0: off, 1: on */
static uint32_t g_freq;
static uint64_t g_period;           /* ticks between resyncs */
static uint64_t g_maxgap;           /* a read pair longer than this is not used */
static uint64_t g_far;              /* past this many ticks from c0 a line is not extrapolated */
static uint32_t g_nominal;          /* 1e9 / freq << SHIFT */
static int g_stats;
static atomic_uint g_calls[NCLK + 1], g_sys[NCLK + 1];

#if defined(__arm__)
static inline uint64_t counter(void)
{
    uint32_t lo, hi;
    __asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint32_t counter_freq(void)
{
    uint32_t f;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f));
    return f;
}
#else
static inline uint64_t counter(void) { return 0; }
static inline uint32_t counter_freq(void) { return 0; }
#endif

static int kernel_clock(clockid_t clk, int64_t *ns)
{
    struct timespec t;
    if (syscall(SYS_clock_gettime, clk, &t)) return -1;
    *ns = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
    return 0;
}

static int fast_clock(clockid_t clk)
{
    switch (clk) {
    case CLOCK_REALTIME: case CLOCK_MONOTONIC: case CLOCK_MONOTONIC_RAW: case CLOCK_REALTIME_COARSE:
    case CLOCK_MONOTONIC_COARSE: case CLOCK_BOOTTIME: return 1;
    }
    return 0;
}

/* the coarse clocks share their precise clock's line */
static int line_of(clockid_t clk)
{
    if (clk == CLOCK_REALTIME_COARSE) return CLOCK_REALTIME;
    if (clk == CLOCK_MONOTONIC_COARSE) return CLOCK_MONOTONIC;
    return clk;
}

/* the line's value at c (c - c0 at most g_far: the offset fits 32 bits) */
static inline void eval(uint64_t c, const struct slot *S, int64_t *sec, uint32_t *nsec)
{
    uint64_t dt = c > S->c0 ? c - S->c0 : 0;
    uint64_t n = (uint64_t)S->nsec0 + (uint32_t)((dt * S->mult) >> SHIFT);
    int64_t s = S->sec0;
    while (n >= 1000000000u) { n -= 1000000000u; s++; }
    *sec = s; *nsec = (uint32_t)n;
}

static inline int64_t eval_ns(uint64_t c, const struct slot *S)
{
    int64_t sec; uint32_t nsec;
    eval(c, S, &sec, &nsec);
    return sec * 1000000000 + nsec;
}

/* the kernel's value with the counter's midpoint; 0 if the pair was too far apart */
static int sample(clockid_t clk, int64_t *k, uint64_t *c)
{
    for (int i = 0; i < 3; i++) {
        uint64_t a = counter();
        if (kernel_clock(clk, k)) return 0;
        uint64_t b = counter();
        if (b - a <= g_maxgap) { *c = a + (b - a) / 2; return 1; }
    }
    return 0;
}

static void publish(struct line *L, uint64_t c0, int64_t ns0, uint32_t mult)
{
    unsigned cur = atomic_load_explicit(&L->cur, memory_order_relaxed);
    unsigned next = cur ? (cur & 1) ^ 1 : 0;
    struct slot *S = &L->slot[next];
    unsigned s = atomic_load_explicit(&S->seq, memory_order_relaxed);
    atomic_store_explicit(&S->seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    S->c0 = c0; S->sec0 = ns0 / 1000000000; S->nsec0 = (uint32_t)(ns0 % 1000000000); S->mult = mult;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&S->seq, s + 2, memory_order_relaxed);
    atomic_store_explicit(&L->cur, next | 2, memory_order_release);   /* bit 1: there is a line */
}

/* a new line from now on (the caller holds L->busy; old: the line readers use, if L->valid) */
static __attribute__((noinline)) void resync(int idx, const struct slot *old)
{
    struct line *L = &lines[idx];
    int64_t k; uint64_t c;
    if (!sample(idx, &k, &c)) return;
    if (g_stats) atomic_fetch_add_explicit(&g_sys[idx], 1, memory_order_relaxed);
    /* the kernel's rate over the last period (ns per tick << SHIFT), else the nominal one */
    uint64_t rate = g_nominal;
    if (L->valid && c > L->k_c + g_period / 2 && k > L->k_ns) {
        uint64_t r = ((uint64_t)(k - L->k_ns) << SHIFT) / (c - L->k_c);   /* < 4 s << 26: no overflow */
        if (r > g_nominal - g_nominal / 64 && r < g_nominal + g_nominal / 64) rate = r;
    }
    L->k_ns = k; L->k_c = c;
    int64_t ours = L->valid ? eval_ns(c, old) : k;
    int64_t err = k - ours;
    uint32_t mult;
    if (!L->valid || err > 2000000 || (err < -2000000 && (idx == CLOCK_REALTIME))) {
        /* first line, a step forward, or CLOCK_REALTIME set back: the kernel's value */
        ours = k; mult = (uint32_t)rate;
    } else {
        /* meet the kernel's clock at the end of the next period: slope = rate + err / period,
           at least half the nominal rate (a clock ahead slows down, never stops or goes back) */
        int64_t m = (int64_t)rate + (err * (1 << SHIFT)) / (int64_t)g_period;   /* |err| <= 2 ms */
        if (m < (int64_t)(g_nominal / 2)) m = g_nominal / 2;
        if (m > (int64_t)g_nominal * 3 / 2) m = (int64_t)g_nominal * 3 / 2;
        mult = (uint32_t)m;
    }
    publish(L, c, ours, mult);
    L->valid = 1;
}

static void stats_tick(void)
{
    static atomic_ullong next;
    static uint64_t last_c;
    uint64_t c = counter(), n = atomic_load_explicit(&next, memory_order_relaxed);
    if (c < n) return;
    if (!atomic_compare_exchange_strong(&next, &n, c + (uint64_t)g_stats * g_freq)) return;
    if (!n) { last_c = c; return; }
    double secs = (double)(c - last_c) / g_freq;
    last_c = c;
    char buf[256]; int o = 0;
    static const char *names[NCLK + 1] = { "realtime", "monotonic", "process-cpu", "thread-cpu", "monotonic-raw",
                                            "realtime-coarse", "monotonic-coarse", "boottime", "other" };
    for (int i = 0; i <= NCLK; i++) {
        unsigned v = atomic_exchange_explicit(&g_calls[i], 0, memory_order_relaxed);
        unsigned s = atomic_exchange_explicit(&g_sys[i], 0, memory_order_relaxed);
        if (v) o += snprintf(buf + o, sizeof buf - o, " %s %.0f/s (%u system calls)", names[i], v / secs, s);
        if (o >= (int)sizeof buf) break;
    }
    fprintf(stderr, "[clock] %s:%s\n", g_on ? "counter" : "system calls", o ? buf : " no calls");
}

#if defined(__arm__)
static sigjmp_buf probe_jb;
static void probe_ill(int sig) { (void)sig; siglongjmp(probe_jb, 1); }
#endif

static int decide(void)
{
    const char *e = getenv("UA_FAST_CLOCK");
    const char *st = getenv("UA_CLOCK_STATS");
    g_stats = st && *st ? atoi(st) : 0;
    if (g_stats < 0) g_stats = 0;
#if !defined(__arm__)
    (void)e;
    return 0;
#else
    /* off unless asked for: 1 where there is no vDSO, force also with one (tests) */
    int force = e && !strcmp(e, "force"), off = !force && !(e && *e == '1');
    if (off && !g_stats) return 0;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_ill;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGILL, &sa, &old)) return 0;
    volatile int ok = 0;
    if (!sigsetjmp(probe_jb, 1)) { g_freq = counter_freq(); (void)counter(); ok = 1; }
    sigaction(SIGILL, &old, NULL);
    if (!ok || g_freq < 1000000 || g_freq > 1000000000) {
        if (!getauxval(AT_SYSINFO_EHDR))
            fprintf(stderr, "[clock] no vDSO, and the timer's counter %s: clock_gettime stays a system call\n",
                    ok ? "has an implausible frequency" : "cannot be read here");
        g_freq = 0;
        return 0;
    }
    g_nominal = (uint32_t)((1000000000ull << SHIFT) / g_freq);
    g_period = g_freq / 4;
    g_maxgap = g_freq / 20000;
    if (g_maxgap < 2) g_maxgap = 2;
    g_far = (uint64_t)g_freq * 4;
    if (off || (!force && getauxval(AT_SYSINFO_EHDR))) return 0;     /* off, or the kernel's vDSO is there */
    /* the counter keeps pace with CLOCK_MONOTONIC_RAW over 10 ms (within 1%) */
    int64_t k0, k1; uint64_t c0, c1;
    if (!sample(CLOCK_MONOTONIC_RAW, &k0, &c0)) return 0;
    struct timespec d = { 0, 10000000 };
    nanosleep(&d, NULL);
    if (!sample(CLOCK_MONOTONIC_RAW, &k1, &c1)) return 0;
    double kn = (double)(k1 - k0), cn = (double)(c1 - c0) * 1e9 / g_freq;
    if (kn <= 0 || cn < kn * 0.99 || cn > kn * 1.01) {
        fprintf(stderr, "[clock] the timer's counter does not keep pace with the kernel's clock (%.0f ns against %.0f ns): "
                "clock_gettime stays a system call\n", cn, kn);
        return 0;
    }
    fprintf(stderr, "[clock] no vDSO for 32-bit processes: clock_gettime reads the timer's counter (%u Hz) instead of "
            "making a system call%s\n", g_freq, force ? " (UA_FAST_CLOCK=force)" : "");
    return 1;
#endif
}

void fastclock_init(void)
{
    if (g_on < 0) g_on = decide();
}

int fastclock_on(void) { if (g_on < 0) fastclock_init(); return g_on; }

/* the slow path of now(): no line yet, or the line is a period old */
static __attribute__((noinline)) int renew(int idx, unsigned cur, const struct slot *copy, uint64_t c,
                                          int64_t *sec, uint32_t *nsec)
{
    struct line *L = &lines[idx];
    /* the first caller makes a new line */
    if (!atomic_flag_test_and_set_explicit(&L->busy, memory_order_acquire)) {
        if (atomic_load_explicit(&L->cur, memory_order_acquire) == cur) {
            if (cur && c - copy->c0 > g_far) L->valid = 0;   /* not asked for seconds: the kernel's value anew */
            resync(idx, copy);
        }
        atomic_flag_clear_explicit(&L->busy, memory_order_release);
        if (atomic_load_explicit(&L->cur, memory_order_acquire) != cur) return 1;   /* read the new line */
    }
    /* another thread (or this one, interrupted by a signal handler) is making it: the old line while
       it is good, else the kernel's value */
    if (cur && c - copy->c0 <= g_far) { eval(c, copy, sec, nsec); return 0; }
    int64_t k;
    if (kernel_clock(idx, &k)) return -1;
    *sec = k / 1000000000; *nsec = (uint32_t)(k % 1000000000);
    return 0;
}

/* the clock's value now; -1 if even the kernel could not tell */
static inline int now(int idx, int64_t *sec, uint32_t *nsec)
{
    struct line *L = &lines[idx];
    for (;;) {
        unsigned cur = atomic_load_explicit(&L->cur, memory_order_relaxed);
        struct slot copy = { 0 };
        uint64_t c = 0;
        if (cur) {
            /* the slot's address depends on cur, and the barrier orders seq before the fields */
            const struct slot *S = &L->slot[cur & 1];
            unsigned s1 = atomic_load_explicit(&S->seq, memory_order_relaxed);
            atomic_thread_fence(memory_order_acquire);
            copy.c0 = S->c0; copy.sec0 = S->sec0; copy.nsec0 = S->nsec0; copy.mult = S->mult;
            atomic_thread_fence(memory_order_acquire);
            c = counter();
            unsigned s2 = atomic_load_explicit(&S->seq, memory_order_relaxed);
            if ((s1 & 1) || s1 != s2) continue;      /* the writer came round to this slot again: reread */
            if (__builtin_expect(c <= copy.c0 || c - copy.c0 <= g_period, 1)) { eval(c, &copy, sec, nsec); return 0; }
        }
        int r = renew(idx, cur, &copy, c, sec, nsec);
        if (r <= 0) return r;
    }
}

int fastclock_gettime(clockid_t clk, void *tsp)
{
    struct b_timespec *ts = tsp;
    if (g_stats) {
        atomic_fetch_add_explicit(&g_calls[(unsigned)clk < NCLK ? (unsigned)clk : NCLK], 1, memory_order_relaxed);
        if (g_freq) stats_tick();
    }
    if (g_on > 0 && ts && fast_clock(clk)) {
        int64_t sec; uint32_t nsec;
        if (!now(line_of(clk), &sec, &nsec)) {
            ts->tv_sec = (long)sec;
            ts->tv_nsec = (long)nsec;
            return 0;
        }
    }
    if (g_stats) atomic_fetch_add_explicit(&g_sys[(unsigned)clk < NCLK ? (unsigned)clk : NCLK], 1, memory_order_relaxed);
    return clock_gettime(clk, (struct timespec *)tsp);
}

int fastclock_gettimeofday(void *tvp, void *tz)
{
    struct b_timeval *tv = tvp;
    if (g_stats) {
        atomic_fetch_add_explicit(&g_calls[CLOCK_REALTIME], 1, memory_order_relaxed);
        if (g_freq) stats_tick();
    }
    if (g_on > 0 && tv && !tz) {
        int64_t sec; uint32_t nsec;
        if (!now(CLOCK_REALTIME, &sec, &nsec)) {
            tv->tv_sec = (long)sec;
            tv->tv_usec = (long)(nsec / 1000);
            return 0;
        }
    }
    return gettimeofday((struct timeval *)tvp, tz);
}
