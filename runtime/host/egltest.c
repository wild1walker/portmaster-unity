// Device check for the native Android-player route: can a 32-bit (armhf) program draw with the
// RGSP's Mali GPU through EGL on the fbdev window? Clears the screen red, green, blue, ... and times
// the swaps. Build: host/build.sh; run on the device: ./egltest [seconds]
//
// ./egltest <seconds> <w>x<h>: the render size's check (fbdev.c, UA_RENDER_SIZE): a w x h window
// smaller than the framebuffer, a still pattern (red, green, blue and yellow quarters from the top
// left, a magenta square near the top left corner, a 1-pixel white border and a dotted middle
// line), and every pan the driver makes carries w x h (this program's ioctl comes first, as
// ua_host's). Where the display engine scales the pan's size to the panel (sunxi's fbdev), the
// whole pattern fills the screen; where it does not, it shows at the top left at its own size.
// What the panel shows, while it runs (sunxi): echo /tmp/de.bmp > /sys/class/disp/disp/attr/capture_dump
// (a BMP of the panel's size), and the layer's crop and screen window: cat /sys/class/disp/disp/attr/sys.
// The framebuffer's geometry is put back afterwards (a pan with its own size) by a parent process,
// however the test ends.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

struct fbdev_window { unsigned short width, height; };   // Mali fbdev native window

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

// the render size's check: the driver's pans carry the window's size
static int g_w, g_h, g_pans;
static dev_t g_fb;
int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    struct stat st;
    if (req == FBIOPAN_DISPLAY && arg && g_w && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == g_fb) {
        struct fb_var_screeninfo v = *(struct fb_var_screeninfo *)arg;
        if (!g_pans++) printf("the driver pans with %ux%u at y %u: made with %dx%d\n", v.xres, v.yres, v.yoffset, g_w, g_h);
        v.xres = g_w;
        v.yres = g_h;
        return (int)syscall(SYS_ioctl, fd, req, &v);
    }
    return (int)syscall(SYS_ioctl, fd, req, arg);
}

static void rect(int x, int y, int w, int h, float r, float g, float b)   // from the top left
{
    glScissor(x, g_h - y - h, w, h);
    glClearColor(r, g, b, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void pattern(void)
{
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.25f, 0.25f, 0.25f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    int hw = g_w / 2, hh = g_h / 2;
    rect(8, 8, hw - 16, hh - 16, 1, 0, 0);
    rect(hw + 8, 8, hw - 16, hh - 16, 0, 1, 0);
    rect(8, hh + 8, hw - 16, hh - 16, 0, 0, 1);
    rect(hw + 8, hh + 8, hw - 16, hh - 16, 1, 1, 0);
    rect(24, 24, 16, 16, 1, 0, 1);
    rect(0, 0, g_w, 1, 1, 1, 1); rect(0, g_h - 1, g_w, 1, 1, 1, 1);
    rect(0, 0, 1, g_h, 1, 1, 1); rect(g_w - 1, 0, 1, g_h, 1, 1, 1);
    for (int x = 0; x < g_w; x += 2) rect(x, hh, 1, 1, 1, 1, 1);
    glDisable(GL_SCISSOR_TEST);
}

static int test(double secs, struct fb_var_screeninfo vi)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) { printf("eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    printf("EGL %d.%d, vendor %s, version %s\n", maj, min, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
    EGLint attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                      EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) { printf("eglChooseConfig: none (0x%x)\n", eglGetError()); return 1; }
    static struct fbdev_window win;
    win.width = g_w ? g_w : vi.xres; win.height = g_h ? g_h : vi.yres;
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)&win, NULL);
    if (surf == EGL_NO_SURFACE) { printf("eglCreateWindowSurface failed 0x%x\n", eglGetError()); return 1; }
    EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) { printf("context failed 0x%x\n", eglGetError()); return 1; }
    printf("GL_VENDOR %s\nGL_RENDERER %s\nGL_VERSION %s\nGL_SHADING_LANGUAGE_VERSION %s\n",
           glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    printf("ASTC LDR: %s\n", ext && strstr(ext, "GL_KHR_texture_compression_astc_ldr") ? "yes" : "no");
    double t0 = now(), last = t0; int frames = 0;
    while (now() - t0 < secs)
    {
        if (g_w) {
            pattern();
        } else {
            int c = (int)((now() - t0) * 2) % 3;
            glClearColor(c == 0, c == 1, c == 2, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        eglSwapBuffers(dpy, surf);
        frames++;
        last = now();
    }
    printf("%d frames in %.2f s: %.1f fps (vsync-limited if ~60)\n", frames, last - t0, frames / (last - t0));
    if (g_w) printf("%d of the driver's pans made with %dx%d%s\n", g_pans, g_w, g_h,
                    g_pans ? "" : " (none came through this program's ioctl: linked without -rdynamic?)");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx); eglDestroySurface(dpy, surf); eglTerminate(dpy);
    return 0;
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 3.0;
    struct fb_var_screeninfo vi = {0};
    struct stat st;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0 || fstat(fd, &st) != 0) { perror("fb0"); return 1; }
    printf("fb0: %ux%u (virtual %ux%u), %u bpp\n", vi.xres, vi.yres, vi.xres_virtual, vi.yres_virtual, vi.bits_per_pixel);
    if (argc > 2 && (sscanf(argv[2], "%dx%d", &g_w, &g_h) != 2 || g_w <= 0 || g_h <= 0 || g_w > (int)vi.xres || g_h > (int)vi.yres)) {
        printf("%s: <w>x<h> up to %ux%u\n", argv[2], vi.xres, vi.yres);
        return 1;
    }
    if (!g_w) { close(fd); return test(secs, vi); }
    g_fb = st.st_rdev;
    // the test in a child: this process puts the pan's size back however it ends
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        close(fd);
        alarm((unsigned)secs + 30);
        int r = test(secs, vi);
        fflush(stdout);
        _exit(r);
    }
    int status = 0;
    if (pid > 0) while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    struct fb_var_screeninfo cur;
    g_w = 0;
    int ok = ioctl(fd, FBIOGET_VSCREENINFO, &cur) == 0 && ioctl(fd, FBIOPAN_DISPLAY, &cur) == 0;
    printf("the pans' size put back to %ux%u: %s\n", cur.xres, cur.yres, ok ? "ok" : strerror(errno));
    close(fd);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
