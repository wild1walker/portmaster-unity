// The handheld quality profile at run time (docs/quality.md). The converter's `quality` step lowers
// the quality levels of the data (QualitySettings) and writes the caps it applied into
// android_package.json ("quality": "pixel_lights=1,shadows=1,..."); a game that sets quality
// itself while it runs (an options menu: QualitySettings.antiAliasing = 4, pixelLightCount, ...)
// would undo them. The host wraps those setters and getters as libunity registers them
// (mono_add_internal_call / il2cpp_add_internal_call, as framerate.c does): Unity gets
// min(the game's value, the cap), the game reads back the value it set. After each frame (UnityMain)
// the values Unity holds are checked again, since a quality level change (SetQualityLevel) sets them
// all without setter calls: a value above its cap is taken as the game's and Unity gets the cap.
//   UA_QUALITY=off        no caps (the data keeps the profile the setup chose)
//   UA_QUALITY=<caps>     these caps instead of android_package.json's ("msaa=0,pixel_lights=1")
// Caps: pixel_lights (pixelLightCount), shadows (1 = hard only), shadow_resolution (0 - 3),
// shadow_cascades, msaa (antiAliasing), anisotropic (0 off, 1 per texture), soft_particles (0).
// Only values above a cap change; nothing else of the game is touched.
#include "quality.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*add_icall_fn)(const char *name, const void *fn);
typedef void (*set_int_fn)(int);
typedef int (*get_int_fn)(void);

enum { Q_PIXEL_LIGHTS, Q_SHADOWS, Q_SHADOW_RES, Q_CASCADES, Q_MSAA, Q_ANISO, Q_SOFT_PARTICLES, Q_N };
static const char *const cap_names[Q_N] = {"pixel_lights", "shadows", "shadow_resolution", "shadow_cascades",
                                           "msaa", "anisotropic", "soft_particles"};
static const char *const prop[Q_N] = {"pixelLightCount", "shadows", "shadowResolution", "shadowCascades",
                                      "antiAliasing", "anisotropicFiltering", "softParticles"};

struct item {
    int cap;                    // -1: no cap
    set_int_fn r_set;
    get_int_fn r_get;
    int game, game_known;       // what the game set (or a level did), as the game sees it
};
static struct item q[Q_N];
static int g_on;
static add_icall_fn r_mono_add, r_il2cpp_add;
static int g_logged;

static void parse_caps(const char *s)
{
    for (int i = 0; i < Q_N; i++) q[i].cap = -1;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, ", ;"); t; t = strtok(NULL, ", ;")) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        for (int i = 0; i < Q_N; i++)
            if (!strcmp(t, cap_names[i])) q[i].cap = atoi(eq + 1);
    }
    for (int i = 0; i < Q_N; i++) g_on |= q[i].cap >= 0;
}

void quality_init(const char *package_caps)
{
    const char *e = getenv("UA_QUALITY");
    if (e && (!strcmp(e, "off") || !strcmp(e, "0") || !strcmp(e, "full"))) return;
    const char *caps = e && strchr(e, '=') ? e : package_caps;
    if (!caps || !*caps) return;
    parse_caps(caps);
    if (g_on) fprintf(stderr, "[host] quality caps: %s%s\n", caps, caps == e ? " (UA_QUALITY)" : "");
}

static int capped(int i, int v) { return q[i].cap >= 0 && v > q[i].cap ? q[i].cap : v; }

static void set_item(int i, int v)
{
    q[i].game = v; q[i].game_known = 1;
    int c = capped(i, v);
    if (c != v && g_logged < 20) {
        g_logged++;
        fprintf(stderr, "[host] quality: the game set %s %d, Unity gets %d\n", prop[i], v, c);
    }
    q[i].r_set(c);
}

static int get_item(int i) { return q[i].game_known ? q[i].game : q[i].r_get(); }

#define WRAP(I) \
    static void w_set_##I(int v) { set_item(I, v); } \
    static int w_get_##I(void) { return get_item(I); }
WRAP(0) WRAP(1) WRAP(2) WRAP(3) WRAP(4) WRAP(5) WRAP(6)
static const void *const w_set[Q_N] = {w_set_0, w_set_1, w_set_2, w_set_3, w_set_4, w_set_5, w_set_6};
static const void *const w_get[Q_N] = {w_get_0, w_get_1, w_get_2, w_get_3, w_get_4, w_get_5, w_get_6};

static int name_is(const char *name, const char *want)
{
    size_t n = strlen(want);
    return !strncmp(name, want, n) && (name[n] == 0 || name[n] == '(');     // IL2CPP adds the signature
}

static const void *wrap(const char *name, const void *fn)
{
    static const char cls[] = "UnityEngine.QualitySettings::";
    if (!name || !fn || strncmp(name, cls, sizeof cls - 1)) return fn;
    const char *m = name + sizeof cls - 1;
    for (int i = 0; i < Q_N; i++) {
        if (q[i].cap < 0 || (strncmp(m, "set_", 4) && strncmp(m, "get_", 4)) || !name_is(m + 4, prop[i])) continue;
        if (m[0] == 's') { q[i].r_set = (set_int_fn)fn; return w_set[i]; }
        q[i].r_get = (get_int_fn)fn;
        return w_get[i];
    }
    return fn;
}

static void w_mono_add(const char *name, const void *fn) { r_mono_add(name, wrap(name, fn)); }
static void w_il2cpp_add(const char *name, const void *fn) { r_il2cpp_add(name, wrap(name, fn)); }

void *quality_dlsym_hook(const char *symbol, void *found)
{
    if (!g_on || !found) return found;
    if (!strcmp(symbol, "mono_add_internal_call") && found != (void *)w_mono_add)
    { r_mono_add = (add_icall_fn)found; return (void *)w_mono_add; }
    if (!strcmp(symbol, "il2cpp_add_internal_call") && found != (void *)w_il2cpp_add)
    { r_il2cpp_add = (add_icall_fn)found; return (void *)w_il2cpp_add; }
    return found;
}

void quality_frame(void *user)
{
    (void)user;
    if (!g_on) return;
    for (int i = 0; i < Q_N; i++) {
        if (q[i].cap < 0 || !q[i].r_get || !q[i].r_set) continue;
        int v = q[i].r_get();
        if (v <= q[i].cap) continue;
        // a level change (or Unity itself) gave it more than the cap: that is the game's value now
        q[i].game = v; q[i].game_known = 1;
        q[i].r_set(q[i].cap);
        if (g_logged < 20) {
            g_logged++;
            fprintf(stderr, "[host] quality: %s %d from the quality level, Unity gets %d\n", prop[i], v, q[i].cap);
        }
    }
}
