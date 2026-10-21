// Frame pacing of converted desktop data. From 2021.1 Unity's Android players ignore
// QualitySettings.vSyncCount and render at Application.targetFrameRate, whose default (-1) is a
// fixed 30 fps (Unity's documentation: "to conserve battery power"); before 2021.1 they follow
// vSyncCount as desktop players do (2018.4 - 2020.3 games that never set a frame rate run at the
// panel's 60 Hz on the handheld). A 2021.1+ desktop game that leaves its frame rate to vsync ran at
// the monitor's rate on the desktop and runs at 30 fps on the Android player. The host gives it the
// desktop's pacing back:
//   UA_FRAME_RATE=desktop  vSyncCount > 0: the display's rate / vSyncCount (targetFrameRate is
//                          ignored, as on the desktop); 0: targetFrameRate, -1 = the display's rate
//                          (the desktop's "as fast as it can", which vsync caps here)
//                 android  Unity's Android pacing, unchanged (nothing is wrapped)
//                 <n>      n fps whatever the game asks for (a porter's cap for a heavy game)
// The default (ua_host.c): desktop for converted desktop data of Unity 2021.1+, android for imported
// Android builds (pmunity apk), older players and packages that do not name their Unity version.
// How: the targetFrameRate setter and getter icalls are wrapped as libunity registers them
// (mono_add_internal_call / il2cpp_add_internal_call, which it gets through dlsym). The game reads
// back what it set; Unity gets the rate above, checked again on UnityMain after each frame, since a
// quality level change sets vSyncCount without a setter call.
#include "framerate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*add_icall_fn)(const char *name, const void *fn);
typedef void (*set_int_fn)(int);
typedef int (*get_int_fn)(void);

enum { M_ANDROID, M_DESKTOP, M_FIXED };
static int g_mode, g_fixed, g_hz = 60;
static const char *g_mode_name = "android";
static add_icall_fn r_mono_add, r_il2cpp_add;
static set_int_fn r_set_target;
static get_int_fn r_get_target, r_get_vsync;
static int g_game = -1, g_game_known;   // what the game set (Unity's default -1 until read)
static int g_applied = -2;              // what Unity was given (-2: nothing yet)
static int g_logged;

void framerate_init(const char *default_mode, float refresh_hz)
{
    const char *e = getenv("UA_FRAME_RATE");
    const char *m = e && *e ? e : default_mode;
    if (refresh_hz > 0) g_hz = (int)(refresh_hz + 0.5f);
    if (m && !strcmp(m, "desktop")) { g_mode = M_DESKTOP; g_mode_name = "desktop"; }
    else if (m && atoi(m) > 0) { g_mode = M_FIXED; g_fixed = atoi(m); g_mode_name = m; }
    else { g_mode = M_ANDROID; g_mode_name = "android"; }
    if (g_mode != M_ANDROID) fprintf(stderr, "[host] frame rate: %s (display %d Hz)\n", g_mode_name, g_hz);
}

static int effective(void)
{
    if (g_mode == M_FIXED) return g_fixed;
    int vs = r_get_vsync ? r_get_vsync() : 1;
    if (vs > 0) return (g_hz + vs / 2) / vs;
    return g_game > 0 ? g_game : g_hz;
}

static void apply(void)
{
    if (!r_set_target) return;
    if (!g_game_known) { g_game = r_get_target ? r_get_target() : -1; g_game_known = 1; }
    int e = effective();
    if (e == g_applied) return;
    r_set_target(e);
    if (g_logged < 20) {
        g_logged++;
        fprintf(stderr, "[host] frame rate %d fps (the game's targetFrameRate %d, vSyncCount %d; UA_FRAME_RATE=%s)\n",
                e, g_game, r_get_vsync ? r_get_vsync() : -1, g_mode_name);
    }
    g_applied = e;
}

static void w_set_target(int v) { g_game = v; g_game_known = 1; apply(); }
static int w_get_target(void) { return g_applied == -2 && r_get_target ? r_get_target() : g_game; }

static int name_is(const char *name, const char *want)
{
    size_t n = strlen(want);
    return !strncmp(name, want, n) && (name[n] == 0 || name[n] == '(');     // IL2CPP adds the signature
}

static const void *wrap(const char *name, const void *fn)
{
    if (!name || !fn) return fn;
    if (name_is(name, "UnityEngine.Application::set_targetFrameRate")) { r_set_target = (set_int_fn)fn; return (const void *)w_set_target; }
    if (name_is(name, "UnityEngine.Application::get_targetFrameRate")) { r_get_target = (get_int_fn)fn; return (const void *)w_get_target; }
    if (name_is(name, "UnityEngine.QualitySettings::get_vSyncCount")) r_get_vsync = (get_int_fn)fn;
    return fn;
}

static void w_mono_add(const char *name, const void *fn) { r_mono_add(name, wrap(name, fn)); }
static void w_il2cpp_add(const char *name, const void *fn) { r_il2cpp_add(name, wrap(name, fn)); }

void *framerate_dlsym_hook(const char *symbol, void *found)
{
    if (g_mode == M_ANDROID || !found) return found;
    if (!strcmp(symbol, "mono_add_internal_call") && found != (void *)w_mono_add)
    { r_mono_add = (add_icall_fn)found; return (void *)w_mono_add; }
    if (!strcmp(symbol, "il2cpp_add_internal_call") && found != (void *)w_il2cpp_add)
    { r_il2cpp_add = (add_icall_fn)found; return (void *)w_il2cpp_add; }
    return found;
}

void framerate_frame(void *user)
{
    (void)user;
    if (g_mode != M_ANDROID) apply();
}
