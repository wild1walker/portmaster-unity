// UA_GL_DRIVER: the GLES driver Unity draws with. Unset or "mali" (the default): the system's libEGL
// and libGLESv2, the Mali blob on the handhelds. "mesa": Mesa's Panfrost (Gallium GLES) on the Mali
// kernel driver the handhelds have (kbase, job manager: Bifrost G31 on the H700 and RK3326), built
// by tools/mesa-kbase/ (its README): a test path, off by default, until it draws every
// frame as the blob does (glreplay --hash, glpixdiff) and costs no more.
//
// Mesa is started like this:
//   - gldriver_early (first thing in main): the program starts again (execv of /proc/self/exe, the
//     same arguments) with UA_MESA_DIR (default: the directory `mesa` beside ua_host, which holds
//     libEGL.so.1, libGLESv2.so.2 and libgallium-*.so) first in LD_LIBRARY_PATH, so every dlopen of
//     libEGL.so.1 / libGLESv2.so.2 (egl_glue, the GL hooks, SDL) gets Mesa's; EGL_PLATFORM=surfaceless
//     (Mesa built without window systems: its EGL has pbuffers only, on the kbase node: MESA_KBASE_NODE,
//     default /dev/mali0) and UA_EGL_PBUFFER=1 (egl_glue: Unity's window is a pbuffer, configs chosen
//     for pbuffers). Without libEGL.so.1 there: a warning, and the blob.
//   - display.c's fbdev path (UA_DISPLAY=auto picks it: UA_EGL_PBUFFER): fbdev_setup as for the blob
//     (buffers, host flips, render size, rotation), then gldriver_fbdev_present: each eglSwapBuffers of
//     Unity's window reads the frame back (glReadPixels, BGRA where the framebuffer is BGRA and the
//     driver has GL_EXT_read_format_bgra), copies it top-down into the framebuffer's next buffer and
//     pans to it. The pan is fbdev.c's (host flips: queued to ua_flip, the render thread waits for
//     the buffer its next frame goes to as with the blob). The blob draws into the framebuffer
//     itself: the read and the copy are the price of this path, logged every 10 s ("[gldriver]
//     present: ..."); UA_GLDRIVER_STATS=0 turns the log off.
#define _GNU_SOURCE
#include "gldriver.h"
#include "fbdev.h"
#include "../android/egl_glue.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_BGRA_EXT 0x80E1
#define GL_EXTENSIONS 0x1F03
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057

static int g_mesa = -1;

int gldriver_mesa(void)
{
    if (g_mesa < 0) {
        const char *d = getenv("UA_GL_DRIVER"), *set = getenv("UA_GL_DRIVER_MESA_DIR");
        g_mesa = d && !strcmp(d, "mesa") && set && *set;
    }
    return g_mesa;
}

void gldriver_early(char **argv)
{
    const char *d = getenv("UA_GL_DRIVER");
    if (!d || !*d || !strcmp(d, "mali")) return;
    if (strcmp(d, "mesa")) { fprintf(stderr, "[gldriver] UA_GL_DRIVER=%s: mali or mesa; the system's driver\n", d); return; }
    if (getenv("UA_GL_DRIVER_MESA_DIR")) return;                  // started again already
    char dir[1024];
    const char *md = getenv("UA_MESA_DIR");
    if (md && *md) snprintf(dir, sizeof dir, "%s", md);
    else {
        ssize_t k = readlink("/proc/self/exe", dir, sizeof dir - 8);
        if (k <= 0) { fprintf(stderr, "[gldriver] UA_GL_DRIVER=mesa: no /proc/self/exe; the system's driver\n"); return; }
        dir[k] = 0;
        char *sl = strrchr(dir, '/');
        if (!sl) return;
        strcpy(sl + 1, "mesa");
    }
    char egl[1100];
    snprintf(egl, sizeof egl, "%s/libEGL.so.1", dir);
    if (access(egl, R_OK) != 0) {
        fprintf(stderr, "[gldriver] UA_GL_DRIVER=mesa: no %s (UA_MESA_DIR); the system's driver\n", egl);
        return;
    }
    if (getenv("ANDROID_SHIM_LIBEGL") && *getenv("ANDROID_SHIM_LIBEGL"))
        fprintf(stderr, "[gldriver] UA_GL_DRIVER=mesa: ANDROID_SHIM_LIBEGL=%s wins over Mesa's libEGL\n", getenv("ANDROID_SHIM_LIBEGL"));
    const char *old = getenv("LD_LIBRARY_PATH");
    size_t n = strlen(dir) + (old ? strlen(old) : 0) + 2;
    char *lp = malloc(n);
    if (!lp) return;
    snprintf(lp, n, "%s%s%s", dir, old && *old ? ":" : "", old && *old ? old : "");
    char *saved_lp = old ? strdup(old) : NULL;
    setenv("LD_LIBRARY_PATH", lp, 1);
    setenv("UA_GL_DRIVER_MESA_DIR", dir, 1);
    setenv("EGL_PLATFORM", "surfaceless", 0);
    setenv("UA_EGL_PBUFFER", "1", 1);
    fprintf(stderr, "[gldriver] UA_GL_DRIVER=mesa: Mesa from %s (surfaceless EGL, the host shows the frames)\n", dir);
    fflush(stderr);
    execv("/proc/self/exe", argv);
    fprintf(stderr, "[gldriver] UA_GL_DRIVER=mesa: execv: %s; the system's driver\n", strerror(errno));
    if (saved_lp) setenv("LD_LIBRARY_PATH", saved_lp, 1); else unsetenv("LD_LIBRARY_PATH");
    unsetenv("UA_GL_DRIVER_MESA_DIR");
    unsetenv("UA_EGL_PBUFFER");
    free(saved_lp);
    free(lp);
}

void gldriver_copy_rows(unsigned char *dst, unsigned long dst_stride, const unsigned char *src, int w, int h, int swap_rb)
{
    size_t row = (size_t)w * 4;
    for (int y = 0; y < h; y++) {
        const unsigned char *s = src + (size_t)(h - 1 - y) * row;   // GL's rows are bottom-up
        unsigned char *d = dst + (size_t)y * dst_stride;
        if (!swap_rb) { memcpy(d, s, row); continue; }
        const uint32_t *s4 = (const uint32_t *)s;
        uint32_t *d4 = (uint32_t *)d;
        for (int x = 0; x < w; x++) {
            uint32_t p = s4[x];
            d4[x] = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
        }
    }
}

// ---- the fbdev present ------------------------------------------------------------------------
static struct {
    int fd, ok, cur, buffers, read_fmt, swap_rb;
    uint8_t *mem;
    size_t len;
    unsigned line;
    struct fb_var_screeninfo v;
    uint8_t *tmp;
    size_t tmp_len;
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    void (*GetIntegerv)(unsigned, int *);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*BindBuffer)(unsigned, unsigned);
    void (*PixelStorei)(unsigned, int);
    const unsigned char *(*GetString)(unsigned);
    unsigned (*QuerySurface)(void *, void *, int, int *);
    int stats;
    double t_last, read_s, copy_s;
    long frames;
} P = { .fd = -1 };

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void *gl_sym(const char *n)
{
    static void *gles;
    if (!gles) gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!gles) gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    void *p = gles ? dlsym(gles, n) : NULL;
    return p ? p : egl_glue_real(n);
}

// On the render thread at the first swap (Unity's context current): the framebuffer and the GL calls
static int present_init(void)
{
    const char *dev = getenv("ANDROID_FBDEV");
    const char *path = dev && *dev ? dev : "/dev/fb0";
    P.fd = open(path, O_RDWR | O_CLOEXEC);
    struct fb_fix_screeninfo f;
    if (P.fd < 0 || ioctl(P.fd, FBIOGET_VSCREENINFO, &P.v) != 0 || ioctl(P.fd, FBIOGET_FSCREENINFO, &f) != 0 || !P.v.yres) {
        fprintf(stderr, "[gldriver] %s: no framebuffer (%s): frames not shown\n", path, strerror(errno));
        return -1;
    }
    if (P.v.bits_per_pixel != 32) {
        fprintf(stderr, "[gldriver] %s: %u bits a pixel: only 32 are copied; frames not shown\n", path, P.v.bits_per_pixel);
        return -1;
    }
    P.len = f.smem_len;
    P.line = f.line_length;
    P.mem = mmap(NULL, P.len, PROT_READ | PROT_WRITE, MAP_SHARED, P.fd, 0);
    if (P.mem == MAP_FAILED) { P.mem = NULL; fprintf(stderr, "[gldriver] %s: mmap: %s\n", path, strerror(errno)); return -1; }
    P.buffers = fbdev_buffers();
    if (P.buffers <= 0) P.buffers = (int)(P.v.yres_virtual / P.v.yres);
    if (P.buffers < 1) P.buffers = 1;
    P.cur = (int)(P.v.yoffset / P.v.yres) % P.buffers;
    *(void **)&P.ReadPixels = gl_sym("glReadPixels");
    *(void **)&P.GetIntegerv = gl_sym("glGetIntegerv");
    *(void **)&P.BindFramebuffer = gl_sym("glBindFramebuffer");
    *(void **)&P.BindBuffer = gl_sym("glBindBuffer");
    *(void **)&P.PixelStorei = gl_sym("glPixelStorei");
    *(void **)&P.GetString = gl_sym("glGetString");
    *(void **)&P.QuerySurface = egl_glue_real("eglQuerySurface");
    if (!P.ReadPixels || !P.GetIntegerv || !P.BindFramebuffer || !P.BindBuffer || !P.PixelStorei || !P.GetString || !P.QuerySurface) {
        fprintf(stderr, "[gldriver] GL entry points missing: frames not shown\n");
        return -1;
    }
    // the framebuffer's byte order: BGRA (red at bit 16, the sunxi and Rockchip consoles) read as BGRA
    // where the driver can, else RGBA and red and blue swapped in the copy
    int fb_bgra = P.v.red.offset == 16 && P.v.blue.offset == 0;
    const char *ext = (const char *)P.GetString(GL_EXTENSIONS);
    int can_bgra = ext && strstr(ext, "GL_EXT_read_format_bgra") != NULL;
    P.read_fmt = fb_bgra && can_bgra ? GL_BGRA_EXT : GL_RGBA;
    P.swap_rb = fb_bgra && !can_bgra;
    const char *st = getenv("UA_GLDRIVER_STATS");
    P.stats = !(st && !strcmp(st, "0"));
    P.t_last = now_s();
    fprintf(stderr, "[gldriver] present: %ux%u, %d buffers, line %u bytes, read as %s%s\n", P.v.xres, P.v.yres, P.buffers, P.line,
            P.read_fmt == GL_BGRA_EXT ? "BGRA" : "RGBA", P.swap_rb ? " (red and blue swapped in the copy)" : "");
    return 0;
}

static int present(void *dpy, void *surface)
{
    if (!P.ok) P.ok = present_init() == 0 ? 1 : -1;
    if (P.ok < 0) return 1;                                       // (nothing shown: Unity goes on)
    int w = 0, h = 0;
    P.QuerySurface(dpy, surface, EGL_WIDTH, &w);
    P.QuerySurface(dpy, surface, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) return 1;
    if (w > (int)P.v.xres) w = (int)P.v.xres;
    if (h > (int)P.v.yres) h = (int)P.v.yres;
    size_t need = (size_t)w * h * 4;
    if (need > P.tmp_len) {
        free(P.tmp);
        P.tmp = malloc(need);
        P.tmp_len = P.tmp ? need : 0;
        if (!P.tmp) return 0;
    }
    double t0 = now_s();
    int rfb = 0, pbo = 0, align = 4, rl = 0, sr = 0, sp = 0;
    P.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    P.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    P.GetIntegerv(GL_PACK_ALIGNMENT, &align);
    P.GetIntegerv(GL_PACK_ROW_LENGTH, &rl);
    P.GetIntegerv(GL_PACK_SKIP_ROWS, &sr);
    P.GetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
    if (rfb) P.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (pbo) P.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (align != 4) P.PixelStorei(GL_PACK_ALIGNMENT, 4);
    if (rl) P.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (sr) P.PixelStorei(GL_PACK_SKIP_ROWS, 0);
    if (sp) P.PixelStorei(GL_PACK_SKIP_PIXELS, 0);
    P.ReadPixels(0, 0, w, h, P.read_fmt, GL_UNSIGNED_BYTE, P.tmp);
    if (rfb) P.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)rfb);
    if (pbo) P.BindBuffer(GL_PIXEL_PACK_BUFFER, (unsigned)pbo);
    if (align != 4) P.PixelStorei(GL_PACK_ALIGNMENT, align);
    if (rl) P.PixelStorei(GL_PACK_ROW_LENGTH, rl);
    if (sr) P.PixelStorei(GL_PACK_SKIP_ROWS, sr);
    if (sp) P.PixelStorei(GL_PACK_SKIP_PIXELS, sp);
    double t1 = now_s();
    int next = (P.cur + 1) % P.buffers;
    size_t base = (size_t)next * P.v.yres * P.line + (size_t)P.v.xoffset * 4;
    if (base + (size_t)(h - 1) * P.line + (size_t)w * 4 > P.len) return 0;
    gldriver_copy_rows(P.mem + base, P.line, P.tmp, w, h, P.swap_rb);
    double t2 = now_s();
    struct fb_var_screeninfo pv = P.v;
    pv.yoffset = (unsigned)next * P.v.yres;
    int r = ioctl(P.fd, FBIOPAN_DISPLAY, &pv);                    // fbdev.c's flips
    P.cur = next;
    P.read_s += t1 - t0;
    P.copy_s += t2 - t1;
    P.frames++;
    if (P.stats && t2 - P.t_last >= 10) {
        fprintf(stderr, "[gldriver] present: %ld frames, read %.2f ms, copy %.2f ms a frame (%dx%d)\n", P.frames,
                1000 * P.read_s / P.frames, 1000 * P.copy_s / P.frames, w, h);
        P.read_s = P.copy_s = 0;
        P.frames = 0;
        P.t_last = t2;
    }
    return r == 0;
}

void gldriver_fbdev_present(int panel_w, int panel_h)
{
    egl_glue_set_present(present, panel_w, panel_h);
}
