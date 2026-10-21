// libandroid.so APIs of Android 7-15 (API 24-35) that players and plugins look up or import weakly,
// answered as a handheld without the services behind them would:
//   ANativeWindow_setFrameRate*   (30, 31, 34)  accepted (0); the panel keeps its one mode
//   ATrace_*                      (23, 29)      tracing never enabled
//   APerformanceHint_*, AWorkDuration_* (33-35) a manager without hint sessions (no ADPF HAL)
//   AThermal_*                    (30-35)       status NONE, headroom unknown (NaN)
//   AHardwareBuffer_*             (26-31)       no gralloc: allocation fails, no buffer exists
// Which of them a game sees is set by the reported API level (shim_tables.c, android_shims_set_api_level).
#define _GNU_SOURCE
#include "android_shims.h"
#include "shim_internal.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

// ---- ANativeWindow frame rate --------------------------------------------------------------
// Unity 2021.3+ and Swappy look ANativeWindow_setFrameRate up and call it with the target frame
// rate; the host's display has one mode, so the hint changes nothing (a phone with one mode
// accepts it the same way). frameRate is a float: SOFTFP on armhf.
SOFTFP int32_t ANativeWindow_setFrameRate(ANativeWindow *w, float frameRate, int8_t compatibility)
{
    if (!w || frameRate < 0 || isnan(frameRate)) return -EINVAL;
    SHIM_TRACE("ANativeWindow_setFrameRate(%.2f, %d)", (double)frameRate, compatibility);
    return 0;
}
SOFTFP int32_t ANativeWindow_setFrameRateWithChangeStrategy(ANativeWindow *w, float frameRate, int8_t compatibility, int8_t strategy)
{
    (void)strategy;
    return ANativeWindow_setFrameRate(w, frameRate, compatibility);
}
int32_t ANativeWindow_clearFrameRate(ANativeWindow *w) { return w ? 0 : -EINVAL; }

// ---- ATrace (API 23; async sections and counters 29) ---------------------------------------
bool ATrace_isEnabled(void) { return false; }
void ATrace_beginSection(const char *name) { (void)name; }
void ATrace_endSection(void) {}
void ATrace_beginAsyncSection(const char *name, int32_t cookie) { (void)name; (void)cookie; }
void ATrace_endAsyncSection(const char *name, int32_t cookie) { (void)name; (void)cookie; }
void ATrace_setCounter(const char *name, int64_t value) { (void)name; (void)value; }

// ---- APerformanceHint (API 33; setThreads 34; power efficiency, AWorkDuration 35) ----------
// A manager exists, but no session can be made: createSession answers NULL ("not supported") and
// the preferred update rate is -1, as on a device without the power HAL's hint support.
typedef struct APerformanceHintManager { int unused; } APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef struct AWorkDuration { int64_t start, total, cpu, gpu; } AWorkDuration;
static APerformanceHintManager g_hint_mgr;

APerformanceHintManager *APerformanceHint_getManager(void) { return &g_hint_mgr; }
APerformanceHintSession *APerformanceHint_createSession(APerformanceHintManager *m, const int32_t *tids, size_t n,
                                                         int64_t initialTargetWorkDurationNanos)
{
    (void)m; (void)tids; (void)n; (void)initialTargetWorkDurationNanos;
    SHIM_TRACE("APerformanceHint_createSession: not supported");
    return NULL;
}
int64_t APerformanceHint_getPreferredUpdateRateNanos(APerformanceHintManager *m) { (void)m; return -1; }
int APerformanceHint_updateTargetWorkDuration(APerformanceHintSession *s, int64_t ns) { (void)ns; return s ? 0 : EINVAL; }
int APerformanceHint_reportActualWorkDuration(APerformanceHintSession *s, int64_t ns) { (void)ns; return s ? 0 : EINVAL; }
void APerformanceHint_closeSession(APerformanceHintSession *s) { (void)s; }
int APerformanceHint_setThreads(APerformanceHintSession *s, const pid_t *tids, size_t n) { (void)tids; (void)n; return s ? 0 : EINVAL; }
int APerformanceHint_setPreferPowerEfficiency(APerformanceHintSession *s, bool on) { (void)on; return s ? 0 : EINVAL; }
int APerformanceHint_reportActualWorkDuration2(APerformanceHintSession *s, AWorkDuration *d) { (void)d; return s ? 0 : EINVAL; }
AWorkDuration *AWorkDuration_create(void) { return calloc(1, sizeof(AWorkDuration)); }
void AWorkDuration_release(AWorkDuration *d) { free(d); }
void AWorkDuration_setWorkPeriodStartTimestampNanos(AWorkDuration *d, int64_t v) { if (d) d->start = v; }
void AWorkDuration_setActualTotalDurationNanos(AWorkDuration *d, int64_t v) { if (d) d->total = v; }
void AWorkDuration_setActualCpuDurationNanos(AWorkDuration *d, int64_t v) { if (d) d->cpu = v; }
void AWorkDuration_setActualGpuDurationNanos(AWorkDuration *d, int64_t v) { if (d) d->gpu = v; }

// ---- AThermal (API 30; headroom 31; thresholds 35) -----------------------------------------
typedef struct AThermalManager { int unused; } AThermalManager;
typedef void (*AThermal_StatusCallback)(void *data, int status);
static AThermalManager g_thermal;

AThermalManager *AThermal_acquireManager(void) { return &g_thermal; }
void AThermal_releaseManager(AThermalManager *m) { (void)m; }
int AThermal_getCurrentThermalStatus(AThermalManager *m) { (void)m; return 0; }   // ATHERMAL_STATUS_NONE
// the status never changes, so a listener is kept by nobody and never called
int AThermal_registerThermalStatusListener(AThermalManager *m, AThermal_StatusCallback cb, void *data)
{ (void)data; return m && cb ? 0 : EINVAL; }
int AThermal_unregisterThermalStatusListener(AThermalManager *m, AThermal_StatusCallback cb, void *data)
{ (void)data; return m && cb ? 0 : EINVAL; }
// NaN: "not supported by the device" (Android's own answer without a thermal HAL)
SOFTFP float AThermal_getThermalHeadroom(AThermalManager *m, int forecastSeconds) { (void)m; (void)forecastSeconds; return NAN; }
int AThermal_getThermalHeadroomThresholds(AThermalManager *m, const void **outThresholds, size_t *size)
{
    (void)m;
    if (outThresholds) *outThresholds = NULL;
    if (size) *size = 0;
    return ENOSYS;
}

// ---- AHardwareBuffer (API 26; isSupported 29, getId 31) ------------------------------------
// 2022.3+ import acquire/describe/release weakly for the Vulkan video path (AImageReader, which
// fails first here). There is no gralloc: no buffer is ever made, so these only see NULL.
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AHardwareBuffer_Desc { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out)
{
    (void)desc;
    if (out) *out = NULL;
    SHIM_TRACE("AHardwareBuffer_allocate: no hardware buffers");
    return -EINVAL;
}
void AHardwareBuffer_acquire(AHardwareBuffer *b) { (void)b; }
void AHardwareBuffer_release(AHardwareBuffer *b) { (void)b; }
void AHardwareBuffer_describe(const AHardwareBuffer *b, AHardwareBuffer_Desc *out) { (void)b; if (out) memset(out, 0, sizeof *out); }
int AHardwareBuffer_lock(AHardwareBuffer *b, uint64_t usage, int32_t fence, const ARect *rect, void **out)
{
    (void)b; (void)usage; (void)rect;
    if (fence >= 0) close(fence);
    if (out) *out = NULL;
    return -EINVAL;
}
int AHardwareBuffer_unlock(AHardwareBuffer *b, int32_t *fence) { (void)b; if (fence) *fence = -1; return -EINVAL; }
int AHardwareBuffer_isSupported(const AHardwareBuffer_Desc *desc) { (void)desc; return 0; }
int AHardwareBuffer_getId(const AHardwareBuffer *b, uint64_t *out) { (void)b; if (out) *out = 0; return -EINVAL; }
