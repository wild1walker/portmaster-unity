// Compute check: runs two GLSL ES 3.10 compute shaders on the same inputs and compares what they
// write, so a translated kernel is checked against a reference one (Unity's own GLES3 kernel of the
// same compute shader, or another translation) without the game.
//
//   computecheck <a.comp> <b.comp> [-g X,Y,Z] [-w WORDS] [-u MAX] [-s SEED] [-r RUNS]
//   computecheck -x <a.comp> -g X,Y,Z s<binding>=<file>... u<binding>=<file>...
//
// -x runs one kernel on given inputs: each storage (s) or uniform (u) block's buffer is the
// file's bytes, and every storage buffer is written back to <file>.out after the dispatch (the
// same files a reference run of the kernel gets, e.g. the original Metal kernel's).
//
// Every storage block and uniform block either program declares gets a buffer at its binding,
// filled from the seed: storage buffers with WORDS (default 65536) random 32-bit words, uniform
// blocks (their size from the driver) with random words below MAX (default 64: counts, offsets
// and indices stay small; as floats they are tiny). Each program is dispatched X,Y,Z groups
// (default 4,1,1) on fresh copies, and every storage buffer is read back and compared word for
// word. RUNS (default 3) seeds are tried. Exit status 0 when all agree, 1 when a word differs or
// a program does not build, 2 for a usage or setup error.
//
// EGL as shadercheck: a pbuffer (Mesa: EGL_PLATFORM=surfaceless) on an OpenGL ES 3.1 context.
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BINDINGS 64

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    if (fread(s, 1, n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    s[n] = 0; fclose(f);
    return s;
}

static int egl_up(void)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "computecheck: no EGL display\n"); return 0; }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1) { fprintf(stderr, "computecheck: no EGL config\n"); return 0; }
    const EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pa);
    const EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "computecheck: no OpenGL ES 3.1 context\n"); return 0; }
    return 1;
}

static GLuint build(const char *path)
{
    char *src = slurp(path);
    if (!src) { fprintf(stderr, "computecheck: cannot read %s\n", path); return 0; }
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    const char *s = src;
    glShaderSource(sh, 1, &s, NULL);
    glCompileShader(sh);
    free(src);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096] = ""; glGetShaderInfoLog(sh, sizeof log, NULL, log); printf("FAIL %s compile: %s\n", path, log); return 0; }
    GLuint pr = glCreateProgram();
    glAttachShader(pr, sh); glLinkProgram(pr); glDeleteShader(sh);
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096] = ""; glGetProgramInfoLog(pr, sizeof log, NULL, log); printf("FAIL %s link: %s\n", path, log); return 0; }
    return pr;
}

// the bindings a program's blocks use: ssbo[b] / ubo[b] set to 1, ubo_size[b] its data size
static void blocks(GLuint pr, int *ssbo, int *ubo, GLint *ubo_size)
{
    GLint n = 0;
    glGetProgramInterfaceiv(pr, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &n);
    for (GLint i = 0; i < n; i++) {
        GLenum p = GL_BUFFER_BINDING; GLint b = -1;
        glGetProgramResourceiv(pr, GL_SHADER_STORAGE_BLOCK, i, 1, &p, 1, NULL, &b);
        if (b >= 0 && b < MAX_BINDINGS) ssbo[b] = 1;
    }
    glGetProgramInterfaceiv(pr, GL_UNIFORM_BLOCK, GL_ACTIVE_RESOURCES, &n);
    for (GLint i = 0; i < n; i++) {
        GLenum p[2] = { GL_BUFFER_BINDING, GL_BUFFER_DATA_SIZE }; GLint v[2] = { -1, 0 };
        glGetProgramResourceiv(pr, GL_UNIFORM_BLOCK, i, 2, p, 2, NULL, v);
        if (v[0] >= 0 && v[0] < MAX_BINDINGS) { ubo[v[0]] = 1; if (v[1] > ubo_size[v[0]]) ubo_size[v[0]] = v[1]; }
    }
}

static uint32_t rng;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// one program on the inputs; out[b] gets each storage buffer's words after the dispatch
static void run(GLuint pr, const int *ssbo, const int *ubo, const GLint *ubo_size, uint32_t **in_s, uint32_t **in_u,
                int words, const GLuint *groups, uint32_t **out)
{
    GLuint bufs[2 * MAX_BINDINGS] = { 0 };
    glGenBuffers(2 * MAX_BINDINGS, bufs);
    for (int b = 0; b < MAX_BINDINGS; b++) {
        if (ssbo[b]) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, bufs[b]);
            glBufferData(GL_SHADER_STORAGE_BUFFER, words * 4, in_s[b], GL_DYNAMIC_COPY);
        }
        if (ubo[b]) {
            glBindBufferBase(GL_UNIFORM_BUFFER, b, bufs[MAX_BINDINGS + b]);
            glBufferData(GL_UNIFORM_BUFFER, ubo_size[b], in_u[b], GL_DYNAMIC_DRAW);
        }
    }
    glUseProgram(pr);
    glDispatchCompute(groups[0], groups[1], groups[2]);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    for (int b = 0; b < MAX_BINDINGS; b++) {
        if (!ssbo[b]) continue;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[b]);
        void *p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, words * 4, GL_MAP_READ_BIT);
        memcpy(out[b], p, words * 4);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    glDeleteBuffers(2 * MAX_BINDINGS, bufs);
}

static int run_files(int argc, char **argv)
{
    GLuint groups[3] = { 1, 1, 1 };
    if (!egl_up()) return 2;
    GLuint pr = build(argv[2]);
    if (!pr) return 1;
    GLuint bufs[MAX_BINDINGS * 2] = { 0 };
    const char *out[MAX_BINDINGS] = { 0 };
    long size[MAX_BINDINGS] = { 0 };
    glGenBuffers(2 * MAX_BINDINGS, bufs);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 1 < argc) { sscanf(argv[++i], "%u,%u,%u", &groups[0], &groups[1], &groups[2]); continue; }
        char kind = argv[i][0]; int b = atoi(argv[i] + 1); char *eq = strchr(argv[i], '=');
        if ((kind != 's' && kind != 'u') || !eq || b < 0 || b >= MAX_BINDINGS) { fprintf(stderr, "computecheck: %s is not s<binding>=<file> or u<binding>=<file>\n", argv[i]); return 2; }
        FILE *f = fopen(eq + 1, "rb");
        if (!f) { fprintf(stderr, "computecheck: cannot read %s\n", eq + 1); return 2; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        void *d = malloc(n ? n : 1);
        if (fread(d, 1, n, f) != (size_t)n) { fclose(f); return 2; }
        fclose(f);
        GLenum t = kind == 's' ? GL_SHADER_STORAGE_BUFFER : GL_UNIFORM_BUFFER;
        glBindBufferBase(t, b, bufs[(kind == 's' ? 0 : MAX_BINDINGS) + b]);
        glBufferData(t, n, d, GL_DYNAMIC_COPY);
        free(d);
        if (kind == 's') { out[b] = eq + 1; size[b] = n; }
    }
    glUseProgram(pr);
    glDispatchCompute(groups[0], groups[1], groups[2]);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    for (int b = 0; b < MAX_BINDINGS; b++) {
        if (!out[b]) continue;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, bufs[b]);
        void *p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, size[b], GL_MAP_READ_BIT);
        char path[1024]; snprintf(path, sizeof path, "%s.out", out[b]);
        FILE *f = fopen(path, "wb");
        if (!f || !p) { fprintf(stderr, "computecheck: cannot write %s\n", path); return 2; }
        fwrite(p, 1, size[b], f); fclose(f);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "-x")) return run_files(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: computecheck <a.comp> <b.comp> [-g X,Y,Z] [-w WORDS] [-u MAX] [-s SEED] [-r RUNS]\n");
        return 2;
    }
    GLuint groups[3] = { 4, 1, 1 };
    int words = 65536, runs = 3; uint32_t umax = 64, seed = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 1 < argc) sscanf(argv[++i], "%u,%u,%u", &groups[0], &groups[1], &groups[2]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) words = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) umax = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) runs = atoi(argv[++i]);
        else { fprintf(stderr, "computecheck: unknown option %s\n", argv[i]); return 2; }
    }
    if (!egl_up()) return 2;
    GLuint pa = build(argv[1]), pb = build(argv[2]);
    if (!pa || !pb) return 1;
    int ssbo[MAX_BINDINGS] = { 0 }, ubo[MAX_BINDINGS] = { 0 };
    GLint ubo_size[MAX_BINDINGS] = { 0 };
    blocks(pa, ssbo, ubo, ubo_size);
    blocks(pb, ssbo, ubo, ubo_size);
    uint32_t *in_s[MAX_BINDINGS] = { 0 }, *in_u[MAX_BINDINGS] = { 0 }, *oa[MAX_BINDINGS] = { 0 }, *ob[MAX_BINDINGS] = { 0 };
    for (int b = 0; b < MAX_BINDINGS; b++) {
        in_s[b] = calloc(words, 4); oa[b] = calloc(words, 4); ob[b] = calloc(words, 4);
        in_u[b] = calloc(ubo_size[b] / 4 + 1, 4);
    }
    long bad = 0, checked = 0;
    for (int r = 0; r < runs; r++) {
        rng = seed + r * 7919u; if (!rng) rng = 1;
        for (int b = 0; b < MAX_BINDINGS; b++) {
            for (int w = 0; ssbo[b] && w < words; w++) in_s[b][w] = next();
            for (int w = 0; ubo[b] && w < ubo_size[b] / 4; w++) in_u[b][w] = umax ? next() % umax : 0;
        }
        run(pa, ssbo, ubo, ubo_size, in_s, in_u, words, groups, oa);
        run(pb, ssbo, ubo, ubo_size, in_s, in_u, words, groups, ob);
        for (int b = 0; b < MAX_BINDINGS; b++) {
            if (!ssbo[b]) continue;
            long diff = 0, first = -1, changed = 0;
            for (int w = 0; w < words; w++) {
                if (oa[b][w] != in_s[b][w]) changed++;
                if (oa[b][w] != ob[b][w]) { if (first < 0) first = w; diff++; }
            }
            checked += words;
            if (diff) {
                printf("DIFF run %d binding %d: %ld of %d words differ (first at %ld: %08x vs %08x)\n", r, b, diff, words,
                       first, oa[b][first], ob[b][first]);
                bad += diff;
            } else if (r == 0) {
                printf("binding %d: the same (%ld words written)\n", b, changed);
            }
        }
    }
    printf("%s: %ld words compared, %ld differ\n", bad ? "DIFFERENT" : "same", checked, bad);
    return bad != 0;
}
