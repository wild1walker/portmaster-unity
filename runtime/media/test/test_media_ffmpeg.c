// The media core against clips made by FFmpeg's own tools (test/ffmpeg_clips.sh, in the
// ua-media-ffmpeg-* image that has Debian's FFmpeg): AAC in MP4 through the system's libavcodec
// (um_ffmpeg.c), H.264 High profile with B-frames through OpenH264 and through libavcodec (the
// two must give the same pictures: H.264 decoding is exact), interlaced H.264 (OpenH264 refuses
// it at configure, the codec moves to libavcodec), and WebM / Matroska written by FFmpeg's muxer.
#define _GNU_SOURCE
#include "../um.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    int frames, formats, eos, ordered, rate, ch;
    long pcm;
    int64_t first, last;
    uint32_t hash;              /* of every picture, in order */
    uint8_t *pics[64];          /* video: the first 64 pictures (packed), for comparisons */
    size_t picn;
    int16_t *s; size_t sn, scap;
} res;

static void run(const char *path, int track, const char *codec_name, res *r)
{
    memset(r, 0, sizeof *r);
    r->ordered = 1; r->first = -1; r->hash = 2166136261u;
    um_extractor *ex = um_extractor_new();
    if (um_extractor_set_path(ex, path) != UM_OK) { printf("   cannot open %s\n", path); um_extractor_free(ex); return; }
    um_format *f = um_extractor_track_format(ex, track);
    const char *mime = um_format_get_string(f, UM_KEY_MIME);
    int video = mime && !strncmp(mime, "video/", 6);
    um_codec *c = codec_name ? um_codec_create_by_name(codec_name) : um_codec_create(mime);
    if (!c || um_codec_configure(c, f, NULL) != UM_OK || um_codec_start(c) != UM_OK) {
        printf("   no decoder (%s)\n", codec_name ? codec_name : mime);
        um_codec_free(c); um_format_free(f); um_extractor_free(ex);
        return;
    }
    um_extractor_select(ex, track, 1);
    int in_eos = 0;
    int64_t last = INT64_MIN;
    for (int spins = 0; spins < 20000 && !r->eos; spins++) {
        if (!in_eos) {
            int i = um_codec_dequeue_input(c, 1000);
            if (i >= 0) {
                size_t cap;
                uint8_t *b = um_codec_input_buffer(c, i, &cap);
                ssize_t n = um_extractor_read(ex, b, cap);
                if (n < 0) { um_codec_queue_input(c, i, 0, 0, 0, UM_BUFFER_FLAG_END_OF_STREAM); in_eos = 1; }
                else { um_codec_queue_input(c, i, 0, (int)n, um_extractor_sample_time(ex), 0); um_extractor_advance(ex); }
            }
        }
        um_buffer_info bi;
        int o = um_codec_dequeue_output(c, &bi, 2000);
        if (o == UM_INFO_OUTPUT_FORMAT_CHANGED) {
            um_format *of = um_codec_output_format(c);
            int32_t v;
            if (um_format_get_int32(of, UM_KEY_SAMPLE_RATE, &v)) r->rate = v;
            if (um_format_get_int32(of, UM_KEY_CHANNEL_COUNT, &v)) r->ch = v;
            um_format_free(of);
            r->formats++;
            continue;
        }
        if (o < 0) continue;
        if (bi.flags & UM_BUFFER_FLAG_END_OF_STREAM) r->eos = 1;
        else {
            size_t cap;
            uint8_t *p = um_codec_output_buffer(c, o, &cap);
            if (r->first < 0) r->first = bi.pts_us;
            if (bi.pts_us < last) r->ordered = 0;
            last = r->last = bi.pts_us;
            if (video) {
                for (int k = 0; k < bi.size; k++) r->hash = (r->hash ^ p[k]) * 16777619u;
                if (r->frames < 64) { r->pics[r->frames] = malloc(bi.size); memcpy(r->pics[r->frames], p, bi.size); r->picn = bi.size; }
                r->frames++;
            } else {
                size_t n = bi.size / 2;
                if (r->sn + n > r->scap) { r->scap = (r->sn + n) * 2; r->s = realloc(r->s, r->scap * 2); }
                memcpy(r->s + r->sn, p + bi.offset, bi.size);
                r->sn += n;
                r->pcm += bi.size / (2 * (r->ch ? r->ch : 2));
            }
        }
        um_codec_release_output(c, o, 0);
    }
    um_codec_free(c);
    um_format_free(f);
    um_extractor_free(ex);
}

static double freq(const int16_t *s, long frames, int ch, int chs, int rate)
{
    int z = 0;
    for (long i = 1; i < frames; i++) if ((s[(i - 1) * chs + ch] < 0) != (s[i * chs + ch] < 0)) z++;
    return z / 2.0 * rate / frames;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *d = argc > 1 ? argv[1] : "/tmp/clips";
    char p[512];
    res r, r2;
    printf("decoders: %s; system libavcodec: %s\n", um_decoders(), um_ffmpeg_version() ? um_ffmpeg_version() : "none");
    CHECK(um_ffmpeg_version() != NULL, "the system's libavcodec loads");

    printf("-- AAC in MP4 (libavcodec)\n");
    snprintf(p, sizeof p, "%s/aac.mp4", d);
    run(p, 0, NULL, &r);
    CHECK(r.rate == 48000 && r.ch == 2 && r.eos, "AAC: %d Hz, %d ch", r.rate, r.ch);
    CHECK(labs(r.pcm - 48000) < 2048, "AAC: %ld frames", r.pcm);
    if (r.pcm > 8192) {
        double f0 = freq(r.s + 4096 * 2, r.pcm - 8192, 0, 2, 48000), f1 = freq(r.s + 4096 * 2, r.pcm - 8192, 1, 2, 48000);
        CHECK(fabs(f0 - 440) < 5 && fabs(f1 - 660) < 5, "AAC tones %.1f / %.1f Hz", f0, f1);
    }
    CHECK(r.first > -50000 && r.first < 50000, "AAC: first time %lld us (priming taken by the edit list)", (long long)r.first);
    free(r.s);

    printf("-- H.264 High profile with B-frames\n");
    snprintf(p, sizeof p, "%s/high.mp4", d);
    run(p, 0, "ffmpeg.h264.decoder", &r);
    CHECK(r.frames == 30 && r.ordered && r.eos, "libavcodec: %d frames, in order %d", r.frames, r.ordered);
    run(p, 0, NULL, &r2);
    CHECK(r2.frames == r.frames && r2.hash == r.hash, "B-frames: the H.264 decoder of video/avc goes to libavcodec");
    setenv("UM_H264", "openh264", 1);                  /* OpenH264 even for B-frames */
    run(p, 0, "OMX.google.h264.decoder", &r2);
    unsetenv("UM_H264");
    printf("   OpenH264: %d frames, in order %d, %s pictures as libavcodec\n", r2.frames, r2.ordered,
           r2.hash == r.hash ? "the same" : "NOT the same");
    double worst = 99;
    for (int i = 0; i < r.frames && i < r2.frames && i < 64; i++) {
        double se = 0;
        for (size_t k = 0; k < r.picn; k++) { int dd = r.pics[i][k] - r2.pics[i][k]; se += dd * dd; }
        double q = se ? 10 * log10(255.0 * 255 * r.picn / se) : 99.0;
        if (q < worst) worst = q;
    }
    printf("   OpenH264 against libavcodec: worst picture %.1f dB\n", worst);
    CHECK(r2.frames == r.frames && r2.ordered && worst > 35, "OpenH264 with B-frames: close (%.1f dB)", worst);
    CHECK(r.first == 0 && r.last == 966666, "times %lld .. %lld", (long long)r.first, (long long)r.last);

    printf("-- interlaced H.264: OpenH264 refuses, libavcodec takes it\n");
    snprintf(p, sizeof p, "%s/interlaced.mp4", d);
    run(p, 0, NULL, &r);
    CHECK(r.frames == 30 && r.eos, "interlaced: %d frames", r.frames);

    printf("-- WebM by FFmpeg's muxer (VP8 + Vorbis), Matroska with H.264 + AAC\n");
    snprintf(p, sizeof p, "%s/ff.webm", d);
    run(p, 0, NULL, &r);
    CHECK(r.frames == 30 && r.ordered, "ff.webm video: %d frames", r.frames);
    run(p, 1, NULL, &r);
    CHECK(r.rate == 48000 && labs(r.pcm - 48000) < 4096, "ff.webm audio: %ld frames", r.pcm);
    free(r.s);
    snprintf(p, sizeof p, "%s/ff.mkv", d);
    run(p, 0, NULL, &r);
    CHECK(r.frames == 30 && r.ordered, "ff.mkv H.264: %d frames", r.frames);
    run(p, 1, NULL, &r);
    CHECK(r.rate == 48000 && labs(r.pcm - 48000) < 4096, "ff.mkv AAC: %ld frames", r.pcm);
    free(r.s);

    printf("%s: %d failure(s) in %d checks\n", fails ? "FAILED" : "PASSED", fails, checks);
    return fails != 0;
}
