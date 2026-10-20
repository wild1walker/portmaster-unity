// um_codec: a MediaCodec decoder with Android's buffer model.
//
// The client owns input buffers between dequeueInputBuffer and queueInputBuffer, and output
// buffers between dequeueOutputBuffer and releaseOutputBuffer, as on Android. A decoding thread
// per codec takes queued input buffers in order and runs the backend (libvpx, libvorbis, ...),
// whose pictures / PCM go into free output buffers (the thread waits for one; that is the
// back-pressure Android's codecs have too). The first output, and every later change of picture
// size or of sample rate / channel count, is announced by INFO_OUTPUT_FORMAT_CHANGED before the
// buffer. End of stream: an input with BUFFER_FLAG_END_OF_STREAM drains the backend and is
// followed by an empty output buffer carrying the flag.
//
// Video with a Surface: an output buffer holds a decoded picture (packed 4:2:0, see um.h);
// releaseOutputBuffer(i, true) hands it to the surface (SurfaceTexture), false drops it. Without a
// surface the picture is the buffer's memory.
#define _GNU_SOURCE
#include "um_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_IN 8
#define MAX_OUT 8
#define AUDIO_OUT_CAP (128 * 1024)

enum { ST_CREATED, ST_CONFIGURED, ST_RUNNING, ST_EOS };
enum { IN_CODEC, IN_CLIENT, IN_QUEUED };
enum { OUT_FREE, OUT_FILLING, OUT_READY, OUT_CLIENT };

typedef struct {
    uint8_t *data;
    size_t cap;
    int state;
    int32_t off, size;
    int64_t pts;
    uint32_t flags;
} inbuf;

typedef struct {
    int state;
    uint8_t *data;              /* audio PCM; video without a surface: the packed picture; video
                                   with one: a few bytes (Unity wants a buffer behind every index) */
    size_t cap;
    um_frame *frame;            /* video with a surface */
    um_buffer_info info;
    um_format *new_fmt;         /* announce this format before the buffer */
    uint64_t seq;
} outbuf;

static const um_dec_entry decoders[] = {
    { UM_MIME_VP8, "OMX.google.vp8.decoder", um_vpx_create, 1 },
    { UM_MIME_VP9, "OMX.google.vp9.decoder", um_vpx_create, 1 },
    { UM_MIME_AVC, "OMX.google.h264.decoder", um_h264_create, 1 },
    { UM_MIME_VORBIS, "OMX.google.vorbis.decoder", um_vorbis_create, 0 },
    { UM_MIME_OPUS, "OMX.google.opus.decoder", um_opus_create, 0 },
    /* the system's libavcodec, when there is one: what has no decoder of its own here */
    { UM_MIME_AVC, "ffmpeg.h264.decoder", um_ffmpeg_create, 1 },
    { UM_MIME_HEVC, "ffmpeg.hevc.decoder", um_ffmpeg_create, 1 },
    { UM_MIME_AAC, "ffmpeg.aac.decoder", um_ffmpeg_create, 0 },
    { UM_MIME_MPEG, "ffmpeg.mp3.decoder", um_ffmpeg_create, 0 },
};

struct um_codec {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    const um_dec_entry *ent;
    um_dec *dec;
    int video;
    int state;
    um_surface *surface;
    um_format *in_fmt, *out_fmt, *cur_fmt;    /* cur: the decoder's latest; out: announced */
    int nin, nout;
    inbuf in[MAX_IN];
    outbuf out[MAX_OUT];
    int inq[MAX_IN], inq_head, inq_n;          /* queued inputs, FIFO */
    uint64_t out_seq;                          /* ready outputs are returned in seq order */
    pthread_t th;
    int th_on, quit, flushing, busy;
    int buffers_changed;                       /* an output buffer moved: INFO_OUTPUT_BUFFERS_CHANGED */
    int64_t cur_pts;                           /* of the input being decoded */
    int64_t audio_frames_done;                 /* PCM frames emitted for the current input */
    um_emit emit;
};

static int64_t deadline_us(int64_t timeout_us) { return timeout_us < 0 ? -1 : um_now_us() + timeout_us; }

/* wait on the codec's condition until the deadline (-1: forever); 0 on timeout */
static int wait_until(um_codec *c, int64_t dl)
{
    if (dl < 0) { pthread_cond_wait(&c->cv, &c->mu); return 1; }
    int64_t now = um_now_us();
    if (now >= dl) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t ns = ts.tv_nsec + (dl - now) * 1000;
    ts.tv_sec += ns / 1000000000;
    ts.tv_nsec = ns % 1000000000;
    return pthread_cond_timedwait(&c->cv, &c->mu, &ts) != ETIMEDOUT;
}

/* ---- creation -------------------------------------------------------------------------------- */
static um_codec *create_entry(const um_dec_entry *ent)
{
    um_dec *d = ent->create(ent->mime);
    if (!d) return NULL;
    um_codec *c = calloc(1, sizeof *c);
    if (!c) { d->destroy(d); return NULL; }
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->ent = ent;
    c->dec = d;
    c->video = ent->is_video;
    c->state = ST_CREATED;
    return c;
}

um_codec *um_codec_create(const char *mime)
{
    if (!mime) return NULL;
    for (size_t i = 0; i < sizeof decoders / sizeof decoders[0]; i++)
        if (!strcasecmp(decoders[i].mime, mime)) {
            um_codec *c = create_entry(&decoders[i]);
            if (c) { um_log("codec: %s for %s", decoders[i].name, mime); return c; }
        }
    um_log("codec: no decoder for %s", mime);
    return NULL;
}

um_codec *um_codec_create_by_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof decoders / sizeof decoders[0]; i++)
        if (!strcmp(decoders[i].name, name)) return create_entry(&decoders[i]);
    return NULL;
}

int um_codec_supported(const char *mime)
{
    um_codec *c = um_codec_create(mime);
    if (!c) return 0;
    um_codec_free(c);
    return 1;
}

const char *um_decoders(void)
{
    static char buf[256];
    if (buf[0]) return buf;
    const char *seen[16];
    int ns = 0;
    for (size_t i = 0; i < sizeof decoders / sizeof decoders[0]; i++) {
        um_dec *d = decoders[i].create(decoders[i].mime);
        if (!d) continue;
        int dup = 0;
        for (int k = 0; k < ns; k++) if (!strcmp(seen[k], decoders[i].mime)) dup = 1;
        if (!dup && ns < 16) {
            seen[ns++] = decoders[i].mime;
            size_t l = strlen(buf);
            snprintf(buf + l, sizeof buf - l, "%s%s(%s)", l ? " " : "", strchr(decoders[i].mime, '/') + 1, d->name);
        }
        d->destroy(d);
    }
    if (!buf[0]) snprintf(buf, sizeof buf, "none");
    return buf;
}

const char *um_codec_name(um_codec *c) { return c ? c->ent->name : NULL; }

/* ---- output side (decoder thread) ---------------------------------------------------------------- */
/* a free output buffer, waiting for the client to release one; -1 when flushing or quitting */
static int take_out(um_codec *c)
{
    for (;;) {
        if (c->flushing || c->quit) return -1;
        for (int i = 0; i < c->nout; i++)
            if (c->out[i].state == OUT_FREE) { c->out[i].state = OUT_FILLING; return i; }
        pthread_cond_wait(&c->cv, &c->mu);
    }
}

static void ready_out(um_codec *c, int i)
{
    c->out[i].state = OUT_READY;
    c->out[i].seq = ++c->out_seq;
    pthread_cond_broadcast(&c->cv);
}

/* the decoder's output format changed: the next ready buffer announces it */
static um_format *format_change(um_codec *c, um_format *nf)
{
    um_format_free(c->cur_fmt);
    c->cur_fmt = nf;
    return um_format_copy(nf);
}

static int emit_video(void *cp, const uint8_t *y, int ys, const uint8_t *u, int us, const uint8_t *v, int vs,
                      int width, int height, int64_t pts)
{
    um_codec *c = cp;
    um_frame *f = um_frame_get(width, height);
    if (!f) return -1;
    f->pts_us = pts;
    pthread_mutex_lock(&c->mu);
    int to_surface = c->surface != NULL;
    pthread_mutex_unlock(&c->mu);
    if (to_surface) um_frame_fill(f, y, ys, u, us, v, vs);
    pthread_mutex_lock(&c->mu);
    int32_t ow = -1, oh = -1;
    um_format *announce = NULL;
    if (!c->cur_fmt || !um_format_get_int32(c->cur_fmt, UM_KEY_WIDTH, &ow) ||
        !um_format_get_int32(c->cur_fmt, UM_KEY_HEIGHT, &oh) || ow != width || oh != height) {
        um_format *nf = c->cur_fmt ? um_format_copy(c->cur_fmt) : um_format_copy(c->in_fmt);
        um_format_set_int32(nf, UM_KEY_WIDTH, width);
        um_format_set_int32(nf, UM_KEY_HEIGHT, height);
        um_format_set_int32(nf, UM_KEY_STRIDE, f->pw);
        um_format_set_int32(nf, UM_KEY_SLICE_HEIGHT, f->ph);
        um_format_set_int32(nf, "crop-left", 0);
        um_format_set_int32(nf, "crop-top", 0);
        um_format_set_int32(nf, "crop-right", width - 1);
        um_format_set_int32(nf, "crop-bottom", height - 1);
        um_format_set_int32(nf, UM_KEY_COLOR_FORMAT, 19);    /* COLOR_FormatYUV420Planar */
        um_format_remove(nf, "csd-0"); um_format_remove(nf, "csd-1");
        announce = format_change(c, nf);
        um_log("codec %s: output %dx%d", c->ent->name, width, height);
    }
    int i = take_out(c);
    if (i < 0) { pthread_mutex_unlock(&c->mu); um_frame_put(f); um_format_free(announce); return -1; }
    outbuf *o = &c->out[i];
    size_t fsize = (size_t)f->pw * (f->ph + f->ph / 2);
    if (!to_surface) {
        /* the picture into the buffer's own memory (its ByteBuffer is fixed); a bigger picture
           than configured moves the buffer, which the client is told */
        if (fsize > o->cap) {
            uint8_t *nb = realloc(o->data, fsize);
            if (!nb) { o->state = OUT_FREE; pthread_mutex_unlock(&c->mu); um_frame_put(f); um_format_free(announce); return -1; }
            o->data = nb; o->cap = fsize;
            c->buffers_changed = 1;
        }
        uint8_t *keep = f->data;
        f->data = o->data;
        um_frame_fill(f, y, ys, u, us, v, vs);
        f->data = keep;
        um_frame_put(f);
        f = NULL;
    }
    o->frame = f;
    o->new_fmt = announce;
    o->info.offset = 0;
    o->info.size = to_surface ? 0 : (int32_t)fsize;
    o->info.pts_us = pts;
    o->info.flags = 0;
    ready_out(c, i);
    pthread_mutex_unlock(&c->mu);
    UM_TRACE("codec %s: frame %lld", c->ent->name, (long long)pts);
    return 0;
}

static int emit_audio(void *cp, const int16_t *pcm, int frames, int channels, int rate, int64_t pts_in)
{
    um_codec *c = cp;
    pthread_mutex_lock(&c->mu);
    int32_t orate = -1, och = -1;
    um_format *announce = NULL;
    if (!c->cur_fmt || !um_format_get_int32(c->cur_fmt, UM_KEY_SAMPLE_RATE, &orate) ||
        !um_format_get_int32(c->cur_fmt, UM_KEY_CHANNEL_COUNT, &och) || orate != rate || och != channels) {
        um_format *nf = c->cur_fmt ? um_format_copy(c->cur_fmt) : um_format_copy(c->in_fmt);
        um_format_set_string(nf, UM_KEY_MIME, UM_MIME_RAW);
        um_format_set_int32(nf, UM_KEY_SAMPLE_RATE, rate);
        um_format_set_int32(nf, UM_KEY_CHANNEL_COUNT, channels);
        um_format_set_int32(nf, UM_KEY_PCM_ENCODING, 2);     /* AudioFormat.ENCODING_PCM_16BIT */
        for (int k = 0; k < 3; k++) { char key[8]; snprintf(key, sizeof key, "csd-%d", k); um_format_remove(nf, key); }
        announce = format_change(c, nf);
        um_log("codec %s: output %d Hz, %d channels", c->ent->name, rate, channels);
    }
    size_t bytes_per = (size_t)channels * 2;
    int done = 0;
    while (done < frames) {
        int i = take_out(c);
        if (i < 0) { um_format_free(announce); pthread_mutex_unlock(&c->mu); return -1; }
        outbuf *o = &c->out[i];
        int n = frames - done;
        if ((size_t)n * bytes_per > o->cap) n = (int)(o->cap / bytes_per);
        memcpy(o->data, pcm + (size_t)done * channels, (size_t)n * bytes_per);
        o->new_fmt = announce;
        announce = NULL;
        o->info.offset = 0;
        o->info.size = (int32_t)(n * bytes_per);
        o->info.pts_us = pts_in + (rate > 0 ? (c->audio_frames_done + done) * 1000000LL / rate : 0);
        o->info.flags = 0;
        ready_out(c, i);
        done += n;
    }
    c->audio_frames_done += frames;
    pthread_mutex_unlock(&c->mu);
    return 0;
}

static void emit_eos(um_codec *c)
{
    int i = take_out(c);
    if (i < 0) return;
    outbuf *o = &c->out[i];
    o->frame = NULL;
    o->new_fmt = NULL;
    o->info.offset = 0;
    o->info.size = 0;
    o->info.pts_us = c->cur_pts;
    o->info.flags = UM_BUFFER_FLAG_END_OF_STREAM;
    ready_out(c, i);
}

static void *decode_thread(void *p)
{
    um_codec *c = p;
    char nm[16];
    snprintf(nm, sizeof nm, "um-%s", c->video ? "video" : "audio");
    pthread_setname_np(pthread_self(), nm);
    pthread_mutex_lock(&c->mu);
    while (!c->quit) {
        if (!c->inq_n || c->flushing || c->state != ST_RUNNING) { pthread_cond_wait(&c->cv, &c->mu); continue; }
        int i = c->inq[c->inq_head];
        c->inq_head = (c->inq_head + 1) % MAX_IN;
        c->inq_n--;
        inbuf *b = &c->in[i];
        c->busy = 1;
        c->cur_pts = b->pts;
        c->audio_frames_done = 0;
        pthread_mutex_unlock(&c->mu);
        uint32_t fl = b->flags;
        if (b->size > 0 || (fl & UM_BUFFER_FLAG_END_OF_STREAM)) {
            int64_t t0 = um_tracing() ? um_now_us() : 0;
            c->dec->decode(c->dec, b->data + b->off, b->size, b->pts, fl, &c->emit);
            if (t0) UM_TRACE("codec %s: input %d bytes pts %lld: %lld us", c->ent->name, b->size, (long long)b->pts,
                             (long long)(um_now_us() - t0));
        }
        pthread_mutex_lock(&c->mu);
        if ((fl & UM_BUFFER_FLAG_END_OF_STREAM) && !c->flushing) { emit_eos(c); c->state = ST_EOS; }
        b->state = IN_CODEC;
        c->busy = 0;
        pthread_cond_broadcast(&c->cv);
    }
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

/* ---- client side ------------------------------------------------------------------------------- */
static void free_buffers(um_codec *c)
{
    for (int i = 0; i < c->nin; i++) free(c->in[i].data);
    for (int i = 0; i < c->nout; i++) {
        free(c->out[i].data);
        um_frame_put(c->out[i].frame);
        um_format_free(c->out[i].new_fmt);
    }
    memset(c->in, 0, sizeof c->in);
    memset(c->out, 0, sizeof c->out);
    c->nin = c->nout = 0;
}

int um_codec_configure(um_codec *c, const um_format *f, um_surface *surface)
{
    if (!c || !f) return UM_ERR_INVALID;
    pthread_mutex_lock(&c->mu);
    if (c->state != ST_CREATED) { pthread_mutex_unlock(&c->mu); return UM_ERR_STATE; }
    um_format_free(c->in_fmt);
    c->in_fmt = um_format_copy(f);
    um_format *out = um_format_new();
    int r = c->dec->configure(c->dec, f, out);
    if (r != UM_OK) {
        um_format_free(out);
        pthread_mutex_unlock(&c->mu);
        um_log("codec %s: configure failed (%d)", c->ent->name, r);
        return r;
    }
    um_format_free(c->out_fmt);
    c->out_fmt = um_format_copy(f);              /* until the first output: the input's */
    for (int i = 0; i < um_format_count(out); i++) {
        const char *k = um_format_key(out, i);
        int32_t v;
        if (um_format_get_int32(out, k, &v)) um_format_set_int32(c->out_fmt, k, v);
    }
    um_format_free(out);
    if (c->surface) um_surface_unref(c->surface);
    c->surface = c->video && surface ? um_surface_ref(surface) : NULL;
    int32_t w = 0, h = 0, mis = 0;
    um_format_get_int32(f, UM_KEY_WIDTH, &w);
    um_format_get_int32(f, UM_KEY_HEIGHT, &h);
    um_format_get_int32(f, UM_KEY_MAX_INPUT_SIZE, &mis);
    size_t icap = c->video ? (size_t)(w > 0 && h > 0 ? w * h * 3 / 4 : 1 << 20) : 64 * 1024;
    if (c->video && icap < 256 * 1024) icap = 256 * 1024;
    if (c->video && icap > 4 << 20) icap = 4 << 20;
    if (mis > 0) icap = (size_t)mis;              /* the extractor's largest sample of the track */
    if (icap < 4096) icap = 4096;
    free_buffers(c);
    c->nin = c->video ? 4 : 8;
    c->nout = c->video ? 4 : 4;
    for (int i = 0; i < c->nin; i++) {
        c->in[i].data = malloc(icap);
        c->in[i].cap = c->in[i].data ? icap : 0;
        c->in[i].state = IN_CODEC;
    }
    size_t ocap = !c->video ? AUDIO_OUT_CAP : c->surface ? 64 :
                  w > 0 && h > 0 ? (size_t)((w + 1) & ~1) * (((h + 1) & ~1) * 3 / 2) : 64;
    for (int i = 0; i < c->nout; i++) {
        c->out[i].data = malloc(ocap);
        c->out[i].cap = c->out[i].data ? ocap : 0;
        c->out[i].state = OUT_FREE;
    }
    c->buffers_changed = 0;
    c->emit.video = emit_video;
    c->emit.audio = emit_audio;
    c->emit.codec = c;
    c->state = ST_CONFIGURED;
    pthread_mutex_unlock(&c->mu);
    char *s = um_format_to_string(f);
    um_log("codec %s: configured %s%s, input buffers %zu bytes", c->ent->name, s ? s : "", c->surface ? " with a surface" : "", icap);
    free(s);
    return UM_OK;
}

int um_codec_set_surface(um_codec *c, um_surface *surface)
{
    if (!c || !c->video) return UM_ERR_INVALID;
    pthread_mutex_lock(&c->mu);
    um_surface *old = c->surface;
    c->surface = surface ? um_surface_ref(surface) : NULL;
    pthread_mutex_unlock(&c->mu);
    if (old) um_surface_unref(old);
    return UM_OK;
}

int um_codec_start(um_codec *c)
{
    if (!c) return UM_ERR_INVALID;
    pthread_mutex_lock(&c->mu);
    if (c->state != ST_CONFIGURED) { pthread_mutex_unlock(&c->mu); return UM_ERR_STATE; }
    c->state = ST_RUNNING;
    c->quit = 0;
    if (!c->th_on && pthread_create(&c->th, NULL, decode_thread, c) == 0) c->th_on = 1;
    pthread_mutex_unlock(&c->mu);
    return c->th_on ? UM_OK : UM_ERR_UNKNOWN;
}

/* return every buffer to the codec and drop queued work; the decoder thread is idle afterwards */
static void drain_locked(um_codec *c)
{
    c->flushing = 1;
    pthread_cond_broadcast(&c->cv);
    while (c->busy) pthread_cond_wait(&c->cv, &c->mu);
    c->inq_n = c->inq_head = 0;
    for (int i = 0; i < c->nin; i++) c->in[i].state = IN_CODEC;
    for (int i = 0; i < c->nout; i++) {
        um_frame_put(c->out[i].frame);
        c->out[i].frame = NULL;
        um_format_free(c->out[i].new_fmt);
        c->out[i].new_fmt = NULL;
        c->out[i].state = OUT_FREE;
    }
    /* a format change the client was never told about is announced again with the next output */
    um_format_free(c->cur_fmt);
    c->cur_fmt = c->out_fmt && (um_format_type(c->out_fmt, UM_KEY_STRIDE) || um_format_type(c->out_fmt, UM_KEY_PCM_ENCODING))
               ? um_format_copy(c->out_fmt) : NULL;
    c->flushing = 0;
}

int um_codec_flush(um_codec *c)
{
    if (!c) return UM_ERR_INVALID;
    pthread_mutex_lock(&c->mu);
    if (c->state != ST_RUNNING && c->state != ST_EOS) { pthread_mutex_unlock(&c->mu); return UM_ERR_STATE; }
    drain_locked(c);
    c->dec->flush(c->dec);
    c->state = ST_RUNNING;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    UM_TRACE("codec %s: flush", c->ent->name);
    return UM_OK;
}

static void join_thread(um_codec *c)
{
    pthread_mutex_lock(&c->mu);
    drain_locked(c);
    c->quit = 1;
    pthread_cond_broadcast(&c->cv);
    int on = c->th_on;
    c->th_on = 0;
    pthread_mutex_unlock(&c->mu);
    if (on) pthread_join(c->th, NULL);
}

int um_codec_stop(um_codec *c)
{
    if (!c) return UM_ERR_INVALID;
    join_thread(c);
    pthread_mutex_lock(&c->mu);
    c->dec->flush(c->dec);
    /* Android: stop() returns the codec to Uninitialized; it is configured again before a start */
    c->state = ST_CREATED;
    um_format_free(c->cur_fmt);
    c->cur_fmt = NULL;
    pthread_mutex_unlock(&c->mu);
    return UM_OK;
}

void um_codec_free(um_codec *c)
{
    if (!c) return;
    join_thread(c);
    c->dec->destroy(c->dec);
    free_buffers(c);
    if (c->surface) um_surface_unref(c->surface);
    um_format_free(c->in_fmt);
    um_format_free(c->out_fmt);
    um_format_free(c->cur_fmt);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    free(c);
}

int um_codec_input_count(um_codec *c) { return c ? c->nin : 0; }
int um_codec_output_count(um_codec *c) { return c ? c->nout : 0; }

uint8_t *um_codec_input_buffer(um_codec *c, int i, size_t *cap)
{
    if (!c || i < 0 || i >= c->nin) { if (cap) *cap = 0; return NULL; }
    if (cap) *cap = c->in[i].cap;
    return c->in[i].data;
}

uint8_t *um_codec_output_buffer(um_codec *c, int i, size_t *cap)
{
    if (!c || i < 0 || i >= c->nout) { if (cap) *cap = 0; return NULL; }
    pthread_mutex_lock(&c->mu);
    uint8_t *p = c->out[i].data;
    size_t n = c->out[i].cap;
    pthread_mutex_unlock(&c->mu);
    if (cap) *cap = n;
    return p;
}

int um_codec_dequeue_input(um_codec *c, int64_t timeout_us)
{
    if (!c) return UM_ERR_INVALID;
    pthread_mutex_lock(&c->mu);
    int64_t dl = deadline_us(timeout_us);
    int r = UM_INFO_TRY_AGAIN_LATER;
    for (;;) {
        if (c->state != ST_RUNNING && c->state != ST_EOS) { r = UM_ERR_STATE; break; }
        int found = -1;
        for (int i = 0; i < c->nin; i++) if (c->in[i].state == IN_CODEC) { found = i; break; }
        if (found >= 0 && c->state == ST_RUNNING) { c->in[found].state = IN_CLIENT; r = found; break; }
        if (!wait_until(c, dl)) break;
    }
    pthread_mutex_unlock(&c->mu);
    return r;
}

int um_codec_queue_input(um_codec *c, int i, int offset, int size, int64_t pts, uint32_t flags)
{
    if (!c) return UM_ERR_INVALID;
    pthread_mutex_lock(&c->mu);
    int r = UM_OK;
    if (c->state != ST_RUNNING) r = UM_ERR_STATE;
    else if (i < 0 || i >= c->nin || c->in[i].state != IN_CLIENT) r = UM_ERR_INVALID;
    else if (offset < 0 || size < 0 || (size_t)offset + size > c->in[i].cap) r = UM_ERR_INVALID;
    if (r == UM_OK) {
        inbuf *b = &c->in[i];
        b->off = offset; b->size = size; b->pts = pts; b->flags = flags;
        b->state = IN_QUEUED;
        c->inq[(c->inq_head + c->inq_n) % MAX_IN] = i;
        c->inq_n++;
        pthread_cond_broadcast(&c->cv);
    }
    pthread_mutex_unlock(&c->mu);
    return r;
}

int um_codec_dequeue_output(um_codec *c, um_buffer_info *info, int64_t timeout_us)
{
    if (!c) return UM_ERR_INVALID;
    pthread_mutex_lock(&c->mu);
    int64_t dl = deadline_us(timeout_us);
    int r = UM_INFO_TRY_AGAIN_LATER;
    for (;;) {
        if (c->state != ST_RUNNING && c->state != ST_EOS) { r = UM_ERR_STATE; break; }
        int best = -1;
        for (int i = 0; i < c->nout; i++)
            if (c->out[i].state == OUT_READY && (best < 0 || c->out[i].seq < c->out[best].seq)) best = i;
        if (best >= 0) {
            outbuf *o = &c->out[best];
            if (c->buffers_changed) {
                c->buffers_changed = 0;
                r = UM_INFO_OUTPUT_BUFFERS_CHANGED;
            } else if (o->new_fmt) {
                um_format_free(c->out_fmt);
                c->out_fmt = o->new_fmt;
                o->new_fmt = NULL;
                r = UM_INFO_OUTPUT_FORMAT_CHANGED;
            } else {
                o->state = OUT_CLIENT;
                if (info) *info = o->info;
                r = best;
            }
            break;
        }
        if (!wait_until(c, dl)) break;
    }
    pthread_mutex_unlock(&c->mu);
    return r;
}

um_format *um_codec_output_format(um_codec *c)
{
    if (!c) return NULL;
    pthread_mutex_lock(&c->mu);
    um_format *f = um_format_copy(c->out_fmt);
    pthread_mutex_unlock(&c->mu);
    return f;
}

um_format *um_codec_input_format(um_codec *c)
{
    if (!c) return NULL;
    pthread_mutex_lock(&c->mu);
    um_format *f = um_format_copy(c->in_fmt);
    pthread_mutex_unlock(&c->mu);
    return f;
}

int um_codec_release_output(um_codec *c, int i, int render)
{
    if (!c) return UM_ERR_INVALID;
    pthread_mutex_lock(&c->mu);
    if (i < 0 || i >= c->nout || c->out[i].state != OUT_CLIENT) {
        pthread_mutex_unlock(&c->mu);
        return UM_ERR_INVALID;
    }
    um_frame *f = c->out[i].frame;
    c->out[i].frame = NULL;
    c->out[i].state = OUT_FREE;
    um_surface *s = render && f && c->surface ? um_surface_ref(c->surface) : NULL;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    if (s) {
        um_surface_queue(s, f);
        um_surface_unref(s);
    } else {
        um_frame_put(f);
    }
    return UM_OK;
}
