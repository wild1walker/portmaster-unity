/* armtimer.h - the ARM generic timer's virtual counter read from user space (CNTVCT), for short
 * bounded waits that must not make a system call to know the time. armtimer_probe() reads it once
 * under a SIGILL handler (a kernel may not let user space read it; QEMU's 32-bit user mode cannot):
 * it returns the counter's frequency, or 0 when it cannot be used. Elsewhere than ARM: the
 * monotonic clock in nanoseconds. */
#ifndef ARMTIMER_H
#define ARMTIMER_H
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#if defined(__arm__)
static inline uint64_t armtimer_ticks(void)
{
    uint32_t lo, hi;
    __asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t armtimer_freq_reg(void) { uint32_t f; __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f)); return f; }
static inline void armtimer_relax(void) { __asm__ volatile("yield" ::: "memory"); }
#elif defined(__aarch64__)
static inline uint64_t armtimer_ticks(void) { uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) :: "memory"); return v; }
static inline uint64_t armtimer_freq_reg(void) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f; }
static inline void armtimer_relax(void) { __asm__ volatile("yield" ::: "memory"); }
#else
static inline uint64_t armtimer_ticks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static inline uint64_t armtimer_freq_reg(void) { return 1000000000u; }
static inline void armtimer_relax(void) {}
#endif

static sigjmp_buf armtimer_jb;
static void armtimer_sigill(int s) { (void)s; siglongjmp(armtimer_jb, 1); }

static inline uint64_t armtimer_probe(void)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = armtimer_sigill;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGILL, &sa, &old)) return 0;
    volatile uint64_t f = 0;
    if (!sigsetjmp(armtimer_jb, 1)) {
        uint64_t a = armtimer_ticks(), fr = armtimer_freq_reg();
        f = a && fr >= 1000000 && fr <= 1000000000 ? fr : 0;
    }
    sigaction(SIGILL, &old, NULL);
    return f;
}
#endif
