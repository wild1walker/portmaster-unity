/* pm_resize: pmunity's downscale(), PIL's img.convert('RGBa').resize((w, h), LANCZOS).convert('RGBA'),
 * with Pillow's own Resample.c (its coefficients, its two-pass plan) and its Convert.c premultiply and
 * unpremultiply formulas. The two 8-bit passes for 4-band images are done here with WebAssembly SIMD, a
 * pixel's four channels at once: the same integer sums in the same 22-bit fixed point, so the same
 * pixels (pm_resize_ref is Pillow's own path, for tests). */
#include "Imaging.h"
#include "Resample.c"
#include <wasm_simd128.h>

#define EXPORT(n) __attribute__((export_name(n)))
#define SHIFTFORDIV255(a) ((((a) >> 8) + a) >> 8)
#define MULDIV255(a, b, tmp) (tmp = (a) * (b) + 128, SHIFTFORDIV255(tmp))
#define CLIP8(v) ((v) <= 0 ? 0 : (v) < 256 ? (v) : 255)

static inline v128_t px4(const UINT8 *p)                 /* 4 bytes -> 4 x int32 */
{
    uint32_t v; memcpy(&v, p, 4);
    return wasm_u32x4_extend_low_u16x8(wasm_u16x8_extend_low_u8x16(wasm_i32x4_make((int)v, 0, 0, 0)));
}
static inline void out4(UINT8 *p, v128_t ss)             /* clip8(): ss >> 22 clamped to 0..255 */
{
    v128_t v = wasm_i32x4_shr(ss, PRECISION_BITS);
    v = wasm_i32x4_max(wasm_i32x4_min(v, wasm_i32x4_splat(255)), wasm_i32x4_splat(0));
    v = wasm_u8x16_narrow_i16x8(wasm_i16x8_narrow_i32x4(v, v), wasm_i16x8_splat(0));
    uint32_t w = (uint32_t)wasm_i32x4_extract_lane(v, 0);
    memcpy(p, &w, 4);
}
/* _ImagingResampleHorizontal_8bpc, 4 bands */
static void horiz4(Imaging out, Imaging in, int offset, int ksize, int *bounds, double *prekk)
{
    INT32 *kk = (INT32 *)prekk;
    normalize_coeffs_8bpc(out->xsize, ksize, prekk);
    const v128_t half = wasm_i32x4_splat(1 << (PRECISION_BITS - 1));
    for (int yy = 0; yy < out->ysize; yy++) {
        const UINT8 *line = (const UINT8 *)in->image[yy + offset];
        UINT8 *o = (UINT8 *)out->image[yy];
        for (int xx = 0; xx < out->xsize; xx++) {
            int xmin = bounds[xx * 2], xmax = bounds[xx * 2 + 1];
            const INT32 *k = &kk[xx * ksize];
            v128_t ss = half;
            for (int x = 0; x < xmax; x++) ss = wasm_i32x4_add(ss, wasm_i32x4_mul(px4(line + (x + xmin) * 4), wasm_i32x4_splat(k[x])));
            out4(o + xx * 4, ss);
        }
    }
}
/* _ImagingResampleVertical_8bpc, 4 bands */
static void vert4(Imaging out, Imaging in, int offset, int ksize, int *bounds, double *prekk)
{
    (void)offset;
    INT32 *kk = (INT32 *)prekk;
    normalize_coeffs_8bpc(out->ysize, ksize, prekk);
    const v128_t half = wasm_i32x4_splat(1 << (PRECISION_BITS - 1));
    for (int yy = 0; yy < out->ysize; yy++) {
        UINT8 *o = (UINT8 *)out->image[yy];
        const INT32 *k = &kk[yy * ksize];
        int ymin = bounds[yy * 2], ymax = bounds[yy * 2 + 1];
        for (int xx = 0; xx < out->xsize; xx++) {
            v128_t ss = half;
            for (int y = 0; y < ymax; y++) ss = wasm_i32x4_add(ss, wasm_i32x4_mul(px4((const UINT8 *)in->image[y + ymin] + xx * 4), wasm_i32x4_splat(k[y])));
            out4(o + xx * 4, ss);
        }
    }
}

static Imaging premultiplied(const UINT8 *src, int sw, int sh)
{
    Imaging in = ImagingNewDirty(IMAGING_MODE_RGBa, sw, sh);
    if (!in) return NULL;
    for (int y = 0; y < sh; y++) {                       /* Convert.c rgbA2rgba */
        const UINT8 *s = src + (size_t)y * sw * 4;
        UINT8 *o = (UINT8 *)in->image[y];
        unsigned int tmp;
        for (int x = 0; x < sw; x++, s += 4, o += 4) {
            unsigned int a = s[3];
            o[0] = MULDIV255(s[0], a, tmp); o[1] = MULDIV255(s[1], a, tmp); o[2] = MULDIV255(s[2], a, tmp); o[3] = s[3];
        }
    }
    return in;
}
static void unpremultiply(Imaging out, UINT8 *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {                       /* Convert.c rgba2rgbA */
        const UINT8 *s = (const UINT8 *)out->image[y];
        UINT8 *o = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++, s += 4, o += 4) {
            unsigned int a = s[3];
            if (a == 255 || a == 0) { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; }
            else { o[0] = CLIP8((255 * s[0]) / a); o[1] = CLIP8((255 * s[1]) / a); o[2] = CLIP8((255 * s[2]) / a); }
            o[3] = s[3];
        }
    }
}

/* src: sw x sh RGBA (top row first), dst: dw x dh RGBA. 0 on success. */
EXPORT("pm_resize")
int pm_resize(const UINT8 *src, int sw, int sh, UINT8 *dst, int dw, int dh)
{
    Imaging in = premultiplied(src, sw, sh);
    if (!in) return -1;
    float box[4] = {0, 0, (float)sw, (float)sh};
    /* ImagingResampleInner with the SIMD passes */
    Imaging out = ImagingResampleInner(in, dw, dh, &LANCZOS, box, horiz4, vert4);
    ImagingDelete(in);
    if (!out) return -2;
    unpremultiply(out, dst, dw, dh);
    ImagingDelete(out);
    return 0;
}

EXPORT("pm_resize_ref")
int pm_resize_ref(const UINT8 *src, int sw, int sh, UINT8 *dst, int dw, int dh)
{
    Imaging in = premultiplied(src, sw, sh);
    if (!in) return -1;
    float box[4] = {0, 0, (float)sw, (float)sh};
    Imaging out = ImagingResample(in, dw, dh, IMAGING_TRANSFORM_LANCZOS, box);
    ImagingDelete(in);
    if (!out) return -2;
    unpremultiply(out, dst, dw, dh);
    ImagingDelete(out);
    return 0;
}
