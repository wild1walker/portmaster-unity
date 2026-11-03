/* A small C interface to libvpx's VP8 decoder and encoder (vendor/libvpx), so the Rust side
 * (src/vpx.rs) needs none of libvpx's structs: a decoder that hands out I420 planes, and an
 * encoder fed I420 planes that hands back compressed frames one by one. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vpx/vp8cx.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vpx_encoder.h"

typedef struct vd {
    vpx_codec_ctx_t ctx;
    vpx_codec_iter_t it;
} vd;

vd *vd_new(int threads)
{
    vd *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    vpx_codec_dec_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.threads = threads > 0 ? (unsigned)threads : 1;
    if (vpx_codec_dec_init(&d->ctx, vpx_codec_vp8_dx(), &cfg, 0) != VPX_CODEC_OK) { free(d); return NULL; }
    return d;
}

/* 0: decoded (the pictures it made come from vd_frame), else libvpx's error */
int vd_decode(vd *d, const uint8_t *data, size_t n)
{
    d->it = NULL;
    return (int)vpx_codec_decode(&d->ctx, data, (unsigned)n, NULL, 0);
}

/* the next picture of the last vd_decode: 1 and its planes (8-bit 4:2:0), or 0 */
int vd_frame(vd *d, const uint8_t **y, int *ys, const uint8_t **u, int *us, const uint8_t **v, int *vs,
             int *w, int *h)
{
    vpx_image_t *img = vpx_codec_get_frame(&d->ctx, &d->it);
    if (!img) return 0;
    if (img->fmt != VPX_IMG_FMT_I420) return -1;
    *y = img->planes[VPX_PLANE_Y]; *ys = img->stride[VPX_PLANE_Y];
    *u = img->planes[VPX_PLANE_U]; *us = img->stride[VPX_PLANE_U];
    *v = img->planes[VPX_PLANE_V]; *vs = img->stride[VPX_PLANE_V];
    *w = (int)img->d_w; *h = (int)img->d_h;
    return 1;
}

const char *vd_error(vd *d) { return vpx_codec_error(&d->ctx); }

void vd_free(vd *d)
{
    if (!d) return;
    vpx_codec_destroy(&d->ctx);
    free(d);
}

typedef struct ve {
    vpx_codec_ctx_t ctx;
    vpx_codec_iter_t it;
    vpx_image_t img;
    int w, h;
} ve;

/* time base 1/1000 s (pts and durations in ms); constant quality cq_level (0 - 63) under
 * max_kbps; good-quality deadline at speed cpu_used (0 - 5); no look-ahead (frames come out in
 * order), key frames only where ve_encode asks; 1 << log2_partitions token partitions (a decoder
 * can split those rows over threads) */
ve *ve_new(int w, int h, int threads, int log2_partitions, int cpu_used, int cq_level, int max_kbps)
{
    ve *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    vpx_codec_enc_cfg_t cfg;
    if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0) != VPX_CODEC_OK) { free(e); return NULL; }
    cfg.g_w = (unsigned)w;
    cfg.g_h = (unsigned)h;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = 1000;
    cfg.g_threads = threads > 0 ? (unsigned)threads : 1;
    cfg.g_lag_in_frames = 0;
    cfg.g_error_resilient = 0;
    cfg.rc_end_usage = VPX_CQ;
    cfg.rc_target_bitrate = (unsigned)max_kbps;
    cfg.rc_min_quantizer = 0;
    cfg.rc_max_quantizer = 63;
    cfg.kf_mode = VPX_KF_DISABLED;
    if (vpx_codec_enc_init(&e->ctx, vpx_codec_vp8_cx(), &cfg, 0) != VPX_CODEC_OK) { free(e); return NULL; }
    if (vpx_codec_control(&e->ctx, VP8E_SET_CPUUSED, cpu_used) != VPX_CODEC_OK ||
        vpx_codec_control(&e->ctx, VP8E_SET_CQ_LEVEL, cq_level) != VPX_CODEC_OK ||
        vpx_codec_control(&e->ctx, VP8E_SET_TOKEN_PARTITIONS, log2_partitions) != VPX_CODEC_OK ||
        vpx_codec_control(&e->ctx, VP8E_SET_NOISE_SENSITIVITY, 0) != VPX_CODEC_OK) {
        vpx_codec_destroy(&e->ctx);
        free(e);
        return NULL;
    }
    /* the planes are the caller's (ve_encode): only the image's description here */
    e->img.fmt = VPX_IMG_FMT_I420;
    e->img.w = e->img.d_w = (unsigned)w;
    e->img.h = e->img.d_h = (unsigned)h;
    e->img.x_chroma_shift = e->img.y_chroma_shift = 1;
    e->img.bps = 12;
    e->w = w; e->h = h;
    return e;
}

/* one picture (y NULL: the end, the encoder flushed); its packets come from ve_packet */
int ve_encode(ve *e, const uint8_t *y, int ys, const uint8_t *u, int us, const uint8_t *v, int vs,
              int64_t pts_ms, unsigned long duration_ms, int key)
{
    e->it = NULL;
    if (!y) return (int)vpx_codec_encode(&e->ctx, NULL, 0, 0, 0, VPX_DL_GOOD_QUALITY);
    e->img.planes[VPX_PLANE_Y] = (unsigned char *)y; e->img.stride[VPX_PLANE_Y] = ys;
    e->img.planes[VPX_PLANE_U] = (unsigned char *)u; e->img.stride[VPX_PLANE_U] = us;
    e->img.planes[VPX_PLANE_V] = (unsigned char *)v; e->img.stride[VPX_PLANE_V] = vs;
    return (int)vpx_codec_encode(&e->ctx, &e->img, pts_ms, duration_ms ? duration_ms : 1,
                                 key ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_GOOD_QUALITY);
}

/* the next compressed frame of the last ve_encode: 1, else 0 */
int ve_packet(ve *e, const uint8_t **data, size_t *n, int64_t *pts_ms, int *key)
{
    const vpx_codec_cx_pkt_t *p;
    while ((p = vpx_codec_get_cx_data(&e->ctx, &e->it))) {
        if (p->kind != VPX_CODEC_CX_FRAME_PKT) continue;
        *data = p->data.frame.buf;
        *n = p->data.frame.sz;
        *pts_ms = p->data.frame.pts;
        *key = (p->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
        return 1;
    }
    return 0;
}

const char *ve_error(ve *e) { return vpx_codec_error(&e->ctx); }

void ve_free(ve *e)
{
    if (!e) return;
    vpx_codec_destroy(&e->ctx);
    free(e);
}
