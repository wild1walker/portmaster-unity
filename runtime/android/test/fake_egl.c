// A fake libEGL for testing egl_glue without a GPU: records what the wrappers pass through.
#include <stdint.h>
#include <string.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
struct fbdev_window { unsigned short width, height; };

void *fake_last_window;
struct fbdev_window fake_last_fbdev;
EGLint fake_last_config_attribs[64];
int fake_swaps;
int fake_surface_calls;
static EGLint last_error = 0x3000;

EGLint eglGetError(void) { EGLint e = last_error; last_error = 0x3000; return e; }

EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, void *win, const EGLint *attribs)
{
    fake_surface_calls++;
    if (attribs && attribs[0] != 0x3038) { last_error = 0x3004; return NULL; }  // rejects any attribute
    fake_last_window = win;
    if ((uintptr_t)win > 0x10000) fake_last_fbdev = *(struct fbdev_window *)win;   // tests pass 0xabc as a "host" window
    return (EGLSurface)0x5f;
}

EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *cfgs, EGLint size, EGLint *num)
{
    int i = 0;
    for (; a && a[i] != 0x3038 && i < 62; i += 2) { fake_last_config_attribs[i] = a[i]; fake_last_config_attribs[i + 1] = a[i + 1]; }
    fake_last_config_attribs[i] = 0x3038;
    if (cfgs && size > 0) cfgs[0] = (EGLConfig)0x1;
    if (num) *num = 1;
    return 1;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr, EGLint *v)
{
    if (attr >= 0x3140 && attr <= 0x314F) { last_error = 0x3004; return 0; }
    *v = 42;
    return 1;
}

void *fake_last_context_config;
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *a)
{
    fake_last_context_config = c;
    for (int i = 0; a && a[i] != 0x3038; i += 2)
        if (a[i] != 0x3098) { last_error = 0x3004; return NULL; }
    return (EGLContext)0xc0;
}

EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) { fake_swaps++; return 1; }

EGLBoolean eglFakeOnly(void) { return 7; }

// What the host-display wrappers pass through (or must not)
int fake_terminates, fake_make_currents, fake_destroyed_surfaces, fake_interval = -1;
EGLDisplay eglGetDisplay(void *native) { return (EGLDisplay)0xd0; }
EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    if (major) *major = 1;
    if (minor) *minor = 4;
    return 1;
}
EGLBoolean eglTerminate(EGLDisplay d) { fake_terminates++; return 1; }
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c) { fake_make_currents++; return 1; }
EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) { fake_destroyed_surfaces++; return 1; }
EGLBoolean eglSwapInterval(EGLDisplay d, EGLint n) { fake_interval = n; return 1; }
void *fake_current_draw, *fake_last_query_surface;
EGLSurface eglGetCurrentSurface(EGLint which) { return fake_current_draw; }
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *v)
{
    fake_last_query_surface = s;
    if (attr == 0x309D) { *v = 0x308A; return 1; }   // EGL_GL_COLORSPACE_KHR: linear
    if (attr == 0x3057) { *v = 77; return 1; }       // EGL_WIDTH
    last_error = 0x3004;
    return 0;
}

// eglSurfaceAttrib: the surface, attribute and value of the last call; EGL_SWAP_BEHAVIOR (0x3093) only
int fake_attrib_calls;
void *fake_attrib_surface;
EGLint fake_attrib_attr, fake_attrib_value;
EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint attr, EGLint value)
{
    fake_attrib_calls++;
    fake_attrib_surface = s;
    fake_attrib_attr = attr;
    fake_attrib_value = value;
    if (attr != 0x3093) { last_error = 0x3004; return 0; }
    return 1;
}

// a driver that hands out Android's frame-timing entry points (Mali's might)
EGLBoolean eglPresentationTimeANDROID(EGLDisplay d, EGLSurface s, int64_t t) { return 1; }

void (*eglGetProcAddress(const char *name))(void)
{
    if (!strcmp(name, "eglFakeOnly")) return (void (*)(void))eglFakeOnly;
    if (!strcmp(name, "eglPresentationTimeANDROID")) return (void (*)(void))eglPresentationTimeANDROID;
    return NULL;
}
