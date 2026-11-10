// a64fix_test.c - every instruction a64fix rewrites, run as the CPU has it and as rewritten, over
// register choices and values: the registers, NZCV, sp, the vector registers and the memory
// around must come out the same. Needs a CPU with LSE, RCPC and SHA3 (Apple silicon: the arm64
// container on the Mac). Then threads count through the rewritten atomics.
#define _GNU_SOURCE
#include "../a64fix.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>

struct state { uint64_t x[18], nzcv, sp, v[32][2]; };
void run_state(void *code, struct state *s);

#define ISLAND (64 << 10)
#define RET 0xD65F03C0u
static uint8_t *g_island;
static uint32_t *g_native, *g_fixed;
static _Alignas(16) uint8_t g_stack[4096], g_cell[64];
#define SP (g_stack + 2048)
#define CELL (g_cell + 16)
static long g_runs, g_fail;

static uint64_t rnd(void)
{
    static uint64_t s = 0x9E3779B97F4A7C15ull;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

static void set_code(struct a64fix *f, uint32_t insn)
{
    g_native[0] = insn; g_native[1] = RET;
    g_fixed[0] = insn; g_fixed[1] = RET;
    f->island_used = 0;
    f->rewritten = f->refused = 0;
    if (a64fix_code(f, (uint8_t *)g_fixed, 4, NULL, 0) != 0 || f->rewritten != 1 || f->refused || g_fixed[0] == insn || g_fixed[1] != RET) {
        printf("FAIL: 0x%08x (%s) was not rewritten\n", insn, a64fix_kind_name(a64fix_kind(insn)));
        exit(1);
    }
    __builtin___clear_cache((char *)g_native, (char *)(g_native + 2));
    __builtin___clear_cache((char *)g_fixed, (char *)(g_fixed + 2));
}

static void state_init(struct state *s)
{
    for (int i = 0; i < 18; i++) s->x[i] = rnd();
    for (int i = 0; i < 32; i++) { s->v[i][0] = rnd(); s->v[i][1] = rnd(); }
    s->nzcv = 0;
    s->sp = (uint64_t)(uintptr_t)SP;
}

struct mem { uint8_t stack[256], cell[64]; };      // the stack: the red zone and what is above sp

static void one(uint32_t insn, const struct state *in, uint64_t memval, int at_sp)
{
    struct state out[2] = { *in, *in };
    struct mem m[2];
    for (int k = 0; k < 2; k++) {
        memset(SP - 256, 0x5A, 384);
        memset(g_cell, 0xC3, sizeof g_cell);
        memcpy(at_sp ? SP : CELL, &memval, 8);
        run_state(k ? g_fixed : g_native, &out[k]);
        memcpy(m[k].stack, SP - 128, 256);
        memcpy(m[k].cell, g_cell, 64);
    }
    g_runs++;
    if (memcmp(&out[0], &out[1], sizeof out[0]) == 0 && memcmp(&m[0], &m[1], sizeof m[0]) == 0) return;
    if (g_fail++ > 20) return;
    printf("FAIL: 0x%08x (%s) mem 0x%016llx nzcv 0x%llx%s\n", insn, a64fix_kind_name(a64fix_kind(insn)),
           (unsigned long long)memval, (unsigned long long)in->nzcv, at_sp ? " at sp" : "");
    for (int i = 0; i < 18; i++)
        if (out[0].x[i] != out[1].x[i]) printf("  x%d: in %016llx  cpu %016llx  rewritten %016llx\n", i, (unsigned long long)in->x[i], (unsigned long long)out[0].x[i], (unsigned long long)out[1].x[i]);
    if (out[0].nzcv != out[1].nzcv) printf("  nzcv: cpu %llx  rewritten %llx\n", (unsigned long long)out[0].nzcv, (unsigned long long)out[1].nzcv);
    if (out[0].sp != out[1].sp) printf("  sp: cpu %llx  rewritten %llx\n", (unsigned long long)out[0].sp, (unsigned long long)out[1].sp);
    if (memcmp(out[0].v, out[1].v, sizeof out[0].v) != 0) printf("  vector registers differ\n");
    if (memcmp(m[0].stack, m[1].stack, 256) != 0) printf("  the stack differs (red zone or above sp)\n");
    if (memcmp(m[0].cell, m[1].cell, 64) != 0) printf("  the memory cell differs\n");
}

static const uint64_t V[] = {
    0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000ull, 0xffffffffull,
    0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull, 0x0123456789abcdefull, 0xfedcba9876543210ull };
#define NV (sizeof V / sizeof V[0])
static const uint64_t NZ[] = { 0, 0xF0000000ull, 0x60000000ull, 0x90000000ull };
// Rs, Rt, Rn: apart, the zero register, sp, the same register twice, the island's own scratch registers
static const unsigned R[][3] = {
    { 1, 2, 3 }, { 31, 2, 3 }, { 1, 31, 3 }, { 31, 31, 3 }, { 1, 1, 3 }, { 1, 2, 31 }, { 31, 2, 31 }, { 1, 31, 31 },
    { 5, 5, 31 }, { 9, 10, 11 }, { 12, 13, 14 }, { 15, 8, 16 }, { 17, 9, 10 }, { 16, 17, 8 }, { 1, 3, 3 }, { 3, 2, 3 },
    { 3, 3, 3 }, { 9, 9, 9 }, { 0, 17, 31 }, { 10, 11, 31 }, { 13, 12, 9 }, { 14, 15, 17 } };
#define NR (sizeof R / sizeof R[0])

static void test_lse(struct a64fix *f)
{
    for (unsigned size = 0; size < 4; size++)
    for (unsigned ar = 0; ar < 4; ar++)
    for (unsigned op = 0; op < 9; op++)                         // 8: swp
    for (size_t c = 0; c < NR; c++) {
        unsigned rs = R[c][0], rt = R[c][1], rn = R[c][2];
        uint32_t insn = 0x38200000u | size << 30 | ar << 22 | rs << 16 | (op == 8 ? 0x8000u : op << 12) | rn << 5 | rt;
        set_code(f, insn);
        for (size_t i = 0; i < NV; i++)
        for (size_t j = 0; j < NV; j++) {
            struct state s;
            state_init(&s);
            s.nzcv = NZ[(i + j) & 3];
            if (rs != 31) s.x[rs] = V[j];
            if (rn != 31) s.x[rn] = (uint64_t)(uintptr_t)CELL;
            one(insn, &s, V[i], rn == 31);
        }
    }
}

static void test_cas(struct a64fix *f)
{
    for (unsigned size = 0; size < 4; size++)
    for (unsigned lr = 0; lr < 4; lr++)
    for (size_t c = 0; c < NR; c++) {
        unsigned rs = R[c][0], rt = R[c][1], rn = R[c][2];
        uint32_t insn = 0x08A07C00u | size << 30 | (lr & 1) << 22 | rs << 16 | (lr >> 1) << 15 | rn << 5 | rt;
        set_code(f, insn);
        for (size_t i = 0; i < NV; i++)
        for (size_t j = 0; j <= NV; j++) {                      // NV: equal in the bits compared, not above them
            struct state s;
            state_init(&s);
            s.nzcv = NZ[(i + j) & 3];
            if (rs != 31) s.x[rs] = j < NV ? V[j] : V[i] ^ (size < 3 ? ~0ull << (8u << size) : 0);
            if (rn != 31) s.x[rn] = (uint64_t)(uintptr_t)CELL;
            one(insn, &s, V[i], rn == 31);
        }
    }
}

static void test_ldapr(struct a64fix *f)
{
    static const unsigned r[][2] = { { 1, 2 }, { 31, 2 }, { 1, 31 }, { 2, 2 }, { 9, 10 }, { 17, 31 } };
    for (unsigned size = 0; size < 4; size++)
    for (size_t c = 0; c < sizeof r / sizeof r[0]; c++) {
        uint32_t insn = 0x38BFC000u | size << 30 | r[c][1] << 5 | r[c][0];
        set_code(f, insn);
        for (size_t i = 0; i < NV; i++) {
            struct state s;
            state_init(&s);
            s.nzcv = NZ[i & 3];
            if (r[c][1] != 31) s.x[r[c][1]] = (uint64_t)(uintptr_t)CELL;
            one(insn, &s, V[i], r[c][1] == 31);
        }
    }
}

static void test_eor3(struct a64fix *f)
{
    static const unsigned r[] = { 0, 1, 2, 31 };
    for (unsigned c = 0; c < 256; c++) {
        uint32_t insn = 0xCE000000u | r[c & 3] << 16 | r[(c >> 2) & 3] << 10 | r[(c >> 4) & 3] << 5 | r[c >> 6];
        set_code(f, insn);
        for (int i = 0; i < 16; i++) {
            struct state s;
            state_init(&s);
            s.nzcv = NZ[i & 3];
            one(insn, &s, 0, 0);
        }
    }
}

// ---- threads through the rewritten atomics ---------------------------------------------------
// sdot/udot, by vector and by element: every register pattern over the island's scratch
// registers, the bytes at their extremes and at random
static void test_dot(struct a64fix *f)
{
    static const unsigned r[] = { 0, 5, 16, 17, 18, 31 };
    static const uint64_t edge[] = { 0, ~0ull, 0x8080808080808080ull, 0x7f7f7f7f7f7f7f7full, 0x80ff7f0180ff7f01ull, 0x0100ff807f80ff00ull };
    for (unsigned form = 0; form < 8; form++) {
        unsigned q = form & 1, u = (form >> 1) & 1, elem = form >> 2;
        for (unsigned c = 0; c < 216; c++) {
            unsigned rd = r[c % 6], rn = r[c / 6 % 6], rm = r[c / 36];
            for (unsigned idx = 0; idx < (elem ? 4u : 1u); idx++) {
                uint32_t insn = elem ? 0x0F80E000u | q << 30 | u << 29 | (idx & 1) << 21 | rm << 16 | (idx >> 1) << 11 | rn << 5 | rd
                                     : 0x0E809400u | q << 30 | u << 29 | rm << 16 | rn << 5 | rd;
                set_code(f, insn);
                for (unsigned i = 0; i < 24; i++) {
                    struct state s;
                    state_init(&s);
                    s.nzcv = NZ[i & 3];
                    if (i < 12) {
                        s.v[rn][0] = edge[i % 6]; s.v[rn][1] = edge[(i + 1) % 6];
                        s.v[rm][0] = edge[i / 2 % 6]; s.v[rm][1] = edge[(i / 2 + 3) % 6];
                    }
                    if (i & 1) s.v[rd][0] = s.v[rd][1] = i & 2 ? 0x7fffffff80000000ull : 0xffffffffffffffffull;
                    one(insn, &s, 0, 0);
                }
            }
        }
    }
}

#define THREADS 4
#define COUNT 300000
static uint64_t g_add64, g_cas64;
static uint32_t g_add32;
static uint8_t g_cas8[8];
static uint64_t (*g_ldadd64)(uint64_t *, uint64_t), (*g_cas)(uint64_t *, uint64_t, uint64_t);
static uint64_t (*g_ldadd32)(uint32_t *, uint64_t), (*g_casb)(uint8_t *, uint64_t, uint64_t);

static void *count_thread(void *arg)
{
    for (int i = 0; i < COUNT; i++) {
        g_ldadd64(&g_add64, 3);
        g_ldadd32(&g_add32, 1);
        for (uint64_t old = g_cas64, got; (got = g_cas(&g_cas64, old, old + 1)) != old; old = got) ;
        for (uint64_t old = g_cas8[3], got; (got = g_casb(&g_cas8[3], old, (old + 1) & 0xff)) != old; old = got) ;
    }
    return arg;
}

static void test_threads(struct a64fix *f)
{
    // x0 pointer, x1 Rs, x2 Rt; the value the instruction leaves comes back in x0
    static const uint32_t code[4][3] = {
        { 0xF8E10002u, 0xAA0203E0u, RET },      // ldaddal x1, x2, [x0]; mov x0, x2
        { 0xB8E10002u, 0xAA0203E0u, RET },      // ldaddal w1, w2, [x0]
        { 0xC8E1FC02u, 0xAA0103E0u, RET },      // casal x1, x2, [x0]; mov x0, x1
        { 0x08E1FC02u, 0xAA0103E0u, RET } };    // casalb w1, w2, [x0]
    uint32_t *at = g_fixed + 16;
    f->island_used = 0;
    f->rewritten = 0;
    memcpy(at, code, sizeof code);
    if (a64fix_code(f, (uint8_t *)at, sizeof code, NULL, 0) != 0 || f->rewritten != 4) { printf("FAIL: the thread test's code was not rewritten\n"); exit(1); }
    __builtin___clear_cache((char *)at, (char *)(at + 12));
    g_ldadd64 = (uint64_t (*)(uint64_t *, uint64_t))(uintptr_t)at;
    g_ldadd32 = (uint64_t (*)(uint32_t *, uint64_t))(uintptr_t)(at + 3);
    g_cas = (uint64_t (*)(uint64_t *, uint64_t, uint64_t))(uintptr_t)(at + 6);
    g_casb = (uint64_t (*)(uint8_t *, uint64_t, uint64_t))(uintptr_t)(at + 9);
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, count_thread, NULL);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    uint64_t n = (uint64_t)THREADS * COUNT;
    static const uint8_t around[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t want8[8];
    memcpy(want8, around, 8);
    want8[3] = (uint8_t)n;
    if (g_add64 != 3 * n || g_add32 != (uint32_t)n || g_cas64 != n || memcmp(g_cas8, want8, 8) != 0) {
        printf("FAIL: threads counted %llu %u %llu %u, not %llu %u %llu %u\n", (unsigned long long)g_add64, g_add32,
               (unsigned long long)g_cas64, g_cas8[3], (unsigned long long)(3 * n), (uint32_t)n, (unsigned long long)n, want8[3]);
        g_fail++;
    }
}

int main(void)
{
    unsigned long h = getauxval(AT_HWCAP);
    if (!(h & 1ul << 8) || !(h & 1ul << 15) || !(h & 1ul << 17) || !(h & 1ul << 20)) { printf("SKIP: this CPU lacks LSE, RCPC, SHA3 or the dot product: nothing to compare with\n"); return 2; }
    uint8_t *p = mmap(NULL, ISLAND + 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    g_island = p;
    g_native = (uint32_t *)(p + ISLAND);
    g_fixed = g_native + 8;
    setenv("UA_A64FIX", "all", 1);
    struct a64fix f;
    a64fix_init(&f, g_island, ISLAND);
    long before = 0;
    test_lse(&f);    printf("ld<op>/swp: %ld cases\n", g_runs - before); before = g_runs;
    test_cas(&f);    printf("cas: %ld cases\n", g_runs - before); before = g_runs;
    test_ldapr(&f);  printf("ldapr: %ld cases\n", g_runs - before); before = g_runs;
    test_eor3(&f);   printf("eor3: %ld cases\n", g_runs - before); before = g_runs;
    test_dot(&f);    printf("sdot/udot: %ld cases\n", g_runs - before);
    test_threads(&f); printf("threads: %d x %d\n", THREADS, COUNT);
    printf("%s: %ld cases, %ld differ\n", g_fail ? "FAIL" : "PASS", g_runs, g_fail);
    return g_fail != 0;
}
