// Pad navigation for Unity UI menus made for touch. Unity's UI (uGUI) already moves between buttons
// with the pad or the arrow keys and presses them with Submit (the Input Manager's default axes:
// arrows and the left stick, Submit = A / Enter, Cancel = B / Escape), but only from a selected
// button: a game made for touch never selects one, so the pad does nothing in its menus.
//   UA_UI_NAV=1   whenever nothing is selected, or the selection is gone (its panel hidden), the
//                 host selects the topmost interactable button on screen (then the leftmost); the
//                 game's own EventSystem does the rest. A tap (pointer mode) still selects what it
//                 taps.
// IL2CPP players only, through the API libil2cpp exports; checked on UnityMain after each frame, at
// most every 0.1 s, so Unity's own calls run on its main thread between frames.
#include "uinav.h"
#include "../loader/so_loader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_on;
static int g_state;             // 0 not resolved yet, 1 ready, -1 given up
static double g_last;
static struct {
    void *(*domain_get)(void);
    void *(*assembly_open)(void *domain, const char *name);
    void *(*assembly_image)(void *assembly);
    void *(*class_from_name)(void *image, const char *ns, const char *name);
    void *(*method)(void *klass, const char *name, int argc);
    void *(*invoke)(void *method, void *obj, void **params, void **exc);
    void *(*unbox)(void *obj);
    uint32_t (*array_length)(void *array);
    uint16_t *(*string_chars)(void *str);
    int32_t (*string_length)(void *str);
} I;
static struct {
    void *es_current, *es_selected, *es_select;         // EventSystem
    void *all_selectables, *interactable;               // Selectable
    void *active_enabled;                               // Behaviour.isActiveAndEnabled
    void *game_object, *transform;                      // Component
    void *position;                                     // Transform
    void *active_in_hierarchy;                          // GameObject
    void *name;                                         // Object
} M;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

void uinav_init(void)
{
    const char *e = getenv("UA_UI_NAV");
    g_on = e && *e && strcmp(e, "0");
}

static void *image(const char *name)
{
    char dll[96];
    snprintf(dll, sizeof dll, "%s.dll", name);
    void *a = I.assembly_open(I.domain_get(), dll);
    if (!a) a = I.assembly_open(I.domain_get(), name);
    return a ? I.assembly_image(a) : NULL;
}

static void *method(void *image, const char *ns, const char *cls, const char *name, int argc)
{
    void *k = image ? I.class_from_name(image, ns, cls) : NULL;
    void *m = k ? I.method(k, name, argc) : NULL;
    if (!m) fprintf(stderr, "[ui] UA_UI_NAV: %s.%s::%s not found\n", ns, cls, name);
    return m;
}

static int resolve(void)
{
    void *h = so_dlopen("libil2cpp.so", 4 /* RTLD_NOLOAD */);
    if (!h) { fprintf(stderr, "[ui] UA_UI_NAV: not an IL2CPP player, off\n"); return -1; }
#define S(f, n) (*(void **)&I.f = so_dlsym(h, n))
    if (!S(domain_get, "il2cpp_domain_get") || !S(assembly_open, "il2cpp_domain_assembly_open") ||
        !S(assembly_image, "il2cpp_assembly_get_image") || !S(class_from_name, "il2cpp_class_from_name") ||
        !S(method, "il2cpp_class_get_method_from_name") || !S(invoke, "il2cpp_runtime_invoke") ||
        !S(unbox, "il2cpp_object_unbox") || !S(array_length, "il2cpp_array_length"))
    { fprintf(stderr, "[ui] UA_UI_NAV: libil2cpp lacks the API it needs, off\n"); return -1; }
    S(string_chars, "il2cpp_string_chars"); S(string_length, "il2cpp_string_length");
#undef S
    void *ui = image("UnityEngine.UI"), *core = image("UnityEngine.CoreModule");
    if (!ui || !core) return 0;                         // not loaded yet: try again later
    M.es_current = method(ui, "UnityEngine.EventSystems", "EventSystem", "get_current", 0);
    M.es_selected = method(ui, "UnityEngine.EventSystems", "EventSystem", "get_currentSelectedGameObject", 0);
    M.es_select = method(ui, "UnityEngine.EventSystems", "EventSystem", "SetSelectedGameObject", 1);
    M.all_selectables = method(ui, "UnityEngine.UI", "Selectable", "get_allSelectablesArray", 0);
    M.interactable = method(ui, "UnityEngine.UI", "Selectable", "IsInteractable", 0);
    M.active_enabled = method(core, "UnityEngine", "Behaviour", "get_isActiveAndEnabled", 0);
    M.game_object = method(core, "UnityEngine", "Component", "get_gameObject", 0);
    M.transform = method(core, "UnityEngine", "Component", "get_transform", 0);
    M.position = method(core, "UnityEngine", "Transform", "get_position", 0);
    M.active_in_hierarchy = method(core, "UnityEngine", "GameObject", "get_activeInHierarchy", 0);
    M.name = method(core, "UnityEngine", "Object", "get_name", 0);
    if (!M.es_current || !M.es_selected || !M.es_select || !M.all_selectables || !M.interactable || !M.active_enabled ||
        !M.game_object || !M.transform || !M.position || !M.active_in_hierarchy)
    { fprintf(stderr, "[ui] UA_UI_NAV: the game has no Unity UI to navigate, off\n"); return -1; }
    fprintf(stderr, "[ui] UA_UI_NAV: on (the topmost button is selected when nothing is)\n");
    return 1;
}

static void *call(void *m, void *obj, void *arg)
{
    void *exc = NULL, *args[1] = { arg };
    void *r = I.invoke(m, obj, arg ? args : NULL, &exc);
    return exc ? NULL : r;
}

static int truth(void *m, void *obj)
{
    void *r = call(m, obj, NULL);
    return r && *(uint8_t *)I.unbox(r);
}

static void log_selected(void *go)
{
    char buf[64] = "?";
    void *s = M.name && I.string_chars && I.string_length ? call(M.name, go, NULL) : NULL;
    if (s) {
        int n = I.string_length(s), i;
        uint16_t *c = I.string_chars(s);
        for (i = 0; i < n && i < (int)sizeof buf - 1; i++) buf[i] = c[i] < 128 ? (char)c[i] : '?';
        buf[i] = 0;
    }
    fprintf(stderr, "[ui] selected \"%s\"\n", buf);
}

void uinav_frame(void *user)
{
    (void)user;
    if (!g_on || g_state < 0) return;
    double t = now();
    if (t - g_last < 0.1) return;
    g_last = t;
    if (!g_state && !(g_state = resolve())) return;
    if (g_state < 0) return;

    void *es = call(M.es_current, NULL, NULL);
    if (!es) return;
    void *sel = call(M.es_selected, es, NULL);
    if (sel && truth(M.active_in_hierarchy, sel)) return;

    void *arr = call(M.all_selectables, NULL, NULL);
    if (!arr) return;
    uint32_t n = I.array_length(arr);
    void **items = (void **)((char *)arr + 4 * sizeof(void *));    // Il2CppArray: klass, monitor, bounds, max_length
    void *best = NULL;
    float bx = 0, by = 0;
    for (uint32_t i = 0; i < n; i++) {
        void *s = items[i];
        if (!s || !truth(M.active_enabled, s) || !truth(M.interactable, s)) continue;
        void *tr = call(M.transform, s, NULL), *p = tr ? call(M.position, tr, NULL) : NULL;
        if (!p) continue;
        const float *v = I.unbox(p);
        if (!best || v[1] > by + 0.5f || (v[1] > by - 0.5f && v[0] < bx)) { best = s; bx = v[0]; by = v[1]; }
    }
    if (!best) return;
    void *go = call(M.game_object, best, NULL);
    if (!go) return;
    call(M.es_select, es, go);
    log_selected(go);
}
