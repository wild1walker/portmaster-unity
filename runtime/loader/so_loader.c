/*
 * so_loader.c - ELF loader for Android ARM shared libraries on glibc: armeabi-v7a (ELF32, REL
 * relocations) on armhf, arm64-v8a (ELF64, RELA) on aarch64. See so_loader.h. Our own code; ideas
 * from the Vita so_util.c and gmloader.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "so_loader.h"

/* the one kind of Android library this build loads: the host's own architecture */
#if defined(__aarch64__)
#define SO_ELFCLASS ELFCLASS64
#define SO_MACHINE  EM_AARCH64
#define SO_ARCH     "AArch64 (arm64-v8a)"
#elif defined(__arm__)
#define SO_ELFCLASS ELFCLASS32
#define SO_MACHINE  EM_ARM
#define SO_ARCH     "ARM32 (armeabi-v7a)"
#else
#error "so_loader runs Android ARM code: build it for armhf or aarch64"
#endif
/* ELF32_ / ELF64_ macros of that class (link.h's ElfW names the types only) */
#if defined(__LP64__)
#define ELFW(x) ELF64_##x
#else
#define ELFW(x) ELF32_##x
#endif

#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
#ifndef PT_GNU_RELRO
#define PT_GNU_RELRO 0x6474e552
#endif
#ifndef DT_RELRSZ
#define DT_RELRSZ 35
#endif
#ifndef DT_RELR
#define DT_RELR 36
#endif
#ifndef DT_RELRENT
#define DT_RELRENT 37
#endif
/* Android's packed relocations ("APS2", bionic linker_reloc_iterators.h) and its RELR tags from
 * before RELR was standard */
#define DT_ANDROID_REL     0x6000000f
#define DT_ANDROID_RELSZ   0x60000010
#define DT_ANDROID_RELA    0x60000011
#define DT_ANDROID_RELASZ  0x60000012
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003
#define APS2_GROUPED_BY_INFO         1
#define APS2_GROUPED_BY_OFFSET_DELTA 2
#define APS2_GROUPED_BY_ADDEND       4
#define APS2_GROUP_HAS_ADDEND        8
#ifndef R_ARM_IRELATIVE
#define R_ARM_IRELATIVE 160
#endif
#if defined(__aarch64__) && !defined(R_AARCH64_TLS_DTPMOD)   /* older elf.h: the ...64 names */
#define R_AARCH64_TLS_DTPMOD 1028
#define R_AARCH64_TLS_DTPREL 1029
#define R_AARCH64_TLS_TPREL  1030
#endif

/* bionic dlfcn values: LP32 and LP64 differ */
#define B_RTLD_LAZY    1
#define B_RTLD_NOLOAD  4
#if defined(__LP64__)
#define B_RTLD_GLOBAL  0x100
#define B_RTLD_DEFAULT ((void *)0)
#define B_RTLD_NEXT    ((void *)-1L)
#else
#define B_RTLD_GLOBAL  2
#define B_RTLD_DEFAULT ((void *)0xffffffff)
#define B_RTLD_NEXT    ((void *)0xfffffffe)
#endif
/* NULL, RTLD_DEFAULT, RTLD_NEXT and the other top values: never one of our handles */
#define PSEUDO_HANDLE(h) (!(h) || (uintptr_t)(h) >= (uintptr_t)-16)

#define PAGE_SZ 4096u
#define PAGE_DOWN(x) ((uintptr_t)(x) & ~(uintptr_t)(PAGE_SZ - 1))
#define PAGE_UP(x) PAGE_DOWN((uintptr_t)(x) + PAGE_SZ - 1)

/* ------------------------------------------------------------------------ */

enum { H_MODULE = 1, H_HOST, H_FAKE, H_GLOBAL };
#define HANDLE_MAGIC 0x534f484eu /* "SOHN" */

typedef struct so_handle {
    uint32_t magic;
    int kind;
    int refs;
    char *name;              /* android name (basename) */
    so_module *mod;          /* H_MODULE */
    void *host;              /* H_HOST: glibc handle */
    const so_shim *table;    /* H_FAKE: may be NULL */
    struct so_handle *next;
} so_handle;

typedef struct {
    const char *name;
    int is_func;
} unres_t;

struct so_module {
    struct so_module *next;
    char *path;
    const char *name;        /* basename of path */
    const char *soname;
    uintptr_t bias;
    uintptr_t map_start;
    size_t map_size;
    const ElfW(Phdr) *phdr;
    int phnum;
    ElfW(Dyn) *dyn;
    const ElfW(Sym) *symtab;
    const char *strtab;
    uint32_t nsyms;
    const uint32_t *hash;
    uint32_t gnu_nbucket, gnu_symoffset, gnu_bloom_size, gnu_shift;
    const ElfW(Addr) *gnu_bloom;            /* words of the ELF class's size */
    const uint32_t *gnu_bucket, *gnu_chain;
    const uint16_t *versym;
    /* relocation tables: REL (ARM32) and/or RELA (AArch64), the PLT's, Android's packed one, RELR */
    const ElfW(Rel) *rel; size_t nrel;
    const ElfW(Rela) *rela; size_t nrela;
    const void *jmprel; size_t jmprelsz; int jmprel_rela;
    const uint8_t *packed; size_t packedsz; int packed_rela;
    const uintptr_t *relr; size_t nrelr;
    void (*init)(void);
    void (**init_array)(int, char **, char **); size_t ninit_array;
    void (**preinit_array)(int, char **, char **); size_t npreinit_array;
    void (*fini)(void);
    void (**fini_array)(void); size_t nfini_array;
    uintptr_t exidx; int exidx_count;
    int symbolic;
    int textrel;
    int ready;               /* relocated and protected: its IFUNC resolvers can run */
    uintptr_t data_start;
    char **needed; int nneeded;
    so_module **deps; int ndeps;
    const so_shim *shims;
    int init_state, fini_done;
    unres_t *unres; int nunres, capunres;
    so_handle handle;
    /* segment protections to apply after relocation */
    struct { uintptr_t start; size_t len; int prot; } segs[16]; int nsegs;
    uintptr_t relro_start, relro_end;
};

static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static so_module *volatile g_modules;     /* append-only list */
static so_handle *g_handles;              /* host/fake handles */
static so_handle g_global = { HANDLE_MAGIC, H_GLOBAL, 1, "<global>", NULL, NULL, NULL, NULL };
static int g_trace = -1;
static char **g_libpath; static int g_nlibpath;
static int g_host_fallback = -1;

#define MAX_SHIM_TABLES 64
static const so_shim *g_tables[MAX_SHIM_TABLES];
static int g_ntables;

/* host library handles opened for DT_NEEDED of Android modules */
static void *g_host_libs[64]; static int g_nhost_libs;

/* android name -> host name mapping */
typedef struct libmap { char *android; char *host; int has_host; struct libmap *next; } libmap;
static libmap *g_libmap;
static int g_libmap_init;

/* fake lib registrations */
typedef struct fakelib { char *name; const so_shim *table; struct fakelib *next; } fakelib;
static fakelib *g_fakelibs;

static __thread char t_err[512];
static __thread int t_err_set;

static void so_lock(void) { pthread_mutex_lock(&g_lock); }
static void so_unlock(void) { pthread_mutex_unlock(&g_lock); }

static int trace(void)
{
    if (g_trace < 0) {
        const char *e = getenv("SO_TRACE");
        g_trace = e ? atoi(e) : 0;
        if (e && !*e) g_trace = 1;
    }
    return g_trace;
}

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t_err, sizeof t_err, fmt, ap);
    va_end(ap);
    t_err_set = 1;
    if (trace()) fprintf(stderr, "[so] error: %s\n", t_err);
}

const char *so_error(void) { return t_err_set ? t_err : NULL; }

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* ------------------------------------------------------------------------ */
/* shim tables: a hash index over all registered tables, first entry wins */

typedef struct shent { const char *name; void *fn; struct shent *next; } shent;
#define SHIM_BUCKETS 4096
static shent *g_shim_hash[SHIM_BUCKETS];
static int g_shim_hash_ok;

static uint32_t str_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static void shim_index_add(const so_shim *t)
{
    for (; t && t->name; t++) {
        uint32_t b = str_hash(t->name) & (SHIM_BUCKETS - 1);
        shent *e;
        for (e = g_shim_hash[b]; e; e = e->next)
            if (!strcmp(e->name, t->name)) break;
        if (e) continue;  /* earlier table wins */
        e = malloc(sizeof *e);
        e->name = t->name; e->fn = t->fn; e->next = g_shim_hash[b];
        g_shim_hash[b] = e;
    }
}

static void shim_index_build(void)
{
    if (g_shim_hash_ok) return;
    for (int i = 0; i < SHIM_BUCKETS; i++) {
        shent *e = g_shim_hash[i];
        while (e) { shent *n = e->next; free(e); e = n; }
        g_shim_hash[i] = NULL;
    }
    for (int i = 0; i < g_ntables; i++) shim_index_add(g_tables[i]);
    g_shim_hash_ok = 1;
}

void so_add_shims(const so_shim *table)
{
    if (!table) return;
    so_lock();
    for (int i = 0; i < g_ntables; i++)
        if (g_tables[i] == table) { so_unlock(); return; }
    if (g_ntables < MAX_SHIM_TABLES) {
        g_tables[g_ntables++] = table;
        if (g_shim_hash_ok) shim_index_add(table);   /* appended: lower priority */
    } else {
        fprintf(stderr, "[so] too many shim tables\n");
    }
    so_unlock();
}

static void *table_lookup(const so_shim *t, const char *name, int *found)
{
    for (; t && t->name; t++)
        if (!strcmp(t->name, name)) { *found = 1; return t->fn; }
    return NULL;
}

/* returns 1 if found (value may legitimately be NULL) */
static int shims_lookup(const char *name, void **out)
{
    so_lock();
    shim_index_build();
    uint32_t b = str_hash(name) & (SHIM_BUCKETS - 1);
    for (shent *e = g_shim_hash[b]; e; e = e->next)
        if (!strcmp(e->name, name)) { *out = e->fn; so_unlock(); return 1; }
    so_unlock();
    return 0;
}

void *so_shim_lookup(const char *name)
{
    void *p = NULL;
    shims_lookup(name, &p);
    return p;
}

/* ------------------------------------------------------------------------ */
/* library path / maps */

void so_set_library_path(const char *dirs)
{
    so_lock();
    for (int i = 0; i < g_nlibpath; i++) free(g_libpath[i]);
    free(g_libpath); g_libpath = NULL; g_nlibpath = 0;
    if (dirs) {
        char *d = strdup(dirs), *save = NULL;
        for (char *t = strtok_r(d, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
            g_libpath = realloc(g_libpath, sizeof(char *) * (g_nlibpath + 1));
            g_libpath[g_nlibpath++] = strdup(t);
        }
        free(d);
    }
    so_unlock();
}

static void libmap_set(const char *android, const char *host)
{
    libmap *m;
    for (m = g_libmap; m; m = m->next)
        if (!strcmp(m->android, android)) break;
    if (!m) {
        m = calloc(1, sizeof *m);
        m->android = strdup(android);
        m->next = g_libmap; g_libmap = m;
    }
    free(m->host);
    m->host = host ? strdup(host) : NULL;
    m->has_host = 1;
}

static void libmap_defaults(void)
{
    if (g_libmap_init) return;
    g_libmap_init = 1;
    static const char *defs[][2] = {
        { "libc.so", "libc.so.6" },
        /* glibc by its own name: only code written for glibc asks for it (Mono P/Invoke through a
         * Linux dllmap, e.g. a game helper calling malloc_trim on the process's real allocator) */
        { "libc.so.6", "libc.so.6" },
        { "libm.so", "libm.so.6" },
        { "libdl.so", "libdl.so.2" },
        { "libz.so", "libz.so.1" },
        { "libEGL.so", "libEGL.so.1" },
        { "libGLESv1_CM.so", "libGLESv1_CM.so.1" },
        { "libGLESv2.so", "libGLESv2.so.2" },
        { "libGLESv3.so", "libGLESv2.so.2" },
    };
    for (size_t i = 0; i < sizeof defs / sizeof defs[0]; i++) {
        libmap *m;
        for (m = g_libmap; m; m = m->next)
            if (!strcmp(m->android, defs[i][0])) break;
        if (!m) libmap_set(defs[i][0], defs[i][1]);
    }
    if (!g_libpath) {
        const char *e = getenv("SO_LIBRARY_PATH");
        if (e) so_set_library_path(e);
    }
}

void so_map_library(const char *android_name, const char *host)
{
    so_lock();
    libmap_set(base_name(android_name), host);
    so_unlock();
}

static libmap *libmap_find(const char *android)
{
    libmap_defaults();
    for (libmap *m = g_libmap; m; m = m->next)
        if (!strcmp(m->android, android)) return m;
    return NULL;
}

/* Android-only libraries served by shims when nobody registered a table */
static const char *const g_android_only[] = {
    "liblog.so", "libandroid.so", "libOpenSLES.so", "libjnigraphics.so", "libstdc++.so",
    "libnativewindow.so", "libsync.so", NULL
};

/* libraries whose dlopen() must fail cleanly (features we do not provide) */
static const char *const g_denied_prefix[] = {
    "libmediandk", "libvulkan", "libOpenMAXAL", "libOMX", "libomx", "libstagefright",
    "libaaudio", "libcamera2ndk", "libneuralnetworks", NULL
};

static int fakelib_registered_hint(const char *name);
static int is_denied(const char *name)
{
    if (fakelib_registered_hint(name)) return 0;
    for (int i = 0; g_denied_prefix[i]; i++)
        if (!strncmp(name, g_denied_prefix[i], strlen(g_denied_prefix[i]))) return 1;
    return 0;
}

static int is_android_only(const char *name)
{
    for (int i = 0; g_android_only[i]; i++)
        if (!strcmp(g_android_only[i], name)) return 1;
    return 0;
}

void so_register_fake_lib(const char *android_name, const so_shim *table)
{
    so_lock();
    fakelib *f = calloc(1, sizeof *f);
    f->name = strdup(base_name(android_name));
    f->table = table;
    f->next = g_fakelibs; g_fakelibs = f;
    so_unlock();
}

static fakelib *fakelib_find(const char *name)
{
    for (fakelib *f = g_fakelibs; f; f = f->next)
        if (!strcmp(f->name, name)) return f;
    return NULL;
}

static int fakelib_registered_hint(const char *name) { return fakelib_find(name) != NULL; }

static void host_lib_add(void *h)
{
    for (int i = 0; i < g_nhost_libs; i++)
        if (g_host_libs[i] == h) return;
    if (g_nhost_libs < (int)(sizeof g_host_libs / sizeof g_host_libs[0]))
        g_host_libs[g_nhost_libs++] = h;
}

static void *host_open(const char *host, int global)
{
    void *h = dlopen(host, RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
    if (!h && trace()) fprintf(stderr, "[so] host dlopen(%s) failed: %s\n", host, dlerror());
    return h;
}

/* ------------------------------------------------------------------------ */
/* symbol tables */

static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static uint32_t gnu_hash(const char *s)
{
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return h;
}

static int sym_exported(const so_module *m, uint32_t i)
{
    const ElfW(Sym) *s = &m->symtab[i];
    int bind = ELFW(ST_BIND)(s->st_info);
    if (s->st_shndx == SHN_UNDEF) return 0;
    if (bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_GNU_UNIQUE) return 0;
    if (ELFW(ST_VISIBILITY)(s->st_other) == STV_HIDDEN || ELFW(ST_VISIBILITY)(s->st_other) == STV_INTERNAL)
        return 0;
    if (m->versym && (m->versym[i] & 0x8000)) return 0;   /* hidden version */
    return 1;
}

static const ElfW(Sym) *module_lookup(const so_module *m, const char *name)
{
    if (m->gnu_bucket) {
        /* the Bloom filter's words are 32 bits in ELF32, 64 in ELF64 */
        const uint32_t bits = 8 * sizeof(ElfW(Addr));
        uint32_t h = gnu_hash(name);
        ElfW(Addr) word = m->gnu_bloom[(h / bits) % m->gnu_bloom_size];
        ElfW(Addr) mask = ((ElfW(Addr))1 << (h % bits)) | ((ElfW(Addr))1 << ((h >> m->gnu_shift) % bits));
        if ((word & mask) != mask) return NULL;
        uint32_t n = m->gnu_bucket[h % m->gnu_nbucket];
        if (n == 0) return NULL;
        for (;;) {
            uint32_t hv = m->gnu_chain[n - m->gnu_symoffset];
            if (((hv ^ h) >> 1) == 0 && !strcmp(m->strtab + m->symtab[n].st_name, name) && sym_exported(m, n))
                return &m->symtab[n];
            if (hv & 1) break;
            n++;
        }
        return NULL;
    }
    if (m->hash) {
        uint32_t nbucket = m->hash[0];
        const uint32_t *bucket = m->hash + 2, *chain = bucket + nbucket;
        for (uint32_t i = bucket[elf_hash(name) % nbucket]; i; i = chain[i])
            if (!strcmp(m->strtab + m->symtab[i].st_name, name) && sym_exported(m, i))
                return &m->symtab[i];
        return NULL;
    }
    for (uint32_t i = 1; i < m->nsyms; i++)
        if (!strcmp(m->strtab + m->symtab[i].st_name, name) && sym_exported(m, i))
            return &m->symtab[i];
    return NULL;
}

static uintptr_t call_ifunc(uintptr_t resolver);

/* an IFUNC symbol's address is what its resolver returns, once its module can run code */
static uintptr_t sym_addr(const so_module *m, const ElfW(Sym) *s)
{
    if (s->st_shndx == SHN_ABS) return s->st_value;
    uintptr_t a = m->bias + s->st_value;
    if (ELFW(ST_TYPE)(s->st_info) == STT_GNU_IFUNC && m->ready) a = call_ifunc(a);
    return a;
}

void *so_sym(so_module *m, const char *name)
{
    if (!m) return NULL;
    const ElfW(Sym) *s = module_lookup(m, name);
    return s ? (void *)sym_addr(m, s) : NULL;
}

/* ------------------------------------------------------------------------ */
/* host lookup */

static void *host_lookup(const char *name)
{
    void *p = NULL;
    for (int i = 0; i < g_nhost_libs && !p; i++) p = dlsym(g_host_libs[i], name);
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    if (p) p = so_softfp_wrap(name, p);
    return p;
}

static void *modules_lookup(const char *name, so_module *skip)
{
    for (so_module *m = g_modules; m; m = m->next) {
        if (m == skip) continue;
        const ElfW(Sym) *s = module_lookup(m, name);
        if (s) return (void *)sym_addr(m, s);
    }
    return NULL;
}

void *so_resolve(const char *name)
{
    void *p = NULL;
    if (shims_lookup(name, &p)) return p;
    if ((p = modules_lookup(name, NULL))) return p;
    return host_lookup(name);
}

/* ------------------------------------------------------------------------ */
/* unresolved stubs */

static void so_unresolved_called(const char *name)
{
    fprintf(stderr, "[so] FATAL: unresolved function '%s' called\n", name);
    fflush(stderr);
    abort();
}

/* a stub per unresolved function: so_unresolved_called(name). Sizes in 32-bit words */
#if defined(__aarch64__)
#define STUB_WORDS 8
#else
#define STUB_WORDS 4
#endif

static void *make_stub(const char *name)
{
    static uint32_t *page; static int used;
    static int rwx = 1;
    so_lock();
    if (!page || used + STUB_WORDS > (int)(PAGE_SZ / 4)) {
        page = mmap(NULL, PAGE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            rwx = 0;
            page = mmap(NULL, PAGE_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }
        used = 0;
    }
    if (!rwx) mprotect(page, PAGE_SZ, PROT_READ | PROT_WRITE);
    uint32_t *s = page + used;
    used += STUB_WORDS;
#if defined(__aarch64__)
    void (*fn)(const char *) = so_unresolved_called;
    s[0] = 0x58000080;                 /* ldr x0, #16   (-> s[4..5]) */
    s[1] = 0x580000b0;                 /* ldr x16, #20  (-> s[6..7]) */
    s[2] = 0xd61f0200;                 /* br x16 */
    s[3] = 0xd503201f;                 /* nop */
    memcpy(s + 4, &name, sizeof name);
    memcpy(s + 6, &fn, sizeof fn);
#else
    s[0] = 0xe59f0000;                 /* ldr r0, [pc, #0]  (-> s[2]) */
    s[1] = 0xe59ff000;                 /* ldr pc, [pc, #0]  (-> s[3]) */
    s[2] = (uint32_t)(uintptr_t)name;
    s[3] = (uint32_t)(uintptr_t)so_unresolved_called;
#endif
    __builtin___clear_cache((char *)s, (char *)(s + STUB_WORDS));
    if (!rwx) mprotect(page, PAGE_SZ, PROT_READ | PROT_EXEC);
    so_unlock();
    return s;
}

static void add_unres(so_module *m, const char *name, int is_func)
{
    if (m->nunres == m->capunres) {
        m->capunres = m->capunres ? m->capunres * 2 : 32;
        m->unres = realloc(m->unres, sizeof(unres_t) * m->capunres);
    }
    m->unres[m->nunres].name = name;
    m->unres[m->nunres].is_func = is_func;
    m->nunres++;
}

int so_unresolved_count(so_module *m) { return m ? m->nunres : 0; }

void so_foreach_unresolved(so_module *m, void (*cb)(const char *, int, void *), void *ud)
{
    if (!m) return;
    for (int i = 0; i < m->nunres; i++) cb(m->unres[i].name, m->unres[i].is_func, ud);
}

/* ------------------------------------------------------------------------ */
/* import resolution */

static uintptr_t resolve_import(so_module *m, uint32_t symidx)
{
    const ElfW(Sym) *s = &m->symtab[symidx];
    const char *name = m->strtab + s->st_name;
    int weak = ELFW(ST_BIND)(s->st_info) == STB_WEAK;
    int type = ELFW(ST_TYPE)(s->st_info);
    void *p = NULL;
    int found = 0;
    const char *src = NULL;

    if (m->shims) {
        p = table_lookup(m->shims, name, &found);
        if (found) { src = "module shim"; goto done; }
    }
    if (s->st_shndx != SHN_UNDEF) {                 /* defined here: bind to self */
        p = (void *)sym_addr(m, s);
        src = "self"; found = 1; goto done;
    }
    if (shims_lookup(name, &p)) { found = 1; src = "shim"; goto done; }
    if (!strcmp(name, "__data_start") || !strcmp(name, "data_start")) {
        p = (void *)m->data_start; found = 1; src = "module data"; goto done;
    }
    for (int i = 0; i < m->ndeps; i++) {
        const ElfW(Sym) *d = module_lookup(m->deps[i], name);
        if (d) { p = (void *)sym_addr(m->deps[i], d); found = 1; src = m->deps[i]->name; goto done; }
    }
    if ((p = modules_lookup(name, m))) { found = 1; src = "module"; goto done; }
    if (!weak && (p = host_lookup(name))) { found = 1; src = "host"; goto done; }

done:
    if (!found) {
        if (weak) {
            if (trace() >= 2) fprintf(stderr, "[so] %s: weak %s -> 0\n", m->name, name);
            return 0;
        }
        int is_func = type == STT_FUNC || type == STT_NOTYPE || type == STT_GNU_IFUNC;
        add_unres(m, name, is_func);
        if (is_func) p = make_stub(name);
        else p = calloc(1, s->st_size > 256 ? s->st_size : 256);
        src = "UNRESOLVED";
        if (trace()) fprintf(stderr, "[so] %s: unresolved %s %s\n", m->name, is_func ? "function" : "object", name);
        return (uintptr_t)p;
    }
    if (trace() >= 2) fprintf(stderr, "[so] %s: %s -> %p (%s)\n", m->name, name, p, src);
    return (uintptr_t)p;
}

/* One relocation from any table. REL tables (ARM32) keep the addend in the word being relocated,
 * RELA tables (AArch64) in the entry; Android's packed tables come in both flavours. */
typedef struct { uintptr_t offset, info; intptr_t addend; } reloc_t;

typedef struct {
    so_module *m;
    uintptr_t *val; uint8_t *done;                  /* resolved imports by symbol index */
    /* IRELATIVE, and references to the module's own IFUNC symbols: *where = resolver() + addend
     * once the module's code is executable */
    struct irel { uintptr_t *where, resolver, addend; } *irel;
    size_t nirel, capirel;
} relctx;

#define SELF_IFUNC 2   /* done[]: val is the resolver of an IFUNC symbol of the module itself */

static uintptr_t cached_resolve(relctx *c, uint32_t idx)
{
    if (idx >= c->m->nsyms) return 0;
    if (!c->done[idx]) {
        const ElfW(Sym) *s = &c->m->symtab[idx];
        c->val[idx] = resolve_import(c->m, idx);
        c->done[idx] = s->st_shndx != SHN_UNDEF && ELFW(ST_TYPE)(s->st_info) == STT_GNU_IFUNC &&
                       c->val[idx] == sym_addr(c->m, s) ? SELF_IFUNC : 1;
    }
    return c->val[idx];
}

static void defer_ifunc(relctx *c, uintptr_t *where, uintptr_t resolver, uintptr_t addend)
{
    if (c->nirel == c->capirel) {
        c->capirel = c->capirel ? c->capirel * 2 : 16;
        c->irel = realloc(c->irel, c->capirel * sizeof *c->irel);
    }
    c->irel[c->nirel].where = where;
    c->irel[c->nirel].resolver = resolver;
    c->irel[c->nirel].addend = addend;
    c->nirel++;
}

static int in_image(const so_module *m, const void *p)
{
    return (uintptr_t)p >= m->map_start && (uintptr_t)p + sizeof(uintptr_t) <= m->map_start + m->map_size;
}

/* bionic passes the resolver what glibc does: the hardware capabilities (AArch64: also the
 * _IFUNC_ARG_HWCAP flag and a struct with both words) */
static uintptr_t call_ifunc(uintptr_t resolver)
{
#if defined(__aarch64__)
    struct { unsigned long size, hwcap, hwcap2; } arg = { sizeof arg, getauxval(AT_HWCAP), getauxval(AT_HWCAP2) };
    return ((uintptr_t (*)(uint64_t, void *))resolver)(arg.hwcap | (1ull << 62), &arg);
#else
    return ((uintptr_t (*)(unsigned long))resolver)(getauxval(AT_HWCAP));
#endif
}

static int reloc_one(relctx *c, const reloc_t *r, int rela)
{
    so_module *m = c->m;
    uint32_t type = ELFW(R_TYPE)(r->info);
    uint32_t sym = ELFW(R_SYM)(r->info);
    uintptr_t *P = (uintptr_t *)(m->bias + r->offset);
#if defined(__aarch64__)
    if (type == R_AARCH64_NONE || type == 256 /* R_AARCH64_NONE of the first ABI release */) return 0;
#else
    if (type == R_ARM_NONE) return 0;
#endif
    if (!in_image(m, P)) {
        set_err("%s: relocation type %u outside the image (offset 0x%lx)", m->name, type, (unsigned long)r->offset);
        return -1;
    }
    uintptr_t A = rela ? (uintptr_t)r->addend : *P, S = 0, add;
    switch (type) {
#if defined(__aarch64__)
    case R_AARCH64_ABS64:
#else
    case R_ARM_ABS32:
#endif
        add = A;
        goto symbolic;
#if defined(__arm__)
    case R_ARM_REL32:
        add = A - (uintptr_t)P;
        goto symbolic;
#endif
#if defined(__aarch64__)
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
#else
    case R_ARM_GLOB_DAT:
    case R_ARM_JUMP_SLOT:
#endif
        /* the Android linker adds RELA addends here too; a REL word in place is not an addend */
        add = rela ? A : 0;
    symbolic:
        if (sym) S = cached_resolve(c, sym);
        if (sym && sym < m->nsyms && c->done[sym] == SELF_IFUNC) defer_ifunc(c, P, S, add);
        else *P = S + add;
        break;
#if defined(__aarch64__)
    case R_AARCH64_RELATIVE:
#else
    case R_ARM_RELATIVE:
#endif
        *P = m->bias + A;
        break;
#if defined(__aarch64__)
    case R_AARCH64_IRELATIVE:
#else
    case R_ARM_IRELATIVE:
#endif
        defer_ifunc(c, P, m->bias + A, 0);
        break;
#if defined(__aarch64__)
    case R_AARCH64_COPY:
#else
    case R_ARM_COPY:
#endif
        set_err("%s: COPY relocation in a shared library", m->name);
        return -1;
#if defined(__aarch64__)
    case R_AARCH64_TLS_DTPMOD:
    case R_AARCH64_TLS_DTPREL:
    case R_AARCH64_TLS_TPREL:
    case R_AARCH64_TLSDESC:
#else
    case R_ARM_TLS_DTPMOD32:
    case R_ARM_TLS_DTPOFF32:
    case R_ARM_TLS_TPOFF32:
#endif
        set_err("%s: TLS relocation %u not supported", m->name, type);
        return -1;
    default:
        set_err("%s: unsupported relocation type %u at 0x%lx", m->name, type, (unsigned long)r->offset);
        return -1;
    }
    return 0;
}

static int reloc_table(relctx *c, const void *table, size_t size, int rela)
{
    if (rela) {
        const ElfW(Rela) *r = table;
        for (size_t i = 0; i < size / sizeof *r; i++) {
            reloc_t x = { r[i].r_offset, r[i].r_info, r[i].r_addend };
            if (reloc_one(c, &x, 1) < 0) return -1;
        }
    } else {
        const ElfW(Rel) *r = table;
        for (size_t i = 0; i < size / sizeof *r; i++) {
            reloc_t x = { r[i].r_offset, r[i].r_info, 0 };
            if (reloc_one(c, &x, 0) < 0) return -1;
        }
    }
    return 0;
}

/* signed LEB128, as bionic's sleb128_decoder reads it: values wrap at the word size */
typedef struct { const uint8_t *p, *end; int bad; } sleb128;

static uintptr_t sleb128_next(sleb128 *d)
{
    uintptr_t v = 0;
    unsigned shift = 0;
    uint8_t byte;
    do {
        if (d->p >= d->end) { d->bad = 1; return 0; }
        byte = *d->p++;
        if (shift < 8 * sizeof v) v |= (uintptr_t)(byte & 127) << shift;
        shift += 7;
    } while (byte & 128);
    if (shift < 8 * sizeof v && (byte & 64)) v |= ~(uintptr_t)0 << shift;
    return v;
}

/* Android's packed relocations (DT_ANDROID_REL / DT_ANDROID_RELA): "APS2", the count and the
 * first offset, then groups of relocations sharing an offset step, r_info and/or addend; every
 * field is a delta from the previous relocation's */
static int reloc_packed(relctx *c)
{
    so_module *m = c->m;
    if (m->packedsz < 4 || memcmp(m->packed, "APS2", 4)) {
        set_err("%s: packed relocations without the APS2 signature", m->name);
        return -1;
    }
    sleb128 d = { m->packed + 4, m->packed + m->packedsz, 0 };
    size_t n = sleb128_next(&d);
    reloc_t r = { sleb128_next(&d), 0, 0 };
    for (size_t i = 0; i < n && !d.bad;) {
        size_t size = sleb128_next(&d), flags = sleb128_next(&d);
        uintptr_t step = 0;
        if (flags & APS2_GROUPED_BY_OFFSET_DELTA) step = sleb128_next(&d);
        if (flags & APS2_GROUPED_BY_INFO) r.info = sleb128_next(&d);
        size_t addends = flags & (APS2_GROUP_HAS_ADDEND | APS2_GROUPED_BY_ADDEND);
        if ((flags & APS2_GROUP_HAS_ADDEND) && !m->packed_rela) {
            set_err("%s: addends in a packed REL table", m->name);
            return -1;
        }
        if (addends == (APS2_GROUP_HAS_ADDEND | APS2_GROUPED_BY_ADDEND)) r.addend += sleb128_next(&d);
        else if (addends != APS2_GROUP_HAS_ADDEND) r.addend = 0;
        if (d.bad || size > n - i) break;
        for (size_t k = 0; k < size; k++) {
            r.offset += (flags & APS2_GROUPED_BY_OFFSET_DELTA) ? step : sleb128_next(&d);
            if (!(flags & APS2_GROUPED_BY_INFO)) r.info = sleb128_next(&d);
            if (addends == APS2_GROUP_HAS_ADDEND) r.addend += sleb128_next(&d);
            if (d.bad) break;
            if (reloc_one(c, &r, m->packed_rela) < 0) return -1;
        }
        if (d.bad) break;
        i += size;
        if (i == n) return 0;
    }
    if (n == 0 && !d.bad) return 0;
    set_err("%s: packed relocations end early or are corrupt", m->name);
    return -1;
}

/* DT_RELR: relative relocations as addresses (even entries) and bitmaps of the following words
 * (odd entries, one bit per word, 31 or 63 of them) */
static int apply_relr(so_module *m)
{
    uintptr_t *where = NULL;
    for (size_t i = 0; i < m->nrelr; i++) {
        uintptr_t e = m->relr[i];
        if (!(e & 1)) {
            where = (uintptr_t *)(m->bias + e);
            if (!in_image(m, where)) goto bad;
            *where++ += m->bias;
        } else {
            if (!where) goto bad;
            for (uintptr_t *p = where; (e >>= 1) != 0; p++)
                if (e & 1) {
                    if (!in_image(m, p)) goto bad;
                    *p += m->bias;
                }
            where += 8 * sizeof(uintptr_t) - 1;
        }
    }
    return 0;
bad:
    set_err("%s: RELR relocation outside the image", m->name);
    return -1;
}

/* in the Android linker's order: packed, RELR, REL/RELA, PLT; IFUNC ones are queued */
static int relocate(relctx *c)
{
    so_module *m = c->m;
    if (m->packed && reloc_packed(c) < 0) return -1;
    if (m->relr && apply_relr(m) < 0) return -1;
    if (m->rel && reloc_table(c, m->rel, m->nrel * sizeof *m->rel, 0) < 0) return -1;
    if (m->rela && reloc_table(c, m->rela, m->nrela * sizeof *m->rela, 1) < 0) return -1;
    if (m->jmprel && reloc_table(c, m->jmprel, m->jmprelsz, m->jmprel_rela) < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* bionic's thread slots (AArch64): see so_loader.h */

#if defined(__aarch64__)
#define BIONIC_TLS_FIRST 2          /* slots 0 and 1 are glibc's TCB (dtv, unused word) */
#define BIONIC_TLS_GUARD 5          /* TLS_SLOT_STACK_GUARD, tpidr_el0 + 0x28 */
#define BIONIC_TLS_LAST 7           /* TLS_SLOT_ART_THREAD_SELF: the last of bionic's fixed slots */
/* The stack protector's canary. Each frame compares the canary it stored on entry with the slot
 * when it returns, so the value only has to stay the same for the life of a thread; a fixed one
 * spares rewriting the executable's TLS image (read only after relocation). The zero low byte
 * stops string overruns from copying it out or over it, as with bionic's and glibc's random ones. */
#define BIONIC_TLS_GUARD_VALUE 0x5ca1ab1e0d15ea00ul

/* The only initialized thread-local data in the executable (everything else is zero, in .tbss,
 * which the linker places after .tdata), so this is the start of the executable's static TLS
 * block, which glibc puts right after its 16-byte TCB in every thread: slots 2-7 are ours, and
 * each new thread gets the canary from the block's initialization image. */
static __attribute__((aligned(16), used)) __thread uintptr_t
    bionic_tls[BIONIC_TLS_LAST - BIONIC_TLS_FIRST + 1] = { [BIONIC_TLS_GUARD - BIONIC_TLS_FIRST] = BIONIC_TLS_GUARD_VALUE };

static int bionic_tls_check(void)
{
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if ((uintptr_t)&bionic_tls[0] == tp + 8 * BIONIC_TLS_FIRST &&
        bionic_tls[BIONIC_TLS_GUARD - BIONIC_TLS_FIRST] == BIONIC_TLS_GUARD_VALUE)
        return 0;
    set_err("bionic's thread slots are at %p, Android code expects them at tpidr_el0 + 16 = %p: link "
            "libsoloader.a into the executable and give no other thread-local variable of the executable "
            "an initial value or an alignment above 16 (so_loader.h)", (void *)&bionic_tls[0], (void *)(tp + 16));
    return -1;
}
#endif

/* ------------------------------------------------------------------------ */
/* loading */

static int is_android_elf_file(const char *path)
{
    /* heuristic: an ELF shared object of our architecture whose DT_NEEDED include "libc.so" (bionic) */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ElfW(Ehdr) eh;
    int ok = 0;
    if (pread(fd, &eh, sizeof eh, 0) == sizeof eh && !memcmp(eh.e_ident, ELFMAG, SELFMAG) &&
        eh.e_ident[EI_CLASS] == SO_ELFCLASS && eh.e_machine == SO_MACHINE && eh.e_type == ET_DYN) {
        ElfW(Phdr) ph[32];
        int n = eh.e_phnum < 32 ? eh.e_phnum : 32;
        if (pread(fd, ph, n * sizeof(ElfW(Phdr)), eh.e_phoff) == (ssize_t)(n * sizeof(ElfW(Phdr)))) {
            for (int i = 0; i < n; i++) {
                if (ph[i].p_type == PT_INTERP) {
                    char buf[32] = {0};
                    if (pread(fd, buf, sizeof buf - 1, ph[i].p_offset) > 0 && strstr(buf, "/system/bin/linker")) ok = 1;
                }
            }
            if (!ok) {
                /* look for DT_NEEDED libc.so */
                for (int i = 0; i < n && !ok; i++) {
                    if (ph[i].p_type != PT_DYNAMIC) continue;
                    size_t sz = ph[i].p_filesz;
                    ElfW(Dyn) *d = malloc(sz);
                    if (pread(fd, d, sz, ph[i].p_offset) == (ssize_t)sz) {
                        uintptr_t strtab_vaddr = 0;
                        for (size_t k = 0; k < sz / sizeof *d && d[k].d_tag != DT_NULL; k++)
                            if (d[k].d_tag == DT_STRTAB) strtab_vaddr = d[k].d_un.d_val;
                        /* translate vaddr -> offset via PT_LOAD */
                        uintptr_t stroff = 0;
                        for (int j = 0; j < n; j++)
                            if (ph[j].p_type == PT_LOAD && strtab_vaddr >= ph[j].p_vaddr &&
                                strtab_vaddr < ph[j].p_vaddr + ph[j].p_filesz)
                                stroff = strtab_vaddr - ph[j].p_vaddr + ph[j].p_offset;
                        for (size_t k = 0; stroff && k < sz / sizeof *d && d[k].d_tag != DT_NULL; k++) {
                            if (d[k].d_tag != DT_NEEDED) continue;
                            char nb[16] = {0};
                            if (pread(fd, nb, sizeof nb - 1, stroff + d[k].d_un.d_val) > 0 && !strcmp(nb, "libc.so")) ok = 1;
                        }
                    }
                    free(d);
                }
            }
        }
    }
    close(fd);
    return ok;
}

static so_module *find_loaded(const char *name)
{
    const char *b = base_name(name);
    for (so_module *m = g_modules; m; m = m->next) {
        if (!strcmp(m->path, name)) return m;
        if (!strcmp(m->name, b)) return m;
        if (m->soname && !strcmp(m->soname, b)) return m;
    }
    return NULL;
}

so_module *so_find(const char *name)
{
    so_lock();
    so_module *m = find_loaded(name);
    so_unlock();
    return m;
}

const char *so_name(so_module *m) { return m ? (m->soname ? m->soname : m->name) : NULL; }
uintptr_t so_base(so_module *m) { return m ? m->bias : 0; }
size_t so_size(so_module *m) { return m ? m->map_size : 0; }

/* search the Android library path for name; returns malloc'd path or NULL */
static char *search_android_lib(const char *name)
{
    char buf[1024];
    const char *b = base_name(name);
    if (strchr(name, '/') && access(name, R_OK) == 0) return strdup(name);
    for (int i = 0; i < g_nlibpath; i++) {
        snprintf(buf, sizeof buf, "%s/%s", g_libpath[i], b);
        if (access(buf, R_OK) == 0) return strdup(buf);
    }
    for (so_module *m = g_modules; m; m = m->next) {
        const char *s = strrchr(m->path, '/');
        if (!s) continue;
        snprintf(buf, sizeof buf, "%.*s/%s", (int)(s - m->path), m->path, b);
        if (access(buf, R_OK) == 0) return strdup(buf);
    }
    return NULL;
}

static int map_segments(so_module *m, int fd, const ElfW(Ehdr) *eh, const ElfW(Phdr) *ph)
{
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (lo == UINTPTR_MAX) { set_err("%s: no PT_LOAD", m->path); return -1; }
    lo = PAGE_DOWN(lo); hi = PAGE_UP(hi);
    void *res = mmap(NULL, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) { set_err("%s: reserve %zu bytes: %s", m->path, (size_t)(hi - lo), strerror(errno)); return -1; }
    m->map_start = (uintptr_t)res;
    m->map_size = hi - lo;
    m->bias = (uintptr_t)res - lo;

    for (int i = 0; i < eh->e_phnum; i++) {
        const ElfW(Phdr) *p = &ph[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
        uintptr_t seg = m->bias + p->p_vaddr;
        uintptr_t seg_page = PAGE_DOWN(seg);
        uintptr_t file_end = seg + p->p_filesz;
        uintptr_t file_page_end = PAGE_UP(file_end);
        uintptr_t seg_end = PAGE_UP(seg + p->p_memsz);
        int prot = (p->p_flags & PF_R ? PROT_READ : 0) | (p->p_flags & PF_W ? PROT_WRITE : 0) |
                   (p->p_flags & PF_X ? PROT_EXEC : 0);
        if (p->p_filesz) {
            void *r = MAP_FAILED;
            if ((p->p_offset & (PAGE_SZ - 1)) == (p->p_vaddr & (PAGE_SZ - 1)))
                r = mmap((void *)seg_page, file_page_end - seg_page, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_FIXED, fd, PAGE_DOWN(p->p_offset));
            if (r == MAP_FAILED) {
                /* fallback: anonymous memory + read */
                r = mmap((void *)seg_page, file_page_end - seg_page, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
                if (r == MAP_FAILED) { set_err("%s: map segment: %s", m->path, strerror(errno)); return -1; }
                size_t done = 0;
                while (done < p->p_filesz) {
                    ssize_t k = pread(fd, (char *)seg + done, p->p_filesz - done, p->p_offset + done);
                    if (k <= 0) { set_err("%s: read segment: %s", m->path, strerror(errno)); return -1; }
                    done += k;
                }
            }
        }
        if (p->p_memsz > p->p_filesz) {
            if (file_end & (PAGE_SZ - 1)) memset((void *)file_end, 0, file_page_end - file_end);
            if (seg_end > file_page_end) {
                void *r = mmap((void *)file_page_end, seg_end - file_page_end, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
                if (r == MAP_FAILED) { set_err("%s: map bss: %s", m->path, strerror(errno)); return -1; }
            }
        } else if (!p->p_filesz) {
            /* nothing */
        }
        if (m->nsegs < 16) {
            m->segs[m->nsegs].start = seg_page;
            m->segs[m->nsegs].len = seg_end - seg_page;
            m->segs[m->nsegs].prot = prot;
            m->nsegs++;
        }
        if ((p->p_flags & PF_W) && !m->data_start) m->data_start = seg;
    }
    return 0;
}

static int parse_dynamic(so_module *m)
{
    uintptr_t b = m->bias;
    size_t relsz = 0, relasz = 0, init_arraysz = 0, fini_arraysz = 0, preinit_arraysz = 0, relrsz = 0;
    intptr_t pltrel = -1;
    for (ElfW(Dyn) *d = m->dyn; d->d_tag != DT_NULL; d++) {
        uintptr_t v = d->d_un.d_val;
        switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const ElfW(Sym) *)(b + v); break;
        case DT_STRTAB: m->strtab = (const char *)(b + v); break;
        case DT_HASH: m->hash = (const uint32_t *)(b + v); break;
        case DT_GNU_HASH: {
            const uint32_t *g = (const uint32_t *)(b + v);
            m->gnu_nbucket = g[0]; m->gnu_symoffset = g[1]; m->gnu_bloom_size = g[2]; m->gnu_shift = g[3];
            m->gnu_bloom = (const ElfW(Addr) *)(g + 4);
            m->gnu_bucket = (const uint32_t *)(m->gnu_bloom + m->gnu_bloom_size);
            m->gnu_chain = m->gnu_bucket + m->gnu_nbucket;
            break;
        }
        case DT_VERSYM: m->versym = (const uint16_t *)(b + v); break;
        case DT_REL: m->rel = (const ElfW(Rel) *)(b + v); break;
        case DT_RELSZ: relsz = v; break;
        case DT_RELA: m->rela = (const ElfW(Rela) *)(b + v); break;
        case DT_RELASZ: relasz = v; break;
        case DT_JMPREL: m->jmprel = (const void *)(b + v); break;
        case DT_PLTRELSZ: m->jmprelsz = v; break;
        case DT_PLTREL: pltrel = v; break;
        case DT_ANDROID_REL: case DT_ANDROID_RELA:
            m->packed = (const uint8_t *)(b + v);
            m->packed_rela = d->d_tag == DT_ANDROID_RELA;
            break;
        case DT_ANDROID_RELSZ: case DT_ANDROID_RELASZ: m->packedsz = v; break;
        case DT_RELR: case DT_ANDROID_RELR: m->relr = (const uintptr_t *)(b + v); break;
        case DT_RELRSZ: case DT_ANDROID_RELRSZ: relrsz = v; break;
        case DT_RELRENT: case DT_ANDROID_RELRENT:
            if (v != sizeof(uintptr_t)) { set_err("%s: RELR entries of %lu bytes", m->name, (unsigned long)v); return -1; }
            break;
        case DT_INIT: m->init = (void (*)(void))(b + v); break;
        case DT_FINI: m->fini = (void (*)(void))(b + v); break;
        case DT_INIT_ARRAY: m->init_array = (void *)(b + v); break;
        case DT_INIT_ARRAYSZ: init_arraysz = v; break;
        case DT_FINI_ARRAY: m->fini_array = (void *)(b + v); break;
        case DT_FINI_ARRAYSZ: fini_arraysz = v; break;
        case DT_PREINIT_ARRAY: m->preinit_array = (void *)(b + v); break;
        case DT_PREINIT_ARRAYSZ: preinit_arraysz = v; break;
        case DT_SYMBOLIC: m->symbolic = 1; break;
        case DT_TEXTREL: m->textrel = 1; break;
        case DT_FLAGS:
            if (v & DF_SYMBOLIC) m->symbolic = 1;
            if (v & DF_TEXTREL) m->textrel = 1;
            break;
        default: break;
        }
    }
    if (!m->symtab || !m->strtab) { set_err("%s: no dynamic symbol table", m->name); return -1; }
    /* the PLT's table: DT_PLTREL says which kind; without it, the architecture's own */
#if defined(__aarch64__)
    if (pltrel < 0) pltrel = DT_RELA;
#else
    if (pltrel < 0) pltrel = DT_REL;
#endif
    if (m->jmprel && pltrel != DT_REL && pltrel != DT_RELA) {
        set_err("%s: DT_PLTREL is %ld", m->name, (long)pltrel);
        return -1;
    }
    m->jmprel_rela = pltrel == DT_RELA;
    m->nrel = relsz / sizeof(ElfW(Rel));
    m->nrela = relasz / sizeof(ElfW(Rela));
    m->nrelr = relrsz / sizeof(uintptr_t);
    m->ninit_array = init_arraysz / sizeof(void *);
    m->nfini_array = fini_arraysz / sizeof(void *);
    m->npreinit_array = preinit_arraysz / sizeof(void *);
    if (m->hash) m->nsyms = m->hash[1];
    else if (m->gnu_bucket) {
        uint32_t maxi = 0;
        for (uint32_t i = 0; i < m->gnu_nbucket; i++)
            if (m->gnu_bucket[i] > maxi) maxi = m->gnu_bucket[i];
        if (maxi >= m->gnu_symoffset) {
            while (!(m->gnu_chain[maxi - m->gnu_symoffset] & 1)) maxi++;
        }
        m->nsyms = maxi + 1;
    }
    /* soname and needed */
    for (ElfW(Dyn) *d = m->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SONAME) m->soname = m->strtab + d->d_un.d_val;
        if (d->d_tag == DT_NEEDED) {
            m->needed = realloc(m->needed, sizeof(char *) * (m->nneeded + 1));
            m->needed[m->nneeded++] = (char *)(m->strtab + d->d_un.d_val);
        }
    }
    return 0;
}

static so_module *load_locked(const char *path, const so_shim *shims, int depth);

static int load_needed(so_module *m, int depth)
{
    for (int i = 0; i < m->nneeded; i++) {
        const char *n = m->needed[i];
        const char *b = base_name(n);
        so_module *d = find_loaded(n);
        if (!d) {
            libmap *lm = libmap_find(b);
            if (lm) {
                if (lm->host) {
                    void *h = host_open(lm->host, 1);
                    if (h) host_lib_add(h);
                    else if (trace() || !strstr(b, "GL") ) {
                        static char warned[16][32]; static int nw;
                        int seen = 0;
                        for (int k = 0; k < nw; k++) if (!strcmp(warned[k], b)) seen = 1;
                        if (!seen) {
                            fprintf(stderr, "[so] warning: host library %s (for %s) not available\n", lm->host, b);
                            if (nw < 16) snprintf(warned[nw++], 32, "%s", b);
                        }
                    }
                }
                continue;
            }
            if (fakelib_find(b) || is_android_only(b)) continue;
            char *p = search_android_lib(n);
            if (!p) {
                if (trace()) fprintf(stderr, "[so] %s: DT_NEEDED %s not found, treating as system library\n", m->name, n);
                continue;
            }
            if (depth > 16) { free(p); set_err("%s: DT_NEEDED nesting too deep", m->name); return -1; }
            d = load_locked(p, NULL, depth + 1);
            free(p);
            if (!d) return -1;
        }
        m->deps = realloc(m->deps, sizeof(so_module *) * (m->ndeps + 1));
        m->deps[m->ndeps++] = d;
    }
    return 0;
}

static so_module *load_locked(const char *path, const so_shim *shims, int depth)
{
    so_module *m = find_loaded(path);
    if (m) return m;
    libmap_defaults();

#if defined(__aarch64__)
    if (bionic_tls_check() < 0) return NULL;
#endif
    /* segments are mapped and protected in 4 KiB pages: a kernel with larger ones (some arm64
     * kernels use 16 or 64 KiB) cannot place them */
    if (sysconf(_SC_PAGESIZE) != (long)PAGE_SZ) {
        set_err("%s: the kernel's page size is %ld bytes, the loader handles %u only", path, sysconf(_SC_PAGESIZE), PAGE_SZ);
        return NULL;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { set_err("%s: %s", path, strerror(errno)); return NULL; }
    ElfW(Ehdr) eh;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG)) {
        set_err("%s: not an ELF file", path); close(fd); return NULL;
    }
    if (eh.e_ident[EI_CLASS] != SO_ELFCLASS || eh.e_machine != SO_MACHINE || eh.e_type != ET_DYN) {
        set_err("%s: not an %s shared object (ELF class %d, machine %d, type %d)", path, SO_ARCH,
                eh.e_ident[EI_CLASS], eh.e_ident[EI_CLASS] == SO_ELFCLASS ? eh.e_machine : -1,
                eh.e_ident[EI_CLASS] == SO_ELFCLASS ? eh.e_type : -1);
        close(fd); return NULL;
    }
    ElfW(Phdr) *ph = malloc(eh.e_phnum * sizeof *ph);
    if (pread(fd, ph, eh.e_phnum * sizeof *ph, eh.e_phoff) != (ssize_t)(eh.e_phnum * sizeof *ph)) {
        set_err("%s: cannot read program headers", path); free(ph); close(fd); return NULL;
    }

    m = calloc(1, sizeof *m);
    m->path = strdup(path);
    m->name = base_name(m->path);
    m->shims = shims;
    m->handle.magic = HANDLE_MAGIC;
    m->handle.kind = H_MODULE;
    m->handle.mod = m;
    m->handle.refs = 1;

    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_TLS) {
            set_err("%s: PT_TLS (ELF TLS) is not supported", path);
            goto fail;
        }
    }
    if (map_segments(m, fd, &eh, ph) < 0) goto fail;

    for (int i = 0; i < eh.e_phnum; i++) {
        switch (ph[i].p_type) {
        case PT_DYNAMIC: m->dyn = (ElfW(Dyn) *)(m->bias + ph[i].p_vaddr); break;
#if defined(__arm__)
        case PT_ARM_EXIDX:
            m->exidx = m->bias + ph[i].p_vaddr;
            m->exidx_count = ph[i].p_memsz / 8;
            break;
#endif
        case PT_GNU_RELRO:
            m->relro_start = PAGE_DOWN(m->bias + ph[i].p_vaddr);
            m->relro_end = PAGE_DOWN(m->bias + ph[i].p_vaddr + ph[i].p_memsz);
            break;
        case PT_PHDR:
            m->phdr = (const ElfW(Phdr) *)(m->bias + ph[i].p_vaddr);
            break;
        }
    }
    if (!m->phdr) {
        for (int i = 0; i < eh.e_phnum; i++)
            if (ph[i].p_type == PT_LOAD && eh.e_phoff >= ph[i].p_offset &&
                eh.e_phoff + eh.e_phnum * sizeof *ph <= ph[i].p_offset + ph[i].p_filesz)
                m->phdr = (const ElfW(Phdr) *)(m->bias + ph[i].p_vaddr + eh.e_phoff - ph[i].p_offset);
    }
    if (!m->phdr) { m->phdr = ph; ph = NULL; }   /* keep our copy */
    m->phnum = eh.e_phnum;
    if (!m->dyn) { set_err("%s: no PT_DYNAMIC", path); goto fail; }
    if (parse_dynamic(m) < 0) goto fail;
    if (trace())
        fprintf(stderr, "[so] loading %s at %p (size 0x%zx), %zu+%zu relocations%s%s\n", m->path,
                (void *)m->bias, m->map_size, m->nrel + m->nrela,
                m->jmprelsz / (m->jmprel_rela ? sizeof(ElfW(Rela)) : sizeof(ElfW(Rel))),
                m->packed ? " + packed" : "", m->relr ? " + RELR" : "");

    if (load_needed(m, depth) < 0) goto fail;

    /* relocate */
    relctx rc;
    memset(&rc, 0, sizeof rc);
    rc.m = m;
    rc.val = calloc(m->nsyms ? m->nsyms : 1, sizeof(uintptr_t));
    rc.done = calloc(m->nsyms ? m->nsyms : 1, 1);
    int r = relocate(&rc);
    free(rc.val); free(rc.done);
    if (r < 0) { free(rc.irel); goto fail; }
    /* final protections; then the IRELATIVE resolvers, which are code of the module */
    for (int i = 0; i < m->nsegs; i++)
        mprotect((void *)m->segs[i].start, m->segs[i].len, m->segs[i].prot);
    for (size_t i = 0; i < rc.nirel; i++) *rc.irel[i].where = call_ifunc(rc.irel[i].resolver) + rc.irel[i].addend;
    free(rc.irel);
    m->ready = 1;
    if (m->relro_end > m->relro_start && !getenv("SO_NO_RELRO"))
        mprotect((void *)m->relro_start, m->relro_end - m->relro_start, PROT_READ);
    /* the executable segments only: AArch64's cache maintenance faults on the PROT_NONE gaps */
    for (int i = 0; i < m->nsegs; i++)
        if (m->segs[i].prot & PROT_EXEC)
            __builtin___clear_cache((char *)m->segs[i].start, (char *)(m->segs[i].start + m->segs[i].len));

    close(fd);
    free(ph);
    /* append to module list */
    m->next = NULL;
    if (!g_modules) g_modules = m;
    else { so_module *t = g_modules; while (t->next) t = t->next; t->next = m; }
    if (m->nunres && !trace())
        fprintf(stderr, "[so] %s: %d unresolved imports (SO_TRACE=1 lists them)\n", m->name, m->nunres);
    else if (trace())
        fprintf(stderr, "[so] loaded %s: %d unresolved\n", m->name, m->nunres);
    return m;

fail:
    close(fd);
    free(ph);
    if (m->map_start) munmap((void *)m->map_start, m->map_size);
    free(m->needed); free(m->deps); free(m->unres);
    free(m->path); free(m);
    return NULL;
}

so_module *so_load(const char *path, const so_shim *module_shims)
{
    so_lock();
    t_err_set = 0;
    so_module *m = load_locked(path, module_shims, 0);
    so_unlock();
    return m;
}

void so_run_init(so_module *m)
{
    if (!m || m->init_state) return;
    m->init_state = 1;
    for (int i = 0; i < m->ndeps; i++) so_run_init(m->deps[i]);
    extern char **environ;
    if (trace()) fprintf(stderr, "[so] init %s: DT_INIT %p, %zu init_array entries\n", m->name,
                         (void *)m->init, m->ninit_array);
    for (size_t i = 0; i < m->npreinit_array; i++) {
        uintptr_t f = (uintptr_t)m->preinit_array[i];
        if (f && f != (uintptr_t)-1) m->preinit_array[i](0, NULL, environ);
    }
    if (m->init) m->init();
    for (size_t i = 0; i < m->ninit_array; i++) {
        uintptr_t f = (uintptr_t)m->init_array[i];
        if (trace() >= 3) fprintf(stderr, "[so]   init_array[%zu] = %p (+0x%x)\n", i, (void *)f, (unsigned)(f - m->bias));
        if (f && f != (uintptr_t)-1) m->init_array[i](0, NULL, environ);
    }
    m->init_state = 2;
}

void so_run_fini(so_module *m)
{
    if (!m || m->fini_done || m->init_state != 2) return;
    m->fini_done = 1;
    for (size_t i = m->nfini_array; i-- > 0;) {
        uintptr_t f = (uintptr_t)m->fini_array[i];
        if (f && f != (uintptr_t)-1) m->fini_array[i]();
    }
    if (m->fini) m->fini();
}

/* ------------------------------------------------------------------------ */
/* EHABI (ARM32 only): exidx lookup. AArch64 unwinds with .eh_frame, found through
 * dl_iterate_phdr (so_dl_iterate_phdr below) */

#if defined(__arm__)
struct exidx_q { uintptr_t pc; uintptr_t exidx; int count; };

static int exidx_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    struct exidx_q *q = data;
    int match = 0;
    uintptr_t ex = 0; int cnt = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD) {
            uintptr_t s = info->dlpi_addr + p->p_vaddr;
            if (q->pc >= s && q->pc < s + p->p_memsz) match = 1;
        } else if (p->p_type == PT_ARM_EXIDX) {
            ex = info->dlpi_addr + p->p_vaddr;
            cnt = p->p_memsz / 8;
        }
    }
    if (match) { q->exidx = ex; q->count = cnt; return 1; }
    return 0;
}

uintptr_t so_find_exidx(uintptr_t pc, int *pcount)
{
    for (so_module *m = g_modules; m; m = m->next) {
        if (pc >= m->map_start && pc < m->map_start + m->map_size) {
            *pcount = m->exidx_count;
            return m->exidx;
        }
    }
    struct exidx_q q = { pc, 0, 0 };
    if (dl_iterate_phdr(exidx_cb, &q)) {
        *pcount = q.count;
        return q.exidx;
    }
    *pcount = 0;
    return 0;
}

/* interposes glibc's version for the host libgcc_s unwinder (export it!) */
__attribute__((visibility("default"))) uintptr_t __gnu_Unwind_Find_exidx(uintptr_t pc, int *pcount)
{
    return so_find_exidx(pc, pcount);
}
#else
uintptr_t so_find_exidx(uintptr_t pc, int *pcount)
{
    (void)pc;
    *pcount = 0;
    return 0;
}
#endif

/* ------------------------------------------------------------------------ */
/* dl* shims for Android code */

static so_handle *new_handle(int kind, const char *name)
{
    so_handle *h = calloc(1, sizeof *h);
    h->magic = HANDLE_MAGIC;
    h->kind = kind;
    h->refs = 1;
    h->name = strdup(name);
    h->next = g_handles; g_handles = h;
    return h;
}

static so_handle *find_handle(int kind, const char *name)
{
    for (so_handle *h = g_handles; h; h = h->next)
        if (h->kind == kind && !strcmp(h->name, name)) return h;
    return NULL;
}

void *so_dlopen(const char *filename, int flags)
{
    if (!filename) return &g_global;
    so_lock();
    libmap_defaults();
    const char *b = base_name(filename);
    so_handle *h = NULL;
    so_module *m = find_loaded(filename);
    if (m) { m->handle.refs++; h = &m->handle; goto out; }

    if (is_denied(b)) { set_err("dlopen(%s): library not available", filename); goto out; }
    fakelib *f = fakelib_find(b);
    if (f || is_android_only(b)) {
        h = find_handle(H_FAKE, b);
        if (!h) { h = new_handle(H_FAKE, b); h->table = f ? f->table : NULL; }
        else h->refs++;
        goto out;
    }
    libmap *lm = libmap_find(b);
    if (lm) {
        if (!lm->host) { set_err("dlopen(%s): library not available", filename); goto out; }
        h = find_handle(H_HOST, b);
        if (h) { h->refs++; goto out; }
        void *hh = host_open(lm->host, (flags & B_RTLD_GLOBAL) != 0);
        if (!hh) { set_err("dlopen(%s): host %s: %s", filename, lm->host, dlerror()); goto out; }
        h = new_handle(H_HOST, b);
        h->host = hh;
        goto out;
    }
    if (flags & B_RTLD_NOLOAD) { set_err("dlopen(%s): not loaded", filename); goto out; }
    char *p = search_android_lib(filename);
    if (p && is_android_elf_file(p)) {
        m = load_locked(p, NULL, 0);
        free(p);
        if (!m) goto out;
        h = &m->handle;
        so_unlock();
        so_run_init(m);
        if (trace()) fprintf(stderr, "[so] dlopen(%s) -> module %s\n", filename, m->name);
        return h;
    }
    free(p);
    if (g_host_fallback < 0) g_host_fallback = getenv("SO_DLOPEN_HOST_FALLBACK") != NULL;
    if (g_host_fallback) {
        void *hh = host_open(filename, (flags & B_RTLD_GLOBAL) != 0);
        if (hh) { h = new_handle(H_HOST, b); h->host = hh; goto out; }
    }
    set_err("dlopen(%s): library not found", filename);
out:
    if (trace()) fprintf(stderr, "[so] dlopen(%s, 0x%x) -> %p%s\n", filename, flags, (void *)h, h ? "" : " (fail)");
    so_unlock();
    return h;
}

static void *module_and_deps_lookup(so_module *m, const char *name, int depth)
{
    const ElfW(Sym) *s = module_lookup(m, name);
    if (s) return (void *)sym_addr(m, s);
    if (depth > 8) return NULL;
    for (int i = 0; i < m->ndeps; i++) {
        void *p = module_and_deps_lookup(m->deps[i], name, depth + 1);
        if (p) return p;
    }
    return NULL;
}

static void *(*g_dlsym_hook)(const char *symbol, void *found);

void *so_dlsym(void *handle, const char *symbol)
{
    void *p = NULL;
    so_handle *h = handle;
    if (!symbol) { set_err("dlsym: NULL symbol"); return NULL; }
    if (!h || handle == B_RTLD_DEFAULT || handle == B_RTLD_NEXT || h == &g_global) {
        p = so_resolve(symbol);
    } else if (h->magic != HANDLE_MAGIC) {
        set_err("dlsym(%p, %s): invalid handle", handle, symbol);
        return NULL;
    } else if (h->kind == H_MODULE) {
        p = module_and_deps_lookup(h->mod, symbol, 0);
    } else if (h->kind == H_HOST) {
        if (!shims_lookup(symbol, &p) || !p) {
            p = dlsym(h->host, symbol);
            if (p) p = so_softfp_wrap(symbol, p);
        }
    } else if (h->kind == H_FAKE) {
        int found = 0;
        if (h->table) p = table_lookup(h->table, symbol, &found);
        if (!found) shims_lookup(symbol, &p);
    }
    if (p && g_dlsym_hook) p = g_dlsym_hook(symbol, p);
    if (trace() >= 2) fprintf(stderr, "[so] dlsym(%s, %s) -> %p\n", !PSEUDO_HANDLE(h) && h != &g_global ? h->name : "default", symbol, p);
    if (!p) set_err("dlsym: symbol %s not found", symbol);
    return p;
}

void so_set_dlsym_hook(void *(*hook)(const char *symbol, void *found)) { g_dlsym_hook = hook; }

int so_dlclose(void *handle)
{
    so_handle *h = handle;
    if (!PSEUDO_HANDLE(h) && h->magic == HANDLE_MAGIC && h->refs > 0) h->refs--;
    return 0;   /* never unload */
}

char *so_dlerror(void)
{
    if (!t_err_set) return NULL;
    t_err_set = 0;
    return t_err;
}

/* bionic Dl_info has the same layout as glibc's */
int so_dladdr(const void *addr, Dl_info *info)
{
    uintptr_t a = (uintptr_t)addr;
    for (so_module *m = g_modules; m; m = m->next) {
        if (a < m->map_start || a >= m->map_start + m->map_size) continue;
        info->dli_fname = m->path;
        info->dli_fbase = (void *)m->map_start;
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
        uintptr_t best = 0;
        for (uint32_t i = 1; i < m->nsyms; i++) {
            const ElfW(Sym) *s = &m->symtab[i];
            if (s->st_shndx == SHN_UNDEF || !s->st_value) continue;
            int t = ELFW(ST_TYPE)(s->st_info);
            if (t != STT_FUNC && t != STT_OBJECT) continue;
#if defined(__arm__)
            uintptr_t sa = (m->bias + s->st_value) & ~(uintptr_t)1;   /* Thumb bit */
#else
            uintptr_t sa = m->bias + s->st_value;
#endif
            if (sa <= a && sa > best && (a < sa + s->st_size || s->st_size == 0)) {
                best = sa;
                info->dli_sname = m->strtab + s->st_name;
                info->dli_saddr = (void *)(m->bias + s->st_value);
            }
        }
        return 1;
    }
    return dladdr(addr, info);
}

static int host_load_counts(struct dl_phdr_info *info, size_t size, void *data)
{
    unsigned long long *c = data;
    if (size >= offsetof(struct dl_phdr_info, dlpi_subs) + sizeof info->dlpi_subs) {
        c[0] = info->dlpi_adds;
        c[1] = info->dlpi_subs;
    }
    return 1;
}

/* Our modules, then the host's. bionic's struct dl_phdr_info has glibc's layout. Unwinders linked
 * into Android libraries (libgcc's, LLVM libunwind: the AArch64 C++ exception path) find .eh_frame
 * with this; libgcc's caches what it found while dlpi_adds / dlpi_subs of the first object stay
 * the same, so ours count the host's loads and our own. */
int so_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data)
{
    unsigned long long counts[2] = { 0, 0 }, nmods = 0;
    dl_iterate_phdr(host_load_counts, counts);
    for (so_module *m = g_modules; m; m = m->next) nmods++;
    for (so_module *m = g_modules; m; m = m->next) {
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = m->bias;
        info.dlpi_name = m->path;
        info.dlpi_phdr = m->phdr;
        info.dlpi_phnum = m->phnum;
        info.dlpi_adds = counts[0] + nmods;
        info.dlpi_subs = counts[1];
        int r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return dl_iterate_phdr(cb, data);
}

uintptr_t so_dl_unwind_find_exidx(uintptr_t pc, int *pcount)
{
    return so_find_exidx(pc, pcount);
}

void *so_android_dlopen_ext(const char *filename, int flags, const void *extinfo)
{
    (void)extinfo;
    return so_dlopen(filename, flags);
}
