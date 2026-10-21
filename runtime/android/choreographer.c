// libandroid.so AChoreographer_* (API 24+): frame callbacks at the display's vsync times, run on the
// thread whose looper made the choreographer, as Android's DisplayEventReceiver delivers them.
//
// The vsync clock is shared with the Java Choreographer of runtime/jni (up_input.c): vsyncs are the
// multiples of one refresh period on CLOCK_MONOTONIC (android_vsync_set_refresh_rate), and a callback
// posted at t (or due at t + delay) runs at the first vsync after it, with that vsync as its frame time.
//
// AChoreographer_getInstance() is the calling thread's, as on Android (a thread-local instance):
// - on a thread with an ALooper (ALooper_prepare; Swappy's "SwappyChoreographer" thread), a timerfd on
//   that looper fires at the next vsync with a callback due; the callbacks run inside the thread's
//   ALooper_pollOnce/pollAll;
// - on a looper thread of the host's own (runtime/jni's Java loopers: UnityMain, HandlerThreads,
//   android_set_host_looper), the callbacks run as jobs posted to that looper;
// - elsewhere it is NULL ("no looper prepared for thread"), as on Android.
// Callbacks may be posted from any thread (Swappy posts from its swap thread under its own lock); no
// lock of ours is held while a callback runs, so a callback may post the next one.
#define _GNU_SOURCE
#include "android_shims.h"
#include "shim_internal.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

// ---- the vsync clock -----------------------------------------------------------------------
static int64_t g_period_ns = 16666666;          // 60 Hz until the host says otherwise

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t android_vsync_period_ns(void) { return __atomic_load_n(&g_period_ns, __ATOMIC_RELAXED); }
int64_t android_vsync_after(int64_t t_ns) { int64_t p = android_vsync_period_ns(); return (t_ns / p + 1) * p; }
static int64_t vsync_at_or_before(int64_t t_ns) { int64_t p = android_vsync_period_ns(); return t_ns / p * p; }

// ---- choreographers ------------------------------------------------------------------------
enum { CB_FRAME, CB_FRAME64, CB_VSYNC };
#define MAX_CB 64
#define MAX_RR 8

struct AChoreographer {
    ALooper *looper;            // an ALooper thread: tfd is on it
    void *host;                 // or a host looper (android_set_host_looper)
    int tfd;
    struct { int kind; void *fn; void *data; int64_t at; } cb[MAX_CB];   // fn NULL: free slot
    struct { AChoreographer_refreshRateCallback fn; void *data; int pending; } rr[MAX_RR];
    int64_t armed;              // the time the timer or the posted job fires at (0: none)
    int64_t vsync_id;
    AChoreographer *next;       // every choreographer, for refresh rate changes
};

// what AChoreographerFrameCallbackData_* read during a vsync callback: one timeline, its deadline the
// next vsync and its expected presentation the one after (a double-buffered pipeline)
struct AChoreographerFrameCallbackData { int64_t frame_ns, vsync_id, deadline_ns, present_ns; };

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // every choreographer's lists
static AChoreographer *g_all;
static __thread AChoreographer *t_choreo;

static void *(*g_host_current)(void);
static void (*g_host_post)(void *looper, void (*fn)(void *), void *arg, int64_t delay_ms);

void android_set_host_looper(void *(*current)(void), void (*post)(void *looper, void (*fn)(void *), void *arg, int64_t delay_ms))
{
    g_host_current = current;
    g_host_post = post;
}

static void host_job(void *arg);
#define ASAP 1                                      // a time long past: the timer fires at once

static void arm(AChoreographer *c, int64_t at)     // g_mu held
{
    if (c->armed && c->armed <= at) return;
    c->armed = at;
    if (c->looper) {
        struct itimerspec it = { { 0, 0 }, { at / 1000000000LL, at % 1000000000LL } };
        timerfd_settime(c->tfd, TFD_TIMER_ABSTIME, &it, NULL);
    } else if (c->host && g_host_post) {
        int64_t d = at - now_ns();
        g_host_post(c->host, host_job, c, d > 0 ? (d + 999999) / 1000000 + 1 : 0);
    }
}

static void rearm(AChoreographer *c)                // g_mu held: for the earliest pending callback
{
    int64_t first = 0;
    for (int i = 0; i < MAX_CB; i++)
        if (c->cb[i].fn && (!first || c->cb[i].at < first)) first = c->cb[i].at;
    for (int i = 0; i < MAX_RR; i++)
        if (c->rr[i].fn && c->rr[i].pending) first = ASAP;
    if (first) arm(c, first);
}

// on the choreographer's thread: refresh rate callbacks first (registration, a period change), then
// the frame callbacks whose vsync has come, with that vsync as the frame time
static void dispatch(AChoreographer *c)
{
    struct { int kind; void *fn; void *data; } due[MAX_CB];
    struct { AChoreographer_refreshRateCallback fn; void *data; } rr[MAX_RR];
    int n = 0, nrr = 0;
    int64_t now = now_ns(), period = android_vsync_period_ns();
    pthread_mutex_lock(&g_mu);
    c->armed = 0;
    for (int i = 0; i < MAX_RR; i++)
        if (c->rr[i].fn && c->rr[i].pending) { c->rr[i].pending = 0; rr[nrr].fn = c->rr[i].fn; rr[nrr++].data = c->rr[i].data; }
    for (int i = 0; i < MAX_CB; i++)
        if (c->cb[i].fn && c->cb[i].at <= now) {
            due[n].kind = c->cb[i].kind; due[n].fn = c->cb[i].fn; due[n++].data = c->cb[i].data;
            c->cb[i].fn = NULL;
        }
    int64_t id = n ? ++c->vsync_id : c->vsync_id;
    rearm(c);
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < nrr; i++) rr[i].fn(period, rr[i].data);
    if (!n) return;
    int64_t frame = vsync_at_or_before(now);
    struct AChoreographerFrameCallbackData d = { frame, id, frame + period, frame + 2 * period };
    for (int i = 0; i < n; i++) {
        if (due[i].kind == CB_FRAME) ((AChoreographer_frameCallback)due[i].fn)((long)frame, due[i].data);
        else if (due[i].kind == CB_FRAME64) ((AChoreographer_frameCallback64)due[i].fn)(frame, due[i].data);
        else ((AChoreographer_vsyncCallback)due[i].fn)(&d, due[i].data);
    }
}

static int timer_cb(int fd, int events, void *data)
{
    (void)events;
    uint64_t v;
    (void)!read(fd, &v, sizeof v);      // nonblocking: EAGAIN if re-armed meanwhile
    dispatch(data);
    return 1;
}

static void host_job(void *arg)
{
    AChoreographer *c = arg;
    pthread_mutex_lock(&g_mu);
    int early = c->armed && c->armed > now_ns();   // an older job, or one rounded to milliseconds
    pthread_mutex_unlock(&g_mu);
    if (!early) dispatch(c);
}

AChoreographer *AChoreographer_getInstance(void)
{
    void *host = g_host_current ? g_host_current() : NULL;
    ALooper *l = host ? NULL : ALooper_forThread();
    AChoreographer *c = t_choreo;
    if (c && (host ? c->host == host : c->looper == l)) return c;
    if (!host && !l) {
        static int told;
        if (!told++) SHIM_WARN("AChoreographer_getInstance: no looper prepared for this thread (NULL)");
        return NULL;
    }
    c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->tfd = -1;
    if (host) c->host = host;
    else {
        c->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (c->tfd < 0 || ALooper_addFd(l, c->tfd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, timer_cb, c) != 1) {
            SHIM_WARN("AChoreographer_getInstance: no timer on the looper (%s)", strerror(errno));
            if (c->tfd >= 0) close(c->tfd);
            free(c);
            return NULL;
        }
        c->looper = l;
    }
    pthread_mutex_lock(&g_mu);
    c->next = g_all;
    g_all = c;
    pthread_mutex_unlock(&g_mu);
    t_choreo = c;
    SHIM_TRACE("AChoreographer_getInstance -> %p (%s)", (void *)c, host ? "host looper" : "ALooper");
    return c;
}

static void post(AChoreographer *c, int kind, void *fn, void *data, int64_t delay_ms)
{
    if (!c || !fn) return;
    int64_t now = now_ns();
    int64_t at = android_vsync_after(now + (delay_ms > 0 ? delay_ms * 1000000LL : 0));
    pthread_mutex_lock(&g_mu);
    int i;
    for (i = 0; i < MAX_CB && c->cb[i].fn; i++) {}
    if (i < MAX_CB) {
        c->cb[i].kind = kind; c->cb[i].fn = fn; c->cb[i].data = data; c->cb[i].at = at;
        arm(c, at);
    }
    pthread_mutex_unlock(&g_mu);
    if (i == MAX_CB) SHIM_WARN("AChoreographer: more than %d frame callbacks pending, one dropped", MAX_CB);
}

void AChoreographer_postFrameCallback(AChoreographer *c, AChoreographer_frameCallback cb, void *data)
{ post(c, CB_FRAME, (void *)cb, data, 0); }
void AChoreographer_postFrameCallbackDelayed(AChoreographer *c, AChoreographer_frameCallback cb, void *data, long delayMillis)
{ post(c, CB_FRAME, (void *)cb, data, delayMillis); }
void AChoreographer_postFrameCallback64(AChoreographer *c, AChoreographer_frameCallback64 cb, void *data)
{ post(c, CB_FRAME64, (void *)cb, data, 0); }
void AChoreographer_postFrameCallbackDelayed64(AChoreographer *c, AChoreographer_frameCallback64 cb, void *data, uint32_t delayMillis)
{ post(c, CB_FRAME64, (void *)cb, data, delayMillis); }
int AChoreographer_postVsyncCallback(AChoreographer *c, AChoreographer_vsyncCallback cb, void *data)
{
    if (!c || !cb) return -EINVAL;
    post(c, CB_VSYNC, (void *)cb, data, 0);
    return 0;
}

// The callback runs once on the choreographer's thread soon after registering (Android sends the
// current period then), and again when the period changes. Registering the same pair twice is a no-op.
void AChoreographer_registerRefreshRateCallback(AChoreographer *c, AChoreographer_refreshRateCallback cb, void *data)
{
    if (!c || !cb) return;
    pthread_mutex_lock(&g_mu);
    int i, freei = -1;
    for (i = 0; i < MAX_RR; i++) {
        if (c->rr[i].fn == cb && c->rr[i].data == data) break;
        if (!c->rr[i].fn && freei < 0) freei = i;
    }
    if (i == MAX_RR && freei >= 0) {
        c->rr[freei].fn = cb; c->rr[freei].data = data; c->rr[freei].pending = 1;
        arm(c, ASAP);
    }
    pthread_mutex_unlock(&g_mu);
}

void AChoreographer_unregisterRefreshRateCallback(AChoreographer *c, AChoreographer_refreshRateCallback cb, void *data)
{
    if (!c) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_RR; i++)
        if (c->rr[i].fn == cb && c->rr[i].data == data) c->rr[i].fn = NULL;
    pthread_mutex_unlock(&g_mu);
}

void android_vsync_set_refresh_rate(float hz)
{
    int64_t p = (int64_t)(1e9 / (hz > 0 ? hz : 60.0f));
    if (p == android_vsync_period_ns()) return;
    __atomic_store_n(&g_period_ns, p, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_mu);
    for (AChoreographer *c = g_all; c; c = c->next) {
        int any = 0;
        for (int i = 0; i < MAX_RR; i++) if (c->rr[i].fn) c->rr[i].pending = any = 1;
        if (any) arm(c, ASAP);
    }
    pthread_mutex_unlock(&g_mu);
}

// ---- AChoreographerFrameCallbackData (API 33): valid during a vsync callback -----------------
int64_t AChoreographerFrameCallbackData_getFrameTimeNanos(const AChoreographerFrameCallbackData *d)
{ return d ? d->frame_ns : 0; }
size_t AChoreographerFrameCallbackData_getFrameTimelinesLength(const AChoreographerFrameCallbackData *d)
{ return d ? 1 : 0; }
size_t AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(const AChoreographerFrameCallbackData *d)
{ (void)d; return 0; }
int64_t AChoreographerFrameCallbackData_getFrameTimelineVsyncId(const AChoreographerFrameCallbackData *d, size_t i)
{ return d && i == 0 ? d->vsync_id : -1; }
int64_t AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(const AChoreographerFrameCallbackData *d, size_t i)
{ return d && i == 0 ? d->present_ns : -1; }
int64_t AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(const AChoreographerFrameCallbackData *d, size_t i)
{ return d && i == 0 ? d->deadline_ns : -1; }
