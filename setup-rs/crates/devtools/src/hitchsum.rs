//! `pmu-dev hitchsum <log> [--each] [--top N] [--state NAME] [--callers FUNCTION] [--glprog] [--prof
//! [--thread NAME]]` (was tools/analysis/hitchsum.py): UA_HITCH_TRACE output (runtime/host/hitches.c)
//! summed up.
//!
//! For every frame that was sampled, and for all of them together: the functions the sampled thread was
//! in most often, counted once per sample wherever they appear on the stack ("in", so callers add up) and
//! as the innermost named frame ("top"). libunity.so offsets are named from the release player's
//! libunity.sym.so (crate::syms: UA_LIBUNITY_SYM=<path>); other modules stay module+offset.
//! --each prints the per-frame tables too (default: only frames of 200 ms or more get one line).
//! --state <name>: only frames in that boss state ("loading" for loads); --callers <function>: the
//! frames that called it, most frequent first.
//! --glprog: ua_host's UA_GL_PROGLOG lines instead (no UA_HITCH_TRACE needed): for every "[hitch]" frame
//! (over 100 ms), the shader compiles, program links and program binary loads that ended inside it, and
//! the totals: how much of the long frames was the GL driver making programs. --each lists every such
//! frame (default: those with program work, and the 10 longest).
//! --prof: the UA_PROF_EVERY samples instead ("[prof]" lines, all frames), per boss state: where the
//! sampled thread spends its time (innermost frame) and what is on its stack (inclusive).
//! The game port's own log lines that say where the game is (groups: scene, time, state, end) and when
//! it loads default to Cuphead's helper (portmaster-cuphead); other ports: set UA_STATE_RE / UA_LOADING_RE.
use std::collections::HashSet;
use std::fmt::Write as _;

use regex::Regex;

use crate::syms::UnityNamer;
use crate::util::Counter;

struct Rx {
    sample: Regex,
    prof: Regex,
    done: Regex,
    bench: Regex,
    loading: Regex,
}

fn rx() -> Result<Rx, String> {
    let env = |k: &str, d: &str| std::env::var(k).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| d.to_string());
    let re = |s: &str| Regex::new(s).map_err(|e| format!("{s}: {e}"));
    Ok(Rx {
        sample: re(r"\[hitch-sample\] \+(\d+) (\S+) pc (\S+) lr (\S+) \|(.*)")?,
        prof: re(r"\[prof\] (\S+) pc (\S+) lr (\S+) \|(.*)")?,
        done: re(r"\[hitch-sample\] frame of (\d+) ms, (\d+) samples")?,
        bench: re(&env("UA_STATE_RE", r"\[cupheadport bench\] (\w+?)(?:Level)? t=(\d+) state=(\S+) hp=\S+ x=\S+( WON)?"))?,
        loading: re(&env(
            "UA_LOADING_RE",
            r"\[cupheadport\] (code|font|shader) warmup for |\[cupheadport\] memory trim \(scene|\[cupheadport bench\] next:",
        ))?,
    })
}

/// where the game is after a line, if the line says (the state regex's groups 1, 3, 4)
fn place(r: &Rx, line: &str) -> Option<String> {
    let m = r.bench.captures(line)?;
    Some(if m.get(4).is_some() {
        "loading".into()
    } else {
        format!("{}/{}", m.get(1).map_or("", |x| x.as_str()), m.get(3).map_or("", |x| x.as_str()))
    })
}

fn uniq(st: &[String]) -> Vec<String> {
    let mut seen = HashSet::new();
    st.iter().filter(|x| seen.insert(x.as_str())).cloned().collect()
}

fn prof_main(o: &mut String, text: &str, want: Option<&str>, top_n: usize, thread: Option<&str>) -> Result<(), String> {
    let r = rx()?;
    let name = UnityNamer::new(true);
    let mut per: Vec<(String, (i64, Counter<String>, Counter<String>))> = Vec::new();
    let mut where_ = "loading".to_string();
    for line in text.lines() {
        if r.loading.is_match(line) {
            where_ = "loading".into();
        }
        if let Some(p) = place(&r, line) {
            where_ = p;
            continue;
        }
        let Some(m) = r.prof.captures(line) else { continue };
        if thread.is_some_and(|t| &m[1] != t) {
            continue;
        }
        if want.is_some_and(|w| !where_.contains(w)) {
            continue;
        }
        let st: Vec<String> = std::iter::once(&m[2]).chain(m[4].split_whitespace()).map(|t| name.name(t)).collect();
        let i = match per.iter().position(|x| x.0 == where_) {
            Some(i) => i,
            None => {
                per.push((where_.clone(), (0, Counter::new(), Counter::new())));
                per.len() - 1
            }
        };
        let d = &mut per[i].1;
        d.0 += 1;
        d.1.add(st[0].clone(), 1);
        for x in uniq(&st) {
            d.2.add(x, 1);
        }
    }
    for (w, (n, top, inc)) in &per {
        writeln!(o, "== {w}: {n} samples\n  innermost:").unwrap();
        for (k, c) in top.most_common(Some(top_n)) {
            writeln!(o, "    {:5.1}%  {k}", 100.0 * c as f64 / *n as f64).unwrap();
        }
        o.push_str("  on the stack:\n");
        for (k, c) in inc.most_common(Some(top_n * 2)) {
            writeln!(o, "    {:5.1}%  {k}", 100.0 * c as f64 / *n as f64).unwrap();
        }
    }
    Ok(())
}

fn glprog_main(o: &mut String, path: &str, text: &str, each: bool) {
    let hitch = Regex::new(r"\[hitch\] ([\d.]+) ms at ([\d.]+) s").unwrap();
    let glprog = Regex::new(r"\[glprog\] (compile|link|binary|get-binary) \S+ ([\d.]+) ms at (-?[\d.]+) s").unwrap();
    let (mut hitches, mut calls): (Vec<(f64, f64)>, Vec<(String, f64, f64)>) = (vec![], vec![]);
    let f = |s: &str| s.parse::<f64>().unwrap_or(0.0);
    for line in text.lines() {
        if let Some(m) = hitch.captures(line) {
            hitches.push((f(&m[1]), f(&m[2])));
        }
        if let Some(m) = glprog.captures(line) {
            calls.push((m[1].to_string(), f(&m[2]), f(&m[3])));
        }
    }
    let kinds = ["compile", "link", "binary", "get-binary"];
    let tot = |k: &str| calls.iter().filter(|c| c.0 == k).map(|c| c.1).fold(0.0, |a, b| a + b); // (an empty sum is 0, not -0)
    let cnt = |k: &str| calls.iter().filter(|c| c.0 == k).count();
    let mx = |k: &str| calls.iter().filter(|c| c.0 == k).map(|c| c.1).fold(0.0f64, |a, b| if b > a { b } else { a });
    let parts: Vec<String> = kinds.iter().map(|k| format!("{} {k} {:.0} ms", cnt(k), tot(k))).collect();
    writeln!(o, "{path}: {} (longest link {:.0} ms, binary {:.1} ms)", parts.join(", "), mx("link"), mx("binary")).unwrap();
    let (mut rows, mut hit_ms, mut hit_gl, mut n_gl) = (vec![], 0.0, 0.0, 0);
    for (ms, at) in &hitches {
        let inside: Vec<&(String, f64, f64)> = calls.iter().filter(|(_, _, t)| at - ms / 1000.0 - 1e-3 <= *t && *t <= at + 1e-3).collect();
        let gl: f64 = inside.iter().map(|x| x.1).fold(0.0, |a, b| a + b);
        hit_ms += ms;
        hit_gl += gl;
        n_gl += !inside.is_empty() as i64;
        let mut c = Counter::<String>::new();
        for x in &inside {
            c.add(x.0.clone(), 1);
        }
        rows.push((*ms, *at, gl, c));
    }
    writeln!(
        o,
        "  {} frames over 100 ms, {hit_ms:.0} ms in all; {n_gl} of them made programs: {hit_gl:.0} ms of compiles, links and binary loads ({:.0} %)",
        hitches.len(),
        if hit_ms != 0.0 { 100.0 * hit_gl / hit_ms } else { 0.0 }
    )
    .unwrap();
    let mut idx: Vec<usize> = (0..rows.len()).collect();
    idx.sort_by(|a, b| rows[*b].0.partial_cmp(&rows[*a].0).unwrap_or(std::cmp::Ordering::Equal));
    let longest: HashSet<usize> = idx.into_iter().take(10).collect();
    for (i, (ms, at, gl, c)) in rows.iter().enumerate() {
        if each || *gl != 0.0 || longest.contains(&i) {
            let what: Vec<String> = kinds.iter().filter(|k| c.get(&k.to_string()) != 0).map(|k| format!("{} {k}", c.get(&k.to_string()))).collect();
            let w = if what.is_empty() { String::new() } else { format!(" ({})", what.join(", ")) };
            writeln!(o, "  {ms:8.1} ms at {at:7.1} s: programs {gl:7.1} ms{w}").unwrap();
        }
    }
}

fn opt<'a>(args: &'a [String], k: &str) -> Option<&'a String> {
    args.iter().position(|a| a == k).and_then(|i| args.get(i + 1))
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let pos: Vec<&String> = args.iter().filter(|a| !a.starts_with("--")).collect();
    let Some(path) = pos.first() else {
        return Err("usage: pmu-dev hitchsum <log> [--each] [--top N] [--state NAME] [--callers F] [--glprog] [--prof [--thread NAME]]".into());
    };
    let text = String::from_utf8_lossy(&std::fs::read(path).map_err(|e| format!("{path}: {e}"))?).into_owned();
    let has = |k: &str| args.iter().any(|a| a == k);
    let top = |d: usize| opt(args, "--top").and_then(|v| v.parse().ok()).unwrap_or(d);
    let mut o = String::new();
    if has("--glprog") {
        glprog_main(&mut o, path, &text, has("--each"));
    } else if has("--prof") {
        let thread = opt(args, "--thread").map_or("UnityMain", |s| s.as_str());
        prof_main(&mut o, &text, opt(args, "--state").map(|s| s.as_str()), top(15), Some(thread))?;
    } else {
        hitches(&mut o, &text, has("--each"), top(12), opt(args, "--state").map(|s| s.as_str()), opt(args, "--callers").map(|s| s.as_str()))?;
    }
    print!("{o}");
    Ok(0)
}

fn hitches(o: &mut String, text: &str, each: bool, top_n: usize, want: Option<&str>, callers_of: Option<&str>) -> Result<(), String> {
    let r = rx()?;
    let name = UnityNamer::new(true);
    let mut frames: Vec<(i64, Vec<Vec<String>>, String)> = vec![];
    let mut cur: Vec<Vec<String>> = vec![];
    let mut where_ = "loading".to_string();
    for line in text.lines() {
        if r.loading.is_match(line) {
            where_ = "loading".into();
        }
        if let Some(p) = place(&r, line) {
            where_ = p;
            continue;
        }
        if let Some(m) = r.sample.captures(line) {
            cur.push(std::iter::once(&m[3]).chain(m[5].split_whitespace()).map(|t| name.name(t)).collect());
            continue;
        }
        if let Some(m) = r.done.captures(line) {
            if want.is_none_or(|w| where_.contains(w)) {
                frames.push((m[1].parse().unwrap_or(0), std::mem::take(&mut cur), where_.clone()));
            }
            cur.clear();
        }
    }
    let (mut tot_in, mut tot_top, mut callers) = (Counter::<String>::new(), Counter::<String>::new(), Counter::<String>::new());
    let mut nsamp = 0usize;
    for (ms, samples, wh) in &frames {
        if let Some(c) = callers_of {
            for st in samples {
                if let Some(i) = st.iter().position(|n| n.contains(c)) {
                    callers.add(st[i + 1..(i + 5).min(st.len())].join(" < "), 1);
                }
            }
        }
        let (mut f_in, mut f_top) = (Counter::<String>::new(), Counter::<String>::new());
        for st in samples {
            for x in uniq(st) {
                f_in.add(x, 1);
            }
            f_top.add(st[0].clone(), 1);
        }
        for (k, v) in f_in.items() {
            tot_in.add(k, v);
        }
        for (k, v) in f_top.items() {
            tot_top.add(k, v);
        }
        nsamp += samples.len();
        if each || *ms >= 200 {
            let t: Vec<String> = f_top.most_common(Some(3)).iter().map(|(n, c)| format!("{n} {c}")).collect();
            writeln!(o, "{wh:<24} frame {ms} ms, {} samples: {}", samples.len(), t.join("; ")).unwrap();
            if each {
                for (n, c) in f_in.most_common(Some(top_n)) {
                    writeln!(o, "    in {c:3}  {n}").unwrap();
                }
            }
        }
    }
    if let Some(c) = callers_of {
        writeln!(o, "\ncallers of {c}:").unwrap();
        for (n, k) in callers.most_common(Some(top_n)) {
            writeln!(o, "  {k:4}  {n}").unwrap();
        }
    }
    let ns = nsamp.max(1) as f64;
    writeln!(o, "\n{} frames, {nsamp} samples\ninnermost:", frames.len()).unwrap();
    for (n, c) in tot_top.most_common(Some(top_n)) {
        writeln!(o, "  {:5.1}%  {n}", 100.0 * c as f64 / ns).unwrap();
    }
    o.push_str("on the stack:\n");
    for (n, c) in tot_in.most_common(Some(top_n * 2)) {
        writeln!(o, "  {:5.1}%  {n}", 100.0 * c as f64 / ns).unwrap();
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frames_and_glprog() {
        let log = "[cupheadport bench] FlyingBirdLevel t=3 state=Main hp=3 x=1\n\
[hitch-sample] +1 UnityMain pc libunity.so+0x10 lr libunity.so+0x20 | libc.so+0x30 libunity.so+0x40\n\
[hitch-sample] +2 UnityMain pc libunity.so+0x10 lr libunity.so+0x20 | libunity.so+0x40\n\
[hitch-sample] frame of 250 ms, 2 samples\n";
        let mut o = String::new();
        hitches(&mut o, log, false, 5, None, Some("libc.so")).unwrap();
        assert!(o.contains(&format!("{:<24} frame 250 ms, 2 samples: libunity.so+0x10 2", "FlyingBird/Main")), "{o}");
        assert!(o.contains("1 frames, 2 samples") && o.contains("100.0%  libunity.so+0x10") && o.contains("     1  libunity.so+0x40"), "{o}");
        let gl = "[glprog] link p1 40.0 ms at 1.95 s\n[hitch] 120.0 ms at 2.0 s\n[glprog] compile s1 3.0 ms at 5.0 s\n";
        let mut o = String::new();
        glprog_main(&mut o, "x.log", gl, false);
        assert!(o.starts_with("x.log: 1 compile 3 ms, 1 link 40 ms, 0 binary 0 ms, 0 get-binary 0 ms (longest link 40 ms, binary 0.0 ms)"), "{o}");
        assert!(o.contains("1 frames over 100 ms, 120 ms in all; 1 of them made programs: 40 ms") && o.contains("(1 link)"), "{o}");
    }
}
