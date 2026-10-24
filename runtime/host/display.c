// Display backend selection (display.h): UA_DISPLAY=auto|fbdev|sdl.
#define _GNU_SOURCE
#include "display.h"
#include "fbdev.h"
#include "../android/android_shims.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EGL_EXTENSIONS 0x3055

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
// with UA_RENDER_SIZE / UA_RENDER_SCALE the window has that size (the display engine scales it up).
static int use_fbdev(void)
{
    if (pbuffer_mode()) return 0;
    fbdev_setup();
    int w, h;
    if (fbdev_render_size(&w, &h)) android_window_set_size(w, h);
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
    android_window_set_size(w, h);
    const char *rs = getenv("UA_RENDER_SIZE"), *rc = getenv("UA_RENDER_SCALE");
    if ((rs && *rs) || (rc && *rc))
        fprintf(stderr, "[display] UA_RENDER_SIZE / UA_RENDER_SCALE: the fbdev path only; the SDL window's size %dx%d\n", w, h);
    g_refresh = hz;
    g_backend = "sdl";
    return 0;
}

const char *display_backend(void) { return g_backend; }

const so_shim *display_egl_shims(void) { return strcmp(g_backend, "sdl") ? NULL : egl_host_shims; }

int display_refresh_rate(void) { return g_refresh; }

int display_quit_requested(void) { return !strcmp(g_backend, "sdl") && display_sdl_quit_requested(); }
