// fmodasync.c: FMOD's once-a-frame update on a thread of its own, overlapped with the rest of UnityMain's frame
// (UA_FMOD_ASYNC=1), and the measurement that shows where it may run (UA_FMOD_COUNT=<s>).
//
// Unity's built-in audio is FMOD Ex 4.44.56 (Unity's own build of it, linked into libunity.so: 6000.0's players and
// 2017.4's alike). Once a frame, in PostLateUpdate.UpdateAudio, AudioManager::Update calls FMOD::System::update on
// UnityMain: the channels' virtual-voice and audibility pass (SystemI::updateChannels, UpdateAudibilities: the
// function with the most load-miss stall cycles of the whole 64-bit frame, docs/arm64-routes.md), the 3D and reverb
// update, sound groups, streams, the channels' end callbacks. AudioManager::Update drops its result (the rows tie
// this to the code read: the instruction after the call reads other state).
//
// FMOD Ex's API takes no lock of its own (FMOD Studio's thread-safe API came later): each public function (135 in
// these players: System::, Channel::, ChannelGroup::, Sound::, DSP::, DSPConnection::, Reverb::) first calls its
// handle's validate (SystemI::validate, ChannelI::validate, ... 7 functions), then the implementation; Unity's own
// code reaches FMOD only through those 135 functions (every call into FMOD's code from outside it in both players'
// libunity.so was listed from their symbol files: the public functions, FMOD's start-up, and the Java AudioTrack
// output's mixer entry, which is FMOD's own mixing thread's work), and AudioManager::Update holds no lock around the
// update. So the update can run elsewhere exactly when nothing that would have come after it starts before it ends:
//
//   - System::update is replaced: called from AudioManager::Update's call site (its return address), it validates
//     the handle on UnityMain, as FMOD's would, hands SystemI::update to the worker (`ua_fmod_update`, one
//     persistent thread, woken by a futex) and returns FMOD_OK; any other caller runs it in place, as before.
//   - each validate is hooked: while an update is queued or running, a thread that enters FMOD's API waits for it
//     to end first (the fast path: one load of the busy word). FMOD's own threads (named `FMOD*`: the mixer, whose
//     DSP callbacks into Unity call DSP::getUserData every block, the stream and non-blocking loaders) and the audio
//     output's (`SLES*`, `SDLAudio*`) never wait: they run with FMOD's internal locks held, which the update may
//     need, and in Unity's own frame they run concurrently with the update anyway. Other threads wait (Unity's
//     loading threads, `Loading.Preload` and `Loading.AsyncRead`, call System::createSound while scenes load): they
//     enter FMOD from outside it holding none of its locks, and the only lock the update's callbacks can reach is the
//     log's (an FMOD error logged). The worker itself (the channels' end callbacks call FMOD from inside the update)
//     never waits.
//   - managed code entered on UnityMain (mono_runtime_invoke / il2cpp_runtime_invoke: libunity's only ways into
//     the game's scripts, docs/mono-arm64.md) waits for it: a script may change the audio objects the update's
//     callbacks read (an AudioSource's channels) without calling FMOD first. Some calls are let through: Unity's own
//     RenderPipelineManager.HandleRenderPipelineChange and Camera.FireOnPreCull / PreRender / PostRender when their
//     IL is the one read and the statics they test say they will change nothing (below, pass_noop): in Hollow
//     Knight the first comes 0.5 - 0.8 ms after every update, the camera's soon after.
//   - the end of the frame (after nativeRender) waits for it, so nothing of the next frame overlaps it.
// Every thread that would have run after the update still does, and sees everything it wrote; the update reads
// what it read (the listener, the channels' 3D attributes and volumes the frame's scripts and AudioManager::Update
// set before it). What moves is only when it runs, against UnityMain's own native work up to the next managed call
// (culling and rendering, which touch no audio state), and against FMOD's mixer, which runs alongside it either way.
// Callbacks the update makes into Unity run on the worker: SoundChannelInstance::FMODChannelCallback (a finished
// channel's Unity object released: Unity's allocator is thread-safe), AudioSource::rolloffCallback (returns 1.0),
// AudioManager::systemCallback (a device list change: FMOD_OS_CheckDriverList is a stub that never reports one on
// Android, so the flag AudioManager::Update reads after the update is never set by it). Unity 2017.4's
// AudioManager::Update calls managed code (AudioSettings.InvokeOnAudioManagerUpdate) right after the update, so
// there the join comes at once: exact, and nothing gained.
//
// UA_FMOD_COUNT=<s> (a measurement; the update stays where it is): the same hooks count, every <s> seconds, what
// enters FMOD in the window between the update's return and the end of the frame, by thread, which managed calls
// UnityMain makes there (the first one named), the calls FMOD's callbacks make during the update, where in the frame
// the update runs and how long it takes: `[fmodasync] count ...` lines; every 30 s the window's FMOD entries by the
// wrapper they came from (libunity offsets). With UA_FMOD_ASYNC=1 the same <s> gives `[fmodasync] async ...` lines:
// the updates handed over, the worker's time, what joined them and how long it waited.
//
// How it is found: pmu-dev unityhooks prints the rows (FMOD::System::update as a 'replace' row, the validates
// hooked, SystemI::update and AudioManager::Update as 'call' rows) and the return address of AudioManager::Update's
// call. A libunity.so without matching rows is left alone (logged).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "fmodasync.h"
#include "unityhooks.h"
void *mono_runtime_lib(void);                           // hitches.c
void *so_dlsym(void *handle, const char *symbol);

enum { ISA_ARM = 'a', ISA_THUMB = 't', ISA_A64 = 'x' };
enum { NVAL = 7 };                    // the validates: 0 SystemI (also called by the replacement), then the others

struct fa_player {
    struct uh_row upd;                // FMOD::System::update (replaced whole)
    struct uh_row sysi_upd;           // FMOD::SystemI::update (called: the update itself)
    struct uh_row am_upd;             // AudioManager::Update (checked: the caller whose result is dropped)
    uint32_t am_ret;                  // where its call of System::update returns (a Thumb one with bit 0)
    struct uh_row val[NVAL];          // the validates (hooked)
};

static const struct fa_player g_players[] = {
    {
        { "6000.0.61f1 armeabi-v7a", "FMOD::System::update", 0x1257d3e, 0x1a, ISA_THUMB, 0x6dd1f3df13ec02a1ull },
        { "6000.0.61f1 armeabi-v7a", "FMOD::SystemI::update", 0x1211650, 0x1ec, ISA_THUMB, 0xd5e3604e0f8639d7ull },
        { "6000.0.61f1 armeabi-v7a", "AudioManager::Update", 0x755658, 0x178, ISA_THUMB, 0x13337b06dbf51b80ull },
        0x755791,
        {
            { "6000.0.61f1 armeabi-v7a", "FMOD::SystemI::validate", 0x120a854, 0x40, ISA_THUMB, 0xa16ffd0206b34525ull },
            { "6000.0.61f1 armeabi-v7a", "FMOD::ChannelI::validate", 0x11fea5c, 0x76, ISA_THUMB, 0xbc7ac53321804f4cull },
            { "6000.0.61f1 armeabi-v7a", "FMOD::ChannelGroupI::validate", 0x1202f36, 0x14, ISA_THUMB, 0x464a65d704ad2269ull },
            { "6000.0.61f1 armeabi-v7a", "FMOD::SoundI::validate", 0x12072c2, 0x14, ISA_THUMB, 0xfd688a1cd8a87331ull },
            { "6000.0.61f1 armeabi-v7a", "FMOD::DSPI::validate", 0x124f60a, 0x14, ISA_THUMB, 0xfd688a1cd8a87331ull },
            { "6000.0.61f1 armeabi-v7a", "FMOD::DSPConnectionI::validate", 0x123b998, 0x14, ISA_THUMB, 0xfd688a1cd8a87331ull },
            { "6000.0.61f1 armeabi-v7a", "FMOD::ReverbI::validate", 0x12046c4, 0x14, ISA_THUMB, 0xfd688a1cd8a87331ull },
        },
    },
    {
        { "6000.0.61f1 arm64-v8a", "FMOD::System::update", 0x18b7358, 0x2c, ISA_A64, 0x084ce44b5dd878f7ull },
        { "6000.0.61f1 arm64-v8a", "FMOD::SystemI::update", 0x185ed5c, 0x2c0, ISA_A64, 0x716015ef09f15d6eull },
        { "6000.0.61f1 arm64-v8a", "AudioManager::Update", 0xc2d9d4, 0x1e8, ISA_A64, 0x50c52b4b240e409dull },
        0xc2db64,
        {
            { "6000.0.61f1 arm64-v8a", "FMOD::SystemI::validate", 0x1856130, 0x64, ISA_A64, 0x399b6aab878d0259ull },
            { "6000.0.61f1 arm64-v8a", "FMOD::ChannelI::validate", 0x1845e14, 0xc8, ISA_A64, 0x152b7f8b34875b9full },
            { "6000.0.61f1 arm64-v8a", "FMOD::ChannelGroupI::validate", 0x184b7f4, 0x28, ISA_A64, 0xb89f0b51fd53d2d4ull },
            { "6000.0.61f1 arm64-v8a", "FMOD::SoundI::validate", 0x18515f4, 0x28, ISA_A64, 0xe9911bf387e9f7a4ull },
            { "6000.0.61f1 arm64-v8a", "FMOD::DSPI::validate", 0x18ac7fc, 0x28, ISA_A64, 0xe9911bf387e9f7a4ull },
            { "6000.0.61f1 arm64-v8a", "FMOD::DSPConnectionI::validate", 0x1893d24, 0x28, ISA_A64, 0xe9911bf387e9f7a4ull },
            { "6000.0.61f1 arm64-v8a", "FMOD::ReverbI::validate", 0x184db14, 0x28, ISA_A64, 0xe9911bf387e9f7a4ull },
        },
    },
    {
        { "2017.4.9f1 armeabi-v7a", "FMOD::System::update", 0xb02084, 0x28, ISA_ARM, 0x132b8b9ae2b55df5ull },
        { "2017.4.9f1 armeabi-v7a", "FMOD::SystemI::update", 0xaadb54, 0x24c, ISA_ARM, 0xf63853f544800d93ull },
        { "2017.4.9f1 armeabi-v7a", "AudioManager::Update", 0x19cf4c, 0x184, ISA_ARM, 0x5faadca47da268ceull },
        0x19d090,
        {
            { "2017.4.9f1 armeabi-v7a", "FMOD::SystemI::validate", 0xaa8820, 0x70, ISA_ARM, 0xa3838bdcab13f3b2ull },
            { "2017.4.9f1 armeabi-v7a", "FMOD::ChannelI::validate", 0xb24484, 0xb4, ISA_ARM, 0x68c0974a3701f5c8ull },
            { "2017.4.9f1 armeabi-v7a", "FMOD::ChannelGroupI::validate", 0xb207b8, 0x20, ISA_ARM, 0xd75a5e048628faadull },
            { "2017.4.9f1 armeabi-v7a", "FMOD::SoundI::validate", 0xaa3b7c, 0x24, ISA_ARM, 0x57342ba2b4baae9eull },
            { "2017.4.9f1 armeabi-v7a", "FMOD::DSPI::validate", 0xaf71b0, 0x24, ISA_ARM, 0x57342ba2b4baae9eull },
            { "2017.4.9f1 armeabi-v7a", "FMOD::DSPConnectionI::validate", 0xade914, 0x24, ISA_ARM, 0x57342ba2b4baae9eull },
            { "2017.4.9f1 armeabi-v7a", "FMOD::ReverbI::validate", 0xaa228c, 0x24, ISA_ARM, 0x57342ba2b4baae9eull },
        },
    },
    {
        { "2017.4.40f1 armeabi-v7a", "FMOD::System::update", 0xb0cc80, 0x2c, ISA_ARM, 0xe83299cb0b403715ull },
        { "2017.4.40f1 armeabi-v7a", "FMOD::SystemI::update", 0xab774c, 0x2c4, ISA_ARM, 0xb532075f13ab2c20ull },
        { "2017.4.40f1 armeabi-v7a", "AudioManager::Update", 0x19d418, 0x184, ISA_ARM, 0x4c971e718c46d443ull },
        0x19d55c,
        {
            { "2017.4.40f1 armeabi-v7a", "FMOD::SystemI::validate", 0xaae6c8, 0x60, ISA_ARM, 0x284a2def6bd337f8ull },
            { "2017.4.40f1 armeabi-v7a", "FMOD::ChannelI::validate", 0xb2ccfc, 0xbc, ISA_ARM, 0x70d02e17ba3973c5ull },
            { "2017.4.40f1 armeabi-v7a", "FMOD::ChannelGroupI::validate", 0xb2bc98, 0x24, ISA_ARM, 0x704a887024078bd7ull },
            { "2017.4.40f1 armeabi-v7a", "FMOD::SoundI::validate", 0xaa9a84, 0x24, ISA_ARM, 0xe926bf594c93fe73ull },
            { "2017.4.40f1 armeabi-v7a", "FMOD::DSPI::validate", 0xb00da0, 0x24, ISA_ARM, 0xe926bf594c93fe73ull },
            { "2017.4.40f1 armeabi-v7a", "FMOD::DSPConnectionI::validate", 0xae6380, 0x24, ISA_ARM, 0xe926bf594c93fe73ull },
            { "2017.4.40f1 armeabi-v7a", "FMOD::ReverbI::validate", 0xaa7f60, 0x24, ISA_ARM, 0xe926bf594c93fe73ull },
        },
    },
    {
        { "2017.4.40f1 arm64-v8a", "FMOD::System::update", 0xa6289c, 0x2c, ISA_A64, 0x078d97aa8020f175ull },
        { "2017.4.40f1 arm64-v8a", "FMOD::SystemI::update", 0xa1699c, 0x2ec, ISA_A64, 0xf1c3dcad80b594a6ull },
        { "2017.4.40f1 arm64-v8a", "AudioManager::Update", 0x2286a4, 0x194, ISA_A64, 0x6e3b55b51362a627ull },
        0x2287f4,
        {
            { "2017.4.40f1 arm64-v8a", "FMOD::SystemI::validate", 0xa0e65c, 0x68, ISA_A64, 0xb12b51e9e5745682ull },
            { "2017.4.40f1 arm64-v8a", "FMOD::ChannelI::validate", 0xa802e8, 0xe0, ISA_A64, 0x91edac01dec428c9ull },
            { "2017.4.40f1 arm64-v8a", "FMOD::ChannelGroupI::validate", 0xa7f24c, 0x28, ISA_A64, 0x93d4df721e43bb58ull },
            { "2017.4.40f1 arm64-v8a", "FMOD::SoundI::validate", 0xa0a14c, 0x28, ISA_A64, 0x7b9ba1ce020ca320ull },
            { "2017.4.40f1 arm64-v8a", "FMOD::DSPI::validate", 0xa57934, 0x28, ISA_A64, 0x7b9ba1ce020ca320ull },
            { "2017.4.40f1 arm64-v8a", "FMOD::DSPConnectionI::validate", 0xa3f918, 0x28, ISA_A64, 0x7b9ba1ce020ca320ull },
            { "2017.4.40f1 arm64-v8a", "FMOD::ReverbI::validate", 0xa087a0, 0x28, ISA_A64, 0x7b9ba1ce020ca320ull },
        },
    },
};

enum { M_OFF, M_COUNT, M_ASYNC };
static int g_mode = -1;
static double g_every;
static uintptr_t g_base, g_ret;
typedef int (*validate_fn)(void *handle, void **out);
static validate_fn g_val_orig[NVAL];
static int (*g_sysi_update)(void *si);
typedef void *(*invoke_fn)(void *method, void *obj, void **params, void **exc);
static invoke_fn g_invoke;

static int fmode(void)
{
    if (g_mode < 0) {
        const char *a = getenv("UA_FMOD_ASYNC"), *c = getenv("UA_FMOD_COUNT"), *off = getenv("UA_UNITY_HOOKS");
        double n = c ? atof(c) : 0;
        g_mode = off && !strcmp(off, "0") ? M_OFF : a && !strcmp(a, "1") ? M_ASYNC : n > 0 ? M_COUNT : M_OFF;
        g_every = n > 0 ? n : 0;
    }
    return g_mode;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static long futex(int *w, int op, int v) { return syscall(SYS_futex, w, op, v, NULL, NULL, 0); }

// --- threads: who is who (cached per thread)
enum { T_UNKNOWN, T_MAIN, T_WORKER, T_FMOD, T_OTHER };
static __thread int t_kind;
static __thread char t_name[16];
static int g_main_tid, g_worker_tid;

static int thread_kind(void)
{
    if (t_kind) return t_kind;
    int me = (int)syscall(SYS_gettid);
    prctl(PR_GET_NAME, t_name, 0, 0, 0);
    if (me == g_worker_tid && g_worker_tid) t_kind = T_WORKER;
    else if (me == g_main_tid && g_main_tid) t_kind = T_MAIN;
    else if (!strncmp(t_name, "FMOD", 4) || !strncmp(t_name, "SLES", 4) || !strncmp(t_name, "SDLAudio", 8)) t_kind = T_FMOD;
    else if (g_main_tid) t_kind = T_OTHER;
    else return T_UNKNOWN;                              // before the first update: not cached yet
    return t_kind;
}

// --- the handover: g_busy is 1 from the update's handover until the worker has run it (the futex word)
static int g_busy, g_waiters, g_worker_ok;
static void *g_job;
static int g_joined;                                    // this update was waited for already (counted once)
enum { J_FMOD_MAIN, J_FMOD_OTHER, J_SCRIPT, J_FRAME, J_NJ };
static const char *const g_jname[J_NJ] = { "FMOD from UnityMain", "FMOD from another thread", "a script", "the frame's end" };

// stats (UA_FMOD_COUNT / the async lines), reset each period
static struct {
    unsigned frames, updates, inplace, joins[J_NJ], waited, passed;
    double upd_s, wait_s, wake_s, at_s, window_s;
    unsigned win_fmod[5], win_script, in_update_calls, first[J_NJ], fmod_threads_busy, first2;
    double first_s[J_NJ], first2_s;
} g_st;
static double g_frame_t0, g_queue_t, g_upd_end_t;
static int g_window;                                    // UA_FMOD_COUNT: after the update's return, until the frame's end
static int g_first_seen, g_first2_seen;
static int g_in_update;                                 // UA_FMOD_COUNT: the update runs (UnityMain)

// the window's FMOD entries by thread name and return address (UA_FMOD_COUNT), kept for the end
enum { NSITE = 64 };
static struct { char name[16]; uintptr_t ra; unsigned n; } g_sites[NSITE];
static pthread_mutex_t g_site_lock = PTHREAD_MUTEX_INITIALIZER;
static void site(const char *name, uintptr_t ra)
{
    pthread_mutex_lock(&g_site_lock);
    for (int i = 0; i < NSITE; i++) {
        if (g_sites[i].n && (g_sites[i].ra != ra || strcmp(g_sites[i].name, name))) continue;
        if (!g_sites[i].n) { snprintf(g_sites[i].name, sizeof g_sites[i].name, "%.15s", name); g_sites[i].ra = ra; }
        g_sites[i].n++;
        break;
    }
    pthread_mutex_unlock(&g_site_lock);
}

// UA_FMOD_COUNT: the managed methods that came first in the window, by method (named at the report)
enum { NMETH = 16 };
static struct { void *m; unsigned n; } g_meth[NMETH];
static const char *(*r_method_name)(void *), *(*r_class_name)(void *), *(*r_class_ns)(void *);
static void *(*r_method_class)(void *);
static void *g_first_method;
static struct { void *m; unsigned n; } g_meth2[NMETH];

static void first_event(int why)
{
    if (g_first_seen) return;
    g_first_seen = 1;
    if (why == J_SCRIPT && g_first_method)
        for (int i = 0; i < NMETH; i++)
            if (!g_meth[i].m || g_meth[i].m == g_first_method) { g_meth[i].m = g_first_method; g_meth[i].n++; break; }
    g_st.first[why]++;
    g_st.first_s[why] += now() - g_upd_end_t;
}

static void join(int why)
{
    if (!__atomic_load_n(&g_busy, __ATOMIC_SEQ_CST)) return;
    if (!__atomic_exchange_n(&g_joined, 1, __ATOMIC_RELAXED)) g_st.joins[why]++;
    double t0 = now();
    __atomic_add_fetch(&g_waiters, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_busy, __ATOMIC_SEQ_CST)) futex(&g_busy, FUTEX_WAIT_PRIVATE, 1);
    __atomic_sub_fetch(&g_waiters, 1, __ATOMIC_SEQ_CST);
    g_st.waited++;
    g_st.wait_s += now() - t0;
}

static void *worker(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "ua_fmod_update", 0, 0, 0);
    __atomic_store_n(&g_worker_tid, (int)syscall(SYS_gettid), __ATOMIC_SEQ_CST);
    t_kind = T_WORKER;
    for (;;) {
        while (!__atomic_load_n(&g_busy, __ATOMIC_SEQ_CST)) futex(&g_busy, FUTEX_WAIT_PRIVATE, 0);
        double t0 = g_every ? now() : 0;
        g_sysi_update(g_job);                           // (its result is dropped by AudioManager::Update)
        if (g_every) { double t1 = now(); g_st.upd_s += t1 - t0; g_st.wake_s += t0 - g_queue_t; }
        __atomic_store_n(&g_busy, 0, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_waiters, __ATOMIC_SEQ_CST)) futex(&g_busy, FUTEX_WAKE_PRIVATE, INT_MAX);
    }
    return NULL;
}

// --- the gate in each validate
static void gate(void *ra)
{
    int k = thread_kind();
    if (k == T_WORKER) return;
    if (g_mode == M_COUNT) {
        if (k == T_MAIN && g_in_update) { g_st.in_update_calls++; return; }
        if (!g_window) return;
        g_st.win_fmod[k]++;
        if (k == T_MAIN) first_event(J_FMOD_MAIN);
        else if (k == T_OTHER) first_event(J_FMOD_OTHER);
        site(t_name[0] ? t_name : "?", (uintptr_t)ra - g_base);
        return;
    }
    if (k == T_FMOD) { g_st.fmod_threads_busy++; return; }       // never waits (above)
    join(k == T_MAIN ? J_FMOD_MAIN : J_FMOD_OTHER);
}
#define VAL(i) static int val_##i(void *h, void **out) \
    { if (g_mode == M_COUNT || __atomic_load_n(&g_busy, __ATOMIC_SEQ_CST)) gate(__builtin_return_address(0)); \
      return g_val_orig[i](h, out); }
VAL(0) VAL(1) VAL(2) VAL(3) VAL(4) VAL(5) VAL(6)
static const validate_fn g_val_hook[NVAL] = { val_0, val_1, val_2, val_3, val_4, val_5, val_6 };

// --- FMOD::System::update, replaced
static int sys_update(void *sys)
{
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    join(thread_kind() == T_MAIN ? J_FMOD_MAIN : J_FMOD_OTHER);  // the last one (never pending here: every FMOD call joined it)
    void *si = NULL;
    int r = g_val_orig[0](sys, &si);
    if (r != 0) return r;
    if (ra != g_ret) { g_st.inplace++; return g_sysi_update(si); }
    if (!g_main_tid) { g_main_tid = (int)syscall(SYS_gettid); t_kind = T_MAIN; }
    double t0 = g_every ? now() : 0;
    if (g_every) g_st.at_s += t0 - g_frame_t0;
    g_st.updates++;
    if (g_mode == M_ASYNC && g_worker_ok) {
        g_job = si;
        g_joined = 0;
        g_queue_t = t0;
        g_upd_end_t = t0;
        __atomic_store_n(&g_busy, 1, __ATOMIC_SEQ_CST);
        futex(&g_busy, FUTEX_WAKE_PRIVATE, INT_MAX);
        return 0;                                       // FMOD_OK (dropped by the caller)
    }
    g_in_update = 1;
    r = g_sysi_update(si);
    g_in_update = 0;
    if (g_every) { g_upd_end_t = now(); g_st.upd_s += g_upd_end_t - t0; }
    if (g_mode == M_COUNT) { g_window = 1; g_first_seen = g_first2_seen = 0; }
    return r;
}

// --- managed code entered (the dlsym hook)
// A few of Unity's own managed methods need no join when they will change nothing. libunity calls them on UnityMain
// as its rendering starts, before any game script in Hollow Knight's frames (UA_FMOD_COUNT names them):
//   RenderPipelineManager.HandleRenderPipelineChange(asset) (6000.0's CoreModule, 39 bytes of IL):
//     if (s_CurrentPipelineAsset != asset || s_CleanUpPipeline) { CleanupRenderPipeline(); s_CurrentPipelineAsset = asset; }
//     a no-op when asset is the same reference as s_CurrentPipelineAsset and s_CleanUpPipeline is false;
//   Camera.FireOnPreCull / FireOnPreRender / FireOnPostRender(cam) (26 bytes each):
//     if (onPreCull != null) onPreCull(cam);   a no-op while the static delegate is null (no script subscribed).
// A method is taken only when its IL is that code (the opcodes compared byte for byte, its field tokens resolved to
// those fields of its own class through Mono's API), and each call only when the statics, read just before it, say
// so: it then reads them and returns (no write, no call, no game code). Any other call joins as every managed call
// does.
static const uint8_t HRPC_IL[39] = {
    0x00, 0x7e, 0, 0, 0, 0, 0x02, 0xfe, 0x01, 0x16, 0xfe, 0x01, 0x0a,          // ldsfld cur; ldarg.0; ceq; ldc.i4.0; ceq; stloc.0
    0x7e, 0, 0, 0, 0, 0x06, 0x60, 0x0b, 0x07, 0x2c, 0x0e,                      // ldsfld cleanup; ldloc.0; or; stloc.1; ldloc.1; brfalse.s
    0x00, 0x28, 0, 0, 0, 0, 0x00, 0x02, 0x80, 0, 0, 0, 0, 0x00, 0x2a };        // call Cleanup; ldarg.0; stsfld cur; ret
static const uint8_t FIRE_IL[26] = {
    0x00, 0x7e, 0, 0, 0, 0, 0x14, 0xfe, 0x03, 0x0a, 0x06, 0x2c, 0x0c,          // ldsfld cb; ldnull; cgt.un; stloc.0; ldloc.0; brfalse.s
    0x7e, 0, 0, 0, 0, 0x02, 0x6f, 0, 0, 0, 0, 0x00, 0x2a };                    // ldsfld cb; ldarg.0; callvirt Invoke; ret
enum { P_HRPC, P_FIRE };
enum { NPASS = 8 };
static struct pass { void *m; int kind; void *vt, *f1, *f2; } g_pass[NPASS];
static int g_npass;
static void *g_notpass[64];                             // methods checked and not taken (each checked once)
static unsigned g_nnot;
static void (*m_static_get)(void *vt, void *field, void *out);
static void *(*m_hdr)(void *), *(*m_field_tok)(void *, uint32_t, void **, void *), *(*m_image)(void *);
static const uint8_t *(*m_code)(void *, uint32_t *, uint32_t *);
static void *(*m_vtable)(void *, void *), *(*m_domain)(void);
static int g_api = -1;

// Mono's API: the 32-bit player's libmono through the loader; the 64-bit Mono route's libmonobdwgc-2.0.so is a host
// library the bridge loaded
static void *mono_api(const char *name)
{
    void *h = mono_runtime_lib(), *p = h ? so_dlsym(h, name) : NULL;
    return p ? p : dlsym(RTLD_DEFAULT, name);
}
static int api(void)
{
    if (g_api < 0) {
        m_hdr = mono_api("mono_method_get_header");
        m_code = (const uint8_t *(*)(void *, uint32_t *, uint32_t *))mono_api("mono_method_header_get_code");
        m_field_tok = (void *(*)(void *, uint32_t, void **, void *))mono_api("mono_field_from_token");
        m_image = mono_api("mono_class_get_image");
        m_vtable = (void *(*)(void *, void *))mono_api("mono_class_vtable");
        m_domain = (void *(*)(void))mono_api("mono_domain_get");
        m_static_get = (void (*)(void *, void *, void *))mono_api("mono_field_static_get_value");
        g_api = m_hdr && m_code && m_field_tok && m_image && m_vtable && m_domain && m_static_get && r_method_class &&
                r_class_name && r_method_name;
        if (!g_api) fprintf(stderr, "[fmodasync] UA_FMOD_ASYNC: no Mono API to read Unity's methods: every managed call "
                                    "waits for the update\n");
    }
    return g_api;
}
// the IL matches tmpl outside the 4-byte tokens at tok[]; the field tokens resolved (in the method's class's image)
static int il_is(void *method, const uint8_t *tmpl, uint32_t n, const int *tok, int ntok, void **fields, int nf)
{
    void *hd = m_hdr(method);
    uint32_t sz = 0, ms = 0;
    const uint8_t *il = hd ? m_code(hd, &sz, &ms) : NULL;
    if (!il || sz != n) return 0;
    for (uint32_t i = 0; i < n; i++) {
        int in_tok = 0;
        for (int k = 0; k < ntok; k++) in_tok |= i >= (uint32_t)tok[k] && i < (uint32_t)tok[k] + 4;
        if (!in_tok && il[i] != tmpl[i]) return 0;
    }
    void *img = m_image(r_method_class(method)), *k = NULL;
    for (int i = 0; i < nf; i++) {
        uint32_t t;
        memcpy(&t, il + tok[i], 4);
        if ((t >> 24) != 0x04 || !(fields[i] = m_field_tok(img, t, &k, NULL))) return 0;    // a FieldDef token
        if (k != r_method_class(method)) return 0;                                        // a field of its own class
    }
    return 1;
}
static struct pass *pass_find(void *method)
{
    for (int i = 0; i < g_npass; i++) if (g_pass[i].m == method) return &g_pass[i];
    for (unsigned i = 0; i < g_nnot && i < 64; i++) if (g_notpass[i] == method) return NULL;
    if (!api()) return NULL;
    const char *mn = r_method_name(method), *cn = r_class_name(r_method_class(method));
    struct pass p = { method, -1, NULL, NULL, NULL };
    void *f[3];
    if (!strcmp(cn, "RenderPipelineManager") && !strcmp(mn, "HandleRenderPipelineChange")) {
        static const int tok[] = { 2, 14, 33, 26 };
        if (il_is(method, HRPC_IL, sizeof HRPC_IL, tok, 4, f, 3) && f[2] == f[0]) { p.kind = P_HRPC; p.f1 = f[0]; p.f2 = f[1]; }
    } else if (!strcmp(cn, "Camera") && (!strcmp(mn, "FireOnPreCull") || !strcmp(mn, "FireOnPreRender") || !strcmp(mn, "FireOnPostRender"))) {
        static const int tok[] = { 2, 14, 20 };
        if (il_is(method, FIRE_IL, sizeof FIRE_IL, tok, 3, f, 2) && f[1] == f[0]) { p.kind = P_FIRE; p.f1 = f[0]; }
    }
    if (p.kind >= 0) p.vt = m_vtable(m_domain(), r_method_class(method));
    if (p.kind < 0 || !p.vt || g_npass >= NPASS) {
        if (g_nnot < 64) g_notpass[g_nnot++] = method;
        if (p.kind >= 0 || !strcmp(mn, "HandleRenderPipelineChange") || !strncmp(mn, "FireOn", 6))
            fprintf(stderr, "[fmodasync] UA_FMOD_ASYNC: %s.%s is not the IL read: its calls wait for the update\n", cn, mn);
        return NULL;
    }
    g_pass[g_npass] = p;
    fprintf(stderr, "[fmodasync] UA_FMOD_ASYNC: %s.%s is the IL read: its calls that change nothing do not wait for the "
                    "update\n", cn, mn);
    return &g_pass[g_npass++];
}
// 1: this call is one of those methods on its no-op path
static int pass_noop(void *method, void **params)
{
    struct pass *p = pass_find(method);
    if (!p) return 0;
    if (p->kind == P_HRPC) {
        void *cur = NULL;
        uint8_t clean = 1;
        if (!params) return 0;
        m_static_get(p->vt, p->f1, &cur);
        m_static_get(p->vt, p->f2, &clean);
        return cur == params[0] && !clean;
    }
    void *cb = (void *)1;
    m_static_get(p->vt, p->f1, &cb);
    return cb == NULL;
}

static void *w_invoke(void *method, void *obj, void **params, void **exc)
{
    if (g_mode == M_COUNT) {
        if (g_window && thread_kind() == T_MAIN) {
            g_st.win_script++; g_first_method = method; first_event(J_SCRIPT);
            // the first other than RenderPipelineManager.HandleRenderPipelineChange (a measurement of the room past it)
            if (!g_first2_seen && !pass_noop(method, params)) {
                g_first2_seen = 1;
                g_st.first2++; g_st.first2_s += now() - g_upd_end_t;
                for (int i = 0; i < NMETH; i++)
                    if (!g_meth2[i].m || g_meth2[i].m == method) { g_meth2[i].m = method; g_meth2[i].n++; break; }
            }
        }
    } else if (__atomic_load_n(&g_busy, __ATOMIC_SEQ_CST) && thread_kind() == T_MAIN) {
        if (pass_noop(method, params)) g_st.passed++;
        else join(J_SCRIPT);
    }
    return g_invoke(method, obj, params, exc);
}

void *fmodasync_dlsym_hook(const char *sym, void *p)
{
    if (!p || fmode() == M_OFF) return p;
    const char *n = !strncmp(sym, "mono_", 5) ? sym + 5 : !strncmp(sym, "il2cpp_", 7) ? sym + 7 : "";
    if (!strcmp(n, "method_get_name")) r_method_name = (const char *(*)(void *))p;
    else if (!strcmp(n, "method_get_class")) r_method_class = (void *(*)(void *))p;
    else if (!strcmp(n, "class_get_name")) r_class_name = (const char *(*)(void *))p;
    else if (!strcmp(n, "class_get_namespace")) r_class_ns = (const char *(*)(void *))p;
    if (!strcmp(sym, "mono_runtime_invoke") || !strcmp(sym, "il2cpp_runtime_invoke")) {
        g_invoke = (invoke_fn)p;
        return (void *)w_invoke;
    }
    return p;
}

// --- each frame (UnityMain, after nativeRender)
static void report(double span)
{
    double f = g_st.frames ? g_st.frames : 1, u = g_st.updates ? g_st.updates : 1;
    if (g_mode == M_COUNT) {
        fprintf(stderr, "[fmodasync] count, last %.0f s: %u frames, %u updates (%u elsewhere), the update %.3f ms, from %.2f ms "
                        "into the frame; window to the frame's end %.2f ms; first in it: FMOD from UnityMain %u (%.2f ms in), "
                        "from another thread %u (%.2f ms), a script %u (%.2f ms), the frame's end %u; in it a frame: FMOD calls "
                        "UnityMain %.1f, other threads %.1f, FMOD's own threads %.1f, scripts %.1f; FMOD calls from the "
                        "update's callbacks %.1f a frame; the first script but Unity's no-op ones %u times, %.2f ms in\n",
                span, g_st.frames, g_st.updates, g_st.inplace, g_st.upd_s / u * 1e3, g_st.at_s / u * 1e3,
                g_st.window_s / u * 1e3,
                g_st.first[J_FMOD_MAIN], g_st.first[J_FMOD_MAIN] ? g_st.first_s[J_FMOD_MAIN] / g_st.first[J_FMOD_MAIN] * 1e3 : 0,
                g_st.first[J_FMOD_OTHER], g_st.first[J_FMOD_OTHER] ? g_st.first_s[J_FMOD_OTHER] / g_st.first[J_FMOD_OTHER] * 1e3 : 0,
                g_st.first[J_SCRIPT], g_st.first[J_SCRIPT] ? g_st.first_s[J_SCRIPT] / g_st.first[J_SCRIPT] * 1e3 : 0,
                g_st.first[J_FRAME], g_st.win_fmod[T_MAIN] / f, g_st.win_fmod[T_OTHER] / f, g_st.win_fmod[T_FMOD] / f,
                g_st.win_script / f, g_st.in_update_calls / f, g_st.first2, g_st.first2 ? g_st.first2_s / g_st.first2 * 1e3 : 0);
    } else {
        fprintf(stderr, "[fmodasync] async, last %.0f s: %u frames, %u updates on the worker (%u elsewhere in place), the update "
                        "%.3f ms there, started %.3f ms after the handover, %.2f ms into the frame; joined by %s %u, %s %u, "
                        "%s %u, %s %u; waited %u times, %.3f ms each; Unity's no-op calls passed %u; FMOD's threads in FMOD meanwhile %u\n",
                span, g_st.frames, g_st.updates, g_st.inplace, g_st.upd_s / u * 1e3, g_st.wake_s / u * 1e3,
                g_st.at_s / u * 1e3, g_jname[0], g_st.joins[0], g_jname[1], g_st.joins[1], g_jname[2], g_st.joins[2],
                g_jname[3], g_st.joins[3], g_st.waited, g_st.waited ? g_st.wait_s / g_st.waited * 1e3 : 0,
                g_st.passed, g_st.fmod_threads_busy);
    }
    memset(&g_st, 0, sizeof g_st);
}

static void sites_report(void)
{
    for (int i = 0; i < NMETH && g_meth[i].m; i++) {
        void *c = r_method_class ? r_method_class(g_meth[i].m) : NULL;
        fprintf(stderr, "[fmodasync] count: first in the window %u times: %s.%s:%s\n", g_meth[i].n,
                c && r_class_ns ? r_class_ns(c) : "?", c && r_class_name ? r_class_name(c) : "?",
                r_method_name ? r_method_name(g_meth[i].m) : "?");
    }
    for (int i = 0; i < NMETH && g_meth2[i].m; i++) {
        void *c = r_method_class ? r_method_class(g_meth2[i].m) : NULL;
        fprintf(stderr, "[fmodasync] count: first but Unity's no-op ones %u times: %s.%s:%s\n", g_meth2[i].n,
                c && r_class_ns ? r_class_ns(c) : "?", c && r_class_name ? r_class_name(c) : "?",
                r_method_name ? r_method_name(g_meth2[i].m) : "?");
    }
    int any = 0;
    for (int i = 0; i < NSITE; i++)
        if (g_sites[i].n) {
            if (!any++) fprintf(stderr, "[fmodasync] count: FMOD entries in the window (thread, return address in libunity, calls):\n");
            fprintf(stderr, "[fmodasync]   %s +0x%lx %u\n", g_sites[i].name, (unsigned long)g_sites[i].ra, g_sites[i].n);
        }
}

void fmodasync_frame(void)
{
    static double next, t_last;
    if (g_mode <= M_OFF || !g_sysi_update) return;
    double t = now();
    if (g_mode == M_ASYNC) join(J_FRAME);
    else if (g_window) {
        first_event(J_FRAME);
        if (g_every) g_st.window_s += t - g_upd_end_t;
        g_window = 0;
    }
    g_frame_t0 = now();
    g_st.frames++;
    if (!g_every) return;
    if (!next) { next = t + g_every; t_last = t; return; }
    if (t < next) return;
    report(t - t_last);
    t_last = t;
    next = t + g_every;
    static int n;
    if (g_mode == M_COUNT && ++n % 6 == 0) sites_report();
}

void fmodasync_on_load(uintptr_t base, size_t size)
{
    if (fmode() == M_OFF) return;
    const struct fa_player *p = NULL;
    for (size_t i = 0; i < sizeof g_players / sizeof *g_players && !p; i++) {
        const struct fa_player *c = &g_players[i];
        int ok = unityhooks_matches(&c->upd, base, size) && unityhooks_matches(&c->sysi_upd, base, size) &&
                 unityhooks_matches(&c->am_upd, base, size);
        for (int k = 0; k < NVAL && ok; k++) ok = unityhooks_matches(&c->val[k], base, size);
        if (ok) p = c;
    }
    const char *what = g_mode == M_ASYNC ? "UA_FMOD_ASYNC" : "UA_FMOD_COUNT";
    if (!p) {
        fprintf(stderr, "[fmodasync] %s: this libunity.so is not one whose FMOD update was read (pmu-dev unityhooks); "
                        "the update stays on UnityMain\n", what);
        g_mode = M_OFF;
        return;
    }
    g_base = base;
    g_ret = base + p->am_ret;
    g_sysi_update = (int (*)(void *))(base + p->sysi_upd.off + (p->sysi_upd.isa == ISA_THUMB));
    for (int k = 0; k < NVAL; k++) {
        // the same function may be listed twice (identical bytes at another offset are another function): each
        // offset is hooked once
        if (unityhooks_hook(base + p->val[k].off, p->val[k].isa, (void *)g_val_hook[k], (void **)&g_val_orig[k]) != 0) {
            fprintf(stderr, "[fmodasync] %s: %s: hook failed; the update stays on UnityMain\n", what, p->val[k].func);
            // the hooks already made only gate (nothing is pending without the replacement)
            g_mode = M_OFF;
            return;
        }
    }
    if (unityhooks_jump(base + p->upd.off, p->upd.isa, (void *)sys_update) != 0) {
        fprintf(stderr, "[fmodasync] %s: FMOD::System::update: patch failed; the update stays on UnityMain\n", what);
        g_mode = M_OFF;
        return;
    }
    if (g_mode == M_ASYNC) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        g_worker_ok = pthread_create(&t, &a, worker, NULL) == 0;
        pthread_attr_destroy(&a);
        if (!g_worker_ok) fprintf(stderr, "[fmodasync] UA_FMOD_ASYNC: no worker thread; the update stays on UnityMain\n");
    }
    fprintf(stderr, "[fmodasync] %s: FMOD::System::update %s (libunity %s, +0x%x; %d entry checks)\n", what,
            g_mode == M_ASYNC ? "runs on the worker ua_fmod_update, overlapped with the rest of UnityMain's frame"
                              : "counted where it runs", p->upd.unity, p->upd.off, NVAL);
}
