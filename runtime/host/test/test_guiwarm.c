// guiwarm.c's check (runtime/host/test/plugins_test.sh), on stand-ins for Mono's embedding calls:
//   on         UA_GUI_WARM=1: nothing in frames 1 and 2; in frame 3 one call of the IMGUI module's UnityEngine.GUI
//              method DoSetSkin with one argument, a null skin and no object; nothing in the frames after
//   oldmodule  a player without UnityEngine.IMGUIModule: the class comes from UnityEngine
//   nomethod   a GUI class without DoSetSkin(GUISkin): no call
//   throws     the call raises: no second call
//   nomono     no Mono runtime (an IL2CPP player): nothing looked up
//   off, zero  no UA_GUI_WARM, UA_GUI_WARM=0: Mono never asked for anything
#include "../guiwarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *g_mode = "on";
static int g_asked, g_lookups, g_calls, g_argc = -1, g_no_object, g_null_skin, g_right_method;
static void *g_image;
static char g_ns[64], g_class[64], g_name[64];
static char imgui_module[] = "UnityEngine.IMGUIModule", engine[] = "UnityEngine", gui_class[] = "GUI", do_set_skin[] = "DoSetSkin";

static void *image_loaded(const char *name)
{
    if (!strcmp(name, "UnityEngine.IMGUIModule")) return strcmp(g_mode, "oldmodule") ? imgui_module : NULL;
    return !strcmp(name, "UnityEngine") ? engine : NULL;
}
static void *class_from_name(void *image, const char *ns, const char *name)
{
    g_image = image;
    snprintf(g_ns, sizeof g_ns, "%s", ns); snprintf(g_class, sizeof g_class, "%s", name);
    return gui_class;
}
static void *get_method(void *klass, const char *name, int argc)
{
    if (klass != gui_class) return NULL;
    snprintf(g_name, sizeof g_name, "%s", name); g_argc = argc;
    return strcmp(g_mode, "nomethod") ? do_set_skin : NULL;
}
static void *invoke(void *method, void *object, void **args, void **exc)
{
    g_calls++;
    g_right_method = method == do_set_skin; g_no_object = object == NULL; g_null_skin = args && args[0] == NULL;
    if (!strcmp(g_mode, "throws") && exc) *exc = (void *)0x30;
    return NULL;
}

void *mono_runtime_lib(void) { g_asked++; return strcmp(g_mode, "nomono") ? (void *)1 : NULL; }
void *so_dlsym(void *h, const char *n)
{
    (void)h; g_lookups++;
    if (!strcmp(n, "mono_image_loaded")) return (void *)image_loaded;
    if (!strcmp(n, "mono_class_from_name")) return (void *)class_from_name;
    if (!strcmp(n, "mono_class_get_method_from_name")) return (void *)get_method;
    if (!strcmp(n, "mono_runtime_invoke")) return (void *)invoke;
    return NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) g_mode = argv[1];
    int off = !strcmp(g_mode, "off") || !strcmp(g_mode, "zero");
    if (!strcmp(g_mode, "off")) unsetenv("UA_GUI_WARM");
    else setenv("UA_GUI_WARM", off ? "0" : "1", 1);
    guiwarm_init();
    guiwarm_frame(); guiwarm_frame();
    CHECK(!g_asked && !g_calls, "frames 1 and 2: Mono asked %d times, %d calls", g_asked, g_calls);
    guiwarm_frame();
    int want = !off && strcmp(g_mode, "nomono") && strcmp(g_mode, "nomethod");
    CHECK(g_calls == want, "frame 3: %d calls, %d expected", g_calls, want);
    if (want) {
        CHECK(g_image == (strcmp(g_mode, "oldmodule") ? (void *)imgui_module : (void *)engine), "the class's image: %s", (char *)g_image);
        CHECK(!strcmp(g_ns, "UnityEngine") && !strcmp(g_class, "GUI"), "the class: %s.%s", g_ns, g_class);
        CHECK(!strcmp(g_name, "DoSetSkin") && g_argc == 1, "the method: %s with %d arguments", g_name, g_argc);
        CHECK(g_right_method && g_no_object && g_null_skin, "the call: method %d, no object %d, null skin %d", g_right_method, g_no_object, g_null_skin);
    }
    if (off) CHECK(!g_asked && !g_lookups, "off: Mono asked %d times, %d lookups", g_asked, g_lookups);
    if (!strcmp(g_mode, "nomono")) CHECK(!g_lookups, "no Mono: %d lookups", g_lookups);
    int asked = g_asked, lookups = g_lookups;
    for (int i = 0; i < 200; i++) guiwarm_frame();
    CHECK(g_calls == want && g_asked == asked && g_lookups == lookups, "later frames: %d calls, Mono asked %d more times, %d more lookups",
          g_calls - want, g_asked - asked, g_lookups - lookups);
    printf("guiwarm %s: %s (%d failures)\n", g_mode, fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
