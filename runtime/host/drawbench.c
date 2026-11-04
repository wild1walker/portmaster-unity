// Device check: what the driver costs the render thread per draw of dynamic geometry, the way Unity's
// GLES device draws sprites (docs/profiling.md, "The render thread's draws"). On the Mali fbdev window
// (as passbench), an offscreen 600x400 RGBA8 target; each frame <draws> draws of a sprite chunk (16
// quads: 64 vertices of position, colour and UV, 96 indices), each from geometry written that frame,
// with a texture bind among 8 textures and a program switch every other draw (as Hollow Knight's
// Greenpath: ~200 draws, ~100 program switches, ~430 texture binds a frame). Frames run back to
// back for <seconds> (glFlush each frame, glFinish every 4), the CPU's clock around them; the GPU's
// work is a few small quads, so the time is the render thread's: the driver and the calls.
//   unity     every draw takes two buffers of a pool (vertices, indices; 3 frames' worth, so the GPU
//             is done with them) and maps each (write | invalidate range | explicit flush |
//             unsynchronized), writes, flushes and unmaps it: Unity's DynamicVBO
//   persist   the same buffers with persistent, coherent storage mapped once (UA_GL_PERSIST): the
//             writes go straight into the mappings, no map, flush or unmap
//   arena     one persistent buffer for all of it: every draw's geometry at its own offset, the same
//             buffer bound for every draw (attribute pointers and the indices' offset move)
//   basevtx   the arena with the attribute pointers set once a frame and glDrawElementsBaseVertex
//             finding each chunk's vertices (the indices still at their own offset)
//   pvao      persist's buffers, each pair with a vertex array object made once (its attribute
//             pointers and index buffer): a draw binds the object instead of setting them
//   nodraw    persist without the glDrawElements (what the draw call itself costs: the difference)
//   nostate   (each of the above with a "-flat" twin: no texture binds or program switches)
// Build: host/build.sh; on the device: ./drawbench [seconds per case] [draws a frame]
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

struct fbdev_window { unsigned short width, height; };

#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#define GL_MAP_COHERENT_BIT_EXT 0x0080
#define GL_DYNAMIC_STORAGE_BIT_EXT 0x0100

static void (*BufferStorageEXT)(GLenum, GLsizeiptr, const void *, GLbitfield);
static void (*DrawElementsBaseVertexOES)(GLenum, GLsizei, GLenum, const void *, GLint);

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

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
    glBindAttribLocation(p, 1, "col");
    glBindAttribLocation(p, 2, "uv");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "link failed\n"); exit(1); }
    return p;
}

enum { QUADS = 16, VERTS = QUADS * 4, INDS = QUADS * 6, STRIDE = 24, VBYTES = VERTS * STRIDE, IBYTES = INDS * 2, RING = 3 };
enum { UNITY, PERSIST, PVAO, NODRAW, ARENA, BASEVTX };
static const char *const names[] = { "unity", "persist", "pvao", "nodraw", "arena", "basevtx" };

static int D = 200;
static GLuint *vb, *ib, *vaos, mainvao;
static void **vmap, **imap;
static GLuint arena; static char *amap; static size_t aframe;
static GLuint prog[2], tex[8];
static GLint uTint[2];

static void fill(char *v, char *ix, int k, unsigned n, int base)
{
    for (int q = 0; q < QUADS; q++) {
        float x = (float)((k * 37 + q * 11 + n * 3) % 580) / 300 - 1, y = (float)((k * 13 + q * 7) % 380) / 200 - 1;
        for (int c = 0; c < 4; c++) {
            float *p = (float *)(v + (q * 4 + c) * STRIDE);
            p[0] = x + (c & 1) * 0.05f; p[1] = y + (c >> 1) * 0.05f; p[2] = 0;
            unsigned *col = (unsigned *)(p + 3); *col = 0xff808080u ^ (unsigned)(k * 2654435761u);
            p[4] = (float)(c & 1); p[5] = (float)(c >> 1);
        }
        unsigned short *i = (unsigned short *)ix + q * 6, b = (unsigned short)(base + q * 4);
        i[0] = b; i[1] = b + 1; i[2] = b + 2; i[3] = b + 2; i[4] = b + 1; i[5] = b + 3;
    }
}

static void attribs(GLintptr off)
{
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, STRIDE, (void *)off);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, STRIDE, (void *)(off + 12));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, STRIDE, (void *)(off + 16));
}

static void frame(int mode, int state, unsigned n)
{
    int slot = (int)(n % RING) * D;
    size_t abase = (n % RING) * aframe;
    glBindVertexArray(mainvao);
    if (mode >= ARENA) { glBindBuffer(GL_ARRAY_BUFFER, arena); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, arena); }
    if (mode == BASEVTX) attribs((GLintptr)abase);
    for (int k = 0; k < D; k++) {
        if (state) {
            if (!(k & 1)) { glUseProgram(prog[k >> 1 & 1]); }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex[k & 7]);
        }
        glUniform4f(uTint[state ? k >> 1 & 1 : 0], (k & 3) / 3.f, 0.5f, 0.5f, 1);
        int b = slot + k;
        if (mode == UNITY) {
            glBindBuffer(GL_ARRAY_BUFFER, vb[b]);
            char *v = glMapBufferRange(GL_ARRAY_BUFFER, 0, VBYTES, 0x36);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib[b]);
            char *ix = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, IBYTES, 0x36);
            fill(v, ix, k, n, 0);
            glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, VBYTES); glUnmapBuffer(GL_ARRAY_BUFFER);
            glFlushMappedBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, IBYTES); glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
            attribs(0);
            glDrawElements(GL_TRIANGLES, INDS, GL_UNSIGNED_SHORT, 0);
        } else if (mode == PERSIST || mode == NODRAW) {
            glBindBuffer(GL_ARRAY_BUFFER, vb[b]);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib[b]);
            fill(vmap[b], imap[b], k, n, 0);
            attribs(0);
            if (mode == PERSIST) glDrawElements(GL_TRIANGLES, INDS, GL_UNSIGNED_SHORT, 0);
        } else if (mode == PVAO) {
            glBindVertexArray(vaos[b]);
            fill(vmap[b], imap[b], k, n, 0);
            glDrawElements(GL_TRIANGLES, INDS, GL_UNSIGNED_SHORT, 0);
        } else {
            size_t voff = abase + (size_t)k * (VBYTES + IBYTES), ioff = voff + VBYTES;
            fill(amap + voff, amap + ioff, k, n, 0);
            if (mode == ARENA) {
                attribs((GLintptr)voff);
                glDrawElements(GL_TRIANGLES, INDS, GL_UNSIGNED_SHORT, (void *)ioff);
            } else {
                DrawElementsBaseVertexOES(GL_TRIANGLES, INDS, GL_UNSIGNED_SHORT, (void *)ioff, (GLint)((voff - abase) / STRIDE));
            }
        }
    }
    glFlush();
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 3.0;
    if (argc > 2) D = atoi(argv[2]);
    struct fb_var_screeninfo vi = {0};
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0) { perror("fb0"); return 1; }
    close(fd);
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, NULL, NULL)) { printf("eglInitialize failed\n"); return 1; }
    EGLint attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                      EGL_RENDERABLE_TYPE, 0x40, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) { printf("no config\n"); return 1; }
    static struct fbdev_window win;
    win.width = vi.xres; win.height = vi.yres;
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)&win, NULL);
    EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) { printf("context failed\n"); return 1; }
    *(void **)&BufferStorageEXT = (void *)eglGetProcAddress("glBufferStorageEXT");
    *(void **)&DrawElementsBaseVertexOES = (void *)eglGetProcAddress("glDrawElementsBaseVertexOES");
    printf("drawbench: %s, %d draws a frame (%d quads each), %.1f s a case; buffer storage %s, base vertex %s\n",
           glGetString(GL_RENDERER), D, QUADS, secs, BufferStorageEXT ? "yes" : "no", DrawElementsBaseVertexOES ? "yes" : "no");

    GLuint fb, ct;
    glGenTextures(1, &ct); glBindTexture(GL_TEXTURE_2D, ct); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 600, 400);
    glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ct, 0);
    glViewport(0, 0, 600, 400);
    unsigned px[16 * 16];
    glGenTextures(8, tex);
    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < 256; i++) px[i] = 0xff000000u | ((unsigned)(i * 2654435761u >> 8) ^ (unsigned)t * 0x111111u);
        glBindTexture(GL_TEXTURE_2D, tex[t]); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    const char *vs = "#version 300 es\nin vec4 pos; in vec4 col; in vec2 uv; out vec4 c; out vec2 t;\n"
                     "void main() { gl_Position = pos; c = col; t = uv; }\n";
    const char *fs0 = "#version 300 es\nprecision mediump float; in vec4 c; in vec2 t; uniform sampler2D s; uniform vec4 tint; out vec4 o;\n"
                      "void main() { o = texture(s, t) * c * tint; }\n";
    const char *fs1 = "#version 300 es\nprecision mediump float; in vec4 c; in vec2 t; uniform sampler2D s; uniform vec4 tint; out vec4 o;\n"
                      "void main() { vec4 x = texture(s, t); o = vec4(x.rgb * tint.rgb, x.a * c.a); }\n";
    prog[0] = program(vs, fs0); prog[1] = program(vs, fs1);
    for (int p = 0; p < 2; p++) { uTint[p] = glGetUniformLocation(prog[p], "tint"); glUseProgram(prog[p]); glUniform1i(glGetUniformLocation(prog[p], "s"), 0); }
    glUseProgram(prog[0]);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int a = 0; a < 3; a++) glEnableVertexAttribArray(a);
    glGenVertexArrays(1, &mainvao); glBindVertexArray(mainvao);
    for (int a = 0; a < 3; a++) glEnableVertexAttribArray(a);

    int nb = RING * D;
    vb = calloc(nb, sizeof *vb); ib = calloc(nb, sizeof *ib); vaos = calloc(nb, sizeof *vaos); vmap = calloc(nb, sizeof *vmap); imap = calloc(nb, sizeof *imap);
    aframe = (size_t)D * (VBYTES + IBYTES);
    for (int mode = UNITY; mode <= BASEVTX; mode++) {
        if (mode >= PERSIST && !BufferStorageEXT) continue;
        if (mode == BASEVTX && !DrawElementsBaseVertexOES) continue;
        // the buffers of this mode
        if (mode < ARENA) {
            glGenBuffers(nb, vb); glGenBuffers(nb, ib);
            for (int i = 0; i < nb; i++) {
                glBindBuffer(GL_ARRAY_BUFFER, vb[i]); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib[i]);
                if (mode == UNITY) {
                    glBufferData(GL_ARRAY_BUFFER, VBYTES, NULL, GL_DYNAMIC_DRAW);
                    glBufferData(GL_ELEMENT_ARRAY_BUFFER, IBYTES, NULL, GL_DYNAMIC_DRAW);
                } else {
                    GLbitfield f = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT, m = f | GL_DYNAMIC_STORAGE_BIT_EXT;
                    BufferStorageEXT(GL_ARRAY_BUFFER, VBYTES, NULL, m); vmap[i] = glMapBufferRange(GL_ARRAY_BUFFER, 0, VBYTES, f);
                    BufferStorageEXT(GL_ELEMENT_ARRAY_BUFFER, IBYTES, NULL, m); imap[i] = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, IBYTES, f);
                    if (!vmap[i] || !imap[i]) { printf("persistent map failed\n"); return 1; }
                }
            }
            if (mode == PVAO) {             // a vertex array per buffer pair, made once
                glGenVertexArrays(nb, vaos);
                for (int i = 0; i < nb; i++) {
                    glBindVertexArray(vaos[i]);
                    for (int a = 0; a < 3; a++) glEnableVertexAttribArray(a);
                    glBindBuffer(GL_ARRAY_BUFFER, vb[i]); attribs(0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib[i]);
                }
                glBindVertexArray(mainvao);
            }
        } else if (!arena) {
            GLbitfield f = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
            glGenBuffers(1, &arena); glBindBuffer(GL_ARRAY_BUFFER, arena);
            BufferStorageEXT(GL_ARRAY_BUFFER, (GLsizeiptr)(aframe * RING), NULL, f | GL_DYNAMIC_STORAGE_BIT_EXT);
            amap = glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(aframe * RING), f);
            if (!amap) { printf("arena map failed\n"); return 1; }
        }
        for (int state = 1; state >= 0; state--) {
            unsigned fn = 0;
            for (int i = 0; i < 24; i++) { frame(mode, state, ++fn); if (i % 4 == 3) glFinish(); }
            glFinish();
            double t0 = now(), t;
            unsigned frames = 0;
            do { frame(mode, state, ++fn); frames++; if (frames % 4 == 0) glFinish(); } while (frames % 4 || (t = now()) - t0 < secs);
            glFinish();
            t = now() - t0;
            GLenum e = glGetError();
            printf("%-8s%-6s %6.2f ms a frame, %5.1f us a draw%s\n", names[mode], state ? "" : "-flat", t * 1e3 / frames,
                   t * 1e6 / frames / D, e ? " (GL error)" : "");
        }
        if (mode == PVAO) glDeleteVertexArrays(nb, vaos);
        if (mode < ARENA) { glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); glDeleteBuffers(nb, vb); glDeleteBuffers(nb, ib); }
    }
    return 0;
}
