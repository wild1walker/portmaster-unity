// Does the GPU wait for work inside the frames? A measurement, off unless UA_GPU_IDLE=<s> is set:
// every <s> seconds a line with the time the GPU had nothing to do, in ms a frame.
//
// A frame's commands reach the GPU in spans: the driver submits the commands of a framebuffer when
// the game binds another one (a tiler renders a pass at a time), and the rest at the swap. With
// UA_GPU_IDLE each span ends with an EGL fence (EGL_KHR_fence_sync) and a glFlush, on the render
// thread, at each change of the draw framebuffer and before each eglSwapBuffers; a thread of the
// host ("ua_gpuidle") waits for the fences in order (eglClientWaitSyncKHR needs no context) and
// notes when each one signals. The GPU runs the spans in order, so when span k was flushed after
// span k - 1 was done, the GPU had nothing submitted in between: that gap is counted as idle.
// Nothing is serialized (unlike UA_GL_TRACE_GPU=3): the frames run as they do without it.
//   [gpuidle] 5.0 s, 166 frames (30.1 ms): GPU idle 0.62 ms a frame (2.1 %): before the frame's
//     first span 0.05, inside frames 0.57; 24.0 spans a frame; most before span 7 (fb 31) 0.30, ...
// What it can miss, either way: the driver may hand a pass's vertex work to the GPU before its
// flush (the GPU then had work in a counted gap: idle over-counted), and the flush only queues the
// work for the driver's own submission thread (mali-cmar-backend on the Mali), which may start it
// later (idle under-counted); the waiting thread wakes some tens of microseconds after a fence
// signals (idle under-counted by that, per gap). The fences, flushes and the waiting thread's
// wake-ups (about 25 a frame) are the measurement's own cost; compare with runs without it.
#define _GNU_SOURCE
#include "gpuidle.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

typedef void *EGLDisplay;
typedef void *EGLSyncKHR;
typedef int32_t EGLint;
typedef unsigned EGLenum;
typedef uint64_t EGLTimeKHR;
#define EGL_SYNC_FENCE_KHR 0x30F9
#define EGL_FOREVER_KHR 0xFFFFFFFFFFFFFFFFull
#define GL_FRAMEBUFFER 0x8D40
#define GL_DRAW_FRAMEBUFFER 0x8CA9

static EGLSyncKHR (*e_CreateSync)(EGLDisplay, EGLenum, const EGLint *);
static EGLint (*e_ClientWait)(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR);
static unsigned (*e_DestroySync)(EGLDisplay, EGLSyncKHR);
static EGLDisplay (*e_GetCurrentDisplay)(void);
static void (*r_BindFramebuffer)(unsigned, unsigned);
static void (*r_Flush)(void);

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

void gpuidle_account(gpuidle_acc *a, double sub, double done, int first, int span, unsigned fb)
{
    if (a->busy_from > 0 && sub > a->busy_from) {
        double gap = sub - a->busy_from;
        a->idle += gap;
        if (first) a->idle_first += gap;
        if (span >= 0 && span < 32) a->by_span[span] += gap;
        if (gap > a->largest) { a->largest = gap; a->largest_span = span; }
    }
    if (span >= 0 && span < 32) a->fb_of[span] = fb;
    if (done > a->busy_from) a->busy_from = done;
    a->spans++;
    if (first) a->frames++;
}

#define QN 1024
static struct { EGLSyncKHR s; double sub; unsigned fb; int first, span, reset; } g_q[QN];
static int g_qh, g_qn, g_reset;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static double g_every;
static int g_on;                             // 1 measuring, -1 given up
static EGLDisplay g_dpy;
static pthread_t g_thread;                   // the render thread (the one that swaps)
static int g_have_thread;
static unsigned g_cur_fb;
static int g_span, g_first = 1;
static long g_dropped;

static void report(const gpuidle_acc *a, double secs)
{
    if (!a->frames) return;
    double f = (double)a->frames, frame = secs / f;
    int top[3] = { -1, -1, -1 };
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 3; j++)
            if (a->by_span[i] > 0 && (top[j] < 0 || a->by_span[i] > a->by_span[top[j]])) {
                for (int m = 2; m > j; m--) top[m] = top[m - 1];
                top[j] = i;
                break;
            }
    char most[160] = "";
    size_t o = 0;
    for (int j = 0; j < 3 && top[j] >= 0; j++)
        o += (size_t)snprintf(most + o, sizeof most - o, "%s span %d (fb %u) %.2f", j ? "," : "; most before", top[j],
                              a->fb_of[top[j]], 1000 * a->by_span[top[j]] / f);
    fprintf(stderr, "[gpuidle] %.1f s, %ld frames (%.1f ms): GPU idle %.2f ms a frame (%.1f %%): before the frame's first span %.2f, "
            "inside frames %.2f; %.1f spans a frame%s; largest gap %.2f ms (span %d)%s\n",
            secs, a->frames, 1000 * frame, 1000 * a->idle / f, 100 * a->idle / secs, 1000 * a->idle_first / f,
            1000 * (a->idle - a->idle_first) / f, a->spans / f, most, 1000 * a->largest, a->largest_span,
            g_dropped ? " (fences dropped: the waiting thread fell behind)" : "");
}

static void *waiter(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "ua_gpuidle", 0, 0, 0);
    gpuidle_acc acc;
    memset(&acc, 0, sizeof acc);
    double t0 = 0;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_qn) pthread_cond_wait(&g_cond, &g_lock);
        EGLSyncKHR s = g_q[g_qh].s;
        double sub = g_q[g_qh].sub;
        unsigned fb = g_q[g_qh].fb;
        int first = g_q[g_qh].first, span = g_q[g_qh].span, reset = g_q[g_qh].reset;
        pthread_mutex_unlock(&g_lock);
        e_ClientWait(g_dpy, s, 0, EGL_FOREVER_KHR);
        double done = now();
        e_DestroySync(g_dpy, s);
        pthread_mutex_lock(&g_lock);
        g_qh = (g_qh + 1) % QN;
        g_qn--;
        pthread_mutex_unlock(&g_lock);
        if (reset) acc.busy_from = 0;                  // a fence was dropped before this one: no gap
        if (!t0) t0 = done;
        if (first && done - t0 >= g_every && acc.frames > 0) {
            report(&acc, done - t0);
            double from = acc.busy_from;
            memset(&acc, 0, sizeof acc);
            acc.busy_from = from;
            t0 = done;
        }
        gpuidle_account(&acc, sub, done, first, span, fb);
    }
    return NULL;
}

// the end of a span: its fence, then the flush that submits it
static void span_end(void)
{
    if (g_on <= 0) return;
    pthread_mutex_lock(&g_lock);
    int full = g_qn == QN;
    pthread_mutex_unlock(&g_lock);
    if (full) { g_dropped++; g_reset = 1; return; }
    EGLSyncKHR s = e_CreateSync(g_dpy, EGL_SYNC_FENCE_KHR, NULL);
    if (!s) { g_on = -1; fprintf(stderr, "[gpuidle] eglCreateSyncKHR failed: no measurement\n"); return; }
    r_Flush();
    double t = now();
    pthread_mutex_lock(&g_lock);
    int i = (g_qh + g_qn) % QN;
    g_q[i].s = s; g_q[i].sub = t; g_q[i].fb = g_cur_fb; g_q[i].first = g_first; g_q[i].span = g_span; g_q[i].reset = g_reset;
    g_qn++;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    g_first = 0; g_reset = 0;
    g_span++;
}

static void w_BindFramebuffer(unsigned t, unsigned fb)
{
    if ((t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) && fb != g_cur_fb && g_have_thread && pthread_equal(pthread_self(), g_thread)) {
        span_end();
        g_cur_fb = fb;
    }
    r_BindFramebuffer(t, fb);
}

void gpuidle_frame(void)
{
    if (g_every <= 0 || g_on < 0) return;
    if (!g_on) {                                       // the first swap: the render thread, its context current
        g_thread = pthread_self(); g_have_thread = 1;
        g_dpy = e_GetCurrentDisplay ? e_GetCurrentDisplay() : NULL;
        pthread_t th;
        if (!g_dpy || pthread_create(&th, NULL, waiter, NULL) != 0) {
            g_on = -1;
            fprintf(stderr, "[gpuidle] no EGL display or no thread: no measurement\n");
            return;
        }
        pthread_detach(th);
        g_on = 1;
        fprintf(stderr, "[gpuidle] a fence at each framebuffer change and swap; GPU idle every %g s\n", g_every);
    }
    span_end();                                        // the frame's last span, before its swap
    g_first = 1;
    g_span = 0;
}

static so_shim g_shims[2];

const so_shim *gpuidle_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    const char *e = getenv("UA_GPU_IDLE");
    g_every = e && *e ? atof(e) : 0;
    if (g_every <= 0) return g_shims;
    void *gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!gl) gl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!egl) egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    void *(*gpa)(const char *) = egl ? (void *(*)(const char *))dlsym(egl, "eglGetProcAddress") : NULL;
    *(void **)&r_BindFramebuffer = next ? next("glBindFramebuffer") : NULL;
    if (!r_BindFramebuffer && gl) *(void **)&r_BindFramebuffer = dlsym(gl, "glBindFramebuffer");
    if (gl) *(void **)&r_Flush = dlsym(gl, "glFlush");
    if (egl) *(void **)&e_GetCurrentDisplay = dlsym(egl, "eglGetCurrentDisplay");
    if (gpa) {
        *(void **)&e_CreateSync = gpa("eglCreateSyncKHR");
        *(void **)&e_ClientWait = gpa("eglClientWaitSyncKHR");
        *(void **)&e_DestroySync = gpa("eglDestroySyncKHR");
    }
    if (!r_BindFramebuffer || !r_Flush || !e_GetCurrentDisplay || !e_CreateSync || !e_ClientWait || !e_DestroySync) {
        fprintf(stderr, "[gpuidle] no EGL_KHR_fence_sync or GL functions: no measurement\n");
        g_every = 0;
        return g_shims;
    }
    g_shims[0] = (so_shim){ "glBindFramebuffer", (void *)w_BindFramebuffer };
    return g_shims;
}
