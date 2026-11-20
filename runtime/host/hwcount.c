// hwcount: the CPU's hardware counters (the kernel's PMU through perf_event_open) on chosen threads
// of a running game, by event group, for comparing one build's code with another's on the same
// scene (docs/arm64-routes.md: the armhf and aarch64 players at King's Pass).
//
//   hwcount [-p pid] [-t name[,name*]] [-s secs] [-q ms] [-k] [-g groups] [-e ev,ev,...]
//     -p pid     the game's process (default: the newest ua_host; a 32-bit process is counted the
//                same way by this aarch64 build: the counters are the core's)
//     -t names   its threads by name, a trailing * a prefix (default UnityMain)
//     -s secs    how long to count (default 20)
//     -q ms      the slice each group counts before the next takes over (default 100): one group
//                at a time, so no group is multiplexed with another and its counts are exact for
//                the slices it ran; every group has cycles and instructions, so the other events
//                are read against them
//     -k         count kernel mode too (default user mode only: the code the builds differ in)
//     -g groups  the preset groups to rotate (default all: front,data,l2,branch,mix,stall)
//     -e list    one more group of events by name or raw:0x<n> (at most 4 besides cycles and
//                instructions)
//   hwcount -L   the memory hierarchy seen from this core: a random pointer chase (one 64-byte
//                line a step) over 8 KB - 16 MB, ns a load; where it steps up are the L1D and L2
//                sizes (the kernel does not list the caches on these handhelds)
//
// The presets are the ARMv8 common events and the Cortex-A53's implementation-defined ones (its
// TRM, table 12-29: 0xC0 - 0xE8; the Cortex-A35 has most of the same codes): a group the kernel
// refuses is left out ("[hwcount] group ... refused"). Output: each group's wall time counted
// ("# wall"), then per group and thread its counts, the thread's time on a CPU while counted
// (per-thread counters run only while the thread does), and each event per 1000 instructions and
// per 1000 cycles; counts a frame = count / (the group's wall time x the run's frame rate).
// Nothing is changed: no governor, no setting; the counters are opened on the threads only
// (root, as the games run; perf_event_paranoid is not touched).
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAXEV 8
#define MAXGRP 12
#define MAXTHR 32

typedef struct { const char *name; unsigned code; } evdef;
static const evdef EVENTS[] = {
    { "l1i_cache_refill", 0x01 }, { "l1i_tlb_refill", 0x02 }, { "l1d_cache_refill", 0x03 },
    { "l1d_cache", 0x04 }, { "l1d_tlb_refill", 0x05 }, { "ld_retired", 0x06 }, { "st_retired", 0x07 },
    { "inst_retired", 0x08 }, { "exc_taken", 0x09 }, { "exc_return", 0x0A }, { "pc_write_retired", 0x0C },
    { "br_immed_retired", 0x0D }, { "br_return_retired", 0x0E }, { "unaligned_ldst_retired", 0x0F },
    { "br_mis_pred", 0x10 }, { "cpu_cycles", 0x11 }, { "br_pred", 0x12 }, { "mem_access", 0x13 },
    { "l1i_cache", 0x14 }, { "l1d_cache_wb", 0x15 }, { "l2d_cache", 0x16 }, { "l2d_cache_refill", 0x17 },
    { "l2d_cache_wb", 0x18 }, { "bus_access", 0x19 }, { "bus_cycles", 0x1D },
    // Cortex-A53 implementation defined
    { "ext_mem_req", 0xC0 }, { "ext_mem_req_nc", 0xC1 }, { "prefetch_linefill", 0xC2 },
    { "prefetch_linefill_drop", 0xC3 }, { "read_alloc_enter", 0xC4 }, { "read_alloc", 0xC5 },
    { "pre_decode_err", 0xC6 }, { "stall_sb_full", 0xC7 }, { "ext_snoop", 0xC8 }, { "br_cond", 0xC9 },
    { "br_indirect_mispred", 0xCA }, { "br_indirect_addr_mispred", 0xCB }, { "br_cond_mispred", 0xCC },
    { "other_iq_dep_stall", 0xE0 },   // IQ empty, not an i-cache or iTLB miss or pre-decode error
    { "ic_dep_stall", 0xE1 },         // IQ empty while an instruction cache miss is processed
    { "iutlb_dep_stall", 0xE2 },      // IQ empty while an instruction micro-TLB miss is processed
    { "decode_dep_stall", 0xE3 },     // IQ empty, a pre-decode error
    { "other_interlock_stall", 0xE4 },// an interlock other than SIMD/FP, a load's data or a full store buffer
    { "agu_dep_stall", 0xE5 },        // a load/store waits for an address register (pointer chasing)
    { "simd_dep_stall", 0xE6 },       // an Advanced SIMD/FP interlock
    { "ld_dep_stall", 0xE7 },         // Wr stage stalled by a load miss
    { "st_dep_stall", 0xE8 },         // Wr stage stalled by a store
};
static int ev_code(const char *n, unsigned *c)
{
    if (!strncmp(n, "raw:", 4)) { *c = (unsigned)strtoul(n + 4, NULL, 0); return 1; }
    for (unsigned i = 0; i < sizeof EVENTS / sizeof *EVENTS; i++)
        if (!strcmp(EVENTS[i].name, n)) { *c = EVENTS[i].code; return 1; }
    return 0;
}
static const char *ev_name(unsigned c)
{
    static char b[16];
    for (unsigned i = 0; i < sizeof EVENTS / sizeof *EVENTS; i++) if (EVENTS[i].code == c) return EVENTS[i].name;
    snprintf(b, sizeof b, "raw:0x%x", c);
    return b;
}

typedef struct { const char *name; const char *ev[MAXEV]; } preset;
static const preset PRESETS[] = {
    { "front",  { "l1i_cache", "l1i_cache_refill", "l1i_tlb_refill", "ic_dep_stall", 0 } },
    { "data",   { "l1d_cache", "l1d_cache_refill", "l1d_tlb_refill", "ld_dep_stall", 0 } },
    { "l2",     { "l2d_cache", "l2d_cache_refill", "bus_access", "prefetch_linefill", 0 } },
    { "branch", { "br_pred", "br_mis_pred", "br_indirect_mispred", "br_cond_mispred", 0 } },
    { "mix",    { "ld_retired", "st_retired", "mem_access", "pc_write_retired", 0 } },
    { "stall",  { "iutlb_dep_stall", "other_iq_dep_stall", "agu_dep_stall", "st_dep_stall", 0 } },
    { "stall2", { "other_interlock_stall", "simd_dep_stall", "decode_dep_stall", "stall_sb_full", 0 } },
};

static int g_type = PERF_TYPE_RAW;   // armv8_pmuv3's type from sysfs if it is there
static int g_kernel;

typedef struct {
    const char *gname;
    int nev; unsigned code[MAXEV];
    int fd[MAXTHR][MAXEV];
    uint64_t sum[MAXTHR][MAXEV], run_ns[MAXTHR], wall_ns;   // run_ns: the thread's time on a CPU while counted
    int ok;
} group;

typedef struct { int tid; char comm[20]; } thr;

static long peo(struct perf_event_attr *a, int tid, int leader) { return syscall(SYS_perf_event_open, a, tid, -1, leader, 0); }

static int open_group(group *g, thr *t, int nt)
{
    for (int i = 0; i < nt; i++) {
        for (int e = 0; e < g->nev; e++) {
            struct perf_event_attr a;
            memset(&a, 0, sizeof a);
            a.size = sizeof a; a.type = (unsigned)g_type; a.config = g->code[e];
            a.disabled = e == 0; a.exclude_kernel = !g_kernel; a.exclude_hv = 1;
            a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
            int fd = (int)peo(&a, t[i].tid, e == 0 ? -1 : g->fd[i][0]);
            if (fd < 0) {
                fprintf(stderr, "[hwcount] group %s refused on %d (%s, event %s): %s\n", g->gname, t[i].tid, t[i].comm,
                        ev_name(g->code[e]), strerror(errno));
                for (int k = 0; k < e; k++) close(g->fd[i][k]);
                for (int j = 0; j < i; j++) for (int k = 0; k < g->nev; k++) close(g->fd[j][k]);
                return 0;
            }
            g->fd[i][e] = fd;
        }
    }
    g->ok = 1;
    return 1;
}

static void read_group(group *g, int nt)
{
    for (int i = 0; i < nt; i++) {
        uint64_t buf[3 + MAXEV];
        ssize_t n = read(g->fd[i][0], buf, sizeof buf);
        if (n < (ssize_t)(3 * sizeof(uint64_t))) continue;
        // nr, time_enabled, time_running, values...
        for (int e = 0; e < g->nev && e < (int)buf[0]; e++) g->sum[i][e] = buf[3 + e];
        g->run_ns[i] = buf[2];
    }
}

static int newest_pid(const char *comm)
{
    DIR *d = opendir("/proc");
    struct dirent *de; int best = -1; unsigned long long bst = 0;
    while (d && (de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char p[64], c[64] = ""; snprintf(p, sizeof p, "/proc/%s/comm", de->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (fgets(c, sizeof c, f)) c[strcspn(c, "\n")] = 0;
        fclose(f);
        if (strcmp(c, comm)) continue;
        snprintf(p, sizeof p, "/proc/%s/stat", de->d_name);
        f = fopen(p, "r"); if (!f) continue;
        char line[1024]; unsigned long long st = 0;
        if (fgets(line, sizeof line, f)) {
            char *q = strrchr(line, ')'); int k = 0;
            for (char *s = q ? strtok(q + 2, " ") : NULL; s; s = strtok(NULL, " ")) if (++k == 20) { st = strtoull(s, NULL, 10); break; }
        }
        fclose(f);
        if (best < 0 || st > bst) { best = atoi(de->d_name); bst = st; }
    }
    if (d) closedir(d);
    return best;
}

static int match(const char *pats, const char *comm)
{
    char b[256]; snprintf(b, sizeof b, "%s", pats);
    for (char *s = strtok(b, ","); s; s = strtok(NULL, ",")) {
        size_t n = strlen(s);
        if (n && s[n - 1] == '*') { if (!strncmp(comm, s, n - 1)) return 1; }
        else if (!strcmp(comm, s)) return 1;
    }
    return 0;
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int memlat(void)
{
    for (size_t kb = 8; kb <= 16384; kb *= 2) {
        size_t n = kb * 1024 / 64;
        char *m = aligned_alloc(4096, n * 64);
        size_t *perm = malloc(n * sizeof *perm);
        if (!m || !perm) return 1;
        for (size_t i = 0; i < n; i++) perm[i] = i;
        unsigned long long r = 88172645463325252ULL;
        for (size_t i = n - 1; i > 0; i--) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; size_t j = r % (i + 1), t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
        for (size_t i = 0; i < n; i++) *(void **)(m + perm[i] * 64) = m + perm[(i + 1) % n] * 64;
        void **p = (void **)(m + perm[0] * 64);
        size_t steps = 4000000;
        for (size_t i = 0; i < n; i++) p = (void **)*p;          // warm
        double t0 = now_s();
        for (size_t i = 0; i < steps; i++) p = (void **)*p;
        double t1 = now_s();
        printf("%6zu KB %7.2f ns%s\n", kb, (t1 - t0) * 1e9 / steps, p ? "" : " ");
        free(m); free(perm);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "-L")) return memlat();
    int pid = -1, secs = 20, qms = 100, opt;
    const char *names = "UnityMain", *groups = "front,data,l2,branch,mix,stall,stall2", *extra = NULL;
    while ((opt = getopt(argc, argv, "p:t:s:q:kg:e:")) != -1) {
        switch (opt) {
        case 'p': pid = atoi(optarg); break;
        case 't': names = optarg; break;
        case 's': secs = atoi(optarg); break;
        case 'q': qms = atoi(optarg); break;
        case 'k': g_kernel = 1; break;
        case 'g': groups = optarg; break;
        case 'e': extra = optarg; break;
        default: fprintf(stderr, "usage: hwcount [-p pid] [-t names] [-s secs] [-q ms] [-k] [-g groups] [-e ev,...]\n"); return 2;
        }
    }
    FILE *tf = fopen("/sys/bus/event_source/devices/armv8_pmuv3/type", "r");
    if (tf) { if (fscanf(tf, "%d", &g_type) != 1) g_type = PERF_TYPE_RAW; fclose(tf); }
    if (pid < 0) pid = newest_pid("ua_host");
    if (pid < 0) { fprintf(stderr, "[hwcount] no ua_host running (-p)\n"); return 1; }

    thr t[MAXTHR]; int nt = 0;
    char p[128]; snprintf(p, sizeof p, "/proc/%d/task", pid);
    DIR *d = opendir(p); struct dirent *de;
    while (d && (de = readdir(d)) && nt < MAXTHR) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char cp[160], c[20] = ""; snprintf(cp, sizeof cp, "%s/%s/comm", p, de->d_name);
        FILE *f = fopen(cp, "r"); if (!f) continue;
        if (fgets(c, sizeof c, f)) c[strcspn(c, "\n")] = 0;
        fclose(f);
        if (!match(names, c)) continue;
        t[nt].tid = atoi(de->d_name); snprintf(t[nt].comm, sizeof t[nt].comm, "%s", c); nt++;
    }
    if (d) closedir(d);
    if (!nt) { fprintf(stderr, "[hwcount] pid %d: no thread named %s\n", pid, names); return 1; }

    static group g[MAXGRP]; int ng = 0;
    char gb[256]; snprintf(gb, sizeof gb, "%s", groups);
    for (char *s = strtok(gb, ","); s && ng < MAXGRP; s = strtok(NULL, ",")) {
        for (unsigned k = 0; k < sizeof PRESETS / sizeof *PRESETS; k++) {
            if (strcmp(PRESETS[k].name, s)) continue;
            group *G = &g[ng++]; G->gname = PRESETS[k].name;
            G->code[G->nev++] = 0x11; G->code[G->nev++] = 0x08;
            for (int e = 0; PRESETS[k].ev[e]; e++) ev_code(PRESETS[k].ev[e], &G->code[G->nev++]);
        }
    }
    if (extra && ng < MAXGRP) {
        group *G = &g[ng]; G->gname = "extra";
        G->code[G->nev++] = 0x11; G->code[G->nev++] = 0x08;
        char eb[256]; snprintf(eb, sizeof eb, "%s", extra);
        int bad = 0;
        for (char *s = strtok(eb, ","); s; s = strtok(NULL, ",")) {
            if (G->nev >= 6) { fprintf(stderr, "[hwcount] -e: at most 4 events\n"); return 2; }
            if (!ev_code(s, &G->code[G->nev++])) { fprintf(stderr, "[hwcount] no event %s\n", s); bad = 1; }
        }
        if (!bad) ng++;
    }
    int live = 0;
    for (int i = 0; i < ng; i++) live += open_group(&g[i], t, nt);
    if (!live) return 1;

    fprintf(stderr, "[hwcount] pid %d, %d thread(s), %d group(s), %d s, slices of %d ms, %s\n", pid, nt, live, secs, qms,
            g_kernel ? "user+kernel" : "user only");
    struct timespec q = { qms / 1000, (long)(qms % 1000) * 1000000L };
    struct timespec t0, now; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int k = 0;; k = (k + 1) % ng) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9 >= secs) break;
        if (!g[k].ok) continue;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < nt; i++) ioctl(g[k].fd[i][0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
        nanosleep(&q, NULL);
        for (int i = 0; i < nt; i++) ioctl(g[k].fd[i][0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        clock_gettime(CLOCK_MONOTONIC, &b);
        g[k].wall_ns += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec));
    }
    printf("# hwcount pid %d, %d s, slices %d ms, %s\n", pid, secs, qms, g_kernel ? "user+kernel" : "user");
    for (int k = 0; k < ng; k++) if (g[k].ok) printf("# wall %s %.3f\n", g[k].gname, g[k].wall_ns / 1e9);
    printf("# group thread tid run_s event count per_s per_kinst per_kcycle   (run_s: on a CPU; per_s: per second on a CPU;\n"
           "#   counts a frame = count / (wall of the group x fps))\n");
    for (int k = 0; k < ng; k++) {
        if (!g[k].ok) continue;
        read_group(&g[k], nt);
        for (int i = 0; i < nt; i++) {
            double rs = g[k].run_ns[i] / 1e9, cyc = (double)g[k].sum[i][0], ins = (double)g[k].sum[i][1];
            for (int e = 0; e < g[k].nev; e++) {
                double c = (double)g[k].sum[i][e];
                printf("%s %s %d %.3f %s %llu %.0f %.3f %.3f\n", g[k].gname, t[i].comm, t[i].tid, rs, ev_name(g[k].code[e]),
                       (unsigned long long)g[k].sum[i][e], rs > 0 ? c / rs : 0, ins > 0 ? 1000 * c / ins : 0,
                       cyc > 0 ? 1000 * c / cyc : 0);
            }
        }
    }
    return 0;
}
