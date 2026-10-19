// Device check for the native Android-player route: can a 32-bit (armhf) program draw with the
// RGSP's Mali GPU through EGL on the fbdev window? Clears the screen red, green, blue, ... and times
// the swaps. Build: host/build.sh; run on the device: ./egltest [seconds]
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

struct fbdev_window { unsigned short width, height; };   // Mali fbdev native window

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 3.0;
    struct fb_var_screeninfo vi = {0};
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0) { perror("fb0"); return 1; }
    close(fd);
    printf("fb0: %ux%u (virtual %ux%u), %u bpp\n", vi.xres, vi.yres, vi.xres_virtual, vi.yres_virtual, vi.bits_per_pixel);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) { printf("eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    printf("EGL %d.%d, vendor %s, version %s\n", maj, min, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
    EGLint attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
                      EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) { printf("eglChooseConfig: none (0x%x)\n", eglGetError()); return 1; }
    static struct fbdev_window win;
    win.width = vi.xres; win.height = vi.yres;
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
        int c = (int)((now() - t0) * 2) % 3;
        glClearColor(c == 0, c == 1, c == 2, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(dpy, surf);
        frames++;
        last = now();
    }
    printf("%d frames in %.2f s: %.1f fps (vsync-limited if ~60)\n", frames, last - t0, frames / (last - t0));
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx); eglDestroySurface(dpy, surf); eglTerminate(dpy);
    return 0;
}
