// Device check: what a render pass costs a tiler, piece by piece (docs/profiling.md, "What a pass
// costs"). On the Mali fbdev window (as egltest), offscreen framebuffers of a game's size (default
// 720x450: Hollow Knight's), RGBA8 colour and a D24S8 depth/stencil texture. Each case is a frame
// of 16 passes, alternating between framebuffer A (the one measured) and B (colour only, cleared
// and invalidated: a separator), each pass one small draw (a 16x16 quad) unless the case says
// otherwise; the frame ends with glFinish, frames run for <seconds> after a warm-up, and the time
// per frame is the CPU's clock around them (no swaps: no vsync). A's passes start by clearing,
// loading (neither cleared nor invalidated) or invalidating colour and depth/stencil, and end by
// storing or invalidating them; differences between cases give a load's and a store's cost per
// pixel, and frames of 8 and 32 separator passes the cost of a pass itself.
//   Content: each A pass draws its quad somewhere else and clears to another colour, so the GPU's
//   transaction elimination (tiles written only when their CRC changed) does not hide stores;
//   "same" cases repeat the frame exactly (what a pass that changes few tiles gets).
// Build: host/build.sh; on the device: ./passbench [seconds per case] [width height]
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <glob.h>

struct fbdev_window { unsigned short width, height; };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int gpu_mhz(void)
{
    glob_t g;
    int mhz = 0;
    if (glob("/sys/class/devfreq/*/cur_freq", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && !mhz; i++) {
            FILE *f = fopen(g.gl_pathv[i], "r");
            long v = 0;
            if (f) { if (fscanf(f, "%ld", &v) == 1 && strstr(g.gl_pathv[i], "gpu")) mhz = (int)(v / 1000000); fclose(f); }
        }
        if (!mhz && g.gl_pathc) { FILE *f = fopen(g.gl_pathv[0], "r"); long v = 0; if (f) { if (fscanf(f, "%ld", &v) == 1) mhz = (int)(v / 1000000); fclose(f); } }
        globfree(&g);
    }
    return mhz;
}

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "shader: %s\n", log); exit(1); }
    return s;
}
static GLuint program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "link failed\n"); exit(1); }
    return p;
}

static int W = 720, H = 450;
static GLuint fbA, fbB, texA, dsA, texB, texS, progQuad, progCopy, vbo;
static GLint uOff, uCol;
static unsigned frameno;

enum { CLEAR, LOAD, INVAL };
enum { STORE, DROP };
typedef struct {
    const char *name;
    int c_start, ds_start;      // CLEAR / LOAD / INVAL
    int c_end, ds_end;          // STORE / DROP (invalidated at the end)
    int full;                   // A's draw is a full-screen copy from texS (not a 16x16 quad)
    int depth_test;             // the quad tests depth (LEQUAL, no writes): the DS values are read
    int same;                   // every frame identical (transaction elimination can skip stores)
    int seps;                   // separator passes (B) a frame; 0: 8 A and 8 B passes
    int window;                 // A is the window (fb 0)
    int merged;                 // (window) one pass: the A passes' draws all in one
} bench_case;

static void quad(int k, int n)
{
    // a 16x16 quad at a place that changes with k and the frame (or not, for "same")
    float x = (float)((k * 97 + n * 31) % (W - 16)), y = (float)((k * 53 + n * 17) % (H - 16));
    glUniform2f(uOff, x / W * 2 - 1, y / H * 2 - 1);
    glUniform4f(uCol, (k % 7) / 7.f, (n % 5) / 5.f, 0.5f, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void invalidate(GLenum target, int c, int ds, int window)
{
    GLenum a[3]; int n = 0;
    if (c) a[n++] = window ? GL_COLOR : GL_COLOR_ATTACHMENT0;
    if (ds) { if (window) { a[n++] = GL_DEPTH; a[n++] = GL_STENCIL; } else a[n++] = GL_DEPTH_STENCIL_ATTACHMENT; }
    if (n) glInvalidateFramebuffer(target, n, a);
}

static void a_pass(const bench_case *c, int k, unsigned n)
{
    GLuint fb = c->window ? 0 : fbA;
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0, 0, W, H);
    invalidate(GL_FRAMEBUFFER, c->c_start == INVAL, c->ds_start == INVAL, c->window);
    GLbitfield m = (c->c_start == CLEAR ? GL_COLOR_BUFFER_BIT : 0) | (c->ds_start == CLEAR ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : 0);
    if (m) { glClearColor((n % 3) / 3.f, (k % 4) / 4.f, 0.25f, 1); glClear(m); }
    if (c->depth_test) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE); }
    if (c->full) {
        glUseProgram(progCopy);
        glBindTexture(GL_TEXTURE_2D, texS);
        glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
        glUseProgram(progQuad);
    } else quad(k, (int)n);
    if (c->depth_test) glDisable(GL_DEPTH_TEST);
    invalidate(GL_FRAMEBUFFER, c->c_end == DROP, c->ds_end == DROP, c->window);
}

static void b_pass(int k, unsigned n)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbB);
    glViewport(0, 0, W, H);
    glClearColor(0, (k % 3) / 3.f, (n % 2) / 2.f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    quad(k + 1, (int)n);
    invalidate(GL_FRAMEBUFFER, 1, 0, 0);
}

static void frame(const bench_case *c, unsigned n)
{
    if (c->seps) {
        for (int k = 0; k < c->seps; k++) b_pass(k, n);
    } else if (c->merged) {
        // the A passes' draws in one pass on A, then the separators
        bench_case one = *c;
        glBindFramebuffer(GL_FRAMEBUFFER, c->window ? 0 : fbA);
        glViewport(0, 0, W, H);
        invalidate(GL_FRAMEBUFFER, c->c_start == INVAL, c->ds_start == INVAL, c->window);
        GLbitfield m = (c->c_start == CLEAR ? GL_COLOR_BUFFER_BIT : 0) | (c->ds_start == CLEAR ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : 0);
        if (m) { glClearColor((n % 3) / 3.f, 0, 0.25f, 1); glClear(m); }
        for (int k = 0; k < 8; k++) quad(k, (int)n);
        invalidate(GL_FRAMEBUFFER, c->c_end == DROP, c->ds_end == DROP, c->window);
        (void)one;
        for (int k = 0; k < 8; k++) b_pass(k, n);
    } else {
        for (int k = 0; k < 8; k++) { a_pass(c, k, n); b_pass(k, n); }
    }
    glFinish();
}

static double run(const bench_case *c, double secs, int *mhz_lo, int *mhz_hi)
{
    unsigned base = c->same ? 7 : 0;
    for (int i = 0; i < 20; i++) frame(c, c->same ? base : ++frameno);
    *mhz_lo = 100000; *mhz_hi = 0;
    double t0 = now(), t;
    int frames = 0;
    do {
        frame(c, c->same ? base : ++frameno);
        frames++;
        if (frames % 50 == 0) { int m = gpu_mhz(); if (m < *mhz_lo) *mhz_lo = m; if (m > *mhz_hi) *mhz_hi = m; }
    } while ((t = now()) - t0 < secs);
    return (t - t0) * 1000 / frames;
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 2.0;
    if (argc > 3) { W = atoi(argv[2]); H = atoi(argv[3]); }
    struct fb_var_screeninfo vi = {0};
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0) { perror("fb0"); return 1; }
    close(fd);
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, NULL, NULL)) { printf("eglInitialize failed\n"); return 1; }
    EGLint attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
                      EGL_RENDERABLE_TYPE, 0x40, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) { printf("no config\n"); return 1; }
    static struct fbdev_window win;
    win.width = vi.xres; win.height = vi.yres;
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)&win, NULL);
    EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) { printf("context failed\n"); return 1; }
    printf("passbench: %dx%d framebuffers, window %ux%u, %s, %.1f s a case\n", W, H, vi.xres, vi.yres, glGetString(GL_RENDERER), secs);
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    static const char *const fetch[] = { "GL_ARM_shader_framebuffer_fetch", "GL_ARM_shader_framebuffer_fetch_depth_stencil",
                                         "GL_EXT_shader_framebuffer_fetch", "GL_EXT_shader_pixel_local_storage", NULL };
    for (int i = 0; fetch[i]; i++) {
        size_t n = strlen(fetch[i]);
        const char *p = ext;
        int has = 0;
        while (p && (p = strstr(p, fetch[i]))) { if (p[n] == ' ' || p[n] == 0) { has = 1; break; } p += n; }
        printf("%s: %s\n", fetch[i], has ? "yes" : "no");
    }

    // A: RGBA8 + D24S8 textures; B: RGBA8; S: a texture to copy from
    glGenTextures(1, &texA); glBindTexture(GL_TEXTURE_2D, texA); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glGenTextures(1, &dsA); glBindTexture(GL_TEXTURE_2D, dsA); glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, W, H);
    glGenTextures(1, &texB); glBindTexture(GL_TEXTURE_2D, texB); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glGenTextures(1, &texS); glBindTexture(GL_TEXTURE_2D, texS); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    unsigned char *px = malloc((size_t)W * H * 4);
    for (int i = 0; i < W * H * 4; i++) px[i] = (unsigned char)(i * 2654435761u >> 24);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbA); glBindFramebuffer(GL_FRAMEBUFFER, fbA);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texA, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, dsA, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { printf("fb A incomplete\n"); return 1; }
    glGenFramebuffers(1, &fbB); glBindFramebuffer(GL_FRAMEBUFFER, fbB);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texB, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { printf("fb B incomplete\n"); return 1; }

    progQuad = program("#version 300 es\nin vec2 pos; uniform vec2 off; void main() { gl_Position = vec4(pos * vec2(32.0/720.0, 32.0/450.0) + off, 0.5, 1.0); }\n",
                       "#version 300 es\nprecision mediump float; uniform vec4 col; out vec4 o; void main() { o = col; }\n");
    progCopy = program("#version 300 es\nin vec2 pos; out vec2 uv; void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.5, 1.0); }\n",
                       "#version 300 es\nprecision mediump float; uniform sampler2D t; in vec2 uv; out vec4 o; void main() { o = texture(t, uv); }\n");
    uOff = glGetUniformLocation(progQuad, "off"); uCol = glGetUniformLocation(progQuad, "col");
    static const float v[] = { 0, 0, 1, 0, 0, 1, 1, 1, -1, -1, 1, -1, -1, 1, 1, 1 };
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glUseProgram(progQuad);
    glUniform1i(glGetUniformLocation(progCopy, "t"), 0);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);

    static const bench_case cases[] = {
        { "sep 8 (8 separator passes)", .seps = 8 },
        { "sep 32", .seps = 32 },
        { "A clear/clear, drop/drop", CLEAR, CLEAR, DROP, DROP },
        { "A clear/clear, store C", CLEAR, CLEAR, STORE, DROP },
        { "A load C, store C", LOAD, CLEAR, STORE, DROP },
        { "A clear/clear, store DS", CLEAR, CLEAR, DROP, STORE },
        { "A load DS, store DS", CLEAR, LOAD, DROP, STORE },
        { "A load DS (tested), store DS", CLEAR, LOAD, DROP, STORE, .depth_test = 1 },
        { "A load all, store all", LOAD, LOAD, STORE, STORE },
        { "A load all, store all, same", LOAD, LOAD, STORE, STORE, .same = 1 },
        { "A clear/clear, store all, same", CLEAR, CLEAR, STORE, STORE, .same = 1 },
        { "A inval/inval, store C", INVAL, INVAL, STORE, DROP },
        { "A full copy, clear, drop", CLEAR, CLEAR, DROP, DROP, .full = 1 },
        { "A full copy, clear, store C", CLEAR, CLEAR, STORE, DROP, .full = 1 },
        { "A full copy, load C, store C", LOAD, CLEAR, STORE, DROP, .full = 1 },
        { "A full copy, load all, store all", LOAD, LOAD, STORE, STORE, .full = 1 },
        { "A 8 quads in one pass, load all, store all", LOAD, LOAD, STORE, STORE, .merged = 1 },
        { "win load all, store C", LOAD, LOAD, STORE, DROP, .window = 1 },
        { "win load C, drop DS", LOAD, INVAL, STORE, DROP, .window = 1 },
        { "win clear all, store C", CLEAR, CLEAR, STORE, DROP, .window = 1 },
        { "win 8 quads in one pass, load all", LOAD, LOAD, STORE, DROP, .window = 1, .merged = 1 },
        { "sep 8 again", .seps = 8 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int lo, hi;
        double ms = run(&cases[i], secs, &lo, &hi);
        printf("%-46s %7.3f ms a frame  (GPU %d-%d MHz)\n", cases[i].name, ms, lo, hi);
        fflush(stdout);
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx); eglDestroySurface(dpy, surf); eglTerminate(dpy);
    return 0;
}
