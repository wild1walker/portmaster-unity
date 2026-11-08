// The hardware decoder path (um_pmhw.c) against a stand-in libpmhwdec.so.1 (fake_pmhwdec.c): which
// decoder takes an H.264 stream, the pictures it hands on (NV12 with a crop, exactly), the
// no-free-buffer retry, and the way back to OpenH264 when the library refuses the stream at
// configure or gives up in the middle. One mode per run (build.sh runs them all):
//   test_pmhw none     no libpmhwdec on the library path: OpenH264, as before
//   test_pmhw hw       FAKE_PMHW unset: pmhwdec decodes it all
//   test_pmhw nohw     FAKE_PMHW=nohw: the library has no decoder for the device: OpenH264
//   test_pmhw off      UM_PMHW=0: OpenH264 although the library is there
//   test_pmhw path     no library path, UM_PMHW=<the library>: pmhwdec decodes it all
//   test_pmhw h264env  UM_H264=ffmpeg (a software choice): pmhwdec still decodes it all
//   test_pmhw refuse   FAKE_PMHW=refuse: refused at configure: OpenH264
//   test_pmhw midway   FAKE_PMHW=midway: pmhwdec, then OpenH264 from the 11th picture (an IDR)
#define _GNU_SOURCE
#include "../um.h"
#include "clips.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wels/codec_api.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int sends, again, refused, pictures, held_max, sps_before_first_slice, slices; } fake_stats_t;

typedef struct { uint8_t *p; size_t n; } buf;
static void put(buf *b, const uint8_t *d, size_t n) { b->p = realloc(b->p, b->n + n); memcpy(b->p + b->n, d, n); b->n += n; }

/* the frame's packed picture against the test pattern: exact pixels and PSNR of Y */
static void compare(const uint8_t *p, int pw, int ph, int w, int h, int f, int *exact, double *psnr)
{
    double se = 0;
    int same = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) { int d = p[y * pw + x] - pix_y(x, y, f); se += d * d; if (d) same = 0; }
    const uint8_t *c = p + (size_t)pw * ph;
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++)
            if (c[y * pw + x] != pix_u(x, y, f) || c[y * pw + pw / 2 + x] != pix_v(x, y, f)) same = 0;
    *exact = same;
    *psnr = se == 0 ? 99 : 10 * log10(255.0 * 255 * w * h / se);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "hw";
    printf("-- pmhwdec path: %s\n", mode);
    fake_stats_t *st = NULL;
    const char *path = getenv("UM_PMHW");
    void *fake = dlopen(path && *path && strcmp(path, "0") ? path : "libpmhwdec.so.1", RTLD_NOW);
    if (fake) {
        void (*setup)(void *, void *, void *) = (void (*)(void *, void *, void *))dlsym(fake, "fake_pmhw_setup");
        st = dlsym(fake, "fake_pmhw_stats");
        if (setup) setup((void *)pix_y, (void *)pix_u, (void *)pix_v);
    }
    CHECK(!strcmp(mode, "none") ? !fake : fake && st, "libpmhwdec.so.1 %s", fake ? "found" : "missing");

    /* 30 frames, an IDR every 10, Annex B units (the SPS / PPS apart, as csd-0 / csd-1) */
    int w = 160, h = 96, frames = 30;
    ISVCEncoder *enc = NULL;
    WelsCreateSVCEncoder(&enc);
    SEncParamBase ep;
    memset(&ep, 0, sizeof ep);
    ep.iUsageType = CAMERA_VIDEO_REAL_TIME; ep.fMaxFrameRate = 30; ep.iPicWidth = w; ep.iPicHeight = h;
    ep.iTargetBitrate = 3000000; ep.iRCMode = RC_QUALITY_MODE;
    (*enc)->Initialize(enc, &ep);
    int idr = 10, vf = videoFormatI420;
    (*enc)->SetOption(enc, ENCODER_OPTION_IDR_INTERVAL, &idr);
    (*enc)->SetOption(enc, ENCODER_OPTION_DATAFORMAT, &vf);
    uint8_t *Y = malloc(w * h), *U = malloc(w * h / 4), *V = malloc(w * h / 4);
    buf au[64] = { 0 }, sps = { 0 }, pps = { 0 };
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
        for (int l = 0; l < info.iLayerNum; l++) {
            SLayerBSInfo *L = &info.sLayerInfo[l];
            const uint8_t *p = L->pBsBuf;
            for (int k = 0; k < L->iNalCount; k++) {
                int len = L->pNalLengthInByte[k], t = p[4] & 31;
                /* the first SPS / PPS are csd-0 / csd-1; all of them also stay in the units (the
                   encoder makes a new PPS id at each IDR) */
                if (t == 7 && !sps.n) put(&sps, p, len);
                if (t == 8 && !pps.n) put(&pps, p, len);
                put(&au[f], p, len);
                p += len;
            }
        }
    }
    (*enc)->Uninitialize(enc);
    WelsDestroySVCEncoder(enc);
    CHECK(sps.n && pps.n && au[frames - 1].n, "stream encoded");

    um_format *fmt = um_format_new();
    um_format_set_string(fmt, UM_KEY_MIME, UM_MIME_AVC);
    um_format_set_int32(fmt, UM_KEY_WIDTH, w);
    um_format_set_int32(fmt, UM_KEY_HEIGHT, h);
    um_format_set_buffer(fmt, "csd-0", sps.p, sps.n);
    um_format_set_buffer(fmt, "csd-1", pps.p, pps.n);
    um_codec *c = um_codec_create(UM_MIME_AVC);
    CHECK(c != NULL, "a decoder");
    if (!c) return 1;
    CHECK(um_codec_configure(c, fmt, NULL) == UM_OK && um_codec_start(c) == UM_OK, "configure/start");
    const char *name = um_codec_name(c);
    int hw = !strcmp(mode, "hw") || !strcmp(mode, "path") || !strcmp(mode, "h264env") || !strcmp(mode, "midway");
    CHECK(!strcmp(name, hw ? "pmhwdec.h264.decoder" : "OMX.google.h264.decoder"), "decoder %s", name);

    int got = 0, eos = 0, in_eos = 0, next = 0, stride = 0, fmts = 0, exact_n = 0, ordered = 1;
    double qmin = 99;
    int64_t last = -1;
    int exact_at[64] = { 0 };
    for (int spins = 0; spins < 4000 && !eos; spins++) {
        if (!in_eos) {
            int i = um_codec_dequeue_input(c, 1000);
            if (i >= 0) {
                size_t cap;
                uint8_t *b = um_codec_input_buffer(c, i, &cap);
                if (next == frames) { um_codec_queue_input(c, i, 0, 0, 0, UM_BUFFER_FLAG_END_OF_STREAM); in_eos = 1; }
                else {
                    memcpy(b, au[next].p, au[next].n);
                    um_codec_queue_input(c, i, 0, (int)au[next].n, (int64_t)next * 1000000 / 30, 0);
                    next++;
                }
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
            uint8_t *p = um_codec_output_buffer(c, o, &cap);
            int f = (int)((bi.pts_us * 30 + 500000) / 1000000), exact;
            double q;
            compare(p, stride, h, w, h, f, &exact, &q);
            if (f >= 0 && f < 64) exact_at[f] = exact;
            exact_n += exact;
            if (q < qmin) qmin = q;
            if (bi.pts_us <= last) ordered = 0;
            last = bi.pts_us;
            got++;
        }
        um_codec_release_output(c, o, 0);
    }
    CHECK(got == frames && eos && ordered, "%d of %d pictures, eos %d, in order %d", got, frames, eos, ordered);
    CHECK(fmts >= 1 && stride == w, "output format (stride %d)", stride);
    if (!strcmp(mode, "hw") || !strcmp(mode, "path") || !strcmp(mode, "h264env")) {
        CHECK(exact_n == frames, "every picture exactly the pattern (%d of %d)", exact_n, frames);
        CHECK(st && st->sps_before_first_slice, "the parameter sets went ahead of the first slice");
        CHECK(st && st->again > 0, "PMHW_AGAIN was waited out (%d times)", st ? st->again : 0);
        CHECK(st && st->held_max == 1, "each picture handed back before the next (%d out at once)", st ? st->held_max : -1);
    } else if (!strcmp(mode, "midway")) {
        int first10 = 1;
        for (int f = 0; f < 10; f++) first10 &= exact_at[f];
        CHECK(first10, "the first ten pictures from the hardware path, exactly");
        CHECK(st && st->refused == 1, "refused once");
        CHECK(qmin > 30, "OpenH264 went on (PSNR %.1f dB)", qmin);
    } else {
        CHECK(qmin > 30 && exact_n < frames, "OpenH264's pictures (PSNR %.1f dB)", qmin);
        if (!strcmp(mode, "refuse")) CHECK(st && st->refused >= 1 && st->slices == 0, "refused at configure, no slice sent");
        if (!strcmp(mode, "nohw") || !strcmp(mode, "off")) CHECK(st && st->sends == 0, "the library was not used");
    }
    um_codec_free(c);
    um_format_free(fmt);
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
