// H.264 in MP4 (macOS / Windows builds' clips): frames encoded here with OpenH264's encoder,
// muxed by mp4_writer.c (with an edit list, chunks of five samples), then demuxed and decoded.
#define _GNU_SOURCE
#include "../um.h"
#include "clips.h"
#include "mp4_writer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wels/codec_api.h>

extern int test_fails, test_checks;
#define CHECK(c, ...) do { test_checks++; if (!(c)) { test_fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint8_t *p; size_t n; } buf;

/* Annex B NAL units of one encoded picture -> length-prefixed sample (SPS/PPS kept apart) */
static void to_sample(const uint8_t *bs, size_t n, buf *sample, buf *sps, buf *pps)
{
    size_t i = 0;
    while (i + 4 <= n) {
        size_t sc = (bs[i + 2] == 1) ? 3 : 4;
        size_t s = i + sc, e = s;
        while (e + 3 <= n && !(bs[e] == 0 && bs[e + 1] == 0 && (bs[e + 2] == 1 || (e + 3 < n && bs[e + 2] == 0 && bs[e + 3] == 1)))) e++;
        if (e + 3 > n) e = n;
        int type = bs[s] & 31;
        /* the first SPS / PPS go into the avcC; all parameter sets also stay in the samples (the
           encoder makes a new PPS id at each IDR) */
        buf *ps = type == 7 ? sps : type == 8 ? pps : NULL;
        if (ps && !ps->n) { ps->p = malloc(e - s); memcpy(ps->p, bs + s, e - s); ps->n = e - s; }
        buf *dst = sample;
        {
            dst->p = realloc(dst->p, dst->n + 4 + (e - s));
            uint32_t l = (uint32_t)(e - s);
            uint8_t *q = dst->p + dst->n;
            q[0] = l >> 24; q[1] = l >> 16; q[2] = l >> 8; q[3] = l;
            memcpy(q + 4, bs + s, e - s);
            dst->n += 4 + (e - s);
        }
        i = e;
    }
}

static double psnr(const uint8_t *packed, int pw, int w, int h, int f)
{
    double se = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { double d = packed[y * pw + x] - pix_y(x, y, f); se += d * d; }
    return se == 0 ? 99 : 10 * log10(255.0 * 255 * w * h / se);
}

void test_mp4_h264(void)
{
    printf("-- MP4 H.264 (OpenH264)\n");
    int w = 160, h = 96, frames = 30;
    ISVCEncoder *enc = NULL;
    CHECK(WelsCreateSVCEncoder(&enc) == 0 && enc, "encoder");
    SEncParamBase p;
    memset(&p, 0, sizeof p);
    p.iUsageType = CAMERA_VIDEO_REAL_TIME;
    p.fMaxFrameRate = 30;
    p.iPicWidth = w; p.iPicHeight = h;
    p.iTargetBitrate = 3000000;
    p.iRCMode = RC_QUALITY_MODE;
    (*enc)->Initialize(enc, &p);
    int idr = 10, fmt = videoFormatI420;
    (*enc)->SetOption(enc, ENCODER_OPTION_IDR_INTERVAL, &idr);
    (*enc)->SetOption(enc, ENCODER_OPTION_DATAFORMAT, &fmt);
    uint8_t *Y = malloc(w * h), *U = malloc(w * h / 4), *V = malloc(w * h / 4);
    buf samples[64] = { 0 }, sps = { 0 }, pps = { 0 };
    int keys[64] = { 0 }, n = 0;
    for (int f = 0; f < frames; f++) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) Y[y * w + x] = pix_y(x, y, f);
        for (int y = 0; y < h / 2; y++) for (int x = 0; x < w / 2; x++) { U[y * w / 2 + x] = pix_u(x, y, f); V[y * w / 2 + x] = pix_v(x, y, f); }
        SSourcePicture pic;
        memset(&pic, 0, sizeof pic);
        pic.iPicWidth = w; pic.iPicHeight = h; pic.iColorFormat = videoFormatI420;
        pic.iStride[0] = w; pic.iStride[1] = pic.iStride[2] = w / 2;
        pic.pData[0] = Y; pic.pData[1] = U; pic.pData[2] = V;
        pic.uiTimeStamp = f * 1000 / 30;
        if (f % 10 == 0) (*enc)->ForceIntraFrame(enc, true);
        SFrameBSInfo info;
        memset(&info, 0, sizeof info);
        (*enc)->EncodeFrame(enc, &pic, &info);
        if (info.eFrameType == videoFrameTypeSkip) continue;
        for (int l = 0; l < info.iLayerNum; l++) {
            SLayerBSInfo *L = &info.sLayerInfo[l];
            size_t sz = 0;
            for (int k = 0; k < L->iNalCount; k++) sz += L->pNalLengthInByte[k];
            to_sample(L->pBsBuf, sz, &samples[n], &sps, &pps);
        }
        keys[n] = info.eFrameType == videoFrameTypeIDR;
        n++;
    }
    (*enc)->Uninitialize(enc);
    WelsDestroySVCEncoder(enc);
    CHECK(n == frames && sps.n && pps.n, "%d samples, SPS %zu, PPS %zu bytes", n, sps.n, pps.n);
    /* avcC */
    uint8_t avcc[512];
    size_t an = 0;
    avcc[an++] = 1; avcc[an++] = sps.p[1]; avcc[an++] = sps.p[2]; avcc[an++] = sps.p[3]; avcc[an++] = 0xFF;
    avcc[an++] = 0xE1; avcc[an++] = sps.n >> 8; avcc[an++] = sps.n & 255; memcpy(avcc + an, sps.p, sps.n); an += sps.n;
    avcc[an++] = 1; avcc[an++] = pps.n >> 8; avcc[an++] = pps.n & 255; memcpy(avcc + an, pps.p, pps.n); an += pps.n;
    const uint8_t *data[64]; size_t size[64]; int64_t dts[64], cto[64];
    for (int i = 0; i < n; i++) { data[i] = samples[i].p; size[i] = samples[i].n; dts[i] = (int64_t)i * 3000; cto[i] = 1500; }
    /* timescale 90000, composition offsets of 1500 ticks that the edit list takes back (as a
       B-frame stream has them): frame i at i * 33.333 ms */
    mp4_track t = { 1, 90000, w, h, 0, 0, avcc, an, n, data, size, dts, cto, keys, frames * 1000 / 30, 1500 };
    wb clip = mux_mp4(&t, 1);
    char path[] = "/tmp/um-test-XXXXXX";
    int tfd = mkstemp(path); close(tfd);
    int64_t off;
    int fd = write_embedded(path, &clip, &off);
    unlink(path);

    um_extractor *ex = um_extractor_new();
    CHECK(um_extractor_set_fd(ex, fd, off, clip.n) == UM_OK && !strcmp(um_extractor_container(ex), "mp4"), "mp4 opens");
    close(fd);
    um_format *vf = um_extractor_track_format(ex, 0);
    int32_t i32; int64_t i64; float fr; size_t cn;
    CHECK(vf && !strcmp(um_format_get_string(vf, UM_KEY_MIME), UM_MIME_AVC), "video/avc");
    CHECK(um_format_get_int32(vf, UM_KEY_WIDTH, &i32) && i32 == w && um_format_get_int32(vf, UM_KEY_HEIGHT, &i32) && i32 == h, "size");
    const uint8_t *c0 = um_format_get_buffer(vf, "csd-0", &cn);
    CHECK(c0 && cn == sps.n + 4 && !memcmp(c0, "\0\0\0\1", 4) && (c0[4] & 31) == 7, "csd-0: the SPS with a start code");
    CHECK(um_format_get_buffer(vf, "csd-1", &cn) && cn == pps.n + 4, "csd-1: the PPS");
    CHECK(um_format_get_int64(vf, UM_KEY_DURATION, &i64) && i64 == 1000000, "duration %lld", (long long)i64);
    CHECK(um_format_get_float(vf, UM_KEY_FRAME_RATE, &fr) && fabsf(fr - 30) < 0.01f, "frame rate %g", fr);
    um_extractor_select(ex, 0, 1);
    CHECK(um_extractor_sample_time(ex) == 0, "edit list: first sample at 0 (%lld)", (long long)um_extractor_sample_time(ex));
    uint8_t sbuf[65536];
    ssize_t sn = um_extractor_read(ex, sbuf, sizeof sbuf);
    CHECK(sn == (ssize_t)samples[0].n && !memcmp(sbuf, "\0\0\0\1", 4), "samples come in Annex B form");
    um_extractor_seek(ex, 400000, UM_SEEK_PREVIOUS_SYNC);
    CHECK(um_extractor_sample_time(ex) == 333333, "seek to the IDR before 0.4 s (%lld)", (long long)um_extractor_sample_time(ex));
    um_extractor_seek(ex, 0, UM_SEEK_PREVIOUS_SYNC);

    um_codec *c = um_codec_create(UM_MIME_AVC);
    CHECK(c && !strcmp(um_codec_name(c), "OMX.google.h264.decoder"), "h264 decoder");
    CHECK(um_codec_configure(c, vf, NULL) == UM_OK && um_codec_start(c) == UM_OK, "configure/start");
    int got = 0, fmts = 0, eos = 0, in_eos = 0, ordered = 1, stride = 0, last_size = 0;
    double qmin = 99;
    int64_t last = -1;
    for (int spins = 0; spins < 4000 && !eos; spins++) {
        if (!in_eos) {
            int i = um_codec_dequeue_input(c, 1000);
            if (i >= 0) {
                size_t cap;
                uint8_t *b = um_codec_input_buffer(c, i, &cap);
                ssize_t r = um_extractor_read(ex, b, cap);
                if (r < 0) { um_codec_queue_input(c, i, 0, 0, 0, UM_BUFFER_FLAG_END_OF_STREAM); in_eos = 1; }
                else { um_codec_queue_input(c, i, 0, (int)r, um_extractor_sample_time(ex), 0); um_extractor_advance(ex); }
            }
        }
        um_buffer_info bi;
        int o = um_codec_dequeue_output(c, &bi, 2000);
        if (o == UM_INFO_OUTPUT_FORMAT_CHANGED) {
            um_format *of = um_codec_output_format(c);
            um_format_get_int32(of, UM_KEY_STRIDE, &stride);
            um_format_free(of);
            fmts++;
            continue;
        }
        if (o < 0) continue;
        if (bi.flags & UM_BUFFER_FLAG_END_OF_STREAM) eos = 1;
        else {
            size_t cap;
            uint8_t *pp = um_codec_output_buffer(c, o, &cap);
            int f = (int)((bi.pts_us * 30 + 500000) / 1000000);
            double q = psnr(pp, stride, w, h, f);
            if (q < qmin) qmin = q;
            if (bi.pts_us <= last) ordered = 0;
            last_size = bi.size;
            last = bi.pts_us;
            got++;
        }
        um_codec_release_output(c, o, 0);
    }
    CHECK(got == frames && fmts == 1 && eos && ordered, "%d frames, %d format changes, eos %d, in order %d", got, fmts, eos, ordered);
    CHECK(qmin > 30, "PSNR %.1f dB", qmin);
    CHECK(stride == w && last_size == w * (h + h / 2), "packed pictures (%d, %d)", stride, last_size);
    um_codec_free(c);
    um_format_free(vf);
    um_extractor_free(ex);
    wb_free(&clip);
    for (int i = 0; i < n; i++) free(samples[i].p);
    free(sps.p); free(pps.p); free(Y); free(U); free(V);
}

/* AAC without the system's libavcodec: the track still decodes, to silence of its length */
void test_mp4_aac_silence(void)
{
    if (um_ffmpeg_version()) { printf("-- MP4 AAC: the system has libavcodec (test_media_ffmpeg covers it)\n"); return; }
    printf("-- MP4 AAC without a decoder: silence\n");
    static const uint8_t asc[2] = { 0x11, 0x90 };          /* AAC LC, 48 kHz, stereo */
    uint8_t frame[64];
    memset(frame, 0x21, sizeof frame);
    const uint8_t *data[10]; size_t size[10]; int64_t dts[10]; int key[10];
    for (int i = 0; i < 10; i++) { data[i] = frame; size[i] = sizeof frame; dts[i] = (int64_t)i * 1024; key[i] = 1; }
    mp4_track t = { 0, 48000, 0, 0, 48000, 2, asc, 2, 10, data, size, dts, NULL, key, 213, 0 };
    wb clip = mux_mp4(&t, 1);
    char path[] = "/tmp/um-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(write(fd, clip.p, clip.n) == (ssize_t)clip.n, "write");
    close(fd);
    um_extractor *ex = um_extractor_new();
    CHECK(um_extractor_set_path(ex, path) == UM_OK, "open");
    unlink(path);
    um_format *af = um_extractor_track_format(ex, 0);
    size_t n;
    int32_t v;
    CHECK(af && !strcmp(um_format_get_string(af, UM_KEY_MIME), UM_MIME_AAC) && um_format_get_buffer(af, "csd-0", &n) && n == 2, "audio/mp4a-latm + csd-0");
    CHECK(um_format_get_int32(af, UM_KEY_SAMPLE_RATE, &v) && v == 48000, "rate from the AudioSpecificConfig");
    um_codec *c = um_codec_create(UM_MIME_AAC);
    CHECK(c && !strcmp(um_codec_name(c), "OMX.google.aac.decoder"), "the silent decoder");
    CHECK(um_codec_configure(c, af, NULL) == UM_OK && um_codec_start(c) == UM_OK, "configure");
    um_extractor_select(ex, 0, 1);
    long frames = 0, nonzero = 0;
    int eos = 0, in_eos = 0;
    for (int spins = 0; spins < 2000 && !eos; spins++) {
        if (!in_eos) {
            int i = um_codec_dequeue_input(c, 1000);
            if (i >= 0) {
                size_t cap;
                uint8_t *b = um_codec_input_buffer(c, i, &cap);
                ssize_t r = um_extractor_read(ex, b, cap);
                if (r < 0) { um_codec_queue_input(c, i, 0, 0, 0, UM_BUFFER_FLAG_END_OF_STREAM); in_eos = 1; }
                else { um_codec_queue_input(c, i, 0, (int)r, um_extractor_sample_time(ex), 0); um_extractor_advance(ex); }
            }
        }
        um_buffer_info bi;
        int o = um_codec_dequeue_output(c, &bi, 2000);
        if (o < 0) continue;
        if (bi.flags & UM_BUFFER_FLAG_END_OF_STREAM) eos = 1;
        size_t cap;
        int16_t *p = (int16_t *)um_codec_output_buffer(c, o, &cap);
        for (int k = 0; k < bi.size / 2; k++) nonzero += p[k] != 0;
        frames += bi.size / 4;
        um_codec_release_output(c, o, 0);
    }
    CHECK(frames == 10240 && !nonzero && eos, "%ld frames of silence", frames);
    um_codec_free(c);
    um_format_free(af);
    um_extractor_free(ex);
    wb_free(&clip);
}
