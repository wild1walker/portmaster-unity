// unityphys2d.c's unit test (AArch64): its FlagContactsForRecreate and WakeAllBodyContacts against models of Unity
// 2017.4.40f1's, written here after the player's code separately from unityphys2d.c's.
//
// The contact map: core::hash_map<pair<Collider2D*, Collider2D*>, Collision2D> in a PhysicsManager2D model (the map at
// +72: buckets, mask = (buckets - 1) * 16, count, empty left; 112-byte buckets, hash -1 empty / -2 erased, the pair at
// +8 / +16), its operator[] (insert with the player's probing, growing when no empty bucket is left: every bucket moves)
// and erase_node. Random runs of inserts, erases, grows and flags: after each flag through the hook the whole map
// must be byte for byte what Unity's scan leaves on a copy, in mode 1 and verify (which must count nothing missed).
// The wake: bodies and contacts with their edge lists (Box2D's layout as read: body type +0, flags +4, velocities
// +64 - +88, sleep time +172, contact list +136; edge contact +8, next +24; contact flags +8, fixtures +96 / +104;
// fixture body +32; collider fixture count +104, fixtures +88). Walks inside a sync with contacts removed and bodies
// changed between them only as Unity's rebuild can; outside a sync every walk is Unity's. After each step the bodies
// must equal the model's.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../unityphys2d.c"

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }
int unityhooks_jump(uintptr_t fn, char isa, void *to) { (void)fn; (void)isa; (void)to; return -1; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#if !defined(__aarch64__)
int main(void) { printf("PASSED unityphys2d: 0 failure(s) (AArch64 only)\n"); return 0; }
#else
static uint8_t pm[256];                         // PhysicsManager2D: the map at +72
#define MAP ((uintptr_t)pm + 72)
static uint8_t colliders[64][16];               // the keys (only their addresses and +8, the instance id)

static uint32_t m_hash(uintptr_t a, uintptr_t b)
{
    uint32_t x = U32(a, 8) * 4097u + 0x7ed55d16u;
    x ^= U32(b, 8); x ^= x >> 19; x ^= 0xc761c23cu; x *= 33; x = (x + 0xe9f8cc1du) ^ (0xaccf6200u + (x << 9));
    return x & 0xfffffffcu;
}
static void m_alloc(uint32_t nb)
{
    uint8_t *b = malloc(112ull * nb);
    for (uint32_t i = 0; i < nb; i++) { memset(b + 112ull * i, 0xab, 112); U32(b + 112ull * i, 0) = 0xffffffffu; }
    U64(MAP, 0) = (uintptr_t)b; U32(MAP, 8) = (nb - 1) * 16; U32(MAP, 12) = 0; U32(MAP, 16) = nb;
}
static uintptr_t m_probe(uintptr_t a, uintptr_t b, uint32_t h, int *found)
{
    uintptr_t base = (uintptr_t)U64(MAP, 0); uint32_t mask = U32(MAP, 8);
    uintptr_t first_free = 0;
    for (uint32_t i = h & mask, step = 16;; i = (i + step) & mask, step += 16) {
        uintptr_t k = base + i * 7ull;
        uint32_t hv = U32(k, 0);
        if (hv == h && U64(k, 8) == a && U64(k, 16) == b) { *found = 1; return k; }
        if (hv == 0xfffffffeu && !first_free) first_free = k;
        if (hv == 0xffffffffu) { *found = 0; return first_free ? first_free : k; }
    }
}
static void m_grow(void)
{
    uintptr_t ob = (uintptr_t)U64(MAP, 0); uint32_t onb = U32(MAP, 8) / 16 + 1;
    m_alloc(onb * 2);
    for (uint32_t i = 0; i < onb; i++) {
        uintptr_t k = ob + 112ull * i;
        if (U32(k, 0) >= 0xfffffffeu) continue;
        int f; uintptr_t d = m_probe(U64(k, 8), U64(k, 16), U32(k, 0), &f);
        if (U32(d, 0) == 0xffffffffu) U32(MAP, 16)--;
        memcpy((void *)d, (void *)k, 112); U32(MAP, 12)++;
    }
    free((void *)ob);
}
static uintptr_t m_opidx(uintptr_t map, uintptr_t key)       // Unity's operator[]: the value (bucket + 24)
{
    (void)map;
    uintptr_t a = U64(key, 0), b = U64(key, 8);
    uint32_t h = m_hash(a, b);
    int f; uintptr_t k = m_probe(a, b, h, &f);
    if (f) return k + 24;
    if (U32(MAP, 16) <= 1) { m_grow(); k = m_probe(a, b, h, &f); }
    if (U32(k, 0) == 0xffffffffu) U32(MAP, 16)--;
    U32(MAP, 12)++;
    U32(k, 0) = h; U64(k, 8) = a; U64(k, 16) = b;
    U32(k, 56) = (uint32_t)rand(); U8(k, 98) = 0;
    return k + 24;
}
static void m_erase(uintptr_t map, uintptr_t node) { (void)map; U32(node, 0) = 0xfffffffeu; U32(MAP, 12)--; }
static void m_flag(uintptr_t self, uintptr_t c)              // Unity's scan, as read
{
    uintptr_t base = (uintptr_t)U64(self + 72, 0);
    uintptr_t end = base + U32(self + 72, 8) * 7ull + 0x70;
    for (uintptr_t k = base; k < end; k += 0x70) {
        if (U32(k, 0) >= 0xfffffffeu) continue;
        if (U64(k, 8) == c || U64(k, 16) == c) { U8(k, 98) = 1; U32(k, 56) = 4; }
    }
}
static void m_delnodes(uintptr_t map) { (void)map; }

// --- wake model
enum { NB = 24, NC = 80 };
static uint8_t bodies[NB][192], fixtures[NB][64], contacts[NC][128];
static uint8_t col[192]; static uint64_t colfix[1];
static void m_setawake_static_or_not(uintptr_t bd)
{
    if (U32(bd, 0)) { if (!(U16(bd, 4) & 2)) { U16(bd, 4) |= 2; U32(bd, 172) = 0; } }
    else { U16(bd, 4) &= 0xfffd; U32(bd, 172) = 0; U64(bd, 72) = 0; U64(bd, 80) = 0; U64(bd, 64) = 0; }
}
static void m_wake(uintptr_t c)                              // Unity's walk, as read
{
    if (!U64(c, 104)) return;
    uintptr_t body = U64(U64(U64(c, 88), 0), 32);
    if (!body) return;
    for (uintptr_t e = U64(body, 136); e; e = U64(e, 24)) {
        uintptr_t ct = U64(e, 8);
        if (U8(ct, 8) & 0x40) continue;
        m_setawake_static_or_not(U64(U64(ct, 96), 32));
        m_setawake_static_or_not(U64(U64(ct, 104), 32));
    }
}
static void m_sync(uintptr_t self) { (void)self; }

// --- DestroyFixture model: two worlds built alike, one driven through Unity's DestroyFixture (written here as read), the
// other through ours; compared by index after every step
enum { NFX = 160, NCT = 400, NBD = 6 };
struct W {
    uint8_t world[0x19400];
    uint8_t body[NBD][192];
    uint8_t fx[NFX][88];
    uint8_t ct[NCT][128];
    int fx_used[NFX], ct_used[NCT];
    char log[1 << 16]; int loglen;
};
static struct W *WA, *WB, *cur;
#define CM(w) ((uintptr_t)(w)->world + 0x192a0)
static int fx_idx(struct W *w, uintptr_t f) { return f ? (int)((f - (uintptr_t)w->fx[0]) / 88) : -1; }
static int ct_idx(struct W *w, uintptr_t c) { return c ? (int)((c - (uintptr_t)w->ct[0]) / 128) : -1; }
static void wlog(const char *t, uintptr_t f) { cur->loglen += snprintf(cur->log + cur->loglen, sizeof cur->log - cur->loglen, "%s%d ", t, fx_idx(cur, f)); }
static void m_proxies(uintptr_t f, uintptr_t bp) { (void)bp; wlog("P", f); }
static void m_fixdestroy(uintptr_t f, uintptr_t a) { (void)a; wlog("D", f); }
static void m_blockfree(uintptr_t a, uintptr_t f, int n) { (void)a; (void)n; wlog("F", f); cur->fx_used[fx_idx(cur, f)] = 0; }
static void m_resetmass(uintptr_t b) { (void)b; wlog("R", 0); }
static void m_cmdestroy(uintptr_t cm, uintptr_t c)            // b2ContactManager::Destroy as read
{
    if (U64(c, 16)) U64(U64(c, 16), 24) = U64(c, 24);
    if (U64(c, 24)) U64(U64(c, 24), 16) = U64(c, 16);
    if (U64(cm, 88) == c) U64(cm, 88) = U64(c, 24);
    uintptr_t ba = U64(U64(c, 96), 32), bb = U64(U64(c, 104), 32);
    if (U64(c, 48)) U64(U64(c, 48), 24) = U64(c, 56);
    if (U64(c, 56)) U64(U64(c, 56), 16) = U64(c, 48);
    if (U64(ba, 136) == c + 32) U64(ba, 136) = U64(c, 56);
    if (U64(c, 80)) U64(U64(c, 80), 24) = U64(c, 88);
    if (U64(c, 88)) U64(U64(c, 88), 16) = U64(c, 80);
    if (U64(bb, 136) == c + 64) U64(bb, 136) = U64(c, 88);
    U32(cm, 96)--;
    cur->loglen += snprintf(cur->log + cur->loglen, sizeof cur->log - cur->loglen, "C%d ", ct_idx(cur, c));
    cur->ct_used[ct_idx(cur, c)] = 0;
}
static uintptr_t m_createfix(uintptr_t body, uintptr_t def, int reset)
{
    (void)reset; int i = (int)def;
    uintptr_t f = (uintptr_t)cur->fx[i]; memset((void *)f, 0, 88); cur->fx_used[i] = 1;
    U64(f, 24) = U64(body, 112); U64(body, 112) = f; U32(body, 120)++; U64(f, 32) = body;
    return f;
}
static void m_addpair(uintptr_t cm, uintptr_t fa, uintptr_t fb)   // (the test passes fixtures; the contact's index: the lowest free)
{
    int i = 0; while (cur->ct_used[i]) i++;
    cur->ct_used[i] = 1;
    uintptr_t c = (uintptr_t)cur->ct[i]; memset((void *)c, 0, 128);
    U64(c, 96) = fa; U64(c, 104) = fb;
    U64(c, 24) = U64(cm, 88); if (U64(cm, 88)) U64(U64(cm, 88), 16) = c; U64(cm, 88) = c;
    uintptr_t ba = U64(fa, 32), bb = U64(fb, 32);
    U64(c, 32) = bb; U64(c, 40) = c; U64(c, 56) = U64(ba, 136); if (U64(ba, 136)) U64(U64(ba, 136), 16) = c + 32; U64(ba, 136) = c + 32;
    U64(c, 64) = ba; U64(c, 72) = c; U64(c, 88) = U64(bb, 136); if (U64(bb, 136)) U64(U64(bb, 136), 16) = c + 64; U64(bb, 136) = c + 64;
    U32(cm, 96)++;
}
static void m_destroyfix(uintptr_t body, uintptr_t fix, int reset)  // Unity's, as read
{
    if (!fix) return;
    uintptr_t world = U64(body, 88);
    if (U8(world, 0x19298) & 2) return;
    for (uintptr_t *node = (uintptr_t *)(body + 112); *node; node = (uintptr_t *)(*node + 24))
        if (*node == fix) { *node = U64(fix, 24); break; }
    for (uintptr_t e = U64(body, 136); e;) {
        uintptr_t c = U64(e, 8); e = U64(e, 24);
        if (U64(c, 96) == fix || U64(c, 104) == fix) m_cmdestroy(world + 0x192a0, c);
    }
    if (U8(body, 4) & 0x20) m_proxies(fix, world + 0x192a0);
    U64(fix, 24) = 0; U64(fix, 32) = 0;
    m_fixdestroy(fix, world); m_blockfree(world, fix, 88);
    U32(body, 120)--;
    if (reset & 1) m_resetmass(body);
}
static int w_same(void)
{
    if (U32(CM(WA), 96) != U32(CM(WB), 96) || WA->loglen != WB->loglen || memcmp(WA->log, WB->log, WA->loglen)) return 0;
    for (uintptr_t a = U64(CM(WA), 88), b = U64(CM(WB), 88);; a = U64(a, 24), b = U64(b, 24)) {
        if (ct_idx(WA, a) != ct_idx(WB, b)) return 0;
        if (!a) break;
    }
    for (int k = 0; k < NBD; k++) {
        if (U32(WA->body[k], 120) != U32(WB->body[k], 120)) return 0;
        for (uintptr_t a = U64(WA->body[k], 112), b = U64(WB->body[k], 112);; a = U64(a, 24), b = U64(b, 24)) {
            if (fx_idx(WA, a) != fx_idx(WB, b)) return 0;
            if (!a) break;
        }
        for (uintptr_t a = U64(WA->body[k], 136), b = U64(WB->body[k], 136);; a = U64(a, 24), b = U64(b, 24)) {
            if (!a || !b) { if (a || b) return 0; break; }
            if (ct_idx(WA, U64(a, 8)) != ct_idx(WB, U64(b, 8))) return 0;
        }
    }
    return 1;
}
static void destroyfix_test(void)
{
    WA = calloc(1, sizeof *WA); WB = calloc(1, sizeof *WB);
    f_proxies = m_proxies; f_fixdestroy = m_fixdestroy; f_blockfree = m_blockfree; f_resetmass = m_resetmass;
    o_cmdestroy = m_cmdestroy; o_createfix = m_createfix; o_addpair = m_addpair;
    unsigned long steps = 0;
    for (int mode = 1; mode <= 2; mode++) {
        g_mode = mode; c_df_bad = 0;
        for (int run = 0; run < 300; run++) {
            for (int w = 0; w < 2; w++) {
                struct W *x = w ? WB : WA;
                memset(x->body, 0, sizeof x->body); memset(x->fx_used, 0, sizeof x->fx_used); memset(x->ct_used, 0, sizeof x->ct_used);
                memset((void *)CM(x), 0, 128); x->loglen = 0;
                for (int k = 0; k < NBD; k++) { U64(x->body[k], 88) = (uintptr_t)x->world; U8(x->body[k], 4) = k % 2 ? 0x20 : 0; U32(x->body[k], 0) = k ? 2 : 0; }
            }
            g_cm = 0; g_cnt_ok = 0;
            for (int st = 0; st < 400; st++, steps++) {
                int r = rand() % 10, i = rand() % NFX, k = rand() % NBD, j = rand() % NFX;
                if (r < 4) {                    // a fixture made on a body
                    if (WA->fx_used[i]) continue;
                    cur = WA; m_createfix((uintptr_t)WA->body[k], (uintptr_t)i, 0);
                    cur = WB; createfix_hook((uintptr_t)WB->body[k], (uintptr_t)i, 0);
                } else if (r < 7) {             // a contact between two fixtures on different bodies (not already paired)
                    if (!WA->fx_used[i] || !WA->fx_used[j] || U64(WA->fx[i], 32) == U64(WA->fx[j], 32)) continue;
                    cur = WA; m_addpair(CM(WA), (uintptr_t)WA->fx[i], (uintptr_t)WA->fx[j]);
                    cur = WB; addpair_hook(CM(WB), (uintptr_t)WB->fx[i], (uintptr_t)WB->fx[j]);
                } else if (r < 8) {             // a contact destroyed elsewhere (the step's Collide)
                    uintptr_t a = U64(CM(WA), 88), b = U64(CM(WB), 88);
                    if (!a) continue;
                    cur = WA; m_cmdestroy(CM(WA), a);
                    cur = WB; cmdestroy_hook(CM(WB), b);
                } else {                        // DestroyFixture
                    if (!WA->fx_used[i]) continue;
                    int reset = rand() % 2;
                    uintptr_t ba = U64(WA->fx[i], 32), bb = U64(WB->fx[i], 32);
                    cur = WA; m_destroyfix(ba, (uintptr_t)WA->fx[i], reset);
                    cur = WB; destroyfix_hook(bb, (uintptr_t)WB->fx[i], reset);
                }
                if (!w_same()) { CHECK(0, "destroyfix mode %d run %d step %d: the worlds differ", mode, run, st); break; }
            }
        }
        CHECK(c_df_bad == 0, "destroyfix mode %d: verify counted %llu differences", mode, (unsigned long long)c_df_bad);
    }
    printf("destroyfix: %lu steps, %llu unlinked by the map, %llu without contacts, %llu count rebuilds\n", steps,
           (unsigned long long)c_df_fast_unlink, (unsigned long long)c_df_no_contacts, (unsigned long long)c_cnt_rebuild);
    free(WA); free(WB);
}

// --- the manifold-buffer pool: Unity's emplace_back reserve(4) and ~dynamic_array<Collision2D> modelled; every block
// Unity hands out is freed exactly once or sits in the pool, every element buffer is cleared, outer arrays freed
static int live_blocks;
static uintptr_t m_realloc_block(void) { live_blocks++; return (uintptr_t)malloc(416); }
static void m_mfreserve(uintptr_t arr, uint64_t n) { U64(arr, 24) = n; U64(arr, 0) = n == 4 ? m_realloc_block() : (live_blocks++, (uintptr_t)malloc(104 * n)); }
static void m_freeint(uintptr_t b, uint32_t label) { (void)label; if (b) { free((void *)b); live_blocks--; } }
static void pool_test(void)
{
    o_mfreserve = m_mfreserve; f_freeint = m_freeint;
    unsigned long steps = 0;
    for (int mode = 1; mode <= 2; mode++) {
        g_pool_mode = mode; c_pool_bad = 0;
        for (int st = 0; st < 3000; st++, steps++) {
            int n = rand() % 40;
            uint8_t *outer = malloc(88 * (n ? n : 1)); live_blocks++;
            uint8_t hdr[32] = { 0 };
            U64(hdr, 0) = (uintptr_t)outer; U32(hdr, 8) = 37; U64(hdr, 16) = (uint64_t)n; U64(hdr, 24) = (uint64_t)(n ? n : 1);
            for (int i = 0; i < n; i++) {
                uintptr_t e = (uintptr_t)outer + 88ull * i;
                memset((void *)e, 0, 88); U32(e, 8) = rand() % 8 ? 37 : 12;
                mfreserve_hook(e, 4);
                if (rand() % 10 == 0) { m_freeint(U64(e, 0), 37); U64(e, 0) = 0; mfreserve_hook(e, 4); }   // (an array freed elsewhere)
                if (rand() % 12 == 0) { uint64_t c = 4 + rand() % 4; uintptr_t nb = (live_blocks++, (uintptr_t)malloc(104 * c)); m_freeint(U64(e, 0), 37); U64(e, 0) = nb; U64(e, 24) = c; }
                if (rand() % 25 == 0) U64(e, 24) |= 1ull << 63;  // not owned: left alone
                U64(e, 16) = rand() % 5;
            }
            for (int i = 0; i < n; i++) {               // the not-owned ones: their buffers are someone else's
                uintptr_t e = (uintptr_t)outer + 88ull * i;
                if (U64(e, 24) >> 63) { m_freeint(U64(e, 0), 37); U64(e, 0) = 0; }
            }
            colldtor_hook((uintptr_t)hdr);
            CHECK(U64(hdr, 0) == 0, "pool mode %d step %d: the array's buffer not cleared", mode, st);
        }
        CHECK(c_pool_bad == 0, "pool mode %d: %llu bad", mode, (unsigned long long)c_pool_bad);
    }
    CHECK(live_blocks == (int)g_pooln, "pool: %d blocks alive, %u pooled (a leak or a double free)", live_blocks, g_pooln);
    printf("pool: %lu steps, %llu reused, %llu kept, %u pooled\n", steps, (unsigned long long)c_pool_out, (unsigned long long)c_pool_back, g_pooln);
}

int main(void)
{
    srand(7);
    g_main = pthread_self(); g_on = 1;
    o_opidx = m_opidx; o_erase = m_erase; o_flag = m_flag; o_delnodes = m_delnodes; o_wake = m_wake; o_sync = m_sync;
    for (int i = 0; i < 64; i++) U32(colliders[i], 8) = (uint32_t)(1000 + i * 2);
    unsigned long flags = 0, grows = 0;
    for (int mode = 1; mode <= 2; mode++) {
        g_mode = mode; c_flag_bad = 0;
        m_alloc(8); ix_clear(); g_map = 0;
        uint8_t *copy = NULL;
        for (int step = 0; step < 40000; step++) {
            int r = rand() % 10;
            uint32_t nb0 = U32(MAP, 8);
            if (r < 5) {
                int i = rand() % 64, j = rand() % 64;
                if (i == j) continue;
                uintptr_t key[2] = { (uintptr_t)colliders[i], (uintptr_t)colliders[j] };
                opidx_hook(MAP, (uintptr_t)key);
                if (U32(MAP, 8) != nb0) grows++;
            } else if (r < 7) {                 // erase a random occupied bucket (ProcessContacts / DestroyContacts)
                uint32_t nb = U32(MAP, 8) / 16 + 1, s = (uint32_t)rand() % nb;
                for (uint32_t t = 0; t < nb; t++) {
                    uintptr_t k = U64(MAP, 0) + 112ull * ((s + t) % nb);
                    if (U32(k, 0) < 0xfffffffeu) { erase_hook(MAP, k); break; }
                }
            } else {                            // flag one collider: ours on the map, Unity's on a copy
                uintptr_t c = (uintptr_t)colliders[rand() % 64];
                size_t sz = 112ull * (U32(MAP, 8) / 16 + 1);
                copy = realloc(copy, sz);
                memcpy(copy, (void *)U64(MAP, 0), sz);
                uint8_t savepm[256]; memcpy(savepm, pm, sizeof pm);
                U64(MAP, 0) = (uintptr_t)copy; m_flag((uintptr_t)pm, c);
                memcpy(pm, savepm, sizeof pm);
                flag_hook((uintptr_t)pm, c);
                CHECK(!memcmp(copy, (void *)U64(MAP, 0), sz), "mode %d step %d: the map differs from Unity's scan", mode, step);
                flags++;
            }
            if (step % 9000 == 4500) delnodes_hook(MAP);   // (the map kept: the index must rebuild)
        }
        CHECK(c_flag_bad == 0, "mode %d: verify counted %llu missed buckets", mode, (unsigned long long)c_flag_bad);
        free((void *)U64(MAP, 0)); free(copy);
    }
    // the wake: one ground body (static, index 0) with many contacts, dynamic bodies, a sync with several walks
    unsigned long walks = 0;
    for (int mode = 1; mode <= 2; mode++) {
        g_mode = mode; c_wake_bad = 0;
        for (int run = 0; run < 3000; run++) {
            memset(bodies, 0, sizeof bodies); memset(contacts, 0, sizeof contacts);
            for (int b = 0; b < NB; b++) {
                U32(bodies[b], 0) = b == 0 ? 0 : (uint32_t)(1 + rand() % 2);
                U16(bodies[b], 4) = (uint16_t)(rand() & 0xff);
                U32(bodies[b], 172) = (uint32_t)rand();
                if (b == 0 && rand() % 2) { U64(bodies[b], 64) = (uint64_t)rand(); U64(bodies[b], 80) = 1; }
                U64(fixtures[b], 32) = (uintptr_t)bodies[b];
            }
            int n = rand() % NC;
            uintptr_t prev = 0;
            for (int i = n - 1; i >= 0; i--) {  // the ground body's edge list: each contact's node A at +16
                uintptr_t ct = (uintptr_t)contacts[i];
                U8(ct, 8) = (uint8_t)(rand() % 5 == 0 ? 0x40 : 0);
                U64(ct, 96) = (uintptr_t)fixtures[0]; U64(ct, 104) = (uintptr_t)fixtures[1 + rand() % (NB - 1)];
                if (rand() % 2) { uint64_t t = U64(ct, 96); U64(ct, 96) = U64(ct, 104); U64(ct, 104) = t; }
                U64(ct, 16 + 8) = (uintptr_t)ct;            // edge = ct + 16: contact at +8, next at +24
                U64(ct, 16 + 24) = prev; prev = ct + 16;
            }
            U64(bodies[0], 136) = prev;
            colfix[0] = (uintptr_t)fixtures[0]; U64(col, 88) = (uintptr_t)colfix; U64(col, 104) = 1;
            uint8_t want[NB][192];
            // a sync: walks, and between them contacts unlinked from the front (DestroyFixture) as Unity's rebuild does
            g_epoch++; g_sync_depth++;
            int k = 1 + rand() % 6;
            for (int w = 0; w < k; w++) {
                memcpy(want, bodies, sizeof bodies);
                uint8_t ours[NB][192];
                // Unity's on a copy of the bodies: run it on the live ones, then swap
                m_wake((uintptr_t)col); memcpy(ours, bodies, sizeof bodies);
                memcpy(bodies, want, sizeof bodies);
                wake_hook((uintptr_t)col); walks++;
                CHECK(!memcmp(ours, bodies, sizeof bodies), "wake mode %d run %d walk %d: bodies differ", mode, run, w);
                uintptr_t e = U64(bodies[0], 136);
                if (e && rand() % 2) U64(bodies[0], 136) = U64(e, 24);
            }
            g_sync_depth--; g_epoch++;
            // outside a sync, a body put to sleep: the next walk must wake it
            for (int b = 1; b < NB; b++) U16(bodies[b], 4) &= 0xfffd;
            memcpy(want, bodies, sizeof bodies);
            m_wake((uintptr_t)col);
            uint8_t ours2[NB][192]; memcpy(ours2, bodies, sizeof bodies); memcpy(bodies, want, sizeof bodies);
            wake_hook((uintptr_t)col);
            CHECK(!memcmp(ours2, bodies, sizeof bodies), "wake mode %d run %d: a walk after the sync differs", mode, run);
        }
        CHECK(c_wake_bad == 0, "wake mode %d: verify counted %llu changes in skipped walks", mode, (unsigned long long)c_wake_bad);
    }
    destroyfix_test();
    pool_test();
    printf("%s unityphys2d: %d failure(s) (%lu flags, %lu grows, %llu index rebuilds, %lu walks, %llu skipped)\n",
           fails ? "FAILED" : "PASSED", fails, flags, grows, (unsigned long long)c_rebuild, walks, (unsigned long long)c_wake_skipped);
    return fails != 0;
}
#endif
