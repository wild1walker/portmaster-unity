// Bionic-layout semaphores from bionic_compat_table(): zeroed word = value 0, post/wait across
// threads, trywait, timedwait timeout. Build: see build.sh; run under QEMU (docker --platform linux/arm/v7).
#include "../so_loader.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int (*s_init)(void *, int, unsigned), (*s_post)(void *), (*s_wait)(void *), (*s_try)(void *),
           (*s_timed)(void *, const struct timespec *), (*s_get)(void *, int *);
static void *find(const char *n)
{
    for (const so_shim *s = bionic_compat_table(); s->name; s++) if (!strcmp(s->name, n)) return s->fn;
    return NULL;
}
static uint32_t sem;          /* never sem_init'ed: zeroed, like Mono's */
static int got;
static void *consumer(void *a) { (void)a; for (int i = 0; i < 100000; i++) { s_wait(&sem); __atomic_add_fetch(&got, 1, __ATOMIC_SEQ_CST); } return NULL; }
int main(void)
{
    s_init = find("sem_init"); s_post = find("sem_post"); s_wait = find("sem_wait");
    s_try = find("sem_trywait"); s_timed = find("sem_timedwait"); s_get = find("sem_getvalue");
    int v = -5, fail = 0;
    s_get(&sem, &v); if (v != 0) { printf("FAIL zero value %d\n", v); fail++; }
    if (s_try(&sem) != -1 || errno != EAGAIN) { printf("FAIL trywait on 0\n"); fail++; }
    pthread_t t[2];
    pthread_create(&t[0], NULL, consumer, NULL); pthread_create(&t[1], NULL, consumer, NULL);
    for (int i = 0; i < 200000; i++) s_post(&sem);
    pthread_join(t[0], NULL); pthread_join(t[1], NULL);
    if (got != 200000) { printf("FAIL got %d\n", got); fail++; }
    s_get(&sem, &v); if (v != 0) { printf("FAIL value after %d\n", v); fail++; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    if (s_timed(&sem, &ts) != -1 || errno != ETIMEDOUT) { printf("FAIL timedwait timeout\n"); fail++; }
    s_init(&sem, 0, 3); s_get(&sem, &v); if (v != 3 || s_timed(&sem, &ts) != 0) { printf("FAIL init 3\n"); fail++; }
    printf(fail ? "sem: %d FAILED\n" : "sem: ok\n", fail);
    return fail != 0;
}
