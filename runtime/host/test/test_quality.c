// quality.c check: a fake libunity registers QualitySettings setters and getters through the wrapped
// mono_add_internal_call / il2cpp_add_internal_call; Unity's values and what the game reads back are
// checked after setter calls and after "frames" (a quality level change sets values without
// setters). Plain C: cc test/test_quality.c -o /tmp/t && /tmp/t
#include "../quality.c"

#include <assert.h>

static int u_plc = 4, u_aa = 2, u_aniso = 2, u_shadows = 2, registered;
static void u_set_plc(int v) { u_plc = v; }
static int u_get_plc(void) { return u_plc; }
static void u_set_aa(int v) { u_aa = v; }
static int u_get_aa(void) { return u_aa; }
static void u_set_aniso(int v) { u_aniso = v; }
static int u_get_aniso(void) { return u_aniso; }
static void u_set_shadows(int v) { u_shadows = v; }
static int u_get_shadows(void) { return u_shadows; }
static void (*set_plc)(int), (*set_aa)(int), (*set_aniso)(int), (*set_shadows)(int);
static int (*get_plc)(void), (*get_aa)(void), (*get_aniso)(void), (*get_shadows)(void);

static void fake_add(const char *name, const void *fn)
{
    registered++;
    char m[128];
    snprintf(m, sizeof m, "%s", strstr(name, "::") + 2);
    if (strchr(m, '(')) *strchr(m, '(') = 0;
#define K(n, s, g) if (!strcmp(m, "set_" n)) s = (void (*)(int))fn; else if (!strcmp(m, "get_" n)) g = (int (*)(void))fn;
    K("pixelLightCount", set_plc, get_plc)
    K("antiAliasing", set_aa, get_aa)
    K("anisotropicFiltering", set_aniso, get_aniso)
    K("shadows", set_shadows, get_shadows)
#undef K
}

// a fresh "process": quality.c's state, Unity's (an Ultra level), then libunity's registrations
static void start(const char *env, const char *caps, const char *add_sym, int il2cpp)
{
    memset(q, 0, sizeof q);
    g_on = 0; g_logged = 0; r_mono_add = r_il2cpp_add = NULL;
    u_plc = 4; u_aa = 2; u_aniso = 2; u_shadows = 2; registered = 0;
    set_plc = set_aa = set_aniso = set_shadows = NULL;
    get_plc = get_aa = get_aniso = get_shadows = NULL;
    if (env) setenv("UA_QUALITY", env, 1); else unsetenv("UA_QUALITY");
    quality_init(caps);
    add_icall_fn add = (add_icall_fn)quality_dlsym_hook(add_sym, (void *)fake_add);
    const char *sig = il2cpp ? "(System.Int32)" : "";
    char n[128];
#define REG(p, s, g) snprintf(n, sizeof n, "UnityEngine.QualitySettings::set_" p "%s", sig); add(n, (const void *)s); \
                     snprintf(n, sizeof n, "UnityEngine.QualitySettings::get_" p "%s", il2cpp ? "()" : ""); add(n, (const void *)g);
    REG("pixelLightCount", u_set_plc, u_get_plc)
    REG("antiAliasing", u_set_aa, u_get_aa)
    REG("anisotropicFiltering", u_set_aniso, u_get_aniso)
    REG("shadows", u_set_shadows, u_get_shadows)
    REG("shadowsX", u_set_shadows, u_get_shadows)          // not ours: a longer name
#undef REG
    assert(registered == 10);
}

int main(void)
{
    // no caps (a full-picture setup): nothing wrapped
    start(NULL, "", "mono_add_internal_call", 0);
    assert(set_plc == u_set_plc && get_aa == u_get_aa);
    quality_frame(NULL);
    assert(u_plc == 4 && u_aa == 2);

    // the package's caps: the level's values above them are capped after the first frame, the game
    // reads its own
    start(NULL, "pixel_lights=1,msaa=0,anisotropic=1", "mono_add_internal_call", 0);
    assert(set_plc != u_set_plc && get_plc != u_get_plc && set_shadows == u_set_shadows);
    assert(get_plc() == 4);                             // before the first frame: Unity's own value
    quality_frame(NULL);
    assert(u_plc == 1 && u_aa == 0 && u_aniso == 1 && u_shadows == 2);
    assert(get_plc() == 4 && get_aa() == 2 && get_aniso() == 2);
    set_aa(8);                                          // an options menu
    assert(u_aa == 0 && get_aa() == 8);
    set_plc(0);                                         // below the cap: as the game asks
    assert(u_plc == 0 && get_plc() == 0);
    quality_frame(NULL);
    assert(u_plc == 0 && get_plc() == 0);
    u_aa = 4;                                           // SetQualityLevel: a level with 4x MSAA
    quality_frame(NULL);
    assert(u_aa == 0 && get_aa() == 4);

    // IL2CPP names (with signatures); soft shadows drawn hard
    start(NULL, "shadows=1", "il2cpp_add_internal_call", 0 + 1);
    assert(set_shadows != u_set_shadows && set_plc == u_set_plc);
    set_shadows(2);
    assert(u_shadows == 1 && get_shadows() == 2);

    // UA_QUALITY: off wins over the package; caps of its own replace the package's
    start("off", "msaa=0", "mono_add_internal_call", 0);
    assert(set_aa == u_set_aa);
    start("msaa=1", "msaa=0,pixel_lights=1", "mono_add_internal_call", 0);
    assert(set_plc == u_set_plc && set_aa != u_set_aa);
    quality_frame(NULL);
    assert(u_aa == 1 && get_aa() == 2);
    printf("quality: ok\n");
    return 0;
}
