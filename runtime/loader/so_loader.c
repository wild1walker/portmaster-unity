/*
 * so_loader.c - ELF loader for Android arm64-v8a shared libraries (ELF64, RELA) on aarch64 glibc. See so_loader.h. Our own code; ideas
 * from the Vita so_util.c and gmloader.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
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
#include "relayout.h"

/* the one kind of Android library this build loads: the host's own architecture */
#define SO_ELFCLASS ELFCLASS64
#define SO_MACHINE  EM_AARCH64
#define SO_ARCH     "AArch64 (arm64-v8a)"
/* ELF32_ / ELF64_ macros of that class (link.h's ElfW names the types only) */
#define ELFW(x) ELF64_##x

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
#if !defined(R_AARCH64_TLS_DTPMOD)   /* older elf.h: the ...64 names */
#define R_AARCH64_TLS_DTPMOD 1028
#define R_AARCH64_TLS_DTPREL 1029
#define R_AARCH64_TLS_TPREL  1030
#endif

/* bionic dlfcn values: LP32 and LP64 differ */
#define B_RTLD_LAZY    1
#define B_RTLD_NOLOAD  4
#define B_RTLD_GLOBAL  0x100
#define B_RTLD_DEFAULT ((void *)0)
#define B_RTLD_NEXT    ((void *)-1L)
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

/* Android-only libraries served by shims when nobody registered a table. libmediandk.so: Unity
 * 2022.3's libunity needs it (DT_NEEDED, AMediaCodec_* / AMEDIAFORMAT_KEY_* imports); the Android
 * shims serve stubs that fail cleanly (android/media_ndk.c). A host that maps it to NULL
 * (so_map_library) keeps dlopen() of it failing (older players then take their JNI path) while
 * DT_NEEDED and the imports still resolve to the shims. */
static const char *const g_android_only[] = {
    "liblog.so", "libandroid.so", "libOpenSLES.so", "libjnigraphics.so", "libstdc++.so",
    "libnativewindow.so", "libsync.so", "libmediandk.so", NULL
};

/* libraries whose dlopen() must fail cleanly (features we do not provide) */
static const char *const g_denied_prefix[] = {
    "libvulkan", "libOpenMAXAL", "libOMX", "libomx", "libstagefright",
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
#define STUB_WORDS 8

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
    void (*fn)(const char *) = so_unresolved_called;
    s[0] = 0x58000080;                 /* ldr x0, #16   (-> s[4..5]) */
    s[1] = 0x580000b0;                 /* ldr x16, #20  (-> s[6..7]) */
    s[2] = 0xd61f0200;                 /* br x16 */
    s[3] = 0xd503201f;                 /* nop */
    memcpy(s + 4, &name, sizeof name);
    memcpy(s + 6, &fn, sizeof fn);
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
    struct { unsigned long size, hwcap, hwcap2; } arg = { sizeof arg, getauxval(AT_HWCAP), getauxval(AT_HWCAP2) };
    return ((uintptr_t (*)(uint64_t, void *))resolver)(arg.hwcap | (1ull << 62), &arg);
}

static int reloc_one(relctx *c, const reloc_t *r, int rela)
{
    so_module *m = c->m;
    uint32_t type = ELFW(R_TYPE)(r->info);
    uint32_t sym = ELFW(R_SYM)(r->info);
    uintptr_t *P = (uintptr_t *)(m->bias + r->offset);
    if (type == R_AARCH64_NONE || type == 256 /* R_AARCH64_NONE of the first ABI release */) return 0;
    if (!in_image(m, P)) {
        set_err("%s: relocation type %u outside the image (offset 0x%lx)", m->name, type, (unsigned long)r->offset);
        return -1;
    }
    uintptr_t A = rela ? (uintptr_t)r->addend : *P, S = 0, add;
    switch (type) {
    case R_AARCH64_ABS64:
        add = A;
        goto symbolic;
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
        /* the Android linker adds RELA addends here too; a REL word in place is not an addend */
        add = rela ? A : 0;
    symbolic:
        if (sym) S = cached_resolve(c, sym);
        if (sym && sym < m->nsyms && c->done[sym] == SELF_IFUNC) defer_ifunc(c, P, S, add);
        else *P = S + add;
        break;
    case R_AARCH64_RELATIVE:
        *P = m->bias + A;
        break;
    case R_AARCH64_IRELATIVE:
        defer_ifunc(c, P, m->bias + A, 0);
        break;
    case R_AARCH64_COPY:
        set_err("%s: COPY relocation in a shared library", m->name);
        return -1;
    case R_AARCH64_TLS_DTPMOD:
    case R_AARCH64_TLS_DTPREL:
    case R_AARCH64_TLS_TPREL:
    case R_AARCH64_TLSDESC:
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

/* ------------------------------------------------------------------------ */
/* bionic's TLS_SLOT_THREAD_ID (both architectures; so_loader.h) */

/* Slot 1 (thread pointer + 4 / + 8) is bionic's pthread_internal_t of the thread; mimalloc,
 * inside libunity from Unity 6000.6, takes it as its thread id (_mi_thread_id: mrc p15 /
 * mrs tpidr_el0, then [tp + one word]). In glibc's TCB that word is unused and zero, so every
 * thread had id 0, mimalloc's "no owner": blocks freed by other threads were treated as the
 * freeing thread's own, and heaps broke (SIGSEGV in _mi_theap_malloc_zero, Mono asserting in
 * mono_thread_info_current). The word gets the thread pointer itself, unique per live thread:
 * for the thread that loads the first library, and for every thread created through
 * pthread_create below (the executable's definition, which the host's code, libhybris's hook for
 * Android code and the shared libraries, which reference it, all bind to). */
static inline uintptr_t *thread_pointer(void)
{
    uintptr_t *tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

void so_thread_slots(void)
{
    uintptr_t *tp = thread_pointer();
    if (!tp[1]) tp[1] = (uintptr_t)tp;
}

struct thread_start { void *(*fn)(void *); void *arg; };

static void *thread_start(void *p)
{
    struct thread_start s = *(struct thread_start *)p;
    free(p);
    so_thread_slots();
    return s.fn(s.arg);
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real) {
        real = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))dlsym(RTLD_NEXT, "pthread_create");
        if (!real) return EAGAIN;
    }
    struct thread_start *s = malloc(sizeof *s);
    if (!s) return EAGAIN;
    s->fn = fn;
    s->arg = arg;
    int r = real(th, attr, thread_start, s);
    if (r == EAGAIN || r == ENOMEM) {
        /* glibc gives a thread the stack size of ulimit -s (often 8 MB; Android: 1 MB): in a 32-bit process the
         * address space for another one can run out, and a caller that does not check (Unity 2017.4's
         * PlatformThread::Create) then waits for a thread that never started (a video clip switch froze the game,
         * 1 GB RK3326 handheld, 2026-11-12). Once more with Android's 1 MB; logged either way. */
        size_t size = 0, guard = 0;
        int detach = PTHREAD_CREATE_JOINABLE;
        if (attr) { pthread_attr_getstacksize(attr, &size); pthread_attr_getguardsize(attr, &guard); pthread_attr_getdetachstate(attr, &detach); }
        else { pthread_attr_t d; if (!pthread_getattr_default_np(&d)) { pthread_attr_getstacksize(&d, &size); pthread_attr_destroy(&d); } }
        long vm = 0;
        FILE *f = fopen("/proc/self/status", "r");
        char line[128];
        while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "VmSize:", 7)) vm = atol(line + 7) / 1024;
        if (f) fclose(f);
        int r2 = r;
        if (size > (1u << 20)) {
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setstacksize(&a, 1u << 20);
            if (guard) pthread_attr_setguardsize(&a, guard);
            pthread_attr_setdetachstate(&a, detach);
            r2 = real(th, &a, thread_start, s);
            pthread_attr_destroy(&a);
        }
        fprintf(stderr, "[loader] pthread_create: %s with a %zu KB stack (address space %ld MB)%s\n", strerror(r),
                size / 1024, vm, size > (1u << 20) ? (r2 ? ": with 1 MB too" : ": with 1 MB it starts") : "");
        r = r2;
    } else if (r) {
        fprintf(stderr, "[loader] pthread_create: %s\n", strerror(r));
    }
    if (r) free(s);
    return r;
}

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
    if (pltrel < 0) pltrel = DT_RELA;
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

static void (*g_load_hook)(so_module *m);
void so_set_load_hook(void (*hook)(so_module *m)) { g_load_hook = hook; }

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

    if (bionic_tls_check() < 0) return NULL;
    so_thread_slots();
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
    if (g_load_hook) g_load_hook(m);
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
/* UA_RELAYOUT (AArch64): copies of a module's hot functions laid out together (relayout.c) */

typedef struct relregion {
    uintptr_t lo, code_hi, hi;   /* the mapping; its code ends at code_hi */
    so_module *m;
    rl_image im;
    rl_plan plan;
    ElfW(Phdr) ph[2];            /* what so_dl_iterate_phdr reports: the code, its .eh_frame_hdr */
    struct relregion *next;
} relregion;
static relregion *volatile g_regions;    /* append-only, as g_modules */
static int g_nregions;

static relregion *region_at(uintptr_t a)
{
    for (relregion *r = g_regions; r; r = r->next)
        if (a >= r->lo && a < r->code_hi) return r;
    return NULL;
}

uintptr_t so_relayout_orig(uintptr_t a)
{
    relregion *r = region_at(a);
    if (!r) return a;
    uint64_t o = rl_orig_of(&r->plan, a, r->lo);
    return o ? (uintptr_t)o : a;
}

static void relayout_why(uint64_t start, const char *reason, void *ud)
{
    int *n = ud;
    if (trace() || (*n)++ < 5) fprintf(stderr, "[relayout] %llx stays: %s\n", (unsigned long long)start, reason);
}

/* a mapping of len bytes within reach of the module's code (B/BL: 128 MB) */
static void *region_map(const rl_image *im, so_module *m, size_t len)
{
    uintptr_t end = PAGE_UP(m->map_start + m->map_size), start = m->map_start;
    for (int k = 0; k < 96; k++) {
        /* after the module first (1 MB steps), then before it */
        uintptr_t hint = k < 48 ? end + (uintptr_t)k * (1u << 20) : start - len - (uintptr_t)(k - 48 + 1) * (1u << 20);
        void *p = mmap((void *)hint, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) continue;
        if (rl_reach(im, (uintptr_t)p, len)) return p;
        munmap(p, len);
    }
    return NULL;
}

static int cmp_uptr(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

/* SO_RELAYOUT_DATA: the module's data words that hold a copied function's address (its R_AARCH64_RELATIVE
 * relocations, applied: vtables, function tables, GOT entries) pointed at the copy, so virtual and indirect
 * calls skip the original's entry. Only functions whose address is never formed in code (ADRP + ADD, ADR)
 * nor exported: then every pointer to such a function the program can hold came from those words, all of
 * them now the copy's, and comparisons of function pointers come out as before. */
static int relayout_data(so_module *m, relregion *r, uint8_t *need_entry)
{
    int n = r->plan.n, nd = 0;
    uintptr_t *from = malloc(sizeof *from * (size_t)n * 3);
    if (!from) return 0;
    for (int i = 0; i < n; i++) { from[3 * i] = m->bias + r->plan.fn[i].start; from[3 * i + 1] = r->lo + r->plan.fn[i].at; from[3 * i + 2] = (uintptr_t)i; }
    qsort(from, (size_t)n, 3 * sizeof *from, cmp_uptr);
    uint8_t *keep = calloc((size_t)n, 1);
#define FIND(v) ((uintptr_t *)bsearch(&(v), from, (size_t)n, 3 * sizeof *from, cmp_uptr))
#define FN(e) ((int)(e)[2])
    /* exported */
    for (uint32_t i = 1; i < m->nsyms; i++) {
        uintptr_t v = m->bias + m->symtab[i].st_value;
        uintptr_t *e = m->symtab[i].st_shndx != SHN_UNDEF ? FIND(v) : NULL;
        if (e) keep[FN(e)] = 1;
    }
    /* formed in code: ADR, and ADRP followed (within 32 instructions) by ADD #imm on its register; and
       the direct branches from code that stays (B, BL, B.cond, CBZ, TBZ): those functions keep the
       branch at their original entry */
    for (int s = 0; s < m->nsegs; s++) {
        if (!(m->segs[s].prot & PROT_EXEC)) continue;
        const uint32_t *w = (const uint32_t *)m->segs[s].start;
        size_t nw = m->segs[s].len / 4;
        for (size_t i = 0; i < nw; i++) {
            uint32_t a = w[i];
            uintptr_t pc = m->segs[s].start + 4 * i;
            int64_t bo = 0; int br = 1;
            if ((a & 0x7c000000u) == 0x14000000u) bo = (int64_t)((uint64_t)(a & 0x3ffffff) << 38) >> 36;
            else if ((a & 0xff000000u) == 0x54000000u || (a & 0x7e000000u) == 0x34000000u) bo = (int64_t)((uint64_t)((a >> 5) & 0x7ffff) << 45) >> 43;
            else if ((a & 0x7e000000u) == 0x36000000u) bo = (int64_t)((uint64_t)((a >> 5) & 0x3fff) << 50) >> 48;
            else br = 0;
            if (br) {
                uintptr_t t = pc + (uintptr_t)bo, *e = FIND(t);
                if (e && !need_entry[FN(e)] && !rl_copy_of(&r->plan, pc - m->bias, r->lo)) need_entry[FN(e)] = 1;
                continue;
            }
            if ((a & 0x1f000000u) != 0x10000000u) continue;             /* ADR / ADRP */
            int64_t imm = (int64_t)((((uint64_t)((a >> 5) & 0x7ffff) << 2) | ((a >> 29) & 3)) << 43) >> 43;
            if (!(a >> 31)) { uintptr_t t = pc + (uintptr_t)imm; uintptr_t *e = FIND(t); if (e) keep[FN(e)] = 1; continue; }
            uintptr_t page = (pc & ~(uintptr_t)0xfff) + (uintptr_t)(imm * 4096);
            unsigned rd = a & 31;
            for (size_t k = i + 1; k < nw && k <= i + 32; k++) {
                uint32_t b = w[k];
                if ((b & 0xffc00000u) == 0x91000000u && ((b >> 5) & 31) == rd) {
                    uintptr_t t = page + ((b >> 10) & 0xfff);
                    uintptr_t *e = FIND(t);
                    if (e) keep[FN(e)] = 1;
                }
            }
        }
    }
    /* the data words: every writable segment (RELRO included: opened for the change, closed again) */
    for (int s = 0; s < m->nsegs; s++) {
        if ((m->segs[s].prot & PROT_EXEC) || !(m->segs[s].prot & PROT_WRITE)) continue;
        uintptr_t lo = m->segs[s].start, hi = lo + m->segs[s].len;
        int changed = 0;
        for (uintptr_t a = (lo + 7) & ~(uintptr_t)7; a + 8 <= hi; a += 8) {
            uintptr_t v = *(const uintptr_t *)a, *e;
            if (v < from[0] || v > from[3 * (n - 1)] || !(e = FIND(v)) || keep[FN(e)]) continue;
            if (!changed && m->relro_end > m->relro_start && lo < m->relro_end && hi > m->relro_start)
                mprotect((void *)m->relro_start, m->relro_end - m->relro_start, PROT_READ | PROT_WRITE);
            changed = 1;
            *(uintptr_t *)a = e[1];
            nd++;
        }
        if (changed && m->relro_end > m->relro_start && lo < m->relro_end && hi > m->relro_start && !getenv("SO_NO_RELRO"))
            mprotect((void *)m->relro_start, m->relro_end - m->relro_start, PROT_READ);
    }
#undef FIND
#undef FN
    int kept = 0;
    for (int i = 0; i < n; i++) { kept += keep[i]; if (keep[i]) need_entry[i] = 1; }
    fprintf(stderr, "[relayout] %s: %d data words point at copies now (%d functions whose address code forms or exports keep theirs)\n", m->name, nd, kept);
    free(keep); free(from);
    return nd;
}

int so_relayout(so_module *m, const char *list, int flags)
{
    if (!m || !list) return -1;
    char path[600];
    struct stat st;
    if (!stat(list, &st) && S_ISDIR(st.st_mode)) {       /* a directory of lists: <build id>.txt */
        rl_image im0;
        char e0[200], b0[140];
        if (rl_image_init(&im0, (const uint8_t *)m->bias, m->bias, m->map_start + m->map_size - m->bias, m->phdr, m->phnum, e0, sizeof e0) < 0) {
            fprintf(stderr, "[relayout] %s: %s\n", m->name, e0); return -1;
        }
        rl_buildid_hex(&im0, b0, sizeof b0);
        snprintf(path, sizeof path, "%s/%s.txt", list, b0);
        if (access(path, R_OK)) { fprintf(stderr, "[relayout] %s: no list for build id %s in %s: not laid out\n", m->name, b0, list); return 0; }
        list = path;
    }
    FILE *f = fopen(list, "r");
    if (!f) { fprintf(stderr, "[relayout] %s: %s\n", list, strerror(errno)); return -1; }
    rl_image im;
    char err[256], bid[140], want[140] = "";
    if (rl_image_init(&im, (const uint8_t *)m->bias, m->bias, m->map_start + m->map_size - m->bias, m->phdr, m->phnum, err, sizeof err) < 0) {
        fprintf(stderr, "[relayout] %s: %s\n", m->name, err); fclose(f); return -1;
    }
    rl_buildid_hex(&im, bid, sizeof bid);
    rl_want *starts = NULL; int n = 0, cap = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        rl_want w;
        if (!strncmp(line, "buildid ", 8)) { sscanf(line + 8, "%139s", want); continue; }
        if (rl_want_parse(line, &w) < 0) continue;
        if (n == cap) { cap = cap ? 2 * cap : 1024; starts = realloc(starts, sizeof *starts * (size_t)cap); }
        starts[n++] = w;
    }
    fclose(f);
    if (!want[0] || strcmp(want, bid)) {
        fprintf(stderr, "[relayout] %s: %s is for build id %s, this %s is %s: not laid out\n", m->name, list, want[0] ? want : "(none)", m->name, bid);
        free(starts); return -1;
    }
    /* only code that is still the file's own: a function hooked or patched in memory stays */
    int fd = open(m->path, O_RDONLY | O_CLOEXEC), changed = 0;
    uint64_t toff = 0, tva = 0;
    for (int i = 0; i < m->phnum; i++)
        if (m->phdr[i].p_type == PT_LOAD && (m->phdr[i].p_flags & PF_X)) { toff = m->phdr[i].p_offset; tva = m->phdr[i].p_vaddr; }
    if (fd < 0) { fprintf(stderr, "[relayout] %s: %s\n", m->path, strerror(errno)); free(starts); return -1; }
    int kept = 0;
    uint8_t *buf = malloc(512u << 10);
    for (int i = 0; i < n; i++) {
        uint64_t s, z;
        if (rl_fde_find(&im, starts[i].start, &s, &z, NULL) == 0 && s == starts[i].start && z <= (512u << 10) && s >= tva &&
            pread(fd, buf, z, (off_t)(s - tva + toff)) == (ssize_t)z && memcmp(buf, (const void *)(m->bias + s), z)) {
            changed++;
            continue;
        }
        starts[kept++] = starts[i];
    }
    free(buf);
    close(fd);
    relregion *r = calloc(1, sizeof *r);
    int quiet = 0;
    r->im = im;
    if (rl_plan_make(&r->plan, &r->im, starts, kept, relayout_why, &quiet) < 0 || !r->plan.n) {
        fprintf(stderr, "[relayout] %s: nothing to lay out\n", m->name); free(starts); free(r); return -1;
    }
    free(starts);
    size_t len = PAGE_UP(r->plan.total);
    uint8_t *dst = region_map(&r->im, m, len);
    if (!dst) { fprintf(stderr, "[relayout] %s: no room within 128 MB of its code\n", m->name); rl_plan_free(&r->plan); free(r); return -1; }
    if (rl_plan_emit(&r->plan, dst, (uintptr_t)dst, err, sizeof err) < 0) {
        fprintf(stderr, "[relayout] %s: %s: not laid out\n", m->name, err);
        munmap(dst, len); rl_plan_free(&r->plan); free(r); return -1;
    }
    mprotect(dst, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)dst, (char *)dst + r->plan.code_size);
    r->m = m;
    r->lo = (uintptr_t)dst; r->code_hi = r->lo + r->plan.code_size; r->hi = r->lo + len;
    /* (before the entries change: the scan for addresses formed in code reads them) */
    /* which original entries need the branch: all of them; with SO_RELAYOUT_DATA only those that code
       which stays branches to directly or whose address stays the original's (the rest are reached only
       from the copies and through the redirected words: their original pages stay clean) */
    uint8_t *need = malloc((size_t)r->plan.n + 1);
    memset(need, (flags & SO_RELAYOUT_DATA) ? 0 : 1, (size_t)r->plan.n + 1);
    int nd = (flags & SO_RELAYOUT_DATA) ? relayout_data(m, r, need) : -1;
    /* each original entry: a branch to its copy (calls through pointers, vtables, unmoved callers) */
    int prot = PROT_READ | PROT_EXEC;
    for (int i = 0; i < m->nsegs; i++) if (m->segs[i].prot & PROT_EXEC) prot = m->segs[i].prot;
    uintptr_t open_page = 0;
    int npatched = 0;
    for (int i = 0; i < r->plan.n; i++) {
        const rl_fn *fn = &r->plan.fn[i];
        if (!need[i]) continue;
        npatched++;
        uintptr_t at = m->bias + fn->start, pg = PAGE_DOWN(at);
        uint32_t b = 0x14000000u | (uint32_t)((((intptr_t)((uintptr_t)dst + fn->at) - (intptr_t)at) >> 2) & 0x3ffffff);
        if (pg != open_page) {
            if (open_page) mprotect((void *)open_page, PAGE_SZ, prot);
            mprotect((void *)pg, PAGE_SZ, prot | PROT_WRITE);
            open_page = pg;
        }
        memcpy((void *)at, &b, 4);
        __builtin___clear_cache((char *)at, (char *)at + 4);
    }
    if (open_page) mprotect((void *)open_page, PAGE_SZ, prot);
    free(need);
    r->ph[0].p_type = PT_LOAD; r->ph[0].p_flags = PF_R | PF_X; r->ph[0].p_vaddr = 0;
    r->ph[0].p_memsz = r->ph[0].p_filesz = r->plan.total; r->ph[0].p_align = PAGE_SZ;
    r->ph[1].p_type = PT_GNU_EH_FRAME; r->ph[1].p_flags = PF_R; r->ph[1].p_vaddr = r->plan.hdr_off;
    r->ph[1].p_memsz = r->ph[1].p_filesz = r->plan.hdr_size; r->ph[1].p_align = 4;
    so_lock();
    r->next = NULL;
    if (!g_regions) g_regions = r;
    else { relregion *t = g_regions; while (t->next) t = t->next; t->next = r; }
    g_nregions++;
    so_unlock();
    fprintf(stderr, "[relayout] %s: %d functions (%llu KB; %d split into hot and cold pieces) laid out at %p in the list's order (%s); %d left out%s\n",
            m->name, r->plan.n, (unsigned long long)(r->plan.code_size >> 10), r->plan.split, dst, list, r->plan.skipped + changed,
            changed ? " (some changed in memory by a hook)" : "");
    if (nd >= 0) fprintf(stderr, "[relayout] %s: %d original entries branch to their copies (the others are reached only from copies and redirected words)\n", m->name, npatched);
    return r->plan.n;
}

int so_atomics_llsc(so_module *m)
{
    if (!m) return -1;
    /* HWCAP_ATOMICS (LSE): the helpers' test would take the LSE instruction there; leave them */
    if (getauxval(AT_HWCAP) & (1ul << 8)) {
        fprintf(stderr, "[relayout] %s: this CPU has LSE atomics: the outline-atomic helpers stay as they are\n", m->name);
        return 0;
    }
    int n = 0, prot = PROT_READ | PROT_EXEC;
    uint64_t flag = 0, f2 = 0;
    for (int i = 0; i < m->nsegs; i++) {
        if (!(m->segs[i].prot & PROT_EXEC)) continue;
        prot = m->segs[i].prot;
        uint8_t *c = (uint8_t *)m->segs[i].start;
        size_t len = m->segs[i].len;
        int k = rl_atomics(c, (uintptr_t)c, len, 0, &flag);
        if (!k) continue;
        mprotect(c, len, prot | PROT_WRITE);
        n += rl_atomics(c, (uintptr_t)c, len, 1, &f2);
        mprotect(c, len, prot);
        __builtin___clear_cache((char *)c, (char *)c + len);
    }
    for (relregion *r = g_regions; r; r = r->next) {
        if (r->m != m) continue;
        uint64_t f3 = 0;
        if (!rl_atomics((uint8_t *)r->lo, r->lo, r->code_hi - r->lo, 0, &f3)) continue;
        if (flag && f3 != flag) continue;
        mprotect((void *)r->lo, r->hi - r->lo, PROT_READ | PROT_WRITE);
        n += rl_atomics((uint8_t *)r->lo, r->lo, r->code_hi - r->lo, 1, &f3);
        mprotect((void *)r->lo, r->hi - r->lo, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)r->lo, (char *)r->code_hi);
    }
    fprintf(stderr, "[relayout] %s: %d outline-atomic helpers go straight to their LL/SC loops (this CPU has no LSE)\n", m->name, n);
    return n;
}

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

static const char *(*g_dlopen_rewrite)(const char *path, char *buf, size_t size);
void so_set_dlopen_rewrite(const char *(*rewrite)(const char *, char *, size_t)) { g_dlopen_rewrite = rewrite; }
/* ICU: Android has it as a system library (libicuuc.so, libicui18n.so), which .NET's globalization code in newer
 * IL2CPP players dlopens and probes for its versioned symbols (u_getVersion_NN); the handheld's own ICU stands in
 * for it, the newest of the usual folders. */
static int host_icu(const char *b, char *out, size_t n)
{
    const char *lib = !strncmp(b, "libicuuc", 8) ? "libicuuc" : !strncmp(b, "libicui18n", 10) ? "libicui18n" : NULL;
    if (!lib) return 0;
    static const char *dirs[] = { "/usr/lib/aarch64-linux-gnu", "/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib", "/lib" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        char pat[256];
        snprintf(pat, sizeof pat, "%s/%s.so.*", dirs[d], lib);
        glob_t g;
        if (glob(pat, 0, NULL, &g) != 0) continue;
        int best = -1, bestv = -1;          /* the highest major version (libicuuc.so.76, not .so.76.1's minor) */
        for (size_t i = 0; i < g.gl_pathc; i++) {
            const char *v = strstr(g.gl_pathv[i], ".so.") + 4;
            if (strchr(v, '.')) continue;
            if (atoi(v) > bestv) { bestv = atoi(v); best = (int)i; }
        }
        if (best >= 0) snprintf(out, n, "%s", g.gl_pathv[best]);
        globfree(&g);
        if (best >= 0) return 1;
    }
    return 0;
}

void *so_dlopen(const char *filename, int flags)
{
    if (!filename) return &g_global;
    char rw[1024];
    const char *served = g_dlopen_rewrite ? g_dlopen_rewrite(filename, rw, sizeof rw) : NULL;
    if (served) filename = served;
    so_lock();
    libmap_defaults();
    const char *b = base_name(filename);
    so_handle *h = NULL;
    so_module *m = find_loaded(filename);
    if (m) { m->handle.refs++; h = &m->handle; goto out; }

    if (is_denied(b)) { set_err("dlopen(%s): library not available", filename); goto out; }
    fakelib *f = fakelib_find(b);
    libmap *lm = libmap_find(b);
    /* so_map_library(name, NULL) refuses dlopen even for an Android-only library, unless the host
     * registered a table for it */
    if (!f && lm && !lm->host) { set_err("dlopen(%s): library not available", filename); goto out; }
    if (f || is_android_only(b)) {
        h = find_handle(H_FAKE, b);
        if (!h) { h = new_handle(H_FAKE, b); h->table = f ? f->table : NULL; }
        else { h->refs++; if (f) h->table = f->table; }   /* a table registered since the last dlopen */
        goto out;
    }
    if (lm) {
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
    if (p && (served || is_android_elf_file(p))) {
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
    char icu[300];
    if (host_icu(b, icu, sizeof icu)) {
        void *hh = host_open(icu, (flags & B_RTLD_GLOBAL) != 0);
        if (hh) {
            libmap_set(b, icu);
            h = new_handle(H_HOST, b);
            h->host = hh;
            fprintf(stderr, "[so] %s: the handheld's %s\n", b, icu);
            goto out;
        }
    }
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

const char *so_module_at(const void *addr, uintptr_t *bias)
{
    uintptr_t a = (uintptr_t)addr;
    relregion *r = region_at(a);
    if (r) {                                             /* a copy: its module, a bias that gives the original's offset */
        if (bias) *bias = r->m->bias + (a - so_relayout_orig(a));
        return r->m->path;
    }
    for (so_module *m = g_modules; m; m = m->next)      /* lock-free: modules are never unloaded */
        if (a >= m->map_start && a < m->map_start + m->map_size) {
            if (bias) *bias = m->bias;
            return m->path;
        }
    return NULL;
}

/* bionic Dl_info has the same layout as glibc's */
int so_dladdr(const void *addr, Dl_info *info)
{
    uintptr_t a = so_relayout_orig((uintptr_t)addr);    /* a copy is named as its original */
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
            uintptr_t sa = m->bias + s->st_value;
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
    nmods += (unsigned long long)g_nregions;
    for (relregion *r = g_regions; r; r = r->next) {    /* relayout's copies, with their own .eh_frame_hdr */
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = r->lo;
        info.dlpi_name = r->m->path;
        info.dlpi_phdr = r->ph;
        info.dlpi_phnum = 2;
        info.dlpi_adds = counts[0] + nmods;
        info.dlpi_subs = counts[1];
        int ret = cb(&info, sizeof info, data);
        if (ret) return ret;
    }
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

void *so_android_dlopen_ext(const char *filename, int flags, const void *extinfo)
{
    (void)extinfo;
    return so_dlopen(filename, flags);
}
