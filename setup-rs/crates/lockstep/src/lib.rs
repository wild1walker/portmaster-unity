//! The lockstep checker's files (ua_host's UA_LOCKSTEP, runtime/host/lockstep.c; docs/lockstep.md):
//! a run's hash file (one `F` line a frame, `C` lines for the classes whose scripts changed, `S` when the
//! active scene changed, `I` input events given, `W` waits for Unity's loading threads, `E` at the end)
//! and the dump of one frame (`O <object>` then `  <field path> <value>` lines). [`compare`] finds the
//! first frame two runs part at and what part of the state; [`diff`] names the objects and fields two
//! dumps of that frame differ in.
use std::collections::BTreeMap;
use std::fmt::Write as _;

/// One frame's line: the frame's hash and its parts.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Frame {
    pub all: String,
    pub parts: BTreeMap<String, String>,
}

/// A run's hash file, read.
#[derive(Debug, Default)]
pub struct Run {
    pub header: String,
    pub frames: BTreeMap<u64, Frame>,
    /// (frame, class) -> (count, hash) as written: a class's value holds until its next line
    pub classes: BTreeMap<String, Vec<(u64, u32, String)>>,
    pub scenes: Vec<(u64, String)>,
    pub inputs: BTreeMap<u64, u32>,
    pub waits: BTreeMap<u64, f64>,
    /// `E <frame> <ms hashing> <ms waited>`
    pub end: Option<(u64, f64, f64)>,
}

pub fn parse_run(text: &str) -> Run {
    let mut r = Run::default();
    for line in text.lines() {
        let mut w = line.split_whitespace();
        let (Some(kind), Some(fr)) = (w.next(), w.next()) else { continue };
        if kind == "#" {
            r.header = line.to_string();
            continue;
        }
        let Ok(frame) = fr.parse::<u64>() else { continue };
        match kind {
            "F" => {
                let all = w.next().unwrap_or("").to_string();
                let parts = w.filter_map(|p| p.split_once('=')).map(|(k, v)| (k.to_string(), v.to_string())).collect();
                r.frames.insert(frame, Frame { all, parts });
            }
            "C" => {
                let rest: Vec<&str> = w.collect();
                if rest.len() >= 3 {
                    // a class name has no spaces, but keep any: the last two words are count and hash
                    let n = rest.len();
                    let name = rest[..n - 2].join(" ");
                    let count = rest[n - 2].parse().unwrap_or(0);
                    r.classes.entry(name).or_default().push((frame, count, rest[n - 1].to_string()));
                }
            }
            "S" => r.scenes.push((frame, w.collect::<Vec<_>>().join(" "))),
            "I" => {
                r.inputs.insert(frame, w.next().and_then(|v| v.parse().ok()).unwrap_or(0));
            }
            "W" => {
                r.waits.insert(frame, w.next().and_then(|v| v.parse().ok()).unwrap_or(0.0));
            }
            "E" => {
                let a = w.next().and_then(|v| v.parse().ok()).unwrap_or(0.0);
                let b = w.next().and_then(|v| v.parse().ok()).unwrap_or(0.0);
                r.end = Some((frame, a, b));
            }
            _ => {}
        }
    }
    r
}

impl Run {
    /// a class's (count, hash) in a frame (its last line at or before it)
    pub fn class_at(&self, name: &str, frame: u64) -> (u32, &str) {
        let Some(v) = self.classes.get(name) else { return (0, "") };
        let i = v.partition_point(|e| e.0 <= frame);
        if i == 0 { (0, "") } else { (v[i - 1].1, v[i - 1].2.as_str()) }
    }
    pub fn scene_at(&self, frame: u64) -> &str {
        let i = self.scenes.partition_point(|e| e.0 <= frame);
        if i == 0 { "" } else { self.scenes[i - 1].1.as_str() }
    }
}

/// How two runs compare.
#[derive(Debug, Default)]
pub struct Comparison {
    pub frames: u64,
    pub same: u64,
    pub first: Option<u64>,
    pub report: String,
}

fn ms_a_frame(r: &Run) -> String {
    match r.end {
        Some((f, ms, wait)) if f > 0 => format!("{:.2} ms a frame hashing, {:.0} ms waited for loading", ms / r.frames.len().max(1) as f64, wait),
        _ => "no end line (cut short?)".into(),
    }
}

pub fn compare(a: &Run, b: &Run, max_classes: usize) -> Comparison {
    let mut c = Comparison::default();
    let mut out = String::new();
    if a.header != b.header {
        let _ = writeln!(out, "note: the runs' settings differ:\n  A {}\n  B {}", a.header, b.header);
    }
    let last = a.frames.keys().next_back().copied().unwrap_or(0).max(b.frames.keys().next_back().copied().unwrap_or(0));
    let mut differing = 0u64;
    for (f, fa) in &a.frames {
        let Some(fb) = b.frames.get(f) else { continue };
        c.frames += 1;
        // the frame's hash decides; the parts only logged (unscaledTime, deltaTime: they count from the real start) do not
        if fa.all == fb.all { c.same += 1 } else {
            differing += 1;
            if c.first.is_none() { c.first = Some(*f) }
        }
    }
    let only_a = a.frames.keys().filter(|f| !b.frames.contains_key(f)).count();
    let only_b = b.frames.keys().filter(|f| !a.frames.contains_key(f)).count();
    let _ = writeln!(out, "{} frames in both ({} only in A, {} only in B, last {}): {} the same, {} differ", c.frames, only_a, only_b, last, c.same, differing);
    let _ = writeln!(out, "  A: {}\n  B: {}", ms_a_frame(a), ms_a_frame(b));
    // the scenes: a load that finished at another frame is what parts runs most often
    let sa: Vec<_> = a.scenes.iter().map(|s| s.1.clone()).collect();
    let sb: Vec<_> = b.scenes.iter().map(|s| s.1.clone()).collect();
    for (i, (x, y)) in a.scenes.iter().zip(b.scenes.iter()).enumerate() {
        if x != y {
            let _ = writeln!(out, "scene change {}: A \"{}\" at frame {}, B \"{}\" at frame {}{}", i + 1, x.1, x.0, y.1, y.0,
                if x.1 == y.1 { " (a load finished in another frame)" } else { "" });
            break;
        }
    }
    if sa.len() != sb.len() {
        let _ = writeln!(out, "scene changes: {} in A, {} in B", sa.len(), sb.len());
    }
    if let Some(f) = c.first {
        let (fa, fb) = (&a.frames[&f], &b.frames[&f]);
        let _ = writeln!(out, "FIRST DIFFERENCE: frame {} (scene A \"{}\", B \"{}\")", f, a.scene_at(f), b.scene_at(f));
        let mut keys: Vec<&String> = fa.parts.keys().chain(fb.parts.keys()).collect();
        keys.sort();
        keys.dedup();
        for k in keys {
            let (x, y) = (fa.parts.get(k).map(String::as_str).unwrap_or("-"), fb.parts.get(k).map(String::as_str).unwrap_or("-"));
            if x != y {
                let what = match k.as_str() {
                    "t" => "Time.time",
                    "fc" => "Time.frameCount",
                    "ts" => "Time.timeScale",
                    "ut" => "Time.unscaledTime (logged only: from the real start)",
                    "dt" => "Time.deltaTime (logged only)",
                    "cdt" => "Time.captureDeltaTime (logged only)",
                    "rs" => "UnityEngine.Random.state",
                    "nb" => "scripts (active)",
                    "nt" => "transforms (active)",
                    "tr" => "all transforms",
                    _ => "",
                };
                let _ = writeln!(out, "  {} {}: A {} B {}", k, what, x, y);
            }
        }
        let mut names: Vec<&String> = a.classes.keys().chain(b.classes.keys()).collect();
        names.sort();
        names.dedup();
        let mut n = 0;
        for name in names {
            let (x, y) = (a.class_at(name, f), b.class_at(name, f));
            if x != y {
                n += 1;
                if n <= max_classes {
                    let _ = writeln!(out, "  class {}: A {} scripts {} B {} scripts {}", name, x.0, if x.1.is_empty() { "-" } else { x.1 }, y.0, if y.1.is_empty() { "-" } else { y.1 });
                }
            }
        }
        if n > max_classes {
            let _ = writeln!(out, "  ... {} classes differ", n);
        }
        if n == 0 && fa.parts.get("tr") == fb.parts.get("tr") {
            let _ = writeln!(out, "  (no class differs: the time, Random.state or the counts above)");
        }
        let near: Vec<String> = (f.saturating_sub(5)..=f)
            .filter(|i| a.inputs.get(i) != b.inputs.get(i))
            .map(|i| format!("frame {}: A {} B {}", i, a.inputs.get(&i).unwrap_or(&0), b.inputs.get(&i).unwrap_or(&0)))
            .collect();
        if !near.is_empty() {
            let _ = writeln!(out, "  input events given differ: {}", near.join(", "));
        }
        let _ = writeln!(out, "next: replay both with UA_LOCKSTEP_DUMP={} and `lockstep diff` the two dumps", f);
    } else if c.frames > 0 {
        let _ = writeln!(out, "IDENTICAL: every frame in both runs has the same state hash");
    }
    c.report = out;
    c
}

/// A dump: objects (key -> its lines), in file order. Objects with the same key (two scripts of one class on one
/// GameObject: FindObjectsByType gives them in no set order) are sorted by their lines and numbered "#2", "#3"
pub fn parse_dump(text: &str) -> Vec<(String, Vec<(String, String)>)> {
    let mut order: Vec<String> = Vec::new();
    let mut groups: BTreeMap<String, Vec<Vec<(String, String)>>> = BTreeMap::new();
    let mut cur: Option<String> = None;
    for line in text.lines() {
        if let Some(k) = line.strip_prefix("O ") {
            let g = groups.entry(k.to_string()).or_default();
            if g.is_empty() { order.push(k.to_string()) }
            g.push(Vec::new());
            cur = Some(k.to_string());
        } else if let (Some(rest), Some(k)) = (line.strip_prefix("  "), cur.as_ref()) {
            let (p, v) = rest.split_once(' ').unwrap_or((rest, ""));
            if let Some(o) = groups.get_mut(k).and_then(|g| g.last_mut()) { o.push((p.to_string(), v.to_string())) }
        }
    }
    let mut objs = Vec::new();
    for k in order {
        let mut g = groups.remove(&k).unwrap_or_default();
        g.sort();
        for (i, lines) in g.into_iter().enumerate() {
            objs.push((if i > 0 { format!("{}#{}", k, i + 1) } else { k.clone() }, lines));
        }
    }
    objs
}

/// The objects and fields two dumps of a frame differ in (at most `max` lines)
pub fn diff(a: &str, b: &str, max: usize) -> (usize, String) {
    let (da, db) = (parse_dump(a), parse_dump(b));
    let mb: BTreeMap<&str, &Vec<(String, String)>> = db.iter().map(|o| (o.0.as_str(), &o.1)).collect();
    let ma: BTreeMap<&str, &Vec<(String, String)>> = da.iter().map(|o| (o.0.as_str(), &o.1)).collect();
    let mut out = String::new();
    let mut n = 0usize;
    let mut say = |out: &mut String, s: String| {
        n += 1;
        if n <= max {
            out.push_str(&s);
            out.push('\n');
        }
    };
    for (k, fa) in &da {
        let Some(fb) = mb.get(k.as_str()) else { say(&mut out, format!("only in A: {}", k)); continue };
        // fields by path, in A's order; a path seen twice (a struct printed whole, then its fields) keeps both
        let mut fbm: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
        for (p, v) in fb.iter() { fbm.entry(p.as_str()).or_default().push(v.as_str()) }
        let mut fam: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
        for (p, v) in fa.iter() { fam.entry(p.as_str()).or_default().push(v.as_str()) }
        let mut shown = std::collections::BTreeSet::new();
        for (p, _) in fa.iter() {
            if !shown.insert(p.as_str()) { continue }
            let (x, y) = (&fam[p.as_str()], fbm.get(p.as_str()));
            match y {
                None => say(&mut out, format!("{}  {}: A {} B (none)", k, p, x.join(" | "))),
                Some(y) if y != x => say(&mut out, format!("{}  {}: A {} B {}", k, p, x.join(" | "), y.join(" | "))),
                _ => {}
            }
        }
        for (p, y) in &fbm {
            if !fam.contains_key(p) { say(&mut out, format!("{}  {}: A (none) B {}", k, p, y.join(" | "))) }
        }
    }
    for (k, _) in &db {
        if !ma.contains_key(k.as_str()) { say(&mut out, format!("only in B: {}", k)) }
    }
    if n > max {
        let _ = writeln!(out, "... {} differences in all", n);
    }
    (n, out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const A: &str = "# lockstep 1 fps=60 seed=1 depth=3 elems=32\nS 1 Menu_Title 1\nC 1 Hero 1 aa\nC 1 Cam 1 bb\nF 1 0001 t=3f000000 rs=1 nb=2 nt=5 tr=9\n\
F 2 0002 t=3f010000 rs=1 nb=2 nt=5 tr=9\nI 2 1\nS 3 Tutorial_01 2\nC 3 Hero 1 ac\nF 3 0003 t=3f020000 rs=1 nb=2 nt=5 tr=9\nE 3 3.0 0\n";

    #[test]
    fn same_runs() {
        let (a, b) = (parse_run(A), parse_run(A));
        let c = compare(&a, &b, 10);
        assert_eq!((c.frames, c.same, c.first), (3, 3, None));
        assert!(c.report.contains("IDENTICAL"), "{}", c.report);
        assert_eq!(a.class_at("Hero", 2), (1, "aa"));
        assert_eq!(a.class_at("Hero", 3), (1, "ac"));
        assert_eq!(a.scene_at(2), "Menu_Title 1");
        assert_eq!(a.end, Some((3, 3.0, 0.0)));
    }

    #[test]
    fn logged_parts_do_not_count() {
        let b_text = A.replace("F 2 0002 t=3f010000", "F 2 0002 ut=4199d5e5 t=3f010000");
        let c = compare(&parse_run(A), &parse_run(&b_text), 10);
        assert_eq!(c.first, None, "{}", c.report);
    }

    #[test]
    fn names_the_class_and_frame() {
        let b_text = A.replace("C 3 Hero 1 ac\nF 3 0003 t=3f020000 rs=1", "C 3 Hero 1 ad\nF 3 0004 t=3f020000 rs=2");
        let c = compare(&parse_run(A), &parse_run(&b_text), 10);
        assert_eq!(c.first, Some(3));
        assert!(c.report.contains("FIRST DIFFERENCE: frame 3"), "{}", c.report);
        assert!(c.report.contains("class Hero: A 1 scripts ac B 1 scripts ad"), "{}", c.report);
        assert!(c.report.contains("rs UnityEngine.Random.state: A 1 B 2"), "{}", c.report);
        assert!(!c.report.contains("class Cam"), "{}", c.report);
        assert!(c.report.contains("UA_LOCKSTEP_DUMP=3"), "{}", c.report);
    }

    #[test]
    fn a_late_load() {
        let b_text = A.replace("S 3 Tutorial_01 2\nC 3 Hero 1 ac\nF 3 0003", "C 3 Hero 1 ac\nF 3 0009").replace("E 3", "S 4 Tutorial_01 2\nE 3");
        let c = compare(&parse_run(A), &parse_run(&b_text), 10);
        assert!(c.report.contains("A \"Tutorial_01 2\" at frame 3, B \"Tutorial_01 2\" at frame 4 (a load finished in another frame)"), "{}", c.report);
    }

    #[test]
    fn dump_diff() {
        let a = "# lockstep dump frame 3\nO ~time\n  Time.time 3f000000\nO /Knight[0]|Hero\n  hp 5\n  data.x 4\nO /Thing[0]|Hero\n  hp 1\nO /Thing[0]|Hero\n  hp 2\nO /Gone[0]|X\n";
        let b = "# lockstep dump frame 3\nO ~time\n  Time.time 3f000000\nO /Knight[0]|Hero\n  hp 6\n  data.x 4\n  data.y 1\nO /Thing[0]|Hero\n  hp 1\nO /Thing[0]|Hero\n  hp 2\nO /New[0]|Y\n";
        let (n, out) = diff(a, b, 50);
        assert_eq!(n, 4, "{}", out);
        assert!(out.contains("/Knight[0]|Hero  hp: A 5 B 6"), "{}", out);
        assert!(out.contains("/Knight[0]|Hero  data.y: A (none) B 1"), "{}", out);
        assert!(out.contains("only in A: /Gone[0]|X") && out.contains("only in B: /New[0]|Y"), "{}", out);
        assert_eq!(diff(a, a, 50).0, 0);
        let swapped = a.replace("O /Thing[0]|Hero\n  hp 1\nO /Thing[0]|Hero\n  hp 2\n", "O /Thing[0]|Hero\n  hp 2\nO /Thing[0]|Hero\n  hp 1\n");
        assert_ne!(swapped, a);
        assert_eq!(diff(a, &swapped, 50).0, 0, "{}", diff(a, &swapped, 50).1);
    }
}
