// test_unityjobs.c: unityjobs.c's wait (UA_JOB_SLEEP) against a model of Unity 2017.4's job queue: a shared stack
// of jobs, groups whose list tag is version - 2 (made), version - 1 (running) and moves on when the group's last job
// ends (then the semaphores SignalOnFinish added are posted), worker threads that pop and run jobs. A thread named
// UnityGfxDeviceW waits for each group in turn, with the sleep (=1), the yield (=yield) and the spinning C loop (=spin); every wait
// must return only once its group is done, none may hang, and its own jobs may be run by the waiter itself.
#define _GNU_SOURCE
#include "../unityjobs.c"

#include <pthread.h>
#include <semaphore.h>
#include <assert.h>
#include <unistd.h>

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }

enum { GROUPS = 400, JOBS = 6, WORKERS = 2 };
struct job { struct job *next; char pad[0x20 - sizeof(void *)]; void *group; int spin; };   // the group at +0x20, as Unity's JobInfo
struct group { int64_t tag; void *head; int left; sem_t *waiters[8]; int nw; uint32_t version; int done; };
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct job *stack_top;
static struct job jobs[GROUPS][JOBS];
static struct group groups[GROUPS];
static struct { void *stack; } queue = { &stack_top };
static volatile int quit;

static void *m_pop(void *stack)
{
    (void)stack;
    pthread_mutex_lock(&mu);
    struct job *j = stack_top;
    if (j) stack_top = j->next;
    pthread_mutex_unlock(&mu);
    return j;
}
static void *m_load(void *list, int64_t *tag)
{
    struct group *g = list;
    pthread_mutex_lock(&mu);
    *tag = g->tag;
    void *h = g->head;
    pthread_mutex_unlock(&mu);
    return h;
}
static int64_t m_tag(void *list) { int64_t t; m_load(list, &t); return t; }
static int m_exec(void *q, void *job, int64_t tag, int one)
{
    (void)q; (void)tag; (void)one;
    struct job *j = job;
    struct group *g = j->group;
    for (volatile int i = 0; i < j->spin; i++) {}
    pthread_mutex_lock(&mu);
    int last = --g->left == 0;
    if (last) {
        g->tag = g->version;                 // the tag moves on: done
        g->done = 1;
        for (int i = 0; i < g->nw; i++) sem_post(g->waiters[i]);
        g->nw = 0;
    }
    pthread_mutex_unlock(&mu);
    return last;
}
static int m_steal(void *q, void *group, void *head, int64_t tag, int a, int b) { (void)q; (void)group; (void)head; (void)tag; (void)a; (void)b; return 0; }
static sem_t *sem_of(void *at);
static int m_signal(void *q, void *group, uint64_t version, void *sem)
{
    (void)q;
    struct group *g = group;
    pthread_mutex_lock(&mu);
    int ok = g->tag == (int64_t)(uint32_t)((uint32_t)version - 1);
    if (ok) g->waiters[g->nw++] = sem_of((char *)sem + 4);
    pthread_mutex_unlock(&mu);
    return ok;
}
// (the model's semaphores)
// Unity's PlatformSemaphore is a bionic sem_t (one 4-byte word at Semaphore + 4); the model keeps a glibc one for each
static struct { void *at; sem_t s; } sems[8];
static sem_t *sem_of(void *at)
{
    for (int i = 0; i < 8; i++) if (sems[i].at == at) return &sems[i].s;
    for (int i = 0; i < 8; i++) if (!sems[i].at) { sems[i].at = at; sem_init(&sems[i].s, 0, 0); return &sems[i].s; }
    abort();
}
static void m_semcreate(void *s) { pthread_mutex_lock(&mu); sem_of(s); pthread_mutex_unlock(&mu); }
static void m_semwait(void *s) { pthread_mutex_lock(&mu); sem_t *g = sem_of(s); pthread_mutex_unlock(&mu); while (sem_wait(g) != 0) {} }
static int orig_calls;
static void m_wait_orig(void *q, void *group, uint64_t v) { (void)q; (void)group; (void)v; orig_calls++; }

static void *worker(void *a)
{
    (void)a;
    prctl(PR_SET_NAME, "Worker Thread", 0, 0, 0);
    while (!quit) {
        struct job *j = m_pop(queue.stack);
        if (j) m_exec(&queue, j, 0, 1); else usleep(20);
    }
    return NULL;
}

static void schedule(int i)
{
    struct group *g = &groups[i];
    memset(g, 0, sizeof *g);
    g->version = 10 + 4 * (uint32_t)i;
    g->left = JOBS;
    pthread_mutex_lock(&mu);
    g->tag = g->version - 1;                 // running
    for (int k = 0; k < JOBS; k++) {
        struct job *j = &jobs[i][k];
        j->group = g;
        j->spin = (i * 7 + k * 13) % 5 ? 2000 : 200000;
        j->next = stack_top;
        stack_top = j;
    }
    pthread_mutex_unlock(&mu);
}

static void *render(void *a)
{
    (void)a;
    prctl(PR_SET_NAME, "UnityGfxDeviceW", 0, 0, 0);
    for (int i = 0; i < GROUPS; i++) {
        schedule(i);
        if (i % 3 == 0) usleep(50);
        wait_hook(&queue, &groups[i], groups[i].version | 0xdead00000000ull);
        pthread_mutex_lock(&mu);
        assert(groups[i].done);              // returned only once its group was done
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static void run(int mode)
{
    g_mode = mode;
    atomic_store(&s_sleeps, 0);
    stack_top = NULL;
    quit = 0;
    pthread_t w[WORKERS], r;
    for (int i = 0; i < WORKERS; i++) pthread_create(&w[i], NULL, worker, NULL);
    pthread_create(&r, NULL, render, NULL);
    pthread_join(r, NULL);
    quit = 1;
    for (int i = 0; i < WORKERS; i++) pthread_join(w[i], NULL);
    printf("mode %s: %d groups waited for, %llu sleeps or yields\n", mode == 1 ? "sleep" : mode == 3 ? "yield" : "spin", GROUPS,
           (unsigned long long)atomic_load(&s_sleeps));
}

int main(void)
{
    u_wait = m_wait_orig; u_signal = m_signal; u_exec = m_exec; u_steal = m_steal; u_pop = m_pop; u_load = m_load;
    u_tag = m_tag; u_semcreate = (void (*)(void *))m_semcreate; u_semwait = (void (*)(void *))m_semwait;
    g_stats_s = 1000;                        // counts kept (printed only after 1000 s)
    run(1);
    assert(atomic_load(&s_sleeps) > 0);       // it did sleep
    run(2);
    assert(atomic_load(&s_sleeps) == 0);      // the spinning loop never sleeps
    run(3);
    // a thread not listed goes to Unity's own function
    t_kind = 0;
    prctl(PR_SET_NAME, "UnityMain", 0, 0, 0);
    wait_hook(&queue, &groups[0], groups[0].version);
    assert(orig_calls == 1);
    puts("test_unityjobs: ok");
    return 0;
}
