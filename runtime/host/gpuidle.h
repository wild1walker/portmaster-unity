// Whether the GPU waits for work inside the frames (gpuidle.c): UA_GPU_IDLE=<s>.
#ifndef GPUIDLE_H
#define GPUIDLE_H
#include "../loader/so_loader.h"
// The glBindFramebuffer wrapper when UA_GPU_IDLE is set (else an empty table): register with
// so_add_shims() before the other GL wrappers; next(name) is the wrapper it calls (NULL: the driver's).
const so_shim *gpuidle_table(void *(*next)(const char *name));
void gpuidle_frame(void);           // from the swap hook, right before each eglSwapBuffers

// The accounting, without EGL (test_gpuidle.c): a span of commands flushed at `sub`, which the GPU
// had finished at `done` (both s, one clock); `first` = the frame's first span. Spans are given in
// the order they were flushed.
typedef struct {
    double idle, idle_first, busy_from;     // s: idle in all, before frames' first spans; last done
    long spans, frames;
    double by_span[32];                     // idle before the n-th span of a frame (s), n < 32
    unsigned fb_of[32];                     // the framebuffer that span drew into (the last seen)
    double largest; int largest_span;
} gpuidle_acc;
void gpuidle_account(gpuidle_acc *a, double sub, double done, int first, int span, unsigned fb);
#endif
