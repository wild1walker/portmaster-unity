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
 * which adds a softfp -> hardfp thunk for the names that pass floats.
 */
#include "../thirdparty/libhybris/hybris/common/hooks.c"
#include "so_loader.h"

#include <limits.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <sys/random.h>
#include <sys/statfs.h>
#include <sys/stat.h>

/* bionic_setjmp.S: setjmp/longjmp with bionic's 256-byte jmp_buf */
int bionic_setjmp(void *), bionic__setjmp(void *), bionic_sigsetjmp(void *, int);
void bionic_longjmp(void *, int), bionic__longjmp(void *, int), bionic_siglongjmp(void *, int);
int __cxa_atexit(void (*)(void *), void *, void *);

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

/* bionic_setjmp.S keeps the kernel's 64-bit signal mask in two words of the jmp_buf */
void bionic_jmp_savemask(uint32_t *mask) { syscall(SYS_rt_sigprocmask, SIG_BLOCK, NULL, mask, 8); }
void bionic_jmp_restoremask(const uint32_t *mask) { syscall(SYS_rt_sigprocmask, SIG_SETMASK, mask, NULL, 8); }

static void bionic_assert2(const char *file, int line, const char *function, const char *expr)
{
    fprintf(stderr, "%s:%d: %s: assertion \"%s\" failed\n", file, line, function, expr);
    abort();
}

static int bionic_tkill(int tid, int sig) { return syscall(SYS_tkill, tid, sig); }
static int *bionic_get_h_errno(void) { return __h_errno_location(); }
static int bionic_aeabi_atexit(void *obj, void (*dtor)(void *), void *dso) { return __cxa_atexit(dtor, obj, dso); }
static sighandler_t bionic_bsd_signal(int sig, sighandler_t h) { return signal(sig, h); }

/* 32-bit bionic struct stat / statfs have the kernel's 64-bit layouts = glibc's *64 structs */
static int bionic_stat(const char *p, struct stat64 *st) { return stat64(p, st); }
static int bionic_lstat(const char *p, struct stat64 *st) { return lstat64(p, st); }
static int bionic_fstat(int fd, struct stat64 *st) { return fstat64(fd, st); }
static int bionic_fstatat(int dirfd, const char *p, struct stat64 *st, int flags) { return fstatat64(dirfd, p, st, flags); }
static int bionic_statfs(const char *p, struct statfs64 *st) { return statfs64(p, st); }
static int bionic_fstatfs(int fd, struct statfs64 *st) { return fstatfs64(fd, st); }

static void mask_in(sigset_t *g, const uint32_t *b);
static void mask_out(uint32_t *b, const sigset_t *g);

/* bionic sigset_t is one 32-bit word */
static int bionic_pthread_sigmask(int how, const uint32_t *set, uint32_t *old)
{
    sigset_t s, o;
    mask_in(&s, set);
    int r = pthread_sigmask(how, set ? &s : NULL, &o);
    if (r == 0 && old) mask_out(old, &o);
    return r;
}

/* bionic signal types (32-bit ARM): sigset_t is one word (signals 1-32), struct sigaction is
 * { handler, mask, flags, restorer } = 16 bytes; glibc's mask alone is 128 bytes, so glibc must
 * never see the bionic structs (libhybris leaves its signal hooks disabled for this reason). */
struct bionic_sigaction { void *handler; uint32_t mask; int flags; void *restorer; };

static void mask_in(sigset_t *g, const uint32_t *b) { sigemptyset(g); if (b) for (int i = 0; i < 32; i++) if (*b & (1u << i)) sigaddset(g, i + 1); }
static void mask_out(uint32_t *b, const sigset_t *g) { *b = 0; for (int i = 0; i < 32; i++) if (sigismember(g, i + 1) == 1) *b |= 1u << i; }

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

static int bionic_sigprocmask(int how, const uint32_t *set, uint32_t *old)
{
    sigset_t s, o;
    mask_in(&s, set);
    int r = sigprocmask(how, set ? &s : NULL, &o);
    if (r == 0 && old) mask_out(old, &o);
    return r;
}

static int bionic_sigsuspend(const uint32_t *set)
{
    sigset_t s;
    mask_in(&s, set);
    return sigsuspend(&s);
}

/* bionic's sem_t is one 32-bit word that *is* the semaphore (bionic/libc/bionic/semaphore.cpp):
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

/* bionic mbstate_t is 4 bytes, glibc's 8: a per-thread glibc state stands in for the caller's
 * (Unity converts whole strings; a sequence split across calls keeps its state per thread) */
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
    { "ro.build.version.sdk", "19" }, { "ro.build.version.release", "4.4.4" },
    { "ro.product.model", "RG34XX-SP" }, { "ro.product.manufacturer", "Anbernic" },
    { "ro.product.brand", "Anbernic" }, { "ro.product.device", "rgsp" },
    { "ro.product.cpu.abi", "armeabi-v7a" }, { "ro.product.cpu.abi2", "armeabi" },
    { "ro.hardware", "sun50iw9" }, { "ro.board.platform", "sun50iw9" },
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

/* ---- pthread mutexes and condition variables ------------------------------------------
 * bionic's pthread_mutex_t / pthread_cond_t are one 32-bit word, glibc's much larger, so (as in
 * libhybris) the word holds a pointer to a real glibc object, made on first use when the word
 * still holds a bionic static initializer. libhybris installs that pointer with a plain store:
 * two threads first touching the same object at once each make their own (mutual exclusion
 * broken, a signal sent to a condition nobody waits on). Here it is a compare-and-swap.
 * Condition variables also remember their clock (pthread_condattr_setclock), so relative and
 * *_monotonic waits use the right one (libhybris always assumes CLOCK_REALTIME). */
static pthread_mutex_t *mutex_of(void *m)
{
    uintptr_t v = __atomic_load_n((uintptr_t *)m, __ATOMIC_ACQUIRE);
    while (v <= ANDROID_TOP_ADDR_VALUE_MUTEX)
    {
        pthread_mutex_t *r = hybris_alloc_init_mutex(v);
        if (__atomic_compare_exchange_n((uintptr_t *)m, &v, (uintptr_t)r, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return r;
        pthread_mutex_destroy(r);       /* another thread won: use its mutex (now in v) */
        free(r);
    }
    return hybris_is_pointer_in_shm((void *)v) ? (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)v)
                                                : (pthread_mutex_t *)v;
}
#define MUTEX_SHARED_WITH_ANDROID(m) hybris_check_android_shared_mutex(__atomic_load_n((uintptr_t *)(m), __ATOMIC_RELAXED))
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
    uintptr_t v = __atomic_load_n((uintptr_t *)m, __ATOMIC_ACQUIRE);
    if (hybris_check_android_shared_mutex(v)) return 0;
    if (v <= ANDROID_TOP_ADDR_VALUE_MUTEX) return EPERM;     /* never locked */
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
    uintptr_t v = __atomic_exchange_n((uintptr_t *)m, 0, __ATOMIC_ACQ_REL);
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
    uintptr_t v = __atomic_load_n((uintptr_t *)c, __ATOMIC_ACQUIRE);
    while (v <= ANDROID_TOP_ADDR_VALUE_COND)
    {
        struct bcond *b = bcond_new(NULL);
        if (!b) abort();
        if (__atomic_compare_exchange_n((uintptr_t *)c, &v, (uintptr_t)b, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return b;
        pthread_cond_destroy(&b->c);
        free(b);
    }
    return (struct bcond *)v;
}
#define COND_SHARED_WITH_ANDROID(c) hybris_check_android_shared_cond(__atomic_load_n((uintptr_t *)(c), __ATOMIC_RELAXED))
static int bionic_cond_init(void *c, const pthread_condattr_t *attr)
{
    struct bcond *b = bcond_new(attr);
    if (!b) return ENOMEM;
    __atomic_store_n((uintptr_t *)c, (uintptr_t)b, __ATOMIC_RELEASE);
    return 0;
}
static int bionic_cond_destroy(void *c)
{
    uintptr_t v = __atomic_exchange_n((uintptr_t *)c, 0, __ATOMIC_ACQ_REL);
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

static const so_shim ours[] = {
    { "dlopen", (void *)so_dlopen }, { "dlsym", (void *)so_dlsym }, { "dlclose", (void *)so_dlclose },
    { "dlerror", (void *)so_dlerror },
    { "setjmp", (void *)bionic_setjmp }, { "_setjmp", (void *)bionic__setjmp }, { "sigsetjmp", (void *)bionic_sigsetjmp },
    { "longjmp", (void *)bionic_longjmp }, { "_longjmp", (void *)bionic__longjmp }, { "siglongjmp", (void *)bionic_siglongjmp },
    { "__page_size", &bionic_page_size }, { "__stack_chk_guard", &bionic_stack_chk_guard },
    { "_ctype_", &bionic_ctype_ }, { "_tolower_tab_", &bionic_tolower_tab_ }, { "_toupper_tab_", &bionic_toupper_tab_ },
    { "__assert2", (void *)bionic_assert2 }, { "tkill", (void *)bionic_tkill }, { "__get_h_errno", (void *)bionic_get_h_errno },
    { "__aeabi_atexit", (void *)bionic_aeabi_atexit }, { "bsd_signal", (void *)bionic_bsd_signal },
    { "stat", (void *)bionic_stat }, { "lstat", (void *)bionic_lstat }, { "fstat", (void *)bionic_fstat },
    { "fstatat", (void *)bionic_fstatat }, { "statfs", (void *)bionic_statfs }, { "fstatfs", (void *)bionic_fstatfs },
    { "pthread_sigmask", (void *)bionic_pthread_sigmask }, { "sigaction", (void *)bionic_sigaction },
    { "sigprocmask", (void *)bionic_sigprocmask }, { "sigsuspend", (void *)bionic_sigsuspend },
    { "pthread_mutex_lock", (void *)bionic_mutex_lock }, { "pthread_mutex_trylock", (void *)bionic_mutex_trylock },
    { "pthread_mutex_unlock", (void *)bionic_mutex_unlock }, { "pthread_mutex_timedlock", (void *)bionic_mutex_timedlock },
    { "pthread_mutex_lock_timeout_np", (void *)bionic_mutex_lock_timeout_np }, { "pthread_mutex_destroy", (void *)bionic_mutex_destroy },
    { "pthread_cond_init", (void *)bionic_cond_init }, { "pthread_cond_destroy", (void *)bionic_cond_destroy },
    { "pthread_cond_signal", (void *)bionic_cond_signal }, { "pthread_cond_broadcast", (void *)bionic_cond_broadcast },
    { "pthread_cond_wait", (void *)bionic_cond_wait }, { "pthread_cond_timedwait", (void *)bionic_cond_timedwait },
    { "pthread_cond_clockwait", (void *)bionic_cond_clockwait },
    { "pthread_cond_timedwait_monotonic", (void *)bionic_cond_timedwait_monotonic },
    { "pthread_cond_timedwait_monotonic_np", (void *)bionic_cond_timedwait_monotonic },
    { "pthread_cond_timedwait_relative_np", (void *)bionic_cond_timedwait_relative_np },
    { "sem_init", (void *)bionic_sem_init }, { "sem_destroy", (void *)bionic_sem_destroy }, { "sem_post", (void *)bionic_sem_post },
    { "sem_wait", (void *)bionic_sem_wait }, { "sem_trywait", (void *)bionic_sem_trywait },
    { "sem_timedwait", (void *)bionic_sem_timedwait }, { "sem_getvalue", (void *)bionic_sem_getvalue },
    { "mbrtowc", (void *)bionic_mbrtowc }, { "wcrtomb", (void *)bionic_wcrtomb }, { "ungetwc", (void *)bionic_ungetwc },
    { "getnameinfo", (void *)bionic_getnameinfo }, { "__system_property_get", (void *)bionic_system_property_get },
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
