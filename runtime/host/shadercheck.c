// Shader check: compiles and links GLSL ES programs with the device's own GLES driver, and
// optionally times their fragment shaders, so a converted game's shaders are checked (and their
// GPU cost compared) without starting the game.
//
//   shadercheck <dir> [first [count]]             compile + link every <dir>/<nnnn>.vert/.frag pair,
//                                                 and every <dir>/<nnnn>.comp (a compute kernel)
//   shadercheck -t <dir> <nnnn>... [-s WxH] [-n N] [-o]
//                                                 time each program's fragment shader: N full-screen
//                                                 quads (default 720x480, 200) into an RGBA8 texture
//                                                 (-o: opaque, no blending)
//
// <dir> is what `python -m pmunity shaders-export` writes (index.tsv names the shaders). Prints one
// line per failure with the driver's log, then a summary; exit status 1 if any program failed.
// Timing binds every 2D sampler to a 256x256 RGBA8 texture (other sampler types get a unit each with
// no texture: they sample zeros) and sets every float uniform to 0.5, so
// it measures the shader's own cost (ALU, texture fetches, bandwidth of one RGBA8 target), not the
// game's data. Quads are blended (premultiplied alpha, as sprites), so each one is shaded in full.
// EGL: a pbuffer surface (Mesa: EGL_PLATFORM=surfaceless), else the Mali fbdev window.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9                   // (OpenGL ES 3.1)
#endif
#include <dirent.h>
#include <fcntl.h>
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
    glLinkProgram(pr);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = ""; glGetProgramInfoLog(pr, sizeof log, NULL, log);
        printf("FAIL %04d link (%s): %s\n", n, g_names[n], log);
        glDeleteProgram(pr);
        return 0;
    }
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
    glLinkProgram(pr);
    glDeleteShader(c);
    GLint ok = 0; glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = ""; glGetProgramInfoLog(pr, sizeof log, NULL, log);
        printf("FAIL %04d link (%s): %s\n", n, g_names[n], log);
        glDeleteProgram(pr);
        return 0;
    }
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

// every active float uniform 0.5 (vectors and arrays too), every sampler a 256x256 RGBA8 texture
static void default_uniforms(GLuint pr, GLuint tex2d)
{
    GLint nu = 0; glGetProgramiv(pr, GL_ACTIVE_UNIFORMS, &nu);
    int unit = 0;
    for (GLint i = 0; i < nu; i++) {
        char name[256]; GLint size; GLenum type;
        glGetActiveUniform(pr, i, sizeof name, NULL, &size, &type, name);
        GLint loc = glGetUniformLocation(pr, name);
        if (loc < 0) continue;
        static GLfloat half[4 * 16 * 64];
        if (!half[0]) for (size_t k = 0; k < sizeof half / sizeof half[0]; k++) half[k] = 0.5f;
        if (size > 64) size = 64;
        switch (type) {
        case GL_FLOAT: glUniform1fv(loc, size, half); break;
        case GL_FLOAT_VEC2: glUniform2fv(loc, size, half); break;
        case GL_FLOAT_VEC3: glUniform3fv(loc, size, half); break;
        case GL_FLOAT_VEC4: glUniform4fv(loc, size, half); break;
        case GL_FLOAT_MAT4: glUniformMatrix4fv(loc, size, GL_FALSE, half); break;
        case GL_SAMPLER_2D:
            glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, tex2d);
            glUniform1i(loc, unit++); break;
        // other sampler types get a unit of their own with no texture (they sample zeros): left on
        // unit 0 beside the 2D texture they would make every draw fail (two sampler types on one
        // unit), and the program would time as free (Hollow Knight's Sprites/Lit, a sampler3D)
        case GL_SAMPLER_3D: case GL_SAMPLER_CUBE: case GL_SAMPLER_2D_SHADOW: case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_SAMPLER_CUBE_SHADOW: case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D: case GL_INT_SAMPLER_CUBE: case GL_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
            glUniform1i(loc, unit++); break;
        default: break;
        }
    }
}

int main(int argc, char **argv)
{
    int timing = argc > 1 && !strcmp(argv[1], "-t");
    if (argc < 2 + timing) {
        fprintf(stderr, "usage: shadercheck <dir> [first [count]] | shadercheck -t <dir> <nnnn>... [-s WxH] [-n N]\n");
        return 2;
    }
    const char *dir = argv[1 + timing];
    int w = 720, h = 480, reps = 200, blend = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) blend = 0;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) reps = atoi(argv[++i]);
    }
    load_index(dir);
    if (!egl_up(64, 64)) return 1;
    if (!timing) {
        int first = argc > 2 ? atoi(argv[2]) : 0, count = argc > 3 ? atoi(argv[3]) : 10000;
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
    // timing: an RGBA8 target of w x h, a 256x256 RGBA8 noise texture, one full-screen quad per draw
    GLuint tex, rt, fbo;
    unsigned char *px = malloc(256 * 256 * 4);
    for (int i = 0; i < 256 * 256 * 4; i++) px[i] = (unsigned char)(rand() & 255);
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    glViewport(0, 0, w, h);
    static const GLfloat quad[] = { -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1 };
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) continue;
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "-n")) { i++; continue; }
        if (i == 2) continue;                                   // the directory
        int n = atoi(argv[i]);
        GLuint pr = build(dir, n, 0);
        if (!pr) continue;
        // every vertex attribute reads the quad's position (texcoords then span the quad too)
        glUseProgram(pr);
        GLint na = 0; glGetProgramiv(pr, GL_ACTIVE_ATTRIBUTES, &na);
        for (GLint a = 0; a < na; a++) {
            char an[128]; GLint sz; GLenum ty; glGetActiveAttrib(pr, a, sizeof an, NULL, &sz, &ty, an);
            GLint loc = glGetAttribLocation(pr, an);
            if (loc >= 0) { glEnableVertexAttribArray(loc); glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0); }
        }
        default_uniforms(pr, tex);
        // the model/view/projection uniforms of 0.5 would squash the quad: identity for Unity's matrices
        static const GLfloat id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const char *mats[] = { "hlslcc_mtx4x4unity_ObjectToWorld", "hlslcc_mtx4x4unity_MatrixVP", "hlslcc_mtx4x4unity_MatrixV",
                               "hlslcc_mtx4x4glstate_matrix_projection", "hlslcc_mtx4x4unity_MatrixMVP" };
        for (size_t k = 0; k < sizeof mats / sizeof mats[0]; k++) {
            char nm[128]; snprintf(nm, sizeof nm, "%s[0]", mats[k]);
            GLint loc = glGetUniformLocation(pr, nm);
            if (loc >= 0) glUniform4fv(loc, 4, id);
        }
        // blended like a sprite: an opaque full-screen quad would let the GPU drop the hidden ones
        if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); }
        else glDisable(GL_BLEND);
        for (int k = 0; k < 10; k++) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        double t0 = now();
        for (int k = 0; k < reps; k++) {
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            if ((k & 15) == 15) glFlush();
        }
        unsigned char probe[4]; glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, probe);
        double dt = now() - t0;
        printf("TIME %04d %8.3f ms/quad %6.2f ns/pixel  (%s)\n", n, dt * 1000 / reps, dt * 1e9 / reps / (w * h), g_names[n]);
        for (GLint a = 0; a < na; a++) glDisableVertexAttribArray(a);
        glDeleteProgram(pr);
    }
    return 0;
}
