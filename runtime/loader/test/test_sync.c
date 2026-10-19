// bionic-layout pthread mutexes / condition variables from bionic_compat_table(): statically
// initialized objects first touched by several threads at once (the libhybris race), producer /
// consumer on a static condition, relative timed waits. Run under arm/v7 emulation.
#include "../so_loader.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int (*m_lock)(void *), (*m_unlock)(void *), (*m_destroy)(void *), (*c_wait)(void *, void *),
           (*c_signal)(void *), (*c_broadcast)(void *), (*c_rel)(void *, void *, const struct timespec *);
static void *find(const char *n)
{
    for (const so_shim *s = bionic_compat_table(); s->name; s++) if (!strcmp(s->name, n)) return s->fn;
    return NULL;
}
#define NT 4
static uint32_t mtx, cnd, cmtx;       /* bionic static initializers (0) */
static volatile int go, counter, items, consumed;
static pthread_barrier_t bar;
static void *adder(void *a)
{
    (void)a;
    pthread_barrier_wait(&bar);       /* all threads hit the fresh mutex together */
    for (int i = 0; i < 2000; i++) { m_lock(&mtx); counter++; m_unlock(&mtx); }
    return NULL;
}
static void *consumer(void *a)
{
    (void)a;
    for (;;)
    {
        m_lock(&cmtx);
        while (items == 0 && consumed < 20000) c_wait(&cnd, &cmtx);
        if (consumed >= 20000) { m_unlock(&cmtx); return NULL; }
        items--; consumed++;
        if (consumed >= 20000) c_broadcast(&cnd);
        m_unlock(&cmtx);
    }
}
int main(void)
{
    m_lock = find("pthread_mutex_lock"); m_unlock = find("pthread_mutex_unlock"); m_destroy = find("pthread_mutex_destroy");
    c_wait = find("pthread_cond_wait"); c_signal = find("pthread_cond_signal"); c_broadcast = find("pthread_cond_broadcast");
    c_rel = find("pthread_cond_timedwait_relative_np");
    int fail = 0;
    pthread_barrier_init(&bar, NULL, NT);
    for (int round = 0; round < 300; round++)
    {
        mtx = 0; counter = 0;
        pthread_t t[NT];
        for (int i = 0; i < NT; i++) pthread_create(&t[i], NULL, adder, NULL);
        for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
        if (counter != NT * 2000) { printf("FAIL round %d: counter %d\n", round, counter); fail++; break; }
        m_destroy(&mtx);
    }
    uint32_t rec = 0x4000;            /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER, never used */
    if (m_destroy(&rec) != 0) { printf("FAIL destroy static recursive\n"); fail++; }
    pthread_t c[3];
    for (int i = 0; i < 3; i++) pthread_create(&c[i], NULL, consumer, NULL);
    for (int i = 0; i < 20000; i++) { m_lock(&cmtx); items++; c_signal(&cnd); m_unlock(&cmtx); }
    for (int i = 0; i < 3; i++) pthread_join(c[i], NULL);
    if (consumed != 20000) { printf("FAIL consumed %d\n", consumed); fail++; }
    uint32_t c2 = 0, m2 = 0;
    struct timespec rel = { 0, 50000000 }, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    m_lock(&m2);
    int r = c_rel(&c2, &m2, &rel);
    m_unlock(&m2);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (r != ETIMEDOUT || ms < 45 || ms > 500) { printf("FAIL relative wait: %d after %.1f ms\n", r, ms); fail++; }
    printf(fail ? "sync: %d FAILED\n" : "sync: ok\n", fail);
    return fail != 0;
}
