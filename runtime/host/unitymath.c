// unitymath.c: Unity's 4x4 matrix inverse with its last results kept (UA_INV_MEMO=1; off by default).
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

static const struct uh_row g_rows[] = {
    { "6000.0.61f1 armeabi-v7a", "InvertMatrix4x4_Full", 0xac6f00, 0x17e4, 'a', 0xd0837ff6914fd53cull },
};

static int (*o_inv)(const float *m, float *out);
#define NMEMO 4
typedef struct { float in[16], out[16]; int r, used; } memo_t;
static __thread memo_t t_memo[NMEMO];
static __thread unsigned t_next;
static atomic_ulong g_calls, g_hits;
static double g_stats_every, g_stats_next;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int inv_memo(const float *m, float *out)
{
    atomic_fetch_add_explicit(&g_calls, 1, memory_order_relaxed);
    for (int i = 0; i < NMEMO; i++)
        if (t_memo[i].used && !memcmp(t_memo[i].in, m, sizeof t_memo[i].in)) {
            memcpy(out, t_memo[i].out, sizeof t_memo[i].out);
            atomic_fetch_add_explicit(&g_hits, 1, memory_order_relaxed);
            return t_memo[i].r;
        }
    float in[16];
    memcpy(in, m, sizeof in);
    int r = o_inv(m, out);
    if (r) {
        memo_t *e = &t_memo[t_next++ % NMEMO];
        memcpy(e->in, in, sizeof e->in); memcpy(e->out, out, sizeof e->out);
        e->r = r; e->used = 1;
    }
    if (g_stats_every > 0) {
        double t = now();
        if (t >= g_stats_next) {
            g_stats_next = t + g_stats_every;
            unsigned long c = atomic_exchange(&g_calls, 0), h = atomic_exchange(&g_hits, 0);
            fprintf(stderr, "[unitymath] InvertMatrix4x4_Full: %lu calls, %lu from the last results (%.0f %%)\n", c, h, c ? 100.0 * h / c : 0.0);
        }
    }
    return r;
}

void unitymath_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_INV_MEMO");
    if (!v || strcmp(v, "1")) return;
    const char *s = getenv("UA_INV_MEMO_STATS");
    if (s && atof(s) > 0) g_stats_every = atof(s);
    for (size_t i = 0; i < sizeof g_rows / sizeof *g_rows; i++) {
        const struct uh_row *r = &g_rows[i];
        if (!unityhooks_matches(r, base, size)) continue;
        if (unityhooks_hook(base + r->off, r->isa, (void *)inv_memo, (void **)&o_inv) != 0) {
            fprintf(stderr, "[unitymath] %s: hook failed; Unity's own is used\n", r->func);
            return;
        }
        fprintf(stderr, "[unitymath] %s: its last results kept (UA_INV_MEMO; libunity %s)\n", r->func, r->unity);
        return;
    }
    fprintf(stderr, "[unitymath] UA_INV_MEMO: this libunity.so is not one whose inverse was read; Unity's own is used\n");
}
