// unityscripts.c's unit test: the kept answers against a model of Unity's script lookup. The model: a sorted vector
// of instance IDs, a map of loaded objects (Unity's lookup: a 12-byte node {hash, key, object} or the end node),
// scripts with a class at [[s + 0x50] + 4], and a walk that, as FindRuntimeScript, takes the first loaded script of
// the class, loading an entry that is not loaded (when it can: its class is then set through Renew). Checked: every
// answer the hook gives equals the walk's, over lookups mixed with a script's end, a script loaded, a class changed,
// an insert into the vector, a vector moved; answers are kept only on UnityMain, never before the first frame, never
// for a class with an entry not loaded before its own; the destructor and Renew hooks pass their result on.
// Run by unityhooks_test.sh (32-bit only, as the player's layout).
#include <stdio.h>
#include <string.h>

#include "../unityscripts.c"

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }

#if UINTPTR_MAX > 0xffffffffu
int main(void) { printf("SKIPPED unityscripts: the ARMv7 player's layout (32-bit only)\n"); return 0; }
#else
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// the model's objects: script k has instance ID 100 + k, class klass[k], loaded[k], loadable[k]
#define NS 12
static struct script { uint8_t pad[0x50]; uintptr_t cache; uint8_t rest[0x10]; } scripts[NS];
static struct cache { uintptr_t pad, klass; } caches[NS];
static int is_loaded[NS], loadable[NS];
static uintptr_t klass_of[NS];
static int32_t vec[64]; static int nvec;
static int32_t vec2[64];
static uintptr_t mgr[2];
// the map: lookup returns &nodes[k] for a loaded script, else the end node
static struct node { uint32_t hash; int32_t key; uintptr_t obj; } nodes[NS + 1];
static uintptr_t mapmem[2];
static uintptr_t mapvar;
static int walks;

static int index_of(int32_t id) { return id >= 100 && id < 100 + NS ? id - 100 : -1; }
static uintptr_t f_lookup(uintptr_t map, const int32_t *key)
{
    (void)map;
    int k = index_of(*key);
    return k >= 0 && is_loaded[k] ? (uintptr_t)&nodes[k] : (uintptr_t)&nodes[NS];
}
static void set_class(int k, uintptr_t kl) { klass_of[k] = kl; caches[k].klass = kl; scripts[k].cache = (uintptr_t)&caches[k]; }
static uintptr_t f_renew(uintptr_t s, uintptr_t kl) { (void)s; (void)kl; return 0x5eed; }
static uintptr_t f_dtor(uintptr_t s) { return s; }
// Unity's walk: dereference every entry (loading what can be loaded: its class through Renew), the first of the class
static uintptr_t f_find(uintptr_t m, uintptr_t kl)
{
    walks++;
    const int32_t *b = (const int32_t *)((uintptr_t *)m)[0], *e = (const int32_t *)((uintptr_t *)m)[1];
    for (; b < e; b++) {
        int k = index_of(*b);
        if (k < 0) continue;
        if (!is_loaded[k]) {
            if (!loadable[k]) continue;
            is_loaded[k] = 1;
            renew_hook((uintptr_t)&scripts[k], klass_of[k]);
        }
        if (klass_of[k] == kl) return (uintptr_t)&scripts[k];
    }
    return 0;
}
static uintptr_t truth(uintptr_t kl)
{
    // what the walk would answer, without its loads (the loadable ones count as loaded)
    for (int i = 0; i < nvec; i++) {
        int k = index_of(vec[i]);
        if (k >= 0 && (is_loaded[k] || loadable[k]) && klass_of[k] == kl) return (uintptr_t)&scripts[k];
    }
    return 0;
}
static void set_vec(const int *ks, int n, int32_t *store)
{
    nvec = n;
    for (int i = 0; i < n; i++) store[i] = 100 + ks[i];
    mgr[0] = (uintptr_t)store; mgr[1] = (uintptr_t)(store + n);
}

int main(void)
{
    for (int k = 0; k < NS; k++) {
        set_class(k, 0x1000 + 0x10 * (k % 5));      // classes 0x1000..0x1040, several scripts of each
        is_loaded[k] = 1; loadable[k] = 1;
        uint32_t id = 100 + k; memcpy((uint8_t *)&scripts[k] + 4, &id, 4);
        nodes[k].key = 100 + k; nodes[k].obj = (uintptr_t)&scripts[k];
    }
    // the map: [0] the nodes, [4] such that [0] + 3 * [4] + 12 is &nodes[NS]
    mapmem[0] = (uintptr_t)&nodes[0]; mapmem[1] = (uintptr_t)(sizeof(struct node) * NS - 12) / 3;
    mapvar = (uintptr_t)mapmem;
    uintptr_t slotvar = (uintptr_t)&mapvar;
    g_idmap_slot = (uintptr_t)&slotvar;
    p_lookup = f_lookup; o_find = f_find; o_renew = f_renew; o_dtor = f_dtor;
    int order[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    set_vec(order, 10, vec);

    // before the first frame: always Unity's walk
    walks = 0;
    find_hook((uintptr_t)mgr, 0x1010);
    CHECK(walks == 1 && g_nsent == 0, "an answer was kept before the first frame");
    unityscripts_frame();                       // UnityMain known from here
    // off UnityMain: always the walk
    g_main_tid = tid() + 1;
    walks = 0; find_hook((uintptr_t)mgr, 0x1010); find_hook((uintptr_t)mgr, 0x1010);
    CHECK(walks == 2 && g_nsent == 0, "an answer was kept off UnityMain");
    g_main_tid = tid();
    // kept: the second lookup of a class makes no walk, with the same answer
    walks = 0;
    uintptr_t a1 = find_hook((uintptr_t)mgr, 0x1010), a2 = find_hook((uintptr_t)mgr, 0x1010);
    CHECK(a1 == truth(0x1010) && a2 == a1 && walks == 1, "kept answer (walks %d)", walks);
    CHECK(find_hook((uintptr_t)mgr, 0x9999) == 0, "a class with no script");
    // a long run of lookups mixed with changes: every answer equals the model's
    unsigned rnd = 12345;
    for (int step = 0; step < 4000; step++) {
        rnd = rnd * 1103515245u + 12345u;
        int what = (rnd >> 16) % 23;
        int k = (rnd >> 8) % NS;
        if (what == 0) { if (is_loaded[k]) { dtor_hook((uintptr_t)&scripts[k]); is_loaded[k] = 0; loadable[k] = (rnd >> 4) & 1; } }
        else if (what == 1) { if (!is_loaded[k] && loadable[k]) { is_loaded[k] = 1; renew_hook((uintptr_t)&scripts[k], klass_of[k]); } }
        else if (what == 2) { set_class(k, 0x1000 + 0x10 * ((rnd >> 12) % 5)); renew_hook((uintptr_t)&scripts[k], klass_of[k]); }
        else if (what == 3 && nvec < 12) {        // a script inserted in order, in place
            int ks[12], n = 0;
            for (int i = 0; i < nvec; i++) ks[n++] = index_of(vec[i]);
            int add = 10 + ((rnd >> 3) & 1);
            int dup = 0; for (int i = 0; i < n; i++) if (ks[i] == add) dup = 1;
            if (!dup) { ks[n++] = add; for (int i = n - 1; i > 0 && ks[i] < ks[i - 1]; i--) { int t = ks[i]; ks[i] = ks[i - 1]; ks[i - 1] = t; } set_vec(ks, n, vec); }
        } else if (what == 4) {                   // the vector moved (same content, other storage)
            int32_t *dst = (int32_t *)mgr[0] == vec ? vec2 : vec;
            int ks[12]; for (int i = 0; i < nvec; i++) ks[i] = index_of(((int32_t *)mgr[0])[i]);
            set_vec(ks, nvec, dst);
        } else {
            uintptr_t kl = 0x1000 + 0x10 * ((rnd >> 20) % 6);       // 0x1050: no script
            uintptr_t want = truth(kl), got = find_hook((uintptr_t)mgr, kl);
            CHECK(got == want, "step %d class %#x: %#x, the walk %#x", step, (unsigned)kl, (unsigned)got, (unsigned)want);
        }
    }
    // an entry not loaded before the class's own: never kept
    forget();
    for (int k = 0; k < NS; k++) { is_loaded[k] = 1; loadable[k] = 1; set_class(k, 0x1000 + 0x10 * (k % 5)); }
    set_vec(order, 10, vec);
    is_loaded[1] = 0; loadable[1] = 0;
    walks = 0; find_hook((uintptr_t)mgr, 0x1020); find_hook((uintptr_t)mgr, 0x1020);
    CHECK(walks == 2, "a class with an unloaded entry before it was kept (walks %d)", walks);
    // the hooks pass their results on
    CHECK(renew_hook(1, 2) == 0x5eed && dtor_hook(0x777) == 0x777, "the hooks' results");
    printf("%s unityscripts: %d failure(s) (%lu kept answers, %lu walks)\n", fails ? "FAILED" : "PASSED", fails, g_hits, g_misses);
    return fails != 0;
}
#endif
