/* Internal interfaces of the media core (see um.h). */
#ifndef UM_INTERNAL_H
#define UM_INTERNAL_H

#include "um.h"
#include <pthread.h>
#include <stdint.h>

#define UM_TRACE(...) do { if (um_tracing()) um_log(__VA_ARGS__); } while (0)

/* ---- byte sources ---------------------------------------------------------------------------- */
typedef struct um_source {
    int fd;                     /* dup'ed, or -1 */
    int64_t base, size;         /* the range [base, base + size) of fd */
    um_read_at_fn read_at;      /* or a callback (AMediaDataSource) */
    void (*close_cb)(void *);
    void *user;
} um_source;
int     um_source_fd(um_source *s, int fd, int64_t offset, int64_t length);
int     um_source_path(um_source *s, const char *path);
void    um_source_close(um_source *s);
/* exactly n bytes at off (0), or a short read / error (-1) */
int     um_source_read(um_source *s, int64_t off, void *buf, size_t n);

/* ---- demuxed tracks ---------------------------------------------------------------------------- */
typedef struct {
    int64_t pos;                /* byte offset in the source */
    uint32_t size;
    uint32_t flags;             /* UM_SAMPLE_SYNC */
    int64_t pts_us;
} um_sample;

typedef struct {
    um_format *fmt;
    um_sample *s;
    int n, cap;
    int cur;                    /* next sample to return */
    int selected;
    uint32_t max_size;
    int nal_len;                /* MP4 H.264 / HEVC: NAL length bytes, replaced by start codes on read */
} um_track;

typedef struct um_demux {
    const char *container;      /* "webm", "mp4" */
    um_track *t;
    int nt;
    um_format *file_fmt;
} um_demux;

/* both fill d->t / d->nt with complete sample tables (the whole file is indexed on open) */
int  um_webm_open(um_source *src, um_demux *d);
int  um_mp4_open(um_source *src, um_demux *d);
int  um_ffmpeg_demux_open(um_source *src, um_demux *d);    /* um_ffmpeg.c, when available */
void um_demux_free(um_demux *d);
um_track *um_demux_add_track(um_demux *d);
int  um_track_add(um_track *t, int64_t pos, uint32_t size, int64_t pts_us, uint32_t flags);
void um_track_finish(um_track *t);   /* sort by time, set max-input-size, frame rate */

/* ---- decoder backends -------------------------------------------------------------------------- */
typedef struct um_dec um_dec;
typedef struct {
    /* video: one decoded picture (I420 planes), pts in us */
    int (*video)(void *codec, const uint8_t *y, int ys, const uint8_t *u, int us, const uint8_t *v, int vs,
                 int width, int height, int64_t pts_us);
    /* audio: interleaved 16-bit PCM */
    int (*audio)(void *codec, const int16_t *pcm, int frames, int channels, int rate, int64_t pts_us);
    void *codec;
} um_emit;

struct um_dec {
    const char *name;           /* Android-like codec name, e.g. "OMX.google.vp8.decoder" */
    /* the input format (csd-*, sample rate...); fills out (width/height or rate/channels) when known */
    int  (*configure)(um_dec *d, const um_format *in, um_format *out);
    /* one access unit (or codec config data with UM_BUFFER_FLAG_CODEC_CONFIG); size 0 + EOS: drain */
    int  (*decode)(um_dec *d, const uint8_t *data, size_t size, int64_t pts_us, uint32_t flags, um_emit *emit);
    void (*flush)(um_dec *d);
    void (*destroy)(um_dec *d);
};

typedef struct {
    const char *mime;
    const char *name;
    um_dec *(*create)(const char *mime);
    int is_video;
} um_dec_entry;

/* each returns NULL when its library is not built in / not found */
um_dec *um_vpx_create(const char *mime);
um_dec *um_vorbis_create(const char *mime);
um_dec *um_opus_create(const char *mime);
um_dec *um_h264_create(const char *mime);
um_dec *um_ffmpeg_create(const char *mime);
um_dec *um_silent_create(const char *mime);          /* AAC / MP3 without a decoder: silence */
int     um_ffmpeg_available(void);

/* H.264 / HEVC (um_mp4.c): avcC / hvcC -> csd-0 (/ csd-1) in Annex B form, the NAL length size;
   length-prefixed NAL units -> start codes (in == out is fine for 4-byte lengths) */
int  um_avcc_csd(um_format *f, const uint8_t *avcc, size_t n, int *nal_len_size);
int  um_hvcc_csd(um_format *f, const uint8_t *hvcc, size_t n, int *nal_len_size);
ssize_t um_nal_convert(const uint8_t *in, size_t n, int nal_len_size, uint8_t *out, size_t cap);

int  um_cpu_count(void);
int64_t um_now_us(void);

#endif
