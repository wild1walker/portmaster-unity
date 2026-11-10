// dyld.c - the loaded macOS images as a set, and dyld as their code sees it: which images
// there are (il2cpp finds its own metadata sections so), dlopen/dlsym/dladdr over them (a
// plugin that is a Mach-O file is loaded when the game opens it), the process's arguments.
// docs/macho-route.md.
#define _GNU_SOURCE
#include "darwin.h"
#include "a64fix.h"
#include "cxx/ua_unwind.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DYLD_MAX 32
// Room for the routines of the rewritten instructions, before each image (untouched pages cost
// nothing). A site's routine takes 20 to 100 bytes.
#define ISLAND_BYTES ((size_t)8 << 20)
// How deep one plugin's dylibs may need others before the load is refused.
#define NEED_DEPTH 8

static struct darwin_image images[DYLD_MAX];
// images[0, nimages) are linked and stay for good: any thread may look at them. Up to nloaded
// come those loaded and not yet linked, which only the thread that holds the lock sees: a load
// that fails takes them away again.
static _Atomic int nimages;
static int nloaded;
// One load at a time, as in dyld; an initializer may itself call dlopen.
static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int g_argc;
static char **g_argv, **g_envp;
static char *no_args[] = { NULL };
static char g_exe[1024];
static char g_plugins[1024];
static const struct darwin_dlopen_host *g_host;
static void (*add_image[8])(const void *header, intptr_t slide);
static int nadd_image;

int darwin_dyld_count(void) { return nimages; }
struct darwin_image *darwin_dyld_image(int i) { return i >= 0 && i < nimages ? &images[i] : NULL; }

struct darwin_image *darwin_dyld_image_at(const void *addr)
{
    for (int i = 0, n = nimages; i < n; i++) if (macho_contains(images[i].m, addr)) return &images[i];
    return NULL;
}

void darwin_dyld_set_args(int argc, char **argv, char **envp) { g_argc = argc; g_argv = argv; g_envp = envp; }
void darwin_dyld_set_executable(const char *path) { snprintf(g_exe, sizeof g_exe, "%s", path); }
void darwin_dyld_set_dlopen(const struct darwin_dlopen_host *h) { g_host = h; }
void darwin_dyld_set_plugin_dir(const char *dir) { snprintf(g_plugins, sizeof g_plugins, "%s", dir ? dir : ""); }

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// The lock is held from here to the images' own calls. flat: as a lookup without a library's
// name goes, past the images a dlopen kept to its handle (RTLD_LOCAL).
static void *find_export(const char *name, int flat)
{
    for (int i = 0; i < nloaded; i++) {
        if (flat && images[i].local) continue;
        void *p = macho_export(images[i].m, name);
        if (p) return p;
    }
    return NULL;
}

void *darwin_dyld_symbol(const char *name)
{
    pthread_mutex_lock(&lock);
    void *p = find_export(name, 0);
    pthread_mutex_unlock(&lock);
    return p;
}

// The image a library's name stands for: the one of that install name, else the one whose file
// is called so (a bundle has no install name).
static struct darwin_image *loaded_as(const char *leaf)
{
    for (int i = 0; i < nloaded; i++) if (!strcmp(images[i].m->install_name, leaf)) return &images[i];
    for (int i = 0; i < nloaded; i++) if (!strcmp(base_name(images[i].m->path), leaf)) return &images[i];
    return NULL;
}

// An import's answer: from the image whose library the import names when that one is loaded
// (what it re-exports is another loaded image's), else the shim.
static void *resolve(const char *name, const char *lib, void *ctx)
{
    (void)ctx;
    if (*lib) {
        struct darwin_image *im = loaded_as(lib);
        if (!im) return darwin_lookup(name);
        void *p = macho_export(im->m, name);
        return p ? p : find_export(name, 0);
    }
    void *p = find_export(name, 1);             // a flat lookup
    return p ? p : darwin_lookup(name);
}

// A coalesced name's one definition: the first image, in load order, that defines it for good;
// else the first that defines it at all. Only images that say they define or use such names
// take part. NULL: none (the asking image keeps its own).
static void *resolve_weak(const char *name, const char *lib, void *ctx)
{
    (void)lib; (void)ctx;
    void *weak = NULL;
    for (int i = 0; i < nloaded; i++) {
        if (!(images[i].m->flags & (MACHO_WEAK_DEFINES | MACHO_BINDS_TO_WEAK))) continue;
        unsigned flags = 0;
        void *p = macho_export_flags(images[i].m, name, &flags);
        if (!p) continue;
        if (!(flags & MACHO_EXPORT_WEAK)) return p;
        if (!weak) weak = p;
    }
    return weak;
}

static struct darwin_image *load_one(const char *path, char *err, size_t errlen)
{
    if (nloaded == DYLD_MAX) { snprintf(err, errlen, "%s: more than %d images", path, DYLD_MAX); return NULL; }
    struct macho_image *m = macho_load(path, ISLAND_BYTES, resolve, NULL, err, errlen);
    if (!m) return NULL;
    struct darwin_image *im = &images[nloaded];
    memset(im, 0, sizeof *im);
    im->m = m;
    im->tlv_slot = -1;
    nloaded++;
    return im;
}

struct darwin_image *darwin_dyld_load(const char *path, char *err, size_t errlen)
{
    pthread_mutex_lock(&lock);
    struct darwin_image *im = load_one(path, err, errlen);
    pthread_mutex_unlock(&lock);
    return im;
}

// The images loaded and not linked, from the last back to images[first], are no more.
static void unload_from(int first)
{
    if (first < nimages) first = nimages;
    while (nloaded > first) {
        struct darwin_image *im = &images[--nloaded];
        darwin_tlv_drop(im);
        macho_unload(im->m);
        im->m = NULL;
    }
}

static int rewrite(struct darwin_image *im, char *err, size_t errlen)
{
    struct macho_image *m = im->m;
    long n = macho_data_in_code(m, NULL, 0);
    if (n < 0) { snprintf(err, errlen, "%s: data-in-code ranges outside the image", m->path); return -1; }
    const uint8_t **skip = calloc((size_t)n * 2 + 2, sizeof *skip);
    if (!skip || macho_data_in_code(m, skip, n) != n) { free(skip); snprintf(err, errlen, "%s: data-in-code list", m->path); return -1; }
    struct a64fix f;
    a64fix_init(&f, m->island, m->island_bytes);
    const struct macho_section *s;
    for (int i = 0; (s = macho_code_section(m, i)); i++) {
        long before = f.rewritten;
        if (a64fix_code(&f, s->addr, s->size, skip, (int)n) != 0) {
            snprintf(err, errlen, "%s: the island for rewritten instructions is full or out of reach (%zu of %zu bytes used)", m->path, f.island_used, f.island_bytes);
            free(skip);
            return -1;
        }
        if (f.rewritten != before) __builtin___clear_cache((char *)s->addr, (char *)s->addr + s->size);
    }
    free(skip);
    if (f.refused) { snprintf(err, errlen, "%s: %ld instructions this CPU lacks and no rewrite covers: %s", m->path, f.refused, f.refused_what); return -1; }
    for (int k = 0; k < A64_NKIND; k++) im->a64_found += f.found[k];
    im->a64_rewritten = f.rewritten;
    return 0;
}

static int find_unwind(uintptr_t pc, struct ua_unwind_sections *s)
{
    struct darwin_image *im = darwin_dyld_image_at((const void *)pc);
    if (!im) return 0;
    const struct macho_section *eh = macho_section(im->m, "__TEXT", "__eh_frame");
    const struct macho_section *cu = macho_section(im->m, "__TEXT", "__unwind_info");
    memset(s, 0, sizeof *s);
    s->dso_base = (uintptr_t)im->m->header;
    for (int k = 0; k < im->m->nseg; k++)
        if (im->m->seg[k].addr == im->m->header) s->text_length = im->m->seg[k].vmsize;
    if (eh) { s->dwarf = (uintptr_t)eh->addr; s->dwarf_length = eh->size; }
    if (cu) { s->compact = (uintptr_t)cu->addr; s->compact_length = cu->size; }
    return 1;
}

// Every image loaded since the last link, together. Nothing an image keeps for good happens
// before all of them passed what can fail.
static int link_loaded(char *err, size_t errlen)
{
    int first = nimages;
    for (int i = first; i < nloaded; i++)
        if (macho_bind_weak(images[i].m, resolve_weak, NULL, err, errlen) != 0) return -1;
    for (int i = first; i < nloaded; i++) {
        struct darwin_image *im = &images[i];
        if (rewrite(im, err, errlen) != 0 || darwin_tlv_setup(im, err, errlen) != 0) return -1;
        if (macho_protect(im->m) != 0) { snprintf(err, errlen, "%s: segment protections: %s", im->m->path, strerror(errno)); return -1; }
    }
    ua_unwind_find_macho = find_unwind;
    nimages = nloaded;
    for (int i = first; i < nloaded; i++)
        for (int k = 0; k < nadd_image; k++) add_image[k](images[i].m->header, images[i].m->slide);
    return 0;
}

int darwin_dyld_link(char *err, size_t errlen)
{
    pthread_mutex_lock(&lock);
    int r = link_loaded(err, errlen);
    if (r != 0) unload_from(nimages);
    pthread_mutex_unlock(&lock);
    return r;
}

static void run_initializers(int first, int last)
{
    darwin_thread_enter();
    for (int i = first; i < last; i++) {
        struct darwin_image *im = &images[i];
        if (im->inited) continue;
        im->inited = 1;
        macho_run_initializers(im->m, g_argc, g_argv ? g_argv : no_args, g_envp ? g_envp : environ);
    }
}

void darwin_dyld_run_initializers(void)
{
    pthread_mutex_lock(&lock);
    run_initializers(0, nimages);
    pthread_mutex_unlock(&lock);
}

// ---- what the images call ---------------------------------------------------------------

static long darwin__dyld_image_count(void) { return nimages; }
static const void *darwin__dyld_get_image_header(uint32_t i) { return i < (uint32_t)nimages ? images[i].m->header : NULL; }
static const char *darwin__dyld_get_image_name(uint32_t i) { return i < (uint32_t)nimages ? images[i].m->path : NULL; }
static intptr_t darwin__dyld_get_image_vmaddr_slide(uint32_t i) { return i < (uint32_t)nimages ? images[i].m->slide : 0; }

// Called for every image there is, then for each that comes.
static void darwin__dyld_register_func_for_add_image(void (*fn)(const void *, intptr_t))
{
    pthread_mutex_lock(&lock);
    if (nadd_image == 8) { darwin_unanswered("_dyld_register_func_for_add_image", "more than 8 functions: this one hears of no later image"); }
    else add_image[nadd_image++] = fn;
    for (int i = 0; i < nimages; i++) fn(images[i].m->header, images[i].m->slide);
    pthread_mutex_unlock(&lock);
}
static void darwin__dyld_register_func_for_remove_image(void (*fn)(const void *, intptr_t)) { (void)fn; }   // no image ever leaves

static uint8_t *darwin_getsectiondata(const void *header, const char *seg, const char *sect, unsigned long *size)
{
    for (int i = 0; i < nimages; i++) {
        if (images[i].m->header != header) continue;
        const struct macho_section *s = macho_section(images[i].m, seg, sect);
        if (!s) break;
        *size = s->size;
        return s->addr;
    }
    *size = 0;
    return NULL;
}

static long darwin__NSGetExecutablePath(char *buf, uint32_t *size)
{
    if (!g_exe[0]) {
        ssize_t n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1);
        g_exe[n > 0 ? n : 0] = 0;
    }
    uint32_t need = (uint32_t)strlen(g_exe) + 1;
    if (*size < need) { *size = need; return -1; }
    memcpy(buf, g_exe, need);
    return 0;
}

static char ***darwin__NSGetEnviron(void) { return &environ; }
static char ***darwin__NSGetArgv(void) { if (!g_argv) g_argv = no_args; return &g_argv; }
static int *darwin__NSGetArgc(void) { return &g_argc; }

static void darwin_dyld_stub_binder(void)
{
    fprintf(stderr, "[darwin] dyld_stub_binder entered: a lazy pointer was left unbound\n");
    abort();
}

// dlopen's handles: an image's darwin_image, &main_handle for the process itself (every image
// and the shims), a host_handle for what the host opened.
static char main_handle;
struct host_handle { uint32_t magic; void *h; };
#define HOST_MAGIC 0x686f7374u

static void dl_fail(const char *fmt, ...)
{
    struct darwin_thread_bufs *b = darwin_thread_bufs();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->dl_error, sizeof b->dl_error, fmt, ap);
    va_end(ap);
    b->dl_failed = 1;
}

static int tracing(void)
{
    static int t = -1;
    if (t < 0) t = getenv("UA_MACHO_LOG") != NULL;
    return t;
}

// ---- a plugin that is a Mach-O file ---------------------------------------------------
// The files do not change while the game runs: one that did not load is not read again, and
// is said once, however often the game asks.
static struct { char *file, *err; } refused[32];
static int nrefused;

static const char *refused_before(const char *file)
{
    for (int i = 0; i < nrefused; i++) if (!strcmp(refused[i].file, file)) return refused[i].err;
    return NULL;
}

static void refuse(const char *file, const char *err)
{
    fprintf(stderr, "[darwin] dlopen: %s\n", err);
    if (nrefused == 32) return;
    char *f = strdup(file), *e = strdup(err);
    if (!f || !e) { free(f); free(e); return; }
    refused[nrefused].file = f;
    refused[nrefused++].err = e;
}

static int in_dir(char *out, size_t n, const char *dir, size_t dirlen, const char *pre, const char *stem, const char *ext)
{
    return (size_t)snprintf(out, n, "%.*s/%s%s%s", (int)dirlen, dir, pre, stem, ext) < n && macho_probe(out);
}

// The Mach-O file a dlopen's path stands for: the file itself; else, among the plugins, the
// one of its name as the game's code may spell it - il2cpp tries "name", "name.dylib",
// "libname.dylib", and under the Android player first the path that player would look at,
// ".../libname.so". 0: none (the host's library is asked for).
static int find_plugin(const char *path, char *out, size_t n)
{
    if (macho_probe(path)) return (size_t)snprintf(out, n, "%s", path) < n;
    const char *base = base_name(path);
    size_t dl = strlen(g_plugins);
    char stem[256];
    if (!dl || !*base || (size_t)snprintf(stem, sizeof stem, "%s", base) >= sizeof stem) return 0;
    if (in_dir(out, n, g_plugins, dl, "", stem, "")) return 1;
    char *dot = strrchr(stem, '.');
    if (dot && (!strcmp(dot, ".dylib") || !strcmp(dot, ".bundle") || !strcmp(dot, ".so"))) *dot = 0;
    if (!*stem) return 0;
    if (in_dir(out, n, g_plugins, dl, "", stem, ".dylib") || in_dir(out, n, g_plugins, dl, "", stem, ".bundle") ||
        in_dir(out, n, g_plugins, dl, "lib", stem, ".dylib") || in_dir(out, n, g_plugins, dl, "lib", stem, ".bundle")) return 1;
    return !strncmp(stem, "lib", 3) && stem[3] &&
           (in_dir(out, n, g_plugins, dl, "", stem + 3, ".dylib") || in_dir(out, n, g_plugins, dl, "", stem + 3, ".bundle"));
}

struct needs {
    const char *file;
    int    depth, failed;
    char  *err;
    size_t errlen;
    char   absent[200];         // the dylibs it names and no file is there for
};

static struct darwin_image *load_tree(const char *file, int depth, char *err, size_t errlen);

// One dylib an image names (macho_needed): the system's are a loaded image of that name or the
// shims; another is loaded now, from beside the image or from among the plugins, before the
// image that binds to it. One that is not there is left to the binds: only an import that
// finds no answer fails a load.
static void need(const char *name, int weak, void *ctx)
{
    struct needs *n = ctx;
    (void)weak;
    if (n->failed || !strncmp(name, "/usr/lib/", 9) || !strncmp(name, "/System/", 8)) return;
    const char *leaf = base_name(name);
    if (!*leaf || loaded_as(leaf)) return;
    char dep[1024];
    const char *slash = strrchr(n->file, '/');
    if (!(slash && in_dir(dep, sizeof dep, n->file, (size_t)(slash - n->file), "", leaf, "")) &&
        !(g_plugins[0] && in_dir(dep, sizeof dep, g_plugins, strlen(g_plugins), "", leaf, ""))) {
        size_t used = strlen(n->absent);
        snprintf(n->absent + used, sizeof n->absent - used, "%s%s", used ? ", " : "", leaf);
        return;
    }
    if (!load_tree(dep, n->depth + 1, n->err, n->errlen)) n->failed = 1;
}

// file and, before it, the dylibs it needs that are not loaded yet.
static struct darwin_image *load_tree(const char *file, int depth, char *err, size_t errlen)
{
    if (depth > NEED_DEPTH) { snprintf(err, errlen, "%s: the dylibs it needs nest deeper than %d (two that need each other?)", file, NEED_DEPTH); return NULL; }
    struct needs n = { .file = file, .depth = depth, .err = err, .errlen = errlen };
    if (macho_needed(file, need, &n, err, errlen) != 0 || n.failed) return NULL;
    struct darwin_image *im = load_one(file, err, errlen);
    if (!im && n.absent[0]) {
        size_t used = strlen(err);
        if (used < errlen) snprintf(err + used, errlen - used, " (it names %s, not found beside it)", n.absent);
    }
    return im;
}

// Loads, links and starts file with what it needs, or leaves no trace.
static struct darwin_image *open_plugin(const char *file, int mode, char *err, size_t errlen)
{
    int first = nloaded;
    struct darwin_image *im = load_tree(file, 0, err, errlen);
    if (!im || link_loaded(err, errlen) != 0) { unload_from(first); return NULL; }
    if (mode & DARWIN_RTLD_LOCAL) im->local = 1;
    run_initializers(first, nimages);
    return im;
}

static void *darwin_dlopen(const char *path, int mode)
{
    if (!path) return &main_handle;
    char file[1024], err[600];
    const char *why = NULL;                 // a Mach-O file stands for the path and is not to be had
    pthread_mutex_lock(&lock);
    struct darwin_image *im = NULL;
    for (int i = 0; i < nloaded && !im; i++) if (!strcmp(images[i].m->path, path)) im = &images[i];
    if (!im) im = loaded_as(base_name(path));
    if (!im && find_plugin(path, file, sizeof file)) {
        for (int i = 0; i < nloaded && !im; i++) if (!strcmp(images[i].m->path, file)) im = &images[i];
        if (!im && (mode & DARWIN_RTLD_NOLOAD)) why = "not loaded";
        else if (!im && !(why = refused_before(file)) && !(im = open_plugin(file, mode, err, sizeof err))) {
            refuse(file, err);
            why = err;
        }
    }
    if (im && !(mode & DARWIN_RTLD_LOCAL)) im->local = 0;
    if (tracing()) fprintf(stderr, "[darwin] dlopen(%s, 0x%x): %s%s\n", path, mode, im ? "the image " : why ? "refused: " : "no Mach-O file, the host is asked", im ? im->m->path : why ? why : "");
    if (why) dl_fail("dlopen(%s): %s", path, why);
    pthread_mutex_unlock(&lock);
    if (im || why) return im;
    if (g_host) {
        void *h = g_host->open(path, mode);
        if (h) {
            struct host_handle *hh = malloc(sizeof *hh);
            if (hh) { hh->magic = HOST_MAGIC; hh->h = h; return hh; }
            g_host->close(h);
        }
    } else darwin_unanswered("dlopen", path);
    dl_fail("dlopen(%s): image not found", path);
    return NULL;
}

static int is_image(const void *h) { return (const struct darwin_image *)h >= images && (const struct darwin_image *)h < images + DYLD_MAX; }

static void *darwin_dlsym(void *handle, const char *name)
{
    char mangled[512];
    void *p = NULL;
    int special = handle == &main_handle || (intptr_t)handle == (intptr_t)DARWIN_RTLD_DEFAULT || (intptr_t)handle == (intptr_t)DARWIN_RTLD_NEXT ||
                  (intptr_t)handle == (intptr_t)DARWIN_RTLD_SELF || (intptr_t)handle == (intptr_t)DARWIN_RTLD_MAIN_ONLY;
    if ((size_t)snprintf(mangled, sizeof mangled, "_%s", name) >= sizeof mangled) { dl_fail("dlsym(%s): name too long", name); return NULL; }
    if (special) {
        pthread_mutex_lock(&lock);
        p = find_export(mangled, 1);
        pthread_mutex_unlock(&lock);
        if (!p) p = darwin_lookup(mangled);
    } else if (is_image(handle)) {
        pthread_mutex_lock(&lock);
        p = macho_export(((struct darwin_image *)handle)->m, mangled);
        if (!p) p = find_export(mangled, 0);            // what it re-exports, its dependencies
        pthread_mutex_unlock(&lock);
    } else if (handle && ((struct host_handle *)handle)->magic == HOST_MAGIC && g_host) {
        p = g_host->sym(((struct host_handle *)handle)->h, name);
    }
    if (!p) dl_fail("dlsym(%s): symbol not found", name);
    return p;
}

static long darwin_dlclose(void *handle)
{
    if (handle == &main_handle || is_image(handle)) return 0;      // no image ever leaves
    if (handle && ((struct host_handle *)handle)->magic == HOST_MAGIC && g_host) {
        int r = g_host->close(((struct host_handle *)handle)->h);
        ((struct host_handle *)handle)->magic = 0;
        free(handle);
        return r;
    }
    dl_fail("dlclose: %s", "invalid handle");
    return -1;
}

static const char *darwin_dlerror(void)
{
    struct darwin_thread_bufs *b = darwin_thread_bufs();
    if (!b->dl_failed) return NULL;
    b->dl_failed = 0;
    return b->dl_error;
}

// Dl_info is four pointers on both sides. For an image's address: its path, its header, the
// symbol without the leading underscore.
static long darwin_dladdr(const void *addr, Dl_info *info)
{
    struct darwin_image *im = darwin_dyld_image_at(addr);
    if (!im) return dladdr(addr, info) != 0;
    uint64_t off = 0;
    const char *name = macho_symbolize(im->m, addr, &off);
    info->dli_fname = im->m->path;
    info->dli_fbase = im->m->header;
    info->dli_sname = name ? (name[0] == '_' ? name + 1 : name) : NULL;
    info->dli_saddr = name ? (void *)((uintptr_t)addr - off) : NULL;
    return 1;
}

const struct darwin_sym darwin_syms_dyld[] = {
    NAMED("__dyld_image_count", darwin__dyld_image_count),
    NAMED("__dyld_get_image_header", darwin__dyld_get_image_header),
    NAMED("__dyld_get_image_name", darwin__dyld_get_image_name),
    NAMED("__dyld_get_image_vmaddr_slide", darwin__dyld_get_image_vmaddr_slide),
    NAMED("__dyld_register_func_for_add_image", darwin__dyld_register_func_for_add_image),
    NAMED("__dyld_register_func_for_remove_image", darwin__dyld_register_func_for_remove_image),
    NAMED("__NSGetExecutablePath", darwin__NSGetExecutablePath),
    NAMED("__NSGetEnviron", darwin__NSGetEnviron),
    NAMED("__NSGetArgv", darwin__NSGetArgv),
    NAMED("__NSGetArgc", darwin__NSGetArgc),
    SHIM(getsectiondata), SHIM(dlopen), SHIM(dlsym), SHIM(dlclose), SHIM(dlerror), SHIM(dladdr),
    NAMED("dyld_stub_binder", darwin_dyld_stub_binder),
    { NULL, NULL }
};
