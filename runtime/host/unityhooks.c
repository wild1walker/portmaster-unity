// unityhooks.c: opt-in changes to functions inside libunity.so, found by Unity's own symbol names,
// applied in memory when the loader has relocated libunity and before any of its code runs. The
// library file is never changed.
//
//   UA_GFX_JOBS=legacy   Unity's legacy graphics jobs on OpenGL ES (kGfxThreadingModeClientWorkerJobs):
//                        the render loop's draw-call building (each camera's render queue, the
//                        shadow and depth passes, SRP's DrawRenderers) is split into jobs on the job
//                        workers, each recording into its own GfxDeviceClient command buffer; the one
//                        render thread plays the buffers back, in order, on the one GL context. The
//                        player turns any graphics-jobs mode into the plain render thread on OpenGL
//                        ES (AdjustThreadingModeForAPI: the check for GfxDeviceRenderer 11); the hook
//                        keeps ClientWorkerJobs there, and ua_host adds `-force-gfx-jobs legacy` to
//                        Unity's command line unless UA_UNITY_ARGS already names a threading switch.
//                        Other modes (native, split) need native command buffers GLES does not have
//                        and are left as Unity sets them.
//   UA_GLES_POOL=1       Unity's pool of GLES buffers in lists that cost O(1) (unitypool.c)
//   UA_FAST_VERTS=1      dynamic batching's vertex transform without its per-vertex overhead (unityverts.c)
//   UA_INV_MEMO=1        the 4x4 matrix inverse's last results kept per thread (unitymath.c)
//   UA_PRESENT_SYNC=end  Unity's GraphicsDeviceSettings.waitForPresentSyncPoint = EndFrame, set once from
//                        UnityMain after its first frame (=begin: BeginFrame; unset: as the game has it).
//                        GfxDeviceClient waits for the render thread to have presented the last frame in
//                        BeginFrame (0, the default) or in EndFrame (1): at the end the main thread runs
//                        the frame's scripts, animation and culling while the render thread still plays
//                        the last frame back. Unity's own setting (a script can set it); nothing is hooked.
//   UA_UNITY_HOOKS=0     no hook at all (the default without UA_GFX_JOBS, UA_GLES_POOL or UA_FAST_VERTS)
//
// How a function is found. Unity publishes symbol files for its Android players (`pmunity player
// fetch <version> --symbols`); tools/re/unityhooks.py reads a player's libunity.so with its
// symbol file and prints a row for each function below: its offset, size, instruction set, and an
// FNV-1a hash of all of its bytes. At load the hook is installed only where a row's hash matches the
// loaded library's bytes at that offset: the code is the very code that was read and checked (the
// function's arguments, its enum values, a prologue that can be moved). Any other build of libunity,
// or a library of another version with a function at the same offset, matches no row and is left
// alone; the log says so. No Unity code is kept here: offsets and hashes only.
//
// How it hooks. The function's first instructions (8 bytes in ARM state, 16 in A64; the tool checks
// that they use no pc-relative operand and that nothing in the function branches into them) are
// copied to a trampoline that continues at the next instruction; the entry becomes a jump to the
// wrapper here, which calls the original through the trampoline and adjusts only its result.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "unityhooks.h"

#include "unitypool.h"
#include "unityverts.h"
#include "unitymath.h"

enum { ISA_ARM = 'a', ISA_THUMB = 't', ISA_A64 = 'x' };
#define row uh_row

// AdjustThreadingModeForAPI(GfxThreadingMode, GfxDeviceRenderer, GfxCreateDeviceFlags) -> GfxThreadingMode
// Read in each: modes 0 Direct, 1 NonThreaded, 2 Threaded, 3 ClientWorkerJobs, 4 ClientWorkerNativeJobs,
// 5 DirectNativeJobs, 6 SplitJobs (GfxThreadingModeName's table); renderer 11 OpenGL ES 3.x; for 11 every
// mode 3 - 6 becomes 2, and only there (OpenGL Core, 17, keeps ClientWorkerJobs); 6000.3 and 6000.6 then ask
// CheckPlatformGraphicsJobsSupport, which changes the mode only for Vulkan (21). GfxDeviceClient::ExecuteAsync
// records ClientWorkerJobs into deferred client devices in each. Unity 2021.2 - 2022.2 players have no such
// function (the check is inlined into CreateGfxDevice, and their ExecuteAsync has no ClientWorkerJobs path to
// keep); 2017.x Android players have no client ExecuteAsync at all.
static const struct row g_adjust[] = {
    { "6000.0.58f2 arm64-v8a", "AdjustThreadingModeForAPI", 0x1064384, 0x124, ISA_A64, 0xcb638d1da6e7ceabull },
    { "6000.0.61f1 armeabi-v7a", "AdjustThreadingModeForAPI", 0xae495c, 0x148, ISA_ARM, 0xf35961184571fb1cull },
    { "6000.0.61f1 arm64-v8a", "AdjustThreadingModeForAPI", 0x10665f8, 0x134, ISA_A64, 0x355bbd317a3bb811ull },
    { "6000.3.11f1 armeabi-v7a", "AdjustThreadingModeForAPI", 0xb7bb1c, 0x1d4, ISA_ARM, 0x1c4d6f9fc49fc82cull },
    { "6000.6.2f1 armeabi-v7a", "AdjustThreadingModeForAPI", 0xe47158, 0x214, ISA_ARM, 0x72524d146331a93dull },
};
enum { MODE_THREADED = 2, MODE_CLIENT_WORKER_JOBS = 3, RENDERER_GLES3X = 11 };

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

// --- the jump and the trampoline
static int patch_bytes(uintptr_t at, const void *code, size_t n)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t a = at & ~(uintptr_t)(pg - 1), e = (at + n + pg - 1) & ~(uintptr_t)(pg - 1);
    if (mprotect((void *)a, e - a, PROT_READ | PROT_WRITE) != 0) return -1;
    memcpy((void *)at, code, n);
    mprotect((void *)a, e - a, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)(at + n));
    return 0;
}

// a trampoline: the moved instructions, then a jump to fn + moved; returns its address or 0
static uintptr_t make_trampoline(uintptr_t fn, size_t moved, char isa)
{
    static uint8_t *pool;
    static size_t used;
    long pg = sysconf(_SC_PAGESIZE);
    if (!pool) {
        pool = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pool == MAP_FAILED) { pool = NULL; return 0; }
    }
    size_t need = moved + 16;
    if (used + need > (size_t)pg) return 0;
    uint8_t *t = pool + used;
    mprotect(pool, pg, PROT_READ | PROT_WRITE);
    memcpy(t, (void *)fn, moved);
    uintptr_t back = fn + moved;
    if (isa == ISA_ARM) {
        uint32_t j[2] = { 0xe51ff004u /* ldr pc, [pc, #-4] */, (uint32_t)back };
        memcpy(t + moved, j, 8);
    } else {
        uint32_t j[4] = { 0x58000050u /* ldr x16, #8 */, 0xd61f0200u /* br x16 */, 0, 0 };
        uint64_t b = back;
        memcpy(&j[2], &b, 8);
        memcpy(t + moved, j, 16);
    }
    mprotect(pool, pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)t, (char *)(t + need));
    used += (need + 15) & ~(size_t)15;
    return (uintptr_t)t;
}

// the entry of fn becomes a jump to to; *orig gets a callable original. 0 on success
int unityhooks_hook(uintptr_t fn, char isa, void *to, void **orig)
{
    size_t moved = isa == ISA_ARM ? 8 : 16;
    uintptr_t t = make_trampoline(fn, moved, isa);
    if (!t) return -1;
    if (isa == ISA_ARM) {
        uint32_t j[2] = { 0xe51ff004u, (uint32_t)(uintptr_t)to };     // Thumb bit kept: ldr pc interworks
        if (patch_bytes(fn, j, 8) != 0) return -1;
    } else {
        uint32_t j[4] = { 0x58000050u, 0xd61f0200u, 0, 0 };
        uint64_t a = (uint64_t)(uintptr_t)to;
        memcpy(&j[2], &a, 8);
        if (patch_bytes(fn, j, 16) != 0) return -1;
    }
    *orig = (void *)t;
    return 0;
}

// --- the row whose bytes the loaded library has
int unityhooks_matches(const struct row *r, uintptr_t base, size_t size)
{
    if ((r->isa != ISA_A64) != (sizeof(void *) == 4)) return 0;
    if ((size_t)r->off + r->size > size) return 0;
    return fnv1a((const uint8_t *)(base + r->off), r->size) == r->fnv;
}
static const struct row *find_row(const struct row *rows, size_t n, uintptr_t base, size_t size)
{
    for (size_t i = 0; i < n; i++)
        if (unityhooks_matches(&rows[i], base, size)) return &rows[i];
    return NULL;
}

// --- UA_GFX_JOBS=legacy
static int (*g_adjust_orig)(int mode, int renderer, int flags);
static int g_adjust_logged;

static int adjust_hook(int mode, int renderer, int flags)
{
    int r = g_adjust_orig(mode, renderer, flags);
    if (mode == MODE_CLIENT_WORKER_JOBS && renderer == RENDERER_GLES3X && r == MODE_THREADED) {
        if (!g_adjust_logged++)
            fprintf(stderr, "[unityhooks] graphics jobs: ClientWorkerJobs kept on OpenGL ES (Unity would use Threaded)\n");
        r = mode;
    } else if (!g_adjust_logged++) {
        fprintf(stderr, "[unityhooks] graphics jobs: Unity asked for threading mode %d on renderer %d -> %d (unchanged; "
                        "-force-gfx-jobs legacy selects 3)\n", mode, renderer, r);
    }
    return r;
}

static int g_gfx_jobs = -1;

int unityhooks_gfx_jobs(void)
{
    if (g_gfx_jobs < 0) {
        const char *v = getenv("UA_GFX_JOBS");
        const char *off = getenv("UA_UNITY_HOOKS");
        g_gfx_jobs = v && !strcmp(v, "legacy") && !(off && !strcmp(off, "0"));
        if (v && *v && strcmp(v, "legacy") && strcmp(v, "0") && strcmp(v, "off"))
            fprintf(stderr, "[unityhooks] UA_GFX_JOBS=%s: only 'legacy' is known; left off\n", v);
    }
    return g_gfx_jobs;
}

// --- UA_PRESENT_SYNC
// GraphicsDeviceSettings_Set_Custom_PropWaitForPresentSyncPoint(GfxDeviceWaitForPresentSyncPoint): stores the
// value in GetGfxDevice() at 0x23a0; GfxDeviceClient::BeginFrame calls WaitForPendingPresent when it is 0,
// EndFrame when it is 1 (both read in 6000.0.61f1 armeabi-v7a; their rows tie the setter to that code). The
// arm64-v8a IL2CPP player of 6000.0.61f1 the same at 0x25d8 (read 2026-11-09, Hollow Knight's 64-bit build)
static const struct row g_psync[] = {
    { "6000.0.61f1 armeabi-v7a", "GraphicsDeviceSettings.waitForPresentSyncPoint (set)", 0x31ebd6, 0x10, ISA_THUMB, 0x973b56982359eb07ull },
    { "6000.0.61f1 armeabi-v7a", "GfxDeviceClient::BeginFrame", 0x62f468, 0x70, ISA_THUMB, 0x0f58bedb8c26c3acull },
    { "6000.0.61f1 armeabi-v7a", "GfxDeviceClient::EndFrame", 0x62f554, 0x6c, ISA_THUMB, 0xe6b3b700099f6c08ull },
    { "6000.0.61f1 arm64-v8a", "GraphicsDeviceSettings.waitForPresentSyncPoint (set)", 0x6a9398, 0x18, ISA_A64, 0x466b859985ec5841ull },
    { "6000.0.61f1 arm64-v8a", "GfxDeviceClient::BeginFrame", 0xab6408, 0xac, ISA_A64, 0x463888fc7b8a3132ull },
    { "6000.0.61f1 arm64-v8a", "GfxDeviceClient::EndFrame", 0xab6564, 0x9c, ISA_A64, 0x14176be35341ce5full },
};
static void (*g_psync_set)(int);
static int g_psync_value = -1;

static void present_sync_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_PRESENT_SYNC");
    if (!v || !*v) return;
    int want = !strcmp(v, "end") ? 1 : !strcmp(v, "begin") ? 0 : -1;
    if (want < 0) { fprintf(stderr, "[unityhooks] UA_PRESENT_SYNC=%s: only 'end' or 'begin'; left as the game has it\n", v); return; }
    for (size_t i = 0; i + 3 <= sizeof g_psync / sizeof *g_psync; i += 3)
        if (unityhooks_matches(&g_psync[i], base, size) && unityhooks_matches(&g_psync[i + 1], base, size) &&
            unityhooks_matches(&g_psync[i + 2], base, size)) {
            g_psync_set = (void (*)(int))(base + g_psync[i].off + (g_psync[i].isa == ISA_THUMB));
            g_psync_value = want;
            return;
        }
    fprintf(stderr, "[unityhooks] UA_PRESENT_SYNC: this libunity.so is not one whose present sync point was read "
                    "(tools/re/unityhooks.py); left as the game has it\n");
}

void unityhooks_frame(void)
{
    static unsigned long frames;
    if (!g_psync_set || ++frames != 2) return;       // (the device made, the game's own start-up settings applied)
    g_psync_set(g_psync_value);
    fprintf(stderr, "[unityhooks] UA_PRESENT_SYNC: the main thread waits for the last frame's present at the %s of its frame\n",
            g_psync_value ? "end" : "beginning");
}

void unityhooks_on_load(so_module *m)
{
    const char *name = so_name(m), *off = getenv("UA_UNITY_HOOKS");
    if (!name || strcmp(name, "libunity.so") || (off && !strcmp(off, "0"))) return;
    uintptr_t base = so_base(m);
    size_t size = so_size(m);
    unitypool_on_load(base, size);              // UA_GLES_POOL
    unityverts_on_load(base, size);             // UA_FAST_VERTS
    unitymath_on_load(base, size);              // UA_INV_MEMO
    present_sync_on_load(base, size);           // UA_PRESENT_SYNC
    if (!unityhooks_gfx_jobs()) return;
    const struct row *r = find_row(g_adjust, sizeof g_adjust / sizeof *g_adjust, base, size);
    if (!r) {
        fprintf(stderr, "[unityhooks] UA_GFX_JOBS: this libunity.so is not one whose AdjustThreadingModeForAPI was read "
                        "(tools/re/unityhooks.py); graphics jobs stay as Unity sets them\n");
        return;
    }
    if (unityhooks_hook(base + r->off, r->isa, (void *)adjust_hook, (void **)&g_adjust_orig) != 0) {
        fprintf(stderr, "[unityhooks] %s: hook failed; graphics jobs stay as Unity sets them\n", r->func);
        return;
    }
    fprintf(stderr, "[unityhooks] %s hooked (libunity %s, +0x%x)\n", r->func, r->unity, r->off);
}
