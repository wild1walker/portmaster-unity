/* guided_astc for pmu (setup-rs/crates/texcodec): convert/native/guided_astc/guided_astc.c, the
 * textures step's guided ASTC encoder (this repository, MIT), without its Python module. Its block
 * code (the quantization tables through work()) is copied unchanged; ga_encode() at the end does
 * the module's encode() on plain buffers. Keep the two in step: the blocks must stay the same bytes.
 *
 * From the original: each block of an RGBA8 image is looked at first: one colour (a void-extent
 * block), 4x4 blocks close to one line (a fitted single-partition block, kept within the caller's
 * error bound), unscaled DXT1/DXT5 sources (the source block transcoded, one more candidate);
 * every other block is left to astc-encoder, in one of three lists by how far its texels are from
 * one line. Errors are measured against the decode_unorm8 behaviour (USE_DECODE_UNORM8).
 */
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ------------------------------------------------------------ quantization */

/* ASTC's quantization ranges: levels, trits, quints, bits (the ISE's order, smallest first) */
static const int QL[21][4] = {
    {2, 0, 0, 1}, {3, 1, 0, 0}, {4, 0, 0, 2}, {5, 0, 1, 0}, {6, 1, 0, 1}, {8, 0, 0, 3},
    {10, 0, 1, 1}, {12, 1, 0, 2}, {16, 0, 0, 4}, {20, 0, 1, 2}, {24, 1, 0, 3}, {32, 0, 0, 5},
    {40, 0, 1, 3}, {48, 1, 0, 4}, {64, 0, 0, 6}, {80, 0, 1, 4}, {96, 1, 0, 5}, {128, 0, 0, 7},
    {160, 0, 1, 5}, {192, 1, 0, 6}, {256, 0, 0, 8}};

static uint8_t CU[21][256];         /* colour value index -> 0..255 */
static uint8_t CQ[21][256];         /* 0..255 -> nearest colour value index */
static uint8_t WU[12][32];          /* weight index -> 0..64 */
static uint8_t WS[12][32];          /* weight indices sorted by value */
static uint8_t WK[12][65];          /* 0..64 -> the position in WS of the last level <= it */
static uint8_t TRIT_ENC[243], QUINT_ENC[125];
static int ready;

static int ise_bits(int q, int n)
{
    int b = QL[q][3] * n;
    if (QL[q][1]) b += (8 * n + 4) / 5;
    if (QL[q][2]) b += (7 * n + 2) / 3;
    return b;
}

static int replicate(int v, int from, int to)
{
    int out = 0, have = 0;
    while (have < to) {
        int take = from < to - have ? from : to - have;
        out = (out << take) | (v >> (from - take));
        have += take;
    }
    return out;
}

static int bit(int v, int i) { return (v >> i) & 1; }

static int colour_unquant(int q, int v)
{
    int t = QL[q][1], qu = QL[q][2], b = QL[q][3];
    if (!t && !qu) return replicate(v, b, 8);
    int m = v & ((1 << b) - 1), D = v >> b, A = (m & 1) ? 0x1FF : 0, B = 0, C = 0;
    int bb = bit(m, 1), c = bit(m, 2), d = bit(m, 3), e = bit(m, 4), f = bit(m, 5);
    if (t) {
        switch (b) {
        case 1: C = 204; break;
        case 2: B = bb * 0x116; C = 93; break;
        case 3: B = c * 0x10A + bb * 0x85; C = 44; break;
        case 4: B = d * 0x104 + c * 0x82 + bb * 0x41; C = 22; break;
        case 5: B = e * 0x102 + d * 0x81 + c * 0x40 + bb * 0x20; C = 11; break;
        case 6: B = f * 0x101 + e * 0x80 + d * 0x40 + c * 0x20 + bb * 0x10; C = 5; break;
        }
    } else {
        switch (b) {
        case 1: C = 113; break;
        case 2: B = bb * 0x10C; C = 54; break;
        case 3: B = c * 0x105 + bb * 0x82; C = 26; break;
        case 4: B = d * 0x102 + c * 0x81 + bb * 0x40; C = 13; break;
        case 5: B = e * 0x101 + d * 0x80 + c * 0x40 + bb * 0x20; C = 6; break;
        }
    }
    int T = D * C + B;
    T ^= A;
    return (A & 0x80) | (T >> 2);
}

static int weight_unquant(int q, int v)
{
    int t = QL[q][1], qu = QL[q][2], b = QL[q][3], r;
    if (!t && !qu) {
        r = replicate(v, b, 6);
    } else if (b == 0) {
        static const int T3[3] = {0, 32, 63}, Q5[5] = {0, 16, 32, 47, 63};
        r = t ? T3[v] : Q5[v];
    } else {
        int m = v & ((1 << b) - 1), D = v >> b, A = (m & 1) ? 0x7F : 0, B = 0, C = 0;
        int bb = bit(m, 1), c = bit(m, 2);
        if (t) {
            if (b == 1) C = 50;
            else if (b == 2) { B = bb * 0x45; C = 23; }
            else { B = c * 0x42 + bb * 0x21; C = 11; }
        } else {
            if (b == 1) C = 28;
            else { B = bb * 0x42; C = 13; }
        }
        int T = D * C + B;
        T ^= A;
        r = (A & 0x20) | (T >> 2);
    }
    return r > 32 ? r + 1 : r;
}

static void trit_decode(int T, int *t)
{
    int C;
    if (((T >> 2) & 7) == 7) {
        C = (((T >> 5) & 7) << 2) | (T & 3);
        t[4] = 2; t[3] = 2;
    } else {
        C = T & 0x1F;
        if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = bit(T, 7); }
        else { t[4] = bit(T, 7); t[3] = (T >> 5) & 3; }
    }
    if ((C & 3) == 3) { t[2] = 2; t[1] = bit(C, 4); t[0] = (bit(C, 3) << 1) | (bit(C, 2) & !bit(C, 3)); }
    else if (((C >> 2) & 3) == 3) { t[2] = 2; t[1] = 2; t[0] = C & 3; }
    else { t[2] = bit(C, 4); t[1] = (C >> 2) & 3; t[0] = (bit(C, 1) << 1) | (bit(C, 0) & !bit(C, 1)); }
}

static void quint_decode(int Q, int *q)
{
    if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
        q[2] = (bit(Q, 0) << 2) | ((bit(Q, 4) & !bit(Q, 0)) << 1) | (bit(Q, 3) & !bit(Q, 0));
        q[1] = 4; q[0] = 4;
    } else {
        int C;
        if (((Q >> 1) & 3) == 3) { q[2] = 4; C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1); }
        else { q[2] = (Q >> 5) & 3; C = Q & 0x1F; }
        if ((C & 7) == 5) { q[1] = 4; q[0] = (C >> 3) & 3; }
        else { q[1] = (C >> 3) & 3; q[0] = C & 7; }
    }
}

static void init_tables(void)
{
    if (ready) return;
    for (int q = 0; q < 21; q++) {
        int n = QL[q][0];
        for (int v = 0; v < n; v++) CU[q][v] = (uint8_t)colour_unquant(q, v);
        for (int x = 0; x < 256; x++) {
            int best = 0, bd = 1 << 30;
            for (int v = 0; v < n; v++) {
                int d = abs(CU[q][v] - x);
                if (d < bd) { bd = d; best = v; }
            }
            CQ[q][x] = (uint8_t)best;
        }
    }
    for (int q = 0; q < 12; q++) {
        int n = QL[q][0];
        for (int v = 0; v < n; v++) { WU[q][v] = (uint8_t)weight_unquant(q, v); WS[q][v] = (uint8_t)v; }
        for (int i = 1; i < n; i++)                  /* insertion sort by value */
            for (int j = i; j > 0 && WU[q][WS[q][j]] < WU[q][WS[q][j - 1]]; j--) {
                uint8_t s = WS[q][j]; WS[q][j] = WS[q][j - 1]; WS[q][j - 1] = s;
            }
        for (int t = 0, k = 0; t <= 64; t++) {
            while (k + 1 < n && WU[q][WS[q][k + 1]] <= t) k++;
            WK[q][t] = (uint8_t)k;
        }
    }
    memset(TRIT_ENC, 0xFF, sizeof TRIT_ENC);
    memset(QUINT_ENC, 0xFF, sizeof QUINT_ENC);
    for (int T = 0; T < 256; T++) {
        int t[5];
        trit_decode(T, t);
        int k = t[0] + 3 * (t[1] + 3 * (t[2] + 3 * (t[3] + 3 * t[4])));
        if (TRIT_ENC[k] == 0xFF) TRIT_ENC[k] = (uint8_t)T;
    }
    for (int Q = 0; Q < 128; Q++) {
        int q[3];
        quint_decode(Q, q);
        int k = q[0] + 5 * (q[1] + 5 * q[2]);
        if (QUINT_ENC[k] == 0xFF) QUINT_ENC[k] = (uint8_t)Q;
    }
    ready = 1;
}

/* ------------------------------------------------------------ bits */

typedef struct { uint8_t *p; int pos, end; } bitw;

static void put(bitw *w, int v, int n)
{
    for (int i = 0; i < n; i++, w->pos++)
        if (w->pos < w->end && ((v >> i) & 1)) w->p[w->pos >> 3] |= (uint8_t)(1 << (w->pos & 7));
}

/* n values of range q, ISE-coded into buf from bit `start`, at most `nbits` bits */
static void ise_write(uint8_t *buf, int start, int q, int n, const uint8_t *vals)
{
    bitw w = {buf, start, start + ise_bits(q, n)};
    int b = QL[q][3];
    if (QL[q][1]) {
        for (int i = 0; i < n; i += 5) {
            int m[5] = {0}, t[5] = {0};
            for (int j = 0; j < 5 && i + j < n; j++) { m[j] = vals[i + j] & ((1 << b) - 1); t[j] = vals[i + j] >> b; }
            int T = TRIT_ENC[t[0] + 3 * (t[1] + 3 * (t[2] + 3 * (t[3] + 3 * t[4])))];
            put(&w, m[0], b); put(&w, T, 2);
            put(&w, m[1], b); put(&w, T >> 2, 2);
            put(&w, m[2], b); put(&w, T >> 4, 1);
            put(&w, m[3], b); put(&w, T >> 5, 2);
            put(&w, m[4], b); put(&w, T >> 7, 1);
        }
    } else if (QL[q][2]) {
        for (int i = 0; i < n; i += 3) {
            int m[3] = {0}, t[3] = {0};
            for (int j = 0; j < 3 && i + j < n; j++) { m[j] = vals[i + j] & ((1 << b) - 1); t[j] = vals[i + j] >> b; }
            int Q = QUINT_ENC[t[0] + 5 * (t[1] + 5 * t[2])];
            put(&w, m[0], b); put(&w, Q, 3);
            put(&w, m[1], b); put(&w, Q >> 3, 2);
            put(&w, m[2], b); put(&w, Q >> 5, 2);
        }
    } else {
        for (int i = 0; i < n; i++) put(&w, vals[i], b);
    }
}

static int reverse8(int v)
{
    v = ((v & 0xF0) >> 4) | ((v & 0x0F) << 4);
    v = ((v & 0xCC) >> 2) | ((v & 0x33) << 2);
    return ((v & 0xAA) >> 1) | ((v & 0x55) << 1);
}

/* ------------------------------------------------------------ blocks */

/* the void-extent block astc-encoder writes for a one-colour LDR block (UNORM16 = c * 257) */
static void void_extent(uint8_t *out, const uint8_t *c)
{
    static const uint8_t head[8] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(out, head, 8);
    for (int i = 0; i < 4; i++) out[8 + 2 * i] = out[9 + 2 * i] = c[i];
}

/* A single-partition 4x4 block with a 4x4 weight grid, one plane. cem 8 (RGB) or 12 (RGBA);
 * ev: the endpoint value indices in ISE order (r0 r1 g0 g1 b0 b1 [a0 a1]), cq their range;
 * wv: 16 weight indices of range wq. */
static void pack_block(uint8_t *out, int cem, int cq, const uint8_t *ev, int wq, const uint8_t *wv)
{
    uint8_t wb[16] = {0};
    memset(out, 0, 16);
    int H = wq >= 6, R = wq % 6 + 2;
    int mode = (H << 9) | (2 << 5) | ((R & 1) << 4) | (((R >> 2) & 1) << 1) | ((R >> 1) & 1);
    bitw w = {out, 0, 128};
    put(&w, mode, 11);
    put(&w, 0, 2);                                   /* one partition */
    put(&w, cem, 4);
    ise_write(out, 17, cq, cem == 12 ? 8 : 6, ev);
    ise_write(wb, 0, wq, 16, wv);                    /* weights: bit-reversed from the top */
    for (int i = 0; i < 16; i++) out[15 - i] |= (uint8_t)reverse8(wb[i]);
}

/* the colour range a single-plane 4x4 grid with weight range wq leaves for nv endpoint values */
static int colour_range(int wq, int nv)
{
    int avail = 128 - 17 - ise_bits(wq, 16);
    for (int q = 20; q >= 0; q--)
        if (ise_bits(q, nv) <= avail) return q;
    return -1;
}

static int decode_ch(int e0, int e1, int w)
{
    return ((e0 * 257 * (64 - w) + e1 * 257 * w + 32) >> 6) >> 8;
}

typedef struct {
    int err;
    int cem, cq, wq;
    uint8_t ev[8], wv[16];
} cand;

/* the best weight for every texel given decoded endpoints e0, e1 (nc channels); returns the error,
 * or as soon as it is over limit, something over limit (wv then incomplete) */
static int choose_weights(const uint8_t *px, int nc, const int *e0, const int *e1, int wq, uint8_t *wv, int limit)
{
    int n = QL[wq][0], dd = 0, err = 0;
    int d[4];
    int dec[32][4];                                  /* the decoded colour of each level, in value order */
    for (int c = 0; c < nc; c++) { d[c] = e1[c] - e0[c]; dd += d[c] * d[c]; }
    for (int j = 0; j < n; j++)
        for (int c = 0; c < nc; c++) dec[j][c] = decode_ch(e0[c], e1[c], WU[wq][WS[wq][j]]);
    float inv = dd ? 64.0f / dd : 0;
    for (int i = 0; i < 16; i++) {
        const uint8_t *p = px + 4 * i;
        int dot = 0;
        for (int c = 0; c < nc; c++) dot += (p[c] - e0[c]) * d[c];
        int t64 = (int)(dot * inv + 0.5f);
        t64 = t64 < 0 ? 0 : t64 > 64 ? 64 : t64;
        /* the two levels around t64, each checked against the exact decode */
        int k = WK[wq][t64], best = k, be = 0;
        for (int c = 0; c < nc; c++) { int v = dec[k][c] - p[c]; be += v * v; }
        if (k + 1 < n && be) {
            int e = 0;
            for (int c = 0; c < nc; c++) { int v = dec[k + 1][c] - p[c]; e += v * v; }
            if (e < be) { be = e; best = k + 1; }
        }
        wv[i] = WS[wq][best];
        err += be;
        if (err > limit) return err;
    }
    return err;
}

/* quantized endpoints from float ones; swapped when the encoding needs e0's sum <= e1's (as
 * direct LDR endpoints without blue contraction must). Fills ev (ISE order) and e0/e1 (decoded). */
static void quantize_endpoints(const float *f0, const float *f1, int nc, int cq, uint8_t *ev, int *e0, int *e1)
{
    int i0[4], i1[4];
    for (int c = 0; c < nc; c++) {
        int a = (int)(f0[c] + 0.5f), b = (int)(f1[c] + 0.5f);
        a = a < 0 ? 0 : a > 255 ? 255 : a;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
        i0[c] = CQ[cq][a]; i1[c] = CQ[cq][b];
    }
    int s0 = CU[cq][i0[0]] + CU[cq][i0[1]] + CU[cq][i0[2]];
    int s1 = CU[cq][i1[0]] + CU[cq][i1[1]] + CU[cq][i1[2]];
    if (s1 < s0)
        for (int c = 0; c < nc; c++) { int s = i0[c]; i0[c] = i1[c]; i1[c] = s; }
    for (int c = 0; c < nc; c++) {
        ev[2 * c] = (uint8_t)i0[c]; ev[2 * c + 1] = (uint8_t)i1[c];
        e0[c] = CU[cq][i0[c]]; e1[c] = CU[cq][i1[c]];
    }
}

/* one candidate encoding: endpoints f0/f1 (float), refined by least squares `iters` times */
static void fit_candidate(const uint8_t *px, int nc, int wq, const float *f0in, const float *f1in, int iters, cand *best)
{
    int cem = nc == 4 ? 12 : 8, cq = colour_range(wq, 2 * nc);
    if (cq < 0) return;
    float f0[4], f1[4];
    memcpy(f0, f0in, sizeof f0);
    memcpy(f1, f1in, sizeof f1);
    for (int it = 0; it <= iters; it++) {
        cand c;
        int e0[4], e1[4];
        c.cem = cem; c.cq = cq; c.wq = wq;
        quantize_endpoints(f0, f1, nc, cq, c.ev, e0, e1);
        c.err = choose_weights(px, nc, e0, e1, wq, c.wv, it == iters ? best->err : 1 << 30);
        if (c.err < best->err) *best = c;
        if (it == iters || c.err == 0) break;
        /* least squares endpoints for these weights */
        double a = 0, b = 0, d = 0, r0[4] = {0}, r1[4] = {0};
        for (int i = 0; i < 16; i++) {
            double t = WU[wq][c.wv[i]] / 64.0, s = 1 - t;
            a += s * s; b += s * t; d += t * t;
            for (int k = 0; k < nc; k++) { r0[k] += s * px[4 * i + k]; r1[k] += t * px[4 * i + k]; }
        }
        double det = a * d - b * b;
        if (det < 1e-9) break;
        for (int k = 0; k < nc; k++) {
            /* weights for e0 and e1 follow the swap made in quantize_endpoints */
            float x0 = (float)((d * r0[k] - b * r1[k]) / det), x1 = (float)((a * r1[k] - b * r0[k]) / det);
            f0[k] = x0; f1[k] = x1;
        }
    }
}

/* The best single-partition block for 16 texels: error (sum of squared 8-bit differences over the
 * four channels) into best, unless the texels are too far from one line for any such block to
 * come within max_err. Returns the line residual (the squared distances of the texels from their
 * principal axis), which no single-plane, single-partition block can beat by more than its
 * rounding. */
static float fit_block(const uint8_t *px, int opaque, int max_err, cand *best)
{
    int nc = opaque ? 3 : 4;
    float mean[4] = {0}, M[4][4] = {{0}};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < nc; c++) mean[c] += px[4 * i + c];
    for (int c = 0; c < nc; c++) mean[c] *= 1.0f / 16;
    for (int i = 0; i < 16; i++) {
        float d[4];
        for (int c = 0; c < nc; c++) d[c] = px[4 * i + c] - mean[c];
        for (int r = 0; r < nc; r++)
            for (int c = r; c < nc; c++) M[r][c] += d[r] * d[c];
    }
    float trace = 0;
    for (int r = 0; r < nc; r++) {
        trace += M[r][r];
        for (int c = 0; c < r; c++) M[r][c] = M[c][r];
    }
    /* principal axis: power iteration from the channel with the largest variance */
    float ax[4] = {0};
    int top = 0;
    for (int c = 1; c < nc; c++) if (M[c][c] > M[top][top]) top = c;
    for (int c = 0; c < nc; c++) ax[c] = M[top][c];
    float lambda = 0;
    for (int it = 0; it < 8; it++) {
        float nx[4] = {0}, len = 0;
        for (int r = 0; r < nc; r++) {
            for (int c = 0; c < nc; c++) nx[r] += M[r][c] * ax[c];
            len += nx[r] * nx[r];
        }
        if (len < 1e-12f) break;
        float inv = 1.0f / sqrtf(len);
        for (int r = 0; r < nc; r++) ax[r] = nx[r] * inv;
    }
    {
        float len = 0;
        for (int c = 0; c < nc; c++) len += ax[c] * ax[c];
        if (len < 1e-12f) { ax[0] = 1; for (int c = 1; c < nc; c++) ax[c] = 0; }
        else { float inv = 1.0f / sqrtf(len); for (int c = 0; c < nc; c++) ax[c] *= inv; }
        for (int r = 0; r < nc; r++)
            for (int c = 0; c < nc; c++) lambda += ax[r] * M[r][c] * ax[c];
    }
    float resid = trace - lambda;
    resid = resid < 0 ? 0 : resid;
    /* decoded texels are a line's points rounded to integers: at most 0.5 from it per channel */
    float slack = sqrtf(resid) - sqrtf(16 * nc * 0.25f);
    if (max_err < 0 || (slack > 0 && slack * slack > max_err)) return resid;
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < 16; i++) {
        float t = 0;
        for (int c = 0; c < nc; c++) t += (px[4 * i + c] - mean[c]) * ax[c];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    float f0[4], f1[4];
    for (int c = 0; c < nc; c++) { f0[c] = mean[c] + tmin * ax[c]; f1[c] = mean[c] + tmax * ax[c]; }
    /* one encoding: 12 weight levels and 8-bit RGB endpoints, or 8 levels and 192-level RGBA ones
     * (measured on Cuphead's textures: more weight ranges, iterations, or a search over the
     * quantized endpoints fitted 1-4 % more of the blocks for 2-5 times the time) */
    fit_candidate(px, nc, opaque ? 7 : 5, f0, f1, 1, best);
    if (best->err > max_err && best->err <= 4 * max_err)        /* close: 16 levels and 192 (RGB), 10 and 128 (RGBA) */
        fit_candidate(px, nc, opaque ? 8 : 6, f0, f1, 1, best);
    return resid;
}

/* ------------------------------------------------------------ DXT seeds */

static void rgb565(int v, int *c)
{
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    c[0] = (r << 3) | (r >> 2); c[1] = (g << 2) | (g >> 4); c[2] = (b << 3) | (b >> 2);
}

/* A DXT1 or DXT5 block's colour part (8 bytes) as a candidate: its two endpoints exactly (8-bit
 * values are representable in the ranges these modes leave) and its indices as weights 0, 21,
 * 43, 64 (range 4) or, for a three-colour block without its transparent index, 0, 32, 64
 * (range 3). Alpha: opaque, or one value over the block (taken from the texels). */
static void dxt_candidate(const uint8_t *src, int dxt5, const uint8_t *px, int opaque, cand *best)
{
    int c0v = src[0] | (src[1] << 8), c1v = src[2] | (src[3] << 8);
    uint32_t idx = src[4] | (src[5] << 8) | (src[6] << 16) | ((uint32_t)src[7] << 24);
    int four = dxt5 || c0v > c1v;
    int a = px[3];
    for (int i = 1; i < 16; i++) if (px[4 * i + 3] != a) return;   /* alpha varies: not this candidate */
    int c0[4], c1[4];
    rgb565(c0v, c0); rgb565(c1v, c1);
    c0[3] = c1[3] = a;
    int nc = opaque ? 3 : 4;
    int wq = four ? 2 : 1;                           /* range 4: 0 21 43 64; range 3: 0 32 64 */
    int cq = colour_range(wq, 2 * nc);
    /* DXT index -> fraction of the way from c0 to c1 (in thirds or halves) */
    static const uint8_t W4[4] = {0, 3, 1, 2}, W3[4] = {0, 2, 1, 255};
    int s0 = c0[0] + c0[1] + c0[2], s1 = c1[0] + c1[1] + c1[2];
    int swap = s1 < s0;
    cand c;
    c.cem = nc == 4 ? 12 : 8; c.cq = cq; c.wq = wq;
    int e0[4], e1[4];
    for (int k = 0; k < nc; k++) {
        int lo = swap ? c1[k] : c0[k], hi = swap ? c0[k] : c1[k];
        c.ev[2 * k] = CQ[cq][lo]; c.ev[2 * k + 1] = CQ[cq][hi];
        e0[k] = CU[cq][c.ev[2 * k]]; e1[k] = CU[cq][c.ev[2 * k + 1]];
    }
    int err = 0;
    for (int i = 0; i < 16; i++) {
        int di = (idx >> (2 * i)) & 3;
        int f = four ? W4[di] : W3[di];              /* in steps of the range */
        if (f == 255) return;                        /* transparent black: not a line */
        int steps = four ? 3 : 2;
        if (swap) f = steps - f;
        /* the weight index of value f/steps (sorted order = value order) */
        int wi = WS[wq][f];
        c.wv[i] = (uint8_t)wi;
        int wt = WU[wq][wi];
        for (int k = 0; k < nc; k++) { int v = decode_ch(e0[k], e1[k], wt) - px[4 * i + k]; err += v * v; }
        if (nc == 3) { int v = px[4 * i + 3] - 255; err += v * v; }
    }
    c.err = err;
    if (err < best->err) *best = c;
}

/* ------------------------------------------------------------ the image */

typedef struct {
    const uint8_t *img;
    int w, h, bw, bh, bx, by;                        /* image, footprint, blocks across and down */
    int max_err, line, near;                         /* fit bound (< 0: none), route bounds (< 0: none) */
    const uint8_t *dxt; int dxt5;                    /* seed blocks (4x4 only) or NULL */
    uint8_t *out;                                    /* 16 bytes per block */
    uint8_t *cls;                                    /* per block: a class below */
    int y0, y1;                                      /* block rows of this worker */
    long long err_fit;
} job;

static void load_block(const job *J, int x, int y, uint8_t *px)
{
    for (int j = 0; j < J->bh; j++) {
        int yy = y * J->bh + j;
        if (yy >= J->h) yy = J->h - 1;
        for (int i = 0; i < J->bw; i++) {
            int xx = x * J->bw + i;
            if (xx >= J->w) xx = J->w - 1;
            memcpy(px + 4 * (j * J->bw + i), J->img + 4 * ((size_t)yy * J->w + xx), 4);
        }
    }
}

enum { OTHER, ONE_COLOUR, FIT, SEED, LINE, NEAR };

static void *work(void *arg)
{
    job *J = arg;
    uint8_t px[4 * 144];
    int n = J->bw * J->bh;
    for (int y = J->y0; y < J->y1; y++)
        for (int x = 0; x < J->bx; x++) {
            size_t b = (size_t)y * J->bx + x;
            load_block(J, x, y, px);
            const uint32_t *p32 = (const uint32_t *)px;
            int same = 1, opaque = 1;
            for (int i = 1; i < n && same; i++) same = p32[i] == p32[0];
            if (same) { void_extent(J->out + 16 * b, px); J->cls[b] = 1; continue; }
            if (J->bw != 4 || J->bh != 4 || (J->max_err < 0 && J->line < 0 && J->near < 0)) { J->cls[b] = OTHER; continue; }
            for (int i = 0; i < 16; i++) opaque &= px[4 * i + 3] == 255;
            cand best;
            best.err = 1 << 30;
            int seeded = 0;
            if (J->dxt && J->max_err >= 0) {
                dxt_candidate(J->dxt + (J->dxt5 ? 16 : 8) * b + (J->dxt5 ? 8 : 0), J->dxt5, px, opaque, &best);
                seeded = best.err < (1 << 30);
            }
            int seed_err = best.err;
            float resid = 0;
            if (best.err > 0) resid = fit_block(px, opaque, J->max_err, &best);
            if (best.err <= J->max_err) {
                pack_block(J->out + 16 * b, best.cem, best.cq, best.ev, best.wq, best.wv);
                J->cls[b] = (seeded && best.err == seed_err) ? SEED : FIT;
                J->err_fit += best.err;
            } else {
                J->cls[b] = resid <= J->line ? LINE : resid <= J->near ? NEAR : OTHER;
            }
        }
    return NULL;
}

/* ga_init(): the tables, once, before encoding on several threads (encode() set them up under
 * Python's lock) */
void ga_init(void)
{
    init_tables();
}

/* ga_encode(): the module's encode() on plain buffers. rgba: w*h*4 bytes; out: 16 bytes per block
 * (zeroed here), cls: one class per block (enum above); dxt: NULL or the seed blocks. Returns the
 * fitted blocks' summed error. The caller turns cls into the counts and the line / near / other
 * lists exactly as encode() does. */
long long ga_encode(const uint8_t *rgba, int w, int h, int bw, int bh, int max_err, int threads, int line,
                    int near, const uint8_t *dxt, int dxt5, uint8_t *out, uint8_t *cls)
{
    init_tables();
    int bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    size_t nb = (size_t)bx * by;
    memset(out, 0, nb * 16);
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    if (threads > by) threads = by;
    job jobs[64];
    pthread_t tid[64];
    for (int t = 0; t < threads; t++) {
        job *J = &jobs[t];
        J->img = rgba; J->w = w; J->h = h; J->bw = bw; J->bh = bh; J->bx = bx; J->by = by;
        J->max_err = max_err; J->line = line; J->near = near; J->dxt = dxt; J->dxt5 = dxt5;
        J->out = out; J->cls = cls; J->err_fit = 0;
        J->y0 = (int)((long long)by * t / threads); J->y1 = (int)((long long)by * (t + 1) / threads);
    }
    int started = 1;
    for (int t = 1; t < threads; t++)
        if (pthread_create(&tid[t], NULL, work, &jobs[t]) == 0) started++;
        else { work(&jobs[t]); }
    work(&jobs[0]);
    for (int t = 1; t < started; t++) pthread_join(tid[t], NULL);
    long long err_fit = 0;
    for (int t = 0; t < threads; t++) err_fit += jobs[t].err_fit;
    return err_fit;
}
