/* pm_decode: a Unity texture's pixel data -> RGBA, as UnityPy's parse_image_data(flip=False) and
 * pmunity's texture_image() make it: block formats through Pillow's own BcnDecode.c (decoded at the
 * size padded to whole blocks, then cropped), raw formats as Pillow's raw unpackers read them. Rows
 * stay in the data's order (Unity's: bottom first). */
#include "Imaging.h"
#include "Bcn.h"
#include "BcnDecode.c"

#define EXPORT(n) __attribute__((export_name(n)))

/* Unity TextureFormat numbers this decodes */
enum { TF_RGB24 = 3, TF_RGBA32 = 4, TF_ARGB32 = 5, TF_DXT1 = 10, TF_DXT5 = 12, TF_BGRA32 = 14, TF_BC4 = 26,
       TF_BC5 = 27, TF_BC7 = 25, TF_DXT1C = 28, TF_DXT5C = 29 };

static int bcn_of(int fmt)
{
    switch (fmt) {
    case TF_DXT1: case TF_DXT1C: return 1;
    case TF_DXT5: case TF_DXT5C: return 3;
    case TF_BC4: return 4;
    case TF_BC5: return 5;
    case TF_BC7: return 7;
    }
    return 0;
}

/* decode_bc7_block, step for step (the same tables, endpoints, p-bits, expansion, Pillow's index bit
 * positions, anchors, lerp and rotation), with the block read once as two 64-bit halves instead of a
 * two-byte read per field: the same pixels, faster (pm_decode_ref keeps Pillow's own for tests). */
static inline unsigned bits64(uint64_t lo, uint64_t hi, int at, int n)
{
    uint64_t v = at >= 64 ? hi >> (at - 64) : (lo >> at) | (at && at + n > 64 ? hi << (64 - at) : 0);
    return (unsigned)v & ((1u << n) - 1);
}
static void bc7_fast(rgba *col, const UINT8 *src)
{
    uint64_t lo, hi;
    memcpy(&lo, src, 8); memcpy(&hi, src + 8, 8);
    int mode = src[0];
    if (!mode) { for (int i = 0; i < 16; i++) { col[i].r = col[i].g = col[i].b = 0; col[i].a = 255; } return; }
    int bit = __builtin_ctz(mode) + 1;
    mode = bit - 1;
    const bc7_mode_info *info = &bc7_modes[mode];
#define GET(at, n) ((UINT8)((n) ? bits64(lo, hi, (at), (n)) : 0))
#define LOAD(dst, n) do { dst = GET(bit, n); bit += (n); } while (0)
    int cb = info->cb, ab = info->ab;
    const char *cw = bc7_get_weights(info->ib), *aw = bc7_get_weights((ab && info->ib2) ? info->ib2 : info->ib);
    UINT8 partition, rotation, index_sel, val;
    LOAD(partition, info->pb); LOAD(rotation, info->rb); LOAD(index_sel, info->isb);
    int numep = info->ns << 1;
    rgba ep[6];
    for (int i = 0; i < numep; i++) LOAD(ep[i].r, cb);
    for (int i = 0; i < numep; i++) LOAD(ep[i].g, cb);
    for (int i = 0; i < numep; i++) LOAD(ep[i].b, cb);
    for (int i = 0; i < numep; i++) { if (ab) LOAD(ep[i].a, ab); else ep[i].a = 255; }
    if (info->epb) {
        cb++; if (ab) ab++;
        for (int i = 0; i < numep; i++) {
            LOAD(val, 1);
            ep[i].r = (UINT8)(ep[i].r << 1 | val); ep[i].g = (UINT8)(ep[i].g << 1 | val); ep[i].b = (UINT8)(ep[i].b << 1 | val);
            if (ab) ep[i].a = (UINT8)(ep[i].a << 1 | val);
        }
    }
    if (info->spb) {
        cb++; if (ab) ab++;
        for (int i = 0; i < numep; i += 2) {
            LOAD(val, 1);
            for (int j = 0; j < 2; j++) {
                ep[i + j].r = (UINT8)(ep[i + j].r << 1 | val); ep[i + j].g = (UINT8)(ep[i + j].g << 1 | val);
                ep[i + j].b = (UINT8)(ep[i + j].b << 1 | val);
                if (ab) ep[i + j].a = (UINT8)(ep[i + j].a << 1 | val);
            }
        }
    }
    for (int i = 0; i < numep; i++) {
        ep[i].r = expand_quantized(ep[i].r, cb); ep[i].g = expand_quantized(ep[i].g, cb); ep[i].b = expand_quantized(ep[i].b, cb);
        if (ab) ep[i].a = expand_quantized(ep[i].a, ab);
    }
    int cibit = bit, aibit = cibit + 16 * info->ib - info->ns;
    for (int i = 0; i < 16; i++) {
        int s = bc7_get_subset(info->ns, partition, i) << 1;
        int ib = info->ib;
        if (i == 0) ib--;
        else if (info->ns == 2) { if (i == bc7_ai0[partition]) ib--; }
        else if (info->ns == 3) { if (i == bc7_ai1[partition]) ib--; else if (i == bc7_ai2[partition]) ib--; }
        int i0 = GET(cibit, ib); cibit += ib;
        if (ab && info->ib2) {
            int ib2 = info->ib2;
            if (i == 0) ib2--;
            int i1 = GET(aibit, ib2); aibit += ib2;
            if (index_sel) bc7_lerp(&col[i], &ep[s], aw[i1], cw[i0]); else bc7_lerp(&col[i], &ep[s], cw[i0], aw[i1]);
        } else bc7_lerp(&col[i], &ep[s], cw[i0], cw[i0]);
        if (rotation == 1) { val = col[i].r; col[i].r = col[i].a; col[i].a = val; }
        else if (rotation == 2) { val = col[i].g; col[i].g = col[i].a; col[i].a = val; }
        else if (rotation == 3) { val = col[i].b; col[i].b = col[i].a; col[i].a = val; }
    }
#undef LOAD
#undef GET
}

static int decode_any(int fmt, const UINT8 *src, int len, int w, int h, UINT8 *dst, int ref);

/* src: len bytes of the first level (block formats: DXT bytes, crunch already unpacked); dst: w*h*4.
 * Returns 0, or < 0 for a format it does not decode or too little data. */
EXPORT("pm_decode")
int pm_decode(int fmt, const UINT8 *src, int len, int w, int h, UINT8 *dst) { return decode_any(fmt, src, len, w, h, dst, 0); }
EXPORT("pm_decode_ref")
int pm_decode_ref(int fmt, const UINT8 *src, int len, int w, int h, UINT8 *dst) { return decode_any(fmt, src, len, w, h, dst, 1); }

static int decode_any(int fmt, const UINT8 *src, int len, int w, int h, UINT8 *dst, int ref)
{
    if (fmt == TF_BC7 && !ref) {                         /* (as put_block writes a padded image, then cropped) */
        int bx = (w + 3) / 4, by = (h + 3) / 4;
        if ((long)len < (long)bx * by * 16) {            /* Pillow decodes the blocks it has; the rest stays zero */
            memset(dst, 0, (size_t)w * h * 4);
        }
        int nb = len / 16;
        for (int b = 0; b < bx * by && b < nb; b++) {
            rgba col[16];
            bc7_fast(col, src + 16 * (size_t)b);
            int x0 = (b % bx) * 4, y0 = (b / bx) * 4;
            for (int j = 0; j < 4; j++) {
                int y = y0 + j;
                if (y >= h) break;
                int n = w - x0 < 4 ? w - x0 : 4;
                memcpy(dst + ((size_t)y * w + x0) * 4, &col[j * 4], (size_t)n * 4);
            }
        }
        return 0;
    }
    size_t n = (size_t)w * h;
    if (fmt == TF_RGB24 || fmt == TF_RGBA32 || fmt == TF_ARGB32 || fmt == TF_BGRA32) {
        int ps = fmt == TF_RGB24 ? 3 : 4;
        if ((size_t)len < n * ps) return -2;
        for (size_t i = 0; i < n; i++, src += ps, dst += 4) {
            switch (fmt) {
            case TF_RGB24: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; break;
            case TF_RGBA32: memcpy(dst, src, 4); break;
            case TF_ARGB32: dst[0] = src[1]; dst[1] = src[2]; dst[2] = src[3]; dst[3] = src[0]; break;
            case TF_BGRA32: dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3]; break;
            }
        }
        return 0;
    }
    int N = bcn_of(fmt);
    if (!N) return -1;
    int pw = (w + 3) & ~3, ph = (h + 3) & ~3;
    int mode = N == 4 ? IMAGING_MODE_L : N == 5 ? IMAGING_MODE_RGB : IMAGING_MODE_RGBA;
    Imaging im = ImagingNewDirty(mode, pw, ph);
    if (!im) return -3;
    memset(im->block, 0, (size_t)pw * ph * im->pixelsize);
    struct ImagingCodecStateInstance st = {0};
    BCNSTATE ctx = {(char *)""};
    st.xsize = pw; st.ysize = ph; st.ystep = 1; st.context = &ctx;
    decode_bcn(im, &st, src, len, N, 0, ctx.pixel_format);
    for (int y = 0; y < h; y++) {
        const UINT8 *s = (const UINT8 *)im->image[y];
        for (int x = 0; x < w; x++, dst += 4) {
            if (N == 4) { dst[0] = s[x]; dst[1] = 0; dst[2] = 0; dst[3] = 255; }       /* L -> (r, 0, 0, 1) */
            else if (N == 5) { memcpy(dst, s + 4 * x, 3); dst[3] = 255; }                /* RGB -> RGBA */
            else memcpy(dst, s + 4 * x, 4);
        }
    }
    ImagingDelete(im);
    return 0;
}
