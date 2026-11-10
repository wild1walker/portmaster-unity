// sh_thread.c - threads: pthread's objects as macOS lays them out, the Mach thread calls a
// collector stops the world with, dispatch semaphores, the __ulock waits.
//
// What a thread_suspend may interrupt decides how this file is written. macOS stops a thread
// from outside, wherever it is; here a signal does, so a thread can be stopped inside a shim.
// The calls a collector makes while the others are stopped (the Mach calls, a mutex, a
// condition variable, mmap) therefore take no lock a stopped thread may hold and allocate
// nothing: a condition variable lives in macOS's own 48 bytes, a statically initialized mutex
// needs no first use, the thread's record is reached through an initial-exec thread-local.
#define _GNU_SOURCE
#include "darwin.h"

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PUT(buf, off, v) memcpy((unsigned char *)(buf) + (off), &(v), sizeof(v))

static long futex(void *addr, int op, uint32_t val, const struct timespec *rel)
{
    return syscall(SYS_futex, addr, op, val, rel, NULL, 0);
}

static int64_t now_ns(clockid_t clk)
{
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// Sleeps while *addr is val, until the monotonic clock reaches deadline (-1: no end). 0: woken
// or *addr is another value; -1: the time ran out. A signal's return does not end it.
static int futex_wait_until(_Atomic uint32_t *addr, uint32_t val, int64_t deadline)
{
    for (;;) {
        struct timespec rel, *to = NULL;
        if (deadline >= 0) {
            int64_t left = deadline - now_ns(CLOCK_MONOTONIC);
            if (left <= 0) return -1;
            rel.tv_sec = left / 1000000000;
            rel.tv_nsec = left % 1000000000;
            to = &rel;
        }
        if (futex(addr, FUTEX_WAIT_PRIVATE, val, to) == 0) return 0;
        if (errno == EAGAIN) return 0;
        if (errno == ETIMEDOUT) return -1;
        if (errno != EINTR) { fprintf(stderr, "[darwin] futex wait: %s\n", strerror(errno)); abort(); }
        if (atomic_load(addr) != val) return 0;
    }
}

// A count that is waited for: the thread start below and dispatch's semaphores.
static void tokens_post(_Atomic uint32_t *tokens)
{
    atomic_fetch_add(tokens, 1);
    futex(tokens, FUTEX_WAKE_PRIVATE, 1, NULL);
}

// 0: took one; -1: none until the deadline
static int tokens_take(_Atomic uint32_t *tokens, int64_t deadline)
{
    for (;;) {
        uint32_t t = atomic_load(tokens);
        while (t > 0)
            if (atomic_compare_exchange_weak(tokens, &t, t - 1)) return 0;
        if (futex_wait_until(tokens, 0, deadline) != 0) {
            t = atomic_load(tokens);
            while (t > 0)
                if (atomic_compare_exchange_weak(tokens, &t, t - 1)) return 0;
            return -1;
        }
    }
}

// ---- the threads that run image code ------------------------------------------------------

#define STOP_SIGNAL (SIGRTMIN + 8)          // the host's profiler and hitch log have +5, +6 and +7
#define STOPPED     0x10000u                // in word, above the count of suspends
#define COUNT(w)    ((w) & 0xffffu)
#define ALT_STACK   ((size_t)DARWIN_SIGSTKSZ)

struct dthread {
    _Atomic int            tid;             // 0: free. Stands for the thread's Mach port
    pthread_t              thread;
    _Atomic uint32_t       word;            // the thread_suspend calls that hold it | STOPPED
    _Atomic(ucontext_t *)  uc;              // its registers while it is stopped
    _Atomic unsigned       stops;           // how often it was stopped
    void                  *alt;             // the alternate stack mapped for it, guard page first
    size_t                 alt_size;
    struct dthread        *next;
};

// Records are reused, never freed: a signal handler may hold one.
static struct dthread *threads;
// The list, and one thread_suspend, thread_resume or thread_get_state at a time.
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  stop_once = PTHREAD_ONCE_INIT;
static int             stops_said;          // UA_MACHO_STOPS, below
static __thread struct dthread *self __attribute__((tls_model("initial-exec")));

// A thread_suspend's signal: the thread says it is stopped and sleeps until the last
// thread_resume. Every other signal waits meanwhile, so no handler of the image runs in a
// stopped thread.
static void stop_handler(int sig, siginfo_t *si, void *ctx)
{
    struct dthread *t = self;
    if (!t) return;
    int saved = errno;
    atomic_store(&t->uc, (ucontext_t *)ctx);
    uint32_t w = atomic_load(&t->word);
    for (;;) {
        if (COUNT(w) == 0) { errno = saved; return; }       // resumed before the signal arrived
        if (atomic_compare_exchange_weak(&t->word, &w, w | STOPPED)) break;
    }
    w |= STOPPED;
    atomic_fetch_add(&t->stops, 1);
    futex(&t->word, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);
    for (;;) {
        if (COUNT(w) == 0) {
            // leaves only if no thread_suspend came since: one that did finds it still stopped
            if (atomic_compare_exchange_weak(&t->word, &w, 0)) break;
            continue;
        }
        futex(&t->word, FUTEX_WAIT_PRIVATE, w, NULL);
        w = atomic_load(&t->word);
    }
    errno = saved;
}

static void stop_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = stop_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigfillset(&sa.sa_mask);
    if (sigaction(STOP_SIGNAL, &sa, NULL) != 0) { fprintf(stderr, "[darwin] no signal to stop threads with: %s\n", strerror(errno)); abort(); }
    const char *e = getenv("UA_MACHO_STOPS");
    stops_said = e && *e && *e != '0';
}

int darwin_thread_stop_signal(void) { return STOP_SIGNAL; }

unsigned darwin_thread_stops(void)
{
    struct dthread *t = self;
    return t ? atomic_load(&t->stops) : 0;
}

static int gone(int tid)
{
    return syscall(SYS_tgkill, getpid(), tid, 0) != 0 && errno == ESRCH;
}

// ---- a thread's buffers ------------------------------------------------------------------------
static pthread_key_t  bufs_key;
static pthread_once_t bufs_once = PTHREAD_ONCE_INIT;

static void bufs_init(void)
{
    if (pthread_key_create(&bufs_key, free) != 0) { fprintf(stderr, "[darwin] no thread key for a thread's buffers\n"); abort(); }
}

// A destructor of the thread that runs after this key's (an image's thread-local one calling
// strerror, say) gets a new block: glibc goes round the keys again and frees that one too.
struct darwin_thread_bufs *darwin_thread_bufs(void)
{
    pthread_once(&bufs_once, bufs_init);
    struct darwin_thread_bufs *b = pthread_getspecific(bufs_key);
    if (b) return b;
    b = calloc(1, sizeof *b);
    if (!b || pthread_setspecific(bufs_key, b) != 0) { fprintf(stderr, "[darwin] no memory for a thread's buffers\n"); abort(); }
    return b;
}

void darwin_thread_enter(void)
{
    if (self) return;
    int saved = errno;
    pthread_once(&stop_once, stop_init);
    void *alt = NULL;
    size_t alt_size = 0;
    stack_t ss;
    if (sigaltstack(NULL, &ss) != 0 || (ss.ss_flags & SS_DISABLE)) {
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        alt_size = page + ALT_STACK;
        alt = mmap(NULL, alt_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (alt == MAP_FAILED || mprotect(alt, page, PROT_NONE) != 0) { fprintf(stderr, "[darwin] no signal stack for a thread: %s\n", strerror(errno)); abort(); }
        ss.ss_sp = (char *)alt + page;
        ss.ss_size = ALT_STACK;
        ss.ss_flags = 0;
        if (sigaltstack(&ss, NULL) != 0) { fprintf(stderr, "[darwin] sigaltstack: %s\n", strerror(errno)); abort(); }
    }
    int tid = (int)syscall(SYS_gettid);
    // allocated before the lock: a thread stopped inside malloc must not keep this one waiting
    // with the lock a thread_suspend needs
    struct dthread *spare = calloc(1, sizeof *spare);
    if (!spare) { fprintf(stderr, "[darwin] no memory for a thread's record\n"); abort(); }
    pthread_mutex_lock(&reg_lock);
    // A thread gives nothing back at its own end (the image's thread-exit code may still run
    // after any hook here): the records of the threads that are gone are taken back now.
    struct dthread *t = NULL;
    for (struct dthread *e = threads; e; e = e->next) {
        int et = atomic_load(&e->tid);
        if (et && (et == tid || gone(et))) {
            if (e->alt) munmap(e->alt, e->alt_size);
            e->alt = NULL;
            atomic_store(&e->word, 0);
            atomic_store(&e->tid, 0);
            et = 0;
        }
        if (!et && !t) t = e;
    }
    if (!t) {
        t = spare;
        spare = NULL;
        t->next = threads;
        threads = t;
    }
    t->thread = pthread_self();
    t->alt = alt;
    t->alt_size = alt_size;
    atomic_store(&t->uc, NULL);
    self = t;
    atomic_store(&t->tid, tid);             // from here a thread_suspend can name it
    pthread_mutex_unlock(&reg_lock);
    free(spare);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, STOP_SIGNAL);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    errno = saved;
}

uint32_t darwin_thread_port(void)
{
    if (!self) darwin_thread_enter();
    return (uint32_t)atomic_load(&self->tid);
}

static struct dthread *by_port(uint32_t port)       // reg_lock held
{
    if (!port) return NULL;
    for (struct dthread *e = threads; e; e = e->next)
        if ((uint32_t)atomic_load(&e->tid) == port) return e;
    return NULL;
}

// ---- Mach: a thread's port is its Linux thread number ---------------------------------------

static uint32_t task_self = 0x103;          // mach_task_self_: a port name, never looked at

static long darwin_mach_port_deallocate(uint32_t task, uint32_t name) { return DARWIN_KERN_SUCCESS; }

static uint32_t darwin_mach_thread_self(void) { return darwin_thread_port(); }

static uint32_t darwin_pthread_mach_thread_np(pthread_t th)
{
    if (pthread_equal(th, pthread_self())) return darwin_thread_port();
    uint32_t port = 0;
    pthread_mutex_lock(&reg_lock);
    for (struct dthread *e = threads; e; e = e->next) {
        int et = atomic_load(&e->tid);
        if (et && pthread_equal(e->thread, th) && !gone(et)) { port = (uint32_t)et; break; }
    }
    pthread_mutex_unlock(&reg_lock);
    if (!port) darwin_unanswered("pthread_mach_thread_np", "a thread that has run no code of the images");
    return port;
}

// UA_MACHO_STOPS=1: what the stops cost (a collector stops every thread for each of its
// steps), said every 5 s in which there were any. Stopping is the time a thread_suspend took
// to have the thread stand; stopped is the time from the first thread held to the last let go.
static struct {                             // reg_lock held
    unsigned held;                          // the suspends in force
    int64_t  began, last;
    uint64_t times, suspends;
    int64_t  stopping, stopping_max, stopped, stopped_max;
} stops;

static void stops_held(int64_t t0)
{
    int64_t now = now_ns(CLOCK_MONOTONIC);
    if (stops.held++ == 0) stops.began = t0;
    if (!stops.last) stops.last = t0;       // the first 5 s are from the first stop
    stops.suspends++;
    stops.stopping += now - t0;
    if (now - t0 > stops.stopping_max) stops.stopping_max = now - t0;
}

static void stops_let_go(void)
{
    if (--stops.held) return;
    int64_t d = now_ns(CLOCK_MONOTONIC) - stops.began;
    stops.times++;
    stops.stopped += d;
    if (d > stops.stopped_max) stops.stopped_max = d;
}

// after reg_lock is given back, and with no thread held: one of them may stand in stdio
static void stops_say(void)
{
    if (!stops_said) return;
    pthread_mutex_lock(&reg_lock);
    int64_t now = now_ns(CLOCK_MONOTONIC);
    if (stops.held || !stops.times || now - stops.last < 5000000000) {
        pthread_mutex_unlock(&reg_lock);
        return;
    }
    uint64_t times = stops.times, suspends = stops.suspends;
    int64_t stopping = stops.stopping, stopping_max = stops.stopping_max, stopped = stops.stopped, stopped_max = stops.stopped_max;
    double span = (double)(now - stops.last) / 1e9;
    stops.times = stops.suspends = 0;
    stops.stopping = stops.stopping_max = stops.stopped = stops.stopped_max = 0;
    stops.last = now;
    pthread_mutex_unlock(&reg_lock);
    darwin_log("stops in %.1f s: %llu, stopped %.1f ms in all, %.0f us each, %.0f us the longest; %llu thread_suspend of %.0f us each, %.0f us the longest",
               span, (unsigned long long)times, (double)stopped / 1e6, (double)stopped / 1e3 / (double)times, (double)stopped_max / 1e3,
               (unsigned long long)suspends, suspends ? (double)stopping / 1e3 / (double)suspends : 0, (double)stopping_max / 1e3);
}

// reg_lock held. The thread's own suspend is left to the caller.
static long suspend_locked(struct dthread *t)
{
    int64_t t0 = stops_said ? now_ns(CLOCK_MONOTONIC) : 0;
    uint32_t w = atomic_load(&t->word);
    do {
        if (COUNT(w) == 0xffffu) return DARWIN_KERN_FAILURE;
    } while (!atomic_compare_exchange_weak(&t->word, &w, w + 1));
    if (t == self) {
        if (stops_said) stops_held(t0);
        return DARWIN_KERN_SUCCESS;
    }
    int tid = atomic_load(&t->tid);
    // a thread still inside the handler after its last resume stays there: no signal
    if (COUNT(w) == 0 && !(w & STOPPED) && syscall(SYS_tgkill, getpid(), tid, STOP_SIGNAL) != 0) {
        atomic_fetch_sub(&t->word, 1);
        return DARWIN_KERN_INVALID_ARGUMENT;
    }
    int said = 0;
    for (;;) {
        w = atomic_load(&t->word);
        if (w & STOPPED) {
            if (stops_said) stops_held(t0);
            return DARWIN_KERN_SUCCESS;
        }
        if (futex_wait_until(&t->word, w, now_ns(CLOCK_MONOTONIC) + 5000000000) == 0) continue;
        if (gone(tid)) {                    // ended with the signal on its way
            atomic_fetch_sub(&t->word, 1);
            return DARWIN_KERN_INVALID_ARGUMENT;
        }
        if (!said) darwin_log("thread %d has not stopped for thread_suspend in 5 s (signal %d blocked?): still waiting", tid, STOP_SIGNAL);
        said = 1;
    }
}

static long resume_locked(struct dthread *t)
{
    uint32_t w = atomic_load(&t->word);
    do {
        if (COUNT(w) == 0) return DARWIN_KERN_FAILURE;
    } while (!atomic_compare_exchange_weak(&t->word, &w, w - 1));
    if (COUNT(w) == 1) futex(&t->word, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);
    if (stops_said) stops_let_go();
    return DARWIN_KERN_SUCCESS;
}

// kern_return_t thread_suspend(thread_act_t): back when the thread runs no more code
static long darwin_thread_suspend(uint32_t port)
{
    int saved = errno;
    pthread_mutex_lock(&reg_lock);
    struct dthread *t = by_port(port);
    long r = t ? suspend_locked(t) : DARWIN_KERN_INVALID_ARGUMENT;
    pthread_mutex_unlock(&reg_lock);
    // its own: stops here, until another thread resumes it
    if (r == DARWIN_KERN_SUCCESS && t == self) syscall(SYS_tgkill, getpid(), (int)port, STOP_SIGNAL);
    errno = saved;
    return r;
}

static long darwin_thread_resume(uint32_t port)
{
    int saved = errno;
    pthread_mutex_lock(&reg_lock);
    struct dthread *t = by_port(port);
    long r = t ? resume_locked(t) : DARWIN_KERN_INVALID_ARGUMENT;
    pthread_mutex_unlock(&reg_lock);
    stops_say();
    errno = saved;
    return r;
}

static void put_state(unsigned char *st, const ucontext_t *uc)
{
    const mcontext_t *m = &uc->uc_mcontext;
    uint32_t cpsr = (uint32_t)m->pstate;
    memset(st, 0, DARWIN_SIZEOF_ARM_THREAD_STATE64);
    memcpy(st + DARWIN_ARM_THREAD_STATE64___x, m->regs, 29 * 8);
    PUT(st, DARWIN_ARM_THREAD_STATE64___fp, m->regs[29]);
    PUT(st, DARWIN_ARM_THREAD_STATE64___lr, m->regs[30]);
    PUT(st, DARWIN_ARM_THREAD_STATE64___sp, m->sp);
    PUT(st, DARWIN_ARM_THREAD_STATE64___pc, m->pc);
    PUT(st, DARWIN_ARM_THREAD_STATE64___cpsr, cpsr);
}

// kern_return_t thread_get_state(thread_act_t, thread_state_flavor_t, thread_state_t,
// mach_msg_type_number_t *): the general registers. A thread that runs is stopped for the
// reading; the caller's own are those it has on return from here.
static long darwin_thread_get_state(uint32_t port, int flavor, unsigned char *state, uint32_t *count)
{
    if (flavor != DARWIN_ARM_THREAD_STATE64) {
        darwin_unanswered("thread_get_state", "a flavor other than ARM_THREAD_STATE64");
        return DARWIN_KERN_INVALID_ARGUMENT;
    }
    if (*count < DARWIN_ARM_THREAD_STATE64_COUNT) return DARWIN_KERN_INVALID_ARGUMENT;
    int saved = errno;
    long r = DARWIN_KERN_SUCCESS;
    pthread_mutex_lock(&reg_lock);
    struct dthread *t = by_port(port);
    if (!t) {
        r = DARWIN_KERN_INVALID_ARGUMENT;
    } else if (t == self) {
        ucontext_t uc;
        getcontext(&uc);
        put_state(state, &uc);
    } else {
        r = suspend_locked(t);
        if (r == DARWIN_KERN_SUCCESS) {
            put_state(state, atomic_load(&t->uc));
            resume_locked(t);
        }
    }
    pthread_mutex_unlock(&reg_lock);
    if (r == DARWIN_KERN_SUCCESS) *count = DARWIN_ARM_THREAD_STATE64_COUNT;
    errno = saved;
    return r;
}

// ---- pthread ------------------------------------------------------------------------------

// A mutex: macOS's signature word, then glibc's mutex in the opaque bytes. The default static
// initializer's bytes after the signature are zero, which is glibc's default mutex too.
#define MUTEX_LIVE 0x4d555458L              // macOS's own signature of a mutex in use
#define COND_LIVE  0x434f4e44L
_Static_assert(sizeof(pthread_mutex_t) + 8 <= DARWIN_SIZEOF_PTHREAD_MUTEX, "glibc's mutex behind the signature");
_Static_assert(sizeof(pthread_attr_t) <= DARWIN_SIZEOF_PTHREAD_ATTR, "pthread_attr_t");
_Static_assert(sizeof(pthread_mutexattr_t) <= DARWIN_SIZEOF_PTHREAD_MUTEXATTR, "pthread_mutexattr_t");
_Static_assert(sizeof(pthread_t) == DARWIN_SIZEOF_PTHREAD_T, "pthread_t");

static long sig_of(void *obj) { return __atomic_load_n((long *)obj, __ATOMIC_ACQUIRE); }
static void sig_set(void *obj, long sig) { __atomic_store_n((long *)obj, sig, __ATOMIC_RELEASE); }

static pthread_mutex_t first_lock = PTHREAD_MUTEX_INITIALIZER;

// The recursive and error-checking static initializers: glibc's mutex gets its kind at the
// first use. (Under a lock: the one place a mutex call takes one. A collector's own locks are
// default ones.) NULL: not a mutex, which macOS answers with EINVAL.
static pthread_mutex_t *mutex_first(void *m, long sig)
{
    int kind = sig == DARWIN_PTHREAD_RECURSIVE_SIG_INIT ? PTHREAD_MUTEX_RECURSIVE
             : sig == DARWIN_PTHREAD_ERRORCHECK_SIG_INIT ? PTHREAD_MUTEX_ERRORCHECK : -1;
    if (kind < 0) return NULL;
    pthread_mutex_t *h = (pthread_mutex_t *)((char *)m + 8);
    pthread_mutex_lock(&first_lock);
    if (sig_of(m) == sig) {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, kind);
        pthread_mutex_init(h, &a);
        pthread_mutexattr_destroy(&a);
        sig_set(m, MUTEX_LIVE);
    }
    pthread_mutex_unlock(&first_lock);
    return h;
}

static inline pthread_mutex_t *mutex_of(void *m)
{
    long sig = sig_of(m);
    if (__builtin_expect(sig == MUTEX_LIVE || sig == DARWIN_PTHREAD_MUTEX_SIG_INIT, 1)) return (pthread_mutex_t *)((char *)m + 8);
    return mutex_first(m, sig);
}

static long darwin_pthread_mutex_init(void *m, const pthread_mutexattr_t *attr)
{
    int r = pthread_mutex_init((pthread_mutex_t *)((char *)m + 8), attr);
    if (r == 0) sig_set(m, MUTEX_LIVE);
    return DARWIN_ERR(r);
}

static long darwin_pthread_mutex_destroy(void *m)
{
    pthread_mutex_t *h = mutex_of(m);
    if (!h) return DARWIN_EINVAL;
    int r = pthread_mutex_destroy(h);
    if (r == 0) sig_set(m, 0);
    return DARWIN_ERR(r);
}

static long darwin_pthread_mutex_lock(void *m)
{
    pthread_mutex_t *h = mutex_of(m);
    if (!h) return DARWIN_EINVAL;
    int r = pthread_mutex_lock(h);
    return r ? DARWIN_ERR(r) : 0;
}

static long darwin_pthread_mutex_trylock(void *m)
{
    pthread_mutex_t *h = mutex_of(m);
    if (!h) return DARWIN_EINVAL;
    int r = pthread_mutex_trylock(h);
    return r ? DARWIN_ERR(r) : 0;
}

static long darwin_pthread_mutex_unlock(void *m)
{
    pthread_mutex_t *h = mutex_of(m);
    if (!h) return DARWIN_EINVAL;
    int r = pthread_mutex_unlock(h);
    return r ? DARWIN_ERR(r) : 0;
}

static long darwin_pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    int kind = type == DARWIN_PTHREAD_MUTEX_NORMAL ? PTHREAD_MUTEX_NORMAL
             : type == DARWIN_PTHREAD_MUTEX_ERRORCHECK ? PTHREAD_MUTEX_ERRORCHECK
             : type == DARWIN_PTHREAD_MUTEX_RECURSIVE ? PTHREAD_MUTEX_RECURSIVE : -1;
    if (kind < 0) return DARWIN_EINVAL;
    return DARWIN_ERR(pthread_mutexattr_settype(attr, kind));
}

// A condition variable, whole in macOS's 48 bytes: a count of signals the waiters sleep on (a
// waiter that read it before a signal does not sleep), and how many wait. A signal may wake
// more than one waiter, which a condition variable is allowed to do.
struct dcond { long sig; _Atomic uint32_t seq; _Atomic uint32_t waiters; };
_Static_assert(sizeof(struct dcond) <= DARWIN_SIZEOF_PTHREAD_COND, "the condition variable in macOS's bytes");

static struct dcond *cond_of(void *c)
{
    long sig = sig_of(c);
    return sig == COND_LIVE || sig == DARWIN_PTHREAD_COND_SIG_INIT ? c : NULL;
}

static long darwin_pthread_cond_init(void *c, const void *attr)
{
    struct dcond *d = c;
    atomic_store(&d->seq, 0);
    atomic_store(&d->waiters, 0);
    sig_set(c, COND_LIVE);
    return 0;
}

static long darwin_pthread_cond_destroy(void *c)
{
    struct dcond *d = cond_of(c);
    if (!d) return DARWIN_EINVAL;
    if (atomic_load(&d->waiters)) return DARWIN_EBUSY;
    sig_set(c, 0);
    return 0;
}

// deadline: on the monotonic clock, -1 for none
static long cond_wait(void *c, void *m, int64_t deadline)
{
    struct dcond *d = cond_of(c);
    pthread_mutex_t *h = mutex_of(m);
    if (!d || !h) return DARWIN_EINVAL;
    uint32_t seq = atomic_load(&d->seq);
    atomic_fetch_add(&d->waiters, 1);
    int r = pthread_mutex_unlock(h);
    if (r != 0) {
        atomic_fetch_sub(&d->waiters, 1);
        return DARWIN_ERR(r);
    }
    int timed_out = futex_wait_until(&d->seq, seq, deadline) != 0;
    atomic_fetch_sub(&d->waiters, 1);
    pthread_mutex_lock(h);
    return timed_out ? DARWIN_ETIMEDOUT : 0;
}

static long darwin_pthread_cond_wait(void *c, void *m) { return cond_wait(c, m, -1); }

// The time is the wall clock's; macOS turns it into a wait of that length when called, and so
// does this.
static long darwin_pthread_cond_timedwait(void *c, void *m, const struct timespec *abs)
{
    if (!abs || abs->tv_nsec < 0 || abs->tv_nsec >= 1000000000) return DARWIN_EINVAL;
    int64_t left = (int64_t)abs->tv_sec * 1000000000 + abs->tv_nsec - now_ns(CLOCK_REALTIME);
    if (left < 0) left = 0;
    return cond_wait(c, m, now_ns(CLOCK_MONOTONIC) + left);
}

static long cond_wake(void *c, int n)
{
    struct dcond *d = cond_of(c);
    if (!d) return DARWIN_EINVAL;
    if (atomic_load(&d->waiters)) {
        atomic_fetch_add(&d->seq, 1);
        futex(&d->seq, FUTEX_WAKE_PRIVATE, (uint32_t)n, NULL);
    }
    return 0;
}

static long darwin_pthread_cond_signal(void *c) { return cond_wake(c, 1); }
static long darwin_pthread_cond_broadcast(void *c) { return cond_wake(c, INT_MAX); }

// pthread_key_t is an unsigned long there
static long darwin_pthread_key_create(unsigned long *key, void (*gone_fn)(void *))
{
    pthread_key_t k;
    int r = pthread_key_create(&k, gone_fn);
    if (r == 0) *key = k;
    return DARWIN_ERR(r);
}

struct start { void *(*fn)(void *); void *arg; _Atomic uint32_t ready; };

static void *thread_start(void *v)
{
    struct start *s = v;
    void *(*fn)(void *) = s->fn;
    void *arg = s->arg;
    darwin_thread_enter();
    tokens_post(&s->ready);                 // s was the creating thread's
    return fn(arg);
}

// Back once the new thread has its signal stack and its port, as a macOS thread has from its
// first instruction.
static long darwin_pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    struct start s = { fn, arg, 0 };
    int r = pthread_create(out, attr, thread_start, &s);
    if (r != 0) return DARWIN_ERR(r);
    int saved = errno;
    tokens_take(&s.ready, -1);
    errno = saved;
    return 0;
}

static long darwin_pthread_join(pthread_t th, void **value) { return DARWIN_ERR(pthread_join(th, value)); }
static long darwin_pthread_detach(pthread_t th) { return DARWIN_ERR(pthread_detach(th)); }

static long darwin_pthread_setcancelstate(int state, int *old)
{
    int h = state == DARWIN_PTHREAD_CANCEL_ENABLE ? PTHREAD_CANCEL_ENABLE
          : state == DARWIN_PTHREAD_CANCEL_DISABLE ? PTHREAD_CANCEL_DISABLE : -1;
    if (h < 0) return DARWIN_EINVAL;
    int was;
    int r = pthread_setcancelstate(h, &was);
    if (r == 0 && old) *old = was == PTHREAD_CANCEL_ENABLE ? DARWIN_PTHREAD_CANCEL_ENABLE : DARWIN_PTHREAD_CANCEL_DISABLE;
    return DARWIN_ERR(r);
}

// int pthread_setname_np(const char *): the calling thread's, there. Linux keeps 15 bytes of it.
static long darwin_pthread_setname_np(const char *name)
{
    char buf[16];
    if (!name) return DARWIN_EINVAL;
    snprintf(buf, sizeof buf, "%s", name);
    pthread_setname_np(pthread_self(), buf);
    return 0;
}

// void *pthread_get_stackaddr_np(pthread_t): where the stack starts, its highest address
static void *darwin_pthread_get_stackaddr_np(pthread_t th)
{
    pthread_attr_t a;
    void *addr = NULL;
    size_t size = 0;
    int saved = errno;
    if (pthread_getattr_np(th, &a) != 0) {
        darwin_unanswered("pthread_get_stackaddr_np", "the thread's stack is not known");
        errno = saved;
        return NULL;
    }
    pthread_attr_getstack(&a, &addr, &size);
    pthread_attr_destroy(&a);
    errno = saved;
    return (char *)addr + size;
}

// ---- dispatch: semaphores, as libdispatch counts them -----------------------------------------

#define DSEMA_MAGIC   0x616d657364617564ull
#define DSEMA_TIMEOUT 49                    // KERN_OPERATION_TIMED_OUT: what a wait that ran out returns
#define WALL_NOW      (~1ull)               // DISPATCH_WALLTIME_NOW

struct dsema {
    uint64_t         magic;
    _Atomic long     refs;
    _Atomic long     value;                 // below zero: that many wait
    _Atomic uint32_t wakes;                 // signals for the waiters
};

static struct dsema *dsema_of(void *obj, const char *call)
{
    struct dsema *s = obj;
    if (s && s->magic == DSEMA_MAGIC) return s;
    fprintf(stderr, "[darwin] %s: %p is not a semaphore made by dispatch_semaphore_create\n", call, obj);
    abort();
}

static void *darwin_dispatch_semaphore_create(long value)
{
    if (value < 0) return NULL;
    struct dsema *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->magic = DSEMA_MAGIC;
    atomic_store(&s->refs, 1);
    atomic_store(&s->value, value);
    return s;
}

static void darwin_dispatch_release(void *obj)
{
    struct dsema *s = dsema_of(obj, "dispatch_release");
    if (atomic_fetch_sub(&s->refs, 1) != 1) return;
    s->magic = 0;
    free(s);
}

static long darwin_dispatch_semaphore_signal(void *obj)
{
    struct dsema *s = dsema_of(obj, "dispatch_semaphore_signal");
    if (atomic_fetch_add(&s->value, 1) + 1 > 0) return 0;
    int saved = errno;
    tokens_post(&s->wakes);
    errno = saved;
    return 1;
}

// A dispatch_time_t: 0 now, ~0 never; a value with the top bit clear is a time of the uptime
// clock in nanoseconds, one with it set the negated wall-clock time.
static uint64_t darwin_dispatch_time(uint64_t when, int64_t delta)
{
    if (when == DARWIN_DISPATCH_TIME_FOREVER) return when;
    int wall = when == WALL_NOW || (int64_t)when < 0;
    int64_t base = when == WALL_NOW ? now_ns(CLOCK_REALTIME) : wall ? -(int64_t)when
                 : when == DARWIN_DISPATCH_TIME_NOW ? now_ns(CLOCK_MONOTONIC) : (int64_t)when;
    int64_t t;
    if (__builtin_add_overflow(base, delta, &t)) return DARWIN_DISPATCH_TIME_FOREVER;
    if (t <= 0) t = wall ? 2 : 1;           // a time long past, not one of the two special values
    return wall ? (uint64_t)-t : (uint64_t)t;
}

// The monotonic time at which a wait until `when` ends; -1: never.
static int64_t dispatch_deadline(uint64_t when)
{
    if (when == DARWIN_DISPATCH_TIME_FOREVER) return -1;
    if (when == WALL_NOW || when == DARWIN_DISPATCH_TIME_NOW) return 0;
    if ((int64_t)when < 0) {
        int64_t left = -(int64_t)when - now_ns(CLOCK_REALTIME);
        return now_ns(CLOCK_MONOTONIC) + (left > 0 ? left : 0);
    }
    return (int64_t)when;
}

// 0, or non-zero when the time ran out
static long darwin_dispatch_semaphore_wait(void *obj, uint64_t when)
{
    struct dsema *s = dsema_of(obj, "dispatch_semaphore_wait");
    if (atomic_fetch_sub(&s->value, 1) - 1 >= 0) return 0;
    int saved = errno;
    long r = 0;
    if (tokens_take(&s->wakes, dispatch_deadline(when)) != 0) {
        // takes its wait back, unless a signal has counted it since: then that signal's wake is its own
        long v = atomic_load(&s->value);
        for (;;) {
            if (v >= 0) { tokens_take(&s->wakes, -1); break; }
            if (atomic_compare_exchange_weak(&s->value, &v, v + 1)) { r = DSEMA_TIMEOUT; break; }
        }
    }
    errno = saved;
    return r;
}

// ---- __ulock_wait, __ulock_wake: what libc++'s atomic waits stand on --------------------------
// A wait sleeps while the word at addr still holds the value; a wake names the address. The
// compares are 32 bits wide for operations 1 to 3 and 64 for 4 to 6, which a futex cannot do,
// so the waiters are kept here: records on their own stacks, in buckets by address.

#define UL_OPCODE(op)       ((op) & 0xff)
#define ULF_WAKE_ALL        0x00000100u
#define ULF_WAKE_THREAD     0x00000200u
#define ULF_NO_ERRNO        0x01000000u
#define UL_BUCKETS          64

struct uwaiter {
    void             *addr;
    uint32_t          port;
    _Atomic uint32_t  woken;
    struct uwaiter   *next, **link;         // link: NULL once it is off its bucket's list
};

static struct ubucket { pthread_mutex_t lock; struct uwaiter *first; } ubuckets[UL_BUCKETS] = {
    [0 ... UL_BUCKETS - 1] = { PTHREAD_MUTEX_INITIALIZER, NULL }
};

static struct ubucket *ubucket_of(void *addr)
{
    uintptr_t a = (uintptr_t)addr;
    return &ubuckets[((a >> 3) ^ (a >> 12)) % UL_BUCKETS];
}

static void uwaiter_off(struct uwaiter *w)
{
    if (!w->link) return;
    *w->link = w->next;
    if (w->next) w->next->link = w->link;
    w->link = NULL;
}

static long ulock_fail(uint32_t op, int darwin)
{
    if (op & ULF_NO_ERRNO) return -darwin;
    darwin_set_errno(darwin);
    return -1;
}

// int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us)
// The waiters still queued on the address, or -1 with errno (ULF_NO_ERRNO: the negated number).
static long darwin___ulock_wait(uint32_t op, void *addr, uint64_t value, uint32_t timeout_us)
{
    unsigned code = UL_OPCODE(op);
    if (code < 1 || code > 6) return ulock_fail(op, DARWIN_EINVAL);
    if (!addr) return ulock_fail(op, DARWIN_EFAULT);
    int saved = errno;
    int wide = code >= 4;
    struct ubucket *b = ubucket_of(addr);
    struct uwaiter w = { .addr = addr, .port = darwin_thread_port() };
    int64_t deadline = timeout_us ? now_ns(CLOCK_MONOTONIC) + (int64_t)timeout_us * 1000 : -1;

    pthread_mutex_lock(&b->lock);
    uint64_t now = wide ? atomic_load((_Atomic uint64_t *)addr) : atomic_load((_Atomic uint32_t *)addr);
    if (now != (wide ? value : (uint32_t)value)) {
        pthread_mutex_unlock(&b->lock);
        errno = saved;
        return 0;
    }
    w.next = b->first;
    w.link = &b->first;
    if (b->first) b->first->link = &w.next;
    b->first = &w;
    pthread_mutex_unlock(&b->lock);

    int timed_out = 0;
    while (!atomic_load(&w.woken))
        if (futex_wait_until(&w.woken, 0, deadline) != 0) { timed_out = 1; break; }

    pthread_mutex_lock(&b->lock);           // a waker writes the record under this lock
    long left = 0;
    if (atomic_load(&w.woken)) timed_out = 0;
    uwaiter_off(&w);
    for (struct uwaiter *o = b->first; o; o = o->next) left += o->addr == addr;
    pthread_mutex_unlock(&b->lock);
    errno = saved;
    return timed_out ? ulock_fail(op, DARWIN_ETIMEDOUT) : left;
}

// int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value): one waiter, all of
// them with ULF_WAKE_ALL, the thread whose port wake_value is with ULF_WAKE_THREAD
static long darwin___ulock_wake(uint32_t op, void *addr, uint64_t wake_value)
{
    unsigned code = UL_OPCODE(op);
    if (code < 1 || code > 6) return ulock_fail(op, DARWIN_EINVAL);
    if ((op & ULF_WAKE_ALL) && (op & ULF_WAKE_THREAD)) return ulock_fail(op, DARWIN_EINVAL);
    int saved = errno;
    struct ubucket *b = ubucket_of(addr);
    int woke = 0;
    pthread_mutex_lock(&b->lock);
    for (struct uwaiter *w = b->first, *next; w; w = next) {
        next = w->next;
        if (w->addr != addr) continue;
        if ((op & ULF_WAKE_THREAD) && w->port != (uint32_t)wake_value) continue;
        uwaiter_off(w);
        atomic_store(&w->woken, 1);
        futex(&w->woken, FUTEX_WAKE_PRIVATE, 1, NULL);
        woke++;
        if (!(op & ULF_WAKE_ALL)) break;
    }
    pthread_mutex_unlock(&b->lock);
    errno = saved;
    return woke ? 0 : ulock_fail(op, DARWIN_ENOENT);
}

const struct darwin_sym darwin_syms_thread[] = {
    SHIM(pthread_create), SHIM(pthread_join), SHIM(pthread_detach), SHIM(pthread_key_create),
    SHIM(pthread_setcancelstate), SHIM(pthread_setname_np), SHIM(pthread_get_stackaddr_np),
    SHIM(pthread_mach_thread_np),
    SHIM(pthread_mutex_init), SHIM(pthread_mutex_destroy), SHIM(pthread_mutex_lock), SHIM(pthread_mutex_trylock),
    SHIM(pthread_mutex_unlock), SHIM(pthread_mutexattr_settype),
    SHIM(pthread_cond_init), SHIM(pthread_cond_destroy), SHIM(pthread_cond_wait), SHIM(pthread_cond_timedwait),
    SHIM(pthread_cond_signal), SHIM(pthread_cond_broadcast),
    SHIM(mach_thread_self), SHIM(mach_port_deallocate), SHIM(thread_suspend), SHIM(thread_resume),
    SHIM(thread_get_state),
    SHIM(dispatch_semaphore_create), SHIM(dispatch_semaphore_signal), SHIM(dispatch_semaphore_wait),
    SHIM(dispatch_time), SHIM(dispatch_release),
    SHIM(__ulock_wait), SHIM(__ulock_wake),
    SAME(pthread_self), SAME(sched_yield), SAME(pthread_getspecific), SAME(pthread_setspecific),
    SAME(pthread_key_delete), SAME(pthread_attr_init), SAME(pthread_attr_destroy),
    SAME(pthread_mutexattr_init), SAME(pthread_mutexattr_destroy), SAME(pthread_atfork),
    NAMED("_mach_task_self_", &task_self),
    { NULL, NULL }
};
