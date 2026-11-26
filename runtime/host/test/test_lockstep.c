// lockstep.c's check (runtime/host/test/plugins_test.sh), on stand-ins for Mono's embedding calls, a small
// scene (two scripts, their transforms) and the runtime's input tap:
//   off      no UA_LOCKSTEP: no hook, no tap, nothing asked of Mono
//   hash     the clock fixed after the first frame (captureDeltaTime 1/60, Random.InitState(seed)), Unity's
//            realtimeSinceStartup and Mono's clocks answered from the frame count; a frame's hash the same
//            while nothing changes; it changes with a number, a followed object's field, a string, a struct,
//            an array element, an engine object destroyed, a transform, enabled; it does not change with an
//            IntPtr, a collection's buckets, the order FindObjectsOfType gives; the changed class named (C lines)
//   dump     UA_LOCKSTEP_DUMP: every hashed value of that frame by object and field
//   record   events through the tap are held, given after the frame and written with it; their times the frame
//            clock's, counted from the uptime at start
//   replay   (after record, its file) the same events after the same frames, the pad's own dropped, quit after the last
//   nudge    UA_LOCKSTEP_NUDGE=Hero.hp@2+5: hp 5 more after frame 2, logged
//   settle   a thread named Loading.* busy 150 ms: the frame waits for it
#define _GNU_SOURCE
#include "../lockstep.h"
#include "../../jni/unityplayer.h"
#include "../../loader/so_loader.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define HDR (2 * sizeof(void *))

/* ---- a stand-in Mono --------------------------------------------------------------------- */
struct fclass;
struct ffield { const char *name; int type; uint32_t off; uint32_t flags; struct fclass *cls; };
struct fclass { const char *ns, *name; struct fclass *parent; struct ffield *f; int nf; int valuetype, vsize, rank, esize; struct fclass *elem; };
struct fmethod { struct fclass *k; const char *name; int n; int icall; };
enum { T_BOOL = 2, T_I4 = 8, T_R4 = 0xc, T_STR = 0xe, T_VT = 0x11, T_CLASS = 0x12, T_I = 0x18, T_SZARRAY = 0x1d };
#define P sizeof(void *)
static struct fclass c_object = { "System", "Object" }, c_int = { "System", "Int32", 0, 0, 0, 1, 4 }, c_string = { "System", "String", &c_object };
static struct ffield f_int[] = { { "m_value", T_I4, HDR, 0 } };
static struct fclass c_uobj = { "UnityEngine", "Object", &c_object }, c_comp = { "UnityEngine", "Component", &c_uobj },
    c_beh = { "UnityEngine", "Behaviour", &c_comp }, c_mb = { "UnityEngine", "MonoBehaviour", &c_beh }, c_tr = { "UnityEngine", "Transform", &c_comp },
    c_time = { "UnityEngine", "Time", &c_object }, c_rand = { "UnityEngine", "Random", &c_object },
    c_sm = { "UnityEngine.SceneManagement", "SceneManager", &c_object }, c_scene = { "UnityEngine.SceneManagement", "Scene", 0, 0, 0, 1, 4 },
    c_go = { "UnityEngine", "GameObject", &c_uobj }, c_delegate = { "System", "Delegate", &c_object }, c_type = { "System", "Type", &c_object };
static struct ffield f_uobj[] = { { "m_CachedPtr", T_I, HDR, 0 } };
static struct ffield f_vec[] = { { "x", T_R4, HDR, 0 }, { "y", T_R4, HDR + 4, 0 }, { "z", T_R4, HDR + 8, 0 } };
static struct fclass c_vec = { "UnityEngine", "Vector3", 0, f_vec, 3, 1, 12 };
static struct ffield f_data[] = { { "x", T_I4, HDR, 0 }, { "label", T_STR, HDR + 8, 0 } };
static struct fclass c_data = { "", "Data", &c_object, f_data, 2 };
// Dictionary`2 and its nested Entry (no namespace of its own): hashCode left out, value hashed
static struct ffield f_entry[] = { { "hashCode", T_I4, HDR, 0 }, { "value", T_I4, HDR + 4, 0 } };
static struct fclass c_entry = { "", "Entry", 0, f_entry, 2, 1, 8 };
static struct ffield f_dict[] = { { "buckets", T_SZARRAY, HDR, 0 }, { "count", T_I4, HDR + P, 0 }, { "entry", T_VT, HDR + P + 8, 0, &c_entry } };
static struct fclass c_dict = { "System.Collections.Generic", "Dictionary`2", &c_object, f_dict, 3 };
static struct fclass c_intarr = { "System", "Int32[]", &c_object, 0, 0, 0, 0, 1, 4, &c_int };
// Hero : MonoBehaviour
#define HO(i) (HDR + P + 8 * (i))
static struct ffield f_hero[] = {
    { "hp", T_I4, HO(0), 0 }, { "speed", T_R4, HO(1), 0 }, { "name", T_STR, HO(2), 0 }, { "vel", T_VT, HO(3), 0, &c_vec },
    { "data", T_CLASS, HO(5), 0, &c_data }, { "arr", T_SZARRAY, HO(6), 0, &c_intarr }, { "native", T_I, HO(7), 0 },
    { "target", T_CLASS, HO(8), 0, &c_go }, { "dict", T_CLASS, HO(9), 0, &c_dict }, { "counter", T_I4, HO(10), 0x10 /* static */ },
};
static struct fclass c_hero = { "", "Hero", &c_mb, f_hero, 10 }, c_cam = { "", "CameraRig", &c_mb };
static struct fclass *classes[] = { &c_object, &c_int, &c_string, &c_uobj, &c_comp, &c_beh, &c_mb, &c_tr, &c_time, &c_rand, &c_sm, &c_scene,
                                    &c_go, &c_delegate, &c_type, &c_vec, &c_data, &c_dict, &c_hero, &c_cam, NULL };
static struct fmethod methods[] = {
    { &c_uobj, "FindObjectsOfType", 1 }, { &c_time, "get_time", 0 }, { &c_time, "set_captureDeltaTime", 1 }, { &c_rand, "get_state", 0 },
    { &c_rand, "InitState", 1 }, { &c_sm, "GetActiveScene", 0 }, { &c_sm, "get_sceneCount", 0 }, { &c_scene, "get_name", 0 },
    { &c_comp, "get_transform", 0 }, { &c_beh, "get_enabled", 0 }, { &c_tr, "get_localPosition_Injected", 1, 1 },
    { &c_tr, "get_localRotation", 0 }, { &c_tr, "get_localScale", 0 }, { &c_tr, "get_parent", 0 }, { &c_tr, "GetSiblingIndex", 0 },
    { &c_uobj, "get_name", 0 }, { NULL, NULL, 0 }
};
static int asked;
struct obj { struct fclass *k; void *sync; char body[256]; };
static struct obj *new_obj(struct fclass *k) { struct obj *o = calloc(1, sizeof *o); o->k = k; return o; }
static void *str_new(const char *s)
{
    size_t n = strlen(s);
    char *o = calloc(1, HDR + 4 + 2 * n + 2);
    *(struct fclass **)o = &c_string; *(int32_t *)(o + HDR) = (int32_t)n;
    for (size_t i = 0; i < n; i++) ((uint16_t *)(o + HDR + 4))[i] = (unsigned char)s[i];
    return o;
}
struct arr { struct fclass *k; void *sync; uintptr_t len; void *items[8]; };
static void seti(struct arr *a, int i, int32_t v) { memcpy((char *)a->items + 4 * i, &v, 4); }
static struct arr *arr_new(struct fclass *k, int n) { struct arr *a = calloc(1, sizeof *a); a->k = k; a->len = (uintptr_t)n; return a; }
static void *box(const void *v, size_t n) { struct obj *o = new_obj(&c_object); memcpy((char *)o + HDR, v, n); return o; }
#define F(o, i, T) (*(T *)((char *)(o) + f_hero[i].off))

// the scene: two Heroes (one on a GameObject whose transform moves), a CameraRig; their transforms
static struct obj *hero1, *hero2, *rig, *tr1, *tr2, *tr3, *data1, *go1, *dict1;
static struct arr *intarr, *buckets;
static float pos1[3] = { 1, 2, 3 }, game_time = 0.5f, captured;
static int enabled1 = 1, seeded = -1, swap_order;
static void *tr_of(void *b) { return b == hero1 ? tr1 : b == hero2 ? tr2 : tr3; }
static void set_cached(struct obj *o, void *p) { *(void **)((char *)o + HDR) = p; }
static void scene(void)
{
    hero1 = new_obj(&c_hero); hero2 = new_obj(&c_hero); rig = new_obj(&c_cam);
    tr1 = new_obj(&c_tr); tr2 = new_obj(&c_tr); tr3 = new_obj(&c_tr); go1 = new_obj(&c_go);
    struct obj *all[] = { hero1, hero2, rig, tr1, tr2, tr3, go1 };
    for (int i = 0; i < 7; i++) set_cached(all[i], (void *)(uintptr_t)(0x1000 + i));
    data1 = new_obj(&c_data);
    *(int32_t *)((char *)data1 + HDR) = 4; *(void **)((char *)data1 + HDR + 8) = str_new("crawlid");
    intarr = arr_new(&c_intarr, 3); seti(intarr, 0, 1); seti(intarr, 1, 2); seti(intarr, 2, 3);
    buckets = arr_new(&c_intarr, 2);
    struct obj *dict = dict1 = new_obj(&c_dict); *(void **)((char *)dict + HDR) = buckets; *(int32_t *)((char *)dict + HDR + P) = 1;
    F(hero1, 0, int32_t) = 5; F(hero1, 1, float) = 8.3f; F(hero1, 2, void *) = str_new("Knight");
    float v[3] = { 0.5f, 0, 0 }; memcpy((char *)hero1 + f_hero[3].off, v, 12);
    F(hero1, 4, void *) = data1; F(hero1, 5, void *) = intarr; F(hero1, 7, void *) = go1; F(hero1, 8, void *) = dict;
    F(hero2, 0, int32_t) = 1;
}
static void *find_all(struct fclass *k)
{
    struct arr *a = arr_new(&c_object, 3);
    if (k == &c_tr) { a->items[0] = tr1; a->items[1] = tr2; a->items[2] = tr3; }
    else { a->items[0] = swap_order ? hero2 : hero1; a->items[1] = swap_order ? hero1 : hero2; a->items[2] = rig; }
    return a;
}
static void pos_icall(void *t, float *out) { float z[3] = { 0 }; memcpy(out, t == tr1 ? pos1 : z, 12); }
static void *m_invoke(void *mm, void *self, void **args, void **exc)
{
    struct fmethod *m = mm;
    *exc = NULL;
    if (!strcmp(m->name, "FindObjectsOfType")) return find_all(args[0]);
    if (!strcmp(m->name, "get_time")) return box(&game_time, 4);
    if (!strcmp(m->name, "set_captureDeltaTime")) { captured = *(float *)args[0]; return NULL; }
    if (!strcmp(m->name, "InitState")) { seeded = *(int *)args[0]; return NULL; }
    if (!strcmp(m->name, "get_state")) { int s[4] = { 1, 2, 3, 4 }; return box(s, 16); }
    if (!strcmp(m->name, "GetActiveScene")) { int h = 7; return box(&h, 4); }
    if (!strcmp(m->name, "get_sceneCount")) { int n = 1; return box(&n, 4); }
    if (!strcmp(m->name, "get_name")) return str_new(m->k == &c_scene ? "Tutorial_01" : self == tr1 ? "Knight" : "Thing");
    if (!strcmp(m->name, "get_transform")) return tr_of(self);
    if (!strcmp(m->name, "get_enabled")) { unsigned char e = self == hero1 ? (unsigned char)enabled1 : 1; return box(&e, 1); }
    if (!strcmp(m->name, "get_localRotation")) { float q[4] = { 0, 0, 0, 1 }; return box(q, 16); }
    if (!strcmp(m->name, "get_localScale")) { float s[3] = { 1, 1, 1 }; return box(s, 12); }
    if (!strcmp(m->name, "get_parent")) return NULL;
    if (!strcmp(m->name, "GetSiblingIndex")) { int i = 0; return box(&i, 4); }
    return NULL;
}
static void *image_loaded(const char *n) { return !strcmp(n, "UnityEngine.CoreModule") || !strcmp(n, "mscorlib") ? (void *)1 : NULL; }
static void *class_from_name(void *img, const char *ns, const char *n)
{
    (void)img;
    for (int i = 0; classes[i]; i++) if (!strcmp(classes[i]->ns, ns) && !strcmp(classes[i]->name, n)) return classes[i];
    return NULL;
}
static void *method_from_name(void *k, const char *n, int argc)
{
    for (int i = 0; methods[i].name; i++) if (methods[i].k == k && !strcmp(methods[i].name, n) && methods[i].n == argc) return &methods[i];
    return NULL;
}
static void *domain_get(void) { return (void *)1; }
static void *class_get_type(void *k) { return k; }
static void *type_get_object(void *d, void *t) { (void)d; return t; }
static uintptr_t array_length(void *a) { return ((struct arr *)a)->len; }
static char *array_addr(void *a, int size, uintptr_t i) { return (char *)((struct arr *)a)->items + (size_t)size * i; }
static void *unbox(void *o) { return (char *)o + HDR; }
static void *object_get_class(void *o) { return *(struct fclass **)o; }
static const char *class_get_name(void *k) { return ((struct fclass *)k)->name; }
static const char *class_get_namespace(void *k) { return ((struct fclass *)k)->ns; }
static void *class_get_parent(void *k) { return ((struct fclass *)k)->parent; }
static void *class_get_fields(void *k, void **it)
{
    struct fclass *c = k == &c_uobj ? &(struct fclass){ .f = f_uobj, .nf = 1 } : k == &c_int ? &(struct fclass){ .f = f_int, .nf = 1 } : k;
    intptr_t i = (intptr_t)*it;
    if (i >= c->nf) return NULL;
    *it = (void *)(i + 1);
    return &c->f[i];
}
static const char *field_get_name(void *f) { return ((struct ffield *)f)->name; }
static void *field_get_type(void *f) { return f; }
static uint32_t field_get_offset(void *f) { return ((struct ffield *)f)->off; }
static uint32_t field_get_flags(void *f) { return ((struct ffield *)f)->flags; }
static int type_get_type(void *t) { return ((struct ffield *)t)->type; }
static void *class_from_mono_type(void *t) { return ((struct ffield *)t)->cls; }
static int class_is_valuetype(void *k) { return ((struct fclass *)k)->valuetype; }
static int32_t class_value_size(void *k, uint32_t *a) { (void)a; return ((struct fclass *)k)->vsize; }
static int class_get_rank(void *k) { return ((struct fclass *)k)->rank; }
static void *class_get_element_class(void *k) { return ((struct fclass *)k)->elem; }
static int32_t array_element_size(void *k) { return ((struct fclass *)k)->esize; }
static int class_is_subclass_of(void *k, void *p, int i) { (void)i; for (struct fclass *c = ((struct fclass *)k)->parent; c; c = c->parent) if (c == p) return 1; return 0; }
static void *class_get_field_from_name(void *k, const char *n) { return k == &c_uobj && !strcmp(n, "m_CachedPtr") ? &f_uobj[0] : NULL; }
static void *class_get_nesting_type(void *k) { return k == &c_entry ? &c_dict : NULL; }
static uint32_t method_get_flags(void *m, uint32_t *iflags) { *iflags = ((struct fmethod *)m)->icall ? 0x1000 : 0; return 0; }
static void *lookup_icall(void *m) { return ((struct fmethod *)m)->icall ? (void *)pos_icall : NULL; }
static char *string_to_utf8(void *s)
{
    int32_t n = *(int32_t *)((char *)s + HDR);
    char *u = malloc((size_t)n + 1);
    for (int i = 0; i < n; i++) u[i] = (char)((uint16_t *)((char *)s + HDR + 4))[i];
    u[n] = 0;
    return u;
}
static struct { const char *name; const void *fn; } icalls[16];
static int nicalls;
static void add_icall(const char *name, const void *fn) { if (nicalls < 16) { icalls[nicalls].name = name; icalls[nicalls++].fn = fn; } }
static const void *icall(const char *name) { for (int i = nicalls - 1; i >= 0; i--) if (!strcmp(icalls[i].name, name)) return icalls[i].fn; return NULL; }
static void *jit_init(const char *f, const char *v) { (void)f; (void)v; return (void *)1; }
static float realtime_orig(void) { return -1; }

void *mono_runtime_lib(void) { asked++; return (void *)1; }
void *so_dlsym(void *h, const char *n)
{
    (void)h;
    static const struct { const char *n; void *f; } t[] = {
#define S(x, f) { x, (void *)f }
        S("mono_domain_get", domain_get), S("mono_image_loaded", image_loaded), S("mono_class_from_name", class_from_name),
        S("mono_class_get_method_from_name", method_from_name), S("mono_runtime_invoke", m_invoke), S("mono_class_get_type", class_get_type),
        S("mono_type_get_object", type_get_object), S("mono_array_addr_with_size", array_addr), S("mono_object_unbox", unbox),
        S("mono_object_get_class", object_get_class), S("mono_class_get_name", class_get_name), S("mono_class_get_namespace", class_get_namespace),
        S("mono_class_get_parent", class_get_parent), S("mono_class_get_fields", class_get_fields), S("mono_field_get_name", field_get_name),
        S("mono_field_get_type", field_get_type), S("mono_field_get_offset", field_get_offset), S("mono_field_get_flags", field_get_flags),
        S("mono_type_get_type", type_get_type), S("mono_class_from_mono_type", class_from_mono_type), S("mono_class_is_valuetype", class_is_valuetype),
        S("mono_class_value_size", class_value_size), S("mono_class_get_rank", class_get_rank), S("mono_class_get_element_class", class_get_element_class),
        S("mono_array_element_size", array_element_size), S("mono_class_is_subclass_of", class_is_subclass_of),
        S("mono_class_get_field_from_name", class_get_field_from_name), S("mono_string_to_utf8", string_to_utf8), S("mono_free", free),
        S("mono_array_length", array_length), S("mono_method_get_flags", method_get_flags), S("mono_lookup_internal_call", lookup_icall),
        S("mono_add_internal_call", add_icall), S("mono_class_get_nesting_type", class_get_nesting_type),
#undef S
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, n)) return t[i].f;
    return NULL;
}

/* ---- the runtime's side ------------------------------------------------------------------ */
static int (*g_tap)(const struct up_input_rec *);
static int64_t (*g_clock)(void);
static struct up_input_rec sent[64];
static long sent_frame[64], frame_now;
static int nsent, finished;
static int64_t sent_ms[64];
void up_set_input_tap(int (*tap)(const struct up_input_rec *)) { g_tap = tap; }
void up_set_event_clock(int64_t (*fn)(void)) { g_clock = fn; }
int up_inject_rec(const struct up_input_rec *r) { if (nsent < 64) { sent_frame[nsent] = frame_now; sent_ms[nsent] = g_clock ? g_clock() : -1; sent[nsent++] = *r; } return 1; }
static int (*g_unity_hook)(int, long *, long *);
void so_set_clock_hook(int (*hook)(int, long *, long *)) { g_unity_hook = hook; }
void up_finish(const char *why) { (void)why; finished = (int)frame_now; }

static void frame(void) { frame_now++; lockstep_frame(); }
static int key(int code, int down) { struct up_input_rec r = { .kind = UP_REC_KEY, .device = 3, .source = 0x401, .action = down ? 0 : 1, .keycode = code }; return g_tap(&r); }

// the F line of a frame and the C lines of a frame, from the hash file
static char hashes[64][160], clines[64][1024];
static void read_hashes(const char *path)
{
    memset(hashes, 0, sizeof hashes); memset(clines, 0, sizeof clines);
    FILE *f = fopen(path, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        long fr = atol(line + 2);
        if (fr < 0 || fr >= 64) continue;
        if (line[0] == 'F') snprintf(hashes[fr], sizeof hashes[fr], "%s", line);
        if (line[0] == 'C') strncat(clines[fr], line, sizeof clines[fr] - strlen(clines[fr]) - 1);
    }
    if (f) fclose(f);
}
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return calloc(1, 1);
    static char buf[1 << 16]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return buf;
}
static uint64_t all_of(int fr) { return strtoull(hashes[fr] + 2 + strcspn(hashes[fr] + 2, " ") + 1, NULL, 16); }

// Unity's clock on UnityMain: two reads in one frame 1 us apart, CLOCK_REALTIME and other threads left alone
static long uc_ns[3]; static int uc_other, uc_real;
static void *unity_main(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "UnityMain");
    long s, n;
    for (int i = 0; i < 2; i++) { g_unity_hook(1, &s, &n); uc_ns[i] = s * 1000000000L + n; }
    uc_real = g_unity_hook(0, &s, &n);
    return NULL;
}
static void *other_thread(void *arg) { (void)arg; long s, n; uc_other = g_unity_hook(1, &s, &n); return NULL; }
static void *busy(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "Loading.Preload");
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    do clock_gettime(CLOCK_MONOTONIC, &b); while ((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000 < 150);
    sleep(5);
    return NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "hash", *dir = argc > 2 ? argv[2] : "/tmp";
    char in[512], hp[512];
    snprintf(in, sizeof in, "%s/rec.txt", dir); snprintf(hp, sizeof hp, "%s/%s.hash", dir, mode);
    unsetenv("UA_LOCKSTEP"); unsetenv("UA_LOCKSTEP_DUMP"); unsetenv("UA_LOCKSTEP_NUDGE"); unsetenv("UA_LOCKSTEP_FRAMES");
    setenv("UA_LOCKSTEP_HASH", hp, 1);
    if (!strcmp(mode, "off")) {
        void *x = (void *)add_icall;
        CHECK(lockstep_dlsym_hook("mono_add_internal_call", x) == x && lockstep_dlsym_hook("mono_jit_init_version", (void *)jit_init) == (void *)jit_init, "off: a hook");
        lockstep_init();
        for (int i = 0; i < 10; i++) frame();
        CHECK(!g_unity_hook, "off: a clock hook");
        CHECK(!g_tap && !g_clock && !asked && !lockstep_on(), "off: tap %p clock %p, Mono asked %d times", (void *)g_tap, (void *)g_clock, asked);
        CHECK(access(hp, F_OK) != 0, "off: a hash file");
    } else if (!strcmp(mode, "hash") || !strcmp(mode, "dump") || !strcmp(mode, "nudge")) {
        setenv("UA_LOCKSTEP", "hash", 1);
        if (!strcmp(mode, "dump")) setenv("UA_LOCKSTEP_DUMP", "3", 1);
        if (!strcmp(mode, "nudge")) setenv("UA_LOCKSTEP_NUDGE", "Hero.hp@2+5", 1);
        setenv("UA_LOCKSTEP_SEED", "42", 1);
        if (!strcmp(mode, "hash")) setenv("UA_LOCKSTEP_IGNORE", "Other.x,Hero.speed,CameraRig", 1);
        // Unity and Mono register through the hooked calls
        void (*add)(const char *, const void *) = lockstep_dlsym_hook("mono_add_internal_call", (void *)add_icall);
        add("UnityEngine.Time::get_realtimeSinceStartup", (const void *)realtime_orig);
        add("UnityEngine.Time::get_deltaTime", (const void *)realtime_orig);
        void *(*init)(const char *, const char *) = lockstep_dlsym_hook("mono_jit_init_version", (void *)jit_init);
        CHECK(init != jit_init && init("x", "v") == (void *)1, "mono_jit_init_version not wrapped");
        CHECK(icall("UnityEngine.Time::get_deltaTime") == (void *)realtime_orig, "another icall wrapped");
        typedef SO_ABI float (*rt_fn)(void);
        rt_fn rt = (rt_fn)icall("UnityEngine.Time::get_realtimeSinceStartup");
        int32_t (*tick)(void) = (int32_t (*)(void))icall("System.Environment::get_TickCount");
        CHECK(rt && rt != (void *)realtime_orig && tick && icall("System.Diagnostics.Stopwatch::GetTimestamp") && icall("System.DateTime::GetSystemTimeAsFileTime"),
              "the clocks not registered: realtime %p tick %p", (void *)rt, (void *)tick);
        scene();
        lockstep_init();
        frame();
        CHECK(seeded == 42 && captured > 0.0166f && captured < 0.0167f, "clock: captureDeltaTime %g, seed %d", captured, seeded);
        float r1 = rt ? rt() : 0, r1b = rt ? rt() : 0; int32_t t1 = tick ? tick() : 0;
        frame();
        float r2 = rt ? rt() : 0; int32_t t2 = tick ? tick() : 0;
        CHECK(r1b > r1 && r1b - r1 < 1e-5f && r2 - r1 > 0.0165f && r2 - r1 < 0.0168f && t2 - t1 >= 16 && t2 - t1 <= 17,
              "virtual clock: realtime %.6f %.6f -> %.6f, tick %d -> %d", r1, r1b, r2, t1, t2);
        CHECK(g_unity_hook, "no clock hook");
        if (g_unity_hook) {
            pthread_t t; pthread_create(&t, NULL, unity_main, NULL); pthread_join(t, NULL);
            pthread_create(&t, NULL, other_thread, NULL); pthread_join(t, NULL);
            CHECK(uc_ns[1] - uc_ns[0] == 1000 && uc_other != 0 && uc_real != 0, "Unity's clock: %ld then %ld ns, other thread %d, realtime %d",
                  uc_ns[0], uc_ns[1], uc_other, uc_real);
        }
        if (!strcmp(mode, "nudge")) {
            frame(); frame();
            CHECK(F(hero1, 0, int32_t) == 10 && F(hero2, 0, int32_t) == 6, "nudge: hp %d, %d", F(hero1, 0, int32_t), F(hero2, 0, int32_t));
        } else if (!strcmp(mode, "dump")) {
            frame(); frame();
            lockstep_flush();
            char dp[600]; snprintf(dp, sizeof dp, "%s.3.dump", hp);
            char *d = slurp(dp);
            const char *want[] = { "O /Knight[0]|Hero", "  hp 5", "  speed 8.30000019", "  name \"Knight\"", "  data.x 4", "  data.label \"crawlid\"",
                                   "  arr[1] 2", "  target <UnityEngine.GameObject alive>", "  localPosition 1 2 3", "  enabled 1", "O /Thing[0]|CameraRig", NULL };
            for (int i = 0; want[i]; i++) CHECK(strstr(d, want[i]), "dump: no \"%s\" in\n%s", want[i], d);
            CHECK(!strstr(d, "native") && !strstr(d, "buckets") && !strstr(d, "counter"), "dump: a field left out is there");
        } else {
            // frame 3: the state as it is; frames 4..: one change each, undone after the frame
            frame();
            float v1 = 0;
            for (int st = 0; st < 14; st++) {
                switch (st) {
                case 1: F(hero1, 0, int32_t) = 6; break;
                case 2: F(hero1, 6, intptr_t) = 99; break;
                case 3: seti(buckets, 0, 77); *(int32_t *)((char *)dict1 + HDR + P + 8) = 99; break;
                case 4: swap_order = 1; break;
                case 5: *(int32_t *)((char *)data1 + HDR) = 5; break;
                case 6: *(void **)((char *)data1 + HDR + 8) = str_new("crawlie"); break;
                case 7: memcpy(&v1, (char *)hero1 + f_hero[3].off + 4, 4); { float y = 1; memcpy((char *)hero1 + f_hero[3].off + 4, &y, 4); } break;
                case 8: seti(intarr, 2, 30); break;
                case 9: set_cached(go1, NULL); break;
                case 10: pos1[0] = 1.5f; break;
                case 11: enabled1 = 0; break;
                case 12: game_time = 0.75f; break;
                case 13: F(hero1, 1, float) = 9.5f; break;
                }
                frame();
                switch (st) {
                case 1: F(hero1, 0, int32_t) = 5; break;
                case 2: F(hero1, 6, intptr_t) = 0; break;
                case 3: seti(buckets, 0, 0); *(int32_t *)((char *)dict1 + HDR + P + 8) = 0; break;
                case 4: swap_order = 0; break;
                case 5: *(int32_t *)((char *)data1 + HDR) = 4; break;
                case 6: *(void **)((char *)data1 + HDR + 8) = str_new("crawlid"); break;
                case 7: memcpy((char *)hero1 + f_hero[3].off + 4, &v1, 4); break;
                case 8: seti(intarr, 2, 3); break;
                case 9: set_cached(go1, (void *)1); break;
                case 10: pos1[0] = 1; break;
                case 11: enabled1 = 1; break;
                case 12: game_time = 0.5f; break;
                case 13: F(hero1, 1, float) = 8.3f; break;
                }
            }
            frame();
            lockstep_flush();
            read_hashes(hp);
            uint64_t base = all_of(3);
            CHECK(base && all_of(1) == base && all_of(2) == base && all_of(18) == base, "the same state, frames 1-3 and 18: %s%s%s%s", hashes[1], hashes[2], hashes[3], hashes[18]);
            static const char *const what[] = { "nothing", "hp", "an IntPtr", "a Dictionary's buckets and an entry's hashCode", "FindObjectsOfType's order", "a followed object's int",
                                                "a followed object's string", "a struct's field", "an array element", "a referenced engine object destroyed",
                                                "the transform", "enabled", "Time.time", "a field in UA_LOCKSTEP_IGNORE" };
            static const int changes[] = { 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0 };
            for (int st = 0; st < 14; st++) {
                int fr = 4 + st, diff = all_of(fr) != base;
                CHECK(diff == changes[st], "%s: frame %d's hash %s the first's (%s)", what[st], fr, diff ? "differs from" : "is", hashes[fr]);
                if (changes[st] && st != 10 && st != 12)
                    CHECK(strstr(clines[fr], " Hero 2 ") && !strstr(clines[fr], "CameraRig"), "%s: frame %d's class lines: %s", what[st], fr, clines[fr]);
            }
            CHECK(strstr(clines[1], " Hero 2 ") && strstr(clines[1], " CameraRig 1 "), "frame 1's class lines: %s", clines[1]);
            CHECK(strstr(hashes[16], "t=3f400000"), "Time.time in frame 16's line: %s", hashes[16]);
            CHECK(strstr(slurp(hp), "S 1 Tutorial_01 1\n") != NULL, "no scene line");
        }
    } else if (!strcmp(mode, "record") || !strcmp(mode, "replay")) {
        int rec = !strcmp(mode, "record");
        char e[600]; snprintf(e, sizeof e, "%s:%s", mode, in);
        setenv("UA_LOCKSTEP", e, 1);
        if (rec) setenv("UA_LOCKSTEP_FRAMES", "6", 1);
        lockstep_init();
        CHECK(g_tap && g_clock, "no tap or event clock");
        frame();
        CHECK(key(96, 1) == 1 && key(96, 0) == 1, "the tap's answer");
        if (rec) CHECK(nsent == 0, "an event sent before the frame ended");
        frame(); frame();
        if (rec) { struct up_input_rec r = { .kind = UP_REC_AXES, .device = 3, .source = 0x1000010, .n = 2, .axis = { 15, 16 }, .value = { 1.f, -0.5f } }; g_tap(&r); }
        frame(); frame(); frame(); frame();
        CHECK(finished == 6, "quit asked after frame %d", finished);
        CHECK(nsent == 3, "%d events sent", nsent);
        CHECK(nsent >= 3 && sent_frame[0] == 2 && sent_frame[1] == 2 && sent_frame[2] == 4, "sent after frames %ld %ld %ld", sent_frame[0], sent_frame[1], sent_frame[2]);
        CHECK(nsent >= 3 && sent[0].kind == UP_REC_KEY && sent[0].keycode == 96 && sent[0].action == 0 && sent[1].action == 1 && sent[2].kind == UP_REC_AXES &&
              sent[2].n == 2 && sent[2].axis[1] == 16 && sent[2].value[1] == -0.5f, "the events sent");
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long long up_ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;
        CHECK(nsent >= 3 && sent_ms[2] - sent_ms[0] == 33 && sent_ms[0] > up_ms - 5000 && sent_ms[0] <= up_ms + 100,
              "event times %lld %lld (uptime %lld)", (long long)sent_ms[0], (long long)sent_ms[2], up_ms);
        if (rec) { lockstep_flush(); CHECK(strstr(slurp(in), "E 6\n") != NULL, "the record's end:\n%s", slurp(in)); }
    } else if (!strcmp(mode, "settle")) {
        setenv("UA_LOCKSTEP", "hash", 1);
        lockstep_init();
        pthread_t t; pthread_create(&t, NULL, busy, NULL);
        usleep(20000);
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        frame();
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        CHECK(ms > 100 && ms < 1000, "the frame waited %.0f ms for the loading thread", ms);
        clock_gettime(CLOCK_MONOTONIC, &a); frame(); clock_gettime(CLOCK_MONOTONIC, &b);
        ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        CHECK(ms < 20, "the next frame waited %.0f ms (the thread sleeps)", ms);
    }
    printf("lockstep %s: %s (%d failures)\n", mode, fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
