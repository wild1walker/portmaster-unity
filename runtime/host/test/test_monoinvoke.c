// monoinvoke.c's unit test: a double of Mono 2017.4's mono_jit_runtime_invoke (the same domain lock, info hash,
// class init and wrapper call, with MonoMethod's flag bits where the real one reads them), called through the
// cache's replacement and directly with the same script; everything but the lock and the lookups must happen
// in the same order with the same arguments and results: class init (full with exc, plain without), its
// exception as the result, the wrapper with the method's compiled code, the wrapper's exception passed back.
// NULL instances of instance methods (a static method's is cached), wrapper and dynamic methods, Mono's dyn-call
// infos, a second domain and monoinvoke_off take the original each time. Then four threads call 2,000 methods at once (each must reach its own code).
// The dispatch thunk: a double of mono_runtime_invoke (no-exec warning, profiler start/end with the method events,
// the call through default_mono_runtime_invoke) around the same doubles, a script run through it with the original
// runtime invoke and through monoinvoke_thunk with the cache installed: the same calls, class inits (traced only when
// they do work: the vtable's initialized bit clear, as Mono's return at once), results and exceptions, the profiler's
// end call when the events come on during a call; the original taken for NULL instances, wrappers, first calls,
// method events on, no-exec mode, another runtime invoke installed and after monoinvoke_off.
#define _GNU_SOURCE
#include "../monoinvoke.h"
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#if defined(__arm__)
// monoinvoke.c's UA_MONO_TLS patches with unityhooks.c, not linked here (its load hook is not exercised)
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig);
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig) { (void)fn; (void)isa; (void)to; (void)orig; return -1; }
#endif

// ---- the double's Mono ----
struct method { uint16_t flags; unsigned char head[18], bits[4]; uint32_t token; const char *name; void *vtable; int dyn, throws, events_on; };   // flags at 0, bits at 20, as in Mono
#define VT_FLAGS 0x1b                        // MonoVTable: initialized is bit 1 of byte 0x1b (Unity 2017.4's armeabi-v7a)
#define VT_INIT 0x02
typedef unsigned char vtable_t[32];
struct domain { int pad; int mutex; char rest[0xdc - 8]; char *jit_info; };
struct jit_info { char pad[0x28]; void *invoke_hash; };   // (the offsets go in the env: a 64-bit build pads them)
#define NH 4096
struct hash { void *key[NH]; void *val[NH]; int n; };

static __thread struct domain *cur_domain;
static __thread void **cur_params, **cur_exc;           // what the call was given (the wrapper must get the same)
static void *dom_get(void) { return cur_domain; }
static void *hash_lookup(void *t, const void *k)
{
    struct hash *h = t;
    for (int i = 0; i < h->n; i++) if (h->key[i] == k) return h->val[i];
    return NULL;
}
static pthread_mutex_t hash_mutex = PTHREAD_MUTEX_INITIALIZER;   // stands in for the domain lock (bionic shims)
static int lock(void *m) { (void)m; return pthread_mutex_lock(&hash_mutex); }
static int unlock(void *m) { (void)m; return pthread_mutex_unlock(&hash_mutex); }

// the trace (single-threaded parts only)
static char trace[1 << 16];
static size_t tlen;
static int tracing = 1;
static void tr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tr(const char *fmt, ...)
{
    if (!tracing) return;
    va_list a; va_start(a, fmt);
    tlen += vsnprintf(trace + tlen, sizeof trace - tlen, fmt, a);
    va_end(a);
    if (tlen < sizeof trace - 1) trace[tlen++] = '|';
}

static vtable_t bad_vtable;                  // its class init fails
static void *exc_obj = (void *)0xe0e0;
static int prof_events, no_exec;             // mono_profiler_events, mono_runtime_get_no_exec's variable
// Mono's class init returns at once when the vtable is initialised (traced only when it does work)
static void *class_init_full(void *vt, int raise)
{
    unsigned char *v = vt;
    if (v[VT_FLAGS] & VT_INIT) return NULL;
    tr("cinit_full %p %d", vt, raise);
    if (vt == (void *)bad_vtable) return exc_obj;
    v[VT_FLAGS] |= VT_INIT;
    return NULL;
}
static void class_init(void *vt)
{
    unsigned char *v = vt;
    if (v[VT_FLAGS] & VT_INIT) return;
    tr("cinit %p", vt);
    if (vt != (void *)bad_vtable) v[VT_FLAGS] |= VT_INIT;
}
static void *rt_wrapper(void *obj, void **params, void **exc, void *compiled)
{
    struct method *m = compiled;              // the "compiled code" is the method itself
    tr("wrapper %p params %s exc %s %s", obj, params == cur_params ? "same" : "OTHER", exc == cur_exc ? "same" : "OTHER", m->name);
    if (!tracing && m->token != (uint32_t)(uintptr_t)obj) __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);   // (threads: each its own)
    if (m->events_on) prof_events = 0x10000;  // (a profiler turned on during the call)
    if (m->throws && exc) { *exc = exc_obj; return NULL; }
    return (void *)(uintptr_t)(m->token + 1);
}
static int orig_calls, lookups;
static void *orig(void *method, void *obj, void **params, void **exc)
{
    struct method *m = method;
    struct domain *d = dom_get();
    __atomic_fetch_add(&orig_calls, 1, __ATOMIC_RELAXED);
    if (!obj && !(m->flags & 0x10) && !(m->bits[0] >> 7) && !(m->bits[0] >> 2 & 31)) { tr("null warning %s", m->name); return NULL; }
    lock(&d->mutex);
    struct hash *h = (struct hash *)((struct jit_info *)d->jit_info)->invoke_hash;
    void **info = hash_lookup(h, m);
    __atomic_fetch_add(&lookups, 1, __ATOMIC_RELAXED);
    if (!info) {
        info = calloc(6, sizeof *info);
        info[1] = m; info[2] = (void *)rt_wrapper; info[3] = m->vtable; info[4] = m->dyn ? (void *)1 : NULL;
        h->key[h->n] = m; h->val[h->n] = info; __atomic_store_n(&h->n, h->n + 1, __ATOMIC_RELEASE);
    }
    unlock(&d->mutex);
    if (exc) { *exc = class_init_full(info[3], 0); if (*exc) return NULL; }
    else class_init(info[3]);
    if (info[4]) tr("dyn call %s", m->name);
    return rt_wrapper(obj, params, exc, info[1]);
}

static struct domain *new_domain(void)
{
    struct domain *d = calloc(1, sizeof *d);
    struct jit_info *j = calloc(1, sizeof *j);
    j->invoke_hash = calloc(1, sizeof(struct hash));
    d->jit_info = (char *)j;
    return d;
}

#define NM 13
static struct method M[NM];
static vtable_t vt[NM];
static void vt_reset(void) { memset(vt, 0, sizeof vt); memset(bad_vtable, 0, sizeof bad_vtable); }
// the script: (domain 0/1, method, obj 0/1, exc given)
static const int script[][4] = {
    {0,0,1,1},{0,1,1,1},{0,0,1,1},{0,0,1,0},{0,1,1,1},{0,2,1,1},{0,2,1,1},{0,3,1,1},{0,3,1,1},{0,3,1,0},
    {0,4,1,1},{0,4,1,1},{0,5,1,1},{0,5,1,1},{0,6,0,1},{0,6,0,1},{0,7,1,1},{0,7,1,1},{0,8,1,1},{0,8,1,1},
    {1,0,1,1},{1,0,1,1},{0,0,1,1},{1,1,1,0},{0,9,1,1},{0,9,1,1},{0,10,0,1},{0,10,1,1},{0,10,1,1},{0,0,1,1},
};
static void run_script(void *(*inv)(void *, void *, void **, void **), struct domain **doms, const char *what)
{
    tlen = 0;
    vt_reset();
    void *params[2] = { 0, 0 };
    for (size_t i = 0; i < sizeof script / sizeof *script; i++) {
        cur_domain = doms[script[i][0]];
        void *ex = (void *)0x1;                 // the wrappers expect it set to NULL first: must not leak through
        cur_params = params; cur_exc = script[i][3] ? &ex : NULL;
        void *r = inv(&M[script[i][1]], script[i][2] ? (void *)(uintptr_t)M[script[i][1]].token : NULL, params,
                      script[i][3] ? &ex : NULL);
        tr("-> %p exc %p", r, script[i][3] ? ex : (void *)0x99);
    }
    (void)what;
}

struct targ { void *(*inv)(void *, void *, void **, void **); struct domain *d; struct method *ms; int n, rounds; };
static void *hammer(void *p)
{
    struct targ *a = p;
    cur_domain = a->d;
    for (int r = 0; r < a->rounds; r++)
        for (int i = 0; i < a->n; i++) {
            struct method *m = &a->ms[(i * 7 + r) % a->n];
            void *ex = NULL;
            cur_exc = &ex; cur_params = NULL;
            void *res = a->inv(m, (void *)(uintptr_t)m->token, NULL, &ex);
            if (res != (void *)(uintptr_t)(m->token + 1) || ex) __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
        }
    return NULL;
}

// ---- the dispatch thunk's double: Mono 2017.4's mono_runtime_invoke
static void *rt_var;                         // default_mono_runtime_invoke
static int ri_calls;
static void prof_start(void *m) { tr("prof start %s", ((struct method *)m)->name); }
static void prof_end(void *m) { tr("prof end %s", ((struct method *)m)->name); }
static void *orig_ri(void *method, void *obj, void **params, void **exc)
{
    __atomic_fetch_add(&ri_calls, 1, __ATOMIC_RELAXED);
    if (no_exec) tr("no-exec warning %s", ((struct method *)method)->name);
    if (prof_events & 0x10000) prof_start(method);
    void *r = ((void *(*)(void *, void *, void **, void **))rt_var)(method, obj, params, exc);
    if (prof_events & 0x10000) prof_end(method);
    return r;
}
// (domain, method, obj, exc given, method events, no-exec)
static const int tscript[][6] = {
    {0,0,1,1,0,0},{0,0,1,1,0,0},{0,0,1,0,0,0},{0,2,1,1,0,0},{0,2,1,1,0,0},{0,2,1,0,0,0},{0,3,1,1,0,0},{0,3,1,1,0,0},
    {0,3,1,0,0,0},{0,1,1,1,1,0},{0,1,1,1,1,0},{0,1,1,1,0,0},{0,1,1,0,0,0},{0,0,1,1,0,1},{0,6,0,1,0,0},{0,6,0,1,0,0},
    {0,10,0,1,0,0},{0,10,0,1,0,0},{0,4,1,1,0,0},
    {0,4,1,1,0,0},{1,0,1,1,0,0},{1,0,1,1,0,0},{0,9,1,1,0,0},{0,9,1,1,0,0},{0,12,1,1,0,0},{0,12,1,1,0,0},{0,0,1,1,0,0},
};
static void run_tscript(void *(*inv)(void *, void *, void **, void **), struct domain **doms)
{
    tlen = 0;
    vt_reset();
    void *params[2] = { 0, 0 };
    for (size_t i = 0; i < sizeof tscript / sizeof *tscript; i++) {
        cur_domain = doms[tscript[i][0]];
        prof_events = tscript[i][4] ? 0x10000 : 0;
        no_exec = tscript[i][5];
        void *ex = (void *)0x1;
        cur_params = params; cur_exc = tscript[i][3] ? &ex : NULL;
        struct method *m = &M[tscript[i][1]];
        void *r = inv(m, tscript[i][2] ? (void *)(uintptr_t)m->token : NULL, params, tscript[i][3] ? &ex : NULL);
        tr("-> %p exc %p", r, tscript[i][3] ? ex : (void *)0x99);
    }
    prof_events = 0; no_exec = 0;
}

#if defined(__arm__)
#include <sys/auxv.h>
// ---- UA_MONO_DIV: the replacements against C's division, with doubles of Mono's originals (which only see a
// divisor of 0, or -1 signed: their exceptions, here marked results)
static int div_orig_calls;
static int32_t d_idiv(int32_t a, int32_t b) { div_orig_calls++; return b == 0 ? 0x7777 : a == INT32_MIN ? 0x6666 : -a; }
static int32_t d_irem(int32_t a, int32_t b) { div_orig_calls++; return b == 0 ? 0x5555 : a == INT32_MIN ? 0x4444 : 0; }
static uint32_t d_idiv_un(uint32_t a, uint32_t b) { div_orig_calls++; (void)a; (void)b; return 0x3333; }
static uint32_t d_irem_un(uint32_t a, uint32_t b) { div_orig_calls++; (void)a; (void)b; return 0x2222; }
static void div_check(int hw)
{
    struct monoinvoke_div o = { d_idiv, d_irem, d_idiv_un, d_irem_un }, f;
    monoinvoke_div_start(&o, hw, &f);
    static const int32_t v[] = { 0, 1, -1, 2, -2, 3, -3, 7, -7, 10, -10, 16, -16, 255, 256, -256, 1000, -1000, 65535, -65536,
                                 0x12345678, -0x12345678, INT32_MAX, INT32_MIN, INT32_MAX - 1, INT32_MIN + 1 };
    int n = (int)(sizeof v / sizeof *v), bad = 0, expect_orig = 0;
    div_orig_calls = 0;
    uint32_t x = 2463534242u;
    for (int i = 0; i < n * n + 200000; i++) {
        int32_t a, b;
        if (i < n * n) { a = v[i / n]; b = v[i % n]; }
        else { x ^= x << 13; x ^= x >> 17; x ^= x << 5; a = (int32_t)x; x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = (int32_t)(x >> (x & 31)); }
        int32_t q = f.idiv(a, b), r = f.irem(a, b);
        uint32_t uq = f.idiv_un((uint32_t)a, (uint32_t)b), ur = f.irem_un((uint32_t)a, (uint32_t)b);
        if (b == 0 || b == -1) {
            expect_orig += 2;
            if (q != d_idiv(a, b) || r != d_irem(a, b)) bad++;
            div_orig_calls -= 2;
        } else if (q != a / b || r != a % b) bad++;
        if (b == 0) {
            expect_orig += 2;
            if (uq != 0x3333 || ur != 0x2222) bad++;
        } else if (uq != (uint32_t)a / (uint32_t)b || ur != (uint32_t)a % (uint32_t)b) bad++;
    }
    CHECK(!bad, "UA_MONO_DIV (%s): %d results differ from C's division", hw ? "SDIV/UDIV" : "C divide", bad);
    CHECK(div_orig_calls == expect_orig, "UA_MONO_DIV (%s): the originals called %d times, want %d (divisor 0, or -1 signed)",
          hw ? "SDIV/UDIV" : "C divide", div_orig_calls, expect_orig);
}
#endif

int main(void)
{
    static const char *names[NM] = { "Update", "LateUpdate", "Throws", "BadClass", "Wrapper", "Dynamic", "Static",
                                     "DynCall", "StringCtor", "Other", "NullThenObj", "Spare", "EventsOn" };
    for (int i = 0; i < NM; i++) { M[i].token = 0x100 + i * 4; M[i].name = names[i]; M[i].vtable = vt[i]; }
    M[12].events_on = 1;
    M[2].throws = 1;
    M[3].vtable = bad_vtable;
    M[4].bits[0] = 3 << 2;                      // wrapper_type 3
    M[5].bits[1] = 1 << 1;                      // dynamic
    M[6].flags = 0x10;                          // static (a NULL instance is fine)
    M[7].dyn = 1;
    M[8].bits[0] = 1 << 7;                      // string_ctor

    // the reference: the double alone
    struct domain *ref[2] = { new_domain(), new_domain() };
    run_script(orig, ref, "orig");
    char want[sizeof trace];
    memcpy(want, trace, tlen); want[tlen] = 0;

    // through the cache
    struct domain *doms[2] = { new_domain(), new_domain() };
    struct monoinvoke_env env = { .orig = orig, .domain_get = dom_get, .hash_lookup = hash_lookup,
                                  .class_init_full = class_init_full, .class_init = class_init, .lock = lock, .unlock = unlock,
                                  .domain_lock_off = offsetof(struct domain, mutex), .jit_info_off = offsetof(struct domain, jit_info),
                                  .invoke_hash_off = offsetof(struct jit_info, invoke_hash) };
    CHECK(offsetof(struct method, bits) == 20, "the double's layout");
    void *(*fast)(void *, void *, void **, void **) = monoinvoke_start(&env);
    orig_calls = 0;
    run_script(fast, doms, "fast");
    trace[tlen] = 0;
    CHECK(!strcmp(trace, want), "the same calls, arguments and results through the cache:\n  want %s\n  got  %s", want, trace);
    // first calls (and NULL instances, wrapper/dynamic/dyn-call methods, the other domain's first calls) only
    int expect = 0;
    { int seen[2][NM] = {{0}}; for (size_t i = 0; i < sizeof script / sizeof *script; i++) {
        int d = script[i][0], m = script[i][1];
        int null_inst = !script[i][2] && !(M[m].flags & 0x10);   // (a static method's NULL instance is cached as any call)
        if (null_inst || m == 4 || m == 5 || m == 7 || !seen[d][m]) expect++;
        if (!null_inst) seen[d][m] = 1; } }
    CHECK(orig_calls == expect, "the original %d times, want %d", orig_calls, expect);
    int cached = monoinvoke_cached();
    CHECK(cached == 10, "%d methods cached, want 10 (8 in domain 0 with the static one, 2 in domain 1)", cached);

    // the dispatch thunk: the double's mono_runtime_invoke with the original runtime invoke, then the thunk with the cache
    {
        struct domain *rd[2] = { new_domain(), new_domain() };
        rt_var = (void *)orig;
        run_tscript(orig_ri, rd);
        char twant[sizeof trace];
        memcpy(twant, trace, tlen); twant[tlen] = 0;
        struct domain *td[2] = { new_domain(), new_domain() };
        struct monoinvoke_env tenv = env;
        tenv.runtime_invoke = orig_ri; tenv.no_exec = &no_exec; tenv.prof_events = (const unsigned *)&prof_events;
        tenv.rt_var = (void *const *)&rt_var; tenv.prof_end = prof_end; tenv.vt_flags_off = VT_FLAGS; tenv.vt_initialized = VT_INIT;
        rt_var = (void *)monoinvoke_start(&tenv);
        ri_calls = 0;
        run_tscript(monoinvoke_thunk, td);
        trace[tlen] = 0;
        CHECK(!strcmp(trace, twant), "the thunk: the same calls, class inits, results and profiler calls:\n  want %s\n  got  %s", twant, trace);
        // the original: first calls (Update, Throws, BadClass, LateUpdate with the events on, the other domain's Update,
        // Other, EventsOn, Static's NULL instance), the events on again (1), no-exec (1), an instance method's NULL instance
        // (2), the wrapper (2)
        CHECK(ri_calls == 14, "the thunk took the original %d times, want 14", ri_calls);
        // another runtime invoke installed: the original every time
        rt_var = (void *)orig;
        ri_calls = 0;
        run_tscript(monoinvoke_thunk, td);
        CHECK(ri_calls == (int)(sizeof tscript / sizeof *tscript), "another runtime invoke: the original each call (%d)", ri_calls);
        rt_var = (void *)monoinvoke_start(&tenv);
        monoinvoke_off();
        ri_calls = 0;
        run_tscript(monoinvoke_thunk, td);
        CHECK(ri_calls == (int)(sizeof tscript / sizeof *tscript), "off: the thunk takes the original each call (%d)", ri_calls);
        monoinvoke_start(&env);              // (the cache alone again for the rest)
        cached = monoinvoke_cached();
    }

    // off: the original every time
    monoinvoke_off();
    orig_calls = 0;
    run_script(fast, doms, "off");
    trace[tlen] = 0;
    CHECK(!strcmp(trace, want) && orig_calls == (int)(sizeof script / sizeof *script), "off: the original each call (%d)", orig_calls);

    // threads: a fresh start (a new domain), 2,000 methods, four threads at once
    tracing = 0;
    orig_calls = 0;
    monoinvoke_start(&env);
    enum { N = 2000 };
    struct method *ms = calloc(N, sizeof *ms);
    static vtable_t vts[N];
    for (int i = 0; i < N; i++) { ms[i].token = 0x10000 + i * 8; ms[i].name = "m"; ms[i].vtable = vts[i]; }
    struct domain *d = new_domain();
    pthread_t t[4];
    struct targ a = { fast, d, ms, N, 50 };
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, hammer, &a);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    CHECK(monoinvoke_cached() == cached + N, "threads: %d cached, want %d", monoinvoke_cached(), cached + N);
#if defined(__arm__)
    div_check(0);
    unsigned long hw = getauxval(AT_HWCAP);
    if ((hw & (1ul << 17)) && (hw & (1ul << 18))) div_check(1);
    else fprintf(stderr, "test_monoinvoke: no SDIV/UDIV on this CPU: the C divide only\n");
#endif
    fprintf(stderr, "test_monoinvoke: %s (%d methods cached; the original %d times for 400,000 calls)\n",
            failures ? "FAILED" : "ok", monoinvoke_cached(), orig_calls);
    return failures != 0;
}
