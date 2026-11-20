/* Pillow's Resample.c (vendor/pillow, unchanged: included here, so it is compiled once, in this
 * file) and the two passes of its 8-bit resampling for 4-byte pixels written with NEON vectors on
 * aarch64. The vector passes do Pillow's own integer arithmetic (its coefficients, normalized to
 * PRECISION_BITS as normalize_coeffs_8bpc does, the same 32-bit sums, which -fwrapv makes wrap as
 * the vector lanes do, and clip8: the sum shifted and clamped to 0-255, which is what its lookup
 * table holds), so every byte comes out as Pillow's. tc_resample_rgba resamples with them;
 * ImagingResample is Pillow's path (tc_resample_pillow, which the unit tests compare against). */
#include "Resample.c"

#if defined(__aarch64__)
#include <arm_neon.h>

/* normalize_coeffs_8bpc (the same expressions; the products are exact, a power of two) */
static void
tc_normalize(int outSize, int ksize, double *prekk) {
    INT32 *kk = (INT32 *)prekk;
    for (int x = 0; x < outSize * ksize; x++) {
        if (prekk[x] < 0) {
            kk[x] = (int)(-0.5 + prekk[x] * (1 << PRECISION_BITS));
        } else {
            kk[x] = (int)(0.5 + prekk[x] * (1 << PRECISION_BITS));
        }
    }
}

/* four sums (one pixel's channels) -> four bytes: clip8 of each */
static inline uint32_t
tc_clip4(int32x4_t acc) {
    uint16x4_t n = vqmovun_s32(vshrq_n_s32(acc, PRECISION_BITS));
    uint8x8_t b = vqmovn_u16(vcombine_u16(n, n));
    return vget_lane_u32(vreinterpret_u32_u8(b), 0);
}

static inline int32x4_t
tc_px(const UINT8 *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    uint8x8_t b = vreinterpret_u8_u32(vdup_n_u32(v));
    return vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(b))));
}

/* _ImagingResampleHorizontal_8bpc for 4 bands */
static void
tc_horizontal4(Imaging imOut, Imaging imIn, int offset, int ksize, int *bounds, double *prekk) {
    INT32 *kk = (INT32 *)prekk;
    tc_normalize(imOut->xsize, ksize, prekk);
    int xsize = imOut->xsize, ysize = imOut->ysize;
    for (int yy = 0; yy < ysize; yy++) {
        const UINT8 *lineIn = (const UINT8 *)imIn->image[yy + offset];
        UINT8 *lineOut = (UINT8 *)imOut->image[yy];
        for (int xx = 0; xx < xsize; xx++) {
            int xmin = bounds[xx * 2 + 0];
            int xmax = bounds[xx * 2 + 1];
            const INT32 *k = &kk[xx * ksize];
            const UINT8 *p = lineIn + (size_t)xmin * 4;
            int32x4_t a0 = vdupq_n_s32(1 << (PRECISION_BITS - 1));
            int32x4_t a1 = vdupq_n_s32(0);
            int32x4_t a2 = vdupq_n_s32(0), a3 = vdupq_n_s32(0);
            int x = 0;
            /* (four taps a step: their coefficients loaded as one vector and taken by lane, four
             * sums so no multiply waits for the one before) */
            for (; x + 4 <= xmax; x += 4) {
                uint8x16_t v = vld1q_u8(p + (size_t)x * 4);
                int32x4_t kv = vld1q_s32(k + x);
                uint16x8_t lo = vmovl_u8(vget_low_u8(v));
                uint16x8_t hi = vmovl_u8(vget_high_u8(v));
                a0 = vmlaq_laneq_s32(a0, vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(lo))), kv, 0);
                a1 = vmlaq_laneq_s32(a1, vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(lo))), kv, 1);
                a2 = vmlaq_laneq_s32(a2, vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(hi))), kv, 2);
                a3 = vmlaq_laneq_s32(a3, vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(hi))), kv, 3);
            }
            a0 = vaddq_s32(a0, a2);
            a1 = vaddq_s32(a1, a3);
            for (; x < xmax; x++) {
                a0 = vmlaq_n_s32(a0, tc_px(p + (size_t)x * 4), k[x]);
            }
            uint32_t v = tc_clip4(vaddq_s32(a0, a1));
            memcpy(lineOut + (size_t)xx * 4, &v, 4);
        }
    }
}

/* _ImagingResampleVertical_8bpc for 4 bands: four pixels (16 channels) at a time */
static void
tc_vertical4(Imaging imOut, Imaging imIn, int offset, int ksize, int *bounds, double *prekk) {
    (void)offset;
    INT32 *kk = (INT32 *)prekk;
    tc_normalize(imOut->ysize, ksize, prekk);
    int xsize = imOut->xsize, ysize = imOut->ysize;
    size_t line = (size_t)xsize * 4;
    for (int yy = 0; yy < ysize; yy++) {
        UINT8 *lineOut = (UINT8 *)imOut->image[yy];
        const INT32 *k = &kk[yy * ksize];
        int ymin = bounds[yy * 2 + 0];
        int ymax = bounds[yy * 2 + 1];
        size_t b = 0;
        for (; b + 16 <= line; b += 16) {
            int32x4_t a0 = vdupq_n_s32(1 << (PRECISION_BITS - 1)), a1 = a0, a2 = a0, a3 = a0;
            for (int y = 0; y < ymax; y++) {
                uint8x16_t v = vld1q_u8((const UINT8 *)imIn->image[y + ymin] + b);
                uint16x8_t lo = vmovl_u8(vget_low_u8(v));
                uint16x8_t hi = vmovl_u8(vget_high_u8(v));
                int32x4_t w = vld1q_dup_s32(k + y);   /* (loaded to every lane: no move from a core register) */
                a0 = vmlaq_s32(a0, vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(lo))), w);
                a1 = vmlaq_s32(a1, vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(lo))), w);
                a2 = vmlaq_s32(a2, vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(hi))), w);
                a3 = vmlaq_s32(a3, vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(hi))), w);
            }
            uint16x8_t n0 = vcombine_u16(
                vqmovun_s32(vshrq_n_s32(a0, PRECISION_BITS)), vqmovun_s32(vshrq_n_s32(a1, PRECISION_BITS))
            );
            uint16x8_t n1 = vcombine_u16(
                vqmovun_s32(vshrq_n_s32(a2, PRECISION_BITS)), vqmovun_s32(vshrq_n_s32(a3, PRECISION_BITS))
            );
            vst1q_u8(lineOut + b, vcombine_u8(vqmovn_u16(n0), vqmovn_u16(n1)));
        }
        for (; b < line; b += 4) {
            int32x4_t a = vdupq_n_s32(1 << (PRECISION_BITS - 1));
            for (int y = 0; y < ymax; y++) {
                a = vmlaq_n_s32(a, tc_px((const UINT8 *)imIn->image[y + ymin] + b), k[y]);
            }
            uint32_t v = tc_clip4(a);
            memcpy(lineOut + b, &v, 4);
        }
    }
}
#endif

/* ImagingResample for the 4-byte 8-bit images texcodec resamples (RGBa, RGBA), with the vector
 * passes where there are; Pillow's own otherwise. */
Imaging
tc_resample_rgba(Imaging imIn, int xsize, int ysize, int filter, float box[4], int pillow) {
#if defined(__aarch64__)
    if (!pillow && imIn->type == IMAGING_TYPE_UINT8 && imIn->bands == 4 && !imIn->image8 &&
        filter == IMAGING_TRANSFORM_LANCZOS) {
        return ImagingResampleInner(imIn, xsize, ysize, &LANCZOS, box, tc_horizontal4, tc_vertical4);
    }
#else
    (void)pillow;
#endif
    return ImagingResample(imIn, xsize, ysize, filter, box);
}
