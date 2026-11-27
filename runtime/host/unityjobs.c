// unityjobs.c: Unity 2017.x's wait for a job group sleeps on the render thread instead of spinning
// (UA_JOB_SLEEP=1; off by default).
//
// JobQueue::WaitForJobGroup(group) in the 2017.4 players (read in 2017.4.27f1 and 2017.4.40f1 arm64-v8a, the
// same instructions in both): if the group has not been scheduled yet the waiter steals its jobs (Steal); then,
// while the group's tag says it is running, it pops a job from the queue's shared stack and executes it (Exec),
// whatever group that job belongs to, and returns as soon as a job of its own group reports the group done. When
// the stack is empty (every job taken by some thread), the main thread registers a semaphore with the group
// (SignalOnFinish: a node added to the group's list, refused when the group's tag has moved on) and sleeps on it
// until the group's end signals it; every other thread calls AtomicList::Relax (an empty function) and loops:
// it spins, each turn an exclusive load/store pair on the queue's stack and one on the group's list. Enter the
// Gungeon's render thread waits there for the dynamic batches' geometry jobs (DynamicVBO::DrawChunk ->
// GeometryJobTasks::PutDynamicVBOGeometryJobFence): 6 - 7 ms a frame of spinning in the Breach on the RG34XX-SP
// (muOS), on one of four cores while the worker that runs the job, UnityMain, the Wwise player and Mali's
// threads share the other three.
//
// The hook runs Unity's own loop again in C, call for call (the same functions at the same offsets: Load, Steal,
// Pop, Tag, Exec, SignalOnFinish, PlatformSemaphore), and changes one condition: the threads named in
// UA_JOB_SLEEP_THREADS (default the render thread, UnityGfxDeviceW) take the main thread's branch when the stack
// is empty, with a semaphore of their own (Unity's Semaphore: a word, then a bionic sem_t, which SignalSemaphore
// posts at +4; made once per thread with Unity's PlatformSemaphore::Create). Every other thread, the main thread
// among them, runs Unity's function unchanged (the trampoline). Nothing a job computes changes, nor which jobs run
// or in what order they are taken from the stack (the sleeper takes the same branch the main thread takes in the
// same state); only when the waiting thread resumes moves, by the semaphore's wake. A sleeper still pops and runs
// queued jobs before each sleep and after each wake, as the main thread does. Not for job workers: a worker
// asleep in a wait could leave jobs that only it would have run (it never waits here, so it is not listed).
//
//   UA_JOB_SLEEP=1          the listed threads sleep when there is nothing to help with
//   UA_JOB_SLEEP=yield      the listed threads keep Unity's loop (they still pop and run every job pushed while they
//                           wait) but give their core away each empty turn (sched_yield) instead of AtomicList::Relax
//   UA_JOB_SLEEP=spin       the C loop with Unity's spin kept (the control for an A/B: the hook's own cost)
//   UA_JOB_SLEEP_SPIN=<us>  first spin up to <us> microseconds as Unity does, then sleep (default 0)
//   UA_JOB_SLEEP_THREADS=a,b  thread name prefixes (default UnityGfxDeviceW)
//   UA_JOB_SLEEP_STATS=<s>  every <s> seconds: the listed threads' waits, sleeps, ms waiting a second
#define _GNU_SOURCE
#include "unityjobs.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sched.h>
#include <time.h>

#include "unityhooks.h"

enum { ISA_A64 = 'x' };
enum { F_WAIT, F_SIGNAL, F_EXEC, F_STEAL, F_POP, F_LOAD, F_TAG, F_SEMCREATE, F_SEMWAIT, F_SIGSEM, F_N };
// per player: every function the loop calls, in the order above (tools/re/unityhooks.py)
static const struct uh_row g_rows[][F_N] = {
    {
        { "2017.4.27f1 arm64-v8a", "JobQueue::WaitForJobGroup", 0x6f1608, 0x194, ISA_A64, 0xf7a9601cf29bb1ccull },
        { "2017.4.27f1 arm64-v8a", "JobQueue::SignalOnFinish", 0x6f14ec, 0x11c, ISA_A64, 0xe7536bf66787ec23ull },
        { "2017.4.27f1 arm64-v8a", "JobQueue::Exec", 0x6f0bb8, 0x2d4, ISA_A64, 0xd9b4159f53ed7c67ull },
        { "2017.4.27f1 arm64-v8a", "JobQueue::Steal", 0x6f0a78, 0x140, ISA_A64, 0x530ec8caaa0ddcacull },
        { "2017.4.27f1 arm64-v8a", "AtomicStack::Pop", 0x8f5764, 0x50, ISA_A64, 0x146652419e263790ull },
        { "2017.4.27f1 arm64-v8a", "AtomicList::Load", 0x8f5a14, 0x18, ISA_A64, 0x3255dc4b3ac38fddull },
        { "2017.4.27f1 arm64-v8a", "AtomicList::Tag", 0x8f59ec, 0x14, ISA_A64, 0x498e5d4010f4d214ull },
        { "2017.4.27f1 arm64-v8a", "PlatformSemaphore::Create", 0x2625a8, 0xcc, ISA_A64, 0xd397676ac7dbd202ull },
        { "2017.4.27f1 arm64-v8a", "PlatformSemaphore::WaitForSignal", 0x262014, 0xd8, ISA_A64, 0xceea0723a181e3b7ull },
        { "2017.4.27f1 arm64-v8a", "SignalSemaphore", 0x6f0e8c, 0x4, ISA_A64, 0xa51aa07666f15391ull },
    },
    {
        { "2017.4.40f1 arm64-v8a", "JobQueue::WaitForJobGroup", 0x6f12a4, 0x194, ISA_A64, 0x6503eb21d76fac66ull },
        { "2017.4.40f1 arm64-v8a", "JobQueue::SignalOnFinish", 0x6f1188, 0x11c, ISA_A64, 0xb8c01470ff36e20bull },
        { "2017.4.40f1 arm64-v8a", "JobQueue::Exec", 0x6f0854, 0x2d4, ISA_A64, 0x93d13ff6837aaf94ull },
        { "2017.4.40f1 arm64-v8a", "JobQueue::Steal", 0x6f0714, 0x140, ISA_A64, 0xf8ba8916e4a21ff0ull },
        { "2017.4.40f1 arm64-v8a", "AtomicStack::Pop", 0x8f55c8, 0x50, ISA_A64, 0x146652419e263790ull },
        { "2017.4.40f1 arm64-v8a", "AtomicList::Load", 0x8f5878, 0x18, ISA_A64, 0x3255dc4b3ac38fddull },
        { "2017.4.40f1 arm64-v8a", "AtomicList::Tag", 0x8f5850, 0x14, ISA_A64, 0x498e5d4010f4d214ull },
        { "2017.4.40f1 arm64-v8a", "PlatformSemaphore::Create", 0x262704, 0xcc, ISA_A64, 0x92c382031f31eb4cull },
        { "2017.4.40f1 arm64-v8a", "PlatformSemaphore::WaitForSignal", 0x262170, 0xd8, ISA_A64, 0xf565169ad131ffdeull },
        { "2017.4.40f1 arm64-v8a", "SignalSemaphore", 0x6f0b28, 0x4, ISA_A64, 0xf2188901a43b2542ull },
    },
};

// Unity's functions (A64 calling convention; a JobGroupID is the group and its 32-bit version in two registers)
static void (*u_wait)(void *q, void *group, uint64_t version);       // the trampoline: Unity's own
static int (*u_signal)(void *q, void *group, uint64_t version, void *sem);
static int (*u_exec)(void *q, void *job, int64_t tag, int one);
static int (*u_steal)(void *q, void *group, void *head, int64_t tag, int one, int one2);
static void *(*u_pop)(void *stack);
static void *(*u_load)(void *list, int64_t *tag);
static int64_t (*u_tag)(void *list);
static void (*u_semcreate)(void *platformsem);
static void (*u_semwait)(void *platformsem);

static int g_mode;              // 1 sleep, 2 the C loop spinning (control), 3 yield
static uint64_t g_spin_ns;
static char g_threads[256] = "UnityGfxDeviceW";
static int g_stats_s;

struct sem { uint32_t word; uint32_t sem_t_[4]; };   // Unity's Semaphore: SignalSemaphore posts the sem_t at +4
static __thread int t_kind;     // 0 not looked at, 1 listed, 2 not
static __thread struct sem t_sem;
static __thread int t_sem_made;

static _Atomic uint64_t s_waits, s_sleeps, s_ns, s_jobs;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int listed(void)
{
    if (!t_kind) {
        char name[17] = { 0 };
        prctl(PR_GET_NAME, name, 0, 0, 0);
        t_kind = 2;
        for (const char *p = g_threads; *p;) {
            size_t n = strcspn(p, ",");
            if (n && !strncmp(name, p, n)) { t_kind = 1; break; }
            p += n + (p[n] == ',');
        }
        if (t_kind == 1) fprintf(stderr, "[unityjobs] UA_JOB_SLEEP: thread '%s' %s in WaitForJobGroup\n", name,
                                 g_mode == 1 ? "sleeps" : g_mode == 3 ? "yields its core" : "spins (the C loop)");
    }
    return t_kind == 1;
}

static void stats(uint64_t t)
{
    static _Atomic uint64_t next;
    uint64_t n = atomic_load_explicit(&next, memory_order_relaxed);
    if (!n) { atomic_compare_exchange_strong(&next, &n, t + (uint64_t)g_stats_s * 1000000000ull); return; }
    if (t < n || !atomic_compare_exchange_strong(&next, &n, t + (uint64_t)g_stats_s * 1000000000ull)) return;
    uint64_t w = atomic_exchange(&s_waits, 0), s = atomic_exchange(&s_sleeps, 0), ns = atomic_exchange(&s_ns, 0),
             j = atomic_exchange(&s_jobs, 0);
    fprintf(stderr, "[unityjobs] %d s: %llu waits, %llu sleeps (or yields), %llu jobs run while waiting, %.1f ms a second waiting\n",
            g_stats_s, (unsigned long long)w, (unsigned long long)s, (unsigned long long)j, ns / 1e6 / g_stats_s);
}

// Unity's WaitForJobGroup, the listed threads' branch changed (see the top)
static void wait_hook(void *q, void *group, uint64_t version)
{
    if (!group || !listed()) { u_wait(q, group, version); return; }
    uint32_t v = (uint32_t)version;
    uint64_t t0 = g_stats_s ? now_ns() : 0, spin_until = 0;
    int sleeps = 0, jobs = 0;
    int64_t tag;
    void *head = u_load(group, &tag);
    if (tag == (int64_t)(uint32_t)(v - 2)) {                  // not scheduled yet: take its jobs
        if (u_steal(q, group, head, tag, 1, 1) > 0) goto out;
        u_load(group, &tag);
    }
    int64_t running = tag;
    if (running != (int64_t)(uint32_t)(v - 1)) goto out;      // done
    if (g_mode == 1 && g_spin_ns) spin_until = now_ns() + g_spin_ns;
    do {
        void *job = u_pop(*(void **)q);
        if (job) {
            void *jg = *(void **)((char *)job + 0x20);
            int r = u_exec(q, job, u_tag(jg) + 1, 1);
            jobs++;
            if (jg == group && r > 0) break;
        } else if (g_mode == 1 && (!spin_until || now_ns() >= spin_until)) {
            if (!t_sem_made) { u_semcreate(&t_sem.sem_t_); t_sem_made = 1; }
            if (u_signal(q, group, v, &t_sem)) { u_semwait(&t_sem.sem_t_); sleeps++; }
        } else if (g_mode == 3) {
            sched_yield();
            sleeps++;
        }                                                     // else Unity's AtomicList::Relax: nothing
        u_load(group, &tag);
    } while (tag == running);
out:
    if (g_stats_s) {
        uint64_t t = now_ns();
        atomic_fetch_add_explicit(&s_waits, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_sleeps, (uint64_t)sleeps, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_jobs, (uint64_t)jobs, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_ns, t - t0, memory_order_relaxed);
        stats(t);
    }
}

void unityjobs_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_JOB_SLEEP");
    if (!v || !*v || !strcmp(v, "0") || !strcmp(v, "off")) return;
    g_mode = !strcmp(v, "1") ? 1 : !strcmp(v, "spin") ? 2 : !strcmp(v, "yield") ? 3 : 0;
    if (!g_mode) { fprintf(stderr, "[unityjobs] UA_JOB_SLEEP=%s: 1, yield or spin; left as Unity has it\n", v); return; }
    const char *s = getenv("UA_JOB_SLEEP_SPIN"), *th = getenv("UA_JOB_SLEEP_THREADS"), *st = getenv("UA_JOB_SLEEP_STATS");
    if (s && *s) g_spin_ns = strtoull(s, NULL, 10) * 1000ull;
    if (th && *th) snprintf(g_threads, sizeof g_threads, "%s", th);
    if (st && *st) g_stats_s = atoi(st);
    if (g_stats_s < 0) g_stats_s = 0;
    const struct uh_row *r = NULL;
    for (size_t i = 0; i < sizeof g_rows / sizeof *g_rows && !r; i++) {
        size_t k = 0;
        while (k < F_N && unityhooks_matches(&g_rows[i][k], base, size)) k++;
        if (k == F_N) r = g_rows[i];
    }
    if (!r) {
        fprintf(stderr, "[unityjobs] UA_JOB_SLEEP: this libunity.so is not one whose job queue was read "
                        "(tools/re/unityhooks.py); left as Unity has it\n");
        return;
    }
    u_signal = (void *)(base + r[F_SIGNAL].off);
    u_exec = (void *)(base + r[F_EXEC].off);
    u_steal = (void *)(base + r[F_STEAL].off);
    u_pop = (void *)(base + r[F_POP].off);
    u_load = (void *)(base + r[F_LOAD].off);
    u_tag = (void *)(base + r[F_TAG].off);
    u_semcreate = (void *)(base + r[F_SEMCREATE].off);
    u_semwait = (void *)(base + r[F_SEMWAIT].off);
    if (unityhooks_hook(base + r[F_WAIT].off, ISA_A64, (void *)wait_hook, (void **)&u_wait) != 0) {
        fprintf(stderr, "[unityjobs] %s: hook failed; left as Unity has it\n", r[F_WAIT].func);
        return;
    }
    fprintf(stderr, "[unityjobs] %s hooked (libunity %s): %s for '%s'%s\n", r[F_WAIT].func, r[F_WAIT].unity,
            g_mode == 1 ? "sleep" : g_mode == 3 ? "yield" : "the C loop, spinning", g_threads, g_spin_ns ? ", after a spin" : "");
}
