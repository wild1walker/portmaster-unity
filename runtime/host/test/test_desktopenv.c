// desktopenv.c's check (runtime/host/test/plugins_test.sh): the game's command line through a stand-in
// for Mono's main-arguments setter, and Screen.resolutions through stand-ins for Mono's icall
// registration and arrays (an empty answer becomes the window's mode; others pass through)
#include "../desktopenv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Mono's main arguments
static int got_argc, set_calls;
static char got[8][64];
static void fake_set_args(int argc, const char *argv[])
{
    set_calls++;                    // (Unity's 2017.x Mono asserts when they are set twice)
    got_argc = argc;
    for (int i = 0; i < argc && i < 8; i++) snprintf(got[i], sizeof got[i], "%s", argv[i]);
}

// arrays: a class is its element size; an array has Mono's layout (vtable (here: its class), sync,
// bounds, max_length, the elements); Unity's 2017.x Mono has no mono_array_length function
typedef struct { int elsize; } klass;
typedef struct { klass *k; void *sync, *bounds; uintptr_t len; char data[64]; } array;
static klass res12 = { 12 }, res16 = { 16 }, *elem_of = &res12;
static array empty, one = { &res12, 0, 0, 1, { 0 } }, made;
static int made_count;
static void *f_object_get_class(void *a) { return ((array *)a)->k; }
static void *f_class_get_element_class(void *c) { return c; }
static void *f_domain_get(void) { return (void *)1; }
static void *f_array_new(void *d, void *ec, uintptr_t n) { (void)d; made.k = ec; made.len = n; made_count++; return &made; }
static int f_array_element_size(void *c) { return ((klass *)c)->elsize; }
static char *f_array_addr_with_size(void *a, int size, uintptr_t i) { return ((array *)a)->data + size * i; }
static void *f_image_loaded(const char *n) { return !strcmp(n, "UnityEngine.CoreModule") ? (void *)2 : NULL; }
static void *f_class_from_name(void *im, const char *ns, const char *n) { return im == (void *)2 && !strcmp(ns, "UnityEngine") && !strcmp(n, "Resolution") ? &res12 : NULL; }
static void *f_array_class_get(void *ec, uint32_t rank) { return rank == 1 ? ec : NULL; }
static void *fake_sym(const char *n)
{
    static const struct { const char *n; void *f; } t[] = {
        { "mono_image_loaded", (void *)f_image_loaded }, { "mono_class_from_name", (void *)f_class_from_name },
        { "mono_array_class_get", (void *)f_array_class_get },
        { "mono_object_get_class", (void *)f_object_get_class }, { "mono_class_get_element_class", (void *)f_class_get_element_class },
        { "mono_domain_get", (void *)f_domain_get }, { "mono_array_new", (void *)f_array_new },
        { "mono_array_element_size", (void *)f_array_element_size },
        { "mono_array_addr_with_size", (void *)f_array_addr_with_size } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(n, t[i].n)) return t[i].f;
    return NULL;
}
static array *answer;
static void *fake_get_resolutions(void) { return answer; }
static void *other_icall(void) { return NULL; }
static const char *added_name;
static const void *added_fn;
static void fake_add(const char *name, const void *fn) { added_name = name; added_fn = fn; }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    desktopenv_set_resolver(fake_sym);
    desktopenv_init("-force-gfx-jobs legacy", 720, 480, 60);
    void *w = desktopenv_dlsym_hook("mono_unity_runtime_set_main_args", (void *)fake_set_args);
    CHECK(w && w != (void *)fake_set_args, "the setter is wrapped");
    CHECK(got_argc == 3 && strstr(got[0], "test_desktopenv") && !strcmp(got[1], "-force-gfx-jobs") && !strcmp(got[2], "legacy"),
          "set at once: the program's path and Unity's switches: %d %s %s", got_argc, got[0], got[1]);
    ((void (*)(int, const char **))w)(0, NULL);
    CHECK(set_calls == 1 && got_argc == 3, "libunity's own call afterwards is left out (Mono takes them once): %d calls", set_calls);
    CHECK(desktopenv_dlsym_hook("mono_runtime_set_main_args", (void *)fake_set_args) != (void *)fake_set_args, "the newer Mono's setter too");
    CHECK(desktopenv_dlsym_hook("mono_jit_init", (void *)fake_add) == (void *)fake_add && desktopenv_dlsym_hook("other", (void *)fake_add) == (void *)fake_add,
          "other symbols as they are");

    void *add = desktopenv_dlsym_hook("mono_add_internal_call", (void *)fake_add);
    ((void (*)(const char *, const void *))add)("UnityEngine.Application::get_isPlaying", (void *)other_icall);
    CHECK(added_fn == (void *)other_icall, "other icalls registered as they are");
    ((void (*)(const char *, const void *))add)("UnityEngine.Screen::get_resolutions", (void *)fake_get_resolutions);
    CHECK(added_name && !strcmp(added_name, "UnityEngine.Screen::get_resolutions") && added_fn != (void *)fake_get_resolutions, "get_resolutions wrapped");
    void *(*get)(void) = (void *(*)(void))added_fn;
    empty.k = &res12; empty.len = 0;
    answer = &empty;
    array *a = get();
    int32_t v[4];
    memcpy(v, made.data, 16);
    CHECK(a == &made && made.len == 1 && made.k == &res12 && v[0] == 720 && v[1] == 480 && v[2] == 60,
          "an empty list: one mode, the window at the display's rate: %d x %d @ %d", v[0], v[1], v[2]);
    answer = &one;
    int before = made_count;
    CHECK(get() == &one && made_count == before, "Unity's own list as it is");
    empty.k = &res16;
    memset(made.data, 0, sizeof made.data);
    answer = &empty;
    a = get();
    memcpy(v, made.data, 16);
    CHECK(a == &made && v[0] == 720 && v[1] == 480 && v[2] == 60 && v[3] == 1, "2022.2+: the refresh rate as 60/1: %d/%d", v[2], v[3]);
    // no array at all: the class from UnityEngine.CoreModule
    answer = NULL;
    memset(made.data, 0, sizeof made.data);
    a = get();
    memcpy(v, made.data, 12);
    CHECK(a == &made && made.k == &res12 && v[0] == 720 && v[2] == 60, "a null list: one mode too");
    (void)elem_of;
    printf("desktopenv: %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
