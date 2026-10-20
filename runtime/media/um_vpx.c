// VP8 / VP9 decoding with libvpx (BSD-3-Clause, linked in statically when UM_HAVE_VPX).
// One compressed frame in, at most one visible picture out (VP8 alt-ref frames show nothing,
// a VP9 superframe shows one), with the input's time. libvpx's own threads: up to 4 (VP8 uses
// them for streams with several token partitions, VP9 for tiles).
#include "um_internal.h"

#include <stdlib.h>
#include <string.h>

#ifndef UM_HAVE_VPX
um_dec *um_vpx_create(const char *mime) { (void)mime; return NULL; }
#else
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

typedef struct {
    um_dec base;
    vpx_codec_ctx_t ctx;
    int vp9, inited;
    uint8_t *tmp;               /* 4:4:4 / 4:2:2 / 4:4:0 pictures subsampled to 4:2:0 */
    size_t tmp_n;
} vpxdec;

static int vpx_init(vpxdec *v)
{
    if (v->inited) vpx_codec_destroy(&v->ctx);
    v->inited = 0;
    vpx_codec_dec_cfg_t cfg = { 0 };
    int n = um_cpu_count();
    cfg.threads = n > 4 ? 4 : n;
    const char *e = getenv("UM_VPX_THREADS");
    if (e && atoi(e) > 0) cfg.threads = atoi(e);
    vpx_codec_err_t r = vpx_codec_dec_init(&v->ctx, v->vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(), &cfg, 0);
    if (r != VPX_CODEC_OK) { um_log("vpx: init failed: %s", vpx_codec_err_to_string(r)); return UM_ERR_UNKNOWN; }
    v->inited = 1;
    return UM_OK;
}

static int vpx_configure(um_dec *d, const um_format *in, um_format *out)
{
    (void)in; (void)out;
    return vpx_init((vpxdec *)d);
}

/* a picture that is not 8-bit 4:2:0 (VP9 profiles 1-3): to 4:2:0 by taking every other chroma
   sample (8-bit only) */
static int to420(vpxdec *v, vpx_image_t *img, const uint8_t **u, int *us, const uint8_t **vv, int *vs)
{
    if (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) return -1;
    int cw = (img->d_w + 1) / 2, ch = (img->d_h + 1) / 2;
    size_t need = (size_t)cw * ch * 2;
    if (need > v->tmp_n) {
        uint8_t *t = realloc(v->tmp, need);
        if (!t) return -1;
        v->tmp = t; v->tmp_n = need;
    }
    int sx = img->x_chroma_shift ? 1 : 2, sy = img->y_chroma_shift ? 1 : 2;
    for (int p = 0; p < 2; p++) {
        uint8_t *dst = v->tmp + (size_t)p * cw * ch;
        const uint8_t *src = img->planes[p + 1];
        int stride = img->stride[p + 1];
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                dst[(size_t)y * cw + x] = src[(size_t)y * sy * stride + (size_t)x * sx];
    }
    *u = v->tmp; *us = cw;
    *vv = v->tmp + (size_t)cw * ch; *vs = cw;
    return 0;
}

static int vpx_decode(um_dec *d, const uint8_t *data, size_t size, int64_t pts, uint32_t flags, um_emit *em)
{
    vpxdec *v = (vpxdec *)d;
    if (!v->inited && vpx_init(v) != UM_OK) return UM_ERR_UNKNOWN;
    if (flags & UM_BUFFER_FLAG_CODEC_CONFIG) return UM_OK;
    vpx_codec_err_t r = vpx_codec_decode(&v->ctx, size ? data : NULL, (unsigned)size, NULL, 0);
    if (r != VPX_CODEC_OK) {
        const char *det = vpx_codec_error_detail(&v->ctx);
        um_log("vpx: decode error at %lld us: %s%s%s", (long long)pts, vpx_codec_err_to_string(r), det ? ": " : "", det ? det : "");
        return UM_ERR_MALFORMED;
    }
    vpx_codec_iter_t it = NULL;
    vpx_image_t *img;
    while ((img = vpx_codec_get_frame(&v->ctx, &it))) {
        const uint8_t *u = img->planes[VPX_PLANE_U], *vp = img->planes[VPX_PLANE_V];
        int us = img->stride[VPX_PLANE_U], vs = img->stride[VPX_PLANE_V];
        if (img->fmt == VPX_IMG_FMT_YV12) { const uint8_t *t = u; u = vp; vp = t; int ts = us; us = vs; vs = ts; }
        else if (img->fmt != VPX_IMG_FMT_I420) {
            if (to420(v, img, &u, &us, &vp, &vs) != 0) {
                static int warned;
                if (!warned++) um_log("vpx: picture format 0x%x not handled", img->fmt);
                continue;
            }
        }
        em->video(em->codec, img->planes[VPX_PLANE_Y], img->stride[VPX_PLANE_Y], u, us, vp, vs,
                  (int)img->d_w, (int)img->d_h, pts);
    }
    return UM_OK;
}

static void vpx_flush(um_dec *d)
{
    /* the next frame after a seek is a key frame, which resets the decoder; a fresh decoder
       also drops whatever VP9 frame-parallel mode would have queued */
    vpxdec *v = (vpxdec *)d;
    if (v->inited && v->vp9) vpx_init(v);
}

static void vpx_destroy(um_dec *d)
{
    vpxdec *v = (vpxdec *)d;
    if (v->inited) vpx_codec_destroy(&v->ctx);
    free(v->tmp);
    free(v);
}

um_dec *um_vpx_create(const char *mime)
{
    vpxdec *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->vp9 = !strcmp(mime, UM_MIME_VP9);
    v->base.name = v->vp9 ? "libvpx-vp9" : "libvpx-vp8";
    v->base.configure = vpx_configure;
    v->base.decode = vpx_decode;
    v->base.flush = vpx_flush;
    v->base.destroy = vpx_destroy;
    return &v->base;
}
#endif
