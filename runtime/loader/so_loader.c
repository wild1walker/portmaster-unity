/*
 * so_loader.c - ELF loader for Android ARMv7 shared libraries on armhf glibc.
 * See so_loader.h. Our own code; ideas from the Vita so_util.c and gmloader.
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "so_loader.h"

#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
#ifndef PT_GNU_RELRO
#define PT_GNU_RELRO 0x6474e552
#endif
#ifndef DT_RELR
#define DT_RELRSZ 35
#define DT_RELR 36
#endif
#define DT_ANDROID_REL     0x6000000f
#define DT_ANDROID_RELSZ   0x60000010
#define DT_ANDROID_RELA    0x60000011
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#ifndef R_ARM_IRELATIVE
#define R_ARM_IRELATIVE 160
#endif

/* bionic dlfcn values (LP32) */
#define B_RTLD_LAZY    1
#define B_RTLD_GLOBAL  2
#define B_RTLD_NOLOAD  4
#define B_RTLD_DEFAULT ((void *)0xffffffff)
#define B_RTLD_NEXT    ((void *)0xfffffffe)

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
    const Elf32_Phdr *phdr;
    int phnum;
    Elf32_Dyn *dyn;
    const Elf32_Sym *symtab;
    const char *strtab;
    uint32_t nsyms;
    const uint32_t *hash;
    uint32_t gnu_nbucket, gnu_symoffset, gnu_bloom_size, gnu_shift;
    const uint32_t *gnu_bloom, *gnu_bucket, *gnu_chain;
    const uint16_t *versym;
    Elf32_Rel *rel; size_t nrel;
    Elf32_Rel *jmprel; size_t njmprel;
    const uint32_t *relr; size_t nrelr;
    void (*init)(void);
    void (**init_array)(int, char **, char **); size_t ninit_array;
    void (**preinit_array)(int, char **, char **); size_t npreinit_array;
    void (*fini)(void);
    void (**fini_array)(void); size_t nfini_array;
    uintptr_t exidx; int exidx_count;
    int symbolic;
    int textrel;
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
    const Elf32_Sym *s = &m->symtab[i];
    int bind = ELF32_ST_BIND(s->st_info);
    if (s->st_shndx == SHN_UNDEF) return 0;
    if (bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_GNU_UNIQUE) return 0;
    if (ELF32_ST_VISIBILITY(s->st_other) == STV_HIDDEN || ELF32_ST_VISIBILITY(s->st_other) == STV_INTERNAL)
        return 0;
    if (m->versym && (m->versym[i] & 0x8000)) return 0;   /* hidden version */
    return 1;
}

static const Elf32_Sym *module_lookup(const so_module *m, const char *name)
{
    if (m->gnu_bucket) {
        uint32_t h = gnu_hash(name);
        uint32_t word = m->gnu_bloom[(h / 32) % m->gnu_bloom_size];
        uint32_t mask = (1u << (h % 32)) | (1u << ((h >> m->gnu_shift) % 32));
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

static uintptr_t sym_addr(const so_module *m, const Elf32_Sym *s)
{
    if (s->st_shndx == SHN_ABS) return s->st_value;
    return m->bias + s->st_value;
}

void *so_sym(so_module *m, const char *name)
{
    if (!m) return NULL;
    const Elf32_Sym *s = module_lookup(m, name);
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
        const Elf32_Sym *s = module_lookup(m, name);
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

static void *make_stub(const char *name)
{
    static uint32_t *page; static int used;
    static int rwx = 1;
    so_lock();
    if (!page || used + 4 > (int)(PAGE_SZ / 4)) {
        page = mmap(NULL, PAGE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            rwx = 0;
            page = mmap(NULL, PAGE_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }
        used = 0;
    }
    if (!rwx) mprotect(page, PAGE_SZ, PROT_READ | PROT_WRITE);
    uint32_t *s = page + used;
    used += 4;
    s[0] = 0xe59f0000;                 /* ldr r0, [pc, #0]  (-> s[2]) */
    s[1] = 0xe59ff000;                 /* ldr pc, [pc, #0]  (-> s[3]) */
    s[2] = (uint32_t)(uintptr_t)name;
    s[3] = (uint32_t)(uintptr_t)so_unresolved_called;
    __builtin___clear_cache((char *)s, (char *)(s + 4));
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
    const Elf32_Sym *s = &m->symtab[symidx];
    const char *name = m->strtab + s->st_name;
    int weak = ELF32_ST_BIND(s->st_info) == STB_WEAK;
    int type = ELF32_ST_TYPE(s->st_info);
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
        const Elf32_Sym *d = module_lookup(m->deps[i], name);
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

typedef struct { uintptr_t *val; uint8_t *done; } symcache;

static uintptr_t cached_resolve(so_module *m, symcache *c, uint32_t idx)
{
    if (idx >= m->nsyms) return 0;
    if (!c->done[idx]) { c->val[idx] = resolve_import(m, idx); c->done[idx] = 1; }
    return c->val[idx];
}

static int apply_rels(so_module *m, symcache *c, const Elf32_Rel *rel, size_t n, int irelative_pass)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t type = ELF32_R_TYPE(rel[i].r_info);
        uint32_t symidx = ELF32_R_SYM(rel[i].r_info);
        uint32_t *P = (uint32_t *)(m->bias + rel[i].r_offset);
        if (irelative_pass) {
            if (type == R_ARM_IRELATIVE) {
                uintptr_t (*resolver)(void) = (uintptr_t (*)(void))(m->bias + *P);
                *P = resolver();
            }
            continue;
        }
        switch (type) {
        case R_ARM_NONE:
            break;
        case R_ARM_ABS32:
            *P += symidx ? cached_resolve(m, c, symidx) : 0;
            break;
        case R_ARM_REL32:
            *P += (symidx ? cached_resolve(m, c, symidx) : 0) - (uintptr_t)P;
            break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
            *P = symidx ? cached_resolve(m, c, symidx) : 0;
            break;
        case R_ARM_RELATIVE:
            *P += m->bias;
            break;
        case R_ARM_IRELATIVE:
            break;   /* second pass, after protections */
        case R_ARM_COPY:
            set_err("%s: R_ARM_COPY relocation in a shared library", m->name);
            return -1;
        case R_ARM_TLS_DTPMOD32:
        case R_ARM_TLS_DTPOFF32:
        case R_ARM_TLS_TPOFF32:
            set_err("%s: TLS relocation %u not supported", m->name, type);
            return -1;
        default:
            set_err("%s: unsupported relocation type %u at 0x%x", m->name, type, rel[i].r_offset);
            return -1;
        }
    }
    return 0;
}

static void apply_relr(so_module *m)
{
    uint32_t *where = NULL;
    for (size_t i = 0; i < m->nrelr; i++) {
        uint32_t e = m->relr[i];
        if (!(e & 1)) {
            where = (uint32_t *)(m->bias + e);
            *where++ += m->bias;
        } else {
            for (uint32_t *p = where; (e >>= 1) != 0; p++)
                if (e & 1) *p += m->bias;
            where += 31;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* loading */

static int is_android_elf_file(const char *path)
{
    /* heuristic: an ARM ELF shared object whose DT_NEEDED include "libc.so" (bionic) */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    Elf32_Ehdr eh;
    int ok = 0;
    if (pread(fd, &eh, sizeof eh, 0) == sizeof eh && !memcmp(eh.e_ident, ELFMAG, SELFMAG) &&
        eh.e_ident[EI_CLASS] == ELFCLASS32 && eh.e_machine == EM_ARM && eh.e_type == ET_DYN) {
        Elf32_Phdr ph[32];
        int n = eh.e_phnum < 32 ? eh.e_phnum : 32;
        if (pread(fd, ph, n * sizeof(Elf32_Phdr), eh.e_phoff) == (ssize_t)(n * sizeof(Elf32_Phdr))) {
            for (int i = 0; i < n; i++) {
                if (ph[i].p_type == PT_INTERP) {
                    char buf[32] = {0};
                    pread(fd, buf, sizeof buf - 1, ph[i].p_offset);
                    if (strstr(buf, "/system/bin/linker")) ok = 1;
                }
            }
            if (!ok) {
                /* look for DT_NEEDED libc.so */
                for (int i = 0; i < n && !ok; i++) {
                    if (ph[i].p_type != PT_DYNAMIC) continue;
                    size_t sz = ph[i].p_filesz;
                    Elf32_Dyn *d = malloc(sz);
                    if (pread(fd, d, sz, ph[i].p_offset) == (ssize_t)sz) {
                        uint32_t strtab_vaddr = 0;
                        for (size_t k = 0; k < sz / sizeof *d && d[k].d_tag != DT_NULL; k++)
                            if (d[k].d_tag == DT_STRTAB) strtab_vaddr = d[k].d_un.d_val;
                        /* translate vaddr -> offset via PT_LOAD */
                        uint32_t stroff = 0;
                        for (int j = 0; j < n; j++)
                            if (ph[j].p_type == PT_LOAD && strtab_vaddr >= ph[j].p_vaddr &&
                                strtab_vaddr < ph[j].p_vaddr + ph[j].p_filesz)
                                stroff = strtab_vaddr - ph[j].p_vaddr + ph[j].p_offset;
                        for (size_t k = 0; stroff && k < sz / sizeof *d && d[k].d_tag != DT_NULL; k++) {
                            if (d[k].d_tag != DT_NEEDED) continue;
                            char nb[16] = {0};
                            pread(fd, nb, sizeof nb - 1, stroff + d[k].d_un.d_val);
                            if (!strcmp(nb, "libc.so")) ok = 1;
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

static int map_segments(so_module *m, int fd, const Elf32_Ehdr *eh, const Elf32_Phdr *ph)
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
        const Elf32_Phdr *p = &ph[i];
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
    size_t relsz = 0, pltrelsz = 0, init_arraysz = 0, fini_arraysz = 0, preinit_arraysz = 0, relrsz = 0;
    int pltrel = DT_REL;
    for (Elf32_Dyn *d = m->dyn; d->d_tag != DT_NULL; d++) {
        uint32_t v = d->d_un.d_val;
        switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf32_Sym *)(b + v); break;
        case DT_STRTAB: m->strtab = (const char *)(b + v); break;
        case DT_HASH: m->hash = (const uint32_t *)(b + v); break;
        case DT_GNU_HASH: {
            const uint32_t *g = (const uint32_t *)(b + v);
            m->gnu_nbucket = g[0]; m->gnu_symoffset = g[1]; m->gnu_bloom_size = g[2]; m->gnu_shift = g[3];
            m->gnu_bloom = g + 4;
            m->gnu_bucket = m->gnu_bloom + m->gnu_bloom_size;
            m->gnu_chain = m->gnu_bucket + m->gnu_nbucket;
            break;
        }
        case DT_VERSYM: m->versym = (const uint16_t *)(b + v); break;
        case DT_REL: m->rel = (Elf32_Rel *)(b + v); break;
        case DT_RELSZ: relsz = v; break;
        case DT_JMPREL: m->jmprel = (Elf32_Rel *)(b + v); break;
        case DT_PLTRELSZ: pltrelsz = v; break;
        case DT_PLTREL: pltrel = v; break;
        case DT_RELA: case DT_ANDROID_RELA: case DT_ANDROID_REL:
            set_err("%s: unsupported relocation table (tag 0x%x)", m->name, d->d_tag);
            return -1;
        case DT_RELR: case DT_ANDROID_RELR: m->relr = (const uint32_t *)(b + v); break;
        case DT_RELRSZ: case DT_ANDROID_RELRSZ: relrsz = v; break;
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
    if (pltrel != DT_REL) { set_err("%s: DT_PLTREL is not DT_REL", m->name); return -1; }
    m->nrel = relsz / sizeof(Elf32_Rel);
    m->njmprel = pltrelsz / sizeof(Elf32_Rel);
    m->nrelr = relrsz / 4;
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
    for (Elf32_Dyn *d = m->dyn; d->d_tag != DT_NULL; d++) {
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

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { set_err("%s: %s", path, strerror(errno)); return NULL; }
    Elf32_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG)) {
        set_err("%s: not an ELF file", path); close(fd); return NULL;
    }
    if (eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_machine != EM_ARM || eh.e_type != ET_DYN) {
        set_err("%s: not an ARM32 shared object", path); close(fd); return NULL;
    }
    Elf32_Phdr *ph = malloc(eh.e_phnum * sizeof *ph);
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
        case PT_DYNAMIC: m->dyn = (Elf32_Dyn *)(m->bias + ph[i].p_vaddr); break;
        case PT_ARM_EXIDX:
            m->exidx = m->bias + ph[i].p_vaddr;
            m->exidx_count = ph[i].p_memsz / 8;
            break;
        case PT_GNU_RELRO:
            m->relro_start = PAGE_DOWN(m->bias + ph[i].p_vaddr);
            m->relro_end = PAGE_DOWN(m->bias + ph[i].p_vaddr + ph[i].p_memsz);
            break;
        case PT_PHDR:
            m->phdr = (const Elf32_Phdr *)(m->bias + ph[i].p_vaddr);
            break;
        }
    }
    if (!m->phdr) {
        for (int i = 0; i < eh.e_phnum; i++)
            if (ph[i].p_type == PT_LOAD && eh.e_phoff >= ph[i].p_offset &&
                eh.e_phoff + eh.e_phnum * sizeof *ph <= ph[i].p_offset + ph[i].p_filesz)
                m->phdr = (const Elf32_Phdr *)(m->bias + ph[i].p_vaddr + eh.e_phoff - ph[i].p_offset);
    }
    if (!m->phdr) { m->phdr = ph; ph = NULL; }   /* keep our copy */
    m->phnum = eh.e_phnum;
    if (!m->dyn) { set_err("%s: no PT_DYNAMIC", path); goto fail; }
    if (parse_dynamic(m) < 0) goto fail;
    if (trace())
        fprintf(stderr, "[so] loading %s at %p (size 0x%zx), %zu+%zu relocations\n", m->path,
                (void *)m->bias, m->map_size, m->nrel, m->njmprel);

    if (load_needed(m, depth) < 0) goto fail;

    /* relocate */
    {
        symcache c;
        c.val = calloc(m->nsyms ? m->nsyms : 1, sizeof(uintptr_t));
        c.done = calloc(m->nsyms ? m->nsyms : 1, 1);
        int r = 0;
        if (m->relr) apply_relr(m);
        if (m->rel) r = apply_rels(m, &c, m->rel, m->nrel, 0);
        if (!r && m->jmprel) r = apply_rels(m, &c, m->jmprel, m->njmprel, 0);
        free(c.val); free(c.done);
        if (r < 0) goto fail;
    }
    /* final protections */
    for (int i = 0; i < m->nsegs; i++)
        mprotect((void *)m->segs[i].start, m->segs[i].len, m->segs[i].prot);
    if (m->rel) apply_rels(m, NULL, m->rel, m->nrel, 1);
    if (m->jmprel) apply_rels(m, NULL, m->jmprel, m->njmprel, 1);
    if (m->relro_end > m->relro_start && !getenv("SO_NO_RELRO"))
        mprotect((void *)m->relro_start, m->relro_end - m->relro_start, PROT_READ);
    __builtin___clear_cache((char *)m->map_start, (char *)(m->map_start + m->map_size));

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
/* EHABI: exidx lookup */

struct exidx_q { uintptr_t pc; uintptr_t exidx; int count; };

static int exidx_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    struct exidx_q *q = data;
    int match = 0;
    uintptr_t ex = 0; int cnt = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const Elf32_Phdr *p = &info->dlpi_phdr[i];
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
    const Elf32_Sym *s = module_lookup(m, name);
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
    if (trace() >= 2) fprintf(stderr, "[so] dlsym(%s, %s) -> %p\n", h && (uintptr_t)h < 0xfffffff0u && h != &g_global ? h->name : "default", symbol, p);
    if (!p) set_err("dlsym: symbol %s not found", symbol);
    return p;
}

void so_set_dlsym_hook(void *(*hook)(const char *symbol, void *found)) { g_dlsym_hook = hook; }

int so_dlclose(void *handle)
{
    so_handle *h = handle;
    if (h && (uintptr_t)h < 0xfffffff0u && h->magic == HANDLE_MAGIC && h->refs > 0) h->refs--;
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
            const Elf32_Sym *s = &m->symtab[i];
            if (s->st_shndx == SHN_UNDEF || !s->st_value) continue;
            int t = ELF32_ST_TYPE(s->st_info);
            if (t != STT_FUNC && t != STT_OBJECT) continue;
            uintptr_t sa = (m->bias + s->st_value) & ~(uintptr_t)1;
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

int so_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data)
{
    for (so_module *m = g_modules; m; m = m->next) {
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = m->bias;
        info.dlpi_name = m->path;
        info.dlpi_phdr = m->phdr;
        info.dlpi_phnum = m->phnum;
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
