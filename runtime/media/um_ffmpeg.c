// Decoders of the system's FFmpeg (libavcodec, LGPL; loaded with dlopen when the handheld's
// firmware has it, or a port's own build through UM_FFMPEG=<path>; this runtime never ships or
// links one): AAC and MP3 audio (macOS / Windows builds' clips),
// and H.264 / HEVC video that OpenH264 does not take. libavcodec majors 58 - 61 (FFmpeg 4 - 7):
// only functions and the leading fields of AVCodec, AVCodecParameters, AVPacket and AVFrame are
// used, which are the same in all of them; sample rate and channels come from the stream's own
// configuration (AudioSpecificConfig) or the "ar" / "ac" options.
// UM_FFMPEG=0 keeps it off; UM_FFMPEG=<path> loads that libavcodec.
// UM_FFMPEG_SKIP_LOOP_FILTER (video): the deblocking filter libavcodec skips, as its skip_loop_filter
// option: none, nonref, bidir, nonintra, nonkey, all; auto (the default): none until the client keeps
// releasing pictures unshown because they come too late (um_codec.c: um_dec.behind), then all, from the
// next packet on, and from the start of every later video of the run. 1080p H.264 on a TrimUI Smart
// Pro under a game: 10.8 pictures a second shown and the player seeking ahead every ~2 s (its audio
// decoder made again each time); without deblocking 21.6 and no seeks (the filter is a large share of
// H.264's decoding time on in-order ARM cores; its blocks hardly show at handheld sizes).
#define _GNU_SOURCE
#include "um_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef UM_HAVE_FFMPEG
/* the stable heads of FFmpeg's structures */
typedef struct { const char *name, *long_name; int type, id; } ff_codec;
typedef struct { int codec_type, codec_id; uint32_t codec_tag; uint8_t *extradata; int extradata_size; } ff_par;
typedef struct { void *buf; int64_t pts, dts; uint8_t *data; int size, stream_index, flags; } ff_packet;
typedef struct { uint8_t *data[8]; int linesize[8]; uint8_t **extended_data; int width, height, nb_samples, format; } ff_frame;
#define FF_EAGAIN (-EAGAIN)

static struct {
    int tried, ok, major;
    ff_codec *(*find_decoder_by_name)(const char *);
    void *(*alloc_context3)(const ff_codec *);
    ff_par *(*parameters_alloc)(void);
    void (*parameters_free)(ff_par **);
    int (*parameters_to_context)(void *, const ff_par *);
    int (*open2)(void *, const ff_codec *, void **);
    int (*send_packet)(void *, const ff_packet *);
    int (*receive_frame)(void *, ff_frame *);
    void (*flush_buffers)(void *);
    void (*free_context)(void **);
    ff_packet *(*packet_alloc)(void);
    void (*packet_free)(ff_packet **);
    ff_frame *(*frame_alloc)(void);
    void (*frame_free)(ff_frame **);
    void (*frame_unref)(ff_frame *);
    int (*opt_get_int)(void *, const char *, int, int64_t *);
    int (*opt_set_int)(void *, const char *, int64_t, int);
    void *(*mallocz)(size_t);
    unsigned (*version)(void);
    char desc[32];
} ff;
static pthread_mutex_t ff_mu = PTHREAD_MUTEX_INITIALIZER;

static int ff_load(void)
{
    pthread_mutex_lock(&ff_mu);
    if (ff.tried) { pthread_mutex_unlock(&ff_mu); return ff.ok; }
    ff.tried = 1;
    const char *e = getenv("UM_FFMPEG");
    void *h = NULL;
    if (e && !strcmp(e, "0")) { pthread_mutex_unlock(&ff_mu); return 0; }
    if (e && *e) h = dlopen(e, RTLD_NOW | RTLD_LOCAL);
    static const char *const names[] = { "libavcodec.so.61", "libavcodec.so.60", "libavcodec.so.59", "libavcodec.so.58", NULL };
    for (int i = 0; !h && names[i]; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) { pthread_mutex_unlock(&ff_mu); return 0; }
#define S(f, n) ff.f = dlsym(h, n)
    S(find_decoder_by_name, "avcodec_find_decoder_by_name"); S(alloc_context3, "avcodec_alloc_context3");
    S(parameters_alloc, "avcodec_parameters_alloc"); S(parameters_free, "avcodec_parameters_free");
    S(parameters_to_context, "avcodec_parameters_to_context"); S(open2, "avcodec_open2");
    S(send_packet, "avcodec_send_packet"); S(receive_frame, "avcodec_receive_frame");
    S(flush_buffers, "avcodec_flush_buffers"); S(free_context, "avcodec_free_context");
    S(packet_alloc, "av_packet_alloc"); S(packet_free, "av_packet_free");
    S(frame_alloc, "av_frame_alloc"); S(frame_free, "av_frame_free"); S(frame_unref, "av_frame_unref");
    S(opt_get_int, "av_opt_get_int"); S(opt_set_int, "av_opt_set_int"); S(mallocz, "av_mallocz");
    S(version, "avcodec_version");
#undef S
    ff.ok = ff.find_decoder_by_name && ff.alloc_context3 && ff.parameters_alloc && ff.parameters_free &&
            ff.parameters_to_context && ff.open2 && ff.send_packet && ff.receive_frame && ff.flush_buffers &&
            ff.free_context && ff.packet_alloc && ff.packet_free && ff.frame_alloc && ff.frame_free &&
            ff.frame_unref && ff.opt_get_int && ff.mallocz && ff.version;
    if (ff.ok) {
        ff.major = (int)(ff.version() >> 16);
        if (ff.major < 58 || ff.major > 61) { um_log("ffmpeg: libavcodec %d not handled (58 - 61)", ff.major); ff.ok = 0; }
        else snprintf(ff.desc, sizeof ff.desc, "libavcodec %d", ff.major);
    }
    if (!ff.ok) dlclose(h);
    pthread_mutex_unlock(&ff_mu);
    if (ff.ok) um_log("ffmpeg: %s (the system's) for AAC / MP3 and H.264 / HEVC OpenH264 does not take", ff.desc);
    return ff.ok;
}

int um_ffmpeg_available(void) { return ff_load(); }
const char *um_ffmpeg_version(void) { return ff_load() ? ff.desc : NULL; }
int um_ffmpeg_demux_open(um_source *src, um_demux *d) { (void)src; (void)d; return UM_ERR_UNSUPPORTED; }

typedef struct {
    um_dec base;
    const ff_codec *codec;
    void *ctx;
    ff_packet *pkt;
    ff_frame *frm;
    int video, rate, channels;
    int64_t *pts;               /* video: pending input times, ascending (frames come out in this order) */
    int npts, cappts;
    int16_t *pcm;
    size_t pcmn;
    int warned;
    int skip_lf;                /* video: the skip_loop_filter level set now (AVDISCARD_*) */
    volatile int behind;        /* video: 1 = the client said it falls behind (any thread), 2 = acted on (decode thread) */
} ffdec;

#define AVDISCARD_ALL 48
static volatile int g_fell_behind;          /* a video of this run fell behind: later ones start without deblocking */

/* UM_FFMPEG_SKIP_LOOP_FILTER: an AVDISCARD level, or -1 for auto */
static int skip_lf_mode(void)
{
    static int mode = -2;
    if (mode != -2) return mode;
    const char *e = getenv("UM_FFMPEG_SKIP_LOOP_FILTER");
    static const struct { const char *n; int v; } names[] = {
        { "auto", -1 }, { "none", 0 }, { "default", 0 }, { "nonref", 8 }, { "bidir", 16 }, { "nonintra", 24 },
        { "nonkey", 32 }, { "all", AVDISCARD_ALL },
    };
    mode = -1;
    if (e && *e) {
        int found = 0;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!strcmp(e, names[i].n)) { mode = names[i].v; found = 1; }
        if (!found) um_log("ffmpeg: UM_FFMPEG_SKIP_LOOP_FILTER=%s not known (auto, none, nonref, bidir, nonintra, nonkey, all): auto", e);
    }
    return mode;
}

static void set_skip_lf(ffdec *f, int level)
{
    if (ff.opt_set_int && ff.opt_set_int(f->ctx, "skip_loop_filter", level, 0) >= 0) f->skip_lf = level;
}

static void f_behind(um_dec *d)
{
    ffdec *f = (ffdec *)d;
    if (f->video && skip_lf_mode() == -1 && f->skip_lf < AVDISCARD_ALL && !f->behind) f->behind = 1;
}

static const char *ff_name(const char *mime)
{
    if (!strcmp(mime, UM_MIME_AVC)) return "h264";
    if (!strcmp(mime, UM_MIME_HEVC)) return "hevc";
    if (!strcmp(mime, UM_MIME_AAC)) return "aac";
    if (!strcmp(mime, UM_MIME_MPEG)) return "mp3float";
    return NULL;
}

static void close_ctx(ffdec *f)
{
    if (f->ctx) ff.free_context(&f->ctx);
    f->ctx = NULL;
}

static int f_configure(um_dec *d, const um_format *in, um_format *out)
{
    ffdec *f = (ffdec *)d;
    close_ctx(f);
    f->ctx = ff.alloc_context3(f->codec);
    if (!f->ctx) return UM_ERR_UNKNOWN;
    ff_par *par = ff.parameters_alloc();
    if (!par) return UM_ERR_UNKNOWN;
    par->codec_type = f->codec->type;
    par->codec_id = f->codec->id;
    /* extradata: AAC's AudioSpecificConfig, or H.264 / HEVC parameter sets (Annex B) */
    uint8_t ex[4096];
    size_t exn = 0, n;
    for (int k = 0; k < 3; k++) {
        char key[8];
        snprintf(key, sizeof key, "csd-%d", k);
        const void *c = um_format_get_buffer(in, key, &n);
        if (!c || (f->video == 0 && k > 0)) continue;
        if (exn + n > sizeof ex) break;
        memcpy(ex + exn, c, n);
        exn += n;
    }
    if (exn) {
        par->extradata = ff.mallocz(exn + 64);
        if (par->extradata) { memcpy(par->extradata, ex, exn); par->extradata_size = (int)exn; }
    }
    int r = ff.parameters_to_context(f->ctx, par);
    ff.parameters_free(&par);
    if (r < 0) return UM_ERR_UNKNOWN;
    if (f->video && ff.opt_set_int) { int t = um_cpu_count(); ff.opt_set_int(f->ctx, "threads", t > 4 ? 4 : t, 0); }
    f->skip_lf = 0;
    if (f->video) {
        int m = skip_lf_mode();
        if (m > 0) set_skip_lf(f, m);
        else if (m == -1 && g_fell_behind) {
            set_skip_lf(f, AVDISCARD_ALL);
            um_log("ffmpeg: %s starts without its loop filter (a video before fell behind; UM_FFMPEG_SKIP_LOOP_FILTER=auto)", f->codec->name);
        }
    }
    if (ff.open2(f->ctx, f->codec, NULL) < 0) { um_log("ffmpeg: cannot open the %s decoder", f->codec->name); return UM_ERR_UNSUPPORTED; }
    int32_t v;
    f->rate = um_format_get_int32(in, UM_KEY_SAMPLE_RATE, &v) ? v : 0;
    f->channels = um_format_get_int32(in, UM_KEY_CHANNEL_COUNT, &v) ? v : 0;
    if (!f->video && f->rate && f->channels) {
        um_format_set_int32(out, UM_KEY_SAMPLE_RATE, f->rate);
        um_format_set_int32(out, UM_KEY_CHANNEL_COUNT, f->channels);
    }
    f->npts = 0;
    return UM_OK;
}

static void push_pts(ffdec *f, int64_t p)
{
    if (f->npts == f->cappts) {
        int nc = f->cappts ? f->cappts * 2 : 32;
        int64_t *np = realloc(f->pts, nc * sizeof *np);
        if (!np) return;
        f->pts = np; f->cappts = nc;
    }
    int i = f->npts++;
    while (i > 0 && f->pts[i - 1] > p) { f->pts[i] = f->pts[i - 1]; i--; }
    f->pts[i] = p;
}

static int64_t pop_pts(ffdec *f)
{
    if (!f->npts) return 0;
    int64_t p = f->pts[0];
    memmove(f->pts, f->pts + 1, (size_t)--f->npts * sizeof *f->pts);
    return p;
}

static float sample_at(const ff_frame *fr, int fmt, int ch, int channels, int i)
{
    int planar = fmt >= 5;
    int base = planar ? fmt - 5 : fmt;
    const uint8_t *p = planar ? fr->extended_data[ch] : fr->extended_data[0];
    int idx = planar ? i : i * channels + ch;
    switch (base) {
    case 0: return (p[idx] - 128) / 128.0f;
    case 1: return ((const int16_t *)p)[idx] / 32768.0f;
    case 2: return ((const int32_t *)p)[idx] / 2147483648.0f;
    case 3: return ((const float *)p)[idx];
    case 4: return (float)((const double *)p)[idx];
    }
    return 0;
}

static void drain(ffdec *f, int64_t pts_in, um_emit *em)
{
    for (;;) {
        int r = ff.receive_frame(f->ctx, f->frm);
        if (r < 0) break;
        ff_frame *fr = f->frm;
        if (f->video) {
            int64_t p = pop_pts(f);
            if (fr->format == 0 || fr->format == 12)            /* yuv420p, yuvj420p */
                em->video(em->codec, fr->data[0], fr->linesize[0], fr->data[1], fr->linesize[1], fr->data[2], fr->linesize[2],
                          fr->width, fr->height, p);
            else if (!f->warned++) um_log("ffmpeg: picture format %d not handled", fr->format);
        } else {
            int64_t v;
            int ch = f->channels, rate = f->rate;
            if (ff.opt_get_int(f->ctx, "ac", 0, &v) >= 0 && v > 0) ch = (int)v;
            if (ff.opt_get_int(f->ctx, "ar", 0, &v) >= 0 && v > 0) rate = (int)v;
            int fmt = fr->format;
            if (ch <= 0 || ch > 8 || rate <= 0 || fmt < 0 || fmt > 9) { if (!f->warned++) um_log("ffmpeg: audio layout unknown"); ff.frame_unref(fr); continue; }
            size_t need = (size_t)fr->nb_samples * ch;
            if (need > f->pcmn) { int16_t *t = realloc(f->pcm, need * 2); if (!t) break; f->pcm = t; f->pcmn = need; }
            for (int i = 0; i < fr->nb_samples; i++)
                for (int c = 0; c < ch; c++) {
                    float x = sample_at(fr, fmt, c, ch, i) * 32768.0f;
                    f->pcm[(size_t)i * ch + c] = x >= 32767 ? 32767 : x <= -32768 ? -32768 : (int16_t)lrintf(x);
                }
            em->audio(em->codec, f->pcm, fr->nb_samples, ch, rate, pts_in);
        }
        ff.frame_unref(fr);
    }
}

static int f_decode(um_dec *d, const uint8_t *data, size_t size, int64_t pts, uint32_t flags, um_emit *em)
{
    ffdec *f = (ffdec *)d;
    if (!f->ctx || (flags & UM_BUFFER_FLAG_CODEC_CONFIG)) return UM_OK;
    if (f->behind == 1) {               /* (here, on the decode thread: libavcodec's frame threads take it from the next packet) */
        f->behind = 2;
        set_skip_lf(f, AVDISCARD_ALL);
        g_fell_behind = 1;
        um_log("ffmpeg: %s falls behind: its loop filter (deblocking) is skipped from here on (UM_FFMPEG_SKIP_LOOP_FILTER=auto)%s",
               f->codec->name, f->skip_lf == AVDISCARD_ALL ? "" : "; setting it failed");
    }
    if (size) {
        f->pkt->data = (uint8_t *)data;
        f->pkt->size = (int)size;
        f->pkt->pts = pts;
        f->pkt->dts = pts;
        if (f->video) push_pts(f, pts);
        int r = ff.send_packet(f->ctx, f->pkt);
        f->pkt->data = NULL; f->pkt->size = 0;
        if (r < 0 && r != FF_EAGAIN && f->warned++ < 5) um_log("ffmpeg: send_packet %d at %lld us", r, (long long)pts);
        drain(f, pts, em);
    }
    if (flags & UM_BUFFER_FLAG_END_OF_STREAM) {
        ff.send_packet(f->ctx, NULL);
        drain(f, pts, em);
    }
    return UM_OK;
}

static void f_flush(um_dec *d)
{
    ffdec *f = (ffdec *)d;
    if (f->ctx) ff.flush_buffers(f->ctx);
    f->npts = 0;
}

static void f_destroy(um_dec *d)
{
    ffdec *f = (ffdec *)d;
    close_ctx(f);
    if (f->pkt) ff.packet_free(&f->pkt);
    if (f->frm) ff.frame_free(&f->frm);
    free(f->pts);
    free(f->pcm);
    free(f);
}

um_dec *um_ffmpeg_create(const char *mime)
{
    const char *name = ff_name(mime);
    if (!name || !ff_load()) return NULL;
    ff_codec *codec = ff.find_decoder_by_name(name);
    if (!codec && !strcmp(name, "mp3float")) codec = ff.find_decoder_by_name("mp3");
    if (!codec) return NULL;
    ffdec *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->codec = codec;
    f->video = !strncmp(mime, "video/", 6);
    f->pkt = ff.packet_alloc();
    f->frm = ff.frame_alloc();
    if (!f->pkt || !f->frm) { f_destroy(&f->base); return NULL; }
    f->base.name = ff.desc;
    f->base.configure = f_configure;
    f->base.decode = f_decode;
    f->base.flush = f_flush;
    f->base.destroy = f_destroy;
    f->base.behind = f_behind;
    return &f->base;
}
#endif
