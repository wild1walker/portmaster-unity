/*
 * um: the media core behind Android's media APIs in this runtime (Java android.media.* in
 * ../jni/up_media.c, the NDK's libmediandk in ../android/media_ndk.c).
 *
 *   um_format     a MediaFormat: typed key/value pairs (int32, int64, float, string, buffer)
 *   um_extractor  a MediaExtractor: tracks, formats and samples of a WebM/Matroska file (own
 *                 demuxer, webm.c) or of an MP4/MOV file (own demuxer, mp4.c), over a file, a
 *                 file descriptor range (an APK/OBB entry) or a read callback
 *   um_codec      a MediaCodec decoder with Android's buffer model (input/output buffer indices,
 *                 INFO_* codes, a decoding thread), backends: libvpx (VP8, VP9), libvorbis,
 *                 libopus, OpenH264 (H.264), the system's libavcodec when it is there (dlopen)
 *   um_surface    the consumer side of a video Surface: a SurfaceTexture's frame queue,
 *                 updateTexImage (frames into the GL_TEXTURE_EXTERNAL_OES texture through an
 *                 EGLImage) and getTransformMatrix
 *
 * Video frames leave the decoders in one layout, "packed 4:2:0": a W' x (H' + H'/2) array of
 * bytes (W', H' = width and height rounded up to even): the Y plane on top, then each chroma row
 * as U (W'/2 bytes) followed by V (W'/2 bytes). It is uploaded as it is into a one-channel
 * texture that is the EGLImage behind the SurfaceTexture's external texture, and the converter's
 * Hidden/VideoDecodeAndroid shader (convert/pmunity/video_decode_android.shader) reads Y, U and V
 * from it.
 */
#ifndef UM_H
#define UM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- MediaFormat ------------------------------------------------------------------------- */
enum { UM_NONE = 0, UM_INT32, UM_INT64, UM_FLOAT, UM_STRING, UM_BUFFER };

typedef struct um_format um_format;
um_format  *um_format_new(void);
um_format  *um_format_copy(const um_format *f);
void        um_format_free(um_format *f);
void        um_format_set_int32(um_format *f, const char *key, int32_t v);
void        um_format_set_int64(um_format *f, const char *key, int64_t v);
void        um_format_set_float(um_format *f, const char *key, float v);
void        um_format_set_string(um_format *f, const char *key, const char *v);
void        um_format_set_buffer(um_format *f, const char *key, const void *data, size_t size);
void        um_format_remove(um_format *f, const char *key);
int         um_format_type(const um_format *f, const char *key);       /* UM_NONE if absent */
/* 1 if present with that type, else 0 (out untouched) */
int         um_format_get_int32(const um_format *f, const char *key, int32_t *out);
int         um_format_get_int64(const um_format *f, const char *key, int64_t *out);
int         um_format_get_float(const um_format *f, const char *key, float *out);
const char *um_format_get_string(const um_format *f, const char *key);  /* NULL if absent  */
const void *um_format_get_buffer(const um_format *f, const char *key, size_t *size);
int         um_format_count(const um_format *f);
const char *um_format_key(const um_format *f, int i);
/* "{mime=video/x-vnd.on2.vp8, width=1920, ...}" like Android's toString; malloc'd */
char       *um_format_to_string(const um_format *f);

/* Android's MediaFormat key strings (KEY_*) and mime types */
#define UM_KEY_MIME           "mime"
#define UM_KEY_WIDTH          "width"
#define UM_KEY_HEIGHT         "height"
#define UM_KEY_DURATION       "durationUs"
#define UM_KEY_FRAME_RATE     "frame-rate"
#define UM_KEY_SAMPLE_RATE    "sample-rate"
#define UM_KEY_CHANNEL_COUNT  "channel-count"
#define UM_KEY_MAX_INPUT_SIZE "max-input-size"
#define UM_KEY_LANGUAGE       "language"
#define UM_KEY_COLOR_FORMAT   "color-format"
#define UM_KEY_STRIDE         "stride"
#define UM_KEY_SLICE_HEIGHT   "slice-height"
#define UM_KEY_PCM_ENCODING   "pcm-encoding"
#define UM_KEY_ROTATION       "rotation-degrees"
#define UM_KEY_TRACK_ID       "track-id"
#define UM_MIME_VP8   "video/x-vnd.on2.vp8"
#define UM_MIME_VP9   "video/x-vnd.on2.vp9"
#define UM_MIME_AVC   "video/avc"
#define UM_MIME_HEVC  "video/hevc"
#define UM_MIME_VORBIS "audio/vorbis"
#define UM_MIME_OPUS  "audio/opus"
#define UM_MIME_AAC   "audio/mp4a-latm"
#define UM_MIME_MPEG  "audio/mpeg"
#define UM_MIME_RAW   "audio/raw"

/* ---- MediaExtractor ------------------------------------------------------------------------ */
enum { UM_SEEK_PREVIOUS_SYNC = 0, UM_SEEK_NEXT_SYNC = 1, UM_SEEK_CLOSEST_SYNC = 2 };
enum { UM_SAMPLE_SYNC = 1, UM_SAMPLE_ENCRYPTED = 2, UM_SAMPLE_PARTIAL = 4 };
/* errors: negative; the NDK's media_status_t values */
enum { UM_OK = 0, UM_ERR_UNKNOWN = -10000, UM_ERR_MALFORMED = -10001, UM_ERR_UNSUPPORTED = -10002,
       UM_ERR_INVALID = -10003, UM_ERR_IO = -10004, UM_ERR_STATE = -10005 };

typedef ssize_t (*um_read_at_fn)(void *user, int64_t offset, void *buf, size_t size);

typedef struct um_extractor um_extractor;
um_extractor *um_extractor_new(void);
void        um_extractor_free(um_extractor *e);
/* the bytes [offset, offset + length) of fd (dup'ed: the caller may close fd); length < 0: to the end */
int         um_extractor_set_fd(um_extractor *e, int fd, int64_t offset, int64_t length);
int         um_extractor_set_path(um_extractor *e, const char *path);   /* "file://" accepted */
int         um_extractor_set_reader(um_extractor *e, um_read_at_fn read_at, int64_t size, void *user,
                                    void (*close)(void *user));
int         um_extractor_track_count(um_extractor *e);
um_format  *um_extractor_track_format(um_extractor *e, int track);     /* a copy; NULL if no track */
um_format  *um_extractor_file_format(um_extractor *e);                 /* a copy (duration, mime) */
int         um_extractor_select(um_extractor *e, int track, int on);
/* the current sample: data (returns its size, or -1 when there is none), time (us, -1), track
   (-1), flags (UM_SAMPLE_*), size (-1); advance: 1 if there is a next sample */
ssize_t     um_extractor_read(um_extractor *e, void *buf, size_t cap);
int64_t     um_extractor_sample_time(um_extractor *e);
int         um_extractor_sample_track(um_extractor *e);
uint32_t    um_extractor_sample_flags(um_extractor *e);
int64_t     um_extractor_sample_size(um_extractor *e);
int         um_extractor_advance(um_extractor *e);
int         um_extractor_seek(um_extractor *e, int64_t time_us, int mode);
const char *um_extractor_container(um_extractor *e);                    /* "webm", "mp4", NULL */

/* ---- video surfaces (SurfaceTexture / ANativeWindow) ----------------------------------------- */
typedef struct um_frame {
    struct um_frame *next;
    int width, height;              /* the picture (crop) */
    int pw, ph;                     /* padded (even) size: the packed image is pw x (ph + ph/2) bytes */
    int64_t pts_us;
    size_t cap;
    uint8_t *data;
} um_frame;
um_frame *um_frame_get(int width, int height);   /* pooled */
void      um_frame_put(um_frame *f);
/* copy planes into a packed frame (odd sizes and padding: the last row / column repeated) */
void      um_frame_fill(um_frame *f, const uint8_t *y, int ys, const uint8_t *u, int us,
                        const uint8_t *v, int vs);

typedef struct um_surface um_surface;
um_surface *um_surface_new(unsigned gl_texture);     /* SurfaceTexture(texName); refcount 1 */
um_surface *um_surface_ref(um_surface *s);
void        um_surface_unref(um_surface *s);
/* frame available: called on the producing thread right after a frame is queued (the Java side
   posts onFrameAvailable to the SurfaceTexture's looper from it) */
void        um_surface_set_listener(um_surface *s, void (*fn)(void *user, um_surface *s), void *user);
void        um_surface_queue(um_surface *s, um_frame *f);  /* takes the frame */
int         um_surface_pending(um_surface *s);            /* frames queued, not yet latched */
/* updateTexImage on the thread that has the GL context: latches the next queued frame into the
   texture. Returns 1 if a new frame was latched, 0 if none was queued, <0 on a GL problem. */
int         um_surface_update(um_surface *s);
void        um_surface_matrix(um_surface *s, float m[16]);  /* getTransformMatrix (column-major) */
int64_t     um_surface_timestamp(um_surface *s);           /* getTimestamp, ns */
void        um_surface_set_default_size(um_surface *s, int w, int h);
void        um_surface_detach_gl(um_surface *s);           /* GL objects freed on the next update */

/* ---- MediaCodec ---------------------------------------------------------------------------- */
enum { UM_INFO_TRY_AGAIN_LATER = -1, UM_INFO_OUTPUT_FORMAT_CHANGED = -2, UM_INFO_OUTPUT_BUFFERS_CHANGED = -3 };
enum { UM_BUFFER_FLAG_KEY_FRAME = 1, UM_BUFFER_FLAG_CODEC_CONFIG = 2, UM_BUFFER_FLAG_END_OF_STREAM = 4,
       UM_BUFFER_FLAG_PARTIAL_FRAME = 8 };

typedef struct { int32_t offset, size; int64_t pts_us; uint32_t flags; } um_buffer_info;

typedef struct um_codec um_codec;
/* NULL if no decoder for mime (encoders are not provided) */
um_codec   *um_codec_create(const char *mime);
um_codec   *um_codec_create_by_name(const char *name);
int         um_codec_supported(const char *mime);
const char *um_codec_name(um_codec *c);
int         um_codec_configure(um_codec *c, const um_format *f, um_surface *surface);
int         um_codec_set_surface(um_codec *c, um_surface *surface);
int         um_codec_start(um_codec *c);
int         um_codec_stop(um_codec *c);
int         um_codec_flush(um_codec *c);
void        um_codec_free(um_codec *c);                     /* release() */
int         um_codec_input_count(um_codec *c);
int         um_codec_output_count(um_codec *c);
uint8_t    *um_codec_input_buffer(um_codec *c, int i, size_t *cap);
/* output buffer memory: audio PCM; video without a surface: the packed 4:2:0 picture; video with
   a surface: a few bytes that are never written (Unity wants a buffer behind every index). The
   memory of an index stays put until INFO_OUTPUT_BUFFERS_CHANGED. */
uint8_t    *um_codec_output_buffer(um_codec *c, int i, size_t *cap);
int         um_codec_dequeue_input(um_codec *c, int64_t timeout_us);    /* index or TRY_AGAIN */
int         um_codec_queue_input(um_codec *c, int i, int offset, int size, int64_t pts_us, uint32_t flags);
int         um_codec_dequeue_output(um_codec *c, um_buffer_info *info, int64_t timeout_us);
um_format  *um_codec_output_format(um_codec *c);            /* a copy */
um_format  *um_codec_input_format(um_codec *c);             /* a copy */
int         um_codec_release_output(um_codec *c, int i, int render);

/* ---- misc ---------------------------------------------------------------------------------- */
/* "[media] ..." on stderr; UM_TRACE=1 also logs per-buffer events */
void        um_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int         um_tracing(void);
/* what this build decodes, e.g. "vp8 vp9 vorbis opus avc(openh264) aac(libavcodec 59)" */
const char *um_decoders(void);
/* the system's libavcodec this build found ("libavcodec 59"), or NULL (none, or UM_FFMPEG=0) */
const char *um_ffmpeg_version(void);

#ifdef __cplusplus
}
#endif
#endif
