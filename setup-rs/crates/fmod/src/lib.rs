//! `pmu fmod`: a Unity game's FMOD Studio banks, read once at port setup into the data of
//! ua_host's FMOD player (docs/fmod-player.md): per bank `<Bank>.uafm.json` (its events resolved
//! into what the player plays, its buses, a strings bank's paths, a sound table) and
//! `<Bank>.uafm.pak` (its samples: FMOD's Vorbis rebuilt as standard Ogg Vorbis, with the setup
//! headers of the game's own FMOD library). Bank format 0x67 (FMOD Studio 1.10). The samples are
//! read and written one at a time; a bank's file is mapped, not copied.
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

use serde_json::{json, Map, Value};

pub mod bank;
pub mod fsb;
pub mod rd;
pub mod strings;

use bank::{hex, Bank, BusKind, Guid, InstKind, ModKind, ZERO};
pub use rd::Result;

pub const FORMAT: u32 = 1;

#[derive(Default)]
pub struct Opts {
    /// Decode every Ogg stream written (lewton) and compare its length; resolve every event.
    pub check: bool,
    /// The game's FMOD library (libfmod.so), or a folder with it (default: the `Plugins` folder
    /// nearest above the banks): the Vorbis setup headers are read from it.
    pub fmod_lib: Option<PathBuf>,
}

#[derive(Debug)]
pub enum Error {
    /// A bank of a format this reader does not know (status 3).
    Unsupported(String),
    Fail(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            Error::Unsupported(s) | Error::Fail(s) => f.write_str(s),
        }
    }
}

impl From<String> for Error {
    fn from(s: String) -> Self {
        Error::Fail(s)
    }
}

#[derive(Default, Debug)]
pub struct Summary {
    pub lines: Vec<String>,
    pub problems: Vec<String>,
    pub check_failures: usize,
}

/// The banks of a folder (or one bank file), sorted, strings banks first.
pub fn bank_files(p: &Path) -> Result<Vec<PathBuf>> {
    if p.is_file() {
        return Ok(vec![p.to_path_buf()]);
    }
    let mut v: Vec<PathBuf> = fs::read_dir(p)
        .map_err(|e| format!("{}: {e}", p.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.is_file() && p.extension().is_some_and(|x| x.eq_ignore_ascii_case("bank")))
        .collect();
    v.sort_by_key(|p| (!stem(p).to_ascii_lowercase().ends_with(".strings"), p.clone()));
    if v.is_empty() {
        return Err(format!("{}: no .bank files", p.display()));
    }
    Ok(v)
}

fn stem(p: &Path) -> String {
    p.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default()
}

/// A float as its shortest decimal (an f32 from the bank, without f64 noise).
fn num(x: f32) -> Value {
    let x: f64 = x.to_string().parse().unwrap_or(0.0);
    json!(if x == 0.0 { 0.0 } else { x })
}

fn g(x: &Guid) -> Value {
    Value::String(hex(x))
}

/// The game's FMOD library: the file, or `libfmod.so` in the folder, or in the `Plugins` folder
/// nearest above the banks (any architecture below it).
pub fn find_lib(banks: &Path, given: Option<&Path>) -> Option<PathBuf> {
    let names = ["libfmod.so", "libfmodL.so"];
    let in_dir = |d: &Path| -> Option<PathBuf> {
        for n in names {
            let p = d.join(n);
            if p.is_file() {
                return Some(p);
            }
        }
        let mut subs: Vec<PathBuf> = fs::read_dir(d).ok()?.filter_map(|e| e.ok().map(|e| e.path())).filter(|p| p.is_dir()).collect();
        subs.sort();
        // x86_64 before x86 (both carry the same headers)
        subs.sort_by_key(|p| !p.to_string_lossy().ends_with("x86_64"));
        for s in subs {
            for n in names {
                let p = s.join(n);
                if p.is_file() {
                    return Some(p);
                }
            }
        }
        None
    };
    if let Some(gv) = given {
        return if gv.is_file() { Some(gv.to_path_buf()) } else { in_dir(gv) };
    }
    let mut d = if banks.is_file() { banks.parent()?.to_path_buf() } else { banks.to_path_buf() };
    for _ in 0..4 {
        let p = d.join("Plugins");
        if p.is_dir() {
            if let Some(x) = in_dir(&p) {
                return Some(x);
            }
        }
        d = d.parent()?.to_path_buf();
    }
    None
}

/// Points of a controller: every curve the controller owns (its own may be empty), by x. A
/// timeline's controllers keep x as a timeline position (u32 in the float's bits).
fn points(b: &Bank, ctrl: &Guid, timeline: bool) -> Vec<Value> {
    let mut pts: Vec<(f32, f32, f32)> = Vec::new();
    for c in b.curves.iter().filter(|c| &c.owner == ctrl || &c.id == ctrl) {
        for p in &c.points {
            let x = if timeline { p.x.to_bits() as f32 } else { p.x };
            pts.push((x, p.y, p.shape));
        }
    }
    pts.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));
    pts.dedup_by(|a, b| a.0 == b.0 && a.1 == b.1);
    pts.into_iter().map(|(x, y, s)| json!([num(x), num(y), num(s)])).collect()
}

fn ahdsr(a: &[f32; 10]) -> Value {
    json!(a[..7].iter().map(|x| num(*x)).collect::<Vec<_>>())
}

struct Conv<'a> {
    b: &'a Bank,
    problems: Vec<String>,
}

impl Conv<'_> {
    /// One event resolved: its tracks, instruments (flattened), timeline, sheets, controllers.
    fn event(&mut self, e: &bank::Event, path: &str) -> Value {
        let b = self.b;
        let mut o = Map::new();
        let inb = b.buses.get(&e.input_bus);
        o.insert("bus".into(), g(&inb.map(|x| x.out).unwrap_or(ZERO)));
        if let Some(x) = inb.filter(|x| x.vol != 0.0) {
            o.insert("in_vol".into(), num(x.vol));
        }
        let master = b.buses.get(&e.master);
        if let Some(m) = master {
            if m.vol != 0.0 {
                o.insert("vol".into(), num(m.vol));
            }
            if m.pitch != 0.0 {
                o.insert("pitch".into(), num(m.pitch));
            }
        }
        if e.max_poly > 0 && e.max_poly < i32::MAX {
            o.insert("max".into(), json!(e.max_poly));
            o.insert("steal".into(), json!(e.steal));
        }
        if e.snapshot != ZERO {
            o.insert("snapshot".into(), json!(true));
        }
        // tracks: the group buses routed to this event's master
        let mut tracks: Vec<Guid> = b.buses.values().filter(|x| x.kind == BusKind::Group && x.out == e.master && e.master != ZERO).map(|x| x.id).collect();
        tracks.sort();
        let track_of = |gid: &Guid| -> i64 { tracks.iter().position(|t| t == gid).map(|i| i as i64).unwrap_or(-1) };
        o.insert("tracks".into(), json!(tracks.iter().map(|t| { let x = &b.buses[t]; let mut m = Map::new(); if x.vol != 0.0 { m.insert("vol".into(), num(x.vol)); } Value::Object(m) }).collect::<Vec<_>>()));
        // parameters: the event's sheets, and those its transitions test
        let mut lays: Vec<&bank::Layout> = b.layouts.iter().filter(|l| l.event == e.id || e.layouts.contains(&l.id)).collect();
        lays.sort_by_key(|l| e.layouts.iter().position(|x| x == &l.id).unwrap_or(usize::MAX));
        let mut params: Vec<Guid> = Vec::new();
        for l in &lays {
            if !params.contains(&l.param) {
                params.push(l.param);
            }
        }
        let tl = b.timelines.get(&e.timeline);
        if let Some(t) = tl {
            for tr in &t.transitions {
                for c in &tr.cond {
                    if !params.contains(&c.0) && b.params.contains_key(&c.0) {
                        params.push(c.0);
                    }
                }
            }
        }
        let pidx = |p: &Guid, params: &Vec<Guid>| params.iter().position(|x| x == p).map(|i| i as i64).unwrap_or(-1);
        o.insert(
            "params".into(),
            json!(params
                .iter()
                .filter_map(|p| b.params.get(p))
                .map(|p| {
                    let mut m = Map::new();
                    m.insert("name".into(), json!(p.name));
                    m.insert("id".into(), g(&p.id));
                    m.insert("min".into(), num(p.min));
                    m.insert("max".into(), num(p.max));
                    if p.default != 0.0 {
                        m.insert("def".into(), num(p.default));
                    }
                    if p.seek_up != 0.0 {
                        m.insert("up".into(), num(p.seek_up));
                    }
                    if p.seek_down != 0.0 {
                        m.insert("down".into(), num(p.seek_down));
                    }
                    if p.velocity != 0.0 {
                        m.insert("vel".into(), num(p.velocity));
                    }
                    if p.kind != 0 {
                        m.insert("kind".into(), json!(p.kind));
                    }
                    Value::Object(m)
                })
                .collect::<Vec<_>>()),
        );
        // instruments, flattened (a multi instrument's entries after it)
        let mut insts: Vec<Value> = Vec::new();
        let mut inst_ix: HashMap<Guid, usize> = HashMap::new();
        let add = |gid: &Guid, this: &mut Conv, insts: &mut Vec<Value>, inst_ix: &mut HashMap<Guid, usize>| -> i64 { this.inst(gid, insts, inst_ix, &track_of, 0) };
        let boxes = |list: &[bank::Box_], this: &mut Conv, insts: &mut Vec<Value>, inst_ix: &mut HashMap<Guid, usize>| -> Vec<Value> {
            list.iter().filter_map(|x| { let i = add(&x.inst, this, insts, inst_ix); (i >= 0).then(|| json!([i, x.start, x.len])) }).collect()
        };
        let mut len = 0u32;
        if let Some(t) = tl {
            let a = boxes(&t.async_boxes, self, &mut insts, &mut inst_ix);
            let l = boxes(&t.locked, self, &mut insts, &mut inst_ix);
            for x in t.async_boxes.iter().chain(&t.locked) {
                len = len.max(x.start.saturating_add(x.len));
            }
            if !a.is_empty() {
                o.insert("async".into(), json!(a));
            }
            if !l.is_empty() {
                o.insert("locked".into(), json!(l));
            }
            let mut mk: Vec<&bank::Marker> = t.markers.iter().collect();
            mk.sort_by_key(|m| m.pos);
            if !mk.is_empty() {
                o.insert("markers".into(), json!(mk.iter().map(|m| json!([m.pos, m.name])).collect::<Vec<_>>()));
            }
            if !t.tempo.is_empty() {
                let mut tp: Vec<&bank::Tempo> = t.tempo.iter().collect();
                tp.sort_by_key(|x| x.pos);
                o.insert("tempo".into(), json!(tp.iter().map(|x| json!([x.pos, num(x.bpm), x.upper, x.lower])).collect::<Vec<_>>()));
            }
            let mut jumps = Vec::new();
            for tr in &t.transitions {
                let Some(dest) = t.markers.iter().find(|m| m.id == tr.dest) else {
                    self.problems.push(format!("{path}: a transition to a destination that is not a marker of its timeline"));
                    continue;
                };
                let mut m = Map::new();
                m.insert("at".into(), json!(tr.start));
                if tr.end != tr.start {
                    m.insert("end".into(), json!(tr.end));
                }
                m.insert("to".into(), json!(dest.pos));
                let cond: Vec<Value> = tr.cond.iter().map(|c| json!([pidx(&c.0, &params), num(c.1), num(c.2)])).collect();
                if !cond.is_empty() {
                    m.insert("if".into(), json!(cond));
                }
                if tr.chance < 100.0 {
                    m.insert("chance".into(), num(tr.chance));
                }
                if tr.quant.0 != 0 && tr.quant.0 <= 2 {
                    m.insert("quant".into(), json!([tr.quant.0, tr.quant.1]));
                }
                jumps.push(Value::Object(m));
            }
            if !jumps.is_empty() {
                o.insert("jumps".into(), json!(jumps));
            }
        }
        // a sheet's box: an instrument on the parameter's axis (start, length); an instrument that is
        // also on the timeline takes it as a condition instead (format 0x67: the timeline triggers
        // it only while the parameter is in [start, start + length])
        let on_timeline: HashSet<Guid> = tl.map(|t| t.async_boxes.iter().chain(&t.locked).map(|x| x.inst).collect()).unwrap_or_default();
        let mut sheets = Vec::new();
        let mut conds: HashMap<usize, Vec<Value>> = HashMap::new();
        for l in &lays {
            let mut bx = Vec::new();
            for x in &l.boxes {
                let i = add(&x.0, self, &mut insts, &mut inst_ix);
                if i < 0 {
                    continue;
                }
                if on_timeline.contains(&x.0) {
                    conds.entry(i as usize).or_default().push(json!([pidx(&l.param, &params), num(x.1), num(x.1 + x.2)]));
                } else {
                    bx.push(json!([i, num(x.1), num(x.2)]));
                }
            }
            if !bx.is_empty() {
                sheets.push(json!([pidx(&l.param, &params), bx]));
            }
        }
        for (i, c) in conds {
            if let Some(Value::Object(m)) = insts.get_mut(i) {
                m.insert("if".into(), json!(c));
            }
        }
        if !sheets.is_empty() {
            o.insert("sheets".into(), json!(sheets));
        }
        o.insert("len".into(), json!(len));
        // controllers: the timeline's and each sheet's
        let mut ctrls = Vec::new();
        let mut sources: Vec<(&Vec<Guid>, i64)> = Vec::new();
        if let Some(t) = tl {
            sources.push((&t.controllers, -1));
        }
        for l in &lays {
            sources.push((&l.controllers, pidx(&l.param, &params)));
        }
        for (list, by) in sources {
            for c in list {
                let Some(c) = b.controllers.get(c) else { continue };
                let (on, i) = if c.owner == e.master {
                    ("master", -1)
                } else if let Some(t) = tracks.iter().position(|t| t == &c.owner) {
                    ("track", t as i64)
                } else if let Some(&ix) = inst_ix.get(&c.owner) {
                    ("inst", ix as i64)
                } else if let Some(fx) = b.effects.get(&c.owner) {
                    if fx.dsp != 27 || c.prop != 1001 {
                        continue; // (effects other than the panner's position: not played)
                    }
                    let holder = tracks.iter().position(|t| b.buses[t].pre.contains(&fx.id) || b.buses[t].post.contains(&fx.id)).map(|x| x as i64).unwrap_or(-1);
                    ("pan", holder)
                } else {
                    continue;
                };
                let prop = match (on, c.prop) {
                    ("pan", _) => "pos",
                    (_, 0) => "vol",
                    (_, 1) => "pitch",
                    ("inst", 4) => "fade",
                    _ => continue,
                };
                let pts = points(b, &c.id, by < 0);
                if pts.is_empty() {
                    continue;
                }
                ctrls.push(json!({"on": on, "i": i, "prop": prop, "by": by, "pts": pts}));
            }
        }
        if !ctrls.is_empty() {
            o.insert("ctrl".into(), json!(ctrls));
        }
        for m in b.modulators.iter().filter(|m| m.owner == e.master && m.prop == 0) {
            if let ModKind::Ahdsr(a) = &m.kind {
                o.insert("ahdsr".into(), ahdsr(a));
            }
        }
        if !insts.is_empty() {
            o.insert("insts".into(), json!(insts));
        }
        Value::Object(o)
    }

    /// An instrument (and a multi instrument's entries) into `insts`; its index, or -1.
    fn inst(&mut self, gid: &Guid, insts: &mut Vec<Value>, ix: &mut HashMap<Guid, usize>, track_of: &dyn Fn(&Guid) -> i64, depth: usize) -> i64 {
        if let Some(&i) = ix.get(gid) {
            return i as i64;
        }
        let b = self.b;
        let Some(it) = b.insts.get(gid) else {
            return -1;
        };
        if depth > 8 {
            return -1;
        }
        let me = insts.len();
        insts.push(Value::Null);
        ix.insert(*gid, me);
        let mut o = Map::new();
        match &it.kind {
            InstKind::Wave { wav } => {
                o.insert("k".into(), json!("wave"));
                match b.wavs.get(wav) {
                    Some(w) => {
                        o.insert("m".into(), json!(w.subsound));
                        if w.sound_bank != 0 {
                            o.insert("sb".into(), json!(w.sound_bank));
                        }
                    }
                    None => self.problems.push(format!("instrument {}: its waveform is not in this bank", hex(gid))),
                }
            }
            InstKind::Multi => {
                o.insert("k".into(), json!("multi"));
                if let Some((play, sel, entries)) = &it.playlist {
                    let mut kids = Vec::new();
                    let mut w = Vec::new();
                    for (eg, wt) in entries {
                        let k = self.inst(eg, insts, ix, track_of, depth + 1);
                        if k >= 0 {
                            kids.push(json!(k));
                            w.push(num(*wt));
                        }
                    }
                    o.insert("kids".into(), json!(kids));
                    if w.iter().any(|x| x.as_f64() != w[0].as_f64()) {
                        o.insert("w".into(), json!(w));
                    }
                    o.insert("mode".into(), json!(play));
                    if *sel != 1 {
                        o.insert("sel".into(), json!(sel));
                    }
                }
            }
            InstKind::Programmer { name } => {
                o.insert("k".into(), json!("prog"));
                o.insert("name".into(), json!(name));
            }
            InstKind::Event { event } => {
                o.insert("k".into(), json!("event"));
                o.insert("event".into(), g(event));
            }
            InstKind::Scatterer => {
                o.insert("k".into(), json!("scatter"));
            }
            InstKind::Other(k) => {
                o.insert("k".into(), json!(k));
            }
        }
        if let Some(x) = &it.body {
            for (k, v, d) in [("vol", x.vol, 0.0), ("pitch", x.pitch, 0.0), ("chance", x.chance, 100.0), ("seek", x.seek, 0.0)] {
                if v != d {
                    o.insert(k.into(), num(v));
                }
            }
            if x.loop_count != 0 {
                o.insert("loop".into(), json!(x.loop_count));
            }
            if x.delay != (0.0, 0.0) {
                o.insert("delay".into(), json!([num(x.delay.0), num(x.delay.1)]));
            }
            if x.max_poly > 0 && x.max_poly < i32::MAX {
                o.insert("max".into(), json!(x.max_poly));
            }
            if x.flags != 0 {
                o.insert("flags".into(), json!(x.flags));
            }
            if x.seek_pct != 0.0 {
                o.insert("seek_pct".into(), num(x.seek_pct));
            }
            let t = track_of(&x.out);
            if t >= 0 {
                o.insert("track".into(), json!(t));
            }
        }
        for m in b.modulators.iter().filter(|m| &m.owner == gid) {
            match (&m.kind, m.prop) {
                (ModKind::Random(a), 0) => {
                    o.insert("rvol".into(), num(*a));
                }
                (ModKind::Random(a), 1) => {
                    o.insert("rpitch".into(), num(*a));
                }
                (ModKind::Random(a), 11) => {
                    o.insert("rstart".into(), num(*a));
                }
                (ModKind::Ahdsr(a), 0) => {
                    o.insert("ahdsr".into(), ahdsr(a));
                }
                _ => {}
            }
        }
        insts[me] = Value::Object(o);
        me as i64
    }
}

/// Converts the banks of `banks` (a folder or a .bank) into `out`.
pub fn run(banks: &Path, out: &Path, opts: &Opts, log: &mut dyn FnMut(&str)) -> std::result::Result<Summary, Error> {
    let files = bank_files(banks)?;
    fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    let mut sum = Summary::default();
    let lib_path = find_lib(banks, opts.fmod_lib.as_deref());
    let lib = lib_path.as_ref().and_then(|p| fs::read(p).ok());
    match &lib_path {
        Some(p) => log(&format!("FMOD library (for its Vorbis setup headers): {}", p.display())),
        None => sum.problems.push("the game's FMOD library (libfmod.so) was not found: the Vorbis samples are left out (silent)".into()),
    }
    let mut setups: HashMap<u32, fsb::Setup> = HashMap::new();
    let mut unsupported = Vec::new();
    for f in &files {
        let name = stem(f);
        let d = fs::read(f).map_err(|e| format!("{}: {e}", f.display()))?;
        let ver = match bank::version(&d) {
            Ok(v) => v,
            Err(e) => {
                sum.problems.push(format!("{name}: {e}"));
                continue;
            }
        };
        if !bank::VERSIONS.contains(&ver) {
            unsupported.push(format!("{name}: bank format {ver:#x}"));
            continue;
        }
        let b = bank::parse(&d).map_err(|e| format!("{name}: {e}"))?;
        for p in &b.problems {
            sum.problems.push(format!("{name}: {p}"));
        }
        // (the samples go to <name>.uafm.pak as they are made: the bank and all of its Ogg samples together were
        // Blasphemous's peak, 2 x 159 MB for BackgroundLayer)
        let pp = out.join(format!("{name}.uafm.pak"));
        let (j, lines) = convert_bank(&name, &d, &b, lib.as_deref(), &mut setups, opts.check, &mut sum, &pp)?;
        let jp = out.join(format!("{name}.uafm.json"));
        fs::write(&jp, serde_json::to_vec(&j).unwrap()).map_err(|e| format!("{}: {e}", jp.display()))?;
        for l in lines {
            log(&l);
            sum.lines.push(l);
        }
    }
    if !unsupported.is_empty() && sum.lines.is_empty() {
        return Err(Error::Unsupported(format!("banks of a format this converter does not read ({}); the port stays silent", unsupported.join(", "))));
    }
    for u in unsupported {
        sum.problems.push(format!("{u}: not read (the converter reads 0x67)"));
    }
    Ok(sum)
}

type Converted = (Value, Vec<String>);

/// The pak a bank's samples are written to as they are made (made with its sound data's first sample block).
struct Pak<'a> {
    path: &'a Path,
    w: BufWriter<File>,
    len: usize,
}

impl Pak<'_> {
    fn put(&mut self, b: &[u8]) -> Result<()> {
        self.w.write_all(b).map_err(|e| format!("{}: {e}", self.path.display()))?;
        self.len += b.len();
        Ok(())
    }
}

#[allow(clippy::too_many_arguments)]
fn convert_bank(name: &str, d: &[u8], b: &Bank, lib: Option<&[u8]>, setups: &mut HashMap<u32, fsb::Setup>, check: bool, sum: &mut Summary, pak_path: &Path) -> Result<Converted> {
    let mut j = Map::new();
    j.insert("format".into(), json!(FORMAT));
    j.insert("bank".into(), json!({"name": name, "id": hex(&b.id), "version": b.version}));
    let mut lines = Vec::new();
    let paths: HashMap<Guid, &str> = b.strings.iter().flatten().map(|(p, gd)| (*gd, p.as_str())).collect();
    if let Some(s) = &b.strings {
        let m: BTreeMap<&str, String> = s.iter().map(|(p, gd)| (p.as_str(), hex(gd))).collect();
        j.insert("paths".into(), json!(m));
        lines.push(format!("{name}: {} paths", s.len()));
    }
    // buses outside events
    let masters: HashSet<Guid> = b.events.values().map(|e| e.master).collect();
    let mut internal: HashSet<Guid> = b.events.values().flat_map(|e| [e.master, e.input_bus]).collect();
    for x in b.buses.values() {
        if x.kind == BusKind::Group && masters.contains(&x.out) {
            internal.insert(x.id);
        }
    }
    let mut buses = BTreeMap::new();
    for x in b.buses.values().filter(|x| !internal.contains(&x.id)) {
        let mut m = Map::new();
        m.insert("kind".into(), json!(match x.kind { BusKind::Group => "group", BusKind::Master => "master", BusKind::Return => "return", BusKind::Input => "input" }));
        if x.out != ZERO {
            m.insert("out".into(), g(&x.out));
        }
        if x.vol != 0.0 {
            m.insert("vol".into(), num(x.vol));
        }
        buses.insert(hex(&x.id), Value::Object(m));
    }
    if !buses.is_empty() {
        j.insert("buses".into(), json!(buses));
    }
    let mut conv = Conv { b, problems: Vec::new() };
    let mut evs = BTreeMap::new();
    let mut ids: Vec<&Guid> = b.events.keys().collect();
    ids.sort();
    for id in ids {
        let e = &b.events[id];
        let p = paths.get(id).copied().unwrap_or("");
        evs.insert(hex(id), conv.event(e, if p.is_empty() { "an event" } else { p }));
    }
    let nev = evs.len();
    if !evs.is_empty() {
        j.insert("events".into(), json!(evs));
    }
    sum.problems.extend(conv.problems.drain(..).map(|p| format!("{name}: {p}")));
    if let Some(t) = &b.sound_table {
        let m: BTreeMap<String, u32> = t.keys.iter().zip(&t.index).map(|(k, i)| (format!("{k:016x}"), *i)).collect();
        j.insert("sounds".into(), json!(m));
    }
    // the samples
    let mut media = Vec::new();
    let (mut nv, mut nleft, mut total) = (0usize, 0usize, 0usize);
    for (k, &(at, len)) in b.fsb.iter().enumerate() {
        let f = fsb::read(d, at, len).map_err(|e| format!("sound data {k}: {e}"))?;
        if f.codec != fsb::CODEC_VORBIS {
            sum.problems.push(format!("{name}: sound data {k}: codec {} (only Vorbis is read): left out", f.codec));
            continue;
        }
        if k > 0 {
            sum.problems.push(format!("{name}: a second sound data block: left out"));
            break;
        }
        let need: Vec<(u32, u32)> = f.samples.iter().filter_map(|s| s.setup_id.map(|i| (i, s.channels))).collect();
        if let Some(l) = lib {
            let todo: Vec<(u32, u32)> = need.iter().copied().filter(|(i, c)| !setups.get(i).is_some_and(|s| s.parsed.contains_key(c))).collect::<HashSet<_>>().into_iter().collect();
            if !todo.is_empty() {
                let (found, missing) = fsb::setup_headers(l, &todo);
                for (i, s) in found {
                    match setups.get_mut(&i) {
                        Some(e) => e.parsed.extend(s.parsed),
                        None => {
                            lines.push(format!("{name}: Vorbis setup header {i:#010x} ({} bytes) read from the FMOD library", s.bytes.len()));
                            setups.insert(i, s);
                        }
                    }
                }
                for i in missing {
                    sum.problems.push(format!("{name}: Vorbis setup header {i:#010x} is not in the FMOD library: its samples are left out"));
                }
            }
        }
        let mut out = Pak { path: pak_path, w: BufWriter::new(File::create(pak_path).map_err(|e| format!("{}: {e}", pak_path.display()))?), len: 0 };
        for (i, s) in f.samples.iter().enumerate() {
            let setup = s.setup_id.and_then(|id| setups.get(&id));
            let ogg = match setup {
                Some(st) => fsb::to_ogg(d, s, st, i as u32 + 1),
                None => Err("no setup header".to_string()),
            };
            match ogg {
                Ok(o) => {
                    if out.len % 16 != 0 {
                        out.put(&[0u8; 16][..16 - out.len % 16])?;
                    }
                    if check {
                        if let Err(e) = check_ogg(&o, s.samples) {
                            sum.problems.push(format!("{name}: sample {i} ({}): {e}", s.name));
                            sum.check_failures += 1;
                        }
                    }
                    media.push(json!({"off": out.len, "len": o.len(), "ch": s.channels, "rate": s.rate, "samples": s.samples, "name": s.name}));
                    out.put(&o)?;
                    nv += 1;
                }
                Err(e) => {
                    if lib.is_some() {
                        sum.problems.push(format!("{name}: sample {i} ({}): {e}: left out", s.name));
                    }
                    media.push(json!({"samples": s.samples, "ch": s.channels, "rate": s.rate, "name": s.name}));
                    nleft += 1;
                }
            }
        }
        total = out.len;
        out.w.flush().map_err(|e| format!("{}: {e}", pak_path.display()))?;
    }
    if !media.is_empty() {
        j.insert("media".into(), json!(media));
    }
    lines.push(format!("{name}: {nev} events, {} buses, {nv} samples as Ogg Vorbis ({:.1} MB){}", j.get("buses").and_then(|x| x.as_object()).map(|x| x.len()).unwrap_or(0), total as f64 / 1e6, if nleft > 0 { format!(", {nleft} left out") } else { String::new() }));
    if check {
        let n = check_events(&j);
        if !n.is_empty() {
            sum.check_failures += n.len();
            sum.problems.extend(n.into_iter().map(|p| format!("{name}: check: {p}")));
        }
    }
    Ok((Value::Object(j), lines))
}

/// Decodes a stream (lewton) and compares its length with the sample count.
fn check_ogg(o: &[u8], samples: u32) -> Result<()> {
    let mut r = lewton::inside_ogg::OggStreamReader::new(std::io::Cursor::new(o)).map_err(|e| format!("{e:?}"))?;
    let mut n = 0u64;
    while let Some(p) = r.read_dec_packet_itl().map_err(|e| format!("{e:?}"))? {
        n += (p.len() / r.ident_hdr.audio_channels.max(1) as usize) as u64;
    }
    if n != samples as u64 {
        return Err(format!("decodes to {n} samples, the header says {samples}"));
    }
    Ok(())
}

/// Every event's instruments name a sample of the bank (or a programmer sound), every index in
/// range.
fn check_events(j: &Map<String, Value>) -> Vec<String> {
    let mut bad = Vec::new();
    let nm = j.get("media").and_then(|m| m.as_array()).map(|m| m.len()).unwrap_or(0);
    let Some(evs) = j.get("events").and_then(|e| e.as_object()) else { return bad };
    for (id, e) in evs {
        let insts = e.get("insts").and_then(|x| x.as_array()).cloned().unwrap_or_default();
        let ntr = e.get("tracks").and_then(|x| x.as_array()).map(|x| x.len()).unwrap_or(0) as i64;
        let np = e.get("params").and_then(|x| x.as_array()).map(|x| x.len()).unwrap_or(0) as i64;
        for (k, i) in insts.iter().enumerate() {
            if i.get("k").and_then(|x| x.as_str()) == Some("wave") {
                match i.get("m").and_then(|x| x.as_u64()) {
                    Some(m) if (m as usize) < nm => {}
                    _ => bad.push(format!("event {id}: instrument {k} names no sample of the bank")),
                }
            }
            if i.get("track").and_then(|x| x.as_i64()).is_some_and(|t| t >= ntr) {
                bad.push(format!("event {id}: instrument {k}: track out of range"));
            }
            for kid in i.get("kids").and_then(|x| x.as_array()).into_iter().flatten() {
                if kid.as_u64().is_none_or(|x| x as usize >= insts.len()) {
                    bad.push(format!("event {id}: instrument {k}: an entry out of range"));
                }
            }
        }
        for s in e.get("sheets").and_then(|x| x.as_array()).into_iter().flatten() {
            if s[0].as_i64().is_none_or(|p| p < 0 || p >= np) {
                bad.push(format!("event {id}: a sheet without its parameter"));
            }
        }
    }
    bad
}
