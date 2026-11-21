// nativebeh.c's unit test (AArch64; run by unityhooks_test.sh). A model of Unity's behaviour managers (a map of
// queues, each a list and a pending list of nodes inside the behaviours), of MonoBehaviours (handles of each kind,
// a script cache behind a weak_ptr, Start, the enabled byte, a GameObject's cached active byte, a managed object's
// cached pointer) and of the functions nativebeh.c calls. "Unity's loop" here is written after the shipped
// player's code (CommonUpdate, SafeIterator's constructor and Next, IntegrateLists, AddBehaviour,
// RemoveBehaviour, CallUpdateMethod, IsStateMachineBehaviour) in the shape that code has, separately from
// nativebeh.c's. Scripted frames run three times from the same start: Unity's loop, ours (UA_NATIVE_BEH=1) and
// ours checked step by step against Unity's (=verify); the scripts change the lists while they run (one removes
// the next, one disables and enables itself, one adds a new behaviour, one destroys one already run, a Start that
// disables its own behaviour), and the behaviours cover every early exit of CallUpdateMethod. Each run's calls
// (Start and update methods, native behaviours, the control blocks' callbacks: in order), the lists' final order
// and every behaviour's started byte, the weak_ptrs' counts must be the same in all three, and verify must count
// no mismatch. Also: a verify run against a Unity whose Next and CallUpdateMethod differ from ours by one detail
// each must report them.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nativebeh.c"

int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }
int unityhooks_jump(uintptr_t fn, char isa, void *to) { (void)fn; (void)isa; (void)to; return -1; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#if !defined(__aarch64__)
int main(void) { printf("PASSED nativebeh: 0 failure(s) (the loop: AArch64 only)\n"); return 0; }
#else
// --- the world
#define NB 40
static U g_b[NB];                   // behaviours by id (0: none)
static U g_mgr[3];                  // the three managers
static U g_vt_mb[0x40], g_vt_native[0x40];
static U g_methods_a[12], g_methods_b[12], g_methods_c[12];
static char g_log[16384];
static size_t g_loglen;
static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_log + g_loglen, sizeof g_log - g_loglen, fmt, ap);
    va_end(ap);
    if (n > 0) g_loglen += (size_t)n < sizeof g_log - g_loglen ? (size_t)n : 0;
}
static uint32_t id_of(U b) { for (int i = 1; i < NB; i++) if (g_b[i] == b) return (uint32_t)i; return 0; }

// Unity's list functions, after the player's code
static void ref_remove(U node)                      // BaseBehaviourManager::RemoveBehaviour
{
    U p = rd(node);
    if (!p) return;
    U n = rd(node + 8);
    wr(p + 8, n); wr(n, p);
    wr(node, 0); wr(node + 8, 0);
}
static void ref_push_back(U root, U node)           // List::push_back (a node in another list is unlinked first)
{
    if (root == node) return;
    ref_remove(node);
    U t = rd(root);
    wr(node + 8, root); wr(t + 8, node);
    U y = rd(node + 8);
    wr(node, t); wr(y, node);
}
static U queue_of(U mgr, int which)                 // the map's node for queue `which` (0: the root, 1: its left)
{
    U root = rd(mgr + 0x10);
    return which ? rd(root) : root;
}
static void ref_add(U mgr, U node, int which)       // AddBehaviour: into the queue's pending list
{
    ref_push_back(rd(queue_of(mgr, which) + 0x30), node);
}
static void ref_integrate(U mgr)                    // IntegrateLists: each queue's pending list onto its list's end
{
    for (U q = rd(mgr + 8), end = mgr + 0x10; q != end; ) {
        U pend = rd(q + 0x30), list = rd(q + 0x28);
        U first = rd(pend + 8);
        if (first != pend) {
            U lt = rd(list), pt = rd(pend);
            wr(lt + 8, first); wr(first, lt);
            wr(pt + 8, list); wr(list, pt);
            wr(pend, pend); wr(pend + 8, pend);
        }
        U r = rd(q + 8), x;
        if (r) { do { x = r; r = rd(r); } while (r); }
        else { U c = q; for (;;) { x = rd(c + 0x10); if (rd(x) == c) break; c = x; } }
        q = x;
    }
}
static void ref_it_ctor(void *itp, U list)          // SafeIterator(list): the list's nodes moved into the iterator
{
    U it = (U)itp;
    wr(it, it); wr(it + 8, it); wr(it + 0x10, list); wr(it + 0x18, 0);
    U first = rd(list + 8);
    if (first != list) {
        U last = rd(list);
        wr(it + 8, first); wr(first, it);
        wr(it, last); wr(last + 8, it);
        wr(list, list); wr(list + 8, list);
    }
}
static U ref_next(void *itp)                        // Next: the first node back at the list's end, returned
{
    U it = (U)itp, n = rd(it + 8);
    if (n == it) { wr(it + 0x18, 0); return 0; }
    wr(it + 0x18, n);
    ref_remove(n);
    ref_push_back(rd(it + 0x10), n);
    return n;
}
static int g_bad_next;                              // a wrong "Unity" for the verify test
static U bad_next(void *itp)
{
    U it = (U)itp, n = ref_next(itp);
    if (n && g_bad_next) { U list = rd(it + 0x10); ref_remove(n); ref_push_back(list, n); wr(n, rd(n)); wr(list + 8, rd(list + 8)); }
    if (n && g_bad_next == 2) { ref_remove(n); }    // the node left out of the list
    return n;
}

// the scripts' world: GC handles, objects, the managed calls
static U g_slot[NB], g_mobj[NB][4];                 // a strong handle's slot; the managed object (cached pointer at +0x10)
static U ref_resolve(U h) { uint32_t id = (uint32_t)(h >> 1) & 0xff; return id < NB ? (U)g_mobj[id] : 0; }
static uint32_t ref_iiv(U obj) { return obj && rd(obj + 0x10) != 0; }
static uint32_t ref_is_active(U go)
{
    uint32_t a = rdb(go + 0x47);
    if (a == 0xff) { a = rdb(go + 0x46); wrb(go + 0x47, a); logf_("active(go)=%u ", a); }
    return a != 0;
}
static uint32_t ref_get_enabled(U b) { return rdb(b + 0x28) != 0; }
static uint32_t override_enabled(U b) { logf_("override-enabled(%u) ", id_of(b)); return 1; }

static int g_disable_in_start;
static U ref_imocc_impl(U b, U method, U obj)
{
    logf_("Start(%u,%lu,%lu) ", id_of(b), (unsigned long)(method & 0xfff), (unsigned long)obj);
    if (g_disable_in_start && id_of(b) == 12) wrb(b + 0x28, 0);
    return 0;
}
static void ref_inv_ctor(void *inv, U obj, U method)
{
    memset(inv, 0xaa, 0x130);
    wr((U)inv + 0x100, obj); wr((U)inv + 0xf8, method);
}
static void script_action(uint32_t id, U method);
static U ref_invoke_impl(void *inv, void *exc, uint32_t convert)
{
    U obj = rd((U)inv + 0x100), m = rd((U)inv + 0xf8);
    uint32_t id = rd32((U)inv + 0x110);
    logf_("call(%u,obj%u,m%lu,%u,%lu) ", id, (unsigned)(obj ? ((U *)obj - &g_mobj[0][0]) / 4 : 0), (unsigned long)(m & 0xfff), convert,
          (unsigned long)rd((U)exc) + rd((U)exc + 8));
    script_action(id, m);
    return 0;
}
// the entries Unity's CallUpdateMethod calls: the hooks in verify, the functions otherwise
static U (*t_imocc)(U, U, U) = ref_imocc_impl;
static U (*t_invoke)(void *, void *, uint32_t) = ref_invoke_impl;

static void cb_on_zero_shared(U cb) { logf_("zero-shared(%lu) ", (unsigned long)(cb & 0xfff)); }
static void cb_on_zero_weak(U cb) { logf_("zero-weak(%lu) ", (unsigned long)(cb & 0xfff)); }
static U g_cb_vt[8];

static uint32_t ref_is_smb(U b)                     // IsStateMachineBehaviour, as libc++'s weak_ptr::lock does it
{
    U p = rd(b + 0xe0), cb = rd(b + 0xe8);
    if (!cb) return 0;
    __atomic_fetch_add((int64_t *)(cb + 0x10), 1, __ATOMIC_RELAXED);
    U locked = 0;
    int64_t c = __atomic_load_n((int64_t *)(cb + 8), __ATOMIC_ACQUIRE);
    while (c != -1) {
        int64_t e = c;
        if (__atomic_compare_exchange_n((int64_t *)(cb + 8), &e, c + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { locked = cb; break; }
        c = e;
    }
    U q = locked ? p : 0;
    if (__atomic_load_n((int64_t *)(cb + 0x10), __ATOMIC_ACQUIRE) == 0 || __atomic_fetch_add((int64_t *)(cb + 0x10), -1, __ATOMIC_ACQ_REL) == 0)
        cb_on_zero_weak(cb);
    uint32_t r = q ? rdb(q + 0x38) != 0 : 0;
    if (locked && __atomic_fetch_add((int64_t *)(locked + 8), -1, __ATOMIC_ACQ_REL) == 0) {
        cb_on_zero_shared(locked);
        if (__atomic_load_n((int64_t *)(locked + 0x10), __ATOMIC_ACQUIRE) == 0 || __atomic_fetch_add((int64_t *)(locked + 0x10), -1, __ATOMIC_ACQ_REL) == 0)
            cb_on_zero_weak(locked);
    }
    return r;
}

static int g_bad_cum;
static void ref_cum(U b, int idx)                   // MonoBehaviour::CallUpdateMethod
{
    if (ref_is_smb(b)) return;
    U h = rd(b + 0x18);
    if (!h) return;
    U obj = (h & 1) ? ref_resolve(h) : rd(h);
    if (!obj) return;
    if (!rdb(b + 0x125)) {
        U m = rd(b + 0xf8);
        wrb(b + 0x125, 1);
        if (m && rd(m + 0x58)) t_imocc(b, rd(m + 0x58), 0);
    }
    if (!ref_iiv(obj)) return;
    if (!(((uint32_t (*)(U))rd(rd(b) + 0xd0))(b) & 1)) return;
    U m = rd(b + 0xf8);
    if (!m || !rd(m + 8 * (U)idx)) return;
    U me = rd(m + 8 * (U)idx);
    U go = rd(b + 0x20);
    if (!go || !ref_is_active(go)) return;
    h = rd(b + 0x18);
    U obj2 = h ? ((h & 1) ? ref_resolve(h) : rd(h)) : 0;
    if (g_bad_cum && id_of(b) == 20) obj2 = 0;      // a wrong "Unity" for the verify test
    U inv[0x140 / 8] __attribute__((aligned(16)));
    ref_inv_ctor(inv, obj2, me);
    inv[0x130 / 8] = inv[0x138 / 8] = 0;
    *(uint32_t *)((char *)inv + 0x110) = rd32(b + 8);
    t_invoke(inv, (char *)inv + 0x130, 0);
}
static void mb_update(U b) { ref_cum(b, 0); }
static void mb_late(U b) { ref_cum(b, 1); }
static void mb_fixed(U b) { ref_cum(b, 2); }
static void native_update(U b) { logf_("native(%u) ", id_of(b)); }

static int g_scope;
static void ref_scope_ctor(void *s, int policy) { memset(s, 0x55, 0x18); g_scope++; logf_("scope(%d) ", policy); }
static void ref_scope_dtor(void *s) { (void)s; g_scope--; logf_("~scope "); }

static void ref_common_update(U mgr, int k)         // BaseBehaviourManager::CommonUpdate<...>
{
    U scope[4];
    ref_scope_ctor(scope, 1);
    ref_integrate(mgr);
    for (U q = rd(mgr + 8), end = mgr + 0x10; q != end; ) {
        U it[4];
        ref_it_ctor(it, rd(q + 0x28));
        while (ref_next(it)) {
            U b = rd(it[3] + 0x10);
            ((void (*)(U))rd(rd(b) + k_slot[k]))(b);
        }
        U r = rd(q + 8), x;
        if (r) { do { x = r; r = rd(r); } while (r); }
        else { U c = q; for (;;) { x = rd(c + 0x10); if (rd(x) == c) break; c = x; } }
        q = x;
    }
    ref_scope_dtor(scope);
}

// the scripts' actions while they run (the frame's number decides some)
static int g_frame;
static U node_of(uint32_t id, int k) { return g_b[id] + (k == 0 ? 0x70 : k == 1 ? 0xa0 : 0x88); }
static void script_action(uint32_t id, U method)
{
    (void)method;
    if (id == 3) ref_remove(node_of(4, 0));                                      // removes the next one
    if (id == 5 && g_frame == 1) { ref_remove(node_of(5, 0)); ref_add(g_mgr[0], node_of(5, 0), 0); }  // disables, enables itself
    if (id == 6 && g_frame == 0) ref_add(g_mgr[0], node_of(30, 0), 1);           // adds a new one (pending)
    if (id == 7 && g_frame == 1) ref_remove(node_of(2, 0));                      // destroys one already run
    if (id == 21 && g_frame == 2) ref_remove(node_of(22, 1));                    // LateUpdate: the next one
}

static U galloc(size_t n) { void *p = aligned_alloc(64, (n + 63) & ~(size_t)63); memset(p, 0, n); return (U)p; }
static U g_cache_smb, g_cache_plain, g_cb_a, g_cb_b, g_cb_dead, g_gos[NB];

static U make_mgr(void)
{
    // a map of two queues: the root (order 0) and its left child (order -1); end node at +0x10, size at +0x18
    U mgr = galloc(0x40), root = galloc(0x40), left = galloc(0x40);
    wr(mgr + 8, left); wr(mgr + 0x10, root); wr(mgr + 0x18, 2);
    wr(root, left); wr(root + 8, 0); wr(root + 0x10, mgr + 0x10);
    wr(left, 0); wr(left + 8, 0); wr(left + 0x10, root);
    U qs[2] = { root, left };
    for (int i = 0; i < 2; i++) {
        U l = galloc(16), p = galloc(16);
        wr(l, l); wr(l + 8, l); wr(p, p); wr(p + 8, p);
        wr(qs[i] + 0x28, l); wr(qs[i] + 0x30, p);
    }
    return mgr;
}

static void build(void)
{
    memset(g_b, 0, sizeof g_b);
    for (int i = 0; i < 0x40; i++) { g_vt_mb[i] = (U)native_update; g_vt_native[i] = (U)native_update; }
    g_vt_mb[0xe0 / 8] = (U)mb_update; g_vt_mb[0xf0 / 8] = (U)mb_late; g_vt_mb[0xe8 / 8] = (U)mb_fixed;
    g_vt_mb[0xd0 / 8] = (U)ref_get_enabled; g_vt_native[0xd0 / 8] = (U)ref_get_enabled;
    memset(g_methods_a, 0, sizeof g_methods_a); memset(g_methods_b, 0, sizeof g_methods_b); memset(g_methods_c, 0, sizeof g_methods_c);
    g_methods_a[0] = 0x101; g_methods_a[1] = 0x102; g_methods_a[2] = 0x103;   // Update, LateUpdate, FixedUpdate
    g_methods_b[0] = 0x201; g_methods_b[1] = 0x202; g_methods_b[11] = 0x20b;  // Start at +0x58
    g_methods_c[1] = 0x302;                                                   // no Update
    g_cb_vt[0x10 / 8] = (U)cb_on_zero_shared; g_cb_vt[0x20 / 8] = (U)cb_on_zero_weak;
    g_cache_smb = galloc(0x40); wrb(g_cache_smb + 0x38, 1);
    g_cache_plain = galloc(0x40);
    g_cb_a = galloc(0x20); wr(g_cb_a, (U)g_cb_vt); wr(g_cb_a + 8, 0); wr(g_cb_a + 0x10, 3);
    g_cb_b = galloc(0x20); wr(g_cb_b, (U)g_cb_vt); wr(g_cb_b + 8, 0); wr(g_cb_b + 0x10, 0);
    g_cb_dead = galloc(0x20); wr(g_cb_dead, (U)g_cb_vt); wr(g_cb_dead + 8, -1); wr(g_cb_dead + 0x10, 2);
    for (int k = 0; k < 3; k++) g_mgr[k] = make_mgr();
    for (int i = 1; i < NB; i++) {
        U b = galloc(0x140);
        g_b[i] = b;
        wr(b, (U)g_vt_mb);
        *(uint32_t *)(b + 8) = (uint32_t)i;
        memset(g_mobj[i], 0, sizeof g_mobj[i]);
        g_mobj[i][2] = 0x1000 + (U)i;                                  // the cached native pointer: valid
        g_slot[i] = (U)g_mobj[i];
        wr(b + 0x18, (U)&g_slot[i]);
        g_gos[i] = galloc(0x80); wrb(g_gos[i] + 0x47, 1); wrb(g_gos[i] + 0x46, 1);
        wr(b + 0x20, g_gos[i]);
        wrb(b + 0x28, 1);
        wr(b + 0xe0, g_cache_plain); wr(b + 0xe8, i % 2 ? g_cb_a : g_cb_b);
        wr(b + 0xf8, (U)g_methods_a);
        wrb(b + 0x125, 1);
        wr(b + 0x70 + 0x10, b); wr(b + 0x88 + 0x10, b); wr(b + 0xa0 + 0x10, b);
    }
    // the early exits and the other kinds
    wr(g_b[8] + 0xe0, g_cache_smb);                                    // a state machine behaviour
    wr(g_b[9] + 0x18, 0);                                              // no handle
    wr(g_b[10] + 0x18, (10u << 1) | 1);                                // a weak handle
    wrb(g_b[11] + 0x125, 0); wr(g_b[11] + 0xf8, (U)g_methods_b);       // Start due
    wrb(g_b[12] + 0x125, 0); wr(g_b[12] + 0xf8, (U)g_methods_b);       // Start, which disables it
    wrb(g_gos[13] + 0x47, 0);                                          // inactive
    wrb(g_gos[14] + 0x47, 0xff);                                       // active, not cached yet
    wr(g_b[15] + 0xf8, (U)g_methods_c);                                // no Update method
    wr(g_b[16], (U)g_vt_native);                                       // a native behaviour
    g_mobj[17][2] = 0;                                                 // its native object gone
    wr(g_b[18] + 0xe8, g_cb_dead);                                     // its script cache expired
    wr(g_b[19] + 0xe8, 0);                                             // no script cache
    wrb(g_b[23] + 0x28, 0);                                            // disabled
    wr(g_b[24] + 0x20, 0);                                             // no GameObject
    wr(g_b[25] + 0x18, (39u << 1) | 1); g_mobj[39][2] = 0x1027;        // a weak handle to another object
    wrb(g_b[26] + 0x125, 0); wr(g_b[26] + 0xf8, 0);                    // never started, no method table
    wr(g_b[27] + 0xe0, 0);                                             // a cache pointer of 0 behind a live block
    U o = galloc(0x40 * 8); memcpy((void *)o, g_vt_mb, sizeof g_vt_mb); ((U *)o)[0xd0 / 8] = (U)override_enabled;
    wr(g_b[28], o);                                                    // GetEnabled overridden
    // the lists: Update queue 0 (root) 1 - 15 then 16 - 19 and 23 - 28, queue 1 (left) 20, 2, 29; Late: 21, 22, 11, 20;
    // Fixed: 3, 16, 10
    for (int i = 1; i <= 19; i++) if (i != 2) ref_add(g_mgr[0], node_of((uint32_t)i, 0), 0);
    for (int i = 23; i <= 28; i++) ref_add(g_mgr[0], node_of((uint32_t)i, 0), 0);
    ref_add(g_mgr[0], node_of(20, 0), 1); ref_add(g_mgr[0], node_of(2, 0), 1); ref_add(g_mgr[0], node_of(29, 0), 1);
    ref_add(g_mgr[1], node_of(21, 1), 0); ref_add(g_mgr[1], node_of(22, 1), 0); ref_add(g_mgr[1], node_of(11, 1), 1); ref_add(g_mgr[1], node_of(20, 1), 1);
    ref_add(g_mgr[2], node_of(3, 2), 0); ref_add(g_mgr[2], node_of(16, 2), 0); ref_add(g_mgr[2], node_of(10, 2), 0);
    wr(g_b[20] + 0xf8, (U)g_methods_b);                                // (Start already run)
}

static void digest(void)
{
    logf_("| ");
    for (int k = 0; k < 3; k++)
        for (int w = 0; w < 2; w++) {
            U q = queue_of(g_mgr[k], w);
            for (int l = 0; l < 2; l++) {
                U root = rd(q + 0x28 + 8 * (U)l);
                logf_("[");
                for (U n = rd(root + 8); n != root; n = rd(n + 8)) logf_("%u ", id_of(rd(n + 0x10)));
                logf_("]");
            }
        }
    for (int i = 1; i < NB; i++) logf_("%u", rdb(g_b[i] + 0x125));
    logf_(" cb %ld/%ld %ld/%ld %ld/%ld", (long)rd(g_cb_a + 8), (long)rd(g_cb_a + 0x10), (long)rd(g_cb_b + 8), (long)rd(g_cb_b + 0x10),
          (long)rd(g_cb_dead + 8), (long)rd(g_cb_dead + 0x10));
}

static void set_unity(int mode)
{
    memset(&u, 0, sizeof u);
    u.scope_ctor = ref_scope_ctor; u.scope_dtor = ref_scope_dtor; u.integrate = ref_integrate; u.it_ctor = ref_it_ctor;
    u.next = g_bad_next ? bad_next : ref_next; u.cum = ref_cum; u.is_smb = ref_is_smb; u.resolve = ref_resolve; u.iiv = ref_iiv;
    u.is_active = ref_is_active; u.inv_ctor = ref_inv_ctor;
    u.upd[0] = (U)mb_update; u.upd[1] = (U)mb_late; u.upd[2] = (U)mb_fixed;
    u.get_enabled = (U)ref_get_enabled; u.vt_mb = (U)g_vt_mb;
    o_imocc = ref_imocc_impl; o_invoke = ref_invoke_impl;
    u.imocc = mode == 2 ? imocc_hook : ref_imocc_impl;
    u.invoke = mode == 2 ? invoke_hook : ref_invoke_impl;
    t_imocc = u.imocc; t_invoke = u.invoke;
    g_mode = mode;
    memset(&g_v, 0, sizeof g_v);
    g_vlogged = 0;
}

// one run: 3 frames of Update, LateUpdate and FixedUpdate; mode -1: Unity's loop
static void run(int mode, char *out, size_t cap)
{
    g_loglen = 0; g_log[0] = 0;
    build();
    set_unity(mode < 0 ? 0 : mode);
    if (g_bad_next || g_bad_cum) g_vlogged = 20;    // the wrong Unitys' mismatches are expected: not logged
    g_disable_in_start = 1;
    for (g_frame = 0; g_frame < 3; g_frame++) {
        for (int k = 0; k < 3; k++) {
            logf_("<%d> ", k);
            int kk = k == 0 ? NB_UPDATE : k == 1 ? NB_LATE : NB_FIXED;
            if (mode < 0) ref_common_update(g_mgr[k], kk);
            else nb_common_update(g_mgr[k], kk);
        }
        logf_("\n");
    }
    digest();
    snprintf(out, cap, "%s", g_log);
}

static char g_a[16384], g_o[16384], g_vv[16384];
int main(void)
{
    run(-1, g_a, sizeof g_a);
    run(1, g_o, sizeof g_o);
    CHECK(!strcmp(g_a, g_o), "UA_NATIVE_BEH=1 differs from Unity's loop:\nUnity's: %s\nours:    %s", g_a, g_o);
    run(2, g_vv, sizeof g_vv);
    CHECK(!strcmp(g_a, g_vv), "UA_NATIVE_BEH=verify differs from Unity's loop:\nUnity's: %s\nverify:  %s", g_a, g_vv);
    uint64_t bad = g_v.step_bad + g_v.smb_bad + g_v.start_bad + g_v.inv_bad + g_v.none_bad + g_v.queues_bad;
    CHECK(bad == 0 && g_v.steps > 50 && g_v.inv_ok > 20 && g_v.none_ok > 10 && g_v.start_ok >= 2 && g_v.unchecked >= 1,
          "verify counts: %lu steps, %lu bad; inv %lu ok; none %lu ok; start %lu ok; unchecked %lu", (unsigned long)g_v.steps,
          (unsigned long)bad, (unsigned long)g_v.inv_ok, (unsigned long)g_v.none_ok, (unsigned long)g_v.start_ok, (unsigned long)g_v.unchecked);
    CHECK(g_scope == 0, "the temp-memory scopes are balanced");
    // the expected shape (by hand): in frame 0 behaviour 4 is never called (3 removes it), 30 is called from frame 1 on
    CHECK(strstr(g_a, "call(3,") && !strstr(g_a, "call(4,") && strstr(g_a, "call(30,") && strstr(g_a, "Start(11,") && strstr(g_a, "Start(12,") &&
          !strstr(g_a, "call(12,") && strstr(g_a, "native(16)") && strstr(g_a, "active(go)=1") && !strstr(g_a, "call(8,") &&
          !strstr(g_a, "call(17,") && strstr(g_a, "call(18,") && strstr(g_a, "call(25,obj39"), "the scenario's calls:\n%s", g_a);
    // a "Unity" whose Next or CallUpdateMethod differs: verify reports it
    g_bad_next = 2;
    run(2, g_vv, sizeof g_vv);
    CHECK(g_v.step_bad > 0, "verify finds a Next that leaves the node out (%lu)", (unsigned long)g_v.step_bad);
    g_bad_next = 0; g_bad_cum = 1;
    run(2, g_vv, sizeof g_vv);
    CHECK(g_v.inv_bad > 0, "verify finds a CallUpdateMethod that calls with another object (%lu)", (unsigned long)g_v.inv_bad);
    g_bad_cum = 0;
    printf("%s nativebeh: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
#endif
