/* fastclock.c: bionic's clock_gettime/gettimeofday/time for Android code, through the host's clock
 * hook (so_set_clock_hook) */
#ifndef FASTCLOCK_H
#define FASTCLOCK_H
#include <time.h>
void fastclock_init(void);                       /* (nothing to decide on aarch64) */
int fastclock_on(void);                          /* 0: the counter clock was 32-bit only */
int fastclock_gettime(clockid_t clk, void *ts);  /* bionic's clock_gettime */
int fastclock_gettimeofday(void *tv, void *tz);  /* bionic's gettimeofday */
long fastclock_time(long *t);                    /* bionic's time */
#endif
