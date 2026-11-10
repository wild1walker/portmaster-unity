// darwin_ctx.c - the part of darwin_c.c's test that is built with _XOPEN_SOURCE: macOS's
// <ucontext.h> asks for it (the 880-byte ucontext_t), and with it fopen and select are imported
// under their $DARWIN_EXTSN names.
#define _XOPEN_SOURCE 500
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <ucontext.h>

static int failed;
#define CHECK(name, cond) do { int ok_ = !!(cond); failed += !ok_; fprintf(stdout, "%s %s\n", ok_ ? "ok  " : "FAIL", name); } while (0)
#define SHOW(...) fprintf(stdout, __VA_ARGS__)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

long ctx_link_in_handler(void)
{
    static ucontext_t in_handler;
    in_handler.uc_link = (ucontext_t *)(uintptr_t)0x5a5a;
    return getcontext(&in_handler) == 0 ? (long)(uintptr_t)in_handler.uc_link : -1;
}

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static ucontext_t ctx_main, ctx_a, ctx_b;
static char stack_a[131072], stack_b[131072];

static void three(int a, int b, int c)
{
    volatile char here;
    SHOW("in a context: %d %d %d, on its stack %d\n", a, b, c, &here > stack_a && &here < stack_a + sizeof stack_a);
}

static void eleven(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, int k)
{
    SHOW("eleven arguments: %d %d %d %d %d %d %d %d %d %d %d\n", a, b, c, d, e, f, g, h, i, j, k);
}

static void ping(int id, int rounds)
{
    double keep = id * 1.5;
    for (int i = 0; i < rounds; i++) {
        SHOW("%s round %d keeps %g\n", id == 1 ? "A" : "B", i, keep);
        keep += 0.25;
        int r = swapcontext(id == 1 ? &ctx_a : &ctx_b, id == 1 ? &ctx_b : &ctx_a);
        if (r != 0) SHOW("swapcontext %d\n", r);
    }
    SHOW("%s ends with %g\n", id == 1 ? "A" : "B", keep);
}

static void none(void) { }

int test_contexts(void)
{
    static ucontext_t u;
    volatile int local = 0;
    memset(&u, 0x5a, sizeof u);
    int r = getcontext(&u);
    struct rlimit rl = { 0, 0 };
    getrlimit(RLIMIT_STACK, &rl);
    SHOW("getcontext %d: %d bytes, mcsize %d, link %lx, onstack %x, stack flags %x, mask %x\n", r, (int)sizeof u, (int)u.uc_mcsize,
         (unsigned long)(uintptr_t)u.uc_link, (unsigned)u.uc_onstack, (unsigned)u.uc_stack.ss_flags, (unsigned)u.uc_sigmask);
    CHECK("uc_mcontext is its own", u.uc_mcontext == &u.__mcontext_data);
    CHECK("the stack is this one", (uintptr_t)&local - (uintptr_t)u.uc_stack.ss_sp < 8192);
    CHECK("as long as the limit says", u.uc_stack.ss_size == rl.rlim_cur);

    volatile int laps = 0;
    r = getcontext(&u);
    laps++;
    if (laps < 5) { r = setcontext(&u); SHOW("setcontext came back %d\n", r); }
    SHOW("getcontext returned %d each of %d times\n", r, laps);

    memset(stack_a, 0x33, sizeof stack_a);
    getcontext(&ctx_a);
    ctx_a.uc_stack.ss_sp = stack_a;
    ctx_a.uc_stack.ss_size = sizeof stack_a;
    ctx_a.uc_link = &ctx_main;
    makecontext(&ctx_a, (void (*)(void))three, 3, 7, -8, 9);
    CHECK("makecontext clears the stack", stack_a[0] == 0 && stack_a[4096] == 0 && stack_a[sizeof stack_a - 4096] == 0);
    r = swapcontext(&ctx_main, &ctx_a);
    SHOW("back by uc_link: swapcontext %d, the ended context's mcsize %d, onstack %x\n", r, (int)ctx_a.uc_mcsize,
         (unsigned)ctx_main.uc_onstack);
    errno = 0;
    r = setcontext(&ctx_a);
    SHOW("setcontext to an ended context %d errno %d\n", r, errno);

    getcontext(&ctx_b);
    ctx_b.uc_stack.ss_sp = stack_b;
    ctx_b.uc_stack.ss_size = sizeof stack_b;
    ctx_b.uc_link = &ctx_main;
    makecontext(&ctx_b, (void (*)(void))eleven, 11, 1, -2, 3, -4, 5, -6, 7, -8, 9, -10, 11);
    swapcontext(&ctx_main, &ctx_b);

    getcontext(&ctx_a);
    ctx_a.uc_stack.ss_sp = stack_a;
    ctx_a.uc_stack.ss_size = sizeof stack_a;
    ctx_a.uc_link = &ctx_main;
    makecontext(&ctx_a, (void (*)(void))ping, 2, 1, 3);
    getcontext(&ctx_b);
    ctx_b.uc_stack.ss_sp = stack_b;
    ctx_b.uc_stack.ss_size = sizeof stack_b;
    ctx_b.uc_link = &ctx_main;
    makecontext(&ctx_b, (void (*)(void))ping, 2, 2, 3);
    swapcontext(&ctx_main, &ctx_a);
    SHOW("A is through\n");
    swapcontext(&ctx_main, &ctx_b);
    SHOW("B is through\n");

    errno = 0;
    r = swapcontext(NULL, &ctx_main);
    SHOW("swapcontext from nothing %d errno %d\n", r, errno);
    errno = 0;
    r = swapcontext(&u, NULL);
    SHOW("swapcontext to nothing %d errno %d\n", r, errno);
    getcontext(&u);
    u.uc_stack.ss_sp = NULL;
    makecontext(&u, none, 0);
    SHOW("makecontext without a stack: mcsize %d\n", (int)u.uc_mcsize);
    getcontext(&u);
    u.uc_stack.ss_sp = stack_a;
    u.uc_stack.ss_size = sizeof stack_a;
    makecontext(&u, none, -1);
    SHOW("makecontext with -1 arguments: mcsize %d\n", (int)u.uc_mcsize);

    // a context carries its signal mask
    sigset_t usr2 = sigmask(SIGUSR2), cur = 0;
    volatile int pass = 0;
    getcontext(&u);
    if (pass++ == 0) {
        pthread_sigmask(SIG_BLOCK, &usr2, NULL);
        setcontext(&u);
    }
    pthread_sigmask(SIG_BLOCK, NULL, &cur);
    SHOW("the mask after setcontext %x\n", cur);
    pthread_sigmask(SIG_BLOCK, &usr2, NULL);
    getcontext(&u);
    pthread_sigmask(SIG_UNBLOCK, &usr2, NULL);
    SHOW("the mask a context holds %x\n", (unsigned)u.uc_sigmask);

    // the names this file imports them under
    errno = 0;
    FILE *volatile f = fopen("/no/such/dir/file", "r");
    SHOW("fopen of no file %d errno %d\n", f != NULL, errno);
    struct timeval now = { 0, 0 };
    SHOW("select of nothing %d\n", select(0, NULL, NULL, NULL, &now));
    return failed;
}

#pragma clang diagnostic pop
