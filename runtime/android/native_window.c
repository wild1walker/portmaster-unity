// libandroid.so ANativeWindow_*: one window, sized like the framebuffer (or as the host says:
// android_window_set_size, e.g. the drawable size of the host's SDL window, runtime/host/display*.c).
//
// The ANativeWindow struct starts with a Mali fbdev `struct fbdev_window { u16 width, height; }`,
// so the pointer Unity hands to eglCreateWindowSurface is already a valid fbdev native window.
// egl_glue.c still translates it (android_window_native_handle) to refresh the size from the
// buffer geometry and to allow a host-provided native window instead. With a host display
// (egl_glue_set_host_display) egl_glue answers with the host's surface and only the size matters.
#define _GNU_SOURCE
#include "android_shims.h"
#include "shim_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ANW_MAGIC 0x414e5753u   /* 'ANWS' */

struct fbdev_window { unsigned short width, height; };

struct ANativeWindow {
    struct fbdev_window fb;     // must stay first (see above)
    uint32_t magic;
    int refcount;               // informational: the window is never freed
    int width, height;          // physical size
    int buf_width, buf_height;  // setBuffersGeometry (0 = physical size)
    int buf_format;             // setBuffersGeometry format (0 = default)
    void *native;               // host-provided EGLNativeWindowType, or NULL
};

static ANativeWindow g_win;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void query_size(int *w, int *h)
{
    const char *e = getenv("ANDROID_WINDOW_SIZE");
    if (e && sscanf(e, "%dx%d", w, h) == 2 && *w > 0 && *h > 0) return;
    const char *dev = getenv("ANDROID_FBDEV");
    int fd = open(dev && *dev ? dev : "/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct fb_var_screeninfo vi;
        memset(&vi, 0, sizeof vi);
        int ok = ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0 && vi.xres && vi.yres;
        close(fd);
        if (ok) { *w = vi.xres; *h = vi.yres; return; }
    }
    *w = 720; *h = 480;
    SHIM_WARN("no framebuffer size (open/ioctl /dev/fb0 failed), using %dx%d", *w, *h);
}

static void win_init(void)
{
    g_win.magic = ANW_MAGIC;
    if (!g_win.width) query_size(&g_win.width, &g_win.height);
    g_win.fb.width = g_win.width;
    g_win.fb.height = g_win.height;
    SHIM_TRACE("ANativeWindow %p: %dx%d", (void *)&g_win, g_win.width, g_win.height);
}

ANativeWindow *android_window_get(void)
{
    pthread_once(&g_once, win_init);
    return &g_win;
}

void android_window_set_size(int width, int height)
{
    pthread_mutex_lock(&g_lock);
    g_win.width = width;              // set before init so win_init keeps it
    g_win.height = height;
    pthread_mutex_unlock(&g_lock);
    android_window_get();
    pthread_mutex_lock(&g_lock);
    g_win.width = width;              // and again in case init ran first
    g_win.height = height;
    g_win.fb.width = g_win.buf_width ? g_win.buf_width : width;
    g_win.fb.height = g_win.buf_height ? g_win.buf_height : height;
    pthread_mutex_unlock(&g_lock);
}

void android_window_set_native(void *native) { android_window_get()->native = native; }

int android_window_is_ours(const void *p)
{
    return p && p == (const void *)&g_win && g_win.magic == ANW_MAGIC;
}

void *android_window_native_handle(ANativeWindow *w)
{
    if (!android_window_is_ours(w)) return NULL;
    pthread_mutex_lock(&g_lock);
    w->fb.width = w->buf_width ? w->buf_width : w->width;
    w->fb.height = w->buf_height ? w->buf_height : w->height;
    void *r = w->native ? w->native : (void *)&w->fb;
    pthread_mutex_unlock(&g_lock);
    return r;
}

ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env;
    ANativeWindow *w = android_window_get();
    if (!surface) return NULL;              // Android returns NULL for a null Surface
    ANativeWindow_acquire(w);               // fromSurface returns a reference
    SHIM_TRACE("ANativeWindow_fromSurface(%p) -> %p", surface, (void *)w);
    return w;
}

void ANativeWindow_acquire(ANativeWindow *w)
{
    if (w) __atomic_add_fetch(&w->refcount, 1, __ATOMIC_SEQ_CST);
}

void ANativeWindow_release(ANativeWindow *w)
{
    if (w) __atomic_sub_fetch(&w->refcount, 1, __ATOMIC_SEQ_CST);
}

int32_t ANativeWindow_getWidth(ANativeWindow *w)
{
    if (!w) return -EINVAL;
    return w->buf_width ? w->buf_width : w->width;
}

int32_t ANativeWindow_getHeight(ANativeWindow *w)
{
    if (!w) return -EINVAL;
    return w->buf_height ? w->buf_height : w->height;
}

int32_t ANativeWindow_getFormat(ANativeWindow *w)
{
    if (!w) return -EINVAL;
    return w->buf_format ? w->buf_format : WINDOW_FORMAT_RGBA_8888;
}

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format)
{
    if (!w || width < 0 || height < 0 || (!width) != (!height)) return -EINVAL;
    SHIM_TRACE("ANativeWindow_setBuffersGeometry(%d, %d, format %d)", width, height, format);
    pthread_mutex_lock(&g_lock);
    w->buf_width = width;
    w->buf_height = height;
    w->buf_format = format;
    w->fb.width = width ? width : w->width;
    w->fb.height = height ? height : w->height;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

// CPU rendering into the window is not supported (Unity renders with EGL only).
int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *out, ARect *dirty)
{
    (void)w; (void)out; (void)dirty;
    SHIM_WARN("ANativeWindow_lock: not supported");
    return -EINVAL;
}

int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) { (void)w; return -EINVAL; }
