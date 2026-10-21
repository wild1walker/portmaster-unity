// Unit tests for the Android shims, run without a GPU (armhf under QEMU, aarch64 natively): liblog,
// AAssetManager, ALooper, sensors, ANativeWindow + egl_glue (against fake_egl.c), the libmediandk
// stubs, OpenSL ES on SDL's disk audio driver.
#define _GNU_SOURCE
#include "android_shims.h"
#include "egl_glue.h"
#include "sles_min.h"
#include "shim_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char tmpdir[256];

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    if (len) *len = n;
    return b;
}

static void write_file(const char *rel, const char *data, size_t n)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", tmpdir, rel);
    FILE *f = fopen(p, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}

// ---- liblog -----------------------------------------------------------------------------------
static void test_log(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/log.txt", tmpdir);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    int saved = dup(2);
    dup2(fd, 2);
    android_log_set_min_priority(3 /* DEBUG */);
    __android_log_print(4, "Unity", "hello %d %s", 42, "world");
    __android_log_print(2, "Unity", "verbose is filtered");
    __android_log_write(6, "Mono", "error line\n");
    __android_log_buf_print(0, 5, "FMOD", "warn %.2f", 1.5);
    char big[5000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    __android_log_print(3, "Big", "%s|end", big);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fd);
    char *s = slurp(path, NULL);
    CHECK(s && strstr(s, "I/Unity(") && strstr(s, "): hello 42 world\n"));
    CHECK(s && !strstr(s, "verbose is filtered"));
    CHECK(s && strstr(s, "E/Mono(") && strstr(s, "): error line\n") && !strstr(s, "error line\n\n"));
    CHECK(s && strstr(s, "W/FMOD(") && strstr(s, "warn 1.50\n"));
    CHECK(s && strstr(s, "xxxx|end\n"));
    free(s);
}

// ---- AAssetManager ----------------------------------------------------------------------------
static void test_assets(void)
{
    char p[512];
    snprintf(p, sizeof p, "%s/assets", tmpdir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/assets/bin", tmpdir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/assets/bin/Data", tmpdir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/assets/bin/Data/sub", tmpdir); mkdir(p, 0755);
    write_file("assets/bin/Data/settings.xml", "0123456789abcdef", 16);
    write_file("assets/bin/Data/empty", "", 0);
    write_file("assets/top.txt", "top", 3);
    write_file("secret.txt", "secret", 6);
    snprintf(p, sizeof p, "%s/assets", tmpdir);
    android_set_asset_dir(p);

    AAssetManager *m = AAssetManager_fromJava(NULL, (void *)1);
    CHECK(m != NULL);
    AAsset *a = AAssetManager_open(m, "bin/Data/settings.xml", AASSET_MODE_STREAMING);
    CHECK(a != NULL);
    if (a) {
        char buf[32] = { 0 };
        CHECK(AAsset_getLength(a) == 16 && AAsset_getLength64(a) == 16);
        CHECK(AAsset_read(a, buf, 4) == 4 && !memcmp(buf, "0123", 4));
        CHECK(AAsset_getRemainingLength(a) == 12);
        CHECK(AAsset_seek(a, 10, SEEK_SET) == 10);
        CHECK(AAsset_read(a, buf, 32) == 6 && !memcmp(buf, "abcdef", 6));
        CHECK(AAsset_read(a, buf, 32) == 0);
        CHECK(AAsset_seek(a, -2, SEEK_END) == 14);
        CHECK(AAsset_seek(a, -3, SEEK_CUR) == 11);
        CHECK(AAsset_seek(a, 17, SEEK_SET) == -1);
        CHECK(AAsset_seek64(a, 0, SEEK_SET) == 0);
        const char *b = AAsset_getBuffer(a);
        CHECK(b && !memcmp(b, "0123456789abcdef", 16));
        a_off_t start = -1, len = -1;
        int fd = AAsset_openFileDescriptor(a, &start, &len);
        CHECK(fd >= 0 && start == 0 && len == 16);
        if (fd >= 0) { char c; CHECK(pread(fd, &c, 1, 5) == 1 && c == '5'); close(fd); }
        CHECK(AAsset_isAllocated(a) == 0);
        AAsset_close(a);
    }
    a = AAssetManager_open(m, "bin/Data/empty", AASSET_MODE_BUFFER);
    CHECK(a && AAsset_getLength(a) == 0 && AAsset_getBuffer(a) != NULL);
    if (a) AAsset_close(a);
    CHECK(AAssetManager_open(m, "/top.txt", 0) != NULL);   // leading slash tolerated (leaks one; fine)
    CHECK(AAssetManager_open(m, "missing", 0) == NULL);
    CHECK(AAssetManager_open(m, "../secret.txt", 0) == NULL);
    CHECK(AAssetManager_open(m, "bin/../../secret.txt", 0) == NULL);
    CHECK(AAssetManager_open(m, "bin/Data", 0) == NULL);  // directories are not assets

    AAssetDir *d = AAssetManager_openDir(m, "bin/Data");
    CHECK(d != NULL);
    int n = 0, saw_settings = 0, saw_sub = 0;
    const char *name;
    while ((name = AAssetDir_getNextFileName(d))) {
        n++;
        if (!strcmp(name, "settings.xml")) saw_settings = 1;
        if (!strcmp(name, "sub")) saw_sub = 1;
    }
    CHECK(n == 2 && saw_settings && !saw_sub);
    AAssetDir_rewind(d);
    CHECK(AAssetDir_getNextFileName(d) != NULL);
    AAssetDir_close(d);
    d = AAssetManager_openDir(m, "nope");
    CHECK(d != NULL && AAssetDir_getNextFileName(d) == NULL);
    AAssetDir_close(d);
    d = AAssetManager_openDir(m, "");
    n = 0;
    while ((name = AAssetDir_getNextFileName(d))) n++;
    CHECK(n == 1);   // top.txt only; "bin" is a directory
    AAssetDir_close(d);
}

// ---- ALooper / sensors / input ----------------------------------------------------------------
static int cb_calls;
static int looper_cb(int fd, int events, void *data)
{
    char c;
    (void)!read(fd, &c, 1);
    cb_calls++;
    *(int *)data = events;
    return 1;
}

static void *waker(void *l) { usleep(50000); ALooper_wake(l); return NULL; }

static void test_looper_sensors_input(void)
{
    CHECK(ALooper_forThread() == NULL);
    ALooper *l = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    CHECK(l != NULL && ALooper_forThread() == l && ALooper_prepare(0) == l);
    int p[2];
    CHECK(pipe(p) == 0);
    int evs = 0;
    CHECK(ALooper_addFd(l, p[0], 5, ALOOPER_EVENT_INPUT, looper_cb, &evs) == 1);
    CHECK(ALooper_pollOnce(0, NULL, NULL, NULL) == ALOOPER_POLL_TIMEOUT);
    (void)!write(p[1], "x", 1);
    CHECK(ALooper_pollOnce(100, NULL, NULL, NULL) == ALOOPER_POLL_CALLBACK && cb_calls == 1 && (evs & ALOOPER_EVENT_INPUT));
    CHECK(ALooper_removeFd(l, p[0]) == 1);
    CHECK(ALooper_addFd(l, p[0], 7, ALOOPER_EVENT_INPUT, NULL, &evs) == 1);
    (void)!write(p[1], "y", 1);
    int ofd = -1, oev = 0; void *odata = NULL;
    CHECK(ALooper_pollAll(100, &ofd, &oev, &odata) == 7 && ofd == p[0] && odata == &evs);
    ALooper_removeFd(l, p[0]);
    pthread_t t;
    pthread_create(&t, NULL, waker, l);
    CHECK(ALooper_pollOnce(2000, NULL, NULL, NULL) == ALOOPER_POLL_WAKE);
    pthread_join(t, NULL);

    ASensorManager *sm = ASensorManager_getInstance();
    CHECK(sm != NULL);
    ASensorList list = NULL;
    CHECK(ASensorManager_getSensorList(sm, &list) == 0 && list != NULL);
    CHECK(ASensorManager_getDefaultSensor(sm, 1) == NULL);
    ASensorEventQueue *q = ASensorManager_createEventQueue(sm, l, 3, NULL, NULL);
    CHECK(q != NULL && ASensorEventQueue_hasEvents(q) == 0 && ASensorEventQueue_getEvents(q, NULL, 8) == 0);
    CHECK(ASensorManager_destroyEventQueue(sm, q) == 0);
    // Unity's dump_sensor_info calls this through the Android convention (softfp on ARM).
    float (SOFTFP *res)(const void *) = android_shims_lookup("ASensor_getResolution");
    CHECK(res && res(NULL) == 0.0f);

    AInputEvent *e = android_input_event_new_key(3, AKEY_EVENT_ACTION_DOWN, 96 /* BUTTON_A */, 0);
    CHECK(AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY && AInputEvent_getDeviceId(e) == 3);
    CHECK(AKeyEvent_getKeyCode(e) == 96 && AKeyEvent_getAction(e) == 0 && AKeyEvent_getMetaState(e) == 0);
    android_input_event_free(e);

    AConfiguration *c = AConfiguration_new();
    AConfiguration_fromAssetManager(c, NULL);
    CHECK(AConfiguration_getOrientation(c) == 2 && AConfiguration_getDensity(c) == 160);
    AConfiguration_delete(c);
}

// ---- AChoreographer, the vsync clock, newer NDK functions -------------------------------------
static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

struct frames { pthread_t thread; int n, bad_thread, bad_time, max; int64_t posted, t[16]; AChoreographer *c; long delay; };
static void on_frame(long t, void *d)        // Swappy's pattern: each frame posts the next
{
    struct frames *f = d;
    if (!pthread_equal(pthread_self(), f->thread)) f->bad_thread++;
    int64_t now = mono_ns(), p = android_vsync_period_ns();
    if (sizeof(long) == 8 && (t % p || t > now || t < f->posted - p)) f->bad_time++;
    if (f->n < 16) f->t[f->n] = now / p * p;        // the vsync this frame runs in
    if (++f->n < f->max) { f->posted = mono_ns(); AChoreographer_postFrameCallbackDelayed(f->c, on_frame, f, f->delay); }
}
static void on_frame64(int64_t t, void *d)
{
    struct frames *f = d;
    int64_t now = mono_ns(), p = android_vsync_period_ns();
    if (!pthread_equal(pthread_self(), f->thread)) f->bad_thread++;
    if (t % p || t > now || t < f->posted - p) f->bad_time++;
    f->n++;
}
static int64_t vs_frame, vs_deadline, vs_present, vs_id, vs_len;
static void on_vsync(const AChoreographerFrameCallbackData *d, void *data)
{
    (void)data;
    vs_len = AChoreographerFrameCallbackData_getFrameTimelinesLength(d);
    size_t i = AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(d);
    vs_frame = AChoreographerFrameCallbackData_getFrameTimeNanos(d);
    vs_deadline = AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(d, i);
    vs_present = AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(d, i);
    vs_id = AChoreographerFrameCallbackData_getFrameTimelineVsyncId(d, i);
}
static int rr_calls; static int64_t rr_period; static pthread_t rr_thread; static int rr_bad_thread;
static void on_rr(int64_t period, void *d)
{
    (void)d;
    rr_calls++; rr_period = period;
    if (!pthread_equal(pthread_self(), rr_thread)) rr_bad_thread++;
}
static void poll_until(int *counter, int want, int ms)
{
    int64_t end = mono_ns() + ms * 1000000LL;
    while (*counter < want && mono_ns() < end) ALooper_pollOnce(50, NULL, NULL, NULL);
}

static void *no_looper_thread(void *r) { *(AChoreographer **)r = AChoreographer_getInstance(); return NULL; }

// Swappy's NDKChoreographerThread: a looper thread blocked in ALooper_pollAll(-1); the frame
// callbacks are posted from another thread under a lock the callback takes too
static pthread_mutex_t sw_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sw_cv = PTHREAD_COND_INITIALIZER;
static AChoreographer *sw_choreo; static ALooper *sw_looper; static int sw_running, sw_frames, sw_before_idle;
static pthread_t sw_thread;
static void sw_schedule(void);
static void sw_on_frame(long t, void *d)
{
    (void)t; (void)d;
    pthread_mutex_lock(&sw_mu);
    if (--sw_before_idle > 0) sw_schedule();
    sw_frames++;
    pthread_cond_broadcast(&sw_cv);
    pthread_mutex_unlock(&sw_mu);
}
static void sw_schedule(void) { AChoreographer_postFrameCallbackDelayed(sw_choreo, sw_on_frame, NULL, 1); }
static void *sw_looper_main(void *a)
{
    (void)a;
    pthread_mutex_lock(&sw_mu);
    sw_looper = ALooper_prepare(0);
    sw_choreo = AChoreographer_getInstance();
    rr_thread = pthread_self();
    AChoreographer_registerRefreshRateCallback(sw_choreo, on_rr, NULL);
    pthread_cond_broadcast(&sw_cv);
    while (sw_running) {
        pthread_mutex_unlock(&sw_mu);
        int fd, ev; void *data;
        ALooper_pollAll(-1, &fd, &ev, &data);
        pthread_mutex_lock(&sw_mu);
    }
    pthread_mutex_unlock(&sw_mu);
    return NULL;
}

// a host looper of the kind runtime/jni has: a job list serviced by one thread
static struct hjob { void (*fn)(void *); void *arg; int64_t at; } hjobs[64];
static int nhjobs, hquit;
static pthread_mutex_t hmu = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_is_host;
static void *host_current(void) { return t_is_host ? (void *)hjobs : NULL; }
static void host_post(void *looper, void (*fn)(void *), void *arg, int64_t delay_ms)
{
    (void)looper;
    pthread_mutex_lock(&hmu);
    if (nhjobs < 64) hjobs[nhjobs++] = (struct hjob){ fn, arg, mono_ns() + delay_ms * 1000000LL };
    pthread_mutex_unlock(&hmu);
}
static struct frames hframes;
static void *host_thread(void *a)
{
    (void)a;
    t_is_host = 1;
    hframes.thread = pthread_self();
    hframes.c = AChoreographer_getInstance();
    hframes.max = 5;
    hframes.posted = mono_ns();
    AChoreographer_postFrameCallback(hframes.c, on_frame, &hframes);
    while (!hquit) {
        struct hjob j = { 0 };
        pthread_mutex_lock(&hmu);
        for (int i = 0; i < nhjobs; i++)
            if (hjobs[i].at <= mono_ns()) { j = hjobs[i]; hjobs[i] = hjobs[--nhjobs]; break; }
        pthread_mutex_unlock(&hmu);
        if (j.fn) j.fn(j.arg); else usleep(1000);
    }
    return NULL;
}

static void *choreo_thread(void *arg)
{
    (void)arg;
    CHECK(AChoreographer_getInstance() == NULL);          // no looper yet
    ALooper *l = ALooper_prepare(0);
    AChoreographer *c = AChoreographer_getInstance();
    CHECK(l && c && AChoreographer_getInstance() == c);
    int64_t p = android_vsync_period_ns();
    CHECK(p == 16666666);

    // frames chained from the callback, at successive vsyncs, on this thread
    struct frames f = { .thread = pthread_self(), .c = c, .max = 6, .delay = 0 };
    f.posted = mono_ns();
    AChoreographer_postFrameCallback(c, on_frame, &f);
    poll_until(&f.n, 6, 5000);
    CHECK(f.n == 6 && !f.bad_thread && !f.bad_time);
    int mono_ok = 1;
    for (int i = 1; i < 6; i++) if (f.t[i] <= f.t[i - 1]) mono_ok = 0;   // one per vsync
    CHECK(mono_ok);

    // delayed: not before its delay
    struct frames d = { .thread = pthread_self(), .c = c, .max = 1, .delay = 0 };
    int64_t t0 = mono_ns();
    d.posted = t0;
    AChoreographer_postFrameCallbackDelayed(c, on_frame, &d, 80);
    poll_until(&d.n, 1, 5000);
    CHECK(d.n == 1 && d.t[0] - t0 >= 80000000LL);

    // 64-bit frame times (API 29) and vsync callbacks with their frame data (API 33)
    struct frames g = { .thread = pthread_self(), .c = c };
    g.posted = mono_ns();
    AChoreographer_postFrameCallback64(c, on_frame64, &g);
    AChoreographer_postFrameCallbackDelayed64(c, on_frame64, &g, 20);
    poll_until(&g.n, 2, 5000);
    CHECK(g.n == 2 && !g.bad_thread && !g.bad_time);
    vs_len = 0;
    int64_t before = mono_ns();
    CHECK(AChoreographer_postVsyncCallback(c, on_vsync, NULL) == 0);
    CHECK(AChoreographer_postVsyncCallback(c, NULL, NULL) != 0);
    int got = 0;
    for (int i = 0; i < 100 && !vs_len; i++) ALooper_pollOnce(50, NULL, NULL, NULL);
    got = vs_len == 1;
    CHECK(got && vs_frame % p == 0 && vs_frame > before - p && vs_deadline == vs_frame + p
          && vs_present == vs_frame + 2 * p && vs_id > 0);

    // refresh rate callbacks: once after registering, again when the period changes, not twice for a pair
    rr_calls = 0; rr_thread = pthread_self();
    AChoreographer_registerRefreshRateCallback(c, on_rr, NULL);
    AChoreographer_registerRefreshRateCallback(c, on_rr, NULL);
    poll_until(&rr_calls, 1, 3000);
    for (int i = 0; i < 3; i++) ALooper_pollOnce(20, NULL, NULL, NULL);
    CHECK(rr_calls == 1 && rr_period == p);
    android_vsync_set_refresh_rate(30.0f);
    poll_until(&rr_calls, 2, 3000);
    CHECK(rr_calls == 2 && rr_period == 33333333 && android_vsync_period_ns() == 33333333);
    struct frames h = { .thread = pthread_self(), .c = c, .max = 2 };
    h.posted = mono_ns();
    AChoreographer_postFrameCallback(c, on_frame, &h);
    poll_until(&h.n, 2, 5000);
    CHECK(h.n == 2 && !h.bad_time && h.t[1] - h.t[0] >= 33333333);    // 30 Hz vsyncs now
    AChoreographer_unregisterRefreshRateCallback(c, on_rr, NULL);
    android_vsync_set_refresh_rate(60.0f);
    for (int i = 0; i < 4; i++) ALooper_pollOnce(20, NULL, NULL, NULL);
    CHECK(rr_calls == 2 && !rr_bad_thread && android_vsync_period_ns() == 16666666);
    return NULL;
}

static void test_choreographer(void)
{
    pthread_t t;
    AChoreographer *none = (AChoreographer *)1;
    pthread_create(&t, NULL, no_looper_thread, &none);
    pthread_join(t, NULL);
    CHECK(none == NULL);
    pthread_create(&t, NULL, choreo_thread, NULL);
    pthread_join(t, NULL);
    CHECK(android_vsync_after(16666666 * 3) == 16666666 * 4 && android_vsync_after(16666666 * 3 + 5) == 16666666 * 4);

    // Swappy's threads: the looper thread in pollAll(-1), callbacks posted from "the swap thread"
    rr_calls = 0; rr_bad_thread = 0;
    sw_running = 1;
    pthread_mutex_lock(&sw_mu);
    pthread_create(&sw_thread, NULL, sw_looper_main, NULL);
    while (!sw_choreo) pthread_cond_wait(&sw_cv, &sw_mu);
    for (int round = 0; round < 3; round++) {        // postFrameCallbacks() before each "swap"
        if (sw_before_idle == 0) sw_schedule();
        sw_before_idle = 3;
        int want = sw_frames + 3;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;
        while (sw_frames < want && pthread_cond_timedwait(&sw_cv, &sw_mu, &ts) == 0) {}
    }
    CHECK(sw_frames >= 9 && sw_before_idle == 0);
    sw_running = 0;                                    // ~NDKChoreographerThread
    ALooper_acquire(sw_looper);
    ALooper_wake(sw_looper);
    pthread_mutex_unlock(&sw_mu);
    pthread_join(sw_thread, NULL);
    ALooper_release(sw_looper);
    CHECK(rr_calls == 1 && !rr_bad_thread);

    // on a host looper thread (runtime/jni's Java loopers)
    android_set_host_looper(host_current, host_post);
    pthread_create(&t, NULL, host_thread, NULL);
    for (int i = 0; i < 500 && hframes.n < 5; i++) usleep(10000);
    hquit = 1;
    pthread_join(t, NULL);
    android_set_host_looper(NULL, NULL);
    CHECK(hframes.c != NULL && hframes.n == 5 && !hframes.bad_thread && !hframes.bad_time);
}

static void *table_fn(const so_shim *t, const char *name)
{
    for (; t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    return (void *)-1;
}

static void test_ndk_newer(void)
{
    // which functions a game sees follows the reported API level (dlsym table and global table alike)
    CHECK(android_shims_api_level() == 0 && table_fn(android_ndk_shims, "AChoreographer_getInstance") != NULL);
    android_shims_set_api_level(23);
    CHECK(table_fn(android_ndk_shims, "AChoreographer_getInstance") == NULL);
    CHECK(android_shims_lookup("AChoreographer_postFrameCallback") == NULL);
    CHECK(table_fn(android_ndk_shims, "ATrace_isEnabled") != NULL && table_fn(android_ndk_shims, "ATrace_setCounter") == NULL);
    CHECK(table_fn(android_ndk_shims, "ALooper_prepare") != NULL && table_fn(android_ndk_shims, "AConfiguration_getSdkVersion") != NULL);
    android_shims_set_api_level(24);
    CHECK(table_fn(android_ndk_shims, "AChoreographer_getInstance") == (void *)AChoreographer_getInstance);
    CHECK(table_fn(android_ndk_shims, "AChoreographer_postFrameCallback64") == NULL);
    CHECK(table_fn(android_ndk_shims, "AHardwareBuffer_acquire") == NULL);
    android_shims_set_api_level(29);
    CHECK(table_fn(android_ndk_shims, "AChoreographer_postFrameCallback64") != NULL);
    CHECK(table_fn(android_ndk_shims, "AChoreographer_registerRefreshRateCallback") == NULL);
    CHECK(table_fn(android_ndk_shims, "ANativeWindow_setFrameRate") == NULL && table_fn(android_ndk_shims, "AHardwareBuffer_isSupported") != NULL);
    android_shims_set_api_level(30);
    CHECK(table_fn(android_ndk_shims, "AChoreographer_registerRefreshRateCallback") != NULL && table_fn(android_ndk_shims, "ANativeWindow_setFrameRate") != NULL);
    CHECK(table_fn(android_ndk_shims, "AChoreographer_postVsyncCallback") == NULL && table_fn(android_ndk_shims, "APerformanceHint_getManager") == NULL);
    CHECK(table_fn(android_shims, "AThermal_acquireManager") != NULL && table_fn(android_shims, "AThermal_getThermalHeadroom") == NULL);
    AConfiguration *c = AConfiguration_new();
    AConfiguration_fromAssetManager(c, NULL);
    CHECK(AConfiguration_getSdkVersion(c) == 30 && AConfiguration_getTouchscreen(c) == 1 && AConfiguration_getNavigation(c) == 2);
    CHECK(AConfiguration_getScreenWidthDp(c) == ANativeWindow_getWidth(android_window_get()) && AConfiguration_getLayoutDirection(c) == 1);
    AConfiguration *d = AConfiguration_new();
    AConfiguration_copy(d, c);
    CHECK(AConfiguration_diff(c, d) == 0 && AConfiguration_match(c, d));
    AConfiguration_setOrientation(d, 1);
    AConfiguration_setLanguage(d, "de");
    CHECK(AConfiguration_diff(c, d) == (0x0080 | 0x0004) && !AConfiguration_match(c, d));
    AConfiguration *e = AConfiguration_new();
    AConfiguration_setSdkVersion(e, 21);
    CHECK(AConfiguration_match(c, e) && AConfiguration_getOrientation(e) == 0);
    AConfiguration_setSdkVersion(e, 31);
    CHECK(!AConfiguration_match(c, e));
    AConfiguration_delete(c); AConfiguration_delete(d); AConfiguration_delete(e);
    android_shims_set_api_level(35);
    CHECK(table_fn(android_ndk_shims, "AWorkDuration_create") != NULL && table_fn(android_ndk_shims, "ANativeWindow_clearFrameRate") != NULL);
    android_shims_set_api_level(0);
    CHECK(table_fn(android_ndk_shims, "AChoreographer_getInstance") != NULL && table_fn(android_shims, "APerformanceHint_getManager") != NULL);

    // handheld answers, called through the tables as a game would (softfp on ARM)
    ANativeWindow *w = android_window_get();
    int32_t (SOFTFP *setfr)(ANativeWindow *, float, int8_t) = android_shims_lookup("ANativeWindow_setFrameRate");
    CHECK(setfr && setfr(w, 60.0f, 0) == 0 && setfr(NULL, 60.0f, 0) != 0 && setfr(w, -1.0f, 0) != 0);
    bool (*traceon)(void) = android_shims_lookup("ATrace_isEnabled");
    CHECK(traceon && !traceon());
    void *(*hintmgr)(void) = android_shims_lookup("APerformanceHint_getManager");
    void *(*hintsess)(void *, const int32_t *, size_t, int64_t) = android_shims_lookup("APerformanceHint_createSession");
    int64_t (*hintrate)(void *) = android_shims_lookup("APerformanceHint_getPreferredUpdateRateNanos");
    int32_t tid = 1;
    CHECK(hintmgr && hintmgr() && hintsess(hintmgr(), &tid, 1, 16666666) == NULL && hintrate(hintmgr()) == -1);
    void *(*thmgr)(void) = android_shims_lookup("AThermal_acquireManager");
    int (*thstatus)(void *) = android_shims_lookup("AThermal_getCurrentThermalStatus");
    float (SOFTFP *headroom)(void *, int) = android_shims_lookup("AThermal_getThermalHeadroom");
    CHECK(thmgr && thmgr() && thstatus(thmgr()) == 0 && headroom && headroom(thmgr(), 0) != headroom(thmgr(), 0));  // NaN
    int (*hballoc)(const void *, void **) = android_shims_lookup("AHardwareBuffer_allocate");
    void (*hbdesc)(const void *, void *) = android_shims_lookup("AHardwareBuffer_describe");
    void *hb = (void *)1;
    uint32_t desc[10]; memset(desc, 0xff, sizeof desc);
    CHECK(hballoc && hballoc(desc, &hb) != 0 && hb == NULL);
    hbdesc(NULL, desc);
    CHECK(desc[0] == 0 && desc[3] == 0);
}

// ---- ANativeWindow + egl_glue -----------------------------------------------------------------
static void *egl(const char *name)
{
    for (so_shim *s = egl_shims; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}

static int hook_calls;
static void swap_hook(void *ctx) { hook_calls += *(int *)ctx; }

static void test_window_egl(const char *fakeegl)
{
    ANativeWindow *w = ANativeWindow_fromSurface(NULL, (void *)0x1234);
    CHECK(w != NULL && android_window_is_ours(w));
    CHECK(ANativeWindow_fromSurface(NULL, NULL) == NULL);
    CHECK(ANativeWindow_getWidth(w) == 640 && ANativeWindow_getHeight(w) == 480);   // ANDROID_WINDOW_SIZE
    CHECK(ANativeWindow_getFormat(w) == WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_acquire(w);
    ANativeWindow_release(w);
    ANativeWindow_release(w);

    void *lib = dlopen(fakeegl, RTLD_NOW | RTLD_GLOBAL);
    CHECK(lib != NULL);
    if (!lib) return;
    void **last_win = dlsym(lib, "fake_last_window");
    unsigned short *fb = dlsym(lib, "fake_last_fbdev");
    int32_t *cfg_attr = dlsym(lib, "fake_last_config_attribs");
    int *swaps = dlsym(lib, "fake_swaps");
    int *surface_calls = dlsym(lib, "fake_surface_calls");

    void *(*CreateWindowSurface)(void *, void *, void *, const int32_t *) = egl("eglCreateWindowSurface");
    unsigned (*ChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *) = egl("eglChooseConfig");
    unsigned (*GetConfigAttrib)(void *, void *, int32_t, int32_t *) = egl("eglGetConfigAttrib");
    void *(*CreateContext)(void *, void *, void *, const int32_t *) = egl("eglCreateContext");
    unsigned (*SwapBuffers)(void *, void *) = egl("eglSwapBuffers");
    void *(*GetProcAddress)(const char *) = egl("eglGetProcAddress");
    CHECK(CreateWindowSurface && ChooseConfig && GetConfigAttrib && CreateContext && SwapBuffers && GetProcAddress);

    // The fbdev_window handed to EGL is the window itself, sized like the buffer geometry.
    CHECK(CreateWindowSurface(NULL, NULL, w, NULL) == (void *)0x5f);
    CHECK(*last_win == (void *)w && fb[0] == 640 && fb[1] == 480);
    CHECK(ANativeWindow_setBuffersGeometry(w, 320, 240, 0) == 0);
    CHECK(ANativeWindow_getWidth(w) == 320 && ANativeWindow_getHeight(w) == 240);
    const int32_t sattr[] = { 0x309D /* GL_COLORSPACE */, 0x3089, 0x3038 };
    int before = *surface_calls;
    CHECK(CreateWindowSurface(NULL, NULL, w, sattr) == (void *)0x5f);   // retried without attributes
    CHECK(*surface_calls == before + 2 && fb[0] == 320 && fb[1] == 240);
    CHECK(ANativeWindow_setBuffersGeometry(w, 0, 0, 0) == 0 && ANativeWindow_getWidth(w) == 640);
    CHECK(ANativeWindow_setBuffersGeometry(w, 10, 0, 0) != 0);
    CreateWindowSurface(NULL, NULL, w, NULL);
    CHECK(fb[0] == 640 && fb[1] == 480);
    // A host-provided native window replaces the fbdev one.
    android_window_set_native((void *)0xabc);
    CreateWindowSurface(NULL, NULL, w, NULL);
    CHECK(*last_win == (void *)0xabc);
    android_window_set_native(NULL);

    const int32_t cattr[] = { 0x3024, 8, 0x3142 /* RECORDABLE_ANDROID */, 1, 0x3040, 0x40, 0x3038 };
    void *cfg = NULL; int32_t num = 0;
    CHECK(ChooseConfig(NULL, cattr, &cfg, 1, &num) == 1 && num == 1);
    CHECK(cfg_attr[0] == 0x3024 && cfg_attr[2] == 0x3040 && cfg_attr[4] == 0x3038);
    int32_t v = -1;
    CHECK(GetConfigAttrib(NULL, cfg, 0x3142, &v) == 1 && v == 0);
    CHECK(GetConfigAttrib(NULL, cfg, 0x302E, &v) == 1 && v == 42);
    const int32_t ctxattr[] = { 0x3098, 3, 0x30FC /* CONTEXT_FLAGS_KHR */, 1, 0x3038 };
    CHECK(CreateContext(NULL, cfg, NULL, ctxattr) == (void *)0xc0);
    int inc = 1;
    egl_glue_set_swap_hook(swap_hook, &inc);
    CHECK(SwapBuffers(NULL, NULL) == 1 && *swaps == 1 && hook_calls == 1);
    egl_glue_set_swap_hook(NULL, NULL);
    CHECK(GetProcAddress("eglCreateWindowSurface") == (void *)CreateWindowSurface);
    CHECK(GetProcAddress("eglFakeOnly") != NULL && GetProcAddress("eglNope") == NULL);
    CHECK(egl_glue_real("eglSwapBuffers") == dlsym(lib, "eglSwapBuffers"));
    // Android's frame timestamps stay hidden even when the driver has them; the presentation time
    // is our no-op (Swappy's EGL table needs it; nothing queues frames here), not the driver's
    CHECK(egl_glue_real("eglPresentationTimeANDROID") != NULL);
    CHECK(GetProcAddress("eglGetFrameTimestampsANDROID") == NULL && GetProcAddress("eglGetNextFrameIdANDROID") == NULL);
    int (*PresentationTime)(void *, void *, int64_t) = GetProcAddress("eglPresentationTimeANDROID");
    CHECK(PresentationTime && (void *)PresentationTime == egl("eglPresentationTimeANDROID"));
    CHECK((void *)PresentationTime != egl_glue_real("eglPresentationTimeANDROID"));
    CHECK(PresentationTime && PresentationTime(NULL, NULL, 123456789) == 1);
    // eglSurfaceAttrib is not wrapped on this path: an import binds to the real one
    CHECK(egl("eglSurfaceAttrib") == NULL && egl_glue_real("eglSurfaceAttrib") == dlsym(lib, "eglSurfaceAttrib"));
}

// ---- egl_glue with a host display (UA_DISPLAY=sdl) --------------------------------------------
// The host's callbacks, standing in for SDL: make_current makes the "real" surface current in the
// fake libEGL; a swap may replace the real surface (SDL's KMSDRM driver does that).
static int hd_currents, hd_releases, hd_swaps, hd_interval = -1;
static void *hd_ctx, *hd_real, *hd_real_next, **hd_current_draw;
static int hd_make_current(void *user, void *ctx)
{
    if (ctx) { hd_currents++; hd_ctx = ctx; *hd_current_draw = hd_real; }
    else { hd_releases++; *hd_current_draw = NULL; }
    return 0;
}
static int hd_swap(void *user)
{
    hd_swaps++;
    if (hd_real_next) { hd_real = hd_real_next; hd_real_next = NULL; *hd_current_draw = hd_real; }
    return 0;
}
static int hd_swap_interval(void *user, int n) { hd_interval = n; return 0; }

// imports bind to the host table first, then egl_shims (the loader's registration order)
static void *hegl(const char *name)
{
    for (so_shim *s = egl_host_shims; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return egl(name);
}

static void test_host_display(const char *fakeegl)
{
    void *lib = dlopen(fakeegl, RTLD_NOW | RTLD_GLOBAL);
    CHECK(lib != NULL);
    if (!lib) return;
    int *terminates = dlsym(lib, "fake_terminates"), *currents = dlsym(lib, "fake_make_currents");
    int *destroyed = dlsym(lib, "fake_destroyed_surfaces"), *interval = dlsym(lib, "fake_interval");
    int *swaps = dlsym(lib, "fake_swaps"), *surface_calls = dlsym(lib, "fake_surface_calls");
    void **ctx_cfg = dlsym(lib, "fake_last_context_config"), **queried = dlsym(lib, "fake_last_query_surface");
    int *attrib_calls = dlsym(lib, "fake_attrib_calls"), *attrib_attr = dlsym(lib, "fake_attrib_attr");
    int *attrib_value = dlsym(lib, "fake_attrib_value");
    void **attrib_surface = dlsym(lib, "fake_attrib_surface");
    hd_current_draw = dlsym(lib, "fake_current_draw");
    void *(*GetDisplay)(void *) = hegl("eglGetDisplay");
    unsigned (*Initialize)(void *, int32_t *, int32_t *) = hegl("eglInitialize");
    unsigned (*Terminate)(void *) = hegl("eglTerminate");
    unsigned (*ChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *) = hegl("eglChooseConfig");
    unsigned (*GetConfigAttrib)(void *, void *, int32_t, int32_t *) = hegl("eglGetConfigAttrib");
    void *(*CreateContext)(void *, void *, void *, const int32_t *) = hegl("eglCreateContext");
    void *(*CreateWindowSurface)(void *, void *, void *, const int32_t *) = hegl("eglCreateWindowSurface");
    unsigned (*DestroySurface)(void *, void *) = hegl("eglDestroySurface");
    unsigned (*MakeCurrent)(void *, void *, void *, void *) = hegl("eglMakeCurrent");
    void *(*GetCurrentSurface)(int32_t) = hegl("eglGetCurrentSurface");
    unsigned (*QuerySurface)(void *, void *, int32_t, int32_t *) = hegl("eglQuerySurface");
    unsigned (*SwapBuffers)(void *, void *) = hegl("eglSwapBuffers");
    unsigned (*SwapInterval)(void *, int32_t) = hegl("eglSwapInterval");
    void *(*GetProcAddress)(const char *) = hegl("eglGetProcAddress");
    unsigned (*SurfaceAttrib)(void *, void *, int32_t, int32_t) = hegl("eglSurfaceAttrib");
    CHECK(terminates && currents && destroyed && interval && swaps && surface_calls && ctx_cfg && queried && hd_current_draw);
    CHECK(attrib_calls && attrib_attr && attrib_value && attrib_surface && SurfaceAttrib);
    if (!attrib_calls || !attrib_surface || !SurfaceAttrib) return;
    CHECK(GetDisplay && Initialize && Terminate && ChooseConfig && GetConfigAttrib && CreateContext && CreateWindowSurface &&
          DestroySurface && MakeCurrent && GetCurrentSurface && QuerySurface && SwapBuffers && SwapInterval && GetProcAddress);
    if (!hd_current_draw || !queried) return;

    // without a host display the wrappers pass through
    CHECK(GetDisplay(NULL) == (void *)0xd0);
    void *const D = (void *)0xd1, *const C = (void *)0xcf, *const S = (void *)0x5e, *const ctx = (void *)0xc0;
    egl_host_display bad = { D, C, NULL, hd_make_current, hd_swap, hd_swap_interval, NULL };
    egl_glue_set_host_display(&bad);                       // no surface: refused
    CHECK(GetDisplay(NULL) == (void *)0xd0);
    egl_host_display hd = { D, C, S, hd_make_current, hd_swap, hd_swap_interval, NULL };
    egl_glue_set_host_display(&hd);
    hd_real = S;

    CHECK(GetDisplay(NULL) == D);
    int32_t major = 0, minor = 0;
    CHECK(Initialize(D, &major, &minor) == 1 && major == 1 && minor == 4);
    const int32_t cattr[] = { 0x3024, 8, 0x3142 /* RECORDABLE_ANDROID */, 1, 0x3033, 4, 0x3038 };
    void *cfgs[4] = { 0 };
    int32_t num = -1;
    CHECK(ChooseConfig(D, cattr, NULL, 0, &num) == 1 && num == 1);
    CHECK(ChooseConfig(D, cattr, cfgs, 4, &num) == 1 && num == 1 && cfgs[0] == C);
    CHECK(ChooseConfig(D, cattr, cfgs, 0, &num) == 1 && num == 0);
    CHECK(ChooseConfig((void *)0xd0, cattr, cfgs, 4, &num) == 1 && cfgs[0] == (void *)0x1);   // another display: as before
    int32_t v = -1;
    CHECK(GetConfigAttrib(D, C, 0x3142, &v) == 1 && v == 0);
    const int32_t ctxattr[] = { 0x3098, 3, 0x3038 };
    CHECK(CreateContext(D, (void *)0x77, NULL, ctxattr) == ctx && *ctx_cfg == C);   // always the host config

    // Unity's surface handle stands for the host's real surface
    ANativeWindow *w = android_window_get();
    int before = *surface_calls;
    void *H = CreateWindowSurface(D, C, w, NULL);
    CHECK(H != NULL && H != S);
    const int32_t srgb[] = { 0x309D, 0x3089, 0x3038 };       // linear-color-space game: warns, still the host surface
    CHECK(CreateWindowSurface(D, C, w, srgb) == H && *surface_calls == before);
    v = 0;
    CHECK(QuerySurface(D, H, 0x3057, &v) == 1 && v == 77 && *queried == S);   // before any make-current: the host's

    int m = *currents;
    CHECK(MakeCurrent(D, H, H, ctx) == 1 && hd_currents == 1 && hd_ctx == ctx && *currents == m);
    CHECK(GetCurrentSurface(0x3059) == H && GetCurrentSurface(0x305A) == H);   // IsCurrentInternal
    CHECK(MakeCurrent(D, NULL, NULL, NULL) == 1 && hd_releases == 1 && *currents == m + 1);
    CHECK(GetCurrentSurface(0x3059) == NULL);
    CHECK(MakeCurrent(D, (void *)0x99, (void *)0x99, ctx) == 1 && hd_releases == 2 && *currents == m + 2);  // a pbuffer
    CHECK(MakeCurrent((void *)0xd0, S, S, ctx) == 1 && hd_currents == 1 && *currents == m + 3);           // another display

    int inc = 1, s0 = *swaps;
    hook_calls = 0;
    egl_glue_set_swap_hook(swap_hook, &inc);
    CHECK(MakeCurrent(D, H, H, ctx) == 1 && hd_currents == 2);
    // eglSurfaceAttrib (Unity 2020.3+) on the handle: the real surface; one the driver refuses is not kept
    int a0 = *attrib_calls;
    CHECK(SurfaceAttrib(D, H, 0x3093 /* SWAP_BEHAVIOR */, 0x3095 /* BUFFER_DESTROYED */) == 1 && *attrib_surface == S &&
          *attrib_attr == 0x3093 && *attrib_value == 0x3095 && *attrib_calls == a0 + 1);
    CHECK(SurfaceAttrib(D, H, 0x3430 /* TIMESTAMPS_ANDROID */, 1) == 0 && *attrib_surface == S && *attrib_calls == a0 + 2);
    CHECK(SwapBuffers(D, H) == 1 && hd_swaps == 1 && hook_calls == 1 && *swaps == s0 && *attrib_calls == a0 + 2);
    // the window system replaces its surface during a swap: the handle follows it, the attribute is set again
    void *const S2 = (void *)0x5f00;
    hd_real_next = S2;
    CHECK(SwapBuffers(D, H) == 1 && hd_swaps == 2 && GetCurrentSurface(0x3059) == H);
    CHECK(QuerySurface(D, H, 0x3057, &v) == 1 && *queried == S2);
    CHECK(*attrib_calls == a0 + 3 && *attrib_surface == S2 && *attrib_attr == 0x3093 && *attrib_value == 0x3095);
    CHECK(SwapBuffers(D, H) == 1 && *attrib_calls == a0 + 3);                          // once
    CHECK(SurfaceAttrib(D, (void *)0x99, 0x3093, 0x3094) == 1 && *attrib_surface == (void *)0x99);   // a pbuffer
    CHECK(SurfaceAttrib((void *)0xd0, S, 0x3093, 0x3094) == 1 && *attrib_surface == S);              // another display
    CHECK(SwapBuffers(D, (void *)0x99) == 1 && hd_swaps == 3 && hook_calls == 4 && *swaps == s0 + 1);
    egl_glue_set_swap_hook(NULL, NULL);
    CHECK(SwapInterval(D, 2) == 1 && hd_interval == 2 && *interval == -1);
    CHECK(SwapInterval((void *)0xd0, 0) == 1 && *interval == 0);
    CHECK(MakeCurrent(D, NULL, NULL, NULL) == 1);

    int d0 = *destroyed;
    CHECK(DestroySurface(D, H) == 1 && *destroyed == d0);
    CHECK(DestroySurface(D, (void *)0x99) == 1 && *destroyed == d0 + 1);
    CHECK(CreateWindowSurface(D, C, w, NULL) == H);           // pause/resume: the same handle again
    int t0 = *terminates;
    CHECK(Terminate(D) == 1 && *terminates == t0);
    CHECK(Terminate((void *)0xd0) == 1 && *terminates == t0 + 1);

    CHECK(GetProcAddress("eglMakeCurrent") == (void *)MakeCurrent && GetProcAddress("eglSwapBuffers") == (void *)SwapBuffers);
    CHECK(GetProcAddress("eglGetConfigAttrib") == (void *)GetConfigAttrib);
    CHECK(GetProcAddress("eglFakeOnly") != NULL);
    CHECK(GetProcAddress("eglSurfaceAttrib") == (void *)SurfaceAttrib);
    CHECK(GetProcAddress("eglPresentationTimeANDROID") == egl("eglPresentationTimeANDROID"));
    CHECK(GetProcAddress("eglGetFrameTimestampsANDROID") == NULL);
    egl_glue_set_host_display(NULL);
    CHECK(GetDisplay(NULL) == (void *)0xd0);
}

// ---- libmediandk: stubs failing the NDK way (Unity 2022.3 links against it) ---------------------
static void *media(const char *name)
{
    for (so_shim *s = android_mediandk_shims; s->name; s++) if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}

static void test_mediandk(void)
{
    CHECK(android_shims_for_library("libmediandk.so") == android_mediandk_shims);
    CHECK(android_shims_for_library("/system/lib/libmediandk.so") == android_mediandk_shims);
    int n = 0, in_all = 0;
    for (so_shim *s = android_mediandk_shims; s->name; s++, n++) in_all += android_shims_lookup(s->name) == s->fn;
    CHECK(n > 60 && in_all == n);                          // the global table has them too (DT_NEEDED imports)

    // the key constants: data objects holding the NDK's strings
    static const char *const keys[][2] = {
        { "AMEDIAFORMAT_KEY_MIME", "mime" }, { "AMEDIAFORMAT_KEY_WIDTH", "width" }, { "AMEDIAFORMAT_KEY_HEIGHT", "height" },
        { "AMEDIAFORMAT_KEY_DURATION", "durationUs" }, { "AMEDIAFORMAT_KEY_FRAME_RATE", "frame-rate" },
        { "AMEDIAFORMAT_KEY_COLOR_FORMAT", "color-format" }, { "AMEDIAFORMAT_KEY_STRIDE", "stride" },
        { "AMEDIAFORMAT_KEY_SAMPLE_RATE", "sample-rate" }, { "AMEDIAFORMAT_KEY_CHANNEL_COUNT", "channel-count" },
        { "AMEDIAFORMAT_KEY_LANGUAGE", "language" }, { "AMEDIAFORMAT_KEY_ROTATION", "rotation-degrees" },
        { "AMEDIAFORMAT_KEY_SLICE_HEIGHT", "slice-height" }, { "AMEDIAFORMAT_KEY_COLOR_RANGE", "color-range" },
        { "AMEDIAFORMAT_KEY_COLOR_STANDARD", "color-standard" }, { "AMEDIAFORMAT_KEY_ENCODER_DELAY", "encoder-delay" },
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *const *k = media(keys[i][0]);
        CHECK(k && *k && !strcmp(*k, keys[i][1]));
    }

    const int32_t UNSUPPORTED = -10002;
    void *(*ext_new)(void) = media("AMediaExtractor_new");
    int32_t (*ext_fd)(void *, int, int64_t, int64_t) = media("AMediaExtractor_setDataSourceFd");
    int32_t (*ext_url)(void *, const char *) = media("AMediaExtractor_setDataSource");
    size_t (*ext_tracks)(void *) = media("AMediaExtractor_getTrackCount");
    void *(*ext_format)(void *, size_t) = media("AMediaExtractor_getTrackFormat");
    ssize_t (*ext_read)(void *, uint8_t *, size_t) = media("AMediaExtractor_readSampleData");
    int64_t (*ext_time)(void *) = media("AMediaExtractor_getSampleTime");
    bool (*ext_advance)(void *) = media("AMediaExtractor_advance");
    int32_t (*ext_delete)(void *) = media("AMediaExtractor_delete");
    void *(*codec_new)(const char *) = media("AMediaCodec_createDecoderByType");
    int32_t (*codec_configure)(void *, const void *, void *, void *, uint32_t) = media("AMediaCodec_configure");
    uint8_t *(*codec_in)(void *, size_t, size_t *) = media("AMediaCodec_getInputBuffer");
    ssize_t (*codec_dequeue)(void *, void *, int64_t) = media("AMediaCodec_dequeueOutputBuffer");
    int32_t (*codec_delete)(void *) = media("AMediaCodec_delete");
    bool (*fmt_int)(void *, const char *, int32_t *) = media("AMediaFormat_getInt32");
    bool (*fmt_str)(void *, const char *, const char **) = media("AMediaFormat_getString");
    int32_t (*reader_new)(int32_t, int32_t, int32_t, uint64_t, int32_t, void **) = media("AImageReader_newWithUsage");
    void (*image_delete_async)(void *, int) = media("AImage_deleteAsync");
    void *(*source_new)(void) = media("AMediaDataSource_new");
    CHECK(ext_new && ext_fd && ext_url && ext_tracks && ext_format && ext_read && ext_time && ext_advance && ext_delete &&
          codec_new && codec_configure && codec_in && codec_dequeue && codec_delete && fmt_int && fmt_str && reader_new &&
          image_delete_async && source_new);
    if (!(ext_new && ext_fd && ext_url && ext_tracks && ext_format && ext_read && ext_time && ext_advance && ext_delete &&
          codec_new && codec_configure && codec_in && codec_dequeue && codec_delete && fmt_int && fmt_str && reader_new &&
          image_delete_async && source_new))
        return;
    // what a VideoPlayer does, failing at every step without touching the NULL objects
    void *ex = ext_new();
    CHECK(ex == NULL);
    CHECK(ext_fd(ex, 0, 0, 1000) == UNSUPPORTED && ext_url(ex, "file:///clip.mp4") == UNSUPPORTED);
    CHECK(ext_tracks(ex) == 0 && ext_format(ex, 0) == NULL);
    uint8_t buf[16];
    CHECK(ext_read(ex, buf, sizeof buf) == -1 && ext_time(ex) == -1 && !ext_advance(ex));
    CHECK(ext_delete(ex) == 0);
    void *codec = codec_new("video/avc");
    CHECK(codec == NULL && codec_configure(codec, NULL, NULL, NULL, 0) == UNSUPPORTED);
    size_t sz = 77;
    CHECK(codec_in(codec, 0, &sz) == NULL && sz == 0 && codec_dequeue(codec, NULL, 1000) < 0 && codec_delete(codec) == 0);
    int32_t iv = 5;
    const char *sv = "x";
    CHECK(!fmt_int(NULL, "width", &iv) && iv == 5 && !fmt_str(NULL, "mime", &sv));
    void *reader = (void *)1;
    CHECK(reader_new(640, 360, 0x23, 0, 2, &reader) == UNSUPPORTED && reader == NULL);
    int fds[2];
    CHECK(pipe(fds) == 0);
    image_delete_async(NULL, fds[0]);                      // the fence fd is the callee's
    CHECK(fcntl(fds[0], F_GETFD) == -1 && errno == EBADF);
    close(fds[1]);
    CHECK(source_new() == NULL);
}

// ---- OpenSL ES, driven like FMOD::OutputOpenSL ------------------------------------------------
#define BUF_FRAMES 256
#define NBUF 4
#define TOTAL_BUFS 40

static int16_t bufs[NBUF][BUF_FRAMES * 2];
static volatile int next_seq, callbacks;
static int16_t fill_value_override;

static void bq_callback(SLBufferQueueItf bq, void *ctx)
{
    (void)ctx;
    callbacks++;
    if (next_seq >= TOTAL_BUFS) return;
    int16_t *b = bufs[next_seq % NBUF];
    int16_t v = fill_value_override ? fill_value_override : (int16_t)(next_seq + 1);
    for (int i = 0; i < BUF_FRAMES * 2; i++) b[i] = v;
    next_seq++;
    CHECK((*bq)->Enqueue(bq, b, sizeof bufs[0]) == SL_RESULT_SUCCESS);
}

static void wait_for(volatile int *v, int target, int ms)
{
    for (int i = 0; i < ms / 5 && *v < target; i++) usleep(5000);
}

static void test_opensles(const char *rawpath)
{
    const so_shim *t = android_shims_for_library("/system/lib/libOpenSLES.so");
    CHECK(t == android_sles_shims);
    // Resolve like FMOD does (dlsym of slCreateEngine and of the SL_IID_* data symbols).
    SLresult (*create)(SLObjectItf *, SLuint32, const SLEngineOption *, SLuint32, const SLInterfaceID *, const SLboolean *) = NULL;
    const SLInterfaceID *iid_engine = NULL, *iid_bq = NULL, *iid_play = NULL, *iid_cfg = NULL, *iid_seek = NULL, *iid_vol = NULL;
    for (const so_shim *s = t; s->name; s++) {
        if (!strcmp(s->name, "slCreateEngine")) *(void **)&create = s->fn;
        if (!strcmp(s->name, "SL_IID_ENGINE")) iid_engine = s->fn;
        if (!strcmp(s->name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE")) iid_bq = s->fn;
        if (!strcmp(s->name, "SL_IID_PLAY")) iid_play = s->fn;
        if (!strcmp(s->name, "SL_IID_ANDROIDCONFIGURATION")) iid_cfg = s->fn;
        if (!strcmp(s->name, "SL_IID_SEEK")) iid_seek = s->fn;
        if (!strcmp(s->name, "SL_IID_VOLUME")) iid_vol = s->fn;
    }
    CHECK(create == slCreateEngine && iid_engine && *iid_engine == SL_IID_ENGINE && iid_bq && iid_play && iid_cfg);
    CHECK((*iid_engine)->time_low == 0x8d97c260 && (*iid_bq)->time_low == 0x198e4940);
    if (!create || !iid_engine) return;

    SLObjectItf engineObj = NULL, mixObj = NULL, playerObj = NULL;
    SLEngineItf engine = NULL;
    CHECK(create(&engineObj, 0, NULL, 0, NULL, NULL) == SL_RESULT_SUCCESS);
    CHECK((*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    CHECK((*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK((*engineObj)->GetInterface(engineObj, *iid_engine, &engine) == SL_RESULT_SUCCESS && engine);
    CHECK((*engine)->CreateOutputMix(engine, &mixObj, 0, NULL, NULL) == SL_RESULT_SUCCESS);
    CHECK((*mixObj)->Realize(mixObj, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);

    SLDataLocator_BufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, NBUF };
    SLDataFormat_PCM pcm = { SL_DATAFORMAT_PCM, 2, 48000 * 1000, 16, 16,
                             SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource src = { &loc, &pcm };
    SLDataLocator_OutputMix outloc = { SL_DATALOCATOR_OUTPUTMIX, (void *)mixObj };
    SLDataSink snk = { &outloc, NULL };
    const SLInterfaceID ids[2] = { *iid_bq, *iid_cfg };
    const SLboolean req[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
    const SLInterfaceID bad[1] = { *iid_seek };
    CHECK((*engine)->CreateAudioPlayer(engine, &playerObj, &src, &snk, 1, bad, req) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK((*engine)->CreateAudioPlayer(engine, &playerObj, &src, &snk, 2, ids, req) == SL_RESULT_SUCCESS);

    SLAndroidConfigurationItf cfg = NULL;
    SLPlayItf play = NULL;
    SLBufferQueueItf bq = NULL;
    SLVolumeItf vol = NULL;
    CHECK((*playerObj)->GetInterface(playerObj, *iid_cfg, &cfg) == SL_RESULT_SUCCESS);   // before Realize
    CHECK((*playerObj)->GetInterface(playerObj, *iid_play, &play) == SL_RESULT_PRECONDITIONS_VIOLATED);
    SLint32 stream = SL_ANDROID_STREAM_MEDIA;
    CHECK((*cfg)->SetConfiguration(cfg, (const SLchar *)SL_ANDROID_KEY_STREAM_TYPE, &stream, sizeof stream) == SL_RESULT_SUCCESS);
    CHECK((*playerObj)->Realize(playerObj, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    CHECK((*playerObj)->GetInterface(playerObj, *iid_play, &play) == SL_RESULT_SUCCESS);
    CHECK((*playerObj)->GetInterface(playerObj, *iid_bq, &bq) == SL_RESULT_SUCCESS);
    CHECK((*playerObj)->GetInterface(playerObj, *iid_vol, &vol) == SL_RESULT_SUCCESS);
    CHECK((*playerObj)->GetInterface(playerObj, *iid_seek, &vol) == SL_RESULT_FEATURE_UNSUPPORTED && vol == NULL);
    CHECK((*playerObj)->GetInterface(playerObj, SL_IID_VOLUME, &vol) == SL_RESULT_SUCCESS);
    if (!play || !bq) return;
    CHECK((*bq)->RegisterCallback(bq, bq_callback, NULL) == SL_RESULT_SUCCESS);

    // Prime the queue like FMOD's init (updateOutput x numBuffers), then overflow by one.
    for (int i = 0; i < NBUF; i++) {
        for (int j = 0; j < BUF_FRAMES * 2; j++) bufs[i][j] = (int16_t)(i + 1);
        CHECK((*bq)->Enqueue(bq, bufs[i], sizeof bufs[i]) == SL_RESULT_SUCCESS);
    }
    next_seq = NBUF;
    CHECK((*bq)->Enqueue(bq, bufs[0], sizeof bufs[0]) == SL_RESULT_BUFFER_INSUFFICIENT);
    CHECK((*bq)->Enqueue(bq, NULL, 4) == SL_RESULT_PARAMETER_INVALID);
    SLBufferQueueState st;
    CHECK((*bq)->GetState(bq, &st) == SL_RESULT_SUCCESS && st.count == NBUF && st.index == 0);

    CHECK((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    SLuint32 ps = 0;
    CHECK((*play)->GetPlayState(play, &ps) == SL_RESULT_SUCCESS && ps == SL_PLAYSTATE_PLAYING);
    CHECK((*bq)->RegisterCallback(bq, bq_callback, NULL) == SL_RESULT_PRECONDITIONS_VIOLATED);
    wait_for(&callbacks, TOTAL_BUFS, 20000);
    CHECK(callbacks >= TOTAL_BUFS);
    SLmillisecond pos = 0;
    CHECK((*play)->GetPosition(play, &pos) == SL_RESULT_SUCCESS && pos >= TOTAL_BUFS * BUF_FRAMES * 1000 / 48000 - 1);
    CHECK((*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED) == SL_RESULT_SUCCESS);
    CHECK((*bq)->GetState(bq, &st) == SL_RESULT_SUCCESS && st.count == 0 && st.index == TOTAL_BUFS);
    fprintf(stderr, "opensles: %d callbacks, position %u ms\n", callbacks, pos);

    // Second phase: half volume (-602 mB) on a constant signal of 1000.
    CHECK((*bq)->Clear(bq) == SL_RESULT_SUCCESS);
    CHECK((*bq)->GetState(bq, &st) == SL_RESULT_SUCCESS && st.count == 0 && st.index == 0);
    CHECK((*vol)->SetVolumeLevel(vol, -602) == SL_RESULT_SUCCESS);
    fill_value_override = 1000;
    next_seq = 0;
    callbacks = 0;
    for (int i = 0; i < NBUF; i++) {
        for (int j = 0; j < BUF_FRAMES * 2; j++) bufs[i][j] = 1000;
        (*bq)->Enqueue(bq, bufs[i], sizeof bufs[i]);
    }
    next_seq = TOTAL_BUFS - 8;
    CHECK((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    wait_for(&callbacks, 8 + NBUF, 20000);
    CHECK((*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED) == SL_RESULT_SUCCESS);

    (*playerObj)->Destroy(playerObj);
    (*mixObj)->Destroy(mixObj);
    (*engineObj)->Destroy(engineObj);

    // The disk driver wrote everything SDL played: silence, then 1..TOTAL_BUFS in order, each for
    // exactly one buffer, then silence/half-volume samples.
    if (!rawpath) return;   // e.g. SDL's dummy driver: queue logic only, no output to check
    size_t len = 0;
    int16_t *raw = (int16_t *)slurp(rawpath, &len);
    CHECK(raw && len > 0);
    if (!raw) return;
    size_t n = len / 2, i = 0;
    while (i < n && raw[i] == 0) i++;
    int ok = 1, gaps = 0;
    for (int seq = 1; seq <= TOTAL_BUFS && ok; seq++) {
        while (i < n && raw[i] == 0) { i++; gaps++; }   // underruns (QEMU is slow) show as silence
        for (int k = 0; k < BUF_FRAMES * 2; k++, i++) {
            if (i >= n || raw[i] != seq) {
                fprintf(stderr, "opensles: sample %zu = %d, expected %d\n", i, i < n ? raw[i] : -999, seq);
                ok = 0;
                break;
            }
        }
    }
    CHECK(ok);
    int halves = 0;
    for (; i < n; i++) if (raw[i] == 500) halves++;
    CHECK(halves >= 8 * BUF_FRAMES * 2);
    fprintf(stderr, "opensles: sequence ok=%d, %d silent samples between buffers, %d half-volume samples\n", ok, gaps, halves);
    free(raw);
}

int main(int argc, char **argv)
{
    const char *fakeegl = argc > 1 ? argv[1] : "./libfakeegl.so";
    setenv("ANDROID_SHIM_LIBEGL", fakeegl, 1);   // egl_glue must load the fake, not a real libEGL
    snprintf(tmpdir, sizeof tmpdir, "/tmp/shimtest.%d", (int)getpid());
    mkdir(tmpdir, 0755);
    test_log();
    test_assets();
    test_looper_sensors_input();
    test_choreographer();
    test_ndk_newer();
    test_window_egl(fakeegl);
    test_host_display(fakeegl);
    test_mediandk();
    const char *drv = getenv("SDL_AUDIODRIVER");
    if (drv && *drv) test_opensles(!strcmp(drv, "disk") ? getenv("SDL_DISKAUDIOFILE") : NULL);
    else fprintf(stderr, "opensles: skipped (set SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=... or =dummy)\n");
    fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
