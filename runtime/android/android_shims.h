// Android API shims for running Unity's Android player on ARM glibc Linux: the armeabi-v7a
// player on armhf, the arm64-v8a player on aarch64.
//
// Covers the Android-only libraries the player imports or dlopens:
//   liblog.so      __android_log_*                         -> stderr
//   libandroid.so  ANativeWindow_*, AAssetManager_*/AAsset*, AConfiguration_*, ALooper_*,
//                  AInputEvent_*/AKeyEvent_*/AMotionEvent_*, ASensor* (no sensors)
//   libOpenSLES.so slCreateEngine + SL_IID_* (engine, output mix, PCM buffer-queue player) -> SDL2
//   libmediandk.so AMediaCodec_*, AMediaExtractor_*, AMediaFormat_*, AImageReader_*, AImage_*,
//                  AMediaDataSource_* failing cleanly (no decoder), AMEDIAFORMAT_KEY_* constants
// EGL translation (ANativeWindow* -> Mali fbdev_window*) lives in egl_glue.h (egl_shims[]).
#ifndef ANDROID_SHIMS_H
#define ANDROID_SHIMS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// so_shim comes from the loader's header when it is there (same layout either way).
#if defined(SO_LOADER_H) || defined(SO_SHIM_DEFINED)
#elif defined(__has_include)
#if __has_include("../loader/so_loader.h")
#include "../loader/so_loader.h"
#endif
#endif
#if !defined(SO_LOADER_H) && !defined(SO_SHIM_DEFINED)
#define SO_SHIM_DEFINED
typedef struct { const char *name; void *fn; } so_shim;
#endif

// ---- shim tables (null-terminated) ---------------------------------------------------------
extern so_shim android_shims[];       // everything below (liblog + libandroid + libOpenSLES + libmediandk)
extern so_shim android_log_shims[];   // liblog.so only
extern so_shim android_ndk_shims[];   // libandroid.so only
extern so_shim android_sles_shims[];  // libOpenSLES.so only (SL_IID_* entries point at the variables)
extern so_shim android_mediandk_shims[]; // libmediandk.so only (AMEDIAFORMAT_KEY_* entries: the variables)

// For the loader's dlopen routing: the table for an Android-only library ("liblog.so",
// "/system/lib/libOpenSLES.so", "libmediandk.so", ...), or NULL when we have no shim for it
// (libvulkan.so, OMX libs: let dlopen fail, Unity then disables that feature).
const so_shim *android_shims_for_library(const char *path);
// Lookup by symbol name over android_shims[] (NULL if absent).
void *android_shims_lookup(const char *name);
// The API level the game is told (Build.VERSION.SDK_INT): libandroid functions Android added later
// read NULL in the tables (dlsym fails, weak imports are 0), as on a phone of that level, and
// AConfiguration reports it. 0 (the default): every function present. Call before the loader's
// first symbol lookup (it indexes the tables then).
void android_shims_set_api_level(int api);
int android_shims_api_level(void);

// The vsync clock (choreographer.c): vsyncs at the multiples of 1e9/hz ns on CLOCK_MONOTONIC
// (default 60 Hz), shared with runtime/jni's Java Choreographer. AChoreographer frame callbacks run
// at the first vsync after they are due, with it as their frame time.
void android_vsync_set_refresh_rate(float hz);
int64_t android_vsync_period_ns(void);
int64_t android_vsync_after(int64_t t_ns);       // the first vsync strictly after t
// A host's own looper threads (runtime/jni's Java loopers): current() names the calling thread's
// looper (NULL if it is none), post() runs fn(arg) on that looper after delay_ms. An AChoreographer
// made on such a thread runs its callbacks as posted jobs; elsewhere it needs an ALooper.
void android_set_host_looper(void *(*current)(void),
                             void (*post)(void *looper, void (*fn)(void *), void *arg, int64_t delay_ms));

// ---- host configuration --------------------------------------------------------------------
// Logging: minimum priority printed (ANDROID_LOG_VERBOSE=2 .. ANDROID_LOG_FATAL=7).
// Default from env ANDROID_LOG_LEVEL (V/D/I/W/E/F or a number), else DEBUG.
void android_log_set_min_priority(int prio);

// Assets: directory that AAssetManager_* reads ("assets/" of the APK, unpacked).
// Default from env ANDROID_ASSET_DIR, else "./assets".
void android_set_asset_dir(const char *dir);

// The one native window. Its size defaults to the /dev/fb0 visible size (FBIOGET_VSCREENINFO);
// env ANDROID_WINDOW_SIZE=WxH overrides; if both fail, 720x480.
typedef struct ANativeWindow ANativeWindow;
ANativeWindow *android_window_get(void);                 // creates on first use, not ref-counted
void android_window_set_size(int width, int height);     // physical window size
// The largest buffer geometry setBuffersGeometry may set (0: any): a larger request is cut to it,
// side by side (Mali's fbdev window must fit the framebuffer's screen; the host's fbdev path).
void android_window_set_max_size(int width, int height);
// Optional: a host-provided EGLNativeWindowType (X11 Window, SDL-created fbdev window, ...)
// handed to the real eglCreateWindowSurface instead of the built-in fbdev_window.
void android_window_set_native(void *native_window);
// What egl_glue passes to the real eglCreateWindowSurface for an ANativeWindow* (NULL if `w` is
// not one of ours). Updates the fbdev_window size to the current buffer geometry.
void *android_window_native_handle(ANativeWindow *w);
int android_window_is_ours(const void *p);

// ---- NDK types (opaque outside the shims) --------------------------------------------------
// Bionic's off_t is a long (32 bits on ARMv7, 64 on AArch64), off64_t a long long. Spelled out so
// a host built with _FILE_OFFSET_BITS=64 still sees the right prototypes.
typedef long a_off_t;
typedef long long a_off64_t;
typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
typedef struct AAssetDir AAssetDir;
typedef struct AConfiguration AConfiguration;
typedef struct ALooper ALooper;
typedef struct AInputEvent AInputEvent;
typedef struct AInputQueue AInputQueue;
typedef struct ASensorManager ASensorManager;
typedef struct ASensor ASensor;
typedef struct ASensorEventQueue ASensorEventQueue;
typedef struct ASensorEvent ASensorEvent;
typedef ASensor const *ASensorRef;
typedef ASensorRef const *ASensorList;
typedef int (*ALooper_callbackFunc)(int fd, int events, void *data);

typedef struct ARect { int32_t left, top, right, bottom; } ARect;
typedef struct ANativeWindow_Buffer {
    int32_t width, height, stride, format; void *bits; uint32_t reserved[6];
} ANativeWindow_Buffer;

enum { WINDOW_FORMAT_RGBA_8888 = 1, WINDOW_FORMAT_RGBX_8888 = 2, WINDOW_FORMAT_RGB_565 = 4 };
enum { AASSET_MODE_UNKNOWN = 0, AASSET_MODE_RANDOM = 1, AASSET_MODE_STREAMING = 2, AASSET_MODE_BUFFER = 3 };
enum { ALOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1 };
enum { ALOOPER_POLL_WAKE = -1, ALOOPER_POLL_CALLBACK = -2, ALOOPER_POLL_TIMEOUT = -3, ALOOPER_POLL_ERROR = -4 };
enum { ALOOPER_EVENT_INPUT = 1, ALOOPER_EVENT_OUTPUT = 2, ALOOPER_EVENT_ERROR = 4, ALOOPER_EVENT_HANGUP = 8 };
enum { AINPUT_EVENT_TYPE_KEY = 1, AINPUT_EVENT_TYPE_MOTION = 2 };
enum { AKEY_EVENT_ACTION_DOWN = 0, AKEY_EVENT_ACTION_UP = 1, AKEY_EVENT_ACTION_MULTIPLE = 2 };

// liblog
int __android_log_write(int prio, const char *tag, const char *text);
int __android_log_print(int prio, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap);
void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...) __attribute__((noreturn));
int __android_log_buf_write(int bufID, int prio, const char *tag, const char *text);
int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, ...);

// ANativeWindow
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface);
void ANativeWindow_acquire(ANativeWindow *w);
void ANativeWindow_release(ANativeWindow *w);
int32_t ANativeWindow_getWidth(ANativeWindow *w);
int32_t ANativeWindow_getHeight(ANativeWindow *w);
int32_t ANativeWindow_getFormat(ANativeWindow *w);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *out, ARect *inOutDirty);
int32_t ANativeWindow_unlockAndPost(ANativeWindow *w);

// Assets
AAssetManager *AAssetManager_fromJava(void *env, void *assetManager);
AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode);
AAssetDir *AAssetManager_openDir(AAssetManager *mgr, const char *dirName);
const char *AAssetDir_getNextFileName(AAssetDir *dir);
void AAssetDir_rewind(AAssetDir *dir);
void AAssetDir_close(AAssetDir *dir);
int AAsset_read(AAsset *a, void *buf, size_t count);
a_off_t AAsset_seek(AAsset *a, a_off_t offset, int whence);
a_off64_t AAsset_seek64(AAsset *a, a_off64_t offset, int whence);
void AAsset_close(AAsset *a);
const void *AAsset_getBuffer(AAsset *a);
a_off_t AAsset_getLength(AAsset *a);
a_off64_t AAsset_getLength64(AAsset *a);
a_off_t AAsset_getRemainingLength(AAsset *a);
a_off64_t AAsset_getRemainingLength64(AAsset *a);
int AAsset_openFileDescriptor(AAsset *a, a_off_t *outStart, a_off_t *outLength);
int AAsset_openFileDescriptor64(AAsset *a, a_off64_t *outStart, a_off64_t *outLength);
int AAsset_isAllocated(AAsset *a);

// ALooper
ALooper *ALooper_forThread(void);
ALooper *ALooper_prepare(int opts);
void ALooper_acquire(ALooper *l);
void ALooper_release(ALooper *l);
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData);
int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData);
void ALooper_wake(ALooper *l);
int ALooper_addFd(ALooper *l, int fd, int ident, int events, ALooper_callbackFunc cb, void *data);
int ALooper_removeFd(ALooper *l, int fd);

// Input events (the player imports these but never calls them; a host may still build events)
AInputEvent *android_input_event_new_key(int32_t deviceId, int32_t action, int32_t keyCode, int32_t metaState);
void android_input_event_free(AInputEvent *e);
int32_t AInputEvent_getType(const AInputEvent *e);
int32_t AInputEvent_getDeviceId(const AInputEvent *e);
int32_t AInputEvent_getSource(const AInputEvent *e);
int32_t AKeyEvent_getAction(const AInputEvent *e);
int32_t AKeyEvent_getKeyCode(const AInputEvent *e);
int32_t AKeyEvent_getMetaState(const AInputEvent *e);

// Sensors (none)
ASensorManager *ASensorManager_getInstance(void);
int ASensorManager_getSensorList(ASensorManager *m, ASensorList *list);
ASensor const *ASensorManager_getDefaultSensor(ASensorManager *m, int type);
ASensorEventQueue *ASensorManager_createEventQueue(ASensorManager *m, ALooper *l, int ident, ALooper_callbackFunc cb, void *data);
int ASensorManager_destroyEventQueue(ASensorManager *m, ASensorEventQueue *q);
int ASensorEventQueue_enableSensor(ASensorEventQueue *q, ASensor const *s);
int ASensorEventQueue_disableSensor(ASensorEventQueue *q, ASensor const *s);
int ASensorEventQueue_setEventRate(ASensorEventQueue *q, ASensor const *s, int32_t usec);
int ASensorEventQueue_hasEvents(ASensorEventQueue *q);
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue *q, ASensorEvent *events, size_t count);

// Choreographer (API 24+; 64-bit callbacks 29, refresh rate callbacks 30, vsync callbacks 33)
typedef struct AChoreographer AChoreographer;
typedef struct AChoreographerFrameCallbackData AChoreographerFrameCallbackData;
typedef void (*AChoreographer_frameCallback)(long frameTimeNanos, void *data);
typedef void (*AChoreographer_frameCallback64)(int64_t frameTimeNanos, void *data);
typedef void (*AChoreographer_vsyncCallback)(const AChoreographerFrameCallbackData *callbackData, void *data);
typedef void (*AChoreographer_refreshRateCallback)(int64_t vsyncPeriodNanos, void *data);
AChoreographer *AChoreographer_getInstance(void);
void AChoreographer_postFrameCallback(AChoreographer *c, AChoreographer_frameCallback cb, void *data);
void AChoreographer_postFrameCallbackDelayed(AChoreographer *c, AChoreographer_frameCallback cb, void *data, long delayMillis);
void AChoreographer_postFrameCallback64(AChoreographer *c, AChoreographer_frameCallback64 cb, void *data);
void AChoreographer_postFrameCallbackDelayed64(AChoreographer *c, AChoreographer_frameCallback64 cb, void *data, uint32_t delayMillis);
int AChoreographer_postVsyncCallback(AChoreographer *c, AChoreographer_vsyncCallback cb, void *data);
void AChoreographer_registerRefreshRateCallback(AChoreographer *c, AChoreographer_refreshRateCallback cb, void *data);
void AChoreographer_unregisterRefreshRateCallback(AChoreographer *c, AChoreographer_refreshRateCallback cb, void *data);
int64_t AChoreographerFrameCallbackData_getFrameTimeNanos(const AChoreographerFrameCallbackData *d);
size_t AChoreographerFrameCallbackData_getFrameTimelinesLength(const AChoreographerFrameCallbackData *d);
size_t AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(const AChoreographerFrameCallbackData *d);
int64_t AChoreographerFrameCallbackData_getFrameTimelineVsyncId(const AChoreographerFrameCallbackData *d, size_t i);
int64_t AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(const AChoreographerFrameCallbackData *d, size_t i);
int64_t AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(const AChoreographerFrameCallbackData *d, size_t i);

// Configuration (landscape, the window in dp at 160 dpi, no touch, D-pad; every NDK getter/setter)
AConfiguration *AConfiguration_new(void);
void AConfiguration_delete(AConfiguration *c);
void AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *am);
int32_t AConfiguration_getOrientation(AConfiguration *c);
int32_t AConfiguration_getDensity(AConfiguration *c);
int32_t AConfiguration_getSdkVersion(AConfiguration *c);
int32_t AConfiguration_getTouchscreen(AConfiguration *c);
int32_t AConfiguration_getNavigation(AConfiguration *c);
int32_t AConfiguration_getScreenWidthDp(AConfiguration *c);
int32_t AConfiguration_getScreenHeightDp(AConfiguration *c);
int32_t AConfiguration_getLayoutDirection(AConfiguration *c);
void AConfiguration_copy(AConfiguration *dst, AConfiguration *src);
void AConfiguration_setOrientation(AConfiguration *c, int32_t v);
void AConfiguration_setSdkVersion(AConfiguration *c, int32_t v);
void AConfiguration_setLanguage(AConfiguration *c, const char *v);
int32_t AConfiguration_diff(AConfiguration *a, AConfiguration *b);
int32_t AConfiguration_match(AConfiguration *base, AConfiguration *requested);

#ifdef __cplusplus
}
#endif
#endif
