// unitymath.c: Unity's 4x4 matrix inverses with their last results kept (UA_INV_MEMO=1; off by default).
//
// InvertMatrix4x4_Full(const float *m, float *out) -> bool inverts a matrix in general form (6 KB of ARM
// code: no calls, no globals; its constants in its own literal pool; it reads the 16 floats at m and
// writes out). Unity asks it for the same matrices several times a frame: each camera's view and
// projection from Camera::ExtractCameraRenderingParams, CalculateFrustumPlanes and the culling
// parameters, image-effect blits, ScreenPointToRay (Hollow Knight's Greenpath on the RG34XX-SP: about
// 1.2 % of UnityMain). Each thread keeps the last four inputs it inverted with their results; an input
// whose 64 bytes equal one of them gets that result (copied to out, the same bool) without the
// computation. Only results the function wrote out for (it returned true) are kept: a singular matrix
// is computed each time. The input is copied before the call, so a caller that inverts in place (m ==
// out) is kept by its input. The same bits as Unity's own in every case: the function's output depends
// only on its input.
// Unity 2017.4 has another implementation of the same contract (2.3 KB: the input copied to the stack and
// eliminated there with row swaps; on a singular matrix it zeroes out and returns false) and a second one with
// the same contract for affine matrices, InvertMatrix4x4_General3D (832 bytes, no calls, its constants in its own
// pool), which Camera::GetCameraToWorldMatrix and friends call for every WorldToScreenPoint / ViewportToWorldPoint a
// script makes: each function keeps its own results.
//   UA_INV_MEMO=1      on
//   UA_INV_MEMO_STATS=<s>  calls and hits every <s> seconds
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "unitymath.h"
#include "unityhooks.h"

// rows by function: 0 InvertMatrix4x4_Full, 1 InvertMatrix4x4_General3D
static const struct { int fn; struct uh_row r; } g_rows[] = {
    { 0, { "6000.0.61f1 armeabi-v7a", "InvertMatrix4x4_Full", 0xac6f00, 0x17e4, 'a', 0xd0837ff6914fd53cull } },
    { 0, { "2017.4.27f1 armeabi-v7a", "InvertMatrix4x4_Full", 0x6c1400, 0x910, 'a', 0xa7f9474f32d24a43ull } },
    { 1, { "2017.4.27f1 armeabi-v7a", "InvertMatrix4x4_General3D", 0x6c3590, 0x340, 'a', 0x53578f0ea9bcb6b4ull } },
};
#define NFN 2

typedef int (*inv_fn)(const float *m, float *out);
static inv_fn o_inv[NFN];
#define NMEMO 4
typedef struct { float in[16], out[16]; int r, used; } memo_t;
static __thread memo_t t_memo[NFN][NMEMO];
static __thread unsigned t_next[NFN];
static atomic_ulong g_calls[NFN], g_hits[NFN];
static double g_stats_every, g_stats_next;
static const char *const g_names[NFN] = { "InvertMatrix4x4_Full", "InvertMatrix4x4_General3D" };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int inv_memo_fn(int f, const float *m, float *out)
{
    atomic_fetch_add_explicit(&g_calls[f], 1, memory_order_relaxed);
    memo_t *t = t_memo[f];
    for (int i = 0; i < NMEMO; i++)
        if (t[i].used && !memcmp(t[i].in, m, sizeof t[i].in)) {
            memcpy(out, t[i].out, sizeof t[i].out);
            atomic_fetch_add_explicit(&g_hits[f], 1, memory_order_relaxed);
            return t[i].r;
        }
    float in[16];
    memcpy(in, m, sizeof in);
    int r = o_inv[f](m, out);
    if (r) {
        memo_t *e = &t[t_next[f]++ % NMEMO];
        memcpy(e->in, in, sizeof e->in); memcpy(e->out, out, sizeof e->out);
        e->r = r; e->used = 1;
    }
    if (g_stats_every > 0) {
        double now_ = now();
        if (now_ >= g_stats_next) {
            g_stats_next = now_ + g_stats_every;
            for (int k = 0; k < NFN; k++) {
                if (!o_inv[k]) continue;
                unsigned long c = atomic_exchange(&g_calls[k], 0), h = atomic_exchange(&g_hits[k], 0);
                fprintf(stderr, "[unitymath] %s: %lu calls, %lu from the last results (%.0f %%)\n", g_names[k], c, h, c ? 100.0 * h / c : 0.0);
            }
        }
    }
    return r;
}
static int inv_memo(const float *m, float *out) { return inv_memo_fn(0, m, out); }
static int inv_memo_g3d(const float *m, float *out) { return inv_memo_fn(1, m, out); }

void unitymath_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_INV_MEMO");
    if (!v || strcmp(v, "1")) return;
    const char *s = getenv("UA_INV_MEMO_STATS");
    if (s && atof(s) > 0) g_stats_every = atof(s);
    int any = 0;
    for (size_t i = 0; i < sizeof g_rows / sizeof *g_rows; i++) {
        const struct uh_row *r = &g_rows[i].r;
        int f = g_rows[i].fn;
        if (o_inv[f] || !unityhooks_matches(r, base, size)) continue;
        if (unityhooks_hook(base + r->off, r->isa, (void *)(f ? inv_memo_g3d : inv_memo), (void **)&o_inv[f]) != 0) {
            fprintf(stderr, "[unitymath] %s: hook failed; Unity's own is used\n", r->func);
            o_inv[f] = NULL;
            continue;
        }
        fprintf(stderr, "[unitymath] %s: its last results kept (UA_INV_MEMO; libunity %s)\n", r->func, r->unity);
        any = 1;
    }
    if (!any) fprintf(stderr, "[unitymath] UA_INV_MEMO: this libunity.so is not one whose inverse was read; Unity's own is used\n");
}
