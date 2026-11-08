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

// ---------------------------------------------------------------------------------- UA_MONO_TLS
// Mono's thread-local reads: every managed-to-native call of the JIT's code (the icall wrappers' LMF) goes
// through mono_get_lmf_addr, and the runtime through mono_domain_get; both read their slot with
// mono_native_tls_get_value: pthread_getspecific(keys[id]) through libmono's import, which the loader binds to
// libhybris's hook (bionic's key 0 is never a created key: NULL, else glibc's pthread_getspecific). Unity 2017.4's
// libmono is built without optimisation, so the three are each a frame of loads and stores around one call:
// about 0.6 ms of the fight's frame on the RG34XX-SP with the hook. The replacements compute the same value
// with the same glibc call: keys[id] read each time, 0 gives NULL; mono_get_lmf_addr returns &jit_tls->lmf
// (+8) when the thread has its JIT data and otherwise calls the original (it attaches the thread). Patched in
// at load, before any of libmono's code runs, only into a libmono whose three functions match the checked
// build byte for byte, and only while the import is the loader's hook and libhybris logs nothing per call.
#if defined(__arm__)
#include "unityhooks.h"
static const uint32_t *g_keys, *g_jit_tls_id, *g_domain_tls_id;
static void *(*g_lmf_orig)(void);

static void *tls_get(uint32_t id)
{
    uint32_t k = g_keys[id];
    return k ? pthread_getspecific((pthread_key_t)k) : NULL;
}
static void *fast_get_lmf_addr(void)
{
    char *jit_tls = tls_get(*g_jit_tls_id);
    return jit_tls ? jit_tls + 8 : g_lmf_orig();
}
static void *fast_domain_get(void) { return tls_get(*g_domain_tls_id); }

struct tls_build {
    const char *name;
    uint32_t tls_get, tls_get_end, lmf, lmf_end, dom, dom_end;   // the three functions (with their literal pools)
    uint32_t keys, got_jit_tls_id, domain_tls_id, got_getspecific;
    uint32_t fnv_tls_get, fnv_lmf, fnv_dom;
};
static const struct tls_build g_tls_builds[] = {
    { "Unity 2017.4 armeabi-v7a", 0x28c1f4, 0x28c234, 0x1a69c, 0x1a760, 0x16f830, 0x16f85c,
      0x39f394, 0x3997f4, 0x39a49c, 0x399e9c, 0x6a744fa1, 0x8fc04136, 0xe42766e6 },
};

static void tls_on_load(uintptr_t base, size_t size)
{
    const char *e = getenv("UA_MONO_TLS");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[mono] fast thread-local reads: off (UA_MONO_TLS=0)\n"); return; }
    const struct tls_build *b = NULL;
    for (size_t i = 0; i < sizeof g_tls_builds / sizeof *g_tls_builds && !b; i++) {
        const struct tls_build *t = &g_tls_builds[i];
        if (t->dom_end > size || t->tls_get_end > size || t->lmf_end > size) continue;
        if (fnv((const unsigned char *)base + t->tls_get, t->tls_get_end - t->tls_get) == t->fnv_tls_get &&
            fnv((const unsigned char *)base + t->lmf, t->lmf_end - t->lmf) == t->fnv_lmf &&
            fnv((const unsigned char *)base + t->dom, t->dom_end - t->dom) == t->fnv_dom) b = t;
    }
    if (!b) { fprintf(stderr, "[mono] fast thread-local reads: off (a libmono build it has not been checked against)\n"); return; }
    const char *lv = getenv("HYBRIS_LOGGING_LEVEL");
    void *hook = so_shim_lookup("pthread_getspecific");
    if (lv && *lv) { fprintf(stderr, "[mono] fast thread-local reads: off (HYBRIS_LOGGING_LEVEL: the hook logs)\n"); return; }
    if (!hook || *(void **)(base + b->got_getspecific) != hook) {
        fprintf(stderr, "[mono] fast thread-local reads: off (libmono's pthread_getspecific is not the loader's hook)\n");
        return;
    }
    g_keys = (const uint32_t *)(base + b->keys);
    g_jit_tls_id = *(const uint32_t **)(base + b->got_jit_tls_id);
    g_domain_tls_id = (const uint32_t *)(base + b->domain_tls_id);
    void *o1 = NULL, *o2 = NULL;
    if (unityhooks_hook(base + b->lmf, 'a', (void *)fast_get_lmf_addr, (void **)&g_lmf_orig) != 0 ||
        unityhooks_hook(base + b->tls_get, 'a', (void *)tls_get, &o1) != 0 ||
        unityhooks_hook(base + b->dom, 'a', (void *)fast_domain_get, &o2) != 0) {
        fprintf(stderr, "[mono] fast thread-local reads: FAILED to patch libmono\n");
        return;
    }
    fprintf(stderr, "[mono] fast thread-local reads: on (%s libmono; UA_MONO_TLS=0 turns them off)\n", b->name);
}
#endif

void monoinvoke_on_load(so_module *m)
{
#if defined(__arm__)
    const char *name = so_name(m);
    if (name && !strcmp(name, "libmono.so")) tls_on_load(so_base(m), so_size(m));
#else
    (void)m;
#endif
}

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
