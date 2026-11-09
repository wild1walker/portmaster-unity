// GL/EGL entry points the host replaces for experiments and per-game shader work
// (registered before the EGL glue, so they win both for imports and for dlsym/eglGetProcAddress).
//
//   UA_SHADER_DUMP=<dir>      writes every shader source Unity compiles to <dir>/<hash>.glsl
//                             (once per source; <hash> = FNV-1a 64 of the strings Unity passes)
//   UA_SHADER_OVERRIDE=<dir>  compiles <dir>/<hash>.glsl instead of that source when it exists
//                             (logs "[gl] shader <hash> overridden")
//   Both turn off Unity's program binary cache (data/cache/UnityShaderCache: cached programs are
//   never compiled from source, and an overridden program must not be cached under the original's
//   key): GL_NUM_PROGRAM_BINARY_FORMATS reads 0.
//   UA_COMPUTE_GROUP=<n>      the compute work group limits the driver reports, raised to n
//                             invocations (GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS; _SIZE x and y n, z
//                             at most 64): android_package.json's "compute_group", written for data
//                             whose kernels the converter split to run in groups of 128 (GLES 3.1's
//                             least; docs/shaders.md, "Work groups over 128 invocations"). Unity
//                             checks a kernel's recorded group size against these limits before it
//                             compiles it ("Kernel size exceeds device limits"); the record stays the
//                             game's, which its dispatches are sized by. And glGetProgramiv's
//                             GL_COMPUTE_WORK_GROUP_SIZE, which Unity reads after linking a kernel
//                             (GetKernelThreadGroupSizes), gives a split kernel's own size, the one
//                             its marker line names ("// pm_work_group x y z"). 0: the driver's
//   UA_SWAP_INTERVAL=<n>      eglSwapInterval value used whatever Unity asks for (0: no vsync, so
//                             frame times show the real CPU/GPU cost instead of vsync steps)
//   UA_GL_PROGLOG=1           logs how long each shader compile, program link and program binary
//                             load (glProgramBinary: Unity's program binary cache) takes, on which
//                             thread: "[glprog] link p<n> <ms> ms <thread>"; "binary p<n> <ms> ms ok|failed";
//                             "get-binary"; "compile s<n>"; at the first frame the driver's program
//                             binary formats and its EGL blob cache extension (a measurement: nothing
//                             changes; tools/analysis/hitchsum.py --glprog sums the lines)
//   UA_GL_PROGCACHE=1         the host's own program binary cache, below Unity's (data/cache/ua_progcache/):
//                             a glLinkProgram whose shaders' sources, types and attribute bindings (and the
//                             driver's renderer and version) were linked before loads that link's binary with
//                             glProgramBinary instead; the driver checks it (link status), and a refused one is
//                             linked from source as before. Unity's own cache keeps working above it: Unity
//                             2017.4 on Mali's GBM driver (r13p0) reads about 130 of its cache files a launch
//                             and links those programs again anyway, with byte-identical binaries. Programs
//                             with transform-feedback varyings or program parameters other than the binary
//                             hint are never cached; off with UA_SHADER_DUMP / UA_SHADER_OVERRIDE.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../loader/so_loader.h"
#include "glhooks.h"
#include "glrec.h"

typedef void (*shader_source_fn)(unsigned, int, const char *const *, const int *);
typedef unsigned (*swap_interval_fn)(void *, int);
typedef void (*get_integerv_fn)(unsigned, int *);
typedef void (*get_integeri_v_fn)(unsigned, unsigned, int *);

static shader_source_fn r_ShaderSource;
static swap_interval_fn r_SwapInterval;
static get_integerv_fn r_GetIntegerv;
static get_integeri_v_fn r_GetIntegeri_v;
static int g_compute_group;                  // UA_COMPUTE_GROUP / android_package.json's compute_group
static const char *g_dump, *g_override;
static int g_proglog;

static void *gl_real(const char *lib, const char *alt, const char *name)
{
    void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h && alt) h = dlopen(alt, RTLD_NOW | RTLD_GLOBAL);
    return h ? dlsym(h, name) : NULL;
}

static uint64_t fnv64(const char *const *s, const int *len, int n)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
        if (!s[i]) continue;
        size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(s[i]);
        for (size_t k = 0; k < l; k++) { h ^= (unsigned char)s[i][k]; h *= 1099511628211ull; }
    }
    return h;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

static void pc_note_source(unsigned shader, uint64_t h);
static int g_progcache;

static void w_ShaderSource(unsigned shader, int count, const char *const *string, const int *length)
{
    if (!r_ShaderSource) return;
    if (g_progcache) pc_note_source(shader, fnv64(string, length, count));
    if (!g_dump && !g_override) { r_ShaderSource(shader, count, string, length); return; }
    char path[600];
    uint64_t h = fnv64(string, length, count);
    if (g_dump) {
        struct stat st;
        snprintf(path, sizeof path, "%s/%016llx.glsl", g_dump, (unsigned long long)h);
        if (stat(path, &st) != 0) {
            FILE *f = fopen(path, "wb");
            if (f) {
                for (int i = 0; i < count; i++)
                    if (string[i]) fwrite(string[i], 1, length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]), f);
                fclose(f);
            }
        }
    }
    if (g_override) {
        snprintf(path, sizeof path, "%s/%016llx.glsl", g_override, (unsigned long long)h);
        char *src = read_file(path);
        if (src) {
            fprintf(stderr, "[gl] shader %016llx overridden\n", (unsigned long long)h);
            const char *one[1] = { src };
            r_ShaderSource(shader, 1, one, NULL);
            free(src);
            return;
        }
    }
    r_ShaderSource(shader, count, string, length);
}

// UA_GL_PROGLOG: the time of each compile, link and binary load
typedef void (*u_fn)(unsigned);
typedef void (*prog_binary_fn)(unsigned, unsigned, const void *, int);
typedef void (*get_prog_binary_fn)(unsigned, int, int *, unsigned *, void *);
typedef void (*get_programiv_fn)(unsigned, unsigned, int *);
typedef const unsigned char *(*get_string_fn)(unsigned);
static u_fn r_CompileShader, r_LinkProgram;
static prog_binary_fn r_ProgramBinary;
static get_prog_binary_fn r_GetProgramBinary;
static get_programiv_fn r_GetProgramiv;

static double ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
// the call's end in seconds since the first frame (frames.c's "[hitch] ... at <s>" clock; negative
// before it)
static double g_first_ms;
static double since_first(double end_ms) { return g_first_ms ? (end_ms - g_first_ms) / 1e3 : -1; }
static const char *thread_name(char *b, size_t n)
{
    if (pthread_getname_np(pthread_self(), b, n) != 0 || !*b) snprintf(b, n, "?");
    return b;
}
#define GL_LINK_STATUS 0x8B82
// FNV-1a 64 of a program binary (the same bytes Unity writes after its 4-byte format in
// UnityShaderCache/<hash>: a cache file and the call that loaded or made it can be matched)
static unsigned long long bin_hash(const void *b, int len)
{
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; b && i < len; i++) h = (h ^ ((const unsigned char *)b)[i]) * 1099511628211ULL;
    return h;
}
static void w_CompileShader(unsigned s)
{
    double t = ms_now();
    r_CompileShader(s);
    double e = ms_now();
    char n[32];
    fprintf(stderr, "[glprog] compile s%u %.2f ms at %.3f s %s\n", s, e - t, since_first(e), thread_name(n, sizeof n));
}
// ---- UA_GL_PROGCACHE: the host's program binary cache (the header's comment)
typedef void (*attach_fn)(unsigned, unsigned);
typedef void (*bind_attrib_fn)(unsigned, unsigned, const char *);
typedef void (*tf_varyings_fn)(unsigned, int, const char *const *, unsigned);
typedef void (*prog_param_fn)(unsigned, unsigned, int);
typedef void (*get_shaderiv_fn)(unsigned, unsigned, int *);
static attach_fn r_AttachShader, r_DetachShader;
static bind_attrib_fn r_BindAttribLocation;
static tf_varyings_fn r_TransformFeedbackVaryings;
static prog_param_fn r_ProgramParameteri;
static u_fn r_DeleteShader, r_DeleteProgram;
static get_shaderiv_fn r_GetShaderiv;
static char g_pc_dir[512];
static pthread_mutex_t g_pc_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_pc_hits, g_pc_links, g_pc_refused, g_pc_stored;
static double g_pc_hit_ms, g_pc_link_ms;
static uint64_t g_pc_driver;               // the driver's renderer and version strings, hashed
static const unsigned char *(*r_pc_GetString)(unsigned);
#define PC_SLOTS 8192                       // (open addressing, by GL name; plenty for a game's programs)
#define PC_MAXSH 4
static struct pc_shader { unsigned id; uint64_t h; } g_pc_sh[PC_SLOTS];
static struct pc_prog { unsigned id; unsigned sh[PC_MAXSH]; int nsh; uint64_t attribs; int nocache; } g_pc_pr[PC_SLOTS];
#define GL_SHADER_TYPE 0x8B4F
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257

static uint64_t mix64(uint64_t h, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const unsigned char *)p)[i]; h *= 1099511628211ull; }
    return h;
}
// the slot of a GL name in a table (id 0: free); create: take a free one
// (a deleted object's slot keeps a name no GL object has, so the probe chains stay whole; a new
// object takes the first such slot on its chain)
#define PC_GONE 0xFFFFFFFFu
static struct pc_shader *pc_shader(unsigned id, int create)
{
    struct pc_shader *gone = NULL;
    for (unsigned i = 0, k = id % PC_SLOTS; i < PC_SLOTS; i++, k = (k + 1) % PC_SLOTS) {
        if (g_pc_sh[k].id == id) return &g_pc_sh[k];
        if (g_pc_sh[k].id == PC_GONE && !gone) gone = &g_pc_sh[k];
        if (!g_pc_sh[k].id) { gone = gone ? gone : &g_pc_sh[k]; break; }
    }
    if (!create || !gone) return NULL;
    gone->id = id; gone->h = 0;
    return gone;
}
static struct pc_prog *pc_prog(unsigned id, int create)
{
    struct pc_prog *gone = NULL;
    for (unsigned i = 0, k = id % PC_SLOTS; i < PC_SLOTS; i++, k = (k + 1) % PC_SLOTS) {
        if (g_pc_pr[k].id == id) return &g_pc_pr[k];
        if (g_pc_pr[k].id == PC_GONE && !gone) gone = &g_pc_pr[k];
        if (!g_pc_pr[k].id) { gone = gone ? gone : &g_pc_pr[k]; break; }
    }
    if (!create || !gone) return NULL;
    memset(gone, 0, sizeof *gone);
    gone->id = id;
    gone->attribs = 1469598103934665603ull;
    return gone;
}
static void pc_note_source(unsigned shader, uint64_t h)
{
    pthread_mutex_lock(&g_pc_lock);
    struct pc_shader *s = pc_shader(shader, 1);
    if (s) s->h = h ? h : 1;
    pthread_mutex_unlock(&g_pc_lock);
}
static void w_AttachShader(unsigned p, unsigned sh)
{
    r_AttachShader(p, sh);
    pthread_mutex_lock(&g_pc_lock);
    struct pc_prog *g = pc_prog(p, 1);
    if (g) { if (g->nsh < PC_MAXSH) g->sh[g->nsh++] = sh; else g->nocache = 1; }
    pthread_mutex_unlock(&g_pc_lock);
}
static void w_DetachShader(unsigned p, unsigned sh)
{
    r_DetachShader(p, sh);
    pthread_mutex_lock(&g_pc_lock);
    struct pc_prog *g = pc_prog(p, 0);
    if (g)
        for (int i = 0; i < g->nsh; i++)
            if (g->sh[i] == sh) { g->sh[i] = g->sh[--g->nsh]; break; }
    pthread_mutex_unlock(&g_pc_lock);
}
static void w_BindAttribLocation(unsigned p, unsigned index, const char *name)
{
    r_BindAttribLocation(p, index, name);
    pthread_mutex_lock(&g_pc_lock);
    struct pc_prog *g = pc_prog(p, 1);
    if (g) { g->attribs = mix64(g->attribs, &index, sizeof index); g->attribs = mix64(g->attribs, name, strlen(name) + 1); }
    pthread_mutex_unlock(&g_pc_lock);
}
static void w_TransformFeedbackVaryings(unsigned p, int n, const char *const *v, unsigned mode)
{
    r_TransformFeedbackVaryings(p, n, v, mode);
    pthread_mutex_lock(&g_pc_lock);
    struct pc_prog *g = pc_prog(p, 1);
    if (g) g->nocache = 1;
    pthread_mutex_unlock(&g_pc_lock);
}
static void w_ProgramParameteri(unsigned p, unsigned pname, int value)
{
    r_ProgramParameteri(p, pname, value);
    if (pname == GL_PROGRAM_BINARY_RETRIEVABLE_HINT) return;
    pthread_mutex_lock(&g_pc_lock);
    struct pc_prog *g = pc_prog(p, 1);
    if (g) g->nocache = 1;
    pthread_mutex_unlock(&g_pc_lock);
}
static void w_DeleteShader(unsigned sh)
{
    r_DeleteShader(sh);
    pthread_mutex_lock(&g_pc_lock);
    struct pc_shader *s = pc_shader(sh, 0);
    if (s) { s->id = PC_GONE; s->h = 0; }
    pthread_mutex_unlock(&g_pc_lock);
}
static void w_DeleteProgram(unsigned p)
{
    r_DeleteProgram(p);
    pthread_mutex_lock(&g_pc_lock);
    struct pc_prog *g = pc_prog(p, 0);
    if (g) g->id = PC_GONE;
    pthread_mutex_unlock(&g_pc_lock);
}
// the program's key: each attached shader's type and source hash (in type order), its attribute
// bindings, the driver; 0 when it cannot be cached (a source not seen, too many shaders, transform
// feedback, a program parameter)
#define GL_RENDERER_ 0x1F01
#define GL_VERSION_ 0x1F02
static uint64_t pc_key(unsigned p)
{
    uint64_t key = 0;
    if (!g_pc_driver && r_pc_GetString) {      // (a context is current: the link's)
        const unsigned char *r = r_pc_GetString(GL_RENDERER_), *v = r_pc_GetString(GL_VERSION_);
        uint64_t d = 1469598103934665603ull;
        if (r) d = mix64(d, r, strlen((const char *)r) + 1);
        if (v) d = mix64(d, v, strlen((const char *)v) + 1);
        g_pc_driver = d;
    }
    pthread_mutex_lock(&g_pc_lock);
    struct pc_prog *g = pc_prog(p, 0);
    if (g && !g->nocache && g->nsh > 0) {
        struct { int type; uint64_t h; } e[PC_MAXSH];
        int n = 0, ok = 1;
        for (int i = 0; i < g->nsh; i++) {
            struct pc_shader *s = pc_shader(g->sh[i], 0);
            if (!s || !s->h) { ok = 0; break; }
            e[n].type = 0; e[n].h = s->h; n++;
        }
        if (ok) {
            pthread_mutex_unlock(&g_pc_lock);
            for (int i = 0; i < n; i++) r_GetShaderiv(g->sh[i], GL_SHADER_TYPE, &e[i].type);
            pthread_mutex_lock(&g_pc_lock);
            for (int i = 1; i < n; i++)            // (by type, then hash: the order of attachment does not matter)
                for (int j = i; j > 0 && (e[j].type < e[j - 1].type || (e[j].type == e[j - 1].type && e[j].h < e[j - 1].h)); j--) {
                    __typeof__(e[0]) x = e[j]; e[j] = e[j - 1]; e[j - 1] = x;
                }
            key = mix64(1469598103934665603ull, &g_pc_driver, sizeof g_pc_driver);
            for (int i = 0; i < n; i++) { key = mix64(key, &e[i].type, sizeof e[i].type); key = mix64(key, &e[i].h, sizeof e[i].h); }
            key = mix64(key, &g->attribs, sizeof g->attribs);
            if (!key) key = 1;
        }
    }
    pthread_mutex_unlock(&g_pc_lock);
    return key;
}
#define PC_MAGIC 0x43504155u                // "UAPC"
static void pc_path(char *b, size_t n, uint64_t key) { snprintf(b, n, "%s/%016llx", g_pc_dir, (unsigned long long)key); }
// a cached binary: its format and bytes (malloc'd), or NULL
static void *pc_load(uint64_t key, unsigned *fmt, int *len)
{
    char path[600];
    pc_path(path, sizeof path, key);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint32_t h[3];
    void *b = NULL;
    if (fread(h, 4, 3, f) == 3 && h[0] == PC_MAGIC && h[2] > 0 && h[2] < (64u << 20) && (b = malloc(h[2]))) {
        if (fread(b, 1, h[2], f) == h[2]) { *fmt = h[1]; *len = (int)h[2]; }
        else { free(b); b = NULL; }
    }
    fclose(f);
    return b;
}
static void pc_store(unsigned p, uint64_t key)
{
    int ok = 0, n = 0;
    r_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok != 1) return;
    r_GetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &n);
    if (n <= 0 || n > (64 << 20)) return;
    void *b = malloc((size_t)n);
    if (!b) return;
    int len = 0;
    unsigned fmt = 0;
    r_GetProgramBinary(p, n, &len, &fmt, b);
    if (len > 0) {
        char path[600], tmp[620];
        pc_path(path, sizeof path, key);
        snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
        FILE *f = fopen(tmp, "wb");
        uint32_t h[3] = { PC_MAGIC, fmt, (uint32_t)len };
        if (f) {
            int good = fwrite(h, 4, 3, f) == 3 && fwrite(b, 1, (size_t)len, f) == (size_t)len;
            good = (fclose(f) == 0) && good;
            if (good && rename(tmp, path) == 0) g_pc_stored++;
            else unlink(tmp);
        }
    }
    free(b);
}
static void w_LinkProgram(unsigned p)
{
    double t = ms_now();
    int how = 0;                            // 0 linked from source, 1 loaded from the cache
    uint64_t key = g_progcache ? pc_key(p) : 0;
    if (key) {
        unsigned fmt = 0;
        int len = 0;
        void *b = pc_load(key, &fmt, &len);
        if (b) {
            int ok = 0;
            r_ProgramBinary(p, fmt, b, len);
            r_GetProgramiv(p, GL_LINK_STATUS, &ok);
            free(b);
            if (ok == 1) how = 1;
            else {                          // refused (another driver build): linked from source below
                g_pc_refused++;
                char path[600];
                pc_path(path, sizeof path, key);
                unlink(path);
            }
        }
    }
    if (!how) r_LinkProgram(p);
    if (key && !how) pc_store(p, key);
    double e = ms_now();
    if (how) { g_pc_hits++; g_pc_hit_ms += e - t; } else if (key) { g_pc_links++; g_pc_link_ms += e - t; }
    if (g_proglog) {
        char n[32];
        fprintf(stderr, "[glprog] %s p%u %.2f ms at %.3f s %s\n", how ? "cached" : "link", p, e - t, since_first(e),
                thread_name(n, sizeof n));
    }
}
static void w_ProgramBinary(unsigned p, unsigned fmt, const void *b, int len)
{
    double t = ms_now();
    r_ProgramBinary(p, fmt, b, len);
    glrec_note_program_binary(p, b, len);
    int ok = -1;
    if (r_GetProgramiv) r_GetProgramiv(p, GL_LINK_STATUS, &ok);
    double e = ms_now();
    char n[32];
    fprintf(stderr, "[glprog] binary p%u %.2f ms at %.3f s %s fmt 0x%x %d bytes %s h=%016llx\n", p, e - t, since_first(e),
            ok == 1 ? "ok" : ok == 0 ? "failed" : "?", fmt, len, thread_name(n, sizeof n), bin_hash(b, len));
}
static void w_GetProgramBinary(unsigned p, int size, int *len, unsigned *fmt, void *b)
{
    double t = ms_now();
    r_GetProgramBinary(p, size, len, fmt, b);
    glrec_note_get_program_binary(p, len ? *len : size, b);
    double e = ms_now();
    char n[32];
    fprintf(stderr, "[glprog] get-binary p%u %.2f ms at %.3f s %d bytes %s h=%016llx\n", p, e - t, since_first(e),
            len ? *len : size, thread_name(n, sizeof n), bin_hash(b, len ? *len : size));
}

#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#define GL_PROGRAM_BINARY_FORMATS 0x87FF
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
static void proglog_first_frame(void)
{
    g_first_ms = ms_now();
    get_integerv_fn gi = (get_integerv_fn)gl_real("libGLESv2.so.2", "libGLESv2.so", "glGetIntegerv");
    get_string_fn gs = (get_string_fn)gl_real("libGLESv2.so.2", "libGLESv2.so", "glGetString");
    int nf = -1, f[8] = { 0 };
    if (gi) gi(GL_NUM_PROGRAM_BINARY_FORMATS, &nf);
    if (gi && nf > 0 && nf <= 8) gi(GL_PROGRAM_BINARY_FORMATS, f);
    void *(*cur)(void) = (void *(*)(void))gl_real("libEGL.so.1", "libEGL.so", "eglGetCurrentDisplay");
    const char *(*query)(void *, int) = (const char *(*)(void *, int))gl_real("libEGL.so.1", "libEGL.so", "eglQueryString");
    void *dpy = cur ? cur() : NULL;
    const char *ext = dpy && query ? query(dpy, 0x3055) : NULL;   // EGL_EXTENSIONS
    fprintf(stderr, "[glprog] %s / %s: %d program binary format(s)", gs ? (const char *)gs(GL_RENDERER) : "?",
            gs ? (const char *)gs(GL_VERSION) : "?", nf);
    for (int i = 0; i < nf && i < 8; i++) fprintf(stderr, " 0x%x", f[i]);
    fprintf(stderr, "; EGL_ANDROID_blob_cache %s\n", ext ? (strstr(ext, "EGL_ANDROID_blob_cache") ? "yes" : "no") : "?");
}

#define GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS 0x90EB
#define GL_MAX_COMPUTE_WORK_GROUP_SIZE 0x91BF

// the limit reported for a query (slot: invocations, size x, y, z), the driver's when higher;
// logged once per query
static int compute_limit(int slot, int driver, int n)
{
    static const char *what[4] = { "invocations", "size x", "size y", "size z" };
    static int said[4];
    if (n <= driver) return driver;
    if (!said[slot]) {
        said[slot] = 1;
        fprintf(stderr, "[gl] compute work group %s: %d reported (the driver's: %d; the converted kernels run in groups of 128)\n", what[slot], n, driver);
    }
    return n;
}

static void w_GetIntegerv(unsigned pname, int *data)
{
    r_GetIntegerv(pname, data);
    if (!data) return;
    if (pname == GL_NUM_PROGRAM_BINARY_FORMATS && (g_dump || g_override)) *data = 0;
    else if (pname == GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS && g_compute_group > 0)
        *data = compute_limit(0, *data, g_compute_group);
}

static void w_GetIntegeri_v(unsigned pname, unsigned i, int *data)
{
    r_GetIntegeri_v(pname, i, data);
    if (data && pname == GL_MAX_COMPUTE_WORK_GROUP_SIZE && i < 3 && g_compute_group > 0)
        *data = compute_limit(1 + (int)i, *data, i < 2 || g_compute_group < 64 ? g_compute_group : 64);
}

// glGetProgramiv: a split kernel's work group size is the one its marker line names (the converter's
// "// pm_work_group x y z", shaderx glsl::SPLIT_MARK), read from the program's compute shader:
// Unity takes a kernel's thread group size from the linked program, and the game sizes its
// dispatches by it (GetKernelThreadGroupSizes)
#define GL_COMPUTE_WORK_GROUP_SIZE 0x8267
#define GL_SHADER_SOURCE_LENGTH_ 0x8B88
typedef void (*cg_programiv_fn)(unsigned, unsigned, int *);
typedef void (*cg_attached_fn)(unsigned, int, int *, unsigned *);
typedef void (*cg_shader_source_fn)(unsigned, int, int *, char *);
static cg_programiv_fn r_cg_GetProgramiv, r_cg_GetShaderiv;
static cg_attached_fn r_cg_GetAttachedShaders;
static cg_shader_source_fn r_cg_GetShaderSource;

static void w_GetProgramiv(unsigned p, unsigned pname, int *v)
{
    r_cg_GetProgramiv(p, pname, v);
    if (pname != GL_COMPUTE_WORK_GROUP_SIZE || !v) return;
    unsigned sh[4];
    int n = 0;
    r_cg_GetAttachedShaders(p, 4, &n, sh);
    for (int i = 0; i < n && i < 4; i++) {
        int len = 0;
        r_cg_GetShaderiv(sh[i], GL_SHADER_SOURCE_LENGTH_, &len);
        if (len <= 0 || len > (16 << 20)) continue;
        char *src = malloc((size_t)len);
        if (!src) continue;
        r_cg_GetShaderSource(sh[i], len, NULL, src);
        src[len - 1] = 0;
        static const char mark[] = "\n// pm_work_group ";
        const char *m = strstr(src, mark);
        unsigned x, y, z;
        if (m && sscanf(m + sizeof mark - 1, "%u %u %u", &x, &y, &z) == 3 && x && y && z && (uint64_t)x * y * z <= 1024) {
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "[gl] compute program %u: work group %ux%ux%u reported (its kernel's own; it runs in groups of %dx%dx%d)\n",
                        p, x, y, z, v[0], v[1], v[2]);
            }
            v[0] = (int)x; v[1] = (int)y; v[2] = (int)z;
        }
        free(src);
        break;
    }
}

void glhooks_set_compute_group(int n)
{
    const char *e = getenv("UA_COMPUTE_GROUP");
    if (e && *e) n = atoi(e);
    g_compute_group = n > 1024 ? 1024 : n;
}

static unsigned w_SwapInterval(void *dpy, int n)
{
    const char *e = getenv("UA_SWAP_INTERVAL");
    if (e && *e) n = atoi(e);
    return r_SwapInterval ? r_SwapInterval(dpy, n) : 0;
}

void glhooks_set_cache_dir(const char *cache_dir)
{
    if (cache_dir && *cache_dir) snprintf(g_pc_dir, sizeof g_pc_dir, "%s/ua_progcache", cache_dir);
}

// UA_GL_PROGCACHE's count so far (the first frame, then every 10 s of [fps] lines: frames.c calls it)
void glhooks_progcache_report(const char *when)
{
    if (!g_progcache) return;
    fprintf(stderr, "[progcache] %s: %d programs loaded from the host's cache (%.0f ms), %d linked from source (%.0f ms), "
            "%d stored, %d refused\n", when, g_pc_hits, g_pc_hit_ms, g_pc_links, g_pc_link_ms, g_pc_stored, g_pc_refused);
}

// Unity may never call eglSwapInterval: the host calls this once a context is current (swap hook).
void glhooks_first_frame(void)
{
    glhooks_progcache_report("first frame");
    if (g_proglog) proglog_first_frame();
    const char *e = getenv("UA_SWAP_INTERVAL");
    if (!r_SwapInterval || !e || !*e) return;
    void *(*cur)(void) = (void *(*)(void))gl_real("libEGL.so.1", "libEGL.so", "eglGetCurrentDisplay");
    void *dpy = cur ? cur() : NULL;
    unsigned ok = dpy ? r_SwapInterval(dpy, atoi(e)) : 0;
    fprintf(stderr, "[gl] swap interval %d: %s\n", atoi(e), ok ? "set" : "failed");
}

static so_shim g_shims[32];

const so_shim *glhooks_table(void)
{
    int k = 0;
    g_dump = getenv("UA_SHADER_DUMP");
    g_override = getenv("UA_SHADER_OVERRIDE");
    if (g_dump && !*g_dump) g_dump = NULL;
    if (g_override && !*g_override) g_override = NULL;
    if (g_dump || g_override) {
        r_ShaderSource = (shader_source_fn)gl_real("libGLESv2.so.2", "libGLESv2.so", "glShaderSource");
        if (r_ShaderSource) g_shims[k++] = (so_shim){ "glShaderSource", (void *)w_ShaderSource };
        if (g_dump) mkdir(g_dump, 0755);
    }
    if (g_dump || g_override || g_compute_group > 0) {
        r_GetIntegerv = (get_integerv_fn)gl_real("libGLESv2.so.2", "libGLESv2.so", "glGetIntegerv");
        if (r_GetIntegerv) g_shims[k++] = (so_shim){ "glGetIntegerv", (void *)w_GetIntegerv };
    }
    if (g_compute_group > 0) {
        const char *L2 = "libGLESv2.so.2", *A2 = "libGLESv2.so";
        r_GetIntegeri_v = (get_integeri_v_fn)gl_real(L2, A2, "glGetIntegeri_v");
        if (r_GetIntegeri_v) g_shims[k++] = (so_shim){ "glGetIntegeri_v", (void *)w_GetIntegeri_v };
        r_cg_GetProgramiv = (cg_programiv_fn)gl_real(L2, A2, "glGetProgramiv");
        r_cg_GetShaderiv = (cg_programiv_fn)gl_real(L2, A2, "glGetShaderiv");
        r_cg_GetAttachedShaders = (cg_attached_fn)gl_real(L2, A2, "glGetAttachedShaders");
        r_cg_GetShaderSource = (cg_shader_source_fn)gl_real(L2, A2, "glGetShaderSource");
        if (r_cg_GetProgramiv && r_cg_GetShaderiv && r_cg_GetAttachedShaders && r_cg_GetShaderSource)
            g_shims[k++] = (so_shim){ "glGetProgramiv", (void *)w_GetProgramiv };
    }
    g_proglog = getenv("UA_GL_PROGLOG") && atoi(getenv("UA_GL_PROGLOG")) > 0;
    g_progcache = getenv("UA_GL_PROGCACHE") && atoi(getenv("UA_GL_PROGCACHE")) > 0;
    if (g_progcache && (g_dump || g_override)) {
        fprintf(stderr, "[progcache] off: UA_SHADER_DUMP / UA_SHADER_OVERRIDE compile every source\n");
        g_progcache = 0;
    }
    if (g_progcache && !g_pc_dir[0]) { fprintf(stderr, "[progcache] off: no cache folder\n"); g_progcache = 0; }
    const char *L = "libGLESv2.so.2", *A = "libGLESv2.so";
    if (g_progcache) {
        r_ShaderSource = (shader_source_fn)gl_real(L, A, "glShaderSource");
        r_AttachShader = (attach_fn)gl_real(L, A, "glAttachShader");
        r_DetachShader = (attach_fn)gl_real(L, A, "glDetachShader");
        r_BindAttribLocation = (bind_attrib_fn)gl_real(L, A, "glBindAttribLocation");
        r_TransformFeedbackVaryings = (tf_varyings_fn)gl_real(L, A, "glTransformFeedbackVaryings");
        r_ProgramParameteri = (prog_param_fn)gl_real(L, A, "glProgramParameteri");
        r_DeleteShader = (u_fn)gl_real(L, A, "glDeleteShader");
        r_DeleteProgram = (u_fn)gl_real(L, A, "glDeleteProgram");
        r_GetShaderiv = (get_shaderiv_fn)gl_real(L, A, "glGetShaderiv");
        r_GetProgramiv = (get_programiv_fn)gl_real(L, A, "glGetProgramiv");
        r_LinkProgram = (u_fn)gl_real(L, A, "glLinkProgram");
        r_ProgramBinary = (prog_binary_fn)gl_real(L, A, "glProgramBinary");
        r_GetProgramBinary = (get_prog_binary_fn)gl_real(L, A, "glGetProgramBinary");
        r_pc_GetString = (get_string_fn)gl_real(L, A, "glGetString");   // (read at the first link)
        if (!r_ShaderSource || !r_AttachShader || !r_DetachShader || !r_BindAttribLocation || !r_TransformFeedbackVaryings ||
            !r_ProgramParameteri || !r_DeleteShader || !r_DeleteProgram || !r_GetShaderiv || !r_GetProgramiv ||
            !r_LinkProgram || !r_ProgramBinary || !r_GetProgramBinary) {
            fprintf(stderr, "[progcache] off: the driver lacks a GLES 3 program call\n");
            g_progcache = 0;
        } else {
            mkdir(g_pc_dir, 0755);
            if (!g_dump && !g_override) g_shims[k++] = (so_shim){ "glShaderSource", (void *)w_ShaderSource };
            g_shims[k++] = (so_shim){ "glAttachShader", (void *)w_AttachShader };
            g_shims[k++] = (so_shim){ "glDetachShader", (void *)w_DetachShader };
            g_shims[k++] = (so_shim){ "glBindAttribLocation", (void *)w_BindAttribLocation };
            g_shims[k++] = (so_shim){ "glTransformFeedbackVaryings", (void *)w_TransformFeedbackVaryings };
            g_shims[k++] = (so_shim){ "glProgramParameteri", (void *)w_ProgramParameteri };
            g_shims[k++] = (so_shim){ "glDeleteShader", (void *)w_DeleteShader };
            g_shims[k++] = (so_shim){ "glDeleteProgram", (void *)w_DeleteProgram };
            g_shims[k++] = (so_shim){ "glLinkProgram", (void *)w_LinkProgram };
            fprintf(stderr, "[progcache] on: %s\n", g_pc_dir);
        }
    }
    if (g_proglog) {
        r_GetProgramiv = (get_programiv_fn)gl_real(L, A, "glGetProgramiv");
        if ((r_CompileShader = (u_fn)gl_real(L, A, "glCompileShader")))
            g_shims[k++] = (so_shim){ "glCompileShader", (void *)w_CompileShader };
        if (!g_progcache && (r_LinkProgram = (u_fn)gl_real(L, A, "glLinkProgram")))
            g_shims[k++] = (so_shim){ "glLinkProgram", (void *)w_LinkProgram };
        if ((r_ProgramBinary = (prog_binary_fn)gl_real(L, A, "glProgramBinary")))
            g_shims[k++] = (so_shim){ "glProgramBinary", (void *)w_ProgramBinary };
        if ((r_GetProgramBinary = (get_prog_binary_fn)gl_real(L, A, "glGetProgramBinary")))
            g_shims[k++] = (so_shim){ "glGetProgramBinary", (void *)w_GetProgramBinary };
    }
    if (getenv("UA_SWAP_INTERVAL") && *getenv("UA_SWAP_INTERVAL")) {
        r_SwapInterval = (swap_interval_fn)gl_real("libEGL.so.1", "libEGL.so", "eglSwapInterval");
        if (r_SwapInterval) g_shims[k++] = (so_shim){ "eglSwapInterval", (void *)w_SwapInterval };
    }
    g_shims[k] = (so_shim){ NULL, NULL };
    return g_shims;
}
