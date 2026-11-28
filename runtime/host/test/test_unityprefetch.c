// unityprefetch.c's unit test (AArch64; run by unityhooks_test.sh). The stubs are copied into a page and run the way
// a patched call site runs them: a harness sets the argument registers x0 - x7 and the callee-saved x19 - x28 as
// the loop leaves them, calls the stub, and the stub's jump must reach the callee (a recorder) with every one of
// them and the stack pointer unchanged. The structures they walk are models of Unity's (the behaviour list: an
// iterator's root and nodes {prev, next, behaviour} inside the behaviours, MonoBehaviours with handles of each
// kind and other objects; the sprite loop's context, index list and 24-byte entries) with each piece at the end of
// a page followed by an inaccessible one, so a load past what Unity's own code reads faults; prefetched addresses
// may point anywhere (a PRFM never faults). Lists of 0 to 6 behaviours, sprite loops at every element of lists of
// 0 to 5, the culling walk's stub at any node address, the playable traversal at every input of
// graph nodes of 0 to 4 inputs, the particle systems' loop at every system of lists of 0 to 5, Animator::BuildJobs' loop at every output of lists of 0 to 5. Also: the `bl` encoder's reach, the jump to the callee (near and far), the page placement, UA_PREFETCH's
// names. The memory the stubs walk is compared before and after: they store nothing.
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../unityprefetch.c"

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

uint64_t g_in[32], g_out[32];
void call_stub(void *stub, uint64_t *in);
void rec_callee(void);
__asm__(
    "    .text\n"
    "    .p2align 2\n"
    "    .globl call_stub, rec_callee\n"
    "call_stub:\n"
    "    stp  x29, x30, [sp, #-0x60]!\n"
    "    stp  x19, x20, [sp, #0x10]\n"
    "    stp  x21, x22, [sp, #0x20]\n"
    "    stp  x23, x24, [sp, #0x30]\n"
    "    stp  x25, x26, [sp, #0x40]\n"
    "    stp  x27, x28, [sp, #0x50]\n"
    "    mov  x16, x0\n"
    "    mov  x17, x1\n"
    "    mov  x9, sp\n"
    "    str  x9, [x17, #248]\n"
    "    ldp  x0, x1, [x17]\n"
    "    ldp  x2, x3, [x17, #16]\n"
    "    ldp  x4, x5, [x17, #32]\n"
    "    ldp  x6, x7, [x17, #48]\n"
    "    ldp  x19, x20, [x17, #152]\n"
    "    ldp  x21, x22, [x17, #168]\n"
    "    ldp  x23, x24, [x17, #184]\n"
    "    ldp  x25, x26, [x17, #200]\n"
    "    ldp  x27, x28, [x17, #216]\n"
    "    blr  x16\n"
    "    ldp  x19, x20, [sp, #0x10]\n"
    "    ldp  x21, x22, [sp, #0x20]\n"
    "    ldp  x23, x24, [sp, #0x30]\n"
    "    ldp  x25, x26, [sp, #0x40]\n"
    "    ldp  x27, x28, [sp, #0x50]\n"
    "    ldp  x29, x30, [sp], #0x60\n"
    "    ret\n"
    "rec_callee:\n"
    "    adrp x16, g_out\n"
    "    add  x16, x16, :lo12:g_out\n"
    "    stp  x0, x1, [x16]\n"
    "    stp  x2, x3, [x16, #16]\n"
    "    stp  x4, x5, [x16, #32]\n"
    "    stp  x6, x7, [x16, #48]\n"
    "    stp  x19, x20, [x16, #152]\n"
    "    stp  x21, x22, [x16, #168]\n"
    "    stp  x23, x24, [x16, #184]\n"
    "    stp  x25, x26, [x16, #200]\n"
    "    stp  x27, x28, [x16, #216]\n"
    "    mov  x17, sp\n"
    "    str  x17, [x16, #248]\n"
    "    ret\n");

// n bytes at the end of a fresh page that is followed by an inaccessible page
static uint8_t *guarded(size_t n)
{
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *p = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); _exit(2); }
    mprotect(p + pg, pg, PROT_NONE);
    memset(p, 0xA5, pg);
    return p + pg - n;
}

static uint8_t *g_page;
static size_t g_used;
// a copy of stub st whose jump goes to the recorder
static void *install(int st, uint64_t vt)
{
    size_t len = (size_t)(g_stub_end[st] - g_stub_start[st]);
    uint8_t *at = g_page + g_used;
    mprotect(g_page, 4096, PROT_READ | PROT_WRITE);
    memcpy(at, g_stub_start[st], len);
    unityprefetch_tail((uint32_t *)(at + (g_stub_tail[st] - g_stub_start[st])), (uint64_t)(uintptr_t)rec_callee);
    if (st == STUB_BEH) memcpy(at + (ua_pf_beh_vt - ua_pf_beh_start), &vt, 8);
    mprotect(g_page, 4096, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)(at + len));
    g_used += (len + 15) & ~(size_t)15;
    return at;
}

static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

// runs the stub with x0 - x7, x19 - x28 random but for the given ones; checks they all reach the callee
static void run(void *stub, const int *which, const uint64_t *vals, int n, const char *what)
{
    for (int i = 0; i < 31; i++) g_in[i] = rnd();
    for (int i = 0; i < n; i++) g_in[which[i]] = vals[i];
    memset(g_out, 0, sizeof g_out);
    call_stub(stub, g_in);
    int bad = 0;
    for (int i = 0; i < 8; i++) bad |= g_out[i] != g_in[i];
    for (int i = 19; i <= 28; i++) bad |= g_out[i] != g_in[i];
    bad |= g_out[31] != g_in[31];
    CHECK(!bad, "%s: the callee did not get the caller's registers and stack", what);
}

typedef struct node { struct node *prev, *next; void *data; } node;
enum { MB_SIZE = 0x130, MB_NODE = 0x28, OTHER_SIZE = 0x30, OTHER_NODE = 0x8 };

// a behaviour in the list: a MonoBehaviour (vtable vt, handle of kind 0 none / 1 weak / 2 slot) or another object
static node *make_beh(int mono, int kind, uint64_t vt)
{
    if (!mono) {
        uint8_t *o = guarded(OTHER_SIZE);
        uint64_t other = vt ^ 0x40;
        memcpy(o, &other, 8);
        node *n = (node *)(o + OTHER_NODE);
        n->data = o;
        return n;
    }
    uint8_t *o = guarded(MB_SIZE);
    memcpy(o, &vt, 8);
    uint64_t h = 0;
    if (kind == 1) h = rnd() | 1;
    else if (kind == 2) { uint8_t *slot = guarded(8); uint64_t obj = rnd(); memcpy(slot, &obj, 8); h = (uint64_t)(uintptr_t)slot; }
    memcpy(o + 0x18, &h, 8);
    uint64_t g = rnd(), sc = rnd(), cb = rnd();
    memcpy(o + 0x20, &g, 8); memcpy(o + 0xe0, &sc, 8); memcpy(o + 0xe8, &cb, 8);
    node *n = (node *)(o + MB_NODE);
    n->data = o;
    return n;
}

static void test_beh(void)
{
    uint64_t vt = 0x7f00001000ull;
    void *stub = install(STUB_BEH, vt);
    for (int len = 0; len <= 6; len++)
        for (int mix = 0; mix < 8; mix++) {
            node *root = (node *)guarded(0x20);         // the iterator: root {prev, next}, list, current
            root->prev = root->next = root;
            node *nodes[6];
            for (int i = 0; i < len; i++) {
                int mono = (mix + i) % 4 != 3;
                nodes[i] = make_beh(mono, (mix + i) % 3, vt);
                node *last = root->prev;
                nodes[i]->prev = last; nodes[i]->next = root; last->next = nodes[i]; root->prev = nodes[i];
            }
            // Next takes the nodes off the front, one a call: the stub runs before each call (and on the empty list)
            for (int k = 0; k <= len; k++) {
                uint8_t before[6][MB_SIZE];
                for (int i = 0; i < len; i++) memcpy(before[i], nodes[i]->data, nodes[i]->data == (uint8_t *)nodes[i] - MB_NODE ? MB_SIZE : OTHER_SIZE);
                char what[64];
                snprintf(what, sizeof what, "beh: list of %d (mix %d), %d taken", len, mix, k);
                int which[1] = { 0 }; uint64_t v[1] = { (uint64_t)(uintptr_t)root };
                run(stub, which, v, 1, what);
                for (int i = 0; i < len; i++)
                    CHECK(!memcmp(before[i], nodes[i]->data, nodes[i]->data == (uint8_t *)nodes[i] - MB_NODE ? MB_SIZE : OTHER_SIZE), "%s: memory changed", what);
                if (k < len) {                          // Next: the first node off the iterator's list
                    node *n = root->next;
                    root->next = n->next; n->next->prev = root;
                    n->prev = n->next = NULL;
                }
            }
        }
}

static void test_sprite(void)
{
    void *stub = install(STUB_SPRITE, 0);
    for (int end = 0; end <= 5; end++)
        for (int at = 0; at < (end ? end : 1); at++) {
            uint8_t *ctx = guarded(0x40);
            uint32_t i = (uint32_t)at; memcpy(ctx + 0x10, &i, 4);
            int32_t *idx = end ? (int32_t *)guarded(4 * (size_t)end) : (int32_t *)guarded(4);
            int count = 7;
            uint8_t *ent = guarded(24 * (size_t)count);
            for (int k = 0; k < end; k++) idx[k] = (int32_t)((k * 3 + 1) % count);
            for (int k = 0; k < count; k++) {
                uint64_t r = k % 4 == 2 ? 0 : (uint64_t)(uintptr_t)guarded(8) + 0x28;
                memcpy(ent + 24 * k, &r, 8);
            }
            uint8_t before[24 * 7];
            memcpy(before, ent, sizeof before);
            char what[64];
            snprintf(what, sizeof what, "sprite: element %d of %d", at, end);
            int which[5] = { 19, 24, 25, 27, 3 };
            uint64_t v[5] = { (uint64_t)(uintptr_t)ctx, (uint64_t)end, (uint64_t)(uintptr_t)idx, (uint64_t)(uintptr_t)ent, rnd() };
            run(stub, which, v, 5, what);
            CHECK(!memcmp(before, ent, sizeof before), "%s: memory changed", what);
        }
}

// the playable traversal: a graph node {.., +0x20 inputs, +0x30 count} and its inputs array, each at a page's end
static void test_play(void)
{
    void *stub = install(STUB_PLAY, 0);
    for (int count = 0; count <= 4; count++)
        for (int i = 0; i < (count ? count : 1); i++) {
            uint8_t *gn = guarded(0x38), *in = guarded(16 * (size_t)(count ? count : 1));
            uint64_t a = (uint64_t)(uintptr_t)in, c = (uint64_t)count;
            memcpy(gn + 0x20, &a, 8); memcpy(gn + 0x30, &c, 8);
            for (int k = 0; k < count; k++) { uint64_t p = k == 2 ? 0 : rnd(); memcpy(in + 16 * k, &p, 8); }
            char what[64];
            snprintf(what, sizeof what, "play: input %d of %d", i, count);
            int which[7] = { 0, 1, 2, 3, 22, 23, 24 };
            uint64_t v[7] = { rnd(), rnd(), rnd(), rnd(), (uint64_t)(uintptr_t)gn, 16 * (uint64_t)i, (uint64_t)i };
            run(stub, which, v, 7, what);
        }
}

// the particle systems' loop: x24 + 0x730 holds the list {array, .., count at +0x10}; each system's GameObject at +0x20,
// the GameObject's component array at +0x20; every piece at a page's end
static void test_ps(void)
{
    void *stub = install(STUB_PS, 0);
    for (int count = 0; count <= 5; count++)
        for (int i = 0; i < (count ? count : 1); i++) {
            uint8_t *list = guarded(0x18), *slot = guarded(8), *arr = guarded(8 * (size_t)(count ? count : 1));
            uint64_t a = (uint64_t)(uintptr_t)arr, c = (uint64_t)count, l = (uint64_t)(uintptr_t)list;
            memcpy(list, &a, 8); memcpy(list + 0x10, &c, 8); memcpy(slot, &l, 8);
            for (int k = 0; k < count; k++) {
                uint8_t *ps = guarded(0x48), *go = guarded(0x28);
                uint64_t g = (uint64_t)(uintptr_t)go, r1 = rnd(), r2 = rnd(), r3 = rnd(), comps = rnd();
                memcpy(ps + 0x20, &g, 8); memcpy(ps + 0x30, &r1, 8); memcpy(ps + 0x38, &r2, 8); memcpy(ps + 0x40, &r3, 8);
                memcpy(go + 0x20, &comps, 8);
                uint64_t p = (uint64_t)(uintptr_t)ps; memcpy(arr + 8 * k, &p, 8);
            }
            char what[64];
            snprintf(what, sizeof what, "ps: system %d of %d", i, count);
            int which[4] = { 0, 1, 24, 25 };
            uint64_t v[4] = { rnd(), rnd(), (uint64_t)(uintptr_t)slot - 0x730, (uint64_t)i };
            run(stub, which, v, 4, what);
        }
}

// Animator::BuildJobs' loop: x26 the outputs' vector {array, .., count at +0x10}, x21 the slot of the one being built;
// each output (its +0x30 handle and +0xc0 Animator read; both may be 0) at a page's end
static void test_anim(void)
{
    void *stub = install(STUB_ANIM, 0);
    for (int count = 0; count <= 5; count++)
        for (int i = 0; i < (count ? count : 1); i++) {
            uint8_t *vec = guarded(0x18), *arr = guarded(8 * (size_t)(count ? count : 1));
            uint64_t a = (uint64_t)(uintptr_t)arr, c = (uint64_t)count;
            memcpy(vec, &a, 8); memcpy(vec + 0x10, &c, 8);
            for (int k = 0; k < count; k++) {
                uint8_t *o = guarded(0xc8);
                uint64_t an = k % 3 == 1 ? 0 : rnd(), h = k % 4 == 2 ? 0 : rnd();
                memcpy(o + 0xc0, &an, 8); memcpy(o + 0x30, &h, 8);
                uint64_t op = (uint64_t)(uintptr_t)o; memcpy(arr + 8 * k, &op, 8);
            }
            char what[64];
            snprintf(what, sizeof what, "anim: output %d of %d", i, count);
            int which[4] = { 0, 1, 21, 26 };
            uint64_t v[4] = { count ? 0 : rnd(), rnd(), (uint64_t)(uintptr_t)(arr + 8 * i), (uint64_t)(uintptr_t)vec };
            if (count) memcpy(&v[0], arr + 8 * i, 8);
            run(stub, which, v, 4, what);
        }
}

int main(void)
{
    CHECK(unityprefetch_wanted("1", "beh") && unityprefetch_wanted("beh,sprite", "sprite") && !unityprefetch_wanted("behx", "beh") &&
          !unityprefetch_wanted("0", "beh") && !unityprefetch_wanted("sprite", "beh"), "UA_PREFETCH names");
    CHECK(unityprefetch_bl(0x1000, 0x1008) == 0x94000002u && unityprefetch_bl(0x1008, 0x1000) == 0x97fffffeu, "bl encoding");
    CHECK(!unityprefetch_bl(0, 1u << 27) && unityprefetch_bl(1u << 27, 0) && !unityprefetch_bl(0, 2), "bl reach");
    uint32_t t[4];
    unityprefetch_tail(t, (uint64_t)(uintptr_t)t + 0x100);
    CHECK(t[0] == 0x14000040u && t[1] == 0xd503201fu, "near jump");
    unityprefetch_tail(t, (uint64_t)(uintptr_t)t + (1ull << 30));
    CHECK(t[0] == 0x58000050u && t[1] == 0xd61f0200u, "far jump");
    uintptr_t lo = (uintptr_t)rec_callee & ~(uintptr_t)0xfff;
    g_page = unityprefetch_near(lo, lo + 0x10000, 4096);
    uintptr_t pa = (uintptr_t)g_page, far = pa > lo ? pa + 4096 - lo : lo + 0x10000 - pa;     // the farthest pair of ends
    CHECK(g_page && far < (128u << 20), "a page in reach");
    if (!g_page) { printf("FAILED unityprefetch: no page\n"); return 1; }
    test_beh();
    test_sprite();
    void *cull = install(STUB_CULL, 0);                 // PRFM only: any node address
    for (int k = 0; k < 16; k++) { int which[1] = { 23 }; uint64_t v[1] = { k ? rnd() : 0 }; run(cull, which, v, 1, "cull"); }
    test_play();
    test_ps();
    test_anim();
    printf("%s unityprefetch: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
