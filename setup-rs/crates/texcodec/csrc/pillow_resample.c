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

/* The sum of each output's coefficients (wrapping as the sums do). Where every input an output
 * reads is the same pixel, Pillow's sum init + p * k[0] + p * k[1] + ... is init + p * K: the same
 * 32-bit number (multiplication distributes over the wrapping sums), so the same bytes. */
static INT32 *
tc_coeff_sums(int n, int ksize, const INT32 *kk, const int *bounds) {
    INT32 *sum = malloc(sizeof(INT32) * (size_t)(n ? n : 1));
    if (!sum) return NULL;
    for (int i = 0; i < n; i++) {
        uint32_t t = 0;
        for (int j = 0; j < bounds[i * 2 + 1]; j++) t += (uint32_t)kk[(size_t)i * ksize + j];
        sum[i] = (INT32)t;
    }
    return sum;
}

/* bit x of diff (64 a word): pixel x of the line differs from pixel x + 1 */
static void
tc_row_diff(const UINT8 *line, int n, uint64_t *diff) {
    int words = (n + 63) / 64;
    for (int w = 0; w < words; w++) diff[w] = 0;
    static const uint32_t bit[4] = {1, 2, 4, 8};
    uint32x4_t bv = vld1q_u32(bit);
    int x = 0;
    for (; x + 5 <= n; x += 4) {
        uint32x4_t a = vld1q_u32((const uint32_t *)(line + (size_t)x * 4));
        uint32x4_t b = vld1q_u32((const uint32_t *)(line + (size_t)x * 4 + 4));
        uint64_t m = vaddvq_u32(vbicq_u32(bv, vceqq_u32(a, b)));
        diff[x >> 6] |= m << (x & 63);
    }
    for (; x + 1 < n; x++) {
        uint32_t a, b;
        memcpy(&a, line + (size_t)x * 4, 4);
        memcpy(&b, line + (size_t)x * 4 + 4, 4);
        if (a != b) diff[x >> 6] |= 1ull << (x & 63);
    }
}

/* whether pixels s .. s + len - 1 are all one pixel (no diff bit in s .. s + len - 2) */
static inline int
tc_flat(const uint64_t *diff, int s, int len) {
    int m = len - 1;
    while (m > 0) {
        int o = s & 63, take = 64 - o < m ? 64 - o : m;
        uint64_t mask = take == 64 ? ~0ull : ((1ull << take) - 1);
        if ((diff[s >> 6] >> o) & mask) return 0;
        s += take;
        m -= take;
    }
    return 1;
}

/* _ImagingResampleHorizontal_8bpc for 4 bands; an output whose inputs are all one pixel from the
 * sum of its coefficients (tc_coeff_sums) */
static void
tc_horizontal4(Imaging imOut, Imaging imIn, int offset, int ksize, int *bounds, double *prekk) {
    INT32 *kk = (INT32 *)prekk;
    tc_normalize(imOut->xsize, ksize, prekk);
    int xsize = imOut->xsize, ysize = imOut->ysize;
    INT32 *ksum = tc_coeff_sums(xsize, ksize, kk, bounds);
    uint64_t *diff = malloc(sizeof(uint64_t) * (size_t)((imIn->xsize + 63) / 64 + 1));
    if (!ksum || !diff) {
        free(ksum);
        free(diff);
        ksum = NULL;
        diff = NULL;
    }
    for (int yy = 0; yy < ysize; yy++) {
        const UINT8 *lineIn = (const UINT8 *)imIn->image[yy + offset];
        UINT8 *lineOut = (UINT8 *)imOut->image[yy];
        if (diff) tc_row_diff(lineIn, imIn->xsize, diff);
        for (int xx = 0; xx < xsize; xx++) {
            int xmin = bounds[xx * 2 + 0];
            int xmax = bounds[xx * 2 + 1];
            const INT32 *k = &kk[xx * ksize];
            const UINT8 *p = lineIn + (size_t)xmin * 4;
            if (diff && xmax > 0 && tc_flat(diff, xmin, xmax)) {
                int32x4_t a = vmlaq_n_s32(vdupq_n_s32(1 << (PRECISION_BITS - 1)), tc_px(p), ksum[xx]);
                uint32_t v = tc_clip4(a);
                memcpy(lineOut + (size_t)xx * 4, &v, 4);
                continue;
            }
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
    free(ksum);
    free(diff);
}

/* _ImagingResampleVertical_8bpc for 4 bands: four pixels (16 channels) at a time; where the rows
 * an output reads hold the same 16 bytes, from the sum of its coefficients (tc_coeff_sums) */
static void
tc_vertical4(Imaging imOut, Imaging imIn, int offset, int ksize, int *bounds, double *prekk) {
    (void)offset;
    INT32 *kk = (INT32 *)prekk;
    tc_normalize(imOut->ysize, ksize, prekk);
    int xsize = imOut->xsize, ysize = imOut->ysize;
    size_t line = (size_t)xsize * 4;
    size_t chunks = line / 16;
    /* cnt[y * chunks + c]: how many of rows 0 .. y differ from the row after them in chunk c, modulo
     * 256 (a window of under 256 rows is one value where its first and last counts are equal) */
    INT32 *ksum = NULL;
    UINT8 *cnt = NULL;
    if (ksize < 256 && chunks > 0) {
        ksum = tc_coeff_sums(ysize, ksize, kk, bounds);
        cnt = malloc(chunks * (size_t)(imIn->ysize ? imIn->ysize : 1));
        if (!ksum || !cnt) {
            free(ksum);
            free(cnt);
            ksum = NULL;
            cnt = NULL;
        }
    }
    if (cnt) {
        memset(cnt, 0, chunks);
        for (int y = 1; y < imIn->ysize; y++) {
            const UINT8 *r0 = (const UINT8 *)imIn->image[y - 1];
            const UINT8 *r1 = (const UINT8 *)imIn->image[y];
            const UINT8 *c0 = cnt + (size_t)(y - 1) * chunks;
            UINT8 *c1 = cnt + (size_t)y * chunks;
            for (size_t c = 0; c < chunks; c++) {
                uint8x16_t e = vceqq_u8(vld1q_u8(r0 + c * 16), vld1q_u8(r1 + c * 16));
                c1[c] = (UINT8)(c0[c] + (vminvq_u8(e) == 0));
            }
        }
    }
    for (int yy = 0; yy < ysize; yy++) {
        UINT8 *lineOut = (UINT8 *)imOut->image[yy];
        const INT32 *k = &kk[yy * ksize];
        int ymin = bounds[yy * 2 + 0];
        int ymax = bounds[yy * 2 + 1];
        const UINT8 *cfirst = cnt && ymax > 0 ? cnt + (size_t)ymin * chunks : NULL;
        const UINT8 *clast = cfirst ? cnt + (size_t)(ymin + ymax - 1) * chunks : NULL;
        size_t b = 0;
        for (; b + 16 <= line; b += 16) {
            int32x4_t a0 = vdupq_n_s32(1 << (PRECISION_BITS - 1)), a1 = a0, a2 = a0, a3 = a0;
            if (cfirst && cfirst[b / 16] == clast[b / 16]) {
                uint8x16_t v = vld1q_u8((const UINT8 *)imIn->image[ymin] + b);
                uint16x8_t lo = vmovl_u8(vget_low_u8(v));
                uint16x8_t hi = vmovl_u8(vget_high_u8(v));
                INT32 w = ksum[yy];
                a0 = vmlaq_n_s32(a0, vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(lo))), w);
                a1 = vmlaq_n_s32(a1, vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(lo))), w);
                a2 = vmlaq_n_s32(a2, vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(hi))), w);
                a3 = vmlaq_n_s32(a3, vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(hi))), w);
                goto store;
            }
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
        store:;
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
    free(ksum);
    free(cnt);
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
