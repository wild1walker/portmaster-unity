// framerate.c check: a fake libunity registers the targetFrameRate and vSyncCount icalls through the
// wrapped mono_add_internal_call / il2cpp_add_internal_call; Unity's value is checked after each
// "frame" in each UA_FRAME_RATE mode. Plain C: cc test/test_framerate.c -o /tmp/t && /tmp/t
#include "../framerate.c"

static int64_t video_latch_us;      // the media core's last latched picture, as the test sets it
int64_t um_surface_last_latch_us(void) { return video_latch_us; }
static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

#include <assert.h>

static int unity_target = -1, unity_vsync = 1, registered;
static void u_set_target(int v) { unity_target = v; }
static int u_get_target(void) { return unity_target; }
static int u_get_vsync(void) { return unity_vsync; }
static void (*game_set)(int);
static int (*game_get)(void);
static int (*game_vsync)(void);

static void fake_add(const char *name, const void *fn)
{
    registered++;
    if (strstr(name, "RateX")) { assert(fn == (const void *)u_get_vsync); return; }     // not wrapped
    if (strstr(name, "set_targetFrameRate")) game_set = (void (*)(int))fn;
    else if (strstr(name, "get_targetFrameRate")) game_get = (int (*)(void))fn;
    else if (strstr(name, "get_vSyncCount")) game_vsync = (int (*)(void))fn;
}

// a fresh "process": framerate.c's state, Unity's state, then libunity's registrations
static void start(const char *env, const char *mode, const char *add_sym, int il2cpp)
{
    g_mode = M_ANDROID; g_fixed = 0; g_hz = 60; r_mono_add = r_il2cpp_add = NULL;
    r_set_target = NULL; r_get_target = r_get_vsync = NULL;
    g_game = -1; g_game_known = 0; g_applied = -2; g_logged = 0;
    unity_target = -1; unity_vsync = 1; registered = 0;
    video_latch_us = 0;
    if (env) setenv("UA_FRAME_RATE", env, 1); else unsetenv("UA_FRAME_RATE");
    framerate_init(mode, 59.6f);
    add_icall_fn add = (add_icall_fn)framerate_dlsym_hook(add_sym, (void *)fake_add);
    add(il2cpp ? "UnityEngine.Application::set_targetFrameRate(System.Int32)" : "UnityEngine.Application::set_targetFrameRate", (const void *)u_set_target);
    add(il2cpp ? "UnityEngine.Application::get_targetFrameRate()" : "UnityEngine.Application::get_targetFrameRate", (const void *)u_get_target);
    add(il2cpp ? "UnityEngine.QualitySettings::get_vSyncCount()" : "UnityEngine.QualitySettings::get_vSyncCount", (const void *)u_get_vsync);
    add("UnityEngine.Application::get_targetFrameRateX", (const void *)u_get_vsync);      // not ours
    assert(registered == 4);
}

int main(void)
{
    // android: nothing wrapped, Unity's own pacing
    start(NULL, "android", "mono_add_internal_call", 0);
    assert(game_set == u_set_target && game_get == u_get_target);
    framerate_frame(NULL);
    assert(unity_target == -1);

    // desktop, Mono: a game that never sets a frame rate gets vsync's 60
    start(NULL, "desktop", "mono_add_internal_call", 0);
    assert(game_set != u_set_target && game_vsync == u_get_vsync);
    assert(game_get() == -1);                       // before the first frame: Unity's own value
    framerate_frame(NULL);
    assert(unity_target == 60 && game_get() == -1);
    game_set(-1);                                   // Hollow Knight, frame cap off
    assert(unity_target == 60 && game_get() == -1);
    game_set(30);                                   // vsync on: the desktop ignores targetFrameRate
    assert(unity_target == 60 && game_get() == 30);
    unity_vsync = 0;                                // a quality level without vsync
    framerate_frame(NULL);
    assert(unity_target == 30);
    game_set(-1);                                   // no vsync, no target: as fast as vsync lets it
    assert(unity_target == 60);
    game_set(144);
    assert(unity_target == 144 && game_get() == 144);
    unity_vsync = 2;                                // every second vblank
    framerate_frame(NULL);
    assert(unity_target == 30);

    // desktop, IL2CPP names (with signatures)
    start(NULL, "desktop", "il2cpp_add_internal_call", 1);
    assert(game_set != u_set_target);
    game_set(-1);
    assert(unity_target == 60 && game_get() == -1);

    // UA_FRAME_RATE wins over the default: android on 2021.1+ data, a fixed 45 on an old one
    start("android", "desktop", "mono_add_internal_call", 0);
    assert(game_set == u_set_target);
    start("45", "android", "mono_add_internal_call", 0);
    framerate_frame(NULL);
    assert(unity_target == 45);
    game_set(60);
    assert(unity_target == 45 && game_get() == 60);

    // a video on the screen (a picture latched in the last half second): the display's rate, so Unity
    // 6000's one-picture-in-flight SurfaceTexture cycle can show a 30 fps clip at 30 (Hollow Knight's
    // cinematics: vSyncCount 2); never lower than the game's own rate; a fixed rate stays fixed
    start(NULL, "desktop", "mono_add_internal_call", 0);
    unity_vsync = 2;
    framerate_frame(NULL);
    assert(unity_target == 30);
    video_latch_us = now_us();
    framerate_frame(NULL);
    assert(unity_target == 60 && game_get() == -1);
    video_latch_us = now_us() - 600000;             // the clip ended
    framerate_frame(NULL);
    assert(unity_target == 30);
    unity_vsync = 0;
    game_set(144);
    video_latch_us = now_us();
    framerate_frame(NULL);
    assert(unity_target == 144);
    setenv("UA_VIDEO_RATE", "game", 1);             // the game's rate, video or not
    start(NULL, "desktop", "mono_add_internal_call", 0);
    unity_vsync = 2;
    video_latch_us = now_us();
    framerate_frame(NULL);
    assert(unity_target == 30);
    unsetenv("UA_VIDEO_RATE");
    start("45", "desktop", "mono_add_internal_call", 0);
    video_latch_us = now_us();
    framerate_frame(NULL);
    assert(unity_target == 45);

    // other symbols pass through
    start(NULL, "desktop", "mono_add_internal_call", 0);
    assert(framerate_dlsym_hook("mono_jit_init", (void *)fake_add) == (void *)fake_add);
    puts("test_framerate: ok");
    return 0;
}
