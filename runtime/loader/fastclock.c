/*
 * fastclock.c - bionic's clock_gettime / gettimeofday / time for the Android libraries: the host's
 * clock hook first (so_set_clock_hook: the lockstep checker's UnityMain clock), else glibc's (the
 * vDSO: no system call on aarch64).
 *
 * (Its counter-based clock for 32-bit processes without a vDSO, UA_FAST_CLOCK, went with armhf;
 * the last tree with it is the tag last-armhf.)
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <sys/time.h>
#include <time.h>

#include "fastclock.h"

void fastclock_init(void) {}
int fastclock_on(void) { return 0; }

/* so_set_clock_hook (so_loader.h): asked first; 0 = it answered */
static int (*g_clock_hook)(int clk, long *sec, long *nsec);
void so_set_clock_hook(int (*hook)(int clk, long *sec, long *nsec)) { g_clock_hook = hook; }

int fastclock_gettime(clockid_t clk, void *tsp)
{
    struct timespec *ts = tsp;
    if (__builtin_expect(g_clock_hook != NULL, 0) && ts) {
        long sec, nsec;
        if (!g_clock_hook((int)clk, &sec, &nsec)) { ts->tv_sec = sec; ts->tv_nsec = nsec; return 0; }
    }
    return clock_gettime(clk, ts);
}

int fastclock_gettimeofday(void *tvp, void *tz)
{
    struct timeval *tv = tvp;
    if (__builtin_expect(g_clock_hook != NULL, 0) && tv) {
        long sec, nsec;
        if (!g_clock_hook(CLOCK_REALTIME, &sec, &nsec)) { tv->tv_sec = sec; tv->tv_usec = nsec / 1000; return 0; }
    }
    return gettimeofday(tv, tz);
}

/* bionic's time(): the system's, unless the clock hook answers CLOCK_REALTIME */
long fastclock_time(long *t)
{
    long sec, nsec, r;
    if (__builtin_expect(g_clock_hook != NULL, 0) && !g_clock_hook(CLOCK_REALTIME, &sec, &nsec)) r = sec;
    else r = (long)time(NULL);
    if (t) *t = r;
    return r;
}
