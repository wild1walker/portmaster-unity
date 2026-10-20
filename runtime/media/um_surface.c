// um_surface: the consumer end of a video Surface, as Android's SurfaceTexture (GLConsumer).
//
// A codec releasing an output buffer with render=true queues its picture here and the listener
// is told (onFrameAvailable). updateTexImage, on the thread whose GL context owns the texture,
// latches the oldest queued picture: Android latches buffers in order, one per call, and Unity
// calls it once per frame it was told about.
//
// Android gives the external texture (GL_TEXTURE_EXTERNAL_OES) the decoder's own buffer through
// an EGLImage. Here the picture is uploaded into a one-channel GL_TEXTURE_2D (GL_R8, else
// GL_LUMINANCE) of pw x (ph + ph/2) texels in the packed 4:2:0 layout (um.h), and that texture is
// the EGLImage (EGL_KHR_gl_texture_2D_image) behind the external texture
// (glEGLImageTargetTexture2DOES). Only the upload happens per frame; the YUV -> RGB conversion is
// done by the converter's Hidden/VideoDecodeAndroid shader when Unity samples the texture. GL
// state the upload touches (the active unit's 2D and external bindings, unpack parameters and
// buffer) is put back as it was, so Unity's GL state cache stays right.
//
// getTransformMatrix maps Unity's quad texture coordinates to the shader's frame coordinates:
// (0,0) the top-left of the picture, (w/pw, h/ph) its bottom-right, so the matrix flips y as
// Android's does for its top-down buffers (scale (w/pw, -h/ph), offset (0, h/ph)).
#define _GNU_SOURCE
#include "um_internal.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

/* ---- GL / EGL entry points (resolved at the first update, from the libraries Unity uses) ------- */
typedef unsigned int GLenum, GLuint;
typedef int GLint, GLsizei;
typedef unsigned char GLubyte;
typedef void *EGLDisplay, *EGLContext, *EGLImageKHR, *EGLClientBuffer;
typedef int EGLint;
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_R8 0x8229
#define GL_RED 0x1903
#define GL_LUMINANCE 0x1909
#define GL_UNSIGNED_BYTE 0x1401
#define GL_EXTENSIONS 0x1F03
#define GL_NO_ERROR 0
#define EGL_EXTENSIONS 0x3055
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#define EGL_GL_TEXTURE_LEVEL_KHR 0x30BC
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#define EGL_TRUE 1
#define EGL_NONE 0x3038

static struct {
    int tried, ok;
    void (*GetIntegerv)(GLenum, GLint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*PixelStorei)(GLenum, GLint);
    void (*BindBuffer)(GLenum, GLuint);
    GLenum (*GetError)(void);
    const GLubyte *(*GetString)(GLenum);
    void (*EGLImageTargetTexture2DOES)(GLenum, void *);
    EGLDisplay (*eglGetCurrentDisplay)(void);
    EGLContext (*eglGetCurrentContext)(void);
    EGLImageKHR (*eglCreateImageKHR)(EGLDisplay, EGLContext, GLenum, EGLClientBuffer, const EGLint *);
    int (*eglDestroyImageKHR)(EGLDisplay, EGLImageKHR);
    const char *(*eglQueryString)(EGLDisplay, EGLint);
    void *(*eglGetProcAddress)(const char *);
    int es3;
} gl;

static void *open_first(const char *env, const char *const *names)
{
    const char *e = env ? getenv(env) : NULL;
    void *h = e && *e ? dlopen(e, RTLD_NOW | RTLD_GLOBAL) : NULL;
    for (int i = 0; !h && names[i]; i++) h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    return h;
}

static int gl_load(void)
{
    if (gl.tried) return gl.ok;
    gl.tried = 1;
    static const char *const egl_names[] = { "libEGL.so.1", "libEGL.so", NULL };
    static const char *const gles_names[] = { "libGLESv2.so.2", "libGLESv2.so", NULL };
    void *egl = open_first("ANDROID_SHIM_LIBEGL", egl_names);
    void *gles = open_first("UM_LIBGLES", gles_names);
    if (!egl) { um_log("surface: no libEGL (%s)", dlerror()); return 0; }
    gl.eglGetProcAddress = dlsym(egl, "eglGetProcAddress");
#define EG(n) gl.n = dlsym(egl, #n)
#define GG(n) do { gl.n = gles ? dlsym(gles, "gl" #n) : NULL; \
                   if (!gl.n && gl.eglGetProcAddress) gl.n = gl.eglGetProcAddress("gl" #n); } while (0)
    EG(eglGetCurrentDisplay); EG(eglGetCurrentContext); EG(eglQueryString);
    gl.eglCreateImageKHR = gl.eglGetProcAddress ? gl.eglGetProcAddress("eglCreateImageKHR") : NULL;
    gl.eglDestroyImageKHR = gl.eglGetProcAddress ? gl.eglGetProcAddress("eglDestroyImageKHR") : NULL;
    if (!gl.eglCreateImageKHR) EG(eglCreateImageKHR);
    if (!gl.eglDestroyImageKHR) EG(eglDestroyImageKHR);
    GG(GetIntegerv); GG(BindTexture); GG(GenTextures); GG(DeleteTextures); GG(TexImage2D); GG(TexSubImage2D);
    GG(TexParameteri); GG(PixelStorei); GG(BindBuffer); GG(GetError); GG(GetString);
    gl.EGLImageTargetTexture2DOES = gl.eglGetProcAddress ? gl.eglGetProcAddress("glEGLImageTargetTexture2DOES") : NULL;
#undef EG
#undef GG
    gl.ok = gl.eglGetCurrentDisplay && gl.eglGetCurrentContext && gl.eglCreateImageKHR && gl.eglDestroyImageKHR &&
            gl.GetIntegerv && gl.BindTexture && gl.GenTextures && gl.DeleteTextures && gl.TexImage2D &&
            gl.TexSubImage2D && gl.TexParameteri && gl.PixelStorei && gl.GetError && gl.GetString &&
            gl.EGLImageTargetTexture2DOES;
    if (!gl.ok) um_log("surface: GL/EGL entry points missing (eglCreateImageKHR %p, glEGLImageTargetTexture2DOES %p)",
                       (void *)gl.eglCreateImageKHR, (void *)gl.EGLImageTargetTexture2DOES);
    return gl.ok;
}

/* ---- surfaces ------------------------------------------------------------------------------------ */
struct um_surface {
    pthread_mutex_t mu;
    int refs;
    unsigned tex;               /* the external texture (Unity's) */
    um_frame *q, *qtail;        /* queued, not latched */
    int qn;
    void (*listener)(void *, um_surface *);
    void *user;
    /* latched */
    int w, h, pw, ph;
    int64_t ts_ns;
    /* GL objects (made on the GL thread) */
    GLuint src;
    EGLImageKHR img;
    EGLDisplay img_dpy;
    int src_w, src_h;
    GLenum src_fmt;
    int detach;
    int dropped;
    uint64_t latched;
};

/* GL objects of released surfaces, freed by the next update on a GL thread */
static pthread_mutex_t zombie_mu = PTHREAD_MUTEX_INITIALIZER;
static struct zombie { struct zombie *next; GLuint src; EGLImageKHR img; EGLDisplay dpy; } *zombies;

um_surface *um_surface_new(unsigned tex)
{
    um_surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    pthread_mutex_init(&s->mu, NULL);
    s->refs = 1;
    s->tex = tex;
    return s;
}

um_surface *um_surface_ref(um_surface *s)
{
    if (!s) return NULL;
    pthread_mutex_lock(&s->mu);
    s->refs++;
    pthread_mutex_unlock(&s->mu);
    return s;
}

void um_surface_unref(um_surface *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    int left = --s->refs;
    pthread_mutex_unlock(&s->mu);
    if (left > 0) return;
    while (s->q) { um_frame *f = s->q; s->q = f->next; um_frame_put(f); }
    if (s->src || s->img) {
        struct zombie *z = calloc(1, sizeof *z);
        if (z) {
            z->src = s->src; z->img = s->img; z->dpy = s->img_dpy;
            pthread_mutex_lock(&zombie_mu);
            z->next = zombies; zombies = z;
            pthread_mutex_unlock(&zombie_mu);
        }
    }
    if (s->latched) um_log("surface %u: %llu frames shown, %d dropped", s->tex, (unsigned long long)s->latched, s->dropped);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

void um_surface_set_listener(um_surface *s, void (*fn)(void *, um_surface *), void *user)
{
    pthread_mutex_lock(&s->mu);
    s->listener = fn;
    s->user = user;
    pthread_mutex_unlock(&s->mu);
}

#define QUEUE_MAX 4

void um_surface_queue(um_surface *s, um_frame *f)
{
    if (!s || !f) { um_frame_put(f); return; }
    pthread_mutex_lock(&s->mu);
    f->next = NULL;
    if (s->qtail) s->qtail->next = f; else s->q = f;
    s->qtail = f;
    s->qn++;
    um_frame *drop = NULL;
    if (s->qn > QUEUE_MAX) {                   /* nobody latches: keep the newest */
        drop = s->q;
        s->q = drop->next;
        s->qn--;
        s->dropped++;
    }
    void (*fn)(void *, um_surface *) = s->listener;
    void *user = s->user;
    pthread_mutex_unlock(&s->mu);
    um_frame_put(drop);
    if (fn) fn(user, s);
}

int um_surface_pending(um_surface *s)
{
    pthread_mutex_lock(&s->mu);
    int n = s->qn;
    pthread_mutex_unlock(&s->mu);
    return n;
}

static void free_zombies(void)
{
    pthread_mutex_lock(&zombie_mu);
    struct zombie *z = zombies;
    zombies = NULL;
    pthread_mutex_unlock(&zombie_mu);
    while (z) {
        struct zombie *n = z->next;
        if (z->img) gl.eglDestroyImageKHR(z->dpy, z->img);
        if (z->src) gl.DeleteTextures(1, &z->src);
        free(z);
        z = n;
    }
}

/* the source texture and its EGLImage for a pw x th picture, bound to the external texture */
static int make_source(um_surface *s, int tw, int th)
{
    EGLDisplay dpy = gl.eglGetCurrentDisplay();
    EGLContext ctx = gl.eglGetCurrentContext();
    if (s->img) { gl.eglDestroyImageKHR(s->img_dpy, s->img); s->img = NULL; }
    if (s->src) { gl.DeleteTextures(1, &s->src); s->src = 0; }
    static const GLenum fmts[][3] = { { GL_R8, GL_RED, 0 }, { GL_LUMINANCE, GL_LUMINANCE, 0 } };
    static int logged;
    for (unsigned k = 0; k < 2; k++) {
        GLenum ifmt = fmts[k][0], fmt = fmts[k][1];
        if (ifmt == GL_R8 && gl.es3 < 0) continue;
        while (gl.GetError() != GL_NO_ERROR) {}
        gl.GenTextures(1, &s->src);
        gl.BindTexture(GL_TEXTURE_2D, s->src);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexImage2D(GL_TEXTURE_2D, 0, ifmt, tw, th, 0, fmt, GL_UNSIGNED_BYTE, NULL);
        GLenum err = gl.GetError();
        EGLint attrs[] = { EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
        EGLImageKHR img = err == GL_NO_ERROR ?
            gl.eglCreateImageKHR(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)s->src, attrs) : NULL;
        if (img) {
            gl.BindTexture(GL_TEXTURE_EXTERNAL_OES, s->tex);
            gl.EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
            gl.TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            err = gl.GetError();
            if (err == GL_NO_ERROR) {
                s->img = img; s->img_dpy = dpy;
                s->src_w = tw; s->src_h = th; s->src_fmt = fmt;
                if (!logged++) um_log("surface: frames through an EGLImage of a %s texture", ifmt == GL_R8 ? "GL_R8" : "GL_LUMINANCE");
                return 0;
            }
            gl.eglDestroyImageKHR(dpy, img);
        }
        um_log("surface: %s texture as EGLImage failed (GL error 0x%x)", ifmt == GL_R8 ? "GL_R8" : "GL_LUMINANCE", err);
        gl.DeleteTextures(1, &s->src);
        s->src = 0;
    }
    return -1;
}

int um_surface_update(um_surface *s)
{
    if (!s) return -1;
    pthread_mutex_lock(&s->mu);
    um_frame *f = s->q;
    if (f) { s->q = f->next; if (!s->q) s->qtail = NULL; s->qn--; }
    pthread_mutex_unlock(&s->mu);
    if (!f) return 0;
    s->w = f->width; s->h = f->height; s->pw = f->pw; s->ph = f->ph;
    s->ts_ns = f->pts_us * 1000;
    s->latched++;
    if (!gl_load()) { um_frame_put(f); return -1; }
    if (!gl.eglGetCurrentContext()) {
        static int warned;
        if (!warned++) um_log("surface: updateTexImage without a current GL context");
        um_frame_put(f);
        return -1;
    }
    if (!gl.es3) {
        const char *v = (const char *)gl.GetString(0x1F02 /* GL_VERSION */);
        gl.es3 = v && strstr(v, "OpenGL ES ") && v[10] >= '3' ? 1 : -1;
    }
    free_zombies();
    /* what the upload touches, to restore */
    GLint b2d = 0, bext = 0, align = 4, rowlen = 0, skiprows = 0, skippix = 0, pbo = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &b2d);
    gl.GetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &bext);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    if (gl.es3 > 0) {
        gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &rowlen);
        gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &skiprows);
        gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &skippix);
        gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    }
    int tw = f->pw, th = f->ph + f->ph / 2, r = 1;
    if (s->detach || !s->img || s->src_w != tw || s->src_h != th) {
        s->detach = 0;
        if (make_source(s, tw, th) != 0) r = -1;
    }
    if (r > 0) {
        if (pbo && gl.BindBuffer) gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (gl.es3 > 0) {
            if (rowlen) gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            if (skiprows) gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            if (skippix) gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        }
        gl.BindTexture(GL_TEXTURE_2D, s->src);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, s->src_fmt, GL_UNSIGNED_BYTE, f->data);
        GLenum err = gl.GetError();
        if (err != GL_NO_ERROR) { um_log("surface: upload failed (GL error 0x%x)", err); r = -1; }
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, align);
        if (gl.es3 > 0) {
            if (rowlen) gl.PixelStorei(GL_UNPACK_ROW_LENGTH, rowlen);
            if (skiprows) gl.PixelStorei(GL_UNPACK_SKIP_ROWS, skiprows);
            if (skippix) gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, skippix);
        }
        if (pbo && gl.BindBuffer) gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    }
    gl.BindTexture(GL_TEXTURE_2D, b2d);
    gl.BindTexture(GL_TEXTURE_EXTERNAL_OES, bext);
    UM_TRACE("surface %u: latched %lld us (%dx%d)", s->tex, (long long)f->pts_us, f->width, f->height);
    um_frame_put(f);
    return r;
}

void um_surface_matrix(um_surface *s, float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    float sx = 1, sy = 1;
    if (s && s->pw > 0 && s->ph > 0) { sx = (float)s->w / s->pw; sy = (float)s->h / s->ph; }
    m[0] = sx;
    m[5] = -sy;
    m[10] = 1;
    m[13] = sy;
    m[15] = 1;
}

int64_t um_surface_timestamp(um_surface *s) { return s ? s->ts_ns : 0; }
void um_surface_set_default_size(um_surface *s, int w, int h) { (void)s; (void)w; (void)h; }
void um_surface_detach_gl(um_surface *s) { if (s) s->detach = 1; }
