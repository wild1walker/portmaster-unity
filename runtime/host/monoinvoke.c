// UA_MONO_INVOKE: Mono's runtime invoke through a per-method cache (see monoinvoke.h).
//
// libunity calls every Update, LateUpdate, FixedUpdate, coroutine step ... of the game's scripts through
// mono_runtime_invoke (about 1,200 calls a frame in Enter the Gungeon's fights). Unity 2017.4's armeabi-v7a
// libmono is built without optimisation, and each call of its mono_jit_runtime_invoke takes the domain lock,
// looks the method's RuntimeInvokeInfo up in the domain's runtime_invoke_hash, unlocks, runs the class's
// initialisation check and calls the runtime-invoke wrapper with the method's compiled code: about 1 ms a
// frame on the handheld, a third of it in the lock and the lookup (UA_PROF).
//
// Mono makes an info once per method and domain (on the first call) and never changes it; only a dynamic
// method's is freed (with the method), and a domain's with the domain. So the replacement installed in
// place of mono_jit_runtime_invoke (mono_runtime_invoke's checks and profiler calls stay in front of it)
// keeps, after a method's first call through the original, a pointer to that info in a lock-free table,
// and from then on does exactly what the original does with it: mono_domain_get, then
// mono_runtime_class_init_full (exc given: its exception is the result) or mono_runtime_class_init, then
// the info's runtime-invoke wrapper with the info's compiled code. The same calls in the same order, the
// same wrapper catching the same exceptions (Unity logs them and goes on): what is left out is the lock and
// the lookup, which change nothing. A NULL instance, a wrapper or dynamic method, an info on Mono's dynamic
// call path (dyn_call_info: never here, AOT-only), and every call after a domain unload go to the
// original. It needs the builds' private offsets, so it runs only on a libmono whose two functions match a
// checked build byte for byte, after mono_jit_init_version has installed the original.
#define _GNU_SOURCE
#include "monoinvoke.h"
#include "../loader/so_loader.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct monoinvoke_env E;
static int g_on;                                     // the fast path's switch (atomic)
#define METHOD_BITS 20                               // MonoMethod: inline_info:1 inline_failure:1 wrapper_type:5 string_ctor:1 save_lmf:1 dynamic:1 ...

// the table: open addressing, filled under g_tab_lock, read without it (a slot is published by its method
// pointer, release; never changed or emptied after)
#define SLOTS 8192
#define PROBES 64
struct slot { void *method; void *domain; void **info; };
static struct slot g_tab[SLOTS];
static pthread_mutex_t g_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_count;

static inline unsigned slot_of(const void *m)
{
    uint32_t x = (uint32_t)(uintptr_t)m;
    x ^= x >> 16; x *= 0x45d9f3bu; x ^= x >> 16;
    return x & (SLOTS - 1);
}

static inline void **find(void *domain, void *method)
{
    for (unsigned i = slot_of(method), n = 0; n < PROBES; i = (i + 1) & (SLOTS - 1), n++) {
        void *m = __atomic_load_n(&g_tab[i].method, __ATOMIC_ACQUIRE);
        if (!m) return NULL;
        if (m == method && g_tab[i].domain == domain) return g_tab[i].info;
    }
    return NULL;
}

// after the original's call: the info it made (or found) for (domain, method), looked up as it looks it up
static void remember(void *domain, void *method)
{
    const unsigned char *b = (const unsigned char *)method + METHOD_BITS;
    if ((b[0] >> 2 & 31) || (b[1] >> 1 & 1)) return;            // a wrapper; a dynamic method (its info is freed with it)
    char *jit = *(char **)((char *)domain + E.jit_info_off);
    void *table = jit ? *(void **)(jit + E.invoke_hash_off) : NULL;
    if (!table) return;
    void *mutex = (char *)domain + E.domain_lock_off;
    E.lock(mutex);
    void **info = (void **)E.hash_lookup(table, method);
    E.unlock(mutex);
    if (!info || info[4]) return;                                // none, or Mono's dynamic-call path
    pthread_mutex_lock(&g_tab_lock);
    if (!find(domain, method) && g_count < SLOTS / 2)
        for (unsigned i = slot_of(method), n = 0; n < PROBES; i = (i + 1) & (SLOTS - 1), n++)
            if (!g_tab[i].method) {
                g_tab[i].domain = domain;
                g_tab[i].info = info;
                __atomic_store_n(&g_tab[i].method, method, __ATOMIC_RELEASE);
                g_count++;
                break;
            }
    pthread_mutex_unlock(&g_tab_lock);
}

typedef void *(*rt_invoke_fn)(void *obj, void **params, void **exc, void *compiled_method);

static void *fast_invoke(void *method, void *obj, void **params, void **exc)
{
    if (!obj || !__atomic_load_n(&g_on, __ATOMIC_ACQUIRE)) return E.orig(method, obj, params, exc);
    void *domain = E.domain_get();
    void **info = domain ? find(domain, method) : NULL;
    if (!info) {
        void *r = E.orig(method, obj, params, exc);
        if (domain && __atomic_load_n(&g_on, __ATOMIC_ACQUIRE)) remember(domain, method);
        return r;
    }
    // RuntimeInvokeInfo: +4 compiled_method, +8 runtime_invoke, +12 vtable, +16 dyn_call_info (NULL here)
    rt_invoke_fn rt = (rt_invoke_fn)info[2];
    if (exc) {
        *exc = E.class_init_full(info[3], 0);
        if (*exc) return NULL;
    } else {
        E.class_init(info[3]);
    }
    return rt(obj, params, exc, info[1]);
}

void *(*monoinvoke_start(const struct monoinvoke_env *env))(void *, void *, void **, void **)
{
    E = *env;
    __atomic_store_n(&g_on, 1, __ATOMIC_RELEASE);
    return fast_invoke;
}

void monoinvoke_off(void) { __atomic_store_n(&g_on, 0, __ATOMIC_RELEASE); }

int monoinvoke_cached(void)
{
    pthread_mutex_lock(&g_tab_lock);
    int n = g_count;
    pthread_mutex_unlock(&g_tab_lock);
    return n;
}

// ---------------------------------------------------------------------------------- the builds it knows
#if defined(__arm__)
// offsets in the library (its first segment is mapped at its load bias); fnv: FNV-1a of the bytes of
// mono_jit_runtime_invoke (with its literal pool: the domain, info and method offsets above are in its
// instructions) and of mono_runtime_invoke (which reads default_mono_runtime_invoke at var)
struct build {
    const char *name;
    uint32_t jri, jri_end, ri, ri_end, var, hash_lookup, class_init_full, class_init, domain_get;
    uint32_t fnv_jri, fnv_ri;
};
static const struct build g_builds[] = {
    { "Unity 2017.4 armeabi-v7a", 0x21924, 0x225d8, 0x1f7680, 0x1f7744, 0x39a5c0, 0x2a3490, 0x1f07a8, 0x1f0784, 0x16f830,
      0x49de81b8, 0x85afe431 },
};

static uint32_t fnv(const unsigned char *p, uint32_t n)
{
    uint32_t h = 0x811c9dc5u;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

static void install(const void *in_mono)
{
    const char *e = getenv("UA_MONO_INVOKE");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[mono] invoke cache: off (UA_MONO_INVOKE=0)\n"); return; }
    uintptr_t bias = 0;
    const char *path = so_module_at(in_mono, &bias);
    if (!path) return;
    const unsigned char *base = (const unsigned char *)bias;
    const struct build *b = NULL;
    for (size_t i = 0; i < sizeof g_builds / sizeof *g_builds && !b; i++)
        if (fnv(base + g_builds[i].jri, g_builds[i].jri_end - g_builds[i].jri) == g_builds[i].fnv_jri &&
            fnv(base + g_builds[i].ri, g_builds[i].ri_end - g_builds[i].ri) == g_builds[i].fnv_ri) b = &g_builds[i];
    if (!b) { fprintf(stderr, "[mono] invoke cache: off (a libmono build it has not been checked against)\n"); return; }
    void **var = (void **)(base + b->var);
    if (*var != (void *)(base + b->jri)) {
        fprintf(stderr, "[mono] invoke cache: off (the runtime invoke is not Mono's own: %p)\n", *var);
        return;
    }
    struct monoinvoke_env env = {
        .orig = (void *(*)(void *, void *, void **, void **))(base + b->jri),
        .domain_get = (void *(*)(void))(base + b->domain_get),
        .hash_lookup = (void *(*)(void *, const void *))(base + b->hash_lookup),
        .class_init_full = (void *(*)(void *, int))(base + b->class_init_full),
        .class_init = (void (*)(void *))(base + b->class_init),
        .lock = (int (*)(void *))so_shim_lookup("pthread_mutex_lock"),
        .unlock = (int (*)(void *))so_shim_lookup("pthread_mutex_unlock"),
        .domain_lock_off = 4, .jit_info_off = 0xdc, .invoke_hash_off = 0x28,
    };
    if (!env.lock || !env.unlock) { fprintf(stderr, "[mono] invoke cache: off (no bionic mutex shims)\n"); return; }
    __atomic_store_n(var, (void *)monoinvoke_start(&env), __ATOMIC_SEQ_CST);
    fprintf(stderr, "[mono] invoke cache: on (%s libmono; UA_MONO_INVOKE=0 turns it off)\n", b->name);
}
#else
static void install(const void *in_mono) { (void)in_mono; }
#endif

static void *(*r_jit_init_version)(const char *, const char *);
static void *w_jit_init_version(const char *file, const char *version)
{
    void *domain = r_jit_init_version(file, version);
    install((const void *)r_jit_init_version);
    return domain;
}

static void (*r_domain_unload)(void *);
static void w_domain_unload(void *domain)
{
    if (__atomic_exchange_n(&g_on, 0, __ATOMIC_ACQ_REL))
        fprintf(stderr, "[mono] invoke cache: off from now on (a domain unload; %d methods were cached)\n", monoinvoke_cached());
    r_domain_unload(domain);
}

void *monoinvoke_dlsym_hook(const char *symbol, void *found)
{
    if (!found || strncmp(symbol, "mono_", 5)) return found;
    if (!strcmp(symbol, "mono_jit_init_version") && found != (void *)w_jit_init_version) {
        r_jit_init_version = (void *(*)(const char *, const char *))found;
        return (void *)w_jit_init_version;
    }
    if (!strcmp(symbol, "mono_domain_unload") && found != (void *)w_domain_unload) {
        r_domain_unload = (void (*)(void *))found;
        return (void *)w_domain_unload;
    }
    return found;
}
