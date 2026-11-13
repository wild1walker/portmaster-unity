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
//   UA_HERO_PLACE=<x>,<y>[,<s>]  a test aid that changes the game: <s> seconds (default 3) after the Knight first
//                           stands in the scene (not the load's waiting place far below it), its transform is put at
//                           (x, y) once ("[dump] Knight placed ..."): a test save
//                           whose respawn point is not where a scene is entered (Soul Master's lower room) entered
//                           from its door instead
//   UA_DUMP_OFF=<assembly>:<class>[@<GameObject>],...  a measurement that changes the picture: every 5 s once the Knight exists, the
//                           components of those classes (e.g. Assembly-CSharp-firstpass:UnityStandardAssets.ImageEffects.
//                           BloomOptimized) turned off ("[dump] off ...")
//   UA_DUMP_OFF_Z=<near>,<far>[,go]  a measurement that changes the picture: what a room's scenery costs. Every 5 s once
//                           the Knight exists (every 60 s once some were found), the renderers of the loaded
//                           scenes' own objects (not the game's lasting ones: the Knight, the HUD, the cameras' rig)
//                           on layers only the main camera draws, whose transform lies outside the depths
//                           <near> .. <far> (world z; Hollow Knight plays at z 0, nearer to the camera is
//                           negative), are turned off, with ",go" their GameObjects made inactive (their
//                           scripts, Animators and particle systems stop too) ("[dump] off z: ...")
//   UA_DUMP_TEXTURES=<s>[,<s>...]  <s> seconds after the first frame (up to 16 times), UnityMain logs every texture Unity
//                           holds (Resources.FindObjectsOfTypeAll(Texture)): class, size, format (Texture2D's
//                           TextureFormat, RenderTexture's RenderTextureFormat), mip levels, the bytes its format and
//                           size take in the GPU's memory, readable or not, and name ("[dump] texture ..."), then the
//                           totals, the meshes (count, vertices, vertex and index bytes, how many readable, those of
//                           1000 vertices or more by name), the loaded AudioClips by load type, every object Unity
//                           holds counted by class, and Unity's own totals (the Profiler's allocated and reserved
//                           memory where a release player answers them, the texture manager's count, the async
//                           upload buffer): what a scene's memory is (a memory census)
//   UA_DUMP_MEMORY=<s>[,<s>...]  the same census without a line per texture, mesh or clip; with the word req among the
//                           times, last (UA_DUMP_MEMORY=req, UA_DUMP_TEXTURES=60,req), also whenever a file census.req appears in
//                           the game's folder (looked for every 30 frames; removed): a bench's plan step asks for one
//   UA_DUMP_INPUT=<name>[,<name>...]  what the game's scripts read from Unity's input (the Input Manager's, "legacy"),
//                           asked after every frame on UnityMain and logged when it crosses half travel:
//                           Input.GetAxisRaw(<name>), or with key:<name> Input.GetKey(<name>) (up to 8, e.g.
//                           "joystick 1 analog 4,key:joystick 1 button 0"; "[dump] input <s> f<frames shown>: "<name>"
//                           <old> -> <new>", the clock of UA_INPUT_LOG's lines: how long after the host sent a
//                           press the game could read it, and whether every one arrived). A name Unity refuses
//                           is dropped with a line. It makes a managed string per name and frame: a test aid.
//   UA_UNLOAD_ASSETS=<name>[|<name>...]  (a memory setting) those loaded Textures and Fonts given back with
//                           Resources.UnloadAsset after the first frames and after each new active scene; Unity loads
//                           them again from disk if anything uses them (unload_assets below)
//   UA_ASYNC_UPLOAD=<MB>[,transient]  (a memory setting, not a dump) Unity's async upload ring buffer: its size
//                           (QualitySettings.asyncUploadBufferSize) and, with "transient", given back when idle
//                           (asyncUploadPersistentBuffer false), set once after the first frame through Unity's setters
// Everything else is read through public UnityEngine properties (Object.FindObjectsOfType and getters), on
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
#include "memledger.h"

#include "frames.h"
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
// Mono 2.6: struct MonoArray { MonoObject obj (vtable, synchronisation); MonoArrayBounds *bounds; uint32 max_length; ... }
static uintptr_t array_length(MonoArray *a) { return m_array_length ? m_array_length(a) : (uintptr_t)*(uint32_t *)((char *)a + 3 * sizeof(void *)); }
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
    L(m_type_get_object, "mono_type_get_object"); L(m_array_addr, "mono_array_addr_with_size");
    // Mono 2.6 (Unity 2017.x) has mono_array_length only as a macro: its MonoArray's max_length, read below
    *(void **)&m_array_length = so_dlsym(h, "mono_array_length");
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
    for (uintptr_t i = 0, nc = cams ? array_length(cams) : 0; i < nc; i++) {
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
    uintptr_t n = array_length(all), shown = 0;
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
            // the material's own _Color too (Material.color): PlayMaker faders tint the material, not the renderer
            float mc[4] = { -1, -1, -1, -1 };
            getf(call(r, "get_sharedMaterial", NULL, 0), "get_color", mc, 4);
            snprintf(extra, sizeof extra, " colour (%.2f, %.2f, %.2f, %.2f) material colour (%.2f, %.2f, %.2f, %.2f) sprite %s",
                     c[0], c[1], c[2], c[3], mc[0], mc[1], mc[2], mc[3], sp);
        } else if (!strcmp(cls, "MeshRenderer") && mf_type) {
            void *a1[1] = { mf_type };
            MonoObject *mf = call(go, "GetComponent", a1, 1);
            MonoObject *mesh = mf ? call(mf, "get_sharedMesh", NULL, 0) : NULL;
            MonoArray *cols = mesh ? (MonoArray *)call(mesh, "get_colors32", NULL, 0) : NULL;
            char mn[96]; name_of(mesh, mn, sizeof mn);
            if (cols && array_length(cols) > 0) {
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
    n = array_length(all);
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

// the named GameObject's and the main camera's positions, one write (another thread's [hitch] line must not land inside it)
// a texture level's bytes in the GPU's memory: Unity's TextureFormat (Texture2D) numbers; -1 for one not known here
static long long tex_level_bytes(int fmt, int w, int h)
{
    int bw = 1, bh = 1, bb;                 // block width, height (texels), bytes
    switch (fmt) {
    case 1: case 63: case 75: bb = 1; break;                                         // Alpha8, R8, R8_SIGNED
    case 2: case 7: case 9: case 13: case 15: case 21: case 62: case 76: case 79: bb = 2; break;
    case 3: case 77: bb = 3; break;                                                  // RGB24
    case 4: case 5: case 14: case 16: case 18: case 22: case 72: case 78: case 80: bb = 4; break;   // RGBA32, ARGB32, BGRA32, ...
    case 73: case 81: bb = 6; break;
    case 17: case 19: case 74: case 82: bb = 8; break;                               // RGBAHalf, RGFloat, RGBA64
    case 20: bb = 16; break;                                                          // RGBAFloat
    case 10: case 26: case 28: case 34: case 41: case 42: case 45: case 46: case 64: bw = bh = 4; bb = 8; break;   // DXT1, BC4, ETC, ETC2 RGB, EAC R
    case 12: case 24: case 25: case 27: case 29: case 43: case 44: case 47: case 65: bw = bh = 4; bb = 16; break;  // DXT5, BC6H, BC7, BC5, EAC RG, ETC2 RGBA8
    case 48: case 54: case 66: bw = bh = 4; bb = 16; break;                          // ASTC 4x4 (LDR, the old RGBA name, HDR)
    case 49: case 55: case 67: bw = bh = 5; bb = 16; break;
    case 50: case 56: case 68: bw = bh = 6; bb = 16; break;
    case 51: case 57: case 69: bw = bh = 8; bb = 16; break;
    case 52: case 58: case 70: bw = bh = 10; bb = 16; break;
    case 53: case 59: case 71: bw = bh = 12; bb = 16; break;
    default: return -1;
    }
    return (long long)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bb;
}
// a RenderTexture's bytes a texel: Unity's RenderTextureFormat numbers
static int rt_texel_bytes(int fmt)
{
    switch (fmt) {
    case 16: return 1;                                                               // R8
    case 4: case 5: case 6: case 15: case 25: case 28: return 2;                     // RGB565, ARGB4444, ARGB1555, RHalf, RG16, R16
    case 2: case 9: case 10: case 12: case 18: case 24: case 26: return 8;           // ARGBHalf, DefaultHDR, ARGB64, RGFloat, RGInt, RGBAUShort
    case 11: case 17: return 16;                                                     // ARGBFloat, ARGBInt
    default: return 4;                                                               // ARGB32, Depth, Shadowmap, Default, BGRA32, RGHalf, ...
    }
}
static long long call_i64(MonoClass *k, const char *name)   // a static method or getter of no arguments that returns a long / ulong
{
    MonoMethod *m = k ? m_method_from_name(k, name, 0) : NULL;
    MonoObject *exc = NULL, *r = m ? m_invoke(m, NULL, NULL, &exc) : NULL;
    long long v = -1;
    if (r && !exc) memcpy(&v, m_unbox(r), sizeof v);
    return v;
}
static int call_i32(MonoClass *k, const char *name)        // ... an int, uint or bool (a bool's other bytes are 0)
{
    MonoMethod *m = k ? m_method_from_name(k, name, 0) : NULL;
    MonoObject *exc = NULL, *r = m ? m_invoke(m, NULL, NULL, &exc) : NULL;
    if (!r || exc) return -1;
    const char *cn = m_class_get_name(m_object_get_class(r));
    if (!strcmp(cn, "Boolean")) return *(unsigned char *)m_unbox(r);
    int v; memcpy(&v, m_unbox(r), sizeof v);
    return v;
}
#define MB(x) ((double)(x) / (1 << 20))

// UA_DUMP_TEXTURES / UA_DUMP_MEMORY: what Unity holds, as a script can read it. list: every texture by name too
static void census(int list)
{
    if (!load()) return;
    MonoDomain *dom = m_domain_get();
    MonoImage *core = m_image_loaded("UnityEngine.CoreModule");
    if (!core) core = m_image_loaded("UnityEngine");
    MonoClass *k_res = core ? m_class_from_name(core, "UnityEngine", "Resources") : NULL;
    MonoClass *k_tex = core ? m_class_from_name(core, "UnityEngine", "Texture") : NULL;
    MonoClass *k_obj = core ? m_class_from_name(core, "UnityEngine", "Object") : NULL;
    MonoMethod *all = k_res ? m_method_from_name(k_res, "FindObjectsOfTypeAll", 1) : NULL;
    if (!k_tex || !all) { fprintf(stderr, "[dump] Resources.FindObjectsOfTypeAll(Type) or Texture not found\n"); return; }
    // Unity's own totals (the Profiler's, where a release player answers them), the texture manager's, the upload buffer
    MonoClass *k_prof = m_class_from_name(core, "UnityEngine.Profiling", "Profiler");
    MonoClass *k_q = m_class_from_name(core, "UnityEngine", "QualitySettings");
    int prof_ok = call_i32(k_prof, "get_supported");
    fprintf(stderr, "[dump] unity memory: allocated %.1f MB, reserved %.1f MB (unused %.1f), mono heap %.1f MB (used %.1f), temp allocator %.1f MB, "
            "graphics driver %.1f MB, profiler supported %d\n",
            MB(call_i64(k_prof, "GetTotalAllocatedMemoryLong")), MB(call_i64(k_prof, "GetTotalReservedMemoryLong")),
            MB(call_i64(k_prof, "GetTotalUnusedReservedMemoryLong")), MB(call_i64(k_prof, "GetMonoHeapSizeLong")),
            MB(call_i64(k_prof, "GetMonoUsedSizeLong")), MB((unsigned)call_i32(k_prof, "GetTempAllocatorSize")),
            MB(call_i64(k_prof, "GetAllocatedMemoryForGraphicsDriver")), prof_ok);
    fprintf(stderr, "[dump] texture memory (Unity's count): current %.1f MB, total %.1f MB, non-streaming %.1f MB, desired %.1f MB; "
            "async upload buffer %d MB, persistent %d, time slice %d ms; mipmap limit %d\n",
            MB(call_i64(k_tex, "get_currentTextureMemory")), MB(call_i64(k_tex, "get_totalTextureMemory")),
            MB(call_i64(k_tex, "get_nonStreamingTextureMemory")), MB(call_i64(k_tex, "get_desiredTextureMemory")),
            call_i32(k_q, "get_asyncUploadBufferSize"), call_i32(k_q, "get_asyncUploadPersistentBuffer"),
            call_i32(k_q, "get_asyncUploadTimeSlice"), call_i32(k_q, "get_globalTextureMipmapLimit"));
    MonoMethod *rtsize = k_prof && prof_ok > 0 ? m_method_from_name(k_prof, "GetRuntimeMemorySizeLong", 1) : NULL;

    // the textures: estimated bytes in the GPU's memory, and the CPU copies of readable ones
    void *args[1] = { m_type_get_object(dom, m_class_get_type(k_tex)) };
    MonoObject *exc = NULL;
    MonoArray *arr = (MonoArray *)m_invoke(all, NULL, args, &exc);
    if (!arr || exc) { fprintf(stderr, "[dump] FindObjectsOfTypeAll failed\n"); return; }
    uintptr_t n = array_length(arr);
    long long t2d = 0, t2d_read = 0, rt = 0, other = 0, unknown = 0; int n2d = 0, nread = 0, nrt = 0, nrt_live = 0, nother = 0;
    for (uintptr_t i = 0; i < n; i++) {
        MonoObject *t = *(MonoObject **)m_array_addr(arr, sizeof(void *), i);
        if (!t) continue;
        const char *cls = m_class_get_name(m_object_get_class(t));
        int w = 0, h = 0, fmt = -1, mips = 1, readable = 0;
        char nm[96];
        geti(t, "get_width", &w); geti(t, "get_height", &h);
        geti(t, "get_format", &fmt);                  // Texture2D: TextureFormat; RenderTexture: RenderTextureFormat
        geti(t, "get_mipmapCount", &mips);
        MonoObject *rd = call(t, "get_isReadable", NULL, 0);
        if (rd) readable = *(unsigned char *)m_unbox(rd);
        long long bytes = 0;
        char extra[96] = "";
        if (!strcmp(cls, "RenderTexture")) {
            int depth = 0, aa = 1, mip = 0, created = 0;
            geti(t, "get_depth", &depth); geti(t, "get_antiAliasing", &aa);
            MonoObject *o = call(t, "get_useMipMap", NULL, 0); if (o) mip = *(unsigned char *)m_unbox(o);
            o = call(t, "IsCreated", NULL, 0); if (o) created = *(unsigned char *)m_unbox(o);
            bytes = (long long)w * h * (rt_texel_bytes(fmt) + (depth ? (depth > 16 ? 4 : 2) : 0)) * (aa > 1 ? aa : 1);
            if (mip) bytes = bytes * 4 / 3;
            if (!created) bytes = 0;
            nrt++; nrt_live += created; rt += bytes;
            snprintf(extra, sizeof extra, " depth %d msaa %d%s%s", depth, aa, mip ? " mips" : "", created ? "" : " (not created)");
        } else {
            int layers = 1;
            if (!strcmp(cls, "Cubemap")) layers = 6;
            else if (!strcmp(cls, "Texture2DArray") || !strcmp(cls, "Texture3D")) geti(t, "get_depth", &layers);
            int lw = w, lh = h;
            for (int l = 0; l < (mips > 0 ? mips : 1); l++) {
                long long b = tex_level_bytes(fmt, lw, lh);
                if (b < 0) { bytes = -1; break; }
                bytes += b * layers;
                lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
            }
            if (bytes < 0) { unknown++; bytes = 0; }
            if (!strcmp(cls, "Texture2D")) { n2d++; t2d += bytes; if (readable) { nread++; t2d_read += bytes; } }
            else { nother++; other += bytes; }
        }
        if (list) {
            name_of(t, nm, sizeof nm);
            long long rsz = -1;
            if (rtsize) { void *a1[1] = { t }; MonoObject *e2 = NULL, *r = m_invoke(rtsize, NULL, a1, &e2); if (r && !e2) memcpy(&rsz, m_unbox(r), sizeof rsz); }
            char rs[48] = "";
            if (rsz >= 0) snprintf(rs, sizeof rs, " (Unity: %.2f MB)", MB(rsz));
            fprintf(stderr, "[dump] texture %s %dx%d fmt %d mips %d %.2f MB%s%s%s %s\n", cls, w, h, fmt, mips, MB(bytes), rs,
                    readable ? " readable" : "", extra, nm[0] ? nm : "-");
        }
    }
    fprintf(stderr, "[dump] textures: %lu; Texture2D %d, %.1f MB (readable %d, %.1f MB more in the CPU's memory); RenderTexture %d (%d created), %.1f MB; "
            "others %d, %.1f MB; %lld of formats not counted\n",
            (unsigned long)n, n2d, MB(t2d), nread, MB(t2d_read), nrt, nrt_live, MB(rt), nother, MB(other), unknown);
    // the meshes: vertex and index bytes (the GPU's), the CPU copies of readable ones
    MonoClass *k_mesh = m_class_from_name(core, "UnityEngine", "Mesh");
    if (k_mesh) {
        args[0] = m_type_get_object(dom, m_class_get_type(k_mesh));
        exc = NULL;
        arr = (MonoArray *)m_invoke(all, NULL, args, &exc);
        n = arr && !exc ? array_length(arr) : 0;
        long long verts = 0, big = 0, mbytes = 0, rbytes = 0; int readable = 0;
        for (uintptr_t i = 0; i < n; i++) {
            MonoObject *m = *(MonoObject **)m_array_addr(arr, sizeof(void *), i);
            int v = 0, r = 0, streams = 0, subs = 0, ifmt = 0;
            if (!m || !geti(m, "get_vertexCount", &v)) continue;
            verts += v;
            MonoObject *rd = call(m, "get_isReadable", NULL, 0);
            if (rd && *(unsigned char *)m_unbox(rd)) r = 1;
            readable += r;
            geti(m, "get_vertexBufferCount", &streams); geti(m, "get_subMeshCount", &subs); geti(m, "get_indexFormat", &ifmt);
            long long b = 0;
            for (int s = 0; s < streams && s < 8; s++) {
                int st = 0; void *a1[1] = { &s };
                MonoObject *o = call(m, "GetVertexBufferStride", a1, 1);
                if (o) memcpy(&st, m_unbox(o), sizeof st);
                b += (long long)st * v;
            }
            for (int s = 0; s < subs && s < 256; s++) {
                unsigned ic = 0; void *a1[1] = { &s };
                MonoObject *o = call(m, "GetIndexCount", a1, 1);
                if (o) memcpy(&ic, m_unbox(o), sizeof ic);
                b += (long long)ic * (ifmt ? 4 : 2);
            }
            mbytes += b; if (r) rbytes += b;
            if (v >= 1000 && list) {
                char nm[96]; name_of(m, nm, sizeof nm);
                fprintf(stderr, "[dump] mesh %d vertices %.2f MB%s %s\n", v, MB(b), r ? " readable" : "", nm[0] ? nm : "-");
            }
            if (v >= 1000) big += v;
        }
        fprintf(stderr, "[dump] meshes: %lu, %lld vertices (%lld in meshes of 1000 or more), %.1f MB of vertices and indices; %d readable, %.1f MB of them\n",
                (unsigned long)n, verts, big, MB(mbytes), readable, MB(rbytes));
    }
    // the sounds: how each AudioClip is loaded; decompressed on load = 16-bit PCM in FMOD's memory
    MonoImage *audio = m_image_loaded("UnityEngine.AudioModule");
    MonoClass *k_clip = audio ? m_class_from_name(audio, "UnityEngine", "AudioClip") : NULL;
    if (k_clip) {
        args[0] = m_type_get_object(dom, m_class_get_type(k_clip));
        exc = NULL;
        arr = (MonoArray *)m_invoke(all, NULL, args, &exc);
        n = arr && !exc ? array_length(arr) : 0;
        int cnt[3] = { 0 }, loaded[3] = { 0 };
        long long pcm = 0, secs[3] = { 0 };
        for (uintptr_t i = 0; i < n; i++) {
            MonoObject *c = *(MonoObject **)m_array_addr(arr, sizeof(void *), i);
            int lt = -1, st = 0, samples = 0, ch = 0, fq = 0;
            if (!c || !geti(c, "get_loadType", &lt) || lt < 0 || lt > 2) continue;
            geti(c, "get_loadState", &st); geti(c, "get_samples", &samples); geti(c, "get_channels", &ch); geti(c, "get_frequency", &fq);
            cnt[lt]++;
            if (st == 2) { loaded[lt]++; if (fq > 0) secs[lt] += samples / fq; if (lt == 0) pcm += (long long)samples * ch * 2; }
            if (list && st == 2) {
                char nm[96]; name_of(c, nm, sizeof nm);
                fprintf(stderr, "[dump] clip %s load type %d %d Hz %d ch %.1f s%s\n", nm, lt, fq, ch, fq > 0 ? (double)samples / fq : 0.0,
                        lt == 0 ? "" : lt == 1 ? " (compressed in memory)" : " (streamed)");
            }
        }
        fprintf(stderr, "[dump] sounds: %lu clips; decompressed on load %d (%d loaded, %lld s, %.1f MB of PCM), compressed in memory %d (%d loaded, %lld s), "
                "streamed %d (%d loaded, %lld s)\n", (unsigned long)n, cnt[0], loaded[0], secs[0], MB(pcm), cnt[1], loaded[1], secs[1],
                cnt[2], loaded[2], secs[2]);
    }
    // every object Unity holds, by class (with GetRuntimeMemorySizeLong's bytes where the player answers it)
    if (k_obj) {
        args[0] = m_type_get_object(dom, m_class_get_type(k_obj));
        exc = NULL;
        arr = (MonoArray *)m_invoke(all, NULL, args, &exc);
        n = arr && !exc ? array_length(arr) : 0;
        enum { NK = 1024 };
        static MonoClass *ks[NK]; static int kc[NK]; static long long kb[NK];
        int nk = 0;
        memset(kc, 0, sizeof kc); memset(kb, 0, sizeof kb);
        for (uintptr_t i = 0; i < n; i++) {
            MonoObject *o = *(MonoObject **)m_array_addr(arr, sizeof(void *), i);
            if (!o) continue;
            MonoClass *k = m_object_get_class(o);
            int j = 0;
            while (j < nk && ks[j] != k) j++;
            if (j == nk) { if (nk == NK) continue; ks[nk++] = k; }
            kc[j]++;
            if (rtsize) { void *a1[1] = { o }; MonoObject *e2 = NULL, *r = m_invoke(rtsize, NULL, a1, &e2); long long b = 0; if (r && !e2) memcpy(&b, m_unbox(r), sizeof b); kb[j] += b; }
        }
        // the 40 biggest classes by count (by bytes where known)
        char line[4096]; int len = snprintf(line, sizeof line, "[dump] objects: %lu of %d classes:", (unsigned long)n, nk);
        for (int shown = 0; shown < 40; shown++) {
            int best = -1;
            for (int j = 0; j < nk; j++) if (kc[j] > 0 && (best < 0 || (rtsize ? kb[j] > kb[best] : kc[j] > kc[best]))) best = j;
            if (best < 0) break;
            if (len < (int)sizeof line - 80)
                len += rtsize ? snprintf(line + len, sizeof line - len, " %s %d (%.1f MB)", m_class_get_name(ks[best]), kc[best], MB(kb[best]))
                              : snprintf(line + len, sizeof line - len, " %s %d", m_class_get_name(ks[best]), kc[best]);
            kc[best] = -kc[best];
        }
        fprintf(stderr, "%s\n", line);
    }
    memledger_report();                     // who mapped what (UA_MEM_LEDGER), glibc's malloc arenas
}

static void hero(const char *name, double t)
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
    fprintf(stderr, "[dump] t %.2f %s %s camera %s\n", t, name, a, b);
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

// UA_HERO_PLACE: once, the Knight's transform at (x, y) a few seconds after it first exists
static void hero_place(void)
{
    static int state = -1;                  // -1 not read, 0 off or done, 1 waiting for the Knight
    static double x, y, delay = 3, seen;
    static MonoMethod *find;
    if (!state) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    if (state < 0) {
        const char *e = getenv("UA_HERO_PLACE");
        char *p;
        state = 0;
        if (!e || !*e) return;
        x = strtod(e, &p); if (*p != ',') return;
        y = strtod(p + 1, &p); if (*p == ',') delay = strtod(p + 1, NULL);
        state = 1;
        return;
    }
    if (!find) {
        if (!load()) return;
        MonoImage *core = m_image_loaded("UnityEngine.CoreModule");
        MonoClass *k_go = core ? m_class_from_name(core, "UnityEngine", "GameObject") : NULL;
        find = k_go ? m_method_from_name(k_go, "Find", 1) : NULL;
        if (!find) return;
    }
    MonoObject *exc = NULL;
    void *args[1] = { m_string_new(m_domain_get(), "Knight") };
    MonoObject *go = m_invoke(find, NULL, args, &exc);
    if (exc || !go) { seen = 0; return; }
    // (during a load the Knight waits far below the scene, y -2000, and the game puts it at the respawn point
    // after: placed only once it has stood in the scene for the delay)
    MonoObject *t = call(go, "get_transform", NULL, 0);
    float v[3] = { 0 };
    if (!t || !getf(t, "get_position", v, 3) || v[1] < -500) { seen = 0; return; }
    if (!seen) { seen = now; return; }
    if (now - seen < delay) return;
    float to[3] = { (float)x, (float)y, v[2] };
    void *a1[1] = { to };
    call(t, "set_position", a1, 1);
    fprintf(stderr, "[dump] Knight placed at (%.2f, %.2f), was (%.2f, %.2f) (UA_HERO_PLACE)\n", to[0], to[1], v[0], v[1]);
    state = 0;
}

// UA_DUMP_OFF: components of the named classes turned off, every 5 s while the Knight exists
static void comps_off(void)
{
    static int state = -1;
    static double last;
    static char spec[512];
    if (!state) return;
    if (state < 0) {
        const char *e = getenv("UA_DUMP_OFF");
        state = e && *e ? 1 : 0;
        if (state) snprintf(spec, sizeof spec, "%s", e);
        return;
    }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    static double every = 5;
    if (now - last < every) return;
    last = now;
    if (!load()) return;
    MonoImage *core = m_image_loaded("UnityEngine.CoreModule");
    MonoClass *k_go = core ? m_class_from_name(core, "UnityEngine", "GameObject") : NULL, *k_obj = core ? m_class_from_name(core, "UnityEngine", "Object") : NULL;
    MonoMethod *gfind = k_go ? m_method_from_name(k_go, "Find", 1) : NULL, *find = k_obj ? m_method_from_name(k_obj, "FindObjectsOfType", 1) : NULL;
    if (!gfind || !find) return;
    MonoObject *exc = NULL;
    void *ka[1] = { m_string_new(m_domain_get(), "Knight") };
    if (!m_invoke(gfind, NULL, ka, &exc) || exc) return;
    char list[512], *sv = NULL;
    snprintf(list, sizeof list, "%s", spec);
    for (char *t = strtok_r(list, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
        char *colon = strchr(t, ':');
        if (!colon) continue;
        *colon = 0;
        char *cls = colon + 1, *at = strchr(cls, '@'), *go = NULL;
        if (at) { *at = 0; go = at + 1; }                 // <class>@<GameObject name>: only those components
        char *dot = strrchr(cls, '.'), ns[256] = "";
        if (dot) { snprintf(ns, sizeof ns, "%.*s", (int)(dot - cls), cls); cls = dot + 1; }
        MonoImage *img = m_image_loaded(t);
        MonoClass *k = img ? m_class_from_name(img, ns, cls) : NULL;
        if (!k) { fprintf(stderr, "[dump] off: no class %s:%s.%s\n", t, ns, cls); continue; }
        void *args[1] = { m_type_get_object(m_domain_get(), m_class_get_type(k)) };
        MonoArray *all = (MonoArray *)m_invoke(find, NULL, args, &exc);
        if (!all || exc) continue;
        uintptr_t n = array_length(all), off = 0;
        for (uintptr_t i = 0; i < n; i++) {
            MonoObject *c = *(MonoObject **)m_array_addr(all, sizeof(void *), i);
            MonoObject *en = c ? call(c, "get_enabled", NULL, 0) : NULL;
            if (!en || !*(unsigned char *)m_unbox(en)) continue;
            if (go) {
                MonoObject *g = call(c, "get_gameObject", NULL, 0), *nm = g ? call(g, "get_name", NULL, 0) : NULL;
                char *u = nm ? m_string_to_utf8((MonoString *)nm) : NULL;
                int same = u && !strcmp(u, go);
                if (u) m_free(u);
                if (!same) continue;
            }
            unsigned char no = 0; void *a1[1] = { &no };
            call(c, "set_enabled", a1, 1);
            off++;
        }
        if (off) { fprintf(stderr, "[dump] off: %lu %s (of %lu)\n", (unsigned long)off, cls, (unsigned long)n); every = 60; }   // (the search is a hitch: rarely once found)
    }
}

// UA_DUMP_OFF_Z: the scenery outside a band of depths turned off, every 5 s while the Knight exists
static void scenery_off(void)
{
    static int state = -1, whole;
    static double last, every = 5;
    static float zn, zf;
    if (!state) return;
    if (state < 0) {
        const char *e = getenv("UA_DUMP_OFF_Z");
        char *p;
        state = 0;
        if (!e || !*e) return;
        zn = strtof(e, &p);
        if (*p != ',') { fprintf(stderr, "[dump] off z: UA_DUMP_OFF_Z=<near>,<far>[,go]\n"); return; }
        zf = strtof(p + 1, &p);
        whole = !strcmp(p, ",go");
        state = 1;
        return;
    }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    if (now - last < every) return;
    last = now;
    if (!load()) return;
    MonoImage *core = m_image_loaded("UnityEngine.CoreModule");
    MonoClass *k_go = core ? m_class_from_name(core, "UnityEngine", "GameObject") : NULL, *k_obj = core ? m_class_from_name(core, "UnityEngine", "Object") : NULL;
    MonoClass *k_cam = core ? m_class_from_name(core, "UnityEngine", "Camera") : NULL, *k_ren = core ? m_class_from_name(core, "UnityEngine", "Renderer") : NULL;
    MonoClass *k_scene = core ? m_class_from_name(core, "UnityEngine.SceneManagement", "Scene") : NULL;
    MonoMethod *gfind = k_go ? m_method_from_name(k_go, "Find", 1) : NULL, *find = k_obj ? m_method_from_name(k_obj, "FindObjectsOfType", 1) : NULL;
    MonoMethod *get_main = k_cam ? m_method_from_name(k_cam, "get_main", 0) : NULL, *all_cams = k_cam ? m_method_from_name(k_cam, "get_allCameras", 0) : NULL;
    MonoMethod *scene_name = k_scene ? m_method_from_name(k_scene, "get_name", 0) : NULL;
    if (!gfind || !find || !get_main || !all_cams || !k_ren || !scene_name) { fprintf(stderr, "[dump] off z: UnityEngine classes not found\n"); state = 0; return; }
    MonoObject *exc = NULL;
    void *ka[1] = { m_string_new(m_domain_get(), "Knight") };
    if (!m_invoke(gfind, NULL, ka, &exc) || exc) return;
    // the layers only the main camera draws (not the HUD's, not an effect camera's)
    MonoObject *cam = m_invoke(get_main, NULL, NULL, &exc);
    if (!cam || exc) return;
    int only = 0; geti(cam, "get_cullingMask", &only);
    MonoArray *cams = (MonoArray *)m_invoke(all_cams, NULL, NULL, &exc);
    for (uintptr_t i = 0, nc = cams && !exc ? array_length(cams) : 0; i < nc; i++) {
        MonoObject *c = *(MonoObject **)m_array_addr(cams, sizeof(void *), i);
        int mask = 0;
        if (c != cam && geti(c, "get_cullingMask", &mask)) only &= ~mask;
    }
    void *args[1] = { m_type_get_object(m_domain_get(), m_class_get_type(k_ren)) };
    MonoArray *all = (MonoArray *)m_invoke(find, NULL, args, &exc);
    if (!all || exc) return;
    uintptr_t n = array_length(all), off = 0, scenery = 0;
    for (uintptr_t i = 0; i < n; i++) {
        MonoObject *r = *(MonoObject **)m_array_addr(all, sizeof(void *), i);
        MonoObject *en = r ? call(r, "get_enabled", NULL, 0) : NULL;
        if (!en || !*(unsigned char *)m_unbox(en)) continue;
        MonoObject *go = call(r, "get_gameObject", NULL, 0);
        int layer = -1;
        if (!go || !geti(go, "get_layer", &layer) || layer < 0 || !((unsigned)only >> layer & 1)) continue;
        MonoObject *sc = call(go, "get_scene", NULL, 0);          // (a boxed Scene: its name through the value itself)
        MonoObject *sn = sc ? m_invoke(scene_name, m_unbox(sc), NULL, &exc) : NULL;
        char scn[64]; str(exc ? NULL : sn, scn, sizeof scn); exc = NULL;
        if (!sn || !strcmp(scn, "DontDestroyOnLoad")) continue;
        scenery++;
        float pos[3] = { 0 };
        if (!getf(call(r, "get_transform", NULL, 0), "get_position", pos, 3) || (pos[2] >= zn && pos[2] <= zf)) continue;
        unsigned char no = 0; void *a1[1] = { &no };
        if (whole) call(go, "SetActive", a1, 1); else call(r, "set_enabled", a1, 1);
        off++;
    }
    if (off) {
        fprintf(stderr, "[dump] off z: %lu of the scenes' %lu renderers outside z %.2f .. %.2f %s (layers 0x%08x; %lu renderers in all)\n",
                (unsigned long)off, (unsigned long)scenery, zn, zf, whole ? "made inactive" : "turned off", (unsigned)only, (unsigned long)n);
        every = 60;
    }
}

// UA_DUMP_INPUT: what a script reads from UnityEngine.Input, logged when it crosses half travel
static int half(float v) { return v >= 0.5f ? 1 : v <= -0.5f ? -1 : 0; }
static void input_probe(void)
{
    enum { MAXN = 8 };
    static int state = -1, n;               // -1 not read, 0 off, 1 on
    static char names[MAXN][64];
    static float last[MAXN];
    static MonoMethod *axis, *key;
    if (!state) return;
    if (state < 0) {
        for (const char *p = getenv("UA_DUMP_INPUT"); p && *p && n < MAXN; ) {
            size_t l = strcspn(p, ",");
            if (l && l < sizeof names[0]) { memcpy(names[n], p, l); names[n++][l] = 0; }
            p += l; if (*p) p++;
        }
        state = n > 0;
        return;
    }
    if (!axis) {
        if (!load()) return;
        // (Unity 2019.1 and later: its own module; before: UnityEngine.CoreModule, UnityEngine.dll before 2017.2)
        MonoImage *img = m_image_loaded("UnityEngine.InputLegacyModule");
        if (!img) img = m_image_loaded("UnityEngine.CoreModule");
        MonoClass *k = img ? m_class_from_name(img, "UnityEngine", "Input") : NULL;
        if (!k && (img = m_image_loaded("UnityEngine"))) k = m_class_from_name(img, "UnityEngine", "Input");
        if (!k) return;                     // not loaded yet
        // (GetKey has a KeyCode and a string form; GetKeyString is what the string one calls: Input's own
        // before Unity 6, UnityEngine.Internal.InputUnsafeUtility's from it)
        axis = m_method_from_name(k, "GetAxisRaw", 1);
        key = m_method_from_name(k, "GetKeyString", 1);
        if (!key && img && (k = m_class_from_name(img, "UnityEngine.Internal", "InputUnsafeUtility"))) key = m_method_from_name(k, "GetKeyString", 1);
        if (!axis || !key) { fprintf(stderr, "[dump] input: UnityEngine.Input without GetAxisRaw or GetKeyString: off (UA_DUMP_INPUT)\n"); state = 0; return; }
    }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    MonoDomain *dom = m_domain_get();
    for (int i = 0; i < n; i++) {
        if (!names[i][0]) continue;
        int is_key = !strncmp(names[i], "key:", 4);
        void *args[1] = { m_string_new(dom, names[i] + (is_key ? 4 : 0)) };
        MonoObject *exc = NULL, *r = m_invoke(is_key ? key : axis, NULL, args, &exc);
        if (exc || !r) {
            fprintf(stderr, "[dump] input: Unity knows no %s \"%s\": dropped (UA_DUMP_INPUT)\n", is_key ? "key" : "axis", names[i] + (is_key ? 4 : 0));
            names[i][0] = 0;
            continue;
        }
        float v = is_key ? (float)(*(unsigned char *)m_unbox(r) != 0) : *(float *)m_unbox(r);
        if (half(v) == half(last[i])) continue;
        fprintf(stderr, "[dump] input %.3f f%lu: \"%s\" %g -> %g\n", now - frames_t_start(), frames_presented(), names[i], last[i], v);
        last[i] = v;
    }
}

// UA_ASYNC_UPLOAD=<MB>[,transient] | transient: Unity's ring buffer for asynchronous texture and mesh uploads
// (QualitySettings.asyncUploadBufferSize, MB; asyncUploadPersistentBuffer): its size, and with "transient" the buffer
// given back when no upload is pending instead of kept for the game's life. Set once from UnityMain after the first
// frame, through Unity's own setters; the uploads are the same, a bigger load may take more frames. A memory setting.
static void async_upload(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("UA_ASYNC_UPLOAD");
    if (!e || !*e || !load()) return;
    MonoImage *core = m_image_loaded("UnityEngine.CoreModule");
    MonoClass *k_q = core ? m_class_from_name(core, "UnityEngine", "QualitySettings") : NULL;
    if (!k_q) { fprintf(stderr, "[dump] UA_ASYNC_UPLOAD: QualitySettings not found\n"); return; }
    int was_mb = call_i32(k_q, "get_asyncUploadBufferSize"), was_p = call_i32(k_q, "get_asyncUploadPersistentBuffer");
    int mb = atoi(e);
    MonoObject *exc = NULL;
    if (mb > 0) {
        MonoMethod *m = m_method_from_name(k_q, "set_asyncUploadBufferSize", 1);
        void *a1[1] = { &mb };
        if (m) m_invoke(m, NULL, a1, &exc);
    }
    if (strstr(e, "transient")) {
        MonoMethod *m = m_method_from_name(k_q, "set_asyncUploadPersistentBuffer", 1);
        unsigned char no = 0; void *a1[1] = { &no };
        if (m) m_invoke(m, NULL, a1, &exc);
    }
    fprintf(stderr, "[dump] UA_ASYNC_UPLOAD: async upload buffer %d MB%s -> %d MB%s\n", was_mb, was_p > 0 ? " (persistent)" : "",
            call_i32(k_q, "get_asyncUploadBufferSize"), call_i32(k_q, "get_asyncUploadPersistentBuffer") > 0 ? " (persistent)" : " (transient)");
}

// UA_UNLOAD_ASSETS=<name>[|<name>...]: loaded Textures and Fonts of those names (exact) given back with Unity's
// Resources.UnloadAsset after the first frames and again after every change of the active scene (a scene's own references
// can load them again): their memory (a texture's GPU copy, a font's file data) is freed and Unity loads them from disk
// again if anything uses them later (its documented behaviour for an asset unloaded this way), so a game that needs them
// draws the same, after a load. One found loaded again after it was given back is in use and kept from then on (at most
// one reload for a game that shows it, never one at every scene change). After three scene changes in a row where
// none of them was loaded again the checks stop. For assets a game keeps loaded but shows only in some cases (another language's font
// atlases). Logged: "[dump] unloaded <class> <name> (<w>x<h>)". UA_UNLOAD_ASSETS_AT=<s> the first pass's time (default 8).
static char g_unloaded[2048] = "|";     // the names given back so far
static void note_unloaded(const char *nm)
{
    size_t l = strlen(g_unloaded);
    if (l + strlen(nm) + 2 < sizeof g_unloaded) snprintf(g_unloaded + l, sizeof g_unloaded - l, "%s|", nm);
}
static int unloaded_before(const char *nm)
{
    char key[132]; snprintf(key, sizeof key, "|%s|", nm);
    return strstr(g_unloaded, key) != NULL;
}
static void unload_assets(void)
{
    static int state = -1, frames, pending, quiet;
    static double t0, first = 8, due;
    static MonoMethod *unload, *find_all, *get_active, *scene_handle;
    static MonoObject *types[2];
    static int last_handle = -12345;
    static char names[2048];
    if (!state) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    if (state < 0) {
        const char *e = getenv("UA_UNLOAD_ASSETS");
        state = 0;
        if (!e || !*e || !load()) return;
        snprintf(names, sizeof names, "|%s|", e);
        const char *at = getenv("UA_UNLOAD_ASSETS_AT");
        if (at && atof(at) > 0) first = atof(at);
        MonoImage *core = m_image_loaded("UnityEngine.CoreModule"), *text = m_image_loaded("UnityEngine.TextRenderingModule");
        MonoClass *k_res = core ? m_class_from_name(core, "UnityEngine", "Resources") : NULL;
        MonoClass *k_tex = core ? m_class_from_name(core, "UnityEngine", "Texture") : NULL;
        MonoClass *k_font = text ? m_class_from_name(text, "UnityEngine", "Font") : NULL;
        MonoClass *k_sm = core ? m_class_from_name(core, "UnityEngine.SceneManagement", "SceneManager") : NULL;
        MonoClass *k_scene = core ? m_class_from_name(core, "UnityEngine.SceneManagement", "Scene") : NULL;
        unload = k_res ? m_method_from_name(k_res, "UnloadAsset", 1) : NULL;
        find_all = k_res ? m_method_from_name(k_res, "FindObjectsOfTypeAll", 1) : NULL;
        get_active = k_sm ? m_method_from_name(k_sm, "GetActiveScene", 0) : NULL;
        scene_handle = k_scene ? m_method_from_name(k_scene, "get_handle", 0) : NULL;
        if (!unload || !find_all || !k_tex) { fprintf(stderr, "[dump] UA_UNLOAD_ASSETS: Resources.UnloadAsset or FindObjectsOfTypeAll not found: off\n"); return; }
        MonoDomain *dom = m_domain_get();
        types[0] = m_type_get_object(dom, m_class_get_type(k_tex));
        types[1] = k_font ? m_type_get_object(dom, m_class_get_type(k_font)) : NULL;
        t0 = now; due = now + first; pending = 1; state = 1;
        return;
    }
    // the active scene, every 60 frames: a new one, a pass 2 s later
    if (get_active && scene_handle && ++frames % 60 == 0) {
        MonoObject *exc = NULL, *sc = m_invoke(get_active, NULL, NULL, &exc);
        if (sc && !exc) {
            MonoObject *h = m_invoke(scene_handle, m_unbox(sc), NULL, &exc);
            int hv = h && !exc ? *(int *)m_unbox(h) : 0;
            if (hv != last_handle) { last_handle = hv; if (now - t0 > first) { pending = 1; due = now + 2; } }
        }
    }
    if (!pending || now < due) return;
    pending = 0;
    int n_un = 0, n_kept = 0;
    for (int t = 0; t < 2; t++) {
        if (!types[t]) continue;
        void *args[1] = { types[t] };
        MonoObject *exc = NULL;
        MonoArray *arr = (MonoArray *)m_invoke(find_all, NULL, args, &exc);
        if (!arr || exc) continue;
        for (uintptr_t i = 0, n = array_length(arr); i < n; i++) {
            MonoObject *o = *(MonoObject **)m_array_addr(arr, sizeof(void *), i);
            char nm[128], key[132];
            if (!o) continue;
            name_of(o, nm, sizeof nm);
            snprintf(key, sizeof key, "|%s|", nm);
            char *at = nm[0] ? strstr(names, key) : NULL;
            if (!at) continue;
            // an asset that is loaded again after it was given back is in use (its language is the game's, or a scene
            // shows it): kept from now on, its name taken out of the list
            if (unloaded_before(nm)) {
                memmove(at + 1, at + strlen(key), strlen(at + strlen(key)) + 1);
                n_kept++;
                fprintf(stderr, "[dump] %s loaded again after it was given back: in use, kept\n", nm);
                continue;
            }
            int w = 0, h = 0;
            geti(o, "get_width", &w); geti(o, "get_height", &h);
            const char *cls = m_class_get_name(m_object_get_class(o));
            void *a1[1] = { o };
            exc = NULL;
            m_invoke(unload, NULL, a1, &exc);
            if (exc) fprintf(stderr, "[dump] UA_UNLOAD_ASSETS: unloading %s %s threw\n", cls, nm);
            else { n_un++; note_unloaded(nm); fprintf(stderr, "[dump] unloaded %s %s (%dx%d) at %.0f s\n", cls, nm, w, h, now - t0); }
        }
    }
    // passes that find none of them (after the first that gave some back) stop the checks: each walks every texture
    // and font Unity holds, a few ms on the main thread 2 s after every room change
    if (!n_un && !n_kept) {
        fprintf(stderr, "[dump] UA_UNLOAD_ASSETS: none of them loaded at %.0f s\n", now - t0);
        if (g_unloaded[1] && ++quiet >= 3) {
            fprintf(stderr, "[dump] UA_UNLOAD_ASSETS: none came back after 3 scene changes: no more checks\n");
            state = 0;
        }
    } else quiet = 0;
}

void monodump_frame(void)
{
    async_upload();
    unload_assets();
    flipbook_frame();
    hero_place();
    comps_off();
    scenery_off();
    input_probe();
    static int state = -1, n, next, tn, tnext, tlist, treq, treq_n;   // state -1: not read; 0: off or done; 1: waiting
    static double at[16], tat[16], t0, period, last;
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
        const char *te = getenv("UA_DUMP_TEXTURES");
        if (te && *te) tlist = 1; else te = getenv("UA_DUMP_MEMORY");   // (the totals only)
        if (te && strstr(te, "req")) treq = 1;
        for (char *p = (char *)te; p && *p && tn < 16; ) {
            double s = strtod(p, &p);
            if (s > 0) tat[tn++] = s;
            while (*p == ',' || *p == ' ') p++;
            if (*p && !(*p >= '0' && *p <= '9') && *p != '.') break;
        }
        const char *hv = getenv("UA_DUMP_HERO");
        if (hv) {
            char *p; period = strtod(hv, &p);
            snprintf(name, sizeof name, "%s", *p == ',' ? p + 1 : "Knight");
        }
        state = n || tn || period > 0 || treq ? 1 : 0; t0 = now;
        return;
    }
    if (period > 0 && now - last >= period) {
        last = now;
        hero(name, now - t0);
    }
    if (next < n && now - t0 >= at[next]) {
        fprintf(stderr, "[dump] at %.0f s\n", now - t0);
        dump();
        next++;
    }
    if (tnext < tn && now - t0 >= tat[tnext]) {
        fprintf(stderr, "[dump] %s at %.0f s\n", tlist ? "textures" : "memory", now - t0);
        census(tlist);
        tnext++;
    }
    // UA_DUMP_MEMORY=req: a census whenever census.req appears in the game's folder (a bench's plan step), the file removed
    if (treq && ++treq_n % 30 == 0 && access("census.req", F_OK) == 0) {
        unlink("census.req");
        fprintf(stderr, "[dump] %s at %.0f s (census.req)\n", tlist ? "textures" : "memory", now - t0);
        census(tlist);
    }
    if (next == n && tnext == tn && period <= 0 && !treq) state = 0;
}
