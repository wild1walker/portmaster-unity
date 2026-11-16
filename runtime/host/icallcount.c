// icallcount.c: how often the game's scripts call into the engine (UA_ICALL_COUNT=<s>; a measurement, off by default).
//
// libunity registers each of its internal calls (the engine functions behind UnityEngine's extern methods:
// Transform.position, GetComponent, Input.GetAxisRaw, ...) with Mono's mono_add_internal_call, by name. With
// UA_ICALL_COUNT set, each registered function is registered as a small thunk instead (ARM code: count, then jump
// to the function with every register and the stack as the caller left them), and every <s> seconds UnityMain logs
// the calls a frame of the most called ones (`[icalls] ...`, all threads together; the counts are not atomic, a
// measurement). What the calls do is unchanged; each costs a few more instructions. Mono's own internal calls (its
// class library's) are not registered this way and are not counted.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "icallcount.h"

typedef void (*add_icall_fn)(const char *name, const void *fn);
static add_icall_fn r_add;
static double g_every, g_next;
static unsigned g_frames;

#define NMAX 8192
static const char *g_names[NMAX];
static uint32_t g_counts[NMAX];
static unsigned g_n;
#if defined(__arm__)
static uint32_t *g_code;          // the thunks, 9 words each
static size_t g_code_words;

static const void *thunk(unsigned i, const void *fn)
{
    if (!g_code) {
        g_code_words = NMAX * 9;
        void *p = mmap(NULL, g_code_words * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return NULL;
        g_code = p;
    }
    uint32_t *t = g_code + 9 * i;
    t[0] = 0xe92d0003u;                 // push {r0, r1}
    t[1] = 0xe59f0010u;                 // ldr r0, [pc, #16]   (t[7]: the counter)
    t[2] = 0xe5901000u;                 // ldr r1, [r0]
    t[3] = 0xe2811001u;                 // add r1, r1, #1
    t[4] = 0xe5801000u;                 // str r1, [r0]
    t[5] = 0xe8bd0003u;                 // pop {r0, r1}
    t[6] = 0xe59ff000u;                 // ldr pc, [pc, #0]    (t[8]: the function; a Thumb one by its bit 0)
    t[7] = (uint32_t)(uintptr_t)&g_counts[i];
    t[8] = (uint32_t)(uintptr_t)fn;
    __builtin___clear_cache((char *)t, (char *)(t + 9));
    return t;
}
#else
static const void *thunk(unsigned i, const void *fn) { (void)i; (void)fn; return NULL; }
#endif

static void w_add(const char *name, const void *fn)
{
    const void *t = NULL;
    if (fn && name && g_n < NMAX && (t = thunk(g_n, fn))) {
        g_names[g_n] = name;
        g_n++;
    }
    r_add(name, t ? t : fn);
}

void *icallcount_dlsym_hook(const char *symbol, void *found)
{
    if (!found || strcmp(symbol, "mono_add_internal_call") || found == (void *)w_add) return found;
    const char *v = getenv("UA_ICALL_COUNT");
    if (!v || atof(v) <= 0) return found;
#if defined(__arm__)
    g_every = atof(v);
    r_add = (add_icall_fn)found;
    fprintf(stderr, "[icalls] the engine's internal calls counted (UA_ICALL_COUNT=%s)\n", v);
    return (void *)w_add;
#else
    return found;
#endif
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int by_count(const void *a, const void *b)
{
    uint32_t x = g_counts[*(const unsigned *)a], y = g_counts[*(const unsigned *)b];
    return (x < y) - (x > y);
}

void icallcount_frame(void)
{
    if (!r_add) return;
    g_frames++;
    double t = now();
    if (!g_next) { g_next = t + g_every; memset(g_counts, 0, sizeof g_counts); g_frames = 0; return; }
    if (t < g_next) return;
    g_next = t + g_every;
    static unsigned idx[NMAX];
    unsigned n = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < g_n; i++) if (g_counts[i]) { idx[n++] = i; total += g_counts[i]; }
    qsort(idx, n, sizeof *idx, by_count);
    double f = g_frames ? g_frames : 1;
    fprintf(stderr, "[icalls] %u frames: %.0f calls a frame to %u of the %u internal calls; the most called:\n", g_frames, total / f, n, g_n);
    for (unsigned i = 0; i < n && i < 40; i++)
        fprintf(stderr, "[icalls]   %7.1f %s\n", g_counts[idx[i]] / f, g_names[idx[i]]);
    memset(g_counts, 0, sizeof g_counts);
    g_frames = 0;
}
