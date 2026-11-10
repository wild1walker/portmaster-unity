// sh_signal.c - signal and sigaction: macOS's signal numbers, its struct sigaction, and a
// handler called the way macOS calls one: with its siginfo_t and its ucontext (the thread's
// registers as arm_thread_state64 and arm_neon_state64), built from Linux's and written back
// when the handler returns, so a handler that edits the context (skips an instruction, moves
// the pc) has the effect it has there.
//
// The host's handler for a signal runs on the thread's alternate stack always (SA_ONSTACK):
// macOS code may keep data in the 128 bytes below sp, and a frame pushed there would overwrite it.
//
// A signal's action that was the host's own (a crash reporter's) is shown to the image as one
// handler address, darwin_signal_chain: calling it from the image's handler passes the signal
// on to the host's, and handing it back to sigaction puts the host's action back as it was.
//
// The signal mask (sigprocmask, pthread_sigmask) is the host's, bit by bit; the bits of the
// signals Linux has none for are kept beside it, so a mask reads back as it was set.
#define _GNU_SOURCE
#include "darwin.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

// macOS's number as the host's; 0: Linux has no such signal; -1: not a signal.
static int sig_host(int d)
{
    switch (d) {
    case DARWIN_SIGHUP: return SIGHUP;       case DARWIN_SIGINT: return SIGINT;
    case DARWIN_SIGQUIT: return SIGQUIT;     case DARWIN_SIGILL: return SIGILL;
    case DARWIN_SIGTRAP: return SIGTRAP;     case DARWIN_SIGABRT: return SIGABRT;
    case DARWIN_SIGFPE: return SIGFPE;       case DARWIN_SIGKILL: return SIGKILL;
    case DARWIN_SIGBUS: return SIGBUS;       case DARWIN_SIGSEGV: return SIGSEGV;
    case DARWIN_SIGSYS: return SIGSYS;       case DARWIN_SIGPIPE: return SIGPIPE;
    case DARWIN_SIGALRM: return SIGALRM;     case DARWIN_SIGTERM: return SIGTERM;
    case DARWIN_SIGURG: return SIGURG;       case DARWIN_SIGSTOP: return SIGSTOP;
    case DARWIN_SIGTSTP: return SIGTSTP;     case DARWIN_SIGCONT: return SIGCONT;
    case DARWIN_SIGCHLD: return SIGCHLD;     case DARWIN_SIGTTIN: return SIGTTIN;
    case DARWIN_SIGTTOU: return SIGTTOU;     case DARWIN_SIGIO: return SIGIO;
    case DARWIN_SIGXCPU: return SIGXCPU;     case DARWIN_SIGXFSZ: return SIGXFSZ;
    case DARWIN_SIGVTALRM: return SIGVTALRM; case DARWIN_SIGPROF: return SIGPROF;
    case DARWIN_SIGWINCH: return SIGWINCH;   case DARWIN_SIGUSR1: return SIGUSR1;
    case DARWIN_SIGUSR2: return SIGUSR2;
    case DARWIN_SIGEMT: case DARWIN_SIGINFO: return 0;
    }
    return -1;
}

#define NSIG_DARWIN 32
static int sig_darwin(int h)
{
    for (int d = 1; d < NSIG_DARWIN; d++) if (sig_host(d) == h) return d;
    return 0;
}

// sigset_t there: one bit per signal in 32
#define BIT(d)  (1u << ((d) - 1))
#define CANT    (BIT(DARWIN_SIGKILL) | BIT(DARWIN_SIGSTOP))     // never in a mask
// SIGEMT, SIGINFO, and the 32nd bit, which no signal has and a mask keeps there all the same
#define NOSIG   (BIT(DARWIN_SIGEMT) | BIT(DARWIN_SIGINFO) | BIT(32))

static void mask_host(uint32_t d, sigset_t *h)
{
    sigemptyset(h);
    for (int s = 1; s < NSIG_DARWIN; s++)
        if ((d & (1u << (s - 1))) && sig_host(s) > 0) sigaddset(h, sig_host(s));
}

static uint32_t mask_darwin(const sigset_t *h)
{
    uint32_t d = 0;
    for (int s = 1; s < NSIG_DARWIN; s++)
        if (sig_host(s) > 0 && sigismember(h, sig_host(s)) == 1) d |= 1u << (s - 1);
    return d;
}

static const struct { int darwin, host; } sa_flags[] = {
    { DARWIN_SA_RESTART, SA_RESTART }, { DARWIN_SA_RESETHAND, (int)SA_RESETHAND }, { DARWIN_SA_NOCLDSTOP, SA_NOCLDSTOP },
    { DARWIN_SA_NODEFER, SA_NODEFER }, { DARWIN_SA_NOCLDWAIT, SA_NOCLDWAIT }, { DARWIN_SA_ONSTACK, SA_ONSTACK },
};
#define NFLAGS (sizeof sa_flags / sizeof *sa_flags)

static int code_darwin(int hsig, int code)
{
    switch (code) {
    case SI_USER: case SI_TKILL: return DARWIN_SI_USER;
    case SI_QUEUE: return DARWIN_SI_QUEUE;
    case SI_TIMER: return DARWIN_SI_TIMER;
    case SI_ASYNCIO: return DARWIN_SI_ASYNCIO;
    case SI_MESGQ: return DARWIN_SI_MESGQ;
    }
    if (code <= 0) return 0;
    switch (hsig) {
    case SIGILL:
        switch (code) {
        case ILL_ILLOPC: return DARWIN_ILL_ILLOPC; case ILL_ILLOPN: return DARWIN_ILL_ILLOPN;
        case ILL_ILLADR: return DARWIN_ILL_ILLADR; case ILL_ILLTRP: return DARWIN_ILL_ILLTRP;
        case ILL_PRVOPC: return DARWIN_ILL_PRVOPC; case ILL_PRVREG: return DARWIN_ILL_PRVREG;
        case ILL_COPROC: return DARWIN_ILL_COPROC; case ILL_BADSTK: return DARWIN_ILL_BADSTK;
        }
        return DARWIN_ILL_NOOP;
    case SIGFPE:
        switch (code) {
        case FPE_INTDIV: return DARWIN_FPE_INTDIV; case FPE_INTOVF: return DARWIN_FPE_INTOVF;
        case FPE_FLTDIV: return DARWIN_FPE_FLTDIV; case FPE_FLTOVF: return DARWIN_FPE_FLTOVF;
        case FPE_FLTUND: return DARWIN_FPE_FLTUND; case FPE_FLTRES: return DARWIN_FPE_FLTRES;
        case FPE_FLTINV: return DARWIN_FPE_FLTINV; case FPE_FLTSUB: return DARWIN_FPE_FLTSUB;
        }
        return DARWIN_FPE_NOOP;
    case SIGSEGV: return code == SEGV_MAPERR ? DARWIN_SEGV_MAPERR : DARWIN_SEGV_ACCERR;
    case SIGBUS: return code == BUS_ADRALN ? DARWIN_BUS_ADRALN : code == BUS_ADRERR ? DARWIN_BUS_ADRERR : DARWIN_BUS_OBJERR;
    case SIGTRAP: return code == TRAP_BRKPT ? DARWIN_TRAP_BRKPT : code == TRAP_TRACE ? DARWIN_TRAP_TRACE : 0;
    case SIGCHLD: return code;              // CLD_*: the same six numbers
    }
    return 0;
}

// What the image set for each of macOS's signals. The handler path reads handler and flags
// without the lock.
static struct {
    _Atomic uintptr_t handler;
    _Atomic int flags;
    uint32_t mask;
    struct sigaction host;                  // the host's own action the image's replaced
} act[NSIG_DARWIN];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// The signal the thread's bridge is delivering, as Linux gave it: darwin_signal_chain's arguments.
struct delivery { int sig; siginfo_t *si; void *uc; };
static __thread struct delivery delivering __attribute__((tls_model("initial-exec")));
// The thread's mask bits the host's mask has no place for, and how many handlers of the image
// the thread has run. Read and written in handlers: no record to make first.
static __thread uint32_t nosig __attribute__((tls_model("initial-exec")));
static __thread unsigned handled __attribute__((tls_model("initial-exec")));
// The signal whose handler of the image is running, 0 when none is: one cell for the process,
// set when a handler is called and cleared when it returns, as macOS's own (__in_sigtramp) is.
static _Atomic int in_handler;

int darwin_signal_in_handler(void) { return atomic_load(&in_handler); }
unsigned darwin_signal_handled(void) { return handled; }
uint32_t darwin_signal_nosig(void) { return nosig; }
void darwin_signal_set_nosig(uint32_t bits) { nosig = bits & NOSIG; }

#define PUT(buf, off, v) memcpy((unsigned char *)(buf) + (off), &(v), sizeof(v))
#define GET(buf, off, v) memcpy(&(v), (const unsigned char *)(buf) + (off), sizeof(v))

static struct fpsimd_context *find_fpsimd(mcontext_t *m, uint32_t *esr)
{
    struct fpsimd_context *fp = NULL;
    unsigned char *p = (unsigned char *)m->__reserved, *end = p + sizeof m->__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        struct _aarch64_ctx *c = (struct _aarch64_ctx *)p;
        if (c->magic == 0 || c->size < sizeof *c || c->size > (size_t)(end - p)) break;
        if (c->magic == FPSIMD_MAGIC) fp = (struct fpsimd_context *)p;
        else if (c->magic == ESR_MAGIC) *esr = (uint32_t)((struct esr_context *)p)->esr;
        p += c->size;
    }
    return fp;
}

static void bridge(int sig, siginfo_t *si, void *ctx)
{
    int e = errno, d = sig_darwin(sig);
    uintptr_t h = atomic_load(&act[d].handler);
    int flags = atomic_load(&act[d].flags);
    if (d == 0 || h == DARWIN_SIG_DFL || h == DARWIN_SIG_IGN) { errno = e; return; }    // changed while the signal was on its way
    if (flags & DARWIN_SA_RESETHAND) atomic_store(&act[d].handler, DARWIN_SIG_DFL);
    struct delivery outer = delivering;
    delivering.sig = sig;
    delivering.si = si;
    delivering.uc = ctx;
    // the mask the handler runs under: the host's part of it is the kernel's doing
    uint32_t nosig_outer = nosig;
    nosig |= act[d].mask & NOSIG;
    handled++;
    atomic_store(&in_handler, d);
    if (!(flags & DARWIN_SA_SIGINFO)) {
        ((void (*)(int))h)(d);
        atomic_store(&in_handler, 0);
        nosig = nosig_outer;
    } else {
        ucontext_t *uc = ctx;
        mcontext_t *m = &uc->uc_mcontext;
        unsigned char dsi[DARWIN_SIZEOF_SIGINFO_T] = { 0 }, duc[DARWIN_SIZEOF_UCONTEXT] = { 0 };
        _Alignas(16) unsigned char mc[DARWIN_SIZEOF_MCONTEXT64] = { 0 };
        unsigned char *es = mc + DARWIN_MCONTEXT64___es, *ss = mc + DARWIN_MCONTEXT64___ss, *ns = mc + DARWIN_MCONTEXT64___ns;
        uint32_t esr = 0, u32;
        int32_t i32;
        uint64_t u64;
        struct fpsimd_context *fp = find_fpsimd(m, &esr);

        i32 = d; PUT(dsi, DARWIN_SIGINFO_T_si_signo, i32);
        i32 = si->si_errno; PUT(dsi, DARWIN_SIGINFO_T_si_errno, i32);
        i32 = code_darwin(sig, si->si_code); PUT(dsi, DARWIN_SIGINFO_T_si_code, i32);
        if (sig == SIGILL || sig == SIGFPE || sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP) {
            u64 = (uint64_t)(uintptr_t)si->si_addr; PUT(dsi, DARWIN_SIGINFO_T_si_addr, u64);
        } else if (sig == SIGCHLD) {
            i32 = si->si_pid; PUT(dsi, DARWIN_SIGINFO_T_si_pid, i32);
            u32 = si->si_uid; PUT(dsi, DARWIN_SIGINFO_T_si_uid, u32);
            i32 = si->si_status; PUT(dsi, DARWIN_SIGINFO_T_si_status, i32);
        } else if (sig == SIGIO) {
            u64 = (uint64_t)si->si_band; PUT(dsi, DARWIN_SIGINFO_T_si_band, u64);
        } else if (si->si_code == SI_USER || si->si_code == SI_TKILL || si->si_code == SI_QUEUE) {
            i32 = si->si_pid; PUT(dsi, DARWIN_SIGINFO_T_si_pid, i32);
            u32 = si->si_uid; PUT(dsi, DARWIN_SIGINFO_T_si_uid, u32);
            if (si->si_code == SI_QUEUE) { u64 = (uint64_t)(uintptr_t)si->si_value.sival_ptr; PUT(dsi, DARWIN_SIGINFO_T_si_value, u64); }
        }

        u64 = m->fault_address; PUT(es, DARWIN_ARM_EXCEPTION_STATE64___far, u64);
        PUT(es, DARWIN_ARM_EXCEPTION_STATE64___esr, esr);
        // the Mach exception behind the signal: EXC_BAD_ACCESS 1, EXC_BAD_INSTRUCTION 2, EXC_ARITHMETIC 3, EXC_BREAKPOINT 6
        u32 = sig == SIGSEGV || sig == SIGBUS ? 1 : sig == SIGILL ? 2 : sig == SIGFPE ? 3 : sig == SIGTRAP ? 6 : 0;
        PUT(es, DARWIN_ARM_EXCEPTION_STATE64___exception, u32);
        memcpy(ss + DARWIN_ARM_THREAD_STATE64___x, m->regs, 29 * 8);
        PUT(ss, DARWIN_ARM_THREAD_STATE64___fp, m->regs[29]);
        PUT(ss, DARWIN_ARM_THREAD_STATE64___lr, m->regs[30]);
        PUT(ss, DARWIN_ARM_THREAD_STATE64___sp, m->sp);
        PUT(ss, DARWIN_ARM_THREAD_STATE64___pc, m->pc);
        u32 = (uint32_t)m->pstate; PUT(ss, DARWIN_ARM_THREAD_STATE64___cpsr, u32);
        if (fp) {
            memcpy(ns + DARWIN_ARM_NEON_STATE64___v, fp->vregs, 32 * 16);
            PUT(ns, DARWIN_ARM_NEON_STATE64___fpsr, fp->fpsr);
            PUT(ns, DARWIN_ARM_NEON_STATE64___fpcr, fp->fpcr);
        }

        i32 = (uc->uc_stack.ss_flags & SS_ONSTACK) != 0; PUT(duc, DARWIN_UCONTEXT_uc_onstack, i32);
        u32 = mask_darwin(&uc->uc_sigmask) | nosig_outer; PUT(duc, DARWIN_UCONTEXT_uc_sigmask, u32);
        unsigned char *st = duc + DARWIN_UCONTEXT_uc_stack;
        u64 = (uint64_t)(uintptr_t)uc->uc_stack.ss_sp; PUT(st, DARWIN_STACK_T_ss_sp, u64);
        u64 = uc->uc_stack.ss_size; PUT(st, DARWIN_STACK_T_ss_size, u64);
        i32 = ((uc->uc_stack.ss_flags & SS_ONSTACK) ? DARWIN_SS_ONSTACK : 0) | ((uc->uc_stack.ss_flags & SS_DISABLE) ? DARWIN_SS_DISABLE : 0);
        PUT(st, DARWIN_STACK_T_ss_flags, i32);
        u64 = sizeof mc; PUT(duc, DARWIN_UCONTEXT_uc_mcsize, u64);
        u64 = (uint64_t)(uintptr_t)mc; PUT(duc, DARWIN_UCONTEXT_uc_mcontext, u64);

        ((void (*)(int, void *, void *))h)(d, dsi, duc);
        atomic_store(&in_handler, 0);

        // what the handler left in its context is what the thread resumes with
        memcpy(m->regs, ss + DARWIN_ARM_THREAD_STATE64___x, 29 * 8);
        GET(ss, DARWIN_ARM_THREAD_STATE64___fp, m->regs[29]);
        GET(ss, DARWIN_ARM_THREAD_STATE64___lr, m->regs[30]);
        GET(ss, DARWIN_ARM_THREAD_STATE64___sp, m->sp);
        GET(ss, DARWIN_ARM_THREAD_STATE64___pc, m->pc);
        GET(ss, DARWIN_ARM_THREAD_STATE64___cpsr, u32);
        m->pstate = (m->pstate & ~(uint64_t)0xffffffffu) | u32;
        if (fp) {
            memcpy(fp->vregs, ns + DARWIN_ARM_NEON_STATE64___v, 32 * 16);
            GET(ns, DARWIN_ARM_NEON_STATE64___fpsr, fp->fpsr);
            GET(ns, DARWIN_ARM_NEON_STATE64___fpcr, fp->fpcr);
        }
        GET(duc, DARWIN_UCONTEXT_uc_sigmask, u32);
        for (int s = 1; s < NSIG_DARWIN; s++) {
            int hs = sig_host(s);
            if (hs <= 0 || hs == SIGKILL || hs == SIGSTOP) continue;
            if (u32 & (1u << (s - 1))) sigaddset(&uc->uc_sigmask, hs);
            else sigdelset(&uc->uc_sigmask, hs);
        }
        nosig = u32 & NOSIG;
    }
    delivering = outer;
    errno = e;
}

// The host's own handler for the signal the image's handler was given, called from that
// handler with macOS's arguments: the host's gets Linux's, as the kernel made them.
static void darwin_signal_chain(int d, void *dsi, void *duc)
{
    if (d <= 0 || d >= NSIG_DARWIN || sig_host(d) <= 0) return;
    struct sigaction *o = &act[d].host;
    int hs = sig_host(d);
    if (delivering.sig != hs) {
        fprintf(stderr, "[darwin] the host's handler for signal %d was called outside that signal's delivery\n", d);
        abort();
    }
    if (o->sa_flags & SA_SIGINFO) o->sa_sigaction(hs, delivering.si, delivering.uc);
    else if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) o->sa_handler(hs);
}

static void put_action(unsigned char *out, uintptr_t handler, uint32_t mask, int flags)
{
    memset(out, 0, DARWIN_SIZEOF_SIGACTION);
    PUT(out, 0, handler);
    PUT(out, DARWIN_SIGACTION_sa_mask, mask);
    PUT(out, DARWIN_SIGACTION_sa_flags, flags);
}

// int sigaction(int, const struct sigaction *, struct sigaction *)
static long darwin_sigaction(int d, const unsigned char *new, unsigned char *old)
{
    int hs = d > 0 && d < NSIG_DARWIN ? sig_host(d) : -1;
    if (hs < 0 || (new && (d == DARWIN_SIGKILL || d == DARWIN_SIGSTOP))) { errno = EINVAL; return -1; }
    struct sigaction cur;
    memset(&cur, 0, sizeof cur);
    pthread_mutex_lock(&lock);
    if (hs > 0 && sigaction(hs, NULL, &cur) != 0) { pthread_mutex_unlock(&lock); return -1; }
    // the bridge is in place: the image's action stands; else whatever the host has now
    int ours = hs == 0 || ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == bridge);
    if (old) {
        if (ours) {
            put_action(old, atomic_load(&act[d].handler), act[d].mask, atomic_load(&act[d].flags));
        } else {
            int flags = 0;
            for (size_t i = 0; i < NFLAGS; i++) if (cur.sa_flags & sa_flags[i].host) flags |= sa_flags[i].darwin;
            if (!(cur.sa_flags & SA_SIGINFO) && (cur.sa_handler == SIG_DFL || cur.sa_handler == SIG_IGN)) {
                put_action(old, cur.sa_handler == SIG_DFL ? DARWIN_SIG_DFL : DARWIN_SIG_IGN, mask_darwin(&cur.sa_mask), flags);
            } else {
                // the host's own handler: kept here, and the image is given the chain in its place
                act[d].host = cur;
                put_action(old, (uintptr_t)darwin_signal_chain, mask_darwin(&cur.sa_mask), flags | DARWIN_SA_SIGINFO);
            }
        }
    }
    if (new) {
        uintptr_t handler;
        uint32_t mask;
        int flags;
        GET(new, 0, handler);
        GET(new, DARWIN_SIGACTION_sa_mask, mask);
        GET(new, DARWIN_SIGACTION_sa_flags, flags);
        if (handler == (uintptr_t)darwin_signal_chain) {
            // the host's action, handed back
            if (hs > 0 && sigaction(hs, &act[d].host, NULL) != 0) { pthread_mutex_unlock(&lock); return -1; }
        } else {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            if (handler == DARWIN_SIG_DFL) sa.sa_handler = SIG_DFL;
            else if (handler == DARWIN_SIG_IGN) sa.sa_handler = SIG_IGN;
            else { sa.sa_sigaction = bridge; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; }
            for (size_t i = 0; i < NFLAGS; i++) if (flags & sa_flags[i].darwin) sa.sa_flags |= sa_flags[i].host;
            mask_host(mask, &sa.sa_mask);
            act[d].mask = mask;
            atomic_store(&act[d].flags, flags);
            atomic_store(&act[d].handler, handler);
            if (hs > 0 && sigaction(hs, &sa, NULL) != 0) { pthread_mutex_unlock(&lock); return -1; }
            if (hs == 0 && handler != DARWIN_SIG_DFL && handler != DARWIN_SIG_IGN)
                darwin_unanswered(d == DARWIN_SIGEMT ? "sigaction(SIGEMT)" : "sigaction(SIGINFO)",
                                  "Linux has no such signal: the handler is kept and never called");
        }
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

// void (*signal(int, void (*)(int)))(int): BSD's, the handler stays and calls restart.
static void *darwin_signal(int d, void *handler)
{
    unsigned char new[DARWIN_SIZEOF_SIGACTION], old[DARWIN_SIZEOF_SIGACTION];
    uintptr_t was;
    put_action(new, (uintptr_t)handler, 0, DARWIN_SA_RESTART);
    if (darwin_sigaction(d, new, old) != 0) return (void *)(uintptr_t)DARWIN_SIG_ERR;
    GET(old, 0, was);
    return (void *)was;
}

// The calling thread's mask. how: macOS's SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK, looked at only
// when there is a set, as there. 0, or macOS's error number with nothing changed or written.
static int mask_change(int how, const uint32_t *set, uint32_t *old)
{
    sigset_t cur;
    pthread_sigmask(SIG_SETMASK, NULL, &cur);
    uint32_t was = mask_darwin(&cur) | nosig;
    if (set) {
        uint32_t m = *set & ~CANT, now;
        if (how == DARWIN_SIG_BLOCK) now = was | m;
        else if (how == DARWIN_SIG_UNBLOCK) now = was & ~m;
        else if (how == DARWIN_SIG_SETMASK) now = m;
        else return DARWIN_EINVAL;
        // the host's own signals (the one that stops a thread, the profiler's) stay as they are
        for (int s = 1; s < NSIG_DARWIN; s++) {
            int hs = sig_host(s);
            if (hs <= 0 || hs == SIGKILL || hs == SIGSTOP) continue;
            if (now & BIT(s)) sigaddset(&cur, hs);
            else sigdelset(&cur, hs);
        }
        nosig = now & NOSIG;                // before a signal let through here runs its handler
        pthread_sigmask(SIG_SETMASK, &cur, NULL);
    }
    if (old) *old = was;
    return 0;
}

uint32_t darwin_sigmask(void)
{
    uint32_t m = 0;
    mask_change(0, NULL, &m);
    return m;
}

void darwin_set_sigmask(uint32_t mask) { mask_change(DARWIN_SIG_SETMASK, &mask, NULL); }

// int pthread_sigmask(int, const sigset_t *, sigset_t *): the error is the return value
static long darwin_pthread_sigmask(int how, const uint32_t *set, uint32_t *old)
{
    int e = errno;
    int r = mask_change(how, set, old);
    errno = e;
    return r;
}

// int sigprocmask(int, const sigset_t *, sigset_t *): the calling thread's mask there too
static long darwin_sigprocmask(int how, const uint32_t *set, uint32_t *old)
{
    int e = errno;
    int r = mask_change(how, set, old);
    errno = e;
    if (r == 0) return 0;
    errno = darwin_errno_host(r);
    return -1;
}

// int pthread_kill(pthread_t, int)
static long darwin_pthread_kill(pthread_t th, int d)
{
    if (d == 0) return DARWIN_ERR(pthread_kill(th, 0));
    int hs = d > 0 && d < NSIG_DARWIN ? sig_host(d) : -1;
    if (hs < 0) return DARWIN_EINVAL;
    if (hs == 0) {
        darwin_unanswered(d == DARWIN_SIGEMT ? "pthread_kill(SIGEMT)" : "pthread_kill(SIGINFO)",
                          "Linux has no such signal: none is sent");
        return 0;
    }
    return DARWIN_ERR(pthread_kill(th, hs));
}

const struct darwin_sym darwin_syms_signal[] = {
    SHIM(sigaction), SHIM(signal), SHIM(sigprocmask), SHIM(pthread_sigmask), SHIM(pthread_kill),
    { NULL, NULL }
};
