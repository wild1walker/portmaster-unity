// monoinvoke.c's unit test: a double of Mono 2017.4's mono_jit_runtime_invoke (the same domain lock, info hash,
// class init and wrapper call, with MonoMethod's flag bits where the real one reads them), called through the
// cache's replacement and directly with the same script; everything but the lock and the lookups must happen
// in the same order with the same arguments and results: class init (full with exc, plain without), its
// exception as the result, the wrapper with the method's compiled code, the wrapper's exception passed back.
// NULL instances, wrapper and dynamic methods, Mono's dyn-call infos, a second domain and monoinvoke_off take
// the original each time. Then four threads call 2,000 methods at once (each must reach its own code).
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

// ---- the double's Mono ----
struct method { unsigned char head[20], bits[4]; uint16_t flags; uint32_t token; const char *name; void *vtable; int dyn, throws; };   // bits at 20, as in Mono
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

static int bad_vtable;                       // its class init fails
static void *exc_obj = (void *)0xe0e0;
static void *class_init_full(void *vt, int raise) { tr("cinit_full %p %d", vt, raise); return vt == (void *)&bad_vtable ? exc_obj : NULL; }
static void class_init(void *vt) { tr("cinit %p", vt); }
static void *rt_wrapper(void *obj, void **params, void **exc, void *compiled)
{
    struct method *m = compiled;              // the "compiled code" is the method itself
    tr("wrapper %p params %s exc %s %s", obj, params == cur_params ? "same" : "OTHER", exc == cur_exc ? "same" : "OTHER", m->name);
    if (!tracing && m->token != (uint32_t)(uintptr_t)obj) __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);   // (threads: each its own)
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

#define NM 12
static struct method M[NM];
static int vt[NM];
// the script: (domain 0/1, method, obj 0/1, exc given)
static const int script[][4] = {
    {0,0,1,1},{0,1,1,1},{0,0,1,1},{0,0,1,0},{0,1,1,1},{0,2,1,1},{0,2,1,1},{0,3,1,1},{0,3,1,1},{0,3,1,0},
    {0,4,1,1},{0,4,1,1},{0,5,1,1},{0,5,1,1},{0,6,0,1},{0,6,0,1},{0,7,1,1},{0,7,1,1},{0,8,1,1},{0,8,1,1},
    {1,0,1,1},{1,0,1,1},{0,0,1,1},{1,1,1,0},{0,9,1,1},{0,9,1,1},{0,10,0,1},{0,10,1,1},{0,10,1,1},{0,0,1,1},
};
static void run_script(void *(*inv)(void *, void *, void **, void **), struct domain **doms, const char *what)
{
    tlen = 0;
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

int main(void)
{
    static const char *names[NM] = { "Update", "LateUpdate", "Throws", "BadClass", "Wrapper", "Dynamic", "Static",
                                     "DynCall", "StringCtor", "Other", "NullThenObj", "Spare" };
    for (int i = 0; i < NM; i++) { M[i].token = 0x100 + i * 4; M[i].name = names[i]; M[i].vtable = &vt[i]; }
    M[2].throws = 1;
    M[3].vtable = &bad_vtable;
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
        if (!script[i][2] || m == 4 || m == 5 || m == 7 || !seen[d][m]) expect++;
        if (script[i][2]) seen[d][m] = 1; } }
    CHECK(orig_calls == expect, "the original %d times, want %d", orig_calls, expect);
    int cached = monoinvoke_cached();
    CHECK(cached == 9, "%d methods cached, want 9 (7 in domain 0, 2 in domain 1)", cached);

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
    static int vts[N];
    for (int i = 0; i < N; i++) { ms[i].token = 0x10000 + i * 8; ms[i].name = "m"; ms[i].vtable = &vts[i]; }
    struct domain *d = new_domain();
    pthread_t t[4];
    struct targ a = { fast, d, ms, N, 50 };
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, hammer, &a);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    CHECK(monoinvoke_cached() == cached + N, "threads: %d cached, want %d", monoinvoke_cached(), cached + N);
    fprintf(stderr, "test_monoinvoke: %s (%d methods cached; the original %d times for 400,000 calls)\n",
            failures ? "FAILED" : "ok", monoinvoke_cached(), orig_calls);
    return failures != 0;
}
