// UA_MONO_INVOKE: Mono's runtime invoke through a per-method cache (see monoinvoke.h).
//
// libunity calls every Update, LateUpdate, FixedUpdate, coroutine step ... of the game's scripts through
// mono_runtime_invoke (about 1,200 calls a frame in Enter the Gungeon's fights). Unity 2017.4's (32-bit)
// libmono was built without optimisation, and each call of its mono_jit_runtime_invoke takes the domain lock,
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
// the lookup, which change nothing. A NULL instance (but a static method's: Unity's own static callbacks, its
// coroutines' InvokeMoveNext, the cameras' FireOnPreCull ...), a wrapper or dynamic method, an info on Mono's dynamic
// call path (dyn_call_info: never here, AOT-only), and every call after a domain unload go to the
// original. It needs the builds' private offsets, so it runs only on a libmono whose two functions match a
// checked build byte for byte, after mono_jit_init_version has installed the original.
//
// The dispatch thunk: libunity's own import of mono_runtime_invoke (dlsym) is answered with
// monoinvoke_thunk. libmono's mono_runtime_invoke (unoptimised, like the rest) asks mono_runtime_get_no_exec
// and mono_profiler_get_events (method events) before the call and the events again after it, and calls the
// runtime invoke through default_mono_runtime_invoke (the cache's replacement above), which calls
// mono_domain_get and then mono_runtime_class_init(_full): frames that return at once once the class is
// initialised (MonoVTable's initialized bit; with exc given, *exc = NULL). The thunk does all of that in one
// frame with the cache's entry: the two flags read from libmono's own variables (the getters' bytes are
// checked: they read them and nothing else), the class's initialisation called only while the vtable's bit
// is clear, the profiler's end call when the events are on after the call. Every other case (no-exec mode,
// method events on, the runtime invoke not the cache's, a NULL instance of an instance method, a method not cached yet) is
// libmono's mono_runtime_invoke itself. UA_MONO_INVOKE=cache: the cache without the thunk.
#define _GNU_SOURCE
#include "monoinvoke.h"
#include "../loader/so_loader.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

// MonoMethod.flags (offset 0) & METHOD_ATTRIBUTE_STATIC: a static method's NULL instance is no warning case
// (mono_jit_runtime_invoke tests these first), it goes on as any other call
#define METHOD_STATIC(m) (*(const uint16_t *)(m) & 0x10)

static void *fast_invoke(void *method, void *obj, void **params, void **exc)
{
    if ((!obj && !METHOD_STATIC(method)) || !__atomic_load_n(&g_on, __ATOMIC_ACQUIRE)) return E.orig(method, obj, params, exc);
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

// the dispatch thunk's switch and libmono's own mono_runtime_invoke (what the thunk calls in every case it does
// not take itself: before monoinvoke_start, from the dlsym hook's answer)
static int g_thunk;
static void *(*g_ri)(void *, void *, void **, void **);

void *(*monoinvoke_start(const struct monoinvoke_env *env))(void *, void *, void **, void **)
{
    E = *env;
    if (E.runtime_invoke) g_ri = E.runtime_invoke;
    __atomic_store_n(&g_on, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_thunk, E.runtime_invoke && E.no_exec && E.prof_events && E.rt_var && E.prof_end &&
                     E.vt_initialized, __ATOMIC_RELEASE);
    return fast_invoke;
}

void monoinvoke_off(void)
{
    __atomic_store_n(&g_thunk, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_on, 0, __ATOMIC_RELEASE);
}

int monoinvoke_cached(void)
{
    pthread_mutex_lock(&g_tab_lock);
    int n = g_count;
    pthread_mutex_unlock(&g_tab_lock);
    return n;
}

// ---------------------------------------------------------------------------------- the builds it knows
// None yet on arm64: the checked builds (Unity 2017.4's armeabi-v7a libmono, with its UA_MONO_TLS and UA_MONO_DIV
// patches) went with armhf (tag last-armhf). A build is added here with its offsets once read and checked.
static void install(const void *in_mono) { (void)in_mono; }

// ---------------------------------------------------------------------------------- the dispatch thunk
// libunity's mono_runtime_invoke (see the top): libmono's own (g_ri) in every case it does not take itself
#define METHOD_EVENTS 0x10000                  // MONO_PROFILE_METHOD_EVENTS
void *monoinvoke_thunk(void *method, void *obj, void **params, void **exc)
{
    if ((!obj && !METHOD_STATIC(method)) || !__atomic_load_n(&g_thunk, __ATOMIC_ACQUIRE) || *E.no_exec || (*E.prof_events & METHOD_EVENTS) ||
        *(void *volatile const *)E.rt_var != (void *)fast_invoke || !__atomic_load_n(&g_on, __ATOMIC_ACQUIRE))
        return g_ri(method, obj, params, exc);
    void *domain;
    domain = E.domain_get();
    void **info = domain ? find(domain, method) : NULL;
    if (!info) return g_ri(method, obj, params, exc);   // a first call: the original (and the cache remembers it)
    // what mono_jit_runtime_invoke does with the info (fast_invoke), the class's initialisation called only while the
    // vtable's initialized bit is clear (mono_runtime_class_init_full returns NULL at once when it is set)
    rt_invoke_fn rt = (rt_invoke_fn)info[2];
    void *r = NULL;
    if (((const unsigned char *)info[3])[E.vt_flags_off] & E.vt_initialized) {
        if (exc) *exc = NULL;
        r = rt(obj, params, exc, info[1]);
    } else if (exc) {
        *exc = E.class_init_full(info[3], 0);
        if (!*exc) r = rt(obj, params, exc, info[1]);
    } else {
        E.class_init(info[3]);
        r = rt(obj, params, exc, info[1]);
    }
    if (*E.prof_events & METHOD_EVENTS) E.prof_end(method);   // (mono_runtime_invoke's after the call)
    return r;
}


void monoinvoke_on_load(so_module *m)
{
    (void)m;
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

// UA_MONO_GSHARED=0 (a measurement): Mono's JIT without generic code sharing. libunity sets Mono's default
// optimisations from mono_parse_default_optimizations; its answer is given without the "gshared" bit (found as the
// difference between the defaults and the defaults minus "gshared"), so every generic instance is compiled for its
// own type arguments: no rgctx fetches, generic class-init trampolines or shared casts, more code and compiling.
// What may move: a beforefieldinit generic class's static constructor runs when a method touching its statics is
// compiled instead of at that code's first run (Mono's own rule for unshared code)
static uint32_t (*r_parse_opts)(const char *);
static uint32_t w_parse_opts(const char *p)
{
    uint32_t o = r_parse_opts(p), bit = r_parse_opts(NULL) & ~r_parse_opts("-gshared");
    fprintf(stderr, "[mono] generic sharing off (UA_MONO_GSHARED=0): optimisations %#x -> %#x\n", o, o & ~bit);
    return o & ~bit;
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
    if (!strcmp(symbol, "mono_parse_default_optimizations") && found != (void *)w_parse_opts) {
        const char *e = getenv("UA_MONO_GSHARED");
        if (!e || strcmp(e, "0")) return found;
        r_parse_opts = (uint32_t (*)(const char *))found;
        return (void *)w_parse_opts;
    }
    if (!strcmp(symbol, "mono_runtime_invoke") && found != (void *)monoinvoke_thunk) {
        const char *e = getenv("UA_MONO_INVOKE");
        if (e && (!strcmp(e, "0") || !strcmp(e, "cache"))) return found;
        if (!g_ri) g_ri = (void *(*)(void *, void *, void **, void **))found;
        return g_ri == found ? (void *)monoinvoke_thunk : found;
    }
    return found;
}
