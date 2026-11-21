// nativebeh.c: the scripts' update loops (Update, LateUpdate, FixedUpdate) as our own code on the 64-bit player.
// Off by default; docs/arm64-routes.md ("Our own behaviour loop"), runtime.md.
//
//   UA_NATIVE_BEH=1        BaseBehaviourManager::CommonUpdate<BehaviourManager / LateBehaviourManager /
//                          FixedBehaviourManager> replaced by nb_common_update below
//   UA_NATIVE_BEH=verify   the same loop, but each step that changes something is done by Unity's own function and
//                          ours is checked against it on the live state (the differential test on real game states:
//                          below); a line every 5 s with the counts, the first mismatches in detail
//   UA_NATIVE_BEH_STATS=<s> every <s> seconds: the loops' time a frame, the time in the scripts they call (Start
//                          and the update methods: managed code), what is left (the loops' own work), the scripts
//                          called a frame and a hash of their order (instance id, method) over the last frame.
//                          With UA_NATIVE_BEH unset it times Unity's own loops (their entries hooked), for the A/B
//
// What Unity's loop does (6000.0.61f1 arm64-v8a, read from the shipped player's code): for each of the manager's
// queues (a std::map by execution order) it moves the queue's list into a SafeIterator, takes the nodes back one by
// one (SafeIterator::Next: the node goes back to the list's tail, so a behaviour removed while another one runs is
// never reached, one added goes to the pending list), and calls the node's behaviour through its vtable
// (Update +0xe0, FixedUpdate +0xe8, LateUpdate +0xf0). For a script the slot holds MonoBehaviour::Update (or Late,
// Fixed), which calls MonoBehaviour::CallUpdateMethod(idx): IsStateMachineBehaviour (a weak_ptr to the script's
// cache copied and locked: four atomics and five calls), the managed object through its GC handle, Start on the
// first call, IsInstanceValid, Behaviour::GetEnabled (virtual), the script's method, GameObject::IsActive, and
// ScriptingInvocation's constructor and Invoke. Fourteen functions over ten places in libunity's 19 MB of code,
// for each of 300 - 700 scripts a frame (Hollow Knight), each reading three to five lines of the behaviour and
// the lines of its GameObject, managed object and script cache: on the in-order Cortex-A53 a stack of calls whose
// every load waits for memory.
//
// What ours does. The same work in one function: the iterator's step (Next), the dispatch, CallUpdateMethod,
// IsStateMachineBehaviour with its weak_ptr's atomics, GetEnabled and IsActive's cached byte carried out in place,
// the same loads and stores in the same order (volatile accesses), the same atomics with the same memory orders
// (the helpers' LL/SC loops: the A53/A35 have no LSE), Unity's own functions called for everything else (the
// manager's lists merged, the temp-memory scope, the iterator's set-up, a weak handle's target, IsInstanceValid,
// IsActive when its byte is not cached yet, Start through InvokeMethodOrCoroutineChecked, ScriptingInvocation).
// A behaviour whose slot holds anything else (a native behaviour, an override) is called through its slot as Unity
// does. Ahead of each step it fetches what the next scripts will read (the lines UA_PREFETCH's `beh` stub fetches,
// from the same iterator state): this loop subsumes those stubs (the three call sites they patch are inside the
// functions replaced here; unityprefetch.c finds them changed and leaves them alone), and the other sites
// (sprite, cull, play) work alongside. Each managed call is the same call, with the same arguments, in the same
// order: the scripts cannot tell.
//
// UA_NATIVE_BEH=verify, the differential test: our loop drives, but
//   - each iterator step is computed by our code on a write overlay (nothing stored), then Unity's own Next runs;
//     its return value and every word it changed must be what ours computed, and no other word of the nodes,
//     their neighbours, the list and the iterator may have changed;
//   - IsStateMachineBehaviour is computed by ours and by Unity's (both leave the counts as they were): the same;
//   - Unity's own CallUpdateMethod runs for each script; ours predicts, on the live state, whether it calls Start
//     and with which method (checked when Unity calls InvokeMethodOrCoroutineChecked), and, after Start (at the
//     moment Unity calls ScriptingInvocation::Invoke, or when it returns without), whether it invokes the update
//     method and with which object, method and instance id (checked against the ScriptingInvocation Unity built);
//   - the queues visited must be as many as the manager's map holds.
// Counts and the first mismatches go to the log.
//
// Found as unityhooks.c finds its hooks: tools/re/unityhooks.py rows for the three replaced functions, every
// function called or carried out here, and MonoBehaviour's vtable slots checked at load; any other build matches
// no row and keeps Unity's loops. Hooks first: unityprefetch.c and UA_RELAYOUT come after, and leave these
// changed functions where they are.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nativebeh.h"
#include "unityhooks.h"

#if !defined(__aarch64__)
// the 64-bit player's loops only
void nativebeh_frame(void) {}
void nativebeh_on_load(uintptr_t base, size_t size)
{
    (void)base; (void)size;
    const char *v = getenv("UA_NATIVE_BEH");
    if (v && *v && strcmp(v, "0"))
        fprintf(stderr, "[nativebeh] UA_NATIVE_BEH: the 64-bit player's loops only; Unity's loops are kept\n");
}
#else
typedef uintptr_t U;

enum { NB_UPDATE, NB_LATE, NB_FIXED };
// the manager's vtable slot each loop calls, and CallUpdateMethod's index (the script's method table: Update,
// LateUpdate, FixedUpdate)
static const uint32_t k_slot[3] = { 0xe0, 0xf0, 0xe8 };
static const int k_idx[3] = { 0, 1, 2 };

struct nb_player {
    struct uh_row cu[3];            // CommonUpdate<BehaviourManager / Late / Fixed> (replaced)
    struct uh_row upd[3];           // MonoBehaviour::Update / LateUpdate / FixedUpdate (their entries compared)
    struct uh_row next, it_ctor, integrate, scope_ctor, scope_dtor, cum, is_smb, script_cache, lock, release_weak,
        ldadd_relax, ldadd_acqrel, cas_acqrel, resolve, iiv, get_enabled, is_active, imocc, inv_ctor, invoke;
    uint32_t mb_vtable;             // MonoBehaviour's vtable as objects point to it
};

#define P61 "6000.0.61f1 arm64-v8a"
// 6000.0.61f1 arm64-v8a (read 2026-11-21). The layout used: a manager's map of queues at +8 (begin) with its end
// node at +0x10 and its size at +0x18 (libc++'s tree: a node's left +0, right +8, parent +0x10, the queue's list
// pointer +0x28); a list node {prev, next, data}; the SafeIterator {root prev, root next, list, current} (0x20
// bytes); ManagedTempMemScope 0x18 bytes (policy 1). A MonoBehaviour: instance id +8 (int), GC handle +0x18 (0:
// none; bit 0: a weak handle, resolved by ResolveBackendNativeGCHandle; else the address of its slot), GameObject
// +0x20, enabled byte +0x28 (Behaviour::GetEnabled, vtable +0xd0), the script's cache weak_ptr at +0xe0 / +0xe8
// (SerializableManagedRef at +0xd0, GetScriptCache), its method table +0xf8 (Update, LateUpdate, FixedUpdate at 0,
// 8, 0x10; Start at +0x58), started byte +0x125. The script cache: IsStateMachineBehaviour's byte at +0x38. The
// weak_ptr's control block: vtable (+0x10 on zero shared, +0x20 on zero weak), shared count +8, weak count +0x10
// (libc++: counts minus one). GameObject::IsActive: the byte at +0x47 unless it is 0xff. ScriptingInvocation:
// 0x130 bytes (the object at +0x100, the method at +0xf8, the instance id for the exception's log at +0x110), the
// exception pair after it, as CallUpdateMethod lays them out.
static const struct nb_player g_players[] = {
    {
        {
            { P61, "BaseBehaviourManager::CommonUpdate<BehaviourManager>", 0x79c474, 0xb4, 'x', 0xa193417a7199eaf0ull },
            { P61, "BaseBehaviourManager::CommonUpdate<LateBehaviourManager>", 0x79c6f4, 0xb4, 'x', 0x71d44f729658aceaull },
            { P61, "BaseBehaviourManager::CommonUpdate<FixedBehaviourManager>", 0x79c63c, 0xb4, 'x', 0x85f30964d0eac0d9ull },
        },
        {
            { P61, "MonoBehaviour::Update", 0x9d3f8c, 0x8, 'x', 0x44ceb46c07cc2775ull },
            { P61, "MonoBehaviour::LateUpdate", 0x9d3f94, 0x8, 'x', 0x518cc49ceb6516c1ull },
            { P61, "MonoBehaviour::FixedUpdate", 0x9d3f9c, 0x8, 'x', 0xc38c7b99390df777ull },
        },
        { P61, "SafeIterator<List<ListNode<MonoBehaviour> > >::Next", 0x79c5b0, 0x88, 'x', 0x4b56a8a1ecb3e812ull },
        { P61, "SafeIterator<List<ListNode<Behaviour> > >::SafeIterator", 0x79c528, 0x88, 'x', 0xca6b068da744e26full },
        { P61, "BaseBehaviourManager::IntegrateLists", 0x7989e8, 0x80, 'x', 0xcb8a1a8e07cf0ec9ull },
        { P61, "ManagedTempMemScope::ManagedTempMemScope", 0x9956e4, 0x6c, 'x', 0xc554eb91caf4ea9aull },
        { P61, "ManagedTempMemScope::~ManagedTempMemScope", 0x9957d0, 0x3c, 'x', 0x68388ca98432af55ull },
        { P61, "MonoBehaviour::CallUpdateMethod", 0x9d3e78, 0x114, 'x', 0xd695de744b4c0dbdull },
        { P61, "MonoBehaviour::IsStateMachineBehaviour", 0x9d3490, 0x94, 'x', 0x59a740fd0a8842d7ull },
        { P61, "SerializableManagedRef::GetScriptCache", 0xa0b708, 0x24, 'x', 0x21a75ec90f549d6aull },
        { P61, "std::__shared_weak_count::lock", 0x18dcf08, 0x60, 'x', 0x247b6dce8688e581ull },
        { P61, "std::__shared_weak_count::__release_weak", 0x18dceb0, 0x58, 'x', 0xd85ba72d2c1eaaa3ull },
        { P61, "__aarch64_ldadd8_relax", 0x191f360, 0x30, 'x', 0x12a8618af05edac3ull },
        { P61, "__aarch64_ldadd8_acq_rel", 0x191f3f0, 0x30, 'x', 0xfa00659e70d91a83ull },
        { P61, "__aarch64_cas8_acq_rel", 0x191f080, 0x34, 'x', 0x04acc97da39286adull },
        { P61, "ScriptingGCHandle::ResolveBackendNativeGCHandle", 0x9bc380, 0x10, 'x', 0x44468d6dfbcfc140ull },
        { P61, "IsInstanceValid", 0x9b267c, 0x44, 'x', 0xd2ed891631123933ull },
        { P61, "Behaviour::GetEnabled", 0x756890, 0x10, 'x', 0xfcd2708b97f3c40dull },
        { P61, "GameObject::IsActive", 0x77aa58, 0xac, 'x', 0x60367e8d7a4fe47cull },
        { P61, "MonoBehaviour::InvokeMethodOrCoroutineChecked", 0x9d4eb4, 0x94, 'x', 0xd04b4e4081b0a171ull },
        { P61, "ScriptingInvocation::ScriptingInvocation", 0x9c8d04, 0x78, 'x', 0x17b4228ff6d10408ull },
        { P61, "ScriptingInvocation::Invoke", 0x9c9008, 0xcc, 'x', 0x6c141b866b6aded6ull },
        0x1946f30,
    },
};

// --- Unity's functions this loop calls (set at load; the test sets its own)
struct nb_unity {
    void (*scope_ctor)(void *scope, int policy);
    void (*scope_dtor)(void *scope);
    void (*integrate)(U mgr);
    void (*it_ctor)(void *it, U list);
    U (*next)(void *it);                            // verify: Unity's step
    void (*cum)(U b, int idx);                      // verify: Unity's CallUpdateMethod
    uint32_t (*is_smb)(U b);                        // verify: Unity's IsStateMachineBehaviour
    U (*resolve)(U handle);
    uint32_t (*iiv)(U obj);
    uint32_t (*is_active)(U go);
    U (*imocc)(U b, U method, U obj);
    void (*inv_ctor)(void *inv, U obj, U method);
    U (*invoke)(void *inv, void *exc, uint32_t convert);
    U upd[3];                                       // MonoBehaviour::Update, LateUpdate, FixedUpdate
    U get_enabled;                                  // Behaviour::GetEnabled
    // what MonoBehaviour's vtable holds for them when the loops first run: UA_RELAYOUT (run after the hooks) points
    // vtables at its copies of the functions, which compute the same
    U upd_rt[3], get_enabled_rt;
    int rt_ready;
    U vt_mb;                                        // MonoBehaviour's vtable (the prefetch's test)
};
static struct nb_unity u;
static int g_mode;                                  // 0 Unity's loops, 1 ours, 2 ours with Unity's steps checked
static int g_stats;

static inline U rd(U a) { return *(volatile U *)a; }
static inline void wr(U a, U v) { *(volatile U *)a = v; }
static inline uint32_t rdb(U a) { return *(volatile uint8_t *)a; }
static inline void wrb(U a, uint32_t v) { *(volatile uint8_t *)a = (uint8_t)v; }
static inline uint32_t rd32(U a) { return *(volatile uint32_t *)a; }
static inline void pfl(U a) { __builtin_prefetch((const void *)a, 0, 3); }
static inline void pfs(U a) { __builtin_prefetch((const void *)a, 1, 3); }

// --- the managed calls' order, the time (UA_NATIVE_BEH_STATS)
static inline uint64_t ticks(void)
{
#if defined(__aarch64__)
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t) :: "memory");
    return t;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
static uint64_t g_tick_hz;
static struct nb_count {
    uint64_t loop_ticks, managed_ticks, visits, invokes, starts, frames;
} g_cnt;
static uint64_t g_frame_hash = 0xcbf29ce484222325ull, g_last_hash, g_frame_calls, g_last_calls;
// per thread: Unity's loading threads call managed code (and so the hooks below) while UnityMain runs the loops
static __thread int g_mdepth;                       // inside a managed call made by a loop (nested calls not counted)
static __thread int g_unity_idx;                    // stats on Unity's loops: the loop running
static __thread int g_cudepth;                      // inside a loop
static inline void trace(uint32_t id, uint32_t what)
{
    uint64_t h = g_frame_hash;
    for (int i = 0; i < 4; i++) { h ^= (id >> (8 * i)) & 0xff; h *= 0x100000001b3ull; }
    h ^= what; h *= 0x100000001b3ull;
    g_frame_hash = h;
    g_frame_calls++;
}

// --- UA_NATIVE_BEH=verify's counts
static struct nb_verify {
    uint64_t steps, step_bad, smb, smb_bad, calls, start_ok, start_bad, inv_ok, inv_bad, none_ok, none_bad, queues_bad, unchecked;
} g_v;
static int g_vlogged;
#define VBAD(...) do { if (g_vlogged < 20) { g_vlogged++; fprintf(stderr, "[nativebeh] verify MISMATCH: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// --- the iterator's step: SafeIterator<List<ListNode<MonoBehaviour> > >::Next, in place. RD / WR: memory, or the
// overlay of the verify mode. The iterator's root is {prev +0, next +8}, its list +0x10, its current node +0x18; a
// node {prev +0, next +8}.
#define NB_NEXT_BODY(RD, WR)                                                                         \
    U x8 = it, x0 = RD(it + 8);                                                                      \
    if (x0 == x8) { WR(it + 0x18, 0); return 0; }                                                    \
    U x9 = RD(x0);                                                                                   \
    WR(it + 0x18, x0);                                                                               \
    if (x9) {                       /* out of the iterator's list */                                \
        U x10 = RD(x0 + 8);                                                                          \
        WR(x9 + 8, x10);                                                                             \
        WR(x10, x9);                                                                                 \
        WR(x0, 0); WR(x0 + 8, 0);                                                                    \
        x0 = RD(it + 0x18);                                                                          \
    }                                                                                                \
    x9 = RD(it + 0x10);                                                                              \
    if (x0 == x9) return x0;                                                                         \
    U x11 = RD(x0), x10 = x0;                                                                        \
    if (x11) {                                                                                       \
        x10 = RD(x0 + 8);                                                                            \
        WR(x11 + 8, x10);                                                                            \
        WR(x10, x11);                                                                                \
        WR(x0, 0); WR(x0 + 8, 0);                                                                    \
        x10 = RD(it + 0x18);                                                                         \
    }                                                                                                \
    U t = RD(x9);                   /* back at the list's tail */                                   \
    WR(x0 + 8, x9);                                                                                  \
    WR(t + 8, x0);                                                                                   \
    U y = RD(x0 + 8);                                                                                \
    WR(x0, t);                                                                                       \
    WR(y, x0);                                                                                       \
    return x10;

static U nb_next(U it) { NB_NEXT_BODY(rd, wr) }

#define NB_OV 24
static struct { U a, v; } g_ov[NB_OV];
static int g_nov, g_ov_full;
static U ov_rd(U a)
{
    for (int i = g_nov - 1; i >= 0; i--)
        if (g_ov[i].a == a) return g_ov[i].v;
    return rd(a);
}
static void ov_wr(U a, U v)
{
    for (int i = 0; i < g_nov; i++)
        if (g_ov[i].a == a) { g_ov[i].v = v; return; }
    if (g_nov == NB_OV) { g_ov_full = 1; return; }
    g_ov[g_nov].a = a; g_ov[g_nov].v = v; g_nov++;
}
static U nb_next_ov(U it) { NB_NEXT_BODY(ov_rd, ov_wr) }

// verify: ours on the overlay, then Unity's; the words either could touch compared
static U next_verified(U it)
{
    U cand[12];
    int nc = 0;
    U n = rd(it + 8);
    cand[nc++] = it; cand[nc++] = it + 8; cand[nc++] = it + 0x10; cand[nc++] = it + 0x18;
    if (n != it) {
        U p = rd(n), nx = rd(n + 8), list = rd(it + 0x10);
        cand[nc++] = n; cand[nc++] = n + 8;
        if (p) cand[nc++] = p + 8;
        if (nx) cand[nc++] = nx;
        cand[nc++] = list; cand[nc++] = list + 8;
        U tail = rd(list);
        if (tail) cand[nc++] = tail + 8;
    }
    U before[12];
    for (int i = 0; i < nc; i++) before[i] = rd(cand[i]);
    g_nov = 0; g_ov_full = 0;
    U mine = nb_next_ov(it);
    U got = u.next((void *)it);
    g_v.steps++;
    int bad = mine != got || g_ov_full;
    for (int i = 0; i < g_nov; i++)
        if (rd(g_ov[i].a) != g_ov[i].v) bad = 1;
    for (int i = 0; i < nc; i++) {
        U now = rd(cand[i]);
        if (now == before[i]) continue;
        int found = 0;
        for (int k = 0; k < g_nov; k++) if (g_ov[k].a == cand[i]) found = 1;
        if (!found) bad = 1;                    // Unity changed a word ours left
    }
    if (bad) { g_v.step_bad++; VBAD("iterator step: ours %#lx, Unity's %#lx (%d words written)", (unsigned long)mine, (unsigned long)got, g_nov); }
    return got;
}

// --- IsStateMachineBehaviour, in place: the weak_ptr copied (its weak count +1, relaxed), locked (the shared
// count +1 by compare-and-swap, acq_rel, unless it is -1: expired), the copy released, the cache's byte read, the
// lock released (acq_rel; at zero the control block's on-zero-shared and its weak release), as libc++'s code does
static void nb_release_weak(U cb)
{
    int64_t *w = (int64_t *)(cb + 0x10);
    if (__atomic_load_n(w, __ATOMIC_ACQUIRE) == 0 || __atomic_fetch_add(w, -1, __ATOMIC_ACQ_REL) == 0)
        ((void (*)(U))rd(rd(cb) + 0x20))(cb);
}
static U nb_lock(U cb)
{
    int64_t *s = (int64_t *)(cb + 8);
    int64_t c = __atomic_load_n(s, __ATOMIC_ACQUIRE);
    for (;;) {
        if (c == -1) return 0;
        int64_t seen = c;
        __atomic_compare_exchange_n(s, &seen, c + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        if (seen == c) return cb;
        c = seen;
    }
}
static uint32_t nb_is_smb(U b)
{
    U p = rd(b + 0xe0), cb = rd(b + 0xe8);
    if (!cb) return 0;
    __atomic_fetch_add((int64_t *)(cb + 0x10), 1, __ATOMIC_RELAXED);
    U locked = nb_lock(cb);
    U q = locked ? p : 0;
    nb_release_weak(cb);
    uint32_t r = 0;
    if (q) r = rdb(q + 0x38) != 0;
    if (locked && __atomic_fetch_add((int64_t *)(locked + 8), -1, __ATOMIC_ACQ_REL) == 0) {
        ((void (*)(U))rd(rd(locked) + 0x10))(locked);
        nb_release_weak(locked);
    }
    return r;
}

// --- CallUpdateMethod, in place. Its first part: the managed object (0: nothing is called), Start's call due
static inline U nb_target(U h) { return (h & 1) ? u.resolve(h) : rd(h); }

static U (*o_invoke)(void *inv, void *exc, uint32_t convert);   // Unity's Invoke (its trampoline when hooked)
static U (*o_imocc)(U b, U method, U obj);

// after Start: the update method's call, if any (0: none); *obj2, *meth, *id what it is called with
static int nb_decide(U b, int idx, U obj, U *obj2, U *meth, uint32_t *id, int verifying)
{
    if (!(u.iiv(obj) & 1)) return 0;
    U f = rd(rd(b) + 0xd0);
    uint32_t en;
    if (f == u.get_enabled || f == u.get_enabled_rt) en = rdb(b + 0x28) != 0;
    else if (verifying) { g_v.unchecked++; return -1; }     // an override: not called a second time
    else en = ((uint32_t (*)(U))f)(b);
    if (!(en & 1)) return 0;
    U m = rd(b + 0xf8);
    if (!m) return 0;
    U me = rd(m + 8 * (U)idx);
    if (!me) return 0;
    U go = rd(b + 0x20);
    if (!go) return 0;
    uint32_t a = rdb(go + 0x47);
    uint32_t act = a != 0xff ? a != 0 : u.is_active(go);
    if (!(act & 1)) return 0;
    U h = rd(b + 0x18);
    *obj2 = h ? nb_target(h) : 0;
    *meth = me;
    *id = rd32(b + 8);
    return 1;
}

static void nb_cum(U b, int idx)
{
    if (nb_is_smb(b) & 1) return;
    U h = rd(b + 0x18);
    if (!h) return;
    U obj = nb_target(h);
    if (!obj) return;
    if (!rdb(b + 0x125)) {
        U m = rd(b + 0xf8);
        wrb(b + 0x125, 1);
        if (m) {
            U s = rd(m + 0x58);
            if (s) {
                g_mdepth++;
                uint64_t t0 = g_stats ? ticks() : 0;
                o_imocc(b, s, 0);
                if (g_stats) { g_cnt.managed_ticks += ticks() - t0; g_cnt.starts++; trace(rd32(b + 8), 0x80 | (uint32_t)idx); }
                g_mdepth--;
            }
        }
    }
    U obj2, meth;
    uint32_t id;
    if (nb_decide(b, idx, obj, &obj2, &meth, &id, 0) != 1) return;
    U inv[0x140 / 8] __attribute__((aligned(16)));
    u.inv_ctor(inv, obj2, meth);
    uint32_t w8 = rd32(b + 8);
    inv[0x130 / 8] = inv[0x138 / 8] = 0;
    *(volatile uint32_t *)((char *)inv + 0x110) = w8;
    g_mdepth++;
    uint64_t t0 = g_stats ? ticks() : 0;
    o_invoke(inv, (char *)inv + 0x130, 0);
    if (g_stats) { g_cnt.managed_ticks += ticks() - t0; g_cnt.invokes++; trace(w8, (uint32_t)idx); }
    g_mdepth--;
}

// verify: Unity's CallUpdateMethod runs; ours predicts what it calls (the hooks below compare)
static __thread struct {
    int active, phase;              // phase 0: before Start, 1: in Start, 2: after it
    int nothing;                    // ours: no call at all (a state machine behaviour, no managed object)
    U b, obj, start;
    int idx, start_due, start_seen, inv_seen;
} g_vc;

static void verify_invoke_check(U inv)
{
    U obj2 = 0, meth = 0;
    uint32_t id = 0;
    int d = g_vc.nothing ? 0 : nb_decide(g_vc.b, g_vc.idx, g_vc.obj, &obj2, &meth, &id, 1);
    if (d < 0) return;
    if (d == 1 && rd(inv + 0x100) == obj2 && rd(inv + 0xf8) == meth && rd32(inv + 0x110) == id) { g_v.inv_ok++; return; }
    g_v.inv_bad++;
    VBAD("update call of %#lx idx %d: ours %s (obj %#lx method %#lx id %u), Unity's obj %#lx method %#lx id %u",
         (unsigned long)g_vc.b, g_vc.idx, d ? "calls" : "calls nothing", (unsigned long)obj2, (unsigned long)meth, id,
         (unsigned long)rd(inv + 0x100), (unsigned long)rd(inv + 0xf8), rd32(inv + 0x110));
}

static void cum_verified(U b, int idx)
{
    uint32_t mine = nb_is_smb(b) & 1, theirs = u.is_smb(b) & 1;
    g_v.smb++;
    if (mine != theirs) { g_v.smb_bad++; VBAD("IsStateMachineBehaviour of %#lx: ours %u, Unity's %u", (unsigned long)b, mine, theirs); }
    g_v.calls++;
    memset(&g_vc, 0, sizeof g_vc);
    g_vc.b = b; g_vc.idx = idx;
    g_vc.nothing = mine;
    if (!g_vc.nothing) {
        U h = rd(b + 0x18);
        g_vc.obj = h ? nb_target(h) : 0;
        if (!g_vc.obj) g_vc.nothing = 1;
    }
    if (!g_vc.nothing && !rdb(b + 0x125)) {
        U m = rd(b + 0xf8);
        g_vc.start = m ? rd(m + 0x58) : 0;
        g_vc.start_due = g_vc.start != 0;
    }
    g_vc.active = 1;
    u.cum(b, idx);
    g_vc.active = 0;
    if (g_vc.start_due != g_vc.start_seen) {
        g_v.start_bad++;
        VBAD("Start of %#lx: ours %s, Unity's %s", (unsigned long)b, g_vc.start_due ? "due" : "not due", g_vc.start_seen ? "called" : "not called");
    } else if (g_vc.start_due) g_v.start_ok++;
    if (g_vc.inv_seen) return;
    // Unity returned without the update's call: ours must decide the same on this state
    U obj2, meth;
    uint32_t id;
    int d = g_vc.nothing ? 0 : nb_decide(b, idx, g_vc.obj, &obj2, &meth, &id, 1);
    if (d == 0) g_v.none_ok++;
    else if (d == 1) { g_v.none_bad++; VBAD("update call of %#lx idx %d: ours calls method %#lx, Unity's calls nothing", (unsigned long)b, idx, (unsigned long)meth); }
}

// the hooks on Unity's InvokeMethodOrCoroutineChecked and ScriptingInvocation::Invoke (verify, stats)
static U imocc_hook(U b, U method, U obj)
{
    if (g_mode == 2 && g_vc.active && !g_mdepth) {
        if (g_vc.phase == 0 && !g_vc.nothing && g_vc.start_due && b == g_vc.b && method == g_vc.start && obj == 0) g_vc.start_seen = 1;
        else { g_v.start_bad++; VBAD("Unity called InvokeMethodOrCoroutineChecked(%#lx, %#lx) unpredicted", (unsigned long)b, (unsigned long)method); }
        g_vc.phase = 1;
    }
    int counted = g_cudepth && !g_mdepth;
    uint64_t t0 = counted && g_stats ? ticks() : 0;
    g_mdepth++;
    U r = o_imocc(b, method, obj);
    g_mdepth--;
    if (g_mode == 2 && g_vc.active && !g_mdepth && g_vc.phase == 1) g_vc.phase = 2;
    if (counted && g_stats) { g_cnt.managed_ticks += ticks() - t0; g_cnt.starts++; trace(rd32(b + 8), 0x80 | (uint32_t)(g_mode == 2 ? g_vc.idx : g_unity_idx)); }
    return r;
}
static U invoke_hook(void *inv, void *exc, uint32_t convert)
{
    if (g_mode == 2 && g_vc.active && !g_mdepth) {
        if (g_vc.inv_seen) { g_v.inv_bad++; VBAD("Unity invoked a second time for %#lx", (unsigned long)g_vc.b); }
        else verify_invoke_check((U)inv);
        g_vc.inv_seen = 1;
    }
    int counted = g_cudepth && !g_mdepth;
    uint64_t t0 = counted && g_stats ? ticks() : 0;
    g_mdepth++;
    U r = o_invoke(inv, exc, convert);
    g_mdepth--;
    if (counted && g_stats) {
        g_cnt.managed_ticks += ticks() - t0; g_cnt.invokes++;
        trace(rd32((U)inv + 0x110), (uint32_t)(g_mode == 2 ? g_vc.idx : g_unity_idx));
    }
    return r;
}

// --- the loop
// what the next behaviours read, fetched while this one runs (the lines of unityprefetch.c's `beh` stub, from the
// same iterator state: before the step)
static inline void nb_prefetch(U it)
{
    U n1 = rd(it + 8);
    if (n1 == it) return;
    U b = rd(n1 + 0x10);
    if (rd(b) == u.vt_mb) {
        U h = rd(b + 0x18);
        if (h && !(h & 1)) { U o = rd(h); pfl(o); pfl(o + 0x10); }
    }
    U n2 = rd(n1 + 8);
    if (n2 == it) return;
    b = rd(n2 + 0x10);
    if (rd(b) == u.vt_mb) {
        pfl(rd(b + 0x18)); pfl(rd(b + 0x20) + 0x40);
        pfl(rd(b + 0xe0) + 0x38); pfs(rd(b + 0xe8) + 8);
    }
    U n3 = rd(n2 + 8);
    if (n3 == it) return;
    b = rd(n3 + 0x10);
    pfl(b); pfl(b + 0x20); pfl(b + 0xe0); pfl(b + 0xf8); pfl(b + 0x120);
    U n4 = rd(n3 + 8);
    if (n4 == it) return;
    pfl(n4); pfl(n4 + 0x10);
}

static void nb_common_update(U mgr, int k)
{
    uint64_t t0 = g_stats ? ticks() : 0;
    g_cudepth++;
    U scope[4] __attribute__((aligned(16)));
    U itb[4] __attribute__((aligned(16)));
    U it = (U)itb;
    const uint32_t slot = k_slot[k];
    const int idx = k_idx[k];
    if (!u.rt_ready) {
        for (int j = 0; j < 3; j++) u.upd_rt[j] = rd(u.vt_mb + k_slot[j]);
        u.get_enabled_rt = rd(u.vt_mb + 0xd0);
        u.rt_ready = 1;
    }
    const U upd = u.upd[k], upd_rt = u.upd_rt[k];
    uint64_t queues = 0;
    u.scope_ctor(scope, 1);
    u.integrate(mgr);
    U q = rd(mgr + 8), end = mgr + 0x10;
    if (q != end) {
        do {
            u.it_ctor((void *)it, rd(q + 0x28));
            queues++;
            for (;;) {
                nb_prefetch(it);
                U n = g_mode == 2 ? next_verified(it) : nb_next(it);
                if (!n) break;
                U b = rd(rd(it + 0x18) + 0x10);
                U f = rd(rd(b) + slot);
                if (g_stats) g_cnt.visits++;
                if (f == upd || f == upd_rt) {
                    if (g_mode == 2) cum_verified(b, idx);
                    else nb_cum(b, idx);
                } else {
                    ((void (*)(U))f)(b);
                }
            }
            // the map's next queue (in order: the leftmost of the right subtree, or up to the first parent
            // reached from its left)
            U r = rd(q + 8), x;
            if (r) {
                do { x = r; r = rd(r); } while (r);
            } else {
                U c = q;
                for (;;) { x = rd(c + 0x10); if (rd(x) == c) break; c = x; }
            }
            q = x;
        } while (q != end);
    }
    u.scope_dtor(scope);
    if (g_mode == 2 && queues != rd(mgr + 0x18)) {
        g_v.queues_bad++;
        VBAD("loop %d visited %lu queues, the manager holds %lu", k, (unsigned long)queues, (unsigned long)rd(mgr + 0x18));
    }
    g_cudepth--;
    if (g_stats) g_cnt.loop_ticks += ticks() - t0;
}
static void nb_cu_update(U mgr) { nb_common_update(mgr, NB_UPDATE); }
static void nb_cu_late(U mgr) { nb_common_update(mgr, NB_LATE); }
static void nb_cu_fixed(U mgr) { nb_common_update(mgr, NB_FIXED); }

// stats on Unity's own loops: their entries hooked, timed
static void (*o_cu[3])(U mgr);
static void unity_cu(U mgr, int k)
{
    uint64_t t0 = ticks();
    int saved = g_unity_idx;
    g_unity_idx = k_idx[k];
    g_cudepth++;
    o_cu[k](mgr);
    g_cudepth--;
    g_unity_idx = saved;
    g_cnt.loop_ticks += ticks() - t0;
}
static void unity_cu_update(U mgr) { unity_cu(mgr, NB_UPDATE); }
static void unity_cu_late(U mgr) { unity_cu(mgr, NB_LATE); }
static void unity_cu_fixed(U mgr) { unity_cu(mgr, NB_FIXED); }

// --- once a frame
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double g_every, g_next_line;

void nativebeh_frame(void)
{
    if (!g_mode && !g_stats) return;
    g_last_hash = g_frame_hash; g_last_calls = g_frame_calls;
    g_frame_hash = 0xcbf29ce484222325ull; g_frame_calls = 0;
    g_cnt.frames++;
    double t = now();
    if (!g_next_line) { g_next_line = t + g_every; memset(&g_cnt, 0, sizeof g_cnt); return; }
    if (t < g_next_line) return;
    g_next_line = t + g_every;
    if (g_stats) {
        double f = g_cnt.frames ? (double)g_cnt.frames : 1, ms = 1000.0 / (double)g_tick_hz;
        fprintf(stderr, "[nativebeh] %s loops, %lu frames: %.3f ms a frame in the loops, %.3f in the scripts they call, %.3f their own "
                        "(%.1f behaviours, %.1f calls, %.2f Starts a frame); last frame %lu calls, order hash %016llx\n",
                g_mode ? "our" : "Unity's", (unsigned long)g_cnt.frames, g_cnt.loop_ticks * ms / f, g_cnt.managed_ticks * ms / f,
                ((double)g_cnt.loop_ticks - (double)g_cnt.managed_ticks) * ms / f, g_cnt.visits / f, g_cnt.invokes / f, g_cnt.starts / f,
                (unsigned long)g_last_calls, (unsigned long long)g_last_hash);
    }
    if (g_mode == 2)
        fprintf(stderr, "[nativebeh] verify: %lu steps (%lu bad), %lu IsStateMachineBehaviour (%lu bad), %lu script calls: Start %lu ok %lu bad, "
                        "update call %lu ok %lu bad, none %lu ok %lu bad, %lu unchecked, queue counts %lu bad\n",
                (unsigned long)g_v.steps, (unsigned long)g_v.step_bad, (unsigned long)g_v.smb, (unsigned long)g_v.smb_bad,
                (unsigned long)g_v.calls, (unsigned long)g_v.start_ok, (unsigned long)g_v.start_bad, (unsigned long)g_v.inv_ok,
                (unsigned long)g_v.inv_bad, (unsigned long)g_v.none_ok, (unsigned long)g_v.none_bad, (unsigned long)g_v.unchecked,
                (unsigned long)g_v.queues_bad);
    memset(&g_cnt, 0, sizeof g_cnt);
}

void nativebeh_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_NATIVE_BEH"), *sv = getenv("UA_NATIVE_BEH_STATS");
    int mode = !v || !*v || !strcmp(v, "0") ? 0 : !strcmp(v, "verify") ? 2 : !strcmp(v, "1") ? 1 : -1;
    double every = sv ? atof(sv) : 0;
    if (mode < 0) { fprintf(stderr, "[nativebeh] UA_NATIVE_BEH=%s: 1 or verify; Unity's loops are kept\n", v); return; }
    if (!mode && every <= 0) return;
    for (size_t i = 0; i < sizeof g_players / sizeof *g_players; i++) {
        const struct nb_player *p = &g_players[i];
        if (!unityhooks_matches(&p->cu[0], base, size)) continue;
        const struct uh_row *all[] = { &p->cu[1], &p->cu[2], &p->upd[0], &p->upd[1], &p->upd[2], &p->next, &p->it_ctor, &p->integrate,
                                       &p->scope_ctor, &p->scope_dtor, &p->cum, &p->is_smb, &p->script_cache, &p->lock, &p->release_weak,
                                       &p->ldadd_relax, &p->ldadd_acqrel, &p->cas_acqrel, &p->resolve, &p->iiv, &p->get_enabled,
                                       &p->is_active, &p->imocc, &p->inv_ctor, &p->invoke };
        for (size_t k = 0; k < sizeof all / sizeof *all; k++)
            if (!unityhooks_matches(all[k], base, size)) {
                fprintf(stderr, "[nativebeh] %s differs from the code read; Unity's loops are kept\n", all[k]->func);
                return;
            }
        U vt = base + p->mb_vtable;
        if (rd(vt + 0xe0) != base + p->upd[0].off || rd(vt + 0xf0) != base + p->upd[1].off || rd(vt + 0xe8) != base + p->upd[2].off ||
            rd(vt + 0xd0) != base + p->get_enabled.off) {
            fprintf(stderr, "[nativebeh] MonoBehaviour's vtable is not where it was read; Unity's loops are kept\n");
            return;
        }
        uint64_t hz;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
        g_tick_hz = hz ? hz : 24000000;
        u.scope_ctor = (void (*)(void *, int))(base + p->scope_ctor.off);
        u.scope_dtor = (void (*)(void *))(base + p->scope_dtor.off);
        u.integrate = (void (*)(U))(base + p->integrate.off);
        u.it_ctor = (void (*)(void *, U))(base + p->it_ctor.off);
        u.next = (U (*)(void *))(base + p->next.off);
        u.cum = (void (*)(U, int))(base + p->cum.off);
        u.is_smb = (uint32_t (*)(U))(base + p->is_smb.off);
        u.resolve = (U (*)(U))(base + p->resolve.off);
        u.iiv = (uint32_t (*)(U))(base + p->iiv.off);
        u.is_active = (uint32_t (*)(U))(base + p->is_active.off);
        u.imocc = (U (*)(U, U, U))(base + p->imocc.off);
        u.inv_ctor = (void (*)(void *, U, U))(base + p->inv_ctor.off);
        u.invoke = (U (*)(void *, void *, uint32_t))(base + p->invoke.off);
        for (int k = 0; k < 3; k++) u.upd[k] = base + p->upd[k].off;
        u.get_enabled = base + p->get_enabled.off;
        u.vt_mb = vt;
        o_invoke = u.invoke;
        o_imocc = u.imocc;
        g_stats = every > 0;
        g_every = every > 0 ? every : 5;
        // verify and stats see Unity's own calls of these two
        if (mode == 2 || g_stats) {
            if (unityhooks_hook(base + p->imocc.off, 'x', (void *)imocc_hook, (void **)&o_imocc) != 0 ||
                unityhooks_hook(base + p->invoke.off, 'x', (void *)invoke_hook, (void **)&o_invoke) != 0) {
                fprintf(stderr, "[nativebeh] the script calls could not be hooked; Unity's loops are kept\n");
                return;
            }
        }
        if (mode) {
            void (*const ours[3])(U) = { nb_cu_update, nb_cu_late, nb_cu_fixed };
            for (int k = 0; k < 3; k++)
                if (unityhooks_jump(base + p->cu[k].off, 'x', (void *)ours[k]) != 0) {
                    fprintf(stderr, "[nativebeh] %s: could not be replaced (%d of 3 done)\n", p->cu[k].func, k);
                    if (!k) return;
                }
        } else {
            void (*const wrap[3])(U) = { unity_cu_update, unity_cu_late, unity_cu_fixed };
            for (int k = 0; k < 3; k++)
                if (unityhooks_hook(base + p->cu[k].off, 'x', (void *)wrap[k], (void **)&o_cu[k]) != 0) {
                    fprintf(stderr, "[nativebeh] %s: could not be timed\n", p->cu[k].func);
                    return;
                }
        }
        g_mode = mode;
        fprintf(stderr, "[nativebeh] libunity %s: %s%s\n", p->cu[0].unity,
                mode == 1 ? "the behaviour update loops are ours (UA_NATIVE_BEH)" :
                mode == 2 ? "the behaviour update loops are ours, each step checked against Unity's own (UA_NATIVE_BEH=verify)" :
                            "Unity's behaviour update loops timed",
                g_stats ? " (UA_NATIVE_BEH_STATS)" : "");
        return;
    }
    fprintf(stderr, "[nativebeh] this libunity.so is not one whose behaviour loops were read (tools/re/unityhooks.py); Unity's loops are kept\n");
}
#endif
