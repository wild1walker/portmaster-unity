// threads.c: CPU affinity and nice values for the game's threads, by name (UA_AFFINITY, UA_NICE).
//
//   UA_AFFINITY='UnityMain:3 UnityGfxDeviceW:2 FMOD*:0-1 *:0-2'    cores, per thread name
//   UA_NICE='UnityMain:-4 UnityGfxDeviceW:-4'                       nice values (negative: root or CAP_SYS_NICE)
//
// Rules are separated by spaces: <name>:<value>, the name exact or a prefix ending in '*' (thread
// names are cut to 15 characters: Unity's render thread is UnityGfxDeviceW, its job threads
// "Job.Worker <n>"), '*' alone every other thread; of each variable's rules, the first that matches a
// thread wins. A
// thread "ua_threads" looks at /proc/self/task every half second for the first two minutes, then
// every 5 s, and applies the rules to threads it has not seen yet or whose name changed (threads
// carry their creator's name until they set their own). Each change is logged once
// ("[threads] ..."). Nothing runs unless one of the two variables is set: a tool for measuring
// thread placement, not a default.
#define _GNU_SOURCE
#include <dirent.h>
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

#include "threads.h"

#define MAXRULES 16
#define MAXSEEN 256

struct rule { char name[32]; int prefix; cpu_set_t cpus; int has_cpus; int nice; int has_nice; };
static struct rule rules[MAXRULES];
static int nrules;
static struct { int tid; char comm[16]; } seen[MAXSEEN];
static int nseen;

static int parse_cpus(const char *s, cpu_set_t *set)
{
    CPU_ZERO(set);
    int any = 0;
    while (*s) {
        char *e;
        long a = strtol(s, &e, 10), b = a;
        if (e == s || a < 0 || a >= CPU_SETSIZE) return 0;
        s = e;
        if (*s == '-') { b = strtol(s + 1, &e, 10); if (e == s + 1 || b < a || b >= CPU_SETSIZE) return 0; s = e; }
        for (long i = a; i <= b; i++) CPU_SET(i, set);
        any = 1;
        if (*s == ',') s++;
        else if (*s) return 0;
    }
    return any;
}

// the rule for a name as written ("Job*": the prefix "Job"), made if there is none yet
static struct rule *rule_for(const char *spec)
{
    char name[32];
    snprintf(name, sizeof name, "%s", spec);
    size_t n = strlen(name);
    int prefix = n && name[n - 1] == '*';
    if (prefix) name[n - 1] = 0;
    for (int i = 0; i < nrules; i++)
        if (rules[i].prefix == prefix && !strcmp(rules[i].name, name)) return &rules[i];
    if (nrules == MAXRULES) return NULL;
    struct rule *r = &rules[nrules++];
    memset(r, 0, sizeof *r);
    memcpy(r->name, name, sizeof r->name);
    r->prefix = prefix;
    return r;
}

static void parse(const char *var, int nice)
{
    const char *v = getenv(var);
    if (!v || !*v) return;
    char *copy = strdup(v), *save = NULL;
    for (char *t = strtok_r(copy, " \t;", &save); t; t = strtok_r(NULL, " \t;", &save)) {
        char *colon = strrchr(t, ':');
        if (!colon) { fprintf(stderr, "[threads] %s: '%s' is not <name>:<value>\n", var, t); continue; }
        *colon = 0;
        struct rule *r = rule_for(t);
        if (!r) continue;
        if (nice) {
            char *e; long n = strtol(colon + 1, &e, 10);
            if (*e || n < -20 || n > 19) { fprintf(stderr, "[threads] %s: bad nice value '%s'\n", var, colon + 1); continue; }
            r->nice = (int)n; r->has_nice = 1;
        } else if (parse_cpus(colon + 1, &r->cpus)) r->has_cpus = 1;
        else fprintf(stderr, "[threads] %s: bad core list '%s'\n", var, colon + 1);
    }
    free(copy);
}

// the first rule that matches the name and sets cores (nice 0) or a nice value (nice 1)
static const struct rule *match(const char *comm, int nice)
{
    for (int i = 0; i < nrules; i++) {
        const struct rule *r = &rules[i];
        if (!(nice ? r->has_nice : r->has_cpus)) continue;
        if (r->prefix ? !strncmp(comm, r->name, strlen(r->name)) : !strcmp(comm, r->name)) return r;
    }
    return NULL;
}

static void apply(int tid, const char *comm)
{
    const struct rule *a = match(comm, 0), *p = match(comm, 1);
    if (!a && !p) return;
    char what[96] = "";
    int o = 0;
    if (a) {
        int e = sched_setaffinity(tid, sizeof a->cpus, &a->cpus) ? errno : 0;
        o += snprintf(what + o, sizeof what - o, " cores");
        for (int c = 0; c < 16 && o < (int)sizeof what - 4; c++) if (CPU_ISSET(c, &a->cpus)) o += snprintf(what + o, sizeof what - o, " %d", c);
        if (e) o += snprintf(what + o, sizeof what - o, " (%s)", strerror(e));
    }
    if (p && o < (int)sizeof what) {
        int e = setpriority(PRIO_PROCESS, (id_t)tid, p->nice) ? errno : 0;
        o += snprintf(what + o, sizeof what - o, " nice %d%s%s", p->nice, e ? " " : "", e ? strerror(e) : "");
    }
    fprintf(stderr, "[threads] %d %s:%s\n", tid, comm, what);
}

static void scan(void)
{
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    int self = (int)syscall(SYS_gettid);
    struct dirent *e;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name);
        if (tid <= 0 || tid == self) continue;
        char path[64], comm[16] = "";
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0;
        fclose(f);
        int i;
        for (i = 0; i < nseen && seen[i].tid != tid; i++) ;
        if (i < nseen && !strcmp(seen[i].comm, comm)) continue;
        if (i == nseen) { if (nseen == MAXSEEN) continue; seen[nseen++].tid = tid; }
        snprintf(seen[i].comm, sizeof seen[i].comm, "%s", comm);
        apply(tid, comm);
    }
    closedir(d);
}

static void *watcher(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "ua_threads");
    for (int n = 0;; n++) {
        scan();
        struct timespec t = { n < 240 ? 0 : 5, n < 240 ? 500000000 : 0 };
        nanosleep(&t, NULL);
    }
    return NULL;
}

void threads_init(void)
{
    parse("UA_AFFINITY", 0);
    parse("UA_NICE", 1);
    if (!nrules) return;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 64 * 1024);
    if (pthread_create(&t, &a, watcher, NULL)) fprintf(stderr, "[threads] no watcher thread\n");
    pthread_attr_destroy(&a);
}
