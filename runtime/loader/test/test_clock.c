/* test_clock: fastclock.c's clock_gettime/gettimeofday against the kernel's.
 *   UA_FAST_CLOCK=1 ./test_clock [seconds]   (=1: the counter where there is no vDSO, the RG34XX-SP;
 *                                             =force: also with a vDSO; unset: the system calls)
 * Four threads read CLOCK_MONOTONIC in turn through a shared "last value" and check that no read
 * is below one another thread saw before it (monotonic across threads); every clock is compared
 * with the kernel's own (a system call) and must be within 100 us of it, including across the
 * resyncs; gettimeofday within 100 us of CLOCK_REALTIME. Without a usable counter (or on arm64)
 * the same checks run against the system calls the functions fall back to. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../fastclock.h"

struct b_timespec { long tv_sec; long tv_nsec; };
struct b_timeval { long tv_sec; long tv_usec; };

static int64_t fast(clockid_t c) { struct b_timespec t; if (fastclock_gettime(c, &t)) return -1; return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static int64_t kern(clockid_t c) { struct timespec t; syscall(SYS_clock_gettime, c, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }

static atomic_llong last;
static atomic_int failures, stop;
static atomic_llong reads;

static void *reader(void *arg)
{
    (void)arg;
    long n = 0;
    while (!atomic_load(&stop)) {
        int64_t before = atomic_load(&last);
        int64_t v = fast(CLOCK_MONOTONIC);
        if (v < before) {
            if (atomic_fetch_add(&failures, 1) < 5) fprintf(stderr, "FAIL: monotonic read %lld below an earlier one %lld\n", (long long)v, (long long)before);
        }
        int64_t cur = atomic_load(&last);
        while (v > cur && !atomic_compare_exchange_weak(&last, &cur, v)) ;
        n++;
    }
    atomic_fetch_add(&reads, n);
    return NULL;
}

static int64_t worst[8];

static int near(const char *what, int64_t a, int64_t b, int64_t tol)
{
    int64_t d = a > b ? a - b : b - a;
    if (d > tol) { fprintf(stderr, "FAIL: %s differs from the kernel's by %lld ns\n", what, (long long)d); return 0; }
    return 1;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 3;
    fastclock_init();
    printf("fast clock %s\n", fastclock_on() ? "on (the counter)" : "off (system calls)");
    clockid_t clocks[] = { CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME,
                           CLOCK_MONOTONIC_COARSE, CLOCK_REALTIME_COARSE };
    const char *names[] = { "REALTIME", "MONOTONIC", "MONOTONIC_RAW", "BOOTTIME", "MONOTONIC_COARSE", "REALTIME_COARSE" };
    int ok = 1;
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, reader, NULL);
    /* every 50 ms for <secs> s (several resync periods): each clock against the kernel's */
    for (int round = 0; round < secs * 20; round++) {
        for (unsigned i = 0; i < sizeof clocks / sizeof *clocks; i++) {
            int coarse = clocks[i] == CLOCK_MONOTONIC_COARSE || clocks[i] == CLOCK_REALTIME_COARSE;
            int64_t k0 = kern(clocks[i]), f = fast(clocks[i]), k1 = kern(clocks[i]);
            /* a coarse kernel clock lags its tick (10 ms at HZ=100) */
            int64_t tol = coarse ? 25000000 : 100000;
            if (f < 0 || !near(names[i], f, k0 + (k1 - k0) / 2, tol + (k1 - k0))) ok = 0;
            int64_t d = f - (k0 + (k1 - k0) / 2);
            if (d < 0) d = -d;
            d -= (k1 - k0) / 2;                          /* what the two kernel reads leave open */
            if (d > worst[i]) worst[i] = d;
        }
        struct b_timeval tv;
        int64_t k0 = kern(CLOCK_REALTIME);
        fastclock_gettimeofday(&tv, NULL);
        int64_t k1 = kern(CLOCK_REALTIME);
        if (!near("gettimeofday", (int64_t)tv.tv_sec * 1000000000 + (int64_t)tv.tv_usec * 1000, k0 + (k1 - k0) / 2, 100000 + (k1 - k0))) ok = 0;
        struct timespec d = { 0, 50000000 };
        nanosleep(&d, NULL);
    }
    /* a clock not asked for longer than its extrapolation limit (4 s) */
    if (secs >= 5) {
        int64_t a = fast(CLOCK_BOOTTIME);
        sleep(5);
        int64_t k0 = kern(CLOCK_BOOTTIME), f = fast(CLOCK_BOOTTIME), k1 = kern(CLOCK_BOOTTIME);
        if (f < a || !near("BOOTTIME after 5 s unused", f, k0 + (k1 - k0) / 2, 100000 + (k1 - k0))) ok = 0;
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    /* cost of a call (after 0.3 s of the same, so the core's clock has risen) */
    int n = 1000000;
    int64_t a = kern(CLOCK_MONOTONIC);
    while (kern(CLOCK_MONOTONIC) - a < 300000000) for (int i = 0; i < 1000; i++) fast(CLOCK_MONOTONIC);
    a = kern(CLOCK_MONOTONIC);
    for (int i = 0; i < n; i++) fast(CLOCK_MONOTONIC);
    int64_t b = kern(CLOCK_MONOTONIC);
    printf("most apart from the kernel's clock (us):");
    for (unsigned i = 0; i < sizeof clocks / sizeof *clocks; i++) printf(" %s %.1f", names[i], worst[i] / 1000.0);
    printf("\n");
    printf("%lld monotonic reads in 4 threads, %d out of order; a call %.0f ns\n", (long long)atomic_load(&reads),
           atomic_load(&failures), (double)(b - a) / n);
    if (atomic_load(&failures)) ok = 0;
    printf("%s\n", ok ? "test_clock: OK" : "test_clock: FAILED");
    return ok ? 0 : 1;
}
