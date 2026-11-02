//! Where a step's wall time goes, for measuring on a handheld: with `PMU_PROF` set in the
//! environment, [`time`] adds the wall time of each call to its stage's total and [`report`] lists
//! the totals (pmu prints them to stderr at the end of a step). Without it each call costs one
//! relaxed load. The stages are timed where the step's main thread runs them: a stage that runs on
//! every core (the encoder) counts the wall time it takes, not its CPU time.
use std::sync::atomic::{AtomicU64, AtomicU8, Ordering};
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
}

const N: usize = 12;
const NAMES: [&str; N] = [
    "open", "read", "prepare", "mips", "strips", "guided", "astc", "objects", "write", "commit",
    "journal", "workdir",
];
static NS: [AtomicU64; N] = [const { AtomicU64::new(0) }; N];
static CALLS: [AtomicU64; N] = [const { AtomicU64::new(0) }; N];
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
    let t = Instant::now();
    let r = f();
    let i = stage as usize;
    NS[i].fetch_add(t.elapsed().as_nanos() as u64, Ordering::Relaxed);
    CALLS[i].fetch_add(1, Ordering::Relaxed);
    r
}

/// A stage timed until the value is dropped.
pub struct Timer(Option<(Stage, Instant)>);

impl Timer {
    pub fn new(stage: Stage) -> Timer {
        Timer(on().then(|| (stage, Instant::now())))
    }
}

impl Drop for Timer {
    fn drop(&mut self) {
        if let Some((stage, t)) = self.0 {
            let i = stage as usize;
            NS[i].fetch_add(t.elapsed().as_nanos() as u64, Ordering::Relaxed);
            CALLS[i].fetch_add(1, Ordering::Relaxed);
        }
    }
}

/// The totals since the last report ("stage 12.3 s (456 calls)", the busiest first), and resets
/// them; empty when not measuring.
pub fn report() -> Vec<String> {
    if !on() {
        return Vec::new();
    }
    let mut v: Vec<(u64, u64, &str)> = (0..N)
        .map(|i| {
            (
                NS[i].swap(0, Ordering::Relaxed),
                CALLS[i].swap(0, Ordering::Relaxed),
                NAMES[i],
            )
        })
        .filter(|x| x.1 > 0)
        .collect();
    v.sort_by(|a, b| b.0.cmp(&a.0));
    v.into_iter()
        .map(|(ns, n, name)| format!("{name} {:.1} s ({n} calls)", ns as f64 / 1e9))
        .collect()
}
