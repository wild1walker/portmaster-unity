// gpuidle.c's accounting: spans flushed while the GPU was still busy count no idle time, a span
// flushed after the GPU had finished everything before it counts the gap, by the span's place in
// its frame; a frame's first span is counted apart.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../gpuidle.h"

static int fails;
static void near(const char *what, double got, double want)
{
    if (fabs(got - want) > 1e-9) { printf("FAIL %s: %.6f, want %.6f\n", what, got, want); fails++; }
}

int main(void)
{
    gpuidle_acc a;
    memset(&a, 0, sizeof a);
    // frame 1: three spans, the GPU always behind the CPU (each flushed before the last was done)
    gpuidle_account(&a, 0.000, 0.010, 1, 0, 5);
    gpuidle_account(&a, 0.004, 0.020, 0, 1, 6);
    gpuidle_account(&a, 0.012, 0.030, 0, 2, 0);
    near("busy GPU: no idle", a.idle, 0);
    // frame 2: its first span flushed 2 ms after the GPU finished frame 1, its second 3 ms after
    // the first was done (a CPU-side wait in the driver), the third while the GPU was busy
    gpuidle_account(&a, 0.032, 0.040, 1, 0, 5);
    gpuidle_account(&a, 0.043, 0.050, 0, 1, 6);
    gpuidle_account(&a, 0.045, 0.060, 0, 2, 0);
    near("idle", a.idle, 0.005);
    near("idle before first spans", a.idle_first, 0.002);
    near("idle before span 1", a.by_span[1], 0.003);
    near("largest", a.largest, 0.003);
    if (a.largest_span != 1) { printf("FAIL largest span %d\n", a.largest_span); fails++; }
    if (a.frames != 2 || a.spans != 6) { printf("FAIL counts %ld frames %ld spans\n", a.frames, a.spans); fails++; }
    if (a.fb_of[1] != 6) { printf("FAIL fb of span 1: %u\n", a.fb_of[1]); fails++; }
    // a fence that signals before the one before it was seen (the waiting thread wakes late) does
    // not move the GPU's busy end back
    gpuidle_account(&a, 0.061, 0.058, 1, 0, 5);
    near("busy end kept", a.busy_from, 0.060);
    near("idle after the late one", a.idle, 0.006);
    printf(fails ? "test_gpuidle: %d failures\n" : "test_gpuidle: ok\n", fails);
    return fails != 0;
}
