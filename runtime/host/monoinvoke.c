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
#if defined(__arm__)
// offsets in the library (its first segment is mapped at its load bias); fnv: FNV-1a of the bytes of
// mono_jit_runtime_invoke (with its literal pool: the domain, info and method offsets above are in its
// instructions) and of mono_runtime_invoke (which reads default_mono_runtime_invoke at var, and calls
// mono_runtime_get_no_exec, mono_profiler_get_events and mono_profiler_method_end_invoke at prof_end). The
// dispatch thunk's: no_exec, the variable mono_runtime_get_no_exec returns (its bytes: noexec_fn, fnv_noexec);
// prof_got, the GOT slot whose value is the address of mono_profiler_events, which mono_profiler_get_events
// returns (prof_fn, fnv_prof); mono_runtime_class_init_full's start up to its early return, which tests
// MonoVTable's byte vt_flags, bit vt_initialized (fnv_cinit over class_init_full .. cinit_end)
struct build {
    const char *name;
    uint32_t jri, jri_end, ri, ri_end, var, hash_lookup, class_init_full, class_init, domain_get;
    uint32_t fnv_jri, fnv_ri;
    uint32_t noexec_fn, noexec_end, no_exec, prof_fn, prof_end_fn, prof_got, prof_end, cinit_end, vt_flags, vt_initialized;
    uint32_t fnv_noexec, fnv_prof, fnv_cinit;
};
static const struct build g_builds[] = {
    { "Unity 2017.4 armeabi-v7a", 0x21924, 0x225d8, 0x1f7680, 0x1f7744, 0x39a5c0, 0x2a3490, 0x1f07a8, 0x1f0784, 0x16f830,
      0x49de81b8, 0x85afe431,
      0x12fadc, 0x12fb04, 0x39ea20, 0x20727c, 0x2072b0, 0x3999b0, 0x207ff8, 0x1f07e4, 0x1b, 0x02,
      0xc7373094, 0x456ffa01, 0x6c9a9ab2 },
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
    // the dispatch thunk: libunity's mono_runtime_invoke is libmono's (the dlsym hook saw it), the getters read
    // the variables named and nothing else, class_init_full's early return is the one tested
    const char *thunk = "off (UA_MONO_INVOKE=cache)";
    if (e && !strcmp(e, "cache")) ;
    else if (g_ri != (void *)(base + b->ri)) thunk = "off (libunity's mono_runtime_invoke is not libmono's)";
    else if (fnv(base + b->noexec_fn, b->noexec_end - b->noexec_fn) != b->fnv_noexec ||
             fnv(base + b->prof_fn, b->prof_end_fn - b->prof_fn) != b->fnv_prof ||
             fnv(base + b->class_init_full, b->cinit_end - b->class_init_full) != b->fnv_cinit)
        thunk = "off (a libmono build it has not been checked against)";
    else {
        env.runtime_invoke = g_ri;
        env.no_exec = (const int *)(base + b->no_exec);
        env.prof_events = *(const unsigned *const *)(base + b->prof_got);
        env.rt_var = (void *const *)var;
        env.prof_end = (void (*)(void *))(base + b->prof_end);
        env.vt_flags_off = b->vt_flags;
        env.vt_initialized = b->vt_initialized;
        thunk = "on";
    }
    __atomic_store_n(var, (void *)monoinvoke_start(&env), __ATOMIC_SEQ_CST);
    fprintf(stderr, "[mono] invoke cache: on (%s libmono; UA_MONO_INVOKE=0 turns it off), dispatch thunk: %s\n", b->name, thunk);
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
// The thread's JIT data and domain are then also kept in ua_host's own thread-local variables (two instructions
// to read, no call): libmono's pthread_setspecific import goes through tls_set, which stores what libmono sets
// for those two keys on the calling thread before it returns, so the variable and the slot hold the same value
// whenever libmono reads them; a variable not set yet reads the slot once.
#if defined(__arm__)
#include "unityhooks.h"
static const uint32_t *g_keys, *g_jit_tls_id, *g_domain_tls_id;
static int g_tls_domain;                       // the hooks are on: the thread's domain is t_domain once known
static void *(*g_lmf_orig)(void);
static int (*g_setspecific)(pthread_key_t, const void *);
static __thread char *t_jit_tls;               // the slot's value once known (NULL: read the slot)
static __thread void *t_domain;
static __thread int t_domain_known;

static void *tls_get(uint32_t id)
{
    uint32_t k = g_keys[id];
    return k ? pthread_getspecific((pthread_key_t)k) : NULL;
}
// libmono's pthread_setspecific: the loader's hook, then the thread's copy of the two slots
static int tls_set(pthread_key_t k, const void *v)
{
    int r = g_setspecific(k, v);
    if (r == 0 && k) {
        if (k == (pthread_key_t)g_keys[*g_jit_tls_id]) t_jit_tls = (char *)v;
        if (k == (pthread_key_t)g_keys[*g_domain_tls_id]) { t_domain = (void *)v; t_domain_known = 1; }
    }
    return r;
}
static void *fast_get_lmf_addr(void)
{
    char *jit_tls = t_jit_tls;
    if (__builtin_expect(jit_tls != NULL, 1)) return jit_tls + 8;
    jit_tls = tls_get(*g_jit_tls_id);
    if (!jit_tls) return g_lmf_orig();
    t_jit_tls = jit_tls;
    return jit_tls + 8;
}
static void *fast_domain_get(void)
{
    if (__builtin_expect(t_domain_known, 1)) return t_domain;
    void *d = tls_get(*g_domain_tls_id);
    t_domain = d;
    t_domain_known = 1;
    return d;
}
// a GOT slot of a loaded library rewritten, whatever its page's protection
static int got_write(void **slot, void *v)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t a = (uintptr_t)slot & ~(uintptr_t)(pg - 1);
    int prot = -1;
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 && a >= lo && a < hi) {
            prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    if (f) fclose(f);
    if (prot < 0) return -1;
    if (!(prot & PROT_WRITE) && mprotect((void *)a, pg, prot | PROT_WRITE) != 0) return -1;
    __atomic_store_n(slot, v, __ATOMIC_RELEASE);
    if (!(prot & PROT_WRITE)) mprotect((void *)a, pg, prot);
    return 0;
}

struct tls_build {
    const char *name;
    uint32_t tls_get, tls_get_end, lmf, lmf_end, dom, dom_end;   // the three functions (with their literal pools)
    uint32_t keys, got_jit_tls_id, domain_tls_id, got_getspecific, got_setspecific;
    uint32_t fnv_tls_get, fnv_lmf, fnv_dom;
};
static const struct tls_build g_tls_builds[] = {
    { "Unity 2017.4 armeabi-v7a", 0x28c1f4, 0x28c234, 0x1a69c, 0x1a760, 0x16f830, 0x16f85c,
      0x39f394, 0x3997f4, 0x39a49c, 0x399e9c, 0x399e64, 0x6a744fa1, 0x8fc04136, 0xe42766e6 },
};

static const struct tls_build *g_tls_build;          // the build tls_on_load found (UA_MONO_CODE's check)
static void tls_on_load(uintptr_t base, size_t size)
{
    const struct tls_build *b = NULL;
    for (size_t i = 0; i < sizeof g_tls_builds / sizeof *g_tls_builds && !b; i++) {
        const struct tls_build *t = &g_tls_builds[i];
        if (t->dom_end > size || t->tls_get_end > size || t->lmf_end > size) continue;
        if (fnv((const unsigned char *)base + t->tls_get, t->tls_get_end - t->tls_get) == t->fnv_tls_get &&
            fnv((const unsigned char *)base + t->lmf, t->lmf_end - t->lmf) == t->fnv_lmf &&
            fnv((const unsigned char *)base + t->dom, t->dom_end - t->dom) == t->fnv_dom) b = t;
    }
    g_tls_build = b;
    const char *e = getenv("UA_MONO_TLS");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[mono] fast thread-local reads: off (UA_MONO_TLS=0)\n"); return; }
    if (!b) { fprintf(stderr, "[mono] fast thread-local reads: off (a libmono build it has not been checked against)\n"); return; }
    const char *lv = getenv("HYBRIS_LOGGING_LEVEL");
    void *hook = so_shim_lookup("pthread_getspecific");
    if (lv && *lv) { fprintf(stderr, "[mono] fast thread-local reads: off (HYBRIS_LOGGING_LEVEL: the hook logs)\n"); return; }
    if (!hook || *(void **)(base + b->got_getspecific) != hook) {
        fprintf(stderr, "[mono] fast thread-local reads: off (libmono's pthread_getspecific is not the loader's hook)\n");
        return;
    }
    void *sethook = so_shim_lookup("pthread_setspecific");
    if (!sethook || *(void **)(base + b->got_setspecific) != sethook) {
        fprintf(stderr, "[mono] fast thread-local reads: off (libmono's pthread_setspecific is not the loader's hook)\n");
        return;
    }
    g_setspecific = (int (*)(pthread_key_t, const void *))sethook;
    g_keys = (const uint32_t *)(base + b->keys);
    g_jit_tls_id = *(const uint32_t **)(base + b->got_jit_tls_id);
    g_domain_tls_id = (const uint32_t *)(base + b->domain_tls_id);
    void *o1 = NULL, *o2 = NULL;
    // the setter first: the readers' copies are only right once every store of libmono goes through tls_set
    if (got_write((void **)(base + b->got_setspecific), (void *)tls_set) != 0) {
        fprintf(stderr, "[mono] fast thread-local reads: off (libmono's pthread_setspecific slot could not be written)\n");
        return;
    }
    if (unityhooks_hook(base + b->lmf, 'a', (void *)fast_get_lmf_addr, (void **)&g_lmf_orig) != 0 ||
        unityhooks_hook(base + b->tls_get, 'a', (void *)tls_get, &o1) != 0 ||
        unityhooks_hook(base + b->dom, 'a', (void *)fast_domain_get, &o2) != 0) {
        fprintf(stderr, "[mono] fast thread-local reads: FAILED to patch libmono\n");
        return;
    }
    g_tls_domain = 1;
    fprintf(stderr, "[mono] fast thread-local reads: on (%s libmono; UA_MONO_TLS=0 turns them off)\n", b->name);
}
#endif

// ---------------------------------------------------------------------------------- UA_MONO_DIV
// Integer division: Mono 2.6's ARM JIT emulates every IL div/rem (signed and unsigned) through a JIT icall wrapper
// that calls libmono's mono_idiv, mono_idiv_un, mono_irem, mono_irem_un (MONO_ARCH_EMULATE_DIV): unoptimised C,
// each testing for division by zero (DivideByZeroException) and, signed, INT_MIN by -1 (ArithmeticException), then
// libgcc's software divide (__aeabi_idiv, __aeabi_uidivmod ...). The four are hooked at load: a divisor of 0 or -1
// goes to the original (its exceptions raised from there, as before); any other is divided here, with the CPU's own
// SDIV/UDIV where the kernel says it has them (HWCAP_IDIVA/IDIVT: the A53 and A35), else with this compiler's
// divide: the same truncating quotient and the dividend-signed remainder C and Mono's call give. Only on the
// libmono build whose four functions match byte for byte (UA_MONO_DIV=0: off).
#if defined(__arm__)
#include <sys/auxv.h>
static int g_hwdiv;
static int32_t (*o_idiv)(int32_t, int32_t), (*o_irem)(int32_t, int32_t);
static uint32_t (*o_idiv_un)(uint32_t, uint32_t), (*o_irem_un)(uint32_t, uint32_t);
static inline int32_t sdiv32(int32_t a, int32_t b)
{
    if (g_hwdiv) { int32_t q; __asm__(".arch_extension idiv\n\tsdiv %0, %1, %2" : "=r"(q) : "r"(a), "r"(b)); return q; }
    return a / b;
}
static inline uint32_t udiv32(uint32_t a, uint32_t b)
{
    if (g_hwdiv) { uint32_t q; __asm__(".arch_extension idiv\n\tudiv %0, %1, %2" : "=r"(q) : "r"(a), "r"(b)); return q; }
    return a / b;
}
static int32_t w_idiv(int32_t a, int32_t b) { if (b == 0 || b == -1) return o_idiv(a, b); return sdiv32(a, b); }
static int32_t w_irem(int32_t a, int32_t b) { if (b == 0 || b == -1) return o_irem(a, b); return a - sdiv32(a, b) * b; }
static uint32_t w_idiv_un(uint32_t a, uint32_t b) { if (b == 0) return o_idiv_un(a, b); return udiv32(a, b); }
static uint32_t w_irem_un(uint32_t a, uint32_t b) { if (b == 0) return o_irem_un(a, b); return a - udiv32(a, b) * b; }

// the four with these originals and divide (the unit test's way in; div_on_load sets them from libmono)
void monoinvoke_div_start(const struct monoinvoke_div *orig, int hw, struct monoinvoke_div *fast)
{
    o_idiv = orig->idiv; o_irem = orig->irem; o_idiv_un = orig->idiv_un; o_irem_un = orig->irem_un;
    g_hwdiv = hw;
    fast->idiv = w_idiv; fast->irem = w_irem; fast->idiv_un = w_idiv_un; fast->irem_un = w_irem_un;
}

struct div_build { const char *name; uint32_t idiv, idiv_un, irem, irem_un, end, fnv; };
static const struct div_build g_div_builds[] = {
    { "Unity 2017.4 armeabi-v7a", 0xa3998, 0xa3a10, 0xa3a5c, 0xa3ad8, 0xa3b28, 0x2f84a201 },
};
static void div_on_load(uintptr_t base, size_t size)
{
    const char *e = getenv("UA_MONO_DIV");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[mono] integer division: Mono's own (UA_MONO_DIV=0)\n"); return; }
    const struct div_build *b = NULL;
    for (size_t i = 0; i < sizeof g_div_builds / sizeof *g_div_builds && !b; i++)
        if (g_div_builds[i].end <= size &&
            fnv((const unsigned char *)base + g_div_builds[i].idiv, g_div_builds[i].end - g_div_builds[i].idiv) == g_div_builds[i].fnv)
            b = &g_div_builds[i];
    if (!b) { fprintf(stderr, "[mono] integer division: Mono's own (a libmono build it has not been checked against)\n"); return; }
    unsigned long hw = getauxval(AT_HWCAP);
    g_hwdiv = (hw & (1ul << 17)) && (hw & (1ul << 18));          // HWCAP_IDIVA and HWCAP_IDIVT
    if (unityhooks_hook(base + b->idiv, 'a', (void *)w_idiv, (void **)&o_idiv) != 0 ||
        unityhooks_hook(base + b->idiv_un, 'a', (void *)w_idiv_un, (void **)&o_idiv_un) != 0 ||
        unityhooks_hook(base + b->irem, 'a', (void *)w_irem, (void **)&o_irem) != 0 ||
        unityhooks_hook(base + b->irem_un, 'a', (void *)w_irem_un, (void **)&o_irem_un) != 0) {
        fprintf(stderr, "[mono] integer division: FAILED to patch libmono\n");
        return;
    }
    fprintf(stderr, "[mono] integer division: %s (%s libmono; UA_MONO_DIV=0 turns it off)\n",
            g_hwdiv ? "the CPU's SDIV/UDIV" : "this host's divide", b->name);
}
#endif

// ---------------------------------------------------------------------------------- the dispatch thunk
// libunity's mono_runtime_invoke (see the top): libmono's own (g_ri) in every case it does not take itself
#define METHOD_EVENTS 0x10000                  // MONO_PROFILE_METHOD_EVENTS
void *monoinvoke_thunk(void *method, void *obj, void **params, void **exc)
{
    if ((!obj && !METHOD_STATIC(method)) || !__atomic_load_n(&g_thunk, __ATOMIC_ACQUIRE) || *E.no_exec || (*E.prof_events & METHOD_EVENTS) ||
        *(void *volatile const *)E.rt_var != (void *)fast_invoke || !__atomic_load_n(&g_on, __ATOMIC_ACQUIRE))
        return g_ri(method, obj, params, exc);
    void *domain;
#if defined(__arm__)
    if (g_tls_domain && t_domain_known) domain = t_domain;
    else
#endif
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

#if defined(__arm__)          // (Unity 2017.4's armeabi-v7a libmono only, as the TLS build table above)
// ---------------------------------------------------------------------------------- UA_MONO_CODE
// Where Mono's JIT code goes: Unity 2017.4's libmono takes each code chunk with its own mmap, so the chunks of a session
// land all over the address space (Enter the Gungeon: 751 MB from the lowest method to the highest), and every call
// from one method to another more than 32 MB away (an ARM bl's reach) goes through a long-branch thunk (ldr ip,
// [pc]; bx ip): 1.6 % of the Bullet Hell fight's main thread in JIT-to-JIT thunks, 2.4 % in all (UA_PROF_CODE).
// libmono's mmap import is answered here for executable anonymous mappings from one region reserved at load (32 MB,
// no memory until used, just below libmono so its own functions are mostly in reach too), handed out in order; the
// rest, and anything once the region is full, is the original mmap. The code is the same; only where it is, and so
// which calls Mono can make directly. A mapping given back from the region is reserved again, not reused.
// Only into a libmono whose GOT slots match the checked build (UA_MONO_CODE=0: off).
#define CODE_REGION (32u << 20)
static void *(*o_mmap)(void *, size_t, int, int, int, off_t);
static int (*o_munmap)(void *, size_t);
static uintptr_t g_code_lo, g_code_hi, g_code_next;
static pthread_mutex_t g_code_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_code_full;
static void *w_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (!addr && (prot & PROT_EXEC) && (flags & MAP_ANONYMOUS) && !(flags & MAP_FIXED) && g_code_lo) {
        long pg = sysconf(_SC_PAGESIZE);
        size_t n = (len + (size_t)pg - 1) & ~((size_t)pg - 1);
        pthread_mutex_lock(&g_code_lock);
        void *r = NULL;
        if (g_code_next + n <= g_code_hi) {
            r = o_mmap((void *)g_code_next, n, prot, (flags & ~MAP_NORESERVE) | MAP_FIXED, -1, 0);
            if (r == (void *)g_code_next) g_code_next += n; else r = NULL;
        } else if (!g_code_full) { g_code_full = 1; fprintf(stderr, "[mono] code region full: Mono's mmap from here on\n"); }
        pthread_mutex_unlock(&g_code_lock);
        if (r) return r;
    }
    return o_mmap(addr, len, prot, flags, fd, off);
}
static int w_munmap(void *addr, size_t len)
{
    uintptr_t a = (uintptr_t)addr;
    if (g_code_lo && a >= g_code_lo && a + len <= g_code_hi)        // (reserved again: the region is not reused)
        return o_mmap(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == addr ? 0 : -1;
    return o_munmap(addr, len);
}
struct code_build { const char *name; uint32_t got_mmap, got_munmap; };
static const struct code_build g_code_builds[] = { { "Unity 2017.4 armeabi-v7a", 0x399f54, 0x399f58 } };
static void code_on_load(uintptr_t base, size_t size, const struct tls_build *tb)
{
    const char *e = getenv("UA_MONO_CODE");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[mono] JIT code region: off (UA_MONO_CODE=0)\n"); return; }
    const struct code_build *b = NULL;
    for (size_t i = 0; tb && i < sizeof g_code_builds / sizeof *g_code_builds && !b; i++)
        if (!strcmp(g_code_builds[i].name, tb->name) && g_code_builds[i].got_munmap + 4 <= size) b = &g_code_builds[i];
    if (!b) { fprintf(stderr, "[mono] JIT code region: off (a libmono build it has not been checked against)\n"); return; }
    void **sm = (void **)(base + b->got_mmap), **su = (void **)(base + b->got_munmap);
    if (!*sm || !*su) { fprintf(stderr, "[mono] JIT code region: off (libmono's mmap is not bound)\n"); return; }
    o_mmap = (void *(*)(void *, size_t, int, int, int, off_t))*sm;
    o_munmap = (int (*)(void *, size_t))*su;
    // the region just below libmono (a hint: anywhere else the kernel picks is still one block)
    uintptr_t hint = (base & ~(uintptr_t)0xfffff) - CODE_REGION;
    void *r = o_mmap((void *)hint, CODE_REGION, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) { fprintf(stderr, "[mono] JIT code region: off (no %u MB of address space)\n", CODE_REGION >> 20); return; }
    g_code_lo = g_code_next = (uintptr_t)r; g_code_hi = g_code_lo + CODE_REGION;
    if (got_write(sm, (void *)w_mmap) != 0 || got_write(su, (void *)w_munmap) != 0) {
        fprintf(stderr, "[mono] JIT code region: FAILED to patch libmono\n");
        g_code_lo = 0;
        return;
    }
    fprintf(stderr, "[mono] JIT code region: %u MB at %p, libmono at %p (UA_MONO_CODE=0 turns it off)\n", CODE_REGION >> 20, r, (void *)base);
}

#endif

void monoinvoke_on_load(so_module *m)
{
#if defined(__arm__)
    const char *name = so_name(m);
    if (name && !strcmp(name, "libmono.so")) {
        tls_on_load(so_base(m), so_size(m));
        div_on_load(so_base(m), so_size(m));
        code_on_load(so_base(m), so_size(m), g_tls_build);
    }
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
