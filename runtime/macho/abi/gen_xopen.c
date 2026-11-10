// gen_xopen.c - the part of darwin_abi.h the SDK's headers give only with _XOPEN_SOURCE defined:
// ucontext_t as getcontext, makecontext and swapcontext take it, with the machine context
// inside (a signal handler's ucontext_t, in gen_thread.inc, ends before it).
#define _XOPEN_SOURCE 600
#include <ucontext.h>

#include "gen.h"

void gen_xopen(void)
{
    H("ucontext_t for getcontext, makecontext, swapcontext (_XOPEN_SOURCE)")
    SZ(ucontext_t, UCONTEXT_X) OFF(ucontext_t, UCONTEXT_X, __mcontext_data)
}
