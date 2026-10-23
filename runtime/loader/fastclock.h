/* fastclock.c: clock_gettime/gettimeofday for Android code without a system call where the kernel
 * gives 32-bit processes no vDSO (bionic's 32-bit timespec/timeval) */
#ifndef FASTCLOCK_H
#define FASTCLOCK_H
#include <time.h>
void fastclock_init(void);                       /* decides (UA_FAST_CLOCK, UA_CLOCK_STATS); once */
int fastclock_on(void);
int fastclock_gettime(clockid_t clk, void *ts);  /* bionic's clock_gettime */
int fastclock_gettimeofday(void *tv, void *tz);  /* bionic's gettimeofday */
#endif
