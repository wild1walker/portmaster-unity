// icallcount.c: how often the game's scripts call into the engine (UA_ICALL_COUNT=<s>; a measurement, off by default).
//
// libunity registers each of its internal calls (the engine functions behind UnityEngine's extern methods:
// Transform.position, GetComponent, Input.GetAxisRaw, ...) with Mono's mono_add_internal_call, by name. With
// UA_ICALL_COUNT set, each registered function is registered as a small thunk instead (ARM code: count, then jump
// to the function with every register and the stack as the caller left them), and every <s> seconds UnityMain logs
// the calls a frame of the most called ones (`[icalls] ...`, all threads together; the counts are not atomic, a
// measurement). What the calls do is unchanged; each costs a few more instructions. Mono's own internal calls (its
// class library's) are not registered this way and are not counted. On the 64-bit player libunity registers them with
// il2cpp_add_internal_call (the Mono route's bridge, runtime/mono64, or IL2CPP's own): counted the same way, AArch64
// thunks (x16 and x17, the intra-procedure-call registers a veneer may use, hold the counter and the target).
// UA_API_COUNT=<s>: the same for the runtime's API itself, every mono_* function the 32-bit libunity looks up (Mono's
// players) or il2cpp_* function the 64-bit one does (IL2CPP, the Mono route's bridge): `[api]` lines, the calls a
// frame of the most called ones (runtime invokes, GC handle reads, write barriers, stack walks, ...), which show what
// the engine asks of the runtime each frame on one player and the other.
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
#define AMAX 1024                 // the API's functions, counted after the internal calls (slots NMAX..)
static const char *g_names[NMAX + AMAX];
static uint32_t g_counts[NMAX + AMAX];
static unsigned g_n, g_an;
static double g_api_every;        // UA_API_COUNT
#if defined(__arm__) || defined(__aarch64__)
#if defined(__arm__)
#define TW 9                      // a thunk's words
#else
#define TW 10
#endif
static uint32_t *g_code;          // the thunks, TW words each
static size_t g_code_words;

static uint32_t *thunk_mem(unsigned i)
{
    if (!g_code) {
        g_code_words = (NMAX + AMAX) * TW;
        void *p = mmap(NULL, g_code_words * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return NULL;
        g_code = p;
    }
    return g_code + TW * i;
}
#endif
#if defined(__arm__)
static const void *thunk(unsigned i, const void *fn)
{
    uint32_t *t = thunk_mem(i);
    if (!t) return NULL;
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
#elif defined(__aarch64__)
static const void *thunk(unsigned i, const void *fn)
{
    uint32_t *t = thunk_mem(i);
    if (!t) return NULL;
    t[0] = 0x580000d0u;                 // ldr x16, #24        (the counter's address)
    t[1] = 0xb9400211u;                 // ldr w17, [x16]
    t[2] = 0x11000631u;                 // add w17, w17, #1
    t[3] = 0xb9000211u;                 // str w17, [x16]
    t[4] = 0x58000090u;                 // ldr x16, #16        (the function)
    t[5] = 0xd61f0200u;                 // br x16
    uint64_t c = (uint64_t)(uintptr_t)&g_counts[i], f = (uint64_t)(uintptr_t)fn;
    memcpy(t + 6, &c, 8);
    memcpy(t + 8, &f, 8);
    __builtin___clear_cache((char *)t, (char *)(t + 10));
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

// an address inside an executable mapping (a function, not a variable the API also exports)
static int is_code(const void *a)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int x = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) == 3 && (uintptr_t)a >= lo && (uintptr_t)a < hi) { x = perm[2] == 'x'; break; }
    }
    fclose(f);
    return x;
}

// the runtime's API function a libunity looked up, as a counting thunk (UA_API_COUNT)
static void *api(const char *symbol, void *found)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("UA_API_COUNT"); g_api_every = v ? atof(v) : 0; on = g_api_every > 0; }
    if (!on || g_an >= AMAX || !is_code(found)) return found;
    for (unsigned i = 0; i < g_an; i++)          // looked up again: the same thunk
        if (!strcmp(g_names[NMAX + i], symbol)) return g_code + TW * (NMAX + i);
    char *name = strdup(symbol);
    const void *t = name ? thunk(NMAX + g_an, found) : NULL;
    if (!t) { free(name); return found; }
    if (!g_an) fprintf(stderr, "[api] the runtime's API calls counted (UA_API_COUNT=%s)\n", getenv("UA_API_COUNT"));
    g_names[NMAX + g_an++] = name;
    return (void *)t;
}

void *icallcount_dlsym_hook(const char *symbol, void *found)
{
    if (!found || found == (void *)w_add) return found;
#if defined(__arm__) || defined(__aarch64__)
    if (strcmp(symbol, "mono_add_internal_call") && strcmp(symbol, "il2cpp_add_internal_call")) return found;
    const char *v = getenv("UA_ICALL_COUNT");
    if (!v || atof(v) <= 0) return found;
    g_every = atof(v);
    r_add = (add_icall_fn)found;
    fprintf(stderr, "[icalls] the engine's internal calls counted (UA_ICALL_COUNT=%s)\n", v);
    return (void *)w_add;
#else
    (void)symbol;
    return found;
#endif
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int by_count(const void *a, const void *b)
{
    uint32_t x = g_counts[*(const unsigned *)a], y = g_counts[*(const unsigned *)b];
    return (x < y) - (x > y);
}

// the counts of slots [from, from + count) since the last report, the most called first
static void report(const char *tag, const char *what, unsigned from, unsigned count, double f)
{
    static unsigned idx[NMAX];
    unsigned n = 0;
    uint64_t total = 0;
    for (unsigned i = from; i < from + count; i++) if (g_counts[i]) { idx[n++] = i; total += g_counts[i]; }
    qsort(idx, n, sizeof *idx, by_count);
    fprintf(stderr, "[%s] %.0f frames: %.0f calls a frame to %u of the %u %s; the most called:\n", tag, f, total / f, n, count, what);
    for (unsigned i = 0; i < n && i < 40; i++)
        fprintf(stderr, "[%s]   %7.1f %s\n", tag, g_counts[idx[i]] / f, g_names[idx[i]]);
}

void *icallcount_api_hook(const char *symbol, void *found)
{
#if defined(__arm__) || defined(__aarch64__)
    // (the internal-call registration is left out: UA_ICALL_COUNT replaces it)
    if (found && (!strncmp(symbol, "mono_", 5) || !strncmp(symbol, "il2cpp_", 7)) &&
        strcmp(symbol, "mono_add_internal_call") && strcmp(symbol, "il2cpp_add_internal_call"))
        return api(symbol, found);
#else
    (void)symbol;
#endif
    return found;
}

void icallcount_frame(void)
{
    if (!r_add && !g_an) return;
    g_frames++;
    double t = now(), every = r_add ? g_every : g_api_every;
    if (!g_next) {
        // the calls before the first frame (the engine's start: assembly and class enumeration, icall registration)
        if (g_an) report("api", "API functions looked up, before the first frame", NMAX, g_an, 1);
        g_next = t + every; memset(g_counts, 0, sizeof g_counts); g_frames = 0; return;
    }
    if (t < g_next) return;
    g_next = t + every;
    double f = g_frames ? g_frames : 1;
    if (r_add) report("icalls", "internal calls", 0, g_n, f);
    if (g_an) report("api", "API functions looked up", NMAX, g_an, f);
    memset(g_counts, 0, sizeof g_counts);
    g_frames = 0;
}
