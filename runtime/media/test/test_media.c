// Unit tests of the media core on clips made here (clips.c: frames and tones encoded with the
// encoders of the static libraries the core links, muxed by webm_writer.c), demuxed and decoded through um_extractor /
// um_codec exactly as the Java and NDK layers drive them (Unity's call pattern: select tracks,
// dequeue/queue input, dequeue/release output, seek + flush, end of stream).
#define _GNU_SOURCE
#include "../um.h"
#include "clips.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef UM_HAVE_H264
#include <wels/codec_api.h>
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

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
