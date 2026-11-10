// sh_context.c - getcontext, setcontext, makecontext, swapcontext: macOS's ucontext_t (880
// bytes, its mcontext inside) filled as its libc fills one. A context holds what a call keeps
// (x19 to x28, fp, lr, sp, the low halves of v8 to v15) and the signal mask: resuming one
// returns 0 from the getcontext or swapcontext that saved it.
//
// What macOS's libc does, and is done here as it is there:
// - uc_onstack is left as it was by getcontext; swapcontext sets its top bit in the context
//   it saves and keeps that context's uc_link
// - uc_stack is the sp at the call and RLIMIT_STACK's soft limit, not the thread's stack
// - uc_link is the number of the signal whose handler is running (0: none), until the caller
//   sets it
// - d8 to d15 are saved in the upper halves of their v slots
// - a context that has run to its end has uc_mcsize 0, and setcontext refuses it (EINVAL)
// - a context made by makecontext whose function returns with no uc_link ends the process
//   with status 0
// The slot arm64e keeps its flags in is not written.
#define _GNU_SOURCE
#include "darwin.h"
#include "darwin_va.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PUT(buf, off, v) memcpy((unsigned char *)(buf) + (off), &(v), sizeof(v))
#define GET(buf, off, v) memcpy(&(v), (const unsigned char *)(buf) + (off), sizeof(v))

// The registers' places from the start of the mcontext: sh_asm.S has them as numbers.
#define MC      DARWIN_UCONTEXT_X___mcontext_data
#define SS      (DARWIN_MCONTEXT64___ss)
#define NS      (DARWIN_MCONTEXT64___ns)
#define MC_X(n) (SS + DARWIN_ARM_THREAD_STATE64___x + 8 * (n))
#define MC_FP   (SS + DARWIN_ARM_THREAD_STATE64___fp)
#define MC_LR   (SS + DARWIN_ARM_THREAD_STATE64___lr)
#define MC_SP   (SS + DARWIN_ARM_THREAD_STATE64___sp)
_Static_assert(MC == 64 && DARWIN_SIZEOF_UCONTEXT_X == MC + DARWIN_SIZEOF_MCONTEXT64, "the mcontext inside the ucontext");
_Static_assert(MC_X(0) == 16 && MC_X(19) == 168 && MC_X(27) == 232, "x0, x19, x27 as sh_asm.S has them");
_Static_assert(MC_FP == 248 && MC_LR == 256 && MC_SP == 264, "fp, lr, sp as sh_asm.S has them");
_Static_assert(NS + DARWIN_ARM_NEON_STATE64___v + 16 * 8 + 8 == 424, "d8 as sh_asm.S has it");

#define UCF_SWAPPED 0x80000000u             // in uc_onstack: saved by swapcontext

// sh_asm.S
void ua_getcontext(void);
void ua_swapcontext(void);
void ua_ctx_start(void);
_Noreturn void ua_ctx_jump(const unsigned char *mcontext);

// getcontext's part that is not registers. sp: the caller's.
__attribute__((visibility("hidden"))) void ua_ctx_fill(unsigned char *ucp, uint64_t sp)
{
    uint32_t mask = darwin_sigmask();
    uint64_t size = darwin_stack_limit(), mcsize = DARWIN_SIZEOF_MCONTEXT64;
    uint64_t link = (uint64_t)darwin_signal_in_handler();
    // no alternate stack is the image's: there is no sigaltstack for it to call
    int32_t flags = DARWIN_SS_DISABLE;
    unsigned char *mc = ucp + MC;
    PUT(ucp, DARWIN_UCONTEXT_uc_sigmask, mask);
    PUT(ucp, DARWIN_UCONTEXT_uc_stack + DARWIN_STACK_T_ss_sp, sp);
    PUT(ucp, DARWIN_UCONTEXT_uc_stack + DARWIN_STACK_T_ss_size, size);
    PUT(ucp, DARWIN_UCONTEXT_uc_stack + DARWIN_STACK_T_ss_flags, flags);
    PUT(ucp, DARWIN_UCONTEXT_uc_link, link);
    PUT(ucp, DARWIN_UCONTEXT_uc_mcsize, mcsize);
    PUT(ucp, DARWIN_UCONTEXT_uc_mcontext, mc);
}

// int setcontext(const ucontext_t *): the registers are the ones inside the ucontext,
// wherever uc_mcontext points
static long darwin_setcontext(const unsigned char *ucp)
{
    uint64_t mcsize;
    uint32_t mask;
    GET(ucp, DARWIN_UCONTEXT_uc_mcsize, mcsize);
    if (mcsize == 0) { errno = EINVAL; return -1; }
    GET(ucp, DARWIN_UCONTEXT_uc_sigmask, mask);
    int saved = errno;
    darwin_set_sigmask(mask);
    errno = saved;
    ua_ctx_jump(ucp + MC);
}

// swapcontext after its registers are saved (not saved when one of the two is NULL)
__attribute__((visibility("hidden"))) long ua_ctx_swap(unsigned char *oucp, const unsigned char *ucp, uint64_t sp)
{
    if (!oucp || !ucp) { errno = EINVAL; return -1; }
    uint64_t link;
    uint32_t onstack;
    GET(oucp, DARWIN_UCONTEXT_uc_link, link);
    ua_ctx_fill(oucp, sp);
    PUT(oucp, DARWIN_UCONTEXT_uc_link, link);
    GET(oucp, DARWIN_UCONTEXT_uc_onstack, onstack);
    onstack |= UCF_SWAPPED;
    PUT(oucp, DARWIN_UCONTEXT_uc_onstack, onstack);
    return darwin_setcontext(ucp);
}

// void makecontext(ucontext_t *, void (*)(), int argc, ...): the arguments are ints. The
// whole stack is cleared, the arguments go to its top as the function's stack arguments would
// (4 bytes each), and the context starts in ua_ctx_start, which loads the first eight.
static void darwin_makecontext(unsigned char *ucp, void (*func)(void), int argc, ...)
{
    if (!ucp) return;
    uint64_t stack, size, none = 0;
    GET(ucp, DARWIN_UCONTEXT_uc_stack + DARWIN_STACK_T_ss_sp, stack);
    GET(ucp, DARWIN_UCONTEXT_uc_stack + DARWIN_STACK_T_ss_size, size);
    if (!stack || argc < 0 || argc > DARWIN_NCARGS) {
        PUT(ucp, DARWIN_UCONTEXT_uc_mcsize, none);          // setcontext refuses it
        return;
    }
    unsigned char *mc = ucp + MC;
    PUT(ucp, DARWIN_UCONTEXT_uc_mcontext, mc);
    memset((void *)(uintptr_t)stack, 0, size);
    uint64_t fp = (stack + size) & ~(uint64_t)15;
    uint64_t sp = (fp - 4 * (uint64_t)(argc < 8 ? 8 : argc)) & ~(uint64_t)15;
    va_list ap;
    DARWIN_VA_START(ap, argc);
    for (int i = 0; i < argc; i++) {
        int32_t a = va_arg(ap, int);
        memcpy((void *)(uintptr_t)(sp + 4 * (uint64_t)i), &a, 4);
    }
    va_end(ap);
    uint64_t start = (uint64_t)(uintptr_t)ua_ctx_start, self = (uint64_t)(uintptr_t)ucp, fn = (uint64_t)(uintptr_t)func;
    PUT(mc, MC_X(19), self);
    PUT(mc, MC_X(20), fn);
    PUT(mc, MC_FP, fp);
    PUT(mc, MC_LR, start);
    PUT(mc, MC_SP, sp);
}

// A makecontext function returned: on to its uc_link.
__attribute__((visibility("hidden"))) void ua_ctx_done(unsigned char *ucp)
{
    uint64_t link, none = 0;
    GET(ucp, DARWIN_UCONTEXT_uc_link, link);
    if (!link) _exit(0);
    PUT(ucp, DARWIN_UCONTEXT_uc_mcsize, none);
    darwin_setcontext((const unsigned char *)(uintptr_t)link);
    fprintf(stderr, "[darwin] a context ended and the one it links to has ended before it\n");
    abort();
}

const struct darwin_sym darwin_syms_context[] = {
    NAMED("_getcontext", ua_getcontext), NAMED("_swapcontext", ua_swapcontext),
    SHIM(setcontext), SHIM(makecontext),
    { NULL, NULL }
};
