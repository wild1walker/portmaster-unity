// The so_shim tables. Functions are declared here only to take their addresses (their real
// prototypes live in android_shims.h / sles_min.h); SL_IID_* entries point at the variables,
// since dlsym on a data symbol returns its address.
#include <stddef.h>
#include <string.h>

// Only so_shim is needed from the header; the function prototypes are re-declared below.
#define ANDROID_SHIMS_H
#if defined(__has_include)
#if __has_include("../loader/so_loader.h")
#include "../loader/so_loader.h"
#endif
#endif
#if !defined(SO_LOADER_H)
typedef struct { const char *name; void *fn; } so_shim;
#endif

#define LOG_FUNCS(X) \
    X(__android_log_write) X(__android_log_print) X(__android_log_vprint) X(__android_log_assert) \
    X(__android_log_buf_write) X(__android_log_buf_print) X(__android_log_is_loggable) \
    X(__android_log_set_minimum_priority) X(__android_log_get_minimum_priority)

#define NDK_FUNCS(X) \
    X(ANativeWindow_fromSurface) X(ANativeWindow_acquire) X(ANativeWindow_release) \
    X(ANativeWindow_getWidth) X(ANativeWindow_getHeight) X(ANativeWindow_getFormat) \
    X(ANativeWindow_setBuffersGeometry) X(ANativeWindow_lock) X(ANativeWindow_unlockAndPost) \
    X(AAssetManager_fromJava) X(AAssetManager_open) X(AAssetManager_openDir) \
    X(AAssetDir_getNextFileName) X(AAssetDir_rewind) X(AAssetDir_close) \
    X(AAsset_read) X(AAsset_seek) X(AAsset_seek64) X(AAsset_close) X(AAsset_getBuffer) \
    X(AAsset_getLength) X(AAsset_getLength64) X(AAsset_getRemainingLength) \
    X(AAsset_getRemainingLength64) X(AAsset_openFileDescriptor) X(AAsset_openFileDescriptor64) \
    X(AAsset_isAllocated) \
    X(ALooper_forThread) X(ALooper_prepare) X(ALooper_acquire) X(ALooper_release) \
    X(ALooper_pollOnce) X(ALooper_pollAll) X(ALooper_wake) X(ALooper_addFd) X(ALooper_removeFd) \
    X(AInputEvent_getType) X(AInputEvent_getDeviceId) X(AInputEvent_getSource) \
    X(AKeyEvent_getAction) X(AKeyEvent_getKeyCode) X(AKeyEvent_getMetaState) X(AKeyEvent_getFlags) \
    X(AKeyEvent_getRepeatCount) X(AKeyEvent_getScanCode) X(AKeyEvent_getDownTime) X(AKeyEvent_getEventTime) \
    X(AMotionEvent_getAction) X(AMotionEvent_getPointerCount) X(AMotionEvent_getPointerId) \
    X(AMotionEvent_getX) X(AMotionEvent_getY) X(AMotionEvent_getAxisValue) \
    X(AInputQueue_getEvent) X(AInputQueue_hasEvents) X(AInputQueue_preDispatchEvent) \
    X(AInputQueue_finishEvent) X(AInputQueue_attachLooper) X(AInputQueue_detachLooper) \
    X(ASensorManager_getInstance) X(ASensorManager_getInstanceForPackage) X(ASensorManager_getSensorList) \
    X(ASensorManager_getDefaultSensor) X(ASensorManager_getDefaultSensorEx) \
    X(ASensorManager_createEventQueue) X(ASensorManager_destroyEventQueue) \
    X(ASensorEventQueue_enableSensor) X(ASensorEventQueue_disableSensor) X(ASensorEventQueue_setEventRate) \
    X(ASensorEventQueue_hasEvents) X(ASensorEventQueue_getEvents) \
    X(ASensor_getName) X(ASensor_getVendor) X(ASensor_getType) X(ASensor_getResolution) X(ASensor_getMinDelay) \
    X(AConfiguration_new) X(AConfiguration_delete) X(AConfiguration_fromAssetManager) X(AConfiguration_copy) \
    X(AConfiguration_getOrientation) X(AConfiguration_getDensity) X(AConfiguration_getSdkVersion) \
    X(AConfiguration_getScreenSize) X(AConfiguration_getTouchscreen) X(AConfiguration_getKeyboard) \
    X(AConfiguration_getNavigation) X(AConfiguration_getUiModeType) X(AConfiguration_getLanguage) \
    X(AConfiguration_getCountry) X(AConfiguration_getScreenWidthDp) X(AConfiguration_getScreenHeightDp)

#define SLES_FUNCS(X) X(slCreateEngine) X(slQueryNumSupportedEngineInterfaces) X(slQuerySupportedEngineInterfaces)

#define SLES_IIDS(X) \
    X(SL_IID_NULL) X(SL_IID_OBJECT) X(SL_IID_ENGINE) X(SL_IID_ENGINECAPABILITIES) X(SL_IID_OUTPUTMIX) \
    X(SL_IID_PLAY) X(SL_IID_RECORD) X(SL_IID_VOLUME) X(SL_IID_BUFFERQUEUE) X(SL_IID_ANDROIDSIMPLEBUFFERQUEUE) \
    X(SL_IID_ANDROIDCONFIGURATION) X(SL_IID_EFFECTSEND) X(SL_IID_ANDROIDEFFECTSEND) X(SL_IID_MUTESOLO) \
    X(SL_IID_PLAYBACKRATE) X(SL_IID_SEEK) X(SL_IID_PREFETCHSTATUS) X(SL_IID_METADATAEXTRACTION) \
    X(SL_IID_ANDROIDEFFECT) X(SL_IID_ANDROIDBUFFERQUEUESOURCE)

#define DECL_FN(n) extern void n(void);
#define DECL_VAR(n) extern const void *const n;
LOG_FUNCS(DECL_FN)
NDK_FUNCS(DECL_FN)
SLES_FUNCS(DECL_FN)
SLES_IIDS(DECL_VAR)

#define ENT_FN(n) { #n, (void *)n },
#define ENT_VAR(n) { #n, (void *)&n },

so_shim android_log_shims[] = { LOG_FUNCS(ENT_FN) { NULL, NULL } };
so_shim android_ndk_shims[] = { NDK_FUNCS(ENT_FN) { NULL, NULL } };
so_shim android_sles_shims[] = { SLES_FUNCS(ENT_FN) SLES_IIDS(ENT_VAR) { NULL, NULL } };
so_shim android_shims[] = {
    LOG_FUNCS(ENT_FN) NDK_FUNCS(ENT_FN) SLES_FUNCS(ENT_FN) SLES_IIDS(ENT_VAR) { NULL, NULL }
};

const so_shim *android_shims_for_library(const char *path)
{
    if (!path) return NULL;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!strcmp(base, "liblog.so")) return android_log_shims;
    if (!strcmp(base, "libandroid.so")) return android_ndk_shims;
    if (!strcmp(base, "libOpenSLES.so")) return android_sles_shims;
    return NULL;
}

void *android_shims_lookup(const char *name)
{
    if (!name) return NULL;
    for (const so_shim *s = android_shims; s->name; s++)
        if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}
