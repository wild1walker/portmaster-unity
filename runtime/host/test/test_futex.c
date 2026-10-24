// futex.c's unit test: the syscall wrapper the Android libraries get, used the way Unity's Baselib
// uses futexes (a word, FUTEX_WAIT_PRIVATE while it holds a value, FUTEX_WAKE_PRIVATE after a store),
// with and without UA_FUTEX_SPIN: no wake-up may be lost (each scenario must finish; an alarm ends
// a hang), and a wake with nobody asleep in the kernel makes no system call.
//   test_futex <spin us> [stats s]
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../futex.h"

static long (*sc)(long, ...);

static void wait_while(atomic_int *w, int v) { sc(SYS_futex, w, FUTEX_WAIT_PRIVATE, v, NULL, 0, 0); }
static void wake(atomic_int *w, int n) { sc(SYS_futex, w, FUTEX_WAKE_PRIVATE, n, NULL, 0, 0); }

// 1. ping-pong: two threads hand a turn back and forth, each sleeping on its own word
#define ROUNDS 20000
static atomic_int turn[2];
static void *pong(void *arg)
{
    int me = (int)(intptr_t)arg, other = !me;
    for (int i = 0; i < ROUNDS; i++) {
        while (!atomic_load(&turn[me])) wait_while(&turn[me], 0);
        atomic_store(&turn[me], 0);
        atomic_store(&turn[other], 1);
        wake(&turn[other], 1);
    }
    return NULL;
}

// 2. a counting semaphore, as Baselib's: producers add and wake one, consumers take or sleep at 0
#define ITEMS 40000
#define NP 3
#define NC 3
static atomic_int sem, taken, produced;
static void *producer(void *arg)
{
    unsigned seed = (unsigned)(intptr_t)arg;
    for (int i = 0; i < ITEMS / NP; i++) {
        atomic_fetch_add(&sem, 1);
        atomic_fetch_add(&produced, 1);
        wake(&sem, 1);
        if (rand_r(&seed) % 64 == 0) { struct timespec d = { 0, (rand_r(&seed) % 200) * 1000 }; nanosleep(&d, NULL); }
    }
    return NULL;
}
static void *consumer(void *arg)
{
    (void)arg;
    for (;;) {
        int v = atomic_load(&sem);
        if (v > 0) {
            if (atomic_compare_exchange_weak(&sem, &v, v - 1)) {
                if (atomic_fetch_add(&taken, 1) + 1 >= (ITEMS / NP) * NP) {
                    // the last item: let the other consumers out
                    atomic_store(&sem, -1000000);
                    wake(&sem, 1 << 30);
                    return NULL;
                }
            }
            continue;
        }
        if (v < 0) return NULL;
        wait_while(&sem, 0);
    }
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void on_alarm(int s) { (void)s; static const char m[] = "FAIL: a scenario hung (lost wake-up)\n"; (void)!write(2, m, sizeof m - 1); _exit(1); }

int main(int argc, char **argv)
{
    char spin[16];
    snprintf(spin, sizeof spin, "%d", argc > 1 ? atoi(argv[1]) : 0);
    setenv("UA_FUTEX_SPIN", spin, 1);
    setenv("UA_FUTEX_STATS", argc > 2 ? argv[2] : "1", 1);
    const so_shim *t = futex_table();
    if (!t[0].name || strcmp(t[0].name, "syscall")) {
        // QEMU's 32-bit ARM user mode cannot read the generic timer: futex.c stays off there
        printf("SKIP: no wrapper (the ARM timer is not readable here; run this on the handheld)\n");
        return 0;
    }
    sc = (long (*)(long, ...))t[0].fn;
    signal(SIGALRM, on_alarm);
    alarm(120);

    // a wake with nobody waiting: with the spin it makes no system call (returns 0 either way)
    atomic_int lone = 0;
    long r = sc(SYS_futex, &lone, FUTEX_WAKE_PRIVATE, 1, NULL, 0, 0);
    if (r != 0) { printf("FAIL: lone wake returned %ld\n", r); return 1; }
    // a wait on a value the word does not hold returns at once (EAGAIN)
    r = sc(SYS_futex, &lone, FUTEX_WAIT_PRIVATE, 5, NULL, 0, 0);
    if (r != -1 || errno != EAGAIN) { printf("FAIL: wait on a changed word: %ld errno %d\n", r, errno); return 1; }
    // other system calls pass through
    if (sc(SYS_getpid) != getpid()) { printf("FAIL: getpid through the wrapper\n"); return 1; }

    double t0 = now();
    pthread_t a, b;
    atomic_store(&turn[0], 1);
    pthread_create(&a, NULL, pong, (void *)0);
    pthread_create(&b, NULL, pong, (void *)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    double t1 = now();
    printf("ping-pong: %d rounds in %.3f s (%.1f us a hand-over)\n", ROUNDS, t1 - t0, (t1 - t0) * 1e6 / ROUNDS / 2);

    pthread_t p[NP], c[NC];
    for (int i = 0; i < NC; i++) pthread_create(&c[i], NULL, consumer, NULL);
    for (int i = 0; i < NP; i++) pthread_create(&p[i], NULL, producer, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < NP; i++) pthread_join(p[i], NULL);
    for (int i = 0; i < NC; i++) pthread_join(c[i], NULL);
    double t2 = now();
    int want = (ITEMS / NP) * NP;
    if (atomic_load(&taken) != want || atomic_load(&produced) != want) {
        printf("FAIL: semaphore: %d produced, %d taken, %d wanted\n", atomic_load(&produced), atomic_load(&taken), want);
        return 1;
    }
    printf("semaphore: %d items through %d producers and %d consumers in %.3f s\n", want, NP, NC, t2 - t1);
    printf("PASS (UA_FUTEX_SPIN=%s)\n", spin);
    return 0;
}
