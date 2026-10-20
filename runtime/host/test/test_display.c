// Test of the SDL display backend without a device: the host's display_init() on the main thread,
// then Unity's EGL call sequence (libunity 2017.4: UnityGetSelectedGLESVersion, ConfigEGL::Match,
// ContextEGL::Update, WindowContextEGL::HandleInvalidState/Acquire/Present/SetVSyncInterval/
// Release/DestroySurface, ContextGLES::Destroy) on a second thread through the shim tables the
// loader would bind Unity's imports to, drawing, reading back and swapping while the main thread
// pumps SDL events like ua_host's main loop. On X11 the presented frame is also read back from the
// window with XGetImage. Built and run by display_test.sh (aarch64, Mesa llvmpipe; Xvfb and a
// headless weston).
//   TEST_EXPECT=sdl|fbdev   backend display_init() must pick (default sdl)
//   TEST_SIZE=WxH           expected window size (default: not checked)
//   TEST_FRAMES=<n>         frames drawn (default 120)
#define _GNU_SOURCE
#include "../display.h"
#include "../../android/android_shims.h"
#include "../../android/egl_glue.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#if defined(SDL_VIDEO_DRIVER_X11)
#include <X11/Xutil.h>
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
#define EGL_SUCCESS 0x3000
#define EGL_NONE 0x3038
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_VENDOR 0x3053
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_READ 0x305A
#define EGL_DRAW 0x3059
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WINDOW_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_RECORDABLE_ANDROID 0x3142
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_SCISSOR_TEST 0x0C11
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01

// Unity's imports bind like the loader binds them: the display's table, egl_shims, then libEGL.
static const so_shim *g_tables[2];
static void *U(const char *name)
{
    for (int t = 0; t < 2; t++)
        for (const so_shim *s = g_tables[t]; s && s->name; s++)
            if (!strcmp(s->name, name)) return s->fn;
    return egl_glue_real(name);
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static volatile int g_done;
static int g_frames = 120, g_hook_calls, g_hook_current;
static EGLContext g_ctx;
static EGLContext (*p_GetCurrentContext)(void);
static unsigned char g_last[3];          // last color presented
static double g_fps;

static void on_swap(void *ctx)
{
    (void)ctx;
    g_hook_calls++;
    if (p_GetCurrentContext && p_GetCurrentContext() == g_ctx) g_hook_current++;
}

static int expect_pixel(void (*read)(int, int, int, int, unsigned, unsigned, void *), int x, int y, const unsigned char *rgb)
{
    unsigned char px[4] = { 0 };
    read(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int ok = px[0] == rgb[0] && px[1] == rgb[1] && px[2] == rgb[2];
    if (!ok) fprintf(stderr, "pixel %d,%d: %d,%d,%d, expected %d,%d,%d\n", x, y, px[0], px[1], px[2], rgb[0], rgb[1], rgb[2]);
    return ok;
}

static void *unity_render_thread(void *arg)
{
    (void)arg;
    EGLDisplay (*GetDisplay)(void *) = U("eglGetDisplay");
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *) = U("eglInitialize");
    EGLBoolean (*Terminate)(EGLDisplay) = U("eglTerminate");
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = U("eglChooseConfig");
    EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *) = U("eglGetConfigAttrib");
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = U("eglCreateContext");
    EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext) = U("eglDestroyContext");
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *) = U("eglCreateWindowSurface");
    EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface) = U("eglDestroySurface");
    EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *) = U("eglQuerySurface");
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = U("eglMakeCurrent");
    EGLSurface (*GetCurrentSurface)(EGLint) = U("eglGetCurrentSurface");
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface) = U("eglSwapBuffers");
    EGLBoolean (*SwapInterval)(EGLDisplay, EGLint) = U("eglSwapInterval");
    EGLint (*GetError)(void) = U("eglGetError");
    const char *(*QueryString)(EGLDisplay, EGLint) = U("eglQueryString");
    void *(*GetProcAddress)(const char *) = U("eglGetProcAddress");
    p_GetCurrentContext = U("eglGetCurrentContext");
    CHECK(GetDisplay && Initialize && Terminate && ChooseConfig && GetConfigAttrib && CreateContext && DestroyContext &&
          CreateWindowSurface && DestroySurface && QuerySurface && MakeCurrent && GetCurrentSurface && p_GetCurrentContext &&
          SwapBuffers && SwapInterval && GetError && QueryString && GetProcAddress);
    CHECK(GetProcAddress("eglSwapBuffers") == (void *)SwapBuffers && GetProcAddress("eglMakeCurrent") == (void *)MakeCurrent);

    // ContextGLES::Create / UnityGetSelectedGLESVersion
    EGLDisplay dpy = GetDisplay(NULL);
    EGLint major = 0, minor = 0;
    CHECK(dpy != NULL && Initialize(dpy, &major, &minor) && major >= 1);
    CHECK(Initialize(dpy, NULL, NULL));

    // ConfigEGL::FindFirstMatchingConfig + Match
    const EGLint want[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                            EGL_STENCIL_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                            EGL_RECORDABLE_ANDROID, 1, EGL_NONE };
    EGLint n = -1;
    EGLConfig cfgs[8] = { 0 };
    CHECK(ChooseConfig(dpy, want, NULL, 0, &n) && n == 1);
    CHECK(ChooseConfig(dpy, want, cfgs, 8, &n) && n == 1 && cfgs[0] != NULL);
    EGLConfig cfg = cfgs[0];
    EGLint v = -1;
    CHECK(GetConfigAttrib(dpy, cfg, EGL_RENDERABLE_TYPE, &v) && (v & EGL_OPENGL_ES3_BIT));
    CHECK(GetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &v) && v >= 16);
    CHECK(GetConfigAttrib(dpy, cfg, EGL_RECORDABLE_ANDROID, &v) && v == 0);   // Android-only: 0
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext probe = CreateContext(dpy, cfg, NULL, ctx_attr);
    CHECK(probe != NULL && GetError() == EGL_SUCCESS);   // Match() reads eglGetError after it
    CHECK(DestroyContext(dpy, probe));

    // ContextEGL::Update: the window context, and one linked to it
    EGLContext ctx = CreateContext(dpy, cfg, NULL, ctx_attr);
    EGLContext shared = CreateContext(dpy, cfg, ctx, ctx_attr);
    CHECK(ctx != NULL && shared != NULL);
    g_ctx = ctx;

    // WindowContextEGL::HandleInvalidState: the window surface, sized from eglQuerySurface
    ANativeWindow *win = android_window_get();
    EGLSurface surf = CreateWindowSurface(dpy, cfg, win, NULL);
    EGLint sw = 0, sh = 0;
    CHECK(surf != NULL && QuerySurface(dpy, surf, EGL_WIDTH, &sw) && QuerySurface(dpy, surf, EGL_HEIGHT, &sh));
    CHECK(sw == ANativeWindow_getWidth(win) && sh == ANativeWindow_getHeight(win));
    fprintf(stderr, "test: EGL %d.%d, window surface %p %dx%d\n", major, minor, surf, sw, sh);

    // Acquire, IsCurrentInternal, SetVSyncInterval
    CHECK(MakeCurrent(dpy, surf, surf, ctx));
    CHECK(GetCurrentSurface(EGL_DRAW) == surf && GetCurrentSurface(EGL_READ) == surf && p_GetCurrentContext() == ctx);
    CHECK(SwapInterval(dpy, 1));

    void (*Viewport)(int, int, int, int) = GetProcAddress("glViewport");
    void (*ClearColor)(float, float, float, float) = GetProcAddress("glClearColor");
    void (*Clear)(unsigned) = GetProcAddress("glClear");
    void (*Enable)(unsigned) = GetProcAddress("glEnable");
    void (*Disable)(unsigned) = GetProcAddress("glDisable");
    void (*Scissor)(int, int, int, int) = GetProcAddress("glScissor");
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *) = GetProcAddress("glReadPixels");
    const unsigned char *(*GetString)(unsigned) = GetProcAddress("glGetString");
    CHECK(Viewport && ClearColor && Clear && Enable && Disable && Scissor && ReadPixels && GetString);
    if (!(Viewport && ClearColor && Clear && Enable && Disable && Scissor && ReadPixels && GetString)) { g_done = 1; return NULL; }
    const char *glv = (const char *)GetString(GL_VERSION);
    fprintf(stderr, "test: %s, %s\n", glv ? glv : "?", GetString(GL_RENDERER) ? (const char *)GetString(GL_RENDERER) : "?");
    CHECK(glv && !strncmp(glv, "OpenGL ES 3", 11));

    // frames: a full-screen color and an 8x8 white box at the bottom left, read back, presented
    static const unsigned char white[3] = { 255, 255, 255 };
    int good = 0;
    double t0 = now();
    for (int i = 0; i < g_frames; i++) {
        unsigned char c[3] = { (unsigned char)(i * 37), (unsigned char)(255 - i * 11), (unsigned char)(64 + i * 5) };
        Viewport(0, 0, sw, sh);
        ClearColor(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1.f);
        Clear(GL_COLOR_BUFFER_BIT);
        Enable(GL_SCISSOR_TEST);
        Scissor(0, 0, 8, 8);
        ClearColor(1.f, 1.f, 1.f, 1.f);
        Clear(GL_COLOR_BUFFER_BIT);
        Disable(GL_SCISSOR_TEST);
        good += expect_pixel(ReadPixels, sw / 2, sh / 2, c) && expect_pixel(ReadPixels, 2, 2, white);
        if (!SwapBuffers(dpy, surf)) { fprintf(stderr, "swap %d failed: 0x%x\n", i, GetError()); break; }
        memcpy(g_last, c, 3);
    }
    double dt = now() - t0;
    g_fps = g_frames / dt;
    CHECK(good == g_frames);
    CHECK(GetCurrentSurface(EGL_DRAW) == surf && p_GetCurrentContext() == ctx);   // still Unity's after the swaps
    fprintf(stderr, "test: %d frames in %.2f s (%.1f fps), %d read back right\n", g_frames, dt, g_fps, good);

    // Release, then a pause/resume: DestroySurface + HandleInvalidState + Acquire, one more frame
    CHECK(MakeCurrent(dpy, NULL, NULL, NULL));
    CHECK(p_GetCurrentContext() == NULL);
    CHECK(DestroySurface(dpy, surf));
    EGLSurface again = CreateWindowSurface(dpy, cfg, win, NULL);
    CHECK(again == surf);
    CHECK(MakeCurrent(dpy, again, again, ctx) && p_GetCurrentContext() == ctx);
    const unsigned char fin[3] = { 32, 160, 64 };
    ClearColor(fin[0] / 255.f, fin[1] / 255.f, fin[2] / 255.f, 1.f);
    Clear(GL_COLOR_BUFFER_BIT);
    CHECK(expect_pixel(ReadPixels, sw / 2, sh / 2, fin));
    CHECK(SwapBuffers(dpy, again));
    memcpy(g_last, fin, 3);

    // ContextGLES::Destroy
    CHECK(MakeCurrent(dpy, NULL, NULL, NULL));
    CHECK(DestroyContext(dpy, shared) && DestroyContext(dpy, ctx));
    CHECK(Terminate(dpy));
    const char *(*real_query)(EGLDisplay, EGLint) = egl_glue_real("eglQueryString");
    CHECK(real_query && real_query(dpy, EGL_VENDOR) != NULL);   // the host's display is still initialized
    g_done = 1;
    return NULL;
}

#if defined(SDL_VIDEO_DRIVER_X11)
// The window's pixels as the X server has them: the frame really was presented.
static void check_x11_window(SDL_Window *w)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_X11) return;
    XImage *(*get_image)(Display *, Drawable, int, int, unsigned, unsigned, unsigned long, int) =
        (XImage *(*)(Display *, Drawable, int, int, unsigned, unsigned, unsigned long, int))dlsym(RTLD_DEFAULT, "XGetImage");
    int (*sync)(Display *, Bool) = (int (*)(Display *, Bool))dlsym(RTLD_DEFAULT, "XSync");
    if (!get_image) {
        void *x = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
        if (x) { get_image = (XImage *(*)(Display *, Drawable, int, int, unsigned, unsigned, unsigned long, int))dlsym(x, "XGetImage");
                 sync = (int (*)(Display *, Bool))dlsym(x, "XSync"); }
    }
    CHECK(get_image != NULL && sync != NULL);
    if (!get_image || !sync) return;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    sync(info.info.x11.display, False);
    XImage *img = get_image(info.info.x11.display, info.info.x11.window, ww / 2, wh / 2, 1, 1, ~0UL, ZPixmap);
    CHECK(img != NULL);
    if (!img) return;
    unsigned long p = XGetPixel(img, 0, 0);
    unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
    fprintf(stderr, "test: X11 window center %d,%d,%d (last frame %d,%d,%d)\n", rgb[0], rgb[1], rgb[2], g_last[0], g_last[1], g_last[2]);
    CHECK(!memcmp(rgb, g_last, 3));
    XDestroyImage(img);
}
#endif

int main(void)
{
    const char *expect = getenv("TEST_EXPECT") ? getenv("TEST_EXPECT") : "sdl";
    if (getenv("TEST_FRAMES")) g_frames = atoi(getenv("TEST_FRAMES"));

    // UA_DISPLAY=auto's decision
    char why[160];
    const char *fbdev = getenv("ANDROID_FBDEV") ? strdup(getenv("ANDROID_FBDEV")) : NULL;
    setenv("ANDROID_FBDEV", "/nonexistent/fb0", 1);
    CHECK(!strcmp(display_auto_choice(why, sizeof why), "sdl"));
    fprintf(stderr, "test: auto without a framebuffer: sdl (%s)\n", why);
    setenv("ANDROID_FBDEV", "/dev/null", 1);                        // "a framebuffer", and Mesa's libEGL
    CHECK(!strcmp(display_auto_choice(why, sizeof why), "sdl"));
    fprintf(stderr, "test: auto with a framebuffer and Mesa: sdl (%s)\n", why);
    setenv("UA_EGL_PBUFFER", "1", 1);
    CHECK(!strcmp(display_auto_choice(why, sizeof why), "fbdev"));
    unsetenv("UA_EGL_PBUFFER");
    if (fbdev) setenv("ANDROID_FBDEV", fbdev, 1); else unsetenv("ANDROID_FBDEV");

    CHECK(display_init("test_display") == 0);
    fprintf(stderr, "test: backend %s (expected %s)\n", display_backend(), expect);
    CHECK(!strcmp(display_backend(), expect));
    if (strcmp(display_backend(), "sdl")) {
        CHECK(display_egl_shims() == NULL && display_sdl_window() == NULL);
        fprintf(stderr, "%d checks, %d failures\n", checks, failures);
        return failures != 0;
    }
    CHECK(display_egl_shims() == egl_host_shims);
    SDL_Window *w = display_sdl_window();
    CHECK(w != NULL);
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(w, &dw, &dh);
    ANativeWindow *aw = android_window_get();
    CHECK(ANativeWindow_getWidth(aw) == dw && ANativeWindow_getHeight(aw) == dh);
    int ew, eh;
    if (getenv("TEST_SIZE") && sscanf(getenv("TEST_SIZE"), "%dx%d", &ew, &eh) == 2) CHECK(dw == ew && dh == eh);
    fprintf(stderr, "test: window %dx%d, %d Hz\n", dw, dh, display_refresh_rate());

    g_tables[0] = display_egl_shims();
    g_tables[1] = egl_shims;
    egl_glue_set_swap_hook(on_swap, NULL);
    pthread_t t;
    pthread_create(&t, NULL, unity_render_thread, NULL);
    double t0 = now();
    while (!g_done && now() - t0 < 120) {        // ua_host's main loop: input_poll pumps SDL
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        SDL_Delay(4);
    }
    CHECK(g_done);
    if (!g_done) { fprintf(stderr, "render thread stuck\n"); return 1; }
    pthread_join(t, NULL);
    CHECK(g_hook_calls == g_frames + 1 && g_hook_current == g_hook_calls);
#if defined(SDL_VIDEO_DRIVER_X11)
    check_x11_window(w);
#endif
    CHECK(!display_quit_requested());
    SDL_Event q;
    memset(&q, 0, sizeof q);
    q.type = SDL_QUIT;
    SDL_PushEvent(&q);
    CHECK(display_quit_requested());
    fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
