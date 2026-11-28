//! `pmu-dev unityhooks <libunity.so> <libunity.sym.so> [--unity <version abi>]` (was
//! tools/re/unityhooks.py): the rows of runtime/host/unityhooks.c for one Unity player: each hooked
//! function's offset, size, instruction set and the FNV-1a 64 hash of its bytes, read from the
//! player's libunity.so with Unity's symbol file for it (`pmu player fetch <version> --symbols`:
//! Variations/<backend>/Release/Symbols/<abi>/libunity.sym.so).
//!
//! A64 players only (a 32-bit libunity is refused: armhf was removed, tag last-armhf). It checks what
//! the runtime's hook relies on, and prints no row where a check fails:
//!   - the symbol file belongs to this library (the same GNU build id, when the library has one; else
//!     the function's bytes must at least be in the library's text);
//!   - the bytes the jump replaces (16, four A64 instructions) are instructions that can run anywhere
//!     else: no literal load, no indirect branch, no write of x16 or x17 (the trampoline's jumps use
//!     x16), and a direct branch only to outside those bytes (the runtime rewrites direct branches, adr
//!     and adrp for their new place);
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
    // unityverts.c (UA_FAST_VERTS): dynamic batching's vertex transform
    ("_Z26TransformVerticesInnerLoopILb0ELb0ELb1EE", "TransformVerticesInnerLoop<false, false, true>", ""),
    // unityhooks.c (UA_PRESENT_SYNC): where the main thread waits for the last frame's present
    ("_Z61GraphicsDeviceSettings_Set_Custom_PropWaitForPresentSyncPoint", "GraphicsDeviceSettings.waitForPresentSyncPoint (set)", "call"),
    ("_ZN15GfxDeviceClient10BeginFrameEv", "GfxDeviceClient::BeginFrame", "call"),
    ("_ZN15GfxDeviceClient8EndFrameEv", "GfxDeviceClient::EndFrame", "call"),
    // nativebeh.c, unityprefetch.c, enginerw.c: the script call, the GameObject's active state and the behaviour loop
    ("_ZN13MonoBehaviour16CallUpdateMethodEi", "MonoBehaviour::CallUpdateMethod", ""),
    ("_ZNK10GameObject8IsActiveEv", "GameObject::IsActive", "call"),
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

/// offsets in the function whose direct branch target lies in [lo, hi)
pub fn branches_into(code: &[u8], start: i64, lo: i64, hi: i64) -> Vec<i64> {
    let mut hits = Vec::new();
    let mut i = 0;
    while i + 3 < code.len() {
        let pc = start + i as i64;
        if let Some(t) = a64_target(u32le(code, i), pc) {
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
    if !lib.is64 {
        return Err("a 32-bit libunity: the runtime's hooks are A64 only (armhf was removed, tag last-armhf)".into());
    }
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
        let addr = *val;
        let Some((off, fl)) = lib.file_off(addr, *size).filter(|(_, fl)| fl & 1 != 0) else {
            err += &format!("// {short}: +{addr:#x} is not in the library's code: no row\n");
            continue;
        };
        let _ = fl;
        let code = &lib.d[off..off + *size as usize];
        let moved: usize = 16;
        let row = format!("    {{ \"{unity}\", \"{short}\", {addr:#x}, {size:#x}, ISA_A64, {:#018x}ull }},\n", fnv1a(code));
        let a = addr as i64;
        if *kind == "call" {
            out += &row;
            ok += 1;
            continue;
        }
        if *kind == "replace" {
            let into = branches_into(code, a, a + 1, a + moved as i64);
            if *size as usize <= moved || !into.is_empty() {
                err += &format!("// {short}: {size} bytes, branches into the entry from {}: no row\n", pylist(&into));
                continue;
            }
            out += &row;
            ok += 1;
            continue;
        }
        if code.len() < moved {
            return Err(format!("{short}: {size} bytes, less than the entry"));
        }
        if let Some(bad) = (0..moved / 4).find(|k| !a64_movable(u32le(code, 4 * k))) {
            err += &format!("// {short}: instruction {bad} of the entry cannot be moved: no row\n");
            continue;
        }
        // (a branch among the moved instructions to the first one would reach the jump to the hook)
        let mut into = branches_into(code, a, a + 4, a + moved as i64);
        if into.is_empty() {
            into = branches_into(&code[..moved.min(code.len())], a, a, a + 2);
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
        let addr = *val;
        let target = *uval as i64;
        let mut sites: Vec<i64> = Vec::new();
        if let Some((off, _)) = lib.file_off(addr, *size) {
            let code = &lib.d[off..off + *size as usize];
            let a = addr as i64;
            let mut i = 0;
            while i + 3 < *size as usize {
                let w = u32le(code, i);
                let pc = a + i as i64;
                if a64_target(w, pc) == Some(target) && w & 0xfc000000 == 0x94000000 {
                    sites.push(pc + 4);
                }
                i += 4;
            }
        }
        let s: Vec<String> = sites.iter().map(|x| format!("{x:#x}")).collect();
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

    fn pack32(w: &[u32]) -> Vec<u8> {
        w.iter().flat_map(|x| x.to_le_bytes()).collect()
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
    fn into_entry() {
        let words = [0xa9be57fe, 0xa9014ff4, 0x71000c1f, 0x2a0103f3];
        let back = 0x54000001 | ((((0x1008i64 - 0x1010) / 4) as u32 & 0x7ffff) << 5);
        assert_eq!(branches_into(&pack32(&[words[0], words[1], words[2], words[3], back]), 0x1000, 0x1004, 0x1010), [0x1010]);
        let fwd = 0x54000001 | (((0x1014 - 0x1010) / 4) << 5);
        assert_eq!(branches_into(&pack32(&[words[0], words[1], words[2], words[3], fwd]), 0x1000, 0x1004, 0x1010), Vec::<i64>::new());
    }

    #[test]
    fn fnv() {
        assert_eq!(fnv1a(b""), 0xcbf29ce484222325);
        assert_eq!(fnv1a(b"a"), 0xaf63dc4c8601ec8c);
    }
}
