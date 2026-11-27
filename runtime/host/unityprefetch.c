// unityprefetch.c: software prefetch (PRFM) ahead of the pointer walks of libunity's hot loops, on the 64-bit
// player (UA_PREFETCH; on by default where a player's rows match: 6000.0.61f1 arm64-v8a). docs/arm64-routes.md, route 3.
//
//   unset                  beh, sprite, cull, play
//   UA_PREFETCH=1          every site below
//   UA_PREFETCH=0          none
//   UA_PREFETCH=beh,sprite the named sites only (beh: the behaviour update loops; sprite: the sprite render
//                          nodes' preparation; cull: the camera culling walk;
//                          play: the playable graphs' traversal; ps: the particle systems' transform update)
//
// Why. On the Cortex-A53 / A35 (in order) a load that misses waits for memory at its first use: about 220
// cycles from DRAM on the H700 handhelds. The 64-bit player's structures are larger than the 32-bit one's,
// its working set no longer fits the shared 1 MB L2, and Hollow Knight's UnityMain takes 20 % more L2 refills
// a frame than on the 32-bit player (arm64-routes.md, hwcount). The hardware prefetcher follows strides, not
// pointers: the loops that walk lists of objects (each script's MonoBehaviour, each sprite's renderer) wait
// once or several times for every element. PRFM asks for a line ahead of its use and changes nothing else
// (architecturally a hint: no register, no flag, no memory, never a fault).
//
// How. The loop is not rewritten. One of its calls - the one made once for each element - is pointed at a
// stub here instead of its callee (the `bl`'s one word; the stubs sit in a page within branch range of
// libunity): the stub reads where the loop is (its registers at that call, as the loop's code left them),
// fetches what the next elements will read, and jumps to the callee with every argument register, every
// callee-saved register and the stack as they were. Its scratch registers are x16/x17 (the AAPCS64 lets any
// call clobber them: linker veneers do) and x9 - x12 (caller-saved temporaries: the loop holds nothing in
// them across the call, and each callee's code, which the rows tie to the bytes read, writes them before it
// reads them); the flags likewise. The order of Unity's own loads and stores, every value it computes and
// every call it makes are the same: only when memory arrives moves.
//
// What the stubs load to find the next elements' addresses is memory Unity's own loop reads (the list nodes
// still to run, the index list and its entries up to the loop's end), and fields inside objects of the known
// type at offsets Unity reads (a MonoBehaviour only once its vtable is MonoBehaviour's); everything else is
// PRFM. Each load is one that a PRFM a round earlier asked for, so the stub itself rarely waits.
//
// Sites (6000.0.61f1 arm64-v8a, the Mono route's player):
//   beh     BaseBehaviourManager::CommonUpdate<BehaviourManager / LateBehaviourManager /
//           FixedBehaviourManager>, at its call of SafeIterator::Next (x0: the iterator; its root's next is the
//           node Next returns, the end is the root itself; a node {prev, next, behaviour}). Ahead of the
//           behaviour Next returns (n1) and the three after it: n1's managed object (through its GC handle's
//           slot) and the line of its cached native pointer; n2's handle slot, GameObject (IsActive's cached
//           byte), script cache and its shared_ptr's control block (the weak count's atomic, for writing);
//           n3's MonoBehaviour lines CallUpdateMethod reads (vtable, handle and GameObject, script cache,
//           method table, started flag); n4's node.
//   sprite  PrepareSpriteRenderNodes<false / true>, at its call of BaseRenderer::FlattenBasicData (x19: the
//           thread context, [x19+0x10] the element being prepared, w24 the end; x25 the index list, x27 the
//           24-byte entries whose first word is the renderer + 0x28; x3 the RenderNode being written, 0x1a0
//           bytes). Ahead: the next element's renderer (the 0x1c0 bytes the loop and FlattenBasicData read) and
//           the next RenderNode (for writing); the element after it's entry.
//   cull    ProcessCameraIndexListIsNodeVisibleStep1, at its call of IsNodeVisibleFast (x23: the SceneNode being
//           tested, 24 bytes each, walked in order): the nodes 8 and 10 ahead (PRFM only).
//   play    PreOrderPlayableTraverser::Traverse, at its loop's call of RecursiveVisit for each input of a playable
//           (x22: the playable's graph node, its inputs at +0x20 (16 bytes each, the Playable first) and their count
//           at +0x30; x23 / x24: the input's offset and number): the next input's Playable, four lines.
//   anim    Animator::BuildJobs (6000.0.61f1), at its loop's call of the PlayableOutput's target getter (x21: the output's
//           slot in the outputs' array, x26 that vector {array, .., count at +0x10}): two ahead the output's lines, one
//           ahead its Animator's lines that the loop and Animator::Prepare read and its source playable's handle (opt-in).
//   ps      ParticleSystem::UpdateManualTransformData, at its loop's first call of GameObject::QueryComponentByType (the
//           Transform's lookup; x24 the page of the global list of particle systems, +0x730: {array, .., count at
//           +0x10}; x25 the system's number): three ahead the ParticleSystem, two ahead its GameObject and the objects
//           the loop reads from it (+0x30, +0x38, +0x40), one ahead the GameObject's component array.
//
// Found as unityhooks.c finds its hooks: pmu-dev unityhooks rows (offset, size, FNV-1a of the bytes) for each
// function whose call is changed or whose reads the offsets come from; any other build matches no row and is
// left alone. A function whose call site is changed differs from the file, so UA_RELAYOUT (relayout.c, run
// after the hooks) leaves it where it is, and the copies that call it reach it there.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "unityprefetch.h"
#include "unityhooks.h"

enum { STUB_BEH, STUB_SPRITE, STUB_CULL, STUB_PLAY, STUB_PS, STUB_ANIM, STUB_N };
static const char *const g_stub_name[STUB_N] = { "beh", "sprite", "cull", "play", "ps", "anim" };

#if defined(__aarch64__)
// The stubs: position-independent code copied into a page near libunity. Each ends in four words the installer
// fills with the jump to the callee (`b`, or `ldr x16, #8; br x16; <address>`), then its literals.
__asm__(
    "    .text\n"
    "    .p2align 3\n"
    "    .globl ua_pf_beh_start, ua_pf_beh_tail, ua_pf_beh_vt, ua_pf_beh_end\n"
    "ua_pf_beh_start:\n"
    "    ldr  x17, ua_pf_beh_vt\n"          // MonoBehaviour's vtable as its objects point to it
    "    ldr  x16, [x0, #8]\n"              // n1: the node Next returns
    "    cmp  x16, x0\n"
    "    b.eq ua_pf_beh_tail\n"
    "    ldr  x9, [x16, #0x10]\n"           // its MonoBehaviour (Unity reads it right after Next)
    "    ldr  x10, [x9]\n"
    "    cmp  x10, x17\n"
    "    b.ne 1f\n"
    "    ldr  x10, [x9, #0x18]\n"           // its GC handle: 0, tagged (weak: resolved by a call), or a slot
    "    cbz  x10, 1f\n"
    "    tbnz w10, #0, 1f\n"
    "    ldr  x10, [x10]\n"                 // the slot (fetched a round ago): the managed object
    "    prfm pldl1keep, [x10]\n"
    "    prfm pldl1keep, [x10, #0x10]\n"    // its cached native pointer (GetCachedPtrFromScriptingWrapper)
    "1:  ldr  x16, [x16, #8]\n"             // n2
    "    cmp  x16, x0\n"
    "    b.eq ua_pf_beh_tail\n"
    "    ldr  x9, [x16, #0x10]\n"
    "    ldr  x10, [x9]\n"
    "    cmp  x10, x17\n"
    "    b.ne 2f\n"
    "    ldp  x10, x11, [x9, #0x18]\n"      // handle, GameObject
    "    prfm pldl1keep, [x10]\n"
    "    prfm pldl1keep, [x11, #0x40]\n"    // IsActive's cached byte (+0x47)
    "    ldp  x10, x11, [x9, #0xe0]\n"      // the script cache's shared_ptr: object, control block
    "    prfm pldl1keep, [x10, #0x38]\n"    // IsStateMachineBehaviour's byte
    "    prfm pstl1keep, [x11, #8]\n"       // weak_count::lock's atomic
    "2:  ldr  x16, [x16, #8]\n"             // n3
    "    cmp  x16, x0\n"
    "    b.eq ua_pf_beh_tail\n"
    "    ldr  x9, [x16, #0x10]\n"
    "    prfm pldl1keep, [x9]\n"
    "    prfm pldl1keep, [x9, #0x20]\n"
    "    prfm pldl1keep, [x9, #0xe0]\n"
    "    prfm pldl1keep, [x9, #0xf8]\n"
    "    prfm pldl1keep, [x9, #0x120]\n"
    "    ldr  x16, [x16, #8]\n"             // n4: its node
    "    cmp  x16, x0\n"
    "    b.eq ua_pf_beh_tail\n"
    "    prfm pldl1keep, [x16]\n"
    "    prfm pldl1keep, [x16, #0x10]\n"
    "ua_pf_beh_tail:\n"
    "    .word 0, 0, 0, 0\n"
    "    .p2align 3\n"
    "ua_pf_beh_vt:\n"
    "    .quad 0\n"
    "ua_pf_beh_end:\n"

    "    .p2align 3\n"
    "    .globl ua_pf_sprite_start, ua_pf_sprite_tail, ua_pf_sprite_end\n"
    "ua_pf_sprite_start:\n"
    "    ldr  w16, [x19, #0x10]\n"          // the element being prepared
    "    mov  w11, #24\n"
    "    add  w9, w16, #1\n"
    "    cmp  w9, w24\n"
    "    b.hs ua_pf_sprite_tail\n"
    "    prfm pstl1keep, [x3, #0x1a0]\n"    // the next RenderNode
    "    prfm pstl1keep, [x3, #0x1e0]\n"
    "    prfm pstl1keep, [x3, #0x220]\n"
    "    prfm pstl1keep, [x3, #0x260]\n"
    "    prfm pstl1keep, [x3, #0x2a0]\n"
    "    prfm pstl1keep, [x3, #0x2e0]\n"
    "    prfm pstl1keep, [x3, #0x338]\n"
    "    ldrsw x10, [x25, w9, sxtw #2]\n"   // the next element's index
    "    smaddl x10, w10, w11, x27\n"       // its entry (fetched a round ago)
    "    ldr  x12, [x10]\n"
    "    cbz  x12, 1f\n"
    "    sub  x12, x12, #0x28\n"            // its renderer
    "    prfm pldl1keep, [x12]\n"
    "    prfm pldl1keep, [x12, #0x40]\n"
    "    prfm pldl1keep, [x12, #0x80]\n"
    "    prfm pldl1keep, [x12, #0xc0]\n"
    "    prfm pldl1keep, [x12, #0x100]\n"
    "    prfm pldl1keep, [x12, #0x140]\n"
    "    prfm pldl1keep, [x12, #0x180]\n"
    "    prfm pldl1keep, [x12, #0x1b8]\n"
    "1:  add  w9, w16, #2\n"
    "    cmp  w9, w24\n"
    "    b.hs ua_pf_sprite_tail\n"
    "    ldrsw x10, [x25, w9, sxtw #2]\n"
    "    smaddl x10, w10, w11, x27\n"
    "    prfm pldl1keep, [x10]\n"
    "    prfm pldl1keep, [x10, #0x10]\n"
    "ua_pf_sprite_tail:\n"
    "    .word 0, 0, 0, 0\n"
    "ua_pf_sprite_end:\n"

    "    .p2align 3\n"
    "    .globl ua_pf_cull_start, ua_pf_cull_tail, ua_pf_cull_end\n"
    "ua_pf_cull_start:\n"
    "    prfm pldl1keep, [x23, #0xc0]\n"   // the 24-byte SceneNodes 8 and 10 ahead (past the end: a hint)
    "    prfm pldl1keep, [x23, #0xf0]\n"
    "ua_pf_cull_tail:\n"
    "    .word 0, 0, 0, 0\n"
    "ua_pf_cull_end:\n"

    "    .p2align 3\n"
    "    .globl ua_pf_play_start, ua_pf_play_tail, ua_pf_play_end\n"
    "ua_pf_play_start:\n"
    "    ldr  x10, [x22, #0x30]\n"         // the playable's input count
    "    add  x11, x24, #1\n"              // the input after the one being visited
    "    cmp  x11, x10\n"
    "    b.hs ua_pf_play_tail\n"
    "    ldr  x9, [x22, #0x20]\n"          // its inputs, 16 bytes each: {Playable *, weight, ...}
    "    add  x9, x9, x23\n"
    "    ldr  x12, [x9, #0x10]\n"
    "    prfm pldl1keep, [x12]\n"          // the next input's Playable (0 for an empty input: a hint)
    "    prfm pldl1keep, [x12, #0x40]\n"
    "    prfm pldl1keep, [x12, #0x80]\n"
    "    prfm pldl1keep, [x12, #0xc0]\n"
    "ua_pf_play_tail:\n"
    "    .word 0, 0, 0, 0\n"
    "ua_pf_play_end:\n"

    "    .p2align 3\n"
    "    .globl ua_pf_ps_start, ua_pf_ps_tail, ua_pf_ps_end\n"
    "ua_pf_ps_start:\n"
    "    ldr  x9, [x24, #0x730]\n"         // the list of particle systems: {array, ..., count at +0x10}
    "    ldr  x10, [x9, #0x10]\n"
    "    ldr  x9, [x9]\n"
    "    add  x11, x25, #3\n"              // three ahead: the ParticleSystem
    "    cmp  x11, x10\n"
    "    b.hs 1f\n"
    "    ldr  x12, [x9, x11, lsl #3]\n"
    "    prfm pldl1keep, [x12, #0x20]\n"
    "1:  add  x11, x25, #2\n"              // two ahead: its GameObject and the objects the loop reads
    "    cmp  x11, x10\n"
    "    b.hs 2f\n"
    "    ldr  x12, [x9, x11, lsl #3]\n"
    "    ldr  x16, [x12, #0x20]\n"
    "    prfm pldl1keep, [x16, #0x20]\n"   // its component array and count
    "    ldr  x16, [x12, #0x30]\n"
    "    prfm pldl1keep, [x16, #0x48]\n"
    "    ldr  x16, [x12, #0x38]\n"
    "    prfm pldl1keep, [x16, #0x20]\n"
    "    prfm pldl1keep, [x16, #0x44]\n"
    "    prfm pldl1keep, [x16, #0x80]\n"
    "    prfm pldl1keep, [x16, #0x168]\n"
    "    ldr  x16, [x12, #0x40]\n"
    "    prfm pldl1keep, [x16, #0x218]\n"
    "2:  add  x11, x25, #1\n"              // the next one: its GameObject's components
    "    cmp  x11, x10\n"
    "    b.hs ua_pf_ps_tail\n"
    "    ldr  x12, [x9, x11, lsl #3]\n"
    "    ldr  x16, [x12, #0x20]\n"
    "    ldr  x16, [x16, #0x20]\n"
    "    prfm pldl1keep, [x16]\n"
    "    prfm pldl1keep, [x16, #0x40]\n"
    "ua_pf_ps_tail:\n"
    "    .word 0, 0, 0, 0\n"
    "ua_pf_ps_end:\n"

    "    .p2align 3\n"
    "    .globl ua_pf_anim_start, ua_pf_anim_tail, ua_pf_anim_end\n"
    "ua_pf_anim_start:\n"
    "    ldr  x9, [x26]\n"                 // the outputs: {array, .., count at +0x10}; x21 the one being built
    "    ldr  x10, [x26, #0x10]\n"
    "    add  x10, x9, x10, lsl #3\n"
    "    add  x11, x21, #16\n"             // two ahead: the PlayableOutput's lines
    "    cmp  x11, x10\n"
    "    b.hs 1f\n"
    "    ldr  x12, [x11]\n"
    "    prfm pldl1keep, [x12]\n"
    "    prfm pldl1keep, [x12, #0x40]\n"
    "    prfm pldl1keep, [x12, #0xc0]\n"
    "1:  add  x11, x21, #8\n"              // the next one: its Animator (+0xc0) and source playable's handle (+0x30)
    "    cmp  x11, x10\n"
    "    b.hs ua_pf_anim_tail\n"
    "    ldr  x12, [x11]\n"
    "    ldr  x16, [x12, #0xc0]\n"
    "    cbz  x16, 2f\n"
    "    prfm pldl1keep, [x16]\n"          // vtable (the loop's +0xd0 call), Prepare's and the loop's fields
    "    prfm pldl1keep, [x16, #0x178]\n"
    "    prfm pldl1keep, [x16, #0x188]\n"
    "    prfm pldl1keep, [x16, #0x1c0]\n"
    "    prfm pldl1keep, [x16, #0x478]\n"
    "    prfm pldl1keep, [x16, #0x5f8]\n"
    "2:  ldr  x17, [x12, #0x30]\n"
    "    cbz  x17, ua_pf_anim_tail\n"
    "    prfm pldl1keep, [x17, #0x20]\n"
    "ua_pf_anim_tail:\n"
    "    .word 0, 0, 0, 0\n"
    "ua_pf_anim_end:\n"
);
extern const uint8_t ua_pf_beh_start[], ua_pf_beh_tail[], ua_pf_beh_vt[], ua_pf_beh_end[];
extern const uint8_t ua_pf_sprite_start[], ua_pf_sprite_tail[], ua_pf_sprite_end[];
extern const uint8_t ua_pf_cull_start[], ua_pf_cull_tail[], ua_pf_cull_end[];
extern const uint8_t ua_pf_play_start[], ua_pf_play_tail[], ua_pf_play_end[];
extern const uint8_t ua_pf_ps_start[], ua_pf_ps_tail[], ua_pf_ps_end[];
extern const uint8_t ua_pf_anim_start[], ua_pf_anim_tail[], ua_pf_anim_end[];

static const uint8_t *const g_stub_start[STUB_N] = { ua_pf_beh_start, ua_pf_sprite_start, ua_pf_cull_start, ua_pf_play_start, ua_pf_ps_start, ua_pf_anim_start };
static const uint8_t *const g_stub_tail[STUB_N] = { ua_pf_beh_tail, ua_pf_sprite_tail, ua_pf_cull_tail, ua_pf_play_tail, ua_pf_ps_tail, ua_pf_anim_tail };
static const uint8_t *const g_stub_end[STUB_N] = { ua_pf_beh_end, ua_pf_sprite_end, ua_pf_cull_end, ua_pf_play_end, ua_pf_ps_end, ua_pf_anim_end };
#endif

// a call site: the function (its row), the `bl` in it, the function it calls (its row), the stub
struct pf_site {
    struct uh_row fn;
    uint32_t call;
    struct uh_row callee;
    int stub;
};
struct pf_player {
    const char *unity;
    struct pf_site sites[10];
    struct uh_row checks[6];        // the code the offsets were read from (checked only)
    uint32_t mb_vtable;             // MonoBehaviour's vtable as objects point to it (its symbol + 0x10)
    uint32_t mb_vt_update;          // its Update slot (CommonUpdate<BehaviourManager>'s call) ...
    uint32_t mb_update;             // ... holds MonoBehaviour::Update
};

#define NEXT61 { "6000.0.61f1 arm64-v8a", "SafeIterator<List<ListNode<MonoBehaviour> > >::Next", 0x79c5b0, 0x88, 'x', 0x4b56a8a1ecb3e812ull }
#define FLAT61 { "6000.0.61f1 arm64-v8a", "BaseRenderer::FlattenBasicData", 0xf547b4, 0xec, 'x', 0x25ea40c962bae0a6ull }
// 6000.0.61f1 arm64-v8a (read 2026-11-20): CommonUpdate's loop calls Next at +0x44 with x0 = sp + 8 and reads
// nothing but x0 and its own stack after it (x8 written first); Next writes x8 - x11 before it reads them.
// CallUpdateMethod (through Update, vtable +0xe0) reads the MonoBehaviour's +0x18 (the handle; bit 0: weak),
// +0x20 (GameObject), +0x125 (started), +0xf8 (method table); IsStateMachineBehaviour its +0xe0/+0xe8 (the script
// cache's shared_ptr: GetScriptCache, then lock() on the control block's +8, the cache's +0x38); IsActive the
// GameObject's +0x47; IsInstanceValid the managed object's +0x10 (GetCachedPtrFromScriptingWrapper).
// PrepareSpriteRenderNodes calls FlattenBasicData with x19 the context, w24 its end, x25 / x27 its index list and
// entries (the loop's own registers, kept across the loop's calls), x3 = the RenderNode; FlattenBasicData writes
// x8 and x9 before it reads them; after it the loop writes x8 first. ProcessCameraIndexListIsNodeVisibleStep1 walks
// the nodes with x23 (+24 a step) and calls IsNodeVisibleFast (the cull stub uses no register). Traverse's input loop
// keeps the graph node in x22, the input's offset and number in x23 / x24, reloads x9 after the call; RecursiveVisit
// writes x8 - x12 before it reads them (its float arguments s0 - s3 / d3 are not touched). UpdateManualTransformData keeps
// the list's page in x24 and the number in x25 across its calls; QueryComponentByType writes x8 - x12 before it reads
// them, and the loop's next call (GetTransformAccess) reads none of them.
static const struct pf_player g_players[] = {
    {
        "6000.0.61f1 arm64-v8a",
        {
            { { "6000.0.61f1 arm64-v8a", "BaseBehaviourManager::CommonUpdate<BehaviourManager>", 0x79c474, 0xb4, 'x', 0xa193417a7199eaf0ull }, 0x79c4b8, NEXT61, 1 + STUB_BEH },
            { { "6000.0.61f1 arm64-v8a", "BaseBehaviourManager::CommonUpdate<LateBehaviourManager>", 0x79c6f4, 0xb4, 'x', 0x71d44f729658aceaull }, 0x79c738, NEXT61, 1 + STUB_BEH },
            { { "6000.0.61f1 arm64-v8a", "BaseBehaviourManager::CommonUpdate<FixedBehaviourManager>", 0x79c63c, 0xb4, 'x', 0x85f30964d0eac0d9ull }, 0x79c680, NEXT61, 1 + STUB_BEH },
            { { "6000.0.61f1 arm64-v8a", "PrepareSpriteRenderNodes<false>", 0x75401c, 0x1f4, 'x', 0xdd558a4a76b783a8ull }, 0x754108, FLAT61, 1 + STUB_SPRITE },
            { { "6000.0.61f1 arm64-v8a", "PrepareSpriteRenderNodes<true>", 0x754210, 0x200, 'x', 0xa58497bf39b395d2ull }, 0x7542fc, FLAT61, 1 + STUB_SPRITE },
            { { "6000.0.61f1 arm64-v8a", "ProcessCameraIndexListIsNodeVisibleStep1", 0x785d38, 0xa8, 'x', 0x045c3175edd29d36ull }, 0x785d98,
              { "6000.0.61f1 arm64-v8a", "IsNodeVisibleFast", 0x785de0, 0x6c, 'x', 0x959ef690b160b089ull }, 1 + STUB_CULL },
            { { "6000.0.61f1 arm64-v8a", "PreOrderPlayableTraverser::Traverse", 0x9b1be0, 0x1f0, 'x', 0x196dd5dbc9ecc92dull }, 0x9b1d98,
              { "6000.0.61f1 arm64-v8a", "PreOrderPlayableTraverser::RecursiveVisit", 0x9b2998, 0xbc, 'x', 0xf8b390afd5730f12ull }, 1 + STUB_PLAY },
            { { "6000.0.61f1 arm64-v8a", "ParticleSystem::UpdateManualTransformData", 0x11fa950, 0x1a4, 'x', 0x4dbcab5f515c8486ull }, 0x11fa9cc,
              { "6000.0.61f1 arm64-v8a", "GameObject::QueryComponentByType", 0x77b034, 0x44, 'x', 0x823fd1d5d6ddd8a7ull }, 1 + STUB_PS },
            { { "6000.0.61f1 arm64-v8a", "Animator::BuildJobs", 0xb82fb0, 0x3c8, 'x', 0x215a8a2dbed9cdfdull }, 0xb83050,
              { "6000.0.61f1 arm64-v8a", "PlayableOutput's target getter (ldr x0, [x0, #0xc0])", 0x7916e4, 0x8, 'x', 0x6b8be5fe6d2e9bfaull }, 1 + STUB_ANIM },
        },
        {
            { "6000.0.61f1 arm64-v8a", "MonoBehaviour::CallUpdateMethod", 0x9d3e78, 0x114, 'x', 0xd695de744b4c0dbdull },
            { "6000.0.61f1 arm64-v8a", "MonoBehaviour::Update", 0x9d3f8c, 0x8, 'x', 0x44ceb46c07cc2775ull },
            { "6000.0.61f1 arm64-v8a", "MonoBehaviour::IsStateMachineBehaviour", 0x9d3490, 0x94, 'x', 0x59a740fd0a8842d7ull },
            { "6000.0.61f1 arm64-v8a", "SerializableManagedRef::GetScriptCache", 0xa0b708, 0x24, 'x', 0x21a75ec90f549d6aull },
            { "6000.0.61f1 arm64-v8a", "GameObject::IsActive", 0x77aa58, 0xac, 'x', 0x60367e8d7a4fe47cull },
            { "6000.0.61f1 arm64-v8a", "Scripting::GetCachedPtrFromScriptingWrapper", 0x9c54b4, 0xc, 'x', 0x9d0e990812703736ull },
        },
        0x1946f30, 0xe0, 0x9d3f8c,
    },
};

// UA_PREFETCH: 1 (all) or a comma list of stub names
int unityprefetch_wanted(const char *v, const char *name)
{
    if (!v || !*v || !strcmp(v, "0")) return 0;
    if (!strcmp(v, "1")) return 1;
    size_t n = strlen(name);
    for (const char *p = v; *p; ) {
        const char *e = strchr(p, ',');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == n && !strncmp(p, name, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

// the `bl` at pc to target, or 0 when it does not reach
uint32_t unityprefetch_bl(uint64_t pc, uint64_t target)
{
    int64_t d = (int64_t)(target - pc);
    if (d & 3 || d < -(1ll << 27) || d >= (1ll << 27)) return 0;
    return 0x94000000u | ((uint32_t)(d >> 2) & 0x03ffffffu);
}

// the jump at `at` (four words) to target: `b` when it reaches, else `ldr x16, #8; br x16; <target>`
void unityprefetch_tail(uint32_t *at, uint64_t target)
{
    uint32_t b = unityprefetch_bl((uint64_t)(uintptr_t)at, target);
    if (b) {
        at[0] = b & ~0x80000000u;                       // bl -> b
        at[1] = at[2] = at[3] = 0xd503201fu;            // nop
    } else {
        at[0] = 0x58000050u; at[1] = 0xd61f0200u;       // ldr x16, #8; br x16
        memcpy(&at[2], &target, 8);
    }
}

// a page for the stubs within `bl` range (128 MB) of [lo, hi)
void *unityprefetch_near(uintptr_t lo, uintptr_t hi, size_t len)
{
    const uintptr_t reach = 120u << 20, step = 1u << 20;
    if (hi - lo >= reach) return NULL;
    uintptr_t from = hi > reach ? hi - reach : 0, to = lo + reach;
    for (uintptr_t want = (hi + step - 1) & ~(step - 1), k = 0; k < 2 * (reach / step); k++) {
        // above the library first, then below it
        uintptr_t a = k < reach / step ? want + k * step : (lo & ~(step - 1)) - (k - reach / step + 1) * step;
        if (a < from || a + len > to) continue;
        void *p = mmap((void *)a, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) continue;
        if ((uintptr_t)p >= from && (uintptr_t)p + len <= to) return p;
        munmap(p, len);
    }
    return NULL;
}

static int write_word(uintptr_t at, uint32_t w)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t a = at & ~(uintptr_t)(pg - 1);
    if (mprotect((void *)a, (size_t)pg, PROT_READ | PROT_WRITE) != 0) return -1;
    memcpy((void *)at, &w, 4);
    mprotect((void *)a, (size_t)pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)(at + 4));
    return 0;
}

void unityprefetch_on_load(uintptr_t base, size_t size)
{
    const char *v = getenv("UA_PREFETCH");
    int asked = v && *v;
    if (!asked) v = "beh,sprite,cull,play";     // the default (ps: measured apart); a player without rows: silently none
    if (!strcmp(v, "0")) return;
#if !defined(__aarch64__)
    (void)base; (void)size;
    if (asked) fprintf(stderr, "[unityprefetch] UA_PREFETCH: the 64-bit player's sites only; nothing on this build\n");
#else
    for (size_t k = 0; k < sizeof g_players / sizeof *g_players; k++) {
        const struct pf_player *p = &g_players[k];
        int any = 0, ok = 1;
        for (size_t i = 0; i < sizeof p->sites / sizeof *p->sites; i++)
            if (p->sites[i].stub && unityhooks_matches(&p->sites[i].fn, base, size)) any = 1;
        if (!any) continue;
        for (size_t i = 0; i < sizeof p->checks / sizeof *p->checks; i++)
            if (p->checks[i].func && !unityhooks_matches(&p->checks[i], base, size)) {
                fprintf(stderr, "[unityprefetch] %s differs from the code read; no prefetch\n", p->checks[i].func);
                ok = 0;
            }
        uint64_t slot;
        memcpy(&slot, (const void *)(base + p->mb_vtable + p->mb_vt_update), 8);
        if (slot != base + p->mb_update) {
            fprintf(stderr, "[unityprefetch] MonoBehaviour's vtable is not where it was read; no prefetch\n");
            ok = 0;
        }
        if (!ok) return;
        long pg = sysconf(_SC_PAGESIZE);
        uint8_t *page = unityprefetch_near(base, base + size, (size_t)pg);
        if (!page) { fprintf(stderr, "[unityprefetch] no page within branch range of libunity; no prefetch\n"); return; }
        // one copy of each stub a callee needs (stub, callee) -> its address
        uintptr_t made[8][3]; int nmade = 0; size_t used = 0;
        int done[STUB_N] = { 0 }, n = 0;
        for (size_t i = 0; i < sizeof p->sites / sizeof *p->sites; i++) {
            const struct pf_site *s = &p->sites[i];
            if (!s->stub) continue;
            int st = s->stub - 1;
            if (!unityprefetch_wanted(v, g_stub_name[st])) continue;
            if (!unityhooks_matches(&s->fn, base, size) || !unityhooks_matches(&s->callee, base, size)) {
                fprintf(stderr, "[unityprefetch] %s: not the code read; left alone\n", s->fn.func);
                continue;
            }
            uint32_t w;
            memcpy(&w, (const void *)(base + s->call), 4);
            if (w != unityprefetch_bl(base + s->call, base + s->callee.off)) {
                fprintf(stderr, "[unityprefetch] %s: no call of %s at +0x%x; left alone\n", s->fn.func, s->callee.func, s->call);
                continue;
            }
            uintptr_t stub = 0;
            for (int j = 0; j < nmade; j++)
                if (made[j][0] == (uintptr_t)st && made[j][1] == base + s->callee.off) stub = made[j][2];
            if (!stub) {
                size_t len = (size_t)(g_stub_end[st] - g_stub_start[st]);
                if (used + len > (size_t)pg || nmade == 8) continue;
                uint8_t *at = page + used;
                memcpy(at, g_stub_start[st], len);
                unityprefetch_tail((uint32_t *)(at + (g_stub_tail[st] - g_stub_start[st])), base + s->callee.off);
                if (st == STUB_BEH) {
                    uint64_t vt = base + p->mb_vtable;
                    memcpy(at + (ua_pf_beh_vt - ua_pf_beh_start), &vt, 8);
                }
                stub = (uintptr_t)at;
                made[nmade][0] = (uintptr_t)st; made[nmade][1] = base + s->callee.off; made[nmade][2] = stub; nmade++;
                used += (len + 15) & ~(size_t)15;
            }
            uint32_t to = unityprefetch_bl(base + s->call, stub);
            // the page is made executable before any call reaches it (below); nothing of libunity runs yet
            if (!to || write_word(base + s->call, to) != 0) {
                fprintf(stderr, "[unityprefetch] %s: the call could not be changed; left alone\n", s->fn.func);
                continue;
            }
            done[st]++; n++;
        }
        mprotect(page, (size_t)pg, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)page, (char *)(page + used));
        if (!n) { munmap(page, (size_t)pg); return; }
        fprintf(stderr, "[unityprefetch] libunity %s: %d call sites fetch ahead (beh %d, sprite %d, cull %d, play %d, ps %d, anim %d; UA_PREFETCH=%s)\n",
                p->unity, n, done[STUB_BEH], done[STUB_SPRITE], done[STUB_CULL], done[STUB_PLAY], done[STUB_PS], done[STUB_ANIM], v);
        return;
    }
    if (asked) fprintf(stderr, "[unityprefetch] this libunity.so is not one whose loops were read (pmu-dev unityhooks); no prefetch\n");
#endif
}
