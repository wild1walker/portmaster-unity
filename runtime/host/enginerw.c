// enginerw.c: measuring and rewriting libunity's own UnityMain code (docs/profiling.md, "Unity's engine code on
// UnityMain, by function").
//
//   UA_ENGINE_COUNT=<s>      count the calls of the functions in g_count (rows below) and print the mean a frame
//                            every <s> seconds: a call counter at each one's entry, nothing else changed (the stub
//                            adds one to a counter with x16/x17, which any call may clobber, and jumps to the
//                            function's moved entry). =<s>:<name>,<name> only the functions whose names contain one
//                            of those.
//   UA_ANCHOR_WALK=1         UI::RectTransform::UpdateAnchorPositionIfTransformChanged rewritten (below); =verify: each
//                            stretch it skips is walked again Unity's way and compared; =stats: the walks a frame
//   UA_CULL_STEP1=1          ProcessCameraIndexListIsNodeVisibleStep1 (each camera's first culling pass over the scene's
//                            nodes) rewritten with IsNodeVisibleFast inline (below); =verify: Unity's own runs too
//                            and the two index lists are compared
//   UA_DIRECTOR_PREWALK=1    before each Director prepare pass the pass's playable graphs are walked breadth-first
//                            across all graphs at once with PRFM, so Unity's own depth-first traversal (one dependent
//                            miss after another) finds its nodes in the cache; loads only, of the pointers Unity's
//                            traversal follows (below). =stats: graphs, nodes and levels a frame
//   UA_PREFETCH=...,anim     (also unityprefetch.c's name) on Unity 2017.4 arm64: Animator::BuildJobs' loop fetches the
//                            next outputs' Animators ahead, as unityprefetch.c's `anim` stub does on 6000.0 (below)
//   UA_ALLOC_STATS=<s>       (a measurement) MemoryManager::Allocate's calls counted by memory label and caller (the
//                            return address in libunity) and printed every <s> seconds: which kinds of objects are made
//                            where, for an allocator that keeps one kind's objects together (6000.0.61f1 arm64-v8a)
//   UA_OBJ_HEAPS=1|stats     (an experiment, off) Unity's objects of a few kinds made in heaps of their own: the
//                            allocation each ProduceHelper<T>::Produce makes (label 44, the object heap) goes to one of
//                            four new DualThreadAllocator<DynamicHeapAllocator> heaps (Unity's own allocator type, made
//                            and registered as Unity makes its CACHEOBJECTS heap) by the kind: behaviours; GameObjects
//                            and Transforms; renderers and mesh filters; Animators, particle systems, audio sources and
//                            2D physics. Where objects lie changes, nothing else (below). 6000.0.61f1 arm64-v8a
//
// Rows as unityhooks.c's: offset, size and FNV-1a of the bytes the function was read from, per Unity build; a
// function whose bytes differ is left alone. The rows are matched before any other hook changes libunity (the
// prefetch stubs rewrite call sites inside some of these functions), the entries are hooked after them.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "enginerw.h"
#include "unityhooks.h"
#include "unityprefetch.h"

enum { ISA_A64 = 'x' };
#define row uh_row

static const struct row g_count[] = {
    { "6000.0.61f1 arm64-v8a", "DirectorManager::ExecutePrepareFrames", 0x9af89c, 0x200, ISA_A64, 0x0e779fe165394f3dull },
    { "6000.0.61f1 arm64-v8a", "PlayableOutput::PrepareTraverse", 0x9b1a30, 0x174, ISA_A64, 0x70970997fc7be978ull },
    { "6000.0.61f1 arm64-v8a", "PreOrderPlayableTraverser::Traverse", 0x9b1be0, 0x1f0, ISA_A64, 0x196dd5dbc9ecc92dull },
    { "6000.0.61f1 arm64-v8a", "PreOrderPlayableTraverser::RecursiveVisit", 0x9b2998, 0xbc, ISA_A64, 0xf8b390afd5730f12ull },
    { "6000.0.61f1 arm64-v8a", "Playable::PrepareFrameVisitor", 0x9abde0, 0x1c8, ISA_A64, 0x439be0dd7a3002f1ull },
    { "6000.0.61f1 arm64-v8a", "Playable::Handle", 0x9aadd8, 0x40, ISA_A64, 0x895b868b316305d5ull },
    { "6000.0.61f1 arm64-v8a", "PlayableTraverser::GetEffectivePlayState", 0x84b954, 0x58, ISA_A64, 0x179cc6d025c6d4e3ull },
    { "6000.0.61f1 arm64-v8a", "Animator::Prepare", 0xb806bc, 0x44, ISA_A64, 0x50d68d8e28aec1dfull },
    { "6000.0.61f1 arm64-v8a", "Animator::BuildJobs", 0xb82fb0, 0x3c8, ISA_A64, 0x215a8a2dbed9cdfdull },
    { "6000.0.61f1 arm64-v8a", "Animator::UpdateAvatars", 0xb82420, 0xb80, ISA_A64, 0xba061501375fb241ull },
    { "6000.0.61f1 arm64-v8a", "UI::RectTransform::UpdateAnchorPositionIfTransformChanged", 0x919500, 0x140, ISA_A64, 0x2e08bf851f20a291ull },
    { "6000.0.61f1 arm64-v8a", "UI::Canvas::UpdateCanvasRectTransform", 0xe421a0, 0x69c, ISA_A64, 0x65b7f7dd893d034cull },
    { "6000.0.61f1 arm64-v8a", "GameObject::QueryComponentByType", 0x77b034, 0x44, ISA_A64, 0x823fd1d5d6ddd8a7ull },
    { "6000.0.61f1 arm64-v8a", "IsNodeVisibleFast", 0x785de0, 0x6c, ISA_A64, 0x959ef690b160b089ull },
    { "6000.0.61f1 arm64-v8a", "BaseRenderer::FlattenBasicData", 0xf547b4, 0xec, ISA_A64, 0x25ea40c962bae0a6ull },
    { "6000.0.61f1 arm64-v8a", "InputAxis::Update", 0x870e20, 0x358, ISA_A64, 0x700fa45ef3a2aa71ull },
    { "6000.0.61f1 arm64-v8a", "MonoBehaviour::HandleNotifications", 0x9d52a4, 0x2d8, ISA_A64, 0x038a216797682e70ull },
    { "6000.0.61f1 arm64-v8a", "GameObject::SendMessageAny", 0x77ba88, 0x208, ISA_A64, 0x923e5c9eccdc22d4ull },
    { "6000.0.61f1 arm64-v8a", "Transform::GetPosition", 0x91a044, 0xd8, ISA_A64, 0x11321b75489c7c57ull },
    { "6000.0.61f1 arm64-v8a", "Transform::GetTransformAccess", 0x919a78, 0x44, ISA_A64, 0x9995c37f408d74dcull },
    { "6000.0.61f1 arm64-v8a", "FMOD::SystemI::UpdateAudibilities", 0x185632c, 0x1bc, ISA_A64, 0x775ec45325a7f0bfull },
    { "6000.0.61f1 arm64-v8a", "ParticleSystem::UpdateManualTransformData", 0x11fa950, 0x1a4, ISA_A64, 0x4dbcab5f515c8486ull },
    { "6000.0.61f1 arm64-v8a", "Camera::CustomRender", 0xf7c688, 0x8dc, ISA_A64, 0x6ea5c214a5103578ull },
    { "6000.0.61f1 arm64-v8a", "Camera::CustomCull", 0xf79820, 0x424, ISA_A64, 0x4b24cd9c10980a93ull },
    { "6000.0.61f1 arm64-v8a", "DoRenderLoop", 0xfe4bd8, 0x5ec, ISA_A64, 0x211db38978b63909ull },
    { "6000.0.61f1 arm64-v8a", "DoForwardShaderRenderLoop", 0xfeec88, 0xdf8, ISA_A64, 0x1c6080a82333e11aull },
    { "6000.0.61f1 arm64-v8a", "RendererScene::UpdateVisibility", 0xf63f10, 0x148, ISA_A64, 0x57ae0cbba61d6e82ull },
    { "6000.0.61f1 arm64-v8a", "SpriteRenderer::PrepareRenderingDataIfNeeded", 0x1004d78, 0x164, ISA_A64, 0x5af0028a84e4c98dull },
    { "6000.0.61f1 arm64-v8a", "AllocateAndPrepareSpriteRenderingData", 0x753b14, 0x310, ISA_A64, 0x918cf8fca0d0d706ull },
    { "6000.0.61f1 arm64-v8a", "SpriteRenderer::ScheduleSpriteTilingJobIfNeeded", 0x1001fb4, 0x2d0, ISA_A64, 0x656cb35d634b9e39ull },
    { "6000.0.61f1 arm64-v8a", "Animator::GetAnimatorStateInfo", 0xb886ec, 0x40, ISA_A64, 0xcfb18daa91bc0bfbull },
    { "6000.0.61f1 arm64-v8a", "PhysicsContacts2D::SendCallbackReports", 0xd914a8, 0x284, ISA_A64, 0x46c6db76e41c2933ull },
    { "6000.0.61f1 arm64-v8a", "Camera::SendOnPreCullMessage", 0xf7a274, 0xe4, ISA_A64, 0x807443fbbad51c21ull },
    { "6000.0.61f1 arm64-v8a", "ScriptingInvocation::Invoke", 0x9c9008, 0xcc, ISA_A64, 0x6c141b866b6aded6ull },
    { "6000.0.61f1 arm64-v8a", "MonoBehaviour::CallUpdateMethod", 0x9d3e78, 0x114, ISA_A64, 0xd695de744b4c0dbdull },
    { "2017.4.40f1 arm64-v8a", "DirectorManager::ExecutePrepareFrames", 0x504a0c, 0x24c, ISA_A64, 0x9fdd8d42324456c5ull },
    { "2017.4.40f1 arm64-v8a", "PlayableOutput::PrepareTraverse", 0x50ab74, 0x164, ISA_A64, 0x0300d13fe713d822ull },
    { "2017.4.40f1 arm64-v8a", "PreOrderPlayableTraverser::Traverse", 0x50b260, 0x244, ISA_A64, 0x0021f7a0abb855cbull },
    { "2017.4.40f1 arm64-v8a", "Playable::PrepareFrameVisitor", 0x509f1c, 0x1a0, ISA_A64, 0xeec4d2385578337eull },
    { "2017.4.40f1 arm64-v8a", "Animator::Prepare", 0x3e0cc8, 0x50, ISA_A64, 0x6505c742a4f610e1ull },
    { "2017.4.40f1 arm64-v8a", "Animator::BuildJobs", 0x3e0964, 0x364, ISA_A64, 0x2cf5027e2ab0b7c2ull },
    { "2017.4.40f1 arm64-v8a", "Animator::UpdateAvatars", 0x3e0300, 0x654, ISA_A64, 0xf2d055b04d3fad31ull },
    { "2017.4.40f1 arm64-v8a", "Animator::ApplyOnAnimatorMove", 0x3e1b94, 0x454, ISA_A64, 0xb3a1ac64466032c3ull },
    { "2017.4.40f1 arm64-v8a", "AnimationPlayable::GetApplyMotionX", 0x43917c, 0x74, ISA_A64, 0x7e0d79e2db7a043aull },
    { "2017.4.40f1 arm64-v8a", "AnimationPlayable::GetNextCompatibleDescendant", 0x4326d0, 0x5c, ISA_A64, 0x262782d79af8dab1ull },
    { "2017.4.40f1 arm64-v8a", "UI::RectTransform::UpdateAnchorPositionIfTransformChanged", 0x8f7670, 0x138, ISA_A64, 0xe53d12604f606169ull },
    { "2017.4.40f1 arm64-v8a", "UI::Canvas::UpdateCanvasRectTransform", 0x907154, 0x45c, ISA_A64, 0xe9c761fb7f1d3bb6ull },
    { "2017.4.40f1 arm64-v8a", "GameObject::QueryComponentByType", 0x45875c, 0x44, ISA_A64, 0x7ca00f542b3d75b5ull },
    { "2017.4.40f1 arm64-v8a", "InputAxis::Update", 0x6e5c24, 0x3d4, ISA_A64, 0xa8a8ffa7d818c0d0ull },
    { "2017.4.40f1 arm64-v8a", "InputManager::GetAxis", 0x6e848c, 0xe4, ISA_A64, 0x85f8b8facdfc5571ull },
    { "2017.4.40f1 arm64-v8a", "MonoBehaviour::CallUpdateMethod", 0x75a57c, 0x12c, ISA_A64, 0x440476c132ea0595ull },
    { "2017.4.40f1 arm64-v8a", "UI::Populate", 0x9108fc, 0x258, ISA_A64, 0xb3063810ad622ee8ull },
    { "2017.4.40f1 arm64-v8a", "ThreadsafeLinearAllocator::Allocate", 0x3ce2f0, 0x210, ISA_A64, 0xdf3b55d0c0bbb3d5ull },
};
enum { NCOUNT = sizeof g_count / sizeof *g_count };

static uint64_t g_calls[NCOUNT];            // written by the stubs (plain adds: a count, not a proof)
static uint64_t g_last[NCOUNT];
static int g_matched[NCOUNT];
static double g_count_secs;
static uintptr_t g_base;

// The stub, 12 words: ldr x16, =counter; ldr x17, [x16]; add x17, x17, #1; str x17, [x16]; ldr x16, =orig; br x16;
// 2 words pad; the counter's address; the original's address
static uint32_t *count_stub(uint64_t *counter)
{
    static uint32_t *page; static size_t used;
    long pg = sysconf(_SC_PAGESIZE);
    if (!page || used + 48 > (size_t)pg) {
        page = mmap(NULL, pg, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) { page = NULL; return NULL; }
        used = 0;
    }
    uint32_t *s = page + used / 4;
    used += 48;
    s[0] = 0x58000000u | 8u << 5 | 16;      // ldr x16, #32 (word 8)
    s[1] = 0xf9400211u;                     // ldr x17, [x16]
    s[2] = 0x91000631u;                     // add x17, x17, #1
    s[3] = 0xf9000211u;                     // str x17, [x16]
    s[4] = 0x58000000u | 6u << 5 | 16;      // ldr x16, #24 (word 10)
    s[5] = 0xd61f0200u;                     // br x16
    s[6] = s[7] = 0xd503201fu;              // nop
    uint64_t a = (uint64_t)(uintptr_t)counter;
    memcpy(&s[8], &a, 8);
    memset(&s[10], 0, 8);
    return s;
}

// --- UA_ANCHOR_WALK: UI::RectTransform::UpdateAnchorPositionIfTransformChanged(bool)
//
// What Unity does (read in 6000.0.61f1, 2017.4.27f1 and 2017.4.40f1 arm64-v8a; the same algorithm in all three):
// unless a re-entry count is above 1 (and, in 6000.0, the transform has no hierarchy), it finds the transform's
// hierarchy and index, climbs to the topmost ancestor whose change bit for the RectTransform system is set, and then
// walks that ancestor's whole subtree (deepChildCount entries, in hierarchy order through the next-index array): for
// each it clears the bit (a store every time, of the same word when the bit was clear), and where the bit was set
// (6000.0: also clearing a second system's bit) and the transform is a RectTransform, it calls
// RectTransform::UpdateAnchorPosition(transform, bool). Then it calls the hierarchy's dispatch tail.
// UI::Canvas::UpdateCanvasRectTransform calls it through SetAnchorMin, SetAnchorMax and SetPivot of every root canvas,
// twice a frame (EarlyUpdate and PostLateUpdate): six walks of every canvas's whole subtree a frame, nearly all of
// whose entries have the bit clear.
//
// Ours: the same steps in the same order, except that a stretch of entries whose bit is clear and whose next index
// is the following entry (a subtree laid out in order, as hierarchies are built) is passed by reading the two
// arrays forward instead of one dependent load after another, and the unchanged words are not written back (the
// same value Unity stores). Every entry with the bit set takes Unity's step, with the arrays' addresses read again
// from the hierarchy after each call, as Unity's code does. =verify walks each passed stretch again one next index
// at a time and checks the bit of every entry and where it ends.
struct anchor_build {
    struct row fn;
    int v6;                                     // 6000.0 layout (GetTransformAccess, two bits, tail(this))
    uint32_t guard, bit, bit2, table, rect, update, access, tail, tailarg;
    uint32_t o_parent, o_deep, o_changed, o_xforms, o_next;
};
static const struct anchor_build g_anchor_builds[] = {
    { { "6000.0.61f1 arm64-v8a", "UI::RectTransform::UpdateAnchorPositionIfTransformChanged", 0x919500, 0x140, ISA_A64, 0x2e08bf851f20a291ull },
      1, 0x1a2a4c4, 0x1a2a4b8, 0x1a2a4bc, 0x1995050, 0x199e778, 0x919784, 0x919a78, 0x919abc, 0,
      0x20, 0x28, 0x48, 0x30, 0x70 },
    { { "2017.4.40f1 arm64-v8a", "UI::RectTransform::UpdateAnchorPositionIfTransformChanged", 0x8f7670, 0x138, ISA_A64, 0xe53d12604f606169ull },
      0, 0x12c0330, 0x12c0328, 0, 0x11e66e8, 0x11f0f18, 0x8f790c, 0, 0x8f9dd0, 0x12c0398,
      0x10, 0x18, 0x60, 0x20, 0x80 },
    { { "2017.4.27f1 arm64-v8a", "UI::RectTransform::UpdateAnchorPositionIfTransformChanged", 0x8f780c, 0x138, ISA_A64, 0x4da4ea551194c887ull },
      0, 0x12eb110, 0x12eb108, 0, 0x12116e8, 0x121bef8, 0x8f7aa8, 0, 0x8f9f6c, 0x12eb178,
      0x10, 0x18, 0x60, 0x20, 0x80 },
};
static const struct anchor_build *g_ab;
// what the walk reads and calls, resolved from the row's offsets (a test points them at its model)
struct anchor_env {
    int v6;
    int32_t volatile *guard;
    uint32_t volatile *bit, *bit2;
    uint64_t volatile *table;
    uintptr_t rect;
    void (*update)(uintptr_t, int);
    struct access_pair (*access)(uintptr_t);
    void (*tail6)(uintptr_t);
    void (*tail17)(uintptr_t, uintptr_t);
    uint64_t volatile *tailarg;
    uint32_t o_parent, o_deep, o_changed, o_xforms, o_next;
};
struct access_pair { uintptr_t hier; uint64_t index; };
static struct anchor_env g_ae;
static int g_anchor_mode;                       // 1 on, 2 verify, 3 stats (on, with counts)
static uint64_t g_aw_walks, g_aw_entries, g_aw_set, g_aw_passed, g_aw_verified, g_aw_bad;

#define LD64(p, o) (*(uint64_t volatile *)((p) + (o)))

static void anchor_walk(uintptr_t self, int flag)
{
    const struct anchor_env *b = &g_ae;
    int32_t guard = *b->guard;
    uintptr_t hier; uint32_t idx;
    if (b->v6) {
        if (guard > 1 || LD64(self, 0x28) == 0) return;
        struct access_pair a = b->access(self);
        hier = a.hier; idx = (uint32_t)a.index;
    } else {
        if (guard > 1) return;
        hier = (uintptr_t)LD64(self, 0x38); idx = (uint32_t)LD64(self, 0x40);
    }
    uint64_t bit = 1ull << (*b->bit & 63);
    for (;;) {                                  // up to the topmost ancestor whose bit is set
        int32_t p = ((int32_t volatile *)(uintptr_t)LD64(hier, b->o_parent))[idx];
        if (p < 0) break;
        if (!(((uint64_t volatile *)(uintptr_t)LD64(hier, b->o_changed))[(uint32_t)p] & bit)) break;
        idx = (uint32_t)p;
    }
    uint32_t n = (uint32_t)((int32_t volatile *)(uintptr_t)LD64(hier, b->o_deep))[idx];
    if (g_anchor_mode == 3) { g_aw_walks++; g_aw_entries += n; }
    uintptr_t table = (uintptr_t)*b->table, rect = b->rect;
    while (n) {
        uint64_t volatile *chg = (uint64_t volatile *)(uintptr_t)LD64(hier, b->o_changed);
        uint32_t volatile *next = (uint32_t volatile *)(uintptr_t)LD64(hier, b->o_next);
        uint64_t w = chg[idx];
        if (w & bit) {                          // Unity's step
            w &= ~bit;
            if (b->v6) w &= ~(1ull << (*b->bit2 & 63));
            chg[idx] = w;
            uintptr_t t = (uintptr_t)((uint64_t volatile *)(uintptr_t)LD64(hier, b->o_xforms))[idx];
            uint32_t ty = *(uint32_t volatile *)(t + 0xc) >> 21;
            if (*(uint64_t volatile *)(table + 8ull * ty + 8) == rect) b->update(t, flag & 1);
            if (g_anchor_mode == 3) g_aw_set++;
            idx = ((uint32_t volatile *)(uintptr_t)LD64(hier, b->o_next))[idx];
            n--;
            continue;
        }
        // a stretch with the bit clear: entries in order are passed by index, the arrays read forward
        uint32_t i = idx, left = n;
        for (;;) {
            left--;
            uint32_t nx = next[i];
            if (!left) { i = nx; break; }
            if (nx != i + 1 || (chg[i + 1] & bit)) { i = nx; break; }
            i = i + 1;                          // not nx: the next loads need not wait for this one
        }
        if (g_anchor_mode == 2) {               // the same stretch Unity's way
            uint32_t j = idx, k = n - left;
            for (uint32_t m = 0; m < k; m++) {
                if (chg[j] & bit) g_aw_bad++;
                j = next[j];
            }
            if (j != i) g_aw_bad++;
            g_aw_verified += k;
        }
        if (g_anchor_mode == 3) g_aw_passed += n - left;
        idx = i; n = left;
    }
    if (b->v6) b->tail6(self);
    else b->tail17((uintptr_t)*b->tailarg, (uintptr_t)LD64(self, 0x38));
}

static void anchor_env_from(const struct anchor_build *ab, uintptr_t base)
{
    struct anchor_env *e = &g_ae;
    e->v6 = ab->v6;
    e->guard = (int32_t volatile *)(base + ab->guard);
    e->bit = (uint32_t volatile *)(base + ab->bit);
    e->bit2 = (uint32_t volatile *)(base + ab->bit2);
    e->table = (uint64_t volatile *)(base + ab->table);
    e->rect = base + ab->rect;
    e->update = (void (*)(uintptr_t, int))(base + ab->update);
    e->access = (struct access_pair (*)(uintptr_t))(base + ab->access);
    e->tail6 = (void (*)(uintptr_t))(base + ab->tail);
    e->tail17 = (void (*)(uintptr_t, uintptr_t))(base + ab->tail);
    e->tailarg = (uint64_t volatile *)(base + ab->tailarg);
    e->o_parent = ab->o_parent; e->o_deep = ab->o_deep; e->o_changed = ab->o_changed;
    e->o_xforms = ab->o_xforms; e->o_next = ab->o_next;
}

static void anchor_match(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_ANCHOR_WALK");
    if (!v || !*v || !strcmp(v, "0")) return;
    g_anchor_mode = !strcmp(v, "verify") ? 2 : !strcmp(v, "stats") ? 3 : 1;
    for (size_t i = 0; i < sizeof g_anchor_builds / sizeof *g_anchor_builds; i++)
        if (unityhooks_matches(&g_anchor_builds[i].fn, base, size)) { g_ab = &g_anchor_builds[i]; break; }
    if (g_ab) anchor_env_from(g_ab, base);
    if (!g_ab) {
        fprintf(stderr, "[enginerw] UA_ANCHOR_WALK: this libunity.so is not one whose anchor walk was read; left as Unity's\n");
        g_anchor_mode = 0;
    }
}

static void anchor_patch(uintptr_t base)
{
    if (!g_ab) return;
    if (unityhooks_jump(base + g_ab->fn.off, ISA_A64, (void *)anchor_walk) != 0) {
        fprintf(stderr, "[enginerw] UA_ANCHOR_WALK: hook failed; Unity's walk\n");
        g_ab = NULL; g_anchor_mode = 0;
        return;
    }
    fprintf(stderr, "[enginerw] UA_ANCHOR_WALK=%s: %s replaced (libunity %s, +0x%x)\n",
            g_anchor_mode == 2 ? "verify" : g_anchor_mode == 3 ? "stats" : "1", g_ab->fn.func, g_ab->fn.unity, g_ab->fn.off);
}

static void anchor_report(unsigned long nf)
{
    if (g_anchor_mode == 2)
        fprintf(stderr, "[enginerw] UA_ANCHOR_WALK=verify: %llu entries passed and walked again, %llu differ\n",
                (unsigned long long)g_aw_verified, (unsigned long long)g_aw_bad);
    if (g_anchor_mode == 3 && nf)
        fprintf(stderr, "[enginerw] UA_ANCHOR_WALK stats a frame: %.1f walks, %.1f entries, %.1f set, %.1f passed\n",
                (double)g_aw_walks / nf, (double)g_aw_entries / nf, (double)g_aw_set / nf, (double)g_aw_passed / nf);
    g_aw_walks = g_aw_entries = g_aw_set = g_aw_passed = 0;
}

// --- UA_CULL_STEP1: ProcessCameraIndexListIsNodeVisibleStep1(const SceneCullingParameters &p, const SceneNode *nodes,
// size_t begin, size_t end, IndexList &out)
//
// What Unity does (6000.0.61f1 arm64-v8a, read from the player): for each 24-byte SceneNode from begin to end it skips
// a node whose word at +0x14 has both top bits set, else calls IsNodeVisibleFast(node, p, p's LOD data at +0x1b8),
// and appends the node's number (32 bits) to out's array (out+0) when that says so; at the end out's count (+8, 32
// bits) is the number appended. IsNodeVisibleFast: the camera's layer mask (p+0xc0, 32 bits) shifted right by the
// node's layer (+8; the shift taken mod 32, as LSRV does) must have bit 0, the node's renderer (+0) be set, bit 29 of
// +0x14 clear, the bits 1 - 2 of the byte at +0x13 clear; then a node without LOD (+0x14's low 28 bits 0) is visible,
// one with LOD is visible when its mask byte (+0x12) meets the LOD group's current mask byte (the array at LOD data
// + 24 * the 16-bit group at +0x10, indexed by those 28 bits). Pure: it reads the nodes, p and the LOD data and
// writes only out. Ours is the same tests in one loop without the call per node; Unity's cull prefetch (nodes 8 and 10
// ahead) kept as a hint. =verify runs Unity's into out after ours into a buffer of the thread's and compares.
static const struct row g_cull_rows[] = {
    { "6000.0.61f1 arm64-v8a", "ProcessCameraIndexListIsNodeVisibleStep1", 0x785d38, 0xa8, ISA_A64, 0x045c3175edd29d36ull },
    { "6000.0.61f1 arm64-v8a", "IsNodeVisibleFast", 0x785de0, 0x6c, ISA_A64, 0x959ef690b160b089ull },
};
static int g_cull_mode;                         // 1 on, 2 verify
static void (*g_cull_orig)(uintptr_t, uintptr_t, size_t, size_t, uintptr_t);
static uint64_t g_cull_calls, g_cull_nodes, g_cull_bad;

static uint32_t cull_step1(uintptr_t p, uintptr_t nodes, size_t begin, size_t end, int32_t *dst)
{
    uint32_t n = 0;
    if (begin >= end) return 0;
    uint32_t mask = *(uint32_t volatile *)(p + 0xc0);
    uintptr_t lod = (uintptr_t)*(uint64_t volatile *)(p + 0x1b8);
    const uint8_t *nd = (const uint8_t *)(nodes + 24 * begin);
    for (size_t i = begin; i < end; i++, nd += 24) {
        __builtin_prefetch(nd + 0xc0);
        uint32_t w = *(uint32_t const volatile *)(nd + 0x14);
        if ((w >> 30) > 2) continue;
        uint32_t layer = *(uint32_t const volatile *)(nd + 8);
        if (!((mask >> (layer & 31)) & 1)) continue;
        if (!*(uint64_t const volatile *)nd) continue;
        if (w & (1u << 29)) continue;
        if (nd[0x13] & 6) continue;
        uint32_t li = w & 0xfffffff;
        if (li) {
            uint16_t g = *(uint16_t const volatile *)(nd + 0x10);
            const uint8_t *arr = (const uint8_t *)(uintptr_t)*(uint64_t volatile *)(lod + 24ull * g);
            if (!(nd[0x12] & arr[li])) continue;
        }
        dst[n++] = (int32_t)i;
    }
    return n;
}

static void cull_hook(uintptr_t p, uintptr_t nodes, size_t begin, size_t end, uintptr_t out)
{
    int32_t *dst = (int32_t *)(uintptr_t)*(uint64_t volatile *)out;
    if (g_cull_mode == 2) {
        static __thread int32_t *buf; static __thread size_t cap;
        size_t need = end > begin ? end - begin : 1;
        if (cap < need) { free(buf); buf = malloc(need * 4); cap = buf ? need : 0; }
        uint32_t n = buf ? cull_step1(p, nodes, begin, end, buf) : 0;
        g_cull_orig(p, nodes, begin, end, out);
        uint32_t un = *(uint32_t volatile *)(out + 8);
        if (!buf || n != un || memcmp(buf, dst, 4ull * n)) __atomic_add_fetch(&g_cull_bad, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_cull_calls, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_cull_nodes, end > begin ? end - begin : 0, __ATOMIC_RELAXED);
        return;
    }
    *(uint32_t volatile *)(out + 8) = cull_step1(p, nodes, begin, end, dst);
}

static const struct row *g_cull_row;
static void cull_match(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_CULL_STEP1");
    if (!v || !*v || !strcmp(v, "0")) return;
    g_cull_mode = !strcmp(v, "verify") ? 2 : 1;
    if (unityhooks_matches(&g_cull_rows[0], base, size) && unityhooks_matches(&g_cull_rows[1], base, size)) g_cull_row = &g_cull_rows[0];
    else { fprintf(stderr, "[enginerw] UA_CULL_STEP1: this libunity.so is not one whose culling pass was read; left as Unity's\n"); g_cull_mode = 0; }
}
static void cull_patch(uintptr_t base)
{
    if (!g_cull_row) return;
    int r = g_cull_mode == 2 ? unityhooks_hook(base + g_cull_row->off, ISA_A64, (void *)cull_hook, (void **)&g_cull_orig)
                             : unityhooks_jump(base + g_cull_row->off, ISA_A64, (void *)cull_hook);
    if (r != 0) { fprintf(stderr, "[enginerw] UA_CULL_STEP1: hook failed; Unity's\n"); g_cull_mode = 0; return; }
    fprintf(stderr, "[enginerw] UA_CULL_STEP1=%s: %s replaced (libunity %s, +0x%x)\n", g_cull_mode == 2 ? "verify" : "1",
            g_cull_row->func, g_cull_row->unity, g_cull_row->off);
}
static void cull_report(void)
{
    if (g_cull_mode == 2)
        fprintf(stderr, "[enginerw] UA_CULL_STEP1=verify: %llu passes, %llu nodes, %llu lists differ\n",
                (unsigned long long)g_cull_calls, (unsigned long long)g_cull_nodes, (unsigned long long)g_cull_bad);
}

// --- UA_DIRECTOR_PREWALK: DirectorManager::ExecutePrepareFrames(this, type)
//
// What Unity does (6000.0.61f1 arm64-v8a): ExecutePrepareFrames first calls RebuildPrepareFrameJobs(this) (the bl at
// +0x24, the type still in w1), then prepares the graphs of the type's two lists (this + type * 32 + 0x90 for the
// workers' jobs and + 0x170 for the main thread's, each a vector {data, .., count at +0x10} of 0x50-byte entries
// whose graph is at +0x48): PlayableGraph::PrepareFrame walks the graph's outputs (a list at graph + 0x78, its first
// node at + 0x80, next at node + 8, the output at node - 8) and PlayableOutput::PrepareTraverse each one: its source
// handle (output + 0x30, valid when the handle's +0x20 equals output + 0x38 with bit 0 clear) gives the root
// playable (handle + 0x28, unless its byte +0x38 has bit 1 set); PreOrderPlayableTraverser::Traverse then visits each
// playable P: P's handle node (P + 0x28; its + 0x20 the version), the node it points to (+0x28), that node's input
// set (+0x30: inputs at +0x20, 16 bytes each with the input playable first, count at +0x30), and recurses into
// each input. Every step is a load whose address the previous load gave: on the in-order Cortex-A53 a wait for
// memory a node, about 810 nodes a frame in Hollow Knight's King's Pass (82 Animators' graphs).
//
// Ours: the call of RebuildPrepareFrameJobs is pointed at a function here that calls it and then walks the type's
// lists' graphs the same way, but level by level across all graphs at once, each level's loads issued as PRFM first
// and read after (so the misses of a level overlap instead of following each other). It reads only what Unity's
// traversal reads, through pointers checked as Unity checks them (non-null, the handle versions), within bounds (at
// most 4096 nodes, 32 levels, 64 inputs a playable), and writes nothing of Unity's.
// Unity 2017.4 (2017.4.27f1 and 2017.4.40f1 arm64-v8a) has the same lists and the same traversal over a simpler
// layout: no rebuild call (the function's entry is hooked: ours walks, then Unity's runs), a graph's outputs at
// graph + 0x68 (first node + 0x70), the output's source handle at + 0x28 (valid when its + 0x20 equals output +
// 0x30), the root's skip flag bit 0 of + 0xa8, and a playable's inputs reached through its node (P + 0x18: inputs at
// + 0x20, 24 bytes each with the input playable at + 8, count at + 0x30).
struct dpw_build {
    struct row fn, rebuild, other[3];
    int v6;
    uint32_t g_head, g_first, o_handle, o_ver, p_skip, p_skipbit, in_stride, in_child;
};
static const struct dpw_build g_dpw_builds[] = {
    { { "6000.0.61f1 arm64-v8a", "DirectorManager::ExecutePrepareFrames", 0x9af89c, 0x200, ISA_A64, 0x0e779fe165394f3dull },
      { "6000.0.61f1 arm64-v8a", "DirectorManager::RebuildPrepareFrameJobs", 0x9af1d4, 0x6c, ISA_A64, 0xe7aba697b5e68915ull },
      { { "6000.0.61f1 arm64-v8a", "PlayableOutput::PrepareTraverse", 0x9b1a30, 0x174, ISA_A64, 0x70970997fc7be978ull },
        { "6000.0.61f1 arm64-v8a", "PreOrderPlayableTraverser::Traverse", 0x9b1be0, 0x1f0, ISA_A64, 0x196dd5dbc9ecc92dull },
        { "6000.0.61f1 arm64-v8a", "PlayableGraph::PrepareFrame", 0x9ad0e0, 0x138, ISA_A64, 0xf052d26e9bef7c25ull } },
      1, 0x78, 0x80, 0x30, 0x38, 0x38, 2, 16, 0 },
    { { "2017.4.40f1 arm64-v8a", "DirectorManager::ExecutePrepareFrames", 0x504a0c, 0x24c, ISA_A64, 0x9fdd8d42324456c5ull },
      { 0 },
      { { "2017.4.40f1 arm64-v8a", "PlayableOutput::PrepareTraverse", 0x50ab74, 0x164, ISA_A64, 0x0300d13fe713d822ull },
        { "2017.4.40f1 arm64-v8a", "PreOrderPlayableTraverser::Traverse", 0x50b260, 0x244, ISA_A64, 0x0021f7a0abb855cbull },
        { "2017.4.40f1 arm64-v8a", "PlayableGraph::PrepareFrame", 0x504734, 0x144, ISA_A64, 0x1ff0fcdc70ec4a94ull } },
      0, 0x68, 0x70, 0x28, 0x30, 0xa8, 1, 24, 8 },
    { { "2017.4.27f1 arm64-v8a", "DirectorManager::ExecutePrepareFrames", 0x503e88, 0x24c, ISA_A64, 0x18e57083b56ca495ull },
      { 0 },
      { { "2017.4.27f1 arm64-v8a", "PlayableOutput::PrepareTraverse", 0x509ff0, 0x164, ISA_A64, 0x1f417986f327c36cull },
        { "2017.4.27f1 arm64-v8a", "PreOrderPlayableTraverser::Traverse", 0x50a6dc, 0x244, ISA_A64, 0x0021f7a0abb855cbull },
        { "2017.4.27f1 arm64-v8a", "PlayableGraph::PrepareFrame", 0x503bb0, 0x144, ISA_A64, 0x9fe1e3e3d9ba893eull } },
      0, 0x68, 0x70, 0x28, 0x30, 0xa8, 1, 24, 8 },
};
enum { DPW_CALL = 0x24, DPW_MAXN = 4096, DPW_LEVELS = 32, DPW_MAXIN = 64 };
static const struct dpw_build *g_dpw;
static int g_dpw_mode;                          // 1 on, 3 stats
static void (*g_dpw_rebuild)(uintptr_t);
static void (*g_dpw_orig)(uintptr_t, uint32_t);
static uint64_t g_dpw_passes, g_dpw_graphs, g_dpw_nodes, g_dpw_levels;

#define PF2(a) __builtin_prefetch((const void *)(a), 0, 2)
static inline uint64_t rd64(uintptr_t a) { return *(uint64_t const volatile *)a; }
static inline uint32_t rd32(uintptr_t a) { return *(uint32_t const volatile *)a; }

static void dpw_walk(uintptr_t self, uint32_t type)
{
    static uintptr_t front[2][DPW_MAXN];
    const struct dpw_build *b = g_dpw;
    uintptr_t *cur = front[0], *nxt = front[1];
    size_t n = 0, graphs = 0, nodes = 0;
    if (type > 6) return;
    uintptr_t lists[2] = { self + 32ull * type + 0x90, self + 32ull * type + 0x170 };
    // the roots: every output's source playable, as PrepareTraverse finds it
    for (int l = 0; l < 2; l++) {
        uintptr_t data = (uintptr_t)rd64(lists[l]);
        uint64_t cnt = rd64(lists[l] + 0x10);
        if (!data || cnt > DPW_MAXN) continue;
        for (uint64_t k = 0; k < cnt; k++) PF2(data + 0x50 * k + 0x48);
        for (uint64_t k = 0; k < cnt; k++) {
            uintptr_t g = (uintptr_t)rd64(data + 0x50 * k + 0x48);
            if (!g) continue;
            graphs++;
            uintptr_t head = g + b->g_head;
            int guard = 0;
            for (uintptr_t nd = (uintptr_t)rd64(g + b->g_first); nd && nd != head && guard < 64; nd = (uintptr_t)rd64(nd + 8), guard++) {
                uintptr_t o = nd - 8;
                PF2(o); PF2(o + 0x40);
                uintptr_t h = (uintptr_t)rd64(o + b->o_handle);
                if (!h) continue;
                uint32_t ov = rd32(o + b->o_ver);
                if (rd32(h + 0x20) != (b->v6 ? ov & ~1u : ov)) continue;
                uintptr_t p = (uintptr_t)rd64(h + 0x28);
                if (!p || (*(uint8_t const volatile *)(p + b->p_skip) & b->p_skipbit) || n >= DPW_MAXN) continue;
                PF2(p); PF2(p + 0x40); PF2(p + 0x80);
                cur[n++] = p;
            }
        }
    }
    int levels = 0;
    while (n && levels < DPW_LEVELS) {
        levels++; nodes += n;
        size_t m = 0;
        for (size_t i = 0; i < n; i++) { PF2(cur[i]); PF2(cur[i] + 0x40); PF2(cur[i] + 0x80); }
        if (b->v6) {
            // P -> its handle node (+0x28) -> (version bit 0 clear, Unity's check) the node it points to (+0x28) ->
            // its input set (+0x30)
            for (size_t i = 0; i < n; i++) { uintptr_t hp = (uintptr_t)rd64(cur[i] + 0x28); cur[i] = hp; if (hp) PF2(hp + 0x20); }
            for (size_t i = 0; i < n; i++) {
                uintptr_t hp = cur[i], x = 0;
                if (hp && !(rd32(hp + 0x20) & 1)) x = (uintptr_t)rd64(hp + 0x28);
                cur[i] = x;
                if (x) { PF2(x + 0x30); PF2(x + 0x40); }
            }
            for (size_t i = 0; i < n; i++) {
                uintptr_t x = cur[i], is = x ? (uintptr_t)rd64(x + 0x30) : 0;
                cur[i] = is;
                if (is) PF2(is + 0x20);
            }
        } else {
            // P -> its node (+0x18)
            for (size_t i = 0; i < n; i++) { uintptr_t nd = (uintptr_t)rd64(cur[i] + 0x18); cur[i] = nd; if (nd) PF2(nd + 0x20); }
        }
        // -> the inputs: the array at +0x20, count at +0x30, the playable at in_child of each in_stride bytes
        for (size_t i = 0; i < n; i++) {
            uintptr_t is = cur[i];
            if (!is) continue;
            uintptr_t arr = (uintptr_t)rd64(is + 0x20);
            uint64_t cnt = rd64(is + 0x30);
            if (!arr || cnt > DPW_MAXIN) continue;
            for (uint64_t k = 0; k < cnt; k++) PF2(arr + b->in_stride * k + b->in_child);
            for (uint64_t k = 0; k < cnt && m < DPW_MAXN; k++) {
                uintptr_t c = (uintptr_t)rd64(arr + b->in_stride * k + b->in_child);
                if (c) { PF2(c); nxt[m++] = c; }
            }
        }
        uintptr_t *t = cur; cur = nxt; nxt = t; n = m;
    }
    if (g_dpw_mode == 3) { g_dpw_passes++; g_dpw_graphs += graphs; g_dpw_nodes += nodes; g_dpw_levels += (uint64_t)levels; }
}

// 6000.0: called from ExecutePrepareFrames where it called RebuildPrepareFrameJobs: x0 = this, w1 = the type
// (ExecutePrepareFrames' own w1, untouched until that call)
static void dpw_rebuild_then_walk(uintptr_t self, uint32_t type)
{
    g_dpw_rebuild(self);
    dpw_walk(self, type);
}
// 2017.4: ExecutePrepareFrames' entry
static void dpw_entry(uintptr_t self, uint32_t stage)
{
    dpw_walk(self, stage);
    g_dpw_orig(self, stage);
}

static void dpw_match(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_DIRECTOR_PREWALK");
    if (!v || !*v || !strcmp(v, "0")) return;
    g_dpw_mode = !strcmp(v, "stats") ? 3 : 1;
    for (size_t k = 0; k < sizeof g_dpw_builds / sizeof *g_dpw_builds && !g_dpw; k++) {
        const struct dpw_build *b = &g_dpw_builds[k];
        int ok = unityhooks_matches(&b->fn, base, size);
        for (int i = 0; i < 3 && ok; i++) ok = unityhooks_matches(&b->other[i], base, size);
        if (ok && b->v6) {
            uint32_t w;
            memcpy(&w, (const void *)(base + b->fn.off + DPW_CALL), 4);
            ok = unityhooks_matches(&b->rebuild, base, size) && w == unityprefetch_bl(base + b->fn.off + DPW_CALL, base + b->rebuild.off);
        }
        if (ok) g_dpw = b;
    }
    if (!g_dpw) { fprintf(stderr, "[enginerw] UA_DIRECTOR_PREWALK: this libunity.so is not one whose Director pass was read; off\n"); g_dpw_mode = 0; }
}
static void dpw_patch(uintptr_t base, size_t size)
{
    if (!g_dpw) return;
    if (!g_dpw->v6) {
        if (unityhooks_hook(base + g_dpw->fn.off, ISA_A64, (void *)dpw_entry, (void **)&g_dpw_orig) != 0) {
            fprintf(stderr, "[enginerw] UA_DIRECTOR_PREWALK: hook failed; off\n"); g_dpw = NULL; g_dpw_mode = 0; return;
        }
    } else {
        long pg = sysconf(_SC_PAGESIZE);
        uint32_t *v = unityprefetch_near(base, base + size, (size_t)pg);
        if (!v) { fprintf(stderr, "[enginerw] UA_DIRECTOR_PREWALK: no page near libunity; off\n"); g_dpw = NULL; g_dpw_mode = 0; return; }
        unityprefetch_tail(v, (uint64_t)(uintptr_t)dpw_rebuild_then_walk);
        mprotect(v, (size_t)pg, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)v, (char *)(v + 4));
        g_dpw_rebuild = (void (*)(uintptr_t))(base + g_dpw->rebuild.off);
        uintptr_t at = base + g_dpw->fn.off + DPW_CALL, a = at & ~(uintptr_t)(pg - 1);
        uint32_t bl = unityprefetch_bl(at, (uint64_t)(uintptr_t)v);
        if (!bl || mprotect((void *)a, (size_t)pg, PROT_READ | PROT_WRITE) != 0) { g_dpw = NULL; g_dpw_mode = 0; return; }
        memcpy((void *)at, &bl, 4);
        mprotect((void *)a, (size_t)pg, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)at, (char *)(at + 4));
    }
    fprintf(stderr, "[enginerw] UA_DIRECTOR_PREWALK=%s: %s's graphs walked ahead (libunity %s)\n", g_dpw_mode == 3 ? "stats" : "1",
            g_dpw->fn.func, g_dpw->fn.unity);
}
static void dpw_report(unsigned long nf)
{
    if (g_dpw_mode == 3 && nf)
        fprintf(stderr, "[enginerw] UA_DIRECTOR_PREWALK stats a frame: %.1f passes, %.1f graphs, %.1f nodes, %.1f levels\n",
                (double)g_dpw_passes / nf, (double)g_dpw_graphs / nf, (double)g_dpw_nodes / nf, (double)g_dpw_levels / nf);
    g_dpw_passes = g_dpw_graphs = g_dpw_nodes = g_dpw_levels = 0;
}

// --- UA_PREFETCH's `anim` on Unity 2017.4 (2017.4.27f1, 2017.4.40f1 arm64-v8a)
//
// Animator::BuildJobs walks its outputs (x25: the vector {array, .., count at +0x10}, x20 the index) and calls
// AnimationPlayableOutput::GetTargetAnimator (`ldr x0, [x0, #0x78]`) at +0x84 for each; then reads the Animator's
// vtable, +0xc8 - +0xdc, +0x108 (and that object's +0xa4), +0x2a4, +0x418 (Animator::Prepare). The stub, in place of
// that call: two outputs ahead PRFM of the output's lines; one ahead its Animator (read from +0x78, as the getter
// does) and its source handle (+0x28) PRFM'd. Scratch x9 - x12, x16, x17 (the getter reads only x0; the loop holds
// nothing in them across a call). Loads of the array (up to its count) and of +0x78 / +0x28 of outputs in it only.
__asm__(
    "    .text\n"
    "    .p2align 3\n"
    "    .globl ua_erw_anim17_start, ua_erw_anim17_tail, ua_erw_anim17_end\n"
    "ua_erw_anim17_start:\n"
    "    ldr  x9, [x25]\n"
    "    ldr  x10, [x25, #0x10]\n"
    "    add  x11, x20, #2\n"
    "    cmp  x11, x10\n"
    "    b.hs 1f\n"
    "    ldr  x12, [x9, x11, lsl #3]\n"
    "    prfm pldl1keep, [x12]\n"
    "    prfm pldl1keep, [x12, #0x40]\n"
    "1:  add  x11, x20, #1\n"
    "    cmp  x11, x10\n"
    "    b.hs ua_erw_anim17_tail\n"
    "    ldr  x12, [x9, x11, lsl #3]\n"
    "    ldr  x16, [x12, #0x78]\n"
    "    cbz  x16, 2f\n"
    "    prfm pldl1keep, [x16]\n"
    "    prfm pldl1keep, [x16, #0xc8]\n"
    "    prfm pldl1keep, [x16, #0x108]\n"
    "    prfm pldl1keep, [x16, #0x2a0]\n"
    "    prfm pldl1keep, [x16, #0x418]\n"
    "2:  ldr  x17, [x12, #0x28]\n"
    "    cbz  x17, ua_erw_anim17_tail\n"
    "    prfm pldl1keep, [x17, #0x20]\n"
    "ua_erw_anim17_tail:\n"
    "    .word 0, 0, 0, 0\n"
    "ua_erw_anim17_end:\n"
);
extern const uint8_t ua_erw_anim17_start[], ua_erw_anim17_tail[], ua_erw_anim17_end[];
struct anim17 { struct row fn, getter; uint32_t call; };
static const struct anim17 g_anim17[] = {
    { { "2017.4.40f1 arm64-v8a", "Animator::BuildJobs", 0x3e0964, 0x364, ISA_A64, 0x2cf5027e2ab0b7c2ull },
      { "2017.4.40f1 arm64-v8a", "AnimationPlayableOutput::GetTargetAnimator", 0x43413c, 0x8, ISA_A64, 0xb423b85ef8fd7be6ull }, 0x84 },
    { { "2017.4.27f1 arm64-v8a", "Animator::BuildJobs", 0x3dfed4, 0x364, ISA_A64, 0xd9aee45f61bf59c4ull },
      { "2017.4.27f1 arm64-v8a", "AnimationPlayableOutput::GetTargetAnimator", 0x4336ac, 0x8, ISA_A64, 0xb423b85ef8fd7be6ull }, 0x84 },
};
static const struct anim17 *g_a17;
static void anim17_match(uintptr_t base, size_t size)
{
    if (!unityprefetch_wanted(getenv("UA_PREFETCH"), "anim")) return;
    for (size_t i = 0; i < sizeof g_anim17 / sizeof *g_anim17; i++)
        if (unityhooks_matches(&g_anim17[i].fn, base, size) && unityhooks_matches(&g_anim17[i].getter, base, size)) {
            uint32_t w;
            memcpy(&w, (const void *)(base + g_anim17[i].fn.off + g_anim17[i].call), 4);
            if (w == unityprefetch_bl(base + g_anim17[i].fn.off + g_anim17[i].call, base + g_anim17[i].getter.off)) g_a17 = &g_anim17[i];
        }
}
static void anim17_patch(uintptr_t base, size_t size)
{
    if (!g_a17) return;
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *page = unityprefetch_near(base, base + size, (size_t)pg);
    if (!page) return;
    size_t len = (size_t)(ua_erw_anim17_end - ua_erw_anim17_start);
    memcpy(page, ua_erw_anim17_start, len);
    unityprefetch_tail((uint32_t *)(page + (ua_erw_anim17_tail - ua_erw_anim17_start)), base + g_a17->getter.off);
    mprotect(page, (size_t)pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)page, (char *)(page + len));
    uintptr_t at = base + g_a17->fn.off + g_a17->call, a = at & ~(uintptr_t)(pg - 1);
    uint32_t bl = unityprefetch_bl(at, (uint64_t)(uintptr_t)page);
    if (!bl || mprotect((void *)a, (size_t)pg, PROT_READ | PROT_WRITE) != 0) return;
    memcpy((void *)at, &bl, 4);
    mprotect((void *)a, (size_t)pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)(at + 4));
    fprintf(stderr, "[enginerw] UA_PREFETCH anim: %s's loop fetches ahead (libunity %s)\n", g_a17->fn.func, g_a17->fn.unity);
}

// --- UA_ALLOC_STATS: MemoryManager::Allocate(this, size, align, MemLabelId (x3; its identifier the low 16 bits),
// AllocateOptions, file, line), 6000.0.61f1 arm64-v8a
static const struct row g_alloc_row = { "6000.0.61f1 arm64-v8a", "MemoryManager::Allocate", 0x7697d8, 0x174, ISA_A64, 0x6d3803dbc3d22270ull };
static void *(*g_alloc_orig)(uintptr_t, size_t, size_t, uint64_t, uint32_t, const char *, int);
static double g_alloc_secs;
static uintptr_t g_alloc_base;
static size_t g_alloc_size;
enum { AS_SLOTS = 4096 };
struct as_ent { uint32_t key_label, key_caller; uint64_t n, bytes; };
static struct as_ent g_as[AS_SLOTS];
static pthread_mutex_t g_as_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_as_total, g_as_dropped;
static uintptr_t g_as_mm;                       // the MemoryManager the calls were made on


// --- UA_OBJ_HEAPS
//
// Read in 6000.0.61f1 arm64-v8a: MemoryManager (this) keeps its allocators in arrays at +0x38 (the allocator), +0xb8
// (its main-thread heap) and +0x138 (its other-thread heap), their number at +0 (32 bits), the label -> allocator table
// at +0x1620 (16 bytes a label), its VirtualAllocator at +0x2110. InitializeDefaultAllocators makes the CACHEOBJECTS heap
// (label 44's: Unity's objects) as: two DynamicHeapAllocator(blockSize = BootConfig parameter 0x19ba6d0 [0], threaded
// false / true, NULL, NULL, name, false) in 0x1e0 bytes each, one DualThreadAllocator<DynamicHeapAllocator>(name,
// bucket allocator, main, thread, VirtualAllocator) in 0x140 bytes, appended to the three arrays. Ours makes four more
// the same way right after InitializeDefaultAllocators returns (the bucket allocator and VirtualAllocator read from the
// CACHEOBJECTS heap's +0x100 and +0x130). MemoryManager::Allocate with label 44 from inside a routed
// ProduceHelper<T>::Produce (its return address) asks our heap (its vtable's Allocate, +0x10, with Unity's size and
// alignment rounding) instead of the label's; a failure goes on to Unity's own path. Freeing needs nothing: Unity finds
// the allocator by the pointer (TryDeallocateWithLabel: the label's allocator refuses a pointer it does not hold, then
// GetAllocatorContainingPtr, which asks each registered allocator whether it holds it).
static const struct row g_oh_rows[] = {
    { "6000.0.61f1 arm64-v8a", "MemoryManager::InitializeDefaultAllocators", 0x76ac08, 0x980, ISA_A64, 0xaace5228c16eb8bdull },
    { "6000.0.61f1 arm64-v8a", "DynamicHeapAllocator::DynamicHeapAllocator", 0x767c24, 0xd4, ISA_A64, 0x959865ea3d2fdeb2ull },
    { "6000.0.61f1 arm64-v8a", "DualThreadAllocator<DynamicHeapAllocator>::DualThreadAllocator", 0x7663d4, 0xb8, ISA_A64, 0x30788e6dabad3982ull },
    { "6000.0.61f1 arm64-v8a", "BootConfig::ParameterData<unsigned long>::operator[]", 0x76d900, 0x58, ISA_A64, 0x8cc9966f5c7630a6ull },
    { "6000.0.61f1 arm64-v8a", "MemoryManager::GetAllocatorContainingPtr", 0x76bf78, 0x148, ISA_A64, 0xf674c650e90eb48aull },
    { "6000.0.61f1 arm64-v8a", "MemoryManager::TryDeallocateWithLabel", 0x76c0c0, 0x154, ISA_A64, 0x355a40c7aa91b100ull },
};
// the routed ProduceHelper<T, false>::Produce functions: offset, size, heap
static const struct { uint32_t off, size; int heap; const char *type; } g_oh_produce[] = {
    { 0x9d3168, 0x50, 0, "MonoBehaviour" },
    { 0x77c230, 0x50, 1, "GameObject" }, { 0x920c4c, 0x84, 1, "Transform" },
    { 0x1001a34, 0x15c, 2, "SpriteRenderer" }, { 0xffaed0, 0xcc, 2, "MeshRenderer" }, { 0x101e348, 0x64, 2, "MeshFilter" },
    { 0x11d7360, 0x50, 2, "ParticleSystemRenderer" },
    { 0xb7fb00, 0x50, 3, "Animator" }, { 0x11f3128, 0x50, 3, "ParticleSystem" }, { 0xc24c84, 0x50, 3, "AudioSource" },
    { 0xd8a210, 0x5c, 3, "CircleCollider2D" }, { 0xd91d14, 0x7c, 3, "PolygonCollider2D" }, { 0xd99ab8, 0x6c, 3, "BoxCollider2D" },
    { 0xd9e7b4, 0x70, 3, "EdgeCollider2D" }, { 0xd9fe7c, 0x90, 3, "Rigidbody2D" },
};
enum { OH_HEAPS = 4, OH_LABEL = 44 };
static int g_oh_mode;                           // 1 on, 3 stats
static uintptr_t g_oh_heap[OH_HEAPS];
static uint64_t g_oh_n[OH_HEAPS], g_oh_bytes[OH_HEAPS], g_oh_fail;
static void (*g_oh_init_orig)(uintptr_t);
static const char *const g_oh_names[OH_HEAPS][3] = {
    { "UA_OBJ_BEHAVIOURS", "UA_OBJ_BEHAVIOURS_MAIN", "UA_OBJ_BEHAVIOURS_THREAD" },
    { "UA_OBJ_GAMEOBJECTS", "UA_OBJ_GAMEOBJECTS_MAIN", "UA_OBJ_GAMEOBJECTS_THREAD" },
    { "UA_OBJ_RENDERERS", "UA_OBJ_RENDERERS_MAIN", "UA_OBJ_RENDERERS_THREAD" },
    { "UA_OBJ_COMPONENTS", "UA_OBJ_COMPONENTS_MAIN", "UA_OBJ_COMPONENTS_THREAD" },
};

static void oh_init_hook(uintptr_t mm)
{
    g_oh_init_orig(mm);
    uintptr_t base = g_alloc_base;
    uintptr_t cache = (uintptr_t)*(uint64_t *)(mm + 0x1620 + 16 * OH_LABEL);       // label 44's allocator
    int32_t n = *(int32_t *)mm;
    if (!cache || n < 1 || n + OH_HEAPS > 16) { fprintf(stderr, "[enginerw] UA_OBJ_HEAPS: %d allocators, label 44's %p: off\n", n, (void *)cache); g_oh_mode = 0; return; }
    uintptr_t bucket = (uintptr_t)*(uint64_t *)(cache + 0x100), valloc = (uintptr_t)*(uint64_t *)(cache + 0x130);
    if (valloc != mm + 0x2110) { fprintf(stderr, "[enginerw] UA_OBJ_HEAPS: the object heap is not laid out as read: off\n"); g_oh_mode = 0; return; }
    uint64_t (*param)(uintptr_t, int) = (uint64_t (*)(uintptr_t, int))(base + 0x76d900);
    void (*heap_ctor)(uintptr_t, uint32_t, int, uintptr_t, uintptr_t, const char *, int) =
        (void (*)(uintptr_t, uint32_t, int, uintptr_t, uintptr_t, const char *, int))(base + 0x767c24);
    void (*dual_ctor)(uintptr_t, const char *, uintptr_t, uintptr_t, uintptr_t, uintptr_t) =
        (void (*)(uintptr_t, const char *, uintptr_t, uintptr_t, uintptr_t, uintptr_t))(base + 0x7663d4);
    for (int k = 0; k < OH_HEAPS; k++) {
        void *m1 = NULL, *m2 = NULL, *d = NULL;
        if (posix_memalign(&m1, 64, 0x1e0) || posix_memalign(&m2, 64, 0x1e0) || posix_memalign(&d, 64, 0x140)) { g_oh_mode = 0; return; }
        memset(m1, 0, 0x1e0); memset(m2, 0, 0x1e0); memset(d, 0, 0x140);
        heap_ctor((uintptr_t)m1, (uint32_t)param(base + 0x19ba6d0, 0), 0, 0, 0, g_oh_names[k][1], 0);
        heap_ctor((uintptr_t)m2, (uint32_t)param(base + 0x19ba6d0, 0), 1, 0, 0, g_oh_names[k][2], 0);
        dual_ctor((uintptr_t)d, g_oh_names[k][0], bucket, (uintptr_t)m1, (uintptr_t)m2, valloc);
        int32_t i = *(int32_t *)mm;
        *(uint64_t *)(mm + 0xb8 + 8 * (uint32_t)i) = (uint64_t)(uintptr_t)m1;
        *(uint64_t *)(mm + 0x138 + 8 * (uint32_t)i) = (uint64_t)(uintptr_t)m2;
        *(uint64_t *)(mm + 0x38 + 8 * (uint32_t)i) = (uint64_t)(uintptr_t)d;
        *(int32_t *)mm = i + 1;
        g_oh_heap[k] = (uintptr_t)d;
    }
    fprintf(stderr, "[enginerw] UA_OBJ_HEAPS: %d heaps made after Unity's %d allocators (block size %llu)\n", OH_HEAPS, n,
            (unsigned long long)param(base + 0x19ba6d0, 0));
}

// from alloc_hook: label 44 from a routed Produce -> our heap, else 0
static void *oh_alloc(uint32_t caller, uint32_t lab, size_t size, size_t align)
{
    if (lab != OH_LABEL || !g_oh_heap[0]) return NULL;
    int heap = -1;
    for (size_t i = 0; i < sizeof g_oh_produce / sizeof *g_oh_produce; i++)
        if (caller - g_oh_produce[i].off < g_oh_produce[i].size) { heap = g_oh_produce[i].heap; break; }
    if (heap < 0) return NULL;
    // Unity's rounding (MemoryManager::Allocate): size at least 1, alignment ((align - 1) | 15) + 1
    size_t sz = size > 1 ? size : 1, al = ((align - 1) | 15) + 1;
    uintptr_t a = g_oh_heap[heap];
    void *(*fn)(uintptr_t, size_t, int) = (void *(*)(uintptr_t, size_t, int))*(uint64_t *)(*(uint64_t *)a + 0x10);
    void *p = fn(a, sz, (int)al);
    if (g_oh_mode == 3) {
        if (p) { __atomic_add_fetch(&g_oh_n[heap], 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_oh_bytes[heap], sz, __ATOMIC_RELAXED); }
        else __atomic_add_fetch(&g_oh_fail, 1, __ATOMIC_RELAXED);
    }
    return p;
}
static void oh_report(void)
{
    if (g_oh_mode != 3) return;
    fprintf(stderr, "[enginerw] UA_OBJ_HEAPS stats: behaviours %llu (%llu B), GameObjects+Transforms %llu (%llu B), renderers %llu (%llu B), "
            "components %llu (%llu B), refused %llu\n",
            (unsigned long long)g_oh_n[0], (unsigned long long)g_oh_bytes[0], (unsigned long long)g_oh_n[1], (unsigned long long)g_oh_bytes[1],
            (unsigned long long)g_oh_n[2], (unsigned long long)g_oh_bytes[2], (unsigned long long)g_oh_n[3], (unsigned long long)g_oh_bytes[3],
            (unsigned long long)g_oh_fail);
}
static void *alloc_hook(uintptr_t mm, size_t size, size_t align, uint64_t label, uint32_t opts, const char *file, int line)
{
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    uint32_t caller = ra >= g_alloc_base && ra < g_alloc_base + g_alloc_size ? (uint32_t)(ra - g_alloc_base) : 0;
    uint32_t lab = (uint32_t)(label & 0xffff);
    if (g_oh_mode) { void *p = oh_alloc(caller, lab, size, align); if (p) return p; }
    if (g_alloc_secs <= 0) return g_alloc_orig(mm, size, align, label, opts, file, line);
    uint32_t h = (lab * 0x9e3779b1u ^ caller * 0x85ebca6bu) & (AS_SLOTS - 1);
    pthread_mutex_lock(&g_as_lock);
    g_as_mm = mm;
    g_as_total++;
    for (int k = 0; k < 64; k++, h = (h + 1) & (AS_SLOTS - 1)) {
        struct as_ent *e = &g_as[h];
        if (!e->n) { e->key_label = lab; e->key_caller = caller; }
        if (e->key_label == lab && e->key_caller == caller) { e->n++; e->bytes += size; goto done; }
    }
    g_as_dropped++;
done:
    pthread_mutex_unlock(&g_as_lock);
    return g_alloc_orig(mm, size, align, label, opts, file, line);
}

static int as_cmp(const void *a, const void *b)
{
    const struct as_ent *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}
static void alloc_report(void)
{
    static struct as_ent copy[AS_SLOTS];
    pthread_mutex_lock(&g_as_lock);
    memcpy(copy, g_as, sizeof copy);
    uint64_t tot = g_as_total, dr = g_as_dropped;
    pthread_mutex_unlock(&g_as_lock);
    qsort(copy, AS_SLOTS, sizeof *copy, as_cmp);
    fprintf(stderr, "[enginerw] UA_ALLOC_STATS: %llu allocations so far (%llu not in the table); label caller count bytes:\n",
            (unsigned long long)tot, (unsigned long long)dr);
    for (int i = 0; i < 60 && copy[i].n; i++)
    {
        // and the allocator Unity gives the label (GetAllocator(MemLabelId) at +0x76be1c): its vtable's first two
        // slots as offsets in libunity, to be named with the symbols
        uintptr_t (*get)(uintptr_t, uint64_t) = (uintptr_t (*)(uintptr_t, uint64_t))(g_alloc_base + 0x76be1c);
        uintptr_t al = g_as_mm ? get(g_as_mm, copy[i].key_label) : 0;
        uint64_t s0 = 0, s1 = 0;
        if (al) { uintptr_t vt = (uintptr_t)*(uint64_t *)al; s0 = *(uint64_t *)vt - g_alloc_base; s1 = *(uint64_t *)(vt + 16) - g_alloc_base; }
        fprintf(stderr, "[enginerw] alloc %u +0x%x %llu %llu allocator %p vt +0x%llx +0x%llx\n", copy[i].key_label, copy[i].key_caller,
                (unsigned long long)copy[i].n, (unsigned long long)copy[i].bytes, (void *)al, (unsigned long long)s0,
                (unsigned long long)s1);
    }
}
static void alloc_match(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_ALLOC_STATS"), *o = getenv("UA_OBJ_HEAPS");
    if (v && *v && strcmp(v, "0")) g_alloc_secs = atof(v) > 0 ? atof(v) : 10;
    if (o && *o && strcmp(o, "0")) g_oh_mode = !strcmp(o, "stats") ? 3 : 1;
    if (g_alloc_secs <= 0 && !g_oh_mode) return;
    int ok = unityhooks_matches(&g_alloc_row, base, size);
    for (size_t i = 0; g_oh_mode && i < sizeof g_oh_rows / sizeof *g_oh_rows; i++) ok = ok && unityhooks_matches(&g_oh_rows[i], base, size);
    if (!ok) { g_alloc_secs = 0; g_oh_mode = 0; fprintf(stderr, "[enginerw] UA_ALLOC_STATS / UA_OBJ_HEAPS: not the code read; off\n"); return; }
    g_alloc_base = base; g_alloc_size = size;
}
static void alloc_patch(uintptr_t base)
{
    if (g_oh_mode && unityhooks_hook(base + g_oh_rows[0].off, ISA_A64, (void *)oh_init_hook, (void **)&g_oh_init_orig) != 0) {
        fprintf(stderr, "[enginerw] UA_OBJ_HEAPS: hook failed\n"); g_oh_mode = 0;
    }
    if (g_alloc_secs <= 0 && !g_oh_mode) return;
    if (unityhooks_hook(base + g_alloc_row.off, ISA_A64, (void *)alloc_hook, (void **)&g_alloc_orig) != 0) {
        fprintf(stderr, "[enginerw] UA_ALLOC_STATS: hook failed\n"); g_alloc_secs = 0;
    }
}

void enginerw_on_load_match(uintptr_t base, size_t size)
{
    anchor_match(base, size);
    cull_match(base, size);
    dpw_match(base, size);
    anim17_match(base, size);
    alloc_match(base, size);
    const char *v = getenv("UA_ENGINE_COUNT");
    if (!v || !*v) return;
    g_count_secs = atof(v);
    if (g_count_secs <= 0) g_count_secs = 5;
    const char *names = strchr(v, ':');
    g_base = base;
    int n = 0;
    for (size_t i = 0; i < NCOUNT; i++) {
        if (names) {
            int want = 0;
            char buf[256];
            snprintf(buf, sizeof buf, "%s", names + 1);
            for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) if (strstr(g_count[i].func, t)) want = 1;
            if (!want) continue;
        }
        g_matched[i] = unityhooks_matches(&g_count[i], base, size);
        n += g_matched[i];
    }
    fprintf(stderr, "[enginerw] UA_ENGINE_COUNT: %d functions of this libunity.so match their rows\n", n);
}

void enginerw_on_load_patch(uintptr_t base, size_t size)
{
    (void)size;
    anchor_patch(base);
    cull_patch(base);
    dpw_patch(base, size);
    anim17_patch(base, size);
    alloc_patch(base);
    for (size_t i = 0; i < NCOUNT; i++) {
        if (!g_matched[i]) continue;
        uint32_t *s = count_stub(&g_calls[i]);
        void *orig = NULL;
        if (!s || unityhooks_hook(base + g_count[i].off, ISA_A64, s, &orig) != 0) {
            fprintf(stderr, "[enginerw] %s: hook failed\n", g_count[i].func);
            g_matched[i] = 0;
            continue;
        }
        uint64_t o = (uint64_t)(uintptr_t)orig;
        memcpy(&s[10], &o, 8);              // before the function's first call: hooks go in before libunity runs
        __builtin___clear_cache((char *)s, (char *)(s + 12));
    }
}

void enginerw_frame(void)
{
    static unsigned long frames;
    static struct timespec t0;
    if (g_count_secs <= 0 && !g_anchor_mode && !g_cull_mode && !g_dpw_mode && g_alloc_secs <= 0 && !g_oh_mode) return;
    double secs = g_count_secs > 0 ? g_count_secs : 5;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!frames++) { t0 = t; return; }
    double dt = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) * 1e-9;
    static unsigned long f0 = 1;
    if (dt < secs) return;
    unsigned long nf = frames - f0;
    anchor_report(nf);
    cull_report();
    dpw_report(nf);
    if (g_alloc_secs > 0) alloc_report();
    oh_report();
    if (g_count_secs <= 0) { t0 = t; f0 = frames; return; }
    fprintf(stderr, "[enginerw] calls a frame over %lu frames (%.1f fps):", nf, (double)nf / dt);
    for (size_t i = 0; i < NCOUNT; i++) {
        if (!g_matched[i]) continue;
        uint64_t c = g_calls[i];
        fprintf(stderr, " %s=%.1f", g_count[i].func, (double)(c - g_last[i]) / (double)nf);
        g_last[i] = c;
    }
    fprintf(stderr, "\n");
    t0 = t; f0 = frames;
}
