// sh_misc.c - the process as the images ask about it: the unwinder's entry points, exit and
// atexit, the stack guard, compiler helpers, math, number parsing, limits, sysconf, sysctl,
// uname, random numbers, the terminal that is not there, backtrace.
#define _GNU_SOURCE
#include "darwin.h"
#include "darwin_va.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

// The macOS release the images are told: 11, the oldest a Unity 6 player runs on (and the
// one the game's LC_BUILD_VERSION asks for), as Darwin's own number.
#define DARWIN_RELEASE "20.6.0"

void ua_chkstk_darwin(void);                    // sh_asm.S
extern int __cxa_atexit(void (*)(void *), void *, void *);
extern void __stack_chk_fail(void);

// ___stack_chk_guard: the images' own cell, set before any of them runs and never again.
static uintptr_t stack_guard;
__attribute__((constructor)) static void guard_init(void)
{
    if (getrandom(&stack_guard, sizeof stack_guard, 0) != (ssize_t)sizeof stack_guard)
        stack_guard = (uintptr_t)&stack_guard * 0x9e3779b97f4a7c15u ^ (uintptr_t)time(NULL);
    stack_guard &= ~(uintptr_t)0xff;            // a string copied over it stops at the first byte
}

static long darwin_atexit(void (*fn)(void))
{
    return __cxa_atexit((void (*)(void *))(void *)fn, NULL, NULL);
}

// exit: the calling thread's thread-local destructors first, as macOS runs them, then the
// atexit list (the images' ___cxa_atexit entries are on glibc's, with the host's).
static void darwin_exit(int code)
{
    darwin_tlv_thread_exit();
    exit(code);
}

// int __darwin_check_fd_set_overflow(int fd, const void *set, int unlimited): FD_SET's guard,
// 1 when fd has a bit in an fd_set (1024 of them, there as here).
static long darwin___darwin_check_fd_set_overflow(int fd, const void *set, int unlimited)
{
    if (fd >= 0 && (unlimited || fd < FD_SETSIZE)) return 1;
    darwin_unanswered("__darwin_check_fd_set_overflow", "a descriptor past FD_SETSIZE in an fd_set: left out");
    return 0;
}

static __int128 darwin___divti3(__int128 a, __int128 b) { return a / b; }

// __sincos_stret, __sincosf_stret: both results in d0, d1 (s0, s1), which is how AAPCS64
// returns a struct of two doubles (floats) as well.
struct dpair { double s, c; };
struct fpair { float s, c; };
static struct dpair darwin___sincos_stret(double x)
{
    struct dpair r;
    sincos(x, &r.s, &r.c);
    return r;
}
static struct fpair darwin___sincosf_stret(float x)
{
    struct fpair r;
    sincosf(x, &r.s, &r.c);
    return r;
}

// long double is double on macOS for arm64
static double darwin_strtold(const char *s, char **end) { return strtod(s, end); }

static int rlimit_host(int r)
{
    switch (r) {
    case DARWIN_RLIMIT_CPU: return RLIMIT_CPU;
    case DARWIN_RLIMIT_FSIZE: return RLIMIT_FSIZE;
    case DARWIN_RLIMIT_DATA: return RLIMIT_DATA;
    case DARWIN_RLIMIT_STACK: return RLIMIT_STACK;
    case DARWIN_RLIMIT_CORE: return RLIMIT_CORE;
    case DARWIN_RLIMIT_AS: return RLIMIT_AS;
    case DARWIN_RLIMIT_MEMLOCK: return RLIMIT_MEMLOCK;
    case DARWIN_RLIMIT_NPROC: return RLIMIT_NPROC;
    case DARWIN_RLIMIT_NOFILE: return RLIMIT_NOFILE;
    }
    return -1;
}

static uint64_t rlim_darwin(rlim_t v)
{
    return v == RLIM_INFINITY || v > DARWIN_RLIM_INFINITY ? DARWIN_RLIM_INFINITY : v;
}

// int getrlimit(int, struct rlimit *): two 64-bit numbers there as here.
static long darwin_getrlimit(int resource, uint64_t *out)
{
    struct rlimit l;
    int r = rlimit_host(resource);
    if (r < 0) { errno = EINVAL; return -1; }
    if (getrlimit(r, &l) != 0) return -1;
    out[0] = rlim_darwin(l.rlim_cur);
    out[1] = rlim_darwin(l.rlim_max);
    return 0;
}

// int getrusage(int, struct rusage *): the same layout (macOS's 4-byte tv_usec sits where the
// low half of Linux's is); ru_maxrss is bytes there, kilobytes here.
_Static_assert(sizeof(struct rusage) == DARWIN_SIZEOF_RUSAGE, "struct rusage");
_Static_assert(offsetof(struct rusage, ru_maxrss) == DARWIN_RUSAGE_ru_maxrss, "ru_maxrss");
static long darwin_getrusage(int who, struct rusage *out)
{
    if (who != DARWIN_RUSAGE_SELF && who != DARWIN_RUSAGE_CHILDREN) { errno = EINVAL; return -1; }
    if (getrusage(who == DARWIN_RUSAGE_SELF ? RUSAGE_SELF : RUSAGE_CHILDREN, out) != 0) return -1;
    out->ru_maxrss *= 1024;
    return 0;
}

// int proc_name(int pid, void *buf, uint32_t size): the name's length, 0 with errno when the
// process is not there.
static long darwin_proc_name(int pid, char *buf, uint32_t size)
{
    char path[64], name[64];
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { errno = ESRCH; return 0; }
    ssize_t n = read(fd, name, sizeof name - 1);
    close(fd);
    if (n <= 0) { errno = ESRCH; return 0; }
    if (name[n - 1] == '\n') n--;
    name[n] = 0;
    if (!buf || size == 0) { errno = ENOMEM; return 0; }
    snprintf(buf, size, "%s", name);
    return (long)strlen(buf);
}

static uint64_t phys_bytes(void)
{
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? (uint64_t)pages * (uint64_t)size : 0;
}

static long darwin_sysconf(int name)
{
    switch (name) {
    case DARWIN__SC_ARG_MAX: return sysconf(_SC_ARG_MAX);
    case DARWIN__SC_CHILD_MAX: return sysconf(_SC_CHILD_MAX);
    case DARWIN__SC_CLK_TCK: return sysconf(_SC_CLK_TCK);
    case DARWIN__SC_NGROUPS_MAX: return sysconf(_SC_NGROUPS_MAX);
    case DARWIN__SC_OPEN_MAX: return sysconf(_SC_OPEN_MAX);
    case DARWIN__SC_LINE_MAX: return sysconf(_SC_LINE_MAX);
    case DARWIN__SC_PAGESIZE: return (long)darwin_page_size();
    case DARWIN__SC_IOV_MAX: return sysconf(_SC_IOV_MAX);
    case DARWIN__SC_NPROCESSORS_CONF: return sysconf(_SC_NPROCESSORS_CONF);
    case DARWIN__SC_NPROCESSORS_ONLN: return sysconf(_SC_NPROCESSORS_ONLN);
    case DARWIN__SC_GETGR_R_SIZE_MAX: return sysconf(_SC_GETGR_R_SIZE_MAX);
    case DARWIN__SC_GETPW_R_SIZE_MAX: return sysconf(_SC_GETPW_R_SIZE_MAX);
    case DARWIN__SC_HOST_NAME_MAX: return sysconf(_SC_HOST_NAME_MAX);
    case DARWIN__SC_LOGIN_NAME_MAX: return sysconf(_SC_LOGIN_NAME_MAX);
    case DARWIN__SC_THREAD_STACK_MIN: return sysconf(_SC_THREAD_STACK_MIN);    // the host's: its threads need it
    case DARWIN__SC_PHYS_PAGES: return (long)(phys_bytes() / darwin_page_size());
    }
    static _Atomic int said;
    if (!said) { said = 1; darwin_log("sysconf(%d) is not answered: -1, EINVAL", name); }
    errno = EINVAL;
    return -1;
}

// sysctl's result: n bytes of src to oldp, as the kernel does it (the size alone when oldp is
// NULL, ENOMEM with what fits when the room is short).
static long ctl_give(void *oldp, size_t *oldlenp, const void *src, size_t n)
{
    if (!oldlenp) return 0;
    if (!oldp) { *oldlenp = n; return 0; }
    size_t room = *oldlenp;
    memcpy(oldp, src, room < n ? room : n);
    *oldlenp = room < n ? room : n;
    if (room < n) { errno = ENOMEM; return -1; }
    return 0;
}

static long ctl_int(void *oldp, size_t *oldlenp, uint64_t v)
{
    int i = v > INT_MAX ? INT_MAX : (int)v;
    return ctl_give(oldp, oldlenp, &i, sizeof i);
}

static long ctl_str(void *oldp, size_t *oldlenp, const char *s)
{
    return ctl_give(oldp, oldlenp, s, strlen(s) + 1);
}

// int sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen):
// the names code asks a Mac about itself with; any other is said once and fails as an unknown
// name does there.
static long darwin_sysctl(const int *name, unsigned namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
{
    if (newp) { errno = EPERM; return -1; }
    if (!name || namelen < 2) { errno = EINVAL; return -1; }
    if (name[0] == DARWIN_CTL_HW && namelen == 2) {
        switch (name[1]) {
        // every core a Mac has runs; the ones that can here
        case DARWIN_HW_NCPU: case DARWIN_HW_AVAILCPU: return ctl_int(oldp, oldlenp, (uint64_t)sysconf(_SC_NPROCESSORS_ONLN));
        case DARWIN_HW_MEMSIZE: { uint64_t v = phys_bytes(); return ctl_give(oldp, oldlenp, &v, sizeof v); }
        case DARWIN_HW_PHYSMEM: case DARWIN_HW_USERMEM: return ctl_int(oldp, oldlenp, phys_bytes());
        case DARWIN_HW_PAGESIZE: return ctl_int(oldp, oldlenp, darwin_page_size());
        case DARWIN_HW_MACHINE: return ctl_str(oldp, oldlenp, "arm64");
        }
    }
    if (name[0] == DARWIN_CTL_KERN && namelen == 2) {
        switch (name[1]) {
        case DARWIN_KERN_OSTYPE: return ctl_str(oldp, oldlenp, "Darwin");
        case DARWIN_KERN_OSRELEASE: return ctl_str(oldp, oldlenp, DARWIN_RELEASE);
        case DARWIN_KERN_VERSION: return ctl_str(oldp, oldlenp, "Darwin Kernel Version " DARWIN_RELEASE);
        case DARWIN_KERN_ARGMAX: return ctl_int(oldp, oldlenp, (uint64_t)sysconf(_SC_ARG_MAX));
        case DARWIN_KERN_HOSTNAME: {
            char host[256];
            if (gethostname(host, sizeof host) != 0) return -1;
            host[sizeof host - 1] = 0;
            return ctl_str(oldp, oldlenp, host);
        }
        case DARWIN_KERN_BOOTTIME: {
            // struct timeval: 8 bytes of seconds, 4 of microseconds
            struct timespec now, up;
            unsigned char tv[DARWIN_SIZEOF_TIMEVAL] = { 0 };
            clock_gettime(CLOCK_REALTIME, &now);
            clock_gettime(CLOCK_BOOTTIME, &up);
            int64_t sec = now.tv_sec - up.tv_sec;
            int32_t usec = (int32_t)((now.tv_nsec - up.tv_nsec) / 1000);
            if (usec < 0) { usec += 1000000; sec--; }
            memcpy(tv, &sec, 8);
            memcpy(tv + DARWIN_TIMEVAL_tv_usec, &usec, 4);
            return ctl_give(oldp, oldlenp, tv, sizeof tv);
        }
        }
    }
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static int said[32][2], nsaid;
    int first = 1;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nsaid; i++) if (said[i][0] == name[0] && said[i][1] == name[1]) first = 0;
    if (first && nsaid < 32) { said[nsaid][0] = name[0]; said[nsaid++][1] = name[1]; }
    pthread_mutex_unlock(&lock);
    if (first) darwin_log("sysctl {%d, %d} (%u names) is not answered: -1, ENOENT", name[0], name[1], namelen);
    errno = ENOENT;
    return -1;
}

// int uname(struct utsname *): five 256-byte strings; the system is macOS, the node the host's.
static long darwin_uname(char *out)
{
    struct utsname u;
    if (uname(&u) != 0) return -1;
    memset(out, 0, DARWIN_SIZEOF_UTSNAME);
    snprintf(out, 256, "Darwin");
    snprintf(out + DARWIN_UTSNAME_nodename, 256, "%s", u.nodename);
    snprintf(out + DARWIN_UTSNAME_release, 256, DARWIN_RELEASE);
    snprintf(out + DARWIN_UTSNAME_version, 256, "Darwin Kernel Version " DARWIN_RELEASE);
    snprintf(out + DARWIN_UTSNAME_machine, 256, "arm64");
    return 0;
}

// void syslog(int priority, const char *format, ...): there is no log daemon; stderr has it.
static void darwin_syslog(int priority, const char *fmt, ...)
{
    char buf[256], line[1024];
    int e = *darwin_error();
    const char *g = darwin_fmt(fmt, 0, buf, sizeof buf);
    if (!g) return;
    va_list ap;
    DARWIN_VA_START(ap, fmt);
    errno = darwin_errno_host(e);               // %m
    vsnprintf(line, sizeof line, g, ap);
    va_end(ap);
    if (g != fmt && g != buf) free((void *)g);
    fprintf(stderr, "[syslog %d] %s\n", priority & 7, line);
    darwin_set_errno(e);
}

static void random_bytes(void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { fprintf(stderr, "[darwin] arc4random: the kernel gives no random bytes (%s)\n", strerror(errno)); abort(); }
        p += r;
        n -= (size_t)r;
    }
}

static unsigned long darwin_arc4random(void)
{
    uint32_t v;
    int e = errno;
    random_bytes(&v, sizeof v);
    errno = e;
    return v;
}

static void darwin_arc4random_buf(void *buf, size_t n)
{
    int e = errno;
    random_bytes(buf, n);
    errno = e;
}

// The terminal: none. A descriptor is never one, whatever the host's stdout happens to be, so
// that termios (whose layout and flag bits differ) is never exchanged.
static long no_terminal(int fd)
{
    errno = fcntl(fd, F_GETFD) < 0 ? EBADF : ENOTTY;
    return -1;
}
static long darwin_isatty(int fd) { no_terminal(fd); return 0; }
static long darwin_tcgetattr(int fd, void *t) { return no_terminal(fd); }
static long darwin_tcsetattr(int fd, int how, const void *t) { return no_terminal(fd); }
static long darwin_tcflush(int fd, int which) { return no_terminal(fd); }

// int lchflags(const char *, unsigned): BSD file flags (hidden, immutable, ...), which the
// file systems here do not have. Clearing them all is done already; setting one is refused.
static long darwin_lchflags(const char *path, unsigned flags)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (flags == 0) return 0;
    darwin_set_errno(DARWIN_ENOTSUP);
    return -1;
}

// The calling thread's stack, or the alternate one when a handler runs on it.
static void stack_bounds(uintptr_t *lo, uintptr_t *hi)
{
    static __thread uintptr_t t_lo, t_hi;
    stack_t ss;
    if (sigaltstack(NULL, &ss) == 0 && (ss.ss_flags & SS_ONSTACK)) {
        *lo = (uintptr_t)ss.ss_sp;
        *hi = *lo + ss.ss_size;
        return;
    }
    if (!t_hi) {
        pthread_attr_t a;
        void *addr;
        size_t size;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            if (pthread_attr_getstack(&a, &addr, &size) == 0) { t_lo = (uintptr_t)addr; t_hi = t_lo + size; }
            pthread_attr_destroy(&a);
        }
    }
    *lo = t_lo;
    *hi = t_hi;
}

// int backtrace(void **, int): the return addresses up the frame records (x29, x30 pairs,
// which macOS's ABI has every function keep), the caller's first. Each record is checked to
// lie in the thread's stack above the one before, so a frame that is not one ends the walk.
__attribute__((noinline)) static long darwin_backtrace(void **buf, int size)
{
    uintptr_t lo, hi, *fp = __builtin_frame_address(0);
    int n = 0;
    stack_bounds(&lo, &hi);
    while (n < size) {
        if ((uintptr_t)fp < lo || (uintptr_t)fp > hi - 16 || ((uintptr_t)fp & 7)) break;
        uintptr_t next = fp[0], ret = fp[1];
        if (!ret) break;
        buf[n++] = (void *)ret;
        if (next <= (uintptr_t)fp) break;
        fp = (uintptr_t *)next;
    }
    return n;
}

// One frame as macOS writes it: "3   GameAssembly.dylib   0x0000000104a2b3c4 name + 132".
static void frame_text(int i, void *addr, char *out, size_t len)
{
    const char *image = "???", *name = NULL;
    char hex[32];
    uint64_t off = 0;
    Dl_info di;
    struct darwin_image *im = darwin_dyld_image_at(addr);
    if (im) {
        image = im->m->path;
        name = macho_symbolize(im->m, addr, &off);
        if (name && name[0] == '_') name++;
    } else if (dladdr(addr, &di) && di.dli_fname) {
        image = di.dli_fname;
        if (di.dli_sname) { name = di.dli_sname; off = (uint64_t)((uintptr_t)addr - (uintptr_t)di.dli_saddr); }
    }
    const char *base = strrchr(image, '/');
    if (base) image = base + 1;
    if (!name) { snprintf(hex, sizeof hex, "0x%lx", (unsigned long)(uintptr_t)addr); name = hex; off = 0; }
    snprintf(out, len, "%-4d%-35s 0x%016lx %s + %lu", i, image, (unsigned long)(uintptr_t)addr, name, (unsigned long)off);
}

// char **backtrace_symbols(void *const *, int): one malloc'd block, the pointers then the texts.
static char **darwin_backtrace_symbols(void *const *addrs, int size)
{
    char line[1024];
    size_t bytes = 0;
    if (size < 0) size = 0;
    for (int i = 0; i < size; i++) { frame_text(i, addrs[i], line, sizeof line); bytes += strlen(line) + 1; }
    char **out = malloc((size_t)size * sizeof *out + bytes + 1);
    if (!out) return NULL;
    char *text = (char *)(out + size);
    for (int i = 0; i < size; i++) {
        frame_text(i, addrs[i], line, sizeof line);
        out[i] = text;
        text = stpcpy(text, line) + 1;
    }
    return out;
}

const struct darwin_sym darwin_syms_misc[] = {
    // the unwinder is the host's (cxx/: LLVM's libunwind, which reads the images' unwind info):
    // the images' libc++abi raises through it, _Unwind_Exception laid out the same
    NAMED("__Unwind_DeleteException", _Unwind_DeleteException),
    NAMED("__Unwind_GetIP", _Unwind_GetIP),
    NAMED("__Unwind_GetLanguageSpecificData", _Unwind_GetLanguageSpecificData),
    NAMED("__Unwind_GetRegionStart", _Unwind_GetRegionStart),
    NAMED("__Unwind_RaiseException", _Unwind_RaiseException),
    NAMED("__Unwind_Resume", _Unwind_Resume),
    NAMED("__Unwind_SetGR", _Unwind_SetGR),
    NAMED("__Unwind_SetIP", _Unwind_SetIP),
    NAMED("___chkstk_darwin", ua_chkstk_darwin),
    NAMED("___cxa_atexit", __cxa_atexit),
    NAMED("___stack_chk_guard", &stack_guard),
    NAMED("___stack_chk_fail", __stack_chk_fail),
    NAMED("___darwin_check_fd_set_overflow", darwin___darwin_check_fd_set_overflow),
    NAMED("___divti3", darwin___divti3),
    NAMED("___exp10f", exp10f),
    NAMED("___sincos_stret", darwin___sincos_stret),
    NAMED("___sincosf_stret", darwin___sincosf_stret),
    // glibc's _setjmp fills 180 bytes of macOS's 192-byte jmp_buf, x19-x28 and x29 as they are
    // (a collector that scans the buffer for pointers finds them)
    NAMED("__setjmp", _setjmp),
    SAME(abort), SHIM(atexit), SHIM(exit),
    SAME(getenv), SAME(setenv), SAME(unsetenv), SAME(getpid), SAME(getegid), SAME(geteuid), SAME(gethostname),
    SAME(atoi), SAME(atol), SAME(div),
    SAME(strtod), SAME(strtof), SAME(strtol), SHIM(strtold), SAME(strtoll), SAME(strtoul), SAME(strtoull),
    SAME(acos), SAME(acosf), SAME(asin), SAME(asinf), SAME(atan), SAME(atan2), SAME(atan2f), SAME(atanf),
    SAME(cos), SAME(cosf), SAME(cosh), SAME(exp), SAME(exp2f), SAME(expf), SAME(fmod), SAME(fmodf),
    SAME(hypot), SAME(ldexpf), SAME(log), SAME(log10), SAME(log10f), SAME(logf), SAME(modf), SAME(pow),
    SAME(powf), SAME(sin), SAME(sinf), SAME(sinh), SAME(tan), SAME(tanf), SAME(tanh),
    SHIM(getrlimit), SHIM(getrusage), SHIM(proc_name), SHIM(sysconf), SHIM(sysctl), SHIM(uname),
    NAMED("_syslog$DARWIN_EXTSN", darwin_syslog),
    SHIM(arc4random), SHIM(arc4random_buf),
    SHIM(isatty), SHIM(tcgetattr), SHIM(tcsetattr), SHIM(tcflush), SHIM(lchflags),
    SHIM(backtrace), SHIM(backtrace_symbols),
    { NULL, NULL }
};
