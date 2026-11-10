// darwin.h - macOS as the loaded images see it, answered on Linux: the images' libSystem and
// CoreFoundation imports bound to shims (sh_*.c) that speak Darwin's arm64 ABI (darwin_abi.h:
// its numbers and layouts, read from the SDK) to the caller and glibc's below; the images'
// libc++ imports bound to LLVM's libc++ built as Mach-O (cxx/build-cxx-macho.sh) and loaded
// the same way. docs/macho-route.md.
//
// Rules every shim keeps:
//   - a return value is never narrower than int: Darwin callers read a bool or a short without
//     extending it (the callee extends there), AAPCS64 callees leave the upper bits as they are;
//   - variadic shims read their arguments from the stack in 8-byte slots (darwin_va.h);
//   - errno numbers, flags and layouts are Darwin's on the image's side, always;
//   - what is not answered is said once, by name, on stderr: never a silent default.
#ifndef UA_DARWIN_H
#define UA_DARWIN_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "darwin_abi.h"
#include "macho.h"

struct darwin_sym { const char *name; void *addr; };
#define SHIM(n)     { "_" #n, (void *)darwin_##n }      // answered here
#define SAME(n)     { "_" #n, (void *)n }               // glibc's own: the same ABI and meaning
#define NAMED(s, f) { s, (void *)f }
// one table per area, each closed by a NULL name
extern const struct darwin_sym darwin_syms_errno[], darwin_syms_mem[], darwin_syms_stdio[],
    darwin_syms_file[], darwin_syms_thread[], darwin_syms_time[], darwin_syms_locale[],
    darwin_syms_misc[], darwin_syms_net[], darwin_syms_dyld[], darwin_syms_cf[], darwin_syms_tlv[],
    darwin_syms_signal[];
// A libSystem or CoreFoundation import's answer ("_open"); NULL: none.
void *darwin_lookup(const char *name);
int   darwin_shim_count(void);

// errno: the images' cell holds Darwin's numbers; glibc's own is folded into it (and cleared)
// each time an image asks for the cell, so a call glibc failed since is seen and a value the
// image stored itself stays.
int  *darwin_error(void);
int   darwin_errno_of(int host);            // a Linux errno number as Darwin's
int   darwin_errno_host(int darwin);        // and back (strerror)
void  darwin_set_errno(int darwin);
int   darwin_strerror_r(int darwin, char *buf, size_t len);     // macOS's text for its number
// For shims that return an error number (pthread_*, posix_memalign).
#define DARWIN_ERR(host) darwin_errno_of(host)

void darwin_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// A call that has no answer here was made: said once per name.
void darwin_unanswered(const char *name, const char *what);

// dyld: the loaded images, in load order. They are brought up as a set, as a launch does:
// darwin_dyld_load for each (the libraries before what uses them), darwin_dyld_link once, then
// darwin_dyld_run_initializers.
struct darwin_image {
    struct macho_image *m;
    int      linked, inited;
    int      tlv_slot;                      // -1: no thread-local variables
    uint8_t *tlv_start;                     // their initial contents in the image
    size_t   tlv_size, tlv_align;
    long     a64_found, a64_rewritten;
};
// Maps path and makes its binds: an import from a loaded image's library (by install name) to
// that image's export, every other to its shim. NULL with a message in err; an import with no
// answer fails the load, each named on stderr.
struct darwin_image *darwin_dyld_load(const char *path, char *err, size_t errlen);
// For every image loaded since the last call: the coalesced names bound to one definition for
// all (the first image, in load order, that defines the name for good; else the first that
// defines it at all), the instructions an older core lacks rewritten (UA_A64FIX), protections,
// thread-locals, unwind info. 0, or -1 with a message.
int   darwin_dyld_link(char *err, size_t errlen);
void  darwin_dyld_run_initializers(void);   // of the linked images that have not run theirs, in load order
int   darwin_dyld_count(void);
struct darwin_image *darwin_dyld_image(int i);
struct darwin_image *darwin_dyld_image_at(const void *addr);
void  darwin_dyld_set_args(int argc, char **argv, char **envp);
void  darwin_dyld_set_executable(const char *path);     // _NSGetExecutablePath's answer (default: this process's)
// An export by its Mach-O name ("_il2cpp_init") in every image, in load order.
void *darwin_dyld_symbol(const char *name);
// dlopen for what is not a loaded image (a game's plugin): the host's answer, a handle dlsym
// and dlclose are then given back through sym and close. Unset: such a dlopen fails, said once.
struct darwin_dlopen_host {
    void *(*open)(const char *path, int darwin_mode);
    void *(*sym)(void *handle, const char *name);
    int   (*close)(void *handle);
};
void  darwin_dyld_set_dlopen(const struct darwin_dlopen_host *h);

// Thread-local variables (tlv.c). The images' descriptors call ua_tlv_get_addr, which keeps
// every register but x0, x16 and x17.
void  ua_tlv_get_addr(void);
int   darwin_tlv_setup(struct darwin_image *im, char *err, size_t errlen);
void  darwin_tlv_thread_exit(void);         // the calling thread's _tlv_atexit list, last first

// Threads: every thread that runs image code has an alternate signal stack (macOS leaves the
// 128 bytes below sp to the code, so no handler may run on that stack) and a number that
// stands for its Mach port.
void     darwin_thread_enter(void);         // idempotent; called on a thread's first shim that needs it
uint32_t darwin_thread_port(void);
// Threads are stopped (thread_suspend) by a signal here. A call the kernel does not restart
// after a handler returns EINTR then, which a Mach suspend never causes: a shim for such a call
// compares darwin_thread_stops() around it and goes on waiting when it changed. The signal must
// stay unblocked in every thread that runs the image's code.
unsigned darwin_thread_stops(void);
int   darwin_thread_stop_signal(void);

// What a thread keeps between calls (dlerror's text, strerror's, localeconv's): one heap block
// per thread, made at its first use and freed at its end. Not thread-local variables: as a
// library the host dlopens (bridge/), the runtime's thread-local block must fit the little
// static TLS glibc keeps spare, so only what a signal handler or the thunks read is in it.
struct darwin_thread_bufs {
    char dl_error[512];
    int  dl_failed;
    char strerror[256];
    unsigned char lconv[DARWIN_SIZEOF_LCONV];
    char grouping[32], mon_grouping[32];
};
struct darwin_thread_bufs *darwin_thread_bufs(void);

// Page size: macOS on arm64 has 16 KiB pages and code built for it may count on that; mmap's
// shim hands out memory aligned so.
size_t   darwin_page_size(void);

// Paths: macOS's file systems do not tell upper case from lower, and code built for it may
// name a file in another case than the file has. A path that names nothing as spelled is
// matched without case, component by component (sh_file.c): path itself, or buf (PATH_MAX
// bytes) with the spelling found; what follows the last component that exists stays as given.
const char *darwin_path(const char *path, char *buf);

// A locale as the images hold it (sh_locale.c): glibc's own locale_t behind it.
void    *darwin_locale_host(void *darwin_locale);
// Runs with the thread's locale set to a Darwin locale_t (NULL: the C locale).
#define DARWIN_WITH_LOCALE(loc, stmt) do { \
        locale_t ua_old_ = uselocale((locale_t)darwin_locale_host(loc)); stmt; uselocale(ua_old_); } while (0)

#endif
