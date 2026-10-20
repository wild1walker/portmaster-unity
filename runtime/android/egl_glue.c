// EGL glue between Unity's Android GLES backend and the host's real libEGL (Mali fbdev on the
// device, Mesa in tests). Only calls that need translation are wrapped (egl_shims[]); the wrappers
// call the real functions from a dlopen'ed libEGL. Everything else Unity imports from libEGL can be
// resolved straight from the host libEGL (or via egl_glue_real()).
//
//   eglCreateWindowSurface  ANativeWindow* -> fbdev_window* (or a host-provided native window);
//                           retried without attributes if the driver rejects Android-only ones
//   eglChooseConfig         drops Android-only attributes (EGL_RECORDABLE_ANDROID, ...)
//   eglGetConfigAttrib      answers Android-only attributes with 0 instead of EGL_BAD_ATTRIBUTE
//   eglCreateContext        retried with only EGL_CONTEXT_CLIENT_VERSION on EGL_BAD_ATTRIBUTE
//   eglSwapBuffers          optional host hook before each swap (input pumping, pacing, stats)
//   UA_EGL_PBUFFER=1        tests without a display (Mesa's surfaceless platform): the window surface
//                           is a pbuffer of the window's size, configs are chosen for pbuffers
//   eglGetProcAddress       returns the wrappers for the wrapped names, else the real function
//                           passed through the loader's so_softfp_wrap() (float-taking GL calls)
// With a host display (egl_glue_set_host_display, egl_host_shims[]: an SDL2 window on Wayland,
// KMSDRM, X11, ...), Unity's display, config, window surface, current-making, swaps and swap
// interval are mapped onto the host's EGL objects instead (see egl_glue.h).
#define _GNU_SOURCE
#include "egl_glue.h"
#include "android_shims.h"
#include "shim_internal.h"

#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

// Own minimal EGL declarations (avoids eglplatform.h's X11 dependency; every EGLNative*Type is one
// pointer-sized word on 32- and 64-bit ARM, so void * is ABI-compatible with fbdev, X11, Wayland,
// GBM and Android).
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef void (*eglproc)(void);

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_SUCCESS 0x3000
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_MATCH 0x3009
#define EGL_BAD_NATIVE_WINDOW 0x300B
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_GL_COLORSPACE_KHR 0x309D
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_GL_COLORSPACE_SRGB_KHR 0x3089
#define EGL_DRAW 0x3059
// Android-only config attributes (EGL_ANDROID_* extensions)
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_RECORDABLE_ANDROID 0x3142
#define EGL_FRAMEBUFFER_TARGET_ANDROID 0x3147
#define EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID 0x314C

static struct {
    void *lib;
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
    EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
    eglproc (*GetProcAddress)(const char *);
    EGLint (*GetError)(void);
    // used with a host display only
    EGLDisplay (*GetDisplay)(void *);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*Terminate)(EGLDisplay);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
    EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    EGLSurface (*GetCurrentSurface)(EGLint);
} real;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void (*g_swap_hook)(void *ctx);
static void *g_swap_ctx;

// The loader's softfp wrapper for float-taking GL entry points (loader/so_loader.h); weak so the
// glue also links without the loader (tests).
void *so_softfp_wrap(const char *name, void *fn) __attribute__((weak));
void *so_shim_lookup(const char *name) __attribute__((weak));

static void load_real(void)
{
    const char *names[] = { getenv("ANDROID_SHIM_LIBEGL"), "libEGL.so.1", "libEGL.so", NULL };
    for (int i = 0; i < 3 && !real.lib; i++) {
        if (names[i] && *names[i]) real.lib = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    }
    if (!real.lib) {
        SHIM_WARN("egl_glue: cannot dlopen libEGL: %s", dlerror());
        return;
    }
#define R(f) *(void **)&real.f = dlsym(real.lib, "egl" #f)
    R(CreateWindowSurface); R(ChooseConfig); R(GetConfigAttrib); R(CreateContext);
    R(SwapBuffers); R(GetProcAddress); R(GetError); R(CreatePbufferSurface);
    R(GetDisplay); R(Initialize); R(Terminate); R(MakeCurrent); R(DestroySurface); R(SwapInterval);
    R(QuerySurface); R(GetCurrentSurface);
#undef R
}

static int ensure(void)
{
    pthread_once(&g_once, load_real);
    return real.lib != NULL;
}

void *egl_glue_real(const char *name)
{
    if (!ensure() || !name) return NULL;
    void *p = dlsym(real.lib, name);
    if (!p && real.GetProcAddress) p = (void *)real.GetProcAddress(name);
    return p;
}

void egl_glue_set_swap_hook(void (*hook)(void *ctx), void *ctx)
{
    g_swap_ctx = ctx;
    g_swap_hook = hook;
}

static int is_android_attr(EGLint a)
{
    return a >= EGL_NATIVE_BUFFER_ANDROID && a <= 0x314F;
}

static void trace_attribs(const char *what, const EGLint *a)
{
    if (!shim_trace_enabled()) return;
    char buf[512];
    int n = 0;
    for (; a && a[0] != EGL_NONE && n < (int)sizeof buf - 32; a += 2)
        n += snprintf(buf + n, sizeof buf - n, " 0x%x=%d", a[0], a[1]);
    buf[n] = 0;
    shim_log("trace", "%s:%s", what, n ? buf : " (none)");
}

static int pbuffer_mode(void)
{
    static int m = -1;
    if (m < 0) { const char *e = getenv("UA_EGL_PBUFFER"); m = e && *e && *e != '0'; }
    return m;
}

static EGLSurface shim_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, void *win, const EGLint *attribs)
{
    if (!ensure() || !real.CreateWindowSurface) return NULL;
    void *native = win;
    if (android_window_is_ours(win)) native = android_window_native_handle(win);
    else if (win) SHIM_WARN("egl_glue: eglCreateWindowSurface with a foreign window %p, passed through", win);
    trace_attribs("eglCreateWindowSurface attribs", attribs);
    if (pbuffer_mode() && real.CreatePbufferSurface) {
        ANativeWindow *w = android_window_get();
        EGLint pa[] = { EGL_WIDTH, ANativeWindow_getWidth(w), EGL_HEIGHT, ANativeWindow_getHeight(w), EGL_NONE };
        EGLSurface ps = real.CreatePbufferSurface(dpy, cfg, pa);
        SHIM_WARN("egl_glue: window surface replaced by a %dx%d pbuffer (UA_EGL_PBUFFER): %p", pa[1], pa[3], ps);
        return ps;
    }
    EGLSurface s = real.CreateWindowSurface(dpy, cfg, native, attribs);
    if (!s && attribs && attribs[0] != EGL_NONE) {
        EGLint err = real.GetError ? real.GetError() : 0;
        SHIM_WARN("egl_glue: eglCreateWindowSurface failed (0x%x), retrying without attributes", err);
        s = real.CreateWindowSurface(dpy, cfg, native, NULL);
    }
    SHIM_TRACE("eglCreateWindowSurface(%p -> native %p) = %p", win, native, s);
    return s;
}

static EGLBoolean shim_eglChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *cfgs, EGLint size, EGLint *num)
{
    if (!ensure() || !real.ChooseConfig) return EGL_FALSE;
    trace_attribs("eglChooseConfig attribs", attribs);
    EGLint filtered[128];
    const EGLint *use = attribs;
    if (attribs) {
        int n = 0;
        for (const EGLint *a = attribs; a[0] != EGL_NONE && n < 126; a += 2) {
            if (is_android_attr(a[0])) { SHIM_TRACE("eglChooseConfig: dropping attribute 0x%x", a[0]); continue; }
            filtered[n++] = a[0];
            filtered[n++] = a[0] == EGL_SURFACE_TYPE && pbuffer_mode() ? EGL_PBUFFER_BIT : a[1];
        }
        filtered[n] = EGL_NONE;
        use = filtered;
    }
    EGLBoolean r = real.ChooseConfig(dpy, use, cfgs, size, num);
    SHIM_TRACE("eglChooseConfig -> %u, %d configs", r, num ? *num : -1);
    return r;
}

static EGLBoolean shim_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig cfg, EGLint attr, EGLint *value)
{
    if (!ensure() || !real.GetConfigAttrib) return EGL_FALSE;
    if (is_android_attr(attr)) {
        if (value) *value = 0;
        return EGL_TRUE;
    }
    return real.GetConfigAttrib(dpy, cfg, attr, value);
}

static EGLContext shim_eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attribs)
{
    if (!ensure() || !real.CreateContext) return NULL;
    trace_attribs("eglCreateContext attribs", attribs);
    EGLContext c = real.CreateContext(dpy, cfg, share, attribs);
    if (!c && attribs && real.GetError && real.GetError() == EGL_BAD_ATTRIBUTE) {
        EGLint version = 0;
        for (const EGLint *a = attribs; a[0] != EGL_NONE; a += 2)
            if (a[0] == EGL_CONTEXT_CLIENT_VERSION) version = a[1];
        if (version) {
            EGLint only[] = { EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE };
            SHIM_WARN("egl_glue: eglCreateContext rejected attributes, retrying with client version %d", version);
            c = real.CreateContext(dpy, cfg, share, only);
        }
    }
    SHIM_TRACE("eglCreateContext -> %p", c);
    return c;
}

static EGLBoolean shim_eglSwapBuffers(EGLDisplay dpy, EGLSurface s)
{
    if (!ensure() || !real.SwapBuffers) return EGL_FALSE;
    void (*hook)(void *) = g_swap_hook;
    if (hook) hook(g_swap_ctx);
    return real.SwapBuffers(dpy, s);
}

static eglproc shim_eglGetProcAddress(const char *name);

so_shim egl_shims[] = {
    { "eglCreateWindowSurface", (void *)shim_eglCreateWindowSurface },
    { "eglChooseConfig", (void *)shim_eglChooseConfig },
    { "eglGetConfigAttrib", (void *)shim_eglGetConfigAttrib },
    { "eglCreateContext", (void *)shim_eglCreateContext },
    { "eglSwapBuffers", (void *)shim_eglSwapBuffers },
    { "eglGetProcAddress", (void *)shim_eglGetProcAddress },
    { NULL, NULL },
};

static eglproc shim_eglGetProcAddress(const char *name)
{
    if (!name) return NULL;
    for (so_shim *s = egl_shims; s->name; s++)
        if (!strcmp(s->name, name)) return (eglproc)s->fn;
    // entry points the host replaces (loader shim tables), e.g. glShaderSource for shader overrides
    if (so_shim_lookup) { void *o = so_shim_lookup(name); if (o) return (eglproc)o; }
    if (!ensure() || !real.GetProcAddress) return NULL;
    eglproc p = real.GetProcAddress(name);
    if (!p) SHIM_TRACE("eglGetProcAddress(%s) = NULL", name);
    // GL functions taking floats need a softfp->hardfp thunk for Unity's armeabi-v7a code.
    if (p && so_softfp_wrap) p = (eglproc)so_softfp_wrap(name, (void *)p);
    return p;
}

// ---- host display (egl_glue.h) -------------------------------------------------------------------
// Set once before Unity starts (its threads are created afterwards), read-only from then on.
static egl_host_display g_host;
static int g_host_set;

// Unity's handle for the host's window surface: a token that never changes, because the window
// system's surface may (SDL's KMSDRM driver recreates it inside SDL_GL_SwapWindow after a fullscreen
// switch or a VT switch). Mapped to the real surface wherever Unity passes or gets one back.
static char g_host_token;
#define HOST_SURFACE ((EGLSurface)&g_host_token)
static __thread EGLSurface t_host_real;   // the host's real surface as this thread has it current
static EGLSurface g_host_real;            // the latest one any thread saw (atomic)

void egl_glue_set_host_display(const egl_host_display *d)
{
    if (d && d->display && d->config && d->surface && d->make_current && d->swap) {
        g_host = *d;
        g_host_real = d->surface;
        g_host_set = 1;
        SHIM_TRACE("host display %p: config %p, surface %p", d->display, d->config, d->surface);
    } else {
        memset(&g_host, 0, sizeof g_host);
        g_host_real = NULL;
        g_host_set = 0;
    }
}

static int host_dpy(EGLDisplay d) { return g_host_set && d == g_host.display; }

static EGLSurface host_real_surface(void)
{
    EGLSurface s = t_host_real;
    return s ? s : __atomic_load_n(&g_host_real, __ATOMIC_ACQUIRE);
}

// After make_current or a swap: the surface the host has current now.
static void host_track_current(void)
{
    EGLSurface s = real.GetCurrentSurface ? real.GetCurrentSurface(EGL_DRAW) : NULL;
    t_host_real = s;
    if (s) __atomic_store_n(&g_host_real, s, __ATOMIC_RELEASE);
}

static EGLDisplay host_eglGetDisplay(void *native)
{
    if (!g_host_set) return ensure() && real.GetDisplay ? real.GetDisplay(native) : NULL;
    SHIM_TRACE("eglGetDisplay(%p) = host display %p", native, g_host.display);
    return g_host.display;
}

// The host initialized its display: a second eglInitialize only reports the version.
static EGLBoolean host_eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    if (!ensure() || !real.Initialize) return EGL_FALSE;
    return real.Initialize(d, major, minor);
}

// Unity's teardown must not terminate the display the host's window lives on.
static EGLBoolean host_eglTerminate(EGLDisplay d)
{
    if (host_dpy(d)) { SHIM_TRACE("eglTerminate(host display): kept"); return EGL_TRUE; }
    return ensure() && real.Terminate ? real.Terminate(d) : EGL_FALSE;
}

// Only the host's config: its surface was created with it, and Mesa (among others) wants a
// context's config to be the surface's. Unity checks it with eglGetConfigAttrib like any other
// and falls back to its closest match when it is not what it asked for.
static EGLBoolean host_eglChooseConfig(EGLDisplay d, const EGLint *attribs, EGLConfig *cfgs, EGLint size, EGLint *num)
{
    if (!host_dpy(d)) return shim_eglChooseConfig(d, attribs, cfgs, size, num);
    trace_attribs("eglChooseConfig attribs (host config returned)", attribs);
    if (!num) return EGL_FALSE;
    *num = 0;
    if (!cfgs) *num = 1;
    else if (size > 0) { cfgs[0] = g_host.config; *num = 1; }
    return EGL_TRUE;
}

static EGLContext host_eglCreateContext(EGLDisplay d, EGLConfig cfg, EGLContext share, const EGLint *attribs)
{
    if (!host_dpy(d)) return shim_eglCreateContext(d, cfg, share, attribs);
    if (cfg != g_host.config) SHIM_TRACE("eglCreateContext: config %p -> host config %p", cfg, g_host.config);
    return shim_eglCreateContext(d, g_host.config, share, attribs);
}

static EGLSurface host_eglCreateWindowSurface(EGLDisplay d, EGLConfig cfg, void *win, const EGLint *attribs)
{
    if (!host_dpy(d)) return shim_eglCreateWindowSurface(d, cfg, win, attribs);
    ensure();
    trace_attribs("eglCreateWindowSurface attribs (host surface returned)", attribs);
    if (win && !android_window_is_ours(win)) SHIM_WARN("egl_glue: eglCreateWindowSurface with a foreign window %p", win);
    // A linear-color-space game asks for an sRGB window surface; the host's is created up front.
    static int warned;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE && !warned; a += 2)
        if (a[0] == EGL_GL_COLORSPACE_KHR && a[1] == EGL_GL_COLORSPACE_SRGB_KHR) {
            EGLint cs = 0;
            if (!real.QuerySurface || !real.QuerySurface(d, host_real_surface(), EGL_GL_COLORSPACE_KHR, &cs)) cs = 0;
            if (real.GetError) real.GetError();
            if (cs != EGL_GL_COLORSPACE_SRGB_KHR) {
                SHIM_WARN("egl_glue: Unity asks for an sRGB window surface (linear color space) but the host's is not "
                          "(set UA_DISPLAY_SRGB=1)");
                warned = 1;
            }
        }
    return HOST_SURFACE;
}

static EGLBoolean host_eglDestroySurface(EGLDisplay d, EGLSurface s)
{
    if (host_dpy(d) && s == HOST_SURFACE) { SHIM_TRACE("eglDestroySurface(host surface): kept"); return EGL_TRUE; }
    return ensure() && real.DestroySurface ? real.DestroySurface(d, s) : EGL_FALSE;
}

static EGLBoolean host_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    if (!ensure() || !real.MakeCurrent) return EGL_FALSE;
    if (!host_dpy(d)) return real.MakeCurrent(d, draw, read, ctx);
    if (ctx && draw == HOST_SURFACE && read == HOST_SURFACE) {
        if (g_host.make_current(g_host.user, ctx) != 0) return EGL_FALSE;
        host_track_current();
        return EGL_TRUE;
    }
    // a release or other surfaces: the host first forgets what it made current on this thread
    g_host.make_current(g_host.user, NULL);
    t_host_real = NULL;
    if (draw == HOST_SURFACE) draw = host_real_surface();
    if (read == HOST_SURFACE) read = host_real_surface();
    return real.MakeCurrent(d, draw, read, ctx);
}

// Unity's IsCurrentInternal compares this with its handle before every present.
static EGLSurface host_eglGetCurrentSurface(EGLint which)
{
    if (!ensure() || !real.GetCurrentSurface) return NULL;
    EGLSurface s = real.GetCurrentSurface(which);
    return g_host_set && s && s == t_host_real ? HOST_SURFACE : s;
}

static EGLBoolean host_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *value)
{
    if (!ensure() || !real.QuerySurface) return EGL_FALSE;
    if (host_dpy(d) && s == HOST_SURFACE) s = host_real_surface();
    return real.QuerySurface(d, s, attr, value);
}

static EGLBoolean host_eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    if (!ensure() || !real.SwapBuffers) return EGL_FALSE;
    void (*hook)(void *) = g_swap_hook;
    if (hook) hook(g_swap_ctx);
    if (host_dpy(d) && s == HOST_SURFACE) {
        if (g_host.swap(g_host.user) == 0) { host_track_current(); return EGL_TRUE; }
        s = host_real_surface();
    }
    return real.SwapBuffers(d, s);
}

static EGLBoolean host_eglSwapInterval(EGLDisplay d, EGLint interval)
{
    if (host_dpy(d) && g_host.swap_interval)
        return g_host.swap_interval(g_host.user, interval) == 0 ? EGL_TRUE : EGL_FALSE;
    return ensure() && real.SwapInterval ? real.SwapInterval(d, interval) : EGL_FALSE;
}

static eglproc host_eglGetProcAddress(const char *name);

so_shim egl_host_shims[] = {
    { "eglGetDisplay", (void *)host_eglGetDisplay },
    { "eglInitialize", (void *)host_eglInitialize },
    { "eglTerminate", (void *)host_eglTerminate },
    { "eglChooseConfig", (void *)host_eglChooseConfig },
    { "eglCreateContext", (void *)host_eglCreateContext },
    { "eglCreateWindowSurface", (void *)host_eglCreateWindowSurface },
    { "eglDestroySurface", (void *)host_eglDestroySurface },
    { "eglMakeCurrent", (void *)host_eglMakeCurrent },
    { "eglGetCurrentSurface", (void *)host_eglGetCurrentSurface },
    { "eglQuerySurface", (void *)host_eglQuerySurface },
    { "eglSwapBuffers", (void *)host_eglSwapBuffers },
    { "eglSwapInterval", (void *)host_eglSwapInterval },
    { "eglGetProcAddress", (void *)host_eglGetProcAddress },
    { NULL, NULL },
};

// What an import of the same name resolves to: with the loader, its shim tables in registration
// order (host replacements, egl_host_shims, egl_shims, ...); without it (tests), this table first.
static eglproc host_eglGetProcAddress(const char *name)
{
    if (!name) return NULL;
    if (so_shim_lookup) { void *o = so_shim_lookup(name); if (o) return (eglproc)o; }
    for (so_shim *s = egl_host_shims; s->name; s++)
        if (!strcmp(s->name, name)) return (eglproc)s->fn;
    return shim_eglGetProcAddress(name);
}
