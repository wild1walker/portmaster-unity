// A game that draws anything with Unity's immediate-mode GUI (a script with an OnGUI method) loads the
// default skin the first time such a script runs: GUIUtility.BeginGUI -> GUI.set_skin ->
// GUIUtility.GetDefaultSkin -> the built-in GUISkin and its font, read under the lock of Unity's
// loader (PersistentManager). When that first OnGUI comes while a scene is loading in the background,
// the main thread waits for the loading thread to let go of the lock: one frozen frame for as long as
// the loader reads from the card (a 2017.4 game entering its first level from the title, cold page
// cache, on an A53 handheld: 2.3 s in GetDefaultSkin, the menu's fade and its music stopped).
//   UA_GUI_WARM=1   (off by default; a port turns it on) the same call, GUI.DoSetSkin(null), is made
//                   once on UnityMain after the third frame, at the title, when nothing loads. The
//                   skin and its font are then in memory when the game's first OnGUI asks for them.
//                   Logged: "[guiwarm] IMGUI's default skin loaded ahead in <ms> ms".
// What it changes: the built-in skin and font are loaded at launch even if no OnGUI ever runs (about
// 1 MB), and GUI.skin is the default skin before the first OnGUI, as it is at the start of every one.
// A player without the IMGUI module, or whose GUI class has no DoSetSkin(GUISkin), is left alone.
#include "guiwarm.h"
#include "hitches.h"
#include "../loader/so_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int g_on;
static long g_frame;

void guiwarm_init(void)
{
    const char *e = getenv("UA_GUI_WARM");
    g_on = e && *e && *e != '0';
}

void guiwarm_frame(void)
{
    if (!g_on || ++g_frame < 3) return;
    g_on = 0;
    void *h = mono_runtime_lib();
    if (!h) return;
    void *(*image_loaded)(const char *) = (void *(*)(const char *))so_dlsym(h, "mono_image_loaded");
    void *(*class_from_name)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))so_dlsym(h, "mono_class_from_name");
    void *(*get_method)(void *, const char *, int) = (void *(*)(void *, const char *, int))so_dlsym(h, "mono_class_get_method_from_name");
    void *(*invoke)(void *, void *, void **, void **) = (void *(*)(void *, void *, void **, void **))so_dlsym(h, "mono_runtime_invoke");
    if (!image_loaded || !class_from_name || !get_method || !invoke) return;
    // 2017.2+ keeps IMGUI in its own module; before that it is in UnityEngine.dll
    void *img = image_loaded("UnityEngine.IMGUIModule");
    if (!img) img = image_loaded("UnityEngine");
    void *klass = img ? class_from_name(img, "UnityEngine", "GUI") : NULL;
    void *m = klass ? get_method(klass, "DoSetSkin", 1) : NULL;
    if (!m) { fprintf(stderr, "[guiwarm] no UnityEngine.GUI.DoSetSkin(GUISkin) in this player: nothing loaded ahead\n"); return; }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    void *args[1] = { NULL }, *exc = NULL;
    invoke(m, NULL, args, &exc);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    if (exc) fprintf(stderr, "[guiwarm] GUI.DoSetSkin threw (%.1f ms): the default skin loads with the game's first OnGUI\n", ms);
    else fprintf(stderr, "[guiwarm] IMGUI's default skin loaded ahead in %.1f ms\n", ms);
}
