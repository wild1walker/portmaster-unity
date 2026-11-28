// glarena_scan.h: the lowest and highest index of a draw, read from memory (glarena.c's ranged draws), one
// load an index (scan_plain) or 64 bytes a load group (scan_wide: NEON where there is NEON). Included by
// glarena.c and by test/test_arenascan.c, which checks that both give the same answer.
#ifndef GLARENA_SCAN_H
#define GLARENA_SCAN_H
#include <stddef.h>
#include <stdint.h>
static void scan_plain(const unsigned char *q, int n, unsigned sz, uint32_t *lo, uint32_t *hi)
{
    uint32_t l = 0xffffffffu, h = 0;
    for (int k = 0; k < n; k++, q += sz) {
        uint32_t v = sz == 1 ? q[0] : sz == 2 ? *(const unsigned short *)q : *(const uint32_t *)q;
        if (v < l) l = v;
        if (v > h) h = v;
    }
    *lo = l; *hi = h;
}
#include <arm_neon.h>
static void scan_wide(const unsigned char *q, int n, unsigned sz, uint32_t *lo, uint32_t *hi)
{
    size_t bytes = (size_t)n * sz, w = bytes & ~(size_t)63, k = 0;
    uint32_t l = 0xffffffffu, h = 0;
    if (w) {
        if (sz == 1) {
            uint8x16_t mn = vdupq_n_u8(0xff), mx = vdupq_n_u8(0);
            for (; k < w; k += 64) {
                uint8x16_t a = vld1q_u8(q + k), b = vld1q_u8(q + k + 16), c = vld1q_u8(q + k + 32), d = vld1q_u8(q + k + 48);
                mn = vminq_u8(mn, vminq_u8(vminq_u8(a, b), vminq_u8(c, d)));
                mx = vmaxq_u8(mx, vmaxq_u8(vmaxq_u8(a, b), vmaxq_u8(c, d)));
            }
            uint8_t t[16], u[16]; vst1q_u8(t, mn); vst1q_u8(u, mx);
            for (int i = 0; i < 16; i++) { if (t[i] < l) l = t[i]; if (u[i] > h) h = u[i]; }
        } else if (sz == 2) {
            uint16x8_t mn = vdupq_n_u16(0xffff), mx = vdupq_n_u16(0);
            for (; k < w; k += 64) {
                const uint16_t *p = (const uint16_t *)(const void *)(q + k);
                uint16x8_t a = vld1q_u16(p), b = vld1q_u16(p + 8), c = vld1q_u16(p + 16), d = vld1q_u16(p + 24);
                mn = vminq_u16(mn, vminq_u16(vminq_u16(a, b), vminq_u16(c, d)));
                mx = vmaxq_u16(mx, vmaxq_u16(vmaxq_u16(a, b), vmaxq_u16(c, d)));
            }
            uint16_t t[8], u[8]; vst1q_u16(t, mn); vst1q_u16(u, mx);
            for (int i = 0; i < 8; i++) { if (t[i] < l) l = t[i]; if (u[i] > h) h = u[i]; }
        } else {
            uint32x4_t mn = vdupq_n_u32(0xffffffffu), mx = vdupq_n_u32(0);
            for (; k < w; k += 64) {
                const uint32_t *p = (const uint32_t *)(const void *)(q + k);
                uint32x4_t a = vld1q_u32(p), b = vld1q_u32(p + 4), c = vld1q_u32(p + 8), d = vld1q_u32(p + 12);
                mn = vminq_u32(mn, vminq_u32(vminq_u32(a, b), vminq_u32(c, d)));
                mx = vmaxq_u32(mx, vmaxq_u32(vmaxq_u32(a, b), vmaxq_u32(c, d)));
            }
            uint32_t t[4], u[4]; vst1q_u32(t, mn); vst1q_u32(u, mx);
            for (int i = 0; i < 4; i++) { if (t[i] < l) l = t[i]; if (u[i] > h) h = u[i]; }
        }
    }
    uint32_t l2, h2;
    if (k < bytes) {
        scan_plain(q + k, (int)((bytes - k) / sz), sz, &l2, &h2);
        if (l2 < l) l = l2;
        if (h2 > h) h = h2;
    }
    *lo = l; *hi = h;
}
#endif
