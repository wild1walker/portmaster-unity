/*
 * so_loader.h - ELF loader for Android ARM shared libraries on ARM glibc Linux:
 * armeabi-v7a (ELF32) on armhf, arm64-v8a (ELF64) on aarch64. A build loads the
 * libraries of its own architecture. Part of the Cuphead Unity-Android port.
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
 * Relocation tables: REL and RELA, Android's packed ones (DT_ANDROID_REL / _RELA,
 * "APS2"), RELR (DT_RELR, and Android's earlier DT_ANDROID_RELR); IRELATIVE and
 * IFUNC symbols. ELF TLS (PT_TLS) is refused: NDK code for API levels below 29 uses
 * emulated TLS (__emutls_get_address, linked into each library).
 *
 * ABI WARNING (32-bit ARM): armeabi-v7a code uses the *softfp* calling convention
 * (float and double arguments/results in core registers), our code is hard-float.
 * Every C function that Android code calls (shims, JNI functions) or that we call in
 * Android code (JNI natives, callbacks) with float/double arguments or results must
 * be declared with SO_ABI. Variadic functions are unaffected. On AArch64 Android and
 * Linux share AAPCS64 and SO_ABI is empty.
 *
 * AArch64 THREAD SLOTS: bionic keeps per-thread slots at the thread pointer
 * (tpidr_el0), and NDK code built with the stack protector loads its canary from
 * slot 5, tpidr_el0 + 0x28, on entry to and exit from every protected function
 * (32-bit ARM code reads the global __stack_chk_guard instead, a shim). glibc's
 * tpidr_el0 points at its 16-byte TCB (the dtv pointer and an unused word), followed
 * by the executable's static TLS block, then the libraries' (errno, ...). A canary
 * that changes while a function runs aborts it, so so_loader.c makes that word ours:
 * it defines the executable's first static TLS block, 48 bytes standing for bionic's
 * slots 2-7 (tpidr_el0 + 16 .. + 63), with slot 5 holding a fixed canary that each
 * new thread copies from the executable's TLS image. The block comes first because
 * it is the executable's only initialized thread-local data (.tdata precedes .tbss).
 * Rules for the host program, checked by every so_load() (which fails with an
 * explanation otherwise):
 *   - libsoloader.a is linked into the executable itself, not into a shared library;
 *   - no other thread-local variable of the executable has an initial value
 *     (`__thread int x = 1;`) or an alignment above 16.
 * The other slots: 0-1 are glibc's TCB, which only bionic's libc reads; 3-4 (OpenGL)
 * belong to Android's GLES wrapper libraries, 6 to HWASan, 7 to ART, none of which run
 * here; their words are reserved and zero.
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
/* the Android module holding addr: its path and load bias (addr - bias = the ELF address that
 * symbol files use), else NULL. Takes no lock (safe while a sampled thread is stopped anywhere) */
const char *so_module_at(const void *addr, uintptr_t *bias);

/* Directories (':' separated) searched for Android libraries by name: used for
 * DT_NEEDED of Android modules and for dlopen() calls made by Android code.
 * Also settable with env SO_LIBRARY_PATH. Directories of loaded modules are
 * searched too. */
void so_set_library_path(const char *dirs);

/* Map an Android library name (basename, e.g. "libGLESv2.so") to a host library
 * (e.g. "libGLESv2.so.2", or an absolute path). host == NULL: dlopen fails for it, also for an
 * Android-only library the shims serve (e.g. "libmediandk.so") unless a table was registered for
 * it with so_register_fake_lib; a DT_NEEDED entry of it is skipped, and imports still resolve
 * through the shim tables. */
void so_map_library(const char *android_name, const char *host);

/* Register a fake Android library (e.g. "liblog.so", "libOpenSLES.so"): dlopen()
 * of that name by Android code returns a handle whose dlsym() searches this table
 * (then the global shims). Android-only libraries that are not registered still
 * get a handle that searches the global shims. */
void so_register_fake_lib(const char *android_name, const so_shim *table);

/* Returns a softfp-callable wrapper for a host function that takes or returns
 * floats (GL/GLES, libm), or fn itself if it needs none. Used for dlsym results
 * and by eglGetProcAddress shims (the EGL glue should call this too). AArch64: fn. */
void *so_softfp_wrap(const char *name, void *fn);
/* A wrapper that needs no host function of that name (it calls host code statically), for names
 * the host lacks: bionic's __isfinite, __isfinitef. NULL otherwise, and on AArch64. */
void *so_softfp_static(const char *name);

/* ARM EHABI unwinding support (32-bit ARM): finds the .ARM.exidx table for pc in our
 * modules (then in host objects). Exported under the libgcc/glibc name by so_loader.c:
 * link the host executable with
 *     -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx
 * (or -rdynamic) so that the host libgcc_s unwinder, which libunity uses for C++
 * exceptions, can unwind through Android code.
 * AArch64 unwinds with .eh_frame instead, and arm64 Android libraries carry their own
 * unwinder (bionic never exported one on LP64), which finds frames through
 * dl_iterate_phdr: bionic_compat_table() routes that to the loader, which reports our
 * modules, then the host's. so_find_exidx returns 0 there. */
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
