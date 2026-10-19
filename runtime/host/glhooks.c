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
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../loader/so_loader.h"
#include "glhooks.h"

typedef void (*shader_source_fn)(unsigned, int, const char *const *, const int *);
typedef unsigned (*swap_interval_fn)(void *, int);
typedef void (*get_integerv_fn)(unsigned, int *);

static shader_source_fn r_ShaderSource;
static swap_interval_fn r_SwapInterval;
static get_integerv_fn r_GetIntegerv;
static const char *g_dump, *g_override;

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

#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
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
    const char *e = getenv("UA_SWAP_INTERVAL");
    if (!r_SwapInterval || !e || !*e) return;
    void *(*cur)(void) = (void *(*)(void))gl_real("libEGL.so.1", "libEGL.so", "eglGetCurrentDisplay");
    void *dpy = cur ? cur() : NULL;
    unsigned ok = dpy ? r_SwapInterval(dpy, atoi(e)) : 0;
    fprintf(stderr, "[gl] swap interval %d: %s\n", atoi(e), ok ? "set" : "failed");
}

static so_shim g_shims[4];

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
    if (getenv("UA_SWAP_INTERVAL") && *getenv("UA_SWAP_INTERVAL")) {
        r_SwapInterval = (swap_interval_fn)gl_real("libEGL.so.1", "libEGL.so", "eglSwapInterval");
        if (r_SwapInterval) g_shims[k++] = (so_shim){ "eglSwapInterval", (void *)w_SwapInterval };
    }
    g_shims[k] = (so_shim){ NULL, NULL };
    return g_shims;
}
