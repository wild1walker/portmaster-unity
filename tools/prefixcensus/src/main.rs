//! prefixcensus: would a "static prefix" frame cache pay? A UA_GL_REC_FILE trace (docs/runtime.md) is
//! read frame by frame; within each render pass (the draws between two binds of a draw framebuffer)
//! every draw, clear and blit is described twice: by the bytes of the calls since the one before
//! (strict), and by what it reads from the followed GL state (its program, that program's uniform
//! values, the textures its samplers reach and their contents' versions, the vertex and index bytes
//! its indices reach, the fixed-function state). Compared with the same pass of the frame before:
//! the number of leading operations that repeat, what ends the repeat, the fragments the repeat
//! stands for (each draw's triangles through the program's unity_MatrixVP * unity_ObjectToWorld,
//! cut to the viewport and scissor: rasterized fragments before depth, stencil and discard), runs
//! that begin at a later exact restart point (a whole clear, an opaque whole-target draw or blit),
//! and what a cache that saves the target after the repeat and restores it would skip.
//!
//!   prefixcensus <trace.gz> [--last N] [--skip-end N] [--timeline] [--ns-frag X] [--copy-ms X]
mod state;
mod trace;
use state::*;
use std::collections::{BTreeMap, HashMap};
use std::rc::Rc;
use trace::*;

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Load,
    Clear,
    Draw,
    Blit,
}
struct Op {
    kind: Kind,
    strict: u64,
    sem: u64,
    weight: f64,
    restart: bool,
    d: Option<Box<DrawInfo>>,
}
struct Pass {
    fbo: u32,
    nth: u32,
    size: (u32, u32),
    msaa: bool,
    ops: Vec<Op>,
}
#[derive(Default)]
struct PassStat {
    name: String,
    size: (u32, u32),
    msaa: bool,
    frames: u64,
    nops: Vec<f64>,
    frags: Vec<f64>,
    pre_strict: Vec<f64>,
    pre_sem: Vec<f64>,
    share_strict: Vec<f64>,
    share_sem: Vec<f64>,
    share_restart: Vec<f64>,
    breaks: HashMap<String, (u64, Vec<f64>, String, Vec<f64>)>,
    strict_breaks: HashMap<String, u64>,
    dw: u64,
    sw: u64,
    predraws: u64,
    // the cache simulated: saves, restores, fragments skipped
    snap: usize,
    prev_pre: usize,
    saves: u64,
    restores: u64,
    skipped: f64,
    whole: u64,
    whole_frags: f64,
}

fn median(v: &[f64]) -> f64 {
    if v.is_empty() {
        return 0.0;
    }
    let mut s = v.to_vec();
    s.sort_by(|a, b| a.partial_cmp(b).unwrap());
    s[s.len() / 2]
}
fn mean(v: &[f64]) -> f64 {
    if v.is_empty() { 0.0 } else { v.iter().sum::<f64>() / v.len() as f64 }
}
fn at_least(v: &[f64], t: f64) -> f64 {
    if v.is_empty() { 0.0 } else { 100.0 * v.iter().filter(|x| **x >= t).count() as f64 / v.len() as f64 }
}
/// the frames spent in runs of consecutive frames at or over t: the mean run a frame is in
fn run_len(v: &[f64], t: f64) -> f64 {
    let (mut sum2, mut sum, mut cur) = (0.0, 0.0, 0.0);
    for x in v.iter().chain(std::iter::once(&-1.0)) {
        if *x >= t {
            cur += 1.0;
        } else {
            sum2 += cur * cur;
            sum += cur;
            cur = 0.0;
        }
    }
    if sum > 0.0 { sum2 / sum } else { 0.0 }
}

fn sem_draw(d: &DrawInfo) -> u64 {
    let mut h = hmix(hmix(d.prog as u64, d.params), d.h_idx);
    h = hmix(h, d.h_attr);
    for (k, v) in d.fixed.iter() {
        h = hmix(hmix(hmix(h, k.0 as u64), k.1 as u64), *v);
    }
    for (l, v) in &d.uni {
        h = hmix(hmix(h, *l as u64), v.hash);
    }
    for t in &d.tex {
        h = hmix(hmix(hmix(h, t.loc as u64), t.version), t.phash);
    }
    h
}

/// what differs between a draw and the one at its place a frame before
fn describe(cur: &Op, prev: Option<&Op>, st: &State, names: &[String]) -> (String, f64) {
    let Some(prev) = prev else { return ("the pass was shorter a frame before".into(), 0.0) };
    if cur.kind != prev.kind {
        return ("another kind of operation (draw, clear, blit)".into(), 0.0);
    }
    let (Some(a), Some(b)) = (&cur.d, &prev.d) else {
        return (match cur.kind {
            Kind::Load => "the target's contents at the pass's start".into(),
            Kind::Clear => "the clear (its values or state)".into(),
            _ => "the blit's source or rectangle".into(),
        }, 0.0);
    };
    if a.prog != b.prog {
        return ("another program (another draw at this place)".into(), 0.0);
    }
    let mut parts: Vec<String> = Vec::new();
    let mut delta = 0.0f64;
    if a.params != b.params {
        parts.push("index count".into());
    }
    if !Rc::ptr_eq(&a.fixed, &b.fixed) {
        let (ma, mb): (BTreeMap<_, _>, BTreeMap<_, _>) = (a.fixed.iter().cloned().collect(), b.fixed.iter().cloned().collect());
        for (k, v) in &ma {
            if mb.get(k) != Some(v) {
                parts.push(format!("state: {}", kind_name(*k, names)));
            }
        }
    }
    let p = st.progs.get(&a.prog);
    let ub: HashMap<i32, &Rc<UVal>> = b.uni.iter().map(|(l, v)| (*l, v)).collect();
    for (l, v) in &a.uni {
        match ub.get(l) {
            Some(o) if o.hash == v.hash => {}
            o => {
                let n = p.map(|p| p.name_of(*l)).unwrap_or_default();
                if let Some(o) = o {
                    if o.data.len() == v.data.len() && names[v.op as usize].contains('f') {
                        for (x, y) in v.data.chunks_exact(4).zip(o.data.chunks_exact(4)) {
                            let (x, y) = (f32::from_le_bytes(x.try_into().unwrap()), f32::from_le_bytes(y.try_into().unwrap()));
                            delta = delta.max(((x - y) as f64).abs());
                        }
                    }
                }
                let mut note = String::new();
                if n == "_Time" {
                    // (Hollow Knight's sway shader: its vertices move with _Time unless both are 0)
                    if let Some(p) = p {
                        for u in ["_SwayAmount", "_PushAmount"] {
                            if let Some(v) = p.names.get(u).and_then(|l| a.uni.iter().find(|x| x.0 == *l)) {
                                note += &format!(" {}{}", if note.is_empty() { "(" } else { "" }, if v.1.data[..4] == [0, 0, 0, 0] { format!("{u} 0") } else { format!("{u} not 0") });
                            }
                        }
                        if !note.is_empty() {
                            note += ")";
                        }
                    }
                }
                parts.push(format!("uniform {}{}", n.trim_start_matches("hlslcc_mtx4x4"), note));
            }
        }
    }
    for (x, y) in a.tex.iter().zip(b.tex.iter()) {
        let n = p.map(|p| p.name_of(x.loc)).unwrap_or_default();
        if x.version != y.version {
            parts.push(format!("contents of {n}'s texture"));
        } else if x.phash != y.phash {
            parts.push(format!("sampling parameters of {n}"));
        }
    }
    if a.params == b.params {
        if a.h_pos != b.h_pos {
            parts.push(if a.bbox == b.bbox { "vertex positions (same screen rectangle)".into() } else { "vertex positions".into() });
        } else if a.h_attr != b.h_attr {
            parts.push("vertex data other than positions (UVs, colours)".into());
        }
        if a.h_idx != b.h_idx {
            parts.push("indices".into());
        }
    }
    if parts.is_empty() {
        parts.push("?".into());
    }
    parts.truncate(4);
    (parts.join(" + "), delta)
}

struct Args {
    path: String,
    last: usize,
    skip_end: usize,
    timeline: bool,
    dump: usize,
    skip_empty: bool,
    src: Vec<u32>,
    ns_frag: f64,
    copy_ms: f64,
}

fn main() {
    let mut a = Args { path: String::new(), last: 600, skip_end: 30, timeline: false, dump: usize::MAX, skip_empty: false, src: Vec::new(), ns_frag: 5.7, copy_ms: 0.3 };
    let mut it = std::env::args().skip(1);
    while let Some(x) = it.next() {
        match x.as_str() {
            "--last" => a.last = it.next().and_then(|v| v.parse().ok()).expect("--last N"),
            "--skip-end" => a.skip_end = it.next().and_then(|v| v.parse().ok()).expect("--skip-end N"),
            "--timeline" => a.timeline = true,
            "--skip-empty" => a.skip_empty = true,
            "--src" => a.src.push(it.next().and_then(|v| v.parse().ok()).expect("--src PROGRAM")),
            "--dump" => a.dump = it.next().and_then(|v| v.parse().ok()).expect("--dump FRAME"),
            "--ns-frag" => a.ns_frag = it.next().and_then(|v| v.parse().ok()).expect("--ns-frag X"),
            "--copy-ms" => a.copy_ms = it.next().and_then(|v| v.parse().ok()).expect("--copy-ms X"),
            _ => a.path = x,
        }
    }
    if a.path.is_empty() {
        eprintln!("usage: prefixcensus <trace.gz> [--last N (600)] [--skip-end N (30)] [--timeline] [--ns-frag ns (5.7)] [--copy-ms ms (0.3)]");
        std::process::exit(2);
    }
    if let Err(e) = run(&a) {
        eprintln!("prefixcensus: {e}");
        std::process::exit(1);
    }
}

fn run(a: &Args) -> Result<(), String> {
    // the frames, counted first (a pass over the file), and the render thread: the one that swaps
    let mut t = Trace::open(&a.path)?;
    let (mut nframes, mut main_thread) = (0usize, 255u8);
    while let Some(r) = t.next() {
        if r.op == TR_FRAME {
            nframes += 1;
            main_thread = r.thread;
        }
    }
    let to = nframes.saturating_sub(a.skip_end);
    let from = to.saturating_sub(a.last + 1);
    if to <= from + 1 {
        return Err(format!("{} frames in the trace: nothing to compare", nframes));
    }
    let mut t = Trace::open(&a.path)?;
    let names = t.names.clone();
    let mut st = State::default();
    let mut frame = 0usize;
    let mut prev: Vec<Pass> = Vec::new();
    let mut cur: Vec<Pass> = Vec::new();
    let mut open: Option<Pass> = None;
    let mut strict = 0u64;
    let mut strict_what: Vec<u32> = Vec::new();
    let mut prev_strict_what: HashMap<(u32, u32, usize), Vec<u32>> = HashMap::new();
    let mut cur_strict_what: HashMap<(u32, u32, usize), Vec<u32>> = HashMap::new();
    let mut stats: BTreeMap<usize, PassStat> = BTreeMap::new();
    let mut unmodelled: HashMap<u32, u64> = HashMap::new();
    let mut how = [0u64; 3];
    let mut empties = 0u64;
    let (mut fr_total, mut fr_pre, mut fr_pre_strict, mut fr_restart): (Vec<f64>, Vec<f64>, Vec<f64>, Vec<f64>) = (vec![], vec![], vec![], vec![]);
    let (mut calls, mut cmp_bytes) = (0u64, 0u64);
    let (mut up0, mut upn0, mut fl0) = (0, 0, 0);

    while let Some(r) = t.next() {
        let inwin = frame >= from && frame < to;
        if r.op == TR_FRAME {
            if inwin {
                if let Some(p) = open.take() {
                    close_pass(p, &mut st, &mut cur);
                }
                if frame == a.dump {
                    for (pi, p) in cur.iter().enumerate() {
                        println!("pass fb {} #{} {}x{}", p.fbo, p.nth + 1, p.size.0, p.size.1);
                        let q = prev.get(pi).filter(|q| q.size == p.size);
                        for (i, o) in p.ops.iter().enumerate() {
                            let same = q.and_then(|q| q.ops.get(i)).map(|x| x.sem == o.sem).unwrap_or(false);
                            let what = if same { String::new() } else { describe(o, q.and_then(|q| q.ops.get(i)), &st, &names).0 };
                            match &o.d {
                                Some(d) => println!("  {i:3} draw {:>6} idx {:>8.0} frags{}{}{}{} {} | {}", d.count, o.weight, if d.blend { " blend" } else { " opaque" }, if d.depth_write { " depth-write" } else { "" }, if d.stencil_write { " stencil-write" } else { "" }, if o.restart { " RESTART" } else { "" }, d.label, if same { "=".into() } else { what }),
                                None => println!("  {i:3} {}{} | {}", match o.kind { Kind::Load => "load", Kind::Clear => "clear", _ => "blit" }, if o.restart { " RESTART" } else { "" }, if same { "=".into() } else { what }),
                            }
                        }
                    }
                }
                // this frame against the one before
                if frame > from {
                    let (mut tot, mut pre, mut pres, mut rest) = (0.0, 0.0, 0.0, 0.0);
                    let mut line = String::new();
                    for (pi, p) in cur.iter().enumerate() {
                        // the same pass a frame before: by its place in the frame (a pooled target
                        // comes back under another framebuffer), of the same size
                        let s = stats.entry(pi).or_default();
                        if s.frames == 0 {
                            s.name = format!("{} (fb {}{})", pi + 1, p.fbo, if p.nth > 0 { format!(", again") } else { String::new() });
                        }
                        let q = prev.get(pi).filter(|q| q.size == p.size);
                        let w: f64 = p.ops.iter().map(|o| o.weight).sum();
                        let eq = |f: fn(&Op) -> u64| -> usize {
                            match q {
                                Some(q) => p.ops.iter().zip(q.ops.iter()).take_while(|(x, y)| f(x) == f(y)).count(),
                                None => 0,
                            }
                        };
                        let (ns, nm) = (eq(|o| o.strict), eq(|o| o.sem));
                        let cw = |n: usize| -> f64 { p.ops[..n].iter().map(|o| o.weight).sum() };
                        // runs from a later restart point: an operation that leaves the target the same
                        // whatever was in it, found in the frame before, and what follows it there
                        let mut best = cw(nm);
                        if let Some(q) = q {
                            for (i, o) in p.ops.iter().enumerate().skip(nm) {
                                if !o.restart {
                                    continue;
                                }
                                if let Some(j) = q.ops.iter().position(|x| x.sem == o.sem && x.restart) {
                                    let k = p.ops[i..].iter().zip(q.ops[j..].iter()).take_while(|(x, y)| x.sem == y.sem).count();
                                    best = best.max(p.ops[i..i + k].iter().map(|o| o.weight).sum());
                                }
                            }
                        }
                        s.size = p.size;
                        s.msaa = p.msaa;
                        s.frames += 1;
                        let real = p.ops.iter().filter(|o| o.kind != Kind::Load).count();
                        let lead = |n: usize| p.ops[..n].iter().filter(|o| o.kind != Kind::Load).count() as f64;
                        s.nops.push(real as f64);
                        s.frags.push(w);
                        s.pre_strict.push(lead(ns));
                        s.pre_sem.push(lead(nm));
                        let sh = |x: f64| if w > 0.0 { x / w } else { 0.0 };
                        s.share_strict.push(sh(cw(ns)));
                        s.share_sem.push(sh(cw(nm)));
                        s.share_restart.push(sh(best));
                        for o in &p.ops[..nm] {
                            if let Some(d) = &o.d {
                                s.predraws += 1;
                                s.dw += d.depth_write as u64;
                                s.sw += d.stencil_write as u64;
                            }
                        }
                        if nm < p.ops.len() {
                            let o = &p.ops[nm];
                            let (what, delta) = describe(o, q.and_then(|q| q.ops.get(nm)), &st, &names);
                            let label = o.d.as_ref().map(|d| d.label.to_string()).unwrap_or_default();
                            let e = s.breaks.entry(what).or_insert((0, vec![], label, vec![]));
                            e.0 += 1;
                            e.1.push(lead(nm));
                            if delta > 0.0 {
                                e.3.push(delta);
                            }
                        }
                        if ns < p.ops.len() && ns <= nm {
                            // the call kinds whose bytes differ in the first operation that differs
                            let k = (pi as u32, 0, ns);
                            let (x, y) = (cur_strict_what.get(&k), prev_strict_what.get(&k));
                            let what = match (x, y) {
                                (Some(x), Some(y)) if x.len() == y.len() => {
                                    let mut d: Vec<&str> = x.chunks(2).zip(y.chunks(2)).filter(|(a, b)| a != b).map(|(a, _)| names[a[0] as usize].as_str()).collect();
                                    d.dedup();
                                    d.truncate(3);
                                    if d.is_empty() { "?".to_string() } else { d.join(", ") }
                                }
                                (Some(_), Some(_)) => "another sequence of calls".to_string(),
                                _ => "no such operation a frame before".to_string(),
                            };
                            *s.strict_breaks.entry(what).or_insert(0) += 1;
                        }
                        // the cache, simulated: a snapshot after the first `snap` operations stays good
                        // while every frame repeats at least that many; a new one is taken where two
                        // frames in a row repeated, if that skips at least half a target of fragments
                        let area = (p.size.0 as f64 * p.size.1 as f64).max(1.0);
                        if nm == p.ops.len() && q.map(|q| q.ops.len() == nm).unwrap_or(false) {
                            // the whole pass repeats: left out with no copy (its target holds its result,
                            // or the passes that read it are left out with it)
                            s.whole += 1;
                            s.whole_frags += w;
                        } else if s.snap > 0 && nm >= s.snap {
                            s.restores += 1;
                            s.skipped += cw(s.snap);
                            let n2 = nm.min(s.prev_pre);
                            if n2 > s.snap && cw(n2) - cw(s.snap) >= 0.5 * area {
                                s.snap = n2;
                                s.saves += 1;
                            }
                        } else {
                            s.snap = 0;
                            let n2 = nm.min(s.prev_pre);
                            if n2 > 0 && cw(n2) >= 0.5 * area {
                                s.snap = n2;
                                s.saves += 1;
                            }
                        }
                        s.prev_pre = nm;
                        tot += w;
                        pre += cw(nm);
                        pres += cw(ns);
                        rest += best;
                        if a.timeline {
                            line += &format!(" {}:{}/{}", pi + 1, lead(nm), real);
                        }
                    }
                    fr_total.push(tot);
                    fr_pre.push(if tot > 0.0 { pre / tot } else { 0.0 });
                    fr_pre_strict.push(if tot > 0.0 { pres / tot } else { 0.0 });
                    fr_restart.push(if tot > 0.0 { rest / tot } else { 0.0 });
                    if a.timeline {
                        println!("frame {frame}: {:.2} M fragments, {:.0} % in repeats;{line}", tot / 1e6, 100.0 * pre / tot.max(1.0));
                    }
                }
                prev = std::mem::take(&mut cur);
                prev_strict_what = std::mem::take(&mut cur_strict_what);
            }
            frame += 1;
            if frame == from {
                up0 = st.tex_uploads;
                upn0 = st.tex_uploads_nopixels;
                fl0 = st.flush_nodata;
            }
            strict = 0;
            strict_what.clear();
            continue;
        }
        let name = names.get(r.op as usize).map(|s| s.as_str()).unwrap_or("?");
        let on_main = r.thread == main_thread;
        // the pass a draw belongs to starts with the framebuffer bound when it is made
        if on_main && inwin {
            calls += 1;
            if !(name.starts_with("glGet") || name.starts_with("glIs") || name.contains("Sync")) {
                let mut h = hmix(strict, r.op as u64);
                let mut c = 0u64;
                for (i, v) in r.a.iter().enumerate() {
                    match r.blob(i) {
                        Some(Blob::Data(d)) => {
                            c = h64(c, d);
                            cmp_bytes += d.len() as u64;
                        }
                        Some(Blob::Zero(n)) => c = hmix(c, *n as u64),
                        None => c = hmix(c, *v),
                    }
                    cmp_bytes += 8;
                }
                h = hmix(h, c);
                strict = h;
                strict_what.push(r.op);
                strict_what.push(c as u32);
            }
        }
        if frame == a.dump && (name.contains("TexImage") || name.contains("TexSubImage") || name.contains("TexStorage") || name == "glInvalidateFramebuffer" || name == "glBindFramebuffer" || name == "glClear") {
            println!("  [{}{} {:?}]", if on_main { "" } else { "other thread: " }, name, r.a.iter().take(9).map(|x| *x as i64).collect::<Vec<_>>());
        }
        let ev = st.apply(&r, name, on_main && inwin);
        if !on_main || !inwin {
            continue;
        }
        let dfbo = st.ctx[&main_thread].dfbo;
        let push = |open: &mut Option<Pass>, st: &State, cur: &Vec<Pass>, mut op: Op, strict: &mut u64, sw: &mut Vec<u32>, csw: &mut HashMap<(u32, u32, usize), Vec<u32>>| {
            if open.is_none() {
                let nth = cur.iter().filter(|p| p.fbo == dfbo).count() as u32;
                let (size, msaa) = match st.target_size(dfbo) {
                    Some((w, h, m)) if w > 0 => ((w, h), m),
                    _ => {
                        let v = st.ctx[&main_thread].get((K_VIEWPORT, 0)).cloned().unwrap_or(vec![0; 4]);
                        ((v[2] as u32, v[3] as u32), false)
                    }
                };
                let mut p = Pass { fbo: dfbo, nth, size, msaa, ops: Vec::new() };
                // a pass that does not begin with a whole clear begins with what its target holds
                if !(op.kind == Kind::Clear && op.restart) {
                    let v = hmix(0x10ad, st.target_version(dfbo));
                    p.ops.push(Op { kind: Kind::Load, strict: v, sem: v, weight: 0.0, restart: true, d: None });
                }
                *open = Some(p);
            }
            let p = open.as_mut().unwrap();
            op.strict = *strict;
            csw.insert((cur.len() as u32, 0, p.ops.len()), std::mem::take(sw));
            *strict = 0;
            p.ops.push(op);
        };
        match ev {
            Event::BindDrawFbo { old } => {
                if let Some(p) = open.take() {
                    debug_assert_eq!(p.fbo, old);
                    close_pass(p, &mut st, &mut cur);
                }
            }
            Event::Draw(d) if a.skip_empty && d.empty => empties += 1,
            Event::Draw(d) => {
                how[d.how as usize] += 1;
                let size = st.target_size(dfbo).map(|s| s.0 as f64 * s.1 as f64).unwrap_or(0.0);
                let w = d.weight.unwrap_or(0.0);
                let restart = d.opaque_state && !d.discards && size > 0.0 && w >= 0.999 * size;
                let op = Op { kind: Kind::Draw, strict: 0, sem: sem_draw(&d), weight: w, restart, d: Some(d) };
                push(&mut open, &st, &cur, op, &mut strict, &mut strict_what, &mut cur_strict_what);
            }
            Event::Clear { mask, extra, full } => {
                let mut h = hmix(hmix(0xc1ea, mask as u64), extra);
                for (k, v) in st.ctx.get_mut(&main_thread).unwrap().snapshot().iter() {
                    h = hmix(hmix(hmix(h, k.0 as u64), k.1 as u64), *v);
                }
                let op = Op { kind: Kind::Clear, strict: 0, sem: h, weight: 0.0, restart: full, d: None };
                push(&mut open, &st, &cur, op, &mut strict, &mut strict_what, &mut cur_strict_what);
            }
            Event::Blit { src, args, area, full_w, full_h } => {
                let size = st.target_size(dfbo);
                let c = &st.ctx[&main_thread];
                let full = !c.cap(SCISSOR_TEST) && size.map(|s| s.0 as i64 == full_w && s.1 as i64 == full_h).unwrap_or(false) && area > 0.0;
                let sc = c.get((K_SCISSOR, 0)).map(|v| v.iter().fold(0, |h, x| hmix(h, *x))).unwrap_or(0);
                let h = hmix(hmix(hmix(0xb117, src), args), if c.cap(SCISSOR_TEST) { sc } else { 0 });
                let op = Op { kind: Kind::Blit, strict: 0, sem: h, weight: area, restart: full, d: None };
                push(&mut open, &st, &cur, op, &mut strict, &mut strict_what, &mut cur_strict_what);
            }
            Event::Unmodelled => *unmodelled.entry(r.op).or_insert(0) += 1,
            Event::None => {}
        }
    }

    for p in &a.src {
        for (i, s) in st.sources(*p).iter().enumerate() {
            println!("---- program {p}, shader {i}\n{s}");
        }
    }
    // the report
    let n = fr_total.len().max(1) as f64;
    println!("== {}: {} frames, the render thread's frames {} - {} compared each with the one before ({} frames)", a.path, nframes, from + 1, to - 1, fr_total.len());
    println!("   {:.0} GL calls a frame; texture uploads in the window: {} ({} without their pixels: counted as changed)", calls as f64 / (n + 1.0), st.tex_uploads - up0, st.tex_uploads_nopixels - upn0);
    println!("   draws' fragments: {} by unity_MatrixVP * unity_ObjectToWorld, {} whole viewport (no position attribute), {} not estimated (counted as 0)", how[0], how[1], how[2]);
    if a.skip_empty {
        println!("   --skip-empty: {:.1} draws a frame left out of the comparison (no triangle of theirs can rasterize: two identical corners, or wholly outside one clip plane)", empties as f64 / (n + 1.0));
    }
    if st.flush_nodata > fl0 {
        println!("   WARNING: {} buffer flushes in the window carry no bytes (a trace written by a ua_host whose trace writer lost maps held across other maps: particle batches read as empty)", st.flush_nodata - fl0);
    }
    if st.msaa_seen > 0 || st.ubo_seen > 0 {
        println!("   multisampled attachments defined: {}; uniform buffer binds: {} (their contents are not followed)", st.msaa_seen, st.ubo_seen);
    }
    println!("   a frame: {:.2} M fragments (median); in repeated leading draws: strict {:.1} %, by what is read {:.1} % (mean); frames with at least 25 / 50 / 75 % of their fragments in them: {:.0} / {:.0} / {:.0} %; with later restart points {:.1} %",
        median(&fr_total) / 1e6, 100.0 * mean(&fr_pre_strict), 100.0 * mean(&fr_pre), at_least(&fr_pre, 0.25), at_least(&fr_pre, 0.5), at_least(&fr_pre, 0.75), 100.0 * mean(&fr_restart));
    println!();
    println!("| pass (its place in the frame, framebuffer) | target | frames | ops | fragments (layers of the target) | share of the frame | strict: leading ops repeated (median), fragments | by what is read: leading ops (median), fragments (mean / median) | frames with >= 25 / 50 / 75 % of the pass's fragments repeated | mean run of frames at >= 25 / 50 / 75 % | with later restart points (mean) | repeated draws writing depth / stencil |");
    println!("|---|---|---|---|---|---|---|---|---|---|---|---|");
    let total_frags: f64 = stats.values().map(|s| s.frags.iter().sum::<f64>()).sum();
    for s in stats.values() {
        let area = (s.size.0 as f64 * s.size.1 as f64).max(1.0);
        println!("| {} | {}x{}{} | {} | {:.0} | {:.2} M ({:.2}) | {:.1} % | {:.0}, {:.0} % | {:.0}, {:.0} / {:.0} % | {:.0} / {:.0} / {:.0} % | {:.1} / {:.1} / {:.1} | {:.0} % | {:.0} / {:.0} % |",
            s.name, s.size.0, s.size.1, if s.msaa { " MSAA" } else { "" }, s.frames, median(&s.nops), median(&s.frags) / 1e6, median(&s.frags) / area,
            100.0 * s.frags.iter().sum::<f64>() / total_frags.max(1.0),
            median(&s.pre_strict), 100.0 * mean(&s.share_strict),
            median(&s.pre_sem), 100.0 * mean(&s.share_sem), 100.0 * median(&s.share_sem),
            at_least(&s.share_sem, 0.25), at_least(&s.share_sem, 0.5), at_least(&s.share_sem, 0.75),
            run_len(&s.share_sem, 0.25), run_len(&s.share_sem, 0.5), run_len(&s.share_sem, 0.75),
            100.0 * mean(&s.share_restart),
            100.0 * s.dw as f64 / s.predraws.max(1) as f64, 100.0 * s.sw as f64 / s.predraws.max(1) as f64);
    }
    println!();
    println!("What ends the repeat (the first operation that differs, by what is read), per pass:");
    for s in stats.values() {
        let mut b: Vec<_> = s.breaks.iter().collect();
        b.sort_by(|x, y| y.1 .0.cmp(&x.1 .0));
        let whole = s.frames - b.iter().map(|x| x.1 .0).sum::<u64>();
        println!("- pass {} ({}x{}): the whole pass repeated in {} of {} frames", s.name, s.size.0, s.size.1, whole, s.frames);
        for (what, (cnt, pos, label, deltas)) in b.iter().take(5) {
            // (a uniform's largest float difference from the frame before: a jitter of a last bit, or a move)
            let d = if deltas.is_empty() { String::new() } else {
                format!(", the uniforms' largest float difference: median {:.1e}, under 1e-4 in {} frames, under 1e-2 in {}", median(deltas), deltas.iter().filter(|x| **x < 1e-4).count(), deltas.iter().filter(|x| **x < 1e-2).count())
            };
            println!("    {} frames: {} [after {:.0} ops (median); {}{}]", cnt, what, median(pos), label, d);
        }
        let mut sb: Vec<_> = s.strict_breaks.iter().collect();
        sb.sort_by(|x, y| y.1.cmp(x.1));
        if let Some((w, c)) = sb.first() {
            println!("    strict, where it ends earlier or at the same place: {} frames: {}{}", c, w, sb.get(1).map(|(w, c)| format!("; {c}: {w}")).unwrap_or_default());
        }
    }
    println!();
    println!("The cache simulated (a snapshot after the repeated operations once two frames in a row repeat them and they are worth half a target of fragments; restored while they still repeat):");
    println!("| pass | whole pass left out (share of frames) | saves a frame | restores a frame | fragments skipped a frame (whole pass + restored prefix) | GPU ms saved a frame at {} ns a fragment | copies' cost at {} ms per 720x450 | net ms a frame |", a.ns_frag, a.copy_ms);
    println!("|---|---|---|---|---|---|---|---|");
    let (mut net_all, mut skip_all) = (0.0, 0.0);
    for s in stats.values() {
        if s.saves == 0 && s.restores == 0 && s.whole == 0 {
            continue;
        }
        let f = s.frames.max(1) as f64;
        let scale = (s.size.0 as f64 * s.size.1 as f64) / (720.0 * 450.0);
        let gain = (s.skipped + s.whole_frags) / f * a.ns_frag * 1e-6;
        let cost = (s.saves + s.restores) as f64 / f * a.copy_ms * scale;
        net_all += gain - cost;
        skip_all += (s.skipped + s.whole_frags) / f;
        println!("| {} | {:.3} | {:.3} | {:.3} | {:.2} M + {:.2} M | {:.2} | {:.2} | {:+.2} |", s.name, s.whole as f64 / f, s.saves as f64 / f, s.restores as f64 / f, s.whole_frags / f / 1e6, s.skipped / f / 1e6, gain, cost, gain - cost);
    }
    println!("   all passes: {:.2} M fragments a frame skipped ({:.1} % of the frame's), net {:+.2} ms a frame; comparing costs the render thread a hash of {:.0} KB of call bytes a frame", skip_all / 1e6, 100.0 * skip_all / median(&fr_total).max(1.0), net_all, cmp_bytes as f64 / (n + 1.0) / 1024.0);
    let mut u: Vec<_> = unmodelled.iter().collect();
    u.sort_by(|x, y| y.1.cmp(x.1));
    println!("   calls not followed (a frame): {}", u.iter().take(14).map(|(o, c)| format!("{} {:.1}", names[**o as usize], **c as f64 / (n + 1.0))).collect::<Vec<_>>().join(", "));
    Ok(())
}

/// a pass ends: its target's contents are what its operations made of what it held
fn close_pass(p: Pass, st: &mut State, cur: &mut Vec<Pass>) {
    let mut h = 0x9a55u64;
    for o in &p.ops {
        h = hmix(h, o.sem);
    }
    st.set_target_version(p.fbo, h);
    cur.push(p);
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn runs_of_frames() {
        // two runs, of 3 and of 1: a frame over the threshold is in a run of (9 + 1) / 4 on average
        assert_eq!(run_len(&[1.0, 1.0, 1.0, 0.0, 1.0], 0.5), 2.5);
        assert_eq!(run_len(&[0.0, 0.0], 0.5), 0.0);
        assert_eq!(at_least(&[0.2, 0.6, 0.9, 0.1], 0.5), 50.0);
        assert_eq!(median(&[3.0, 1.0, 2.0]), 2.0);
    }
    #[test]
    fn hashes_tell_bytes_apart() {
        assert_ne!(h64(0, b"abcdefgh1"), h64(0, b"abcdefgh2"));
        assert_ne!(h64(0, &[0u8; 8]), h64(0, &[0u8; 9]));
        assert_ne!(hmix(1, 2), hmix(2, 1));
    }
}
