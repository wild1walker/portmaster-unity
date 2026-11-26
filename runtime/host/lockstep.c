#define _GNU_SOURCE
// lockstep.c: the lockstep checker. A play session is recorded (every input event the host sends and the
// frame it reaches the game in), replayed with the same events at the same frames, and after every frame
// the game's state is hashed: two replays, one of them with a change (an env var, a newer piece of the
// player), are then compared frame by frame (`lockstep compare`, setup-rs/crates/lockstep), down to the
// object and field where they part (`lockstep diff` of two dumps of that frame). Off unless UA_LOCKSTEP
// is set; nothing is looked up or hooked then. Mono games only (IL2CPP: no state walk, inputs and clock only).
//
//   UA_LOCKSTEP=record:<file>   the input events, written with the frame they are given to the game in
//   UA_LOCKSTEP=replay:<file>   those events given at the same frames; the pad's own events are dropped
//   UA_LOCKSTEP=hash            neither (the clock and seeds are still fixed): a run without input
//   UA_LOCKSTEP_HASH=<file>     the state hash of every frame (lines below); without it nothing is hashed
//   UA_LOCKSTEP_FRAMES=<n>      Unity asked to quit after frame <n> (a replay: the record's last frame)
//   UA_LOCKSTEP_DUMP=<f>[,<f>]  every hashed value of frame <f>, by object and field: <hash file>.<f>.dump
//   UA_LOCKSTEP_FPS=<n>         the game's time step, 1/<n> s a frame (default 60)
//   UA_LOCKSTEP_SEED=<n>        UnityEngine.Random's seed (default 1)
//   UA_LOCKSTEP_DEPTH=<n>       how deep plain managed objects are followed from a script's fields (default 2)
//   UA_LOCKSTEP_ELEMS=<n>       elements of an array of objects or structs hashed (default 32; arrays of
//                               numbers: their first 64 KB)
//   UA_LOCKSTEP_SETTLE=<ms>     after each frame, wait (up to <ms>, default 5000; 0: never) until Unity's
//                               loading threads ("Loading.*") are asleep: a scene's files are read in the same
//                               frames however fast the card is
//   UA_LOCKSTEP_CLOCK=0         leave the managed clocks alone (below)
//   UA_LOCKSTEP_LOADS=0         scene loads not held to the frame (hold_loads below; 64-bit players)
//   UA_LOCKSTEP_AUDIO=0         the silent sound output (ANDROID_SLES_OUTPUT=none) on the clock, not paced by frames
//   UA_LOCKSTEP_UNITY_CLOCK=0   leave Unity's own clock on UnityMain alone (unity_clock below)
//   UA_LOCKSTEP_EVENT_CLOCK=0   the input events keep the real uptime as their times
//   UA_LOCKSTEP_ICALLS=0        transforms and enabled read through mono_runtime_invoke only
//   UA_LOCKSTEP_IGNORE=<class>[.<field>][,...]  fields (or all of a class's own fields) left out of the hash: what a
//                               game keeps of the sound's clock
//   UA_LOCKSTEP_NUDGE=<class>.<field>@<frame>[+<delta>]   a test aid that CHANGES the game: after frame
//                               <frame>, <delta> (default 1) added to that number field of every script of
//                               that class: the checker must name it
//
// The clock. After the first frame Mono is up in, Time.captureDeltaTime = 1/fps (Time.captureFramerate
// before 2019.2): Time.time, deltaTime and the FixedUpdate count advance a fixed step a frame whatever the
// real time. Time.realtimeSinceStartup(AsDouble) (wrapped where Unity registers it), Environment.TickCount,
// Stopwatch.GetTimestamp and DateTime.UtcNow/Now (registered over Mono's own internal calls once Mono is up)
// read a virtual clock: frames done * step, plus 1 us for each read past a frame's first 2000 on that thread (a loop
// that waits for the clock still ends). System.Random() is seeded from TickCount, so from the frame.
// UnityEngine.Random.InitState(seed) in the same frame.
// Input. Every event the host sends (pads, keyboard, pointer, touch: unityplayer.c's input tap) is held
// and given to Unity on UnityMain after a frame, so it reaches the game in the next frame, the frame
// written beside it. A replay gives the same events after the same frames. The events' times come from the
// virtual clock, counted from the real uptime the run started at (UA_LOCKSTEP_EVENT_CLOCK=0: the real uptime).
// The hash file: "# lockstep 1 fps=<n> seed=<n> depth=<n> elems=<n>", then for each frame
//   F <frame> <all> t=<Time.time bits> rs=<Random.state> nb=<scripts> nt=<transforms> tr=<transforms' hash>
//   C <frame> <class> <count> <hash>   a class whose scripts' hash changed since the last frame (count 0: gone)
//   S <frame> <scene> <scenes loaded>  the active scene changed
//   W <frame> <ms>                     the frame waited for the loading threads (over 1 ms)
//   I <frame> <events>                 input events given after the frame
// and "E <frame> <ms of hashing> <ms waited>" at the end. A script's hash: its class, enabled, its
// transform's local position, rotation and scale (raw float bits), and its fields: numbers as their bits,
// strings by their characters, structs field by field, arrays element by element, references to engine
// objects (UnityEngine.Object) by class and alive or destroyed, other objects followed (UA_LOCKSTEP_DEPTH)
// and hashed the same way. Left out: static fields, IntPtr fields, UnityEngine.Object's own fields
// (native pointers, instance ids), delegates, reflection objects, hash-derived fields of System.Collections
// (buckets, hashCode, next: they follow object addresses). Scripts and transforms of inactive GameObjects
// are not seen (Object.FindObjectsOfType). A frame's hash is order-free (a sum over objects).
#include "lockstep.h"
#include "../loader/so_loader.h"
#include "../jni/unityplayer.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void *mono_runtime_lib(void);   // hitches.c
void android_sles_set_paced(int on);                  // ../android/opensles.c: the silent output paced by frames
void android_sles_pace(long long frames, int fps);

enum { OFF, M_RECORD, M_REPLAY, M_HASH };
static int g_mode, g_cfg;
static char g_in_path[512], g_hash_path[512];
static long g_frames_max, g_dump[8];
static int g_ndump, g_fps = 60, g_seed = 1, g_depth = 2, g_elems = 32, g_settle_ms = 5000, g_clock = 1;
static char g_nudge_class[128], g_nudge_field[128], g_ignore[1024];
static long g_nudge_frame = -1;
static double g_nudge_delta = 1;

static void cfg(void)
{
    if (g_cfg) return;
    g_cfg = 1;
    const char *e = getenv("UA_LOCKSTEP");
    if (!e || !*e || !strcmp(e, "0")) return;
    if (!strncmp(e, "record:", 7)) { g_mode = M_RECORD; snprintf(g_in_path, sizeof g_in_path, "%s", e + 7); }
    else if (!strncmp(e, "replay:", 7)) { g_mode = M_REPLAY; snprintf(g_in_path, sizeof g_in_path, "%s", e + 7); }
    else if (!strcmp(e, "hash") || !strcmp(e, "1")) g_mode = M_HASH;
    else { fprintf(stderr, "[lockstep] UA_LOCKSTEP=%s: not record:<file>, replay:<file> or hash: off\n", e); return; }
    if ((g_mode == M_RECORD || g_mode == M_REPLAY) && !g_in_path[0]) { fprintf(stderr, "[lockstep] no input file: off\n"); g_mode = OFF; return; }
#define NUM(var, name, lo) if ((e = getenv(name)) && *e) { long v = atol(e); if (v >= lo) var = v; }
    NUM(g_frames_max, "UA_LOCKSTEP_FRAMES", 1); NUM(g_fps, "UA_LOCKSTEP_FPS", 1); NUM(g_depth, "UA_LOCKSTEP_DEPTH", 0);
    NUM(g_elems, "UA_LOCKSTEP_ELEMS", 0); NUM(g_settle_ms, "UA_LOCKSTEP_SETTLE", 0);
#undef NUM
    if ((e = getenv("UA_LOCKSTEP_SEED")) && *e) g_seed = atoi(e);
    if ((e = getenv("UA_LOCKSTEP_CLOCK")) && !strcmp(e, "0")) g_clock = 0;
    if ((e = getenv("UA_LOCKSTEP_IGNORE")) && *e) snprintf(g_ignore, sizeof g_ignore, "%s", e);
    if ((e = getenv("UA_LOCKSTEP_HASH")) && *e) snprintf(g_hash_path, sizeof g_hash_path, "%s", e);
    if ((e = getenv("UA_LOCKSTEP_DUMP")) && *e)
        for (const char *p = e; *p && g_ndump < 8; ) { g_dump[g_ndump++] = atol(p); p = strchr(p, ','); if (!p) break; p++; }
    if ((e = getenv("UA_LOCKSTEP_NUDGE")) && *e) {
        const char *dot = strrchr(e, '.'), *at = strchr(e, '@');
        if (dot && at && dot < at && dot - e < 127 && at - dot - 1 < 127) {
            memcpy(g_nudge_class, e, (size_t)(dot - e)); g_nudge_class[dot - e] = 0;
            memcpy(g_nudge_field, dot + 1, (size_t)(at - dot - 1)); g_nudge_field[at - dot - 1] = 0;
            g_nudge_frame = atol(at + 1);
            const char *plus = strchr(at, '+'), *minus = strchr(at + 1, '-');
            if (plus) g_nudge_delta = atof(plus + 1); else if (minus) g_nudge_delta = atof(minus);
        } else fprintf(stderr, "[lockstep] UA_LOCKSTEP_NUDGE=%s: not <class>.<field>@<frame>[+<delta>]\n", e);
    }
}
int lockstep_on(void) { cfg(); return g_mode != OFF; }

/* ---- the virtual clock -------------------------------------------------------------------- */
static volatile long g_frame;          // frames done (lockstep_frame calls)
static long g_step_us = 16667;
static int64_t g_epoch_unix = 1767225600;   // 2026-01-01 UTC; a record keeps its own
// (no initial values: the loader's thread-local block must stay as so_loader.h says)
static __thread long t_frame1;   // the frame + 1 the count below is of
static __thread long t_calls;
static int64_t virt_us(void)
{
    long f = __atomic_load_n(&g_frame, __ATOMIC_ACQUIRE);
    if (t_frame1 != f + 1) { t_frame1 = f + 1; t_calls = 0; }
    // the frame's time for its first 2000 reads on a thread (so it does not depend on how many reads a frame's work
    // made: a loop timing a slice of work sees none pass), then 1 us more for each read (a loop waiting for the
    // clock still ends), never past the next frame's
    long c = t_calls++, adv = c < 2000 ? 0 : c - 2000;
    if (adv > g_step_us - 1) adv = g_step_us - 1;
    return 1000000 + (int64_t)f * g_step_us + adv;
}
static long g_clock_reads[6];
static SO_ABI float w_realtime(void) { g_clock_reads[0]++; return (float)(virt_us() / 1e6); }
static SO_ABI double w_realtime_d(void) { g_clock_reads[1]++; return virt_us() / 1e6; }
static int32_t w_tickcount(void) { g_clock_reads[2]++; return (int32_t)(virt_us() / 1000); }
static int64_t w_tickcount64(void) { g_clock_reads[3]++; return virt_us() / 1000; }
static int64_t w_timestamp(void) { g_clock_reads[4]++; return virt_us() * 10; }
// Time.unscaledTime / unscaledDeltaTime: Unity counts them from its real start and its first frames' real lengths
// (6000.0 keeps the capture step only from its second frame on): from the frame count instead
static SO_ABI float w_unscaled(void) { return (float)((double)__atomic_load_n(&g_frame, __ATOMIC_ACQUIRE) * g_step_us / 1e6); }
static SO_ABI double w_unscaled_d(void) { return (double)__atomic_load_n(&g_frame, __ATOMIC_ACQUIRE) * g_step_us / 1e6; }
static SO_ABI float w_unscaled_dt(void) { return (float)(g_step_us / 1e6); }
static int64_t w_filetime(void) { g_clock_reads[5]++; return (g_epoch_unix + 11644473600LL) * 10000000LL + virt_us() * 10; }
static int64_t g_base_ns;          // the real monotonic time lockstep_init ran at: the virtual clocks' zero
// the input events' times (getEventTime: Android's uptime, the monotonic clock in ms) on the same virtual clock as
// Unity's own reads (unity_clock below), so the times Unity keeps of them (the Input System's device update
// times) are the same every run
static int64_t event_clock(void) { return g_base_ns / 1000000 + (virt_us() - 1000000) / 1000; }

// Unity's own clock on UnityMain (UA_LOCKSTEP_UNITY_CLOCK, on unless 0): the monotonic clock the Android libraries
// read there (the loader's clock_gettime, so_set_clock_hook) answers from the frame count too, from the real time
// the run started at: Unity's frame times, and the milliseconds it gives a load's main-thread work in a frame
// (Application.backgroundLoadingPriority, the async upload's time slice), are then the same in every run. So does
// Android's UI thread (the process's first) until the first frame: Unity takes its time base there at start, which
// Time.unscaledTime and Time.time count from (later its reads come at any moment of a frame: the real clock). Other
// threads keep the real clock (their timed waits). The wall clock (CLOCK_REALTIME, gettimeofday, time()) on those
// threads reads the same virtual time from the record's start date: an engine seed taken from it is the same.
#include <sys/prctl.h>
#include <sys/syscall.h>
static int g_unity_clock = 1;
static __thread int t_main;       // 0 unknown, 1 UnityMain, 2 another thread
static int unity_clock(int clk, long *sec, long *nsec)
{
    int wall = clk == 0 || clk == 5;                                  // REALTIME(_COARSE), gettimeofday, time()
    if (!wall && clk != 1 && clk != 4 && clk != 6 && clk != 7) return -1;     // MONOTONIC, _RAW, _COARSE, BOOTTIME
    if (!t_main) { char n[17] = ""; prctl(PR_GET_NAME, n, 0, 0, 0); t_main = !strcmp(n, "UnityMain") ? 1 : syscall(SYS_gettid) == getpid() ? 3 : 2; }
    if (t_main == 2 || (t_main == 3 && __atomic_load_n(&g_frame, __ATOMIC_ACQUIRE) > 0)) return -1;
    int64_t ns = (wall ? g_epoch_unix * 1000000000LL : g_base_ns) + (virt_us() - 1000000) * 1000;
    *sec = (long)(ns / 1000000000); *nsec = (long)(ns % 1000000000);
    return 0;
}

/* ---- the hooks: Unity's Time icalls, Mono's clocks ---------------------------------------- */
typedef void (*add_icall_fn)(const char *, const void *);
static add_icall_fn r_mono_add, r_il2cpp_add;
static void *(*r_jit_init)(const char *, const char *);
static void register_clocks(void);
static int name_is(const char *name, const char *want)
{
    size_t n = strlen(want);
    return !strncmp(name, want, n) && (name[n] == 0 || name[n] == '(');
}
// Scene loads held to the frame (UA_LOCKSTEP_LOADS, on unless 0; 64-bit players): the internal call behind
// SceneManager.LoadScene(Async) is wrapped to note the AsyncOperation it returns, and after
// each frame UnityMain waits until every noted load has read its scene (progress 0.9, or done): what a load does
// on its own threads is then over before the next frame however fast the card is, and its main-thread part comes
// in the same frames every run. A wait over 10 s gives up (logged).
static FILE *g_hash;
static double now_ms(void);
#define MAX_OPS 16
static int g_loads = 1;
static void *g_ops[MAX_OPS];                // the native AsyncOperation (2023+) or the managed one
static int g_nops, g_ops_native, g_load_timeouts;
static long g_loads_noted;
static double g_load_wait_ms;
typedef uintptr_t (*any_fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static any_fn r_load;
static float (*r_progress)(void *);         // AsyncOperation.get_progress(_Injected): the managed object or the native one
static int (*r_isdone)(void *);
static void note_op(uintptr_t op, int native)
{
    if (!op || g_nops >= MAX_OPS) return;
    g_ops_native = native;
    g_ops[g_nops++] = (void *)op;
    g_loads_noted++;
}
#if defined(__aarch64__)
// (any arguments of the integer class, up to 8: passed through as they came)
static uintptr_t w_load(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{ uintptr_t op = r_load(a, b, c, d, e, f, g, h); note_op(op, g_ops_native); return op; }
#endif
static void hold_loads(long frame)
{
    if (!g_nops || !r_progress) { g_nops = 0; return; }
    double t0 = now_ms();
    for (int i = 0; i < g_nops; i++) {
        for (;;) {
            if ((r_isdone && (r_isdone(g_ops[i]) & 0xff)) || r_progress(g_ops[i]) >= 0.9f) break;
            if (now_ms() - t0 > 10000) { g_load_timeouts++; fprintf(stderr, "[lockstep] frame %ld: a load still reading after 10 s: not held\n", frame); break; }
            struct timespec ts = { 0, 500000 }; nanosleep(&ts, NULL);
        }
    }
    g_nops = 0;
    double ms = now_ms() - t0;
    g_load_wait_ms += ms;
    if (g_hash) fprintf(g_hash, "L %ld %.1f\n", frame, ms);
}
static const void *wrap_icall(const char *name, const void *fn)
{
#if defined(__aarch64__)
    if (g_loads && name && fn) {
        const char *c = strstr(name, "::");
        if (c && (name_is(c + 2, "LoadSceneAsyncNameIndexInternal") || name_is(c + 2, "LoadSceneAsyncNameIndexInternal_Injected"))) {
            g_ops_native = strstr(c, "_Injected") != NULL; r_load = (any_fn)fn; return (const void *)w_load;
        }
        if (name_is(name, "UnityEngine.AsyncOperation::get_progress_Injected") || (!r_progress && name_is(name, "UnityEngine.AsyncOperation::get_progress")))
            r_progress = (float (*)(void *))fn;
        if (name_is(name, "UnityEngine.AsyncOperation::get_isDone_Injected") || (!r_isdone && name_is(name, "UnityEngine.AsyncOperation::get_isDone")))
            r_isdone = (int (*)(void *))fn;
    }
#endif
    if (g_clock && name && fn) {
        if (name_is(name, "UnityEngine.Time::get_realtimeSinceStartup")) return (const void *)w_realtime;
        if (name_is(name, "UnityEngine.Time::get_realtimeSinceStartupAsDouble")) return (const void *)w_realtime_d;
        if (name_is(name, "UnityEngine.Time::get_unscaledTime")) return (const void *)w_unscaled;
        if (name_is(name, "UnityEngine.Time::get_unscaledTimeAsDouble")) return (const void *)w_unscaled_d;
        if (name_is(name, "UnityEngine.Time::get_unscaledDeltaTime")) return (const void *)w_unscaled_dt;
    }
    return fn;
}
// Unity registers its internal calls once Mono is up: the first one is the moment for Mono's clocks too
// (the Mono route on the IL2CPP player initialises Mono inside its bridge, past mono_jit_init_version's hook)
static void w_mono_add(const char *name, const void *fn) { register_clocks(); r_mono_add(name, wrap_icall(name, fn)); }
static void w_il2cpp_add(const char *name, const void *fn) { register_clocks(); r_il2cpp_add(name, wrap_icall(name, fn)); }
static void *w_jit_init(const char *file, const char *version)
{
    void *d = r_jit_init(file, version);
    if (d) register_clocks();
    return d;
}
static void register_clocks(void)
{
    static int done;
    if (done || !g_clock) return;
    done = 1;
    void *h = mono_runtime_lib();
    add_icall_fn add = h ? (add_icall_fn)so_dlsym(h, "mono_add_internal_call") : NULL;
    if (add && add == w_mono_add) add = r_mono_add;          // (our own wrapper: the call it wraps)
    if (add) {
        // registered names are looked up before Mono's own table (icall.c): these answer from the virtual clock
        add("System.Environment::get_TickCount", (const void *)w_tickcount);
        add("System.Environment::get_TickCount64", (const void *)w_tickcount64);
        add("System.Diagnostics.Stopwatch::GetTimestamp", (const void *)w_timestamp);
        add("System.DateTime::GetSystemTimeAsFileTime", (const void *)w_filetime);
        fprintf(stderr, "[lockstep] Mono's clocks read the frame clock (TickCount, Stopwatch, DateTime)\n");
    } else fprintf(stderr, "[lockstep] no Mono (IL2CPP?): Environment.TickCount, Stopwatch and DateTime keep the real clock\n");
}
void *lockstep_dlsym_hook(const char *symbol, void *found)
{
    cfg();
    if (g_mode == OFF || !found) return found;
    if (!strcmp(symbol, "mono_add_internal_call") && found != (void *)w_mono_add) { r_mono_add = (add_icall_fn)found; return (void *)w_mono_add; }
    if (!strcmp(symbol, "il2cpp_add_internal_call") && found != (void *)w_il2cpp_add) { r_il2cpp_add = (add_icall_fn)found; return (void *)w_il2cpp_add; }
    if (!strcmp(symbol, "mono_jit_init_version") && found != (void *)w_jit_init) { r_jit_init = found; return (void *)w_jit_init; }
    return found;
}

/* ---- input: held, written, given after the frame ------------------------------------------ */
static pthread_mutex_t g_in_mx = PTHREAD_MUTEX_INITIALIZER;
static struct up_input_rec *g_held, *g_recs;
static long *g_rec_frame;
static int g_nheld, g_capheld, g_nrecs, g_next_rec, g_dropped;
static FILE *g_in;
static long g_rec_end = -1;
static int tap(const struct up_input_rec *r)
{
    if (g_mode == M_REPLAY) { __atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED); return 1; }
    pthread_mutex_lock(&g_in_mx);
    if (g_nheld == g_capheld) { g_capheld = g_capheld ? 2 * g_capheld : 64; g_held = realloc(g_held, g_capheld * sizeof *g_held); }
    g_held[g_nheld++] = *r;
    pthread_mutex_unlock(&g_in_mx);
    return 1;
}
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static void write_rec(long frame, const struct up_input_rec *r)
{
    fprintf(g_in, "%ld %d %d %d %d %d %d %d %d %08x %08x %d", frame, r->kind, r->device, r->source, r->action, r->keycode, r->repeat,
            r->meta, r->buttons, fbits(r->vscroll), fbits(r->hscroll), r->n);
    for (int i = 0; i < r->n && i < UP_REC_MAX; i++) fprintf(g_in, " %d:%08x:%08x", r->axis[i], fbits(r->value[i]), fbits(r->y[i]));
    fputc('\n', g_in);
}
static int read_recs(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[lockstep] %s: cannot read the record\n", path); return 0; }
    char line[2048];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') {
            char *p = strstr(line, "epoch=");
            if (p) g_epoch_unix = atoll(p + 6);
            p = strstr(line, "fps=");
            if (p && !getenv("UA_LOCKSTEP_FPS")) g_fps = atoi(p + 4) > 0 ? atoi(p + 4) : g_fps;
            continue;
        }
        if (line[0] == 'E') { g_rec_end = atol(line + 1); continue; }
        struct up_input_rec r = { 0 };
        long fr; unsigned vs, hs; int used = 0;
        if (sscanf(line, "%ld %d %d %d %d %d %d %d %d %x %x %d%n", &fr, &r.kind, &r.device, &r.source, &r.action, &r.keycode, &r.repeat,
                   &r.meta, &r.buttons, &vs, &hs, &r.n, &used) < 12) continue;
        r.vscroll = bitsf(vs); r.hscroll = bitsf(hs);
        if (r.n < 0 || r.n > UP_REC_MAX) continue;
        char *p = line + used;
        for (int i = 0; i < r.n; i++) {
            unsigned a, b; int ax, k = 0;
            if (sscanf(p, " %d:%x:%x%n", &ax, &a, &b, &k) < 3) break;
            r.axis[i] = ax; r.value[i] = bitsf(a); r.y[i] = bitsf(b); p += k;
        }
        if (g_nrecs == cap) { cap = cap ? 2 * cap : 256; g_recs = realloc(g_recs, cap * sizeof *g_recs); g_rec_frame = realloc(g_rec_frame, cap * sizeof *g_rec_frame); }
        g_rec_frame[g_nrecs] = fr; g_recs[g_nrecs++] = r;
    }
    fclose(f);
    return 1;
}
// the first events given, with Unity's answer (nativeInjectEvent: 1 taken)
static void given(long frame, const struct up_input_rec *r, int answer)
{
    static int lines;
    if (lines++ < 6) fprintf(stderr, "[lockstep] after frame %ld: event %d (device %d, action %d, key %d) given -> %d\n", frame, r->kind, r->device, r->action, r->keycode, answer);
}
static int give_inputs(long frame)
{
    int n = 0;
    if (g_mode == M_RECORD) {
        pthread_mutex_lock(&g_in_mx);
        struct up_input_rec *h = g_held;
        int nh = g_nheld;
        g_held = NULL; g_nheld = g_capheld = 0;
        pthread_mutex_unlock(&g_in_mx);
        for (int i = 0; i < nh; i++) { if (g_in) write_rec(frame, &h[i]); given(frame, &h[i], up_inject_rec(&h[i])); }
        if (nh && g_in) fflush(g_in);
        free(h);
        n = nh;
    } else if (g_mode == M_REPLAY) {
        while (g_next_rec < g_nrecs && g_rec_frame[g_next_rec] < frame) g_next_rec++;   // (none: records are in order)
        while (g_next_rec < g_nrecs && g_rec_frame[g_next_rec] == frame) { given(frame, &g_recs[g_next_rec], up_inject_rec(&g_recs[g_next_rec])); g_next_rec++; n++; }
    }
    return n;
}

/* ---- Mono --------------------------------------------------------------------------------- */
typedef void MonoObject, MonoClass, MonoMethod, MonoImage, MonoDomain, MonoType, MonoClassField, MonoArray;
static struct {
    MonoDomain *(*domain_get)(void);
    MonoImage *(*image_loaded)(const char *);
    MonoClass *(*class_from_name)(MonoImage *, const char *, const char *);
    MonoMethod *(*method_from_name)(MonoClass *, const char *, int);
    MonoObject *(*invoke)(MonoMethod *, void *, void **, MonoObject **);
    MonoType *(*class_get_type)(MonoClass *);
    MonoObject *(*type_get_object)(MonoDomain *, MonoType *);
    uintptr_t (*array_length)(MonoArray *);
    char *(*array_addr)(MonoArray *, int, uintptr_t);
    void *(*unbox)(MonoObject *);
    MonoClass *(*object_get_class)(MonoObject *);
    const char *(*class_get_name)(MonoClass *);
    const char *(*class_get_namespace)(MonoClass *);
    MonoClass *(*class_get_parent)(MonoClass *);
    MonoClassField *(*class_get_fields)(MonoClass *, void **);
    const char *(*field_get_name)(MonoClassField *);
    MonoType *(*field_get_type)(MonoClassField *);
    uint32_t (*field_get_offset)(MonoClassField *);
    uint32_t (*field_get_flags)(MonoClassField *);
    int (*type_get_type)(MonoType *);
    MonoClass *(*class_from_mono_type)(MonoType *);
    int (*class_is_valuetype)(MonoClass *);
    int32_t (*class_value_size)(MonoClass *, uint32_t *);
    int (*class_get_rank)(MonoClass *);
    MonoClass *(*class_get_element_class)(MonoClass *);
    int32_t (*array_element_size)(MonoClass *);
    int (*class_is_subclass_of)(MonoClass *, MonoClass *, int);
    MonoClassField *(*class_get_field_from_name)(MonoClass *, const char *);
    MonoClass *(*class_get_nesting_type)(MonoClass *);
    uint32_t (*method_get_flags)(MonoMethod *, uint32_t *);
    void *(*lookup_icall)(MonoMethod *);
    char *(*string_to_utf8)(MonoObject *);
    void (*free)(void *);
} M;
static int g_mono = -1;   // -1 not looked up, 0 none, 1 ready
static MonoClass *k_obj, *k_mb, *k_tr, *k_time, *k_rand, *k_sm, *k_scene, *k_delegate, *k_type;
static MonoObject *t_mb, *t_tr;
static MonoMethod *m_find, *m_find_by, *m_time, *m_fc, *m_ts, *m_ut, *m_cdt, *m_dt, *m_rand_state, *m_active_scene, *m_scene_name, *m_scene_count;
static uint32_t g_cached_ptr_off;
static const unsigned HDR = 2 * sizeof(void *);   // MonoObject: vtable, synchronisation
// Unity's internal calls, called as Mono does (pointers in, an int or pointer back: the same in either ARM float ABI)
typedef MonoObject *(*obj_icall)(MonoObject *);
typedef int (*bool_icall)(MonoObject *);
typedef void (*vec_icall)(MonoObject *, float *);
static obj_icall ic_transform;
static bool_icall ic_enabled;
static vec_icall ic_trs[3];
// 2023.1+: the bindings pass the native object (UnityEngine.Object's m_CachedPtr), not the managed one:
// static extern get_localPosition_Injected(IntPtr _unity_self, out Vector3 ret), get_enabled_Injected(IntPtr)
static int ic_native, ic_enabled_native;
static MonoMethod *m_transform, *m_enabled, *m_trs[3], *m_parent, *m_name;

static MonoMethod *method(MonoClass *k, const char *name, int n)
{
    for (; k; k = M.class_get_parent(k)) { MonoMethod *m = M.method_from_name(k, name, n); if (m) return m; }
    return NULL;
}
static void *icall_of(MonoMethod *m)
{
    uint32_t iflags = 0;
    if (!m || !M.lookup_icall || !M.method_get_flags) return NULL;
    M.method_get_flags(m, &iflags);
    return (iflags & 0x1000) ? M.lookup_icall(m) : NULL;    // METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL
}
static int mono_ready(void)
{
    if (g_mono >= 0) return g_mono;
    void *h = mono_runtime_lib();
    if (!h) return 0;                       // not yet (or IL2CPP): asked again next frame
    g_mono = 0;
#define L(v, n) if (!(*(void **)&M.v = so_dlsym(h, n))) { fprintf(stderr, "[lockstep] %s missing: no state hash\n", n); return 0; }
    L(domain_get, "mono_domain_get"); L(image_loaded, "mono_image_loaded"); L(class_from_name, "mono_class_from_name");
    L(method_from_name, "mono_class_get_method_from_name"); L(invoke, "mono_runtime_invoke"); L(class_get_type, "mono_class_get_type");
    L(type_get_object, "mono_type_get_object"); L(array_addr, "mono_array_addr_with_size"); L(unbox, "mono_object_unbox");
    L(object_get_class, "mono_object_get_class"); L(class_get_name, "mono_class_get_name"); L(class_get_namespace, "mono_class_get_namespace");
    L(class_get_parent, "mono_class_get_parent"); L(class_get_fields, "mono_class_get_fields"); L(field_get_name, "mono_field_get_name");
    L(field_get_type, "mono_field_get_type"); L(field_get_offset, "mono_field_get_offset"); L(field_get_flags, "mono_field_get_flags");
    L(type_get_type, "mono_type_get_type"); L(class_from_mono_type, "mono_class_from_mono_type"); L(class_is_valuetype, "mono_class_is_valuetype");
    L(class_value_size, "mono_class_value_size"); L(class_get_rank, "mono_class_get_rank"); L(class_get_element_class, "mono_class_get_element_class");
    L(array_element_size, "mono_array_element_size"); L(class_is_subclass_of, "mono_class_is_subclass_of");
    L(class_get_field_from_name, "mono_class_get_field_from_name"); L(string_to_utf8, "mono_string_to_utf8"); L(free, "mono_free");
#undef L
    *(void **)&M.array_length = so_dlsym(h, "mono_array_length");
    *(void **)&M.method_get_flags = so_dlsym(h, "mono_method_get_flags");
    *(void **)&M.class_get_nesting_type = so_dlsym(h, "mono_class_get_nesting_type");
    *(void **)&M.lookup_icall = so_dlsym(h, "mono_lookup_internal_call");
    g_mono = 1;
    return 1;
}
static uintptr_t arr_len(MonoArray *a) { return M.array_length ? M.array_length(a) : (uintptr_t)*(uint32_t *)((char *)a + 3 * sizeof(void *)); }

// UnityEngine's classes, once its assembly is loaded (0: not yet)
static int engine_ready(void)
{
    static int done;
    if (done) return done > 0;
    MonoImage *core = M.image_loaded("UnityEngine.CoreModule");
    if (!core) core = M.image_loaded("UnityEngine");
    if (!core) return 0;
    k_obj = M.class_from_name(core, "UnityEngine", "Object");
    k_mb = M.class_from_name(core, "UnityEngine", "MonoBehaviour");
    k_tr = M.class_from_name(core, "UnityEngine", "Transform");
    k_time = M.class_from_name(core, "UnityEngine", "Time");
    k_rand = M.class_from_name(core, "UnityEngine", "Random");
    k_sm = M.class_from_name(core, "UnityEngine.SceneManagement", "SceneManager");
    k_scene = M.class_from_name(core, "UnityEngine.SceneManagement", "Scene");
    if (!k_obj || !k_mb || !k_tr || !k_time) { done = -1; fprintf(stderr, "[lockstep] UnityEngine's classes not found: no state hash\n"); return 0; }
    MonoImage *corlib = M.image_loaded("mscorlib");
    if (corlib) { k_delegate = M.class_from_name(corlib, "System", "Delegate"); k_type = M.class_from_name(corlib, "System", "Type"); }
    MonoDomain *dom = M.domain_get();
    t_mb = M.type_get_object(dom, M.class_get_type(k_mb));
    t_tr = M.type_get_object(dom, M.class_get_type(k_tr));
    // 2023.1+: FindObjectsByType(Type, FindObjectsSortMode.None), FindObjectsOfType's sort by instance id left out
    m_find_by = M.method_from_name(k_obj, "FindObjectsByType", 2);
    m_find = M.method_from_name(k_obj, "FindObjectsOfType", 1);
    m_time = M.method_from_name(k_time, "get_time", 0);
    m_fc = M.method_from_name(k_time, "get_frameCount", 0);
    m_ts = M.method_from_name(k_time, "get_timeScale", 0);
    m_ut = M.method_from_name(k_time, "get_unscaledTime", 0);
    m_cdt = M.method_from_name(k_time, "get_captureDeltaTime", 0);
    m_dt = M.method_from_name(k_time, "get_deltaTime", 0);
    m_rand_state = k_rand ? M.method_from_name(k_rand, "get_state", 0) : NULL;
    m_active_scene = k_sm ? M.method_from_name(k_sm, "GetActiveScene", 0) : NULL;
    m_scene_count = k_sm ? M.method_from_name(k_sm, "get_sceneCount", 0) : NULL;
    m_scene_name = k_scene ? M.method_from_name(k_scene, "get_name", 0) : NULL;
    MonoClassField *cp = M.class_get_field_from_name(k_obj, "m_CachedPtr");
    g_cached_ptr_off = cp ? M.field_get_offset(cp) : 0;
    m_transform = method(k_mb, "get_transform", 0);
    m_enabled = method(k_mb, "get_enabled", 0);
    ic_transform = (obj_icall)icall_of(m_transform);
    ic_enabled = (bool_icall)icall_of(m_enabled);
    static const char *const trs[3][3] = { { "get_localPosition_Injected", "INTERNAL_get_localPosition", "get_localPosition" },
                                           { "get_localRotation_Injected", "INTERNAL_get_localRotation", "get_localRotation" },
                                           { "get_localScale_Injected", "INTERNAL_get_localScale", "get_localScale" } };
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 2 && !ic_trs[i]; j++) ic_trs[i] = (vec_icall)icall_of(M.method_from_name(k_tr, trs[i][j], 1));
        m_trs[i] = M.method_from_name(k_tr, trs[i][2], 0);
    }
    const char *ie = getenv("UA_LOCKSTEP_ICALLS");
    if (!ic_trs[0] && g_cached_ptr_off && !(ie && !strcmp(ie, "0"))) {
        vec_icall v[3];
        for (int i = 0; i < 3; i++) v[i] = (vec_icall)icall_of(M.method_from_name(k_tr, trs[i][0], 2));
        if (v[0] && v[1] && v[2]) {
            for (int i = 0; i < 3; i++) ic_trs[i] = v[i];
            ic_native = 1;
            MonoClass *kb = M.class_get_parent(k_mb);   // Behaviour
            if (kb && !ic_enabled && (ic_enabled = (bool_icall)icall_of(M.method_from_name(kb, "get_enabled_Injected", 1)))) ic_enabled_native = 1;
        }
    }
    if (ie && !strcmp(ie, "0")) { ic_transform = NULL; ic_enabled = NULL; memset(ic_trs, 0, sizeof ic_trs); }
    m_parent = method(k_tr, "get_parent", 0);
    m_name = M.method_from_name(k_obj, "get_name", 0);
    if (!m_find && !m_find_by) { done = -1; fprintf(stderr, "[lockstep] no Object.FindObjectsOfType(Type): no state hash\n"); return 0; }
    fprintf(stderr, "[lockstep] state walk ready: transform %s, enabled %s, local TRS %s%s (UnityEngine.Object's native pointer at +%u)\n",
            ic_transform ? "icall" : "invoke", ic_enabled ? "icall" : "invoke", ic_trs[0] && ic_trs[1] && ic_trs[2] ? "icalls" : "invoke", ic_native ? " (native object)" : "", g_cached_ptr_off);
    done = 1;
    return 1;
}
static MonoObject *call0(MonoMethod *m, void *self)
{
    if (!m) return NULL;
    MonoObject *exc = NULL, *r = M.invoke(m, self, NULL, &exc);
    return exc ? NULL : r;
}
static int alive(MonoObject *o) { return o && (!g_cached_ptr_off || *(void **)((char *)o + g_cached_ptr_off)); }
static MonoObject *transform_of(MonoObject *c) { return ic_transform ? ic_transform(c) : call0(m_transform, c); }
static int enabled_of(MonoObject *c)
{
    if (ic_enabled) return ic_enabled(ic_enabled_native ? *(MonoObject **)((char *)c + g_cached_ptr_off) : c) & 0xff;
    MonoObject *r = call0(m_enabled, c);
    return r ? *(unsigned char *)M.unbox(r) : -1;
}
static void trs_of(MonoObject *t, float v[10])
{
    static const int at[3] = { 0, 3, 7 }, n[3] = { 3, 4, 3 };
    memset(v, 0, 10 * sizeof *v);
    for (int i = 0; i < 3; i++) {
        if (ic_trs[i]) { float tmp[4] = { 0 }; ic_trs[i](ic_native ? *(MonoObject **)((char *)t + g_cached_ptr_off) : t, tmp); memcpy(v + at[i], tmp, n[i] * sizeof(float)); continue; }
        MonoObject *r = call0(m_trs[i], t);
        if (r) memcpy(v + at[i], M.unbox(r), n[i] * sizeof(float));
    }
}

/* ---- hashing ------------------------------------------------------------------------------ */
static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; }
static uint64_t fmix(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33; return k; }
static uint64_t mix_bytes(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = mix(h, w); }
    if (n) { uint64_t w = 0; memcpy(&w, b, n); h = mix(h, w ^ ((uint64_t)n << 56)); }
    return h;
}
static uint64_t str_hash(const char *s) { uint64_t h = 1469598103934665603ULL; for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL; return h; }

enum { K_RAW, K_STR, K_REF, K_STRUCT };
struct plan;
struct pf { uint32_t off; uint8_t kind, type; uint16_t size; struct plan *sub; const char *name; };
struct plan {
    MonoClass *klass;
    char *name;
    uint64_t nh;
    int unity, skip, str, valuetype, blittable, rank, ekind, ign;
    uint32_t vsize, esize;
    struct plan *elem;
    int n;
    struct pf *f;
};
#define PLANS 8192
static struct plan *g_plans[PLANS];
static int g_nplans;
static struct plan *plan_of(MonoClass *k);
static int prim_size(int t)
{
    switch (t) {
    case 0x02: case 0x04: case 0x05: return 1;   // bool, sbyte, byte
    case 0x03: case 0x06: case 0x07: return 2;   // char, short, ushort
    case 0x08: case 0x09: case 0x0c: return 4;   // int, uint, float
    case 0x0a: case 0x0b: case 0x0d: return 8;   // long, ulong, double
    }
    return 0;
}
// fields left out: the hash-derived ones of System.Collections; the real-time stamps of the engine's packages
// (the Input System's device update time follows the input events' times, counted from Unity's real start);
// UA_LOCKSTEP_IGNORE=<class>[.<field>][,...] (a class by its short or full name; a class alone: its own fields;
// what a game keeps of the sound's or another real clock, named in the check's report)
static int ignored(const char *cls_full, const char *cls, const char *fname)
{
    static const char *const builtin[] = { "UnityEngine.InputSystem.InputDevice.m_LastUpdateTimeInternal", NULL };
    char want[384];
    for (int i = 0; fname && builtin[i]; i++) { snprintf(want, sizeof want, "%s.%s", cls_full, fname); if (!strcmp(want, builtin[i])) return 1; }
    if (!g_ignore[0]) return 0;
    for (const char *p = g_ignore; *p; ) {
        const char *e = strchr(p, ','); size_t n = e ? (size_t)(e - p) : strlen(p);
        if ((strlen(cls_full) == n && !strncmp(p, cls_full, n)) || (strlen(cls) == n && !strncmp(p, cls, n))) return 1;   // a whole class
        if (!fname) { if (!e) break; p = e + 1; continue; }
        int a = snprintf(want, sizeof want, "%s.%s", cls_full, fname);
        if ((size_t)a == n && !strncmp(p, want, n)) return 1;
        a = snprintf(want, sizeof want, "%s.%s", cls, fname);
        if ((size_t)a == n && !strncmp(p, want, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}
static int skip_field(const struct plan *p, const char *ns, const char *fname)
{
    (void)p;
    if (strncmp(ns, "System.Collections", 18)) return 0;
    static const char *const hashy[] = { "buckets", "_buckets", "m_buckets", "hashCode", "_hashCode", "next", "_next", NULL };
    for (int i = 0; hashy[i]; i++) if (!strcmp(fname, hashy[i])) return 1;
    return 0;
}
static void add_field(struct plan *p, int *cap, struct pf f)
{
    if (p->n == *cap) { *cap = *cap ? 2 * *cap : 8; p->f = realloc(p->f, *cap * sizeof *p->f); }
    p->f[p->n++] = f;
}
static struct plan *plan_of(MonoClass *k)
{
    unsigned i = (unsigned)(((uintptr_t)k >> 3) * 2654435761u) % PLANS;
    for (unsigned j = 0; j < PLANS; j++, i = (i + 1) % PLANS) {
        if (!g_plans[i]) break;
        if (g_plans[i]->klass == k) return g_plans[i];
    }
    struct plan *p = calloc(1, sizeof *p);
    p->klass = k;
    if (g_nplans < PLANS * 3 / 4) { g_plans[i] = p; g_nplans++; }    // (full: built again each time; slow, still right)
    const char *ns = M.class_get_namespace(k), *nm = M.class_get_name(k);
    size_t ln = strlen(ns) + strlen(nm) + 2;
    p->name = malloc(ln);
    snprintf(p->name, ln, "%s%s%s", ns, *ns ? "." : "", nm);
    p->nh = str_hash(p->name);
    p->ign = ignored(p->name, nm, NULL);   // a whole class in UA_LOCKSTEP_IGNORE: its scripts and their transforms left out
    p->rank = M.class_get_rank(k);
    if (p->rank > 0) {
        MonoClass *e = M.class_get_element_class(k);
        p->esize = (uint32_t)M.array_element_size(k);
        p->elem = plan_of(e);
        p->ekind = p->elem->str ? K_STR : !p->elem->valuetype ? K_REF : p->elem->blittable ? K_RAW : K_STRUCT;
        return p;
    }
    p->str = !strcmp(p->name, "System.String");
    p->unity = k_obj && (k == k_obj || M.class_is_subclass_of(k, k_obj, 0));
    p->skip = (k_delegate && M.class_is_subclass_of(k, k_delegate, 0)) || (k_type && M.class_is_subclass_of(k, k_type, 0)) ||
              !strncmp(p->name, "System.Reflection.", 18) || !strncmp(p->name, "System.Threading.", 17) || !strcmp(p->name, "System.RuntimeType");
    p->valuetype = M.class_is_valuetype(k);
    if (p->valuetype) p->vsize = (uint32_t)M.class_value_size(k, NULL);
    if (p->str) return p;
    int cap = 0, blit = 1;
    for (MonoClass *c = k; c && c != k_obj; c = M.class_get_parent(c)) {
        // a nested class (Dictionary's Entry, HashSet's Slot) has no namespace of its own: its outermost class's
        const char *cns = M.class_get_namespace(c);
        for (MonoClass *o = c; !*cns && M.class_get_nesting_type && (o = M.class_get_nesting_type(o)); ) cns = M.class_get_namespace(o);
        void *it = NULL;
        MonoClassField *fl;
        while ((fl = M.class_get_fields(c, &it))) {
            if (M.field_get_flags(fl) & 0x50) continue;          // static, literal
            MonoType *t = M.field_get_type(fl);
            int tt = M.type_get_type(t);
            const char *fname = M.field_get_name(fl);
            if (skip_field(p, cns, fname)) { blit = 0; continue; }
            {
                const char *cn = M.class_get_name(c), *cn_ns = M.class_get_namespace(c);
                char full[256]; snprintf(full, sizeof full, "%s%s%s", cn_ns, *cn_ns ? "." : "", cn);
                if (ignored(full, cn, fname)) { blit = 0; continue; }
            }
            struct pf f = { .off = M.field_get_offset(fl), .type = (uint8_t)tt, .name = fname };
            if (p->valuetype) f.off -= HDR;
            int ps = prim_size(tt);
            if (ps) { f.kind = K_RAW; f.size = (uint16_t)ps; }
            else if (tt == 0x0e) { f.kind = K_STR; blit = 0; }
            else if (tt == 0x18 || tt == 0x19 || tt == 0x0f || tt == 0x1b || tt == 0x13 || tt == 0x1e || tt == 0x16)
            { blit = 0; continue; }   // IntPtr, UIntPtr, pointers; open generic parameters, typed references
            else if (tt == 0x11 || tt == 0x15) {
                MonoClass *fc = M.class_from_mono_type(t);
                if (fc && M.class_is_valuetype(fc) && !strcmp(M.class_get_name(fc), "Guid") && !strcmp(M.class_get_namespace(fc), "System"))
                { blit = 0; continue; }   // System.Guid: an identity (Guid.NewGuid: random each run), not game play
                if (fc && M.class_is_valuetype(fc)) {
                    struct plan *sp = plan_of(fc);
                    if (sp->blittable) { f.kind = K_RAW; f.size = (uint16_t)sp->vsize; f.sub = sp; }
                    else { f.kind = K_STRUCT; f.sub = sp; blit = 0; }
                } else { f.kind = K_REF; blit = 0; }
            }
            else { f.kind = K_REF; blit = 0; }
            add_field(p, &cap, f);
        }
    }
    p->blittable = p->valuetype && blit;
    return p;
}

// one script's walk: the objects followed so far (a small open-addressed set, their order numbers kept)
#define SEEN 512
struct walk { int nodes, nseen; void *seen[SEEN]; int order[SEEN]; short slot[SEEN / 2]; FILE *out; char path[640]; };
static int seen_find(struct walk *w, void *o, int add)     // its order number, or -1 (added when add)
{
    unsigned i = (unsigned)(((uintptr_t)o >> 3) * 2654435761u) % SEEN;
    for (int k = 0; k < SEEN; k++, i = (i + 1) % SEEN) {
        if (w->seen[i] == o) return w->order[i];
        if (!w->seen[i]) { if (add && w->nseen < SEEN / 2) { w->seen[i] = o; w->slot[w->nseen] = (short)i; w->order[i] = w->nseen++; } return -1; }
    }
    return -1;
}
static uint64_t g_walk_nodes;
static uint64_t h_ref(MonoObject *o, int depth, struct walk *w);
static void put(struct walk *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void put(struct walk *w, const char *fmt, ...)
{
    if (!w->out) return;
    va_list a; va_start(a, fmt);
    fprintf(w->out, "  %s ", w->path);
    vfprintf(w->out, fmt, a);
    fputc('\n', w->out);
    va_end(a);
}
static void put_raw(struct walk *w, const struct pf *f, const unsigned char *a)
{
    if (!w->out) return;
    uint64_t v = 0;
    switch (f->type) {
    case 0x0c: { float x; memcpy(&x, a, 4); put(w, "%.9g (%08x)", x, fbits(x)); return; }
    case 0x0d: { double x; memcpy(&x, a, 8); memcpy(&v, a, 8); put(w, "%.17g (%016llx)", x, (unsigned long long)v); return; }
    case 0x02: put(w, "%s", *a ? "true" : "false"); return;
    case 0x04: put(w, "%d", *(const signed char *)a); return;
    case 0x06: { int16_t x; memcpy(&x, a, 2); put(w, "%d", x); return; }
    case 0x08: { int32_t x; memcpy(&x, a, 4); put(w, "%d", x); return; }
    case 0x0a: { int64_t x; memcpy(&x, a, 8); put(w, "%lld", (long long)x); return; }
    }
    if (f->size <= 8) { memcpy(&v, a, f->size); put(w, "%llu", (unsigned long long)v); return; }
    char hex[2 * 64 + 4] = ""; size_t n = f->size > 64 ? 64 : f->size;
    for (size_t i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", a[i]);
    put(w, "%s%s", hex, f->size > 64 ? "..." : "");
}
static uint64_t h_string(MonoObject *s, struct walk *w)
{
    if (!s) { put(w, "null"); return 0x5a5a; }
    int32_t len = *(int32_t *)((char *)s + HDR);
    const uint16_t *ch = (const uint16_t *)((char *)s + HDR + 4);
    if (w->out) {
        char buf[256]; int n = 0;
        for (int32_t i = 0; i < len && n < 250; i++) buf[n++] = ch[i] >= 32 && ch[i] < 127 && ch[i] != '"' ? (char)ch[i] : '?';
        buf[n] = 0;
        put(w, "\"%s\"%s", buf, len > 250 ? "..." : "");
    }
    return mix_bytes(mix(0x57, (uint64_t)len), ch, (size_t)len * 2);
}
static uint64_t h_fields(char *base, struct plan *p, int depth, struct walk *w)
{
    uint64_t h = p->nh;
    size_t pl = strlen(w->path);
    for (int i = 0; i < p->n; i++) {
        const struct pf *f = &p->f[i];
        char *a = base + f->off;
        if (w->out) snprintf(w->path + pl, sizeof w->path - pl, "%s%s", pl ? "." : "", f->name);
        switch (f->kind) {
        case K_RAW:
            h = mix_bytes(h, a, f->size);
            if (w->out && f->sub) h_fields(a, f->sub, depth, w);      // a struct of numbers: printed field by field
            else put_raw(w, f, (unsigned char *)a);
            break;
        case K_STR: h = mix(h, h_string(*(MonoObject **)a, w)); break;
        case K_STRUCT: h = mix(h, h_fields(a, f->sub, depth, w)); break;
        case K_REF: h = mix(h, h_ref(*(MonoObject **)a, depth, w)); break;
        }
    }
    w->path[pl] = 0;
    return h;
}
static uint64_t h_array(MonoArray *arr, struct plan *p, int depth, struct walk *w)
{
    uintptr_t len = arr_len(arr);
    uint64_t h = mix(p->nh, len);
    if (w->out) put(w, "<%s %lu>", p->name, (unsigned long)len);
    if (!len) return h;
    char *data = M.array_addr(arr, (int)p->esize, 0);
    size_t pl = strlen(w->path);
    if (p->ekind == K_RAW) {
        size_t bytes = (size_t)len * p->esize;
        if (bytes > 65536) bytes = 65536;
        h = mix_bytes(h, data, bytes);
        if (w->out) {
            struct pf f = { .size = (uint16_t)p->esize, .type = p->elem->n == 1 ? p->elem->f[0].type : 0 };
            for (uintptr_t i = 0; i < len && i < (uintptr_t)g_elems; i++) {
                snprintf(w->path + pl, sizeof w->path - pl, "[%lu]", (unsigned long)i);
                put_raw(w, &f, (unsigned char *)data + i * p->esize);
            }
            w->path[pl] = 0;
        }
        return h;
    }
    uintptr_t n = len < (uintptr_t)g_elems ? len : (uintptr_t)g_elems;
    for (uintptr_t i = 0; i < n; i++) {
        if (w->out) snprintf(w->path + pl, sizeof w->path - pl, "[%lu]", (unsigned long)i);
        char *e = data + i * p->esize;
        if (p->ekind == K_STR) h = mix(h, h_string(*(MonoObject **)e, w));
        else if (p->ekind == K_REF) h = mix(h, h_ref(*(MonoObject **)e, depth, w));
        else h = mix(h, h_fields(e, p->elem, depth, w));
    }
    w->path[pl] = 0;
    return h;
}
static uint64_t h_ref(MonoObject *o, int depth, struct walk *w)
{
    if (!o) { put(w, "null"); return 0x6b6b; }
    struct plan *p = plan_of(M.object_get_class(o));
    if (p->str) return h_string(o, w);
    if (p->unity) { int a = alive(o); put(w, "<%s %s>", p->name, a ? "alive" : "destroyed"); return mix(p->nh, (uint64_t)a); }
    if (p->skip || depth <= 0 || w->nodes >= 256) { put(w, "<%s ...>", p->name); return mix(p->nh, 0xd0); }
    int seen = seen_find(w, o, 1);
    if (seen >= 0) { put(w, "<%s seen %d>", p->name, seen); return mix(p->nh, 0x5ee0 + (uint64_t)seen); }
    w->nodes++;
    g_walk_nodes++;
    if (p->rank) return h_array(o, p, depth - 1, w);
    if (w->out) put(w, "<%s>", p->name);
    return mix(p->nh, h_fields(p->valuetype ? (char *)o + HDR : (char *)o, p, depth - 1, w));
}

/* ---- a frame's state ---------------------------------------------------------------------- */
struct cls { struct plan *p; uint64_t sum, last_sum; int count, last_count; long seen; };
static struct cls *g_cls;
static int g_ncls, g_capcls;
static FILE *g_hash;
static char g_scene[256];
static int g_scenes = -1;
static double g_hash_ms, g_wait_ms;
static long g_hashed;
static struct cls *cls_of(struct plan *p)
{
    for (int i = 0; i < g_ncls; i++) if (g_cls[i].p == p) return &g_cls[i];
    if (g_ncls == g_capcls) { g_capcls = g_capcls ? 2 * g_capcls : 256; g_cls = realloc(g_cls, g_capcls * sizeof *g_cls); }
    struct cls *c = &g_cls[g_ncls++];
    memset(c, 0, sizeof *c);
    c->p = p;
    return c;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void path_of(MonoObject *t, char *out, size_t n)
{
    char parts[16][96]; int np = 0;
    for (MonoObject *x = t; x && np < 16; x = call0(m_parent, x)) {
        char nm[64] = "?";
        MonoObject *s = call0(m_name, x);
        if (s) { char *u = M.string_to_utf8(s); if (u) { snprintf(nm, sizeof nm, "%s", u); M.free(u); } }
        // (no sibling index: one object destroyed a frame earlier would shift its siblings' keys; objects of one
        // name are told apart by their contents, lockstep diff)
        snprintf(parts[np++], sizeof parts[0], "%s", nm);
    }
    out[0] = 0;
    size_t l = 0;
    for (int i = np - 1; i >= 0 && l + 2 < n; i--) l += (size_t)snprintf(out + l, n - l, "/%s", parts[i]);
}
static MonoArray *find_all(MonoObject *type)
{
    int none = 0;
    void *args[2] = { type, &none };
    MonoObject *exc = NULL;
    MonoArray *a = M.invoke(m_find_by ? m_find_by : m_find, NULL, args, &exc);
    return exc ? NULL : a;
}
static void nudge(MonoArray *beh, uintptr_t nb)
{
    for (uintptr_t i = 0; i < nb; i++) {
        MonoObject *o = *(MonoObject **)M.array_addr(beh, sizeof(void *), i);
        struct plan *p = o ? plan_of(M.object_get_class(o)) : NULL;
        if (!p) continue;
        const char *shortn = strrchr(p->name, '.');
        if (strcmp(p->name, g_nudge_class) && !(shortn && !strcmp(shortn + 1, g_nudge_class))) continue;
        for (int j = 0; j < p->n; j++) {
            const struct pf *f = &p->f[j];
            if (strcmp(f->name, g_nudge_field) || f->kind != K_RAW) continue;
            char *a = (char *)o + f->off;
            if (f->type == 0x0c) { float x; memcpy(&x, a, 4); x += (float)g_nudge_delta; memcpy(a, &x, 4); }
            else if (f->type == 0x0d) { double x; memcpy(&x, a, 8); x += g_nudge_delta; memcpy(a, &x, 8); }
            else if (f->type == 0x08 || f->type == 0x09) { int32_t x; memcpy(&x, a, 4); x += (int32_t)g_nudge_delta; memcpy(a, &x, 4); }
            else if (f->type == 0x0a || f->type == 0x0b) { int64_t x; memcpy(&x, a, 8); x += (int64_t)g_nudge_delta; memcpy(a, &x, 8); }
            else continue;
            fprintf(stderr, "[lockstep] NUDGE after frame %ld: %s.%s += %g (a test aid that changes the game)\n", g_frame, p->name, f->name, g_nudge_delta);
        }
    }
}
static void hash_frame(long frame)
{
    double t0 = now_ms();
    FILE *dump = NULL;
    for (int i = 0; i < g_ndump; i++)
        if (g_dump[i] == frame) {
            char dp[600]; snprintf(dp, sizeof dp, "%s.%ld.dump", g_hash_path, frame);
            dump = fopen(dp, "w");
            if (dump) fprintf(dump, "# lockstep dump frame %ld\n", frame);
        }
    uint64_t all = 0, tr_sum = 0, beh_sum = 0;
    uint32_t tbits = 0;
    MonoObject *r = call0(m_time, NULL);
    if (r) memcpy(&tbits, M.unbox(r), 4);
    uint32_t fc = 0, tsb = 0, utb = 0;   // Time.frameCount, timeScale, unscaledTime
    if ((r = call0(m_fc, NULL))) memcpy(&fc, M.unbox(r), 4);
    if ((r = call0(m_ts, NULL))) memcpy(&tsb, M.unbox(r), 4);
    if ((r = call0(m_ut, NULL))) memcpy(&utb, M.unbox(r), 4);
    uint32_t cdtb = 0, dtb = 0;   // (logged only) Time.captureDeltaTime, deltaTime
    if ((r = call0(m_cdt, NULL))) memcpy(&cdtb, M.unbox(r), 4);
    if ((r = call0(m_dt, NULL))) memcpy(&dtb, M.unbox(r), 4);
    uint64_t rs = 0;
    r = call0(m_rand_state, NULL);
    if (r) rs = mix_bytes(0, M.unbox(r), 16);
    if (dump) fprintf(dump, "O ~time\n  Time.time %08x\n  Time.frameCount %u\n  Time.timeScale %08x\n  Time.unscaledTime %08x\n  Random.state %016llx\n",
                      tbits, fc, tsb, utb, (unsigned long long)rs);
    // the active scene and how many are loaded
    if (m_active_scene) {
        MonoObject *sc = call0(m_active_scene, NULL), *cnt = call0(m_scene_count, NULL);
        char nm[256] = "?";
        MonoObject *s = sc && m_scene_name ? call0(m_scene_name, M.unbox(sc)) : NULL;
        if (s) { char *u = M.string_to_utf8(s); if (u) { snprintf(nm, sizeof nm, "%s", u); M.free(u); } }
        int n = cnt ? *(int *)M.unbox(cnt) : -1;
        if (strcmp(nm, g_scene) || n != g_scenes) {
            snprintf(g_scene, sizeof g_scene, "%s", nm); g_scenes = n;
            fprintf(g_hash, "S %ld %s %d\n", frame, nm, n);
        }
    }
    // the scripts of a class in UA_LOCKSTEP_IGNORE, and their transforms, are left out (an object whose lifetime the
    // checker cannot fix: a particle effect's that destroys itself when its particles are gone)
    MonoArray *beh = find_all(t_mb);
    uintptr_t nb = beh ? arr_len(beh) : 0, nb_seen = 0, nt_seen = 0;
    MonoObject *excl[256]; int nexcl = 0;
    for (uintptr_t i = 0; i < nb; i++) {
        MonoObject *o = *(MonoObject **)M.array_addr(beh, sizeof(void *), i);
        if (alive(o) && plan_of(M.object_get_class(o))->ign && nexcl < 256) excl[nexcl++] = transform_of(o);
    }
    // transforms
    MonoArray *trs = find_all(t_tr);
    uintptr_t nt = trs ? arr_len(trs) : 0;
    for (uintptr_t i = 0; i < nt; i++) {
        MonoObject *t = *(MonoObject **)M.array_addr(trs, sizeof(void *), i);
        if (!alive(t)) continue;
        int skip = 0;
        for (int k = 0; k < nexcl && !skip; k++) skip = excl[k] == t;
        if (skip) continue;
        nt_seen++;
        float v[10]; trs_of(t, v);
        tr_sum += fmix(mix_bytes(0x7472, v, sizeof v));
        if (dump) {
            char path[512] = "";
            path_of(t, path, sizeof path);
            fprintf(dump, "O %s|~transform\n  localPosition %.9g %.9g %.9g\n  localRotation %.9g %.9g %.9g %.9g\n  localScale %.9g %.9g %.9g\n",
                    path, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
        }
    }
    // scripts
    static struct walk w;
    for (int k = 0; k < w.nseen; k++) w.seen[w.slot[k]] = NULL;
    w.nseen = 0;
    for (uintptr_t i = 0; i < nb; i++) {
        MonoObject *o = *(MonoObject **)M.array_addr(beh, sizeof(void *), i);
        if (!alive(o)) continue;
        struct plan *p = plan_of(M.object_get_class(o));
        if (p->ign) continue;
        nb_seen++;
        for (int k = 0; k < w.nseen; k++) w.seen[w.slot[k]] = NULL;   // (the last script's, cleared)
        w.nodes = 0; w.nseen = 0; w.out = dump; w.path[0] = 0;
        seen_find(&w, o, 1);
        MonoObject *t = transform_of(o);
        float v[10] = { 0 };
        if (t && alive(t)) trs_of(t, v);
        int en = enabled_of(o);
        if (dump) {
            char path[512] = "";
            if (t) path_of(t, path, sizeof path);
            fprintf(dump, "O %s|%s\n  enabled %d\n  localPosition %.9g %.9g %.9g\n  localRotation %.9g %.9g %.9g %.9g\n  localScale %.9g %.9g %.9g\n",
                    path, p->name, en, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
        }
        uint64_t h = mix(mix_bytes(p->nh, v, sizeof v), (uint64_t)en);
        h = mix(h, h_fields((char *)o, p, g_depth, &w));
        uint64_t fh = fmix(h);
        struct cls *c = cls_of(p);
        if (c->seen != frame) { c->seen = frame; c->sum = 0; c->count = 0; }
        c->sum += fh; c->count++;
        beh_sum += fh;
    }
    for (int i = 0; i < g_ncls; i++) {
        struct cls *c = &g_cls[i];
        if (c->seen != frame) { c->sum = 0; c->count = 0; }
        if (c->sum != c->last_sum || c->count != c->last_count)
            fprintf(g_hash, "C %ld %s %d %016llx\n", frame, c->p->name, c->count, (unsigned long long)c->sum);
        c->last_sum = c->sum; c->last_count = c->count;
    }
    nb = nb_seen; nt = nt_seen;
    all = mix(mix(mix(mix(mix(mix(0, tbits), rs), tr_sum), beh_sum), ((uint64_t)nb << 32) | nt), ((uint64_t)fc << 32) | tsb);
    fprintf(g_hash, "F %ld %016llx t=%08x fc=%u ts=%08x ut=%08x dt=%08x cdt=%08x rs=%016llx nb=%lu nt=%lu tr=%016llx\n", frame, (unsigned long long)all, tbits, fc, tsb,
            utb, dtb, cdtb, (unsigned long long)rs, (unsigned long)nb, (unsigned long)nt, (unsigned long long)tr_sum);
    if (dump) { fclose(dump); fprintf(stderr, "[lockstep] frame %ld dumped (%s.%ld.dump)\n", frame, g_hash_path, frame); }
    if (frame == g_nudge_frame && beh) nudge(beh, arr_len(beh));
    g_hash_ms += now_ms() - t0;
    g_hashed++;
}

/* ---- the loading threads ------------------------------------------------------------------ */
static int g_loaders[16], g_nloaders;
static long g_loaders_scan = -1000;
static void scan_loaders(void)
{
    g_nloaders = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_nloaders < 16) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[300], comm[32] = "";
        snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        // Unity's loading threads (waiting for FMOD's mixer threads too was tried: Hollow Knight's sound-recycled
        // objects parted runs more often, not less)
        if (fgets(comm, sizeof comm, f) && !strncmp(comm, "Loading.", 8)) g_loaders[g_nloaders++] = atoi(e->d_name);
        fclose(f);
    }
    closedir(d);
}
static int loaders_busy(void)
{
    for (int i = 0; i < g_nloaders; i++) {
        char p[64], buf[256];
        snprintf(p, sizeof p, "/proc/self/task/%d/stat", g_loaders[i]);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        char *c = strrchr(buf, ')');
        if (c && (c[2] == 'R' || c[2] == 'D')) return 1;
    }
    return 0;
}
static void settle(long frame)
{
    if (!g_settle_ms) return;
    // Unity starts its loading threads when it first needs them: looked for every 5 frames (every frame until two are known)
    if (g_nloaders < 2 || frame - g_loaders_scan >= 5) { scan_loaders(); g_loaders_scan = frame; }
    if (!g_nloaders) return;
    double t0 = now_ms();
    int idle = 0;
    while (idle < 2 && now_ms() - t0 < g_settle_ms) {
        if (loaders_busy()) idle = 0; else idle++;
        if (idle < 2) { struct timespec ts = { 0, 200000 }; nanosleep(&ts, NULL); }
    }
    double ms = now_ms() - t0;
    g_wait_ms += ms;
    if (ms > 1 && g_hash) fprintf(g_hash, "W %ld %.1f\n", frame, ms);
}

/* ---- each frame --------------------------------------------------------------------------- */
static int g_started, g_audio;
static long g_start_frame = -1;
static void start_clock(long frame)
{
    float step = 1.f / (float)g_fps;
    MonoMethod *m = M.method_from_name(k_time, "set_captureDeltaTime", 1);
    void *args[1];
    MonoObject *exc = NULL;
    int fr = g_fps;
    if (m) { args[0] = &step; M.invoke(m, NULL, args, &exc); }
    else if ((m = M.method_from_name(k_time, "set_captureFramerate", 1))) { args[0] = &fr; M.invoke(m, NULL, args, &exc); }
    MonoMethod *seed = k_rand ? M.method_from_name(k_rand, "InitState", 1) : NULL;
    MonoObject *exc2 = NULL;
    if (seed) { args[0] = &g_seed; M.invoke(seed, NULL, args, &exc2); }
    int delta = M.method_from_name(k_time, "set_captureDeltaTime", 1) != NULL;
    if (!m) fprintf(stderr, "[lockstep] frame %ld: no Time.captureDeltaTime or captureFramerate: the game's clock is not fixed\n", frame);
    else fprintf(stderr, "[lockstep] frame %ld: Time.%s = %s%d%s, UnityEngine.Random.InitState(%d)%s\n", frame, delta ? "captureDeltaTime" : "captureFramerate",
                 delta ? "1/" : "", g_fps, exc ? " (threw)" : "", g_seed, !seed ? " not found" : exc2 ? " threw" : "");
    g_start_frame = frame;
}
// the capture step as set, again whenever it is found otherwise (Unity 6000.0 had it back at 0 by the next frames,
// a scene load in between): the frames before that were timed by the real clock
static int g_reapplied;
static void keep_clock(long frame)
{
    MonoMethod *get = M.method_from_name(k_time, "get_captureDeltaTime", 0);
    if (!get) return;
    MonoObject *r = call0(get, NULL);
    float cur = r ? *(float *)M.unbox(r) : 0, step = 1.f / (float)g_fps;
    if (cur == step) return;
    MonoMethod *set = M.method_from_name(k_time, "set_captureDeltaTime", 1);
    void *args[1] = { &step };
    MonoObject *exc = NULL;
    if (set) M.invoke(set, NULL, args, &exc);
    if (g_reapplied++ < 5) fprintf(stderr, "[lockstep] frame %ld: Time.captureDeltaTime was %g: set to 1/%d again\n", frame, cur, g_fps);
}
// before the Android libraries are loaded: their constructors and Unity's start-up take their time base from the
// virtual clock too (Time.unscaledTime, the Input System's times count from it)
void lockstep_early(void)
{
    cfg();
    if (g_mode == OFF || g_base_ns) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    g_base_ns = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    const char *uc = getenv("UA_LOCKSTEP_UNITY_CLOCK");
    if (uc && !strcmp(uc, "0")) g_unity_clock = 0;
    if (g_unity_clock) so_set_clock_hook(unity_clock);
}
void lockstep_init(void)
{
    cfg();
    if (g_mode == OFF) return;
    lockstep_early();
    g_step_us = (1000000 + g_fps / 2) / g_fps;
    if (g_mode == M_REPLAY) {
        if (!read_recs(g_in_path)) { g_mode = OFF; return; }
        if (!g_frames_max && g_rec_end > 0) g_frames_max = g_rec_end;
    }
    if (g_mode == M_RECORD) {
        g_epoch_unix = (int64_t)time(NULL);
        g_in = fopen(g_in_path, "w");
        if (!g_in) { fprintf(stderr, "[lockstep] %s: cannot write the record: off\n", g_in_path); g_mode = OFF; return; }
        fprintf(g_in, "# lockstep input 1 fps=%d epoch=%lld\n", g_fps, (long long)g_epoch_unix);
    }
    if (g_hash_path[0]) {
        g_hash = fopen(g_hash_path, "w");
        if (!g_hash) fprintf(stderr, "[lockstep] %s: cannot write the hashes\n", g_hash_path);
        else {
            setvbuf(g_hash, NULL, _IOFBF, 1 << 16);
            fprintf(g_hash, "# lockstep 1 fps=%d seed=%d depth=%d elems=%d\n", g_fps, g_seed, g_depth, g_elems);
        }
    }
    if (g_mode != M_HASH) up_set_input_tap(tap);
    // the silent sound output (ANDROID_SLES_OUTPUT=none, a test run's) takes a frame's time of sound after each frame
    // instead of on the clock: the game's sound clock (AudioSource.time, isPlaying, a clip's end) follows the frames
    const char *ld = getenv("UA_LOCKSTEP_LOADS");
    if (ld && !strcmp(ld, "0")) g_loads = 0;
    const char *au = getenv("UA_LOCKSTEP_AUDIO");
    g_audio = !(au && !strcmp(au, "0"));
    if (g_audio) android_sles_set_paced(1);
    // UA_LOCKSTEP_EVENT_CLOCK=0: the events keep the real uptime
    const char *ec = getenv("UA_LOCKSTEP_EVENT_CLOCK");
    if (!(ec && !strcmp(ec, "0"))) up_set_event_clock(event_clock);
    fprintf(stderr, "[lockstep] %s%s%s: %d fps steps, seed %d%s%s, quit after frame %ld\n",
            g_mode == M_RECORD ? "recording to " : g_mode == M_REPLAY ? "replaying " : "hash only", g_in_path, "",
            g_fps, g_seed, g_hash ? ", hashes to " : ", no hashes", g_hash ? g_hash_path : "", g_frames_max);
    if (g_mode == M_REPLAY) fprintf(stderr, "[lockstep] %d input events in the record, last frame %ld\n", g_nrecs, g_rec_end);
}
void lockstep_frame(void)
{
    if (g_mode == OFF) return;
    long frame = __atomic_add_fetch(&g_frame, 1, __ATOMIC_ACQ_REL);
    if (!g_started && mono_ready() && engine_ready()) { g_started = 1; start_clock(frame); }
    else if (g_started) keep_clock(frame);
    if (g_hash && g_started) hash_frame(frame);
    int n = give_inputs(frame);
    if (n && g_hash) fprintf(g_hash, "I %ld %d\n", frame, n);
    hold_loads(frame);
    if (g_audio) android_sles_pace(frame, g_fps);
    settle(frame);
    if (g_hash && frame % 60 == 0) fflush(g_hash);
    if (g_frames_max && frame == g_frames_max) {
        fprintf(stderr, "[lockstep] frame %ld: done, Unity asked to quit\n", frame);
        lockstep_flush();
        up_finish("lockstep: the last frame");
    }
}
void lockstep_flush(void)
{
    static int done;
    if (g_mode == OFF || done) return;
    done = 1;
    long frame = g_frame;
    if (g_in) { fprintf(g_in, "E %ld\n", frame); fclose(g_in); g_in = NULL; }
    if (g_hash) {
        fprintf(g_hash, "E %ld %.1f %.1f\n", frame, g_hash_ms, g_wait_ms);
        fclose(g_hash); g_hash = NULL;
    }
    fprintf(stderr, "[lockstep] %ld frames (the clock fixed from frame %ld); hashing %.2f ms a frame (%ld frames, %llu objects followed, %d classes), "
            "%.0f ms waited for loading threads, %ld scene loads held (%.0f ms, %d gave up), capture step set again %d times; clock reads: realtime %ld+%ld, TickCount %ld+%ld, Stopwatch %ld, DateTime %ld; pad events dropped %d\n",
            frame, g_start_frame, g_hashed ? g_hash_ms / g_hashed : 0, g_hashed, (unsigned long long)g_walk_nodes, g_nplans, g_wait_ms, g_loads_noted, g_load_wait_ms, g_load_timeouts, g_reapplied,
            g_clock_reads[0], g_clock_reads[1], g_clock_reads[2], g_clock_reads[3], g_clock_reads[4], g_clock_reads[5], g_dropped);
}
