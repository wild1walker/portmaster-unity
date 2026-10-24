// futex.c: the Android libraries' futex system calls, measured and (opt-in) waited for in user space.
//
// Unity's Baselib (2018.3+: its job system's worker lanes, semaphores and events) waits and wakes
// with syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE / FUTEX_WAKE_PRIVATE, ...) through the libc
// import `syscall`, which the loader resolves here (any Unity version, no change to its code). On
// the RG34XX-SP a job worker that runs out of work tries ten times to steal another (a few
// microseconds) and then sleeps in the kernel; each job batch the main thread schedules afterwards
// wakes one with a FUTEX_WAKE: Hollow Knight's UnityMain spends about 1 ms of a 21 ms frame in those
// system calls (docs/profiling.md).
//
//   UA_FUTEX_STATS=<s>   every <s> seconds a [futex] line per thread: waits that slept in the kernel
//                        and for how long, wakes and how many of them woke a thread, the time the
//                        wakes took, and with UA_FUTEX_SPIN what it saved
//   UA_FUTEX_SPIN=<us>   a FUTEX_WAIT first watches the word for up to <us> microseconds before it
//                        sleeps in the kernel, and a FUTEX_WAKE with no thread asleep in the kernel on
//                        that word makes no system call. The callers' results are the same (a futex
//                        wait may always return early: every caller checks its word again); only when
//                        a waiting thread gets its CPU back changes. Off by default (a measurement)
//
// The wake is left out only when no thread waits in the kernel on that word. Each wait that is
// about to sleep registers in a table (by address): it increments the word's count, issues a
// seq_cst fence, and re-reads the word (seq_cst) before the system call; a wake issues a seq_cst
// fence after the caller's own store to the word and then reads the count. The waiter's word read
// and the waker's count read are a store-buffering pair, which the two seq_cst fences forbid both
// sides of: either the wake sees a non-zero count (and makes its call), or the waiter's re-read
// sees the new word and returns without entering the kernel. A waiter whose word already changed
// never sleeps, so a dropped wake is never lost. Waits the small table has no room for, and other
// futex operations (bitset waits, PI locks, requeues) while they wait, raise an "untracked" count
// that turns the skipping off. Nothing is wrapped unless one of the variables is set.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "futex.h"
#include "../loader/armtimer.h"

#if defined(__arm__)
#define NR_FUTEX 240
#define NR_FUTEX64 422            // futex_time64 (32-bit ARM, Linux 5.1+)
#elif defined(__aarch64__)
#define NR_FUTEX 98
#define NR_FUTEX64 98
#else
#define NR_FUTEX SYS_futex
#define NR_FUTEX64 SYS_futex
#endif

// the loader's (hybris_compat.c); absent in the unit test
void bionic_sem_stats(unsigned long long *spun, unsigned long long *slept) __attribute__((weak));

static int g_stats_s, g_spin_us;
static uint64_t g_spin_ticks, g_freq;

// --- the ARM generic timer's virtual counter (../loader/armtimer.h: probed once at start)
#define ticks armtimer_ticks
#define relax armtimer_relax

// --- kernel waiters per futex word
#define TABLE 2048                // power of two; slots are never freed (Baselib's words live as long as the engine)
struct slot { _Atomic(uintptr_t) addr; atomic_int kernel; };
static struct slot g_table[TABLE];
static atomic_int g_untracked;    // kernel waits without a slot, other futex operations waiting: no skipping

static struct slot *slot_for(uintptr_t a, int make)
{
    unsigned h = (unsigned)((a >> 2) * 2654435761u) & (TABLE - 1);
    for (int i = 0; i < 16; i++, h = (h + 1) & (TABLE - 1)) {
        uintptr_t cur = atomic_load_explicit(&g_table[h].addr, memory_order_acquire);
        if (cur == a) return &g_table[h];
        if (!cur) {
            if (!make) return NULL;
            uintptr_t z = 0;
            if (atomic_compare_exchange_strong(&g_table[h].addr, &z, a) || z == a) return &g_table[h];
        }
    }
    return NULL;
}

// --- statistics, per thread
#define MAXT 96
#define NB 8
static const char *const bucket_name[NB] = { "<10us", "<50us", "<100us", "<250us", "<1ms", "<5ms", "<20ms", ">20ms" };
struct tstat {
    int tid;
    uint64_t waits, wait_ticks, sleeps[NB];   // waits that went to the kernel, and how long they slept
    uint64_t spins_won, spin_ticks;           // waits the user-space watch ended (UA_FUTEX_SPIN)
    uint64_t wakes, woke, wake_ticks;         // wake system calls, threads they woke, their time
    uint64_t wakes_skipped;                   // wakes with nobody in the kernel (UA_FUTEX_SPIN)
    uint64_t other;                           // other futex operations
};
static struct tstat g_t[MAXT];
static atomic_int g_nt;
static __thread struct tstat *t_me;
static atomic_flag g_log_busy = ATOMIC_FLAG_INIT;
static _Atomic(uint64_t) g_next_log;

static struct tstat *me(void)
{
    if (t_me) return t_me;
    int i = atomic_fetch_add(&g_nt, 1);
    if (i >= MAXT) { static struct tstat spare; return t_me = &spare; }   // shared, approximate
    g_t[i].tid = (int)syscall(SYS_gettid);
    return t_me = &g_t[i];
}

static int bucket(uint64_t t)
{
    uint64_t us = t * 1000000u / g_freq;
    static const uint64_t lim[NB - 1] = { 10, 50, 100, 250, 1000, 5000, 20000 };
    int b = 0;
    while (b < NB - 1 && us >= lim[b]) b++;
    return b;
}

static void comm_of(int tid, char *out, size_t n)
{
    char p[64];
    snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    FILE *f = fopen(p, "r");
    out[0] = 0;
    if (f) { if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
    if (!out[0]) snprintf(out, n, "(gone)");
}

static struct tstat g_last[MAXT];
static uint64_t g_last_log;

static void log_stats(uint64_t now)
{
    double secs = g_last_log ? (double)(now - g_last_log) / (double)g_freq : 0;
    g_last_log = now;
    int n = atomic_load(&g_nt);
    if (n > MAXT) n = MAXT;
    if (secs <= 0) { for (int i = 0; i < n; i++) g_last[i] = g_t[i]; return; }
    for (int i = 0; i < n; i++) {
        struct tstat c = g_t[i], *l = &g_last[i];
        uint64_t waits = c.waits - l->waits, wakes = c.wakes - l->wakes, skipped = c.wakes_skipped - l->wakes_skipped;
        uint64_t won = c.spins_won - l->spins_won;
        if (!waits && !wakes && !skipped && !won) { *l = c; continue; }
        char name[32], hist[160];
        comm_of(c.tid, name, sizeof name);
        size_t o = 0;
        hist[0] = 0;
        for (int b = 0; b < NB; b++) {
            uint64_t d = c.sleeps[b] - l->sleeps[b];
            if (d) o += (size_t)snprintf(hist + o, sizeof hist - o, " %s %.0f", bucket_name[b], d / secs);
        }
        double us = 1e6 / (double)g_freq;
        fprintf(stderr, "[futex] %-15s %d: waits %.0f/s (sleeping %.1f ms/s;%s), wakes %.0f/s woke %.0f/s in %.2f ms/s"
                " (%.1f us each)%s",
                name, c.tid, waits / secs, (c.wait_ticks - l->wait_ticks) * us / 1000 / secs, o ? hist : "",
                wakes / secs, (c.woke - l->woke) / secs, (c.wake_ticks - l->wake_ticks) * us / 1000 / secs,
                wakes ? (c.wake_ticks - l->wake_ticks) * us / wakes : 0.0, g_spin_us ? "" : "\n");
        if (g_spin_us)
            fprintf(stderr, ", spin: %.0f/s waits ended in user space (%.2f ms/s watching), %.0f/s wakes skipped\n",
                    won / secs, (c.spin_ticks - l->spin_ticks) * us / 1000 / secs, skipped / secs);
        *l = c;
    }
    // Unity 2017.4's job queue sleeps in bionic's sem_wait (the loader's hybris_compat.c), not here
    static unsigned long long last_spun, last_slept;
    if (g_spin_us && bionic_sem_stats) {
        unsigned long long spun, slept;
        bionic_sem_stats(&spun, &slept);
        if (spun != last_spun || slept != last_slept)
            fprintf(stderr, "[futex] sem_wait: %.0f/s ended watching the semaphore, %.0f/s slept\n",
                    (spun - last_spun) / secs, (slept - last_slept) / secs);
        last_spun = spun;
        last_slept = slept;
    }
}

static inline void maybe_log(uint64_t now)
{
    if (!g_stats_s || now < atomic_load_explicit(&g_next_log, memory_order_relaxed)) return;
    if (atomic_flag_test_and_set(&g_log_busy)) return;
    if (now >= atomic_load(&g_next_log)) {
        atomic_store(&g_next_log, now + (uint64_t)g_stats_s * g_freq);
        log_stats(now);
    }
    atomic_flag_clear(&g_log_busy);
}

// --- the wrapper
static long futex_wait(long nr, int *addr, long op, long val, long a4, long a5, long a6)
{
    struct tstat *s = g_stats_s ? me() : NULL;
    uint64_t t0 = s || g_spin_us ? ticks() : 0;
    if (g_spin_us) {
        // watch the word: a store by the waker ends it (the caller checks its word again either way)
        uint64_t end = t0 + g_spin_ticks;
        for (;;) {
            if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != (int)val) {
                if (s) { s->spins_won++; s->spin_ticks += ticks() - t0; }
                errno = EAGAIN;
                return -1;
            }
            relax();
            if (ticks() >= end) break;
        }
    }
    struct slot *sl = g_spin_us ? slot_for((uintptr_t)addr, 1) : NULL;
    if (g_spin_us) {
        // register as a kernel waiter, then re-read the word, both seq_cst: this load and the waker's
        // seq_cst-fenced read of the count are a store-buffering pair that the fences forbid both
        // sides of, so a wake cannot be skipped between here and the sleep. If the word already
        // changed, do not enter the kernel (a wait may return early anyway).
        if (sl) atomic_fetch_add(&sl->kernel, 1); else atomic_fetch_add(&g_untracked, 1);
        atomic_thread_fence(memory_order_seq_cst);
        if (atomic_load_explicit((_Atomic(int) *)addr, memory_order_seq_cst) != (int)val) {
            if (sl) atomic_fetch_sub(&sl->kernel, 1); else atomic_fetch_sub(&g_untracked, 1);
            if (s) { s->spins_won++; s->spin_ticks += ticks() - t0; }
            errno = EAGAIN;
            return -1;
        }
    }
    uint64_t t1 = s ? ticks() : 0;
    if (s && g_spin_us) s->spin_ticks += t1 - t0;
    long r = syscall(nr, addr, op, val, a4, a5, a6);
    int e = errno;
    if (g_spin_us) {
        if (sl) atomic_fetch_sub(&sl->kernel, 1); else atomic_fetch_sub(&g_untracked, 1);
    }
    if (s) {
        uint64_t t2 = ticks();
        s->waits++;
        s->wait_ticks += t2 - t1;
        s->sleeps[bucket(t2 - t1)]++;
        maybe_log(t2);
    }
    errno = e;
    return r;
}

static long futex_wake(long nr, int *addr, long op, long val, long a4, long a5, long a6)
{
    struct tstat *s = g_stats_s ? me() : NULL;
    if (g_spin_us) {
        atomic_thread_fence(memory_order_seq_cst);          // the caller's store to the word, then the counts
        struct slot *sl = slot_for((uintptr_t)addr, 0);
        if (!atomic_load(&g_untracked) && (!sl || !atomic_load(&sl->kernel))) {
            if (s) { s->wakes_skipped++; maybe_log(ticks()); }
            return 0;                                      // nobody to wake in the kernel
        }
    }
    uint64_t t0 = s ? ticks() : 0;
    long r = syscall(nr, addr, op, val, a4, a5, a6);
    if (s) {
        int e = errno;
        uint64_t t1 = ticks();
        s->wakes++;
        if (r > 0) s->woke += (uint64_t)r;
        s->wake_ticks += t1 - t0;
        maybe_log(t1);
        errno = e;
    }
    return r;
}

// syscall(number, ...): six arguments are passed on as the kernel takes them (variadic and fixed
// integer arguments are passed alike on 32-bit ARM's AAPCS and on AArch64)
static long w_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    if (nr == NR_FUTEX || nr == NR_FUTEX64) {
        int cmd = (int)a2 & FUTEX_CMD_MASK;
        if (cmd == FUTEX_WAIT) return futex_wait(nr, (int *)a1, a2, a3, a4, a5, a6);
        if (cmd == FUTEX_WAKE) return futex_wake(nr, (int *)a1, a2, a3, a4, a5, a6);
        if (g_stats_s) me()->other++;
        if (g_spin_us && (cmd == FUTEX_WAIT_BITSET || cmd == FUTEX_LOCK_PI || cmd == FUTEX_WAIT_REQUEUE_PI
#ifdef FUTEX_LOCK_PI2
                          || cmd == FUTEX_LOCK_PI2
#endif
                          )) {
            atomic_fetch_add(&g_untracked, 1);
            long r = syscall(nr, a1, a2, a3, a4, a5, a6);
            int e = errno;
            atomic_fetch_sub(&g_untracked, 1);
            errno = e;
            return r;
        }
    }
    return syscall(nr, a1, a2, a3, a4, a5, a6);
}

static so_shim g_shims[2];

const so_shim *futex_table(void)
{
    const char *st = getenv("UA_FUTEX_STATS"), *sp = getenv("UA_FUTEX_SPIN");
    g_stats_s = st && *st ? atoi(st) : 0;
    g_spin_us = sp && *sp ? atoi(sp) : 0;
    if (g_stats_s < 0) g_stats_s = 0;
    if (g_spin_us < 0) g_spin_us = 0;
    if (g_spin_us > 1000) g_spin_us = 1000;
    memset(g_shims, 0, sizeof g_shims);
    if (!g_stats_s && !g_spin_us) return g_shims;          // nothing wrapped
    g_freq = armtimer_probe();
    if (!g_freq) {
        fprintf(stderr, "[futex] the ARM generic timer cannot be read from user space: UA_FUTEX_STATS / UA_FUTEX_SPIN off\n");
        g_stats_s = g_spin_us = 0;
        return g_shims;
    }
    g_spin_ticks = g_freq * (uint64_t)g_spin_us / 1000000u;
    atomic_store(&g_next_log, ticks() + (uint64_t)(g_stats_s ? g_stats_s : 1) * g_freq);
    g_shims[0] = (so_shim){ "syscall", (void *)w_syscall };
    fprintf(stderr, "[futex] the Android libraries' futex calls go through futex.c:%s%s\n",
            g_stats_s ? " stats" : "", g_spin_us ? " user-space waits" : "");
    if (g_spin_us) fprintf(stderr, "[futex] UA_FUTEX_SPIN: waits watch their word %d us before sleeping\n", g_spin_us);
    return g_shims;
}
