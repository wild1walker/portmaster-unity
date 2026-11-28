// enginerw.c's UA_CULL_STEP1 unit test (AArch64). Random SceneNodes (layers, renderers, the flag words, LOD groups
// and masks), camera layer masks and ranges; "Unity's pass" here is written after the player's
// ProcessCameraIndexListIsNodeVisibleStep1 and IsNodeVisibleFast (6000.0.61f1 arm64-v8a), separately from
// enginerw.c's. The index lists and counts must be the same.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../enginerw.c"

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }
int unityhooks_jump(uintptr_t fn, char isa, void *to) { (void)fn; (void)isa; (void)to; return -1; }

enum { MAXN = 3000, NGROUPS = 16 };
static uint8_t nodes[MAXN * 24];
static uint8_t params[0x200];
static uint8_t lodmasks[NGROUPS][64];
static struct { uint64_t masks; uint64_t pad[2]; } lod[NGROUPS];
static unsigned rng = 1;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static int unity_visible(const uint8_t *nd, const uint8_t *p, const uint8_t *lodp)
{
    uint32_t layer, mask, w;
    memcpy(&layer, nd + 8, 4); memcpy(&mask, p + 0xc0, 4);
    if (!((mask >> (layer % 32)) & 1)) return 0;
    uint64_t r; memcpy(&r, nd, 8);
    if (!r) return 0;
    memcpy(&w, nd + 0x14, 4);
    if (w & 0x20000000u) return 0;
    if (nd[0x13] & 6) return 0;
    uint64_t li = w & 0xfffffffu;
    if (!li) return 1;
    uint16_t g; memcpy(&g, nd + 0x10, 2);
    uint64_t a; memcpy(&a, lodp + 24 * (uint64_t)g, 8);
    return (nd[0x12] & ((uint8_t *)(uintptr_t)a)[li]) != 0;
}

static uint32_t unity_step1(const uint8_t *p, const uint8_t *nds, size_t b, size_t e, int32_t *out)
{
    uint32_t n = 0;
    uint64_t lp; memcpy(&lp, p + 0x1b8, 8);
    for (size_t i = b; i < e; i++) {
        uint32_t w; memcpy(&w, nds + 24 * i + 0x14, 4);
        if (w >> 30 > 2) continue;
        if (unity_visible(nds + 24 * i, p, (const uint8_t *)(uintptr_t)lp)) out[n++] = (int32_t)i;
    }
    return n;
}

int main(void)
{
    static int32_t a[MAXN], b[MAXN];
    int fails = 0; long lists = 0, vis = 0;
    for (int g = 0; g < NGROUPS; g++) { lod[g].masks = (uint64_t)(uintptr_t)lodmasks[g]; for (int k = 0; k < 64; k++) lodmasks[g][k] = (uint8_t)rnd(); }
    uint64_t lp = (uint64_t)(uintptr_t)lod; memcpy(params + 0x1b8, &lp, 8);
    for (int it = 0; it < 3000; it++) {
        size_t n = 1 + rnd() % MAXN;
        for (size_t i = 0; i < n; i++) {
            uint8_t *nd = nodes + 24 * i;
            uint64_t r = rnd() % 10 ? 0x1000 + i : 0; memcpy(nd, &r, 8);
            uint32_t layer = rnd() % 40; memcpy(nd + 8, &layer, 4);
            uint16_t g = (uint16_t)(rnd() % NGROUPS); memcpy(nd + 0x10, &g, 2);
            nd[0x12] = (uint8_t)rnd(); nd[0x13] = rnd() % 3 ? 0 : (uint8_t)rnd();
            uint32_t w = (rnd() % 4 ? 0 : rnd() % 64) | (rnd() % 8 ? 0 : 1u << 29) | (uint32_t)(rnd() % 4) << 30 | (rnd() % 16 == 0 ? 0x0f00000u : 0);
            if ((w & 0xfffffff) >= 64) w &= ~0xfffffffu;
            memcpy(nd + 0x14, &w, 4);
        }
        uint32_t mask = rnd() | rnd() << 16; if (it % 5 == 0) mask = 0xffffffff; memcpy(params + 0xc0, &mask, 4);
        size_t bgn = rnd() % n, end = bgn + rnd() % (n - bgn + 1);
        uint32_t nu = unity_step1(params, nodes, bgn, end, a);
        uint32_t no = cull_step1((uintptr_t)params, (uintptr_t)nodes, bgn, end, b);
        if (nu != no || memcmp(a, b, 4ull * nu)) { fails++; if (fails < 5) printf("FAIL: it %d: %u against Unity's %u\n", it, no, nu); }
        lists++; vis += nu;
    }
    printf("%s cullstep1: %d failure(s) (%ld lists, %ld visible nodes)\n", fails ? "FAILED" : "PASSED", fails, lists, vis);
    return fails != 0;
}
