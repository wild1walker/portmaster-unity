// Test of fbdev.c's flips without a device: a fake Mali fbdev driver and a fake display.
//   display  FBIOPAN_DISPLAY returns at the next vblank (every PERIOD_US) and shows the buffer then
//   driver   as Mali's r20p0 fbdev winsys: frames are drawn into the buffers in turn; a frame's buffer
//            is taken (dequeued) by the render thread, the GPU draws into it for gpu_us (one frame at
//            a time, in order), then the driver's thread pans to it, and once the pan returns, the
//            buffer shown before is free again (with UA_FB_FLIP=host that return comes at once)
//   render   Unity's render thread: cpu_us of work, dequeue, hand the frame to the GPU, swap
//            (fbdev_swap, then fbdev_swap_done)
// Checked: the GPU never draws into the buffer on the screen (tearing) nor into one a pending flip
// will show, every frame is flipped to, in order (not with mailbox flips: those drop frames), and
// host flips let the GPU work while a flip waits (frames slower than a refresh at more than half the
// refresh rate); a new window in the middle (fbdev_new_window) restarts at the buffer on the screen.
// A window smaller than the 720x480 screen: from its first frame on, every pan carries its size (the
// display engine's crop) at the same offsets; and the render sizes UA_RENDER_SIZE / UA_RENDER_SCALE
// pick.
// Built and run by fbdev_test.sh (armhf under QEMU, arm64).
#define _GNU_SOURCE
#include "../fbdev.h"

#include <linux/fb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

void fbdev_test_init(int mode, int buffers, int queue, double period, int (*pan)(int, unsigned long, ...));

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define YRES 480
#define PERIOD_US 16000
#define US(x) ((x) * PERIOD_US / 4000)   // times below are for a 4 ms refresh

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static void sleep_us(double us) { if (us > 0) { struct timespec t = { (time_t)(us / 1e6), (long)((us - (time_t)(us / 1e6) * 1e6) * 1000) }; nanosleep(&t, NULL); } }

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int B;
static int on_screen;                    // buffer the display shows
static int drawing = -1;                 // buffer the GPU draws into now
static int pending_flips[64], npending;  // buffers the driver panned to, not shown yet (host flips)
static int tears, overwrites, flips, out_of_order, last_flipped_frame;
static int crop_w, crop_h;               // the size the pans should carry (the window's)
static int pans_full, pans_cropped, pans_wrong;   // pans with the screen's size, the window's, another
static int frame_of_buffer[16];          // the frame last drawn into each buffer
static double t0;

// the display: FBIOPAN_DISPLAY returns at the next vblank, showing the buffer from then on
static int fake_pan(int fd, unsigned long req, ...)
{
    (void)fd;
    va_list ap;
    va_start(ap, req);
    struct fb_var_screeninfo *v = va_arg(ap, struct fb_var_screeninfo *);
    va_end(ap);
    if (req != FBIOPAN_DISPLAY) return -1;
    double t = now_us() - t0;
    double next = ((long long)(t / PERIOD_US) + 1) * (double)PERIOD_US;
    sleep_us(next - t);
    pthread_mutex_lock(&m);
    int b = v->yoffset / YRES;
    if (v->xres == 720 && v->yres == YRES) pans_full++;
    if (crop_w && (int)v->xres == crop_w && (int)v->yres == crop_h) pans_cropped++;
    else if (v->xres != 720 || v->yres != YRES) pans_wrong++;
    on_screen = b;
    if (drawing == b) tears++;                         // the GPU is drawing into what is shown now
    for (int i = 0; i < npending; i++)
        if (pending_flips[i] == b) { memmove(pending_flips + i, pending_flips + i + 1, (npending - i - 1) * sizeof(int)); npending--; break; }
    int f = frame_of_buffer[b];
    if (f != last_flipped_frame + 1) out_of_order++;
    last_flipped_frame = f;
    flips++;
    pthread_cond_broadcast(&c);
    pthread_mutex_unlock(&m);
    return 0;
}

// ---- the driver (Mali's winsys and GPU) ----
static int busy[16];                     // taken by a frame not shown yet, or shown (as the driver sees it)
static int shown_idx;                    // the buffer the driver takes as shown
static int next_idx;                     // the buffer the next frame draws into
static int queue_frames[64], queue_buf[64], qh, qn;   // frames handed to the GPU
static int gpu_us, stop;
static int slow_from = -1, slow_gpu_us;  // from this frame on, the GPU needs slow_gpu_us
static double gpu_work_us, gpu_busy, last_queue;

static void *gpu_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&m);
    for (;;) {
        while (!qn && !stop) pthread_cond_wait(&c, &m);
        if (!qn && stop) break;
        int f = queue_frames[qh], b = queue_buf[qh];
        qh = (qh + 1) % 64; qn--;
        int us = slow_from >= 0 && f >= slow_from ? slow_gpu_us : gpu_us;
        // the GPU draws the frame into its buffer
        for (int i = 0; i < npending; i++) if (pending_flips[i] == b) overwrites++;
        if (b == on_screen) tears++;
        drawing = b;
        frame_of_buffer[b] = f;
        pthread_mutex_unlock(&m);
        double s = now_us();
        sleep_us(us);
        gpu_work_us += now_us() - s;
        pthread_mutex_lock(&m);
        drawing = -1;
        pthread_mutex_unlock(&m);
        // the driver's thread: pan to it (the GPU waits meanwhile: Mali's backend feeds it)
        struct fb_var_screeninfo v;
        memset(&v, 0, sizeof v);
        v.xres = 720;
        v.yres = YRES;
        v.yoffset = b * YRES;
        pthread_mutex_lock(&m);
        for (int i = 0; i < npending; i++)           // (mailbox flips: a dropped frame's flip)
            if (pending_flips[i] == b) { memmove(pending_flips + i, pending_flips + i + 1, (npending - i - 1) * sizeof(int)); npending--; i--; }
        if (npending < 64) pending_flips[npending++] = b;
        pthread_mutex_unlock(&m);
        ioctl(3, FBIOPAN_DISPLAY, &v);              // fbdev.c: the libc's, or the host's flip thread
        pthread_mutex_lock(&m);
        busy[shown_idx] = 0;                         // the buffer shown before is free again
        shown_idx = b;
        pthread_cond_broadcast(&c);
    }
    pthread_mutex_unlock(&m);
    return NULL;
}

static int start_w = 720, start_h = YRES, new_w = 720, new_h = YRES;   // the windows: at the start, at new_window_at

// one run; returns the frame rate
static double run(const char *name, int mode, int buffers, int queue, int frames, int cpu_us_, int gpu_us_, int new_window_at)
{
    B = buffers;
    fbdev_test_init(mode, buffers, queue, PERIOD_US / 1e6, fake_pan);
    pthread_mutex_lock(&m);
    memset(busy, 0, sizeof busy);
    memset(frame_of_buffer, 0, sizeof frame_of_buffer);
    on_screen = shown_idx = 0;
    busy[0] = 1;
    next_idx = 1;
    npending = qh = qn = 0;
    tears = overwrites = flips = out_of_order = 0;
    pans_full = pans_cropped = pans_wrong = 0;
    last_flipped_frame = -1;
    stop = 0;
    gpu_us = gpu_us_;
    gpu_work_us = 0;
    pthread_mutex_unlock(&m);
    fbdev_new_window(start_w, start_h);
    t0 = now_us();
    pthread_t g;
    pthread_create(&g, NULL, gpu_main, NULL);
    double start = now_us();
    for (int f = 0; f < frames; f++) {
        if (f == new_window_at) {
            // the window is made again: the driver's new window starts at the buffer on the screen
            fbdev_new_window(new_w, new_h);
            pthread_mutex_lock(&m);
            while (qn) pthread_cond_wait(&c, &m);     // (the old window's frames are done)
            next_idx = (on_screen + 1) % B;
            shown_idx = on_screen;
            memset(busy, 0, sizeof busy);
            busy[on_screen] = 1;
            pthread_mutex_unlock(&m);
        }
        sleep_us(cpu_us_);
        pthread_mutex_lock(&m);
        int b = next_idx;
        while (busy[b]) pthread_cond_wait(&c, &m);  // the driver's dequeue
        busy[b] = 1;
        next_idx = (b + 1) % B;
        queue_frames[(qh + qn) % 64] = f;
        queue_buf[(qh + qn) % 64] = b;
        qn++;
        pthread_cond_broadcast(&c);
        pthread_mutex_unlock(&m);
        fbdev_swap();
        fbdev_swap_done(1);
    }
    pthread_mutex_lock(&m);
    while (qn || flips < frames - (mode == 2 ? frames : 0) + 0) {
        if (mode == 2) break;
        pthread_cond_wait(&c, &m);
    }
    stop = 1;
    pthread_cond_broadcast(&c);
    pthread_mutex_unlock(&m);
    pthread_join(g, NULL);
    if (mode != 0) usleep(3 * PERIOD_US);           // the flip thread's last pans
    double secs = (now_us() - start) / 1e6;
    double fps = frames / secs;
    gpu_busy = gpu_work_us / (secs * 1e6);
    double avg, mx, waited, qsum;
    int nf = fbdev_take_stats(&avg, &mx, &waited, &qsum);
    last_queue = frames ? qsum / frames : 0;
    (void)nf;
    printf("%-38s %d buffers, queue %d (%.2f): %5.1f fps (refresh %d), GPU busy %3.0f%%, tears %d, overwrites %d, flips %d, out of order %d\n",
           name, buffers, queue, last_queue, fps, 1000000 / PERIOD_US, 100 * gpu_busy, tears, overwrites, flips, out_of_order);
    return fps;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    double hz = 1e6 / PERIOD_US;
    // the driver's own flips, two buffers: no tearing, a frame slower than a refresh at half the rate
    double d2 = run("driver, slow frames", 0, 2, 0, 120, US(300), US(5000), -1);
    CHECK(tears == 0 && overwrites == 0 && out_of_order == 0);
    CHECK(d2 < hz * 0.55 && gpu_busy < 0.85);       // the GPU waits while the driver's pan waits
    // host flips: no tearing, every frame in order, slow frames above half the rate
    for (int bufs = 3; bufs <= 4; bufs++)
        for (int q = 1; q <= bufs - 2; q++) {
            run("host, slow frames", 1, bufs, q, 120, US(300), US(5000), -1);
            CHECK(tears == 0 && overwrites == 0 && out_of_order == 0 && flips == 120);
            if (bufs - 2 == q) CHECK(gpu_busy > 0.85);    // the GPU always has the next frame
            run("host, fast frames", 1, bufs, q, 200, US(300), US(600), -1);
            CHECK(tears == 0 && overwrites == 0 && out_of_order == 0 && flips == 200);
            run("host, fast CPU, GPU at the refresh", 1, bufs, q, 150, US(200), US(3900), -1);
            CHECK(tears == 0 && overwrites == 0 && out_of_order == 0 && flips == 150);
            run("host, a new window at frame 60", 1, bufs, q, 150, US(300), US(600), 60);
            CHECK(tears == 0 && overwrites == 0 && out_of_order == 0);
        }
    // the automatic queue depth (UA_FB_QUEUE unset): fast frames go down to 1, slow ones keep 2
    run("host auto, fast frames", 1, 4, 0, 200, US(300), US(600), -1);
    CHECK(tears == 0 && overwrites == 0 && out_of_order == 0 && flips == 200);
    double qf = last_queue;
    CHECK(qf < 1.7);                                 // (30 frames at 2 first; a jittery VM may see a refresh as missed)
    run("host auto, slow frames", 1, 4, 0, 120, US(300), US(5000), -1);
    CHECK(tears == 0 && overwrites == 0 && out_of_order == 0 && flips == 120);
    CHECK(last_queue > 1.9 && gpu_busy > 0.85);
    run("host auto, a new window at frame 60", 1, 4, 0, 150, US(300), US(600), 60);
    CHECK(tears == 0 && overwrites == 0 && out_of_order == 0);
    // fast, then slow from frame 100: a missed refresh brings the second queued frame back
    slow_from = 100; slow_gpu_us = US(5000);
    run("host auto, fast then slow", 1, 4, 0, 220, US(300), US(600), -1);
    slow_from = -1;
    CHECK(tears == 0 && overwrites == 0 && out_of_order == 0 && flips == 220);
    CHECK(last_queue > 1.3);
    // the test sees tearing: mailbox flips (a measurement) tear with fast frames
    run("mailbox, fast frames (tears)", 2, 2, 0, 200, US(100), US(300), -1);
    CHECK(tears + overwrites > 0);
    // a 540x360 window: every pan carries its size, the offsets as before (the driver's flips, and
    // host flips from a new window at frame 60 on)
    crop_w = 540; crop_h = 360;
    start_w = 540; start_h = 360;
    run("driver, a 540x360 window", 0, 2, 0, 120, US(300), US(600), -1);
    CHECK(tears == 0 && overwrites == 0 && out_of_order == 0 && flips == 120);
    CHECK(pans_wrong == 0 && pans_full == 0 && pans_cropped == 120);
    start_w = 720; start_h = YRES; new_w = 540; new_h = 360;
    run("host, a 540x360 window at frame 60", 1, 4, 0, 150, US(300), US(600), 60);
    CHECK(tears == 0 && overwrites == 0 && out_of_order == 0 && flips == 150);
    CHECK(pans_wrong == 0 && pans_full == 60 && pans_cropped == 90);
    printf("  pans: %d with the screen's size, %d with the window's\n", pans_full, pans_cropped);
    // and back to a whole-screen window
    start_w = 540; start_h = 360; new_w = 720; new_h = YRES;
    run("host, 540x360, a 720x480 window at frame 60", 1, 4, 0, 150, US(300), US(600), 60);
    CHECK(tears == 0 && overwrites == 0 && out_of_order == 0 && flips == 150);
    CHECK(pans_wrong == 0 && pans_cropped == 60 && pans_full == 90);
    printf("  pans: %d with the screen's size, %d with the window's\n", pans_full, pans_cropped);
    start_w = 720; start_h = YRES;
    // the render sizes (UA_RENDER_SIZE / UA_RENDER_SCALE / UA_RENDER_RAM)
    static const struct { int pw, ph; const char *size, *scale, *ram; long kb; int ok, w, h; } rs[] = {
        { 720, 480, NULL, "0.75", NULL, 0, 1, 540, 360 },
        { 720, 480, NULL, "0.8333", NULL, 0, 1, 600, 400 },
        { 720, 480, NULL, "0.9167", NULL, 0, 1, 660, 440 },
        { 720, 480, NULL, "0.9", NULL, 0, 1, 648, 432 },
        { 720, 480, "600x400", NULL, NULL, 0, 1, 600, 400 },
        { 720, 480, "576x384", "0.5", NULL, 0, 1, 576, 384 },        // the size before the scale
        { 720, 480, "640x360", NULL, NULL, 0, 1, 540, 360 },         // the panel's aspect within it
        { 720, 480, "601x401", NULL, NULL, 0, 1, 600, 400 },         // even
        { 720, 480, NULL, "0.1", NULL, 0, 1, 180, 120 },             // a quarter at least
        { 720, 480, NULL, "1", NULL, 0, 0, 0, 0 },
        { 720, 480, "720x480", NULL, NULL, 0, 0, 0, 0 },
        { 720, 480, "800x600", NULL, NULL, 0, 0, 0, 0 },
        { 720, 480, "big", NULL, NULL, 0, 0, 0, 0 },
        { 720, 480, NULL, "0", NULL, 0, 0, 0, 0 },
        { 720, 480, NULL, NULL, "1024", 996352, 0, 0, 0 },           // nothing asked
        { 720, 480, NULL, "0.75", "1024", 996352, 1, 540, 360 },     // 973 MB: a 1 GB device
        { 720, 480, NULL, "0.75", "1024", 2 * 1048576L, 0, 0, 0 },   // 2 GB: the panel's size
        { 640, 480, NULL, "0.75", NULL, 0, 1, 480, 360 },
        { 1280, 720, NULL, "0.75", NULL, 0, 1, 960, 540 },
        { 1920, 1152, NULL, "0.5", NULL, 0, 1, 960, 576 },
        { 854, 480, NULL, "0.75", NULL, 0, 1, 640, 360 },           // no whole aspect step: the nearest even sizes
    };
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; i++) {
        int w = 0, h = 0;
        int ok = fbdev_pick_render_size(rs[i].pw, rs[i].ph, rs[i].size, rs[i].scale, rs[i].ram, rs[i].kb, &w, &h);
        CHECK(ok == rs[i].ok && (!ok || (w == rs[i].w && h == rs[i].h)));
        if (ok != rs[i].ok || (ok && (w != rs[i].w || h != rs[i].h)))
            fprintf(stderr, "  render size %dx%d size %s scale %s ram %s: %d %dx%d\n", rs[i].pw, rs[i].ph, rs[i].size ? rs[i].size : "-",
                    rs[i].scale ? rs[i].scale : "-", rs[i].ram ? rs[i].ram : "-", ok, w, h);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
