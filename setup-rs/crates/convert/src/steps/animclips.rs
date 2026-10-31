//! Cuphead's plugin step `animclips` (portmaster-cuphead's convert/animclips.py, built in: the
//! setup's `--plugin <path>/animclips.py` or `--plugin animclips`): animclips.txt for the port
//! helper's off-screen animator culling, what every animation clip animates.
//!
//! One line per clip name: <name> TAB <flag> TAB <comma-separated path hashes of the transforms
//! it animates>; flag S (only SpriteRenderer curves, no animation events), H (SpriteRenderer and
//! Transform curves) or - (anything else). A name used by several clips gets the most
//! restrictive flag. Read from m_ClipBindingConstant.genericBindings (typeID 4 = Transform, 212 =
//! SpriteRenderer; path = CRC-32 of the transform's path) and m_Events.
use super::objects;
use crate::error::Result;
use crate::job::Job;
use std::collections::{BTreeMap, BTreeSet};
use unityfs::Value;

pub const STEP: &str = "animclips";
pub const OUT: &str = "animclips.txt";

/// `re.match(r'(sharedassets\d+|resources)\.assets$', name)`: where Cuphead keeps its clips.
fn clip_file(name: &str) -> bool {
    static RX: std::sync::OnceLock<regex::Regex> = std::sync::OnceLock::new();
    RX.get_or_init(|| regex::Regex::new(r"^(?:sharedassets\d+|resources)\.assets\n?\z").unwrap())
        .is_match(name)
}

fn rank(flag: u8) -> u8 {
    match flag {
        b'S' => 0,
        b'H' => 1,
        _ => 2,
    }
}

/// A binding's field as Python's `b.get(name)` compares it (None when missing).
fn get_int(b: &Value, name: &str) -> Option<i128> {
    b.get(name).and_then(Value::as_int)
}

pub fn run(job: &mut Job) -> Result<()> {
    if job.is_done(STEP, OUT) {
        return Ok(());
    }
    let mut clips: BTreeMap<Vec<u8>, (u8, BTreeSet<i128>)> = BTreeMap::new();
    let mut n = 0usize;
    for rel in job.list("", clip_file) {
        let f = job.open(&job.path_in(&rel), Default::default())?;
        for (pid, class) in objects(&f) {
            if class != "AnimationClip" {
                continue;
            }
            let t = f.sf.read_typetree(pid)?;
            let none = Vec::new();
            let binds = t
                .get("m_ClipBindingConstant")
                .and_then(|c| c.get("genericBindings"))
                .and_then(Value::as_array)
                .unwrap_or(&none);
            let types: BTreeSet<Option<i128>> =
                binds.iter().map(|b| get_int(b, "typeID")).collect();
            let events = t
                .get("m_Events")
                .and_then(Value::as_array)
                .map_or(0, Vec::len);
            let only = |ok: &[i128]| types.iter().all(|x| x.is_some_and(|x| ok.contains(&x)));
            let flag = if events == 0 && only(&[212]) {
                b'S'
            } else if only(&[212, 4]) {
                b'H'
            } else {
                b'-'
            };
            let paths = binds
                .iter()
                .filter(|b| get_int(b, "typeID") == Some(4))
                .map(|b| get_int(b, "path").unwrap_or(0));
            let name = t
                .get("m_Name")
                .and_then(Value::as_bytes)
                .map(<[u8]>::to_vec)
                .unwrap_or_default();
            let c = clips.entry(name).or_insert((flag, BTreeSet::new()));
            if rank(flag) > rank(c.0) {
                c.0 = flag;
            }
            c.1.extend(paths);
            n += 1;
        }
    }
    let mut text = Vec::new();
    let mut lines = 0;
    for (name, (flag, paths)) in &clips {
        if name.is_empty() {
            continue;
        }
        if lines > 0 {
            text.push(b'\n');
        }
        text.extend_from_slice(name);
        text.push(b'\t');
        text.push(*flag);
        text.push(b'\t');
        let p: Vec<String> = paths.iter().map(i128::to_string).collect();
        text.extend_from_slice(p.join(",").as_bytes());
        lines += 1;
    }
    text.push(b'\n');
    job.write(OUT, &text)?;
    job.mark_done(STEP, OUT)?;
    let count = |k: u8| clips.values().filter(|(f, _)| *f == k).count();
    job.log(&format!(
        "animclips: {n} clips, {lines} names (S {}, H {}, other {})",
        count(b'S'),
        count(b'H'),
        count(b'-')
    ));
    Ok(())
}
