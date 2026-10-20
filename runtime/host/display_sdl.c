// SDL2 display backend (display.h): Unity draws into an SDL window, so whatever SDL drives works:
// Wayland compositors, KMSDRM without one, X11 (through EGL), SDL's offscreen driver.
//
// On the main thread, before Unity starts: an SDL window (SDL_WINDOW_OPENGL, fullscreen desktop,
// high-DPI drawable) and a GLES context (3.0, else 2.0; RGBA8888, depth 24, stencil 8, with
// weaker fallbacks). With SDL's context current we read the EGL objects SDL created (display,
// draw surface and its config), set vsync, present one black frame (KMSDRM sets the mode on the
// first flip; a Wayland window is mapped on its first buffer), and release the context. egl_glue
// then maps Unity's EGL calls onto those objects (egl_glue.h); Unity's surface handle is a stable
// token for whichever surface SDL has current (KMSDRM recreates it after fullscreen and VT
// switches). On Unity's render thread:
//   eglMakeCurrent(host surface, ctx) -> SDL_GL_MakeCurrent(window, ctx): any context made with
//       SDL's display and config works, and SDL's per-thread "current window" stays right
//   eglSwapBuffers -> SDL_GL_SwapWindow (KMSDRM flips pages there; newer SDLs pace Wayland
//       swaps with frame callbacks there)
//   eglSwapInterval -> SDL_GL_SetSwapInterval (SDL keeps its own copy for Wayland and KMSDRM)
// The main thread keeps pumping SDL events (input_poll), which Wayland needs. SDL_QUIT (the
// window closed) is seen by an event watch and ends the host's main loop.
//
//   UA_DISPLAY_WINDOW=WxH   a WxH window instead of fullscreen (desktop tests)
//   UA_DISPLAY_SAMPLES=<n>  multisampled window surface (a game whose quality settings use MSAA:
//                           Unity on Android renders MSAA into the window surface)
//   UA_DISPLAY_SRGB=1       sRGB window surface (linear color space games; egl_glue warns when
//                           Unity asks for one and this is off)
//   UA_SWAP_INTERVAL=<n>    initial (and forced) swap interval; default 1, EGL's own default
// SDL's own variables apply (SDL_VIDEODRIVER=wayland|kmsdrm|x11|offscreen, SDL_VIDEO_EGL_DRIVER).
#define _GNU_SOURCE
#include "display.h"

#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;

#define EGL_NONE 0x3038
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONFIG_ID 0x3028
#define EGL_SAMPLES 0x3031
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_DRAW 0x3059
#define EGL_GL_COLORSPACE_KHR 0x309D
#define EGL_GL_COLORSPACE_SRGB_KHR 0x3089
#define EGL_OPENGL_ES3_BIT 0x40
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

static struct {
    EGLDisplay (*GetCurrentDisplay)(void);
    EGLSurface (*GetCurrentSurface)(EGLint);
    EGLContext (*GetCurrentContext)(void);
    EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    const char *(*QueryString)(EGLDisplay, EGLint);
    EGLint (*GetError)(void);
} E;

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static volatile int g_quit;

// ---- egl_host_display callbacks (Unity's threads) ----------------------------------------------
static int sdl_make_current(void *user, void *ctx)
{
    (void)user;
    return SDL_GL_MakeCurrent(g_win, (SDL_GLContext)ctx);   // NULL: release (SDL then forgets the window)
}

static int sdl_swap(void *user)
{
    (void)user;
    if (SDL_GL_GetCurrentWindow() != g_win) return -1;      // not made current through SDL here
    SDL_GL_SwapWindow(g_win);
    return 0;
}

static int sdl_swap_interval(void *user, int interval)
{
    (void)user;
    const char *e = getenv("UA_SWAP_INTERVAL");
    if (e && *e) interval = atoi(e);
    int r = SDL_GL_SetSwapInterval(interval);
    if (r != 0) fprintf(stderr, "[display] swap interval %d: %s\n", interval, SDL_GetError());
    return r;
}

static int SDLCALL on_event(void *user, SDL_Event *e)
{
    (void)user;
    if (e->type == SDL_QUIT) {
        if (!g_quit) fprintf(stderr, "[display] quit requested (window closed)\n");
        g_quit = 1;
    } else if (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        fprintf(stderr, "[display] window resized to %dx%d; Unity keeps the size it started with\n",
                e->window.data1, e->window.data2);
    }
    return 0;
}

int display_sdl_quit_requested(void) { return g_quit; }

void *display_sdl_window(void) { return g_win; }

// ---- setup (main thread) -------------------------------------------------------------------------
static int load_egl(void)
{
#define L(f) *(void **)&E.f = egl_glue_real("egl" #f)
    L(GetCurrentDisplay); L(GetCurrentSurface); L(GetCurrentContext); L(QuerySurface); L(ChooseConfig);
    L(GetConfigAttrib); L(QueryString); L(GetError);
#undef L
    return E.GetCurrentDisplay && E.GetCurrentSurface && E.GetCurrentContext && E.QuerySurface && E.ChooseConfig &&
           E.GetConfigAttrib && E.QueryString && E.GetError;
}

// Let the window system settle the window's size (Wayland's first configure, X11's map).
static void settle(void)
{
    int pw = -1, ph = -1, same = 0;
    for (int i = 0; i < 30 && same < 3; i++) {
        SDL_PumpEvents();
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(g_win, &w, &h);
        if (w == pw && h == ph) same++;
        else { same = 0; pw = w; ph = h; }
        SDL_Delay(10);
    }
}

static void black_frame(void)
{
    void (*clear_color)(float, float, float, float) = (void (*)(float, float, float, float))SDL_GL_GetProcAddress("glClearColor");
    void (*clear)(unsigned) = (void (*)(unsigned))SDL_GL_GetProcAddress("glClear");
    if (clear_color && clear) { clear_color(0.f, 0.f, 0.f, 1.f); clear(GL_COLOR_BUFFER_BIT); }
    SDL_GL_SwapWindow(g_win);
}

static void close_video(void)
{
    if (g_ctx) SDL_GL_DeleteContext(g_ctx);
    if (g_win) SDL_DestroyWindow(g_win);
    g_ctx = NULL; g_win = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int display_sdl_open(const char *title, egl_host_display *out, int *width, int *height, int *refresh)
{
    // SDL and egl_glue (where Unity's EGL calls go) must use the same libEGL
    const char *glue = getenv("ANDROID_SHIM_LIBEGL"), *sdl = getenv("SDL_VIDEO_EGL_DRIVER");
    if (glue && *glue && !(sdl && *sdl)) setenv("SDL_VIDEO_EGL_DRIVER", glue, 1);
    else if (sdl && *sdl && !(glue && *glue)) setenv("ANDROID_SHIM_LIBEGL", sdl, 1);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");      // the host has its own SIGINT/SIGTERM handlers
    SDL_SetHint("SDL_OPENGL_ES_DRIVER", "1");           // X11: GLES through EGL, not GLX
    SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");        // (the same on SDL >= 2.0.12)
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[display] SDL video: %s\n", SDL_GetError());
        return -1;
    }
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!load_egl()) {
        fprintf(stderr, "[display] libEGL lacks the eglGetCurrent*/eglQuery* functions\n");
        close_video();
        return -1;
    }

    int w = 0, h = 0, windowed = 0;
    const char *ws = getenv("UA_DISPLAY_WINDOW");
    if (ws && sscanf(ws, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) windowed = 1;
    else {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { w = dm.w; h = dm.h; }
        if (w <= 0 || h <= 0) { w = 1280; h = 720; }
    }
    int samples = getenv("UA_DISPLAY_SAMPLES") ? atoi(getenv("UA_DISPLAY_SAMPLES")) : 0;
    int srgb = getenv("UA_DISPLAY_SRGB") && atoi(getenv("UA_DISPLAY_SRGB")) > 0;
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | (windowed ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);

    // what Unity's Android player asks for (32-bit display buffer, depth and stencil), then less
    static const struct { int major, alpha, depth, stencil; } tries[] = {
        { 3, 8, 24, 8 }, { 3, 0, 24, 8 }, { 3, 0, 16, 0 }, { 2, 8, 24, 8 }, { 2, 0, 16, 0 },
    };
    char err[256] = "";
    unsigned t;
    for (t = 0; t < sizeof tries / sizeof tries[0]; t++) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, tries[t].major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, tries[t].alpha);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, tries[t].depth);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, tries[t].stencil);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        if (samples > 0) {
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, samples);
        }
        if (srgb) SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 1);
        g_win = SDL_CreateWindow(title && *title ? title : "Unity", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
        if (!g_win) { snprintf(err, sizeof err, "window: %s", SDL_GetError()); continue; }
        g_ctx = SDL_GL_CreateContext(g_win);
        if (g_ctx) break;
        snprintf(err, sizeof err, "GLES %d.0 context: %s", tries[t].major, SDL_GetError());
        SDL_DestroyWindow(g_win);
        g_win = NULL;
    }
    if (!g_ctx) {
        fprintf(stderr, "[display] SDL (%s) has no GLES window: %s\n", driver ? driver : "?", err);
        close_video();
        return -1;
    }

    SDL_GL_MakeCurrent(g_win, g_ctx);           // SDL_GL_CreateContext made it current; to be sure
    EGLDisplay dpy = E.GetCurrentDisplay();
    EGLSurface surf = E.GetCurrentSurface(EGL_DRAW);
    EGLContext ctx = E.GetCurrentContext();
    EGLConfig cfg = NULL;
    EGLint id = 0, n = 0;
    if (dpy && surf && ctx && E.QuerySurface(dpy, surf, EGL_CONFIG_ID, &id)) {
        const EGLint q[] = { EGL_CONFIG_ID, id, EGL_NONE };
        if (!E.ChooseConfig(dpy, q, &cfg, 1, &n) || n != 1) cfg = NULL;
    }
    if (!cfg) {
        fprintf(stderr, "[display] SDL (%s) did not make an EGL surface current with the libEGL Unity uses "
                "(display %p, surface %p, context %p): GLX, or another libEGL (ANDROID_SHIM_LIBEGL / SDL_VIDEO_EGL_DRIVER)?\n",
                driver ? driver : "?", dpy, surf, ctx);
        SDL_GL_MakeCurrent(g_win, NULL);
        close_video();
        return -1;
    }

    EGLint r = 0, g = 0, b = 0, a = 0, d = 0, s = 0, ms = 0, rt = 0, cs = 0;
    E.GetConfigAttrib(dpy, cfg, EGL_RED_SIZE, &r); E.GetConfigAttrib(dpy, cfg, EGL_GREEN_SIZE, &g);
    E.GetConfigAttrib(dpy, cfg, EGL_BLUE_SIZE, &b); E.GetConfigAttrib(dpy, cfg, EGL_ALPHA_SIZE, &a);
    E.GetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &d); E.GetConfigAttrib(dpy, cfg, EGL_STENCIL_SIZE, &s);
    E.GetConfigAttrib(dpy, cfg, EGL_SAMPLES, &ms); E.GetConfigAttrib(dpy, cfg, EGL_RENDERABLE_TYPE, &rt);
    if (!E.QuerySurface(dpy, surf, EGL_GL_COLORSPACE_KHR, &cs)) cs = 0;
    const char *vendor = E.QueryString(dpy, EGL_VENDOR), *version = E.QueryString(dpy, EGL_VERSION);
    const unsigned char *(*get_string)(unsigned) = (const unsigned char *(*)(unsigned))SDL_GL_GetProcAddress("glGetString");
    const unsigned char *renderer = get_string ? get_string(GL_RENDERER) : NULL, *glver = get_string ? get_string(GL_VERSION) : NULL;
    fprintf(stderr, "[display] SDL %s: EGL %s (%s), %s, %s\n", driver ? driver : "?", version ? version : "?",
            vendor ? vendor : "?", renderer ? (const char *)renderer : "?", glver ? (const char *)glver : "?");
    fprintf(stderr, "[display] config %d: RGBA %d%d%d%d depth %d stencil %d samples %d%s%s\n", id, r, g, b, a, d, s, ms,
            rt & EGL_OPENGL_ES3_BIT ? ", ES3" : ", no ES3 (Unity takes GLES2)", cs == EGL_GL_COLORSPACE_SRGB_KHR ? ", sRGB" : "");

    const char *iv = getenv("UA_SWAP_INTERVAL");
    int interval = iv && *iv ? atoi(iv) : 1;
    if (SDL_GL_SetSwapInterval(interval) != 0)
        fprintf(stderr, "[display] swap interval %d: %s\n", interval, SDL_GetError());
    // Black frames until the EGL surface has the window's size: a compositor may configure another
    // size than asked for (a tiling one tiles a window), and Mesa's Wayland surface takes a new
    // size only with its next buffer. Unity reads the size with eglQuerySurface before it draws.
    int dw = 0, dh = 0;
    EGLint sw = 0, sh = 0;
    settle();
    for (int i = 0; i < 5; i++) {
        black_frame();
        settle();
        // SDL's KMSDRM driver recreates its EGL surface on the first swap after a fullscreen
        // switch (window creation is one): the surface SDL has current now
        EGLSurface now = E.GetCurrentSurface(EGL_DRAW);
        if (now && now != surf) { fprintf(stderr, "[display] SDL replaced its EGL surface on a swap\n"); surf = now; }
        SDL_GL_GetDrawableSize(g_win, &dw, &dh);
        if (!E.QuerySurface(dpy, surf, EGL_WIDTH, &sw) || !E.QuerySurface(dpy, surf, EGL_HEIGHT, &sh)) sw = sh = 0;
        if (sw == dw && sh == dh) break;
    }
    SDL_GL_MakeCurrent(g_win, NULL);            // Unity's render thread makes its own context current
    E.GetError();                               // nothing pending on this (Unity's UI) thread
    if (sw != dw || sh != dh) {
        // what Unity will find with eglQuerySurface, as long as the EGL surface has a size
        fprintf(stderr, "[display] drawable %dx%d but EGL surface %dx%d: Unity is told the latter\n", dw, dh, sw, sh);
        if (sw > 0 && sh > 0) { dw = sw; dh = sh; }
    }

    SDL_ShowCursor(SDL_DISABLE);
    SDL_AddEventWatch(on_event, NULL);
    int hz = 0;
    SDL_DisplayMode mode;
    int di = SDL_GetWindowDisplayIndex(g_win);
    if (SDL_GetCurrentDisplayMode(di >= 0 ? di : 0, &mode) == 0) hz = mode.refresh_rate;
    fprintf(stderr, "[display] %s %dx%d%s, %d Hz, swap interval %d\n", windowed ? "window" : "fullscreen", dw, dh,
            t ? " (fallback config)" : "", hz, interval);

    memset(out, 0, sizeof *out);
    out->display = dpy;
    out->config = cfg;
    out->surface = surf;
    out->make_current = sdl_make_current;
    out->swap = sdl_swap;
    out->swap_interval = sdl_swap_interval;
    *width = dw;
    *height = dh;
    *refresh = hz;
    return 0;
}
