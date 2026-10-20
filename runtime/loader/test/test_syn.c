// Loads the synthetic Android libraries (syn_lib.c, syn_dep.c; built and packed by ../build.sh test)
// with so_loader and checks: the five relocation encodings of libsyn_0..4.so (plain; RELR; all in
// Android's APS2 packing; RELR + APS2; Android's pre-standard RELR tags + APS2) give the same image;
// imports through the shim tables, the host and the dependency; init order; dlopen/dlsym routing;
// bionic-layout setjmp, signals, pthread objects at 4 mod 8, struct stat; the stack protector
// (arm64: bionic's TLS slot at tpidr_el0 + 0x28); a corrupt packed table fails cleanly; arm64: an
// unwinder inside the library walks through Android and host frames. Usage: test_syn <dir>
#define _GNU_SOURCE
#include "../so_loader.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int so_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data);

static int order;
static int next_order(void) { return ++order; }
static int shim_add(int a, int b) { return a + b; }
static int shim_data = 1234;
static const so_shim test_shims[] = {
    { "test_next_order", (void *)next_order }, { "test_shim_add", (void *)shim_add }, { "test_shim_data", &shim_data },
    { NULL, NULL }
};

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct modinfo { const char *name; uintptr_t bias; const ElfW(Phdr) *ph; int phnum; };
static int find_mod(struct dl_phdr_info *i, size_t size, void *data)
{
    (void)size;
    struct modinfo *m = data;
    const char *b = strrchr(i->dlpi_name, '/');
    if (!b || strcmp(b + 1, m->name)) return 0;
    m->bias = i->dlpi_addr; m->ph = i->dlpi_phdr; m->phnum = i->dlpi_phnum;
    return 1;
}

// The writable segments of two copies of one library, word by word; a pointer into its own image
// counts as its offset. Left out: the dynamic section (rewritten by the packer) and .bss (no
// relocations there, and the libraries' own variables differ).
static int compare_images(so_module *a, so_module *b)
{
    struct modinfo ma = { so_name(a), 0, NULL, 0 }, mb = { so_name(b), 0, NULL, 0 };
    so_dl_iterate_phdr(find_mod, &ma);
    so_dl_iterate_phdr(find_mod, &mb);
    if (!ma.ph || !mb.ph || ma.phnum != mb.phnum) return -1;
    uintptr_t dyn_lo = 0, dyn_hi = 0;
    for (int i = 0; i < ma.phnum; i++)
        if (ma.ph[i].p_type == PT_DYNAMIC) { dyn_lo = ma.ph[i].p_vaddr; dyn_hi = dyn_lo + ma.ph[i].p_memsz; }
    int diffs = 0, words = 0;
    const uintptr_t W = sizeof(uintptr_t);
    for (int i = 0; i < ma.phnum; i++) {
        const ElfW(Phdr) *p = &ma.ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_W)) continue;
        for (uintptr_t off = (p->p_vaddr + W - 1) & ~(W - 1); off + W <= p->p_vaddr + p->p_filesz; off += W) {
            if (off >= dyn_lo && off < dyn_hi) continue;
            uintptr_t va = *(uintptr_t *)(ma.bias + off), vb = *(uintptr_t *)(mb.bias + off);
            int ia = va >= so_base(a) && va < so_base(a) + so_size(a), ib = vb >= so_base(b) && vb < so_base(b) + so_size(b);
            if (ia) va -= so_base(a);
            if (ib) vb -= so_base(b);
            words++;
            if (ia != ib || va != vb) {
                if (diffs++ < 5) printf("  %s+0x%lx: %s%lx, %s: %s%lx\n", so_name(a), (unsigned long)off, ia ? "+" : "",
                                        (unsigned long)va, so_name(b), ib ? "+" : "", (unsigned long)vb);
            }
        }
    }
    return words ? diffs : -1;
}

static void *sym(so_module *m, const char *n)
{
    void *p = so_sym(m, n);
    if (!p) { printf("FAIL %s: no %s\n", so_name(m), n); fails++; }
    return p;
}

#if defined(__aarch64__)
static unsigned long (*tls_guard)(void);
static int (*protected_fn)(int);
static void *host_thread(void *a) { (void)a; protected_fn(3); return (void *)tls_guard(); }

static so_module *m0;
static int (*backtrace_fn)(uintptr_t *, int);
static uintptr_t pcs[32];
static int npcs;
__attribute__((noinline)) static int host_cb(void) { npcs = backtrace_fn(pcs, 32); return npcs + 1; }
#endif

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    setvbuf(stdout, NULL, _IOLBF, 0);
    so_add_shims(test_shims);
    so_add_shims(bionic_compat_table());
    so_set_library_path(dir);

    static const char *const kinds[] = { "plain", "RELR", "APS2", "RELR+APS2", "Android RELR+APS2" };
    so_module *m[5];
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof path, "%s/libsyn_%d.so", dir, i);
        m[i] = so_load(path, NULL);
        if (!m[i]) { printf("FAIL load %s: %s\n", path, so_error()); return 1; }
        so_run_init(m[i]);
        CHECK(so_unresolved_count(m[i]) == 0, "%s: %d unresolved imports", so_name(m[i]), so_unresolved_count(m[i]));
        int (*check)(void) = (int (*)(void))sym(m[i], "syn_check");
        int r = check ? check() : -1;
        CHECK(r == 0, "%s (%s): syn_check failed, bits 0x%x", so_name(m[i]), kinds[i], r);
        if (i) {
            int d = compare_images(m[0], m[i]);
            CHECK(d == 0, "%s (%s): image differs from %s (%d words)", so_name(m[i]), kinds[i], so_name(m[0]), d);
        }
    }
    so_module *dep = so_find("libsyn_dep.so");
    CHECK(dep && so_unresolved_count(dep) == 0, "libsyn_dep.so not loaded or unresolved imports");

    int (*setjmp_test)(long) = (int (*)(long))sym(m[0], "syn_setjmp_test");
    int (*signal_test)(void) = (int (*)(void))sym(m[0], "syn_signal_test");
    int (*thread_test)(void) = (int (*)(void))sym(m[0], "syn_thread_test");
    int (*stat_test)(const char *, long long) = (int (*)(const char *, long long))sym(m[0], "syn_stat_test");
    void (*smash)(int) = (void (*)(int))sym(m[0], "syn_smash");
    if (fails) goto out;
    int r = setjmp_test(1000);
    CHECK(r == 0, "setjmp/longjmp: %d", r);
    r = signal_test();
    CHECK(r == 0, "signals: %d", r);
    r = thread_test();
    CHECK(r == 0, "pthread objects: %d", r);
    snprintf(path, sizeof path, "%s/libsyn_0.so", dir);
    struct stat st;
    stat(path, &st);
    r = stat_test(path, st.st_size);
    CHECK(r == 0, "stat: %d", r);

#if defined(__aarch64__)
    tls_guard = (unsigned long (*)(void))sym(m[0], "syn_tls_guard");
    protected_fn = (int (*)(int))sym(m[0], "syn_protected");
    backtrace_fn = (int (*)(uintptr_t *, int))sym(m[0], "syn_backtrace");
    int (*call_host)(int (*)(void)) = (int (*)(int (*)(void)))sym(m[0], "syn_call_host");
    if (fails) goto out;
    unsigned long g = tls_guard();
    CHECK(g && !(g & 0xff), "TLS stack guard %#lx", g);
    errno = 0;
    for (int i = 0; i < 100; i++) close(-1);         // errno and other glibc TLS must not land on the slot
    CHECK(tls_guard() == g && protected_fn(1) > 0, "TLS stack guard changed: %#lx -> %#lx", g, tls_guard());
    pthread_t t;
    void *tg = NULL;
    pthread_create(&t, NULL, host_thread, NULL);    // Android code on a thread the host started
    pthread_join(t, &tg);
    CHECK((unsigned long)tg == g, "guard in a host thread %#lx, main %#lx", (unsigned long)tg, g);

    // syn_call_host -> host_cb -> syn_backtrace: library, host, library, host frames
    m0 = m[0];
    call_host(host_cb);
    char seq[33] = "";
    int n = 0;
    for (int i = 0; i < npcs && n < 32; i++) {
        Dl_info di;
        char c = pcs[i] >= so_base(m0) && pcs[i] < so_base(m0) + so_size(m0) ? 'A'
                 : dladdr((void *)pcs[i], &di) && di.dli_fname && strstr(di.dli_fname, "test_syn") ? 'H' : 'o';
        if (!n || seq[n - 1] != c) seq[n++] = c, seq[n] = 0;
    }
    CHECK(!strncmp(seq, "AHAH", 4), "unwinding through Android and host frames: %d frames, %s", npcs, seq);
#else
    void *(*guard_addr)(void) = (void *(*)(void))sym(m[0], "syn_guard_addr");
    CHECK(guard_addr && guard_addr() == so_shim_lookup("__stack_chk_guard"), "__stack_chk_guard not bionic's");
#endif

    // overrunning a buffer in Android code ends in __stack_chk_fail (SIGABRT)
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);               // glibc's "stack smashing detected" goes to the tty
        if (fd >= 0) dup2(fd, 2);
        smash(64);
        _exit(0);
    }
    int ws = 0;
    waitpid(pid, &ws, 0);
    CHECK(WIFSIGNALED(ws) && WTERMSIG(ws) == SIGABRT, "stack overrun not caught (status %#x)", ws);

    // a truncated packed table: an error, not a crash
    snprintf(path, sizeof path, "%s/libsyn_x.so", dir);
    so_module *bad = so_load(path, NULL);
    CHECK(!bad && so_error() && strstr(so_error(), "packed"), "corrupt APS2 table: %s", bad ? "loaded" : so_error());

out:
    if (fails) printf("syn: %d FAILED\n", fails);
    else printf("syn: ok (%s: 5 relocation encodings alike, imports, init order, dl*, setjmp, signals, pthread, stat, stack guard%s)\n",
                sizeof(void *) == 8 ? "arm64" : "arm32", sizeof(void *) == 8 ? ", unwinding" : "");
    return fails != 0;
}
