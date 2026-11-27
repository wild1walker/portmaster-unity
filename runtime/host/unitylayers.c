// unitylayers.c: a question a game's own code may ask libunity through ua_host: does the scene hold a renderer on
// one of these layers? (an internal call, `UaHost::LayersInScene(int mask)`: 1 or 0, registered beside Unity's own
// when libunity registers them; nothing else changes). A game patch asks it before rendering a camera whose culling
// mask names layers that are empty in almost every frame (Enter the Gungeon's Pixelator draws its "Unoccluded"
// layer with a camera of its own after the vignette: ~0.48 ms of the RG34XX-SP's main thread a frame for a camera
// that draws nothing, as Unity 2017.4 culls the whole scene and runs its render loop whatever the mask holds).
//
// How it answers. Unity keeps every active, enabled renderer of the loaded scenes in its RendererScene: an array
// of 32-byte SceneNodes (the renderer at +8, its layer at +12), which every camera's culling walks
// (IsNodeVisibleFast tests `cullingMask & (1 << node.layer)` first). The answer is 1 when a node with a renderer
// has its layer in the mask, or when the scene holds renderers waiting to be added (adds made during a culling job
// wait in a list: they count as present), or when anything here is not as read: a libunity whose functions do not
// have the bytes they were read with (pmu-dev unityhooks's rows below) never answers 0. So 0 means: the camera
// would cull nothing on those layers, as Unity's own culling would find. Read in 2017.4.27f1 armeabi-v7a:
// GetRendererScene() returns the scene; RendererScene::AddRendererInternal stores nodes in the array at +0x40
// (count +0x48), 32 bytes each, the renderer + 32 at +8 and the layer (its argument) at +12;
// RendererScene::AddRenderer queues the add in the list at +0x20 (count +0x28) while +0x78 is set.
// UA_LAYER_QUERY=0: the call is not registered (a game patch then renders as before).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unitylayers.h"
#include "unityhooks.h"

enum { ISA_ARM = 'a' };
static const struct uh_row g_rows[] = {
    { "2017.4.27f1 armeabi-v7a", "GetRendererScene", 0x3fc260, 0x10, ISA_ARM, 0x4dc6a0f1510055b9ull },
    { "2017.4.27f1 armeabi-v7a", "RendererScene::AddRendererInternal", 0x4276d0, 0xc0, ISA_ARM, 0x1b5cc300568fa8a5ull },
    { "2017.4.27f1 armeabi-v7a", "RendererScene::AddRenderer", 0x427790, 0x104, ISA_ARM, 0xa6e3f778ddb9764eull },
    { "2017.4.27f1 armeabi-v7a", "IsNodeVisibleFast", 0x469b40, 0x88, ISA_ARM, 0xf013edc170d0563aull },
};
enum { NODES = 0x40, NODE_COUNT = 0x48, PENDING_COUNT = 0x28, DEFERRING = 0x78, NODE_SIZE = 32, NODE_RENDERER = 8, NODE_LAYER = 12 };

typedef void *(*get_scene_fn)(void);
typedef void (*add_icall_fn)(const char *name, const void *fn);
static get_scene_fn g_get_scene;
static add_icall_fn r_mono_add;
static int g_registered;
static unsigned long g_asked, g_empty;

void unitylayers_on_load(uintptr_t base, size_t size)
{
    const char *e = getenv("UA_LAYER_QUERY");
    if (e && !strcmp(e, "0")) return;
    size_t n = sizeof g_rows / sizeof *g_rows;
    for (size_t i = 0; i < n; i++) if (!unityhooks_matches(&g_rows[i], base, size)) return;   // another build: no answers
    g_get_scene = (get_scene_fn)(base + g_rows[0].off);
}

// 1 when a renderer of the scene is on a layer of mask (or when it cannot be known), 0 when none is
static int layers_in_scene(int mask)
{
    g_asked++;
    if (!g_get_scene) return 1;
    const uint8_t *s = g_get_scene();
    if (!s) return 1;
    if (*(const uint32_t *)(s + DEFERRING) || *(const uint32_t *)(s + PENDING_COUNT)) return 1;
    const uint8_t *node = *(const uint8_t *const *)(s + NODES);
    uint32_t count = *(const uint32_t *)(s + NODE_COUNT);
    if (count && !node) return 1;
    for (uint32_t i = 0; i < count; i++, node += NODE_SIZE) {
        if (!*(const void *const *)(node + NODE_RENDERER)) continue;
        uint32_t layer = *(const uint32_t *)(node + NODE_LAYER);
        if (layer > 31 || ((uint32_t)mask >> layer & 1)) return 1;      // (a layer past 31 is not as read: present)
    }
    g_empty++;
    return 0;
}

static void w_mono_add(const char *name, const void *fn)
{
    r_mono_add(name, fn);
    if (!g_registered && g_get_scene) {
        g_registered = 1;
        r_mono_add("UaHost::LayersInScene", (const void *)layers_in_scene);
        fprintf(stderr, "[unitylayers] UaHost::LayersInScene registered (libunity %s)\n", g_rows[0].unity);
    }
}

void *unitylayers_dlsym_hook(const char *symbol, void *found)
{
    if (!found || strcmp(symbol, "mono_add_internal_call") || found == (void *)w_mono_add) return found;
    r_mono_add = (add_icall_fn)found;
    return (void *)w_mono_add;
}

void unitylayers_frame(void)
{
    static unsigned long frames;
    if (!g_registered || ++frames % 1800) return;
    fprintf(stderr, "[unitylayers] %lu questions, %lu answered empty\n", g_asked, g_empty);
}
