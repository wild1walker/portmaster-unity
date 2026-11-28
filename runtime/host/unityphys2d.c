// unityphys2d.c: Unity 2017.4's 2D physics (Box2D inside libunity) without its per-collider scans (docs/runtime.md,
// UA_PHYS2D; docs/profiling.md, "Cuphead 64-bit ... the moved colliders").
//
//   UA_PHYS2D=1        the two scans below and b2Body::DestroyFixture's two walks (further down) replaced by exact
//                      cheaper ones
//   UA_PHYS2D=verify   the same, and every call checked against Unity's own scan on the live state
//   UA_PHYS2D=stats    (a measurement: Unity's code runs) calls a frame and how much each scan visits
//
// Where the time goes (Cuphead's run-and-gun level, muOS, 2017.4.40f1 arm64-v8a): a collider without a Rigidbody2D
// that a script moves is rebuilt whole at the next sync of transforms (PhysicsManager2D::SyncTransforms, which every
// Physics2D query runs first: autoSyncTransforms): Collider2D::RecreateCollider. Three of the steps of that rebuild
// walk something whose size is the whole scene's, not the collider's:
//
//  - PhysicsManager2D::FlagContactsForRecreate(this, collider) (from Collider2D::Cleanup) visits every bucket of
//    the manager's contact map (core::hash_map<pair<Collider2D*, Collider2D*>, Collision2D> at this + 72: buckets
//    +0, mask +8 (a byte offset / 7: (buckets - 1) * 16), count +12, empty buckets left +16; 112-byte buckets: hash
//    +0, -1 empty, -2 erased, the pair +8 / +16, the Collision2D +24) and in each occupied bucket whose pair holds the
//    collider stores 1 in the byte at +98 and 4 in the word at +56. Ours keeps an index from a collider to the
//    buckets that ever held it (filled where the map gains a pair: its operator[], called only by
//    PhysicsManager2D::BeginContact; built again from the buckets when the map's buckets, mask or count are not what
//    our hooks saw last: grow(), delete_nodes(), anything unseen) and applies Unity's test and stores to those
//    buckets only. Every bucket Unity's walk would change is in the collider's list; the stores are constants, so
//    the order does not matter; a bucket listed twice gets the same stores twice.
//
//  - Collider2D::WakeAllBodyContacts(collider) walks the contact list of the body the collider's first fixture is on
//    (every collider without a Rigidbody2D is on the world's one static ground body, so: every contact of every
//    such collider) and wakes both bodies of each contact (b2Body::SetAwake(true) inline; a static body: its awake
//    flag cleared, sleep time and velocities zeroed). SyncTransforms rebuilds its moved colliders one after another
//    and nothing in between can put a body to sleep, give a static body a velocity, or add a contact to a list
//    (contacts are linked only by b2ContactManager::AddPair, from the step's broad phase; no script runs inside the
//    sync): so after one walk of a body inside a sync, another walk of the same body inside the same sync stores
//    exactly what is already there. Ours walks a body once per sync (AddPair seen in between: again).
//    =verify walks each skipped one again and counts every store that would change a value.
//
// Rows: 2017.4.40f1 arm64-v8a (Cuphead 64-bit, Blasphemous; offsets, sizes and FNV-1a as unityhooks.c's, read from
// the player with its symbol file). Any other libunity.so is left alone (logged).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "unityphys2d.h"
#include "unityhooks.h"

enum { ISA_A64 = 'x' };
#define row uh_row

#if defined(__aarch64__)
enum { R_FLAG, R_WAKE, R_OPIDX, R_ERASE, R_DELNODES, R_SYNC, R_ADDPAIR, R_DESTROYFIX, R_CREATEFIX, R_CMDESTROY, R_PROXIES,
       R_FIXDESTROY, R_BLOCKFREE, R_RESETMASS, NROWS };
static const struct row g_rows[NROWS] = {
    { "2017.4.40f1 arm64-v8a", "PhysicsManager2D::FlagContactsForRecreate", 0x29a7ec, 0x88, ISA_A64, 0x2fad9854b95d4ea9ull },
    { "2017.4.40f1 arm64-v8a", "Collider2D::WakeAllBodyContacts", 0x2b446c, 0xac, ISA_A64, 0x529eaf67646acfdeull },
    { "2017.4.40f1 arm64-v8a", "core::hash_map<pair<Collider2D*, Collider2D*>, Collision2D>::operator[]", 0x295b58, 0x26c, ISA_A64, 0x5ee90d0830954c90ull },
    { "2017.4.40f1 arm64-v8a", "core::hash_map<pair<Collider2D*, Collider2D*>, Collision2D>::erase_node", 0x2ab5f0, 0x54, ISA_A64, 0x662f8bcc52e14e02ull },
    { "2017.4.40f1 arm64-v8a", "core::hash_map<pair<Collider2D*, Collider2D*>, Collision2D>::delete_nodes", 0x2a6e44, 0xa8, ISA_A64, 0x239f06fa6ff2167bull },
    { "2017.4.40f1 arm64-v8a", "PhysicsManager2D::SyncTransforms", 0x294b0c, 0x4bc, ISA_A64, 0x8bcb30fa75f16d8aull },
    { "2017.4.40f1 arm64-v8a", "b2ContactManager::AddPair", 0x1d4d80, 0x224, ISA_A64, 0xda152f71c7189df0ull },
    { "2017.4.40f1 arm64-v8a", "b2Body::DestroyFixture", 0x1d4250, 0x120, ISA_A64, 0x4de7d6894892cf34ull },
    { "2017.4.40f1 arm64-v8a", "b2Body::CreateFixture", 0x1d3fcc, 0x110, ISA_A64, 0xe37053dee9b1aa95ull },
    { "2017.4.40f1 arm64-v8a", "b2ContactManager::Destroy", 0x1d3ed4, 0xf8, ISA_A64, 0x8bec15b3b8bd1279ull },
    { "2017.4.40f1 arm64-v8a", "b2Fixture::DestroyProxies", 0x1d4370, 0x74, ISA_A64, 0x656bb5278c16afe2ull },
    { "2017.4.40f1 arm64-v8a", "b2Fixture::Destroy", 0x1d43e4, 0xf4, ISA_A64, 0x253aaac7a9b5c3b7ull },
    { "2017.4.40f1 arm64-v8a", "b2BlockAllocator::Free", 0x1d334c, 0x3c, ISA_A64, 0xddfc7db446353c20ull },
    { "2017.4.40f1 arm64-v8a", "b2Body::ResetMassData", 0x1d3b90, 0x28c, ISA_A64, 0x03a9cf4f3accfa51ull },
};

static int g_mode;                  // 1 on, 2 verify, 3 stats
static int g_on;                    // every hook in place: the hooks act (else each calls Unity's own)
static pthread_t g_main;            // the thread the 2D physics runs on (UnityMain): checked, counted otherwise

#define U64(p, o) (*(uint64_t volatile *)((uintptr_t)(p) + (o)))
#define U32(p, o) (*(uint32_t volatile *)((uintptr_t)(p) + (o)))
#define U16(p, o) (*(uint16_t volatile *)((uintptr_t)(p) + (o)))
#define U8(p, o) (*(uint8_t volatile *)((uintptr_t)(p) + (o)))

static void (*o_flag)(uintptr_t, uintptr_t);
static void (*o_wake)(uintptr_t);
static uintptr_t (*o_opidx)(uintptr_t, uintptr_t);
static void (*o_erase)(uintptr_t, uintptr_t);
static void (*o_delnodes)(uintptr_t);
static void (*o_sync)(uintptr_t);
static void (*o_addpair)(uintptr_t, uintptr_t, uintptr_t);
static void (*o_destroyfix)(uintptr_t, uintptr_t, int);
static uintptr_t (*o_createfix)(uintptr_t, uintptr_t, int);
static void (*o_cmdestroy)(uintptr_t, uintptr_t);
static void (*f_proxies)(uintptr_t, uintptr_t);
static void (*f_fixdestroy)(uintptr_t, uintptr_t);
static void (*f_blockfree)(uintptr_t, uintptr_t, int);
static void (*f_resetmass)(uintptr_t);

// counters (UnityMain only, plain adds)
static uint64_t c_flag, c_flag_buckets, c_flag_hits, c_flag_listed, c_rebuild, c_wake, c_wake_edges, c_wake_skipped,
    c_wake_bad, c_flag_bad, c_sync, c_insert, c_erase, c_destroyfix, c_df_steps, c_df_edges, c_other_thread,
    c_df_fast_unlink, c_df_no_contacts, c_df_bad, c_cnt_rebuild;

// --- the contact map index: collider -> buckets that ever held it
struct ix_head { uintptr_t key; uint32_t first; };
struct ix_node { uint32_t bucket, next; };
static struct ix_head *g_heads;     // open addressing, power of two
static uint32_t g_hcap, g_hused;
static struct ix_node *g_nodes;
static uint32_t g_ncap, g_nused;
static uintptr_t g_map;             // the map these describe (PhysicsManager2D + 72); 0 none
static uint64_t g_seen_buckets; static uint32_t g_seen_mask, g_seen_count;
static int g_ix_ok;

static inline uint32_t ptr_hash(uintptr_t k) { k ^= k >> 29; k *= 0xbf58476d1ce4e5b9ull; k ^= k >> 32; return (uint32_t)k; }

static void ix_clear(void)
{
    if (g_heads) memset(g_heads, 0, sizeof *g_heads * g_hcap);
    g_hused = 0; g_nused = 0; g_ix_ok = 0;
}

static int ix_grow_heads(void)
{
    uint32_t ncap = g_hcap ? g_hcap * 2 : 1024;
    struct ix_head *nh = calloc(ncap, sizeof *nh);
    if (!nh) return -1;
    for (uint32_t i = 0; i < g_hcap; i++) {
        if (!g_heads[i].key) continue;
        uint32_t h = ptr_hash(g_heads[i].key) & (ncap - 1);
        while (nh[h].key) h = (h + 1) & (ncap - 1);
        nh[h] = g_heads[i];
    }
    free(g_heads); g_heads = nh; g_hcap = ncap;
    return 0;
}

static int ix_add(uintptr_t key, uint32_t bucket)
{
    if (!key) return 0;
    if ((g_hused + 1) * 2 > g_hcap && ix_grow_heads()) return -1;
    if (g_nused == g_ncap) {
        uint32_t nc = g_ncap ? g_ncap * 2 : 4096;
        struct ix_node *nn = realloc(g_nodes, sizeof *nn * nc);
        if (!nn) return -1;
        g_nodes = nn; g_ncap = nc;
    }
    uint32_t h = ptr_hash(key) & (g_hcap - 1);
    while (g_heads[h].key && g_heads[h].key != key) h = (h + 1) & (g_hcap - 1);
    if (!g_heads[h].key) { g_heads[h].key = key; g_heads[h].first = UINT32_MAX; g_hused++; }
    struct ix_node *n = &g_nodes[g_nused];
    n->bucket = bucket; n->next = g_heads[h].first;
    g_heads[h].first = g_nused++;
    return 0;
}

static inline uint32_t ix_first(uintptr_t key)
{
    if (!g_hcap) return UINT32_MAX;
    uint32_t h = ptr_hash(key) & (g_hcap - 1);
    while (g_heads[h].key) {
        if (g_heads[h].key == key) return g_heads[h].first;
        h = (h + 1) & (g_hcap - 1);
    }
    return UINT32_MAX;
}

static void ix_seen(uintptr_t map)
{
    g_seen_buckets = U64(map, 0); g_seen_mask = U32(map, 8); g_seen_count = U32(map, 12);
}

// from the buckets themselves (what Unity's scan visits, once)
static void ix_rebuild(uintptr_t map)
{
    ix_clear();
    c_rebuild++;
    g_map = map;
    uintptr_t b = (uintptr_t)U64(map, 0);
    uint32_t nb = U32(map, 8) / 16 + 1;
    int ok = 1;
    for (uint32_t i = 0; i < nb && ok; i++) {
        uintptr_t k = b + 112ull * i;
        if (U32(k, 0) >= 0xfffffffeu) continue;
        ok = !ix_add((uintptr_t)U64(k, 8), i) && !ix_add((uintptr_t)U64(k, 16), i);
    }
    g_ix_ok = ok;
    ix_seen(map);
}

static int ix_current(uintptr_t map)
{
    return g_ix_ok && g_map == map && U64(map, 0) == g_seen_buckets && U32(map, 8) == g_seen_mask && U32(map, 12) == g_seen_count;
}

// Unity's scan, as read: the buckets in order, the occupied ones (hash below -2) holding the collider get the stores
static void flag_unity_scan(uintptr_t map, uintptr_t c, int count_only, uint64_t *visited, uint64_t *hits)
{
    uintptr_t b = (uintptr_t)U64(map, 0);
    uint32_t nb = U32(map, 8) / 16 + 1;
    for (uint32_t i = 0; i < nb; i++) {
        uintptr_t k = b + 112ull * i;
        (*visited)++;
        if (U32(k, 0) >= 0xfffffffeu) continue;
        if (U64(k, 8) == c || U64(k, 16) == c) {
            (*hits)++;
            if (!count_only) { U8(k, 98) = 1; U32(k, 56) = 4; }
        }
    }
}

static void flag_hook(uintptr_t self, uintptr_t c)
{
    if (!g_on) { o_flag(self, c); return; }
    uintptr_t map = self + 72;
    if (!pthread_equal(pthread_self(), g_main)) { c_other_thread++; o_flag(self, c); return; }
    c_flag++;
    if (g_mode == 3) {
        uint64_t v = 0, h = 0;
        flag_unity_scan(map, c, 1, &v, &h);
        c_flag_buckets += v; c_flag_hits += h;
        o_flag(self, c);
        return;
    }
    if (!ix_current(map)) ix_rebuild(map);
    if (!g_ix_ok) { o_flag(self, c); return; }
    uintptr_t b = (uintptr_t)U64(map, 0);
    uint64_t hits = 0;
    if (g_mode == 2) {          // which buckets Unity's would change: none may be left out of ours
        uint64_t v = 0, h = 0;
        flag_unity_scan(map, c, 1, &v, &h);
        uint32_t nb = U32(map, 8) / 16 + 1;
        for (uint32_t i = 0; i < nb; i++) {
            uintptr_t k = b + 112ull * i;
            if (U32(k, 0) >= 0xfffffffeu || (U64(k, 8) != c && U64(k, 16) != c)) continue;
            int found = 0;
            for (uint32_t n = ix_first(c); n != UINT32_MAX && !found; n = g_nodes[n].next) found = g_nodes[n].bucket == i;
            if (!found) c_flag_bad++;
        }
        c_flag_buckets += v; c_flag_hits += h;
    }
    for (uint32_t n = ix_first(c); n != UINT32_MAX; n = g_nodes[n].next) {
        uintptr_t k = b + 112ull * g_nodes[n].bucket;
        c_flag_listed++;
        if (U32(k, 0) >= 0xfffffffeu) continue;
        if (U64(k, 8) == c || U64(k, 16) == c) { U8(k, 98) = 1; U32(k, 56) = 4; hits++; }
    }
    (void)hits;
}

static uintptr_t opidx_hook(uintptr_t map, uintptr_t key)
{
    if (!g_on) return o_opidx(map, key);
    if (!pthread_equal(pthread_self(), g_main)) { c_other_thread++; g_ix_ok = 0; return o_opidx(map, key); }
    int cur = g_mode != 3 && ix_current(map);
    uint32_t before = U32(map, 12);
    uintptr_t v = o_opidx(map, key);
    if (g_mode == 3) return v;
    if (U32(map, 12) == before + 1) c_insert++;
    if (!cur) return v;                         // rebuilt at the next use
    if (U64(map, 0) != g_seen_buckets || U32(map, 8) != g_seen_mask) { g_ix_ok = 0; return v; }   // grown
    if (U32(map, 12) == before + 1) {           // a new pair in the bucket the value is in
        uint32_t i = (uint32_t)((v - 24 - (uintptr_t)U64(map, 0)) / 112);
        if (ix_add((uintptr_t)U64(v - 24, 8), i) || ix_add((uintptr_t)U64(v - 24, 16), i)) { g_ix_ok = 0; return v; }
        if (g_nused > 8 * (U32(map, 12) + 64)) { g_ix_ok = 0; return v; }   // stale entries: rebuild at the next use
        ix_seen(map);
    } else if (U32(map, 12) != before) g_ix_ok = 0;
    return v;
}

static void erase_hook(uintptr_t map, uintptr_t node)
{
    if (!g_on) { o_erase(map, node); return; }
    if (!pthread_equal(pthread_self(), g_main)) { c_other_thread++; g_ix_ok = 0; o_erase(map, node); return; }
    int cur = g_mode != 3 && ix_current(map);
    o_erase(map, node);
    c_erase++;
    if (cur) ix_seen(map);                      // the bucket stays listed: our test skips an erased one
}

static void delnodes_hook(uintptr_t map)
{
    g_ix_ok = 0;
    o_delnodes(map);
}

// --- the wake walk
static uint64_t g_epoch = 1;                   // a new value: a walk done before may not be the same any more
static int g_sync_depth;
struct wmemo { uintptr_t body; uint64_t epoch; };
static struct wmemo g_wm[16];
static unsigned g_wm_next;

static void sync_hook(uintptr_t self)
{
    if (!g_on) { o_sync(self); return; }
    if (!pthread_equal(pthread_self(), g_main)) { c_other_thread++; o_sync(self); return; }
    c_sync++;
    g_epoch++; g_sync_depth++;
    o_sync(self);
    g_sync_depth--; g_epoch++;
}

static void fx_contact(uintptr_t ct, int d);
static uintptr_t g_cm;              // the contact manager the counts describe (world + 0x192a0)
static uint32_t g_cnt_total;        // contacts seen made less those seen destroyed: must equal its m_contactCount (+96)
static int g_cnt_ok;

static void addpair_hook(uintptr_t cm, uintptr_t a, uintptr_t b)
{
    g_epoch++;
    uint32_t before = U32(cm, 96);
    o_addpair(cm, a, b);
    if (!g_on || g_mode == 3) return;
    if (cm != g_cm) { g_cnt_ok = 0; return; }
    if (U32(cm, 96) == before + 1) { fx_contact((uintptr_t)U64(cm, 88), 1); g_cnt_total++; }   // linked at the list's head
    else if (U32(cm, 96) != before) g_cnt_ok = 0;
}

// Unity's walk (2b446c), its loads and stores in its order; count_only: count the stores that would change a value
static uint64_t wake_walk(uintptr_t c, int count_only, uint64_t *edges)
{
    uint64_t changes = 0;
    if (!U64(c, 104)) return 0;
    uintptr_t body = (uintptr_t)U64(U64(U64(c, 88), 0), 32);
    if (!body) return 0;
    for (uintptr_t e = (uintptr_t)U64(body, 136); e; e = (uintptr_t)U64(e, 24)) {
        uintptr_t ct = (uintptr_t)U64(e, 8);
        (*edges)++;
        if (U8(ct, 8) & 0x40) continue;
        for (int s = 0; s < 2; s++) {
            uintptr_t bd = (uintptr_t)U64(U64(ct, s ? 104 : 96), 32);
            uint32_t type = U32(bd, 0);
            uint16_t fl = U16(bd, 4);
            if (type) {                         // SetAwake(true)
                if (fl & 2) continue;
                if (count_only) { changes++; continue; }
                U16(bd, 4) = (uint16_t)(fl | 2);
                U32(bd, 172) = 0;
            } else {                            // a static body: awake flag cleared, sleep time and velocities zeroed
                if (count_only) {
                    if ((fl & 2) || U32(bd, 172) || U64(bd, 64) || U64(bd, 72) || U64(bd, 80)) changes++;
                    continue;
                }
                U32(bd, 172) = 0;
                U64(bd, 72) = 0; U64(bd, 80) = 0;
                U16(bd, 4) = (uint16_t)(fl & 0xfffd);
                U64(bd, 64) = 0;
            }
        }
    }
    return changes;
}

static void wake_hook(uintptr_t c)
{
    if (!g_on) { o_wake(c); return; }
    if (!pthread_equal(pthread_self(), g_main)) { c_other_thread++; o_wake(c); return; }
    c_wake++;
    if (g_mode == 3) { uint64_t e = 0; wake_walk(c, 1, &e); c_wake_edges += e; o_wake(c); return; }
    uintptr_t body = U64(c, 104) ? (uintptr_t)U64(U64(U64(c, 88), 0), 32) : 0;
    if (body && g_sync_depth > 0) {
        for (unsigned i = 0; i < sizeof g_wm / sizeof *g_wm; i++)
            if (g_wm[i].body == body && g_wm[i].epoch == g_epoch) {
                c_wake_skipped++;
                if (g_mode == 2) { uint64_t e = 0; c_wake_bad += wake_walk(c, 1, &e); }
                return;
            }
    }
    uint64_t e = 0;
    wake_walk(c, 0, &e);
    c_wake_edges += e;
    if (body && g_sync_depth > 0) { g_wm[g_wm_next].body = body; g_wm[g_wm_next].epoch = g_epoch; g_wm_next = (g_wm_next + 1) % 16; }
}

// --- b2Body::DestroyFixture(body, fixture, resetMass) (Box2D as Unity 2017.4.40f1 has it, read from the player):
// unless the fixture is NULL or the world locked (byte world + 0x19298, bit 1; world = body + 88), the fixture is
// unlinked from the body's singly linked list (body + 112, next at fixture + 24), searched from the head; every
// contact on the body's edge list (body + 136; edge: contact +8, next +24) whose fixture A (+96) or B (+104) is the
// fixture is destroyed (b2ContactManager::Destroy, the manager at world + 0x192a0); then, the body active (byte +4
// bit 5), the fixture's proxies (b2Fixture::DestroyProxies(fixture, world + 0x192a0)); the fixture's next and body
// zeroed, b2Fixture::Destroy(fixture, world), b2BlockAllocator::Free(world, fixture, 88), the body's fixture count
// (+120) less one, and ResetMassData(body) when asked. Every collider without a Rigidbody2D is on the ground body:
// its list holds every such fixture and its edge list every contact any of them has.
// Ours: the same, but the fixture's predecessor is kept (a map, filled where b2Body::CreateFixture links a fixture at
// a list's head and where ours unlinks one) and checked before use (its next is the fixture and its body the body,
// or the fixture is the head), else Unity's search; and the edge walk is left out where the fixture has no contact:
// a count per fixture kept where contacts are made (b2ContactManager::AddPair, the only place that links one) and
// destroyed (b2ContactManager::Destroy), used only while the counts add up to the manager's m_contactCount (+96).
// A fixture with contacts gets Unity's walk, in its order. =verify walks the list and the edges Unity's way too and
// counts every difference (the predecessor, a contact that would have been destroyed).
struct fxe { uintptr_t key, prev; uint32_t ncont, has_prev; };
static struct fxe *g_fx;
static uint32_t g_fxcap, g_fxused;

static struct fxe *fx_find(uintptr_t k, int add)
{
    if (!k) return NULL;
    if (add && (g_fxused + 1) * 2 > g_fxcap) {
        uint32_t nc = g_fxcap ? g_fxcap * 2 : 4096;
        struct fxe *n = calloc(nc, sizeof *n);
        if (!n) return NULL;
        for (uint32_t i = 0; i < g_fxcap; i++) if (g_fx[i].key) {
            uint32_t h = ptr_hash(g_fx[i].key) & (nc - 1);
            while (n[h].key) h = (h + 1) & (nc - 1);
            n[h] = g_fx[i];
        }
        free(g_fx); g_fx = n; g_fxcap = nc;
    }
    if (!g_fxcap) return NULL;
    uint32_t h = ptr_hash(k) & (g_fxcap - 1);
    while (g_fx[h].key) { if (g_fx[h].key == k) return &g_fx[h]; h = (h + 1) & (g_fxcap - 1); }
    if (!add) return NULL;
    g_fx[h].key = k; g_fx[h].prev = 0; g_fx[h].ncont = 0; g_fx[h].has_prev = 0; g_fxused++;
    return &g_fx[h];
}
static void fx_contact(uintptr_t ct, int d)
{
    for (int s = 0; s < 2; s++) {
        struct fxe *e = fx_find((uintptr_t)U64(ct, s ? 104 : 96), 1);
        if (!e) { g_cnt_ok = 0; return; }
        e->ncont += (uint32_t)d;
    }
}
// the counts again from the manager's list of every contact (prev +16, next +24)
static void cnt_rebuild(uintptr_t cm)
{
    for (uint32_t i = 0; i < g_fxcap; i++) g_fx[i].ncont = 0;
    g_cm = cm; g_cnt_total = 0; g_cnt_ok = 1; c_cnt_rebuild++;
    for (uintptr_t c = (uintptr_t)U64(cm, 88); c && g_cnt_ok; c = (uintptr_t)U64(c, 24)) { fx_contact(c, 1); g_cnt_total++; }
}

static uintptr_t createfix_hook(uintptr_t body, uintptr_t def, int reset)
{
    uintptr_t f = o_createfix(body, def, reset);
    if (!g_on || g_mode == 3 || !f || !pthread_equal(pthread_self(), g_main)) return f;
    struct fxe *e = fx_find(f, 1);
    if (e) { e->prev = 0; e->has_prev = 1; e->ncont = 0; }
    uintptr_t n = (uintptr_t)U64(f, 24);
    if (n && (e = fx_find(n, 1))) { e->prev = f; e->has_prev = 1; }
    return f;
}

static void cmdestroy_hook(uintptr_t cm, uintptr_t ct)
{
    if (g_on && g_mode != 3 && cm == g_cm && g_cnt_ok) { fx_contact(ct, -1); g_cnt_total--; }
    o_cmdestroy(cm, ct);
}

static void destroyfix_hook(uintptr_t body, uintptr_t fix, int reset)
{
    if (!g_on) { o_destroyfix(body, fix, reset); return; }
    if (!pthread_equal(pthread_self(), g_main)) { c_other_thread++; o_destroyfix(body, fix, reset); return; }
    if (g_mode == 3) {
        if (fix) {
            c_destroyfix++;
            for (uintptr_t f = (uintptr_t)U64(body, 112); f && f != fix; f = (uintptr_t)U64(f, 24)) c_df_steps++;
            for (uintptr_t e = (uintptr_t)U64(body, 136); e; e = (uintptr_t)U64(e, 24)) c_df_edges++;
        }
        o_destroyfix(body, fix, reset);
        return;
    }
    if (!fix) return;
    uintptr_t world = (uintptr_t)U64(body, 88);
    if (U8(world, 0x19298) & 2) return;
    c_destroyfix++;
    uintptr_t cm = world + 0x192a0;
    // the unlink
    struct fxe *e = fx_find(fix, 0);
    uintptr_t pred = 0; int found = 0;
    if (e && e->has_prev) {
        uintptr_t p = e->prev;
        if (p ? (U64(p, 24) == fix && U64(p, 32) == body) : U64(body, 112) == fix) { pred = p; found = 1; c_df_fast_unlink++; }
    }
    if (g_mode == 2 || !found) {             // Unity's search
        uintptr_t p = 0; int f2 = 0;
        for (uintptr_t f = (uintptr_t)U64(body, 112); f; p = f, f = (uintptr_t)U64(f, 24)) {
            c_df_steps++;
            if (f == fix) { f2 = 1; break; }
        }
        if (found && (!f2 || p != pred)) c_df_bad++;
        if (!found) { pred = p; found = f2; }
    }
    uintptr_t next = (uintptr_t)U64(fix, 24);
    if (found) {
        if (pred) U64(pred, 24) = next; else U64(body, 112) = next;
        struct fxe *ne = next ? fx_find(next, 1) : NULL;
        if (ne) { ne->prev = pred; ne->has_prev = 1; }
    }
    // the contacts
    int skip = 0;
    if (cm != g_cm || !g_cnt_ok || g_cnt_total != U32(cm, 96)) cnt_rebuild(cm);
    if (g_cnt_ok && g_cnt_total == U32(cm, 96)) {
        struct fxe *fe = fx_find(fix, 0);
        skip = !fe || fe->ncont == 0;
    }
    if (skip) {
        c_df_no_contacts++;
        if (g_mode == 2)
            for (uintptr_t ed = (uintptr_t)U64(body, 136); ed; ed = (uintptr_t)U64(ed, 24)) {
                uintptr_t c = (uintptr_t)U64(ed, 8);
                if (U64(c, 96) == fix || U64(c, 104) == fix) c_df_bad++;
            }
    } else {
        uintptr_t ed = (uintptr_t)U64(body, 136);
        while (ed) {
            uintptr_t c = (uintptr_t)U64(ed, 8);
            ed = (uintptr_t)U64(ed, 24);
            c_df_edges++;
            if (U64(c, 96) == fix || U64(c, 104) == fix) cmdestroy_hook(cm, c);
        }
    }
    if (U8(body, 4) & 0x20) f_proxies(fix, cm);
    U64(fix, 24) = 0; U64(fix, 32) = 0;
    f_fixdestroy(fix, world);
    f_blockfree(world, fix, 88);
    if ((e = fx_find(fix, 0))) e->has_prev = 0;           // (gone; its entry is made again at its next CreateFixture)
    U32(body, 120) = U32(body, 120) - 1;
    if (reset & 1) f_resetmass(body);
}

static uintptr_t g_base;
static int g_ok[NROWS];

void unityphys2d_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_PHYS2D");
    if (!v || !*v || !strcmp(v, "0")) return;
    g_mode = !strcmp(v, "verify") ? 2 : !strcmp(v, "stats") ? 3 : 1;
    int ok = 1;
    for (int i = 0; i < NROWS; i++) {
        g_ok[i] = unityhooks_matches(&g_rows[i], base, size);
        ok = ok && g_ok[i];
    }
    if (!ok) {
        fprintf(stderr, "[unityphys2d] UA_PHYS2D: this libunity.so is not one whose 2D physics was read; left as Unity's\n");
        g_mode = 0;
        return;
    }
    g_base = base;
}

void unityphys2d_on_load_patch(uintptr_t base, size_t size)
{
    (void)size;
    if (!g_mode) return;
    g_main = pthread_self();                    // replaced at UnityMain's first frame (unityphys2d_frame)
    struct { int r; void *to; void **orig; } h[] = {
        { R_FLAG, (void *)flag_hook, (void **)&o_flag }, { R_WAKE, (void *)wake_hook, (void **)&o_wake },
        { R_OPIDX, (void *)opidx_hook, (void **)&o_opidx }, { R_ERASE, (void *)erase_hook, (void **)&o_erase },
        { R_DELNODES, (void *)delnodes_hook, (void **)&o_delnodes }, { R_SYNC, (void *)sync_hook, (void **)&o_sync },
        { R_ADDPAIR, (void *)addpair_hook, (void **)&o_addpair }, { R_CREATEFIX, (void *)createfix_hook, (void **)&o_createfix },
        { R_CMDESTROY, (void *)cmdestroy_hook, (void **)&o_cmdestroy }, { R_DESTROYFIX, (void *)destroyfix_hook, (void **)&o_destroyfix },
    };
    f_proxies = (void (*)(uintptr_t, uintptr_t))(base + g_rows[R_PROXIES].off);
    f_fixdestroy = (void (*)(uintptr_t, uintptr_t))(base + g_rows[R_FIXDESTROY].off);
    f_blockfree = (void (*)(uintptr_t, uintptr_t, int))(base + g_rows[R_BLOCKFREE].off);
    f_resetmass = (void (*)(uintptr_t))(base + g_rows[R_RESETMASS].off);
    for (size_t i = 0; i < sizeof h / sizeof *h; i++) {
        if (unityhooks_hook(base + g_rows[h[i].r].off, ISA_A64, h[i].to, h[i].orig) != 0) {
            fprintf(stderr, "[unityphys2d] %s: hook failed; UA_PHYS2D off (the hooks made call Unity's own)\n", g_rows[h[i].r].func);
            g_mode = 0;
            return;
        }
    }
    g_on = 1;
    fprintf(stderr, "[unityphys2d] UA_PHYS2D=%s: FlagContactsForRecreate, WakeAllBodyContacts and b2Body::DestroyFixture %s (libunity %s)\n",
            g_mode == 2 ? "verify" : g_mode == 3 ? "stats" : "1", g_mode == 3 ? "counted" : "replaced", g_rows[0].unity);
}

void unityphys2d_frame(void)
{
    static unsigned long frames, f0;
    if (!g_mode) return;
    if (!frames++) { g_main = pthread_self(); f0 = 1; return; }
    if (frames % 600) return;
    double nf = (double)(frames - f0);
    f0 = frames;
    fprintf(stderr, "[unityphys2d] %s a frame (%.0f frames): syncs %.1f; flag %.1f calls, %.0f buckets Unity's scans visit, %.1f hits, "
            "%.1f listed (ours), %llu rebuilds, %llu missed; wake %.1f calls, %.0f contacts walked, %.1f skipped, %llu would change; "
            "pairs +%.1f -%.1f; DestroyFixture %.1f calls, %.0f list steps, %.0f contacts walked, %.1f unlinked by the map, %.1f without "
            "contacts, %llu count rebuilds, %llu differ; %llu calls off UnityMain\n",
            g_mode == 2 ? "verify" : g_mode == 3 ? "stats" : "on", nf, c_sync / nf, c_flag / nf, c_flag_buckets / nf, c_flag_hits / nf,
            c_flag_listed / nf, (unsigned long long)c_rebuild, (unsigned long long)c_flag_bad, c_wake / nf, c_wake_edges / nf,
            c_wake_skipped / nf, (unsigned long long)c_wake_bad, c_insert / nf, c_erase / nf, c_destroyfix / nf, c_df_steps / nf,
            c_df_edges / nf, c_df_fast_unlink / nf, c_df_no_contacts / nf, (unsigned long long)c_cnt_rebuild,
            (unsigned long long)c_df_bad, (unsigned long long)c_other_thread);
    c_sync = c_flag = c_flag_buckets = c_flag_hits = c_flag_listed = c_wake = c_wake_edges = c_wake_skipped = 0;
    c_insert = c_erase = c_destroyfix = c_df_steps = c_df_edges = c_df_fast_unlink = c_df_no_contacts = 0;
}
#else
void unityphys2d_on_load(uintptr_t base, size_t size) { (void)base; (void)size; }
void unityphys2d_on_load_patch(uintptr_t base, size_t size) { (void)base; (void)size; }
void unityphys2d_frame(void) {}
#endif
