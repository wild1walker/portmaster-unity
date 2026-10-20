// libmediandk.so: the NDK media API (AMediaExtractor, AMediaFormat, AMediaCodec, AMediaDataSource;
// AImageReader / AImage as failing stubs). Unity 2022.3's libunity links against libmediandk
// (DT_NEEDED; its VideoPlayer decodes through the NDK, into a Surface made from a Java
// SurfaceTexture: ANativeWindow_fromSurface); older players dlopen it and decode through Java when
// that fails (the host refuses the dlopen: ../jni/up_media.c is their path).
//
// The work is the media core's (../media, libuamedia.a), referenced weakly: a program that links
// the core (ua_host) gets working extractors and decoders; one that does not (this folder's unit
// tests) gets stubs that fail the way the NDK reports failure: constructors NULL, status calls
// AMEDIA_ERROR_UNSUPPORTED, getters false / -1 / 0, out-pointers cleared, NULL objects accepted.
// The AMEDIAFORMAT_KEY_* constants are data objects (const char * variables) with the NDK's
// values, as libunity reads them through GOT entries.
//
// Declarations follow the NDK headers (NdkMediaCodec.h, NdkMediaExtractor.h, NdkMediaFormat.h,
// NdkMediaError.h, NdkImageReader.h, NdkImage.h, NdkMediaDataSource.h); only scalars and pointers
// cross the ABI, except AMediaFormat_setFloat / getFloat's out value (softfp on 32-bit ARM).
// AMediaCodecBufferInfo has the media core's um_buffer_info layout.
#include "android_shims.h"
#include "shim_internal.h"
#include "../media/um.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* the media core, when the program has it */
#pragma weak um_format_new
#pragma weak um_format_free
#pragma weak um_format_get_int32
#pragma weak um_format_get_int64
#pragma weak um_format_get_float
#pragma weak um_format_get_string
#pragma weak um_format_get_buffer
#pragma weak um_format_type
#pragma weak um_format_set_int32
#pragma weak um_format_set_int64
#pragma weak um_format_set_float
#pragma weak um_format_set_string
#pragma weak um_format_set_buffer
#pragma weak um_format_to_string
#pragma weak um_extractor_new
#pragma weak um_extractor_free
#pragma weak um_extractor_set_fd
#pragma weak um_extractor_set_path
#pragma weak um_extractor_set_reader
#pragma weak um_extractor_track_count
#pragma weak um_extractor_track_format
#pragma weak um_extractor_file_format
#pragma weak um_extractor_select
#pragma weak um_extractor_read
#pragma weak um_extractor_sample_time
#pragma weak um_extractor_sample_track
#pragma weak um_extractor_sample_flags
#pragma weak um_extractor_sample_size
#pragma weak um_extractor_advance
#pragma weak um_extractor_seek
#pragma weak um_codec_create
#pragma weak um_codec_create_by_name
#pragma weak um_codec_name
#pragma weak um_codec_configure
#pragma weak um_codec_set_surface
#pragma weak um_codec_start
#pragma weak um_codec_stop
#pragma weak um_codec_flush
#pragma weak um_codec_free
#pragma weak um_codec_input_buffer
#pragma weak um_codec_output_buffer
#pragma weak um_codec_dequeue_input
#pragma weak um_codec_queue_input
#pragma weak um_codec_dequeue_output
#pragma weak um_codec_output_format
#pragma weak um_codec_input_format
#pragma weak um_codec_release_output

typedef int32_t media_status_t;
enum {
    AMEDIA_OK = 0,
    AMEDIA_ERROR_UNKNOWN = -10000,
    AMEDIA_ERROR_MALFORMED = -10001,
    AMEDIA_ERROR_UNSUPPORTED = -10002,
    AMEDIA_ERROR_INVALID_OBJECT = -10003,
    AMEDIA_ERROR_INVALID_PARAMETER = -10004,
    AMEDIA_ERROR_INVALID_OPERATION = -10005,
};

static int have_core(void) { return um_extractor_new != NULL && um_codec_create != NULL && um_format_new != NULL; }

static media_status_t unsupported(const char *what)
{
    SHIM_TRACE("%s: no media decoder", what);
    return AMEDIA_ERROR_UNSUPPORTED;
}

/* the media core's codes are the NDK's */
static media_status_t st(int r) { return r == UM_OK ? AMEDIA_OK : r; }

typedef struct AMediaCodec AMediaCodec;
typedef struct AMediaExtractor AMediaExtractor;
typedef struct AMediaFormat AMediaFormat;
typedef struct AMediaCrypto AMediaCrypto;
typedef struct AMediaCodecBufferInfo { int32_t offset; int32_t size; int64_t presentationTimeUs; uint32_t flags; } AMediaCodecBufferInfo;
typedef struct AImageReader AImageReader;
typedef struct AImage AImage;
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AImageReader_ImageListener AImageReader_ImageListener;
typedef struct AImageReader_BufferRemovedListener AImageReader_BufferRemovedListener;
_Static_assert(sizeof(AMediaCodecBufferInfo) == sizeof(um_buffer_info), "AMediaCodecBufferInfo layout");

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

// ---- AMediaFormat: a um_format, plus the string toString hands out --------------------------------
#define FMT_MAGIC 0x41464d54u   /* 'AFMT' */
struct AMediaFormat { uint32_t magic; um_format *f; char *str; };

static AMediaFormat *wrap_format(um_format *f)
{
    if (!f) return NULL;
    AMediaFormat *w = calloc(1, sizeof *w);
    if (!w) { um_format_free(f); return NULL; }
    w->magic = FMT_MAGIC;
    w->f = f;
    return w;
}
static um_format *fmt(const AMediaFormat *w) { return w && w->magic == FMT_MAGIC ? w->f : NULL; }

AMediaFormat *AMediaFormat_new(void)
{
    if (!have_core()) { SHIM_TRACE("AMediaFormat_new: no media decoder"); return NULL; }
    return wrap_format(um_format_new());
}
media_status_t AMediaFormat_delete(AMediaFormat *f)
{
    if (f && f->magic == FMT_MAGIC) { f->magic = 0; um_format_free(f->f); free(f->str); free(f); }
    return AMEDIA_OK;
}
const char *AMediaFormat_toString(AMediaFormat *f)
{
    if (!fmt(f)) return "";
    free(f->str);
    f->str = um_format_to_string(f->f);
    return f->str ? f->str : "";
}
bool AMediaFormat_getInt32(AMediaFormat *f, const char *name, int32_t *out) { return fmt(f) && um_format_get_int32(f->f, name, out); }
bool AMediaFormat_getInt64(AMediaFormat *f, const char *name, int64_t *out) { return fmt(f) && um_format_get_int64(f->f, name, out); }
bool AMediaFormat_getFloat(AMediaFormat *f, const char *name, float *out) { return fmt(f) && um_format_get_float(f->f, name, out); }
bool AMediaFormat_getSize(AMediaFormat *f, const char *name, size_t *out)
{
    int64_t v;
    int32_t i;
    if (!fmt(f)) return false;
    if (um_format_get_int64(f->f, name, &v)) { if (out) *out = (size_t)v; return true; }
    if (um_format_get_int32(f->f, name, &i)) { if (out) *out = (size_t)i; return true; }
    return false;
}
bool AMediaFormat_getBuffer(AMediaFormat *f, const char *name, void **data, size_t *size)
{
    size_t n = 0;
    const void *p = fmt(f) ? um_format_get_buffer(f->f, name, &n) : NULL;
    if (!p) return false;
    if (data) *data = (void *)p;
    if (size) *size = n;
    return true;
}
bool AMediaFormat_getString(AMediaFormat *f, const char *name, const char **out)
{
    const char *s = fmt(f) ? um_format_get_string(f->f, name) : NULL;
    if (!s) return false;
    if (out) *out = s;
    return true;
}
void AMediaFormat_setInt32(AMediaFormat *f, const char *name, int32_t v) { if (fmt(f)) um_format_set_int32(f->f, name, v); }
void AMediaFormat_setInt64(AMediaFormat *f, const char *name, int64_t v) { if (fmt(f)) um_format_set_int64(f->f, name, v); }
SOFTFP void AMediaFormat_setFloat(AMediaFormat *f, const char *name, float v) { if (fmt(f)) um_format_set_float(f->f, name, v); }
void AMediaFormat_setString(AMediaFormat *f, const char *name, const char *v) { if (fmt(f)) um_format_set_string(f->f, name, v); }
void AMediaFormat_setBuffer(AMediaFormat *f, const char *name, const void *data, size_t size)
{
    if (fmt(f) && data) um_format_set_buffer(f->f, name, data, size);
}

// ---- AMediaDataSource (API 28): callbacks into the app's code -----------------------------------
typedef ssize_t (*AMediaDataSourceReadAt)(void *userdata, a_off64_t offset, void *buffer, size_t size);
typedef ssize_t (*AMediaDataSourceGetSize)(void *userdata);
typedef void (*AMediaDataSourceClose)(void *userdata);
typedef struct AMediaDataSource {
    void *user;
    AMediaDataSourceReadAt read_at;
    AMediaDataSourceGetSize get_size;
    AMediaDataSourceClose close;
} AMediaDataSource;

AMediaDataSource *AMediaDataSource_new(void) { return have_core() ? calloc(1, sizeof(AMediaDataSource)) : NULL; }
void AMediaDataSource_delete(AMediaDataSource *s) { free(s); }
void AMediaDataSource_setUserdata(AMediaDataSource *s, void *user) { if (s) s->user = user; }
void AMediaDataSource_setReadAt(AMediaDataSource *s, void *fn) { if (s) s->read_at = (AMediaDataSourceReadAt)fn; }
void AMediaDataSource_setGetSize(AMediaDataSource *s, void *fn) { if (s) s->get_size = (AMediaDataSourceGetSize)fn; }
void AMediaDataSource_setClose(AMediaDataSource *s, void *fn) { if (s) s->close = (AMediaDataSourceClose)fn; }

static ssize_t ds_read(void *user, int64_t off, void *buf, size_t n)
{
    AMediaDataSource *s = user;
    return s->read_at ? s->read_at(s->user, off, buf, n) : -1;
}

// ---- AMediaExtractor: a um_extractor ------------------------------------------------------------
#define EX(e) ((um_extractor *)(e))
AMediaExtractor *AMediaExtractor_new(void)
{
    if (!have_core()) { SHIM_TRACE("AMediaExtractor_new: no media decoder"); return NULL; }
    return (AMediaExtractor *)um_extractor_new();
}
media_status_t AMediaExtractor_delete(AMediaExtractor *e) { if (e && have_core()) um_extractor_free(EX(e)); return AMEDIA_OK; }
media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor *e, int fd, a_off64_t offset, a_off64_t length)
{
    if (!e) return unsupported("AMediaExtractor_setDataSourceFd");
    SHIM_TRACE("AMediaExtractor_setDataSourceFd(%d, %lld, %lld)", fd, (long long)offset, (long long)length);
    return st(um_extractor_set_fd(EX(e), fd, offset, length));
}
media_status_t AMediaExtractor_setDataSource(AMediaExtractor *e, const char *location)
{
    if (!e) return unsupported("AMediaExtractor_setDataSource");
    if (!location || !strncmp(location, "http:", 5) || !strncmp(location, "https:", 6)) return AMEDIA_ERROR_UNSUPPORTED;
    return st(um_extractor_set_path(EX(e), location));
}
media_status_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *e, AMediaDataSource *src)
{
    if (!e || !src || !src->read_at) return unsupported("AMediaExtractor_setDataSourceCustom");
    ssize_t size = src->get_size ? src->get_size(src->user) : -1;
    if (size <= 0) return AMEDIA_ERROR_UNSUPPORTED;             /* (streams of unknown size) */
    return st(um_extractor_set_reader(EX(e), ds_read, size, src, NULL));
}
size_t AMediaExtractor_getTrackCount(AMediaExtractor *e) { return e ? (size_t)um_extractor_track_count(EX(e)) : 0; }
AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *e, size_t idx)
{
    return e ? wrap_format(um_extractor_track_format(EX(e), (int)idx)) : NULL;
}
media_status_t AMediaExtractor_selectTrack(AMediaExtractor *e, size_t idx) { return e ? st(um_extractor_select(EX(e), (int)idx, 1)) : AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AMediaExtractor_unselectTrack(AMediaExtractor *e, size_t idx) { return e ? st(um_extractor_select(EX(e), (int)idx, 0)) : AMEDIA_ERROR_UNSUPPORTED; }
ssize_t AMediaExtractor_readSampleData(AMediaExtractor *e, uint8_t *buf, size_t cap) { return e ? um_extractor_read(EX(e), buf, cap) : -1; }
uint32_t AMediaExtractor_getSampleFlags(AMediaExtractor *e) { return e ? um_extractor_sample_flags(EX(e)) : 0; }
int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *e) { return e ? um_extractor_sample_track(EX(e)) : -1; }
int64_t AMediaExtractor_getSampleTime(AMediaExtractor *e) { return e ? um_extractor_sample_time(EX(e)) : -1; }
ssize_t AMediaExtractor_getSampleSize(AMediaExtractor *e) { return e ? (ssize_t)um_extractor_sample_size(EX(e)) : -1; }
int64_t AMediaExtractor_getCachedDuration(AMediaExtractor *e) { (void)e; return -1; }
bool AMediaExtractor_advance(AMediaExtractor *e) { return e && um_extractor_advance(EX(e)); }
media_status_t AMediaExtractor_seekTo(AMediaExtractor *e, int64_t us, int mode) { return e ? st(um_extractor_seek(EX(e), us, mode)) : AMEDIA_ERROR_UNSUPPORTED; }
AMediaFormat *AMediaExtractor_getFileFormat(AMediaExtractor *e) { return e ? wrap_format(um_extractor_file_format(EX(e))) : NULL; }

// ---- AMediaCodec: a um_codec; its video Surface is the ANativeWindow of a Surface(SurfaceTexture) --
#define CO(c) ((um_codec *)(c))
AMediaCodec *AMediaCodec_createDecoderByType(const char *mime)
{
    if (!have_core()) { SHIM_TRACE("AMediaCodec_createDecoderByType(%s): no media decoder", mime ? mime : "(null)"); return NULL; }
    return (AMediaCodec *)um_codec_create(mime);
}
AMediaCodec *AMediaCodec_createEncoderByType(const char *mime) { (void)mime; return NULL; }
AMediaCodec *AMediaCodec_createCodecByName(const char *name) { return have_core() ? (AMediaCodec *)um_codec_create_by_name(name) : NULL; }
media_status_t AMediaCodec_getName(AMediaCodec *c, char **out)
{
    if (!c || !out) return AMEDIA_ERROR_INVALID_PARAMETER;
    *out = strdup(um_codec_name(CO(c)));
    return *out ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}
void AMediaCodec_releaseName(AMediaCodec *c, char *name) { (void)c; free(name); }
media_status_t AMediaCodec_delete(AMediaCodec *c) { if (c && have_core()) um_codec_free(CO(c)); return AMEDIA_OK; }
media_status_t AMediaCodec_configure(AMediaCodec *c, const AMediaFormat *f, ANativeWindow *surface, AMediaCrypto *crypto, uint32_t flags)
{
    (void)crypto;
    if (!c) return unsupported("AMediaCodec_configure");
    if (flags & 1) return AMEDIA_ERROR_UNSUPPORTED;               /* AMEDIACODEC_CONFIGURE_FLAG_ENCODE */
    um_surface *s = surface ? (um_surface *)android_window_media_surface(surface) : NULL;
    if (surface && !s) SHIM_WARN("AMediaCodec_configure: a window that is not a SurfaceTexture's: decoding to buffers");
    return st(um_codec_configure(CO(c), fmt(f), s));
}
media_status_t AMediaCodec_setOutputSurface(AMediaCodec *c, ANativeWindow *surface)
{
    if (!c) return AMEDIA_ERROR_UNSUPPORTED;
    return st(um_codec_set_surface(CO(c), surface ? (um_surface *)android_window_media_surface(surface) : NULL));
}
media_status_t AMediaCodec_start(AMediaCodec *c) { return c ? st(um_codec_start(CO(c))) : AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AMediaCodec_stop(AMediaCodec *c) { return c ? st(um_codec_stop(CO(c))) : AMEDIA_ERROR_UNSUPPORTED; }
media_status_t AMediaCodec_flush(AMediaCodec *c) { return c ? st(um_codec_flush(CO(c))) : AMEDIA_ERROR_UNSUPPORTED; }
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *c, size_t idx, size_t *out_size)
{
    if (!c) { if (out_size) *out_size = 0; return NULL; }
    return um_codec_input_buffer(CO(c), (int)idx, out_size);
}
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *c, size_t idx, size_t *out_size)
{
    if (!c) { if (out_size) *out_size = 0; return NULL; }
    return um_codec_output_buffer(CO(c), (int)idx, out_size);
}
// an index, or a negative AMEDIACODEC_INFO_* / error value
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c, int64_t timeout_us)
{
    return c ? um_codec_dequeue_input(CO(c), timeout_us) : AMEDIA_ERROR_UNSUPPORTED;
}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c, AMediaCodecBufferInfo *info, int64_t timeout_us)
{
    return c ? um_codec_dequeue_output(CO(c), (um_buffer_info *)info, timeout_us) : AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c, size_t idx, a_off_t offset, size_t size, uint64_t time, uint32_t flags)
{
    return c ? st(um_codec_queue_input(CO(c), (int)idx, (int)offset, (int)size, (int64_t)time, flags)) : AMEDIA_ERROR_UNSUPPORTED;
}
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *c) { return c ? wrap_format(um_codec_output_format(CO(c))) : NULL; }
AMediaFormat *AMediaCodec_getInputFormat(AMediaCodec *c) { return c ? wrap_format(um_codec_input_format(CO(c))) : NULL; }
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *c, size_t idx, bool render)
{
    return c ? st(um_codec_release_output(CO(c), (int)idx, render)) : AMEDIA_ERROR_UNSUPPORTED;
}
media_status_t AMediaCodec_releaseOutputBufferAtTime(AMediaCodec *c, size_t idx, int64_t timestamp_ns)
{
    (void)timestamp_ns;
    return c ? st(um_codec_release_output(CO(c), (int)idx, 1)) : AMEDIA_ERROR_UNSUPPORTED;
}

// ---- AImageReader / AImage (API 24/26): not provided (Unity's GLES video path is the SurfaceTexture's)
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
