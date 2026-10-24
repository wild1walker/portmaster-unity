// Frame statistics and screenshots, from the EGL glue's hook right before each eglSwapBuffers
// (on Unity's render thread, with Unity's context current):
//   UA_FPS=<seconds>  log fps and frame times every <seconds> (0 = off; default 5): average, 99th
//                     percentile and worst frame, frames over 25 ms (late: a missed vsync) and over
//                     50 ms (hitch), Mono GC collections and heap, and on the fbdev path how long
//                     frames took from their swap to their flip (fbdev.c); any frame over 100 ms is
//                     also logged at once ("[hitch]", seconds since start) to line it up with game logs
//   touch <root>/shot.req   save the next frame as <root>/shot-<n>.ppm (the back buffer being
//                     presented; checked once a second, the request file is removed). Pointer
//                     mode's cursor is drawn after it (UA_POINTER_SHOTS=1: before), so shots show
//                     the game's own picture; an open on-screen keyboard is in them (UA_OSK_SHOTS=0:
//                     not)
#define _GNU_SOURCE
#include "frames.h"
#include "fbdev.h"
#include "glhooks.h"
#include "hitches.h"
#include "prof.h"
#include "gltrace.h"
#include "glopt.h"
#include "glpass.h"
#include "glrename.h"
#include "msaafix.h"
#include "osk.h"
#include "pointer.h"
#include "../android/egl_glue.h"
#include "../loader/so_loader.h"

#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VIEWPORT 0x0BA2
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PACK_ALIGNMENT 0x0D05

static void (*gl_ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
static void (*gl_GetIntegerv)(unsigned, int *);
static void (*gl_BindFramebuffer)(unsigned, unsigned);
static void (*gl_BindBuffer)(unsigned, unsigned);
static void (*gl_PixelStorei)(unsigned, int);

static char g_root[512];
static double g_interval = 5;
static char g_req[600];
static int g_shots;
static int g_width, g_height;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void screenshot(void)
{
    if (!gl_ReadPixels)
    {
        void *gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
        if (!gl) gl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
        if (gl)
        {
            gl_ReadPixels = dlsym(gl, "glReadPixels"); gl_GetIntegerv = dlsym(gl, "glGetIntegerv");
            gl_BindFramebuffer = dlsym(gl, "glBindFramebuffer"); gl_BindBuffer = dlsym(gl, "glBindBuffer");
            gl_PixelStorei = dlsym(gl, "glPixelStorei");
        }
    }
    if (!gl_ReadPixels || !gl_GetIntegerv || !gl_BindFramebuffer || !gl_BindBuffer || !gl_PixelStorei)
    { fprintf(stderr, "[frames] screenshot: no GLES functions\n"); return; }
    int w = g_width, h = g_height, rfb = 0, pbo = 0, align = 4;
    gl_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    gl_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    gl_GetIntegerv(GL_PACK_ALIGNMENT, &align);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl_PixelStorei(GL_PACK_ALIGNMENT, 4);
    unsigned char *px = malloc((size_t)w * h * 4);
    if (px)
    {
        gl_ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        char path[600];
        snprintf(path, sizeof path, "%s/shot-%d.ppm", g_root, ++g_shots);
        FILE *f = fopen(path, "wb");
        if (f)
        {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; y--)             // GL rows are bottom-up
                for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
            fclose(f);
            fprintf(stderr, "[frames] screenshot %s\n", path);
        }
        free(px);
    }
    gl_PixelStorei(GL_PACK_ALIGNMENT, align);
    gl_BindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
}

#define MAX_WIN 4096
static float g_win[MAX_WIN];                 // frame times of the current window (ms)
static int (*mono_gc_count)(int);
static int64_t (*mono_heap)(void);
static double g_t_start;

static int cmp_float(const void *a, const void *b)
{
    float d = *(const float *)a - *(const float *)b;
    return d < 0 ? -1 : d > 0;
}

static volatile unsigned long g_presented;
unsigned long frames_presented(void) { return g_presented; }

static void on_swap(void *ctx)
{
    (void)ctx;
    g_presented++;
    static double t0, last, worst, sum, checked;
    static int n, total, late, hitch, gc_prev = -1;
    double t = now();
    if (!g_t_start) g_t_start = t;
    int shot = 0;
    if (t - checked >= 1.0)
    {
        checked = t;
        if (access(g_req, F_OK) == 0) { unlink(g_req); shot = 1; }
    }
    int osk_first = !shot || osk_in_shots(), cursor_first = shot && pointer_in_shots();
    if (osk_first) osk_draw();                  // the on-screen keyboard (nothing unless the game opened its own)
    int cursor = !osk_active();                 // no cursor while the keyboard has the pad
    if (cursor && cursor_first) pointer_draw();
    if (shot) screenshot();
    if (!osk_first) osk_draw();
    if (cursor && !cursor_first) pointer_draw();          // UA_POINTER's cursor (nothing when off)
    if (++total == 1) glhooks_first_frame();
    hitches_frame();
    prof_swap();
    gltrace_frame();
    glopt_frame();
    glpass_frame();
    glrename_frame();
    msaafix_frame();
    fbdev_swap();                               // the fbdev path's flip times (fbdev.c)
    if (last > 0)
    {
        double dt = t - last;
        sum += dt;
        if (n < MAX_WIN) g_win[n] = (float)(dt * 1000);
        n++;
        if (dt > worst) worst = dt;
        if (dt > 0.025) late++;
        if (dt > 0.050) hitch++;
        if (dt > 0.100) fprintf(stderr, "[hitch] %.1f ms at %.1f s\n", dt * 1000, t - g_t_start);
    }
    else t0 = t;
    last = t;
    if (g_interval > 0 && n > 0 && t - t0 >= g_interval)
    {
        int m = n < MAX_WIN ? n : MAX_WIN;
        qsort(g_win, m, sizeof g_win[0], cmp_float);
        float p99 = g_win[(int)(m * 0.99) < m ? (int)(m * 0.99) : m - 1];
        if (!mono_gc_count)                   // once Unity has loaded Mono (never loads it here)
        {
            void *h = mono_runtime_lib();
            if (h) { mono_gc_count = (int (*)(int))so_dlsym(h, "mono_gc_collection_count");
                     mono_heap = (int64_t (*)(void))so_dlsym(h, "mono_gc_get_heap_size"); }
        }
        int gc = mono_gc_count ? mono_gc_count(0) : -1;
        long rss_pages = 0;
        FILE *sm = fopen("/proc/self/statm", "r");
        if (sm) { if (fscanf(sm, "%*s %ld", &rss_pages) != 1) rss_pages = 0; fclose(sm); }
        double shown_avg = 0, shown_max = 0, waited = 0, queued = 0;
        char shown[160] = "";
        if (fbdev_take_stats(&shown_avg, &shown_max, &waited, &queued) > 0)
            snprintf(shown, sizeof shown, ", shown %.1f avg / %.1f worst ms after the swap (%d buffers, queue %.1f, gate %.1f ms a frame)",
                     shown_avg, shown_max, fbdev_buffers(), queued / n, waited / n);
        fprintf(stderr, "[fps] %.1f fps, frame %.1f avg / %.1f p99 / %.1f worst ms, late %d hitch %d, gc %d heap %lld MB (%d frames, %d total, %.0f s) rss %ld MB%s\n",
                n / sum, 1000 * sum / n, p99, 1000 * worst, late, hitch, gc_prev >= 0 && gc >= 0 ? gc - gc_prev : 0,
                mono_heap ? (long long)(mono_heap() >> 20) : -1LL, n, total, t - g_t_start, rss_pages * (sysconf(_SC_PAGESIZE) >> 10) >> 10, shown);
        gc_prev = gc;
        t0 = t; sum = 0; worst = 0; n = 0; late = 0; hitch = 0;
    }
}

static void on_swap_done(void *ctx, int ok)
{
    (void)ctx;
    fbdev_swap_done(ok);                        // host flips: wait for the next frame's buffer (fbdev.c)
}

void frames_init(const char *root, int width, int height)
{
    snprintf(g_root, sizeof g_root, "%s", root);
    g_width = width; g_height = height;
    if (getenv("UA_FPS")) g_interval = atof(getenv("UA_FPS"));
    snprintf(g_req, sizeof g_req, "%s/shot.req", root);
    egl_glue_set_swap_hook(on_swap, NULL);
    egl_glue_set_swap_done_hook(on_swap_done, NULL);
    egl_glue_set_window_hook(fbdev_new_window);
}
