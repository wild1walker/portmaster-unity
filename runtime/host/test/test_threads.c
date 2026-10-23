// threads.c's unit test: threads named as a game's, rules by name and prefix, the first rule that
// matches wins, a thread renamed after it started gets the rule of its new name.
//   (UA_AFFINITY='Main:0 Job*:0 Late:0-1' UA_NICE='Main:3 *:5': each variable's own first match)
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../threads.h"

struct t { const char *name, *later; int tid; volatile int go; };

static void *body(void *arg)
{
    struct t *t = arg;
    pthread_setname_np(pthread_self(), t->name);
    t->tid = (int)syscall(SYS_gettid);
    while (!t->go) { struct timespec d = { 0, 10000000 }; nanosleep(&d, NULL); }
    if (t->later) pthread_setname_np(pthread_self(), t->later);
    while (t->go < 2) { struct timespec d = { 0, 10000000 }; nanosleep(&d, NULL); }
    return NULL;
}

static int ncores(int tid)
{
    cpu_set_t s;
    if (sched_getaffinity(tid, sizeof s, &s)) return -1;
    return CPU_COUNT(&s);
}

int main(void)
{
    setenv("UA_AFFINITY", "Main:0 Job*:0 Late:0-1", 1);
    setenv("UA_NICE", "Main:3 *:5", 1);
    struct t ts[] = { { "Main", NULL, 0, 0 }, { "Job.Worker 0", NULL, 0, 0 }, { "Other", NULL, 0, 0 }, { "Early", "Late", 0, 0 } };
    enum { N = sizeof ts / sizeof *ts };
    pthread_t th[N];
    for (int i = 0; i < N; i++) pthread_create(&th[i], NULL, body, &ts[i]);
    for (int i = 0; i < N; i++) while (!ts[i].tid) usleep(1000);
    int all = ncores(ts[2].tid);
    threads_init();
    sleep(1);
    for (int i = 0; i < N; i++) ts[i].go = 1;          // "Early" becomes "Late"
    sleep(1);
    int ok = 1;
    int want_cores[N] = { 1, 1, all, all > 1 ? 2 : all }, want_nice[N] = { 3, 5, 5, 5 };
    for (int i = 0; i < N; i++) {
        int c = ncores(ts[i].tid);
        errno = 0;
        int n = getpriority(PRIO_PROCESS, (id_t)ts[i].tid);
        int good = c == want_cores[i] && n == want_nice[i];
        printf("%-13s cores %d (want %d), nice %d (want %d)%s\n", ts[i].later ? ts[i].later : ts[i].name, c, want_cores[i], n,
               want_nice[i], good ? "" : "  FAIL");
        ok &= good;
    }
    for (int i = 0; i < N; i++) ts[i].go = 2;
    for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
    printf("test_threads: %s\n", ok ? "ok" : "FAILED");
    return !ok;
}
