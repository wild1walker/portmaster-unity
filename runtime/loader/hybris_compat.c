/*
 * hybris_compat.c - bionic -> glibc compatibility for Android libraries loaded by so_loader.
 *
 * The bulk comes from libhybris (thirdparty/libhybris, Apache License 2.0, by the libhybris
 * authors): its hook functions (hybris/common/hooks.c: pthread objects, stdio __sF, errno,
 * properties, signals, ...) are compiled into this file so their tables can be offered to the
 * loader as a shim table. libhybris normally runs with its own Android linker and Android's
 * system image; the few bionic symbols that setup covers elsewhere are added here, from the
 * import analysis in ../spec/imports.md.
 *
 * Resolution order (main program): android/egl/jni shims, then bionic_shims (below: ours first,
 * then libhybris's), then the host. Every function handed out goes through so_softfp_wrap(),
 * which adds a softfp -> hardfp thunk for the names that pass floats (32-bit ARM only).
 *
 * Both of bionic's ARM ABIs are handled: 32-bit ARM (LP32) and arm64 (LP64). Types whose bionic
 * layout differs from glibc's, and the arch each difference applies to, are noted where they are
 * translated; the rest (struct stat on arm64, dirent, tm, timespec, addrinfo after libhybris's
 * swap, Dl_info, dl_phdr_info, stack_t, siginfo_t, ucontext_t, pthread_attr_t behind libhybris's
 * pointer, the attribute objects glibc writes itself) match or never reach glibc as bionic objects.
 *
 * Players after 2017.4 (NDK r16b-r23b) add: bionic's own fortify entry points (__FD_SET_chk,
 * __strlen_chk, ...), android_set_abort_message, __mmap2, sys_signame/sys_siglist, and calls whose
 * flag values or 32-bit structures differ from glibc's (nftw/ftw, fnmatch, pathconf, newlocale,
 * getpw*, statvfs, mknod, the mbstate_t string conversions, sem_open). See "newer players" below;
 * the float-returning ones (wcstod, strtold_l, ...) are softfp wrappers in softfp.c. Older ones
 * (5.6 - 2017.1) import the SHA1Init/SHA1Update/SHA1Final that bionic's libc exported then.
 */
#include "../thirdparty/libhybris/hybris/common/hooks.c"
#include "so_loader.h"

#include <fnmatch.h>
#include <ftw.h>
#include <limits.h>
#include <link.h>
#include <math.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/stat.h>

/* bionic_setjmp.S: setjmp/longjmp with bionic's 256-byte jmp_buf (both ABIs: 64 or 32 longs) */
int bionic_setjmp(void *), bionic__setjmp(void *), bionic_sigsetjmp(void *, int);
void bionic_longjmp(void *, int), bionic__longjmp(void *, int), bionic_siglongjmp(void *, int);
int __cxa_atexit(void (*)(void *), void *, void *);

/* so_loader.c: the dynamic linker's functions for Android code */
int so_dladdr(const void *addr, Dl_info *info);
int so_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data);
uintptr_t so_dl_unwind_find_exidx(uintptr_t pc, int *pcount);
void *so_android_dlopen_ext(const char *filename, int flags, const void *extinfo);

/* ---- data the Android libraries import ------------------------------------------------- */

unsigned int bionic_page_size = 4096;              /* __page_size */
uintptr_t bionic_stack_chk_guard = 0x5ca1ab1e;      /* __stack_chk_guard (set at init) */

/* OpenBSD/bionic ctype: char[1 + 256] indexed by c + 1 (EOF first), bit flags as below;
 * _ctype_ / _tolower_tab_ / _toupper_tab_ are *pointers* to the tables in bionic. */
#define B_U 0x01
#define B_L 0x02
#define B_N 0x04
#define B_S 0x08
#define B_P 0x10
#define B_C 0x20
#define B_X 0x40
#define B_B 0x80
static char ctype_table[257];
static short tolower_table[257], toupper_table[257];
const char *bionic_ctype_ = ctype_table;
const short *bionic_tolower_tab_ = tolower_table;
const short *bionic_toupper_tab_ = toupper_table;

static void init_tables(void)
{
    /* the C locale classification, the way bionic's ctype_.c lists it */
    for (int c = 0; c < 256; c++)
    {
        char f = 0;
        if (c < 128)
        {
            if (c >= 'A' && c <= 'Z') f |= B_U;
            if (c >= 'a' && c <= 'z') f |= B_L;
            if (c >= '0' && c <= '9') f |= B_N;
            if (c == ' ' || (c >= '\t' && c <= '\r')) f |= B_S;
            if (c > ' ' && c < 127 && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) f |= B_P;
            if (c < ' ' || c == 127) f |= B_C;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= B_X;
            if (c == ' ') f |= B_B;
        }
        ctype_table[c + 1] = f;
        tolower_table[c + 1] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        toupper_table[c + 1] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    ctype_table[0] = 0; tolower_table[0] = -1; toupper_table[0] = -1;
    uintptr_t g = 0;
    if (getrandom(&g, sizeof g, 0) == sizeof g && g) bionic_stack_chk_guard = g;
}

/* ---- functions ------------------------------------------------------------------------- */

/* bionic_setjmp.S keeps the kernel's 64-bit signal mask in the jmp_buf (two words on 32-bit ARM, one on arm64) */
void bionic_jmp_savemask(void *mask) { syscall(SYS_rt_sigprocmask, SIG_BLOCK, NULL, mask, 8); }
void bionic_jmp_restoremask(const void *mask) { syscall(SYS_rt_sigprocmask, SIG_SETMASK, mask, NULL, 8); }

static void bionic_assert2(const char *file, int line, const char *function, const char *expr)
{
    fprintf(stderr, "%s:%d: %s: assertion \"%s\" failed\n", file, line, function, expr);
    abort();
}

static int bionic_tkill(int tid, int sig) { return syscall(SYS_tkill, tid, sig); }
static int *bionic_get_h_errno(void) { return __h_errno_location(); }
static int bionic_aeabi_atexit(void *obj, void (*dtor)(void *), void *dso) { return __cxa_atexit(dtor, obj, dso); }
static sighandler_t bionic_bsd_signal(int sig, sighandler_t h) { return signal(sig, h); }

/* bionic's struct stat / statfs are the kernel's: on 32-bit ARM its 64-bit layouts, which are glibc's
 * *64 structs; on arm64 the generic ones, which glibc's plain and *64 structs both are. bionic's *64
 * functions (API 21) take the same structs. */
static int bionic_stat(const char *p, struct stat64 *st) { return stat64(p, st); }
static int bionic_lstat(const char *p, struct stat64 *st) { return lstat64(p, st); }
static int bionic_fstat(int fd, struct stat64 *st) { return fstat64(fd, st); }
static int bionic_fstatat(int dirfd, const char *p, struct stat64 *st, int flags) { return fstatat64(dirfd, p, st, flags); }
static int bionic_statfs(const char *p, struct statfs64 *st) { return statfs64(p, st); }
static int bionic_fstatfs(int fd, struct statfs64 *st) { return fstatfs64(fd, st); }

/* ---- signals --------------------------------------------------------------------------
 * bionic's sigset_t is one unsigned long: signals 1-32 on 32-bit ARM, 1-64 on arm64 (the kernel's
 * set there); glibc's is 128 bytes, so glibc must never see a bionic set or struct sigaction
 * (libhybris leaves its signal hooks disabled for this reason; glibc's sigemptyset alone would
 * clear 128 bytes of the caller's 4 or 8): every function taking one is translated here.
 * struct sigaction is { handler, mask, flags, restorer } on 32-bit ARM and
 * { flags, handler, mask, restorer } on arm64. The *64 functions of API 28 are left out. */
typedef unsigned long bionic_sigset_t;
#define BSIG_BITS (8 * sizeof(bionic_sigset_t))
#if defined(__LP64__)
struct bionic_sigaction { int flags; void *handler; bionic_sigset_t mask; void *restorer; };
#else
struct bionic_sigaction { void *handler; bionic_sigset_t mask; int flags; void *restorer; };
#endif

static void mask_in(sigset_t *g, const bionic_sigset_t *b)
{
    sigemptyset(g);
    if (b) for (unsigned i = 0; i < BSIG_BITS; i++) if (*b & (1ul << i)) sigaddset(g, i + 1);
}
static void mask_out(bionic_sigset_t *b, const sigset_t *g)
{
    *b = 0;
    for (unsigned i = 0; i < BSIG_BITS; i++) if (sigismember(g, i + 1) == 1) *b |= 1ul << i;
}

static int bionic_pthread_sigmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old)
{
    sigset_t s, o;
    mask_in(&s, set);
    int r = pthread_sigmask(how, set ? &s : NULL, &o);
    if (r == 0 && old) mask_out(old, &o);
    return r;
}

static int bionic_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *old)
{
    struct sigaction ga, go;
    memset(&ga, 0, sizeof ga);
    memset(&go, 0, sizeof go);
    if (act)
    {
        ga.sa_handler = (sighandler_t)act->handler;   /* same slot as sa_sigaction */
        mask_in(&ga.sa_mask, &act->mask);
        ga.sa_flags = act->flags & ~0x04000000;       /* SA_RESTORER: glibc adds its own */
    }
    int r = sigaction(sig, act ? &ga : NULL, old ? &go : NULL);
    if (r == 0 && old)
    {
        old->handler = (void *)go.sa_handler;
        mask_out(&old->mask, &go.sa_mask);
        old->flags = go.sa_flags & ~0x04000000;
        old->restorer = NULL;
    }
    return r;
}

static int bionic_sigprocmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old)
{
    sigset_t s, o;
    mask_in(&s, set);
    int r = sigprocmask(how, set ? &s : NULL, &o);
    if (r == 0 && old) mask_out(old, &o);
    return r;
}

static int bionic_sigsuspend(const bionic_sigset_t *set)
{
    sigset_t s;
    mask_in(&s, set);
    return sigsuspend(&s);
}

/* the set operations: functions in bionic (32-bit NDK headers before API 21 inline them) */
static int bsig_bit(const void *set, int sig)
{
    if (!set || sig < 1 || sig > (int)BSIG_BITS) { errno = EINVAL; return -1; }
    return sig - 1;
}
static int bionic_sigemptyset(bionic_sigset_t *s) { if (!s) { errno = EINVAL; return -1; } *s = 0; return 0; }
static int bionic_sigfillset(bionic_sigset_t *s) { if (!s) { errno = EINVAL; return -1; } *s = ~0ul; return 0; }
static int bionic_sigaddset(bionic_sigset_t *s, int sig) { int b = bsig_bit(s, sig); if (b < 0) return -1; *s |= 1ul << b; return 0; }
static int bionic_sigdelset(bionic_sigset_t *s, int sig) { int b = bsig_bit(s, sig); if (b < 0) return -1; *s &= ~(1ul << b); return 0; }
static int bionic_sigismember(const bionic_sigset_t *s, int sig) { int b = bsig_bit(s, sig); return b < 0 ? -1 : (int)((*s >> b) & 1); }

static int bionic_sigpending(bionic_sigset_t *set)
{
    sigset_t g;
    int r = sigpending(&g);
    if (r == 0) mask_out(set, &g);
    return r;
}
static int bionic_sigwait(const bionic_sigset_t *set, int *sig) { sigset_t g; mask_in(&g, set); return sigwait(&g, sig); }
static int bionic_sigwaitinfo(const bionic_sigset_t *set, siginfo_t *info) { sigset_t g; mask_in(&g, set); return sigwaitinfo(&g, info); }
static int bionic_sigtimedwait(const bionic_sigset_t *set, siginfo_t *info, const struct timespec *t)
{
    sigset_t g;
    mask_in(&g, set);
    return sigtimedwait(&g, info, t);
}
static int bionic_signalfd(int fd, const bionic_sigset_t *set, int flags) { sigset_t g; mask_in(&g, set); return signalfd(fd, &g, flags); }

/* bionic's sem_t is one 32-bit word that *is* the semaphore (bionic/libc/bionic/semaphore.cpp;
 * on arm64 three reserved words follow it, 16 bytes in all, 4-byte aligned):
 * bit 0 = process-shared, bits 1..31 = the signed value, -1 meaning "0 with waiters". Zeroed
 * memory is a valid semaphore with value 0, which Android code relies on (Mono posts semaphores
 * it never sem_init'ed), so the word can't hold a pointer to a glibc sem_t. Same algorithm here,
 * on futexes; same results and errno as bionic for a target SDK <= 23 (EINTR restarts). */
#define BSEM_SHARED 1u
#define BSEM_ONE 2u
#define BSEM_MINUS_ONE 0xfffffffeu
#define BSEM_VALUE_MAX 0x3fffffff
#define BSEM_VAL(w) ((int)(w) >> 1)
static int bsem_futex(atomic_uint *w, int op, unsigned val, const struct timespec *t, int bits)
{
    if (!(atomic_load_explicit(w, memory_order_relaxed) & BSEM_SHARED)) op |= FUTEX_PRIVATE_FLAG;
    return syscall(SYS_futex, w, op, val, t, NULL, bits);
}
static int bsem_inc(atomic_uint *w)     /* returns the old value */
{
    unsigned old = atomic_load_explicit(w, memory_order_relaxed), nw;
    do {
        if (BSEM_VAL(old) == BSEM_VALUE_MAX) break;
        nw = BSEM_VAL(old) < 0 ? BSEM_ONE | (old & BSEM_SHARED) : old + BSEM_ONE;
    } while (!atomic_compare_exchange_weak(w, &old, nw));
    return BSEM_VAL(old);
}
static int bsem_dec(atomic_uint *w)     /* old value; 0 becomes -1 (waiters), -1 stays */
{
    unsigned old = atomic_load_explicit(w, memory_order_relaxed);
    while (BSEM_VAL(old) >= 0 &&
           !atomic_compare_exchange_weak(w, &old, ((unsigned)(BSEM_VAL(old) - 1) << 1) | (old & BSEM_SHARED)))
        ;
    return BSEM_VAL(old);
}
static int bsem_trydec(atomic_uint *w)  /* old value; decrements only a positive value */
{
    unsigned old = atomic_load_explicit(w, memory_order_relaxed);
    while (BSEM_VAL(old) > 0 &&
           !atomic_compare_exchange_weak(w, &old, ((unsigned)(BSEM_VAL(old) - 1) << 1) | (old & BSEM_SHARED)))
        ;
    return BSEM_VAL(old);
}
static int bionic_sem_init(void *s, int pshared, unsigned v)
{
    if (v > BSEM_VALUE_MAX) { errno = EINVAL; return -1; }
    atomic_store((atomic_uint *)s, (v << 1) | (pshared ? BSEM_SHARED : 0));
    return 0;
}
static int bionic_sem_destroy(void *s) { (void)s; return 0; }
static int bionic_sem_post(void *s)
{
    atomic_uint *w = s;
    int old = bsem_inc(w);
    if (old < 0) bsem_futex(w, FUTEX_WAKE, INT_MAX, NULL, 0);
    else if (old == BSEM_VALUE_MAX) { errno = EOVERFLOW; return -1; }
    return 0;
}
static int bionic_sem_wait(void *s)
{
    atomic_uint *w = s;
    for (;;)
    {
        if (bsem_dec(w) > 0) return 0;
        bsem_futex(w, FUTEX_WAIT, BSEM_MINUS_ONE | (atomic_load_explicit(w, memory_order_relaxed) & BSEM_SHARED), NULL, 0);
    }
}
static int bionic_sem_trywait(void *s)
{
    if (bsem_trydec(s) > 0) return 0;
    errno = EAGAIN;
    return -1;
}
static int bionic_sem_timedwait(void *s, const struct timespec *abstime)   /* CLOCK_REALTIME */
{
    atomic_uint *w = s;
    if (bsem_trydec(w) > 0) return 0;
    if (!abstime || abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000) { errno = EINVAL; return -1; }
    for (;;)
    {
        if (bsem_dec(w) > 0) return 0;
        int r = bsem_futex(w, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME,
                           BSEM_MINUS_ONE | (atomic_load_explicit(w, memory_order_relaxed) & BSEM_SHARED),
                           abstime, FUTEX_BITSET_MATCH_ANY);
        if (r == -1 && errno == ETIMEDOUT) return -1;
    }
}
static int bionic_sem_getvalue(void *s, int *v)
{
    int x = BSEM_VAL(atomic_load((atomic_uint *)s));
    *v = x < 0 ? 0 : x;
    return 0;
}

/* bionic mbstate_t is 4 bytes on 32-bit ARM (8 on arm64), glibc's 8: a per-thread glibc state
 * stands in for the caller's (Unity converts whole strings; a sequence split across calls keeps
 * its state per thread) */
static __thread mbstate_t tls_mbstate;
static size_t bionic_mbrtowc(wchar_t *pwc, const char *s, size_t n, void *ps)
{
    (void)ps;
    return mbrtowc(pwc, s, n, &tls_mbstate);
}
static size_t bionic_wcrtomb(char *s, wchar_t wc, void *ps)
{
    (void)ps;
    return wcrtomb(s, wc, &tls_mbstate);
}

/* stdio through libhybris's FILE translation (bionic __sF entries -> glibc FILE*) */
static wint_t bionic_ungetwc(wint_t wc, FILE *fp) { return ungetwc(wc, _get_actual_fp(fp)); }

/* NI_* flags: bionic (BSD) values -> glibc */
static int bionic_getnameinfo(const struct sockaddr *sa, socklen_t len, char *host, socklen_t hl,
                              char *serv, socklen_t sl, int f)
{
    int g = 0;
    if (f & 1) g |= NI_NOFQDN;
    if (f & 2) g |= NI_NUMERICHOST;
    if (f & 4) g |= NI_NAMEREQD;
    if (f & 8) g |= NI_NUMERICSERV;
    if (f & 16) g |= NI_DGRAM;
    return getnameinfo(sa, len, host, hl, serv, sl, g);
}

/* Android system properties: SO_PROP_<name with '.' -> '_'> from the environment, else these */
static const char *const default_props[][2] = {
#if defined(__aarch64__)
    /* 64-bit Android starts at 5.0 (API 21); 22 is what the fake Java side reports */
    { "ro.build.version.sdk", "22" }, { "ro.build.version.release", "5.1.1" },
    { "ro.product.cpu.abi", "arm64-v8a" }, { "ro.product.cpu.abi2", "" },
    { "ro.product.cpu.abilist", "arm64-v8a" }, { "ro.product.cpu.abilist64", "arm64-v8a" },
    { "ro.product.cpu.abilist32", "" },
#else
    { "ro.build.version.sdk", "19" }, { "ro.build.version.release", "4.4.4" },
    { "ro.product.cpu.abi", "armeabi-v7a" }, { "ro.product.cpu.abi2", "armeabi" },
#endif
    { "ro.product.model", "RG34XX-SP" }, { "ro.product.manufacturer", "Anbernic" },
    { "ro.product.brand", "Anbernic" }, { "ro.product.device", "rgsp" },
    { "ro.hardware", "sun50iw9" }, { "ro.board.platform", "sun50iw9" },
    /* Android Frame Pacing's own off switch (Swappy in libunity 2019.3+): Swappy stays off here,
     * and Unity presents with eglSwapBuffers (android/README.md, "Frame pacing") */
    { "swappy.disable", "1" },
};
static int bionic_system_property_get(const char *name, char *value)
{
    char key[128];
    snprintf(key, sizeof key, "SO_PROP_%s", name);
    for (char *p = key; *p; p++) if (*p == '.') *p = '_';
    const char *v = getenv(key);
    for (size_t i = 0; !v && i < sizeof default_props / sizeof default_props[0]; i++)
        if (!strcmp(default_props[i][0], name)) v = default_props[i][1];
    if (!v) v = "";
    snprintf(value, 92, "%s", v);                   /* PROP_VALUE_MAX */
    return (int)strlen(value);
}

/* ---- the table ------------------------------------------------------------------------ */

/* ---- pthread mutexes, condition variables, rwlocks ----------------------------------------
 * bionic's pthread_mutex_t / pthread_cond_t / pthread_rwlock_t are 4 / 4 / 40 bytes on 32-bit ARM
 * and 40 / 48 / 56 on arm64, all only 4-byte aligned; glibc's are larger, so (as in libhybris) a
 * word of the bionic object holds a pointer to a real glibc object, made on first use while the
 * object still holds a bionic static initializer (its first 32-bit word: 0, or the recursive /
 * error-checking mutex values). libhybris installs that pointer with a plain store: two threads
 * first touching the same object at once each make their own (mutual exclusion broken, a signal
 * sent to a condition nobody waits on). Here it is a compare-and-swap, and on arm64 the word is
 * the object's first 8-byte aligned one (offset 0 or 4, inside the object either way): AArch64's
 * atomic instructions fault on misaligned addresses, and Android code is free to place these
 * objects at 4 mod 8. Condition variables also remember their clock (pthread_condattr_setclock),
 * so relative and *_monotonic waits use the right one (libhybris always assumes CLOCK_REALTIME). */
static uintptr_t *slot_of(void *obj)
{
    return (uintptr_t *)(((uintptr_t)obj + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1));
}
static uintptr_t slot_load(void *obj) { return __atomic_load_n(slot_of(obj), __ATOMIC_ACQUIRE); }
/* the static initializer, while the slot (value v) holds no pointer yet */
static unsigned init_value(void *obj, uintptr_t v)
{
    return (void *)slot_of(obj) == obj ? (unsigned)v : *(const volatile uint32_t *)obj;
}

static pthread_mutex_t *mutex_of(void *m)
{
    uintptr_t *s = slot_of(m);
    uintptr_t v = __atomic_load_n(s, __ATOMIC_ACQUIRE);
    while (v <= ANDROID_TOP_ADDR_VALUE_MUTEX)
    {
        pthread_mutex_t *r = hybris_alloc_init_mutex(init_value(m, v));
        if (__atomic_compare_exchange_n(s, &v, (uintptr_t)r, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return r;
        pthread_mutex_destroy(r);       /* another thread won: use its mutex (now in v) */
        free(r);
    }
    return hybris_is_pointer_in_shm((void *)v) ? (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)v)
                                                : (pthread_mutex_t *)v;
}
static int mutex_shared_with_android(void *m)
{
    uintptr_t v = slot_load(m);
    return v <= ANDROID_TOP_ADDR_VALUE_MUTEX && hybris_check_android_shared_mutex(init_value(m, v));
}
#define MUTEX_SHARED_WITH_ANDROID(m) mutex_shared_with_android(m)
/* libhybris's pthread_mutex_init writes the pointer at offset 0, which mutex_of() would not find on
 * arm64. Process-shared mutexes go to libhybris's shared memory, as there. */
static int bionic_mutex_init(void *m, const pthread_mutexattr_t *attr)
{
    if (!m) return EINVAL;
    int pshared = 0, r;
    if (attr) pthread_mutexattr_getpshared(attr, &pshared);
    if (pshared)
    {
        hybris_shm_pointer_t h = hybris_shm_alloc(sizeof(pthread_mutex_t));
        if (!h) return ENOMEM;
        r = pthread_mutex_init((pthread_mutex_t *)hybris_get_shmpointer(h), attr);
        if (r == 0) __atomic_store_n(slot_of(m), h, __ATOMIC_RELEASE);
        return r;
    }
    pthread_mutex_t *x = malloc(sizeof *x);
    if (!x) return ENOMEM;
    r = pthread_mutex_init(x, attr);
    if (r) { free(x); return r; }
    __atomic_store_n(slot_of(m), (uintptr_t)x, __ATOMIC_RELEASE);   /* a second init leaks the first, as in libhybris */
    return 0;
}
static int bionic_mutex_lock(void *m)
{
    if (!m) return EINVAL;
    if (MUTEX_SHARED_WITH_ANDROID(m)) return 0;
    return pthread_mutex_lock(mutex_of(m));
}
static int bionic_mutex_trylock(void *m)
{
    if (!m) return EINVAL;
    if (MUTEX_SHARED_WITH_ANDROID(m)) return 0;
    return pthread_mutex_trylock(mutex_of(m));
}
static int bionic_mutex_unlock(void *m)
{
    if (!m) return EINVAL;
    uintptr_t v = slot_load(m);
    if (v <= ANDROID_TOP_ADDR_VALUE_MUTEX)                     /* shared with Android, or never locked */
        return hybris_check_android_shared_mutex(init_value(m, v)) ? 0 : EPERM;
    return pthread_mutex_unlock(mutex_of(m));
}
static int bionic_mutex_timedlock(void *m, const struct timespec *abstime)
{
    if (!m) return EINVAL;
    if (MUTEX_SHARED_WITH_ANDROID(m)) return 0;
    return pthread_mutex_timedlock(mutex_of(m), abstime);
}
static int bionic_mutex_destroy(void *m)
{
    if (!m) return EINVAL;
    uintptr_t v = __atomic_exchange_n(slot_of(m), 0, __ATOMIC_ACQ_REL);
    if (v <= ANDROID_TOP_ADDR_VALUE_MUTEX) return 0;          /* static initializer, never used */
    if (hybris_is_pointer_in_shm((void *)v))
        return pthread_mutex_destroy((pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)v));
    int r = pthread_mutex_destroy((pthread_mutex_t *)v);
    free((void *)v);
    return r;
}
static int bionic_mutex_lock_timeout_np(void *m, unsigned msecs)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += msecs / 1000;
    t.tv_nsec += (msecs % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    int r = bionic_mutex_timedlock(m, &t);
    return r == ETIMEDOUT ? EBUSY : r;                         /* bionic's result for a timeout */
}

struct bcond { pthread_cond_t c; clockid_t clock; };
static struct bcond *bcond_new(const pthread_condattr_t *attr)
{
    struct bcond *b = malloc(sizeof *b);
    if (!b) return NULL;
    b->clock = CLOCK_REALTIME;
    if (attr) pthread_condattr_getclock(attr, &b->clock);
    pthread_cond_init(&b->c, attr);
    return b;
}
static struct bcond *cond_of(void *c)
{
    uintptr_t *s = slot_of(c);
    uintptr_t v = __atomic_load_n(s, __ATOMIC_ACQUIRE);
    while (v <= ANDROID_TOP_ADDR_VALUE_COND)
    {
        struct bcond *b = bcond_new(NULL);
        if (!b) abort();
        if (__atomic_compare_exchange_n(s, &v, (uintptr_t)b, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return b;
        pthread_cond_destroy(&b->c);
        free(b);
    }
    return (struct bcond *)v;
}
static int cond_shared_with_android(void *c)
{
    uintptr_t v = slot_load(c);
    return v <= ANDROID_TOP_ADDR_VALUE_COND ? hybris_check_android_shared_cond(init_value(c, v)) : v > HYBRIS_SHM_MASK_TOP;
}
#define COND_SHARED_WITH_ANDROID(c) cond_shared_with_android(c)
static int bionic_cond_init(void *c, const pthread_condattr_t *attr)
{
    struct bcond *b = bcond_new(attr);
    if (!b) return ENOMEM;
    __atomic_store_n(slot_of(c), (uintptr_t)b, __ATOMIC_RELEASE);
    return 0;
}
static int bionic_cond_destroy(void *c)
{
    uintptr_t v = __atomic_exchange_n(slot_of(c), 0, __ATOMIC_ACQ_REL);
    if (v <= ANDROID_TOP_ADDR_VALUE_COND) return 0;
    int r = pthread_cond_destroy(&((struct bcond *)v)->c);
    free((void *)v);
    return r;
}
static int bionic_cond_signal(void *c)
{
    if (COND_SHARED_WITH_ANDROID(c)) return android_pthread_cond_signal((android_cond_t *)c);
    return pthread_cond_signal(&cond_of(c)->c);
}
static int bionic_cond_broadcast(void *c)
{
    if (COND_SHARED_WITH_ANDROID(c)) return android_pthread_cond_broadcast((android_cond_t *)c);
    return pthread_cond_broadcast(&cond_of(c)->c);
}
static int bionic_cond_wait(void *c, void *m)
{
    if (COND_SHARED_WITH_ANDROID(c) || MUTEX_SHARED_WITH_ANDROID(m)) return 0;
    return pthread_cond_wait(&cond_of(c)->c, mutex_of(m));
}
static int bionic_cond_clockwait(void *c, void *m, clockid_t clock, const struct timespec *abstime)
{
    if (COND_SHARED_WITH_ANDROID(c) || MUTEX_SHARED_WITH_ANDROID(m)) return 0;
    return pthread_cond_clockwait(&cond_of(c)->c, mutex_of(m), clock, abstime);
}
static int bionic_cond_timedwait(void *c, void *m, const struct timespec *abstime)   /* the cond's clock */
{
    if (COND_SHARED_WITH_ANDROID(c) || MUTEX_SHARED_WITH_ANDROID(m)) return 0;
    struct bcond *b = cond_of(c);
    return pthread_cond_clockwait(&b->c, mutex_of(m), b->clock, abstime);
}
static int bionic_cond_timedwait_monotonic(void *c, void *m, const struct timespec *abstime)
{
    return bionic_cond_clockwait(c, m, CLOCK_MONOTONIC, abstime);
}
static int bionic_cond_timedwait_relative_np(void *c, void *m, const struct timespec *rel)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += rel->tv_sec;
    t.tv_nsec += rel->tv_nsec;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return bionic_cond_clockwait(c, m, CLOCK_MONOTONIC, &t);
}

static pthread_rwlock_t *rwlock_of(void *l)
{
    uintptr_t *s = slot_of(l);
    uintptr_t v = __atomic_load_n(s, __ATOMIC_ACQUIRE);
    while (v <= ANDROID_TOP_ADDR_VALUE_RWLOCK)
    {
        pthread_rwlock_t *r = hybris_alloc_init_rwlock();
        if (__atomic_compare_exchange_n(s, &v, (uintptr_t)r, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return r;
        pthread_rwlock_destroy(r);
        free(r);
    }
    return hybris_is_pointer_in_shm((void *)v) ? (pthread_rwlock_t *)hybris_get_shmpointer((hybris_shm_pointer_t)v)
                                                : (pthread_rwlock_t *)v;
}
/* libhybris's pthread_rwlockattr_init keeps a pointer to a glibc attribute object in the bionic one */
static int bionic_rwlock_init(void *l, const void *attr)
{
    pthread_rwlockattr_t *a = attr ? *(pthread_rwlockattr_t *const *)attr : NULL;
    int pshared = 0, r;
    if (a) pthread_rwlockattr_getpshared(a, &pshared);
    if (pshared)
    {
        hybris_shm_pointer_t h = hybris_shm_alloc(sizeof(pthread_rwlock_t));
        if (!h) return ENOMEM;
        r = pthread_rwlock_init((pthread_rwlock_t *)hybris_get_shmpointer(h), a);
        if (r == 0) __atomic_store_n(slot_of(l), h, __ATOMIC_RELEASE);
        return r;
    }
    pthread_rwlock_t *x = malloc(sizeof *x);
    if (!x) return ENOMEM;
    r = pthread_rwlock_init(x, a);
    if (r) { free(x); return r; }
    __atomic_store_n(slot_of(l), (uintptr_t)x, __ATOMIC_RELEASE);
    return 0;
}
static int bionic_rwlock_destroy(void *l)
{
    uintptr_t v = __atomic_exchange_n(slot_of(l), 0, __ATOMIC_ACQ_REL);
    if (v <= ANDROID_TOP_ADDR_VALUE_RWLOCK) return 0;
    if (hybris_is_pointer_in_shm((void *)v))
        return pthread_rwlock_destroy((pthread_rwlock_t *)hybris_get_shmpointer((hybris_shm_pointer_t)v));
    int r = pthread_rwlock_destroy((pthread_rwlock_t *)v);
    free((void *)v);
    return r;
}
static int bionic_rwlock_rdlock(void *l) { return pthread_rwlock_rdlock(rwlock_of(l)); }
static int bionic_rwlock_wrlock(void *l) { return pthread_rwlock_wrlock(rwlock_of(l)); }
static int bionic_rwlock_tryrdlock(void *l) { return pthread_rwlock_tryrdlock(rwlock_of(l)); }
static int bionic_rwlock_trywrlock(void *l) { return pthread_rwlock_trywrlock(rwlock_of(l)); }
static int bionic_rwlock_timedrdlock(void *l, const struct timespec *t) { return pthread_rwlock_timedrdlock(rwlock_of(l), t); }
static int bionic_rwlock_timedwrlock(void *l, const struct timespec *t) { return pthread_rwlock_timedwrlock(rwlock_of(l), t); }
static int bionic_rwlock_unlock(void *l)
{
    if (slot_load(l) <= ANDROID_TOP_ADDR_VALUE_RWLOCK) return 0;   /* never locked: libhybris's answer */
    return pthread_rwlock_unlock(rwlock_of(l));
}

/* ==== newer players (Unity 2019.4-2022.3: NDK r19-r23b) ========================================== */

/* ---- android_set_abort_message, and bionic's fortify failures ---------------------------------
 * bionic keeps the first abort message for the crash report (debuggerd's tombstone); here it is
 * printed (libunity sets one right before it aborts) and the first one kept. */
static char g_abort_message[512];
static int g_abort_message_set;
static pthread_mutex_t g_abort_lock = PTHREAD_MUTEX_INITIALIZER;

static void keep_abort_message(const char *msg)
{
    pthread_mutex_lock(&g_abort_lock);
    if (!g_abort_message_set) {
        snprintf(g_abort_message, sizeof g_abort_message, "%s", msg);
        g_abort_message_set = 1;
    }
    pthread_mutex_unlock(&g_abort_lock);
}

static void bionic_android_set_abort_message(const char *msg)
{
    if (!msg) msg = "(null)";
    fprintf(stderr, "[bionic] abort message: %s\n", msg);
    keep_abort_message(msg);
}

/* bionic's __fortify_fatal: "FORTIFY: <what>" (logcat and stderr there), the abort message, abort() */
__attribute__((noreturn, format(printf, 1, 2))) static void fortify_fatal(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "FORTIFY: %s\n", msg);
    keep_abort_message(msg);
    abort();
}

/* ---- fortify: the __*_chk functions only bionic has ---------------------------------------------
 * NDK headers with _FORTIFY_SOURCE turn calls whose buffer sizes the compiler knows into these. glibc
 * has the common ones with the same arguments (__memcpy_chk, __strcpy_chk, __vsnprintf_chk,
 * __open_2, __read_chk, ...: they resolve to the host); these are bionic's own, or (__fgets_chk,
 * __fread_chk) take their arguments in another order than glibc's. Checks and messages as in
 * bionic's fortify.cpp and private/bionic_fortify.h. */
static void check_buffer_access(const char *fn, const char *action, size_t claim, size_t actual)
{
    if (claim > actual) fortify_fatal("%s: prevented %zu-byte %s %zu-byte buffer", fn, claim, action, actual);
}
static void check_count(const char *fn, const char *what, size_t value)
{
    if (value > SSIZE_MAX) fortify_fatal("%s: %s %zu > SSIZE_MAX", fn, what, value);
}

/* bionic's fd_set: FD_SETSIZE (1024) bits in unsigned longs, as glibc's */
#define B_FD_SETSIZE 1024
#define B_NFDBITS (8 * sizeof(unsigned long))
static void check_fd_set(const char *fn, int fd, size_t set_size)
{
    if (fd < 0) fortify_fatal("%s: file descriptor %d < 0", fn, fd);
    if (fd >= B_FD_SETSIZE) fortify_fatal("%s: file descriptor %d >= FD_SETSIZE %zu", fn, fd, (size_t)B_FD_SETSIZE);
    if (set_size < B_FD_SETSIZE / 8) fortify_fatal("%s: set size %zu is too small to be an fd_set", fn, set_size);
}
static void bionic_FD_SET_chk(int fd, unsigned long *set, size_t set_size)
{
    check_fd_set("FD_SET", fd, set_size);
    set[fd / B_NFDBITS] |= 1ul << (fd % B_NFDBITS);
}
static void bionic_FD_CLR_chk(int fd, unsigned long *set, size_t set_size)
{
    check_fd_set("FD_CLR", fd, set_size);
    set[fd / B_NFDBITS] &= ~(1ul << (fd % B_NFDBITS));
}
static int bionic_FD_ISSET_chk(int fd, const unsigned long *set, size_t set_size)
{
    check_fd_set("FD_ISSET", fd, set_size);
    return (set[fd / B_NFDBITS] & (1ul << (fd % B_NFDBITS))) != 0;
}

static size_t bionic_strlen_chk(const char *s, size_t s_len)
{
    size_t n = strlen(s);
    if (n >= s_len) fortify_fatal("strlen: detected read past end of buffer");
    return n;
}
static char *bionic_strchr_chk(const char *p, int ch, size_t s_len)
{
    for (;; p++, s_len--) {
        if (s_len == 0) fortify_fatal("strchr: prevented read past end of buffer");
        if (*p == (char)ch) return (char *)p;
        if (!*p) return NULL;
    }
}
static char *bionic_strrchr_chk(const char *p, int ch, size_t s_len)
{
    for (const char *save = NULL;; p++, s_len--) {
        if (s_len == 0) fortify_fatal("strrchr: prevented read past end of buffer");
        if (*p == (char)ch) save = p;
        if (!*p) return (char *)save;
    }
}
/* BSD strlcpy/strlcat (glibc has them only from 2.38) */
static size_t b_strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}
static size_t b_strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + b_strlcpy(d + dl, s, n - dl);
}
static size_t bionic_strlcpy_chk(char *d, const char *s, size_t n, size_t d_len)
{
    check_buffer_access("strlcpy", "write into", n, d_len);
    return b_strlcpy(d, s, n);
}
static size_t bionic_strlcat_chk(char *d, const char *s, size_t n, size_t d_len)
{
    check_buffer_access("strlcat", "write into", n, d_len);
    return b_strlcat(d, s, n);
}
static char *bionic_strncpy_chk2(char *d, const char *s, size_t n, size_t d_len, size_t s_len)
{
    check_buffer_access("strncpy", "write into", n, d_len);
    if (n > s_len && strnlen(s, s_len) >= s_len) fortify_fatal("strncpy: detected read past end of %zu-byte buffer", s_len);
    return strncpy(d, s, n);
}
static char *bionic_stpncpy_chk2(char *d, const char *s, size_t n, size_t d_len, size_t s_len)
{
    check_buffer_access("stpncpy", "write into", n, d_len);
    if (n > s_len && strnlen(s, s_len) >= s_len) fortify_fatal("stpncpy: detected read past end of %zu-byte buffer", s_len);
    return stpncpy(d, s, n);
}
static void *bionic_memchr_chk(const void *s, int c, size_t n, size_t actual)
{
    check_buffer_access("memchr", "read from", n, actual);
    return memchr(s, c, n);
}
static void *bionic_memrchr_chk(const void *s, int c, size_t n, size_t actual)
{
    check_buffer_access("memrchr", "read from", n, actual);
    return memrchr(s, c, n);
}
static mode_t bionic_umask_chk(mode_t mode)
{
    if ((mode & 0777) != mode) fortify_fatal("umask: called with invalid mask %o", (unsigned)mode);
    return umask(mode);
}
static ssize_t bionic_write_chk(int fd, const void *buf, size_t count, size_t buf_size)
{
    check_count("write", "count", count);
    check_buffer_access("write", "read from", count, buf_size);
    return write(fd, buf, count);
}
/* off_t is bionic's: 32 bits on 32-bit ARM, as glibc's plain pwrite there */
static ssize_t bionic_pwrite_chk(int fd, const void *buf, size_t count, long offset, size_t buf_size)
{
    check_count("pwrite", "count", count);
    check_buffer_access("pwrite", "read from", count, buf_size);
    return pwrite(fd, buf, count, offset);
}
static ssize_t bionic_pwrite64_chk(int fd, const void *buf, size_t count, off64_t offset, size_t buf_size)
{
    check_count("pwrite64", "count", count);
    check_buffer_access("pwrite64", "read from", count, buf_size);
    return pwrite64(fd, buf, count, offset);
}
static ssize_t bionic_sendto_chk(int fd, const void *buf, size_t len, size_t buf_len, int flags,
                                 const struct sockaddr *to, socklen_t to_len)
{
    check_buffer_access("sendto", "read from", len, buf_len);
    return sendto(fd, buf, len, flags, to, to_len);
}
/* stdio: bionic's FILE through libhybris's translation; glibc's __fgets_chk is (buf, size, n, fp) */
static char *bionic_fgets_chk(char *dst, int n, FILE *fp, size_t dst_len)
{
    check_buffer_access("fgets", "write into", (size_t)n, dst_len);
    return fgets(dst, n, _get_actual_fp(fp));
}
static size_t bionic_fread_chk(void *buf, size_t size, size_t count, FILE *fp, size_t buf_size)
{
    size_t total;
    if (!__builtin_mul_overflow(size, count, &total)) check_buffer_access("fread", "write into", total, buf_size);
    return fread(buf, size, count, _get_actual_fp(fp));
}
static size_t bionic_fwrite_chk(const void *buf, size_t size, size_t count, FILE *fp, size_t buf_size)
{
    size_t total;
    if (!__builtin_mul_overflow(size, count, &total)) check_buffer_access("fwrite", "read from", total, buf_size);
    return fwrite(buf, size, count, _get_actual_fp(fp));
}

#if defined(__arm__)
/* bionic's raw mmap2 system call (offset in 4096-byte units), which Mono's libraries call directly */
static void *bionic_mmap2(void *addr, size_t size, int prot, int flags, int fd, size_t pgoff)
{
    return (void *)syscall(SYS_mmap2, addr, size, prot, flags, fd, pgoff);
}
#endif

/* ---- SHA1Init / SHA1Update / SHA1Final: the NetBSD-style SHA-1 old bionic libcs exported, which
 * libunity 5.6 - 2017.1 imports. SHA1_CTX is bionic's: uint32_t state[5], count[2]; uint8_t
 * buffer[64] (92 bytes, allocated by the caller); count is the message length in bits. */
typedef struct { uint32_t state[5], count[2]; uint8_t buffer[64]; } bionic_sha1_ctx;

#define ROL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
static void sha1_block(uint32_t st[5], const uint8_t *p)
{
    uint32_t w[80], a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else f = b ^ c ^ d, k = 0xca62c1d6;
        uint32_t t = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

static void bionic_SHA1Init(bionic_sha1_ctx *c)
{
    static const uint32_t iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(c->state, iv, sizeof iv);
    c->count[0] = c->count[1] = 0;
}

static void bionic_SHA1Update(bionic_sha1_ctx *c, const uint8_t *data, unsigned int len)
{
    unsigned int have = (c->count[0] >> 3) & 63;
    uint32_t bits = (uint32_t)len << 3;
    if ((c->count[0] += bits) < bits) c->count[1]++;
    c->count[1] += len >> 29;
    while (len) {
        unsigned int n = 64 - have < len ? 64 - have : len;
        memcpy(c->buffer + have, data, n);
        have += n; data += n; len -= n;
        if (have == 64) { sha1_block(c->state, c->buffer); have = 0; }
    }
}

static void bionic_SHA1Final(uint8_t digest[20], bionic_sha1_ctx *c)
{
    uint8_t len[8];
    for (int i = 0; i < 4; i++) len[i] = (uint8_t)(c->count[1] >> (24 - 8 * i)), len[4 + i] = (uint8_t)(c->count[0] >> (24 - 8 * i));
    static const uint8_t pad[64] = { 0x80 };
    unsigned int have = (c->count[0] >> 3) & 63;
    bionic_SHA1Update(c, pad, have < 56 ? 56 - have : 120 - have);
    bionic_SHA1Update(c, len, 8);
    for (int i = 0; i < 20; i++) digest[i] = (uint8_t)(c->state[i >> 2] >> (24 - 8 * (i & 3)));
    memset(c, 0, sizeof *c);
}

/* ---- sys_signame / sys_siglist: bionic's tables (NSIG = 65 entries; names without "SIG") ------- */
#define B_NSIG 65
#define SIGDEFS(X) \
    X(SIGHUP, "HUP", "Hangup") X(SIGINT, "INT", "Interrupt") X(SIGQUIT, "QUIT", "Quit") \
    X(SIGILL, "ILL", "Illegal instruction") X(SIGTRAP, "TRAP", "Trap") X(SIGABRT, "ABRT", "Aborted") \
    X(SIGFPE, "FPE", "Floating point exception") X(SIGKILL, "KILL", "Killed") X(SIGBUS, "BUS", "Bus error") \
    X(SIGSEGV, "SEGV", "Segmentation fault") X(SIGPIPE, "PIPE", "Broken pipe") X(SIGALRM, "ALRM", "Alarm clock") \
    X(SIGTERM, "TERM", "Terminated") X(SIGUSR1, "USR1", "User signal 1") X(SIGUSR2, "USR2", "User signal 2") \
    X(SIGCHLD, "CHLD", "Child exited") X(SIGPWR, "PWR", "Power failure") X(SIGWINCH, "WINCH", "Window size changed") \
    X(SIGURG, "URG", "Urgent I/O condition") X(SIGIO, "IO", "I/O possible") X(SIGSTOP, "STOP", "Stopped (signal)") \
    X(SIGTSTP, "TSTP", "Stopped") X(SIGCONT, "CONT", "Continue") X(SIGTTIN, "TTIN", "Stopped (tty input)") \
    X(SIGTTOU, "TTOU", "Stopped (tty output)") X(SIGVTALRM, "VTALRM", "Virtual timer expired") \
    X(SIGPROF, "PROF", "Profiling timer expired") X(SIGXCPU, "XCPU", "CPU time limit exceeded") \
    X(SIGXFSZ, "XFSZ", "File size limit exceeded") X(SIGSTKFLT, "STKFLT", "Stack fault") X(SIGSYS, "SYS", "Bad system call")
#define SIGNAME(s, n, d) [s] = n,
#define SIGDESC(s, n, d) [s] = d,
static const char *const bionic_sys_signame[B_NSIG] = { SIGDEFS(SIGNAME) };
static const char *const bionic_sys_siglist[B_NSIG] = { SIGDEFS(SIGDESC) };

/* ---- nftw / ftw: bionic's (BSD) flag and type values ----------------------------------------------
 * walk flags: PHYS 1, MOUNT 2, DEPTH 4, CHDIR 8 (glibc: CHDIR 4, DEPTH 8); types passed to the
 * callback: F 0, D 1, DNR 2, DP 3, NS 4, SL 5, SLN 6 (glibc: NS 3, SL 4, DP 5). The callback gets
 * bionic's struct stat, which is glibc's stat64 (see stat above); struct FTW is { base, level } in both.
 * A recursive delete with glibc's values would see directories as symlinks, before their contents. */
static int ftw_type_out(int t)
{
    switch (t) {
    case FTW_NS: return 4;
    case FTW_SL: return 5;
    case FTW_DP: return 3;
    default: return t;          /* F, D, DNR, SLN: the same */
    }
}
typedef int (*bionic_nftw_fn)(const char *, const struct stat64 *, int, struct FTW *);
typedef int (*bionic_ftw_fn)(const char *, const struct stat64 *, int);
static __thread bionic_nftw_fn t_nftw_fn;      /* the callback of this thread's innermost walk */
static __thread bionic_ftw_fn t_ftw_fn;
static int nftw_tramp(const char *p, const struct stat64 *st, int t, struct FTW *f) { return t_nftw_fn(p, st, ftw_type_out(t), f); }
static int ftw_tramp(const char *p, const struct stat64 *st, int t) { return t_ftw_fn(p, st, ftw_type_out(t)); }
static int bionic_nftw(const char *dir, bionic_nftw_fn fn, int nfds, int flags)
{
    if (nfds < 1) { errno = EINVAL; return -1; }
    int g = 0;
    if (flags & 1) g |= FTW_PHYS;
    if (flags & 2) g |= FTW_MOUNT;
    if (flags & 4) g |= FTW_DEPTH;
    if (flags & 8) g |= FTW_CHDIR;
    bionic_nftw_fn saved = t_nftw_fn;
    t_nftw_fn = fn;
    int r = nftw64(dir, nftw_tramp, nfds, g);
    t_nftw_fn = saved;
    return r;
}
static int bionic_ftw(const char *dir, bionic_ftw_fn fn, int nfds)
{
    if (nfds < 1) { errno = EINVAL; return -1; }
    bionic_ftw_fn saved = t_ftw_fn;
    t_ftw_fn = fn;
    int r = ftw64(dir, ftw_tramp, nfds);
    t_ftw_fn = saved;
    return r;
}

/* ---- fnmatch: bionic's (BSD) flags NOESCAPE 1, PATHNAME 2 are glibc's PATHNAME 1, NOESCAPE 2 ----- */
static int bionic_fnmatch(const char *pattern, const char *string, int flags)
{
    int g = 0;
    if (flags & 0x01) g |= FNM_NOESCAPE;
    if (flags & 0x02) g |= FNM_PATHNAME;
    if (flags & 0x04) g |= FNM_PERIOD;
    if (flags & 0x08) g |= FNM_LEADING_DIR;
    if (flags & 0x10) g |= FNM_CASEFOLD;
    return fnmatch(pattern, string, g) == 0 ? 0 : FNM_NOMATCH;
}

/* ---- pathconf: bionic numbers the _PC_* names differently (unistd.h there) ---------------------- */
static int pathconf_name(int b)
{
    static const int g[] = {
        _PC_FILESIZEBITS, _PC_LINK_MAX, _PC_MAX_CANON, _PC_MAX_INPUT, _PC_NAME_MAX, _PC_PATH_MAX,
        _PC_PIPE_BUF, _PC_2_SYMLINKS, _PC_ALLOC_SIZE_MIN, _PC_REC_INCR_XFER_SIZE, _PC_REC_MAX_XFER_SIZE,
        _PC_REC_MIN_XFER_SIZE, _PC_REC_XFER_ALIGN, _PC_SYMLINK_MAX, _PC_CHOWN_RESTRICTED, _PC_NO_TRUNC,
        _PC_VDISABLE, _PC_ASYNC_IO, _PC_PRIO_IO, _PC_SYNC_IO,
    };
    return b >= 0 && b < (int)(sizeof g / sizeof g[0]) ? g[b] : -1;
}
static long bionic_pathconf(const char *path, int name)
{
    int g = pathconf_name(name);
    if (g < 0) { errno = EINVAL; return -1; }
    return pathconf(path, g);
}
static long bionic_fpathconf(int fd, int name)
{
    int g = pathconf_name(name);
    if (g < 0) { errno = EINVAL; return -1; }
    return fpathconf(fd, g);
}

/* ---- named semaphores: bionic has none (sem_open fails with ENOSYS); a glibc one would then reach
 * the bionic sem_* functions above, which read its memory as a bionic semaphore word ---------------- */
static void *bionic_sem_open(const char *name, int flags, ...) { (void)name; (void)flags; errno = ENOSYS; return NULL; }
static int bionic_sem_close(void *s) { (void)s; errno = EINVAL; return -1; }
static int bionic_sem_unlink(const char *name) { (void)name; errno = ENOENT; return -1; }

/* ---- newlocale: bionic's locales -------------------------------------------------------------------
 * bionic (API 21+) knows "C"/"POSIX" and "C.UTF-8" ("" and "en_US.UTF-8" are the latter), refuses other
 * names with ENOENT, ignores (and keeps) the base object, and its *_l functions ignore the locale.
 * glibc's newlocale would load whatever the name says, or fail when that locale is not installed (as
 * en_US.UTF-8 often is not on a handheld's firmware: libc++ then throws), and consumes base. So: bionic's
 * checks, then glibc's C.UTF-8 (C when the host lacks it) or C. The objects are glibc's, for the *_l
 * calls (isdigit_l, strtold_l, strftime_l, ...: libc++ inside libunity 2019.4+) that go to glibc, and
 * for uselocale/duplocale/freelocale (libhybris passes them to glibc). */
static locale_t bionic_newlocale(int mask, const char *name, locale_t base)
{
    (void)base;
    if ((mask & ~LC_ALL_MASK) || !name) { errno = EINVAL; return (locale_t)0; }
    int utf8;
    if (!strcmp(name, "C") || !strcmp(name, "POSIX")) utf8 = 0;
    else if (!*name || !strcmp(name, "C.UTF-8") || !strcmp(name, "en_US.UTF-8")) utf8 = 1;
    else { errno = ENOENT; return (locale_t)0; }
    locale_t l = utf8 ? newlocale(LC_ALL_MASK, "C.UTF-8", (locale_t)0) : (locale_t)0;
    if (!l) l = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    return l;
}

#if !defined(__LP64__)
/* ---- 32-bit ARM structure layouts ---------------------------------------------------------------- */

/* struct passwd: bionic's LP32 one has no pw_gecos ({ name, passwd, uid, gid, dir, shell }); glibc's
 * has it before pw_dir. The strings stay where glibc put them (the caller's buffer for the _r forms,
 * glibc's static one otherwise); the non-_r results are per thread, as in bionic. */
struct bionic_passwd { char *pw_name, *pw_passwd; uid_t pw_uid; gid_t pw_gid; char *pw_dir, *pw_shell; };
static __thread struct bionic_passwd t_passwd;
static struct bionic_passwd *passwd_out(struct bionic_passwd *b, const struct passwd *g)
{
    b->pw_name = g->pw_name;
    b->pw_passwd = g->pw_passwd;
    b->pw_uid = g->pw_uid;
    b->pw_gid = g->pw_gid;
    b->pw_dir = g->pw_dir;
    b->pw_shell = g->pw_shell;
    return b;
}
static struct bionic_passwd *bionic_getpwuid(uid_t uid)
{
    struct passwd *g = getpwuid(uid);
    return g ? passwd_out(&t_passwd, g) : NULL;
}
static struct bionic_passwd *bionic_getpwnam(const char *name)
{
    struct passwd *g = getpwnam(name);
    return g ? passwd_out(&t_passwd, g) : NULL;
}
static int bionic_getpwuid_r(uid_t uid, struct bionic_passwd *pw, char *buf, size_t n, struct bionic_passwd **res)
{
    struct passwd g, *gr = NULL;
    int r = getpwuid_r(uid, &g, buf, n, &gr);
    *res = r == 0 && gr ? passwd_out(pw, &g) : NULL;
    return r;
}
static int bionic_getpwnam_r(const char *name, struct bionic_passwd *pw, char *buf, size_t n, struct bionic_passwd **res)
{
    struct passwd g, *gr = NULL;
    int r = getpwnam_r(name, &g, buf, n, &gr);
    *res = r == 0 && gr ? passwd_out(pw, &g) : NULL;
    return r;
}

/* struct statvfs (and statvfs64, the same in bionic): eleven unsigned longs, 44 bytes, its block and
 * file counts being unsigned long (fsblkcnt_t, fsfilcnt_t); glibc's 32-bit one has an int after f_fsid
 * and six spare ints (72 bytes), its statvfs64 64-bit counts. On arm64 both are glibc's layout. */
struct bionic_statvfs {
    unsigned long f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
};
static void statvfs_out(struct bionic_statvfs *b, const struct statvfs64 *g)
{
    b->f_bsize = g->f_bsize;
    b->f_frsize = g->f_frsize;
    b->f_blocks = (unsigned long)g->f_blocks;      /* truncated as bionic does */
    b->f_bfree = (unsigned long)g->f_bfree;
    b->f_bavail = (unsigned long)g->f_bavail;
    b->f_files = (unsigned long)g->f_files;
    b->f_ffree = (unsigned long)g->f_ffree;
    b->f_favail = (unsigned long)g->f_favail;
    b->f_fsid = g->f_fsid;
    b->f_flag = g->f_flag;
    b->f_namemax = g->f_namemax;
}
static int bionic_statvfs(const char *path, struct bionic_statvfs *b)
{
    struct statvfs64 g;
    int r = statvfs64(path, &g);
    if (r == 0) statvfs_out(b, &g);
    return r;
}
static int bionic_fstatvfs(int fd, struct bionic_statvfs *b)
{
    struct statvfs64 g;
    int r = fstatvfs64(fd, &g);
    if (r == 0) statvfs_out(b, &g);
    return r;
}

/* dev_t is 32 bits in bionic's LP32 ABI, 64 in glibc's: in mknod the high word would be r3's leftover
 * (glibc then fails with EINVAL), in mknodat a stack word */
static int bionic_mknod(const char *path, mode_t mode, uint32_t dev) { return mknod(path, mode, (dev_t)dev); }
static int bionic_mknodat(int dirfd, const char *path, mode_t mode, uint32_t dev) { return mknodat(dirfd, path, mode, (dev_t)dev); }

/* the string conversions with a state: bionic's mbstate_t is 4 bytes here, glibc writes 8. The
 * per-thread glibc state of mbrtowc above stands in for the caller's; NULL keeps glibc's internal one.
 * bionic resets a state on an invalid sequence, so this one is reset then too. */
static mbstate_t *tls_state(const void *ps) { return ps ? &tls_mbstate : NULL; }
static size_t mbs_done(size_t r, const void *ps)
{
    if (r == (size_t)-1 && ps) memset(&tls_mbstate, 0, sizeof tls_mbstate);
    return r;
}
static size_t bionic_mbrlen(const char *s, size_t n, void *ps) { return mbs_done(mbrlen(s, n, tls_state(ps)), ps); }
static size_t bionic_mbsrtowcs(wchar_t *d, const char **s, size_t n, void *ps)
{
    return mbs_done(mbsrtowcs(d, s, n, tls_state(ps)), ps);
}
static size_t bionic_mbsnrtowcs(wchar_t *d, const char **s, size_t nms, size_t n, void *ps)
{
    return mbs_done(mbsnrtowcs(d, s, nms, n, tls_state(ps)), ps);
}
static size_t bionic_wcsrtombs(char *d, const wchar_t **s, size_t n, void *ps)
{
    return mbs_done(wcsrtombs(d, s, n, tls_state(ps)), ps);
}
static size_t bionic_wcsnrtombs(char *d, const wchar_t **s, size_t nwc, size_t n, void *ps)
{
    return mbs_done(wcsnrtombs(d, s, nwc, n, tls_state(ps)), ps);
}
static int bionic_mbsinit(const void *ps) { return ps ? mbsinit(&tls_mbstate) : 1; }
#endif

#if defined(__aarch64__)
/* bionic's FP_* values differ from glibc's (FP_INFINITE 1, NAN 2, NORMAL 4, SUBNORMAL 8, ZERO 16).
 * On 32-bit ARM the softfp wrappers translate them (softfp.c); here they are shims. */
static int bionic_fp_class(int c)
{
    switch (c)
    {
    case FP_INFINITE: return 0x01;
    case FP_NAN: return 0x02;
    case FP_NORMAL: return 0x04;
    case FP_SUBNORMAL: return 0x08;
    default: return 0x10;
    }
}
static int bionic_fpclassifyd(double x) { return bionic_fp_class(fpclassify(x)); }
static int bionic_fpclassifyf(float x) { return bionic_fp_class(fpclassify(x)); }
static int bionic_fpclassifyl(long double x) { return bionic_fp_class(fpclassify(x)); }
#endif

static const so_shim ours[] = {
    { "dlopen", (void *)so_dlopen }, { "dlsym", (void *)so_dlsym }, { "dlclose", (void *)so_dlclose },
    { "dlerror", (void *)so_dlerror },
    { "setjmp", (void *)bionic_setjmp }, { "_setjmp", (void *)bionic__setjmp }, { "sigsetjmp", (void *)bionic_sigsetjmp },
    { "longjmp", (void *)bionic_longjmp }, { "_longjmp", (void *)bionic__longjmp }, { "siglongjmp", (void *)bionic_siglongjmp },
    { "__page_size", &bionic_page_size }, { "__stack_chk_guard", &bionic_stack_chk_guard },
    { "_ctype_", &bionic_ctype_ }, { "_tolower_tab_", &bionic_tolower_tab_ }, { "_toupper_tab_", &bionic_toupper_tab_ },
    { "__assert2", (void *)bionic_assert2 }, { "tkill", (void *)bionic_tkill }, { "__get_h_errno", (void *)bionic_get_h_errno },
    { "__aeabi_atexit", (void *)bionic_aeabi_atexit }, { "bsd_signal", (void *)bionic_bsd_signal },
    { "dladdr", (void *)so_dladdr }, { "dl_iterate_phdr", (void *)so_dl_iterate_phdr },
    { "android_dlopen_ext", (void *)so_android_dlopen_ext },
#if defined(__arm__)
    { "dl_unwind_find_exidx", (void *)so_dl_unwind_find_exidx },
#endif
    { "stat", (void *)bionic_stat }, { "lstat", (void *)bionic_lstat }, { "fstat", (void *)bionic_fstat },
    { "fstatat", (void *)bionic_fstatat }, { "statfs", (void *)bionic_statfs }, { "fstatfs", (void *)bionic_fstatfs },
    { "stat64", (void *)bionic_stat }, { "lstat64", (void *)bionic_lstat }, { "fstat64", (void *)bionic_fstat },
    { "fstatat64", (void *)bionic_fstatat }, { "statfs64", (void *)bionic_statfs }, { "fstatfs64", (void *)bionic_fstatfs },
    { "pthread_sigmask", (void *)bionic_pthread_sigmask }, { "sigaction", (void *)bionic_sigaction },
    { "sigprocmask", (void *)bionic_sigprocmask }, { "sigsuspend", (void *)bionic_sigsuspend },
    { "sigemptyset", (void *)bionic_sigemptyset }, { "sigfillset", (void *)bionic_sigfillset },
    { "sigaddset", (void *)bionic_sigaddset }, { "sigdelset", (void *)bionic_sigdelset },
    { "sigismember", (void *)bionic_sigismember }, { "sigpending", (void *)bionic_sigpending },
    { "sigwait", (void *)bionic_sigwait }, { "sigwaitinfo", (void *)bionic_sigwaitinfo },
    { "sigtimedwait", (void *)bionic_sigtimedwait }, { "signalfd", (void *)bionic_signalfd },
    { "pthread_mutex_init", (void *)bionic_mutex_init }, { "pthread_mutex_lock", (void *)bionic_mutex_lock }, { "pthread_mutex_trylock", (void *)bionic_mutex_trylock },
    { "pthread_mutex_unlock", (void *)bionic_mutex_unlock }, { "pthread_mutex_timedlock", (void *)bionic_mutex_timedlock },
    { "pthread_mutex_lock_timeout_np", (void *)bionic_mutex_lock_timeout_np }, { "pthread_mutex_destroy", (void *)bionic_mutex_destroy },
    { "pthread_cond_init", (void *)bionic_cond_init }, { "pthread_cond_destroy", (void *)bionic_cond_destroy },
    { "pthread_cond_signal", (void *)bionic_cond_signal }, { "pthread_cond_broadcast", (void *)bionic_cond_broadcast },
    { "pthread_cond_wait", (void *)bionic_cond_wait }, { "pthread_cond_timedwait", (void *)bionic_cond_timedwait },
    { "pthread_cond_clockwait", (void *)bionic_cond_clockwait },
    { "pthread_cond_timedwait_monotonic", (void *)bionic_cond_timedwait_monotonic },
    { "pthread_cond_timedwait_monotonic_np", (void *)bionic_cond_timedwait_monotonic },
    { "pthread_cond_timedwait_relative_np", (void *)bionic_cond_timedwait_relative_np },
    { "pthread_rwlock_init", (void *)bionic_rwlock_init }, { "pthread_rwlock_destroy", (void *)bionic_rwlock_destroy },
    { "pthread_rwlock_rdlock", (void *)bionic_rwlock_rdlock }, { "pthread_rwlock_wrlock", (void *)bionic_rwlock_wrlock },
    { "pthread_rwlock_tryrdlock", (void *)bionic_rwlock_tryrdlock }, { "pthread_rwlock_trywrlock", (void *)bionic_rwlock_trywrlock },
    { "pthread_rwlock_timedrdlock", (void *)bionic_rwlock_timedrdlock },
    { "pthread_rwlock_timedwrlock", (void *)bionic_rwlock_timedwrlock }, { "pthread_rwlock_unlock", (void *)bionic_rwlock_unlock },
    { "sem_init", (void *)bionic_sem_init }, { "sem_destroy", (void *)bionic_sem_destroy }, { "sem_post", (void *)bionic_sem_post },
    { "sem_wait", (void *)bionic_sem_wait }, { "sem_trywait", (void *)bionic_sem_trywait },
    { "sem_timedwait", (void *)bionic_sem_timedwait }, { "sem_getvalue", (void *)bionic_sem_getvalue },
    { "mbrtowc", (void *)bionic_mbrtowc }, { "wcrtomb", (void *)bionic_wcrtomb }, { "ungetwc", (void *)bionic_ungetwc },
    { "getnameinfo", (void *)bionic_getnameinfo }, { "__system_property_get", (void *)bionic_system_property_get },
    /* newer players */
    { "android_set_abort_message", (void *)bionic_android_set_abort_message },
    { "__FD_SET_chk", (void *)bionic_FD_SET_chk }, { "__FD_CLR_chk", (void *)bionic_FD_CLR_chk },
    { "__FD_ISSET_chk", (void *)bionic_FD_ISSET_chk }, { "__strlen_chk", (void *)bionic_strlen_chk },
    { "__strchr_chk", (void *)bionic_strchr_chk }, { "__strrchr_chk", (void *)bionic_strrchr_chk },
    { "__strlcpy_chk", (void *)bionic_strlcpy_chk }, { "__strlcat_chk", (void *)bionic_strlcat_chk },
    { "__strncpy_chk2", (void *)bionic_strncpy_chk2 }, { "__stpncpy_chk2", (void *)bionic_stpncpy_chk2 },
    { "__memchr_chk", (void *)bionic_memchr_chk }, { "__memrchr_chk", (void *)bionic_memrchr_chk },
    { "__umask_chk", (void *)bionic_umask_chk }, { "__write_chk", (void *)bionic_write_chk },
    { "__pwrite_chk", (void *)bionic_pwrite_chk }, { "__pwrite64_chk", (void *)bionic_pwrite64_chk },
    { "__sendto_chk", (void *)bionic_sendto_chk }, { "__fgets_chk", (void *)bionic_fgets_chk },
    { "__fread_chk", (void *)bionic_fread_chk }, { "__fwrite_chk", (void *)bionic_fwrite_chk },
    { "sys_signame", (void *)bionic_sys_signame }, { "sys_siglist", (void *)bionic_sys_siglist },
    { "SHA1Init", (void *)bionic_SHA1Init }, { "SHA1Update", (void *)bionic_SHA1Update },
    { "SHA1Final", (void *)bionic_SHA1Final },                                  /* 5.6 - 2017.1 */
    { "nftw", (void *)bionic_nftw }, { "nftw64", (void *)bionic_nftw }, { "ftw", (void *)bionic_ftw },
    { "ftw64", (void *)bionic_ftw }, { "fnmatch", (void *)bionic_fnmatch },
    { "pathconf", (void *)bionic_pathconf }, { "fpathconf", (void *)bionic_fpathconf },
    { "sem_open", (void *)bionic_sem_open }, { "sem_close", (void *)bionic_sem_close },
    { "sem_unlink", (void *)bionic_sem_unlink }, { "newlocale", (void *)bionic_newlocale },
#if defined(__arm__)
    { "__mmap2", (void *)bionic_mmap2 },
    { "getpwuid", (void *)bionic_getpwuid }, { "getpwnam", (void *)bionic_getpwnam },
    { "getpwuid_r", (void *)bionic_getpwuid_r }, { "getpwnam_r", (void *)bionic_getpwnam_r },
    { "statvfs", (void *)bionic_statvfs }, { "fstatvfs", (void *)bionic_fstatvfs },
    { "statvfs64", (void *)bionic_statvfs }, { "fstatvfs64", (void *)bionic_fstatvfs },
    { "mknod", (void *)bionic_mknod }, { "mknodat", (void *)bionic_mknodat },
    { "mbrlen", (void *)bionic_mbrlen }, { "mbsrtowcs", (void *)bionic_mbsrtowcs },
    { "mbsnrtowcs", (void *)bionic_mbsnrtowcs }, { "wcsrtombs", (void *)bionic_wcsrtombs },
    { "wcsnrtombs", (void *)bionic_wcsnrtombs }, { "mbsinit", (void *)bionic_mbsinit },
#endif
#if defined(__aarch64__)
    { "__fpclassifyd", (void *)bionic_fpclassifyd }, { "__fpclassify", (void *)bionic_fpclassifyd },
    { "__fpclassifyf", (void *)bionic_fpclassifyf }, { "__fpclassifyl", (void *)bionic_fpclassifyl },
#endif
    { NULL, NULL }
};

/* bionic_compat_table() (so_loader.h): ours, then every libhybris hook */
static so_shim *table;

const so_shim *bionic_compat_table(void)
{
    if (table) return table;
    init_tables();
    size_t n = 0, k = 0;
    for (const so_shim *s = ours; s->name; s++) n++;
    const struct _hook *lists[] = { hooks_properties, hooks_common };
    const size_t sizes[] = { HOOKS_SIZE(hooks_properties), HOOKS_SIZE(hooks_common) };
    for (int l = 0; l < 2; l++) n += sizes[l];
    table = calloc(n + 1, sizeof *table);
    for (const so_shim *s = ours; s->name; s++) table[k++] = *s;
    for (int l = 0; l < 2; l++)
        for (size_t i = 0; i < sizes[l]; i++)
        {
            int dup = 0;
            for (const so_shim *s = ours; s->name && !dup; s++) dup = !strcmp(s->name, lists[l][i].name);
            if (dup) continue;
            table[k].name = lists[l][i].name;
            table[k].fn = so_softfp_wrap(lists[l][i].name, lists[l][i].func);
            k++;
        }
    table[k].name = NULL;
    return table;
}
