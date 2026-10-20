// H.264 decoding with Cisco's OpenH264 (BSD-2-Clause; built from source here, linked statically
// when UM_HAVE_H264). Input: Annex B access units (the extractor converts MP4's length-prefixed
// NAL units, as Android's does) after the SPS / PPS of csd-0 / csd-1. Pictures come out in
// presentation order with the time of the access unit they were coded in (OpenH264 carries the
// time through its reordering).
//
// What OpenH264 does not decode (interlaced pictures, profiles beyond High: 10-bit, 4:2:2, 4:4:4)
// is refused at configure from the SPS, and um_codec then takes the next H.264 decoder: the
// system's libavcodec when there is one (um_ffmpeg.c). Main and High profile are decoded exactly;
// B-frames only approximately (checked against libavcodec, test_media_ffmpeg), so a stream with
// reordered frames goes to libavcodec when the system has it.
#include "um_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef UM_HAVE_H264
#include <wels/codec_api.h>

typedef struct {
    um_dec base;
    ISVCDecoder *dec;
    uint8_t *csd;               /* SPS + PPS, Annex B, fed before the first access unit and after a flush */
    size_t csdn;
    int fed;
    int errors;
} h264dec;

/* ---- the SPS fields that decide (profile, interlacing) ----------------------------------------------- */
typedef struct { const uint8_t *p; size_t n, bit; } bits;
static int rbit(bits *b) { if (b->bit >= b->n * 8) return 0; int v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1; b->bit++; return v; }
static uint32_t rbits(bits *b, int k) { uint32_t v = 0; while (k--) v = v << 1 | rbit(b); return v; }
static uint32_t ue(bits *b) { int z = 0; while (!rbit(b) && z < 32) z++; return z ? ((1u << z) - 1) + rbits(b, z) : 0; }
static void se(bits *b) { (void)ue(b); }

/* 0: decodable by OpenH264; -1: not (the reason in *why) */
static int sps_check(const uint8_t *nal, size_t n, const char **why)
{
    /* emulation prevention bytes out */
    uint8_t r[256];
    size_t m = 0;
    for (size_t i = 1; i < n && m < sizeof r; i++) {
        if (i + 2 < n && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) { r[m++] = 0; if (m < sizeof r) r[m++] = 0; i += 2; continue; }
        r[m++] = nal[i];
    }
    bits b = { r, m, 0 };
    int profile = rbits(&b, 8);
    rbits(&b, 16);                                   /* constraint flags, level */
    ue(&b);                                          /* seq_parameter_set_id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134) {
        uint32_t chroma = ue(&b);
        if (chroma == 3) rbit(&b);
        uint32_t bd_luma = ue(&b), bd_chroma = ue(&b);
        if (chroma != 1) { *why = "chroma format other than 4:2:0"; return -1; }
        if (bd_luma || bd_chroma) { *why = "more than 8 bits per sample"; return -1; }
        rbit(&b);
        if (rbit(&b)) {                              /* seq_scaling_matrix_present_flag */
            for (int i = 0; i < 8; i++)
                if (rbit(&b)) {
                    int sz = i < 6 ? 16 : 64, last = 8, next = 8;
                    for (int j = 0; j < sz; j++) {
                        if (next) { int d = (int)ue(&b); d = d & 1 ? (d + 1) / 2 : -(d / 2); next = (last + d + 256) % 256; }
                        last = next ? next : last;
                    }
                }
        }
    }
    ue(&b);                                          /* log2_max_frame_num_minus4 */
    uint32_t poc = ue(&b);
    if (poc == 0) ue(&b);
    else if (poc == 1) { rbit(&b); se(&b); se(&b); uint32_t k = ue(&b); for (uint32_t i = 0; i < k && i < 256; i++) se(&b); }
    ue(&b);                                          /* max_num_ref_frames */
    rbit(&b);
    ue(&b); ue(&b);                                  /* picture size in macroblocks */
    if (!rbit(&b)) { *why = "interlaced (field) coding"; return -1; }   /* frame_mbs_only_flag */
    return 0;
}

static int h_init(h264dec *h)
{
    if (h->dec) { (*h->dec)->Uninitialize(h->dec); WelsDestroyDecoder(h->dec); h->dec = NULL; }
    if (WelsCreateDecoder(&h->dec) != 0 || !h->dec) return UM_ERR_UNKNOWN;
    SDecodingParam p;
    memset(&p, 0, sizeof p);
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    p.eEcActiveIdc = ERROR_CON_DISABLE;
    if ((*h->dec)->Initialize(h->dec, &p) != 0) return UM_ERR_UNKNOWN;
    h->fed = 0;
    return UM_OK;
}

static void add_csd(h264dec *h, const uint8_t *p, size_t n)
{
    uint8_t *c = realloc(h->csd, h->csdn + n);
    if (!c) return;
    memcpy(c + h->csdn, p, n);
    h->csd = c;
    h->csdn += n;
}

static int h_configure(um_dec *d, const um_format *in, um_format *out)
{
    h264dec *h = (h264dec *)d;
    (void)out;
    size_t n0 = 0, n1 = 0;
    const uint8_t *c0 = um_format_get_buffer(in, "csd-0", &n0), *c1 = um_format_get_buffer(in, "csd-1", &n1);
    if (c0) add_csd(h, c0, n0);
    if (c1) add_csd(h, c1, n1);
    /* B-frames: OpenH264 2.3 decodes them, but not exactly (a few hundred bytes of a picture
       differ from the reference decoder's until the next IDR); libavcodec takes them when the
       system has it */
    int32_t reo = 0;
    const char *force = getenv("UM_H264");             /* UM_H264=openh264: OpenH264 for every stream */
    if (um_format_get_int32(in, "x-reordered", &reo) && reo && !(force && !strcmp(force, "openh264"))) {
        if (um_ffmpeg_available()) return UM_ERR_UNSUPPORTED;
        um_log("openh264: a stream with B-frames and no system libavcodec: pictures can differ slightly from the reference decoder's");
    }
    /* every SPS in the parameter sets */
    for (size_t i = 0; i + 4 < h->csdn; i++) {
        if (h->csd[i] || h->csd[i + 1] || h->csd[i + 2] != 1 || (h->csd[i + 3] & 31) != 7) continue;
        size_t e = i + 3;
        while (e + 3 < h->csdn && !(h->csd[e] == 0 && h->csd[e + 1] == 0 && (h->csd[e + 2] == 1 || (h->csd[e + 2] == 0 && h->csd[e + 3] == 1)))) e++;
        if (e + 3 >= h->csdn) e = h->csdn;
        const char *why = NULL;
        if (sps_check(h->csd + i + 3, e - i - 3, &why) != 0) {
            um_log("openh264: %s: not for OpenH264", why);
            return UM_ERR_UNSUPPORTED;
        }
    }
    return h_init(h);
}

static void emit(h264dec *h, uint8_t **pl, SBufferInfo *bi, um_emit *em)
{
    (void)h;
    SSysMEMBuffer *sb = &bi->UsrData.sSystemBuffer;
    em->video(em->codec, pl[0], sb->iStride[0], pl[1], sb->iStride[1], pl[2], sb->iStride[1], sb->iWidth, sb->iHeight,
              (int64_t)bi->uiOutYuvTimeStamp);
}

static int h_decode(um_dec *d, const uint8_t *data, size_t size, int64_t pts, uint32_t flags, um_emit *em)
{
    h264dec *h = (h264dec *)d;
    if (!h->dec && h_init(h) != UM_OK) return UM_ERR_UNKNOWN;
    uint8_t *pl[3] = { 0 };
    SBufferInfo bi;
    if (!h->fed && h->csdn) {
        memset(&bi, 0, sizeof bi);
        (*h->dec)->DecodeFrameNoDelay(h->dec, h->csd, (int)h->csdn, pl, &bi);
        h->fed = 1;
    }
    if (flags & UM_BUFFER_FLAG_CODEC_CONFIG) {
        if (!h->fed) add_csd(h, data, size);
        memset(&bi, 0, sizeof bi);
        (*h->dec)->DecodeFrameNoDelay(h->dec, data, (int)size, pl, &bi);
        return UM_OK;
    }
    if (size) {
        memset(&bi, 0, sizeof bi);
        bi.uiInBsTimeStamp = (unsigned long long)pts;
        DECODING_STATE r = (*h->dec)->DecodeFrameNoDelay(h->dec, data, (int)size, pl, &bi);
        if (r != dsErrorFree && h->errors++ < 5) um_log("openh264: decode state 0x%x at %lld us", r, (long long)pts);
        if (bi.iBufferStatus == 1) emit(h, pl, &bi, em);
    }
    if (flags & UM_BUFFER_FLAG_END_OF_STREAM) {
        int eos = 1, left = 0;
        (*h->dec)->SetOption(h->dec, DECODER_OPTION_END_OF_STREAM, &eos);
        (*h->dec)->GetOption(h->dec, DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, &left);
        for (int i = 0; i < left + 1 && i < 64; i++) {
            memset(&bi, 0, sizeof bi);
            (*h->dec)->FlushFrame(h->dec, pl, &bi);
            if (bi.iBufferStatus != 1) break;
            emit(h, pl, &bi, em);
        }
    }
    return UM_OK;
}

static void h_flush(um_dec *d) { h_init((h264dec *)d); }

static void h_destroy(um_dec *d)
{
    h264dec *h = (h264dec *)d;
    if (h->dec) { (*h->dec)->Uninitialize(h->dec); WelsDestroyDecoder(h->dec); }
    free(h->csd);
    free(h);
}

um_dec *um_h264_create(const char *mime)
{
    (void)mime;
    h264dec *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->base.name = "openh264";
    h->base.configure = h_configure;
    h->base.decode = h_decode;
    h->base.flush = h_flush;
    h->base.destroy = h_destroy;
    return &h->base;
}
#endif
