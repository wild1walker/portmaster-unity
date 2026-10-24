// Shader check: compiles and links GLSL ES programs with the device's own GLES driver, and
// optionally times their fragment shaders, so a converted game's shaders are checked (and their
// GPU cost compared) without starting the game.
//
//   shadercheck <dir> [first [count]] [-b|-B out]  compile + link every <dir>/<nnnn>.vert/.frag pair,
//                                                 and every <dir>/<nnnn>.comp (a compute kernel);
//                                                 -b: also each program's binary size (BIN lines),
//                                                 -B: and the binary itself (<out>/<nnnn>.bin)
//   shadercheck -t <dir> <nnnn>... [-s WxH] [-n N] [-o|-p] [-r R] [-u name=v[,v...]]... [-b|-B out] [-w out]
//                                                 time each program's fragment shader: N full-screen
//                                                 quads (default 720x480, 200) into an RGBA8 texture
//                                                 (-o: opaque, no blending; -p: premultiplied alpha
//                                                 blending, as before 2026-10-23), R rounds over the list
//                                                 (default 1; with more, the median and the range)
//
// <dir> is what `python -m pmunity shaders-export` writes (index.tsv names the shaders). Prints one
// line per failure with the driver's log, then a summary; exit status 1 if any program failed.
// Timing binds every sampler to a texture of its type on a unit of its own (2D, 3D, cube and 2D
// array: RGBA8 noise; shadow samplers: a depth texture compared; integer samplers: no texture, they
// sample zeros; two sampler types on one unit would make every draw fail, and the program time as
// free) and sets every float uniform to 0.5, in uniform blocks
// too (ints stay 0), so it measures the shader's own cost (ALU, texture fetches, bandwidth of one
// RGBA8 target), not the game's data; -u sets one uniform (or block member, or array from its first
// element) to the values given, e.g. a light count that decides a loop's trips. Quads are blended
// additively (ONE, ONE), so each one is shaded in full: with premultiplied alpha (-p) a shader that
// writes alpha 1 makes its quads opaque to the Mali, whose forward pixel kill then drops the
// fragments of the quads under the last ones (such a shader measured less than a constant colour
// costs). Rounds interleave the programs,
// so a GPU clock that moves affects each the same way; the GPU's devfreq clock is printed at the
// start and the end.
// The binary size (glGetProgramBinary) is the compiled code with the driver's tables: a rough,
// static measure of what a rewrite of the source changed. -w writes what each program draws (one
// quad, unblended, into an RGBA32F target: the values the shader computed) for comparing a
// rewrite's picture with the original's bit for bit, on the device's own driver.
// EGL: a pbuffer surface (Mesa: EGL_PLATFORM=surfaceless), else the Mali fbdev window.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9                   // (OpenGL ES 3.1)
#endif
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct fbdev_window { unsigned short width, height; };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

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

static char g_names[10000][160];
static int g_binary;                               // -b: report program binary sizes
static const char *g_bindir;                       // -B <dir>: and write the binaries there
static const char *g_outdir;                       // -w <dir>: each program's picture (32-bit floats)

static void load_index(const char *dir)
{
    char p[1024]; snprintf(p, sizeof p, "%s/index.tsv", dir);
    char *s = slurp(p);
    if (!s) return;
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
        int n = atoi(line);
        char *t = strchr(line, '\t');
        if (n >= 0 && n < 10000 && t) {
            snprintf(g_names[n], sizeof g_names[n], "%s", t + 1);
            for (char *c = g_names[n]; *c; c++) if (*c == '\t') *c = ' ';
        }
    }
    free(s);
}

static GLuint compile(GLenum type, const char *src, const char *what, int n)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = ""; glGetShaderInfoLog(sh, sizeof log, NULL, log);
        printf("FAIL %04d %s compile (%s): %s\n", n, what, g_names[n], log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_program(GLuint pr, int n)
{
    if (g_binary) glProgramParameteri(pr, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(pr);
    GLint ok = 0; glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = ""; glGetProgramInfoLog(pr, sizeof log, NULL, log);
        printf("FAIL %04d link (%s): %s\n", n, g_names[n], log);
        glDeleteProgram(pr);
        return 0;
    }
    if (g_binary) {
        GLint len = 0; glGetProgramiv(pr, GL_PROGRAM_BINARY_LENGTH, &len);
        printf("BIN %04d %d bytes (%s)\n", n, len, g_names[n]);
        if (g_bindir && len > 0) {                  // -B <dir>: the binary itself, <dir>/<nnnn>.bin
            void *b = malloc(len); GLenum fmt = 0; GLsizei got = 0;
            glGetProgramBinary(pr, len, &got, &fmt, b);
            char p[1024]; snprintf(p, sizeof p, "%s/%04d.bin", g_bindir, n);
            FILE *f = fopen(p, "wb");
            if (f) { fwrite(b, 1, got, f); fclose(f); }
            free(b);
        }
    }
    return pr;
}

static GLuint build(const char *dir, int n, int quiet_missing)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%04d.vert", dir, n); char *vs = slurp(p);
    snprintf(p, sizeof p, "%s/%04d.frag", dir, n); char *fs = slurp(p);
    if (!vs || !fs) { if (!quiet_missing) printf("FAIL %04d missing stage files\n", n); free(vs); free(fs); return 0; }
    GLuint v = compile(GL_VERTEX_SHADER, vs, "vertex", n), f = compile(GL_FRAGMENT_SHADER, fs, "fragment", n);
    free(vs); free(fs);
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
    GLuint pr = glCreateProgram();
    glAttachShader(pr, v); glAttachShader(pr, f);
    pr = link_program(pr, n);
    glDeleteShader(v); glDeleteShader(f);
    return pr;
}

// a compute kernel (ComputeShader assets): compiled and linked alone (OpenGL ES 3.1+)
static GLuint build_compute(const char *dir, int n)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%04d.comp", dir, n); char *cs = slurp(p);
    if (!cs) return 0;
    GLuint c = compile(GL_COMPUTE_SHADER, cs, "compute", n);
    free(cs);
    if (!c) return 0;
    GLuint pr = glCreateProgram();
    glAttachShader(pr, c);
    pr = link_program(pr, n);
    glDeleteShader(c);
    return pr;
}

static int egl_up(int w, int h)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, NULL, NULL)) { printf("eglInitialize failed 0x%x\n", eglGetError()); return 0; }
    EGLint attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                      EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg; EGLint nc = 0;
    EGLSurface surf = EGL_NO_SURFACE;
    if (eglChooseConfig(dpy, attr, &cfg, 1, &nc) && nc > 0) {
        EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
        surf = eglCreatePbufferSurface(dpy, cfg, pa);
    }
    if (surf == EGL_NO_SURFACE) {                      // Mali fbdev: the screen's window
        static struct fbdev_window win;
        struct fb_var_screeninfo vi = {0};
        int fd = open("/dev/fb0", O_RDWR);
        if (fd >= 0) { ioctl(fd, FBIOGET_VSCREENINFO, &vi); close(fd); }
        win.width = vi.xres ? vi.xres : w; win.height = vi.yres ? vi.yres : h;
        attr[11] = EGL_WINDOW_BIT;                     // (the EGL_SURFACE_TYPE value)
        if (!eglChooseConfig(dpy, attr, &cfg, 1, &nc) || nc < 1) { printf("eglChooseConfig: none (0x%x)\n", eglGetError()); return 0; }
        surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)&win, NULL);
        if (surf == EGL_NO_SURFACE) { printf("no EGL surface (0x%x)\n", eglGetError()); return 0; }
    }
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) { printf("context failed 0x%x\n", eglGetError()); return 0; }
    printf("GL_RENDERER %s | GL_VERSION %s | GLSL %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION),
           glGetString(GL_SHADING_LANGUAGE_VERSION));
    return 1;
}

// ---- timing inputs: textures of each sampler type, uniforms, uniform blocks

static GLuint g_tex2d, g_tex3d, g_texcube, g_texarray, g_texdepth, g_texdepthcube;

static void make_textures(void)
{
    unsigned char *px = malloc(256 * 256 * 4 * 6);
    for (int i = 0; i < 256 * 256 * 4 * 6; i++) px[i] = (unsigned char)(rand() & 255);
    GLuint t[6]; glGenTextures(6, t);
    g_tex2d = t[0]; g_tex3d = t[1]; g_texcube = t[2]; g_texarray = t[3]; g_texdepth = t[4]; g_texdepthcube = t[5];
    glBindTexture(GL_TEXTURE_2D, g_tex2d);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_3D, g_tex3d);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 32, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_CUBE_MAP, g_texcube);
    for (int f = 0; f < 6; f++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px + f * 64 * 64 * 4);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_texarray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 256, 256, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // shadow maps: 24-bit depth from the noise, compared (linear: the hardware's 2x2 PCF)
    unsigned *d = malloc(256 * 256 * sizeof *d);
    for (int i = 0; i < 256 * 256; i++) d[i] = ((unsigned)px[i * 4] << 24) | ((unsigned)px[i * 4 + 1] << 16);
    glBindTexture(GL_TEXTURE_2D, g_texdepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 256, 256, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, d);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_CUBE_MAP, g_texdepthcube);
    for (int f = 0; f < 6; f++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_DEPTH_COMPONENT24, 64, 64, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, d + f * 64 * 64);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    free(d); free(px);
}

static int components(GLenum type)
{
    switch (type) {
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: case GL_BOOL: return 1;
    case GL_FLOAT_VEC2: case GL_INT_VEC2: case GL_UNSIGNED_INT_VEC2: case GL_BOOL_VEC2: return 2;
    case GL_FLOAT_VEC3: case GL_INT_VEC3: case GL_UNSIGNED_INT_VEC3: case GL_BOOL_VEC3: return 3;
    case GL_FLOAT_VEC4: case GL_INT_VEC4: case GL_UNSIGNED_INT_VEC4: case GL_BOOL_VEC4: return 4;
    case GL_FLOAT_MAT2: return 4;
    case GL_FLOAT_MAT3: return 9;
    case GL_FLOAT_MAT4: return 16;
    default: return 0;
    }
}

static int is_float(GLenum type)
{
    return type == GL_FLOAT || type == GL_FLOAT_VEC2 || type == GL_FLOAT_VEC3 || type == GL_FLOAT_VEC4 ||
           type == GL_FLOAT_MAT2 || type == GL_FLOAT_MAT3 || type == GL_FLOAT_MAT4;
}

struct uset { char name[128]; int n; float v[256]; };
static struct uset g_uset[64];
static int g_nuset;

static GLuint g_ubo[16];
static unsigned char *g_ubodata[16];
static GLint g_ubosize[16];

// a value for a uniform-block member at <offset>: the array element <e>, column <c> (matrices), component <k>
static void block_put(int b, GLint off, GLint astride, GLint mstride, GLenum type, int e, int k, float v)
{
    int cols = type == GL_FLOAT_MAT4 ? 4 : type == GL_FLOAT_MAT3 ? 3 : type == GL_FLOAT_MAT2 ? 2 : 1;
    int rows = components(type) / cols;
    GLint at = off + e * astride + (cols > 1 ? (k / rows) * mstride + (k % rows) * 4 : k * 4);
    if (at < 0 || at + 4 > g_ubosize[b]) return;
    if (is_float(type)) memcpy(g_ubodata[b] + at, &v, 4);
    else { int i = (int)v; memcpy(g_ubodata[b] + at, &i, 4); }
}

// every active float uniform 0.5 (vectors, arrays and block members too), every sampler a texture
// of its type; then the -u values
static void default_uniforms(GLuint pr)
{
    GLint nb = 0; glGetProgramiv(pr, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
    if (nb > 16) nb = 16;
    for (GLint b = 0; b < nb; b++) {
        GLint size = 0; glGetActiveUniformBlockiv(pr, b, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        if (!g_ubo[b]) glGenBuffers(1, &g_ubo[b]);
        free(g_ubodata[b]);
        g_ubodata[b] = calloc(1, size + 16); g_ubosize[b] = size;
        glUniformBlockBinding(pr, b, b);
    }
    GLint nu = 0; glGetProgramiv(pr, GL_ACTIVE_UNIFORMS, &nu);
    int unit = 0;
    static GLfloat half[4 * 16 * 64];
    if (!half[0]) for (size_t k = 0; k < sizeof half / sizeof half[0]; k++) half[k] = 0.5f;
    for (GLint i = 0; i < nu; i++) {
        char name[256]; GLint size; GLenum type;
        glGetActiveUniform(pr, i, sizeof name, NULL, &size, &type, name);
        GLuint ui = i;
        GLint block = -1, off = 0, astride = 0, mstride = 0;
        glGetActiveUniformsiv(pr, 1, &ui, GL_UNIFORM_BLOCK_INDEX, &block);
        if (block >= 0) {
            if (block >= nb) continue;
            glGetActiveUniformsiv(pr, 1, &ui, GL_UNIFORM_OFFSET, &off);
            glGetActiveUniformsiv(pr, 1, &ui, GL_UNIFORM_ARRAY_STRIDE, &astride);
            glGetActiveUniformsiv(pr, 1, &ui, GL_UNIFORM_MATRIX_STRIDE, &mstride);
            if (is_float(type))
                for (int e = 0; e < size; e++)
                    for (int k = 0; k < components(type); k++) block_put(block, off, astride, mstride, type, e, k, 0.5f);
            continue;
        }
        GLint loc = glGetUniformLocation(pr, name);
        if (loc < 0) continue;
        if (size > 64) size = 64;
        GLenum target = 0; GLuint tex = 0;
        switch (type) {
        case GL_FLOAT: glUniform1fv(loc, size, half); break;
        case GL_FLOAT_VEC2: glUniform2fv(loc, size, half); break;
        case GL_FLOAT_VEC3: glUniform3fv(loc, size, half); break;
        case GL_FLOAT_VEC4: glUniform4fv(loc, size, half); break;
        case GL_FLOAT_MAT4: glUniformMatrix4fv(loc, size, GL_FALSE, half); break;
        case GL_SAMPLER_2D: target = GL_TEXTURE_2D; tex = g_tex2d; break;
        case GL_SAMPLER_3D: target = GL_TEXTURE_3D; tex = g_tex3d; break;
        case GL_SAMPLER_CUBE: target = GL_TEXTURE_CUBE_MAP; tex = g_texcube; break;
        case GL_SAMPLER_2D_ARRAY: target = GL_TEXTURE_2D_ARRAY; tex = g_texarray; break;
        case GL_SAMPLER_2D_SHADOW: target = GL_TEXTURE_2D; tex = g_texdepth; break;
        case GL_SAMPLER_CUBE_SHADOW: target = GL_TEXTURE_CUBE_MAP; tex = g_texdepthcube; break;
        // the other sampler types get a unit of their own with no texture (they sample zeros): left
        // on unit 0 beside a 2D texture they would make every draw fail (two sampler types on one
        // unit), and the program would time as free
        case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_INT_SAMPLER_2D: case GL_INT_SAMPLER_3D: case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_2D_ARRAY: case GL_UNSIGNED_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE: case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
            glUniform1i(loc, unit++); break;
        default: break;
        }
        if (target) {
            glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(target, tex);
            glUniform1i(loc, unit++);
        }
    }
    // -u name=v,...: a plain uniform through its location, a block member into its block's buffer
    for (int s = 0; s < g_nuset; s++) {
        const struct uset *u = &g_uset[s];
        char alt[140]; snprintf(alt, sizeof alt, "%s[0]", u->name);
        const char *names[2] = { u->name, alt };
        GLuint idx[2] = { GL_INVALID_INDEX, GL_INVALID_INDEX };
        glGetUniformIndices(pr, 2, names, idx);
        GLuint ui = idx[0] != GL_INVALID_INDEX ? idx[0] : idx[1];
        if (ui == GL_INVALID_INDEX) continue;
        char name[256]; GLint size; GLenum type;
        glGetActiveUniform(pr, ui, sizeof name, NULL, &size, &type, name);
        int nc = components(type);
        if (!nc) continue;
        GLint block = -1, off = 0, astride = 0, mstride = 0;
        glGetActiveUniformsiv(pr, 1, &ui, GL_UNIFORM_BLOCK_INDEX, &block);
        if (block >= 0) {
            if (block >= nb) continue;
            glGetActiveUniformsiv(pr, 1, &ui, GL_UNIFORM_OFFSET, &off);
            glGetActiveUniformsiv(pr, 1, &ui, GL_UNIFORM_ARRAY_STRIDE, &astride);
            glGetActiveUniformsiv(pr, 1, &ui, GL_UNIFORM_MATRIX_STRIDE, &mstride);
            for (int j = 0; j < u->n && j / nc < size; j++) block_put(block, off, astride, mstride, type, j / nc, j % nc, u->v[j]);
            continue;
        }
        GLint loc = glGetUniformLocation(pr, name);
        if (loc < 0) continue;
        int cnt = u->n / nc; if (cnt < 1) continue; if (cnt > size) cnt = size;
        GLint iv[256]; for (int j = 0; j < u->n; j++) iv[j] = (GLint)u->v[j];
        switch (type) {
        case GL_FLOAT: glUniform1fv(loc, cnt, u->v); break;
        case GL_FLOAT_VEC2: glUniform2fv(loc, cnt, u->v); break;
        case GL_FLOAT_VEC3: glUniform3fv(loc, cnt, u->v); break;
        case GL_FLOAT_VEC4: glUniform4fv(loc, cnt, u->v); break;
        case GL_FLOAT_MAT4: glUniformMatrix4fv(loc, cnt, GL_FALSE, u->v); break;
        case GL_INT: case GL_BOOL: glUniform1iv(loc, cnt, iv); break;
        case GL_INT_VEC2: glUniform2iv(loc, cnt, iv); break;
        case GL_INT_VEC4: glUniform4iv(loc, cnt, iv); break;
        case GL_UNSIGNED_INT: { GLuint uv[256]; for (int j = 0; j < u->n; j++) uv[j] = (GLuint)u->v[j]; glUniform1uiv(loc, cnt, uv); } break;
        default: break;
        }
    }
    for (GLint b = 0; b < nb; b++) {
        glBindBuffer(GL_UNIFORM_BUFFER, g_ubo[b]);
        glBufferData(GL_UNIFORM_BUFFER, g_ubosize[b], g_ubodata[b], GL_STATIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, b, g_ubo[b]);
    }
}

static void gpu_clock(char *out, size_t n)
{
    glob_t g; out[0] = 0;
    if (glob("/sys/class/devfreq/*/cur_freq", 0, NULL, &g) != 0) { snprintf(out, n, "?"); return; }
    for (size_t i = 0; i < g.gl_pathc; i++) {           // (sysfs: no size to seek to, read a line)
        FILE *f = fopen(g.gl_pathv[i], "r");
        char s[64];
        if (f && fgets(s, sizeof s, f)) { size_t l = strlen(out); snprintf(out + l, n - l, "%s%ld", l ? "," : "", atol(s) / 1000000); }
        if (f) fclose(f);
    }
    globfree(&g);
}

static int cmp_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
    int timing = argc > 1 && !strcmp(argv[1], "-t");
    if (argc < 2 + timing) {
        fprintf(stderr, "usage: shadercheck <dir> [first [count]] [-b|-B out] | shadercheck -t <dir> <nnnn>... [-s WxH] [-n N] [-o|-p] [-r R] [-u name=v,...] [-b|-B out] [-w out]\n");
        return 2;
    }
    const char *dir = argv[1 + timing];
    int w = 720, h = 480, reps = 200, blend = 1, rounds = 1;     // blend: 0 off, 1 additive, 2 premultiplied
    int progs[256], np = 0, pos[2], npos = 0;
    // the model/view/projection matrices of 0.5 would squash the quad: identity for Unity's (plain
    // uniforms or members of UnityPerDraw / UnityPerFrame), before the -u values (which may replace them)
    const char *mats[] = { "hlslcc_mtx4x4unity_ObjectToWorld", "hlslcc_mtx4x4unity_MatrixVP", "hlslcc_mtx4x4unity_MatrixV",
                           "hlslcc_mtx4x4glstate_matrix_projection", "hlslcc_mtx4x4unity_MatrixMVP" };
    for (size_t k = 0; k < sizeof mats / sizeof mats[0]; k++) {
        struct uset *u = &g_uset[g_nuset++];
        snprintf(u->name, sizeof u->name, "%s", mats[k]);
        u->n = 16;
        for (int j = 0; j < 16; j++) u->v[j] = j % 5 == 0;
    }
    for (int i = 2 + timing; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) blend = 0;
        else if (!strcmp(argv[i], "-p")) blend = 2;
        else if (!strcmp(argv[i], "-b")) g_binary = 1;
        else if (!strcmp(argv[i], "-B") && i + 1 < argc) { g_binary = 1; g_bindir = argv[++i]; }
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) g_outdir = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-u") && i + 1 < argc && g_nuset < 64) {
            struct uset *u = &g_uset[g_nuset];
            const char *a = argv[++i], *eq = strchr(a, '=');
            if (!eq || eq - a >= (int)sizeof u->name) { fprintf(stderr, "-u name=v[,v...]\n"); return 2; }
            memcpy(u->name, a, eq - a); u->name[eq - a] = 0;
            for (char *p = (char *)eq + 1; *p && u->n < 256;) { u->v[u->n++] = strtof(p, &p); if (*p == ',') p++; else break; }
            g_nuset++;
        } else if (timing && np < 256) progs[np++] = atoi(argv[i]);
        else if (!timing && npos < 2) pos[npos++] = atoi(argv[i]);
    }
    if (rounds < 1) rounds = 1;
    load_index(dir);
    if (!egl_up(64, 64)) return 1;
    if (!timing) {
        int first = npos > 0 ? pos[0] : 0, count = npos > 1 ? pos[1] : 10000;
        int ok = 0, bad = 0, cok = 0, cbad = 0;
        double t0 = now();
        for (int n = first; n < first + count && n < 10000; n++) {
            char p[1024], q[1024];
            snprintf(p, sizeof p, "%s/%04d.vert", dir, n);
            snprintf(q, sizeof q, "%s/%04d.comp", dir, n);
            if (access(q, R_OK) == 0) {
                GLuint pr = build_compute(dir, n);
                if (pr) { cok++; glDeleteProgram(pr); } else cbad++;
                continue;
            }
            if (access(p, R_OK) != 0) { if (n > first && !g_names[n][0]) break; continue; }
            GLuint pr = build(dir, n, 0);
            if (pr) { ok++; glDeleteProgram(pr); } else bad++;
        }
        printf("%d programs linked, %d failed", ok, bad);
        if (cok || cbad) printf("; %d compute kernels linked, %d failed", cok, cbad);
        printf(" (%.1f s)\n", now() - t0);
        return bad + cbad != 0;
    }
    // timing: an RGBA8 target of w x h, textures of each sampler type, one full-screen quad per draw
    make_textures();
    GLuint rt, fbo;
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    glViewport(0, 0, w, h);
    static const GLfloat quad[] = { -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1 };
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    GLuint built[256];
    for (int p = 0; p < np; p++) built[p] = build(dir, progs[p], 0);
    if (g_outdir) {
        // -w <dir>: each program's picture of one quad, unblended, into a 32-bit float target (the
        // values as the shader computed them, not rounded to 8 bits): <dir>/<nnnn>.f32, w x h x 4 floats
        GLuint ft, ffbo;
        glGenTextures(1, &ft); glBindTexture(GL_TEXTURE_2D, ft);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
        glGenFramebuffers(1, &ffbo); glBindFramebuffer(GL_FRAMEBUFFER, ffbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ft, 0);
        int fl = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!fl) { glBindFramebuffer(GL_FRAMEBUFFER, fbo); printf("no RGBA32F target: pictures in RGBA8\n"); }
        float *px = malloc((size_t)w * h * 4 * sizeof *px);
        for (int p = 0; p < np; p++) {
            GLuint pr = built[p];
            if (!pr) continue;
            glUseProgram(pr);
            GLint na = 0; glGetProgramiv(pr, GL_ACTIVE_ATTRIBUTES, &na);
            for (GLint a = 0; a < na; a++) {
                char an[128]; GLint sz; GLenum ty; glGetActiveAttrib(pr, a, sizeof an, NULL, &sz, &ty, an);
                GLint loc = glGetAttribLocation(pr, an);
                if (loc >= 0) { glEnableVertexAttribArray(loc); glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0); }
            }
            default_uniforms(pr);
            glDisable(GL_BLEND);
            glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            char path[1024]; snprintf(path, sizeof path, "%s/%04d.%s", g_outdir, progs[p], fl ? "f32" : "rgba8");
            FILE *f = fopen(path, "wb");
            if (fl) { glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, px); if (f) fwrite(px, sizeof *px, (size_t)w * h * 4, f); }
            else { glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px); if (f) fwrite(px, 1, (size_t)w * h * 4, f); }
            if (f) fclose(f);
            for (GLint a = 0; a < na; a++) glDisableVertexAttribArray(a);
        }
        free(px);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    double *times = calloc((size_t)np * rounds, sizeof *times);
    char clk[64]; gpu_clock(clk, sizeof clk);
    printf("GPU clock at the start: %s MHz\n", clk);
    // a second of full-screen quads first: the governor starts the GPU at its lowest clock and
    // takes a while to raise it, which short programs' rounds would otherwise measure
    for (int p = 0; p < np; p++) {
        if (!built[p]) continue;
        glUseProgram(built[p]);
        GLint na = 0; glGetProgramiv(built[p], GL_ACTIVE_ATTRIBUTES, &na);
        for (GLint a = 0; a < na; a++) {
            char an[128]; GLint sz; GLenum ty; glGetActiveAttrib(built[p], a, sizeof an, NULL, &sz, &ty, an);
            GLint loc = glGetAttribLocation(built[p], an);
            if (loc >= 0) { glEnableVertexAttribArray(loc); glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0); }
        }
        default_uniforms(built[p]);
        if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); }
        for (double t1 = now() + 1.0; now() < t1;) { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glFinish(); }
        for (GLint a = 0; a < na; a++) glDisableVertexAttribArray(a);
        break;
    }
    gpu_clock(clk, sizeof clk);
    printf("GPU clock after a second of warm-up: %s MHz\n", clk);
    for (int r = 0; r < rounds; r++)
        for (int p = 0; p < np; p++) {
            GLuint pr = built[p];
            if (!pr) continue;
            // every vertex attribute reads the quad's position (texcoords then span the quad too)
            glUseProgram(pr);
            GLint na = 0; glGetProgramiv(pr, GL_ACTIVE_ATTRIBUTES, &na);
            for (GLint a = 0; a < na; a++) {
                char an[128]; GLint sz; GLenum ty; glGetActiveAttrib(pr, a, sizeof an, NULL, &sz, &ty, an);
                GLint loc = glGetAttribLocation(pr, an);
                if (loc >= 0) { glEnableVertexAttribArray(loc); glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0); }
            }
            default_uniforms(pr);
            // blended: an opaque full-screen quad would let the GPU drop the hidden ones
            if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, blend == 2 ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE); }
            else glDisable(GL_BLEND);
            for (int k = 0; k < 10; k++) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFinish();
            double t0 = now();
            for (int k = 0; k < reps; k++) {
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                if ((k & 15) == 15) glFlush();
            }
            unsigned char probe[4]; glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, probe);
            times[(size_t)p * rounds + r] = now() - t0;
            for (GLint a = 0; a < na; a++) glDisableVertexAttribArray(a);
        }
    for (int p = 0; p < np; p++) {
        if (!built[p]) continue;
        double *t = times + (size_t)p * rounds;
        qsort(t, rounds, sizeof *t, cmp_double);
        double med = rounds & 1 ? t[rounds / 2] : (t[rounds / 2 - 1] + t[rounds / 2]) / 2;
        printf("TIME %04d %8.3f ms/quad %6.2f ns/pixel", progs[p], med * 1000 / reps, med * 1e9 / reps / (w * h));
        if (rounds > 1) printf("  (median of %d rounds, %.3f - %.3f)", rounds, t[0] * 1000 / reps, t[rounds - 1] * 1000 / reps);
        printf("  (%s)\n", g_names[progs[p]]);
        glDeleteProgram(built[p]);
    }
    gpu_clock(clk, sizeof clk);
    printf("GPU clock at the end: %s MHz\n", clk);
    return 0;
}
