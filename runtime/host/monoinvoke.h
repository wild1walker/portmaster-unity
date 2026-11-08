// monoinvoke.c: Mono's runtime invoke (how libunity calls every Update, LateUpdate, ... of the game's scripts)
// through a lock-free per-method cache of the RuntimeInvokeInfo Mono itself made, instead of the domain
// lock and hash lookup each call takes. Exact: the same class-init call, wrapper and compiled code, in the
// same order. Only for the libmono builds it has been checked against (UA_MONO_INVOKE=0: off).
#pragma once

// for the host's dlsym hook (so_set_dlsym_hook): mono_jit_init_version (installed after it) and mono_domain_unload
void *monoinvoke_dlsym_hook(const char *symbol, void *found);
// for the loader's load hook: UA_MONO_TLS, libmono's thread-local reads patched once it is relocated
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
};
// the replacement for mono_jit_runtime_invoke with env (copied); monoinvoke_off() turns it into a plain pass-through
void *(*monoinvoke_start(const struct monoinvoke_env *env))(void *, void *, void **, void **);
void monoinvoke_off(void);
int monoinvoke_cached(void);   // methods in the cache
