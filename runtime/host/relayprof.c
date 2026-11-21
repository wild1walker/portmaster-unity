// relayprof: the hot-function list UA_RELAYOUT lays out (runtime/loader/relayout.c), made from
// UA_PROF recordings (prof.c) against the donor's own libunity.so; and what can move at all.
//
//   relayprof check <lib.so>                      every function with an FDE: how many can move, why not
//   relayprof atomics <lib.so>                    the outline-atomic helpers the loader sends to LL/SC
//   relayprof make <lib.so> <out.txt> <prof-N.txt>... [options]
//       --threads A,B*    threads whose samples count (default UnityMain,UnityGfxDeviceW*)
//       --cluster BYTES   largest call-chain cluster (default 4096: a page)
//       --max BYTES       stop adding functions at this many bytes (default all sampled)
//       --min-weight W    leave out functions below W (per mille of a recording's samples; default 0)
//       --as NAME         the library's name in the recordings (default: the file's name)
//       --split           split functions into hot pieces (the 64-byte lines samples fell in, gaps of up to
//                         --split-gap lines (1) filled) and cold ones laid out after every hot piece
//       --no-callers      only the functions samples fell in (default: also every libunity function
//                         on a sampled stack: code that runs around its calls each frame)
//   relayprof show <lib.so> <list.txt>            the list's functions with what rl_check says
//   relayprof plan <lib.so> <list.txt>            the layout the loader would make: sizes, islands, unwind data
//
// The list holds offsets, sizes and weights in the library named by its build id: nothing of the
// library itself. Layout: Pettis-Hansen / C3 call-chain clusters (each function, hottest first, joins
// the cluster of its hottest caller while the cluster stays under --cluster bytes), then the clusters
// by density (weight per byte). Weights: each recording's (thread's) samples scaled to 1,000,000, so
// time and event recordings count alike.
//
// Built static for arm64 (runtime/host/build.sh); on a Mac: docker run --rm -v $PWD:/w ua-build-arm64
// /w/relayprof ...
#define _GNU_SOURCE
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../loader/relayout.h"

static void die(const char *m, const char *a) { fprintf(stderr, "relayprof: %s%s%s\n", m, a ? ": " : "", a ? a : ""); exit(1); }

// --- the library as mapped: PT_LOAD segments at their vaddrs
static uint8_t *g_img; static uint64_t g_img_size;
static rl_image g_im;
static const char *g_libname;

static void load_lib(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)n);
    if (!file || fread(file, 1, (size_t)n, f) != (size_t)n) die("cannot read", path);
    fclose(f);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_AARCH64)
        die("not an AArch64 ELF64 library", path);
    Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > g_img_size) g_img_size = ph[i].p_vaddr + ph[i].p_memsz;
    g_img = calloc(1, g_img_size);
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) memcpy(g_img + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);
    char err[200];
    if (rl_image_init(&g_im, g_img, (uint64_t)(uintptr_t)g_img, g_img_size, ph, eh->e_phnum, err, sizeof err) < 0) die(err, path);
    const char *b = strrchr(path, '/');
    g_libname = b ? b + 1 : path;
    free(file);
}

// --- functions (by FDE start) with weights
typedef struct {
    uint64_t start, size;
    double self, stack;          // per-mille-scaled samples in it / on stacks through it
    int cluster;                 // index of its cluster's head
    int next;                    // the next function in its cluster (-1)
    int tail;                    // (heads) the cluster's last function
    uint64_t csize;              // (heads) the cluster's bytes
    double cw;                   // (heads) the cluster's weight
    const char *bad;             // why it cannot move (rl_check)
} fn_t;
static fn_t *g_fn; static int g_nfn, g_capfn;
static int *g_hash; static int g_hcap;   // open addressing on start -> index

static int fn_index(uint64_t start, uint64_t size)
{
    if (!g_hcap) { g_hcap = 1 << 16; g_hash = malloc(sizeof(int) * (size_t)g_hcap); memset(g_hash, -1, sizeof(int) * (size_t)g_hcap); }
    uint64_t h = (start * 0x9e3779b97f4a7c15ull) >> 40;
    for (;;) {
        int *s = &g_hash[h & (uint64_t)(g_hcap - 1)];
        if (*s < 0) {
            if (g_nfn == g_capfn) { g_capfn = g_capfn ? 2 * g_capfn : 4096; g_fn = realloc(g_fn, sizeof *g_fn * (size_t)g_capfn); }
            fn_t *f = &g_fn[g_nfn];
            memset(f, 0, sizeof *f);
            f->start = start; f->size = size; f->next = -1; f->cluster = g_nfn; f->tail = g_nfn;
            *s = g_nfn++;
            if (g_nfn * 2 > g_hcap) {          // grow
                int oc = g_hcap; int *oh = g_hash;
                g_hcap *= 2; g_hash = malloc(sizeof(int) * (size_t)g_hcap); memset(g_hash, -1, sizeof(int) * (size_t)g_hcap);
                for (int i = 0; i < oc; i++) if (oh[i] >= 0) {
                    uint64_t k = (g_fn[oh[i]].start * 0x9e3779b97f4a7c15ull) >> 40;
                    while (g_hash[k & (uint64_t)(g_hcap - 1)] >= 0) k++;
                    g_hash[k & (uint64_t)(g_hcap - 1)] = oh[i];
                }
                free(oh);
                return fn_index(start, size);
            }
            return *s;
        }
        if (g_fn[*s].start == start) return *s;
        h++;
    }
}
static int fn_of(uint64_t vaddr)
{
    uint64_t s, z;
    if (rl_fde_find(&g_im, vaddr, &s, &z, NULL) < 0) return -1;
    return fn_index(s, z);
}

// --- call edges caller -> callee with weights
typedef struct { int a, b; double w; } edge_t;
static edge_t *g_e; static int g_ne, g_cape;
static int *g_eh; static int g_ehcap;
static void edge_add(int a, int b, double w)
{
    if (!g_ehcap) { g_ehcap = 1 << 18; g_eh = malloc(sizeof(int) * (size_t)g_ehcap); memset(g_eh, -1, sizeof(int) * (size_t)g_ehcap); }
    uint64_t h = (((uint64_t)a << 32 | (uint32_t)b) * 0x9e3779b97f4a7c15ull) >> 40;
    for (;; h++) {
        int *s = &g_eh[h & (uint64_t)(g_ehcap - 1)];
        if (*s < 0) {
            if (g_ne * 2 >= g_ehcap) die("too many call edges", NULL);
            if (g_ne == g_cape) { g_cape = g_cape ? 2 * g_cape : 8192; g_e = realloc(g_e, sizeof *g_e * (size_t)g_cape); }
            g_e[g_ne] = (edge_t){ a, b, w };
            *s = g_ne++;
            return;
        }
        if (g_e[*s].a == a && g_e[*s].b == b) { g_e[*s].w += w; return; }
    }
}

// --- the 64-byte lines samples (pcs and return addresses) fell in: a function's hot pieces (--split)
static uint64_t *g_lines; static size_t g_nlines, g_caplines;
static void line_add(uint64_t vaddr)
{
    if (!g_caplines) { g_caplines = 1 << 20; g_lines = malloc(sizeof *g_lines * g_caplines); memset(g_lines, 0xff, sizeof *g_lines * g_caplines); }
    uint64_t l = vaddr >> 6, h = (l * 0x9e3779b97f4a7c15ull) >> 40;
    for (;; h++) {
        uint64_t *e = &g_lines[h & (g_caplines - 1)];
        if (*e == l) return;
        if (*e == ~0ull) { *e = l; if (++g_nlines * 2 > g_caplines) die("too many sampled lines", NULL); return; }
    }
}
static int line_has(uint64_t vaddr)
{
    if (!g_caplines) return 0;
    uint64_t l = vaddr >> 6, h = (l * 0x9e3779b97f4a7c15ull) >> 40;
    for (;; h++) {
        uint64_t e = g_lines[h & (g_caplines - 1)];
        if (e == l) return 1;
        if (e == ~0ull) return 0;
    }
}

// --- UA_PROF recordings
static char **g_threads; static int g_nthreads;
static int thread_wanted(const char *name)
{
    for (int i = 0; i < g_nthreads; i++) {
        size_t n = strlen(g_threads[i]);
        if (n && g_threads[i][n - 1] == '*' ? !strncmp(name, g_threads[i], n - 1) : !strcmp(name, g_threads[i])) return 1;
    }
    return 0;
}
static int g_callers = 1, g_split = 0, g_split_gap = 1;

// a function's hot ranges: its sampled lines (and the entry's), gaps of up to g_split_gap lines filled,
// at most RL_MAXHOT ranges (the narrowest gaps closed first); "" if it stays whole
static void hot_ranges(const fn_t *f, char *out, size_t n, uint64_t *hot_bytes)
{
    out[0] = 0;
    *hot_bytes = f->size;
    if (!g_split || rl_split_check(&g_im, f->start, f->size)) return;
    uint64_t l0 = f->start >> 6, l1 = (f->start + f->size - 1) >> 6;
    uint32_t r[2 * 256]; int nr = 0;
    uint64_t gap = 0; int open = 0;
    for (uint64_t l = l0; l <= l1; l++) {
        int h = l == l0 || line_has(l << 6);
        uint64_t a = l == l0 ? f->start : l << 6, b = (l + 1) << 6;
        if (b > f->start + f->size) b = f->start + f->size;
        if (h) {
            if (open && gap <= (uint64_t)g_split_gap) r[nr - 1] = (uint32_t)(b - f->start);
            else if (nr < 512) { r[nr++] = (uint32_t)(a - f->start); r[nr++] = (uint32_t)(b - f->start); }
            open = 1; gap = 0;
        } else gap++;
    }
    while (nr / 2 > RL_MAXHOT) {                         // close the narrowest gap
        int best = 1; uint32_t bw = ~0u;
        for (int i = 1; i < nr / 2; i++) if (r[2 * i] - r[2 * i - 1] < bw) { bw = r[2 * i] - r[2 * i - 1]; best = i; }
        r[2 * best - 1] = r[2 * best + 1];
        for (int i = 2 * best; i + 2 < nr; i++) r[i] = r[i + 2];
        nr -= 2;
    }
    uint64_t hb = 0;
    for (int i = 0; i < nr; i += 2) hb += r[i + 1] - r[i];
    if (f->size - hb < 128) return;                      // not worth a piece
    *hot_bytes = hb;
    size_t k = (size_t)snprintf(out, n, " h:");
    for (int i = 0; i < nr && k + 24 < n; i += 2) k += (size_t)snprintf(out + k, n - k, "%s%x-%x", i ? "," : "", r[i], r[i + 1]);
}

typedef struct { int idx; char name[64]; int want; double total; } pthread_t_;

static void read_prof(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open", path);
    size_t cap = 1 << 16;
    char *line = malloc(cap);
    uint64_t bias = 0; int have_bias = 0;
    pthread_t_ th[64]; int nth = 0;
    // two passes: totals per thread (for the scale), then the weights
    for (int pass = 0; pass < 2; pass++) {
        rewind(f);
        while (getline(&line, &cap, f) > 0) {
            if (!strncmp(line, "module ", 7)) {
                unsigned long long lo, hi, b; char name[256];
                if (sscanf(line + 7, "%llx %llx %llx %255s", &lo, &hi, &b, name) == 4 && !strcmp(name, g_libname)) { bias = b; have_bias = 1; }
            } else if (!strncmp(line, "thread ", 7) && pass == 0) {
                int i, tid; char name[64];
                if (sscanf(line + 7, "%d %d %63s", &i, &tid, name) == 3 && nth < 64) {
                    th[nth].idx = i; snprintf(th[nth].name, sizeof th[nth].name, "%s", name);
                    th[nth].want = thread_wanted(name); th[nth].total = 0; nth++;
                }
            } else if (!strncmp(line, "stack ", 6)) {
                int id, t; unsigned cnt; char st; unsigned sys; unsigned long long pc, lr; unsigned exact; int used = 0;
                if (sscanf(line + 6, "%d %u %d %c %x %llx %llx %u%n", &id, &cnt, &t, &st, &sys, &pc, &lr, &exact, &used) < 8) continue;
                if (st != 'R') continue;               // on a CPU only
                pthread_t_ *T = NULL;
                for (int i = 0; i < nth; i++) if (th[i].idx == t) T = &th[i];
                if (!T || !T->want) continue;
                if (pass == 0) { T->total += cnt; continue; }
                if (!have_bias || T->total <= 0) continue;
                double w = 1e6 * cnt / T->total;
                int f0 = pc >= bias ? fn_of(pc - bias) : -1;
                if (f0 >= 0) { g_fn[f0].self += w; line_add(pc - bias); }
                // callers: lr, then the stack's return addresses
                const char *p = line + 6 + used;
                int prev = f0, first = 1, seen[160], nseen = 0;
                uint64_t addrs[130]; int na = 0;
                addrs[na++] = lr;
                while (*p && na < 130) {
                    while (*p == ' ') p++;
                    if (*p == '~') p++;
                    char *e; unsigned long long a = strtoull(p, &e, 16);
                    if (e == p) break;
                    addrs[na++] = a; p = e;
                }
                for (int k = 0; k < na; k++) {
                    uint64_t a = addrs[k];
                    if (a < bias + 4) continue;
                    int fc = fn_of(a - bias - 4);      // the call instruction
                    if (fc >= 0) { line_add(a - bias - 4); line_add(a - bias); }
                    if (fc < 0 || fc == prev) continue;
                    int dup = 0;
                    for (int j = 0; j < nseen; j++) dup |= seen[j] == fc;
                    if (!dup && nseen < 160) { seen[nseen++] = fc; if (g_callers) g_fn[fc].stack += w; }
                    if (first && f0 >= 0) edge_add(fc, f0, w);
                    else if (prev >= 0 && !dup) edge_add(fc, prev, w * 0.5);
                    first = 0;
                    prev = fc;
                }
            }
        }
    }
    int any = 0;
    for (int i = 0; i < nth; i++) if (th[i].want && th[i].total > 0) {
        fprintf(stderr, "relayprof: %s: %s, %.0f samples on a CPU\n", path, th[i].name, th[i].total);
        any = 1;
    }
    if (!have_bias) fprintf(stderr, "relayprof: %s: no %s in it\n", path, g_libname);
    else if (!any) fprintf(stderr, "relayprof: %s: none of the threads asked for\n", path);
    free(line);
    fclose(f);
}

static double weight(const fn_t *f) { return f->self + 0.1 * f->stack; }

static int cmp_hot(const void *a, const void *b)
{
    double x = weight(&g_fn[*(const int *)a]), y = weight(&g_fn[*(const int *)b]);
    return x < y ? 1 : x > y ? -1 : (g_fn[*(const int *)a].start > g_fn[*(const int *)b].start) - (g_fn[*(const int *)a].start < g_fn[*(const int *)b].start);
}
static int cmp_density(const void *a, const void *b)
{
    const fn_t *x = &g_fn[*(const int *)a], *y = &g_fn[*(const int *)b];
    double dx = x->cw / (double)(x->csize ? x->csize : 1), dy = y->cw / (double)(y->csize ? y->csize : 1);
    return dx < dy ? 1 : dx > dy ? -1 : (x->start > y->start) - (x->start < y->start);
}

static int cmd_make(int argc, char **argv)
{
    const char *out = argv[1];
    uint64_t cluster = 4096, maxb = ~0ull;
    double minw = 0;
    char *threads = strdup("UnityMain,UnityGfxDeviceW*");
    const char *profs[256]; int np = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = strdup(argv[++i]);
        else if (!strcmp(argv[i], "--cluster") && i + 1 < argc) cluster = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--min-weight") && i + 1 < argc) minw = atof(argv[++i]) * 1000;
        else if (!strcmp(argv[i], "--no-callers")) g_callers = 0;
        else if (!strcmp(argv[i], "--split")) g_split = 1;
        else if (!strcmp(argv[i], "--split-gap") && i + 1 < argc) g_split_gap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--as") && i + 1 < argc) g_libname = argv[++i];
        else if (np < 256) profs[np++] = argv[i];
    }
    if (!np) die("no recordings given", NULL);
    char *threads_shown = strdup(threads);
    for (char *t = strtok(threads, ","); t; t = strtok(NULL, ",")) {
        g_threads = realloc(g_threads, sizeof *g_threads * (size_t)(g_nthreads + 1));
        g_threads[g_nthreads++] = t;
    }
    for (int i = 0; i < np; i++) read_prof(profs[i]);
    // the candidates: movable, weighted
    int *hot = malloc(sizeof(int) * (size_t)(g_nfn + 1)), nh = 0, bad = 0;
    uint64_t badb = 0;
    for (int i = 0; i < g_nfn; i++) {
        fn_t *f = &g_fn[i];
        if (weight(f) <= minw || weight(f) <= 0) continue;
        f->bad = rl_check(&g_im, f->start, f->size);
        if (f->bad) { bad++; badb += f->size; fprintf(stderr, "relayprof: %llx (%llu bytes, weight %.0f) stays: %s\n",
                                                     (unsigned long long)f->start, (unsigned long long)f->size, weight(f), f->bad); continue; }
        hot[nh++] = i;
    }
    qsort(hot, (size_t)nh, sizeof *hot, cmp_hot);
    // --max: the hottest first
    uint64_t sum = 0; int keep = 0;
    for (; keep < nh; keep++) { if (sum + g_fn[hot[keep]].size > maxb) break; sum += g_fn[hot[keep]].size; }
    nh = keep;
    char *in = calloc((size_t)g_nfn + 1, 1);
    for (int i = 0; i < nh; i++) { in[hot[i]] = 1; fn_t *f = &g_fn[hot[i]]; f->csize = f->size; f->cw = weight(f); }
    // C3: hottest first, each into its hottest caller's cluster
    for (int i = 0; i < nh; i++) {
        int b = hot[i], best = -1;
        double bw = 0;
        for (int e = 0; e < g_ne; e++) if (g_e[e].b == b && g_e[e].a != b && in[g_e[e].a] && g_e[e].w > bw) { bw = g_e[e].w; best = g_e[e].a; }
        if (best < 0) continue;
        int cb = g_fn[b].cluster, ca = g_fn[best].cluster;
        if (ca == cb || cb != b) continue;   // b must head its own cluster (it joins behind the caller's)
        if (g_fn[ca].csize + g_fn[cb].csize > cluster) continue;
        g_fn[g_fn[ca].tail].next = cb;
        g_fn[ca].tail = g_fn[cb].tail;
        g_fn[ca].csize += g_fn[cb].csize;
        g_fn[ca].cw += g_fn[cb].cw;
        for (int x = cb; x >= 0; x = g_fn[x].next) g_fn[x].cluster = ca;
    }
    int *heads = malloc(sizeof(int) * (size_t)(nh + 1)), nhd = 0;
    for (int i = 0; i < nh; i++) if (g_fn[hot[i]].cluster == hot[i]) heads[nhd++] = hot[i];
    qsort(heads, (size_t)nhd, sizeof *heads, cmp_density);
    FILE *o = fopen(out, "w");
    if (!o) die("cannot write", out);
    char bid[140];
    rl_buildid_hex(&g_im, bid, sizeof bid);
    fprintf(o, "# UA_RELAYOUT list (runtime/loader/relayout.c): functions of %s to lay out together, in this order\n", g_libname);
    fprintf(o, "# made by relayprof from");
    for (int i = 0; i < np; i++) { const char *b = strrchr(profs[i], '/'); fprintf(o, " %s", b ? b + 1 : profs[i]); }
    fprintf(o, "; threads %s; cluster %llu\n", threads_shown, (unsigned long long)cluster);
    fprintf(o, "ua_relayout 1\nlib %s\nbuildid %s\nfunctions %d bytes %llu\n", g_libname, bid, nh, (unsigned long long)sum);
    fprintf(o, "# vaddr size weight(self+stack/10, per million samples) [h:hot ranges, offsets in the function]\n");
    uint64_t hotb = 0; int nsplit = 0;
    for (int h = 0; h < nhd; h++)
        for (int x = heads[h]; x >= 0; x = g_fn[x].next) {
            char hr[1024]; uint64_t hb;
            hot_ranges(&g_fn[x], hr, sizeof hr, &hb);
            hotb += hb; nsplit += hr[0] != 0;
            fprintf(o, "%llx %llx %.0f%s\n", (unsigned long long)g_fn[x].start, (unsigned long long)g_fn[x].size, weight(&g_fn[x]), hr);
        }
    fclose(o);
    if (g_split) fprintf(stderr, "relayprof: --split: %d functions in hot and cold pieces; %llu bytes hot of %llu\n", nsplit, (unsigned long long)hotb, (unsigned long long)sum);
    fprintf(stderr, "relayprof: %s: %d functions, %llu bytes in %d clusters; %d sampled functions (%llu bytes) cannot move\n",
            out, nh, (unsigned long long)sum, nhd, bad, (unsigned long long)badb);
    return 0;
}

static int cmd_check(void)
{
    int n = 0, ok = 0; uint64_t bytes = 0, okb = 0;
    struct { const char *why; int n; uint64_t b; } r[32]; int nr = 0;
    for (uint32_t i = 0; i < g_im.nfde; i++) {
        int32_t loc; memcpy(&loc, g_im.mem + g_im.table + (uint64_t)i * 8, 4);
        uint64_t s = g_im.hdr + (uint64_t)(int64_t)loc, st, z;
        if (rl_fde_find(&g_im, s, &st, &z, NULL) < 0 || st != s) continue;
        n++; bytes += z;
        const char *w = rl_check(&g_im, s, z);
        if (!w) { ok++; okb += z; continue; }
        int k = 0;
        while (k < nr && strcmp(r[k].why, w)) k++;
        if (k == nr && nr < 32) { r[nr].why = w; r[nr].n = 0; r[nr].b = 0; nr++; }
        if (k < 32) { r[k].n++; r[k].b += z; }
    }
    char bid[140]; rl_buildid_hex(&g_im, bid, sizeof bid);
    printf("%s build id %s: text %llx-%llx, %u FDEs, %d functions (%llu bytes); %d can move (%llu bytes)\n", g_libname, bid,
           (unsigned long long)g_im.text_lo, (unsigned long long)g_im.text_hi, g_im.nfde, n, (unsigned long long)bytes, ok, (unsigned long long)okb);
    for (int k = 0; k < nr; k++) printf("  %6d (%8llu bytes) stay: %s\n", r[k].n, (unsigned long long)r[k].b, r[k].why);
    return 0;
}

static int cmd_atomics(void)
{
    uint64_t len = g_im.text_hi - g_im.text_lo, flag = 0;
    uint8_t *c = malloc(len);
    memcpy(c, g_im.mem + g_im.text_lo, len);
    int n = rl_atomics(c, g_im.text_lo, len, 1, &flag);
    printf("%d outline-atomic helpers testing the byte at %llx\n", n, (unsigned long long)flag);
    for (uint64_t o = 0; o < len; o += 4)
        if (memcmp(c + o, g_im.mem + g_im.text_lo + o, 4)) {
            uint64_t s, z; int has = rl_fde_find(&g_im, g_im.text_lo + o, &s, &z, NULL) == 0;
            printf("  %llx%s\n", (unsigned long long)(g_im.text_lo + o), has ? " (inside an FDE's function)" : "");
        }
    free(c);
    return 0;
}

// the layout the loader would make, emitted at an address just past the image: sizes, islands, unwind data
static void plan_why(uint64_t s, const char *r, void *ud) { (void)ud; fprintf(stderr, "relayprof: %llx stays: %s\n", (unsigned long long)s, r); }
static int cmd_plan(const char *list)
{
    FILE *f = fopen(list, "r");
    if (!f) die("cannot open", list);
    char line[2048];
    rl_want *st = NULL; int n = 0, cap = 0;
    while (fgets(line, sizeof line, f)) {
        rl_want w;
        if (rl_want_parse(line, &w) < 0) continue;
        if (n == cap) { cap = cap ? 2 * cap : 1024; st = realloc(st, sizeof *st * (size_t)cap); }
        st[n++] = w;
    }
    fclose(f);
    rl_plan p;
    if (rl_plan_make(&p, &g_im, st, n, plan_why, NULL) < 0) die("plan failed", NULL);
    uint8_t *dst = calloc(1, p.total + 16);
    uint64_t run = (g_im.run + g_im.size + 0xfffff) & ~0xfffffull;
    char err[200];
    if (rl_plan_emit(&p, dst, run, err, sizeof err) < 0) die(err, NULL);
    uint64_t isl = 0, used = 0, body = 0, hot = 0, hotend = 0, ft = 0;
    for (int i = 0; i < p.np; i++) {
        const rl_piece *q = &p.pc[i];
        isl += q->isl; body += q->size; ft += q->ft ? 4 : 0;
        if (q->hot) { hot += q->size; if (q->at + q->size > hotend) hotend = q->at + q->size; }
        for (uint64_t o = 0; o < q->isl; o += 4) { uint32_t w; memcpy(&w, dst + q->at + q->size + (q->ft ? 4 : 0) + o, 4); used += w != 0; }
    }
    printf("%d functions (%d left out; %d split, %d not splittable), %d pieces: code %llu bytes (bodies %llu, hot %llu in the first %llu, "
           "fall-through branches %llu, padding %llu, islands reserved %llu, used %llu words), unwind %llu + %llu bytes\n",
           p.n, p.skipped, p.split, p.unsplit, p.np, (unsigned long long)p.code_size, (unsigned long long)body, (unsigned long long)hot,
           (unsigned long long)hotend, (unsigned long long)ft, (unsigned long long)(p.code_size - body - isl - ft),
           (unsigned long long)isl, (unsigned long long)used, (unsigned long long)p.eh_size, (unsigned long long)p.hdr_size);
    free(dst); free(st); rl_plan_free(&p);
    return 0;
}

// how the list's functions are reached besides direct calls: their addresses in data (R_AARCH64_RELATIVE:
// vtables, function tables) and formed in code (ADRP + ADD), and dynamic symbols (exported)
static uint8_t *g_file; static long g_file_size;
static int cmd_refs(const char *path, const char *list)
{
    FILE *f = fopen(list, "r");
    if (!f) die("cannot open", list);
    char line[512];
    uint64_t *st = NULL; double *wt = NULL; int n = 0, cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long s, z; double w = 0;
        if (line[0] == '#' || sscanf(line, "%llx %llx %lf", &s, &z, &w) < 2) continue;
        if (n == cap) { cap = cap ? 2 * cap : 1024; st = realloc(st, sizeof *st * (size_t)cap); wt = realloc(wt, sizeof *wt * (size_t)cap); }
        st[n] = s; wt[n++] = w;
    }
    fclose(f);
    FILE *lf = fopen(path, "rb");
    fseek(lf, 0, SEEK_END); g_file_size = ftell(lf); fseek(lf, 0, SEEK_SET);
    g_file = malloc((size_t)g_file_size);
    if (fread(g_file, 1, (size_t)g_file_size, lf) != (size_t)g_file_size) die("read", path);
    fclose(lf);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)g_file;
    Elf64_Shdr *sh = (Elf64_Shdr *)(g_file + eh->e_shoff);
    int *data = calloc((size_t)n, sizeof(int)), *code = calloc((size_t)n, sizeof(int)), *sym = calloc((size_t)n, sizeof(int));
    // a sorted index of the list
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_RELA) {
            Elf64_Rela *r = (Elf64_Rela *)(g_file + sh[i].sh_offset);
            for (size_t k = 0; k < sh[i].sh_size / sizeof *r; k++)
                if (ELF64_R_TYPE(r[k].r_info) == 1027 /* RELATIVE */)
                    for (int j = 0; j < n; j++) if ((uint64_t)r[k].r_addend == st[j]) data[j]++;
        } else if (sh[i].sh_type == SHT_DYNSYM) {
            Elf64_Sym *y = (Elf64_Sym *)(g_file + sh[i].sh_offset);
            for (size_t k = 0; k < sh[i].sh_size / sizeof *y; k++)
                if (y[k].st_shndx) for (int j = 0; j < n; j++) if (y[k].st_value == st[j]) sym[j]++;
        }
    }
    // ADRP xN, page; ADD xN, xN, #lo forming a start
    for (uint64_t pc = g_im.text_lo; pc + 8 <= g_im.text_hi; pc += 4) {
        uint32_t a, b; memcpy(&a, g_im.mem + pc, 4); memcpy(&b, g_im.mem + pc + 4, 4);
        if ((a & 0x9f000000u) != 0x90000000u || (b & 0xffc00000u) != 0x91000000u || (b & 31) != (a & 31) || ((b >> 5) & 31) != (a & 31)) continue;
        int64_t imm = (int64_t)((((uint64_t)((a >> 5) & 0x7ffff) << 2) | ((a >> 29) & 3)) << 43) >> 43;
        uint64_t t = (pc & ~0xfffull) + (uint64_t)(imm * 4096) + ((b >> 10) & 0xfff);
        for (int j = 0; j < n; j++) if (t == st[j]) code[j]++;
    }
    double wall = 0, wd = 0, wc = 0, ws = 0; int nd = 0, nc = 0, ns = 0;
    for (int j = 0; j < n; j++) {
        wall += wt[j];
        if (data[j]) { nd++; wd += wt[j]; }
        if (code[j]) { nc++; wc += wt[j]; }
        if (sym[j]) { ns++; ws += wt[j]; }
    }
    printf("%d functions: %d in data (RELATIVE; %.1f %% of the weight), %d formed in code (ADRP+ADD; %.1f %%), %d exported (%.1f %%)\n",
           n, nd, 100 * wd / wall, nc, 100 * wc / wall, ns, 100 * ws / wall);
    return 0;
}

static int cmd_show(const char *list)
{
    FILE *f = fopen(list, "r");
    if (!f) die("cannot open", list);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long s, z; double w;
        if (sscanf(line, "%llx %llx %lf", &s, &z, &w) != 3) continue;
        const char *why = rl_check(&g_im, s, z), *sp = rl_split_check(&g_im, s, z);
        printf("%llx %llx %.0f %s; split: %s\n", s, z, w, why ? why : "ok", sp ? sp : "ok");
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: relayprof check|atomics <lib.so>\n       relayprof make <lib.so> <out.txt> <prof-N.txt>... [--threads A,B*] [--cluster N] [--max N] [--min-weight W] [--no-callers]\n       relayprof show <lib.so> <list.txt>\n");
        return 2;
    }
    load_lib(argv[2]);
    if (!strcmp(argv[1], "check")) return cmd_check();
    if (!strcmp(argv[1], "atomics")) return cmd_atomics();
    if (!strcmp(argv[1], "show") && argc > 3) return cmd_show(argv[3]);
    if (!strcmp(argv[1], "plan") && argc > 3) return cmd_plan(argv[3]);
    if (!strcmp(argv[1], "refs") && argc > 3) return cmd_refs(argv[2], argv[3]);
    if (!strcmp(argv[1], "make") && argc > 4) return cmd_make(argc - 2, argv + 2);
    die("unknown command", argv[1]);
    return 2;
}
