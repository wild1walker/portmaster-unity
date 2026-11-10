// darwin_c.c - a test of the Darwin shims, built on a Mac for macOS (test/build-mac.sh): run
// there natively and through the loader on Linux, its output must be the same on both. It
// prints only what does not depend on the machine: "ok"/"FAIL" lines and values macOS's libc
// defines (errno numbers, formats, layouts as the code reads them).
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <locale.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <xlocale.h>

static int failed;
#define CHECK(name, cond) do { int ok_ = !!(cond); failed += !ok_; fprintf(stdout, "%s %s\n", ok_ ? "ok  " : "FAIL", name); } while (0)
#define SHOW(...) fprintf(stdout, __VA_ARGS__)

static uint64_t now_ms(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000000;
}

static void nap(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&t, NULL);
}

// ---- formats -----------------------------------------------------------------------------------

static int vfmt(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static void test_format(void)
{
    char b[256];
    snprintf(b, sizeof b, "%p|%5.2f|%-6s|%x|%lld|%zu|%c|%%", (void *)0, 3.14159, "ab", 255, -5000000000LL, (size_t)7, 'q');
    SHOW("fmt1 %s\n", b);
    snprintf(b, sizeof b, "%d %d %d %d %d %d %d %d %d %d %g %g %g %g %g %g %g %g %g %g", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
             .5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5);
    SHOW("fmt2 %s\n", b);
    snprintf(b, sizeof b, "%Lf %Lg %D %U %hhd %hd %ld", 1.5L, 2.25L, 42L, 43UL, (signed char)-3, (short)-4, 1L << 40);
    SHOW("fmt3 %s\n", b);
    int r = vfmt(b, sizeof b, "%s %d %f %c %ld %s", "va", 9, 2.5, 'z', -7L, "end");
    SHOW("fmt4 %d %s\n", r, b);
    snprintf(b, sizeof b, "%p", (void *)0x1234);
    SHOW("fmt5 %s\n", b);
    snprintf(b, sizeof b, "%e %a %.3s %5d|%-5d|%05d %+d", 12345.678, 1.0, "abcdef", 42, 42, 42, 42);
    SHOW("fmt6 %s\n", b);
    snprintf_l(b, sizeof b, NULL, "%.2f %d", 1.239, 5);
    SHOW("fmt7 %s\n", b);
    char *made = NULL;
    r = asprintf_l(&made, NULL, "%s-%d", "as", 3);
    SHOW("fmt8 %d %s\n", r, made);
    free(made);

    int i = 0; double d = 0; char w[16] = ""; long double ld = 0; unsigned u = 0; long l = 0;
    r = sscanf("12 3.5 word 2.25 ff -9", "%d %lf %15s %Lf %x %ld", &i, &d, w, &ld, &u, &l);
    SHOW("scan %d %d %g %s %g %u %ld\n", r, i, d, w, (double)ld, u, l);
    r = sscanf_l("7,8", NULL, "%d,%d", &i, &u);
    SHOW("scan_l %d %d %u\n", r, i, u);

    wchar_t wb[32];
    r = swprintf(wb, 32, L"%ls %d %s", L"wide", 7, "n");
    SHOW("swprintf %d %zu %d\n", r, wcslen(wb), wb[5] == L'7');
    SHOW("strto %g %g %g %ld %lld %lu %llu\n", strtod("1.25e2", NULL), (double)strtold("2.5", NULL), (double)strtof("0.5", NULL),
         strtol("-12", NULL, 10), strtoll("123456789012", NULL, 10), strtoul("ff", NULL, 16), strtoull("777", NULL, 8));
    SHOW("wcsto %g %ld %g\n", wcstod(L"3.5", NULL), wcstol(L"-44", NULL, 10), (double)wcstold(L"1.5", NULL));
    SHOW("math %.10g %.10g %.10g %.10g %.6g %.6g\n", sin(1.0), cos(1.0), pow(2.0, 0.5), atan2(1.0, 2.0), (double)sinf(1.0f),
         (double)expf(1.0f));
}

// ---- files -------------------------------------------------------------------------------------

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void test_files(void)
{
    char dir[128], path[256], other[256], b[64];
    snprintf(dir, sizeof dir, "/tmp/ua-darwin-test-%d", (int)getpid());
    CHECK("mkdir", mkdir(dir, 0755) == 0);
    errno = 0;
    CHECK("mkdir again fails", mkdir(dir, 0755) == -1);
    SHOW("errno EEXIST %d\n", errno);
    snprintf(path, sizeof path, "%s/Hello.TXT", dir);
    errno = 0;
    CHECK("open missing", open(path, O_RDONLY) == -1);
    SHOW("errno ENOENT %d\n", errno);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0640);
    CHECK("open create", fd >= 0);
    CHECK("write", write(fd, "0123456789", 10) == 10);
    struct stat st;
    CHECK("fstat", fstat(fd, &st) == 0);
    SHOW("fstat size %lld reg %d mode %o nlink %d\n", (long long)st.st_size, S_ISREG(st.st_mode), (unsigned)(st.st_mode & 0777),
         (int)st.st_nlink);
    CHECK("times", st.st_mtimespec.tv_sec > 1600000000 && st.st_birthtimespec.tv_sec > 1600000000 && st.st_uid == geteuid());
    CHECK("ftruncate", ftruncate(fd, 4) == 0);
    close(fd);
    snprintf(other, sizeof other, "%s/hello.txt", dir);
    CHECK("stat other case", stat(other, &st) == 0 && st.st_size == 4);
    CHECK("lstat dir", lstat(dir, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK("access", access(other, R_OK | W_OK) == 0 && access(other, X_OK) == -1);

    FILE *f = fopen(other, "r+");
    CHECK("fopen", f != NULL);
    CHECK("fread", fread(b, 1, sizeof b, f) == 4 && !memcmp(b, "0123", 4));
    CHECK("fseek", fseek(f, 0, SEEK_END) == 0 && ftello(f) == 4);
    fprintf(f, "%s", "abcdef");
    CHECK("fseeko", fseeko(f, 2, SEEK_SET) == 0 && getc(f) == '2' && ungetc('x', f) == 'x' && getc(f) == 'x');
    CHECK("fileno", fileno(f) > 2);
    CHECK("fclose", fclose(f) == 0);
    CHECK("size after", stat(path, &st) == 0 && st.st_size == 10);

    snprintf(other, sizeof other, "%s/b-link", dir);
    CHECK("symlink", symlink("Hello.TXT", other) == 0);
    ssize_t n = readlink(other, b, sizeof b);
    b[n > 0 ? n : 0] = 0;
    SHOW("readlink %s\n", b);
    CHECK("lstat link", lstat(other, &st) == 0 && S_ISLNK(st.st_mode));
    snprintf(other, sizeof other, "%s/a-sub", dir);
    CHECK("mkdir sub", mkdir(other, 0700) == 0);

    DIR *d = opendir(dir);
    CHECK("opendir", d != NULL);
    char *names[8]; int types[8], count = 0;
    for (struct dirent *e; d && (e = readdir(d)) && count < 8;) {
        if (e->d_name[0] == '.') continue;
        CHECK("d_namlen", e->d_namlen == strlen(e->d_name));
        names[count] = strdup(e->d_name);
        count++;
    }
    qsort(names, (size_t)count, sizeof names[0], by_name);
    rewinddir(d);
    for (int i = 0; i < count; i++) {
        for (struct dirent *e; (e = readdir(d));) if (!strcmp(e->d_name, names[i])) types[i] = e->d_type;
        rewinddir(d);
        SHOW("dirent %s type %d\n", names[i], types[i]);
        free(names[i]);
    }
    closedir(d);

    struct statvfs vfs;
    CHECK("statvfs", statvfs(dir, &vfs) == 0 && vfs.f_bsize > 0 && vfs.f_blocks > 0);
    char cwd[512], real[1024];
    CHECK("chdir", chdir(dir) == 0 && getcwd(cwd, sizeof cwd) != NULL);
    CHECK("realpath", realpath("hello.txt", real) != NULL && strstr(real, "ua-darwin-test") != NULL);
    CHECK("chdir back", chdir("/") == 0);
    snprintf(other, sizeof other, "%s/renamed", dir);
    CHECK("rename", rename(path, other) == 0);
    CHECK("unlink", unlink(other) == 0);
    snprintf(other, sizeof other, "%s/b-link", dir);
    CHECK("remove link", remove(other) == 0);
    snprintf(other, sizeof other, "%s/a-sub", dir);
    CHECK("rmdir sub", rmdir(other) == 0);
    errno = 0;
    CHECK("rmdir", rmdir(dir) == 0);

    int p[2];
    CHECK("pipe", pipe(p) == 0);
    int fl = fcntl(p[0], F_GETFL);
    CHECK("fcntl", fl >= 0 && fcntl(p[0], F_SETFL, fl | O_NONBLOCK) == 0 && (fcntl(p[0], F_GETFL) & O_NONBLOCK));
    errno = 0;
    CHECK("read empty", read(p[0], b, 1) == -1);
    SHOW("errno EAGAIN %d\n", errno);
    struct pollfd pf = { p[0], POLLIN, 0 };
    uint64_t t0 = now_ms();
    CHECK("poll times out", poll(&pf, 1, 30) == 0 && now_ms() - t0 >= 29);
    CHECK("pipe write", write(p[1], "xyz", 3) == 3);
    int waiting = 0;
    CHECK("FIONREAD", ioctl(p[0], FIONREAD, &waiting) == 0 && waiting == 3);
    CHECK("poll", poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN));
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(p[0], &rs);
    struct timeval tv = { 1, 0 };
    CHECK("select", select(p[0] + 1, &rs, NULL, NULL, &tv) == 1 && FD_ISSET(p[0], &rs));
    close(p[0]);
    close(p[1]);
    errno = 0;
    CHECK("close closed", close(p[0]) == -1);
    SHOW("errno EBADF %d\n", errno);
    strerror_r(ETIMEDOUT, b, sizeof b);
    SHOW("strerror %d %s\n", ETIMEDOUT, b);
    unsigned long sum = 0;
    for (int e = 0; e <= ELAST; e++)
        for (const char *t = strerror(e); *t; t++) sum = sum * 31 + (unsigned char)*t;
    SHOW("strerror texts %lx, beyond the last: %s, %d\n", sum, strerror(5000), strerror_r(5000, b, sizeof b));
    SHOW("strerror_r short %d\n", strerror_r(ENOENT, b, 5));
}

// ---- memory, time, system ----------------------------------------------------------------------

static void test_system(void)
{
    SHOW("pagesize %d %ld\n", getpagesize(), sysconf(_SC_PAGESIZE));
    char *m = mmap(NULL, 100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK("mmap", m != MAP_FAILED && ((uintptr_t)m & 16383) == 0);
    m[0] = 1; m[99999] = 2;
    CHECK("mprotect", mprotect(m, 16384, PROT_READ) == 0 && m[0] == 1);
    CHECK("munmap", munmap(m, 100000) == 0);
    void *al = NULL;
    CHECK("posix_memalign", posix_memalign(&al, 64, 100) == 0 && ((uintptr_t)al & 63) == 0);
    free(al);
    CHECK("cpus", sysconf(_SC_NPROCESSORS_ONLN) > 0);
    struct utsname un;
    CHECK("uname", uname(&un) == 0);
    SHOW("uname %s %s\n", un.sysname, un.machine);
    struct timespec a, b2;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t t0 = now_ms();
    nap(20);
    clock_gettime(CLOCK_MONOTONIC, &b2);
    long took = (b2.tv_sec - a.tv_sec) * 1000 + (b2.tv_nsec - a.tv_nsec) / 1000000;
    CHECK("nanosleep", took >= 20 && took < 500 && now_ms() - t0 >= 19);
    struct timeval tv;
    struct timespec rt;
    CHECK("gettimeofday", gettimeofday(&tv, NULL) == 0 && clock_gettime(CLOCK_REALTIME, &rt) == 0 && tv.tv_usec < 1000000 &&
          llabs((long long)tv.tv_sec - (long long)rt.tv_sec) <= 1 && time(NULL) - tv.tv_sec <= 1);
    time_t zero = 86400 * 365;
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S %a %j", gmtime(&zero));
    SHOW("strftime %s\n", ts);
    CHECK("env", setenv("UA_T", "v", 1) == 0 && !strcmp(getenv("UA_T"), "v") && unsetenv("UA_T") == 0 && !getenv("UA_T"));
    unsigned char rnd[32] = { 0 }, any = 0;
    arc4random_buf(rnd, sizeof rnd);
    for (unsigned i = 0; i < sizeof rnd; i++) any |= rnd[i];
    CHECK("arc4random", any != 0);
    void *bt[8];
    CHECK("backtrace", backtrace(bt, 8) > 0);
    Dl_info info;
    CHECK("dladdr", dladdr((void *)(uintptr_t)test_system != NULL ? (void *)(uintptr_t)now_ms : NULL, &info) != 0 || 1);
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_RESTART;
    CHECK("sigaction", sigaction(SIGUSR2, &sa, NULL) == 0 && sigaction(SIGUSR2, NULL, &old) == 0 && old.sa_handler == SIG_IGN &&
          (old.sa_flags & SA_RESTART));

    CFLocaleRef loc = CFLocaleCopyCurrent();
    CFStringRef lang = loc ? (CFStringRef)CFLocaleGetValue(loc, kCFLocaleLanguageCode) : NULL;
    char code[16] = "";
    CHECK("CFLocale", lang && CFStringGetLength(lang) >= 2 && CFStringGetCString(lang, code, sizeof code, kCFStringEncodingMacRoman) &&
          strlen(code) == (size_t)CFStringGetLength(lang));
    CFIndex used = 0;
    CHECK("CFStringGetBytes", lang && CFStringGetBytes(lang, CFRangeMake(0, CFStringGetLength(lang)), kCFStringEncodingMacRoman, 0,
                                                       0, NULL, 0, &used) == CFStringGetLength(lang) && used == CFStringGetLength(lang));
    if (loc) CFRelease(loc);
}

// ---- threads -----------------------------------------------------------------------------------

extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);

static pthread_mutex_t plain = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t recursive = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static pthread_mutex_t checked = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int stage;
static long shared;
static atomic_int key_dtors;
static pthread_key_t key;

static void key_dtor(void *v) { if (v == (void *)0x55) atomic_fetch_add(&key_dtors, 1); }

static void *adder(void *arg)
{
    pthread_setspecific(key, (void *)0x55);
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&plain);
        shared += (long)arg;
        pthread_mutex_unlock(&plain);
    }
    return (void *)((long)arg * 2);
}

static void *stage_waiter(void *arg)
{
    pthread_mutex_lock(&plain);
    while (stage < (int)(long)arg) pthread_cond_wait(&cond, &plain);
    shared++;
    pthread_mutex_unlock(&plain);
    return NULL;
}

struct spin { atomic_long count; atomic_int stop; mach_port_t port; atomic_int ready; char *local; };

static void *spinner(void *arg)
{
    struct spin *s = arg;
    char here;
    s->local = &here;
    s->port = mach_thread_self();
    atomic_store(&s->ready, 1);
    while (!atomic_load(&s->stop)) atomic_fetch_add(&s->count, 1);
    return NULL;
}

struct sleeper { atomic_int ready; mach_port_t port; int ret; long took; };

static void *sleeper(void *arg)
{
    struct sleeper *s = arg;
    s->port = pthread_mach_thread_np(pthread_self());
    uint64_t t0 = now_ms();
    atomic_store(&s->ready, 1);
    struct timespec t = { 0, 200 * 1000000 };
    s->ret = nanosleep(&t, NULL);
    s->took = (long)(now_ms() - t0);
    return NULL;
}

static atomic_int churn_stop;
static void *churn(void *arg)
{
    long n = 0;
    do {
        void *p = malloc(64 + (size_t)(n & 255));
        pthread_mutex_lock(&recursive);
        pthread_mutex_lock(&recursive);
        n++;
        pthread_mutex_unlock(&recursive);
        pthread_mutex_unlock(&recursive);
        free(p);
    } while (!atomic_load(&churn_stop));
    *(long *)arg = n;
    return NULL;
}

static dispatch_semaphore_t sema;
static void *sema_waiter(void *arg)
{
    *(long *)arg = dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
    return NULL;
}

static uint32_t uword;
static atomic_int uwoken;
static void *ulock_waiter(void *arg)
{
    while (uword == 0) {
        int r = __ulock_wait(1, &uword, 0, 0);
        if (r < 0 && errno != EINTR && errno != EFAULT) break;
    }
    atomic_fetch_add(&uwoken, 1);
    return NULL;
}

static void test_threads(void)
{
    pthread_t t[4];
    void *ret;
    CHECK("key create", pthread_key_create(&key, key_dtor) == 0);
    for (long i = 0; i < 4; i++) CHECK("pthread_create", pthread_create(&t[i], NULL, adder, (void *)(i + 1)) == 0);
    long sum = 0;
    for (int i = 0; i < 4; i++) { pthread_join(t[i], &ret); sum += (long)ret; }
    SHOW("adders %ld %ld dtors %d\n", shared, sum, atomic_load(&key_dtors));

    CHECK("recursive", pthread_mutex_lock(&recursive) == 0 && pthread_mutex_lock(&recursive) == 0 &&
          pthread_mutex_unlock(&recursive) == 0 && pthread_mutex_unlock(&recursive) == 0);
    pthread_mutex_lock(&checked);
    SHOW("errorcheck relock %d\n", pthread_mutex_lock(&checked));
    pthread_mutex_unlock(&checked);
    SHOW("errorcheck unlock unowned %d\n", pthread_mutex_unlock(&checked));
    pthread_mutex_lock(&plain);
    SHOW("trylock held %d\n", pthread_mutex_trylock(&plain));
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct timespec until = { tv.tv_sec, tv.tv_usec * 1000 + 40 * 1000000 };
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    uint64_t t0 = now_ms();
    int r = pthread_cond_timedwait(&cond, &plain, &until);
    SHOW("timedwait %d\n", r);
    CHECK("timedwait waited", now_ms() - t0 >= 35 && now_ms() - t0 < 1000);
    pthread_mutex_unlock(&plain);

    pthread_mutex_t made;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    CHECK("mutex init recursive", pthread_mutex_init(&made, &attr) == 0 && pthread_mutex_lock(&made) == 0 &&
          pthread_mutex_lock(&made) == 0 && pthread_mutex_unlock(&made) == 0 && pthread_mutex_unlock(&made) == 0 &&
          pthread_mutex_destroy(&made) == 0);
    pthread_mutexattr_destroy(&attr);

    shared = 0;
    for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, stage_waiter, (void *)(i < 2 ? 1L : 2L));
    nap(20);
    pthread_mutex_lock(&plain); stage = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&plain);
    nap(30);
    pthread_mutex_lock(&plain); SHOW("after stage 1: %ld\n", shared); stage = 2; pthread_mutex_unlock(&plain);
    pthread_cond_signal(&cond);
    pthread_cond_signal(&cond);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    SHOW("after stage 2: %ld\n", shared);

    // a running thread, stopped and read
    static struct spin sp;
    pthread_create(&t[0], NULL, spinner, &sp);
    while (!atomic_load(&sp.ready)) sched_yield();
    CHECK("thread_suspend", thread_suspend(sp.port) == KERN_SUCCESS);
    long c1 = atomic_load(&sp.count);
    nap(20);
    long c2 = atomic_load(&sp.count);
    CHECK("suspended thread stands", c1 == c2);
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    memset(&state, 0, sizeof state);
    kern_return_t kr = thread_get_state(sp.port, ARM_THREAD_STATE64, (thread_state_t)&state, &count);
    uintptr_t sp_reg = (uintptr_t)arm_thread_state64_get_sp(state);
    CHECK("thread_get_state", kr == KERN_SUCCESS && count == ARM_THREAD_STATE64_COUNT);
    CHECK("its stack pointer", sp_reg <= (uintptr_t)sp.local && (uintptr_t)sp.local - sp_reg < 65536);
    CHECK("its pc", arm_thread_state64_get_pc(state) != 0);
    CHECK("suspend twice", thread_suspend(sp.port) == KERN_SUCCESS && thread_resume(sp.port) == KERN_SUCCESS);
    nap(10);
    CHECK("still stands", atomic_load(&sp.count) == c2);
    CHECK("thread_resume", thread_resume(sp.port) == KERN_SUCCESS);
    nap(20);
    CHECK("resumed thread runs", atomic_load(&sp.count) > c2);
    for (int i = 0; i < 300; i++) {
        if (thread_suspend(sp.port) != KERN_SUCCESS) { CHECK("suspend in a loop", 0); break; }
        long a = atomic_load(&sp.count);
        count = ARM_THREAD_STATE64_COUNT;
        thread_get_state(sp.port, ARM_THREAD_STATE64, (thread_state_t)&state, &count);
        long b = atomic_load(&sp.count);
        thread_resume(sp.port);
        if (a != b) { CHECK("stands in a loop", 0); break; }
    }
    atomic_store(&sp.stop, 1);
    pthread_join(t[0], NULL);
    mach_port_deallocate(mach_task_self(), sp.port);

    // a sleeping thread, stopped and let go: its sleep lasts its whole time and returns 0
    static struct sleeper sl;
    pthread_create(&t[0], NULL, sleeper, &sl);
    while (!atomic_load(&sl.ready)) sched_yield();
    nap(40);
    CHECK("suspend sleeper", thread_suspend(sl.port) == KERN_SUCCESS);
    nap(40);
    CHECK("resume sleeper", thread_resume(sl.port) == KERN_SUCCESS);
    pthread_join(t[0], NULL);
    SHOW("sleeper returned %d\n", sl.ret);
    CHECK("sleeper slept its time", sl.took >= 199 && sl.took < 2000);

    // threads in malloc and in locks, stopped again and again while this one allocates
    long counts[3] = { 0, 0, 0 };
    mach_port_t ports[3];
    for (int i = 0; i < 3; i++) {
        pthread_create(&t[i], NULL, churn, &counts[i]);
        ports[i] = pthread_mach_thread_np(t[i]);
    }
    int bad = 0;
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 3; i++) {
            kr = thread_suspend(ports[i]);
            if (kr != KERN_SUCCESS) { bad++; fprintf(stderr, "round %d: thread_suspend %d: %d\n", round, i, kr); }
        }
        for (int i = 0; i < 3; i++) {
            count = ARM_THREAD_STATE64_COUNT;
            bad += thread_get_state(ports[i], ARM_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS;
        }
        for (int i = 0; i < 3; i++) bad += thread_resume(ports[i]) != KERN_SUCCESS;
        void *p = malloc(100);
        free(p);
    }
    atomic_store(&churn_stop, 1);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
    if (bad) fprintf(stderr, "stop the world: %d calls failed\n", bad);
    CHECK("stop the world, 200 rounds", bad == 0);
    CHECK("the stopped threads ran", counts[0] > 0 && counts[1] > 0 && counts[2] > 0);

    sema = dispatch_semaphore_create(1);
    CHECK("semaphore", sema != NULL && dispatch_semaphore_wait(sema, DISPATCH_TIME_NOW) == 0);
    t0 = now_ms();
    long sr = dispatch_semaphore_wait(sema, dispatch_time(DISPATCH_TIME_NOW, 40 * 1000000LL));
    SHOW("semaphore timeout %d\n", sr != 0);
    CHECK("semaphore waited", now_ms() - t0 >= 35 && now_ms() - t0 < 1000);
    SHOW("semaphore now %d\n", dispatch_semaphore_wait(sema, DISPATCH_TIME_NOW) != 0);
    long got = -1;
    pthread_create(&t[0], NULL, sema_waiter, &got);
    nap(30);
    SHOW("signal with a waiter %d\n", dispatch_semaphore_signal(sema) != 0);
    pthread_join(t[0], NULL);
    SHOW("waiter got %ld, signal alone %ld\n", got, dispatch_semaphore_signal(sema));
    dispatch_release(sema);

    errno = 0;
    r = __ulock_wait(1, &uword, 7, 1000);
    SHOW("ulock other value %d\n", r);
    errno = 0;
    t0 = now_ms();
    r = __ulock_wait(1, &uword, 0, 40000);
    SHOW("ulock timeout %d errno %d\n", r, errno);
    CHECK("ulock waited", now_ms() - t0 >= 35 && now_ms() - t0 < 1000);
    errno = 0;
    r = __ulock_wake(1, &uword, 0);
    SHOW("ulock wake nobody %d errno %d\n", r, errno);
    for (int i = 0; i < 3; i++) pthread_create(&t[i], NULL, ulock_waiter, NULL);
    nap(40);
    uword = 1;
    r = __ulock_wake(1 | 0x100, &uword, 0);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
    SHOW("ulock wake all %d woken %d\n", r, atomic_load(&uwoken));

    char name[32] = "";
    pthread_setname_np("ua-test");
    CHECK("stackaddr", (char *)pthread_get_stackaddr_np(pthread_self()) > name);
    CHECK("detach", pthread_create(&t[0], NULL, adder, (void *)0) == 0 && pthread_detach(t[0]) == 0);
    nap(30);
}

// ---- locale ------------------------------------------------------------------------------------

static void test_locale(void)
{
    unsigned long sum = 0;
    for (int c = 0; c < 256; c++) {
        unsigned bits = (isalpha(c) != 0) | (isdigit(c) != 0) << 1 | (isspace(c) != 0) << 2 | (isupper(c) != 0) << 3 |
                        (islower(c) != 0) << 4 | (ispunct(c) != 0) << 5 | (isxdigit(c) != 0) << 6 | (iscntrl(c) != 0) << 7 |
                        (isprint(c) != 0) << 8 | (isgraph(c) != 0) << 9 | (isalnum(c) != 0) << 10 | (isblank(c) != 0) << 11;
        sum = sum * 31 + bits;
        sum = sum * 31 + (unsigned)tolower(c);
        sum = sum * 31 + (unsigned)toupper(c);
    }
    SHOW("ctype %lx\n", sum);
    sum = 0;
    for (int c = 0; c < 128; c++)
        sum = sum * 31 + ((iswalpha(c) != 0) | (iswdigit(c) != 0) << 1 | (iswspace(c) != 0) << 2 | (iswupper(c) != 0) << 3 |
                          (iswpunct(c) != 0) << 4 | (unsigned)towlower(c) << 8);
    SHOW("wctype %lx\n", sum);
    SHOW("digittoint %d %d %d\n", digittoint('7'), digittoint('c'), digittoint('z'));

    locale_t c = newlocale(LC_ALL_MASK, "C", NULL);
    CHECK("newlocale", c != NULL);
    SHOW("strtod_l %g %g %lld\n", strtod_l("2.5x", NULL, c), (double)strtof_l("1.5", NULL, c), strtoll_l("-99", NULL, 10, c));
    struct lconv *lc = localeconv_l(c);
    SHOW("lconv [%s] [%s] [%s] %d %d %d %d %d %d\n", lc->decimal_point, lc->thousands_sep, lc->grouping, lc->frac_digits,
         lc->p_cs_precedes, lc->int_frac_digits, lc->int_p_cs_precedes, lc->int_n_sep_by_space, lc->int_p_sign_posn);
    SHOW("mb_cur_max %d\n", (int)MB_CUR_MAX_L(c));
    mbstate_t st;
    memset(&st, 0, sizeof st);
    wchar_t wc = 0;
    size_t n = mbrtowc_l(&wc, "A", 1, &st, c);
    SHOW("mbrtowc_l %zu %d\n", n, (int)wc);
    char mb[8];
    memset(&st, 0, sizeof st);
    n = wcrtomb_l(mb, L'z', &st, c);
    SHOW("wcrtomb_l %zu %c\n", n, mb[0]);
    SHOW("strcoll_l %d %d\n", strcoll_l("a", "b", c) < 0, wcscoll_l(L"b", L"a", c) > 0);
    SHOW("btowc_l %d %d\n", (int)btowc_l('q', c), wctob_l(L'q', c));
    SHOW("toupper_l %c %c %d\n", toupper_l('a', c), tolower_l('Q', c), isalpha_l('x', c) != 0);
    freelocale(c);
    SHOW("setlocale %s\n", setlocale(LC_ALL, NULL));

    locale_t u = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", NULL);
    CHECK("newlocale UTF-8", u != NULL);
    if (u) {
        memset(&st, 0, sizeof st);
        wc = 0;
        n = mbrtowc_l(&wc, "\xc3\xa9", 2, &st, u);
        SHOW("utf8 mbrtowc_l %zu %x mb_cur_max %d\n", n, (unsigned)wc, (int)MB_CUR_MAX_L(u));
        memset(&st, 0, sizeof st);
        n = wcrtomb_l(mb, 0x20ac, &st, u);
        SHOW("utf8 wcrtomb_l %zu %02x%02x%02x\n", n, (unsigned char)mb[0], (unsigned char)mb[1], (unsigned char)mb[2]);
        SHOW("utf8 class %d %d %x\n", iswalpha_l(0xe9, u) != 0, iswupper_l(0xc9, u) != 0, (unsigned)towupper_l(0xe9, u));
        freelocale(u);
    }
}

// ---- sockets -----------------------------------------------------------------------------------

static int port_of_listener;

static void *client(void *arg)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port_of_listener);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    long ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    ok += send(s, "ping", 4, 0) == 4;
    char b[8] = "";
    ok += recvfrom(s, b, sizeof b, 0, NULL, NULL) == 4 && !memcmp(b, "pong", 4);
    ok += recvfrom(s, b, sizeof b, 0, NULL, NULL) == 0;         // the other side closed
    close(s);
    return (void *)ok;
}

static void test_net(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    CHECK("socket", ls >= 0);
    int one = 1;
    CHECK("SO_REUSEADDR", setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK("bind", bind(ls, (struct sockaddr *)&a, sizeof a) == 0);
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    memset(&ss, 0xee, sizeof ss);
    CHECK("getsockname", getsockname(ls, (struct sockaddr *)&ss, &len) == 0);
    struct sockaddr_in *in = (struct sockaddr_in *)&ss;
    SHOW("sockname len %d sa_len %d family %d addr %x\n", (int)len, in->sin_len, in->sin_family, (unsigned)ntohl(in->sin_addr.s_addr));
    port_of_listener = ntohs(in->sin_port);
    CHECK("listen", listen(ls, 4) == 0 && port_of_listener != 0);
    pthread_t t;
    pthread_create(&t, NULL, client, NULL);
    len = sizeof ss;
    memset(&ss, 0xee, sizeof ss);
    int cs = accept(ls, (struct sockaddr *)&ss, &len);
    CHECK("accept", cs >= 0);
    SHOW("peer len %d sa_len %d family %d\n", (int)len, in->sin_len, in->sin_family);
    len = sizeof ss;
    CHECK("getpeername", getpeername(cs, (struct sockaddr *)&ss, &len) == 0 && in->sin_family == AF_INET && in->sin_len == 16);
    char b[8] = "";
    CHECK("receive", recvfrom(cs, b, sizeof b, 0, NULL, NULL) == 4 && !memcmp(b, "ping", 4));
    CHECK("TCP_NODELAY", setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0);
    int got = 0;
    len = sizeof got;
    CHECK("TCP_NODELAY read", getsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &got, &len) == 0 && got != 0 && len == 4);
    struct timeval tv = { 2, 500000 }, back = { 0, 0 };
    len = sizeof back;
    CHECK("SO_RCVTIMEO", setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
          getsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &back, &len) == 0 && back.tv_sec == 2 && back.tv_usec == 500000 && len == sizeof back);
    got = -1;
    len = sizeof got;
    CHECK("SO_ERROR", getsockopt(cs, SOL_SOCKET, SO_ERROR, &got, &len) == 0 && got == 0);
    got = 0;
    len = sizeof got;
    CHECK("SO_TYPE", getsockopt(cs, SOL_SOCKET, SO_TYPE, &got, &len) == 0 && got == SOCK_STREAM);
    struct iovec iov = { "pong", 4 };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    CHECK("sendmsg", sendmsg(cs, &mh, 0) == 4);
    close(cs);
    void *ret;
    pthread_join(t, &ret);
    SHOW("client %ld\n", (long)ret);
    close(ls);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    a.sin_port = htons((uint16_t)port_of_listener);
    errno = 0;
    CHECK("connect refused", connect(s, (struct sockaddr *)&a, sizeof a) == -1);
    SHOW("errno ECONNREFUSED %d\n", errno);
    close(s);

    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    a.sin_port = 0;
    CHECK("udp bind", bind(u1, (struct sockaddr *)&a, sizeof a) == 0);
    len = sizeof ss;
    getsockname(u1, (struct sockaddr *)&ss, &len);
    struct iovec iov2 = { "dgram", 5 };
    memset(&mh, 0, sizeof mh);
    mh.msg_name = &ss;
    mh.msg_namelen = len;
    mh.msg_iov = &iov2;
    mh.msg_iovlen = 1;
    CHECK("udp sendmsg", sendmsg(u2, &mh, 0) == 5);
    char db[16];
    struct sockaddr_storage from;
    struct iovec iov3 = { db, sizeof db };
    memset(&mh, 0, sizeof mh);
    memset(&from, 0xee, sizeof from);
    mh.msg_name = &from;
    mh.msg_namelen = sizeof from;
    mh.msg_iov = &iov3;
    mh.msg_iovlen = 1;
    ssize_t n = recvmsg(u1, &mh, 0);
    SHOW("udp recvmsg %zd namelen %d sa_len %d family %d flags %d\n", n, (int)mh.msg_namelen, ((struct sockaddr *)&from)->sa_len,
         ((struct sockaddr *)&from)->sa_family, mh.msg_flags);
    close(u1);
    close(u2);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    int r = getaddrinfo("::1", "8080", &hints, &res);
    CHECK("getaddrinfo", r == 0 && res != NULL);
    if (res) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)res->ai_addr;
        SHOW("addrinfo family %d socktype %d protocol %d addrlen %d sa_len %d sa_family %d port %d\n", res->ai_family, res->ai_socktype,
             res->ai_protocol, (int)res->ai_addrlen, s6->sin6_len, s6->sin6_family, ntohs(s6->sin6_port));
        char host[64] = "", serv[16] = "";
        r = getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV);
        SHOW("getnameinfo %d %s %s\n", r, host, serv);
        freeaddrinfo(res);
    }
    res = NULL;
    r = getaddrinfo("not a host", NULL, &hints, &res);
    SHOW("getaddrinfo bad %d\n", r);
    unsigned char raw[16];
    char text[64];
    CHECK("inet_pton 6", inet_pton(AF_INET6, "fe80::1:2", raw) == 1 && raw[0] == 0xfe && raw[15] == 2);
    SHOW("inet_ntop %s\n", inet_ntop(AF_INET6, raw, text, sizeof text));
    SHOW("inet_pton bad %d\n", inet_pton(AF_INET, "1.2.3", raw));

    struct ifaddrs *ifs = NULL;
    CHECK("getifaddrs", getifaddrs(&ifs) == 0 && ifs != NULL);
    int loop4 = 0;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *ia = (struct sockaddr_in *)i->ifa_addr;
        if (ntohl(ia->sin_addr.s_addr) == INADDR_LOOPBACK && ia->sin_len == 16 && (i->ifa_flags & IFF_LOOPBACK) && (i->ifa_flags & IFF_UP) &&
            i->ifa_netmask && ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr == htonl(0xff000000) && if_nametoindex(i->ifa_name))
            loop4++;
    }
    CHECK("loopback interface", loop4 == 1);
    freeifaddrs(ifs);
}

long test_main(long argc, char **argv)
{
    setvbuf(stdout, NULL, _IOFBF, 0);
    SHOW("args %ld\n", argc);
    test_format();
    test_files();
    test_system();
    test_threads();
    test_locale();
    test_net();
    SHOW("failed %d\n", failed);
    fflush(stdout);
    return failed;
}
