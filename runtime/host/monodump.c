// monodump.c: what Unity's scene holds, read through the game's own Mono at run time (a debugging aid).
//
//   UA_DUMP_RENDERERS=<s>[,<s>...]  <s> seconds after the first frame (up to 16 times), UnityMain logs the main camera
//                           (position, field of view or orthographic size, aspect, clip planes, transparency
//                           sort mode) and every renderer some camera sees: its class, the GameObject's
//                           name and two parents, its bounds (centre, size), sorting layer and order, the
//                           material, and a SpriteRenderer's colour and sprite, a MeshRenderer's first
//                           vertex colour ("[dump] ..."). With UA_DUMP_RENDERERS_ALL=1 the hidden ones too.
//   UA_DUMP_HERO=<period>[,<name>]  every <period> seconds the position of the GameObject <name> (Knight) and of the
//                           main camera ("[dump] t <s> Knight (x, y) camera (x, y)"): where a scripted walk took the player
// Everything is read through public UnityEngine properties (Object.FindObjectsOfType and getters), on
// UnityMain after a frame, as a script of the game would; nothing is changed.
//   UA_FLIPBOOK=<controller>,...|@<file>  a stand-in for Unity's Animator on sprite flip-books: the managed assembly
//                           uaflipbook.dll (runtime/host/managed; UA_FLIPBOOK_DLL, default beside ua_host) loaded
//                           into the game's domain once UnityEngine is up, UAFlipbook.Init(<controllers>, <mode>)
//                           called on UnityMain. Animators of those controllers (one looping clip that only swaps
//                           its own sprite) are shadowed, then turned off and their sprite set the same way before
//                           every render (managed/UAFlipbook.cs). UA_FLIPBOOK_MODE=check: shadowed only, mismatches
//                           counted ("[flipbook] ..." in the log every 10 s).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../loader/so_loader.h"
#include "monodump.h"

#include "hitches.h"

typedef void MonoObject, MonoClass, MonoMethod, MonoImage, MonoDomain, MonoType, MonoArray, MonoString;
static MonoDomain *(*m_domain_get)(void);
static MonoImage *(*m_image_loaded)(const char *);
static MonoClass *(*m_class_from_name)(MonoImage *, const char *, const char *);
static MonoMethod *(*m_method_from_name)(MonoClass *, const char *, int);
static MonoObject *(*m_invoke)(MonoMethod *, void *, void **, MonoObject **);
static MonoType *(*m_class_get_type)(MonoClass *);
static MonoObject *(*m_type_get_object)(MonoDomain *, MonoType *);
static uintptr_t (*m_array_length)(MonoArray *);
static char *(*m_array_addr)(MonoArray *, int, uintptr_t);
static void *(*m_unbox)(MonoObject *);
static char *(*m_string_to_utf8)(MonoString *);
static void (*m_free)(void *);
static MonoClass *(*m_object_get_class)(MonoObject *);
static const char *(*m_class_get_name)(MonoClass *);
static MonoClass *(*m_class_get_parent)(MonoClass *);
static MonoString *(*m_string_new)(MonoDomain *, const char *);
static void *(*m_domain_assembly_open)(MonoDomain *, const char *);
static MonoImage *(*m_assembly_get_image)(void *);

static int load(void)
{
    void *h = mono_runtime_lib();
    if (!h) return 0;
#define L(v, n) if (!(*(void **)&v = so_dlsym(h, n))) { fprintf(stderr, "[dump] %s missing\n", n); return 0; }
    L(m_domain_get, "mono_domain_get"); L(m_image_loaded, "mono_image_loaded"); L(m_class_from_name, "mono_class_from_name");
    L(m_method_from_name, "mono_class_get_method_from_name"); L(m_invoke, "mono_runtime_invoke"); L(m_class_get_type, "mono_class_get_type");
    L(m_type_get_object, "mono_type_get_object"); L(m_array_length, "mono_array_length"); L(m_array_addr, "mono_array_addr_with_size");
    L(m_unbox, "mono_object_unbox"); L(m_string_to_utf8, "mono_string_to_utf8"); L(m_free, "mono_free");
    L(m_object_get_class, "mono_object_get_class"); L(m_class_get_name, "mono_class_get_name"); L(m_class_get_parent, "mono_class_get_parent");
    L(m_string_new, "mono_string_new"); L(m_domain_assembly_open, "mono_domain_assembly_open"); L(m_assembly_get_image, "mono_assembly_get_image");
#undef L
    return 1;
}

// a method found on the class or a base class
static MonoMethod *method(MonoClass *k, const char *name, int n)
{
    for (; k; k = m_class_get_parent(k)) { MonoMethod *m = m_method_from_name(k, name, n); if (m) return m; }
    return NULL;
}
static MonoObject *call(MonoObject *o, const char *name, void **args, int n)
{
    if (!o) return NULL;
    MonoMethod *m = method(m_object_get_class(o), name, n);
    if (!m) return NULL;
    MonoObject *exc = NULL;
    MonoObject *r = m_invoke(m, o, args, &exc);
    return exc ? NULL : r;
}
static void str(MonoObject *s, char *out, size_t n)
{
    out[0] = 0;
    if (!s) { snprintf(out, n, "-"); return; }
    char *u = m_string_to_utf8((MonoString *)s);
    if (u) { snprintf(out, n, "%s", u); m_free(u); }
}
static void name_of(MonoObject *o, char *out, size_t n) { str(call(o, "get_name", NULL, 0), out, n); }
static int getf(MonoObject *o, const char *prop, float *v, int count)
{
    MonoObject *r = call(o, prop, NULL, 0);
    if (!r) return 0;
    memcpy(v, m_unbox(r), sizeof(float) * (size_t)count);
    return 1;
}
static int geti(MonoObject *o, const char *prop, int *v)
{
    MonoObject *r = call(o, prop, NULL, 0);
    if (!r) return 0;
    memcpy(v, m_unbox(r), sizeof *v);
    return 1;
}

static void dump(void)
{
    if (!load()) return;
    MonoDomain *dom = m_domain_get();
    MonoImage *core = m_image_loaded("UnityEngine.CoreModule");
    if (!core) { fprintf(stderr, "[dump] UnityEngine.CoreModule not loaded\n"); return; }
    MonoClass *k_cam = m_class_from_name(core, "UnityEngine", "Camera"), *k_obj = m_class_from_name(core, "UnityEngine", "Object");
    MonoClass *k_ren = m_class_from_name(core, "UnityEngine", "Renderer"), *k_mf = m_class_from_name(core, "UnityEngine", "MeshFilter");
    if (!k_cam || !k_obj || !k_ren) { fprintf(stderr, "[dump] UnityEngine classes not found\n"); return; }
    MonoObject *exc = NULL;
    // the main camera
    MonoMethod *get_main = m_method_from_name(k_cam, "get_main", 0);
    MonoObject *cam = get_main ? m_invoke(get_main, NULL, NULL, &exc) : NULL;
    if (cam) {
        float pos[3] = { 0 }, fov = 0, size = 0, aspect = 0, nearp = 0, farp = 0;
        int ortho = 0, sortmode = 0;
        getf(call(cam, "get_transform", NULL, 0), "get_position", pos, 3);
        getf(cam, "get_fieldOfView", &fov, 1); getf(cam, "get_orthographicSize", &size, 1); getf(cam, "get_aspect", &aspect, 1);
        getf(cam, "get_nearClipPlane", &nearp, 1); getf(cam, "get_farClipPlane", &farp, 1);
        MonoObject *o = call(cam, "get_orthographic", NULL, 0); if (o) ortho = *(unsigned char *)m_unbox(o);
        geti(cam, "get_transparencySortMode", &sortmode);
        char nm[128]; name_of(cam, nm, sizeof nm);
        fprintf(stderr, "[dump] camera %s at (%.2f, %.2f, %.2f) %s %.2f aspect %.3f clip %.2f - %.2f sort mode %d\n", nm, pos[0], pos[1], pos[2],
                ortho ? "orthographic size" : "fov", ortho ? size : fov, aspect, nearp, farp, sortmode);
    }
    // every camera: enabled, depth (the order they render in), culling mask, clear flags, target texture
    MonoMethod *all_cams = m_method_from_name(k_cam, "get_allCameras", 0);
    MonoArray *cams = all_cams ? (MonoArray *)m_invoke(all_cams, NULL, NULL, &exc) : NULL;
    for (uintptr_t i = 0, nc = cams ? m_array_length(cams) : 0; i < nc; i++) {
        MonoObject *c = *(MonoObject **)m_array_addr(cams, sizeof(void *), i);
        float depth = 0; int mask = 0, clear = 0;
        getf(c, "get_depth", &depth, 1); geti(c, "get_cullingMask", &mask); geti(c, "get_clearFlags", &clear);
        MonoObject *rt = call(c, "get_targetTexture", NULL, 0);
        char nm[96], rn[96] = "the screen"; name_of(c, nm, sizeof nm);
        int w = 0, h = 0;
        if (rt) { name_of(rt, rn, sizeof rn); geti(rt, "get_width", &w); geti(rt, "get_height", &h); }
        float bg[4] = { 0 }, rect[4] = { 0 };
        getf(c, "get_backgroundColor", bg, 4); getf(c, "get_rect", rect, 4);
        fprintf(stderr, "[dump] camera %s depth %.1f culling mask 0x%08x clear %d (%.2f, %.2f, %.2f, %.2f) rect (%.3f, %.3f, %.3f, %.3f) into %s %dx%d\n",
                nm, depth, (unsigned)mask, clear, bg[0], bg[1], bg[2], bg[3], rect[0], rect[1], rect[2], rect[3], rn, w, h);
        // UA_DUMP_CAM_OFF=<name>,...: a measurement (it may change the picture): those cameras turned off at each dump
        const char *coff = getenv("UA_DUMP_CAM_OFF");
        if (coff && *coff) {
            char list[256], *sv = NULL; snprintf(list, sizeof list, "%s", coff);
            for (char *t = strtok_r(list, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
                if (!strcmp(t, nm)) { unsigned char no = 0; void *a1[1] = { &no }; call(c, "set_enabled", a1, 1); fprintf(stderr, "[dump]   turned off (UA_DUMP_CAM_OFF)\n"); break; }
        }
    }
    // the renderers
    MonoMethod *find = m_method_from_name(k_obj, "FindObjectsOfType", 1);
    if (!find) { fprintf(stderr, "[dump] Object.FindObjectsOfType(Type) not found\n"); return; }
    void *args[1] = { m_type_get_object(dom, m_class_get_type(k_ren)) };
    MonoArray *all = (MonoArray *)m_invoke(find, NULL, args, &exc);
    if (!all || exc) { fprintf(stderr, "[dump] FindObjectsOfType failed\n"); return; }
    int everything = getenv("UA_DUMP_RENDERERS_ALL") && atoi(getenv("UA_DUMP_RENDERERS_ALL")) > 0;
    uintptr_t n = m_array_length(all), shown = 0;
    MonoObject *mf_type = k_mf ? m_type_get_object(dom, m_class_get_type(k_mf)) : NULL;
    for (uintptr_t i = 0; i < n; i++) {
        MonoObject *r = *(MonoObject **)m_array_addr(all, sizeof(void *), i);
        if (!r) continue;
        MonoObject *vis = call(r, "get_isVisible", NULL, 0);
        int visible = vis && *(unsigned char *)m_unbox(vis);
        if (!visible && !everything) continue;
        shown++;
        const char *cls = m_class_get_name(m_object_get_class(r));
        MonoObject *go = call(r, "get_gameObject", NULL, 0), *tr = call(r, "get_transform", NULL, 0);
        char nm[96], p1[64] = "", p2[64] = "", layer[48], mat[96], extra[160] = "";
        name_of(go, nm, sizeof nm);
        MonoObject *pa = call(tr, "get_parent", NULL, 0);
        if (pa) { name_of(pa, p1, sizeof p1); MonoObject *pb = call(pa, "get_parent", NULL, 0); if (pb) name_of(pb, p2, sizeof p2); }
        float b[6] = { 0 };
        getf(r, "get_bounds", b, 6);                // (Bounds: centre, extents)
        str(call(r, "get_sortingLayerName", NULL, 0), layer, sizeof layer);
        int order = 0; geti(r, "get_sortingOrder", &order);
        name_of(call(r, "get_sharedMaterial", NULL, 0), mat, sizeof mat);
        if (!strcmp(cls, "SpriteRenderer")) {
            float c[4] = { 0 }; char sp[96];
            getf(r, "get_color", c, 4);
            name_of(call(r, "get_sprite", NULL, 0), sp, sizeof sp);
            snprintf(extra, sizeof extra, " colour (%.2f, %.2f, %.2f, %.2f) sprite %s", c[0], c[1], c[2], c[3], sp);
        } else if (!strcmp(cls, "MeshRenderer") && mf_type) {
            void *a1[1] = { mf_type };
            MonoObject *mf = call(go, "GetComponent", a1, 1);
            MonoObject *mesh = mf ? call(mf, "get_sharedMesh", NULL, 0) : NULL;
            MonoArray *cols = mesh ? (MonoArray *)call(mesh, "get_colors32", NULL, 0) : NULL;
            char mn[96]; name_of(mesh, mn, sizeof mn);
            if (cols && m_array_length(cols) > 0) {
                unsigned char *c = (unsigned char *)m_array_addr(cols, 4, 0);
                snprintf(extra, sizeof extra, " mesh %s vertex colour (%d, %d, %d, %d)", mn, c[0], c[1], c[2], c[3]);
            } else snprintf(extra, sizeof extra, " mesh %s", mn);
        }
        fprintf(stderr, "[dump] %s %s%s %s < %s < %s at (%.2f, %.2f, %.2f) size (%.2f, %.2f, %.2f) layer %s %d material %s%s\n",
                cls, visible ? "" : "(hidden) ", "", nm, p1, p2, b[0], b[1], b[2], 2 * b[3], 2 * b[4], 2 * b[5], layer, order, mat, extra);
    }
    fprintf(stderr, "[dump] %lu renderers, %lu logged\n", (unsigned long)n, (unsigned long)shown);
    // the Animators that run (active and enabled): their culling mode (0 always animate, 1 cull update transforms,
    // 2 cull completely), controller, and whether one of the GameObject's renderers is visible
    MonoImage *anim = m_image_loaded("UnityEngine.AnimationModule");
    MonoClass *k_an = anim ? m_class_from_name(anim, "UnityEngine", "Animator") : NULL;
    if (!k_an) return;
    args[0] = m_type_get_object(dom, m_class_get_type(k_an));
    all = (MonoArray *)m_invoke(find, NULL, args, &exc);
    if (!all || exc) return;
    void *ra[1] = { m_type_get_object(dom, m_class_get_type(k_ren)) };
    unsigned counts[3] = { 0 }, running = 0;
    n = m_array_length(all);
    for (uintptr_t i = 0; i < n; i++) {
        MonoObject *a = *(MonoObject **)m_array_addr(all, sizeof(void *), i);
        MonoObject *on = a ? call(a, "get_isActiveAndEnabled", NULL, 0) : NULL;
        if (!on || !*(unsigned char *)m_unbox(on)) continue;
        running++;
        int mode = 0; geti(a, "get_cullingMode", &mode);
        if (mode >= 0 && mode < 3) counts[mode]++;
        MonoObject *go = call(a, "get_gameObject", NULL, 0), *tr = call(a, "get_transform", NULL, 0);
        char nm[96], p1[64] = "", ctl[64];
        name_of(go, nm, sizeof nm);
        MonoObject *pa = call(tr, "get_parent", NULL, 0); if (pa) name_of(pa, p1, sizeof p1);
        name_of(call(a, "get_runtimeAnimatorController", NULL, 0), ctl, sizeof ctl);
        MonoObject *ren = call(go, "GetComponentInChildren", ra, 1);
        MonoObject *vis = ren ? call(ren, "get_isVisible", NULL, 0) : NULL;
        fprintf(stderr, "[dump] Animator %s < %s culling %d controller %s%s\n", nm, p1, mode, ctl,
                !ren ? " (no renderer)" : vis && *(unsigned char *)m_unbox(vis) ? " visible" : " hidden");
        // UA_DUMP_ANIM_OFF=<text>,<text>...: a measurement (it changes the picture): Animators whose controller's
        // name contains one of them are turned off, to see what their updates cost
        const char *off = getenv("UA_DUMP_ANIM_OFF");
        if (off && *off) {
            char list[256]; snprintf(list, sizeof list, "%s", off);
            for (char *t = strtok(list, ","); t; t = strtok(NULL, ","))
                if (strstr(ctl, t)) {
                    unsigned char no = 0; void *a1[1] = { &no };
                    call(a, "set_enabled", a1, 1);
                    fprintf(stderr, "[dump]   turned off (UA_DUMP_ANIM_OFF)\n");
                    break;
                }
        }
    }
    fprintf(stderr, "[dump] %lu Animators, %u running (culling 0: %u, 1: %u, 2: %u)\n", (unsigned long)n, running, counts[0], counts[1], counts[2]);
}

// the named GameObject's and the main camera's positions
static void hero(const char *name)
{
    static int ok = -1;
    static MonoMethod *find, *get_main;
    if (ok < 0) {
        ok = 0;
        if (!load()) return;
        MonoImage *core = m_image_loaded("UnityEngine.CoreModule");
        MonoClass *k_go = core ? m_class_from_name(core, "UnityEngine", "GameObject") : NULL, *k_cam = core ? m_class_from_name(core, "UnityEngine", "Camera") : NULL;
        find = k_go ? m_method_from_name(k_go, "Find", 1) : NULL;
        get_main = k_cam ? m_method_from_name(k_cam, "get_main", 0) : NULL;
        ok = find && get_main;
        if (!ok) { fprintf(stderr, "[dump] GameObject.Find or Camera.main not found\n"); return; }
    }
    if (!ok) return;
    MonoObject *exc = NULL;
    void *args[1] = { m_string_new(m_domain_get(), name) };
    MonoObject *go = m_invoke(find, NULL, args, &exc);
    if (exc) go = NULL;
    MonoObject *cam = m_invoke(get_main, NULL, NULL, &exc);
    if (exc) cam = NULL;
    float h[3] = { 0 }, c[3] = { 0 };
    int fh = go && getf(call(go, "get_transform", NULL, 0), "get_position", h, 3);
    int fc = cam && getf(call(cam, "get_transform", NULL, 0), "get_position", c, 3);
    char a[48] = "-", b[48] = "-";
    if (fh) snprintf(a, sizeof a, "(%.2f, %.2f)", h[0], h[1]);
    if (fc) snprintf(b, sizeof b, "(%.2f, %.2f)", c[0], c[1]);
    fprintf(stderr, "[dump] %s %s camera %s\n", name, a, b);
}

// UA_FLIPBOOK: uaflipbook.dll loaded and UAFlipbook.Init called, once UnityEngine.CoreModule is loaded
static void flipbook_frame(void)
{
    static int state = -1;                  // -1: not read; 0: off or done; 1: waiting for UnityEngine
    static char dll[512];
    if (state == 0) return;
    const char *names = getenv("UA_FLIPBOOK");
    if (state < 0) {
        state = names && *names ? 1 : 0;
        if (!state) return;
        const char *d = getenv("UA_FLIPBOOK_DLL");
        if (d && *d) snprintf(dll, sizeof dll, "%s", d);
        else {
            char exe[400]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
            exe[n > 0 ? n : 0] = 0;
            char *slash = strrchr(exe, '/'); if (slash) *slash = 0;
            snprintf(dll, sizeof dll, "%s/uaflipbook.dll", exe);
        }
    }
    if (!load() || !m_image_loaded("UnityEngine.CoreModule") || !m_image_loaded("UnityEngine.AnimationModule")) return;
    state = 0;
    static char list[16384];
    if (names[0] == '@') {                  // @<file>: the names in a file (commas or lines)
        FILE *f = fopen(names + 1, "r");
        size_t n = f ? fread(list, 1, sizeof list - 1, f) : 0;
        if (f) fclose(f);
        list[n] = 0;
        for (char *c = list; *c; c++) if (*c == '\n' || *c == '\r') *c = ',';
        if (!n) { fprintf(stderr, "[flipbook] %s: nothing to read: off\n", names + 1); return; }
        names = list;
    }
    MonoDomain *dom = m_domain_get();
    void *asm_ = m_domain_assembly_open(dom, dll);
    MonoImage *img = asm_ ? m_assembly_get_image(asm_) : NULL;
    MonoClass *k = img ? m_class_from_name(img, "", "UAFlipbook") : NULL;
    MonoMethod *init = k ? m_method_from_name(k, "Init", 2) : NULL;
    if (!init) { fprintf(stderr, "[flipbook] %s: no UAFlipbook.Init(string, int): off\n", dll); return; }
    const char *m = getenv("UA_FLIPBOOK_MODE");
    int mode = m && !strcmp(m, "check") ? 0 : 1;
    void *args[2] = { m_string_new(dom, names), &mode };
    MonoObject *exc = NULL;
    m_invoke(init, NULL, args, &exc);
    if (exc) fprintf(stderr, "[flipbook] UAFlipbook.Init threw: off\n");
}

void monodump_frame(void)
{
    flipbook_frame();
    static int state = -1, n, next;         // state -1: not read; 0: off or done; 1: waiting
    static double at[16], t0, period, last;
    static char name[64];
    if (state == 0) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    if (state < 0) {
        const char *e = getenv("UA_DUMP_RENDERERS");
        for (char *p = (char *)e; p && *p && n < 16; ) {
            double s = strtod(p, &p);
            if (s > 0) at[n++] = s;
            while (*p == ',' || *p == ' ') p++;
            if (*p && !(*p >= '0' && *p <= '9') && *p != '.') break;
        }
        const char *hv = getenv("UA_DUMP_HERO");
        if (hv) {
            char *p; period = strtod(hv, &p);
            snprintf(name, sizeof name, "%s", *p == ',' ? p + 1 : "Knight");
        }
        state = n || period > 0 ? 1 : 0; t0 = now;
        return;
    }
    if (period > 0 && now - last >= period) {
        last = now;
        fprintf(stderr, "[dump] t %.2f ", now - t0);
        hero(name);
    }
    if (next < n && now - t0 >= at[next]) {
        fprintf(stderr, "[dump] at %.0f s\n", now - t0);
        dump();
        if (++next == n && period <= 0) state = 0;
    }
}
