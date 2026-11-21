/* syn_rl.c: test_relayout.c's C code, built with -fexceptions: a forced unwind through frames with
 * cleanups (their FDEs' LSDA, the personality routine of the CIE, landing pads and _Unwind_Resume in the
 * copies) and a backtrace, both with libgcc_eh linked into this library (as NDK libraries carry an
 * unwinder of their own) finding the copies' unwind info through the loader's dl_iterate_phdr. */
#include <stdint.h>
#include <unwind.h>

long g_cleaned, g_frames;
static void *g_jb[5];
static struct _Unwind_Exception g_exc;

__attribute__((noinline)) void rl_c_cleanup(volatile long *p) { g_cleaned += *p + 1; }

static _Unwind_Reason_Code stop(int version, _Unwind_Action actions, _Unwind_Exception_Class cls,
                                struct _Unwind_Exception *exc, struct _Unwind_Context *ctx, void *arg)
{
    (void)version; (void)cls; (void)exc; (void)ctx; (void)arg;
    if (actions & _UA_END_OF_STACK) __builtin_longjmp(g_jb, 1);
    /* the raise frame, rl_c_inner x (depth + 1): then rl_c_outer, which is not unwound */
    if (++g_frames == (long)(uintptr_t)arg) __builtin_longjmp(g_jb, 1);
    return _URC_NO_REASON;
}

__attribute__((noinline)) void rl_c_raise(long frames)
{
    g_exc.exception_class = 0x52454c41594f5554ull;   /* "RELAYOUT" */
    _Unwind_ForcedUnwind(&g_exc, stop, (void *)(uintptr_t)frames);
}

__attribute__((noinline)) void rl_c_inner(long depth, long frames)
{
    volatile long v __attribute__((cleanup(rl_c_cleanup))) = depth;
    if (depth == 0) rl_c_raise(frames);
    else rl_c_inner(depth - 1, frames);
    g_cleaned += 1000 + v;                           /* never: the unwind does not return */
}

/* the cleanups' sum * 100 + the frames the unwinder passed */
__attribute__((noinline)) long rl_c_outer(long depth)
{
    g_cleaned = 0; g_frames = 0;
    if (__builtin_setjmp(g_jb) == 0) {
        rl_c_inner(depth, depth + 2);
        return -1;
    }
    return g_cleaned * 100 + g_frames;
}

static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *arg)
{
    (void)ctx;
    ++*(long *)arg;
    return _URC_NO_REASON;
}

/* the frames _Unwind_Backtrace sees from depth calls down */
__attribute__((noinline)) long rl_c_bt(long depth)
{
    volatile long r;
    if (depth) r = rl_c_bt(depth - 1);
    else { long n = 0; _Unwind_Backtrace(bt_cb, &n); r = n; }
    return r;
}

/* a switch the compiler makes a jump table of */
__attribute__((noinline)) long rl_c_switch(long x)
{
    switch (x) {
    case 0: return 17;
    case 1: return x * 3 + 1;
    case 2: return 99;
    case 3: return x << 4;
    case 4: return -5;
    case 5: return 123456;
    case 6: return x ^ 0x55;
    case 7: return 7000;
    default: return -1;
    }
}
