// The fbdev display path: Mali's fbdev EGL (Knulli on the Allwinner H700 and other fbdev-flavor
// Mali blobs) renders the window into the framebuffer itself and shows each finished frame by
// panning to it (FBIOPAN_DISPLAY). The pan returns at the vblank that shows the frame (the sunxi
// driver waits for it, eglSwapInterval 0 included), and the driver makes it on the thread that also
// feeds the GPU (mali-cmar-backend): while it waits, the GPU gets no work, and the next frame starts
// at the vblank. A frame that needs 18 ms of GPU time is shown at 30 fps, with the GPU idle a third
// of the time (and clocked down for it), whatever the number of buffers (RG34XX-SP: Cell Machine
// 29.7 fps with 2 or 3; 53.6 with host flips).
//
// Host flips (UA_FB_FLIP=host, the default). ua_host exports ioctl, so the driver's calls come here
// first (every other request goes straight on to the C library). The driver's pan is queued to a
// thread of ours, "ua_flip", which makes it (waiting for the vblank there), in order, and the
// driver's call returns at once: the driver's thread goes on feeding the GPU. It reads the screen
// info before each pan, which the kernel would answer only once a pending pan is done (the fb
// lock): those reads are answered from the host's copy. The driver takes the return as "on the
// screen" and gives the buffer shown before to the next frame to be drawn, which may still be on
// the screen until our flip happens. So the framebuffer gets more buffers (UA_FB_BUFFERS, below) and
// Unity's render thread waits after each eglSwapBuffers until the buffer its next frame will draw
// into has really left the screen: the driver uses its buffers in turn (it pans to (shown + 1) %
// buffers), so frame k + 1 draws into the buffer of frame k + 1 - buffers, which leaves the screen
// with the flip of frame k + 2 - buffers. Nothing the GPU draws into is on the screen: no tearing,
// and every frame is shown, in order, each at a vblank (test/test_fbdev.c).
//   UA_FB_QUEUE=<n>   frames the render thread may be ahead of the screen (the flips of frames
//                     k - n + 1 .. k may be pending when it starts frame k + 1), at most buffers - 2
//                     and 2. Each queued frame is up to one refresh more latency. Default (auto): 2
//                     while frames miss refreshes (the GPU always has the next frame: the most frames
//                     a second), 1 once 30 frames in a row were done while the flip before was still
//                     pending (frames keep up with the refresh, and a second queued frame would only
//                     be latency); back to 2 at a missed refresh, for 2 s (doubling each time, up to
//                     a minute, so a game at the edge does not go back and forth).
// UA_FB_FLIP=driver: the driver's own pans (the firmware's two buffers by default); UA_FB_FLIP=mailbox:
// a measurement, tears: the driver's pan returns at once and only the newest frame is flipped to,
// so the frame rate is what the frames cost (more than the refresh rate when they are cheaper).
//
// Buffers. The driver takes as many window buffers as the framebuffer's virtual height holds
// screens (yres_virtual / yres, read when the EGL display is initialized; more than one needs the
// framebuffer exported as a DMA-BUF, FBIOGET_DMABUF, which sunxi's fbdev does).
//   UA_FB_BUFFERS=<n>  the framebuffer's virtual height set to n screens before Unity starts, when its
//                      memory (smem_len) holds them. Default: 4 for host flips (3 if the memory holds
//                      no more; host flips need 3), else the firmware's. The geometry it had is put
//                      back when the host ends, however it ends (a small watcher process,
//                      "ua_fbrestore", puts it back if the host is killed)
//
// Flip times: each frame's flip is matched with its eglSwapBuffers (fbdev_swap, in the order they
// were made): frames.c logs the time from the swap to the end of its flip. UA_FB_TRACE=<n>: the
// times of n swaps, pans and flips, UA_FB_TRACE_START s (40) after the first swap.
//
// A window smaller than the screen (the render size). Mali's driver draws a window smaller than the
// framebuffer into the top left of each buffer, at the framebuffer's pitch, the buffers still one
// screen apart, and pans with the framebuffer's own size (RG34XX-SP: a 540x360 window in the
// 720x480 buffers). sunxi's fbdev takes a pan's xres/yres as the display layer's source crop and
// leaves its screen window at the panel's size: so the pans carry the window's size (made here, on
// whichever thread makes them), and the display engine's scaler draws the window over the whole
// panel, with no GPU or CPU work (checked with the display engine's own capture of what it sends to
// the panel). The crop follows the window the driver draws (fbdev_new_window: the render size, or
// a smaller buffer geometry the game set) and is put back to the whole screen when the host ends,
// as the geometry is (ua_fbrestore too).
//   UA_RENDER_SIZE=<w>x<h> | UA_RENDER_SCALE=<s>  Unity's window at that size (fbdev_render_size:
//                     display.c sizes the ANativeWindow with it, so the EGL surface, Screen.width
//                     and height, DisplayMetrics, the View, pointer mode and screenshots all have
//                     it): at the panel's aspect, even, a quarter of the panel's size at least; a
//                     size of another aspect is made the largest one of the panel's aspect within
//                     it. Off by default (the picture is softer: a visual setting).
//   UA_RENDER_RAM=<MB> only on a device with at most that much memory (MemTotal), e.g. 1024: a
//                     port's default for 1 GB handhelds that leaves bigger ones at the panel's size
#define _GNU_SOURCE
#include "fbdev.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static char g_path[256];
static struct fb_var_screeninfo g_orig;     // the geometry before ours
static unsigned g_ours_vy;                  // the virtual height we set (restore only if still ours)
static volatile int g_changed;
static int g_buffers;
static int g_watch_fd = -1;                 // write end of the watcher's pipe: closes when the host ends
static volatile int g_track;                // fbdev path: flips are counted
// Host flips: FBIOGET_VSCREENINFO on the framebuffer answered from here (the kernel's answer waits
// for the fb lock, which a pan holds until its vblank: the driver asks before each pan)
static struct fb_var_screeninfo g_var;
static dev_t g_rdev;
static volatile int g_var_ok;
// A window smaller than the screen: the panel's size, the render size, the crop the pans carry (the
// window the driver draws, 0 while it is the whole screen), and whether the crop needs putting back
static int g_fb_w, g_fb_h;
static int g_rw, g_rh;
static volatile int g_cw, g_ch;
static volatile int g_crop;

static long fb_ioctl(int fd, unsigned long req, void *arg) { return syscall(SYS_ioctl, fd, req, arg); }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void say(const char *s) { (void)!write(2, s, strlen(s)); }

// async-signal-safe: also run by the watcher and the crash handler. The crop: a pan to the buffer on
// the screen with the framebuffer's own size (the kernel's var, which pans never change), so the
// display layer shows the whole buffer again; the geometry: put back if it is still ours (a put
// pans with the old geometry's size too).
static int restore_now(void)
{
    g_var_ok = 0;                            // the kernel's answers again
    if (!g_changed && !g_crop) return 0;
    int fd = open(g_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    struct fb_var_screeninfo v;
    int done = 0;
    if (fb_ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0) {
        if (g_crop) {
            struct fb_var_screeninfo p = v;
            done |= fb_ioctl(fd, FBIOPAN_DISPLAY, &p) == 0 ? 2 : 0;
        }
        if (g_changed && v.yres_virtual == g_ours_vy) {
            struct fb_var_screeninfo o = g_orig;
            o.activate = FB_ACTIVATE_NOW;
            done |= fb_ioctl(fd, FBIOPUT_VSCREENINFO, &o) == 0 ? 1 : 0;
        }
    }
    close(fd);
    g_changed = 0;
    g_crop = 0;
    return done;
}

void fbdev_restore(void)
{
    g_cw = g_ch = 0;                         // a flip still made after this one shows the whole buffer
    int done = restore_now();
    if (done & 1) say("[fbdev] framebuffer geometry put back\n");
    if (done & 2) say("[fbdev] the display layer shows the whole framebuffer again\n");
}

// A process of its own (its own session: a kill of the host's process group leaves it) that waits
// for the host to end (its pipe's write end closes with the host, whatever ends it: SIGKILL, a
// crash) and then puts the geometry back if the host could not.
static void start_watcher(void)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return; }
    if (pid == 0) {
        close(p[1]);
        for (int fd = 3; fd < 1024; fd++) if (fd != p[0]) close(fd);
        prctl(PR_SET_NAME, "ua_fbrestore", 0, 0, 0);
        setsid();                            // a kill of the host's process group leaves it
        signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGQUIT, SIG_IGN);
        g_crop = 1;                          // (a crop the host makes after the fork: always the whole screen)
        char c;
        while (read(p[0], &c, 1) < 0 && errno == EINTR) {}
        if (restore_now() & 1) say("[fbdev] framebuffer geometry put back (watcher)\n");
        _exit(0);
    }
    close(p[0]);
    g_watch_fd = p[1];
}

int fbdev_buffers(void) { return g_buffers; }

// UA_RENDER_SIZE / UA_RENDER_SCALE (fbdev.h) for a pw x ph panel and mem_kb of memory: 1 and the size,
// 0 for the panel's own. The size is m times the aspect's smallest whole step (720x480: 3x2), m
// even where a step is odd, so both sides are even; a quarter of the panel at least.
int fbdev_pick_render_size(int pw, int ph, const char *size, const char *scale, const char *ram, long mem_kb, int *w, int *h)
{
    if (pw <= 0 || ph <= 0) return 0;
    double s = 0;
    int sw = 0, sh = 0;
    if (size && *size) {
        if (sscanf(size, "%dx%d", &sw, &sh) != 2 || sw <= 0 || sh <= 0) {
            fprintf(stderr, "[fbdev] UA_RENDER_SIZE=%s: <width>x<height>; the panel's size\n", size);
            return 0;
        }
        s = (double)sw / pw < (double)sh / ph ? (double)sw / pw : (double)sh / ph;
    } else if (scale && *scale) {
        s = atof(scale);
        if (s <= 0) { fprintf(stderr, "[fbdev] UA_RENDER_SCALE=%s: a number up to 1; the panel's size\n", scale); return 0; }
    } else {
        return 0;
    }
    if (ram && *ram && atol(ram) > 0 && mem_kb > 0 && mem_kb > atol(ram) * 1024L) {
        fprintf(stderr, "[fbdev] render size: the device has %ld MB, more than UA_RENDER_RAM=%s: the panel's size\n", mem_kb >> 10, ram);
        return 0;
    }
    if (s >= 1) return 0;
    if (s < 0.25) s = 0.25;
    int a = pw, b = ph;
    while (b) { int t = a % b; a = b; b = t; }         // a = gcd: the aspect's step is (pw / a) x (ph / a)
    int ux = pw / a, uy = ph / a, even = (ux | uy) & 1;
    if (a < 16) {                                       // (854x480: no whole step worth taking) the nearest even sizes
        *w = (int)(pw * s + 1) & ~1;
        *h = (int)(ph * s + 1) & ~1;
        if (*w >= pw || *h >= ph) return 0;
    } else {
        int m = (int)(s * a + 0.5);
        if (even) m &= ~1;
        if (m < 1 || m * ux < pw / 4 || m * uy < ph / 4) m += even ? 2 : 1;
        if (m >= a) return 0;
        *w = m * ux;
        *h = m * uy;
    }
    if (size && *size && (*w != sw || *h != sh))
        fprintf(stderr, "[fbdev] UA_RENDER_SIZE=%s: %dx%d (the panel's aspect, even)\n", size, *w, *h);
    return 1;
}

int fbdev_render_size(int *w, int *h)
{
    if (!g_rw) return 0;
    *w = g_rw;
    *h = g_rh;
    return 1;
}

static long mem_total_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    long kb = 0;
    char line[128];
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) break;
    if (f) fclose(f);
    return kb;
}

enum { FLIP_DRIVER, FLIP_HOST, FLIP_MAILBOX };
static int g_mode = FLIP_DRIVER;
static int g_queue;                         // host flips: frames the render thread may be ahead now
static int g_queue_max, g_queue_auto;       // at most buffers - 2; auto: 1 while frames keep up
static double g_period = 1 / 60.0;          // the refresh period (an average of back-to-back flips)

static int screens_in_memory(const struct fb_var_screeninfo *v, const struct fb_fix_screeninfo *f)
{
    return f->line_length && v->yres ? (int)(f->smem_len / ((unsigned long long)f->line_length * v->yres)) : 0;
}

void fbdev_setup(void)
{
    g_track = 1;
    const char *dev = getenv("ANDROID_FBDEV");
    snprintf(g_path, sizeof g_path, "%s", dev && *dev ? dev : "/dev/fb0");
    const char *fm = getenv("UA_FB_FLIP");
    g_mode = !fm || !*fm ? FLIP_HOST : !strcmp(fm, "host") ? FLIP_HOST : !strcmp(fm, "mailbox") ? FLIP_MAILBOX : FLIP_DRIVER;
    if (fm && *fm && strcmp(fm, "host") && strcmp(fm, "mailbox") && strcmp(fm, "driver"))
        fprintf(stderr, "[fbdev] UA_FB_FLIP=%s: host, driver or mailbox; the driver's flips\n", fm);
    const char *e = getenv("UA_FB_BUFFERS");
    int want = e && *e ? atoi(e) : 0;
    int fd = open(g_path, O_RDWR | O_CLOEXEC);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (fd < 0 || fb_ioctl(fd, FBIOGET_VSCREENINFO, &v) != 0 || fb_ioctl(fd, FBIOGET_FSCREENINFO, &f) != 0 || !v.yres) {
        if (fd >= 0) close(fd);
        if (g_mode != FLIP_DRIVER) fprintf(stderr, "[fbdev] %s: no framebuffer geometry; the driver's flips\n", g_path);
        g_mode = FLIP_DRIVER;
        return;
    }
    g_buffers = v.yres_virtual / v.yres;
    int fit = screens_in_memory(&v, &f);
    g_fb_w = v.xres;
    g_fb_h = v.yres;
    if (fbdev_pick_render_size(v.xres, v.yres, getenv("UA_RENDER_SIZE"), getenv("UA_RENDER_SCALE"), getenv("UA_RENDER_RAM"),
                               mem_total_kb(), &g_rw, &g_rh)) {
        g_cw = g_rw;
        g_ch = g_rh;
        g_crop = 1;                          // before the watcher starts: it puts the crop back
        fprintf(stderr, "[fbdev] render size %dx%d (%.0f %% of %ux%u's pixels): the display engine scales the window to the panel\n",
                g_rw, g_rh, 100.0 * g_rw * g_rh / ((double)v.xres * v.yres), v.xres, v.yres);
    }
    // the refresh period from the mode's timings, when the driver gives them (else 60 Hz until the
    // flips tell)
    double ht = (double)v.xres + v.left_margin + v.right_margin + v.hsync_len, vt = (double)v.yres + v.upper_margin + v.lower_margin + v.vsync_len;
    double p = v.pixclock ? v.pixclock * 1e-12 * ht * vt : 0;
    if (p > 1 / 250.0 && p < 1 / 20.0) g_period = p;
    if (want <= 0 && g_mode == FLIP_HOST && g_buffers < 4) want = fit >= 4 ? 4 : 3;
    if (want > 0 && want != g_buffers) {
        struct fb_var_screeninfo n = v, got;
        n.yres_virtual = v.yres * want;
        n.yoffset = 0;
        n.activate = FB_ACTIVATE_NOW;
        if (want > fit) {
            fprintf(stderr, "[fbdev] %d buffers: the framebuffer's memory holds %d screens\n", want, fit);
        } else {
            g_orig = v;
            g_ours_vy = n.yres_virtual;
            g_changed = 1;                   // before the call: a crash right after it still restores
            start_watcher();
            atexit(fbdev_restore);           // the host's early returns (its normal end calls it itself)
            if (fb_ioctl(fd, FBIOPUT_VSCREENINFO, &n) != 0 || fb_ioctl(fd, FBIOGET_VSCREENINFO, &got) != 0 || !got.yres) {
                fprintf(stderr, "[fbdev] %d buffers: the driver refused the virtual size %ux%u (%s)\n", want,
                        n.xres_virtual, n.yres_virtual, strerror(errno));
                int crop = g_crop;
                restore_now();
                g_crop = crop;
                if (!crop && g_watch_fd >= 0) { close(g_watch_fd); g_watch_fd = -1; }   // the watcher has nothing to do
            } else {
                g_ours_vy = got.yres_virtual;
                g_buffers = got.yres_virtual / got.yres;
            }
        }
    }
    if (g_crop && g_watch_fd < 0) {          // the render size alone: the crop is put back too
        start_watcher();
        atexit(fbdev_restore);
    }
    close(fd);
    if (g_mode == FLIP_HOST && g_buffers < 3) {
        fprintf(stderr, "[fbdev] host flips need 3 buffers (%d): the driver's flips\n", g_buffers);
        g_mode = FLIP_DRIVER;
    }
    if (g_mode != FLIP_DRIVER) {
        int fd2 = open(g_path, O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (fd2 >= 0 && fstat(fd2, &st) == 0 && fb_ioctl(fd2, FBIOGET_VSCREENINFO, &g_var) == 0) { g_rdev = st.st_rdev; g_var_ok = 1; }
        if (fd2 >= 0) close(fd2);
    }
    const char *q = getenv("UA_FB_QUEUE");
    g_queue_max = g_buffers - 2 < 2 ? g_buffers - 2 : 2;     // a third queued frame adds latency, not frames
    g_queue_auto = !(q && *q && atoi(q) > 0);
    g_queue = g_queue_auto ? g_queue_max : atoi(q) < g_queue_max ? atoi(q) : g_queue_max;
    char how[96];
    if (g_mode == FLIP_HOST && g_queue_auto) snprintf(how, sizeof how, "host flips, 1 - %d frames queued", g_queue_max);
    else if (g_mode == FLIP_HOST) snprintf(how, sizeof how, "host flips, up to %d frame%s queued", g_queue, g_queue == 1 ? "" : "s");
    else snprintf(how, sizeof how, "%s", g_mode == FLIP_MAILBOX ? "mailbox flips (tears: a measurement)" : "the driver's flips");
    fprintf(stderr, "[fbdev] %ux%u, %d buffers (virtual %ux%u%s), %s\n", v.xres, v.yres, g_buffers, v.xres_virtual,
            g_buffers * v.yres, g_changed ? ", set by the host" : "", how);
}

// ---- flips -------------------------------------------------------------------------------------
#define QN 16
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond;               // a flip completed, or the queue has room (CLOCK_MONOTONIC)
static pthread_once_t g_cond_once = PTHREAD_ONCE_INIT;
static double g_q[QN];                      // swap times of the frames not shown yet
static int g_qh, g_qn;
static double g_sum, g_max, g_waited;
static int g_n;
static unsigned long g_swaps, g_flips;      // swaps made, flips completed (the first frame is 0)
static long g_skew;                         // swaps that never flipped (a gate timeout's correction),
                                            // and the frames of earlier windows
static unsigned g_seen;                     // buffers (yoffset / yres) the driver showed
static int g_seen_logged, g_order_warned, g_gate_warned;
static unsigned g_last_y = ~0u;
static double g_last_flip;
static int g_streak;                        // pans in a row that found the previous flip pending
static double g_hold_until, g_hold = 2;     // auto queue: buffers - 2 until then (after a miss)
static double g_hold_base = 2;              // the first hold (s)
static double g_queue_sum;                  // sum of the queue depth over the frames (stats)
static double g_q1_since;                   // auto queue: when it last went down to 1
static int g_inflight;                      // ua_flip is in the driver's pan (a flip is pending)
#define FQN 8                               // the driver's pans not made yet (host and mailbox flips)
static struct fb_var_screeninfo g_fq[FQN];
static int g_fq_fd[FQN], g_fqh, g_fqn;
static double g_fq_t[FQN];                  // when the driver's pan came

static void cond_init(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &a);
    pthread_condattr_destroy(&a);
}

// UA_FB_TRACE=<n>: the times of <n> swaps and flips, UA_FB_TRACE_START s (40) after the first swap
static int g_trace_n = -1;
static double g_trace_t0, g_trace_from;
static void trace(const char *what, double t, unsigned y, double t_call)
{
    if (g_trace_n < 0) {
        const char *e = getenv("UA_FB_TRACE"), *st = getenv("UA_FB_TRACE_START");
        g_trace_n = e ? atoi(e) : 0;
        g_trace_from = st ? atof(st) : 40;
    }
    if (g_trace_n <= 0) return;
    if (!g_trace_t0) g_trace_t0 = t;
    double rel = t - g_trace_t0;
    if (rel < g_trace_from) return;
    if (t_call > 0) fprintf(stderr, "[fbtrace] %9.3f %s y %u (called %.3f, %.1f ms)\n", rel * 1000, what, y, (t_call - g_trace_t0) * 1000, (t - t_call) * 1000);
    else fprintf(stderr, "[fbtrace] %9.3f %s\n", rel * 1000, what);
    if (!strcmp(what, "swap")) g_trace_n--;
}

void fbdev_swap(void)
{
    if (!g_track) return;
    pthread_once(&g_cond_once, cond_init);
    double t = now();
    trace("swap", t, 0, 0);
    pthread_mutex_lock(&g_lock);
    if (g_qn == QN) { g_qh = (g_qh + 1) % QN; g_qn--; }      // flips stopped matching: forget the oldest
    g_q[(g_qh + g_qn) % QN] = t;
    g_qn++;
    g_swaps++;
    pthread_mutex_unlock(&g_lock);
}

// Host flips, after the swap of frame k = g_swaps - 1 (frame j has flipped once g_flips > j): frame
// k + 1 draws into the buffer frame k + 1 - buffers was shown from, off the screen once frame
// k + 2 - buffers has flipped (g_flips >= k + 3 - buffers). With up to `queue` frames queued, frame
// k - queue must have flipped (g_flips >= k + 1 - queue); queue <= buffers - 2 makes that the stricter.
static int g_timeouts;                      // gate waits in a row that ran out
void fbdev_swap_done(int ok)
{
    if (g_mode != FLIP_HOST) return;
    double t0 = now();
    pthread_mutex_lock(&g_lock);
    if (!ok) {                                  // a failed swap: the driver flips nothing for it
        g_skew++;
        if (g_qn) g_qn--;                       // (its swap time, the newest, waits for no flip)
    }
    long k = (long)g_swaps - 1 - g_skew;
    long need = k + 1 - g_queue;
    g_queue_sum += g_queue;
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_nsec += 500 * 1000000L;            // a GPU that hangs, or a frame that never flips, must not hang Unity
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    int timed_out = 0;
    while ((long)g_flips < need && !timed_out)
        timed_out = pthread_cond_timedwait(&g_cond, &g_lock, &until) == ETIMEDOUT;
    if (timed_out && (long)g_flips < need) {
        // a late flip comes still (the next gate waits for it again); three in a row: frames that
        // never flip, counted out
        if (++g_timeouts >= 3) { g_skew += need - (long)g_flips; g_timeouts = 0; }
        if (!g_gate_warned++) fprintf(stderr, "[fbdev] a frame was not flipped within 500 ms of its swap\n");
    } else {
        g_timeouts = 0;
    }
    g_waited += now() - t0;
    pthread_mutex_unlock(&g_lock);
}

// The crop of the pans from now on: the new window's size when it is smaller than the screen
static void set_crop(int w, int h)
{
    int cw = 0, ch = 0;
    if (g_fb_w > 0 && w > 0 && h > 0 && w <= g_fb_w && h <= g_fb_h && (w < g_fb_w || h < g_fb_h)) { cw = w; ch = h; }
    if (cw == g_cw && ch == g_ch) return;
    g_cw = cw;
    g_ch = ch;
    if (cw) fprintf(stderr, "[fbdev] window %dx%d: the display engine scales it to %dx%d\n", cw, ch, g_fb_w, g_fb_h);
    else fprintf(stderr, "[fbdev] window %dx%d: the whole screen\n", w, h);
}

// A new window surface takes the buffer on the screen (the framebuffer's yoffset, which our flips
// update when they happen) as shown and draws its first frame into the next one. Once every queued
// flip has happened, that is where the frame count's turn already is; frames that never flipped
// (a swap of a window going away) are counted out. Its size gives the crop of the pans from then
// on (the old window's queued flips keep theirs: they are waited for first).
void fbdev_new_window(int w, int h)
{
    if (g_mode != FLIP_HOST) { if (g_track) set_crop(w, h); return; }
    pthread_once(&g_cond_once, cond_init);
    pthread_mutex_lock(&g_lock);
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_nsec += 200 * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    while (((long)g_flips < (long)g_swaps - g_skew || g_fqn) && pthread_cond_timedwait(&g_cond, &g_lock, &until) != ETIMEDOUT) {}
    g_skew = (long)g_swaps - (long)g_flips;
    g_last_y = ~0u;                             // the turn check starts again
    set_crop(w, h);
    pthread_mutex_unlock(&g_lock);
}

int fbdev_take_stats(double *avg_ms, double *max_ms, double *waited_ms, double *queue_sum)
{
    pthread_mutex_lock(&g_lock);
    int n = g_n;
    if (avg_ms) *avg_ms = n ? 1000 * g_sum / n : 0;
    if (max_ms) *max_ms = 1000 * g_max;
    if (waited_ms) *waited_ms = 1000 * g_waited;
    if (queue_sum) *queue_sum = g_queue_sum;
    g_sum = g_max = g_waited = g_queue_sum = 0;
    g_n = 0;
    pthread_mutex_unlock(&g_lock);
    return n;
}

// A flip completed at t (the driver's own, or ours).
// b2b: our flip thread had this frame queued before the flip before it was done, so the two flips
// are one refresh apart (a kernel that missed one aside).
static void flipped(double t, unsigned yoffset, unsigned yres, double t_call, int b2b)
{
    trace("flip", t, yoffset, t_call);
    pthread_once(&g_cond_once, cond_init);
    pthread_mutex_lock(&g_lock);
    if (g_qn) {                              // a pan before the first swap is not a frame
        double l = t - g_q[g_qh];
        g_qh = (g_qh + 1) % QN;
        g_qn--;
        g_sum += l;
        if (l > g_max) g_max = l;
        g_n++;
    }
    g_flips++;
    int report = 0, order = 0;
    if (yres && yoffset / yres < 32) {
        unsigned bit = 1u << (yoffset / yres);
        if (!(g_seen & bit)) { g_seen |= bit; report = g_seen_logged < 8; g_seen_logged++; }
    }
    // the gate counts on the driver using its buffers in turn: if it does not, its own flips
    if (g_mode != FLIP_MAILBOX && g_last_y != ~0u && g_buffers > 0 && yres && yoffset != (g_last_y + yres) % (g_buffers * yres) && !g_order_warned) {
        order = g_order_warned = 1;
        if (g_mode == FLIP_HOST) g_mode = FLIP_DRIVER;
    }
    g_last_y = yoffset;
    double iv = g_last_flip > 0 ? t - g_last_flip : 0;
    g_last_flip = t;
    if (b2b && iv > 0.002 && iv < 1.4 * g_period) g_period += (iv - g_period) / 16;
    if (g_mode == FLIP_HOST && g_queue_auto && iv > 1.5 * g_period && g_queue < g_queue_max) {
        g_queue = g_queue_max;                       // a missed refresh: the GPU gets the next frame again
        if (t - g_q1_since > 30) g_hold = g_hold_base;   // a lone miss (a load) after a long while at 1
        g_hold_until = t + g_hold;
        g_hold = g_hold * 2 < 64 ? g_hold * 2 : 64;
        g_streak = 0;
    }
    unsigned seen = g_seen;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    if (report) fprintf(stderr, "[fbdev] the driver flips to the buffer at y %u (buffers shown so far: 0x%x)\n", yoffset, seen);
    if (order) fprintf(stderr, "[fbdev] WARNING: the driver flipped to y %u out of turn (%d buffers): its own flips from now on\n", yoffset, g_buffers);
}

static int (*r_ioctl)(int, unsigned long, ...);
__attribute__((constructor)) static void fbdev_resolve(void) { r_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl"); }

// The driver's pan as the kernel gets it: with the crop of a window smaller than the screen (the
// offsets stay: the driver's buffers are a screen apart whatever its window's size)
static int pan(int fd, struct fb_var_screeninfo *v)
{
    int cw = g_cw, ch = g_ch;
    if (cw <= 0 || ch <= 0 || ((unsigned)cw >= v->xres && (unsigned)ch >= v->yres)) return r_ioctl(fd, FBIOPAN_DISPLAY, v);
    struct fb_var_screeninfo c = *v;
    c.xres = cw;
    c.yres = ch;
    g_crop = 1;
    return r_ioctl(fd, FBIOPAN_DISPLAY, &c);
}

// Host and mailbox flips: the driver's pans, made by "ua_flip" in order (mailbox: the newest only).
static pthread_once_t g_flip_once = PTHREAD_ONCE_INIT;
static int g_flip_ok;

static void *flip_main(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "ua_flip", 0, 0, 0);
    int warned = 0;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_fqn) pthread_cond_wait(&g_cond, &g_lock);
        struct fb_var_screeninfo v = g_fq[g_fqh];
        int fd = g_fq_fd[g_fqh];
        int b2b = g_fq_t[g_fqh] < g_last_flip;   // queued while the flip before was pending
        g_fqh = (g_fqh + 1) % FQN;
        g_fqn--;
        g_inflight = 1;
        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
        double tc = now();
        int r = pan(fd, &v);
        if (r != 0 && !warned++) fprintf(stderr, "[fbdev] FBIOPAN_DISPLAY to y %u failed: %s\n", v.yoffset, strerror(errno));
        if (r == 0) { pthread_mutex_lock(&g_lock); g_var.xoffset = v.xoffset; g_var.yoffset = v.yoffset; pthread_mutex_unlock(&g_lock); }
        flipped(now(), v.yoffset, v.yres, tc, b2b);   // a failed one counts too: the gate must go on
        pthread_mutex_lock(&g_lock);
        g_inflight = 0;
    }
    return NULL;
}

static void flip_start(void)
{
    pthread_once(&g_cond_once, cond_init);
    pthread_t t;
    if (pthread_create(&t, NULL, flip_main, NULL) == 0) { pthread_detach(t); g_flip_ok = 1; }
    else { fprintf(stderr, "[fbdev] no flip thread: the driver's flips\n"); g_mode = FLIP_DRIVER; }
}

// The libc's ioctl for everything but the driver's FBIOPAN_DISPLAY on the fbdev path.
int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!r_ioctl) return (int)syscall(SYS_ioctl, fd, req, arg);
    if (req == FBIOGET_VSCREENINFO && g_var_ok && arg && g_mode != FLIP_DRIVER) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == g_rdev) {
            pthread_mutex_lock(&g_lock);
            *(struct fb_var_screeninfo *)arg = g_var;
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }
    if (req != FBIOPAN_DISPLAY || !g_track || !arg) return r_ioctl(fd, req, arg);
    struct fb_var_screeninfo *v = arg;
    if (g_mode != FLIP_DRIVER) pthread_once(&g_flip_once, flip_start);
    if (g_mode != FLIP_DRIVER && g_flip_ok) {
        trace("pan", now(), v->yoffset, 0);
        pthread_mutex_lock(&g_lock);
        if (g_mode == FLIP_MAILBOX && g_fqn) {        // the newest frame replaces the one not shown yet
            g_fq[g_fqh] = *v;
            g_fq_fd[g_fqh] = fd;
            g_fq_t[g_fqh] = now();
            if (g_qn) { g_qh = (g_qh + 1) % QN; g_qn--; }   // its swap time: never shown
        } else {
            // auto queue: a frame done while the one before still waits for its flip came early
            if (g_queue_auto && g_mode == FLIP_HOST) {
                g_streak = g_fqn || g_inflight ? g_streak + 1 : 0;
                double t = now();
                if (g_streak >= 30 && g_queue > 1 && t >= g_hold_until) { g_queue = 1; g_q1_since = t; }
            }
            while (g_fqn == FQN) pthread_cond_wait(&g_cond, &g_lock);
            g_fq[(g_fqh + g_fqn) % FQN] = *v;
            g_fq_fd[(g_fqh + g_fqn) % FQN] = fd;
            g_fq_t[(g_fqh + g_fqn) % FQN] = now();
            g_fqn++;
        }
        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (g_flip_ok) {                                  // host flips gave up: their last pans first
        pthread_mutex_lock(&g_lock);
        while (g_fqn) pthread_cond_wait(&g_cond, &g_lock);
        pthread_mutex_unlock(&g_lock);
    }
    unsigned y = v->yoffset, yres = v->yres;
    double tc = now();
    int r = pan(fd, v);
    if (r == 0) flipped(now(), y, yres, tc, 0);
    return r;
}

#ifdef FBDEV_TEST
// test_fbdev.c: the flip modes without a framebuffer (mode 0 driver, 1 host, 2 mailbox), the fake
// display's FBIOPAN_DISPLAY in place of the C library's ioctl, a 720x480 panel (whole-screen windows
// until fbdev_new_window says otherwise).
void fbdev_test_init(int mode, int buffers, int queue, double period, int (*pan)(int, unsigned long, ...))
{
    g_fb_w = 720;
    g_fb_h = 480;
    g_cw = g_ch = 0;
    pthread_once(&g_cond_once, cond_init);
    pthread_mutex_lock(&g_lock);
    g_swaps = g_flips = 0;
    g_skew = 0;
    g_qh = g_qn = g_fqh = g_fqn = 0;
    g_last_y = ~0u;
    g_order_warned = g_gate_warned = g_timeouts = 0;
    pthread_mutex_unlock(&g_lock);
    g_track = 1;
    g_mode = mode;
    g_buffers = buffers;
    g_queue_max = buffers - 2 < 2 ? buffers - 2 : 2;
    g_queue_auto = queue == 0;
    g_queue = g_queue_auto ? g_queue_max : queue;
    g_streak = 0;
    g_hold_until = g_q1_since = 0;
    g_hold = g_hold_base = 0.2;                  // (the test's runs last a second or two)
    g_last_flip = 0;
    g_period = period;
    r_ioctl = pan;
}
#endif
