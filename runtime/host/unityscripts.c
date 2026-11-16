// unityscripts.c: the script lookup of AddComponent and of serialization without its linear walk (UA_SCRIPT_FIND=1;
// UA_SCRIPT_FIND=check; off by default).
//
// GameObject.AddComponent<T>() (and AddComponent(Type)) asks MonoScriptManager::FindRuntimeScript(class) for the
// MonoScript of T's class: one to three times a call (the component, then CollectComponentsWithoutAdding and
// AddComponentUnchecked again), as do serialization (ShouldTransferField) and MonoBehaviour::SetClass. The lookup
// walks the manager's sorted vector of every script (PPtr<MonoScript>, one instance ID each) from its start and
// dereferences each entry twice (Object::IDToPointer: a hash lookup, a cache miss on the hash table and the object)
// until a loaded script whose class is T's: in Enter the Gungeon's fights 0.25 - 0.34 ms a frame on the RG34XX-SP
// (decals and pooled objects get components added while bullets fly).
//
// The hook keeps, on UnityMain, each class's answer with what it depended on, and returns it again when nothing it
// depended on can have changed:
//   - the vector's storage and every one of its instance IDs (compared word for word with the copy taken then: an
//     insert of a script, sorted by its ID, changes them);
//   - no MonoScript left the map of loaded objects or ended and none had its class set since (the map's erase,
//     MonoScript::~MonoScript and MonoScript::Renew, which every class change goes through, are hooked to count
//     them; any count empties the kept answers);
//   - the answer's own entry is still the same loaded script, with T's class (Unity's own lookup of loaded objects).
// At the time an answer is kept every entry before it is a loaded script of another class (checked then with Unity's
// lookup of loaded objects, which never loads; a class with an entry not loaded before its own is not kept), so with
// all of the above unchanged Unity's walk would pass those entries in memory (no loads: they are loaded, none ended,
// a script that loads gets its class through Renew, which empties the answers) and stop at the same entry.
// The first lookup of each class, any lookup off UnityMain and every lookup after a change take Unity's own walk.
// UA_SCRIPT_FIND=check: Unity's walk every time as well, its answer returned, a kept answer that differs counted
// ([scripts] lines every 10 s).
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "unityscripts.h"
#include "unityhooks.h"

struct scripts_player {
    struct uh_row find, renew, dtor, erase; // hooked
    struct uh_row dtor0, deref, lookup; // checked: D0 calls D2; the dereference whose in-memory half is used; its lookup
    uint32_t idmap_got;                 // the GOT slot the dereference reads the instance-ID map's address from
    uint32_t script_vtable;             // MonoScript's vtable as objects point to it (its symbol + 8)
};
// 2017.4.27f1 armeabi-v7a: the manager's vector at +0 (begin) / +4 (end) of instance IDs; a script's class at
// [[script + 0x50] + 4] (FindRuntimeScript's own test); an Object's instance ID at +4. PPtr<MonoScript>'s dereference:
// the map of loaded objects from its GOT slot (0x11189ac: its literal, in the row's bytes), the map's lookup (a node
// {hash, key, object} of 12 bytes; the end node at [map] + 3 * [map + 4] + 12), the object when found, else a load
// from disk (ReadObjectFromPersistentManager): the hook reads loaded objects only, as the dereference's first half
static const struct scripts_player g_players[] = {
    {
        { "2017.4.27f1 armeabi-v7a", "MonoScriptManager::FindRuntimeScript(ScriptingClassPtr)", 0x734564, 0x80, 'a', 0x00576544f4df1f4bull },
        { "2017.4.27f1 armeabi-v7a", "MonoScript::Renew", 0x7324cc, 0x54, 'a', 0x99484e594176ccbbull },
        { "2017.4.27f1 armeabi-v7a", "MonoScript::~MonoScript (D2)", 0x734f10, 0x7c, 'a', 0xb5c002109d6d2114ull },
        { "2017.4.27f1 armeabi-v7a", "core::hash_map<int, Object*>::erase", 0x3e6e84, 0x44, 'a', 0x3878f393d5095d5aull },
        { "2017.4.27f1 armeabi-v7a", "MonoScript::~MonoScript (D0)", 0x734f8c, 0x10, 'a', 0xd22ce6c564dd0034ull },
        { "2017.4.27f1 armeabi-v7a", "PPtr<MonoScript>::operator MonoScript*", 0x35d5b0, 0x70, 'a', 0xe1c553cd6a209317ull },
        { "2017.4.27f1 armeabi-v7a", "core::hash_map<int, Object*>::lookup", 0xe825c, 0xe0, 'a', 0x75d44a4502e9a5aaull },
        0x11189ac, 0x1108ed4,
    },
};

static uintptr_t (*o_find)(uintptr_t mgr, uintptr_t klass);
// (what they return is passed on: the ARM C++ ABI's destructors return `this`, which the deleting destructor hands
// to operator delete)
static uintptr_t (*o_renew)(uintptr_t script, uintptr_t klass);
static uintptr_t (*o_dtor)(uintptr_t script);
static uintptr_t (*o_erase)(uintptr_t map, const int32_t *key);
static uintptr_t g_vt_script;
static uintptr_t (*p_lookup)(uintptr_t map, const int32_t *key);
static uintptr_t g_idmap_slot;          // the GOT slot of the variable that holds the map of loaded objects

// the object with this instance ID if it is loaded, else 0 (never a load)
static uintptr_t loaded(int32_t id)
{
    if (!id) return 0;
    uintptr_t var = *(const uintptr_t *)g_idmap_slot;          // the GOT slot: the variable's address
    uintptr_t map = var ? *(const uintptr_t *)var : 0;         // the variable: the map
    if (!map) return 0;
    uintptr_t node = p_lookup(map, &id);
    uintptr_t end = *(const uintptr_t *)map + 3 * *(const uintptr_t *)(map + 4) + 12;
    return node != end ? *(const uintptr_t *)(node + 8) : 0;
}
static atomic_uint g_changes;          // scripts ended or given a class since start
static int g_check;
static pid_t g_main_tid;

// the kept answers, one copy of the vector they were taken with
struct sent { uintptr_t klass, script; uint32_t idx; };
#define NSENT 512
static struct sent g_sent[NSENT];
static unsigned g_nsent;
static int32_t *g_copy;
static size_t g_copy_n, g_copy_cap;
static uintptr_t g_copy_begin;
static unsigned g_copy_changes;
static unsigned long g_hits, g_misses, g_wrong, g_calls, g_compared;

static inline uintptr_t rd(uintptr_t a) { return *(const uintptr_t *)a; }
static uintptr_t script_class(uintptr_t s) { uintptr_t c = rd(s + 0x50); return c ? rd(c + 4) : 0; }

// (the count of changes it was emptied at: emptied once per change, not at every lookup until an answer is kept)
static void forget(void)
{
    memset(g_sent, 0, sizeof g_sent);
    g_nsent = 0; g_copy_n = 0; g_copy_begin = 0;
    g_copy_changes = atomic_load(&g_changes);
}

// the vector as it was when the answers were kept: the same storage, the same IDs
static int vector_same(const int32_t *b, const int32_t *e)
{
    size_t n = (size_t)(e - b);
    return g_copy_begin == (uintptr_t)b && g_copy_n == n && !memcmp(g_copy, b, n * sizeof *b);
}

static struct sent *slot(uintptr_t klass, int add)
{
    unsigned h = ((uint32_t)(klass >> 3) * 2654435761u) >> 23;
    for (unsigned i = 0; i < NSENT; i++) {
        struct sent *e = &g_sent[(h + i) & (NSENT - 1)];
        if (e->klass == klass) return e;
        if (!e->klass) {
            if (!add || g_nsent >= NSENT * 3 / 4) return NULL;
            g_nsent++;
            e->klass = klass;
            return e;
        }
    }
    return NULL;
}

// after Unity's walk returned s for klass: keep it if every entry before it is a loaded script of another class
static void keep(uintptr_t mgr, uintptr_t klass, uintptr_t s)
{
    const int32_t *b = (const int32_t *)rd(mgr), *e = (const int32_t *)rd(mgr + 4);
    if (!s || !b || e < b) return;
    size_t n = (size_t)(e - b);
    if (!vector_same(b, e)) {
        forget();
        if (n > g_copy_cap) {
            int32_t *c = realloc(g_copy, n * sizeof *c);
            if (!c) return;
            g_copy = c; g_copy_cap = n;
        }
        memcpy(g_copy, b, n * sizeof *b);
        g_copy_n = n; g_copy_begin = (uintptr_t)b; g_copy_changes = atomic_load(&g_changes);
    }
    int32_t id = *(const int32_t *)(s + 4);
    size_t idx = 0;
    while (idx < n && b[idx] != id) idx++;
    if (idx == n) return;
    for (size_t i = 0; i < idx; i++) {
        uintptr_t o = loaded(b[i]);
        if (!o || script_class(o) == klass) return;            // an entry not loaded (the walk loads or tries it), or a duplicate
    }
    struct sent *t = slot(klass, 1);
    if (!t) return;
    t->script = s; t->idx = (uint32_t)idx;
}

static pid_t tid(void)
{
    static __thread pid_t t;
    if (!t) t = (pid_t)syscall(SYS_gettid);
    return t;
}

static uintptr_t find_hook(uintptr_t mgr, uintptr_t klass)
{
    if (!g_main_tid || tid() != g_main_tid) return o_find(mgr, klass);
    g_calls++;
    uintptr_t kept = 0;
    const int32_t *b = (const int32_t *)rd(mgr), *e = (const int32_t *)rd(mgr + 4);
    if (g_copy_changes != atomic_load(&g_changes)) forget();
    struct sent *t = g_nsent ? slot(klass, 0) : NULL;
    if (t && t->script && vector_same(b, e)) {
        uintptr_t o = loaded(b[t->idx]);
        if (o == t->script && script_class(o) == klass) kept = o;
    }
    if (kept && !g_check) { g_hits++; return kept; }
    uintptr_t r = o_find(mgr, klass);
    if (kept) { g_hits++; g_compared++; if (r != kept) g_wrong++; return r; }
    g_misses++;
    if (r) keep(mgr, klass, r);
    return r;
}

static uintptr_t renew_hook(uintptr_t script, uintptr_t klass) { atomic_fetch_add(&g_changes, 1); return o_renew(script, klass); }
static uintptr_t dtor_hook(uintptr_t script) { atomic_fetch_add(&g_changes, 1); return o_dtor(script); }
// an object leaving the map of loaded objects (delete_object_internal_step1, before its destructor, which may run
// later on another thread): a MonoScript's ID no longer resolves from here on
static uintptr_t erase_hook(uintptr_t map, const int32_t *key)
{
    uintptr_t node = p_lookup(map, key);
    uintptr_t end = rd(map) + 3 * rd(map + 4) + 12;
    if (node != end) {
        uintptr_t o = rd(node + 8);
        if (o && rd(o) == g_vt_script) atomic_fetch_add(&g_changes, 1);
    }
    return o_erase(map, key);
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

void unityscripts_frame(void)
{
    if (!o_find) return;
    if (!g_main_tid) g_main_tid = tid();
    static double next;
    static unsigned frames;
    frames++;
    double t = now();
    if (!next) { next = t + 10; return; }
    if (t < next) return;
    next = t + 10;
    fprintf(stderr, "[scripts] %u frames: FindRuntimeScript on UnityMain %.1f a frame, %lu from the kept answers, %lu walks%s; "
                    "%u classes kept, %u script changes since start\n", frames, g_calls / (double)(frames ? frames : 1), g_hits, g_misses,
            g_check ? (g_wrong ? " (CHECK: some kept answers differ)" : " (check: every kept answer the same as Unity's)") : "",
            g_nsent, atomic_load(&g_changes));
    if (g_check) fprintf(stderr, "[scripts] check: %lu kept answers compared since start, %lu wrong\n", g_compared, g_wrong);
    g_calls = g_hits = g_misses = 0;
    frames = 0;
}

void unityscripts_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_SCRIPT_FIND");
    if (!v || (strcmp(v, "1") && strcmp(v, "check"))) return;
    g_check = !strcmp(v, "check");
    for (size_t i = 0; i < sizeof g_players / sizeof *g_players; i++) {
        const struct scripts_player *p = &g_players[i];
        if (!unityhooks_matches(&p->find, base, size)) continue;
        if (!unityhooks_matches(&p->renew, base, size) || !unityhooks_matches(&p->dtor, base, size) ||
            !unityhooks_matches(&p->erase, base, size) ||
            !unityhooks_matches(&p->dtor0, base, size) || !unityhooks_matches(&p->deref, base, size) ||
            !unityhooks_matches(&p->lookup, base, size) || (size_t)p->idmap_got + 4 > size) {
            fprintf(stderr, "[scripts] UA_SCRIPT_FIND: the scripts' code differs from what was read; Unity's own lookup is used\n");
            return;
        }
        p_lookup = (uintptr_t (*)(uintptr_t, const int32_t *))(base + p->lookup.off);
        g_idmap_slot = base + p->idmap_got;
        // MonoScript's vtable: its slots hold MonoScript's deleting destructor
        int vt_ok = 0;
        if ((size_t)p->script_vtable + 24 * 4 <= size)
            for (int k = 0; k < 24 && !vt_ok; k++) vt_ok = rd(base + p->script_vtable + 4 * k) == base + p->dtor0.off;
        if (!vt_ok) {
            fprintf(stderr, "[scripts] UA_SCRIPT_FIND: MonoScript's vtable is not where it was read; Unity's own lookup is used\n");
            return;
        }
        g_vt_script = base + p->script_vtable;
        // the counters first: a lookup must never see a kept answer from before a change
        if (unityhooks_hook(base + p->renew.off, p->renew.isa, (void *)renew_hook, (void **)&o_renew) != 0 ||
            unityhooks_hook(base + p->dtor.off, p->dtor.isa, (void *)dtor_hook, (void **)&o_dtor) != 0 ||
            unityhooks_hook(base + p->erase.off, p->erase.isa, (void *)erase_hook, (void **)&o_erase) != 0) {
            fprintf(stderr, "[scripts] UA_SCRIPT_FIND: hook failed; Unity's own lookup is used\n");
            return;
        }
        if (unityhooks_hook(base + p->find.off, p->find.isa, (void *)find_hook, (void **)&o_find) != 0) {
            fprintf(stderr, "[scripts] UA_SCRIPT_FIND: hook failed; Unity's own lookup is used\n");
            return;
        }
        fprintf(stderr, "[scripts] FindRuntimeScript's answers kept on UnityMain (UA_SCRIPT_FIND=%s; libunity %s)\n", v, p->find.unity);
        return;
    }
    fprintf(stderr, "[scripts] UA_SCRIPT_FIND: this libunity.so is not one whose script lookup was read; Unity's own lookup is used\n");
}
