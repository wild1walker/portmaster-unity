//! `pmu-dev profsum prof-1.txt [--syms DIR] [--lib FILE]... [--thread NAME] [--top N] [--frames]
//! [--slow MS] [--callers TEXT] [--within TEXT] [--offsets] [--exact] [--code] [--areas]` (was
//! tools/analysis/profsum.py): a UA_PROF recording summed up (runtime/host/prof.c writes <game
//! folder>/prof-<n>.txt).
//!
//! Per sampled thread: how its time splits (on a CPU, waiting for one, blocked and in which call), and
//! for the time on a CPU: by module (Unity's engine, Mono's runtime, managed code, libc, the GLES
//! driver, ...), by function ("self": where the pc was; "on the stack": the function is anywhere in the
//! sample, so callers add up), by managed method (the innermost JIT-compiled frame: the method and the
//! engine/runtime calls it makes), and who waits in the blocked samples. Then every thread's CPU time
//! over the recording and each core's use.
//!
//!   --syms DIR   Unity's symbol files (Variations/mono/Release/Symbols/armeabi-v7a from
//!                `pmu player fetch --symbols`): names libunity.so, libmonobdwgc-2.0.so, libmain.so
//!                (default: libunity.so from $UA_SYMS, $UA_LIBUNITY_SYM: crate::syms)
//!   --lib FILE   another file with symbols (runtime/host/out/ua_host, a library copied from the
//!                device); glibc libraries are otherwise named by the profile's own dladdr names
//!   --thread     only this thread (default: all, UnityMain first)
//!   --frames     UnityMain's time on a CPU per frame (from its frame times and samples)
//!   --slow MS    only samples in UnityMain frames longer than MS ms (the frames that miss a vsync)
//!   --callers T  the callers of functions whose name contains T, most frequent first
//!   --within T   only samples with a function whose name contains T on the stack: what that function's
//!                time is spent on (its callees in "self" and "on the stack"), with its share of each thread
//!   --offsets    keep offsets in managed method names (hot spots inside a method)
//!   --exact      only the callers that were unwound (not those found by scanning) for "on the
//!                stack", the managed method and --callers
//!   --code       (a recording made with UA_PROF_CODE=1) the anonymous code's samples by what the code is:
//!                Mono 2.6's ARM stubs told by their instructions (long-branch thunks with their targets,
//!                IMT thunks, delegate invoke, rgctx fetch, generic class init, static-rgctx, unbox) and its
//!                generic trampolines by kind (mono_trampoline_code)
//!   --areas      UnityMain's frame by area of Unity's player loop, in ms a frame: rendering (each
//!                camera stage), scripts (Update, FixedUpdate, LateUpdate, coroutines, OnGUI), physics,
//!                animation, particles, audio, UI; the waits (the previous frame's present: the render
//!                thread or the GPU; the job threads); and across them the system calls, managed code,
//!                Mono's runtime and memory management
//! Callers are unwound exactly through code with unwind tables (32-bit ARM EHABI; AArch64 frame
//! pointers) and found by scanning the stack above that (Mono's JIT code has no tables): scanned
//! entries can be stale, so without --exact "on the stack" is an upper bound.
use std::collections::{BTreeMap, HashMap};
use std::fmt::Write as _;

use crate::syms::ElfNames;
use crate::util::{bisect_right, fmt_g, pct, Counter};

fn sysname(arch: &str, nr: u64) -> String {
    let armhf: &[(u64, &str)] = &[
        (3, "read"), (4, "write"), (54, "ioctl"), (142, "select"), (158, "sched_yield"), (162, "nanosleep"), (168, "poll"),
        (179, "rt_sigsuspend"), (240, "futex"), (252, "epoll_wait"), (265, "clock_nanosleep"), (335, "pselect6"),
        (336, "ppoll"), (346, "epoll_pwait"), (407, "clock_nanosleep_time64"), (414, "ppoll_time64"),
        (422, "futex_time64"), (0xfffe, "(page fault)"), (0xffff, "?"),
    ];
    let arm64: &[(u64, &str)] = &[
        (22, "epoll_pwait"), (29, "ioctl"), (63, "read"), (64, "write"), (72, "pselect6"), (73, "ppoll"), (98, "futex"),
        (101, "nanosleep"), (115, "clock_nanosleep"), (124, "sched_yield"), (133, "rt_sigsuspend"),
        (0xfffe, "(page fault)"), (0xffff, "?"),
    ];
    let t = if arch == "arm64" { arm64 } else { armhf };
    t.iter().find(|x| x.0 == nr).map_or_else(|| format!("{nr:#x}"), |x| x.1.to_string())
}

const GLES: [&str; 7] = ["libmali", "libGLES", "libEGL", "libMali", "mali_", "libgpu", "libvulkan"];
const LIBC: [&str; 9] = ["libc.so", "libc-", "ld-linux", "libm.so", "libpthread", "libstdc++", "libgcc_s", "libdl", "librt"];
const MONO: [&str; 3] = ["libmonobdwgc", "libmono.so", "libmonosgen"];

#[derive(Clone, Debug)]
pub struct Stack {
    pub count: i64,
    pub thread: i64,
    pub state: String,
    pub sysnr: u64,
    pub pc: u64,
    pub lr: u64,
    pub exact: i64,
    pub rets: Vec<u64>,
    pub scanned: Vec<bool>,
}

pub struct Thread {
    pub tid: i64,
    pub name: String,
    pub dropped: i64,
}

pub struct Cpu {
    pub tid: i64,
    pub name: String,
    pub run: i64,
    pub wait: i64,
    pub slices: i64,
    pub ut: i64,
    pub st: i64,
}

pub struct Profile {
    pub meta: HashMap<String, String>,
    pub threads: BTreeMap<i64, Thread>,
    pub modules: Vec<(u64, u64, u64, String)>,
    pub jits: Vec<(u64, u64, String)>,
    pub syms: HashMap<u64, (String, u64)>,
    pub cpu: Vec<Cpu>,
    pub stat: Vec<(String, Vec<i64>)>,
    /// (stack id, stack) in the file's order
    pub stacks: Vec<(i64, Stack)>,
    pub stack_at: HashMap<i64, usize>,
    pub seq: BTreeMap<i64, Vec<(i64, i64)>>,
    pub swap: Vec<i64>,
    pub main: Vec<i64>,
    pub code: BTreeMap<u64, Vec<u8>>,
    pub tramps: Vec<(u64, usize)>,
    pub version: i64,
    pub hz: i64,
    pub secs: f64,
    pub arch: String,
}

fn hex(s: &str) -> u64 {
    u64::from_str_radix(s, 16).unwrap_or(0)
}
fn int(s: &str) -> i64 {
    s.parse().unwrap_or(0)
}

impl Profile {
    pub fn parse(text: &str) -> Profile {
        let mut p = Profile {
            meta: HashMap::new(),
            threads: BTreeMap::new(),
            modules: vec![],
            jits: vec![],
            syms: HashMap::new(),
            cpu: vec![],
            stat: vec![],
            stacks: vec![],
            stack_at: HashMap::new(),
            seq: BTreeMap::new(),
            swap: vec![],
            main: vec![],
            code: BTreeMap::new(),
            tramps: vec![],
            version: 1,
            hz: 0,
            secs: 0.0,
            arch: String::new(),
        };
        for line in text.lines() {
            let f: Vec<&str> = line.split_whitespace().collect();
            let Some(k) = f.first() else { continue };
            match *k {
                "stack" if f.len() >= 8 => {
                    // version 1: no <exact> field, every caller scanned; 2: the first <exact> unwound, the
                    // rest scanned; 3: scanned callers written ~<address>
                    let v2 = p.version >= 2;
                    let words: &[&str] = if f.len() > if v2 { 9 } else { 8 } { &f[if v2 { 9 } else { 8 }..] } else { &[] };
                    let rets = words.iter().map(|x| hex(x.trim_start_matches('~'))).collect();
                    let exact = if v2 { f.get(8).map_or(0, |x| int(x)) } else { 0 };
                    let scanned = if p.version >= 3 {
                        words.iter().map(|x| x.starts_with('~')).collect()
                    } else {
                        (0..words.len()).map(|i| i as i64 >= exact).collect()
                    };
                    let s = Stack {
                        count: int(f[2]),
                        thread: int(f[3]),
                        state: f[4].to_string(),
                        sysnr: hex(f[5]),
                        pc: hex(f[6]),
                        lr: hex(f[7]),
                        exact,
                        rets,
                        scanned,
                    };
                    let id = int(f[1]);
                    match p.stack_at.get(&id) {
                        Some(&i) => p.stacks[i].1 = s,
                        None => {
                            p.stack_at.insert(id, p.stacks.len());
                            p.stacks.push((id, s));
                        }
                    }
                }
                "seq" if f.len() >= 2 => {
                    let t = int(f[1]);
                    let v = p.seq.entry(t).or_default();
                    for e in &f[2..] {
                        if let Some((us, sid)) = e.split_once(':') {
                            v.push((int(us), int(sid)));
                        }
                    }
                }
                "module" if f.len() >= 5 => p.modules.push((hex(f[1]), hex(f[2]), hex(f[3]), f[4].to_string())),
                "jit" if f.len() >= 3 => {
                    let a = hex(f[1]);
                    p.jits.push((a, a + hex(f[2]), f[3..].join(" ")));
                }
                "sym" if f.len() >= 4 => {
                    p.syms.insert(hex(f[1]), (f[2].to_string(), hex(f[3])));
                }
                "code" if f.len() == 3 => {
                    let b: Vec<u8> = (0..f[2].len() / 2).filter_map(|i| u8::from_str_radix(&f[2][2 * i..2 * i + 2], 16).ok()).collect();
                    p.code.insert(hex(f[1]), b);
                }
                "tramp" if f.len() >= 3 => p.tramps.push((hex(f[2]), int(f[1]) as usize)),
                "thread" if f.len() >= 6 => {
                    p.threads.insert(int(f[1]), Thread { tid: int(f[2]), name: f[3].to_string(), dropped: int(f[5]) });
                }
                "cpu" if f.len() >= 8 => p.cpu.push(Cpu {
                    tid: int(f[1]),
                    name: f[2].to_string(),
                    run: int(f[3]),
                    wait: int(f[4]),
                    slices: int(f[5]),
                    ut: int(f[6]),
                    st: int(f[7]),
                }),
                "stat" if f.len() >= 2 => p.stat.push((f[1].to_string(), f[2..].iter().map(|x| int(x)).collect())),
                "swap" => p.swap.extend(f[1..].iter().map(|x| int(x))),
                "main" => p.main.extend(f[1..].iter().map(|x| int(x))),
                "depth" if f.len() >= 2 => {
                    p.meta.insert("depth".into(), f[1].into());
                }
                "arch" | "hz" | "secs" | "signal" | "mono" | "ua_prof" if f.len() >= 2 => {
                    p.meta.insert(k.to_string(), f[1].to_string());
                    if *k == "ua_prof" {
                        p.version = int(f[1]);
                    }
                }
                _ => {}
            }
        }
        p.modules.sort();
        p.jits.sort();
        p.hz = p.meta.get("hz").map_or(0, |x| int(x));
        p.secs = p.meta.get("secs").and_then(|x| x.parse().ok()).unwrap_or(0.0);
        p.arch = p.meta.get("arch").cloned().unwrap_or_else(|| "armhf".into());
        p
    }

    pub fn module(&self, a: u64) -> Option<&(u64, u64, u64, String)> {
        let i = self.modules.partition_point(|m| m.0 <= a);
        (i > 0 && a < self.modules[i - 1].1).then(|| &self.modules[i - 1])
    }

    pub fn jit(&self, a: u64) -> Option<&(u64, u64, String)> {
        let i = self.jits.partition_point(|j| j.0 <= a);
        (i > 0 && a < self.jits[i - 1].1).then(|| &self.jits[i - 1])
    }

    fn stack(&self, id: i64) -> Option<&Stack> {
        self.stack_at.get(&id).map(|&i| &self.stacks[i].1)
    }
}

// Mono 2.6's generic trampolines (MonoTrampolineType)
const TRAMP_KINDS: [&str; 12] = [
    "JIT", "jump", "class init", "generic class init", "rgctx lazy fetch", "AOT", "AOT PLT", "delegate",
    "restore stack protection", "generic virtual remoting", "monitor enter", "monitor exit",
];

pub fn category(m: &str) -> String {
    if m == "libunity.so" {
        "Unity engine (libunity)".into()
    } else if MONO.iter().any(|x| m.starts_with(x)) {
        "Mono runtime (GC, JIT, icall wrappers)".into()
    } else if GLES.iter().any(|x| m.starts_with(x)) {
        "GLES driver".into()
    } else if LIBC.iter().any(|x| m.starts_with(x)) {
        "libc, libm, libstdc++".into()
    } else if m.starts_with("ua_host") {
        "ua_host".into()
    } else if m.starts_with("libmain") {
        "libmain".into()
    } else {
        m.into()
    }
}

/// address -> (category, name)
pub struct Namer<'a> {
    p: &'a Profile,
    elf: ElfNames,
    offsets: bool,
    cache: HashMap<u64, (String, String)>,
}

impl<'a> Namer<'a> {
    pub fn new(p: &'a Profile, elf: ElfNames, offsets: bool) -> Namer<'a> {
        Namer { p, elf, offsets, cache: HashMap::new() }
    }

    pub fn name(&mut self, a: u64) -> (String, String) {
        if let Some(r) = self.cache.get(&a) {
            return r.clone();
        }
        let r = self.lookup(a);
        self.cache.insert(a, r.clone());
        r
    }

    fn lookup(&mut self, a: u64) -> (String, String) {
        let p = self.p;
        if let Some(j) = p.jit(a) {
            let mut n = j.2.clone();
            if self.offsets {
                n += &format!("+0x{:x}", a - j.0);
            }
            return ("managed".into(), n);
        }
        let Some(m) = p.module(a) else {
            return ("anonymous code (Mono trampolines, unnamed JIT)".into(), "anon".into());
        };
        let (md, off) = (m.3.clone(), a.wrapping_sub(m.2));
        let cat = category(&md);
        if let Some(f) = self.elf.name(&md, off) {
            return (cat, if md == "libunity.so" { f } else { format!("{md}:{f}") });
        }
        if let Some((s, _)) = p.syms.get(&a) {
            return (cat, format!("{md}:{s}"));
        }
        (cat, format!("{md}+0x{off:x}"))
    }
}

/// what the anonymous code at a is (UA_PROF_CODE): Mono 2.6's ARM stubs by their instruction words
fn anon_kind(p: &Profile, a: u64, name: &mut Namer) -> Option<String> {
    let mut tr = p.tramps.clone();
    tr.sort();
    for (i, (t, k)) in tr.iter().enumerate() {
        let end = (t + 0x300).min(if i + 1 < tr.len() { tr[i + 1].0 } else { t + 0x300 });
        if *t <= a && a < end {
            return Some(format!("generic trampoline: {}", TRAMP_KINDS.get(*k).map_or_else(|| k.to_string(), |s| s.to_string())));
        }
    }
    if p.code.is_empty() {
        return None;
    }
    let (st, b) = p.code.range(..=a).next_back()?;
    if a >= st + b.len() as u64 {
        return None;
    }
    let w = |x: i64| -> Option<u32> {
        let o = x - *st as i64;
        (0 <= o && o <= b.len() as i64 - 4).then(|| u32::from_le_bytes(b[o as usize..o as usize + 4].try_into().unwrap()))
    };
    let a = a as i64;
    let near: Vec<Option<u32>> = (-6..3).map(|k| w(a + 4 * k)).collect();
    let has = |x: u32| near.contains(&Some(x));
    for k in [0, -1] {
        // ldr ip, [pc]; bx ip; .word target
        if w(a + 4 * k) == Some(0xe59fc000) && w(a + 4 * k + 4) == Some(0xe12fff1c) {
            let t = w(a + 4 * k + 8);
            return Some(format!(
                "long-branch thunk to {}",
                match t {
                    Some(t) if t != 0 => {
                        let (c, n) = name.name(t as u64);
                        format!("{n} ({c})")
                    }
                    _ => "?".into(),
                }
            ));
        }
    }
    Some(
        if has(0xe92d0003) && has(0xe51e0004) {
            "IMT thunk".into()
        } else if has(0xe1d0c1db) {
            "generic class init (shared code)".into()
        } else if has(0xe590c008) && has(0xe1a0f00c) {
            "delegate invoke".into()
        } else if has(0xe5901020) {
            "rgctx fetch".into()
        } else if has(0xe59f8000) && has(0xe59ff000) {
            "static-rgctx trampoline".into()
        } else if has(0xe59fc004) && has(0xe12fff1c) {
            "unbox trampoline".into()
        } else if w(a) == Some(0xe12fff1e) {
            "a lone bx lr (a nullified class init?)".into()
        } else {
            format!("other ({:08x} at the pc)", w(a).unwrap_or(0))
        },
    )
}

/// pc, then the callers: lr when the unwind did not already use it (32-bit ARM's unwinder starts from
/// lr; AArch64's frame chain starts at the caller's caller), the unwound ones, then (unless
/// exact_only) the scanned ones
pub fn sample_frames(p: &Profile, s: &Stack, exact_only: bool) -> Vec<u64> {
    let use_lr = s.lr != 0 && !(p.arch == "armhf" && s.exact != 0);
    let mut v = vec![s.pc];
    if use_lr {
        v.push(s.lr);
    }
    if exact_only {
        v.extend(s.rets.iter().zip(&s.scanned).filter(|(_, sc)| !**sc).map(|(r, _)| *r));
    } else {
        v.extend(&s.rets);
    }
    v
}

/// index of the frame (between frame times) that holds us, or None
fn frame_of(times: &[i64], us: i64) -> Option<usize> {
    let i = bisect_right(times, &us);
    (0 < i && i < times.len()).then(|| i - 1)
}

// --areas: the first rule whose text is in a sample's frames (the unwound ones; if none matches, the
// scanned ones too) names its area. Engine function names of Unity's player loop (5.x - 6000)
const AREAS: &[(&str, &[&str])] = &[
    ("render: offscreen cameras (render textures)", &["RenderManager::RenderOffscreenCameras"]),
    ("render: image effects (OnRenderImage, blits)", &["RenderImageFilters"]),
    ("render: culling", &["Camera::Cull", "Camera::CustomCull"]),
    ("render: cameras (sorting, batching, draw calls)", &["RenderManager::RenderCameras"]),
    ("render: other", &["PlayerRender"]),
    ("physics: 2D step", &["PhysicsManager2D::Simulate"]),
    ("physics: 3D step", &["PhysicsManager::Simulate", "PhysicsManager::FixedUpdate"]),
    ("scripts: FixedUpdate", &["CommonUpdate<FixedBehaviourManager>"]),
    ("scripts: LateUpdate", &["CommonUpdate<LateBehaviourManager>"]),
    ("scripts: OnGUI (IMGUI)", &["GUIManager::", "MonoBehaviourDoGUI"]),
    ("scripts: coroutines, Invoke", &["DelayedCallManager::Update"]),
    ("scripts: Update", &["CommonUpdate<BehaviourManager>"]),
    ("animation (Animator, Director, Animation)", &["DirectorManager", "AnimationManager", "Animator::"]),
    ("particles", &["ParticleSystem::"]),
    ("audio (FMOD update)", &["AudioManager::Update", "AudioModule::Update", "FMOD::System"]),
    ("UI canvases", &["UI::Canvas"]),
    ("transforms, renderer bounds", &["RendererUpdateManager", "RenderManager::UpdateAllRenderers", "TransformChangeDispatch"]),
    ("input", &["InputUpdate", "InputManager::"]),
    ("other player loop", &["ExecutePlayerLoop", "PlayerLoop"]),
];
const ACROSS: &[(&str, &str, &[&str])] = &[
    ("futex system calls (waking or waiting for threads)", "self", &["Baselib_SystemFutex", "futex"]),
    ("clock_gettime / gettimeofday system calls", "self", &["clock_gettime", "gettimeofday"]),
    ("managed code (JIT)", "cat", &["managed"]),
    ("Mono's runtime", "cat", &["Mono runtime (GC, JIT, icall wrappers)"]),
    (
        "memory (allocators, malloc/free)",
        "any",
        &["MemoryManager::", "malloc", "free_alloc", "DynamicHeapAllocator", "TLSAllocator", "ThreadsafeLinearAllocator", "realloc"],
    ),
    ("job system (scheduling, helping, waiting on a CPU)", "any", &["ujob_", "JobQueue::", "lane_"]),
    ("Physics2D queries (raycasts, casts, overlaps)", "any", &["PhysicsQuery2D", "PhysicsScene2D"]),
];

fn in_sd(st: &str) -> bool {
    "SD".contains(st)
}

fn area_of(nm: &[String]) -> Option<&'static str> {
    AREAS.iter().find(|(_, subs)| subs.iter().any(|t| nm.iter().any(|x| x.contains(t)))).map(|x| x.0)
}

/// UnityMain's samples by area, in ms per UnityMain frame
fn areas_view(o: &mut String, p: &Profile, name: &mut Namer, stacks: &[(i64, &Stack)], frames: usize) {
    let period = if p.hz != 0 { 1000.0 / p.hz as f64 } else { 0.0 };
    let tot: i64 = stacks.iter().map(|x| x.0).sum();
    if tot == 0 || frames == 0 || period == 0.0 {
        return;
    }
    let (mut area, mut across) = (Counter::<String>::new(), Counter::<String>::new());
    for (n, s) in stacks {
        let n = *n;
        if in_sd(&s.state) {
            let nm: Vec<String> = std::iter::once(s.pc).chain(s.rets.iter().copied()).filter(|x| *x != 0).map(|x| name.name(x).1).collect();
            let a = if nm.iter().any(|x| x.contains("WaitForPendingPresent") || x.contains("WaitForLastPresent") || x.contains("PresentFrame")) {
                "wait: the previous frame presented (render thread, GPU)"
            } else if nm.iter().any(|x| x.contains("ujob_") || x.contains("JobQueue::") || x.contains("JobGroup")) {
                "wait: job threads"
            } else {
                "wait: other"
            };
            area.add(a.into(), n);
            continue;
        }
        if s.state != "R" || s.pc == 0 {
            area.add("runnable, not on a CPU (Q)".into(), n);
            continue;
        }
        let named: Vec<(String, String)> = sample_frames(p, s, true).into_iter().filter(|x| *x != 0).map(|x| name.name(x)).collect();
        let nm: Vec<String> = named.iter().map(|x| x.1.clone()).collect();
        let a = match area_of(&nm) {
            Some(a) => a.to_string(),
            None => {
                let nm2: Vec<String> = sample_frames(p, s, false).into_iter().filter(|x| *x != 0).map(|x| name.name(x).1).collect();
                match area_of(&nm2) {
                    Some(a) => format!("{a} (scanned)"),
                    None => "not attributed (the unwind stopped: no player loop frame)".into(),
                }
            }
        };
        area.add(a, n);
        for (label, how, subs) in ACROSS {
            let hit = match *how {
                "self" => {
                    !nm.is_empty()
                        && (subs.iter().any(|t| nm[0].contains(t)) || (nm.len() > 1 && nm[0].contains("syscall") && subs.iter().any(|t| nm[1].contains(t))))
                }
                "cat" => !named.is_empty() && subs.contains(&named[0].0.as_str()),
                _ => subs.iter().any(|t| nm.iter().any(|x| x.contains(t))),
            };
            if hit {
                across.add(label.to_string(), n);
            }
        }
    }
    let ms = |n: i64| n as f64 * period / frames as f64;
    writeln!(o, "  areas, ms a frame ({frames} UnityMain frames, {:.1} ms of samples a frame):", tot as f64 * period / frames as f64).unwrap();
    for (k, v) in area.most_common(None) {
        writeln!(o, "    {:6.2} ms {}  {k}", ms(v), pct(v as f64, tot as f64)).unwrap();
    }
    o.push_str("  across the areas (system calls and managed code: the sample's own function; the rest: anywhere on the stack):\n");
    for (k, v) in across.most_common(None) {
        writeln!(o, "    {:6.2} ms {}  {k}", ms(v), pct(v as f64, tot as f64)).unwrap();
    }
}

#[derive(Default)]
pub struct Args {
    pub profile: String,
    pub syms: Vec<String>,
    pub lib: Vec<String>,
    pub thread: Option<String>,
    pub top: usize,
    pub frames: bool,
    pub slow: Option<f64>,
    pub callers: Option<String>,
    pub within: Option<String>,
    pub offsets: bool,
    pub exact: bool,
    pub areas: bool,
    pub code: bool,
}

pub fn parse_args(args: &[String]) -> Result<Args, String> {
    let usage = "usage: pmu-dev profsum <prof-N.txt> [--syms DIR] [--lib FILE] [--thread NAME] [--top N] [--frames] [--slow MS] [--callers T] [--within T] [--offsets] [--exact] [--areas] [--code]";
    let mut a = Args { top: 15, ..Default::default() };
    let mut pos = Vec::new();
    let mut i = 0;
    while i < args.len() {
        let (k, inline) = match args[i].split_once('=') {
            Some((k, v)) if k.starts_with("--") => (k.to_string(), Some(v.to_string())),
            _ => (args[i].clone(), None),
        };
        let mut val = || -> Result<String, String> {
            if let Some(v) = &inline {
                return Ok(v.clone());
            }
            i += 1;
            args.get(i).cloned().ok_or_else(|| format!("{usage}\n{k}: expected one argument"))
        };
        match k.as_str() {
            "--syms" => a.syms.push(val()?),
            "--lib" => a.lib.push(val()?),
            "--thread" => a.thread = Some(val()?),
            "--top" => a.top = val()?.parse().map_err(|_| format!("{usage}\n--top: not a number"))?,
            "--slow" => a.slow = Some(val()?.parse().map_err(|_| format!("{usage}\n--slow: not a number"))?),
            "--callers" => a.callers = Some(val()?),
            "--within" => a.within = Some(val()?),
            "--frames" => a.frames = true,
            "--offsets" => a.offsets = true,
            "--exact" => a.exact = true,
            "--areas" => a.areas = true,
            "--code" => a.code = true,
            x if x.starts_with('-') && x.len() > 1 => return Err(format!("{usage}\nunrecognized arguments: {x}")),
            _ => pos.push(args[i].clone()),
        }
        i += 1;
    }
    match &pos[..] {
        [p] => a.profile = p.clone(),
        _ => return Err(usage.into()),
    }
    Ok(a)
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let a = parse_args(args)?;
    let text = std::fs::read(&a.profile).map_err(|e| format!("{}: {e}", a.profile))?;
    let p = Profile::parse(&String::from_utf8_lossy(&text));
    let elf = ElfNames::new(&a.syms, &a.lib, true);
    print!("{}", summary(&p, &a, elf));
    Ok(0)
}

/// The whole report (stdout's text).
pub fn summary(p: &Profile, a: &Args, elf: ElfNames) -> String {
    let mut o = String::new();
    let mut name = Namer::new(p, elf, a.offsets);
    let period_ms = if p.hz != 0 { 1000.0 / p.hz as f64 } else { 0.0 };
    let (swaps, mains) = (&p.swap, &p.main);
    let base = std::path::Path::new(&a.profile).file_name().map_or(a.profile.clone(), |b| b.to_string_lossy().into_owned());
    writeln!(o, "{base}: {:.1} s at {} Hz ({}), Mono names: {}", p.secs, p.hz, p.arch, p.meta.get("mono").map_or("?", |s| s.as_str())).unwrap();
    if p.secs != 0.0 {
        let fr = |t: &[i64]| -> String {
            if t.len() < 2 {
                return format!("{} frames", t.len());
            }
            let mut d: Vec<i64> = t.windows(2).map(|w| w[1] - w[0]).collect();
            d.sort();
            let n = d.len();
            format!(
                "{:.1} fps, frame {:.1} ms avg / {:.1} median / {:.1} p99, {} over 25 ms",
                t.len() as f64 / p.secs,
                d.iter().sum::<i64>() as f64 / n as f64 / 1000.0,
                d[n / 2] as f64 / 1000.0,
                d[(n - 1).min((n as f64 * 0.99) as usize)] as f64 / 1000.0,
                d.iter().filter(|x| **x > 25000).count()
            )
        };
        writeln!(o, "  presented: {}", fr(swaps)).unwrap();
        writeln!(o, "  UnityMain: {}", fr(mains)).unwrap();
    }
    let mut order: Vec<i64> = p.threads.keys().copied().collect();
    order.sort_by_key(|i| (p.threads[i].name != "UnityMain", *i));
    let main_idx = order.iter().copied().find(|i| p.threads[i].name == "UnityMain");
    let mut slow_ids: Option<HashMap<i64, Counter<i64>>> = None;
    if let (Some(slow_ms), Some(_)) = (a.slow.filter(|x| *x != 0.0), main_idx) {
        if mains.len() > 1 {
            let slow: std::collections::HashSet<usize> =
                (0..mains.len() - 1).filter(|i| (mains[i + 1] - mains[*i]) as f64 / 1000.0 > slow_ms).collect();
            writeln!(o, "  --slow {}: {} of {} UnityMain frames are longer", fmt_g(slow_ms), slow.len(), mains.len() - 1).unwrap();
            let mut ids = HashMap::new();
            for (t, seq) in &p.seq {
                let mut c = Counter::new();
                for (us, sid) in seq {
                    if let Some(f) = frame_of(mains, *us) {
                        if slow.contains(&f) {
                            c.add(*sid, 1);
                        }
                    }
                }
                ids.insert(*t, c);
            }
            slow_ids = Some(ids);
        }
    }
    let cpu_by_tid: HashMap<i64, &Cpu> = p.cpu.iter().map(|c| (c.tid, c)).collect();
    for ti in &order {
        let th = &p.threads[ti];
        if a.thread.as_ref().is_some_and(|t| *t != th.name) {
            continue;
        }
        let mut stacks: Vec<(i64, &Stack)> = Vec::new();
        for (sid, s) in &p.stacks {
            if s.thread != *ti {
                continue;
            }
            let n = match &slow_ids {
                None => s.count,
                Some(ids) => ids.get(ti).map_or(0, |c| c.get(sid)),
            };
            if n != 0 {
                stacks.push((n, s));
            }
        }
        let mut before = 0;
        if let Some(w) = &a.within {
            before = stacks.iter().map(|x| x.0).sum();
            stacks.retain(|(_, s)| sample_frames(p, s, a.exact).into_iter().any(|x| name.name(x).1.contains(w.as_str())));
        }
        let total: i64 = stacks.iter().map(|x| x.0).sum();
        write!(o, "\n== {} (tid {}): {total} samples", th.name, th.tid).unwrap();
        if th.dropped != 0 {
            write!(o, ", {} dropped (buffer full)", th.dropped).unwrap();
        }
        if let Some(w) = &a.within {
            write!(o, " with {w} on the stack ({} of its {before})", pct(total as f64, before as f64).trim()).unwrap();
        }
        o.push('\n');
        if total == 0 {
            continue;
        }
        if let Some(c) = cpu_by_tid.get(&th.tid).filter(|_| p.secs != 0.0) {
            writeln!(
                o,
                "  schedstat: on a CPU {:.0} ms ({} of the time), waiting for a CPU {:.0} ms ({}), {} slices; user {} ms, kernel {} ms",
                c.run as f64 / 1e6,
                pct(c.run as f64 / 1e9, p.secs),
                c.wait as f64 / 1e6,
                pct(c.wait as f64 / 1e9, p.secs).trim(),
                c.slices,
                c.ut * 10,
                c.st * 10
            )
            .unwrap();
        }
        let mut states = Counter::<String>::new();
        for (n, s) in &stacks {
            let st = if in_sd(&s.state) { format!("{} {}", s.state, sysname(&p.arch, s.sysnr)) } else { s.state.clone() };
            states.add(st, *n);
        }
        let parts: Vec<String> = states.most_common(None).iter().map(|(k, v)| format!("{k} {}", pct(*v as f64, total as f64).trim())).collect();
        writeln!(
            o,
            "  state: {}   (R on a CPU, Q runnable but its sample came late: no CPU or a long kernel path, S/D blocked in <call>)",
            parts.join(", ")
        )
        .unwrap();
        if a.areas && th.name == "UnityMain" {
            areas_view(&mut o, p, &mut name, &stacks, mains.len().saturating_sub(1));
        }
        let run: Vec<(i64, &Stack)> = stacks.iter().filter(|(_, s)| s.state == "R" && s.pc != 0).copied().collect();
        let nrun: i64 = run.iter().map(|x| x.0).sum();
        if nrun != 0 {
            let (mut mods, mut selfc, mut incl, mut managed, mut mself) =
                (Counter::<String>::new(), Counter::<String>::new(), Counter::<String>::new(), Counter::<String>::new(), Counter::<String>::new());
            let (mut unwound, mut udepth) = (0i64, 0i64);
            let mut callers = Counter::<String>::new();
            for (n, s) in &run {
                let n = *n;
                let (cat, f) = name.name(s.pc);
                mods.add(cat.clone(), n);
                selfc.add(f.clone(), n);
                if cat == "managed" {
                    mself.add(f.clone(), n);
                }
                let names: Vec<(String, String)> = sample_frames(p, s, a.exact).into_iter().map(|x| name.name(x)).collect();
                if s.exact != 0 {
                    unwound += n;
                    udepth += n * s.scanned.iter().filter(|x| !**x).count() as i64;
                }
                let mut seen = std::collections::HashSet::new();
                for x in &names {
                    if seen.insert(x.1.clone()) {
                        incl.add(x.1.clone(), n);
                    }
                }
                let inner = names.iter().find(|x| x.0 == "managed").map(|x| x.1.clone());
                managed.add(inner.unwrap_or_else(|| "(no managed frame: engine or runtime alone)".into()), n);
                if let Some(c) = &a.callers {
                    let seq: Vec<&String> = names.iter().map(|x| &x.1).collect();
                    if let Some(i) = seq.iter().position(|x| x.contains(c.as_str())) {
                        let x = seq[i];
                        let nxt: Vec<&str> = seq[i + 1..].iter().filter(|y| **y != x).take(4).map(|y| y.as_str()).collect();
                        callers.add(if nxt.is_empty() { "(none)".into() } else { nxt.join(" < ") }, n);
                    }
                }
            }
            let tail = if unwound != 0 {
                format!(" ({:.1} callers on average), the rest scanned", udepth as f64 / unwound as f64)
            } else {
                ", all scanned".into()
            };
            writeln!(o, "  on a CPU ({nrun} samples, {:.0} ms): {} unwound{tail}; by module:", nrun as f64 * period_ms, pct(unwound as f64, nrun as f64).trim()).unwrap();
            for (k, v) in mods.most_common(None) {
                writeln!(o, "    {}  {k}", pct(v as f64, nrun as f64)).unwrap();
            }
            o.push_str("  self (the pc):\n");
            for (k, v) in selfc.most_common(Some(a.top)) {
                writeln!(o, "    {}  {k}", pct(v as f64, nrun as f64)).unwrap();
            }
            if a.code {
                let mut kinds = Counter::<String>::new();
                let mut lrs: HashMap<String, Counter<(String, String)>> = HashMap::new();
                for (n, s) in &run {
                    if name.name(s.pc).1 != "anon" {
                        continue;
                    }
                    let kd = anon_kind(p, s.pc, &mut name).unwrap_or_else(|| "(no code recorded: UA_PROF_CODE=1)".into());
                    let key = if kd.starts_with("long-branch") { "long-branch thunk".to_string() } else { kd.clone() };
                    kinds.add(key.clone(), *n);
                    let lr = name.name(s.lr).1;
                    lrs.entry(key).or_default().add((kd, lr), *n);
                }
                if !kinds.is_empty() {
                    o.push_str("  anonymous code by kind (--code):\n");
                    for (k, v) in kinds.most_common(None) {
                        writeln!(o, "    {}  {k}", pct(v as f64, nrun as f64)).unwrap();
                        for ((kd, lr), m) in lrs[&k].most_common(Some(4)) {
                            writeln!(o, "           {m:5}  {}  (lr {lr})", if kd != k { kd.as_str() } else { "" }).unwrap();
                        }
                    }
                }
            }
            o.push_str("  on the stack (inclusive):\n");
            for (k, v) in incl.most_common(Some(a.top * 2)) {
                writeln!(o, "    {}  {k}", pct(v as f64, nrun as f64)).unwrap();
            }
            if !mself.is_empty() {
                o.push_str("  managed code itself (pc in JIT code):\n");
                for (k, v) in mself.most_common(Some(a.top)) {
                    writeln!(o, "    {}  {k}", pct(v as f64, nrun as f64)).unwrap();
                }
            }
            if managed.keys().iter().any(|k| !k.starts_with("(no managed")) {
                o.push_str("  by innermost managed method (the method and the engine/runtime calls it makes):\n");
                for (k, v) in managed.most_common(Some(a.top)) {
                    writeln!(o, "    {}  {k}", pct(v as f64, nrun as f64)).unwrap();
                }
            }
            if let Some(c) = &a.callers {
                writeln!(o, "  callers of *{c}*:").unwrap();
                for (k, v) in callers.most_common(Some(a.top)) {
                    writeln!(o, "    {v:5}  {k}").unwrap();
                }
            }
        }
        let blocked: Vec<&(i64, &Stack)> = stacks.iter().filter(|(_, s)| in_sd(&s.state)).collect();
        let nb: i64 = blocked.iter().map(|x| x.0).sum();
        if nb != 0 {
            let mut waits = Counter::<String>::new();
            for (n, s) in blocked {
                let names: Vec<(String, String)> =
                    std::iter::once(s.pc).chain(s.rets.iter().copied()).filter(|x| *x != 0).map(|x| name.name(x)).collect();
                let who = names.iter().find(|x| !x.0.starts_with("libc")).map(|x| x.1.clone()).unwrap_or_else(|| {
                    names.first().map_or_else(|| "(no stack: the call was not readable, e.g. under QEMU)".to_string(), |x| x.1.clone())
                });
                waits.add(format!("{} from {who}", sysname(&p.arch, s.sysnr)), *n);
            }
            writeln!(o, "  blocked ({nb} samples, {:.0} ms): who waits", nb as f64 * period_ms).unwrap();
            for (k, v) in waits.most_common(Some(a.top)) {
                writeln!(o, "    {}  {k}", pct(v as f64, nb as f64)).unwrap();
            }
        }
    }
    if let (true, Some(mi)) = (a.frames, main_idx) {
        if mains.len() > 1 {
            let mut busy = Counter::<usize>::new();
            for (us, sid) in p.seq.get(&mi).map(Vec::as_slice).unwrap_or(&[]) {
                if let (Some(f), Some(s)) = (frame_of(mains, *us), p.stack(*sid)) {
                    if "RQ".contains(s.state.as_str()) {
                        busy.add(f, 1);
                    }
                }
            }
            let mut b: Vec<f64> = (0..mains.len() - 1).map(|i| busy.get(&i) as f64 * period_ms).collect();
            b.sort_by(|x, y| x.partial_cmp(y).unwrap());
            let n = b.len();
            writeln!(
                o,
                "\n== UnityMain per frame ({n} frames): on a CPU (R+Q) {:.1} ms avg, {:.1} median, {:.1} p90, {:.1} max",
                b.iter().fold(0.0f64, |a, b| a + b) / n as f64,
                b[n / 2],
                b[(n as f64 * 0.9) as usize],
                b[n - 1]
            )
            .unwrap();
            let mut hist: BTreeMap<i64, i64> = BTreeMap::new();
            for x in &b {
                *hist.entry((((x / 4.0).floor() as i64) * 4).min(60)).or_default() += 1;
            }
            let mx = hist.values().copied().max().unwrap_or(1);
            for (k, v) in &hist {
                writeln!(o, "    {k:2}-{:<3} ms {v:5} {}", k + 4, "#".repeat((60).min(v * 60 / mx) as usize)).unwrap();
            }
        }
    }
    if !p.cpu.is_empty() && p.secs != 0.0 {
        o.push_str("\n== threads by CPU time over the recording (schedstat):\n");
        let tot: i64 = p.cpu.iter().map(|c| c.run).sum();
        let mut cs: Vec<&Cpu> = p.cpu.iter().collect();
        cs.sort_by_key(|c| -c.run);
        for c in cs.iter().take(12) {
            writeln!(o, "    {:5.1}% of a core  {} (waited for a CPU {:.0} ms)", 100.0 * c.run as f64 / 1e9 / p.secs, c.name, c.wait as f64 / 1e6).unwrap();
        }
        writeln!(o, "    total {:.0}% of a core", 100.0 * tot as f64 / 1e9 / p.secs).unwrap();
    }
    if !p.stat.is_empty() {
        o.push_str("== cores (/proc/stat): busy (user/system), iowait\n");
        for (n, v) in &p.stat {
            if n == "cpu" || v.len() < 7 {
                continue;
            }
            let tot: i64 = v.iter().sum();
            if tot != 0 {
                let t = tot as f64;
                writeln!(
                    o,
                    "    {n}: {} busy ({} user, {} system), iowait {}",
                    pct((tot - v[3] - v[4]) as f64, t),
                    pct((v[0] + v[1]) as f64, t).trim(),
                    pct((v[2] + v[5] + v[6]) as f64, t).trim(),
                    pct(v[4] as f64, t).trim()
                )
                .unwrap();
            }
        }
    }
    o
}

#[cfg(test)]
pub mod tests {
    use super::*;

    // libunity at 0x10000 (bias 0x10000), libc at 0x80000, JIT code at 0x200000; UnityMain 100 samples
    // at 100 Hz over 1 s: 50 in libunity's "Update" called from a managed method, 20 in the managed
    // method itself, 10 in libc memcpy (dladdr name), 10 blocked in futex, 10 Q
    pub const PROFILE: &str = "ua_prof 3
arch armhf
hz 100
secs 1.000
depth 32 scan 16384
signal 39
mono yes
module 10000 20000 10000 libunity.so
module 80000 90000 80000 libc.so.6
sym 80010 memcpy 10
sym 80100 __futex_wait 4
jit 200000 100 M:HeroController:Update
jit 200100 80 W:UnityEngine.Transform:get_position_Injected
thread 0 100 UnityMain 100 0
thread 1 101 UnityGfxDeviceW 10 0
cpu 100 UnityMain 800000000 50000000 400 70 10
cpu 101 UnityGfxDeviceW 200000000 1000000 900 15 5
cpu 102 Job.Worker_0 10000000 0 20 1 0
stat cpu 100 0 20 280 0 0 0
stat cpu0 80 0 10 10 0 0 0
stat cpu1 20 0 10 70 0 0 0
stack 0 50 0 R ffff 11010 200041 1 200045 ~30001
stack 1 20 0 R ffff 200020 0 0 ~30001
stack 2 10 0 R ffff 80010 11011 1 11011 200041
stack 3 10 0 S f0 80100 0 0 ~11801
stack 4 10 0 Q ffff 0 0 0
stack 5 10 1 S f0 80100 0 0 ~12001
seq 0 0:0 10000:0 20000:0 30000:0 40000:0 50000:1 60000:2 70000:3 80000:4 90000:0
swap 0 16000 33000 50000
main 0 10000 100000 200000
end
";

    pub fn names() -> ElfNames {
        let mut e = ElfNames::new(&[], &[], true);
        e.given.insert(
            "libunity.so".into(),
            vec![(0x1000, 0x100, "Behaviour::Update()".into()), (0x1800, 0x100, "PlayerLoop()".into()), (0x2000, 0x10, "GfxDevice::Present()".into())],
        );
        e.given.insert("libc.so.6".into(), vec![]);
        e
    }

    pub fn sum(text: &str, args: &[&str]) -> String {
        let mut v = vec!["prof-1.txt".to_string()];
        v.extend(args.iter().map(|s| s.to_string()));
        let a = parse_args(&v).unwrap();
        summary(&Profile::parse(text), &a, names())
    }

    #[test]
    fn parse() {
        let p = Profile::parse(PROFILE);
        assert_eq!(p.hz, 100);
        assert_eq!(p.stacks.len(), 6);
        assert_eq!(p.seq[&0].len(), 10);
        assert_eq!(p.module(0x11010).unwrap().3, "libunity.so");
        assert_eq!(p.jit(0x200041).unwrap().2, "M:HeroController:Update");
        assert!(p.jit(0x300000).is_none());
    }

    #[test]
    fn views() {
        let out = sum(PROFILE, &["--top", "5"]);
        for x in [
            "== UnityMain (tid 100): 100 samples", "R 80.0%", "S futex 10.0%", "Q 10.0%", "62.5%  Unity engine (libunity)", "25.0%  managed",
            "Behaviour::Update", "libc.so.6:memcpy", "100.0%  M:HeroController:Update", "futex from PlayerLoop", "80.0% of the time",
            "cpu0:  90.0% busy",
        ] {
            assert!(out.contains(x), "{x}\n{out}");
        }
    }

    #[test]
    fn frames_and_slow() {
        assert!(sum(PROFILE, &["--frames", "--thread", "UnityMain"]).contains("UnityMain per frame (3 frames)"));
        let out = sum(PROFILE, &["--slow", "50", "--thread", "UnityMain"]);
        assert!(out.contains("2 of 3 UnityMain frames are longer") && out.contains("9 samples"), "{out}");
    }

    #[test]
    fn callers() {
        assert!(sum(PROFILE, &["--callers", "Update", "--thread", "UnityMain"]).contains("callers of *Update*"));
    }

    #[test]
    fn versions() {
        let v1 = PROFILE.replace("ua_prof 3", "ua_prof 1").replace("11010 200041 1 200045 ~30001", "11010 200041 200045 30001");
        let p = Profile::parse(&v1);
        assert_eq!(p.stacks[0].1.rets, [0x200045, 0x30001]);
        assert_eq!(p.stacks[0].1.exact, 0);
        let v2 = PROFILE.replace("ua_prof 3", "ua_prof 2").replace('~', "");
        let p = Profile::parse(&v2);
        assert_eq!(p.stacks[0].1.scanned, [false, true]);
    }

    #[test]
    fn exact() {
        let out = sum(PROFILE, &["--exact", "--thread", "UnityMain"]);
        assert!(out.contains("75.0% unwound") && !out.contains("anon"), "{out}");
    }

    #[test]
    fn areas() {
        let text = PROFILE.replace("stack 3 10 0 S f0 80100 0 0 ~11801", "stack 3 10 0 S f0 80100 0 0 ~12001 ~11801");
        let mut e = ElfNames::new(&[], &[], true);
        e.given.insert(
            "libunity.so".into(),
            vec![
                (0x1000, 0x100, "void BaseBehaviourManager::CommonUpdate<BehaviourManager>()".into()),
                (0x1800, 0x100, "PlayerLoop()".into()),
                (0x2000, 0x10, "GfxDeviceClient::WaitForPendingPresent()".into()),
            ],
        );
        e.given.insert("libc.so.6".into(), vec![]);
        let a = parse_args(&["prof-1.txt".into(), "--areas".into(), "--thread".into(), "UnityMain".into()]).unwrap();
        let out = summary(&Profile::parse(&text), &a, e);
        for x in [
            "areas, ms a frame (3 UnityMain frames", "200.00 ms  60.0%  scripts: Update", "33.33 ms  10.0%  wait: the previous frame presented",
            "runnable, not on a CPU (Q)", "managed code (JIT)",
        ] {
            assert!(out.contains(x), "{x}\n{out}");
        }
    }

    #[test]
    fn offsets() {
        assert!(sum(PROFILE, &["--offsets", "--thread", "UnityMain"]).contains("M:HeroController:Update+0x20"));
    }

    #[test]
    fn code() {
        let le = |ws: &[u32]| ws.iter().map(|w| w.to_le_bytes().iter().map(|b| format!("{b:02x}")).collect::<String>()).collect::<String>();
        let extra = format!(
            "code 300000 {}\ntramp 3 400000\nstack 6 30 0 R ffff 300004 200041 1 200045\nstack 7 7 0 R ffff 300010 200041 1 200045\nstack 8 5 0 R ffff 400040 200041 1 200045\nstack 9 2 0 R ffff 500000 200041 1 200045\n",
            le(&[0xe59fc000, 0xe12fff1c, 0x11010, 0, 0xe92d0003, 0xe51e0004, 0xe12fff1e])
        );
        let out = sum(&PROFILE.replace("end\n", &(extra + "end\n")), &["--code", "--thread", "UnityMain"]);
        assert!(out.contains("anonymous code by kind (--code):"), "{out}");
        let i = out.find("    ").unwrap_or(0);
        let _ = i;
        assert!(out.lines().collect::<Vec<_>>().windows(2).any(|w| w[0].ends_with("long-branch thunk") && w[1].contains("long-branch thunk to Behaviour::Update (Unity engine")), "{out}");
        for x in ["IMT thunk", "generic trampoline: generic class init", "(no code recorded: UA_PROF_CODE=1)"] {
            assert!(out.contains(x), "{x}\n{out}");
        }
    }
}
