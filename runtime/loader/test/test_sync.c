// bionic-layout pthread mutexes / condition variables / rwlocks from bionic_compat_table():
// statically initialized objects first touched by several threads at once (the libhybris race),
// objects at 4 mod 8 (bionic aligns them to 4 only; arm64 keeps its pointer in the first 8-byte
// aligned word), a static recursive initializer there, producer / consumer on a static condition,
// relative timed waits. Run under arm/v7 emulation (armhf) or natively (arm64).
#include "../so_loader.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__LP64__)                 /* bionic's sizes, in 32-bit words */
#define MUTEX_WORDS 10
#define COND_WORDS 12
#define RWLOCK_WORDS 14
#else
#define MUTEX_WORDS 1
#define COND_WORDS 1
#define RWLOCK_WORDS 10
#endif
typedef struct { int32_t w[MUTEX_WORDS]; } b_mutex;
typedef struct { int32_t w[COND_WORDS]; } b_cond;
typedef struct { int32_t w[RWLOCK_WORDS]; } b_rwlock;

static int (*m_lock)(void *), (*m_trylock)(void *), (*m_unlock)(void *), (*m_destroy)(void *),
           (*m_init)(void *, const void *), (*c_wait)(void *, void *), (*c_signal)(void *),
           (*c_broadcast)(void *), (*c_rel)(void *, void *, const struct timespec *),
           (*rw_rd)(void *), (*rw_wr)(void *), (*rw_unlock)(void *), (*rw_destroy)(void *);
static void *find(const char *n)
{
    for (const so_shim *s = bionic_compat_table(); s->name; s++) if (!strcmp(s->name, n)) return s->fn;
    return NULL;
}
#define NT 4
/* bionic static initializers (0); mtx, rec and rw at 4 mod 8 */
static struct __attribute__((aligned(8))) {
    int32_t pad; b_mutex mtx; b_mutex cmtx; int32_t pad2; b_cond cnd; int32_t pad3; b_mutex rec;
    b_rwlock rw; int32_t pad4; b_mutex m2; b_cond c2;
} S;
static volatile int counter, items, consumed, rw_value, rw_bad;
static pthread_barrier_t bar;
static void *adder(void *a)
{
    (void)a;
    pthread_barrier_wait(&bar);       /* all threads hit the fresh mutex together */
    for (int i = 0; i < 2000; i++) { m_lock(&S.mtx); counter++; m_unlock(&S.mtx); }
    return NULL;
}
static void *rw_user(void *a)
{
    int id = (int)(intptr_t)a;
    pthread_barrier_wait(&bar);       /* and the fresh rwlock */
    for (int i = 0; i < 2000; i++)
    {
        if ((i & 7) == id)
        {
            rw_wr(&S.rw);
            int v = rw_value;
            rw_value = -1;            /* a reader seeing this shares the lock with a writer */
            for (volatile int k = 0; k < 20; k++) ;
            rw_value = v + 1;
            rw_unlock(&S.rw);
        }
        else
        {
            rw_rd(&S.rw);
            if (rw_value < 0) rw_bad++;
            rw_unlock(&S.rw);
        }
    }
    return NULL;
}
static void *consumer(void *a)
{
    (void)a;
    for (;;)
    {
        m_lock(&S.cmtx);
        while (items == 0 && consumed < 20000) c_wait(&S.cnd, &S.cmtx);
        if (consumed >= 20000) { m_unlock(&S.cmtx); return NULL; }
        items--; consumed++;
        if (consumed >= 20000) c_broadcast(&S.cnd);
        m_unlock(&S.cmtx);
    }
}
int main(void)
{
    m_lock = find("pthread_mutex_lock"); m_trylock = find("pthread_mutex_trylock"); m_unlock = find("pthread_mutex_unlock");
    m_destroy = find("pthread_mutex_destroy"); m_init = find("pthread_mutex_init");
    c_wait = find("pthread_cond_wait"); c_signal = find("pthread_cond_signal"); c_broadcast = find("pthread_cond_broadcast");
    c_rel = find("pthread_cond_timedwait_relative_np");
    rw_rd = find("pthread_rwlock_rdlock"); rw_wr = find("pthread_rwlock_wrlock"); rw_unlock = find("pthread_rwlock_unlock");
    rw_destroy = find("pthread_rwlock_destroy");
    int fail = 0;
#if defined(__LP64__)
    if (((uintptr_t)&S.mtx & 7) != 4 || ((uintptr_t)&S.rec & 7) != 4 || ((uintptr_t)&S.rw & 7) != 4)
    { printf("FAIL test layout\n"); fail++; }
#endif
    pthread_barrier_init(&bar, NULL, NT);
    for (int round = 0; round < 300; round++)
    {
        memset(&S.mtx, 0, sizeof S.mtx); counter = 0;
        pthread_t t[NT];
        for (int i = 0; i < NT; i++) pthread_create(&t[i], NULL, adder, NULL);
        for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
        if (counter != NT * 2000) { printf("FAIL round %d: counter %d\n", round, counter); fail++; break; }
        m_destroy(&S.mtx);
    }
    for (int round = 0; round < 50; round++)
    {
        memset(&S.rw, 0, sizeof S.rw); rw_value = 0;
        pthread_t t[NT];
        for (int i = 0; i < NT; i++) pthread_create(&t[i], NULL, rw_user, (void *)(intptr_t)i);
        for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
        if (rw_value != NT * 250 || rw_bad) { printf("FAIL rwlock round %d: %d writes, %d bad reads\n", round, rw_value, rw_bad); fail++; break; }
        rw_destroy(&S.rw);
    }
    if (rw_unlock(&S.rw) != 0) { printf("FAIL unlock of an unused rwlock\n"); fail++; }
    S.rec.w[0] = 0x4000;              /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER */
    if (m_lock(&S.rec) || m_trylock(&S.rec) || m_unlock(&S.rec) || m_unlock(&S.rec))
    { printf("FAIL static recursive mutex\n"); fail++; }
    if (m_destroy(&S.rec) != 0) { printf("FAIL destroy static recursive\n"); fail++; }
    b_mutex unused = { { 0x4000 } };   /* never used */
    if (m_destroy(&unused) != 0) { printf("FAIL destroy unused static recursive\n"); fail++; }
    pthread_mutexattr_t ea;            /* glibc writes bionic's attribute object (4 / 8 bytes) */
    pthread_mutexattr_init(&ea);
    pthread_mutexattr_settype(&ea, PTHREAD_MUTEX_ERRORCHECK);
    memset(&S.rec, 0, sizeof S.rec);
    if (m_init(&S.rec, &ea) || m_lock(&S.rec) || m_lock(&S.rec) != EDEADLK || m_unlock(&S.rec) || m_destroy(&S.rec))
    { printf("FAIL pthread_mutex_init (error checking, at 4 mod 8)\n"); fail++; }
    pthread_t c[3];
    for (int i = 0; i < 3; i++) pthread_create(&c[i], NULL, consumer, NULL);
    for (int i = 0; i < 20000; i++) { m_lock(&S.cmtx); items++; c_signal(&S.cnd); m_unlock(&S.cmtx); }
    for (int i = 0; i < 3; i++) pthread_join(c[i], NULL);
    if (consumed != 20000) { printf("FAIL consumed %d\n", consumed); fail++; }
    struct timespec rel = { 0, 50000000 }, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    m_lock(&S.m2);
    int r = c_rel(&S.c2, &S.m2, &rel);
    m_unlock(&S.m2);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (r != ETIMEDOUT || ms < 45 || ms > 500) { printf("FAIL relative wait: %d after %.1f ms\n", r, ms); fail++; }
    printf(fail ? "sync: %d FAILED\n" : "sync: ok\n", fail);
    return fail != 0;
}
