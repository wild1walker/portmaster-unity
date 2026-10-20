// Unit tests of the media core on clips made here: frames and tones encoded with the encoders of
// the static libraries the core links (libvpx VP8/VP9, libvorbisenc, libopus, OpenH264), muxed by
// test/webm_writer.c and test/mp4_writer.c, then demuxed and decoded through um_extractor /
// um_codec exactly as the Java and NDK layers drive them (Unity's call pattern: select tracks,
// dequeue/queue input, dequeue/release output, seek + flush, end of stream).
#define _GNU_SOURCE
#include "../um.h"
#include "webm_writer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#include <vorbis/vorbisenc.h>
#include <opus/opus.h>
#ifdef UM_HAVE_H264
#include <wels/codec_api.h>
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- synthetic content -------------------------------------------------------------------------- */
static uint8_t pix_y(int x, int y, int f) { int v = 40 + (x * 3 + y * 2 + f * 5) % 160; if (x > 10 + f && x < 30 + f && y > 8 && y < 24) v = 230; return v; }
static uint8_t pix_u(int x, int y, int f) { return 64 + (x * 4 + f) % 128; }
static uint8_t pix_v(int x, int y, int f) { return 200 - (y * 4 + f) % 128; }

typedef struct { uint8_t *data; size_t size; int64_t ms; int key; } packet;
typedef struct { packet *p; int n, cap; } plist;
static void padd(plist *l, const void *d, size_t n, int64_t ms, int key)
{
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->p = realloc(l->p, l->cap * sizeof *l->p); }
    packet *p = &l->p[l->n++];
    p->data = malloc(n ? n : 1); memcpy(p->data, d, n); p->size = n; p->ms = ms; p->key = key;
}

static plist encode_vpx(int vp9, int w, int h, int frames)
{
    plist l = { 0 };
    vpx_codec_iface_t *ifc = vp9 ? vpx_codec_vp9_cx() : vpx_codec_vp8_cx();
    vpx_codec_enc_cfg_t cfg;
    vpx_codec_enc_config_default(ifc, &cfg, 0);
    cfg.g_w = w; cfg.g_h = h;
    cfg.g_timebase.num = 1; cfg.g_timebase.den = 1000;
    cfg.rc_target_bitrate = 4000;
    cfg.rc_min_quantizer = 0; cfg.rc_max_quantizer = 8;
    cfg.kf_max_dist = 10; cfg.kf_min_dist = 10;
    cfg.g_lag_in_frames = 0;
    cfg.g_threads = 1;
    vpx_codec_ctx_t enc;
    if (vpx_codec_enc_init(&enc, ifc, &cfg, 0)) { printf("encoder init failed\n"); exit(1); }
    vpx_image_t img;
    vpx_img_alloc(&img, VPX_IMG_FMT_I420, w, h, 1);
    for (int f = 0; f <= frames; f++) {
        if (f < frames) {
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) img.planes[0][y * img.stride[0] + x] = pix_y(x, y, f);
            for (int y = 0; y < (h + 1) / 2; y++) for (int x = 0; x < (w + 1) / 2; x++) {
                img.planes[1][y * img.stride[1] + x] = pix_u(x, y, f);
                img.planes[2][y * img.stride[2] + x] = pix_v(x, y, f);
            }
        }
        int64_t pts = f * 1000 / 30;
        vpx_codec_encode(&enc, f < frames ? &img : NULL, pts, 33, (f % 10 == 0) ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_GOOD_QUALITY);
        vpx_codec_iter_t it = NULL;
        const vpx_codec_cx_pkt_t *pk;
        while ((pk = vpx_codec_get_cx_data(&enc, &it)))
            if (pk->kind == VPX_CODEC_CX_FRAME_PKT)
                padd(&l, pk->data.frame.buf, pk->data.frame.sz, pk->data.frame.pts, (pk->data.frame.flags & VPX_FRAME_IS_KEY) != 0);
    }
    vpx_img_free(&img);
    vpx_codec_destroy(&enc);
    return l;
}

#define AR 48000
static float tone(int ch, int64_t i) { return 0.5f * sinf(2 * (float)M_PI * (ch ? 660 : 440) * i / AR); }

static plist encode_vorbis(int seconds, uint8_t **priv, size_t *privn)
{
    plist l = { 0 };
    vorbis_info vi; vorbis_info_init(&vi);
    vorbis_encode_init_vbr(&vi, 2, AR, 0.5f);
    vorbis_comment vc; vorbis_comment_init(&vc);
    vorbis_dsp_state vd; vorbis_analysis_init(&vd, &vi);
    vorbis_block vb; vorbis_block_init(&vd, &vb);
    ogg_packet h[3];
    vorbis_analysis_headerout(&vd, &vc, &h[0], &h[1], &h[2]);
    wb p = { 0 };
    const uint8_t *pk[3] = { h[0].packet, h[1].packet, h[2].packet };
    size_t pn[3] = { h[0].bytes, h[1].bytes, h[2].bytes };
    wb_xiph_private(&p, pk, pn, 3);
    *priv = p.p; *privn = p.n;
    int64_t total = (int64_t)seconds * AR, done = 0, prev_gp = 0;
    for (;;) {
        if (done < total) {
            int n = 1024;
            float **buf = vorbis_analysis_buffer(&vd, n);
            for (int i = 0; i < n; i++) { buf[0][i] = tone(0, done + i); buf[1][i] = tone(1, done + i); }
            vorbis_analysis_wrote(&vd, n);
            done += n;
        } else vorbis_analysis_wrote(&vd, 0);
        int got = 0;
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);
            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                padd(&l, op.packet, op.bytes, prev_gp * 1000 / AR, 1);
                if (op.granulepos >= 0) prev_gp = op.granulepos;
                got = 1;
                if (op.e_o_s) goto end;
            }
        }
        if (done >= total && !got) break;
    }
end:
    vorbis_block_clear(&vb); vorbis_dsp_clear(&vd); vorbis_comment_clear(&vc); vorbis_info_clear(&vi);
    return l;
}

static plist encode_opus(int seconds, int *preskip)
{
    plist l = { 0 };
    int err;
    OpusEncoder *oe = opus_encoder_create(AR, 2, OPUS_APPLICATION_AUDIO, &err);
    opus_encoder_ctl(oe, OPUS_GET_LOOKAHEAD(preskip));
    int16_t pcm[960 * 2];
    uint8_t out[4000];
    for (int64_t s = 0; s < (int64_t)seconds * AR; s += 960) {
        for (int i = 0; i < 960; i++) { pcm[i * 2] = (int16_t)(tone(0, s + i) * 32767); pcm[i * 2 + 1] = (int16_t)(tone(1, s + i) * 32767); }
        int n = opus_encode(oe, pcm, 960, out, sizeof out);
        padd(&l, out, n, s * 1000 / AR, 1);
    }
    opus_encoder_destroy(oe);
    return l;
}

/* ---- muxing ----------------------------------------------------------------------------------------- */
typedef struct { const char *codec; int w, h; float fps; const uint8_t *priv; size_t privn; uint64_t delay_ns; plist *pk; int audio; } mtrack;

/* clusters of 500 ms; audio packets of the second cluster laced (Xiph, then EBML) when lace */
static wb mux_webm(mtrack *t, int nt, double duration_ms, int lace)
{
    wb out = { 0 };
    wb_header(&out);
    wb seg = { 0 }, info = { 0 }, tracks = { 0 };
    wb_uint(&info, 0x2AD7B1, 1000000);
    wb_float(&info, 0x4489, duration_ms);
    wb_str(&info, 0x4D80, "test"); wb_str(&info, 0x5741, "test");
    wb_master(&seg, 0x1549A966, &info);
    for (int i = 0; i < nt; i++) {
        wb te = { 0 }, sub = { 0 };
        wb_uint(&te, 0xD7, i + 1);
        wb_uint(&te, 0x73C5, i + 100);
        wb_uint(&te, 0x83, t[i].audio ? 2 : 1);
        wb_str(&te, 0x86, t[i].codec);
        if (t[i].priv) wb_elem(&te, 0x63A2, t[i].priv, t[i].privn);
        if (t[i].delay_ns) wb_uint(&te, 0x56AA, t[i].delay_ns);
        if (t[i].audio) {
            wb_float(&sub, 0xB5, AR); wb_uint(&sub, 0x9F, 2);
            wb_master(&te, 0xE1, &sub);
        } else {
            wb_uint(&sub, 0xB0, t[i].w); wb_uint(&sub, 0xBA, t[i].h);
            if (t[i].fps > 0) wb_float4(&sub, 0x2383E3, t[i].fps);
            wb_master(&te, 0xE0, &sub);
        }
        wb_master(&tracks, 0xAE, &te);
    }
    wb_master(&seg, 0x1654AE6B, &tracks);
    int pos[8] = { 0 };
    for (int64_t c0 = 0; ; c0 += 500) {
        int any = 0;
        for (int i = 0; i < nt; i++) if (pos[i] < t[i].pk->n) any = 1;
        if (!any) break;
        wb cl = { 0 };
        wb_uint(&cl, 0xE7, c0);
        for (;;) {           /* interleave by time within the cluster */
            int best = -1;
            for (int i = 0; i < nt; i++)
                if (pos[i] < t[i].pk->n && t[i].pk->p[pos[i]].ms < c0 + 500 &&
                    (best < 0 || t[i].pk->p[pos[i]].ms < t[best].pk->p[pos[best]].ms)) best = i;
            if (best < 0) break;
            packet *p = &t[best].pk->p[pos[best]];
            if (lace && t[best].audio && c0 == 500 && pos[best] + 3 < t[best].pk->n) {
                const uint8_t *fr[4]; size_t sz[4];
                int n = 0;
                while (n < 4 && pos[best] < t[best].pk->n && t[best].pk->p[pos[best]].ms < c0 + 500) {
                    fr[n] = t[best].pk->p[pos[best]].data; sz[n] = t[best].pk->p[pos[best]].size; n++; pos[best]++;
                }
                static int which;
                wb_block(&cl, best + 1, (int)(p->ms - c0), 1, fr, sz, n, (which++ & 1) ? 3 : 1);
                continue;
            }
            const uint8_t *fr = p->data;
            wb_block(&cl, best + 1, (int)(p->ms - c0), p->key, &fr, &p->size, 1, 0);
            pos[best]++;
        }
        wb_master(&seg, 0x1F43B675, &cl);
    }
    wb_master(&out, 0x18538067, &seg);
    return out;
}

/* the clip inside a bigger file at an offset, as a clip inside an APK */
static int write_embedded(const char *path, const wb *clip, int64_t *off)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    uint8_t junk[1234];
    memset(junk, 0x5A, sizeof junk);
    *off = sizeof junk;
    if (write(fd, junk, sizeof junk) != sizeof junk || write(fd, clip->p, clip->n) != (ssize_t)clip->n ||
        write(fd, junk, 100) != 100) { close(fd); return -1; }
    return fd;
}

/* ---- decoding helpers ------------------------------------------------------------------------------- */
typedef struct {
    int frames, formats, eos;
    double psnr_min;
    int64_t first_pts, last_pts, pts[512];
    int64_t audio_frames;
    int16_t *pcm; size_t pcm_n, pcm_cap;
    int rate, channels, w, h;
} dec_result;

static double psnr_y(const uint8_t *packed, int pw, int w, int h, int f)
{
    double se = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { double d = packed[y * pw + x] - pix_y(x, y, f); se += d * d; }
    if (se == 0) return 99;
    return 10 * log10(255.0 * 255 * w * h / se);
}

/* Unity's loop: feed samples of `track` until the extractor runs dry, then EOS; take outputs */
static void run_codec(um_extractor *ex, um_codec *c, int track, dec_result *r, int video, um_surface *surf, int max_out)
{
    memset(r, 0, sizeof *r);
    r->psnr_min = 99;
    r->first_pts = -1;
    int in_eos = 0;
    for (int spins = 0; spins < 20000 && !r->eos; spins++) {
        if (!in_eos) {
            int i = um_codec_dequeue_input(c, 1000);
            if (i >= 0) {
                size_t cap;
                uint8_t *b = um_codec_input_buffer(c, i, &cap);
                while (um_extractor_sample_track(ex) >= 0 && um_extractor_sample_track(ex) != track) um_extractor_advance(ex);
                ssize_t n = um_extractor_sample_track(ex) == track ? um_extractor_read(ex, b, cap) : -1;
                if (n < 0) { um_codec_queue_input(c, i, 0, 0, 0, UM_BUFFER_FLAG_END_OF_STREAM); in_eos = 1; }
                else {
                    um_codec_queue_input(c, i, 0, (int)n, um_extractor_sample_time(ex), 0);
                    um_extractor_advance(ex);
                }
            }
        }
        um_buffer_info bi;
        int o = um_codec_dequeue_output(c, &bi, 2000);
        if (o == UM_INFO_OUTPUT_FORMAT_CHANGED) {
            r->formats++;
            um_format *f = um_codec_output_format(c);
            int32_t v;
            if (um_format_get_int32(f, UM_KEY_WIDTH, &v)) r->w = v;
            if (um_format_get_int32(f, UM_KEY_HEIGHT, &v)) r->h = v;
            if (um_format_get_int32(f, UM_KEY_SAMPLE_RATE, &v)) r->rate = v;
            if (um_format_get_int32(f, UM_KEY_CHANNEL_COUNT, &v)) r->channels = v;
            um_format_free(f);
            continue;
        }
        if (o < 0) continue;
        if (bi.flags & UM_BUFFER_FLAG_END_OF_STREAM) r->eos = 1;
        if (bi.size > 0 || (video && surf)) {
            if (!(bi.flags & UM_BUFFER_FLAG_END_OF_STREAM)) {
                if (r->first_pts < 0) r->first_pts = bi.pts_us;
                r->last_pts = bi.pts_us;
            }
            if (video && !(bi.flags & UM_BUFFER_FLAG_END_OF_STREAM)) {
                if (r->frames < 512) r->pts[r->frames] = bi.pts_us;
                if (!surf) {
                    size_t cap;
                    uint8_t *p = um_codec_output_buffer(c, o, &cap);
                    int f = (int)((bi.pts_us * 30 + 500000) / 1000000);
                    double q = psnr_y(p, (r->w + 1) & ~1, r->w, r->h, f);
                    if (q < r->psnr_min) r->psnr_min = q;
                }
                r->frames++;
            } else if (!video) {
                size_t cap;
                uint8_t *p = um_codec_output_buffer(c, o, &cap);
                size_t ns = bi.size / 2;
                if (r->pcm_n + ns > r->pcm_cap) { r->pcm_cap = (r->pcm_n + ns) * 2; r->pcm = realloc(r->pcm, r->pcm_cap * 2); }
                memcpy(r->pcm + r->pcm_n, p + bi.offset, bi.size);
                r->pcm_n += ns;
                r->audio_frames += bi.size / 4;
            }
        }
        um_codec_release_output(c, o, surf != NULL);
        if (max_out && r->frames >= max_out) break;
    }
}

/* dominant frequency of one channel by zero crossings */
static double freq(const int16_t *pcm, size_t frames, int ch, int channels)
{
    int z = 0;
    for (size_t i = 1; i < frames; i++)
        if ((pcm[(i - 1) * channels + ch] < 0) != (pcm[i * channels + ch] < 0)) z++;
    return z / 2.0 * AR / frames;
}

/* ---- tests -------------------------------------------------------------------------------------------- */
static void test_format(void)
{
    um_format *f = um_format_new();
    um_format_set_string(f, UM_KEY_MIME, UM_MIME_VP8);
    um_format_set_int32(f, UM_KEY_WIDTH, 64);
    um_format_set_int64(f, UM_KEY_DURATION, 123456789012LL);
    um_format_set_float(f, UM_KEY_FRAME_RATE, 29.97f);
    uint8_t b[3] = { 1, 2, 3 };
    um_format_set_buffer(f, "csd-0", b, 3);
    int32_t i; int64_t j; float fl; size_t n;
    CHECK(um_format_get_int32(f, UM_KEY_WIDTH, &i) && i == 64, "int32");
    CHECK(!um_format_get_int64(f, UM_KEY_WIDTH, &j), "typed getter refuses another type");
    CHECK(um_format_get_int64(f, UM_KEY_DURATION, &j) && j == 123456789012LL, "int64");
    CHECK(um_format_get_float(f, UM_KEY_FRAME_RATE, &fl) && fabsf(fl - 29.97f) < 1e-4, "float");
    CHECK(um_format_get_buffer(f, "csd-0", &n) && n == 3, "buffer");
    um_format_set_int32(f, UM_KEY_WIDTH, 65);
    CHECK(um_format_get_int32(f, UM_KEY_WIDTH, &i) && i == 65 && um_format_count(f) == 5, "replace keeps one entry");
    um_format *c = um_format_copy(f);
    um_format_remove(f, UM_KEY_WIDTH);
    CHECK(um_format_type(f, UM_KEY_WIDTH) == UM_NONE && um_format_type(c, UM_KEY_WIDTH) == UM_INT32, "copy is deep");
    char *s = um_format_to_string(c);
    CHECK(s && strstr(s, "mime=video/x-vnd.on2.vp8") && strstr(s, "width=65"), "toString %s", s);
    free(s);
    um_format_free(f); um_format_free(c);
}

static void test_frame_layout(void)
{
    /* odd size: the packed frame repeats the last column and row */
    int w = 5, h = 3;
    uint8_t y[15], u[6], v[6];
    for (int i = 0; i < 15; i++) y[i] = i;
    for (int i = 0; i < 6; i++) { u[i] = 100 + i; v[i] = 200 + i; }
    um_frame *f = um_frame_get(w, h);
    um_frame_fill(f, y, 5, u, 3, v, 3);
    CHECK(f->pw == 6 && f->ph == 4, "padded size %dx%d", f->pw, f->ph);
    CHECK(f->data[0] == 0 && f->data[4] == 4 && f->data[5] == 4 && f->data[6] == 5, "Y rows");
    CHECK(f->data[3 * 6 + 0] == 10 && f->data[3 * 6 + 5] == 14, "last row repeated");
    uint8_t *c = f->data + 6 * 4;
    CHECK(c[0] == 100 && c[2] == 102 && c[3] == 200 && c[5] == 202, "chroma row 0: U then V");
    CHECK(c[6] == 103 && c[9] == 203, "chroma row 1");
    um_frame_put(f);
}

static int heard;
static void on_frame(void *u, um_surface *s) { (void)s; (*(int *)u)++; }

static void test_webm(int w, int h, int lace)
{
    printf("-- WebM VP8 %dx%d + Vorbis%s\n", w, h, lace ? " (laced audio)" : "");
    int frames = 45;
    plist vp = encode_vpx(0, w, h, frames);
    uint8_t *priv; size_t privn;
    plist au = encode_vorbis(2, &priv, &privn);
    mtrack t[2] = { { "V_VP8", w, h, 30.0f, NULL, 0, 0, &vp, 0 }, { "A_VORBIS", 0, 0, 0, priv, privn, 0, &au, 1 } };
    wb clip = mux_webm(t, 2, 2000, lace);
    char path[] = "/tmp/um-test-XXXXXX";
    int tfd = mkstemp(path); close(tfd);
    int64_t off;
    int fd = write_embedded(path, &clip, &off);
    unlink(path);

    um_extractor *ex = um_extractor_new();
    CHECK(um_extractor_set_fd(ex, fd, off, clip.n) == UM_OK, "set_fd");
    close(fd);                      /* the extractor keeps its own descriptor, as Android's */
    CHECK(um_extractor_track_count(ex) == 2, "tracks %d", um_extractor_track_count(ex));
    CHECK(!strcmp(um_extractor_container(ex), "webm"), "container");
    um_format *vf = um_extractor_track_format(ex, 0), *af = um_extractor_track_format(ex, 1);
    int32_t i32; int64_t i64; float fr; size_t n;
    CHECK(!strcmp(um_format_get_string(vf, UM_KEY_MIME), UM_MIME_VP8), "video mime");
    CHECK(um_format_get_int32(vf, UM_KEY_WIDTH, &i32) && i32 == w, "width");
    CHECK(um_format_get_int32(vf, UM_KEY_HEIGHT, &i32) && i32 == h, "height");
    CHECK(um_format_get_int64(vf, UM_KEY_DURATION, &i64) && i64 == 2000000, "duration %lld", (long long)i64);
    CHECK(um_format_get_float(vf, UM_KEY_FRAME_RATE, &fr) && fr == 30.0f, "frame rate %g", fr);
    CHECK(um_format_get_int32(vf, UM_KEY_MAX_INPUT_SIZE, &i32) && i32 > 0, "max input size");
    CHECK(!strcmp(um_format_get_string(af, UM_KEY_MIME), UM_MIME_VORBIS), "audio mime");
    CHECK(um_format_get_int32(af, UM_KEY_SAMPLE_RATE, &i32) && i32 == AR, "sample rate");
    CHECK(um_format_get_int32(af, UM_KEY_CHANNEL_COUNT, &i32) && i32 == 2, "channels");
    CHECK(um_format_get_buffer(af, "csd-0", &n) && n == 30, "csd-0 (identification header) %zu", n);
    CHECK(um_format_get_buffer(af, "csd-1", &n) && n > 100, "csd-1 (setup header)");
    CHECK(!strcmp(um_format_get_string(af, UM_KEY_LANGUAGE), "und"), "language");

    /* sample order: both tracks, time order */
    um_extractor_select(ex, 0, 1);
    um_extractor_select(ex, 1, 1);
    int nv = 0, na = 0, ordered = 1, keys = 0;
    int64_t last = -1;
    do {
        int64_t tt = um_extractor_sample_time(ex);
        if (tt < last) ordered = 0;
        last = tt;
        if (um_extractor_sample_track(ex) == 0) { nv++; if (um_extractor_sample_flags(ex) & UM_SAMPLE_SYNC) keys++; }
        else na++;
    } while (um_extractor_advance(ex));
    CHECK(nv == vp.n, "video samples %d of %d", nv, vp.n);
    CHECK(na == au.n, "audio samples %d of %d", na, au.n);
    CHECK(ordered, "samples in time order");
    CHECK(keys == (frames + 9) / 10, "sync samples %d", keys);
    CHECK(um_extractor_sample_track(ex) == -1 && um_extractor_sample_time(ex) == -1 && um_extractor_read(ex, NULL, 0) == -1, "end");

    /* video, decoding to buffers */
    um_extractor_select(ex, 1, 0);
    um_extractor_seek(ex, 0, UM_SEEK_PREVIOUS_SYNC);
    um_codec *c = um_codec_create(UM_MIME_VP8);
    CHECK(c && !strcmp(um_codec_name(c), "OMX.google.vp8.decoder"), "vp8 decoder");
    CHECK(um_codec_dequeue_input(c, 0) == UM_ERR_STATE, "dequeue before start: state error");
    CHECK(um_codec_configure(c, vf, NULL) == UM_OK && um_codec_start(c) == UM_OK, "configure/start");
    dec_result r;
    run_codec(ex, c, 0, &r, 1, NULL, 0);
    CHECK(r.formats == 1 && r.w == w && r.h == h, "one format change, %dx%d (%d)", r.w, r.h, r.formats);
    CHECK(r.frames == frames, "frames %d of %d", r.frames, frames);
    CHECK(r.eos, "end of stream");
    CHECK(r.psnr_min > 38, "PSNR %.1f dB", r.psnr_min);
    CHECK(r.first_pts == 0 && r.last_pts == (int64_t)(frames - 1) * 1000 / 30 * 1000, "times %lld..%lld",
          (long long)r.first_pts, (long long)r.last_pts);

    /* seek: previous / next / closest sync, then flush and decode from there */
    um_extractor_seek(ex, 500000, UM_SEEK_PREVIOUS_SYNC);
    CHECK(um_extractor_sample_time(ex) == 333000, "previous sync of 0.5 s: %lld", (long long)um_extractor_sample_time(ex));
    um_extractor_seek(ex, 500000, UM_SEEK_NEXT_SYNC);
    CHECK(um_extractor_sample_time(ex) == 666000, "next sync: %lld", (long long)um_extractor_sample_time(ex));
    um_extractor_seek(ex, 600000, UM_SEEK_CLOSEST_SYNC);
    CHECK(um_extractor_sample_time(ex) == 666000, "closest sync: %lld", (long long)um_extractor_sample_time(ex));
    um_extractor_seek(ex, 1000000, UM_SEEK_PREVIOUS_SYNC);
    CHECK(um_codec_flush(c) == UM_OK, "flush");
    run_codec(ex, c, 0, &r, 1, NULL, 0);
    CHECK(r.first_pts == 1000000 && r.frames == frames - 30 && r.psnr_min > 38, "after seek: first %lld, %d frames, %.1f dB",
          (long long)r.first_pts, r.frames, r.psnr_min);
    CHECK(r.formats == 0, "no format change after a flush");
    um_codec_free(c);

    /* video to a surface: frames reach the queue, the listener hears of each */
    heard = 0;
    um_surface *s = um_surface_new(77);
    um_surface_set_listener(s, on_frame, &heard);
    c = um_codec_create(UM_MIME_VP8);
    um_codec_configure(c, vf, s);
    um_codec_start(c);
    size_t cap = 1;
    um_codec_output_buffer(c, 0, &cap);
    CHECK(cap > 0, "a buffer behind every output index (surface)");
    um_extractor_seek(ex, 0, UM_SEEK_PREVIOUS_SYNC);
    run_codec(ex, c, 0, &r, 1, s, 0);
    CHECK(r.frames == frames && heard == frames, "surface: %d frames, listener %d", r.frames, heard);
    CHECK(um_surface_pending(s) == 4, "queue keeps the newest 4 (%d)", um_surface_pending(s));
    CHECK(um_surface_update(s) == -1, "update without a GL context fails cleanly");
    float m[16];
    um_surface_matrix(s, m);
    CHECK(m[0] == (float)w / ((w + 1) & ~1) && m[5] == -(float)h / ((h + 1) & ~1) && m[13] == -m[5] && m[12] == 0,
          "transform matrix %g %g %g %g", m[0], m[5], m[12], m[13]);
    um_codec_free(c);
    um_surface_unref(s);

    /* audio */
    um_extractor_select(ex, 0, 0);
    um_extractor_select(ex, 1, 1);
    um_extractor_seek(ex, 0, UM_SEEK_PREVIOUS_SYNC);
    c = um_codec_create(UM_MIME_VORBIS);
    CHECK(c && um_codec_configure(c, af, NULL) == UM_OK && um_codec_start(c) == UM_OK, "vorbis decoder");
    run_codec(ex, c, 1, &r, 0, NULL, 0);
    CHECK(r.formats == 1 && r.rate == AR && r.channels == 2, "audio format %d Hz %d ch", r.rate, r.channels);
    CHECK(llabs(r.audio_frames - 2 * AR) < 2048, "audio frames %lld", (long long)r.audio_frames);
    double f0 = freq(r.pcm + 4096 * 2, r.audio_frames - 8192, 0, 2), f1 = freq(r.pcm + 4096 * 2, r.audio_frames - 8192, 1, 2);
    CHECK(fabs(f0 - 440) < 5 && fabs(f1 - 660) < 5, "tones %.1f / %.1f Hz", f0, f1);
    CHECK(r.eos, "audio end of stream");
    free(r.pcm);
    um_codec_free(c);

    um_format_free(vf); um_format_free(af);
    um_extractor_free(ex);
    wb_free(&clip);
    free(priv);
}

static void test_vp9_opus(void)
{
    printf("-- WebM VP9 + Opus\n");
    int w = 96, h = 54, frames = 20, preskip;
    plist vp = encode_vpx(1, w, h, frames);
    plist au = encode_opus(1, &preskip);
    uint8_t head[19] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, preskip & 255, preskip >> 8, 0x80, 0xBB, 0, 0, 0, 0, 0 };
    mtrack t[2] = { { "V_VP9", w, h, 0, NULL, 0, 0, &vp, 0 },
                    { "A_OPUS", 0, 0, 0, head, sizeof head, (uint64_t)preskip * 1000000000ULL / AR, &au, 1 } };
    wb clip = mux_webm(t, 2, 1000, 0);
    char path[] = "/tmp/um-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(write(fd, clip.p, clip.n) == (ssize_t)clip.n, "write");
    close(fd);
    um_extractor *ex = um_extractor_new();
    CHECK(um_extractor_set_path(ex, path) == UM_OK, "set_path");
    unlink(path);
    um_format *vf = um_extractor_track_format(ex, 0), *af = um_extractor_track_format(ex, 1);
    float fr;
    CHECK(!strcmp(um_format_get_string(vf, UM_KEY_MIME), UM_MIME_VP9), "vp9 mime");
    CHECK(um_format_get_float(vf, UM_KEY_FRAME_RATE, &fr) && fabsf(fr - 30.3f) < 0.6f, "frame rate from the samples: %g", fr);
    CHECK(!strcmp(um_format_get_string(af, UM_KEY_MIME), UM_MIME_OPUS), "opus mime");
    size_t n;
    CHECK(um_format_get_buffer(af, "csd-0", &n) && n == 19 && um_format_get_buffer(af, "csd-1", &n) && n == 8, "opus csd");
    um_extractor_select(ex, 0, 1);
    um_codec *c = um_codec_create(UM_MIME_VP9);
    um_codec_configure(c, vf, NULL);
    um_codec_start(c);
    dec_result r;
    run_codec(ex, c, 0, &r, 1, NULL, 0);
    CHECK(r.frames == frames && r.psnr_min > 36, "vp9: %d frames, %.1f dB", r.frames, r.psnr_min);
    um_codec_free(c);
    um_extractor_select(ex, 0, 0);
    um_extractor_select(ex, 1, 1);
    um_extractor_seek(ex, 0, 0);
    c = um_codec_create(UM_MIME_OPUS);
    CHECK(c && um_codec_configure(c, af, NULL) == UM_OK && um_codec_start(c) == UM_OK, "opus decoder");
    run_codec(ex, c, 1, &r, 0, NULL, 0);
    CHECK(r.rate == 48000 && r.channels == 2, "opus format");
    CHECK(r.audio_frames == (int64_t)au.n * 960 - preskip, "opus frames %lld (pre-skip %d)", (long long)r.audio_frames, preskip);
    double f0 = freq(r.pcm, r.audio_frames, 0, 2), f1 = freq(r.pcm, r.audio_frames, 1, 2);
    CHECK(fabs(f0 - 440) < 5 && fabs(f1 - 660) < 5, "opus tones %.1f / %.1f Hz", f0, f1);
    free(r.pcm);
    um_codec_free(c);
    um_format_free(vf); um_format_free(af);
    um_extractor_free(ex);
    wb_free(&clip);
}

#ifdef UM_HAVE_H264
void test_mp4_h264(void);
#endif

static void test_errors(void)
{
    printf("-- errors\n");
    um_extractor *ex = um_extractor_new();
    char path[] = "/tmp/um-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(write(fd, "not a media file at all", 23) == 23, "write");
    CHECK(um_extractor_set_fd(ex, fd, 0, 23) != UM_OK, "unknown container refused");
    CHECK(um_extractor_set_fd(ex, fd, 100, 10) != UM_OK, "offset past the end refused");
    close(fd);
    unlink(path);
    CHECK(um_extractor_set_path(ex, "/nonexistent/clip.webm") != UM_OK, "missing file");
    CHECK(um_extractor_track_count(ex) == 0, "no tracks");
    um_extractor_free(ex);
    CHECK(um_codec_create("video/unknown") == NULL, "unknown mime");
    CHECK(um_codec_create_by_name("OMX.google.vp8.decoder") != NULL, "by name");
    printf("decoders: %s\n", um_decoders());
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_format();
    test_frame_layout();
    test_webm(160, 90, 0);
    test_webm(71, 41, 1);
    test_vp9_opus();
#ifdef UM_HAVE_H264
    test_mp4_h264();
#endif
    test_errors();
    printf("%s: %d failure(s) in %d checks\n", fails ? "FAILED" : "PASSED", fails, checks);
    return fails != 0;
}
