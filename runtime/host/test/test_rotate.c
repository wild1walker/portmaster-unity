// Test of a turned panel's present (fbdev.c's UA_ROTATE, display.c's display_present_rotated) without
// a device: Mesa llvmpipe on its surfaceless platform, a pbuffer as the window. glpresent.c's
// framebuffer 0 (one frame of the host's, made for the context current with the window) gets four
// coloured quarters, display_present_rotated draws it onto the window, and the window is read back:
// each quarter where the turn puts it, the black bars of a frame of another aspect, and Unity's GL
// state as it was. One case a process (the turn is set up once, as in ua_host): test_rotate <case>, 0
// the choice of the turn. Built and run by rotate_test.sh (aarch64).
#define _GNU_SOURCE
#include "../display.h"
#include "../fbdev.h"
#include "../glpresent.h"
#include "../../android/egl_glue.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_SCISSOR_TEST 0x0C11
#define GL_BLEND 0x0BE2
#define GL_VIEWPORT 0x0BA2
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

static struct {
    void (*BindFramebuffer)(unsigned, unsigned), (*Enable)(unsigned), (*Disable)(unsigned), (*Scissor)(int, int, int, int);
    void (*ClearColor)(float, float, float, float), (*Clear)(unsigned), (*Viewport)(int, int, int, int);
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *), (*GetIntegerv)(unsigned, int *);
    void (*GetFloatv)(unsigned, float *), (*Finish)(void);
    unsigned char (*IsEnabled)(unsigned);
} gl;

enum { RED, GREEN, BLUE, WHITE, BLACK };
static const unsigned char rgb[5][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 }, { 0, 0, 0 } };

// the frame (rw x rh, GL's rows): bottom left red, bottom right green, top left blue, top right white
static void draw_frame(int rw, int rh)
{
    gl.BindFramebuffer(GL_FRAMEBUFFER, glpresent_fb0());
    gl.Enable(GL_SCISSOR_TEST);
    static const int q[4][3] = { { 0, 0, RED }, { 1, 0, GREEN }, { 0, 1, BLUE }, { 1, 1, WHITE } };
    for (int i = 0; i < 4; i++) {
        gl.Scissor(q[i][0] * rw / 2, q[i][1] * rh / 2, rw / 2, rh / 2);
        const unsigned char *c = rgb[q[i][2]];
        gl.ClearColor(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1);
        gl.Clear(GL_COLOR_BUFFER_BIT);
    }
}

static int pixel_is(int x, int y, int want)
{
    unsigned char p[4];
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.ReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    int ok = p[0] == rgb[want][0] && p[1] == rgb[want][1] && p[2] == rgb[want][2];
    if (!ok) fprintf(stderr, "  window (%d,%d): %d %d %d, want %d %d %d\n", x, y, p[0], p[1], p[2], rgb[want][0], rgb[want][1], rgb[want][2]);
    return ok;
}

// One window: Unity's rw x rh frame on a dw x dh window turned deg; the colours expected at the
// window's corners (GL's: bottom left, bottom right, top left, top right), inset by 'in' pixels
// from the frame's place (bars: x0..x1 of the window's columns hold the frame, the rest black)
static void run(int deg, int rw, int rh, int dw, int dh, const int corner[4], int x0, int x1)
{
    fprintf(stderr, "== UA_ROTATE=%d: %dx%d frame on a %dx%d window\n", deg, rw, rh, dw, dh);
    EGLDisplay (*GetDisplay)(void *) = egl_glue_real("eglGetDisplay");
    int (*Initialize)(EGLDisplay, EGLint *, EGLint *) = egl_glue_real("eglInitialize");
    int (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = egl_glue_real("eglChooseConfig");
    EGLSurface (*CreatePbuffer)(EGLDisplay, EGLConfig, const EGLint *) = egl_glue_real("eglCreatePbufferSurface");
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = egl_glue_real("eglCreateContext");
    int (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = egl_glue_real("eglMakeCurrent");
    EGLDisplay d = GetDisplay(NULL);
    Initialize(d, NULL, NULL);
    static const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    CHECK(ChooseConfig(d, ca, &cfg, 1, &n) && n == 1);
    EGLint pa[] = { EGL_WIDTH, dw, EGL_HEIGHT, dh, EGL_NONE };
    static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLSurface s = CreatePbuffer(d, cfg, pa);
    EGLContext c = CreateContext(d, cfg, NULL, xa);
    CHECK(s && c && MakeCurrent(d, s, s, c));

    display_gpu_scale(rw, rh, dw, dh);          // (as use_fbdev: Unity's size, the window's)
    display_gpu_rotate(deg);
    glpresent_made_current(c);                  // (egl_glue's eglMakeCurrent hook, the window's context)
    CHECK(glpresent_fb0() != 0 && glpresent_fb0_texture() != 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);      // the window starts grey: the bars must be drawn black
    gl.Disable(GL_SCISSOR_TEST);
    gl.ClearColor(0.5f, 0.5f, 0.5f, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    draw_frame(rw, rh);
    // Unity's state at its swap: framebuffer 0 bound, a scissor, blending, its viewport and clear colour
    gl.BindFramebuffer(GL_FRAMEBUFFER, glpresent_fb0());
    gl.Scissor(1, 2, 3, 4);
    gl.Enable(GL_BLEND);
    gl.Viewport(5, 6, 7, 8);
    gl.ClearColor(0.25f, 0.5f, 0.75f, 1);
    display_present_rotated();
    int fb = -1, vp[4];
    float cc[4];
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    gl.GetIntegerv(GL_VIEWPORT, vp);
    gl.GetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    CHECK(fb == (int)glpresent_fb0());
    CHECK(vp[0] == 5 && vp[1] == 6 && vp[2] == 7 && vp[3] == 8);
    CHECK(cc[0] == 0.25f && cc[1] == 0.5f && cc[2] == 0.75f);
    CHECK(gl.IsEnabled(GL_SCISSOR_TEST) && gl.IsEnabled(GL_BLEND));
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_BLEND);
    gl.Finish();
    CHECK(pixel_is(x0 + 2, 2, corner[0]));
    CHECK(pixel_is(x1 - 3, 2, corner[1]));
    CHECK(pixel_is(x0 + 2, dh - 3, corner[2]));
    CHECK(pixel_is(x1 - 3, dh - 3, corner[3]));
    if (x0 > 0) CHECK(pixel_is(x0 / 2, dh / 2, BLACK));
    if (x1 < dw) CHECK(pixel_is((x1 + dw) / 2, dh / 2, BLACK));
    glpresent_made_current(NULL);
    MakeCurrent(d, NULL, NULL, NULL);
}

int main(int argc, char **argv)
{
    int k = argc > 1 ? atoi(argv[1]) : 0;
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "no libGLESv2\n"); return 1; }
#define L(f) *(void **)&gl.f = dlsym(h, "gl" #f)
    L(BindFramebuffer); L(Enable); L(Disable); L(Scissor); L(ClearColor); L(Clear); L(Viewport); L(ReadPixels);
    L(GetIntegerv); L(GetFloatv); L(Finish); L(IsEnabled);
#undef L
    if (!k) {                                   // the choice of the turn
    CHECK(fbdev_pick_rotation(NULL, NULL, "", 480, 640) == 270);          // taller than wide: PortMaster's 270
    CHECK(fbdev_pick_rotation("auto", NULL, "", 640, 480) == 0);
    CHECK(fbdev_pick_rotation(NULL, "90", "", 480, 640) == 90);           // DISPLAY_ORIENTATION
    CHECK(fbdev_pick_rotation(NULL, "0", "", 480, 640) == 0);
    CHECK(fbdev_pick_rotation(NULL, "270", "", 640, 480) == 0);           // (an HDMI screen: not turned)
    CHECK(fbdev_pick_rotation(NULL, "180", "", 480, 640) == 0);           // (180 only by number)
    CHECK(fbdev_pick_rotation(NULL, NULL, "root=/dev/mmcblk0p2 fbcon=rotate:1 quiet", 480, 640) == 90);
    CHECK(fbdev_pick_rotation(NULL, NULL, "fbcon=rotate:3", 480, 640) == 270);
    CHECK(fbdev_pick_rotation(NULL, NULL, "xfbcon=rotate:1", 480, 640) == 270);
    CHECK(fbdev_pick_rotation("0", "270", "fbcon=rotate:3", 480, 640) == 0);    // a number is used as it is
    CHECK(fbdev_pick_rotation("90", NULL, "", 720, 480) == 90);
    CHECK(fbdev_pick_rotation("180", NULL, "", 720, 480) == 180);
    CHECK(fbdev_pick_rotation("sideways", NULL, "", 480, 640) == 270);    // (not a turn: auto)
    }

    CHECK(glpresent_table(NULL, 1)[0].name != NULL);
    // the picture turned a quarter anticlockwise in the framebuffer (muOS's RG28XX): read upright
    // when the framebuffer's image is turned 90 degrees clockwise
    static const int c270[4] = { BLUE, RED, WHITE, GREEN }, c90[4] = { GREEN, WHITE, RED, BLUE }, c180[4] = { WHITE, BLUE, GREEN, RED };
    if (k == 1) run(270, 64, 48, 48, 64, c270, 0, 48);
    if (k == 2) run(90, 64, 48, 48, 64, c90, 0, 48);
    if (k == 3) run(270, 32, 24, 48, 64, c270, 0, 48);     // a render size: scaled up
    if (k == 4) run(180, 64, 48, 64, 48, c180, 0, 64);
    if (k == 5) run(270, 64, 36, 48, 64, c270, 6, 42);     // a 16:9 frame on a 4:3 handheld: bars at the panel's sides
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
