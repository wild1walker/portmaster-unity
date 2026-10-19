// libandroid.so: ALooper_*, AInputEvent_*/AKeyEvent_*/AMotionEvent_*, ASensor* (no sensors),
// AConfiguration_*.
#define _GNU_SOURCE
#include "android_shims.h"
#include "shim_internal.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

// ---- ALooper: poll() over registered fds plus an eventfd for ALooper_wake -------------------
#define LOOPER_MAX_FDS 32

struct looper_fd { int fd, ident, events; ALooper_callbackFunc cb; void *data; };

struct ALooper {
    pthread_mutex_t lock;
    int refcount;
    int wake_fd;
    int allow_non_callbacks;
    int nfds;
    struct looper_fd fds[LOOPER_MAX_FDS];
};

static __thread ALooper *t_looper;

ALooper *ALooper_forThread(void) { return t_looper; }

ALooper *ALooper_prepare(int opts)
{
    if (t_looper) return t_looper;
    ALooper *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    pthread_mutex_init(&l->lock, NULL);
    l->refcount = 1;
    l->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    l->allow_non_callbacks = opts & ALOOPER_PREPARE_ALLOW_NON_CALLBACKS;
    t_looper = l;
    SHIM_TRACE("ALooper_prepare(%d) -> %p", opts, (void *)l);
    return l;
}

void ALooper_acquire(ALooper *l) { if (l) __atomic_add_fetch(&l->refcount, 1, __ATOMIC_SEQ_CST); }

void ALooper_release(ALooper *l)
{
    if (!l || __atomic_sub_fetch(&l->refcount, 1, __ATOMIC_SEQ_CST) > 0) return;
    if (t_looper == l) t_looper = NULL;
    if (l->wake_fd >= 0) close(l->wake_fd);
    pthread_mutex_destroy(&l->lock);
    free(l);
}

void ALooper_wake(ALooper *l)
{
    uint64_t one = 1;
    if (l && l->wake_fd >= 0) (void)!write(l->wake_fd, &one, sizeof one);
}

int ALooper_addFd(ALooper *l, int fd, int ident, int events, ALooper_callbackFunc cb, void *data)
{
    if (!l || fd < 0) return -1;
    if (!cb && (!l->allow_non_callbacks || ident < 0)) return -1;
    pthread_mutex_lock(&l->lock);
    int i;
    for (i = 0; i < l->nfds && l->fds[i].fd != fd; i++) {}
    if (i == l->nfds) {
        if (l->nfds == LOOPER_MAX_FDS) { pthread_mutex_unlock(&l->lock); return -1; }
        l->nfds++;
    }
    l->fds[i] = (struct looper_fd){ fd, cb ? ALOOPER_POLL_CALLBACK : ident, events, cb, data };
    pthread_mutex_unlock(&l->lock);
    return 1;
}

int ALooper_removeFd(ALooper *l, int fd)
{
    if (!l) return -1;
    pthread_mutex_lock(&l->lock);
    int found = 0;
    for (int i = 0; i < l->nfds; i++) {
        if (l->fds[i].fd == fd) { l->fds[i] = l->fds[--l->nfds]; found = 1; break; }
    }
    pthread_mutex_unlock(&l->lock);
    return found;
}

static int poll_events_to_looper(short re)
{
    int ev = 0;
    if (re & POLLIN) ev |= ALOOPER_EVENT_INPUT;
    if (re & POLLOUT) ev |= ALOOPER_EVENT_OUTPUT;
    if (re & POLLERR) ev |= ALOOPER_EVENT_ERROR;
    if (re & (POLLHUP | POLLNVAL)) ev |= ALOOPER_EVENT_HANGUP;
    return ev;
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData)
{
    ALooper *l = t_looper;
    if (!l) {   // Android aborts here; sleeping is kinder
        if (timeoutMillis > 0) usleep(timeoutMillis * 1000);
        return ALOOPER_POLL_TIMEOUT;
    }
    struct pollfd pfd[LOOPER_MAX_FDS + 1];
    struct looper_fd fds[LOOPER_MAX_FDS];
    pthread_mutex_lock(&l->lock);
    int n = l->nfds;
    memcpy(fds, l->fds, n * sizeof *fds);
    pthread_mutex_unlock(&l->lock);
    pfd[0] = (struct pollfd){ l->wake_fd, POLLIN, 0 };
    for (int i = 0; i < n; i++) {
        short ev = 0;
        if (fds[i].events & ALOOPER_EVENT_INPUT) ev |= POLLIN;
        if (fds[i].events & ALOOPER_EVENT_OUTPUT) ev |= POLLOUT;
        pfd[i + 1] = (struct pollfd){ fds[i].fd, ev, 0 };
    }
    int r = poll(pfd, n + 1, timeoutMillis);
    if (r < 0) return errno == EINTR ? ALOOPER_POLL_WAKE : ALOOPER_POLL_ERROR;
    if (r == 0) return ALOOPER_POLL_TIMEOUT;
    int result = ALOOPER_POLL_TIMEOUT;
    if (pfd[0].revents) {
        uint64_t v;
        (void)!read(l->wake_fd, &v, sizeof v);
        result = ALOOPER_POLL_WAKE;
    }
    for (int i = 0; i < n; i++) {
        if (!pfd[i + 1].revents) continue;
        int ev = poll_events_to_looper(pfd[i + 1].revents);
        if (fds[i].cb) {
            if (!fds[i].cb(fds[i].fd, ev, fds[i].data)) ALooper_removeFd(l, fds[i].fd);
            result = ALOOPER_POLL_CALLBACK;
        } else {
            if (outFd) *outFd = fds[i].fd;
            if (outEvents) *outEvents = ev;
            if (outData) *outData = fds[i].data;
            return fds[i].ident;
        }
    }
    return result;
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData)
{
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int r = ALooper_pollOnce(timeoutMillis, outFd, outEvents, outData);
        if (r != ALOOPER_POLL_CALLBACK) return r;
        if (timeoutMillis >= 0) {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long el = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            if (el >= timeoutMillis) return ALOOPER_POLL_TIMEOUT;
            timeoutMillis -= el;
            t0 = t1;
        }
    }
}

// ---- input events --------------------------------------------------------------------------
#define AINPUT_SOURCE_KEYBOARD 0x101
#define AINPUT_SOURCE_GAMEPAD 0x401

struct AInputEvent {
    int32_t type, deviceId, source;
    int32_t action, keyCode, metaState, flags, repeatCount, scanCode;
    float x, y;
    int64_t downTime, eventTime;
};

AInputEvent *android_input_event_new_key(int32_t deviceId, int32_t action, int32_t keyCode, int32_t metaState)
{
    AInputEvent *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    e->type = AINPUT_EVENT_TYPE_KEY;
    e->deviceId = deviceId;
    e->source = AINPUT_SOURCE_KEYBOARD;
    e->action = action;
    e->keyCode = keyCode;
    e->metaState = metaState;
    e->eventTime = e->downTime = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return e;
}

void android_input_event_free(AInputEvent *e) { free(e); }

int32_t AInputEvent_getType(const AInputEvent *e) { return e ? e->type : 0; }
int32_t AInputEvent_getDeviceId(const AInputEvent *e) { return e ? e->deviceId : 0; }
int32_t AInputEvent_getSource(const AInputEvent *e) { return e ? e->source : 0; }
int32_t AKeyEvent_getAction(const AInputEvent *e) { return e ? e->action : 0; }
int32_t AKeyEvent_getKeyCode(const AInputEvent *e) { return e ? e->keyCode : 0; }
int32_t AKeyEvent_getMetaState(const AInputEvent *e) { return e ? e->metaState : 0; }
int32_t AKeyEvent_getFlags(const AInputEvent *e) { return e ? e->flags : 0; }
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e) { return e ? e->repeatCount : 0; }
int32_t AKeyEvent_getScanCode(const AInputEvent *e) { return e ? e->scanCode : 0; }
int64_t AKeyEvent_getDownTime(const AInputEvent *e) { return e ? e->downTime : 0; }
int64_t AKeyEvent_getEventTime(const AInputEvent *e) { return e ? e->eventTime : 0; }
int32_t AMotionEvent_getAction(const AInputEvent *e) { return e ? e->action : 0; }
size_t AMotionEvent_getPointerCount(const AInputEvent *e) { return e && e->type == AINPUT_EVENT_TYPE_MOTION; }
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t i) { (void)e; (void)i; return 0; }
SOFTFP float AMotionEvent_getX(const AInputEvent *e, size_t i) { (void)i; return e ? e->x : 0; }
SOFTFP float AMotionEvent_getY(const AInputEvent *e, size_t i) { (void)i; return e ? e->y : 0; }
SOFTFP float AMotionEvent_getAxisValue(const AInputEvent *e, int32_t axis, size_t i)
{
    (void)i;
    return !e ? 0 : axis == 0 ? e->x : axis == 1 ? e->y : 0;
}

// AInputQueue: no queue is ever handed out (Unity uses the Java input path).
int32_t AInputQueue_getEvent(AInputQueue *q, AInputEvent **out) { (void)q; if (out) *out = NULL; return -1; }
int32_t AInputQueue_hasEvents(AInputQueue *q) { (void)q; return 0; }
int32_t AInputQueue_preDispatchEvent(AInputQueue *q, AInputEvent *e) { (void)q; (void)e; return 0; }
void AInputQueue_finishEvent(AInputQueue *q, AInputEvent *e, int handled) { (void)q; (void)e; (void)handled; }
void AInputQueue_attachLooper(AInputQueue *q, ALooper *l, int ident, ALooper_callbackFunc cb, void *data)
{ (void)q; (void)l; (void)ident; (void)cb; (void)data; }
void AInputQueue_detachLooper(AInputQueue *q) { (void)q; }

// ---- sensors: a manager with an empty list -------------------------------------------------
struct ASensorManager { int dummy; };
struct ASensorEventQueue { ALooper *looper; int ident; ALooper_callbackFunc cb; void *data; };

static ASensorManager g_sensor_mgr;
static ASensorRef g_no_sensors[1] = { NULL };

ASensorManager *ASensorManager_getInstance(void) { return &g_sensor_mgr; }
ASensorManager *ASensorManager_getInstanceForPackage(const char *pkg) { (void)pkg; return &g_sensor_mgr; }

int ASensorManager_getSensorList(ASensorManager *m, ASensorList *list)
{
    (void)m;
    if (list) *list = g_no_sensors;
    return 0;
}

ASensor const *ASensorManager_getDefaultSensor(ASensorManager *m, int type) { (void)m; (void)type; return NULL; }
ASensor const *ASensorManager_getDefaultSensorEx(ASensorManager *m, int type, int wakeUp)
{ (void)m; (void)type; (void)wakeUp; return NULL; }

ASensorEventQueue *ASensorManager_createEventQueue(ASensorManager *m, ALooper *l, int ident, ALooper_callbackFunc cb, void *data)
{
    (void)m;
    ASensorEventQueue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    *q = (ASensorEventQueue){ l, ident, cb, data };
    return q;
}

int ASensorManager_destroyEventQueue(ASensorManager *m, ASensorEventQueue *q) { (void)m; free(q); return 0; }
int ASensorEventQueue_enableSensor(ASensorEventQueue *q, ASensor const *s) { (void)q; return s ? 0 : -EINVAL; }
int ASensorEventQueue_disableSensor(ASensorEventQueue *q, ASensor const *s) { (void)q; return s ? 0 : -EINVAL; }
int ASensorEventQueue_setEventRate(ASensorEventQueue *q, ASensor const *s, int32_t usec) { (void)q; (void)usec; return s ? 0 : -EINVAL; }
int ASensorEventQueue_hasEvents(ASensorEventQueue *q) { (void)q; return 0; }
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue *q, ASensorEvent *ev, size_t count) { (void)q; (void)ev; (void)count; return 0; }

// Never reached with an empty sensor list, but must not crash if called.
const char *ASensor_getName(ASensor const *s) { (void)s; return "none"; }
const char *ASensor_getVendor(ASensor const *s) { (void)s; return "android-shim"; }
int ASensor_getType(ASensor const *s) { (void)s; return -1; }
SOFTFP float ASensor_getResolution(ASensor const *s) { (void)s; return 0; }
int ASensor_getMinDelay(ASensor const *s) { (void)s; return 0; }

// ---- AConfiguration: fixed handheld configuration ------------------------------------------
struct AConfiguration { int32_t orientation, density, sdk, screenSize, touch, keyboard, navigation, uiMode; char lang[2], country[2]; };

static const struct AConfiguration g_default_cfg = {
    2 /* ORIENTATION_LAND */, 160 /* DENSITY_MEDIUM */, 26, 2 /* SCREENSIZE_NORMAL */,
    1 /* TOUCHSCREEN_NOTOUCH */, 1 /* KEYBOARD_NOKEYS */, 2 /* NAVIGATION_DPAD */, 1 /* UI_MODE_TYPE_NORMAL */,
    { 'e', 'n' }, { 'U', 'S' } };

AConfiguration *AConfiguration_new(void)
{
    AConfiguration *c = malloc(sizeof *c);
    if (c) *c = g_default_cfg;
    return c;
}
void AConfiguration_delete(AConfiguration *c) { free(c); }
void AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *am) { (void)am; if (out) *out = g_default_cfg; }
void AConfiguration_copy(AConfiguration *dst, AConfiguration *src) { if (dst && src) *dst = *src; }
int32_t AConfiguration_getOrientation(AConfiguration *c) { return c ? c->orientation : 0; }
int32_t AConfiguration_getDensity(AConfiguration *c) { return c ? c->density : 0; }
int32_t AConfiguration_getSdkVersion(AConfiguration *c) { return c ? c->sdk : 0; }
int32_t AConfiguration_getScreenSize(AConfiguration *c) { return c ? c->screenSize : 0; }
int32_t AConfiguration_getTouchscreen(AConfiguration *c) { return c ? c->touch : 0; }
int32_t AConfiguration_getKeyboard(AConfiguration *c) { return c ? c->keyboard : 0; }
int32_t AConfiguration_getNavigation(AConfiguration *c) { return c ? c->navigation : 0; }
int32_t AConfiguration_getUiModeType(AConfiguration *c) { return c ? c->uiMode : 0; }
void AConfiguration_getLanguage(AConfiguration *c, char *out) { if (c && out) memcpy(out, c->lang, 2); }
void AConfiguration_getCountry(AConfiguration *c, char *out) { if (c && out) memcpy(out, c->country, 2); }
int32_t AConfiguration_getScreenWidthDp(AConfiguration *c)
{ (void)c; return ANativeWindow_getWidth(android_window_get()) * 160 / g_default_cfg.density; }
int32_t AConfiguration_getScreenHeightDp(AConfiguration *c)
{ (void)c; return ANativeWindow_getHeight(android_window_get()) * 160 / g_default_cfg.density; }
