// sh_time.c - clocks and calendar time: macOS's clock numbers, its timeval, Mach's tick count.
// struct tm, struct timespec, time_t and clock_t are the same on both, so the calendar calls
// are glibc's own.
#define _GNU_SOURCE
#include "darwin.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PUT(buf, off, v) memcpy((unsigned char *)(buf) + (off), &(v), sizeof(v))

_Static_assert(DARWIN_SIZEOF_TM == sizeof(struct tm) && DARWIN_TM_tm_gmtoff == offsetof(struct tm, tm_gmtoff) &&
               DARWIN_TM_tm_zone == offsetof(struct tm, tm_zone), "struct tm");
_Static_assert(DARWIN_SIZEOF_TIMESPEC == sizeof(struct timespec), "struct timespec");
_Static_assert(DARWIN_SIZEOF_CLOCK_T == sizeof(clock_t) && DARWIN_CLOCKS_PER_SEC == CLOCKS_PER_SEC, "clock_t");
_Static_assert(DARWIN_SIZEOF_TIMEZONE == sizeof(struct timezone), "struct timezone");

// -1: not a clock
static int clock_host(int d)
{
    switch (d) {
    case DARWIN_CLOCK_REALTIME: return CLOCK_REALTIME;
    // the "uptime" clocks stand still while the machine sleeps, as Linux's monotonic clock does
    case DARWIN_CLOCK_UPTIME_RAW: case DARWIN_CLOCK_UPTIME_RAW_APPROX: return CLOCK_MONOTONIC;
    // macOS's monotonic clocks count the sleep too
    case DARWIN_CLOCK_MONOTONIC: case DARWIN_CLOCK_MONOTONIC_RAW: case DARWIN_CLOCK_MONOTONIC_RAW_APPROX: return CLOCK_BOOTTIME;
    case DARWIN_CLOCK_PROCESS_CPUTIME_ID: return CLOCK_PROCESS_CPUTIME_ID;
    case DARWIN_CLOCK_THREAD_CPUTIME_ID: return CLOCK_THREAD_CPUTIME_ID;
    }
    return -1;
}

static long darwin_clock_gettime(int clk, struct timespec *ts)
{
    int h = clock_host(clk);
    if (h < 0) { errno = EINVAL; return -1; }
    return clock_gettime(h, ts);
}

static long darwin_clock_getres(int clk, struct timespec *ts)
{
    int h = clock_host(clk);
    if (h < 0) { errno = EINVAL; return -1; }
    return clock_getres(h, ts);
}

static uint64_t ns_of(int host_clock)
{
    struct timespec ts;
    if (clock_gettime(host_clock, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

// uint64_t clock_gettime_nsec_np(clockid_t): 0 and errno for what is not a clock
static uint64_t darwin_clock_gettime_nsec_np(int clk)
{
    int h = clock_host(clk);
    if (h < 0) { errno = EINVAL; return 0; }
    return ns_of(h);
}

// Mach's tick counts, answered in nanoseconds: mach_timebase_info says one tick is one.
static uint64_t darwin_mach_absolute_time(void) { return ns_of(CLOCK_MONOTONIC); }
static uint64_t darwin_mach_approximate_time(void) { return ns_of(CLOCK_MONOTONIC); }
static uint64_t darwin_mach_continuous_time(void) { return ns_of(CLOCK_BOOTTIME); }

_Static_assert(DARWIN_SIZEOF_MACH_TIMEBASE_INFO == 2 * sizeof(uint32_t), "mach_timebase_info_data_t");
static long darwin_mach_timebase_info(uint32_t *info)
{
    info[0] = 1;                            // numer
    info[1] = 1;                            // denom
    return DARWIN_KERN_SUCCESS;
}

// int gettimeofday(struct timeval *, void *): the microseconds are an int there
static long darwin_gettimeofday(unsigned char *tv, void *tz)
{
    struct timeval t;
    if (gettimeofday(&t, tz) != 0) return -1;
    if (tv) {
        int64_t sec = t.tv_sec;
        int32_t usec = (int32_t)t.tv_usec;
        PUT(tv, 0, sec);
        PUT(tv, DARWIN_TIMEVAL_tv_usec, usec);
    }
    return 0;
}

static size_t darwin_strftime_l(char *s, size_t n, const char *fmt, const struct tm *tm, void *loc)
{
    return strftime_l(s, n, fmt, tm, (locale_t)darwin_locale_host(loc));
}

// The sleeps. A thread_suspend stops a thread by a signal here, and a sleep that signal broke
// off goes on for what was left of it, as one on macOS sleeps through a suspension; a signal
// of the image's own ends it with EINTR as there.
static long darwin_nanosleep(const struct timespec *want, struct timespec *rem)
{
    struct timespec t, left;
    if (!want) { errno = EFAULT; return -1; }
    t = *want;
    for (;;) {
        unsigned stops = darwin_thread_stops();
        if (nanosleep(&t, &left) == 0) return 0;
        if (errno != EINTR || stops == darwin_thread_stops()) {
            if (errno == EINTR && rem) *rem = left;
            return -1;
        }
        t = left;
    }
}

// int usleep(useconds_t)
static long darwin_usleep(uint32_t us)
{
    struct timespec t = { us / 1000000, (long)(us % 1000000) * 1000 };
    return darwin_nanosleep(&t, NULL);
}

// unsigned sleep(unsigned): the seconds not slept
static uint32_t darwin_sleep(uint32_t s)
{
    struct timespec t = { s, 0 }, left = { 0, 0 };
    int saved = errno;
    if (darwin_nanosleep(&t, &left) == 0) return 0;
    errno = saved;
    return (uint32_t)(left.tv_sec + (left.tv_nsec >= 500000000));
}

const struct darwin_sym darwin_syms_time[] = {
    SHIM(clock_gettime), SHIM(clock_getres), SHIM(clock_gettime_nsec_np),
    SHIM(mach_absolute_time), SHIM(mach_approximate_time), SHIM(mach_continuous_time), SHIM(mach_timebase_info),
    SHIM(gettimeofday), SHIM(strftime_l),
    SAME(time), SAME(clock), SAME(difftime), SAME(mktime), SAME(timegm), SAME(gmtime), SAME(gmtime_r),
    SAME(localtime), SAME(localtime_r), SAME(asctime), SAME(ctime), SAME(strftime), SAME(strptime), SAME(tzset),
    SHIM(nanosleep), SHIM(usleep), SHIM(sleep),
    NAMED("_timezone", &timezone), NAMED("_daylight", &daylight), NAMED("_tzname", tzname),
    { NULL, NULL }
};
