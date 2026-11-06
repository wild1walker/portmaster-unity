// unityverts.c: Unity's dynamic batching vertex transform without its per-vertex overhead
// (UA_FAST_VERTS=1 / check; off by default).
//
// Unity batches small meshes that share a material by transforming their vertices on the CPU into
// one buffer (dynamic batching; Hollow Knight's tk2d sprites): TransformVerticesInnerLoop<false,
// false, true> writes each vertex's position (the 4x4 matrix times it), a constant normal and
// tangent when the flags ask for them, the vertex colour, the vertex's other attributes copied from
// a second stream, and zero padding. Unity's ARMv7 build of it reloads the matrix from the stack for
// every vertex and calls memmove for the few bytes of each vertex's other attributes: 2.7 % of the
// render thread in Greenpath on its own, plus the memmove calls.
//
// The same function here: the same VFP operations on the same operands in the same order (each
// output is the same float, bit for bit: VFP multiply and add, no fused multiply-add, as Unity's
// code), the matrix in registers, the attribute bytes copied inline for the usual sizes. The flags'
// meaning was read from the code of the row below: 8 a constant normal (the 3x3 matrix's third
// column, negated with 256 clear), 64 a constant tangent (the 4x4's first column and a sign: 256,
// 512, 1024, 2048), 512/1024 a mirrored x/y (the matrix's first/second column negated), 16 the
// colour, 32 eight zero bytes after the other attributes.
//
// UA_FAST_VERTS=check runs Unity's own function as well, into a buffer of its own, and compares
// every byte of every call (`[unityverts] check: <calls> calls, <vertices> vertices, <n> differ`
// every 600 checked calls, and the first differences): how a new row is verified on real game data
// (the emulated rig runs Unity's code too).
#include "unityverts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unityhooks.h"

static const struct uh_row g_rows[] = {
    // TransformVerticesInnerLoop<false, false, true>(unsigned char*, Matrix4x4f const&, Matrix3x3f const&, bool,
    // unsigned int, unsigned char const*, int, unsigned char const*, int, int, GfxTransformVerticesFlags, unsigned int)
    { "6000.0.61f1 armeabi-v7a", "TransformVerticesInnerLoop<false, false, true>", 0xab7344, 0x27c, 'a', 0xa8c11455644d88b3ull },
};

typedef void (*loop_fn)(unsigned char *dst, const float *m, const float *m3, int b, unsigned count,
                        const unsigned char *src, int src_stride, const unsigned char *extra, int extra_stride,
                        int extra_size, unsigned flags, unsigned color);
static loop_fn o_loop;
static int g_check;
static unsigned long g_calls, g_verts, g_differ;

static inline void put(unsigned char **d, float f) { memcpy(*d, &f, 4); *d += 4; }

static void fast_loop(unsigned char *dst, const float *m, const float *m3, int b, unsigned count,
                      const unsigned char *src, int src_stride, const unsigned char *extra, int extra_stride,
                      int extra_size, unsigned flags, unsigned color)
{
    (void)b;
    float n0 = 0, n1 = 0, n2 = 0, t0 = 0, t1 = 0, t2 = 0, tw = 0;
    if (flags & 8) {                                // the normal: +-(m3[6], m3[7], m3[8])
        float s = flags & 256 ? 1.0f : 0.0f;
        s = s + s;
        s = 1.0f - s;
        n0 = s * m3[6]; n1 = s * m3[7]; n2 = s * m3[8];
    }
    unsigned flip = flags & 1536;
    if (flags & 64) {                               // the tangent: +-(m[0], m[1], m[2]), w +-1
        float s = flags & 256 ? -1.0f : 1.0f;
        t0 = m[0]; t1 = m[1]; t2 = m[2];
        if (flip == 1536) { t0 = -t0; t1 = -t1; t2 = -t2; }
        else if (flip == 1024) s = -s;
        else if (flip == 512) { s = -s; t0 = -t0; t1 = -t1; t2 = -t2; }
        tw = flags & 2048 ? -s : s;
    }
    float m0 = m[0], m1 = m[1], m2 = m[2], m4 = m[4], m5 = m[5], m6 = m[6];
    float m8 = m[8], m9 = m[9], m10 = m[10], m12 = m[12], m13 = m[13], m14 = m[14];
    if (flip) {                                     // Matrix4x4f::Scale(sx, sy, 1): columns times the scale
        float sx = flags & 512 ? -1.0f : 1.0f, sy = flags & 1024 ? -1.0f : 1.0f;
        m0 = sx * m0; m1 = sx * m1; m2 = sx * m2;
        m4 = sy * m4; m5 = sy * m5; m6 = sy * m6;
        m8 = 1.0f * m8; m9 = 1.0f * m9; m10 = 1.0f * m10;
    }
    for (; count; count--, src += src_stride) {
        float x, y, z;
        memcpy(&x, src, 4); memcpy(&y, src + 4, 4); memcpy(&z, src + 8, 4);
        float px = m4 * y; px = px + m0 * x; px = px + m8 * z; px = m12 + px;
        float py = y * m5; py = py + m1 * x; py = py + z * m9; py = m13 + py;
        float pz = m6 * y; pz = pz + x * m2; pz = pz + z * m10; pz = m14 + pz;
        put(&dst, px); put(&dst, py); put(&dst, pz);
        if (flags & 8) { put(&dst, n0); put(&dst, n1); put(&dst, n2); }
        if (flags & 64) { put(&dst, t0); put(&dst, t1); put(&dst, t2); put(&dst, tw); }
        if (flags & 16) { memcpy(dst, &color, 4); dst += 4; }
        if (extra) {
            switch (extra_size) {                   // (constant sizes: no call)
            case 0: break;
            case 4: memcpy(dst, extra, 4); break;
            case 8: memcpy(dst, extra, 8); break;
            case 12: memcpy(dst, extra, 12); break;
            case 16: memcpy(dst, extra, 16); break;
            case 20: memcpy(dst, extra, 20); break;
            case 24: memcpy(dst, extra, 24); break;
            default: memmove(dst, extra, (size_t)extra_size); break;
            }
            dst += extra_size;
            extra += extra_stride;
        }
        if (flags & 32) { memset(dst, 0, 8); dst += 8; }
    }
}

// the bytes one call writes
static size_t out_size(unsigned count, const unsigned char *extra, int extra_size, unsigned flags)
{
    size_t v = 12 + (flags & 8 ? 12 : 0) + (flags & 64 ? 16 : 0) + (flags & 16 ? 4 : 0) + (extra ? (size_t)extra_size : 0) +
               (flags & 32 ? 8 : 0);
    return v * count;
}

static void check_loop(unsigned char *dst, const float *m, const float *m3, int b, unsigned count,
                       const unsigned char *src, int src_stride, const unsigned char *extra, int extra_stride,
                       int extra_size, unsigned flags, unsigned color)
{
    size_t n = out_size(count, extra, extra_size, flags);
    unsigned char *mine = n ? malloc(n) : NULL;
    if (mine) fast_loop(mine, m, m3, b, count, src, src_stride, extra, extra_stride, extra_size, flags, color);
    o_loop(dst, m, m3, b, count, src, src_stride, extra, extra_stride, extra_size, flags, color);
    g_calls++; g_verts += count;
    if (mine && memcmp(mine, dst, n)) {
        if (g_differ++ < 5) {
            size_t i = 0;
            while (i < n && mine[i] == dst[i]) i++;
            fprintf(stderr, "[unityverts] check: a call differs (%u vertices, flags 0x%x, extra %d bytes; first at byte %zu of %zu)\n",
                    count, flags, extra ? extra_size : -1, i, n);
        }
    }
    free(mine);
    if (g_calls % 600 == 0)
        fprintf(stderr, "[unityverts] check: %lu calls, %lu vertices, %lu differ\n", g_calls, g_verts, g_differ);
}

void unityverts_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_FAST_VERTS");
    if (!v || (strcmp(v, "1") && strcmp(v, "check"))) return;
    g_check = !strcmp(v, "check");
    for (size_t i = 0; i < sizeof g_rows / sizeof *g_rows; i++) {
        const struct uh_row *r = &g_rows[i];
        if (!unityhooks_matches(r, base, size)) continue;
        if (unityhooks_hook(base + r->off, r->isa, (void *)(g_check ? check_loop : fast_loop), (void **)&o_loop) != 0) {
            fprintf(stderr, "[unityverts] %s: hook failed; Unity's own is used\n", r->func);
            return;
        }
        fprintf(stderr, "[unityverts] %s replaced%s (UA_FAST_VERTS; libunity %s)\n", r->func,
                g_check ? ", checked against Unity's own on every call" : "", r->unity);
        return;
    }
    fprintf(stderr, "[unityverts] UA_FAST_VERTS: this libunity.so is not one whose vertex transform was read; Unity's own is used\n");
}
