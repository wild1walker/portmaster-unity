// Display backend selection (display.h): UA_DISPLAY=auto|fbdev|sdl.
#define _GNU_SOURCE
#include "display.h"
#include "fbdev.h"
#include "../android/android_shims.h"
#include "../android/egl_glue.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EGL_EXTENSIONS 0x3055

// ---- the GPU scaler (render thread, Unity's context current, before the swap) -------------------
// Two glBlitFramebuffer: the rw x rh frame into a texture (the same size: also resolves a
// multisampled surface), then the texture (linear) over the whole surface (a blit within one
// framebuffer may not overlap). GLES 3.
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_SCISSOR_TEST 0x0C11
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_COLOR_BUFFER_BIT 0x4000
static struct {
    void (*GetIntegerv)(unsigned, int *);
    void (*GetBooleanv)(unsigned, unsigned char *);
    unsigned char (*IsEnabled)(unsigned);
    void (*Enable)(unsigned), (*Disable)(unsigned);
    void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (*GenTextures)(int, unsigned *);
    void (*BindTexture)(unsigned, unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*GenFramebuffers)(int, unsigned *);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*CheckFramebufferStatus)(unsigned);
    void (*BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
} G;
static int g_srw, g_srh, g_sdw, g_sdh;          // set: display_gpu_scale
static unsigned g_sfbo;
static int g_sstate;                            // 0 not set up, 1 on, -1 failed (frames stay unscaled)

void display_gpu_scale(int rw, int rh, int dw, int dh) { g_srw = rw; g_srh = rh; g_sdw = dw; g_sdh = dh; }

static int scale_setup(void)
{
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    void *(*gpa)(const char *) = (void *(*)(const char *))egl_glue_real("eglGetProcAddress");
#define L(f) do { *(void **)&G.f = h ? dlsym(h, "gl" #f) : NULL; if (!G.f && gpa) *(void **)&G.f = gpa("gl" #f); } while (0)
    L(GetIntegerv); L(GetBooleanv); L(IsEnabled); L(Enable); L(Disable); L(ColorMask); L(GenTextures);
    L(BindTexture); L(TexImage2D); L(TexParameteri); L(GenFramebuffers); L(BindFramebuffer);
    L(FramebufferTexture2D); L(CheckFramebufferStatus); L(BlitFramebuffer);
#undef L
    void **f = (void **)&G;
    for (unsigned i = 0; i < sizeof G / sizeof *f; i++) if (!f[i]) return 0;
    int tex0 = 0, rfb = 0, dfb = 0;
    unsigned tex;
    G.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    G.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    G.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    G.GenTextures(1, &tex);
    G.BindTexture(GL_TEXTURE_2D, tex);
    G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_srw, g_srh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.GenFramebuffers(1, &g_sfbo);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sfbo);
    G.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    int ok = G.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    G.BindTexture(GL_TEXTURE_2D, (unsigned)tex0);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)rfb);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)dfb);
    return ok;
}

void display_present_scaled(void)
{
    if (!g_srw || g_sstate < 0) return;
    if (!g_sstate) {
        g_sstate = scale_setup() ? 1 : -1;
        fprintf(stderr, g_sstate > 0 ? "[display] render size %dx%d, scaled to %dx%d by the GPU\n"
                                     : "[display] render size %dx%d: no GLES 3 framebuffer copy, the frame stays in the corner of %dx%d\n",
                g_srw, g_srh, g_sdw, g_sdh);
        if (g_sstate < 0) return;
    }
    int rfb = 0, dfb = 0;
    unsigned char mask[4];
    G.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    G.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    G.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    unsigned char sc = G.IsEnabled(GL_SCISSOR_TEST), rd = G.IsEnabled(GL_RASTERIZER_DISCARD);
    if (sc) G.Disable(GL_SCISSOR_TEST);
    if (rd) G.Disable(GL_RASTERIZER_DISCARD);
    G.ColorMask(1, 1, 1, 1);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sfbo);
    G.BlitFramebuffer(0, 0, g_srw, g_srh, 0, 0, g_srw, g_srh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, g_sfbo);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    G.BlitFramebuffer(0, 0, g_srw, g_srh, 0, 0, g_sdw, g_sdh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    G.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)rfb);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)dfb);
    G.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (sc) G.Enable(GL_SCISSOR_TEST);
    if (rd) G.Enable(GL_RASTERIZER_DISCARD);
}

static const char *g_backend = "fbdev";
static egl_host_display g_hd;
static int g_refresh;

static int pbuffer_mode(void)
{
    const char *e = getenv("UA_EGL_PBUFFER");
    return e && *e && *e != '0';
}

// The libEGL egl_glue will use (same search), opened RTLD_LOCAL: egl_glue still opens it
// RTLD_GLOBAL on Unity's first EGL call, as it always has.
static void *open_egl(void)
{
    const char *names[] = { getenv("ANDROID_SHIM_LIBEGL"), "libEGL.so.1", "libEGL.so" };
    for (int i = 0; i < 3; i++) {
        void *h = names[i] && *names[i] ? dlopen(names[i], RTLD_NOW | RTLD_LOCAL) : NULL;
        if (h) return h;
    }
    return NULL;
}

const char *display_auto_choice(char *why, size_t n)
{
    if (pbuffer_mode()) { snprintf(why, n, "UA_EGL_PBUFFER"); return "fbdev"; }
    const char *fb = getenv("ANDROID_FBDEV");
    if (!fb || !*fb) fb = "/dev/fb0";
    if (access(fb, F_OK) != 0) { snprintf(why, n, "no %s", fb); return "sdl"; }
    void *egl = open_egl();
    if (!egl) { snprintf(why, n, "no libEGL"); return "fbdev"; }   // Unity fails later, as before
    const char *(*query)(void *, int32_t) = (const char *(*)(void *, int32_t))dlsym(egl, "eglQueryString");
    int32_t (*get_error)(void) = (int32_t (*)(void))dlsym(egl, "eglGetError");
    // Client extensions (EGL_EXT_client_extensions): Mesa and the window-system flavors of vendor
    // drivers list their platforms; an fbdev-only EGL lists none (or answers NULL).
    const char *ext = query ? query(NULL, EGL_EXTENSIONS) : NULL;
    // A NULL answer leaves EGL_BAD_DISPLAY pending on this thread, which is also Unity's UI thread:
    // Unity reads eglGetError after creating its probe context and would take it for a failure.
    if (get_error) get_error();
    static const char *const platforms[] = { "_platform_gbm", "_platform_wayland", "_platform_x11",
                                             "_platform_xcb", "_platform_device", "_platform_surfaceless" };
    for (size_t i = 0; ext && i < sizeof platforms / sizeof platforms[0]; i++)
        if (strstr(ext, platforms[i])) {
            snprintf(why, n, "libEGL supports window systems (EGL_*%s)", platforms[i]);
            return "sdl";
        }
    snprintf(why, n, "%s and an fbdev-only libEGL", fb);
    return "fbdev";
}

// The fbdev path: Mali's fbdev EGL on the framebuffer; fbdev.c sets up its buffers and flips, and
// with UA_RENDER_SIZE / UA_RENDER_SCALE the window has that size: on Allwinner's display engine the
// framebuffer is cropped to it and the engine scales it up; elsewhere Unity is told it, the EGL
// surface keeps the panel's size and the GPU scaler draws the frame over it.
// PowerVR's whole-screen window: its driver presents (fbdev_driver_presents), and a render size
// goes to the GPU scaler over the whole screen
static void on_whole_screen(void)
{
    static int done;
    if (done++) return;
    int w, h, pw, ph;
    if (fbdev_driver_presents(&w, &h, &pw, &ph)) {
        egl_glue_set_report_size(w, h);
        display_gpu_scale(w, h, pw, ph);
    }
}

static int use_fbdev(void)
{
    if (pbuffer_mode()) return 0;
    fbdev_setup();
    egl_glue_set_whole_screen_hook(on_whole_screen);
    int w, h, pw, ph;
    if (fbdev_render_size(&w, &h)) android_window_set_size(w, h);
    else if (fbdev_gpu_render_size(&w, &h, &pw, &ph)) {
        static struct { unsigned short w, h; } panel;      // Mali's fbdev_window: the surface's size
        panel.w = (unsigned short)pw;
        panel.h = (unsigned short)ph;
        android_window_set_size(w, h);
        android_window_set_native(&panel);
        egl_glue_set_report_size(w, h);
        display_gpu_scale(w, h, pw, ph);
    }
    ANativeWindow *win = android_window_get();
    android_window_set_max_size(ANativeWindow_getWidth(win), ANativeWindow_getHeight(win));
    return 0;
}

int display_init(const char *title)
{
    const char *want = getenv("UA_DISPLAY");
    if (!want || !*want) want = "auto";
    int is_auto = !strcmp(want, "auto");
    if (!is_auto && strcmp(want, "fbdev") && strcmp(want, "sdl")) {
        fprintf(stderr, "[display] UA_DISPLAY=%s: auto, fbdev or sdl\n", want);
        return -1;
    }
    if (!strcmp(want, "fbdev")) return use_fbdev();
    if (pbuffer_mode()) {
        if (!is_auto) fprintf(stderr, "[display] UA_EGL_PBUFFER is set: pbuffer path, UA_DISPLAY=sdl ignored\n");
        return 0;
    }
    if (is_auto) {
        char why[160];
        const char *pick = display_auto_choice(why, sizeof why);
        fprintf(stderr, "[display] auto: %s (%s)\n", pick, why);
        if (strcmp(pick, "sdl")) return use_fbdev();
    }
    int w = 0, h = 0, hz = 0;
    if (display_sdl_open(title, &g_hd, &w, &h, &hz) != 0) {
        if (!is_auto) return -1;
        fprintf(stderr, "[display] no SDL window: falling back to the fbdev path\n");
        return use_fbdev();
    }
    egl_glue_set_host_display(&g_hd);
    android_window_set_size(w, h);              // (with UA_RENDER_SIZE / UA_RENDER_SCALE: the smaller size)
    g_refresh = hz;
    g_backend = "sdl";
    return 0;
}

const char *display_backend(void) { return g_backend; }

const so_shim *display_egl_shims(void) { return strcmp(g_backend, "sdl") ? NULL : egl_host_shims; }

int display_refresh_rate(void) { return g_refresh; }

int display_quit_requested(void) { return !strcmp(g_backend, "sdl") && display_sdl_quit_requested(); }
