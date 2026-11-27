//! `pmu-dev unityhooks <libunity.so> <libunity.sym.so> [--unity <version abi>]` (was
//! tools/re/unityhooks.py): the rows of runtime/host/unityhooks.c for one Unity player: each hooked
//! function's offset, size, instruction set and the FNV-1a 64 hash of its bytes, read from the
//! player's libunity.so with Unity's symbol file for it (`pmu player fetch <version> --symbols`:
//! Variations/<backend>/Release/Symbols/<abi>/libunity.sym.so).
//!
//! It checks what the runtime's hook relies on, and prints no row where a check fails:
//!   - the symbol file belongs to this library (the same GNU build id, when the library has one; else
//!     the function's bytes must at least be in the library's text);
//!   - the bytes the jump replaces (8 in ARM state, 16 in A64, 8 or 10 in Thumb: 10 at an entry that is
//!     not word-aligned) are instructions that can run anywhere else: in ARM state no pc-relative
//!     operand, no literal load, no branch but a b<cond>, which the runtime rewrites; in Thumb the same,
//!     whole instructions, and an IT block whole among them (`bx lr` and a `pop` of the pc may be moved:
//!     they return); in A64 no literal load, no indirect branch, no write of x16 or x17 (the
//!     trampoline's jumps use x16), and a direct branch only to outside those bytes (the runtime
//!     rewrites direct branches, adr and adrp for their new place);
//!   - nothing in the function branches into those bytes after the first instruction.
//! A 'replace' row (a function the runtime replaces whole: only its entry is overwritten, nothing of it
//! runs again) checks only the last point and that the function is longer than the jump.
//! The function's meaning (its arguments, the enum values the hook compares) is read by hand for each
//! version before a row is added; the hash then ties the row to exactly that code. No Unity code is
//! printed.
use std::collections::HashMap;

use crate::elf::{u16le, u32le, Elf};

/// (mangled-name prefix (a trailing '$': the whole name), short name, "" or "call" (a function the
/// runtime only calls: any instruction set, nothing moved; its hash ties the row to the code read) or
/// "replace")
pub const HOOKS: &[(&str, &str, &str)] = &[
    // (mangled-name prefix (a trailing '$': the whole name), short name[, 'call': a function the runtime only calls (any instruction set,
    // nothing moved), whose hash ties the row to the code read])
    ("_Z25AdjustThreadingModeForAPI", "AdjustThreadingModeForAPI", ""),
    // unitypool.c (UA_GLES_POOL): Unity's GLES buffer pool
    ("_ZN17BufferManagerGLES13AcquireBufferE", "BufferManagerGLES::AcquireBuffer", ""),
    ("_ZN17BufferManagerGLES13ReleaseBufferE", "BufferManagerGLES::ReleaseBuffer", ""),
    ("_ZN17BufferManagerGLES24UpdateLiveSetFromPendingE", "BufferManagerGLES::UpdateLiveSetFromPending", ""),
    ("_ZN17BufferManagerGLES16PruneFreeBuffersE", "BufferManagerGLES::PruneFreeBuffers", ""),
    ("_ZN17BufferManagerGLES5ClearE", "BufferManagerGLES::Clear", ""),
    ("_ZN17BufferManagerGLES13InvalidateAllE", "BufferManagerGLES::InvalidateAll", ""),
    ("_ZN17BufferManagerGLESD", "BufferManagerGLES::~BufferManagerGLES", ""),
    // unityjobs.c (UA_JOB_SLEEP): Unity 2017.x's wait for a job group, and what it calls
    ("_ZN8JobQueue15WaitForJobGroupE10JobGroupID", "JobQueue::WaitForJobGroup", ""),
    ("_ZN8JobQueue14SignalOnFinishE10JobGroupIDP9Semaphore", "JobQueue::SignalOnFinish", "call"),
    ("_ZN8JobQueue4ExecEP7JobInfoxi", "JobQueue::Exec", "call"),
    ("_ZN8JobQueue5StealEP8JobGroupP7JobInfoxib", "JobQueue::Steal", "call"),
    ("_ZN11AtomicStack3PopEv", "AtomicStack::Pop", "call"),
    ("_ZN10AtomicList4LoadERx", "AtomicList::Load", "call"),
    ("_ZN10AtomicList3TagEv", "AtomicList::Tag", "call"),
    ("_ZN17PlatformSemaphore6CreateEv", "PlatformSemaphore::Create", "call"),
    ("_ZN17PlatformSemaphore13WaitForSignalEv", "PlatformSemaphore::WaitForSignal", "call"),
    ("_ZL15SignalSemaphorePv", "SignalSemaphore", "call"),
    ("_Z15GetGraphicsCapsv", "GetGraphicsCaps", "call"),
    // unitymath.c (UA_INV_MEMO): the 4x4 inverse, its last results kept
    ("_Z20InvertMatrix4x4_FullPKfPf", "InvertMatrix4x4_Full", ""),
    ("_Z25InvertMatrix4x4_General3DPKfPf", "InvertMatrix4x4_General3D", ""),
    // unityverts.c (UA_FAST_VERTS): dynamic batching's vertex transform
    ("_Z26TransformVerticesInnerLoopILb0ELb0ELb1EE", "TransformVerticesInnerLoop<false, false, true>", ""),
    // unityhooks.c (UA_PRESENT_SYNC): where the main thread waits for the last frame's present
    ("_Z61GraphicsDeviceSettings_Set_Custom_PropWaitForPresentSyncPoint", "GraphicsDeviceSettings.waitForPresentSyncPoint (set)", "call"),
    ("_ZN15GfxDeviceClient10BeginFrameEv", "GfxDeviceClient::BeginFrame", "call"),
    ("_ZN15GfxDeviceClient8EndFrameEv", "GfxDeviceClient::EndFrame", "call"),
    // unitybeh.c (UA_BEH_PREFETCH, UA_BEH_EMPTY): the behaviour loops' iterator and the script call, and the code
    // their offsets were read from
    ("_ZN12SafeIteratorI4ListI8ListNodeI9BehaviourEEE4NextEv", "SafeIterator<List<ListNode<Behaviour> > >::Next", ""),
    ("_ZN13MonoBehaviour16CallUpdateMethodEi", "MonoBehaviour::CallUpdateMethod", ""),
    ("_ZNK10GameObject8IsActiveEv", "GameObject::IsActive", "call"),
    // unityscripts.c (UA_SCRIPT_FIND): the script lookup AddComponent and serialization make, a class's change and a
    // script's end (which empty its cache), and the PPtr dereference it calls
    ("_ZN17MonoScriptManager17FindRuntimeScriptE17ScriptingClassPtr", "MonoScriptManager::FindRuntimeScript(ScriptingClassPtr)", ""),
    ("_ZN10MonoScript5RenewE17ScriptingClassPtr", "MonoScript::Renew", ""),
    ("_ZN10MonoScriptD2Ev", "MonoScript::~MonoScript (D2)", ""),
    ("_ZN10MonoScriptD0Ev", "MonoScript::~MonoScript (D0)", "call"),
    ("_ZNK4PPtrI10MonoScriptEcvPS0_Ev", "PPtr<MonoScript>::operator MonoScript*", "call"),
    ("_ZNK4core8hash_mapIiP6ObjectNS_4hashIiEESt8equal_toIiEE6lookupERKi", "core::hash_map<int, Object*>::lookup", "call"),
    ("_ZN4core8hash_mapIiP6ObjectNS_4hashIiEESt8equal_toIiEE5eraseERKi", "core::hash_map<int, Object*>::erase", ""),
    ("_ZN20BaseBehaviourManager12CommonUpdateI16BehaviourManagerEEvv", "BaseBehaviourManager::CommonUpdate<BehaviourManager>", "call"),
    // fmodasync.c (UA_FMOD_ASYNC, UA_FMOD_COUNT): FMOD's update replaced (it runs on a worker), every entry of FMOD's
    // API gated (its handle's validate, the first call of each), the update's caller (where its result is dropped)
    ("_ZN4FMOD6System6updateEv", "FMOD::System::update", "replace"),
    ("_ZN4FMOD7SystemI6updateEv", "FMOD::SystemI::update", "call"),
    ("_ZN12AudioManager6UpdateEv", "AudioManager::Update", "call"),
    ("_ZN4FMOD7SystemI8validate", "FMOD::SystemI::validate", ""),
    ("_ZN4FMOD8ChannelI8validate", "FMOD::ChannelI::validate", ""),
    ("_ZN4FMOD13ChannelGroupI8validate", "FMOD::ChannelGroupI::validate", ""),
    ("_ZN4FMOD6SoundI8validate", "FMOD::SoundI::validate", ""),
    ("_ZN4FMOD4DSPI8validate", "FMOD::DSPI::validate", ""),
    ("_ZN4FMOD14DSPConnectionI8validate", "FMOD::DSPConnectionI::validate", ""),
    ("_ZN4FMOD7ReverbI8validate", "FMOD::ReverbI::validate", ""),
    // unityprefetch.c (UA_PREFETCH): the loops whose call sites get a prefetch, the functions they call there,
    // and the code the structure offsets were read from (all only checked: a call site's one word is changed)
    ("_ZN20BaseBehaviourManager12CommonUpdateI20LateBehaviourManagerEEvv", "BaseBehaviourManager::CommonUpdate<LateBehaviourManager>", "call"),
    ("_ZN20BaseBehaviourManager12CommonUpdateI21FixedBehaviourManagerEEvv", "BaseBehaviourManager::CommonUpdate<FixedBehaviourManager>", "call"),
    ("_ZN12SafeIteratorI4ListI8ListNodeI13MonoBehaviourEEE4NextEv", "SafeIterator<List<ListNode<MonoBehaviour> > >::Next", "call"),
    ("_ZN13MonoBehaviour6UpdateEv", "MonoBehaviour::Update", "call"),
    ("_ZNK13MonoBehaviour23IsStateMachineBehaviourEv", "MonoBehaviour::IsStateMachineBehaviour", "call"),
    ("_ZNK22SerializableManagedRef14GetScriptCacheEv", "SerializableManagedRef::GetScriptCache", "call"),
    ("_ZN9Scripting32GetCachedPtrFromScriptingWrapperE", "Scripting::GetCachedPtrFromScriptingWrapper", "call"),
    ("_ZL24PrepareSpriteRenderNodesILb0EE", "PrepareSpriteRenderNodes<false>", "call"),
    ("_ZL24PrepareSpriteRenderNodesILb1EE", "PrepareSpriteRenderNodes<true>", "call"),
    ("_ZN12BaseRenderer16FlattenBasicDataE", "BaseRenderer::FlattenBasicData", "call"),
    ("_Z40ProcessCameraIndexListIsNodeVisibleStep1", "ProcessCameraIndexListIsNodeVisibleStep1", "call"),
    ("_Z17IsNodeVisibleFast", "IsNodeVisibleFast", "call"),
    ("_ZN25PreOrderPlayableTraverser8TraverseE", "PreOrderPlayableTraverser::Traverse", "call"),
    ("_ZN25PreOrderPlayableTraverser14RecursiveVisitE", "PreOrderPlayableTraverser::RecursiveVisit", "call"),
    ("_ZN14ParticleSystem25UpdateManualTransformDataEb", "ParticleSystem::UpdateManualTransformData", "call"),
    ("_ZNK10GameObject20QueryComponentByTypeEPKN5Unity4TypeE", "GameObject::QueryComponentByType", "call"),
    // nativebeh.c (UA_NATIVE_BEH): the three behaviour update loops replaced by our own; the functions they call (or
    // whose code they carry out in place: the iterator's step, the script call's checks, the shared_ptr's atomics) only
    // checked; ScriptingInvocation::Invoke and InvokeMethodOrCoroutineChecked hooked for UA_NATIVE_BEH=verify / _STATS
    ("_ZN20BaseBehaviourManager12CommonUpdateI16BehaviourManagerEEvv", "BaseBehaviourManager::CommonUpdate<BehaviourManager> (replaced)", "replace"),
    ("_ZN20BaseBehaviourManager12CommonUpdateI20LateBehaviourManagerEEvv", "BaseBehaviourManager::CommonUpdate<LateBehaviourManager> (replaced)", "replace"),
    ("_ZN20BaseBehaviourManager12CommonUpdateI21FixedBehaviourManagerEEvv", "BaseBehaviourManager::CommonUpdate<FixedBehaviourManager> (replaced)", "replace"),
    ("_ZN12SafeIteratorI4ListI8ListNodeI9BehaviourEEEC2ERS4_", "SafeIterator<List<ListNode<Behaviour> > >::SafeIterator", "call"),
    ("_ZN20BaseBehaviourManager14IntegrateListsEv", "BaseBehaviourManager::IntegrateLists", "call"),
    ("_ZN19ManagedTempMemScopeC2E25ManagedTempMemScopePolicy", "ManagedTempMemScope::ManagedTempMemScope", "call"),
    ("_ZN19ManagedTempMemScopeD2Ev", "ManagedTempMemScope::~ManagedTempMemScope", "call"),
    ("_ZN13MonoBehaviour10LateUpdateEv", "MonoBehaviour::LateUpdate", "call"),
    ("_ZN13MonoBehaviour11FixedUpdateEv", "MonoBehaviour::FixedUpdate", "call"),
    ("_ZN17ScriptingGCHandle28ResolveBackendNativeGCHandleEm", "ScriptingGCHandle::ResolveBackendNativeGCHandle", "call"),
    ("_ZL15IsInstanceValid18ScriptingObjectPtr", "IsInstanceValid", "call"),
    ("_ZNK9Behaviour10GetEnabledEv", "Behaviour::GetEnabled", "call"),
    ("_ZN13MonoBehaviour30InvokeMethodOrCoroutineCheckedE18ScriptingMethodPtr18ScriptingObjectPtr$", "MonoBehaviour::InvokeMethodOrCoroutineChecked", ""),
    // unityhooks.c (UA_JOB_WORKERS): Unity 2017.x's job queue made with its worker count
    ("_Z14CreateJobQueuePKcS0_i", "CreateJobQueue", ""),
    // unityhooks.c (UA_JOB_SPLIT): the job system's split functions, replaced by ones that answer for another worker count
    ("_ZN9JobSystem22GetJobQueueThreadCountEv", "JobSystem::GetJobQueueThreadCount", "replace"),
    ("_Z37CalculateJobCountWithMinIndicesPerJobii", "CalculateJobCountWithMinIndicesPerJob", "replace"),
    ("_Z53CalculateJobCountWithMinIndicesPerJobMaxWorkerThreadsii", "CalculateJobCountWithMinIndicesPerJobMaxWorkerThreads", "replace"),
    ("_ZN19ScriptingInvocationC2E18ScriptingObjectPtr18ScriptingMethodPtr", "ScriptingInvocation::ScriptingInvocation", "call"),
    ("_ZN19ScriptingInvocation6InvokeEP21ScriptingExceptionPtrb", "ScriptingInvocation::Invoke", ""),
    ("_ZNSt6__ndk119__shared_weak_count4lockEv", "std::__shared_weak_count::lock", "call"),
    ("_ZNSt6__ndk119__shared_weak_count14__release_weakEv", "std::__shared_weak_count::__release_weak", "call"),
    ("__aarch64_ldadd8_relax", "__aarch64_ldadd8_relax", "call"),
    ("__aarch64_ldadd8_acq_rel", "__aarch64_ldadd8_acq_rel", "call"),
    ("__aarch64_cas8_acq_rel", "__aarch64_cas8_acq_rel", "call"),
    // enginerw.c: UA_ANCHOR_WALK (replaced whole), UA_CULL_STEP1 (replaced whole; IsNodeVisibleFast above),
    // UA_DIRECTOR_PREWALK (ExecutePrepareFrames: its call of RebuildPrepareFrameJobs in 6000.0, its entry in 2017.4;
    // the traversal's code its offsets were read from), UA_PREFETCH's anim site on 2017.4 (Animator::BuildJobs' call of
    // the output's target getter; on 6000.0 the getter is folded into another symbol: unityprefetch.c names its
    // address), UA_ALLOC_STATS (MemoryManager::Allocate, counted)
    ("_ZN2UI13RectTransform38UpdateAnchorPositionIfTransformChangedEb", "UI::RectTransform::UpdateAnchorPositionIfTransformChanged", "replace"),
    ("_ZN15DirectorManager20ExecutePrepareFramesE", "DirectorManager::ExecutePrepareFrames", ""),
    ("_ZN15DirectorManager24RebuildPrepareFrameJobsEv", "DirectorManager::RebuildPrepareFrameJobs", "call"),
    ("_ZN14PlayableOutput15PrepareTraverseE", "PlayableOutput::PrepareTraverse", "call"),
    ("_ZN13PlayableGraph12PrepareFrameE", "PlayableGraph::PrepareFrame", "call"),
    ("_ZN8Animator9BuildJobsE", "Animator::BuildJobs", "call"),
    ("_ZNK23AnimationPlayableOutput17GetTargetAnimatorEv", "AnimationPlayableOutput::GetTargetAnimator", "call"),
    ("_ZN13MemoryManager8AllocateEmm10MemLabelId15AllocateOptionsPKci$", "MemoryManager::Allocate", ""),
];

pub fn fnv1a(b: &[u8]) -> u64 {
    b.iter().fold(0xcbf29ce484222325u64, |h, &x| (h ^ x as u64).wrapping_mul(0x100000001b3))
}

/// an ARM-state instruction that computes the same wherever it runs (no pc operand), or a b<cond>,
/// which the runtime rewrites for its new place; never a bl
pub fn arm_movable(w: u32) -> bool {
    if w >> 28 == 0xf {
        return false; // unconditional space (blx imm, pld, ...)
    }
    if w & 0x0f000000 == 0x0b000000 {
        return false; // bl
    }
    if w & 0x0f000000 == 0x0a000000 {
        return true; // b<cond>: the runtime loads its target into the pc instead
    }
    if w & 0x0ffffff0 == 0x012fff10 || w & 0x0ffffff0 == 0x012fff30 {
        return false; // bx / blx reg
    }
    if w & 0x0e000000 == 0x08000000 {
        // ldm / stm: pc in the list or as base
        return w & 0x8000 == 0 && (w >> 16) & 15 != 15;
    }
    let mut regs = vec![(w >> 16) & 15, (w >> 12) & 15, w & 15];
    if w & 0x0c000000 == 0x04000000 {
        // ldr / str immediate or register
        regs = vec![(w >> 16) & 15, (w >> 12) & 15];
        if w & 0x02000000 != 0 {
            regs.push(w & 15);
        }
    }
    !regs.contains(&15)
}

/// the target of an A64 direct branch at pc, else None
pub fn a64_target(w: u32, pc: i64) -> Option<i64> {
    let sx = |imm: u32, bits: u32| -> i64 { if imm & (1 << (bits - 1)) != 0 { imm as i64 - (1i64 << bits) } else { imm as i64 } };
    if w & 0x7c000000 == 0x14000000 {
        return Some(pc + 4 * sx(w & 0x3ffffff, 26)); // b / bl
    }
    if w & 0xff000010 == 0x54000000 || w & 0x7e000000 == 0x34000000 {
        return Some(pc + 4 * sx((w >> 5) & 0x7ffff, 19)); // b.cond, cbz / cbnz
    }
    if w & 0x7e000000 == 0x36000000 {
        return Some(pc + 4 * sx((w >> 5) & 0x3fff, 14)); // tbz / tbnz
    }
    None
}

/// an A64 instruction the trampoline can run: one that computes the same wherever it runs, or a direct
/// branch, adr or adrp, which the runtime rewrites for its new place. Never a literal load or an
/// indirect branch, and none that may write x16 or x17 (any register field a write can be in counts)
pub fn a64_movable(w: u32) -> bool {
    if w & 0x3b000000 == 0x18000000 {
        return false; // ldr (literal), ldrsw, prfm literal
    }
    if w & 0xfe000000 == 0xd6000000 {
        return false; // br / blr / ret
    }
    if w & 0x7c000000 == 0x14000000 {
        return true; // b / bl (no register field)
    }
    let mut regs = vec![w & 31];
    if w & 0x0a000000 == 0x08000000 {
        // loads and stores: the base (writeback), a pair's second register
        regs.push((w >> 5) & 31);
        if w & 0x3a000000 == 0x28000000 {
            regs.push((w >> 10) & 31);
        }
    }
    !regs.iter().any(|r| *r == 16 || *r == 17)
}

/// (offset, halfword 1, halfword 2) of each Thumb instruction of code
pub fn thumb_insns(code: &[u8]) -> Vec<(usize, u16, Option<u16>)> {
    let mut out = Vec::new();
    let mut i = 0;
    while i + 2 <= code.len() {
        let h = u16le(code, i);
        if matches!(h >> 11, 0x1d..=0x1f) && i + 4 <= code.len() {
            out.push((i, h, Some(u16le(code, i + 2))));
            i += 4;
        } else {
            out.push((i, h, None));
            i += 2;
        }
    }
    out
}

/// a Thumb instruction that computes the same wherever it runs (no pc operand, no branch but a return)
pub fn thumb_movable(h: u16, h2: Option<u16>) -> bool {
    let Some(h2) = h2 else {
        if h == 0x4770 || h & 0xff00 == 0xbd00 {
            return true; // bx lr, pop {..., pc}: return
        }
        if h & 0xf000 == 0xd000 || h & 0xf800 == 0xe000 || h & 0xf500 == 0xb100 {
            return false; // b<c>, b, cbz / cbnz
        }
        if h & 0xf800 == 0x4800 || h & 0xf800 == 0xa000 {
            return false; // ldr literal, adr
        }
        if h & 0xfc00 == 0x4400 {
            // add / cmp / mov / bx / blx with high registers
            return h & 0x0078 != 0x0078 && h & 0x0087 != 0x0087 && h & 0xff00 != 0x4700;
        }
        return true;
    };
    if h & 0xf800 == 0xf000 && h2 & 0x8000 != 0 {
        return false; // b.w, b<c>.w, bl, blx, and the other branch-space forms
    }
    h & 0xf != 0xf // Rn = pc (literal loads, adr.w, tbb, ...): never moved
}

/// the first `moved` bytes are whole Thumb instructions that can be moved, any IT block whole among them
pub fn thumb_entry_ok(code: &[u8], moved: usize) -> bool {
    let mut it = 0u32;
    for (off, h, h2) in thumb_insns(&code[..(moved + 4).min(code.len())]) {
        if off >= moved {
            return off == moved && it == 0;
        }
        if off + if h2.is_some() { 4 } else { 2 } > moved || !thumb_movable(h, h2) {
            return false;
        }
        it = it.saturating_sub(1);
        if h2.is_none() && h & 0xff00 == 0xbf00 && h & 0xf != 0 {
            // IT: up to four instructions follow
            it = 4 - (h & 0xf).trailing_zeros();
        }
    }
    it == 0
}

/// (pc, target) of each direct Thumb branch in code at start
pub fn thumb_targets(code: &[u8], start: i64) -> Vec<(i64, i64)> {
    let mut out = Vec::new();
    for (off, h, h2) in thumb_insns(code) {
        let pc = start + off as i64;
        let h = h as i64;
        match h2 {
            None => {
                if h & 0xf000 == 0xd000 && h & 0x0e00 != 0x0e00 {
                    out.push((pc, pc + 4 + (((h & 0xff) ^ 0x80) - 0x80) * 2));
                } else if h & 0xf800 == 0xe000 {
                    out.push((pc, pc + 4 + (((h & 0x7ff) ^ 0x400) - 0x400) * 2));
                } else if h & 0xf500 == 0xb100 {
                    out.push((pc, pc + 4 + ((((h >> 9) & 1) << 6) | (((h >> 3) & 0x1f) << 1))));
                }
            }
            Some(h2) => {
                let h2 = h2 as i64;
                if h & 0xf800 == 0xf000 && h2 & 0x8000 != 0 {
                    let (s, j1, j2) = ((h >> 10) & 1, (h2 >> 13) & 1, (h2 >> 11) & 1);
                    if h2 & 0x1000 != 0 {
                        // b.w, bl, blx
                        let (i1, i2) = (1 - (j1 ^ s), 1 - (j2 ^ s));
                        let imm = s << 24 | i1 << 23 | i2 << 22 | (h & 0x3ff) << 12 | (h2 & 0x7ff) << 1;
                        out.push((pc, pc + 4 + imm - (s << 25)));
                    } else if h & 0x0380 != 0x0380 {
                        // b<c>.w
                        let imm = s << 20 | j2 << 19 | j1 << 18 | (h & 0x3f) << 12 | (h2 & 0x7ff) << 1;
                        out.push((pc, pc + 4 + imm - (s << 21)));
                    }
                }
            }
        }
    }
    out
}

/// offsets in the function whose direct branch target lies in [lo, hi)
pub fn branches_into(code: &[u8], start: i64, isa: char, lo: i64, hi: i64) -> Vec<i64> {
    if isa == 't' {
        return thumb_targets(code, start).into_iter().filter(|(_, t)| lo <= *t && *t < hi).map(|(pc, _)| pc).collect();
    }
    let mut hits = Vec::new();
    let mut i = 0;
    while i + 3 < code.len() {
        let w = u32le(code, i);
        let pc = start + i as i64;
        let t = if isa == 'a' {
            if w & 0x0e000000 == 0x0a000000 && w >> 28 != 0xf {
                let imm = (w & 0xffffff) as i64;
                let imm = if imm & 0x800000 != 0 { imm - (1 << 24) } else { imm };
                Some(pc + 8 + imm * 4)
            } else {
                None
            }
        } else {
            a64_target(w, pc)
        };
        if let Some(t) = t {
            if lo <= t && t < hi {
                hits.push(pc);
            }
        }
        i += 4;
    }
    hits
}

fn pylist(v: &[i64]) -> String {
    format!("[{}]", v.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(", "))
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let usage = "usage: pmu-dev unityhooks <libunity.so> <libunity.sym.so> [--unity <version abi>]";
    let mut pos = Vec::new();
    let mut unity = "?".to_string();
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if a == "--unity" {
            unity = args.get(i + 1).ok_or(usage)?.clone();
            i += 1;
        } else if let Some(v) = a.strip_prefix("--unity=") {
            unity = v.to_string();
        } else if a.starts_with('-') {
            return Err(usage.into());
        } else {
            pos.push(a.clone());
        }
        i += 1;
    }
    let [lib, sym] = &pos[..] else { return Err(usage.into()) };
    let (lib, sym) = (Elf::open(lib)?, Elf::open(sym)?);
    let (out, err, ok) = rows(&lib, &sym, &unity)?;
    eprint!("{err}");
    print!("{out}");
    Ok(if ok { 0 } else { 1 })
}

/// (rows, messages, whether any row was written)
pub fn rows(lib: &Elf, sym: &Elf, unity: &str) -> Result<(String, String, bool), String> {
    let (mut out, mut err) = (String::new(), String::new());
    if let (Some(bl), Some(bs)) = (lib.build_id(), sym.build_id()) {
        if bl != bs {
            return Err(format!("the symbol file is not this library's: build id {bs} against {bl}"));
        }
    }
    let mut funcs: HashMap<&str, Vec<(u64, u64, String)>> = HashMap::new();
    for (name, val, size) in sym.funcs() {
        for (pre, short, _) in HOOKS {
            let hit = match pre.strip_suffix('$') {
                Some(whole) => name == whole,
                None => name.starts_with(pre),
            };
            let got = funcs.entry(short).or_default();
            if hit && !got.iter().any(|g| g.0 == val && g.1 == size) {
                got.push((val, size, name.clone())); // (D1/D2 and C1/C2: one function)
            }
        }
    }
    let mut ok = 0;
    for (_, short, kind) in HOOKS {
        let got = funcs.get(short).map(Vec::as_slice).unwrap_or(&[]);
        if got.len() != 1 {
            let names: Vec<&str> = got.iter().map(|g| g.2.as_str()).collect();
            let names = if names.is_empty() { "none".to_string() } else { names.join(", ") };
            err += &format!("// {short}: {} symbols ({names}): no row\n", got.len());
            continue;
        }
        let (val, size, _) = &got[0];
        let (isa, addr) = if lib.is64 { ('x', *val) } else if val & 1 != 0 { ('t', val & !1) } else { ('a', val & !1) };
        let Some((off, fl)) = lib.file_off(addr, *size).filter(|(_, fl)| fl & 1 != 0) else {
            err += &format!("// {short}: +{addr:#x} is not in the library's code: no row\n");
            continue;
        };
        let _ = fl;
        let code = &lib.d[off..off + *size as usize];
        let moved: usize = match isa {
            'a' => 8,
            'x' => 16,
            _ if addr & 2 != 0 => 10,
            _ => 8,
        };
        let isa_name = match isa {
            'a' => "ARM",
            't' => "THUMB",
            _ => "A64",
        };
        let row = format!("    {{ \"{unity}\", \"{short}\", {addr:#x}, {size:#x}, ISA_{isa_name}, {:#018x}ull }},\n", fnv1a(code));
        let a = addr as i64;
        if *kind == "call" {
            out += &row;
            ok += 1;
            continue;
        }
        if *kind == "replace" {
            let into = branches_into(code, a, isa, a + 1, a + moved as i64);
            if *size as usize <= moved || !into.is_empty() {
                err += &format!("// {short}: {size} bytes, branches into the entry from {}: no row\n", pylist(&into));
                continue;
            }
            out += &row;
            ok += 1;
            continue;
        }
        if isa == 't' {
            if !thumb_entry_ok(code, moved) {
                err += &format!("// {short}: the entry's {moved} bytes cannot be moved: no row\n");
                continue;
            }
        } else {
            if code.len() < moved {
                return Err(format!("{short}: {size} bytes, less than the entry"));
            }
            let mov = if isa == 'a' { arm_movable } else { a64_movable };
            if let Some(bad) = (0..moved / 4).find(|k| !mov(u32le(code, 4 * k))) {
                err += &format!("// {short}: instruction {bad} of the entry cannot be moved: no row\n");
                continue;
            }
        }
        // (a branch among the moved instructions to the first one would reach the jump to the hook)
        let mut into = branches_into(code, a, isa, a + if isa == 't' { 2 } else { 4 }, a + moved as i64);
        if into.is_empty() {
            into = branches_into(&code[..moved.min(code.len())], a, isa, a, a + 2);
        }
        if !into.is_empty() {
            let xs: Vec<String> = into.iter().map(|x| format!("{x:#x}")).collect();
            err += &format!("// {short}: branches into the entry from {}: no row\n", xs.join(", "));
            continue;
        }
        out += &row;
        ok += 1;
    }
    // fmodasync.c: where AudioManager::Update's call of FMOD::System::update returns
    let (am, up) = (funcs.get("AudioManager::Update"), funcs.get("FMOD::System::update"));
    if let (Some([(val, size, _)]), Some([(uval, _, _)])) = (am.map(Vec::as_slice), up.map(Vec::as_slice)) {
        let isa = if lib.is64 { 'x' } else if val & 1 != 0 { 't' } else { 'a' };
        let addr = if isa != 'x' { val & !1 } else { *val };
        let target = (uval & !1) as i64;
        let mut sites: Vec<i64> = Vec::new();
        if let Some((off, _)) = lib.file_off(addr, *size) {
            let code = &lib.d[off..off + *size as usize];
            let a = addr as i64;
            if isa == 't' {
                sites = thumb_targets(code, a).into_iter().filter(|(_, t)| *t == target).map(|(pc, _)| pc + 4).collect();
            } else {
                let mut i = 0;
                while i + 3 < *size as usize {
                    let w = u32le(code, i);
                    let pc = a + i as i64;
                    let t = if isa == 'x' {
                        a64_target(w, pc)
                    } else if w & 0x0f000000 == 0x0b000000 {
                        Some(pc + 8 + ((((w & 0xffffff) ^ 0x800000) as i64 - 0x800000) * 4))
                    } else {
                        None
                    };
                    if t == Some(target) && (w & 0xfc000000 == 0x94000000 || isa == 'a') {
                        sites.push(pc + 4);
                    }
                    i += 4;
                }
            }
        }
        let s: Vec<String> = sites.iter().map(|x| format!("{:#x}", x | (isa == 't') as i64)).collect();
        out += &format!(
            "    // AudioManager::Update: FMOD::System::update returns to {}\n",
            if s.is_empty() { "(no call found)".to_string() } else { s.join(", ") }
        );
    }
    Ok((out, err, ok > 0))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pack16(h: &[u16]) -> Vec<u8> {
        h.iter().flat_map(|x| x.to_le_bytes()).collect()
    }
    fn pack32(w: &[u32]) -> Vec<u8> {
        w.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    #[test]
    fn arm() {
        for w in [0xe92d4070, 0xe1a04001, 0xe24dd008, 0xe3500003, 0x0a000004] {
            assert!(arm_movable(w), "{w:#x}");
        }
        for w in [0xe59f0010, 0xe08f0000, 0xeb000010, 0xe12fff1e, 0xfa000001, 0xe8bd8070] {
            assert!(!arm_movable(w), "{w:#x}");
        }
    }

    #[test]
    fn a64() {
        for w in [0xa9be57fe, 0xa9014ff4, 0x71000c1f, 0x2a0103f3, 0xf0ff9409, 0x540003a0, 0x94000010, 0x14000011, 0x36000189, 0xb4000040] {
            assert!(a64_movable(w), "{w:#x}");
        }
        for w in [0x58000050, 0xd65f03c0, 0xd61f0200, 0x91000210] {
            assert!(!a64_movable(w), "{w:#x}");
        }
    }

    #[test]
    fn thumb_entry() {
        // a validate's entry: cmp r0, #0; itt eq; moveq r0, #0x24; bxeq lr; cmp r1, #0; itt eq; ...
        let v = pack16(&[0x2800, 0xbf04, 0x2024, 0x4770, 0x2900, 0xbf04, 0x2025, 0x4770]);
        assert!(thumb_entry_ok(&v, 8));
        assert!(thumb_entry_ok(&v, 10));
        assert!(!thumb_entry_ok(&v, 12)); // an IT block cut in two
        assert!(thumb_entry_ok(&pack16(&[0xb5f0, 0xaf03, 0xf84d, 0x8d04]), 8));
        let wrapper = pack16(&[0xb580, 0x466f, 0xb082, 0xa901, 0xf7b2, 0xfd85]);
        assert!(thumb_entry_ok(&wrapper, 8));
        assert!(!thumb_entry_ok(&wrapper, 10)); // half of the bl
    }

    #[test]
    fn thumb_moves() {
        for (h, h2) in [(0x2800, None), (0xb5f0, None), (0x4770, None), (0xbd80, None), (0x466f, None), (0xe92d, Some(0x41f0))] {
            assert!(thumb_movable(h, h2), "{h:#x}");
        }
        for (h, h2) in [
            (0x4800, None), (0xa000, None), (0x4478, None), (0xd001, None), (0xe7fe, None), (0xb108, None), (0x4780, None),
            (0xf7b2, Some(0xfd85)), (0xf8df, Some(0xf000)),
        ] {
            assert!(!thumb_movable(h, h2), "{h:#x}");
        }
    }

    #[test]
    fn thumb_branches() {
        assert_eq!(thumb_targets(&pack16(&[0xf7b2, 0xfd85]), 0x1257d46), [(0x1257d46, 0x120a854)]);
        assert_eq!(thumb_targets(&pack16(&[0xd000, 0xe7fe]), 0x10), [(0x10, 0x14), (0x12, 0x12)]);
        assert_eq!(branches_into(&pack16(&[0xd000, 0xe7fe]), 0x10, 't', 0x12, 0x14), [0x12]);
    }

    #[test]
    fn into_entry() {
        let words = [0xa9be57fe, 0xa9014ff4, 0x71000c1f, 0x2a0103f3];
        let back = 0x54000001 | ((((0x1008i64 - 0x1010) / 4) as u32 & 0x7ffff) << 5);
        assert_eq!(branches_into(&pack32(&[words[0], words[1], words[2], words[3], back]), 0x1000, 'x', 0x1004, 0x1010), [0x1010]);
        let fwd = 0x54000001 | (((0x1014 - 0x1010) / 4) << 5);
        assert_eq!(branches_into(&pack32(&[words[0], words[1], words[2], words[3], fwd]), 0x1000, 'x', 0x1004, 0x1010), Vec::<i64>::new());
        let b = 0xea000000 | ((-4i32) as u32 & 0xffffff);
        assert_eq!(branches_into(&pack32(&[0xe92d4070, 0xe1a04001, 0xe3500003, b]), 0x2000, 'a', 0x2004, 0x2008), [0x200c]);
    }

    #[test]
    fn fnv() {
        assert_eq!(fnv1a(b""), 0xcbf29ce484222325);
        assert_eq!(fnv1a(b"a"), 0xaf63dc4c8601ec8c);
    }
}
