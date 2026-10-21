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
    pthread_t owner;        // the thread that polls it
    int woken;              // ALooper_wake since the last poll (else the eventfd only says the fds changed)
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
    l->owner = pthread_self();
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
    if (!l) return;
    __atomic_store_n(&l->woken, 1, __ATOMIC_SEQ_CST);
    if (l->wake_fd >= 0) (void)!write(l->wake_fd, &one, sizeof one);
}

/* Android's Looper changes its epoll set at once, so an fd another thread adds is watched by the
   poll already waiting. Here the waiting poll() has its own copy of the set: the eventfd makes it
   look again (without a POLL_WAKE: ALooper_pollOnce goes on waiting). Unity 6 adds a handler fd to
   its video looper for every clip from another thread; the second clip's surface request was
   otherwise never seen ("AndroidVideoMedia surface creation stalled"). */
static void fds_changed(ALooper *l)
{
    uint64_t one = 1;
    if (l->wake_fd >= 0 && !pthread_equal(l->owner, pthread_self())) (void)!write(l->wake_fd, &one, sizeof one);
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
    fds_changed(l);
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
    if (found) fds_changed(l);
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
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int n, r, result;
    for (;;) {
        pthread_mutex_lock(&l->lock);
        n = l->nfds;
        memcpy(fds, l->fds, n * sizeof *fds);
        pthread_mutex_unlock(&l->lock);
        pfd[0] = (struct pollfd){ l->wake_fd, POLLIN, 0 };
        for (int i = 0; i < n; i++) {
            short ev = 0;
            if (fds[i].events & ALOOPER_EVENT_INPUT) ev |= POLLIN;
            if (fds[i].events & ALOOPER_EVENT_OUTPUT) ev |= POLLOUT;
            pfd[i + 1] = (struct pollfd){ fds[i].fd, ev, 0 };
        }
        int left = timeoutMillis;
        if (timeoutMillis > 0) {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long el = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            left = el >= timeoutMillis ? 0 : (int)(timeoutMillis - el);
        }
        r = poll(pfd, n + 1, left);
        if (r < 0) return errno == EINTR ? ALOOPER_POLL_WAKE : ALOOPER_POLL_ERROR;
        if (r == 0) return ALOOPER_POLL_TIMEOUT;
        result = ALOOPER_POLL_TIMEOUT;
        if (pfd[0].revents) {
            uint64_t v;
            (void)!read(l->wake_fd, &v, sizeof v);
            if (__atomic_exchange_n(&l->woken, 0, __ATOMIC_SEQ_CST)) result = ALOOPER_POLL_WAKE;
            else if (r == 1) continue;          // only the fd set changed: wait on the new one
        }
        break;
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

// ---- AConfiguration: the handheld's configuration -----------------------------------------
// Landscape, the window's size in dp at 160 dpi (mdpi), no touch screen, no keyboard, a D-pad,
// en-US, left to right, the reported API level (android_shims_set_api_level; 26 when no host set
// one). Every getter and setter of the NDK's configuration.h (API 9-34); diff/match/isBetterThan
// compare the fields that are set.
struct AConfiguration {
    int32_t mcc, mnc, orientation, touch, density, keyboard, navigation, keysHidden, navHidden, sdk,
            screenSize, screenLong, screenRound, uiModeType, uiModeNight, widthDp, heightDp,
            smallestWidthDp, layoutDirection, grammaticalGender;
    char lang[2], country[2];
};

static void default_cfg(struct AConfiguration *c)
{
    int w = ANativeWindow_getWidth(android_window_get()), h = ANativeWindow_getHeight(android_window_get());
    int api = android_shims_api_level();
    memset(c, 0, sizeof *c);
    c->orientation = 2;                         // ORIENTATION_LAND
    c->touch = 1;                               // TOUCHSCREEN_NOTOUCH
    c->density = 160;                           // DENSITY_MEDIUM
    c->keyboard = 1;                            // KEYBOARD_NOKEYS
    c->navigation = 2;                          // NAVIGATION_DPAD
    c->keysHidden = 3;                          // KEYSHIDDEN_SOFT
    c->navHidden = 1;                           // NAVHIDDEN_NO
    c->sdk = api > 0 ? api : 26;
    c->widthDp = w > 0 ? w : 0;                 // px at 160 dpi = dp
    c->heightDp = h > 0 ? h : 0;
    c->smallestWidthDp = c->widthDp < c->heightDp ? c->widthDp : c->heightDp;
    c->screenSize = c->smallestWidthDp >= 720 ? 4 : c->smallestWidthDp >= 600 ? 3 : c->smallestWidthDp >= 320 ? 2 : 1;
    int lo = c->widthDp < c->heightDp ? c->widthDp : c->heightDp, hi = c->widthDp ^ c->heightDp ^ lo;
    c->screenLong = lo && hi * 3 > lo * 5 ? 2 : 1;   // SCREENLONG_YES above 5:3 (Android's rule)
    c->screenRound = 1;                         // SCREENROUND_NO
    c->uiModeType = 1;                          // UI_MODE_TYPE_NORMAL
    c->uiModeNight = 1;                         // UI_MODE_NIGHT_NO
    c->layoutDirection = 1;                     // LAYOUTDIR_LTR
    memcpy(c->lang, "en", 2);
    memcpy(c->country, "US", 2);
}

AConfiguration *AConfiguration_new(void)
{
    AConfiguration *c = calloc(1, sizeof *c);   // Android's new configuration is empty (all 0)
    return c;
}
void AConfiguration_delete(AConfiguration *c) { free(c); }
void AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *am) { (void)am; if (out) default_cfg(out); }
void AConfiguration_copy(AConfiguration *dst, AConfiguration *src) { if (dst && src) *dst = *src; }

#define CFG_FIELD(Name, f) \
    int32_t AConfiguration_get##Name(AConfiguration *c) { return c ? c->f : 0; } \
    void AConfiguration_set##Name(AConfiguration *c, int32_t v) { if (c) c->f = v; }
CFG_FIELD(Mcc, mcc) CFG_FIELD(Mnc, mnc) CFG_FIELD(Orientation, orientation) CFG_FIELD(Touchscreen, touch)
CFG_FIELD(Density, density) CFG_FIELD(Keyboard, keyboard) CFG_FIELD(Navigation, navigation)
CFG_FIELD(KeysHidden, keysHidden) CFG_FIELD(NavHidden, navHidden) CFG_FIELD(SdkVersion, sdk)
CFG_FIELD(ScreenSize, screenSize) CFG_FIELD(ScreenLong, screenLong) CFG_FIELD(ScreenRound, screenRound)
CFG_FIELD(UiModeType, uiModeType) CFG_FIELD(UiModeNight, uiModeNight) CFG_FIELD(ScreenWidthDp, widthDp)
CFG_FIELD(ScreenHeightDp, heightDp) CFG_FIELD(SmallestScreenWidthDp, smallestWidthDp)
CFG_FIELD(LayoutDirection, layoutDirection) CFG_FIELD(GrammaticalGender, grammaticalGender)
void AConfiguration_getLanguage(AConfiguration *c, char *out) { if (c && out) memcpy(out, c->lang, 2); }
void AConfiguration_getCountry(AConfiguration *c, char *out) { if (c && out) memcpy(out, c->country, 2); }
void AConfiguration_setLanguage(AConfiguration *c, const char *v) { if (c) { c->lang[0] = v ? v[0] : 0; c->lang[1] = v && v[0] ? v[1] : 0; } }
void AConfiguration_setCountry(AConfiguration *c, const char *v) { if (c) { c->country[0] = v ? v[0] : 0; c->country[1] = v && v[0] ? v[1] : 0; } }

// diff: ACONFIGURATION_* bits of the fields that differ; match: every field `req` sets is the
// base's (dp sizes and the API level: at most the base's); isBetterThan: `test` matches `requested`
// and sets more of its fields than `base`
#define CFG_EXACT(X) X(mcc, 0x0001) X(mnc, 0x0002) X(touch, 0x0008) X(keyboard, 0x0010) X(keysHidden, 0x0020) \
    X(navHidden, 0x0020) X(navigation, 0x0040) X(orientation, 0x0080) X(density, 0x0100) X(screenSize, 0x0800) \
    X(screenLong, 0x0800) X(uiModeType, 0x1000) X(uiModeNight, 0x1000) X(layoutDirection, 0x4000) \
    X(screenRound, 0x8000) X(grammaticalGender, 0x20000)
#define CFG_ATMOST(X) X(widthDp, 0x0200) X(heightDp, 0x0200) X(sdk, 0x0400) X(smallestWidthDp, 0x2000)
int32_t AConfiguration_diff(AConfiguration *a, AConfiguration *b)
{
    if (!a || !b) return 0;
    int32_t d = 0;
#define X(f, bit) if (a->f != b->f) d |= bit;
    CFG_EXACT(X) CFG_ATMOST(X)
#undef X
    if (memcmp(a->lang, b->lang, 2) || memcmp(a->country, b->country, 2)) d |= 0x0004;   // LOCALE
    return d;
}
int32_t AConfiguration_match(AConfiguration *base, AConfiguration *req)
{
    if (!base || !req) return 0;
#define X(f, bit) if (req->f && req->f != base->f) return 0;
    CFG_EXACT(X)
#undef X
#define X(f, bit) if (req->f && req->f > base->f) return 0;
    CFG_ATMOST(X)
#undef X
    if (req->lang[0] && memcmp(req->lang, base->lang, 2)) return 0;
    if (req->country[0] && memcmp(req->country, base->country, 2)) return 0;
    return 1;
}
static int cfg_fields_set(const AConfiguration *c)
{
    int n = (c->lang[0] != 0) + (c->country[0] != 0);
#define X(f, bit) n += c->f != 0;
    CFG_EXACT(X) CFG_ATMOST(X)
#undef X
    return n;
}
int32_t AConfiguration_isBetterThan(AConfiguration *base, AConfiguration *test, AConfiguration *requested)
{
    if (!base || !test) return 0;
    if (requested && AConfiguration_match(requested, test) != AConfiguration_match(requested, base))
        return AConfiguration_match(requested, test);
    return cfg_fields_set(test) > cfg_fields_set(base);
}
