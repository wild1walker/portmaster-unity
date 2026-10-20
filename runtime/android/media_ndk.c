// libmediandk.so stand-in: the NDK media API (AMediaCodec, AMediaExtractor, AMediaFormat, AImageReader,
// AImage, AMediaDataSource) with no decoder behind it. Unity 2022.3's libunity links against
// libmediandk (DT_NEEDED; its VideoPlayer moved from Java's MediaCodec to the NDK), so the library
// must resolve for libunity to load; older players dlopen it and fall back to Java when that fails.
// Every call fails the way the NDK reports failure: constructors return NULL, status calls
// AMEDIA_ERROR_UNSUPPORTED, getters false / -1 / 0, out-pointers are cleared, NULL objects are
// accepted everywhere. A VideoPlayer then reports an error and the video is skipped; nothing else
// in the player depends on it. The AMEDIAFORMAT_KEY_* constants are data objects (const char *
// variables) with the NDK's values, as libunity reads them through GOT entries.
//
// Declarations follow the NDK headers (NdkMediaCodec.h, NdkMediaExtractor.h, NdkMediaFormat.h,
// NdkMediaError.h, NdkImageReader.h, NdkImage.h, NdkMediaDataSource.h); only scalars and pointers
// cross the ABI, except AMediaFormat_setFloat (softfp on 32-bit ARM).
#include "android_shims.h"
#include "shim_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef int32_t media_status_t;
enum {
    AMEDIA_OK = 0,
    AMEDIA_ERROR_UNKNOWN = -10000,
    AMEDIA_ERROR_UNSUPPORTED = -10002,
    AMEDIA_ERROR_INVALID_OBJECT = -10003,
};

static media_status_t unsupported(const char *what)
{
    SHIM_TRACE("%s: no media decoder", what);
    return AMEDIA_ERROR_UNSUPPORTED;
}

typedef struct AMediaCodec AMediaCodec;
typedef struct AMediaExtractor AMediaExtractor;
typedef struct AMediaFormat AMediaFormat;
typedef struct AMediaCrypto AMediaCrypto;
typedef struct AMediaCodecBufferInfo AMediaCodecBufferInfo;
typedef struct AMediaDataSource AMediaDataSource;
typedef struct AImageReader AImageReader;
typedef struct AImage AImage;
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AImageReader_ImageListener AImageReader_ImageListener;
typedef struct AImageReader_BufferRemovedListener AImageReader_BufferRemovedListener;

// ---- AMEDIAFORMAT_KEY_* (NdkMediaFormat.cpp; the same strings as android.media.MediaFormat's KEY_*).
// Listed in shim_tables.c (MEDIANDK_KEYS) too; the first group is what libunity 2022.3 imports.
const char *AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char *AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char *AMEDIAFORMAT_KEY_DURATION = "durationUs";
const char *AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
const char *AMEDIAFORMAT_KEY_HEIGHT = "height";
const char *AMEDIAFORMAT_KEY_LANGUAGE = "language";
const char *AMEDIAFORMAT_KEY_MIME = "mime";
const char *AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
const char *AMEDIAFORMAT_KEY_STRIDE = "stride";
const char *AMEDIAFORMAT_KEY_WIDTH = "width";
const char *AMEDIAFORMAT_KEY_COLOR_RANGE = "color-range";               // weak imports from here on
const char *AMEDIAFORMAT_KEY_COLOR_STANDARD = "color-standard";
const char *AMEDIAFORMAT_KEY_ENCODER_DELAY = "encoder-delay";
const char *AMEDIAFORMAT_KEY_ROTATION = "rotation-degrees";
const char *AMEDIAFORMAT_KEY_SLICE_HEIGHT = "slice-height";
const char *AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";                     // others of the same family
const char *AMEDIAFORMAT_KEY_CHANNEL_MASK = "channel-mask";
const char *AMEDIAFORMAT_KEY_COLOR_TRANSFER = "color-transfer";
const char *AMEDIAFORMAT_KEY_ENCODER_PADDING = "encoder-padding";
const char *AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
const char *AMEDIAFORMAT_KEY_IS_ADTS = "is-adts";
const char *AMEDIAFORMAT_KEY_MAX_HEIGHT = "max-height";
const char *AMEDIAFORMAT_KEY_MAX_INPUT_SIZE = "max-input-size";
const char *AMEDIAFORMAT_KEY_MAX_WIDTH = "max-width";
const char *AMEDIAFORMAT_KEY_PCM_ENCODING = "pcm-encoding";
const char *AMEDIAFORMAT_KEY_TIME_US = "timeUs";
const char *AMEDIAFORMAT_KEY_TRACK_ID = "track-id";

// ---- AMediaFormat -------------------------------------------------------------------------------------
AMediaFormat *AMediaFormat_new(void) { SHIM_TRACE("AMediaFormat_new: no media decoder"); return NULL; }
media_status_t AMediaFormat_delete(AMediaFormat *f) { (void)f; return AMEDIA_OK; }
const char *AMediaFormat_toString(AMediaFormat *f) { (void)f; return ""; }
bool AMediaFormat_getInt32(AMediaFormat *f, const char *name, int32_t *out) { (void)f; (void)name; (void)out; return false; }
bool AMediaFormat_getInt64(AMediaFormat *f, const char *name, int64_t *out) { (void)f; (void)name; (void)out; return false; }
bool AMediaFormat_getFloat(AMediaFormat *f, const char *name, float *out) { (void)f; (void)name; (void)out; return false; }
bool AMediaFormat_getSize(AMediaFormat *f, const char *name, size_t *out) { (void)f; (void)name; (void)out; return false; }
bool AMediaFormat_getBuffer(AMediaFormat *f, const char *name, void **data, size_t *size)
{
    (void)f; (void)name; (void)data; (void)size;
    return false;
}
bool AMediaFormat_getString(AMediaFormat *f, const char *name, const char **out) { (void)f; (void)name; (void)out; return false; }
void AMediaFormat_setInt32(AMediaFormat *f, const char *name, int32_t v) { (void)f; (void)name; (void)v; }
void AMediaFormat_setInt64(AMediaFormat *f, const char *name, int64_t v) { (void)f; (void)name; (void)v; }
SOFTFP void AMediaFormat_setFloat(AMediaFormat *f, const char *name, float v) { (void)f; (void)name; (void)v; }
void AMediaFormat_setString(AMediaFormat *f, const char *name, const char *v) { (void)f; (void)name; (void)v; }
void AMediaFormat_setBuffer(AMediaFormat *f, const char *name, const void *data, size_t size)
{
    (void)f; (void)name; (void)data; (void)size;
}

// ---- AMediaExtractor ----------------------------------------------------------------------------------
AMediaExtractor *AMediaExtractor_new(void) { SHIM_TRACE("AMediaExtractor_new: no media decoder"); return NULL; }
media_status_t AMediaExtractor_delete(AMediaExtractor *e) { (void)e; return AMEDIA_OK; }
media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor *e, int fd, a_off64_t offset, a_off64_t length)
{
    (void)e; (void)fd; (void)offset; (void)length;
    return unsupported("AMediaExtractor_setDataSourceFd");
}
media_status_t AMediaExtractor_setDataSource(AMediaExtractor *e, const char *location)
{
    (void)e; (void)location;
    return unsupported("AMediaExtractor_setDataSource");
}
media_status_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *e, AMediaDataSource *src)
{
    (void)e; (void)src;
    return unsupported("AMediaExtractor_setDataSourceCustom");
}
size_t AMediaExtractor_getTrackCount(AMediaExtractor *e) { (void)e; return 0; }
AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *e, size_t idx) { (void)e; (void)idx; return NULL; }
media_status_t AMediaExtractor_selectTrack(AMediaExtractor *e, size_t idx) { (void)e; (void)idx; return AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AMediaExtractor_unselectTrack(AMediaExtractor *e, size_t idx) { (void)e; (void)idx; return AMEDIA_ERROR_UNSUPPORTED; }
ssize_t AMediaExtractor_readSampleData(AMediaExtractor *e, uint8_t *buf, size_t cap) { (void)e; (void)buf; (void)cap; return -1; }
uint32_t AMediaExtractor_getSampleFlags(AMediaExtractor *e) { (void)e; return 0; }
int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *e) { (void)e; return -1; }
int64_t AMediaExtractor_getSampleTime(AMediaExtractor *e) { (void)e; return -1; }
bool AMediaExtractor_advance(AMediaExtractor *e) { (void)e; return false; }
media_status_t AMediaExtractor_seekTo(AMediaExtractor *e, int64_t us, int mode) { (void)e; (void)us; (void)mode; return AMEDIA_ERROR_UNSUPPORTED; }
AMediaFormat *AMediaExtractor_getFileFormat(AMediaExtractor *e) { (void)e; return NULL; }

// ---- AMediaCodec --------------------------------------------------------------------------------------
AMediaCodec *AMediaCodec_createDecoderByType(const char *mime)
{
    SHIM_TRACE("AMediaCodec_createDecoderByType(%s): no media decoder", mime ? mime : "(null)");
    return NULL;
}
AMediaCodec *AMediaCodec_createEncoderByType(const char *mime) { (void)mime; return NULL; }
AMediaCodec *AMediaCodec_createCodecByName(const char *name) { (void)name; return NULL; }
media_status_t AMediaCodec_delete(AMediaCodec *c) { (void)c; return AMEDIA_OK; }
media_status_t AMediaCodec_configure(AMediaCodec *c, const AMediaFormat *f, ANativeWindow *surface, AMediaCrypto *crypto, uint32_t flags)
{
    (void)c; (void)f; (void)surface; (void)crypto; (void)flags;
    return unsupported("AMediaCodec_configure");
}
media_status_t AMediaCodec_start(AMediaCodec *c) { (void)c; return AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AMediaCodec_stop(AMediaCodec *c) { (void)c; return AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AMediaCodec_flush(AMediaCodec *c) { (void)c; return AMEDIA_ERROR_UNSUPPORTED; }
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *c, size_t idx, size_t *out_size)
{
    (void)c; (void)idx;
    if (out_size) *out_size = 0;
    return NULL;
}
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *c, size_t idx, size_t *out_size)
{
    (void)c; (void)idx;
    if (out_size) *out_size = 0;
    return NULL;
}
// an index, or a negative AMEDIACODEC_INFO_* / error value
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c, int64_t timeout_us) { (void)c; (void)timeout_us; return AMEDIA_ERROR_UNSUPPORTED; }
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c, AMediaCodecBufferInfo *info, int64_t timeout_us)
{
    (void)c; (void)info; (void)timeout_us;
    return AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c, size_t idx, a_off_t offset, size_t size, uint64_t time, uint32_t flags)
{
    (void)c; (void)idx; (void)offset; (void)size; (void)time; (void)flags;
    return AMEDIA_ERROR_UNSUPPORTED;
}
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *c) { (void)c; return NULL; }
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *c, size_t idx, bool render)
{
    (void)c; (void)idx; (void)render;
    return AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AMediaCodec_setOutputSurface(AMediaCodec *c, ANativeWindow *surface) { (void)c; (void)surface; return AMEDIA_ERROR_UNSUPPORTED; }

// ---- AMediaDataSource (API 28) ------------------------------------------------------------------------
AMediaDataSource *AMediaDataSource_new(void) { return NULL; }
void AMediaDataSource_delete(AMediaDataSource *s) { (void)s; }
void AMediaDataSource_setUserdata(AMediaDataSource *s, void *user) { (void)s; (void)user; }
void AMediaDataSource_setReadAt(AMediaDataSource *s, void *fn) { (void)s; (void)fn; }
void AMediaDataSource_setGetSize(AMediaDataSource *s, void *fn) { (void)s; (void)fn; }
void AMediaDataSource_setClose(AMediaDataSource *s, void *fn) { (void)s; (void)fn; }

// ---- AImageReader / AImage (API 24/26) ---------------------------------------------------------------
media_status_t AImageReader_new(int32_t w, int32_t h, int32_t format, int32_t max_images, AImageReader **reader)
{
    (void)w; (void)h; (void)format; (void)max_images;
    if (reader) *reader = NULL;
    return unsupported("AImageReader_new");
}
media_status_t AImageReader_newWithUsage(int32_t w, int32_t h, int32_t format, uint64_t usage, int32_t max_images, AImageReader **reader)
{
    (void)w; (void)h; (void)format; (void)usage; (void)max_images;
    if (reader) *reader = NULL;
    return unsupported("AImageReader_newWithUsage");
}
void AImageReader_delete(AImageReader *r) { (void)r; }
media_status_t AImageReader_getWindow(AImageReader *r, ANativeWindow **window)
{
    (void)r;
    if (window) *window = NULL;
    return AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AImageReader_acquireLatestImage(AImageReader *r, AImage **image)
{
    (void)r;
    if (image) *image = NULL;
    return AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AImageReader_acquireNextImage(AImageReader *r, AImage **image)
{
    (void)r;
    if (image) *image = NULL;
    return AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AImageReader_setImageListener(AImageReader *r, AImageReader_ImageListener *l) { (void)r; (void)l; return AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AImageReader_setBufferRemovedListener(AImageReader *r, AImageReader_BufferRemovedListener *l)
{
    (void)r; (void)l;
    return AMEDIA_ERROR_UNSUPPORTED;
}
void AImage_delete(AImage *i) { (void)i; }
// the release fence's ownership passes to the call
void AImage_deleteAsync(AImage *i, int release_fence_fd) { (void)i; if (release_fence_fd >= 0) close(release_fence_fd); }
media_status_t AImage_getHardwareBuffer(const AImage *i, AHardwareBuffer **buffer)
{
    (void)i;
    if (buffer) *buffer = NULL;
    return AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AImage_getTimestamp(const AImage *i, int64_t *ts) { (void)i; (void)ts; return AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AImage_getWidth(const AImage *i, int32_t *w) { (void)i; (void)w; return AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AImage_getHeight(const AImage *i, int32_t *h) { (void)i; (void)h; return AMEDIA_ERROR_UNSUPPORTED; }
