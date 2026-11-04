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
//   UA_SWAP_INTERVAL=<n>      eglSwapInterval value used whatever Unity asks for (0: no vsync, so
//                             frame times show the real CPU/GPU cost instead of vsync steps)
//   UA_GL_PROGLOG=1           logs how long each shader compile, program link and program binary
//                             load (glProgramBinary: Unity's program binary cache) takes, on which
//                             thread: "[glprog] link p<n> <ms> ms <thread>"; "binary p<n> <ms> ms ok|failed";
//                             "get-binary"; "compile s<n>"; at the first frame the driver's program
//                             binary formats and its EGL blob cache extension (a measurement: nothing
//                             changes; tools/analysis/hitchsum.py --glprog sums the lines)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../loader/so_loader.h"
#include "glhooks.h"
#include "glrec.h"

typedef void (*shader_source_fn)(unsigned, int, const char *const *, const int *);
typedef unsigned (*swap_interval_fn)(void *, int);
typedef void (*get_integerv_fn)(unsigned, int *);

static shader_source_fn r_ShaderSource;
static swap_interval_fn r_SwapInterval;
static get_integerv_fn r_GetIntegerv;
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

static void w_ShaderSource(unsigned shader, int count, const char *const *string, const int *length)
{
    if (!r_ShaderSource) return;
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
static void w_CompileShader(unsigned s)
{
    double t = ms_now();
    r_CompileShader(s);
    double e = ms_now();
    char n[32];
    fprintf(stderr, "[glprog] compile s%u %.2f ms at %.3f s %s\n", s, e - t, since_first(e), thread_name(n, sizeof n));
}
static void w_LinkProgram(unsigned p)
{
    double t = ms_now();
    r_LinkProgram(p);
    double e = ms_now();
    char n[32];
    fprintf(stderr, "[glprog] link p%u %.2f ms at %.3f s %s\n", p, e - t, since_first(e), thread_name(n, sizeof n));
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
    fprintf(stderr, "[glprog] binary p%u %.2f ms at %.3f s %s fmt 0x%x %d bytes %s\n", p, e - t, since_first(e),
            ok == 1 ? "ok" : ok == 0 ? "failed" : "?", fmt, len, thread_name(n, sizeof n));
}
static void w_GetProgramBinary(unsigned p, int size, int *len, unsigned *fmt, void *b)
{
    double t = ms_now();
    r_GetProgramBinary(p, size, len, fmt, b);
    glrec_note_get_program_binary(p, len ? *len : size, b);
    double e = ms_now();
    char n[32];
    fprintf(stderr, "[glprog] get-binary p%u %.2f ms at %.3f s %d bytes %s\n", p, e - t, since_first(e),
            len ? *len : size, thread_name(n, sizeof n));
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

static void w_GetIntegerv(unsigned pname, int *data)
{
    r_GetIntegerv(pname, data);
    if (pname == GL_NUM_PROGRAM_BINARY_FORMATS && data) *data = 0;
}

static unsigned w_SwapInterval(void *dpy, int n)
{
    const char *e = getenv("UA_SWAP_INTERVAL");
    if (e && *e) n = atoi(e);
    return r_SwapInterval ? r_SwapInterval(dpy, n) : 0;
}

// Unity may never call eglSwapInterval: the host calls this once a context is current (swap hook).
void glhooks_first_frame(void)
{
    if (g_proglog) proglog_first_frame();
    const char *e = getenv("UA_SWAP_INTERVAL");
    if (!r_SwapInterval || !e || !*e) return;
    void *(*cur)(void) = (void *(*)(void))gl_real("libEGL.so.1", "libEGL.so", "eglGetCurrentDisplay");
    void *dpy = cur ? cur() : NULL;
    unsigned ok = dpy ? r_SwapInterval(dpy, atoi(e)) : 0;
    fprintf(stderr, "[gl] swap interval %d: %s\n", atoi(e), ok ? "set" : "failed");
}

static so_shim g_shims[10];

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
        r_GetIntegerv = (get_integerv_fn)gl_real("libGLESv2.so.2", "libGLESv2.so", "glGetIntegerv");
        if (r_GetIntegerv) g_shims[k++] = (so_shim){ "glGetIntegerv", (void *)w_GetIntegerv };
        if (g_dump) mkdir(g_dump, 0755);
    }
    g_proglog = getenv("UA_GL_PROGLOG") && atoi(getenv("UA_GL_PROGLOG")) > 0;
    if (g_proglog) {
        const char *L = "libGLESv2.so.2", *A = "libGLESv2.so";
        r_GetProgramiv = (get_programiv_fn)gl_real(L, A, "glGetProgramiv");
        if ((r_CompileShader = (u_fn)gl_real(L, A, "glCompileShader")))
            g_shims[k++] = (so_shim){ "glCompileShader", (void *)w_CompileShader };
        if ((r_LinkProgram = (u_fn)gl_real(L, A, "glLinkProgram")))
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
