// libsyn_<n>.so: a synthetic "Android" library for test_syn.c. Built freestanding, without glibc's
// headers (bionic's types are written out below where they differ), -nostdlib, with the stack
// protector on every function: Android's TLS guard (tpidr_el0 + 0x28) on arm64, the global
// __stack_chk_guard on 32-bit ARM. It needs libsyn_dep.so and "libc.so" (DT_NEEDED, as bionic
// libraries do) and imports from the host (strlen, snprintf, pthread_*, sig*), from libsyn_dep.so
// and from the test's shim table. The same objects are linked as libsyn_0.so .. libsyn_4.so, whose
// relocations pack_relocs.py then rewrites (RELR, Android's APS2 packing); test_syn.c checks that
// each relocates to the same image as libsyn_0.so.
#include <stddef.h>
#include <stdint.h>

#if defined(__LP64__)
#define JBLEN 32                         // bionic's _JBLEN (longs)
#define MUTEX_WORDS 10                   // bionic's pthread_mutex_t / cond / rwlock, in int32s
#define COND_WORDS 12
#define RWLOCK_WORDS 14
#define STAT_SIZE 128                    // bionic's struct stat
#define B_RTLD_NOW 2
#define B_RTLD_DEFAULT ((void *)0)
#else
#define JBLEN 64
#define MUTEX_WORDS 1
#define COND_WORDS 1
#define RWLOCK_WORDS 10
#define STAT_SIZE 104
#define B_RTLD_NOW 0
#define B_RTLD_DEFAULT ((void *)0xffffffff)
#endif
#define SENTINEL ((long)0x5e17e1a5)

// ---- imports ----------------------------------------------------------------------------------
size_t strlen(const char *);
int snprintf(char *, size_t, const char *, ...);
int raise(int);
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
extern int dep_value, dep_array[8], dep_init_order;
extern const char *dep_name;
int dep_twice(int);
int test_next_order(void);               // the test's shim table only
int test_shim_add(int, int);
extern int test_shim_data;
__attribute__((weak)) int no_such_function(void);
__attribute__((weak)) extern int no_such_data;

typedef unsigned long b_sigset_t;        // bionic: one word
#if defined(__LP64__)
struct b_sigaction { int flags; void (*handler)(int); b_sigset_t mask; void *restorer; };
#else
struct b_sigaction { void (*handler)(int); b_sigset_t mask; int flags; void *restorer; };
#endif
int sigemptyset(b_sigset_t *), sigfillset(b_sigset_t *), sigaddset(b_sigset_t *, int);
int sigdelset(b_sigset_t *, int), sigismember(const b_sigset_t *, int), sigpending(b_sigset_t *);
int sigprocmask(int, const b_sigset_t *, b_sigset_t *);
int sigaction(int, const struct b_sigaction *, struct b_sigaction *);
#define B_SIG_BLOCK 0
#define B_SIG_UNBLOCK 1
#define B_SIG_SETMASK 2
#define B_SIGUSR1 10
#define B_SIGUSR2 12

int setjmp(long *), sigsetjmp(long *, int);
void longjmp(long *, int), siglongjmp(long *, int);

int pthread_create(long *, const void *, void *(*)(void *), void *);
int pthread_join(long, void **);
int pthread_mutex_init(void *, const void *), pthread_mutex_lock(void *), pthread_mutex_unlock(void *);
int pthread_mutex_destroy(void *);
int pthread_mutexattr_init(void *), pthread_mutexattr_settype(void *, int);
int pthread_cond_wait(void *, void *), pthread_cond_signal(void *), pthread_cond_broadcast(void *);
int pthread_rwlock_rdlock(void *), pthread_rwlock_wrlock(void *), pthread_rwlock_unlock(void *);
int pthread_rwlock_tryrdlock(void *);

int stat(const char *, void *);

struct b_dl_phdr_info {                  // bionic's = glibc's: the fields used here
    uintptr_t addr; const char *name; const void *phdr; uint16_t phnum;
};
int dl_iterate_phdr(int (*)(struct b_dl_phdr_info *, size_t, void *), void *);

// ---- data relocations -------------------------------------------------------------------------
static int local_value = 7;
static int arr[160];
static char storage[32] = "abcdefghij";
static int local_thrice(int x) { return 3 * x; }

int *syn_local_ptr = &local_value;                       // relative
int *syn_dep_ptr = &dep_value;                           // absolute, symbol
int *syn_dep_elem = &dep_array[3];                       // absolute, symbol + addend
char *syn_offset_ptr = storage + 5;                      // relative + addend
const char *syn_strings[] = { "zero", "one", "two" };
int (*syn_funcs[])(int) = { dep_twice, local_thrice };
size_t (*syn_strlen_ptr)(const char *) = strlen;         // a host function's address
int *syn_weak_ptr = &no_such_data;                       // weak, undefined: 0
int *syn_shim_data_ptr = &test_shim_data;
#define P(i) &arr[i]
#define P8(i) P(i), P(i + 1), P(i + 2), P(i + 3), P(i + 4), P(i + 5), P(i + 6), P(i + 7)
int *syn_run[80] = { P8(0), P8(8), P8(16), P8(24), P8(32), P8(40), P8(48), P8(56), P8(64), P8(72) };
int *syn_holes[24] = { P(0), 0, P(2), 0, 0, P(5), P(6), 0, 0, 0, P(10), 0, P(12), P(13), P(14), 0,
                       0, 0, 0, 0, 0, 0, 0, P(23) };    // RELR bitmaps with holes, irregular steps

static int ifunc_impl(void) { return 77; }
static int (*resolve_ifunc(void))(void) { return ifunc_impl; }
int syn_ifunc(void) __attribute__((ifunc("resolve_ifunc")));
int (*syn_ifunc_ptr)(void) = syn_ifunc;

static int init_order;
__attribute__((constructor)) static void syn_init(void) { init_order = test_next_order(); }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static char *self_addr;
static int self_cb(struct b_dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    const unsigned char *ph = info->phdr;
    for (int i = 0; i < info->phnum; i++) {
#if defined(__LP64__)
        const unsigned char *p = ph + 56 * i;
        uint32_t type = *(const uint32_t *)p;
        uintptr_t vaddr = *(const uint64_t *)(p + 16), memsz = *(const uint64_t *)(p + 40);
#else
        const unsigned char *p = ph + 32 * i;
        uint32_t type = *(const uint32_t *)p;
        uintptr_t vaddr = *(const uint32_t *)(p + 8), memsz = *(const uint32_t *)(p + 20);
#endif
        if (type == 1 && (uintptr_t)self_addr >= info->addr + vaddr && (uintptr_t)self_addr < info->addr + vaddr + memsz) {
            *(int *)data = streq(info->name + strlen(info->name) - 11, "libsyn_0.so") ? 1 : 2;
            return 1;
        }
    }
    return 0;
}

// 0 when everything holds, else a bit per failed check
int syn_check(void)
{
    int f = 0;
    if (*syn_local_ptr != 7) f |= 1 << 0;
    if (*syn_dep_ptr != 42 || syn_dep_ptr != &dep_value) f |= 1 << 1;
    if (!streq(syn_strings[0], "zero") || !streq(syn_strings[2], "two") || !streq(dep_name, "dep")) f |= 1 << 2;
    if (syn_funcs[0](5) != 10 || syn_funcs[1](5) != 15) f |= 1 << 3;
    if (syn_strlen_ptr("abc") != 3 || syn_strlen_ptr != strlen) f |= 1 << 4;
    if (no_such_function || &no_such_data || syn_weak_ptr) f |= 1 << 5;
    if (test_shim_add(2, 3) != 5 || test_shim_data != 1234 || *syn_shim_data_ptr != 1234) f |= 1 << 6;
    if (syn_ifunc() != 77 || syn_ifunc_ptr() != 77) f |= 1 << 7;
    void *h = dlopen("libsyn_dep.so", B_RTLD_NOW);
    if (!h || dlsym(h, "dep_twice") != (void *)dep_twice || dlsym(h, "no_such_symbol")) f |= 1 << 8;
    if (dlsym(B_RTLD_DEFAULT, "dep_value") != (void *)&dep_value) f |= 1 << 9;
    if (!dep_init_order || init_order <= dep_init_order) f |= 1 << 10;
    for (int i = 0; i < 80; i++) if (syn_run[i] != &arr[i]) f |= 1 << 11;
    for (int i = 0; i < 24; i++) if (syn_holes[i] && syn_holes[i] != &arr[i]) f |= 1 << 11;
    if (!syn_holes[23] || syn_holes[1] || syn_holes[22]) f |= 1 << 11;
    if (*syn_dep_elem != 30 || *syn_offset_ptr != 'f') f |= 1 << 12;
    char buf[16];
    if (snprintf(buf, sizeof buf, "%d-%s", 12, "ab") != 5 || !streq(buf, "12-ab")) f |= 1 << 13;
    int found = 0;
    self_addr = (char *)&local_value;
    if (dl_iterate_phdr(self_cb, &found) != 1 || !found) f |= 1 << 14;
    return f;
}

// ---- setjmp / longjmp with bionic's buffer sizes ----------------------------------------------
static struct { long buf[JBLEN + 1]; long sentinel[4]; } jb;

__attribute__((noinline)) static long churn(int depth, long x)
{
    // many values live across the recursion: the callee-saved registers are all in use when it
    // longjmps, and only setjmp's copy can bring the caller's back
    long a = x * 3, b = x * 5, c = x * 7, d = x * 11, e = x * 13, g = x * 17, h = x * 19, k = x * 23;
    if (depth == 0) longjmp(jb.buf, 5);
    long r = churn(depth - 1, a + b) + c + d + e + g + h + k;
    return r + a * b - c * d + e * g - h * k;
}

int syn_setjmp_test(long seed)
{
    for (int i = 0; i < 4; i++) jb.sentinel[i] = SENTINEL;
    jb.buf[JBLEN] = SENTINEL;                            // setjmp's jmp_buf is one word shorter
    long a = seed + 1, b = seed * 2, c = seed * 3 + 1, d = seed ^ 0x55, e = seed * 7, g = seed - 9;
    long h = seed * 11, k = seed * 13, l = seed + 17, m = seed * 19;
    double fa = (double)seed * 1.5, fb = (double)seed * 2.25;
    volatile int passes = 0;
    int r = setjmp(jb.buf);
    if (r == 0) {
        passes++;
        churn(6, seed);
        return 1;
    }
    if (r != 5 || passes != 1) return 2;
    if (a != seed + 1 || b != seed * 2 || c != seed * 3 + 1 || d != (seed ^ 0x55) || e != seed * 7 ||
        g != seed - 9 || h != seed * 11 || k != seed * 13 || l != seed + 17 || m != seed * 19)
        return 3;
    if (fa != (double)seed * 1.5 || fb != (double)seed * 2.25) return 4;
    if (jb.buf[JBLEN] != SENTINEL) return 5;
    r = setjmp(jb.buf);
    if (r == 0) longjmp(jb.buf, 0);
    if (r != 1) return 6;                                // longjmp(b, 0) returns 1
    // sigsetjmp(b, 1) restores the signal mask, sigsetjmp(b, 0) does not
    b_sigset_t s, cur;
    sigemptyset(&s);
    sigaddset(&s, B_SIGUSR2);
    sigprocmask(B_SIG_UNBLOCK, &s, 0);
    if (sigsetjmp(jb.buf, 1) == 0) { sigprocmask(B_SIG_BLOCK, &s, 0); siglongjmp(jb.buf, 1); }
    sigprocmask(B_SIG_BLOCK, 0, &cur);
    if (cur & (1ul << (B_SIGUSR2 - 1))) return 7;
    if (sigsetjmp(jb.buf, 0) == 0) { sigprocmask(B_SIG_BLOCK, &s, 0); siglongjmp(jb.buf, 1); }
    sigprocmask(B_SIG_BLOCK, 0, &cur);
    if (!(cur & (1ul << (B_SIGUSR2 - 1)))) return 8;
    sigprocmask(B_SIG_UNBLOCK, &s, 0);
    for (int i = 0; i < 4; i++) if (jb.sentinel[i] != SENTINEL) return 9;
    return 0;
}

// ---- signals with bionic's sigset_t and struct sigaction ---------------------------------------
static volatile int got_usr1;
static void on_usr1(int s) { got_usr1 = s; }

int syn_signal_test(void)
{
    struct { b_sigset_t set; long sentinel; } x = { 0, SENTINEL }, o = { 0, SENTINEL }, p = { 0, SENTINEL };
    if (sigfillset(&x.set) || x.set != ~0ul || x.sentinel != SENTINEL) return 1;
    if (sigemptyset(&x.set) || x.set || x.sentinel != SENTINEL) return 2;
    if (sigaddset(&x.set, B_SIGUSR1) || x.set != 1ul << (B_SIGUSR1 - 1)) return 3;
    if (sigismember(&x.set, B_SIGUSR1) != 1 || sigismember(&x.set, B_SIGUSR2) != 0) return 4;
    if (sigaddset(&x.set, 0) != -1 || sigaddset(&x.set, 8 * sizeof(b_sigset_t) + 1) != -1) return 5;
    struct b_sigaction sa = { 0 }, old = { 0 }, now = { 0 };
    sa.handler = on_usr1;
    if (sigaction(B_SIGUSR1, &sa, &old)) return 6;
    sigprocmask(B_SIG_BLOCK, &x.set, &o.set);
    raise(B_SIGUSR1);
    if (sigpending(&p.set) || !(p.set & (1ul << (B_SIGUSR1 - 1))) || p.sentinel != SENTINEL || got_usr1) return 7;
    sigprocmask(B_SIG_SETMASK, &o.set, 0);              // unblocked: the handler runs
    if (got_usr1 != B_SIGUSR1 || o.sentinel != SENTINEL) return 8;
    if (sigaction(B_SIGUSR1, 0, &now) || now.handler != on_usr1) return 9;
    sigaction(B_SIGUSR1, &old, 0);
    if (sigdelset(&x.set, B_SIGUSR1) || x.set) return 10;
    return 0;
}

// ---- pthread objects at 4 mod 8 (bionic only guarantees 4-byte alignment) ----------------------
static struct __attribute__((aligned(8))) {
    int pad;
    int32_t mutex[MUTEX_WORDS];
    int32_t pad2;
    int32_t cond[COND_WORDS];
    int32_t pad3;
    int32_t rwlock[RWLOCK_WORDS];
    int counter, turn, readers, max_readers;
} S;                                                     // all zero: bionic's static initializers
static unsigned long thread_guard[4];

#if defined(__aarch64__)
unsigned long syn_tls_guard(void)
{
    unsigned long tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return *(volatile unsigned long *)(tp + 40);
}
#else
extern unsigned long __stack_chk_guard;
void *syn_guard_addr(void) { return &__stack_chk_guard; }
#endif

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
#if defined(__aarch64__)
    thread_guard[id] = syn_tls_guard();
#endif
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(S.mutex);
        S.counter++;
        pthread_mutex_unlock(S.mutex);
    }
    for (int i = 0; i < 2000; i++) {                     // readers share, a writer excludes them
        if (i % 50 == id) {
            pthread_rwlock_wrlock(S.rwlock);
            if (S.readers) S.max_readers = -1000;
            pthread_rwlock_unlock(S.rwlock);
        } else {
            pthread_rwlock_rdlock(S.rwlock);
            pthread_mutex_lock(S.mutex);
            if (++S.readers > S.max_readers && S.max_readers >= 0) S.max_readers = S.readers;
            pthread_mutex_unlock(S.mutex);
            pthread_mutex_lock(S.mutex);
            S.readers--;
            pthread_mutex_unlock(S.mutex);
            pthread_rwlock_unlock(S.rwlock);
        }
    }
    pthread_mutex_lock(S.mutex);                         // turns, through the condition variable
    for (int round = 0; round < 50; round++) {
        while (S.turn % 4 != id) pthread_cond_wait(S.cond, S.mutex);
        S.turn++;
        pthread_cond_broadcast(S.cond);
    }
    pthread_mutex_unlock(S.mutex);
    return 0;
}

int syn_thread_test(void)
{
    if (((uintptr_t)S.mutex & 7) != 4) return 1;         // the layout this test is about
    long t[4];
    for (int i = 0; i < 4; i++) if (pthread_create(&t[i], 0, worker, (void *)(intptr_t)i)) return 2;
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
    if (S.counter != 80000) return 3;
    if (S.turn != 200) return 4;
    if (S.max_readers < 0) return 5;                     // a writer saw readers
    if (pthread_rwlock_tryrdlock(S.rwlock) || pthread_rwlock_unlock(S.rwlock)) return 6;
#if defined(__aarch64__)
    for (int i = 0; i < 4; i++) if (thread_guard[i] != syn_tls_guard()) return 7;
#endif
    // a recursive mutex from pthread_mutex_init, also at 4 mod 8
    struct __attribute__((aligned(8))) { int pad; int32_t m[MUTEX_WORDS]; long attr; } r = { 0 };
    pthread_mutexattr_init(&r.attr);
    pthread_mutexattr_settype(&r.attr, 1);               // PTHREAD_MUTEX_RECURSIVE
    if (pthread_mutex_init(r.m, &r.attr) || pthread_mutex_lock(r.m) || pthread_mutex_lock(r.m) ||
        pthread_mutex_unlock(r.m) || pthread_mutex_unlock(r.m) || pthread_mutex_destroy(r.m))
        return 8;
    return 0;
}

// ---- struct stat with bionic's layout: st_size at 48 on both ------------------------------------
int syn_stat_test(const char *path, long long size)
{
    union { unsigned char b[256]; long long ll[32]; } u;
    for (int i = 0; i < 256; i++) u.b[i] = 0xaa;
    if (stat(path, u.b)) return 1;
    if (u.ll[6] != size) return 2;
    for (int i = STAT_SIZE; i < 256; i++) if (u.b[i] != 0xaa) return 3;
    return 0;
}

// ---- the stack protector: overrunning a buffer must end in __stack_chk_fail ---------------------
int syn_protected(int n)
{
    volatile char buf[64];
    int s = 0;
    for (int i = 0; i < 64; i++) buf[i] = (char)(i + n);
    for (int i = 0; i < 64; i++) s += buf[i];
    return s;
}

void syn_smash(int n)
{
    volatile char buf[8];
    for (int i = 0; i < n; i++) buf[i] = (char)i;
}

#if defined(__aarch64__)
// ---- unwinding through .eh_frame with an unwinder of our own (libgcc_eh, linked in hidden, as NDK
// libraries carry libgcc's or LLVM's); it finds frames through dl_iterate_phdr --------------------
struct _Unwind_Context;
int _Unwind_Backtrace(int (*)(struct _Unwind_Context *, void *), void *);
uintptr_t _Unwind_GetIP(struct _Unwind_Context *);
struct bt { uintptr_t *pcs; int n, max; };
static int bt_cb(struct _Unwind_Context *c, void *arg)
{
    struct bt *b = arg;
    if (b->n < b->max) b->pcs[b->n++] = _Unwind_GetIP(c);
    return 0;
}
__attribute__((noinline)) int syn_backtrace(uintptr_t *pcs, int max)
{
    struct bt b = { pcs, 0, max };
    _Unwind_Backtrace(bt_cb, &b);
    return b.n;
}
__attribute__((noinline)) int syn_call_host(int (*cb)(void)) { return cb() + 1; }
#endif

// ---- bionic symbols of newer players (Unity 2019.4-2022.3) -------------------------------------
// fortify entry points only bionic has, android_set_abort_message, sys_signame/sys_siglist, and calls
// whose flag values or (32-bit) structures differ from glibc's; each with bionic's types and values.
int *__errno(void);
void android_set_abort_message(const char *);
void __FD_SET_chk(int, unsigned long *, size_t), __FD_CLR_chk(int, unsigned long *, size_t);
int __FD_ISSET_chk(int, const unsigned long *, size_t);
size_t __strlen_chk(const char *, size_t);
char *__strchr_chk(const char *, int, size_t), *__strrchr_chk(const char *, int, size_t);
size_t __strlcpy_chk(char *, const char *, size_t, size_t), __strlcat_chk(char *, const char *, size_t, size_t);
extern const char *const sys_signame[], *const sys_siglist[];
struct b_ftw { int base, level; };
int nftw(const char *, int (*)(const char *, const void *, int, struct b_ftw *), int, int);
int ftw(const char *, int (*)(const char *, const void *, int), int);
int fnmatch(const char *, const char *, int);
long pathconf(const char *, int), fpathconf(int, int);
void *sem_open(const char *, int, ...);
void *newlocale(int, const char *, void *);
void freelocale(void *);
int isdigit_l(int, void *);
long double strtold_l(const char *, char **, void *);
typedef unsigned int b_wchar;            // bionic's wchar_t: 32 bits
double wcstod(const b_wchar *, b_wchar **);
float wcstof(const b_wchar *, b_wchar **);
int statvfs(const char *, void *), fstatvfs(int, void *);
int open(const char *, int, ...), close(int), unlink(const char *);
struct b_passwd {
    char *pw_name, *pw_passwd;
    unsigned pw_uid, pw_gid;
#if defined(__LP64__)
    char *pw_gecos;                      // LP32 bionic has none
#endif
    char *pw_dir, *pw_shell;
};
struct b_passwd *getpwuid(unsigned), *getpwnam(const char *);
int getpwuid_r(unsigned, struct b_passwd *, char *, size_t, struct b_passwd **);
#if !defined(__LP64__)
void *__mmap2(void *, size_t, int, int, int, size_t);
int munmap(void *, size_t);
int mknod(const char *, unsigned, unsigned, unsigned);   // bionic's has 3 arguments: r3 = junk
size_t mbsrtowcs(b_wchar *, const char **, size_t, void *), wcsrtombs(char *, const b_wchar **, size_t, void *);
size_t mbrlen(const char *, size_t, void *);
int mbsinit(const void *);
#endif
#define B_LC_ALL_MASK 0x1fbf
#define B_ENOENT 2
#define B_EINVAL 22
#define B_ENOSYS 38

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static const char *base_of(const char *p) { const char *b = p; for (; *p; p++) if (*p == '/') b = p + 1; return b; }

// the walk, as "name:type" entries in visiting order; bionic's types: F 0, D 1, DNR 2, DP 3, NS 4, SL 5, SLN 6
static char walk[256];
static int walk_n;
static long long walk_fsize;
static void walk_add(const char *path, int type)
{
    const char *b = base_of(path);
    size_t n = slen(b);
    if (walk_n + n + 4 >= sizeof walk) return;
    for (size_t i = 0; i < n; i++) walk[walk_n++] = b[i];
    walk[walk_n++] = ':';
    walk[walk_n++] = (char)('0' + type);
    walk[walk_n++] = ' ';
    walk[walk_n] = 0;
}
static int nftw_cb(const char *path, const void *st, int type, struct b_ftw *f)
{
    walk_add(path, type);
    if (streq(base_of(path), "f") && f->level == 1) walk_fsize = *(const long long *)((const char *)st + 48);
    return 0;
}
static int ftw_cb(const char *path, const void *st, int type) { (void)st; walk_add(path, type); return 0; }
static int has(const char *hay, const char *needle)
{
    size_t n = slen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] == needle[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}
static int before(const char *a, const char *b)       // both in the walk, a first
{
    const char *pa = 0, *pb = 0;
    size_t na = slen(a), nb = slen(b);
    for (const char *p = walk; *p; p++) {
        size_t i = 0;
        if (!pa) { while (i < na && p[i] == a[i]) i++; if (i == na) pa = p; }
        i = 0;
        if (!pb) { while (i < nb && p[i] == b[i]) i++; if (i == nb) pb = p; }
    }
    return pa && pb && pa < pb;
}

// 0 when everything holds, else a bit per failed area. dir: a directory with tree/{f (5 bytes), sub/g,
// link -> f, dang -> nowhere} and file8k (8192 bytes, 'Q' at 4096); pc: glibc's pathconf of dir for
// NAME_MAX, PIPE_BUF, FILESIZEBITS, LINK_MAX; vfs: glibc's statvfs of dir: bsize, blocks, namemax
int syn_newer_test(const char *dir, const long *pc, const unsigned long *vfs)
{
    int f = 0;
    char path[256];
    // fd_set
    unsigned long set[1024 / (8 * sizeof(unsigned long))];
    for (size_t i = 0; i < sizeof set / sizeof set[0]; i++) set[i] = 0;
    __FD_SET_chk(3, set, sizeof set);
    __FD_SET_chk(70, set, sizeof set);
    if (set[0] != (1ul << 3) || set[70 / (8 * sizeof(long))] != 1ul << (70 % (8 * sizeof(long)))) f |= 1 << 0;
    if (__FD_ISSET_chk(70, set, sizeof set) != 1 || __FD_ISSET_chk(71, set, sizeof set) != 0) f |= 1 << 0;
    __FD_CLR_chk(70, set, sizeof set);
    if (__FD_ISSET_chk(70, set, sizeof set) || !__FD_ISSET_chk(3, set, sizeof set)) f |= 1 << 0;
    // string checks that pass
    static const char hello[] = "hello";
    char b8[8];
    if (__strlen_chk(hello, sizeof hello) != 5 || __strchr_chk(hello, 'l', sizeof hello) != hello + 2 ||
        __strrchr_chk(hello, 'l', sizeof hello) != hello + 3 || __strchr_chk(hello, 'z', sizeof hello) ||
        __strrchr_chk(hello, 0, sizeof hello) != hello + 5)
        f |= 1 << 1;
    if (__strlcpy_chk(b8, "abcdef", 4, sizeof b8) != 6 || !streq(b8, "abc") ||
        __strlcat_chk(b8, "xyzw", sizeof b8, sizeof b8) != 7 || !streq(b8, "abcxyzw"))
        f |= 1 << 1;
    // signal tables
    if (sys_signame[0] || !streq(sys_signame[11], "SEGV") || !streq(sys_signame[30], "PWR") ||
        !streq(sys_siglist[2], "Interrupt") || !streq(sys_siglist[6], "Aborted"))
        f |= 1 << 2;
    // statvfs: bionic's layout (LP32: 11 longs; LP64: glibc's, 11 longs + 24 reserved bytes)
    union { unsigned long w[32]; unsigned char b[256]; } v;
    for (int i = 0; i < 256; i++) v.b[i] = 0xaa;
    size_t vsize = sizeof(long) == 8 ? 112 : 44;
    if (statvfs(dir, v.b) || v.w[0] != vfs[0] || v.w[2] != vfs[1] || v.w[10] != vfs[2]) f |= 1 << 3;
    for (size_t i = vsize; i < 256; i++) if (v.b[i] != 0xaa) { f |= 1 << 3; break; }
    int fd = open(dir, 0);
    if (fd < 0 || fstatvfs(fd, v.b) || v.w[0] != vfs[0] || v.w[10] != vfs[2]) f |= 1 << 3;
    // struct passwd (root: /etc/passwd of the test container)
    struct b_passwd *p = getpwuid(0);
    if (!p || !streq(p->pw_name, "root") || p->pw_uid || !streq(p->pw_dir, "/root") || p->pw_shell[0] != '/') f |= 1 << 4;
    p = getpwnam("root");
    if (!p || p->pw_uid || !streq(p->pw_dir, "/root")) f |= 1 << 4;
    struct { struct b_passwd pw; long sentinel; } pr = { .sentinel = SENTINEL };
    struct b_passwd *res = 0;
    char pbuf[512];
    if (getpwuid_r(0, &pr.pw, pbuf, sizeof pbuf, &res) || res != &pr.pw || !streq(pr.pw.pw_dir, "/root") ||
        pr.sentinel != SENTINEL)
        f |= 1 << 4;
    // nftw: bionic's FTW_PHYS 1 | FTW_DEPTH 4: contents before their directory (FTW_DP), links not followed
    snprintf(path, sizeof path, "%s/tree", dir);
    walk_n = 0; walk[0] = 0; walk_fsize = -1;
    if (nftw(path, nftw_cb, 4, 1 | 4) || !has(walk, "f:0") || !has(walk, "g:0") || !has(walk, "sub:3") ||
        !has(walk, "tree:3") || !has(walk, "link:5") || !has(walk, "dang:5") || !before("g:0", "sub:3") ||
        !before("sub:3", "tree:3") || walk_fsize != 5)
        f |= 1 << 5;
    walk_n = 0; walk[0] = 0;                              // logical, pre-order: links followed
    if (nftw(path, nftw_cb, 4, 0) || !has(walk, "tree:1") || !has(walk, "sub:1") || !has(walk, "link:0") ||
        !has(walk, "dang:6") || !before("tree:1", "f:0"))
        f |= 1 << 5;
    walk_n = 0; walk[0] = 0;
    if (ftw(path, ftw_cb, 4) || !has(walk, "tree:1") || !has(walk, "g:0")) f |= 1 << 5;
    if (nftw(path, nftw_cb, 0, 0) != -1 || *__errno() != B_EINVAL) f |= 1 << 5;
    // fnmatch: bionic's FNM_NOESCAPE 1, FNM_PATHNAME 2
    if (fnmatch("*.txt", "a/b.txt", 0) != 0 || fnmatch("*.txt", "a/b.txt", 2) != 1 ||
        fnmatch("a\\*", "a\\b", 1) != 0 || fnmatch("a\\*", "a\\b", 0) != 1 || fnmatch("a\\*", "a*", 0) != 0)
        f |= 1 << 6;
    // pathconf: bionic's _PC_NAME_MAX 4, _PC_PIPE_BUF 6, _PC_FILESIZEBITS 0, _PC_LINK_MAX 1
    if (pathconf(dir, 4) != pc[0] || pathconf(dir, 6) != pc[1] || pathconf(dir, 0) != pc[2] || pathconf(dir, 1) != pc[3])
        f |= 1 << 7;
    if (fd < 0 || fpathconf(fd, 4) != pc[0] || pathconf(dir, 99) != -1 || *__errno() != B_EINVAL) f |= 1 << 7;
    close(fd);
    // named semaphores: none on Android
    *__errno() = 0;
    if (sem_open("/syn", 0) || *__errno() != B_ENOSYS) f |= 1 << 8;
    // locales: bionic's names; the objects work with the *_l functions
    void *loc = newlocale(B_LC_ALL_MASK, "C.UTF-8", 0), *c = newlocale(B_LC_ALL_MASK, "", 0);
    if (!loc || !c || !isdigit_l('7', loc) || isdigit_l('x', loc)) f |= 1 << 9;
    char *end = 0;
    // 12.5 as long double: IEEE quad on arm64 (no libgcc here for comparing those), double on 32-bit ARM
    union { long double ld; unsigned long long u[2]; } r = { .u = { 1, 1 } };
    if (loc) r.ld = strtold_l("1.25e1z", &end, loc);
    if (sizeof(long double) == 16 ? r.u[1] != 0x4002900000000000ull || r.u[0] : r.u[0] != 0x4029000000000000ull) f |= 1 << 9;
    if (!end || *end != 'z') f |= 1 << 9;
    *__errno() = 0;
    if (newlocale(B_LC_ALL_MASK, "de_DE.UTF-8", 0) || *__errno() != B_ENOENT) f |= 1 << 9;
    if (newlocale(1 << 6, "C", 0) || *__errno() != B_EINVAL) f |= 1 << 9;
    if (loc) freelocale(loc);
    if (c) freelocale(c);
    // float results (softfp wrappers on 32-bit ARM)
    static const b_wchar w1[] = { '2', '.', '5', 'x', 0 }, w2[] = { '-', '0', '.', '7', '5', 0 };
    b_wchar *we = 0;
    if (wcstod(w1, &we) != 2.5 || we != w1 + 3 || wcstof(w2, 0) != -0.75f) f |= 1 << 10;
#if !defined(__LP64__)
    // __mmap2: the offset in 4096-byte units
    snprintf(path, sizeof path, "%s/file8k", dir);
    fd = open(path, 0);
    const char *m = fd >= 0 ? __mmap2(0, 4096, 1 /* PROT_READ */, 2 /* MAP_PRIVATE */, fd, 1) : 0;
    if (!m || m == (const char *)-1 || m[0] != 'Q' || m[1] != 'R') f |= 1 << 11;
    else munmap((void *)m, 4096);
    close(fd);
    // mknod: bionic's 32-bit dev_t (r3 holds junk the callee must ignore)
    unlink("/tmp/syn_fifo");
    if (mknod("/tmp/syn_fifo", 0010600 /* S_IFIFO | 0600 */, 0, 0xdeadbeef)) f |= 1 << 12;
    unlink("/tmp/syn_fifo");
    // conversions with bionic's 4-byte mbstate_t
    struct { unsigned st; unsigned sentinel; } s = { 0, 0x5e17e1a5 };
    const char *src = "abc";
    b_wchar wb[8];
    if (mbsrtowcs(wb, &src, 8, &s.st) != 3 || wb[0] != 'a' || wb[2] != 'c' || src || s.sentinel != 0x5e17e1a5) f |= 1 << 13;
    const b_wchar *wsrc = wb;
    char nb[8];
    if (wcsrtombs(nb, &wsrc, sizeof nb, &s.st) != 3 || !streq(nb, "abc") || s.sentinel != 0x5e17e1a5) f |= 1 << 13;
    if (mbrlen("a", 1, &s.st) != 1 || !mbsinit(&s.st) || s.sentinel != 0x5e17e1a5) f |= 1 << 13;
#endif
    return f;
}

// fortify failures and the abort message (run in a child process by test_syn.c)
void syn_fortify_fail(int k)
{
    unsigned long set[1024 / (8 * sizeof(unsigned long))] = { 0 };
    char b8[8];
    switch (k) {
    case 0: __strlen_chk("hello", 5); break;
    case 1: __FD_SET_chk(1024, set, sizeof set); break;
    case 2: __strlcpy_chk(b8, "abc", 16, sizeof b8); break;
    case 3: __FD_ISSET_chk(-1, set, sizeof set); break;
    case 4: __strchr_chk("hello", 'z', 3); break;
    case 5: __FD_CLR_chk(1, set, 64); break;
    default: android_set_abort_message("syn: giving up"); break;
    }
}
