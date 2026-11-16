// unitybeh.c: the scripts' update loops (Update, LateUpdate, FixedUpdate) with less waiting on memory and without
// calls that compute nothing. Off by default:
//   UA_BEH_PREFETCH=1   the next scripts' memory fetched ahead (=2: by loads where they are safe, PLD elsewhere)
//   UA_BEH_EMPTY=1      a started script's empty Update / LateUpdate / FixedUpdate is not called
//   UA_BEH_STATS=<s>    every <s> seconds: the loops' calls a frame, how many were empty, the most called methods
//
// Every frame Unity walks its lists of behaviours (BaseBehaviourManager::CommonUpdate): the list's nodes live inside
// the scripts' native objects, and for each one the loop moves the node back into its list (SafeIterator::Next),
// calls the object's update through its vtable, and MonoBehaviour::CallUpdateMethod reads the native object (its
// managed object's handle, its GameObject, its script's method table, a started flag: three or four cache lines),
// the GameObject's cached active state and the managed object's pointer back, then invokes the method through
// Mono. Nothing between two scripts is computation: on the in-order Cortex-A53 / A35 each of those reads waits for
// memory (about 200 ns from DRAM on the H700 handhelds), some 1.5 ms of a busy Enter the Gungeon frame (~1,250
// behaviours) in those four functions alone, with the script's own first reads of its fields on top.
//
// UA_BEH_PREFETCH. The hook calls Unity's own Next unchanged and returns what it returned; then, while the
// behaviour it returned runs, it asks the cores to fetch what the next two will read (PLD: a hint, it changes no
// state and never faults):
//   - the node of the third behaviour ahead (its address from the second's node, fetched one call ago);
//   - the native object of the second ahead (its address from its node: the lines Unity's loop and
//     CallUpdateMethod read);
//   - for the next one, whose object was fetched one call ago: its GameObject's line (IsActive's cached byte) and,
//     for a MonoBehaviour whose handle keeps its managed object, the managed object's first two lines.
// Only live memory is read: the nodes still in the iterator's list (a behaviour removed while another one runs is
// unlinked from it by Unity, so it is never reached) and fields inside those nodes' objects; a MonoBehaviour's own
// fields only when its vtable is MonoBehaviour's. The order of calls, every value Unity computes and every write it
// makes are unchanged: a prefetch only moves when memory arrives.
//
// UA_BEH_EMPTY. Unity lists a script for Update if its class has a method of that name at all, also an empty one
// (`void Update() {}`: its IL body a single `ret`). CallUpdateMethod then runs the script's missing Start first (its
// started flag), checks that the managed object, the method, the GameObject and its active state are there, and
// invokes the method; Mono's runtime invoke initialises the method's class first (its static constructor: a no-op
// once the class's vtable is marked initialized, also after a failed constructor) and runs the body. For a script
// whose Start has run, whose method's IL is only `ret` (nops before it allowed), and whose method's class is
// initialized in the domain, every path through CallUpdateMethod computes nothing: so the hook returns at once
// there and calls Unity's own CallUpdateMethod everywhere else (the first calls, before Start, before the class
// is initialized, other methods). What is left out is a call that has no effect: no write, no exception, no
// managed code. (GameObject::IsActive's cached byte is not filled in that call; it holds the same value whenever it
// is computed.)
//
// The layout (the node's next and data words, the object's handle, target, GameObject words, started byte and
// method table, the GameObject's cached active byte, MonoVTable's initialized bit) is the one the rows' code reads;
// the rows tie the offsets to exactly that code: the hooked functions and, as "call" rows (only checked, never
// changed), the functions whose reads the offsets come from. MonoBehaviour's vtable is found by its symbol's offset
// and checked at load: its update slot must hold MonoBehaviour::Update, else nothing is hooked.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "unitybeh.h"
#include "unityhooks.h"
#include "../loader/so_loader.h"

void *mono_runtime_lib(void);   // hitches.c

struct beh_player {
    struct uh_row next;                 // SafeIterator<List<ListNode<Behaviour> > >::Next (hooked: UA_BEH_PREFETCH)
    struct uh_row cum;                  // MonoBehaviour::CallUpdateMethod(int) (hooked: UA_BEH_EMPTY, UA_BEH_STATS)
    struct uh_row layout[2];            // the code the offsets were read from (checked only)
    uint32_t mb_vtable;                 // MonoBehaviour's vtable as objects point to it (its symbol + 8)
    uint32_t mb_vt_update, mb_update;   // the slot CommonUpdate<BehaviourManager> calls, and MonoBehaviour::Update
};

// 2017.4.27f1 armeabi-v7a (Enter the Gungeon), the offsets used here: the iterator's root and each node hold the
// next node at +4 (Next's node surgery), a node its behaviour at +8 (CommonUpdate<BehaviourManager>, which then calls
// vtable +0x7c); CallUpdateMethod reads the object at +0x14 (its managed object's handle kind; 2: the target is kept
// at +0x18), +0x18, +0x1c (the GameObject), +0x44, +0x48 (the script's methods: Update, LateUpdate, FixedUpdate at
// 0, 4, 8, its argument times 4) and +0x99 (started: Start has been called); the Update / LateUpdate / FixedUpdate
// nodes are at +0x54 / +0x6c / +0x60; IsActive returns the GameObject's byte at +0x3f unless it is 0xff. Its Mono
// (libmono.so, Mono 2.6): MonoVTable's byte +0x1b bit 1 is `initialized` (mono_runtime_class_init returns at once
// when it is set)
static const struct beh_player g_players[] = {
    {
        { "2017.4.27f1 armeabi-v7a", "SafeIterator<List<ListNode<Behaviour> > >::Next", 0x4e348c, 0x70, 'a', 0xf9c22af8383f7918ull },
        { "2017.4.27f1 armeabi-v7a", "MonoBehaviour::CallUpdateMethod", 0x72ccb8, 0x128, 'a', 0x57b12eed11ebd6a7ull },
        {
            { "2017.4.27f1 armeabi-v7a", "GameObject::IsActive", 0x3e86e8, 0xc4, 'a', 0xa5f805db0f6e2514ull },
            { "2017.4.27f1 armeabi-v7a", "BaseBehaviourManager::CommonUpdate<BehaviourManager>", 0x4e339c, 0x74, 'a', 0x2f52d0b53d1b541full },
        },
        0x1108dbc, 0x7c, 0x72cde0,
    },
};

static void *(*o_next)(void *it);
static void (*o_cum)(uintptr_t mb, int idx);
static uintptr_t g_vt;
static int g_empty, g_stats_on;
static double g_stats_every, g_stats_next;

static inline uintptr_t rd(uintptr_t a) { return *(const uintptr_t *)a; }
static inline void pf(uintptr_t a) { __builtin_prefetch((const void *)a); }

// --- UA_BEH_PREFETCH
// what the next behaviours will read, while the current one runs (it: Unity's iterator; its root node's next is
// the next behaviour's node, the end of the list is the root itself)
void unitybeh_prefetch(uintptr_t it, uintptr_t vt_mb)
{
    uintptr_t n1 = rd(it + 4);
    if (n1 == it) return;
    uintptr_t o1 = rd(n1 + 8);
    pf(rd(o1 + 0x1c) + 0x3f);
    if (rd(o1) == vt_mb && rd(o1 + 0x14) == 2) {
        uintptr_t m = rd(o1 + 0x18);
        pf(m);
        pf(m + 0x40);
    }
    uintptr_t n2 = rd(n1 + 4);
    if (n2 == it) return;
    uintptr_t o2 = rd(n2 + 8);
    pf(o2);
    pf(o2 + 0x40);
    pf(o2 + 0x80);
    pf(o2 + 0x99);
    uintptr_t n3 = rd(n2 + 4);
    if (n3 != it) pf(n3);
}

// UA_BEH_PREFETCH=2: the same addresses fetched by loads whose value is not used (a load waits for its page walk
// where a PLD may be dropped on a TLB miss): only addresses inside objects known to be that large are loaded (a
// MonoBehaviour's lines once its vtable, fetched one call before, says so; a GameObject's cached byte; a managed
// object's header), the rest by PLD
#if defined(__arm__)
static inline void touch(uintptr_t a) { uint32_t v; __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(a) : "memory"); (void)v; }
static inline void touchb(uintptr_t a) { uint32_t v; __asm__ volatile("ldrb %0, [%1]" : "=r"(v) : "r"(a) : "memory"); (void)v; }
#else
static inline void touch(uintptr_t a) { (void)*(const volatile uint32_t *)a; }
static inline void touchb(uintptr_t a) { (void)*(const volatile uint8_t *)a; }
#endif
void unitybeh_touch(uintptr_t it, uintptr_t vt_mb)
{
    uintptr_t n1 = rd(it + 4);
    if (n1 == it) return;
    uintptr_t o1 = rd(n1 + 8);
    uintptr_t go = rd(o1 + 0x1c);
    if (rd(o1) == vt_mb) {
        touchb(o1 + 0x99);
        if (go) touchb(go + 0x3f);
        if (rd(o1 + 0x14) == 2) { uintptr_t m = rd(o1 + 0x18); if (m) touch(m + 8); }
    } else if (go) pf(go + 0x3f);
    uintptr_t n2 = rd(n1 + 4);
    if (n2 == it) return;
    uintptr_t o2 = rd(n2 + 8);
    touch(o2);
    touch(n2);
    uintptr_t n3 = rd(n2 + 4);
    if (n3 != it) pf(n3);
}

static int g_pref_mode;
static void *next_hook(void *it)
{
    void *cur = o_next(it);
    if (cur) { if (g_pref_mode == 2) unitybeh_touch((uintptr_t)it, g_vt); else unitybeh_prefetch((uintptr_t)it, g_vt); }
    return cur;
}

// --- UA_BEH_EMPTY, UA_BEH_STATS: what each method is, once
static void *(*m_get_header)(uintptr_t method);
static uint32_t (*m_get_flags)(uintptr_t method, uint32_t *iflags);
static const uint8_t *(*m_header_get_code)(void *header, uint32_t *size, uint32_t *max_stack);
static uintptr_t (*m_method_get_class)(uintptr_t method);
static uintptr_t (*m_domain_get)(void);
static uintptr_t (*m_class_vtable)(uintptr_t domain, uintptr_t klass);
static char *(*m_full_name)(uintptr_t method, int signature);
static void (*m_free)(void *);
static int g_mono = -1;

static int mono_ok(void)
{
    if (g_mono >= 0) return g_mono;
    void *h = mono_runtime_lib();
    g_mono = 0;
    if (!h) return 0;
#define L(v, n) if (!(*(void **)&v = so_dlsym(h, n))) { fprintf(stderr, "[unitybeh] %s missing: UA_BEH_EMPTY stays off\n", n); return 0; }
    L(m_get_header, "mono_method_get_header"); L(m_header_get_code, "mono_method_header_get_code");
    L(m_get_flags, "mono_method_get_flags");
    L(m_method_get_class, "mono_method_get_class"); L(m_domain_get, "mono_domain_get");
    L(m_class_vtable, "mono_class_vtable"); L(m_full_name, "mono_method_full_name"); L(m_free, "mono_free");
#undef L
    return g_mono = 1;
}

enum { K_OTHER = 1, K_EMPTY = 2 };
struct ment { uintptr_t m, vtable; uint32_t calls, skipped; uint8_t kind; };
#define NMENT 4096
static struct ment g_ments[NMENT];
static uint32_t g_nment;

// an IL body that computes nothing: nops, then ret
int unitybeh_il_empty(const uint8_t *code, uint32_t size)
{
    if (!code || !size || code[size - 1] != 0x2a) return 0;
    for (uint32_t i = 0; i + 1 < size; i++)
        if (code[i] != 0x00) return 0;
    return 1;
}

static void classify(struct ment *e)
{
    e->kind = K_OTHER;
    if (!mono_ok()) return;
    uint32_t iflags = 0;
    m_get_flags(e->m, &iflags);
    if (iflags & 0x0020) return;                     // MethodImplAttributes.Synchronized: the call takes a lock
    void *hdr = m_get_header(e->m);
    if (!hdr) return;                                // abstract, runtime, internal call: no IL
    uint32_t size = 0, max_stack = 0;
    const uint8_t *code = m_header_get_code(hdr, &size, &max_stack);
    if (!unitybeh_il_empty(code, size)) return;
    uintptr_t klass = m_method_get_class(e->m), dom = m_domain_get();
    uintptr_t vt = klass && dom ? m_class_vtable(dom, klass) : 0;
    if (!vt) return;
    e->vtable = vt;
    e->kind = K_EMPTY;
}

static struct ment *ment(uintptr_t m)
{
    uint32_t h = ((uint32_t)(m >> 3) * 2654435761u) >> 20;
    for (uint32_t i = 0; i < NMENT; i++) {
        struct ment *e = &g_ments[(h + i) & (NMENT - 1)];
        if (e->m == m) return e;
        if (!e->m) {
            if (g_nment >= NMENT * 3 / 4) return NULL;  // full: Unity's own path from here on
            e->m = m;
            g_nment++;
            classify(e);
            return e;
        }
    }
    return NULL;
}

static uint32_t g_calls, g_started, g_empty_calls, g_skipped, g_frames;

static void cum_hook(uintptr_t mb, int idx)
{
    if (g_stats_on) g_calls++;
    if (*(const volatile uint8_t *)(mb + 0x99)) {                // Start has run
        uintptr_t m = rd(rd(mb + 0x48) + 4 * (uintptr_t)idx);
        if (m) {
            struct ment *e = ment(m);
            if (e) {
                if (g_stats_on) { g_started++; e->calls++; if (e->kind == K_EMPTY) g_empty_calls++; }
                if (g_empty && e->kind == K_EMPTY && (*(const volatile uint8_t *)(e->vtable + 0x1b) & 2)) {
                    if (g_stats_on) { g_skipped++; e->skipped++; }
                    return;
                }
            }
        }
    }
    o_cum(mb, idx);
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int by_calls(const void *a, const void *b)
{
    const struct ment *x = *(const struct ment *const *)a, *y = *(const struct ment *const *)b;
    return (x->calls < y->calls) - (x->calls > y->calls);
}

void unitybeh_frame(void)
{
    if (!g_stats_on) return;
    g_frames++;
    double t = now();
    if (!g_stats_next) { g_stats_next = t + g_stats_every; g_calls = g_started = g_empty_calls = g_skipped = g_frames = 0; return; }
    if (t < g_stats_next) return;
    g_stats_next = t + g_stats_every;
    double f = g_frames ? g_frames : 1;
    uint32_t nemptym = 0;
    static struct ment *top[NMENT];
    uint32_t n = 0;
    for (uint32_t i = 0; i < NMENT; i++)
        if (g_ments[i].m) { if (g_ments[i].kind == K_EMPTY) nemptym++; if (g_ments[i].calls) top[n++] = &g_ments[i]; }
    fprintf(stderr, "[unitybeh] %u frames: CallUpdateMethod %.1f a frame (started %.1f; empty methods %.1f, skipped %.1f); "
                    "%u methods seen, %u empty\n", g_frames, g_calls / f, g_started / f, g_empty_calls / f, g_skipped / f, g_nment, nemptym);
    qsort(top, n, sizeof *top, by_calls);
    for (uint32_t i = 0; i < n && i < 25; i++) {
        char *nm = mono_ok() ? m_full_name(top[i]->m, 0) : NULL;
        fprintf(stderr, "[unitybeh]   %6.1f %s%s\n", top[i]->calls / f, top[i]->kind == K_EMPTY ? "EMPTY " : "", nm ? nm : "?");
        if (nm) m_free(nm);
    }
    if (nemptym) {
        int k = 0;
        for (uint32_t i = 0; i < NMENT && k < 40; i++)
            if (g_ments[i].m && g_ments[i].kind == K_EMPTY) {
                char *nm = mono_ok() ? m_full_name(g_ments[i].m, 0) : NULL;
                fprintf(stderr, "[unitybeh]   empty: %s (%.1f a frame)\n", nm ? nm : "?", g_ments[i].calls / f);
                if (nm) m_free(nm);
                k++;
            }
    }
    for (uint32_t i = 0; i < NMENT; i++) g_ments[i].calls = g_ments[i].skipped = 0;
    g_calls = g_started = g_empty_calls = g_skipped = g_frames = 0;
}

void unitybeh_on_load(uintptr_t base, size_t size)
{
    const char *pv = getenv("UA_BEH_PREFETCH"), *ev = getenv("UA_BEH_EMPTY"), *sv = getenv("UA_BEH_STATS");
    int pref = pv && (!strcmp(pv, "1") || !strcmp(pv, "2")), empty = ev && !strcmp(ev, "1");
    g_pref_mode = pref ? atoi(pv) : 0;
    double stats = sv ? atof(sv) : 0;
    if (!pref && !empty && stats <= 0) return;
    for (size_t i = 0; i < sizeof g_players / sizeof *g_players; i++) {
        const struct beh_player *p = &g_players[i];
        if (!unityhooks_matches(&p->next, base, size) || !unityhooks_matches(&p->cum, base, size)) continue;
        for (size_t k = 0; k < sizeof p->layout / sizeof *p->layout; k++)
            if (!unityhooks_matches(&p->layout[k], base, size)) {
                fprintf(stderr, "[unitybeh] %s differs from the code read; Unity's own loops are used\n", p->layout[k].func);
                return;
            }
        if ((size_t)p->mb_vtable + p->mb_vt_update + 4 > size || rd(base + p->mb_vtable + p->mb_vt_update) != base + p->mb_update) {
            fprintf(stderr, "[unitybeh] MonoBehaviour's vtable is not where it was read; Unity's own loops are used\n");
            return;
        }
        g_vt = base + p->mb_vtable;
        if (pref) {
            if (unityhooks_hook(base + p->next.off, p->next.isa, (void *)next_hook, (void **)&o_next) != 0)
                fprintf(stderr, "[unitybeh] %s: hook failed; no prefetch\n", p->next.func);
            else
                fprintf(stderr, "[unitybeh] the behaviour loops fetch the next scripts' objects ahead (UA_BEH_PREFETCH; libunity %s)\n", p->next.unity);
        }
        if (empty || stats > 0) {
            g_empty = empty;
            g_stats_on = stats > 0;
            g_stats_every = stats > 0 ? stats : 0;
            if (unityhooks_hook(base + p->cum.off, p->cum.isa, (void *)cum_hook, (void **)&o_cum) != 0) {
                fprintf(stderr, "[unitybeh] %s: hook failed; empty callbacks are called\n", p->cum.func);
                g_empty = g_stats_on = 0;
            } else {
                fprintf(stderr, "[unitybeh] CallUpdateMethod hooked:%s%s (libunity %s)\n", empty ? " started scripts' empty Update/LateUpdate/FixedUpdate not called (UA_BEH_EMPTY)" : "",
                        g_stats_on ? " counted (UA_BEH_STATS)" : "", p->cum.unity);
            }
        }
        return;
    }
    fprintf(stderr, "[unitybeh] this libunity.so is not one whose behaviour loops were read; Unity's own loops are used\n");
}
