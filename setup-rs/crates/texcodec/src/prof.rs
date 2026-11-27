//! Where a step's wall time goes, for measuring on a handheld: with `PMU_PROF` set in the
//! environment, [`time`] adds the wall time of each call to its stage's total and [`report`] lists
//! the totals (pmu prints them to stderr at the end of a step). Without it each call costs one
//! relaxed load. The stages are timed where the step's main thread runs them: a stage that runs on
//! every core (the encoder) counts the wall time it takes, not its CPU time. Where the step's main
//! thread runs a stage, the report also says how many cores the process kept busy meanwhile (its
//! CPU time over that wall time), and the step's whole wall and CPU time.
use std::cell::Cell;
use std::sync::atomic::{AtomicU64, AtomicU8, Ordering};
use std::sync::Mutex;
use std::time::Instant;

/// The stages measured.
#[derive(Clone, Copy, Debug)]
pub enum Stage {
    /// a data file opened (its tables read)
    Open,
    /// a texture's pixels read from its stream or object
    Read,
    /// decoding (and the flip and the first level's downscale: textures.py's _prepare)
    Prepare,
    /// the other mip levels made from the first
    Mips,
    /// strips of a single-level texture decoded and encoded together
    Strips,
    /// guided_astc's pass over a level
    Guided,
    /// astc-encoder on the blocks left to it (gathered, encoded, put in place)
    Astc,
    /// type trees read and written back, other objects' streams copied
    Objects,
    /// a converted file written (serialized file, bundle, stream)
    Write,
    /// a converted file synced and renamed into place
    Commit,
    /// the journal written
    Journal,
    /// the work folder made and removed
    WorkDir,
    /// (CPU time, summed over threads) source rows decoded inside prepare
    DecodeCpu,
    /// (CPU time, summed over threads) a band's rows resampled inside prepare
    ResizeCpu,
    /// the main thread waiting for a worker's texture
    Wait,
    /// a level's rows turned to Unity's order (bottom first) before it is encoded
    Flip,
    /// (CPU time, summed over threads) a band's decoded rows put top first and premultiplied
    BandCpu,
    /// (CPU time, summed over threads) a scaled band put in its place and unpremultiplied
    PasteCpu,
}

const N: usize = 18;
const NAMES: [&str; N] = [
    "open", "read", "prepare", "mips", "strips", "guided", "astc", "objects", "write", "commit",
    "journal", "workdir", "decode-cpu", "resize-cpu", "wait", "flip", "band-cpu", "paste-cpu",
];
static NS: [AtomicU64; N] = [const { AtomicU64::new(0) }; N];
static CALLS: [AtomicU64; N] = [const { AtomicU64::new(0) }; N];
/// the stages' wall time on the main thread, and the process's CPU time meanwhile
static MAIN_NS: [AtomicU64; N] = [const { AtomicU64::new(0) }; N];
static MAIN_CPU: [AtomicU64; N] = [const { AtomicU64::new(0) }; N];
/// when the totals were last reset (wall, the process's CPU time)
static SINCE: Mutex<Option<(Instant, u64)>> = Mutex::new(None);

thread_local! {
    static MAIN: Cell<u8> = const { Cell::new(0) };
}

/// Whether this is the program's main thread.
fn is_main() -> bool {
    MAIN.with(|m| {
        if m.get() == 0 {
            m.set(if std::thread::current().name() == Some("main") { 2 } else { 1 });
        }
        m.get() == 2
    })
}

/// The process's CPU time (every thread's) in ns; 0 where it cannot be read.
pub fn process_cpu_ns() -> u64 {
    #[cfg(target_os = "linux")]
    {
        #[repr(C)]
        struct Timespec {
            sec: i64,
            nsec: i64,
        }
        extern "C" {
            fn clock_gettime(clock: i32, ts: *mut Timespec) -> i32;
        }
        let mut ts = Timespec { sec: 0, nsec: 0 };
        // SAFETY: CLOCK_PROCESS_CPUTIME_ID (2) into a timespec of 64-bit fields (aarch64, x86_64).
        if unsafe { clock_gettime(2, &mut ts) } == 0 {
            return ts.sec as u64 * 1_000_000_000 + ts.nsec as u64;
        }
    }
    0
}

/// A stage started on this thread: its start, and the process's CPU time then on the main thread.
fn start() -> (Instant, Option<u64>) {
    let mut g = SINCE.lock().unwrap();
    if g.is_none() {
        *g = Some((Instant::now(), process_cpu_ns()));
    }
    drop(g);
    (Instant::now(), is_main().then(process_cpu_ns))
}

fn finish(stage: Stage, t: Instant, cpu: Option<u64>) {
    let i = stage as usize;
    let ns = t.elapsed().as_nanos() as u64;
    NS[i].fetch_add(ns, Ordering::Relaxed);
    CALLS[i].fetch_add(1, Ordering::Relaxed);
    if let Some(c0) = cpu {
        MAIN_NS[i].fetch_add(ns, Ordering::Relaxed);
        MAIN_CPU[i].fetch_add(process_cpu_ns().saturating_sub(c0), Ordering::Relaxed);
    }
}
/// 0: not looked at yet, 1: off, 2: on
static STATE: AtomicU8 = AtomicU8::new(0);

/// Whether PMU_PROF is set.
pub fn on() -> bool {
    match STATE.load(Ordering::Relaxed) {
        0 => {
            let on = std::env::var_os("PMU_PROF").is_some_and(|v| !v.is_empty());
            STATE.store(if on { 2 } else { 1 }, Ordering::Relaxed);
            on
        }
        s => s == 2,
    }
}

/// Runs f, its wall time added to the stage's (when measuring).
pub fn time<T>(stage: Stage, f: impl FnOnce() -> T) -> T {
    if !on() {
        return f();
    }
    let (t, cpu) = start();
    let r = f();
    finish(stage, t, cpu);
    r
}

/// A stage timed until the value is dropped.
pub struct Timer(Option<(Stage, Instant, Option<u64>)>);

impl Timer {
    pub fn new(stage: Stage) -> Timer {
        Timer(on().then(|| {
            let (t, cpu) = start();
            (stage, t, cpu)
        }))
    }
}

impl Drop for Timer {
    fn drop(&mut self) {
        if let Some((stage, t, cpu)) = self.0 {
            finish(stage, t, cpu);
        }
    }
}

/// The totals since the last report ("stage 12.3 s (456 calls)", the busiest first; for a stage
/// the main thread ran, "main 10.2 s at 2.4 cores": its wall time there and the cores the process
/// kept busy meanwhile), then the whole: wall time and cores busy since the first stage, and in
/// none of the main thread's stages. Resets them; empty when not measuring.
pub fn report() -> Vec<String> {
    if !on() {
        return Vec::new();
    }
    let since = SINCE.lock().unwrap().take();
    let mut v: Vec<(u64, u64, &str, u64, u64)> = (0..N)
        .map(|i| {
            (
                NS[i].swap(0, Ordering::Relaxed),
                CALLS[i].swap(0, Ordering::Relaxed),
                NAMES[i],
                MAIN_NS[i].swap(0, Ordering::Relaxed),
                MAIN_CPU[i].swap(0, Ordering::Relaxed),
            )
        })
        .filter(|x| x.1 > 0)
        .collect();
    v.sort_by(|a, b| b.0.cmp(&a.0));
    let (mut main_ns, mut main_cpu) = (0u64, 0u64);
    let mut out: Vec<String> = v
        .into_iter()
        .map(|(ns, n, name, mns, mcpu)| {
            let mut l = format!("{name} {:.1} s ({n} calls)", ns as f64 / 1e9);
            if mns > 0 {
                // (a stage nested in another on the main thread, guided and astc in strips,
                // counts in both)
                main_ns += mns;
                main_cpu += mcpu;
                l += &format!(
                    ", main {:.1} s at {:.2} cores",
                    mns as f64 / 1e9,
                    mcpu as f64 / mns as f64
                );
            }
            l
        })
        .collect();
    if let Some((t0, c0)) = since {
        let wall = t0.elapsed().as_nanos() as u64;
        let cpu = process_cpu_ns().saturating_sub(c0);
        out.push(format!(
            "whole {:.1} s, CPU {:.1} s: {:.2} cores; outside the main thread's stages {:.1} s at {:.2} cores",
            wall as f64 / 1e9,
            cpu as f64 / 1e9,
            cpu as f64 / wall.max(1) as f64,
            wall.saturating_sub(main_ns) as f64 / 1e9,
            cpu.saturating_sub(main_cpu) as f64 / wall.saturating_sub(main_ns).max(1) as f64,
        ));
    }
    out
}
