// unitylayers.c's unit test: UaHost::LayersInScene over a stand-in for Unity 2017.4's RendererScene (nodes of 32
// bytes, renderer at +8, layer at +12; the node array at +0x40, its count at +0x48, the pending adds' count at +0x28,
// the deferring flag at +0x78): 0 only when no node with a renderer has its layer in the mask and nothing is pending;
// 1 for every case it cannot know (no scene, a deferred add, a count without an array, a layer past 31, a libunity of
// another build); the internal call registered once, after Unity's first, and only when the rows matched. Run by
// unityhooks_test.sh (32-bit only: the player's layout).
#include <stdio.h>
#include <string.h>

#include "../unitylayers.c"

#if UINTPTR_MAX > 0xffffffffu
int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return 0; }
int main(void) { printf("SKIPPED unitylayers: the ARMv7 player's layout (32-bit only)\n"); return 0; }
#else
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int g_rows_match = 1;
int unityhooks_matches(const struct uh_row *r, uintptr_t base, size_t size) { (void)r; (void)base; (void)size; return g_rows_match; }

static uint8_t scene[0x100];
static uint8_t nodes[8][NODE_SIZE];
static int renderer_obj;
static void *fake_scene(void) { return scene; }
static void *no_scene(void) { return NULL; }
static void set32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static void node(int i, int has_renderer, uint32_t layer)
{
    void *r = has_renderer ? (void *)&renderer_obj : NULL;
    memcpy(nodes[i] + NODE_RENDERER, &r, sizeof r);
    set32(nodes[i] + NODE_LAYER, layer);
}
static void use(int count)
{
    memset(scene, 0, sizeof scene);
    const void *a = nodes; memcpy(scene + NODES, &a, sizeof a);
    set32(scene + NODE_COUNT, (uint32_t)count);
}

static const char *icalls[8]; static int nicalls;
static void f_add(const char *name, const void *fn) { (void)fn; if (nicalls < 8) icalls[nicalls] = name; nicalls++; }

int main(void)
{
    // another build of libunity: no scene function, every question answered 1, the call never registered
    g_rows_match = 0;
    unitylayers_on_load(0x1000, 0x1000000);
    CHECK(!g_get_scene, "rows of another build gave a scene function");
    CHECK(layers_in_scene(1 << 23) == 1, "no scene function: must answer 1");
    void *f = unitylayers_dlsym_hook("mono_add_internal_call", (void *)f_add);
    ((add_icall_fn)f)("UnityEngine.Object::Destroy", NULL);
    CHECK(nicalls == 1, "registered without the rows: %d calls", nicalls);

    // this build: the scene stand-in
    g_get_scene = fake_scene;
    node(0, 1, 0); node(1, 1, 5); node(2, 0, 23); node(3, 1, 8);
    use(4);
    CHECK(layers_in_scene(1 << 23) == 0, "only a node without a renderer on layer 23: must be empty");
    CHECK(layers_in_scene(1 << 5) == 1, "a renderer on layer 5");
    CHECK(layers_in_scene((1 << 23) | (1 << 8)) == 1, "a renderer on layer 8 of the mask");
    CHECK(layers_in_scene(0) == 0, "an empty mask: nothing to find");
    CHECK(layers_in_scene(-1) == 1, "every layer");
    use(0);
    CHECK(layers_in_scene(-1) == 0, "no nodes: empty");
    node(0, 1, 23); use(1);
    CHECK(layers_in_scene(1 << 23) == 1, "a renderer on layer 23");
    node(0, 1, 40); use(1);
    CHECK(layers_in_scene(1 << 8) == 1, "a layer past 31: must answer 1");
    node(0, 1, 0); use(1);
    set32(scene + PENDING_COUNT, 1);
    CHECK(layers_in_scene(1 << 23) == 1, "an add pending: must answer 1");
    use(1); set32(scene + DEFERRING, 1);
    CHECK(layers_in_scene(1 << 23) == 1, "adds deferred: must answer 1");
    use(3); memset(scene + NODES, 0, sizeof(void *));
    CHECK(layers_in_scene(1 << 23) == 1, "a count without an array: must answer 1");
    g_get_scene = no_scene;
    CHECK(layers_in_scene(1 << 23) == 1, "no scene: must answer 1");

    // registration: once, after Unity's own first call, only with the scene function
    g_get_scene = fake_scene; nicalls = 0; g_registered = 0;
    f = unitylayers_dlsym_hook("mono_add_internal_call", (void *)f_add);
    CHECK(f == (void *)w_mono_add, "mono_add_internal_call not wrapped");
    CHECK(unitylayers_dlsym_hook("mono_add_internal_call", f) == f, "wrapped twice");
    CHECK(unitylayers_dlsym_hook("mono_runtime_invoke", (void *)f_add) == (void *)f_add, "another symbol changed");
    ((add_icall_fn)f)("UnityEngine.Object::Destroy", NULL);
    ((add_icall_fn)f)("UnityEngine.Object::Instantiate", NULL);
    CHECK(nicalls == 3, "%d calls registered, want 3", nicalls);
    CHECK(nicalls >= 2 && !strcmp(icalls[1], "UaHost::LayersInScene"), "LayersInScene not registered after Unity's first");

    if (fails) { printf("unitylayers: %d FAILED\n", fails); return 1; }
    printf("unitylayers: OK (%lu questions, %lu answered empty)\n", g_asked, g_empty);
    return 0;
}
#endif
