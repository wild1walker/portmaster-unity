// monoinvoke.c: Mono's runtime invoke (how libunity calls every Update, LateUpdate, ... of the game's scripts)
// through a lock-free per-method cache of the RuntimeInvokeInfo Mono itself made, instead of the domain
// lock and hash lookup each call takes. Exact: the same class-init call, wrapper and compiled code, in the
// same order. Only for the libmono builds it has been checked against (UA_MONO_INVOKE=0: off).
#pragma once

// for the host's dlsym hook (so_set_dlsym_hook): mono_jit_init_version (installed after it) and mono_domain_unload
void *monoinvoke_dlsym_hook(const char *symbol, void *found);
// for the loader's load hook (nothing patched on arm64 yet)
#include "../loader/so_loader.h"
void monoinvoke_on_load(so_module *m);

// What the fast path calls, as the original mono_jit_runtime_invoke of a known build does (the test fills it
// with doubles; monoinvoke_dlsym_hook from the real libmono).
struct monoinvoke_env {
    void *(*orig)(void *method, void *obj, void **params, void **exc);   // mono_jit_runtime_invoke
    void *(*domain_get)(void);
    void *(*hash_lookup)(void *table, const void *key);                  // eglib g_hash_table_lookup
    void *(*class_init_full)(void *vtable, int raise_exception);         // mono_runtime_class_init_full
    void (*class_init)(void *vtable);                                    // mono_runtime_class_init
    int (*lock)(void *mutex), (*unlock)(void *mutex);                    // the Android pthread_mutex_lock/unlock
    unsigned domain_lock_off, jit_info_off, invoke_hash_off;             // MonoDomain: its lock, its MonoJitDomainInfo
    // the dispatch thunk (all set, or it calls runtime_invoke every time): libmono's mono_runtime_invoke, the variables
    // it reads (mono_runtime_get_no_exec's, mono_profiler_events, default_mono_runtime_invoke), its profiler end call
    // (mono_profiler_method_end_invoke), and MonoVTable's byte and bit for initialized (mono_runtime_class_init_full's
    // first test)
    void *(*runtime_invoke)(void *method, void *obj, void **params, void **exc);
    const int *no_exec;
    const unsigned *prof_events;
    void *const *rt_var;
    void (*prof_end)(void *method);
    unsigned vt_flags_off, vt_initialized;
};
// the replacement for mono_jit_runtime_invoke with env (copied); monoinvoke_off() turns it into a plain pass-through
void *(*monoinvoke_start(const struct monoinvoke_env *env))(void *, void *, void **, void **);
void monoinvoke_off(void);
// libunity's mono_runtime_invoke (the dlsym hook's answer; see monoinvoke.c, "the dispatch thunk"): libmono's own until
// monoinvoke_start with the thunk's fields, and after monoinvoke_off
void *monoinvoke_thunk(void *method, void *obj, void **params, void **exc);
int monoinvoke_cached(void);   // methods in the cache
