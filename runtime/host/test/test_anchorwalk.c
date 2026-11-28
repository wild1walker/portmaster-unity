// enginerw.c's UA_ANCHOR_WALK unit test (AArch64). A model of Unity's TransformHierarchy (parent indices, deep child
// counts, change masks, the transforms and the next indices) and of what UpdateAnchorPositionIfTransformChanged calls.
// "Unity's walk" here is written after the players' code (6000.0.61f1 and 2017.4.x arm64-v8a, the same algorithm),
// separately from enginerw.c's. Random hierarchies (in order, with stretches out of order as reparenting leaves
// them, some entries not RectTransforms), random change bits, an UpdateAnchorPosition that sets bits further down
// the walk and in another system's bit: each start runs through Unity's walk and through ours (on and verify), and
// the calls (in order, with their flag), the final masks and the tail's call must be the same; verify must count no
// difference. Both layouts (6000.0: GetTransformAccess and a second bit cleared; 2017.4: the hierarchy in the
// transform) are run.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../enginerw.c"

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }
int unityhooks_jump(uintptr_t fn, char isa, void *to) { (void)fn; (void)isa; (void)to; return -1; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

enum { MAXN = 600, NTYPES = 8, RECT_TYPE = 5, MAXCALLS = 4096 };
struct hier {                                   // the fields at the players' offsets are filled in place
    uint8_t raw[0x100];
};
struct xform { uint8_t raw[0x48]; };            // +0xc type word (index << 21), +0x28 hierarchy (6000), +0x38/+0x40 (2017)
static int32_t parent[MAXN], deep[MAXN];
static uint32_t next_[MAXN];
static uint64_t chg[MAXN];
static uint64_t xptr[MAXN];
static struct xform xf[MAXN];
static struct hier H;
static uint64_t types[NTYPES + 1];
static uint64_t rect_marker;
static int32_t guard;
static uint32_t bitn = 3, bit2n = 7;
static uint64_t tailarg = 0x1234;
static int layout6;
static uint32_t N;

struct call { uint32_t idx; int flag; };
static struct call calls[MAXCALLS];
static int ncalls, tails;
static uintptr_t tail_x0, tail_x1;
static unsigned rng = 1;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static int poke;                                // the model's UpdateAnchorPosition sets bits further on

static void put64(uint8_t *p, uint32_t o, uint64_t v) { memcpy(p + o, &v, 8); }
static uint32_t idx_of(uintptr_t t) { return (uint32_t)(((struct xform *)t) - xf); }

static void model_update(uintptr_t t, int flag)
{
    if (ncalls < MAXCALLS) { calls[ncalls].idx = idx_of(t); calls[ncalls].flag = flag; ncalls++; }
    if (poke) {                                 // marks a later entry's bit and another system's bit somewhere
        uint32_t i = idx_of(t), j = next_[i];
        if (j < N && rnd() % 2) chg[j] |= 1ull << bitn;
        chg[rnd() % N] |= 1ull << 11;
    }
}
static struct access_pair model_access(uintptr_t t) { struct access_pair a = { (uintptr_t)&H, idx_of(t) }; return a; }
static void model_tail6(uintptr_t t) { tails++; tail_x0 = t; tail_x1 = 0; }
static void model_tail17(uintptr_t a, uintptr_t h) { tails++; tail_x0 = a; tail_x1 = h; }

// Unity's walk, as the players' code does it
static void unity_walk(uintptr_t self, int flag)
{
    if (layout6) { if (guard > 1 || *(uint64_t *)(self + 0x28) == 0) return; }
    else if (guard > 1) return;
    uint32_t x8 = idx_of(self);
    uint64_t bit = 1ull << bitn;
    for (;;) {
        int32_t p = parent[x8];
        if (p < 0) break;
        if (!(chg[p] & bit)) break;
        x8 = (uint32_t)p;
    }
    uint32_t n = (uint32_t)deep[x8];
    while (n) {
        uint64_t w = chg[x8];
        uint64_t was = w & bit;
        w &= ~bit;
        chg[x8] = w;
        if (was) {
            if (layout6) chg[x8] = w & ~(1ull << bit2n);
            uintptr_t t = (uintptr_t)xptr[x8];
            uint32_t ty = *(uint32_t *)(t + 0xc) >> 21;
            if (types[ty + 1] == (uint64_t)(uintptr_t)&rect_marker) model_update(t, flag & 1);
        }
        x8 = next_[x8];
        n--;
    }
    if (layout6) model_tail6(self); else model_tail17(tailarg, *(uint64_t *)(self + 0x38));
}

// a random forest: depth-first order, then a few subtrees moved to the end (next indices then jump)
static void build(void)
{
    N = 20 + rnd() % (MAXN - 40);
    int32_t stack[64]; int sp = 0;
    for (uint32_t i = 0; i < N; i++) {
        while (sp && rnd() % 3 == 0) sp--;
        if (sp > 60) sp = 60;
        parent[i] = sp ? stack[sp - 1] : -1;
        stack[sp++] = (int32_t)i;
    }
    for (uint32_t i = 0; i < N; i++) deep[i] = 1;
    for (int32_t i = (int32_t)N - 1; i >= 0; i--) if (parent[i] >= 0) deep[parent[i]] += deep[i];
    // the order of the walk: a permutation of storage slots, contiguous runs with jumps
    static uint32_t slot[MAXN], order[MAXN];
    for (uint32_t i = 0; i < N; i++) order[i] = i;
    if (rnd() % 2) {                            // a few runs swapped: slots no longer in walk order
        for (int k = 0; k < 3; k++) {
            uint32_t a = rnd() % N, b = rnd() % N, len = 1 + rnd() % 20;
            for (uint32_t m = 0; m < len && a + m < N && b + m < N; m++) {
                uint32_t t = order[a + m]; order[a + m] = order[b + m]; order[b + m] = t;
            }
        }
    }
    // node k (in walk order) lives in slot order[k]; remap parent/deep to slots
    static int32_t p2[MAXN], d2[MAXN];
    for (uint32_t k = 0; k < N; k++) slot[k] = order[k];
    for (uint32_t k = 0; k < N; k++) {
        p2[slot[k]] = parent[k] < 0 ? -1 : (int32_t)slot[parent[k]];
        d2[slot[k]] = deep[k];
        next_[slot[k]] = k + 1 < N ? slot[k + 1] : N;  // past the end: never followed for an entry walked
    }
    memcpy(parent, p2, sizeof(int32_t) * N); memcpy(deep, d2, sizeof(int32_t) * N);
    for (uint32_t i = 0; i < N; i++) {
        uint32_t ty = rnd() % 4 ? RECT_TYPE : rnd() % NTYPES;
        memset(&xf[i], 0, sizeof xf[i]);
        uint32_t w = ty << 21 | (rnd() & 0x1fffff);
        memcpy(xf[i].raw + 0xc, &w, 4);
        put64(xf[i].raw, 0x28, (uint64_t)(uintptr_t)&H);
        put64(xf[i].raw, 0x38, (uint64_t)(uintptr_t)&H);
        put64(xf[i].raw, 0x40, i | 0xabcd00000000ull);
        xptr[i] = (uint64_t)(uintptr_t)&xf[i];
        uint64_t v = rnd() & 0xff0ff;           // other systems' bits
        if (rnd() % 100 < 4) v |= 1ull << bitn;
        if (rnd() % 5 == 0) v |= 1ull << bit2n;
        chg[i] = v;
    }
}

static void setup_env(void)
{
    g_ae.v6 = layout6;
    g_ae.guard = &guard; g_ae.bit = &bitn; g_ae.bit2 = &bit2n;
    static uint64_t tptr; tptr = (uint64_t)(uintptr_t)types; g_ae.table = (uint64_t volatile *)&tptr;
    for (int i = 0; i <= NTYPES; i++) types[i] = 0x1000 + i;
    types[RECT_TYPE + 1] = (uint64_t)(uintptr_t)&rect_marker;
    g_ae.rect = (uintptr_t)&rect_marker;
    g_ae.update = model_update; g_ae.access = model_access; g_ae.tail6 = model_tail6; g_ae.tail17 = model_tail17;
    g_ae.tailarg = &tailarg;
    if (layout6) { g_ae.o_parent = 0x20; g_ae.o_deep = 0x28; g_ae.o_changed = 0x48; g_ae.o_xforms = 0x30; g_ae.o_next = 0x70; }
    else { g_ae.o_parent = 0x10; g_ae.o_deep = 0x18; g_ae.o_changed = 0x60; g_ae.o_xforms = 0x20; g_ae.o_next = 0x80; }
    memset(&H, 0, sizeof H);
    put64(H.raw, g_ae.o_parent, (uint64_t)(uintptr_t)parent);
    put64(H.raw, g_ae.o_deep, (uint64_t)(uintptr_t)deep);
    put64(H.raw, g_ae.o_changed, (uint64_t)(uintptr_t)chg);
    put64(H.raw, g_ae.o_xforms, (uint64_t)(uintptr_t)xptr);
    put64(H.raw, g_ae.o_next, (uint64_t)(uintptr_t)next_);
}

int main(void)
{
    static uint64_t chg0[MAXN], chgU[MAXN];
    static struct call callsU[MAXCALLS];
    long walks = 0, withcalls = 0;
    for (int lay = 0; lay < 2; lay++) {
        layout6 = lay;
        setup_env();
        for (int it = 0; it < 4000; it++) {
            rng = 1000u * (unsigned)it + 7u + (unsigned)lay;
            build();
            poke = it % 3 == 0;
            guard = it % 17 == 0 ? 2 : (int32_t)(rnd() % 2);
            uint32_t start = rnd() % N;
            int flag = (int)(rnd() % 3);
            memcpy(chg0, chg, 8 * N);
            unsigned rng0 = rng;
            ncalls = tails = 0; tail_x0 = tail_x1 = 0;
            unity_walk((uintptr_t)&xf[start], flag);
            int nU = ncalls, tU = tails; uintptr_t u0 = tail_x0, u1 = tail_x1;
            memcpy(callsU, calls, sizeof(struct call) * (size_t)nU);
            memcpy(chgU, chg, 8 * N);
            for (int mode = 1; mode <= 3; mode++) {
                memcpy(chg, chg0, 8 * N);
                rng = rng0;
                ncalls = tails = 0; tail_x0 = tail_x1 = 0;
                g_anchor_mode = mode;
                uint64_t bad0 = g_aw_bad;
                anchor_walk((uintptr_t)&xf[start], flag);
                CHECK(ncalls == nU && !memcmp(calls, callsU, sizeof(struct call) * (size_t)nU),
                      "layout %d it %d mode %d: calls %d against Unity's %d", lay, it, mode, ncalls, nU);
                CHECK(!memcmp(chg, chgU, 8 * N), "layout %d it %d mode %d: masks differ", lay, it, mode);
                CHECK(tails == tU && tail_x0 == u0 && tail_x1 == u1, "layout %d it %d mode %d: tail", lay, it, mode);
                CHECK(g_aw_bad == bad0, "layout %d it %d: verify counted a difference", lay, it);
            }
            walks++; withcalls += nU > 0;
        }
    }
    printf("%s anchorwalk: %d failure(s) (%ld walks, %ld with calls, %llu entries verified)\n", fails ? "FAILED" : "PASSED",
           fails, walks, withcalls, (unsigned long long)g_aw_verified);
    return fails != 0;
}
