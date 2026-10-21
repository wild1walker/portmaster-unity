/* glxes: GLX for OpenGL ES contexts, on EGL. A libGL.so.1 for Unity's Linux player in its OpenGL
 * ES mode (-force-gles) on drivers without desktop OpenGL (Mali and other GLES-only drivers),
 * under box64.
 *
 * Unity's Linux player draws with OpenGL ES through GLX (GLX_EXT_create_context_es2_profile), and
 * before that it opens a desktop core-profile context to learn the GL level: without one it stops
 * ("Unable to find a supported OpenGL core profile"). This library gives it both, made of EGL
 * OpenGL ES contexts on the same X display:
 *   - a context asked for with the ES profile is a plain EGL ES context of that version, drawing
 *     to an EGL window surface of the GLX drawable; glXSwapBuffers is eglSwapBuffers;
 *   - any other context answers as a desktop core profile of version GLXES_PROBE_VERSION (default
 *     "3.2"): Unity turns that number into the level of its ES device (a 3.2 probe gives an
 *     OpenGL ES 3.2 device; a 4.x one would ask for "#version 4x0 es" shaders).
 * GL functions are the GLES driver's own (glXGetProcAddress and dlsym through this library, which
 * links libGLESv2). Not a desktop GL implementation: Unity's OpenGL Core renderer (no -force-gles)
 * draws nothing with it.
 *
 * Env: GLXES_PROBE_VERSION=<major>.<minor>, GLXES_LOG=1 (calls on stderr), GLXES_FPS=<file> (frames
 * per second appended to the file, one line a second).
 * MIT licence (portmaster-unity). */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default")))

/* The few EGL and GLES names used (Khronos values), so that only libX11's headers are needed to
 * build; the library links the device's libEGL.so.1 and libGLESv2.so.2 by name. */
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int EGLint;
typedef unsigned int EGLBoolean, EGLenum, GLenum;
typedef int GLint;
typedef unsigned char GLubyte;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_BUFFER_SIZE 0x3020
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_SURFACE_TYPE 0x3033
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define GL_VERSION 0x1F02
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
EGLDisplay eglGetDisplay(void *native);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
EGLBoolean eglBindAPI(EGLenum);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint *);
const char *eglQueryString(EGLDisplay, EGLint);
EGLint eglGetError(void);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext);
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, unsigned long, const EGLint *);
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint, EGLint *);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean eglSwapInterval(EGLDisplay, EGLint);
void (*eglGetProcAddress(const char *))(void);

/* GLX types and constants (glx.h is not needed for so few) */
typedef struct glxes_ctx *GLXContext;
typedef struct glxes_cfg *GLXFBConfig;
typedef XID GLXDrawable, GLXWindow, GLXPbuffer, GLXPixmap;
typedef void (*fnptr)(void);

#define GLX_USE_GL 1
#define GLX_BUFFER_SIZE 2
#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_DEPTH_SIZE 12
#define GLX_STENCIL_SIZE 13
#define GLX_CONFIG_CAVEAT 0x20
#define GLX_X_VISUAL_TYPE 0x22
#define GLX_TRANSPARENT_TYPE 0x23
#define GLX_NONE 0x8000
#define GLX_TRUE_COLOR 0x8002
#define GLX_VISUAL_ID 0x800B
#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_RENDER_TYPE 0x8011
#define GLX_X_RENDERABLE 0x8012
#define GLX_FBCONFIG_ID 0x8013
#define GLX_MAX_PBUFFER_WIDTH 0x8016
#define GLX_MAX_PBUFFER_HEIGHT 0x8017
#define GLX_WIDTH 0x801D
#define GLX_HEIGHT 0x801E
#define GLX_WINDOW_BIT 1
#define GLX_PBUFFER_BIT 4
#define GLX_RGBA_BIT 1
#define GLX_VENDOR 1
#define GLX_VERSION 2
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_ES_PROFILE_BIT_EXT 0x4

#define GL_CONTEXT_FLAGS 0x821E
#define GL_CONTEXT_PROFILE_MASK 0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT 1

struct glxes_cfg { int id; };
struct glxes_ctx { EGLContext egl; EGLSurface pbuf; int es; };
struct surf { GLXDrawable draw; EGLSurface egl; struct surf *next; };

static struct glxes_cfg cfgs[1] = {{1}};
static EGLDisplay edpy = EGL_NO_DISPLAY;
static EGLConfig econfig;
static struct surf *surfs;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static __thread struct glxes_ctx *current;
static __thread Display *current_dpy;
static __thread GLXDrawable current_draw;
static int ver_major = 3, ver_minor = 2, logging;
static char version_string[160], glsl_string[32];
static void *self, *gles;
static void (*es_GetIntegerv)(GLenum, GLint *);
static const GLubyte *(*es_GetString)(GLenum);

static const char *glx_ext = "GLX_ARB_create_context GLX_ARB_create_context_profile GLX_EXT_create_context_es_profile "
                             "GLX_EXT_create_context_es2_profile GLX_ARB_get_proc_address GLX_ARB_multisample "
                             "GLX_EXT_visual_info GLX_SGI_make_current_read GLX_SGIX_fbconfig GLX_SGIX_pbuffer "
                             "GLX_EXT_swap_control GLX_MESA_swap_control GLX_SGI_swap_control";

#define LOG(...) do { if (logging) fprintf(stderr, "[glxes] " __VA_ARGS__); } while (0)

static void setup(void) {
    static int done;
    if (done)
        return;
    done = 1;
    const char *v = getenv("GLXES_PROBE_VERSION");
    if (v && sscanf(v, "%d.%d", &ver_major, &ver_minor) != 2)
        ver_major = 3, ver_minor = 2;
    logging = getenv("GLXES_LOG") && *getenv("GLXES_LOG") == '1';
    /* the GLSL version that goes with a GL version: 3.0 1.30, 3.1 1.40, 3.2 1.50, 3.3+ the same number */
    if (ver_major == 3 && ver_minor < 3)
        snprintf(glsl_string, sizeof glsl_string, "1.%d0", 3 + ver_minor);
    else
        snprintf(glsl_string, sizeof glsl_string, "%d.%d0", ver_major, ver_minor);
}

static void es_load(void) {
    if (gles)
        return;
    gles = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_NOLOAD);
    if (!gles)
        gles = dlopen("libGLESv2.so.2", RTLD_LAZY);
    if (gles) {
        es_GetString = (const GLubyte *(*)(GLenum))dlsym(gles, "glGetString");
        es_GetIntegerv = (void (*)(GLenum, GLint *))dlsym(gles, "glGetIntegerv");
    }
    Dl_info info;     /* this library's own handle: its symbols, then libGLESv2's and libEGL's */
    if (dladdr((void *)es_load, &info) && info.dli_fname)
        self = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
}

static int egl_open(Display *dpy) {
    setup();
    if (edpy != EGL_NO_DISPLAY)
        return 1;
    edpy = eglGetDisplay(dpy);
    if (edpy == EGL_NO_DISPLAY || !eglInitialize(edpy, NULL, NULL)) {
        LOG("eglInitialize failed (0x%x)\n", eglGetError());
        edpy = EGL_NO_DISPLAY;
        return 0;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint want[][19] = {
        {EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
         EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE},
        {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
         EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_NONE},
    };
    EGLint n = 0;
    for (unsigned i = 0; i < sizeof want / sizeof want[0] && n < 1; i++)
        if (!eglChooseConfig(edpy, want[i], &econfig, 1, &n))
            n = 0;
    if (n < 1) {
        LOG("no EGL config\n");
        return 0;
    }
    LOG("EGL %s, %s\n", eglQueryString(edpy, EGL_VENDOR), eglQueryString(edpy, EGL_VERSION));
    return 1;
}

/* The X visual of the EGL config, so the windows the game makes take an EGL window surface */
static XVisualInfo *visual(Display *dpy, int screen) {
    XVisualInfo tmpl, *vi = NULL;
    int n;
    EGLint id = 0;
    if (egl_open(dpy) && eglGetConfigAttrib(edpy, econfig, EGL_NATIVE_VISUAL_ID, &id) && id) {
        tmpl.visualid = (VisualID)id;
        vi = XGetVisualInfo(dpy, VisualIDMask, &tmpl, &n);
    }
    for (int depth = 24; !vi && depth <= 32; depth += 8) {
        tmpl.screen = screen;
        tmpl.depth = depth;
        tmpl.class = TrueColor;
        vi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
    }
    return vi;
}

/* ---- GLX ---- */

EXPORT Bool glXQueryExtension(Display *dpy, int *err, int *ev) {
    (void)dpy;
    if (err) *err = 0;
    if (ev) *ev = 0;
    return True;
}

EXPORT Bool glXQueryVersion(Display *dpy, int *maj, int *min) {
    (void)dpy;
    if (maj) *maj = 1;
    if (min) *min = 4;
    return True;
}

EXPORT const char *glXQueryExtensionsString(Display *dpy, int screen) { (void)dpy; (void)screen; return glx_ext; }

EXPORT const char *glXGetClientString(Display *dpy, int name) {
    (void)dpy;
    return name == GLX_VENDOR ? "portmaster-unity glxes" : name == GLX_VERSION ? "1.4" : glx_ext;
}

EXPORT const char *glXQueryServerString(Display *dpy, int screen, int name) { (void)screen; return glXGetClientString(dpy, name); }

static int cfg_attrib(Display *dpy, int attribute, int *value) {
    XVisualInfo *vi;
    EGLint v = 0;
    switch (attribute) {
    case GLX_FBCONFIG_ID: *value = 1; break;
    case GLX_VISUAL_ID:
        vi = visual(dpy, DefaultScreen(dpy));
        *value = vi ? (int)vi->visualid : 0;
        if (vi) XFree(vi);
        break;
    case GLX_RENDER_TYPE: *value = GLX_RGBA_BIT; break;
    case GLX_DRAWABLE_TYPE: *value = GLX_WINDOW_BIT | GLX_PBUFFER_BIT; break;
    case GLX_X_RENDERABLE: case GLX_DOUBLEBUFFER: case GLX_USE_GL: case GLX_RGBA: *value = 1; break;
    case GLX_X_VISUAL_TYPE: *value = GLX_TRUE_COLOR; break;
    case GLX_RED_SIZE: case GLX_GREEN_SIZE: case GLX_BLUE_SIZE: *value = 8; break;
    case GLX_ALPHA_SIZE: case GLX_DEPTH_SIZE: case GLX_STENCIL_SIZE: case GLX_BUFFER_SIZE:
        if (egl_open(dpy))
            eglGetConfigAttrib(edpy, econfig, attribute == GLX_ALPHA_SIZE ? EGL_ALPHA_SIZE : attribute == GLX_DEPTH_SIZE ?
                               EGL_DEPTH_SIZE : attribute == GLX_STENCIL_SIZE ? EGL_STENCIL_SIZE : EGL_BUFFER_SIZE, &v);
        *value = v;
        break;
    case GLX_CONFIG_CAVEAT: case GLX_TRANSPARENT_TYPE: *value = GLX_NONE; break;
    case GLX_MAX_PBUFFER_WIDTH: case GLX_MAX_PBUFFER_HEIGHT: *value = 4096; break;
    default: *value = 0;
    }
    return 0;
}

EXPORT GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, const int *attr, int *n) {
    (void)dpy; (void)screen; (void)attr;
    GLXFBConfig *l = malloc(sizeof *l);
    l[0] = &cfgs[0];
    *n = 1;
    return l;
}

EXPORT GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *n) { return glXChooseFBConfig(dpy, screen, NULL, n); }
EXPORT int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig c, int a, int *v) { (void)c; return cfg_attrib(dpy, a, v); }
EXPORT XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig c) { (void)c; return visual(dpy, DefaultScreen(dpy)); }
EXPORT XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attr) { (void)attr; return visual(dpy, screen); }
EXPORT int glXGetConfig(Display *dpy, XVisualInfo *vi, int a, int *v) { (void)vi; return cfg_attrib(dpy, a, v); }

static GLXContext make_context(Display *dpy, GLXContext share, const int *attr) {
    if (!egl_open(dpy))
        return NULL;
    int major = 3, minor = 0, es = 0;
    for (const int *a = attr; a && a[0]; a += 2) {
        if (a[0] == GLX_CONTEXT_MAJOR_VERSION_ARB) major = a[1];
        else if (a[0] == GLX_CONTEXT_MINOR_VERSION_ARB) minor = a[1];
        else if (a[0] == GLX_CONTEXT_PROFILE_MASK_ARB) es = (a[1] & GLX_CONTEXT_ES_PROFILE_BIT_EXT) != 0;
    }
    if (!es)
        major = 3, minor = 0;                           /* the probe: any ES 3 context answers it */
    EGLint cattr[] = {EGL_CONTEXT_MAJOR_VERSION, major, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
    struct glxes_ctx *c = calloc(1, sizeof *c);
    c->es = es;
    c->egl = eglCreateContext(edpy, econfig, share ? share->egl : EGL_NO_CONTEXT, cattr);
    if (c->egl == EGL_NO_CONTEXT && minor) {           /* EGL 1.4 without KHR_create_context: major only */
        cattr[2] = EGL_NONE;
        c->egl = eglCreateContext(edpy, econfig, share ? share->egl : EGL_NO_CONTEXT, cattr);
    }
    if (c->egl == EGL_NO_CONTEXT) {
        LOG("eglCreateContext ES %d.%d failed (0x%x)\n", major, minor, eglGetError());
        free(c);
        return NULL;
    }
    EGLint pattr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    c->pbuf = eglCreatePbufferSurface(edpy, econfig, pattr);
    if (es)
        LOG("context %p: OpenGL ES %d.%d\n", (void *)c, major, minor);
    else
        LOG("context %p: answers as OpenGL %d.%d core (an ES context)\n", (void *)c, ver_major, ver_minor);
    return c;
}

EXPORT GLXContext glXCreateContextAttribsARB(Display *dpy, GLXFBConfig c, GLXContext share, Bool direct, const int *attr) {
    (void)c; (void)direct;
    return make_context(dpy, share, attr);
}

EXPORT GLXContext glXCreateNewContext(Display *dpy, GLXFBConfig c, int type, GLXContext share, Bool direct) {
    (void)c; (void)type; (void)direct;
    return make_context(dpy, share, NULL);
}

EXPORT GLXContext glXCreateContext(Display *dpy, XVisualInfo *vi, GLXContext share, Bool direct) {
    (void)vi; (void)direct;
    return make_context(dpy, share, NULL);
}

/* The EGL window surface of a GLX drawable (made on first use); EGL_NO_SURFACE for pbuffer ids */
static EGLSurface surface_of(GLXDrawable d) {
    if (!d)
        return EGL_NO_SURFACE;
    pthread_mutex_lock(&lock);
    struct surf *s = surfs;
    while (s && s->draw != d)
        s = s->next;
    if (!s) {
        s = calloc(1, sizeof *s);
        s->draw = d;
        s->egl = eglCreateWindowSurface(edpy, econfig, (unsigned long)d, NULL);
        if (s->egl == EGL_NO_SURFACE)
            LOG("eglCreateWindowSurface(0x%lx) failed (0x%x)\n", (unsigned long)d, eglGetError());
        else
            LOG("window 0x%lx: EGL surface %p\n", (unsigned long)d, s->egl);
        s->next = surfs;
        surfs = s;
    }
    pthread_mutex_unlock(&lock);
    return s->egl;
}

EXPORT Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx) {
    if (!ctx) {
        if (edpy != EGL_NO_DISPLAY)
            eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        current = NULL, current_dpy = NULL, current_draw = 0;
        return True;
    }
    EGLSurface d = surface_of(draw), r = read == draw ? d : surface_of(read);
    if (d == EGL_NO_SURFACE)
        d = r = ctx->pbuf;
    if (!eglMakeCurrent(edpy, d, r, ctx->egl)) {
        LOG("eglMakeCurrent failed (0x%x)\n", eglGetError());
        return False;
    }
    current = ctx, current_dpy = dpy, current_draw = draw;
    return True;
}

EXPORT Bool glXMakeCurrent(Display *dpy, GLXDrawable draw, GLXContext ctx) { return glXMakeContextCurrent(dpy, draw, draw, ctx); }

EXPORT void glXDestroyContext(Display *dpy, GLXContext ctx) {
    if (!ctx)
        return;
    if (ctx == current)
        glXMakeContextCurrent(dpy, 0, 0, NULL);
    if (ctx->pbuf)
        eglDestroySurface(edpy, ctx->pbuf);
    eglDestroyContext(edpy, ctx->egl);
    free(ctx);
}

static void count_frame(void) {
    static FILE *f;
    static int frames, off;
    static struct timespec t0;
    struct timespec t;
    if (off)
        return;
    if (!f) {
        const char *p = getenv("GLXES_FPS");
        if (!p || !*p || !(f = fopen(p, "a"))) {
            off = 1;
            return;
        }
        clock_gettime(CLOCK_MONOTONIC, &t0);
    }
    frames++;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double dt = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
    if (dt >= 1.0) {
        fprintf(f, "%.2f\n", frames / dt);
        fflush(f);
        frames = 0;
        t0 = t;
    }
}

EXPORT void glXSwapBuffers(Display *dpy, GLXDrawable d) {
    (void)dpy;
    EGLSurface s = surface_of(d);
    if (s != EGL_NO_SURFACE) {
        eglSwapBuffers(edpy, s);
        count_frame();
    }
}

EXPORT GLXContext glXGetCurrentContext(void) { return current; }
EXPORT Display *glXGetCurrentDisplay(void) { return current_dpy; }
EXPORT GLXDrawable glXGetCurrentDrawable(void) { return current_draw; }
EXPORT GLXDrawable glXGetCurrentReadDrawable(void) { return current_draw; }
EXPORT Bool glXIsDirect(Display *dpy, GLXContext ctx) { (void)dpy; (void)ctx; return True; }
EXPORT void glXWaitGL(void) {}
EXPORT void glXWaitX(void) {}
EXPORT GLXWindow glXCreateWindow(Display *dpy, GLXFBConfig c, Window w, const int *a) { (void)dpy; (void)c; (void)a; return w; }
EXPORT void glXDestroyWindow(Display *dpy, GLXWindow w) { (void)dpy; (void)w; }
EXPORT GLXPbuffer glXCreatePbuffer(Display *dpy, GLXFBConfig c, const int *a) { (void)dpy; (void)c; (void)a; return 0; }
EXPORT void glXDestroyPbuffer(Display *dpy, GLXPbuffer p) { (void)dpy; (void)p; }
EXPORT int glXQueryContext(Display *dpy, GLXContext c, int a, int *v) { (void)c; return cfg_attrib(dpy, a, v); }
EXPORT void glXSwapIntervalEXT(Display *dpy, GLXDrawable d, int i) { (void)dpy; (void)d; if (edpy) eglSwapInterval(edpy, i); }
EXPORT int glXSwapIntervalMESA(unsigned int i) { if (edpy) eglSwapInterval(edpy, (EGLint)i); return 0; }
EXPORT int glXSwapIntervalSGI(int i) { if (edpy) eglSwapInterval(edpy, i); return 0; }

EXPORT void glXQueryDrawable(Display *dpy, GLXDrawable d, int a, unsigned int *v) {
    (void)dpy;
    EGLint x = 0;
    EGLSurface s = surface_of(d);
    if (v)
        *v = 0;
    if (s != EGL_NO_SURFACE && (a == GLX_WIDTH || a == GLX_HEIGHT) &&
        eglQuerySurface(edpy, s, a == GLX_WIDTH ? EGL_WIDTH : EGL_HEIGHT, &x) && v)
        *v = (unsigned)x;
}

/* ---- GL: the probe's version answers; everything else is the GLES driver's ---- */

EXPORT const GLubyte *glGetString(GLenum name) {
    setup();
    es_load();
    if (current && !current->es) {
        if (name == GL_VERSION) {
            if (!version_string[0])
                snprintf(version_string, sizeof version_string, "%d.%d (Core Profile) glxes on %s", ver_major, ver_minor,
                         es_GetString ? (const char *)es_GetString(GL_VERSION) : "OpenGL ES");
            return (const GLubyte *)version_string;
        }
        if (name == GL_SHADING_LANGUAGE_VERSION)
            return (const GLubyte *)glsl_string;
    }
    return es_GetString ? es_GetString(name) : NULL;
}

EXPORT void glGetIntegerv(GLenum name, GLint *v) {
    setup();
    es_load();
    if (current && !current->es) {
        switch (name) {
        case GL_MAJOR_VERSION: *v = ver_major; return;
        case GL_MINOR_VERSION: *v = ver_minor; return;
        case GL_CONTEXT_PROFILE_MASK: *v = GL_CONTEXT_CORE_PROFILE_BIT; return;
        case GL_CONTEXT_FLAGS: *v = 0; return;
        }
    }
    if (es_GetIntegerv)
        es_GetIntegerv(name, v);
}

EXPORT fnptr glXGetProcAddressARB(const GLubyte *name) {
    const char *n = (const char *)name;
    es_load();
    fnptr f = self ? (fnptr)dlsym(self, n) : NULL;     /* ours first, then the GLES driver's */
    if (!f && gles)
        f = (fnptr)dlsym(gles, n);
    if (!f)
        f = (fnptr)eglGetProcAddress(n);
    LOG("glXGetProcAddress(%s) = %p\n", n, (void *)f);
    return f;
}

EXPORT fnptr glXGetProcAddress(const GLubyte *name) { return glXGetProcAddressARB(name); }
