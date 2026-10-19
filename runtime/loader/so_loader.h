/*
 * so_loader.h - ELF loader for Android ARMv7 (armeabi-v7a) shared libraries on
 * 32-bit ARM glibc Linux (armhf). Part of the Cuphead Unity-Android port.
 *
 * Ideas from the PS Vita so_loader (so_util.c) and PortMaster's gmloader;
 * the code is our own.
 *
 * Symbol resolution order for an import of module M:
 *   1. the per-module shim table passed to so_load()
 *   2. the shim tables registered with so_add_shims(), in registration order
 *      (so register android_shims before bionic_shims to let them win)
 *   3. Android modules loaded with so_load(): M's DT_NEEDED modules first, then all
 *   4. the host (glibc + host libraries opened for M's DT_NEEDED, via dlsym);
 *      GL functions taking floats are wrapped for the softfp ABI (see below).
 *      Weak imports are never resolved from the host (Android semantics).
 *   5. unresolved: weak -> 0; functions -> a stub that prints the name and aborts;
 *      data objects -> a zeroed block. SO_TRACE=1 lists them at load time.
 * Symbols a module defines itself are bound to its own definition (as the Android
 * linker does for these libraries) unless the per-module shim table overrides them.
 *
 * ABI WARNING: armeabi-v7a code uses the *softfp* calling convention (float and
 * double arguments/results in core registers), our code is hard-float. Every C
 * function that Android code calls (shims, JNI functions) or that we call in
 * Android code (JNI natives, callbacks) with float/double arguments or results must
 * be declared with SO_ABI. Variadic functions are unaffected.
 */
#ifndef SO_LOADER_H
#define SO_LOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__arm__) && defined(__ARM_PCS_VFP)
#define SO_ABI __attribute__((pcs("aapcs")))
#else
#define SO_ABI
#endif

typedef struct {
    const char *name;
    void *fn;            /* function or data address */
} so_shim;               /* tables are terminated by { NULL, NULL } */

typedef struct so_module so_module;

/* --- core API (DESIGN.md) --- */
void       so_add_shims(const so_shim *table);
so_module *so_load(const char *path, const so_shim *module_shims /* may be NULL */);
void      *so_sym(so_module *m, const char *name);   /* exported definitions only */
void       so_run_init(so_module *m);                 /* deps first, DT_INIT, DT_INIT_ARRAY */
void       so_run_fini(so_module *m);                 /* DT_FINI_ARRAY (reverse), DT_FINI */

/* --- extras --- */
const char *so_error(void);                 /* last so_load/so_* error (thread-local) */
so_module  *so_find(const char *name);      /* by soname or file basename */
const char *so_name(so_module *m);
uintptr_t   so_base(so_module *m);          /* load bias */
size_t      so_size(so_module *m);
int         so_unresolved_count(so_module *m);
/* calls cb for each unresolved non-weak import of m */
void        so_foreach_unresolved(so_module *m, void (*cb)(const char *name, int is_func, void *ud), void *ud);
/* global lookup: shims, Android modules, host (softfp-wrapped). NULL if not found */
void       *so_resolve(const char *name);
/* lookup in the registered shim tables only */
void       *so_shim_lookup(const char *name);

/* Directories (':' separated) searched for Android libraries by name: used for
 * DT_NEEDED of Android modules and for dlopen() calls made by Android code.
 * Also settable with env SO_LIBRARY_PATH. Directories of loaded modules are
 * searched too. */
void so_set_library_path(const char *dirs);

/* Map an Android library name (basename, e.g. "libGLESv2.so") to a host library
 * (e.g. "libGLESv2.so.2", or an absolute path). host == NULL: dlopen fails for it. */
void so_map_library(const char *android_name, const char *host);

/* Register a fake Android library (e.g. "liblog.so", "libOpenSLES.so"): dlopen()
 * of that name by Android code returns a handle whose dlsym() searches this table
 * (then the global shims). Android-only libraries that are not registered still
 * get a handle that searches the global shims. */
void so_register_fake_lib(const char *android_name, const so_shim *table);

/* Returns a softfp-callable wrapper for a host function that takes or returns
 * floats (GL/GLES, libm), or fn itself if it needs none. Used for dlsym results
 * and by eglGetProcAddress shims (the EGL glue should call this too). */
void *so_softfp_wrap(const char *name, void *fn);

/* ARM EHABI unwinding support: finds the .ARM.exidx table for pc in our modules
 * (then in host objects). Exported under the libgcc/glibc name by so_loader.c:
 * link the host executable with
 *     -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx
 * (or -rdynamic) so that the host libgcc_s unwinder, which libunity uses for C++
 * exceptions, can unwind through Android code. */
uintptr_t so_find_exidx(uintptr_t pc, int *pcount);

/* dl* shims used by Android code (also part of bionic_shims). Handles are loader
 * handles, not glibc ones. The bionic RTLD_DEFAULT/RTLD_NEXT values are handled. */
void *so_dlopen(const char *filename, int bionic_flags);
void *so_dlsym(void *handle, const char *symbol);
/* debugging: every symbol Android code gets through dlsym passes through hook(symbol, found),
 * which may return a wrapper instead */
void so_set_dlsym_hook(void *(*hook)(const char *symbol, void *found));
int   so_dlclose(void *handle);
char *so_dlerror(void);

/* hybris_compat.c: bionic -> glibc compatibility (libhybris's hooks plus the remaining bionic
 * symbols, the dl* shims, setjmp, ctype tables, properties: SO_PROP_<name, '.' -> '_'> in the
 * environment overrides a property). Register it after the android/egl/jni tables:
 *     so_add_shims(bionic_compat_table()); */
const so_shim *bionic_compat_table(void);

#ifdef __cplusplus
}
#endif
#endif
