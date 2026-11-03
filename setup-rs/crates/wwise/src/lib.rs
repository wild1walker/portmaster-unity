//! `pmu wwise`: a Unity game's Wwise soundbanks, read once at port setup into the data of
//! ua_host's Wwise player (docs/wwise-player.md): per bank `<Bank>.uaww.json` (the objects step 1
//! plays) and `<Bank>.uaww.pak` (the media: Wwise Vorbis rebuilt as Ogg Vorbis, IMA ADPCM and PCM
//! as they are). Bank version 128 (Wwise 2017.2). The media are read and written one at a time:
//! a bank's DATA is never held in memory.
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

use serde_json::{json, Map, Value};

pub mod bnk;
pub mod codebooks;
pub mod hirc;
pub mod ogg;
pub mod rd;
pub mod wem;

use hirc::{prop, Base, Obj, Props, Tree};
pub use rd::Result;

/// The bank versions this reader knows.
pub const VERSIONS: &[u32] = &[128];
pub const FORMAT: u32 = 1;

/// Wwise's release of a bank version (as docs/plugins.md lists them).
pub fn wwise_line(v: u32) -> &'static str {
    match v {
        120 => "2016.2",
        125 => "2017.1",
        128 => "2017.2",
        132 => "2018.1",
        134 => "2019.1",
        135 => "2019.2",
        140 => "2021.1",
        _ => "?",
    }
}

#[derive(Default)]
pub struct Opts {
    /// Decode every Vorbis stream written and compare sample counts; resolve every event.
    pub check: bool,
    /// The game's Wwise library, or a folder with it (default: the `Plugins` folder nearest above
    /// the banks): Wwise Vorbis's codebooks are read from it.
    pub wwise_lib: Option<PathBuf>,
}

#[derive(Debug)]
pub enum Error {
    /// A bank of a version this reader does not know (status 3).
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

/// What a run did, per bank, and the problems met (media or objects left out, check failures).
#[derive(Default, Debug)]
pub struct Summary {
    pub lines: Vec<String>,
    pub problems: Vec<String>,
    pub check_failures: usize,
}

/// The banks of a folder (or one bank file), sorted, Init first.
pub fn bank_files(p: &Path) -> Result<Vec<PathBuf>> {
    if p.is_file() {
        return Ok(vec![p.to_path_buf()]);
    }
    let mut v: Vec<PathBuf> = fs::read_dir(p)
        .map_err(|e| format!("{}: {e}", p.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.is_file() && p.extension().is_some_and(|x| x.eq_ignore_ascii_case("bnk")))
        .collect();
    v.sort_by_key(|p| (!stem(p).eq_ignore_ascii_case("init"), p.clone()));
    if v.is_empty() {
        return Err(format!("{}: no .bnk files", p.display()));
    }
    Ok(v)
}

fn stem(p: &Path) -> String {
    p.file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn id(x: u32) -> Value {
    Value::String(x.to_string())
}

/// A float as its shortest decimal (an f32 read from the bank, without f64 noise).
fn num(x: f64) -> Value {
    let x = (x as f32).to_string().parse::<f64>().unwrap_or(x);
    json!(if x == 0.0 { 0.0 } else { x })
}

fn ms(x: f64) -> Value {
    // (music times are f64 milliseconds: kept to the microsecond)
    let x = (x * 1000.0).round() / 1000.0;
    json!(if x == 0.0 { 0.0 } else { x })
}

/// Converts the banks of `banks` (a folder or a .bnk) into `out`.
pub fn run(
    banks: &Path,
    out: &Path,
    opts: &Opts,
    log: &mut dyn FnMut(&str),
) -> std::result::Result<Summary, Error> {
    let files = bank_files(banks)?;
    let mut opened = Vec::new();
    for f in &files {
        let b = bnk::open(f)?;
        if !VERSIONS.contains(&b.version) {
            return Err(Error::Unsupported(format!(
                "{}: bank version {} (Wwise {}): the player's converter reads version {}",
                f.display(),
                b.version,
                wwise_line(b.version),
                VERSIONS
                    .iter()
                    .map(|v| v.to_string())
                    .collect::<Vec<_>>()
                    .join(", ")
            )));
        }
        opened.push(b);
    }
    fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    let mut sum = Summary::default();
    // Wwise Vorbis's codebooks, from the game's own Wwise library (none are in this program)
    let at = opts
        .wwise_lib
        .clone()
        .or_else(|| codebooks::plugins_near(banks));
    let (files, libs) = at.as_deref().map(codebooks::from_path).unwrap_or_default();
    let line = match (&at, files.is_empty(), libs.is_empty()) {
        (None, ..) => "Vorbis codebooks: no Plugins folder near the banks (--wwise-lib): Vorbis media are left out".to_string(),
        (Some(p), true, _) => format!("Vorbis codebooks: no Wwise library (AkSoundEngine) in {}: Vorbis media are left out", p.display()),
        (Some(_), false, true) => format!(
            "Vorbis codebooks: none found in {}: Vorbis media are left out",
            files.iter().map(|f| f.display().to_string()).collect::<Vec<_>>().join(", ")
        ),
        _ => libs
            .iter()
            .map(|l| format!("Vorbis codebooks: {} from {} (at 0x{:x}; sha256 {})", l.count, l.source, l.offset, l.sha256))
            .collect::<Vec<_>>()
            .join("\n"),
    };
    log(&line);
    sum.lines.push(line);
    let mut docs = Vec::new();
    for b in &opened {
        let doc = bank(b, banks, out, opts, &libs, &mut sum, log)?;
        docs.push(doc);
    }
    if opts.check {
        check_refs(&docs, &mut sum, log);
    }
    Ok(sum)
}

/// Writes `<out>/<name>` through a temporary file renamed at the end.
fn write_atomic(
    path: &Path,
    f: impl FnOnce(&mut BufWriter<File>) -> std::io::Result<()>,
) -> Result<()> {
    let tmp = path.with_extension("tmp");
    let r = (|| {
        let mut w = BufWriter::with_capacity(1 << 16, File::create(&tmp)?);
        f(&mut w)?;
        w.into_inner().map_err(|e| e.into_error())?.sync_all()?;
        fs::rename(&tmp, path)
    })();
    r.map_err(|e| format!("{}: {e}", path.display()))
}

struct Ctx<'a> {
    states: BTreeMap<u32, &'a [(u16, f32)]>,
    rtpc: Map<String, Value>,
}

fn bank(
    b: &bnk::Bank,
    banks: &Path,
    out: &Path,
    opts: &Opts,
    libs: &[codebooks::Library],
    sum: &mut Summary,
    log: &mut dyn FnMut(&str),
) -> Result<Value> {
    let name = stem(&b.path);
    let objs = hirc::objects(&b.hirc).map_err(|e| format!("{}: HIRC: {e}", b.path.display()))?;
    let mut ok = Vec::new();
    for (t, i, o) in &objs {
        match o {
            Ok(o) => ok.push((*t, *i, o)),
            Err(e) => sum
                .problems
                .push(format!("{name}: object {i} (type {t}) left out: {e}")),
        }
    }
    let mut cx = Ctx {
        states: BTreeMap::new(),
        rtpc: Map::new(),
    };
    let mut actions = BTreeMap::new();
    for (_, i, o) in &ok {
        match o {
            Obj::State { props } => {
                cx.states.insert(*i, props.as_slice());
            }
            Obj::Action { .. } => {
                actions.insert(*i, *o);
            }
            _ => {}
        }
    }
    let mut nodes = Map::new();
    let mut buses = Map::new();
    let mut events = Map::new();
    let mut streamed = BTreeSet::new();
    for (_, i, o) in &ok {
        if let Some(v) = node(*i, o, &mut cx, &mut streamed) {
            nodes.insert(i.to_string(), v);
        }
        match o {
            Obj::Bus {
                parent,
                props,
                rtpcs,
                states,
            } => {
                let mut m = Map::new();
                if *parent != 0 {
                    m.insert("parent".into(), id(*parent));
                }
                put(
                    &mut m,
                    "vol",
                    props.f(prop::VOLUME) + props.f(prop::BUS_VOLUME),
                );
                put(&mut m, "pitch", props.f(prop::PITCH));
                if let Some(s) = state_props(states, &cx) {
                    m.insert("states".into(), s);
                }
                add_rtpcs(&mut cx.rtpc, *i, rtpcs);
                buses.insert(i.to_string(), Value::Object(m));
            }
            Obj::Event { actions: list } => {
                let mut v = Vec::new();
                for a in list {
                    match actions.get(a) {
                        Some(o) => v.push(action(o)),
                        None => sum
                            .problems
                            .push(format!("{name}: event {i}: action {a} not in the bank")),
                    }
                }
                events.insert(i.to_string(), Value::Array(v));
            }
            _ => {}
        }
    }
    // the media: the bank's own, then loose .wem files of streamed sources
    let pak = out.join(format!("{name}.uaww.pak"));
    let mut media = Map::new();
    let (mut n_vorbis, mut n_adpcm, mut n_pcm, mut total) = (0, 0, 0, 0u64);
    let dir = if banks.is_file() {
        banks.parent().unwrap_or(Path::new(".")).to_path_buf()
    } else {
        banks.to_path_buf()
    };
    let mut f = File::open(&b.path).map_err(|e| format!("{}: {e}", b.path.display()))?;
    let mut wembuf = Vec::new();
    let mut outbuf = Vec::new();
    let mut problems = Vec::new();
    let mut checked = 0usize;
    let mut check_fail = 0usize;
    let mut no_codebooks = 0usize;
    write_atomic(&pak, |w| {
        let mut off = 0u64;
        let mut list: Vec<(u32, Option<(u32, u32)>, Option<PathBuf>)> = b
            .media
            .iter()
            .map(|&(i, o, s)| (i, Some((o, s)), None))
            .collect();
        for &s in &streamed {
            let p = dir.join(format!("{s}.wem"));
            if p.is_file() {
                list.retain(|x| x.0 != s);
                list.push((s, None, Some(p)));
            } else if !b.media.iter().any(|m| m.0 == s) {
                problems.push(format!(
                    "{name}: streamed media {s}: no {s}.wem beside the banks"
                ));
            }
        }
        for (mid, inbank, file) in list {
            let r: Result<()> = (|| {
                match (&inbank, &file) {
                    (Some((o, s)), _) => b.read_media(&mut f, *o, *s, &mut wembuf)?,
                    (None, Some(p)) => {
                        wembuf = fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?
                    }
                    _ => unreachable!(),
                }
                let w = wem::parse(&wembuf)?;
                let codec = w.codec()?;
                let samples = w.samples()?;
                if codec == wem::Codec::Vorbis && libs.is_empty() {
                    no_codebooks += 1;
                    return Err(NO_CODEBOOKS.into());
                }
                wem::convert(&wembuf, &w, &mut outbuf, libs)?;
                if opts.check {
                    checked += 1;
                    match codec {
                        wem::Codec::Vorbis => match wem::decode_count(&outbuf) {
                            Ok((n, ch, g, tail))
                                if g == samples
                                    && ch == w.channels
                                    && (samples..samples + tail).contains(&n) => {}
                            Ok((n, ch, g, _)) => {
                                check_fail += 1;
                                problems.push(format!(
                                    "check: media {mid}: decodes to {n} samples x {ch} channels (last granule {g}), the WEM says {samples} x {}",
                                    w.channels
                                ));
                            }
                            Err(e) => {
                                check_fail += 1;
                                problems.push(format!("check: media {mid}: {e}"));
                            }
                        },
                        wem::Codec::Adpcm if w.data.1 % w.block_align as usize != 0 => {
                            check_fail += 1;
                            problems.push(format!("check: media {mid}: ADPCM data of {} bytes is not whole blocks of {}", w.data.1, w.block_align));
                        }
                        wem::Codec::Pcm if w.data.1 % (2 * w.channels as usize) != 0 => {
                            check_fail += 1;
                            problems.push(format!(
                                "check: media {mid}: PCM data of {} bytes is not whole frames",
                                w.data.1
                            ));
                        }
                        _ => {}
                    }
                }
                let mut m = Map::new();
                m.insert("off".into(), json!(off));
                m.insert("len".into(), json!(outbuf.len()));
                m.insert("codec".into(), json!(codec.name()));
                m.insert("rate".into(), json!(w.rate));
                m.insert("channels".into(), json!(w.channels));
                m.insert("samples".into(), json!(samples));
                if codec == wem::Codec::Adpcm {
                    m.insert("block_align".into(), json!(w.block_align));
                }
                if let Some((a, e)) = w.loop_ {
                    m.insert("loop".into(), json!([a, e]));
                }
                media.insert(mid.to_string(), Value::Object(m));
                match codec {
                    wem::Codec::Vorbis => n_vorbis += 1,
                    wem::Codec::Adpcm => n_adpcm += 1,
                    wem::Codec::Pcm => n_pcm += 1,
                }
                Ok(())
            })();
            match r {
                Ok(()) => {
                    w.write_all(&outbuf)?;
                    off += outbuf.len() as u64;
                    let pad = (16 - off % 16) % 16;
                    w.write_all(&[0u8; 16][..pad as usize])?;
                    off += pad;
                }
                Err(e) if e == NO_CODEBOOKS => {}
                Err(e) => problems.push(format!("{name}: media {mid} left out: {e}")),
            }
        }
        total = off;
        Ok(())
    })?;
    if no_codebooks > 0 {
        problems.insert(
            0,
            format!("{name}: {no_codebooks} Vorbis media left out (those sounds stay silent): no codebooks from the game's Wwise library"),
        );
    }
    sum.problems.append(&mut problems);
    sum.check_failures += check_fail;
    let mut doc = Map::new();
    doc.insert("format".into(), json!(FORMAT));
    doc.insert(
        "bank".into(),
        json!({"name": name, "id": b.id.to_string(), "bank_version": b.version, "wwise": wwise_line(b.version)}),
    );
    doc.insert("media".into(), Value::Object(media));
    let nn = nodes.len();
    doc.insert("nodes".into(), Value::Object(nodes));
    let ne = events.len();
    doc.insert("events".into(), Value::Object(events));
    doc.insert("buses".into(), Value::Object(buses));
    doc.insert("rtpc".into(), Value::Object(std::mem::take(&mut cx.rtpc)));
    let mut defaults = Map::new();
    if !b.stmg.is_empty() {
        match bnk::globals(&b.stmg) {
            Ok(g) => defaults = globals_json(&g),
            Err(e) => sum.problems.push(format!(
                "{name}: STMG (the game syncs' defaults) left out: {e}"
            )),
        }
    }
    doc.insert("defaults".into(), Value::Object(defaults));
    let doc = Value::Object(doc);
    let jpath = out.join(format!("{name}.uaww.json"));
    write_atomic(&jpath, |w| {
        serde_json::to_writer(&mut *w, &doc).map_err(std::io::Error::other)?;
        w.write_all(b"\n")
    })?;
    let mut line = format!(
        "{name}.bnk (version {}): {} media ({n_vorbis} vorbis, {n_adpcm} adpcm, {n_pcm} pcm; {:.1} MB), {nn} nodes, {ne} events",
        b.version,
        n_vorbis + n_adpcm + n_pcm,
        total as f64 / 1e6
    );
    if no_codebooks > 0 {
        line += &format!("; {no_codebooks} vorbis left out (no codebooks)");
    }
    if opts.check {
        line += &format!("; checked {checked} media");
    }
    log(&line);
    sum.lines.push(line);
    Ok(doc)
}

fn put(m: &mut Map<String, Value>, k: &str, x: f64) {
    if x != 0.0 {
        m.insert(k.into(), num(x));
    }
}

/// The volume and pitch of a state's properties (AkRTPC_ParameterID: 0 volume, 5 bus volume,
/// 7 make-up gain, 2 pitch).
fn state_props(groups: &[hirc::StateGroup], cx: &Ctx) -> Option<Value> {
    let mut out = Map::new();
    for (g, states) in groups {
        let mut gm = Map::new();
        for (s, inst) in states {
            let mut sm = Map::new();
            if let Some(ps) = cx.states.get(inst) {
                let (mut vol, mut pitch) = (0.0, 0.0);
                for &(p, v) in ps.iter() {
                    match p {
                        0 | 5 | 7 => vol += v as f64,
                        2 => pitch += v as f64,
                        _ => {}
                    }
                }
                put(&mut sm, "vol", vol);
                put(&mut sm, "pitch", pitch);
            }
            if !sm.is_empty() {
                gm.insert(s.to_string(), Value::Object(sm));
            }
        }
        if !gm.is_empty() {
            out.insert(g.to_string(), Value::Object(gm));
        }
    }
    (!out.is_empty()).then_some(Value::Object(out))
}

fn add_rtpcs(rtpc: &mut Map<String, Value>, target: u32, list: &[hirc::Rtpc]) {
    for r in list {
        let p = match (r.kind, r.param) {
            (0, 0 | 5 | 7) => "volume",
            (0, 2) => "pitch",
            _ => continue,
        };
        let curve: Vec<Value> = r
            .points
            .iter()
            .map(|&(x, y, i)| json!([num(x as f64), num(y as f64), i]))
            .collect();
        let scaling = match r.scaling {
            0 => "none",
            2 => "db",
            _ => continue, // (log / frequency scalings: filters, not step 1)
        };
        let e = rtpc.entry(r.id.to_string()).or_insert_with(|| json!([]));
        e.as_array_mut()
            .unwrap()
            .push(json!({"target": id(target), "prop": p, "scaling": scaling, "curve": curve}));
    }
}

fn base_json(m: &mut Map<String, Value>, nid: u32, b: &Base, cx: &mut Ctx) {
    if b.parent != 0 {
        m.insert("parent".into(), id(b.parent));
    }
    let p = &b.props;
    put(
        m,
        "vol",
        p.f(prop::VOLUME) + p.f(prop::MAKEUP_GAIN) + p.f(prop::OUTPUT_BUS_VOLUME),
    );
    put(m, "pitch", p.f(prop::PITCH));
    if let Some((lo, hi)) = p.range(prop::VOLUME) {
        m.insert("rand_vol".into(), json!([num(lo), num(hi)]));
    }
    if let Some((lo, hi)) = p.range(prop::PITCH) {
        m.insert("rand_pitch".into(), json!([num(lo), num(hi)]));
    }
    if b.bus != 0 {
        m.insert("bus".into(), id(b.bus));
    }
    if let Some(s) = state_props(&b.states, cx) {
        m.insert("states".into(), s);
    }
    if b.limit > 0 {
        m.insert("limit".into(), json!(b.limit));
        if b.kill_newest {
            m.insert("limit_newest".into(), json!(true));
        }
    }
    if p.get(prop::PAN_LR).is_some() || p.get(prop::PAN_FR).is_some() {
        m.insert(
            "pan".into(),
            json!([num(p.f(prop::PAN_LR)), num(p.f(prop::PAN_FR))]),
        );
    }
    if let Some(v) = p.get(prop::PROBABILITY) {
        if v.f() != 100.0 {
            m.insert("prob".into(), num(v.f()));
        }
    }
    add_rtpcs(&mut cx.rtpc, nid, &b.rtpcs);
}

fn ids(v: &[u32]) -> Value {
    Value::Array(v.iter().map(|&x| id(x)).collect())
}

fn loop_of(p: &Props) -> i64 {
    match p.get(prop::LOOP) {
        Some(hirc::Val::I(n)) => n as i64,
        Some(v) => v.f() as i64,
        None => 1,
    }
}

fn rules_json(rules: &[hirc::Rule]) -> Value {
    Value::Array(
        rules
            .iter()
            .map(|r| {
                let any = |v: &[u32]| {
                    Value::Array(
                        v.iter()
                            .map(|&x| Value::String((x as i32).to_string()))
                            .collect(),
                    )
                };
                let mut m = Map::new();
                m.insert("src".into(), any(&r.src));
                m.insert("dst".into(), any(&r.dst));
                m.insert("fade_out_ms".into(), json!(r.fade_out));
                m.insert("fade_in_ms".into(), json!(r.fade_in));
                m.insert("sync".into(), json!(r.sync));
                if r.segment != 0 {
                    m.insert("segment".into(), id(r.segment));
                }
                Value::Object(m)
            })
            .collect(),
    )
}

fn item_json(it: &hirc::Item) -> Value {
    let mut m = Map::new();
    if it.children.is_empty() && it.segment != 0 {
        m.insert("segment".into(), id(it.segment));
    } else {
        let kind = match it.kind {
            0 => "sequence",
            1 => "sequence_step",
            2 => "random",
            _ => "random_step",
        };
        m.insert("kind".into(), json!(kind));
        m.insert(
            "children".into(),
            Value::Array(it.children.iter().map(item_json).collect()),
        );
        if it.avoid > 0 {
            m.insert("avoid_repeat".into(), json!(it.avoid));
        }
        if it.shuffle {
            m.insert("shuffle".into(), json!(true));
        }
    }
    if it.loop_count != 1 {
        m.insert("loop".into(), json!(it.loop_count));
    }
    if it.weight != 100_000 {
        m.insert("weight".into(), num(it.weight as f64 / 1000.0));
    }
    Value::Object(m)
}

/// A decision tree; a leaf that is not one of the switch's children (0, or 1 in a tree of no
/// arguments) plays nothing: "0".
fn tree_json(t: &Tree, children: &[u32]) -> Value {
    match t {
        Tree::Leaf(x) => id(if children.contains(x) { *x } else { 0 }),
        Tree::Node(v) => Value::Object(
            v.iter()
                .map(|(k, s)| (k.to_string(), tree_json(s, children)))
                .collect(),
        ),
    }
}

const NO_CODEBOOKS: &str = "(no codebooks)";

/// Segment cue ids: Wwise's entry and exit cues.
const ENTRY_CUE: u32 = 43573010;
const EXIT_CUE: u32 = 1539036744;

fn node(nid: u32, o: &Obj, cx: &mut Ctx, streamed: &mut BTreeSet<u32>) -> Option<Value> {
    let mut m = Map::new();
    let t = match o {
        Obj::Sound { src, base } => {
            base_json(&mut m, nid, base, cx);
            if src.plugin & 0x0f == 1 && src.id != 0 {
                m.insert("media".into(), id(src.id));
                if src.stream != 0 {
                    streamed.insert(src.id);
                }
            } else {
                m.insert("plugin".into(), json!(src.plugin));
            }
            let l = loop_of(&base.props);
            if l != 1 {
                m.insert("loop".into(), json!(l));
            }
            put(
                &mut m,
                "delay_ms",
                base.props.f(prop::INITIAL_DELAY) * 1000.0,
            );
            "sound"
        }
        Obj::RanSeq {
            base,
            loop_count,
            trans_time,
            avoid,
            trans_mode,
            random_mode,
            mode,
            bits,
            children,
            playlist,
        } => {
            base_json(&mut m, nid, base, cx);
            let random = *mode == 0;
            let order: Vec<u32> = if playlist.is_empty() {
                children.clone()
            } else {
                playlist.iter().map(|p| p.0).collect()
            };
            m.insert("children".into(), ids(&order));
            if random && bits & 1 != 0 && !playlist.is_empty() {
                m.insert(
                    "weights".into(),
                    Value::Array(playlist.iter().map(|p| num(p.1 as f64 / 1000.0)).collect()),
                );
            }
            if random && *avoid > 0 {
                m.insert("avoid_repeat".into(), json!(avoid));
            }
            if random && *random_mode == 1 {
                m.insert("shuffle".into(), json!(true));
            }
            if bits & 8 != 0 {
                m.insert("continuous".into(), json!(true));
            }
            if *loop_count != 1 {
                m.insert("loop".into(), json!(loop_count));
            }
            if *trans_mode != 0 {
                let tm = [
                    "none",
                    "xfade_amp",
                    "xfade_power",
                    "delay",
                    "sample",
                    "trigger",
                ];
                m.insert(
                    "transition".into(),
                    json!(tm.get(*trans_mode as usize).copied().unwrap_or("none")),
                );
                put(&mut m, "transition_ms", *trans_time as f64);
            }
            if bits & 2 != 0 {
                m.insert("reset".into(), json!(true));
            }
            if bits & 16 != 0 {
                m.insert("global".into(), json!(true));
            }
            if random {
                "random"
            } else {
                "sequence"
            }
        }
        Obj::Switch {
            base,
            state,
            group,
            default,
            continuous,
            children: _,
            map,
        } => {
            base_json(&mut m, nid, base, cx);
            m.insert("group".into(), id(*group));
            if *state {
                m.insert("state".into(), json!(true));
            }
            m.insert(
                "map".into(),
                Value::Object(map.iter().map(|(k, v)| (k.to_string(), ids(v))).collect()),
            );
            m.insert("default".into(), id(*default));
            if *continuous {
                m.insert("continuous".into(), json!(true));
            }
            "switch"
        }
        Obj::Layer { base, children } => {
            base_json(&mut m, nid, base, cx);
            m.insert("children".into(), ids(children));
            "layer"
        }
        Obj::Mixer { base, children } => {
            base_json(&mut m, nid, base, cx);
            m.insert("children".into(), ids(children));
            "mixer"
        }
        Obj::Segment {
            base,
            children,
            duration,
            markers,
        } => {
            base_json(&mut m, nid, base, cx);
            m.insert("children".into(), ids(children));
            m.insert("duration_ms".into(), ms(*duration));
            let cue = |c: u32, first: bool| {
                markers
                    .iter()
                    .find(|x| x.0 == c)
                    .or(if first {
                        markers.first()
                    } else {
                        markers.last()
                    })
                    .map(|x| x.1)
            };
            m.insert("entry_ms".into(), ms(cue(ENTRY_CUE, true).unwrap_or(0.0)));
            m.insert(
                "exit_ms".into(),
                ms(cue(EXIT_CUE, false).unwrap_or(*duration)),
            );
            "segment"
        }
        Obj::Track {
            base,
            sources,
            clips,
            subtracks,
            kind,
            switch,
        } => {
            base_json(&mut m, nid, base, cx);
            for s in sources {
                if s.stream != 0 {
                    streamed.insert(s.id);
                }
            }
            let cl: Vec<Value> = clips
                .iter()
                .map(|c| {
                    let mut x = Map::new();
                    x.insert("media".into(), id(c.media));
                    if c.sub != 0 {
                        x.insert("sub".into(), json!(c.sub));
                    }
                    x.insert("at_ms".into(), ms(c.at));
                    x.insert("begin_ms".into(), ms(c.begin));
                    x.insert("end_ms".into(), ms(c.end));
                    x.insert("src_ms".into(), ms(c.src));
                    Value::Object(x)
                })
                .collect();
            m.insert("clips".into(), Value::Array(cl));
            if *subtracks != 1 {
                m.insert("subtracks".into(), json!(subtracks));
            }
            let k = ["normal", "random", "sequence", "switch"];
            if *kind != 0 {
                m.insert(
                    "kind".into(),
                    json!(k.get(*kind as usize).copied().unwrap_or("normal")),
                );
            }
            if let Some(s) = switch {
                m.insert(
                    "switch".into(),
                    json!({"group": id(s.group), "state": s.state, "default": id(s.default),
                           "map": Value::Object(s.assoc.iter().enumerate().map(|(i, v)| (v.to_string(), json!(i))).collect())}),
                );
            }
            "track"
        }
        Obj::MusicSwitch {
            base,
            children,
            rules,
            cont,
            args,
            tree,
        } => {
            base_json(&mut m, nid, base, cx);
            m.insert("children".into(), ids(children));
            m.insert(
                "args".into(),
                Value::Array(
                    args.iter()
                        .map(|(g, k)| json!({"group": id(*g), "state": *k == 1}))
                        .collect(),
                ),
            );
            m.insert("tree".into(), tree_json(tree, children));
            if *cont {
                m.insert("continue".into(), json!(true));
            }
            if !rules.is_empty() {
                m.insert("rules".into(), rules_json(rules));
            }
            "music_switch"
        }
        Obj::Playlist {
            base,
            children,
            rules,
            items,
        } => {
            base_json(&mut m, nid, base, cx);
            m.insert("children".into(), ids(children));
            if let Some(it) = items {
                m.insert("items".into(), item_json(it));
            }
            if !rules.is_empty() {
                m.insert("rules".into(), rules_json(rules));
            }
            "playlist"
        }
        _ => return None,
    };
    m.insert("type".into(), json!(t));
    Some(Value::Object(m))
}

fn action(o: &Obj) -> Value {
    let Obj::Action {
        ty,
        target,
        is_bus,
        props,
        params: p,
    } = o
    else {
        unreachable!()
    };
    let (hi, lo) = (ty >> 8, ty & 0xff);
    let scoped = |one: &'static str, all: &'static str| {
        if matches!(lo, 4 | 5 | 8 | 9) {
            all
        } else {
            one
        }
    };
    let name = match hi {
        0x01 => scoped("stop", "stop_all"),
        0x02 => scoped("pause", "pause_all"),
        0x03 => scoped("resume", "resume_all"),
        0x04 => "play",
        0x0A => "set_volume",
        0x0B => "reset_volume",
        0x0C => "set_bus_volume",
        0x0D => "reset_bus_volume",
        0x12 => "set_state",
        0x13 => "set_rtpc",
        0x14 => "reset_rtpc",
        0x19 => "set_switch",
        _ => "other",
    };
    let mut m = Map::new();
    m.insert("action".into(), json!(name));
    m.insert("type".into(), json!(ty));
    if *target != 0 && !matches!(hi, 0x12 | 0x19) {
        m.insert("target".into(), id(*target));
    }
    if *is_bus {
        m.insert("bus".into(), json!(true));
    }
    if hi != 0x04 && lo & 1 == 1 && hi != 0x19 && hi != 0x12 {
        m.insert("object".into(), json!(true));
    }
    if !p.except.is_empty() {
        m.insert(
            "except".into(),
            Value::Array(p.except.iter().map(|e| id(e.0)).collect()),
        );
    }
    match hi {
        0x12 | 0x19 => {
            m.insert("group".into(), id(p.group));
            m.insert("value".into(), id(p.value));
        }
        0x13 | 0x14 => {
            m.insert("group".into(), id(*target));
        }
        _ => {}
    }
    if matches!(hi, 0x0A | 0x0C | 0x13) {
        m.insert("amount".into(), num(p.amount as f64));
        let meaning = match p.meaning {
            1 => "absolute",
            2 => "offset",
            _ => "default",
        };
        m.insert("meaning".into(), json!(meaning));
    }
    if let Some(v) = props.get(prop::TRANSITION_TIME) {
        if v.f() != 0.0 {
            m.insert("fade_ms".into(), num(v.f()));
        }
    }
    if p.curve != 4 && matches!(hi, 0x01..=0x04 | 0x0A..=0x0D | 0x13) {
        m.insert("curve".into(), json!(p.curve));
    }
    if let Some(v) = props.get(prop::DELAY_TIME) {
        if v.f() != 0.0 {
            m.insert("delay_ms".into(), num(v.f()));
        }
    }
    if hi == 0x04 && p.bank != 0 {
        m.insert("bank".into(), id(p.bank));
    }
    Value::Object(m)
}

fn globals_json(g: &bnk::Globals) -> Map<String, Value> {
    let mut d = Map::new();
    d.insert(
        "rtpc".into(),
        Value::Object(
            g.rtpc
                .iter()
                .map(|(i, v)| (i.to_string(), num(*v as f64)))
                .collect(),
        ),
    );
    d.insert("states".into(), json!({}));
    d.insert("switches".into(), json!({}));
    d.insert(
        "state_groups".into(),
        Value::Object(
            g.state_groups
                .iter()
                .map(|(i, fade, t)| {
                    let tr: Vec<Value> = t
                        .iter()
                        .map(|(a, b, ms)| json!([id(*a), id(*b), ms]))
                        .collect();
                    (i.to_string(), json!({"fade_ms": fade, "transitions": tr}))
                })
                .collect(),
        ),
    );
    d.insert(
        "switch_rtpc".into(),
        Value::Object(
            g.switch_rtpc
                .iter()
                .map(|(i, r, p)| {
                    let c: Vec<Value> = p
                        .iter()
                        .map(|(x, s)| json!([num(*x as f64), id(*s)]))
                        .collect();
                    (i.to_string(), json!({"rtpc": id(*r), "curve": c}))
                })
                .collect(),
        ),
    );
    d
}

/// Every node an event reaches resolves (a node, a bus, or media of one of the banks).
fn check_refs(docs: &[Value], sum: &mut Summary, log: &mut dyn FnMut(&str)) {
    let mut nodes: BTreeMap<&str, &Value> = BTreeMap::new();
    let mut media = BTreeSet::new();
    let mut buses = BTreeSet::new();
    for d in docs {
        for (k, v) in d["nodes"].as_object().unwrap() {
            nodes.insert(k, v);
        }
        media.extend(d["media"].as_object().unwrap().keys().map(|s| s.as_str()));
        buses.extend(d["buses"].as_object().unwrap().keys().map(|s| s.as_str()));
    }
    let mut seen = BTreeSet::new();
    let mut bad = Vec::new();
    let (mut n_events, mut n_reached) = (0, 0);
    fn walk<'a>(
        k: &'a str,
        nodes: &BTreeMap<&str, &'a Value>,
        media: &BTreeSet<&str>,
        buses: &BTreeSet<&str>,
        seen: &mut BTreeSet<&'a str>,
        bad: &mut Vec<String>,
        from: &str,
    ) {
        if !seen.insert(k) {
            return;
        }
        let Some(n) = nodes.get(k) else {
            bad.push(format!("{from}: node {k} not found"));
            return;
        };
        let s = |v: &'a Value| v.as_str().unwrap_or("");
        let mut kids: Vec<&'a str> = Vec::new();
        for key in ["children"] {
            if let Some(a) = n.get(key).and_then(|v| v.as_array()) {
                kids.extend(a.iter().map(s));
            }
        }
        if let Some(mp) = n.get("map").and_then(|v| v.as_object()) {
            for v in mp.values() {
                kids.extend(v.as_array().into_iter().flatten().map(s));
            }
        }
        fn leaves<'a>(t: &'a Value, out: &mut Vec<&'a str>) {
            match t {
                Value::String(x) => out.push(x),
                Value::Object(m) => m.values().for_each(|v| leaves(v, out)),
                _ => {}
            }
        }
        if let Some(t) = n.get("tree") {
            leaves(t, &mut kids);
        }
        fn segs<'a>(t: &'a Value, out: &mut Vec<&'a str>) {
            if let Some(x) = t.get("segment").and_then(|v| v.as_str()) {
                out.push(x);
            }
            t.get("children")
                .and_then(|v| v.as_array())
                .into_iter()
                .flatten()
                .for_each(|c| segs(c, out));
        }
        if let Some(it) = n.get("items") {
            segs(it, &mut kids);
        }
        if let Some(b) = n.get("bus").and_then(|v| v.as_str()) {
            if !buses.contains(b) {
                bad.push(format!("{from}: node {k}: bus {b} not found"));
            }
        }
        let mut meds: Vec<&str> = n
            .get("media")
            .and_then(|v| v.as_str())
            .into_iter()
            .collect();
        for c in n
            .get("clips")
            .and_then(|v| v.as_array())
            .into_iter()
            .flatten()
        {
            meds.extend(c.get("media").and_then(|v| v.as_str()));
        }
        for m in meds {
            if !media.contains(m) {
                bad.push(format!("{from}: node {k}: media {m} not found"));
            }
        }
        for c in kids {
            if c == "0" {
                continue; // (a decision tree's "nothing plays")
            }
            walk(c, nodes, media, buses, seen, bad, from);
        }
    }
    for d in docs {
        for (e, acts) in d["events"].as_object().unwrap() {
            n_events += 1;
            for a in acts.as_array().unwrap() {
                let Some(t) = a.get("target").and_then(|v| v.as_str()) else {
                    continue;
                };
                let what = a["action"].as_str().unwrap_or("");
                let on_bus = a.get("bus").is_some() || what.contains("bus");
                if what == "set_rtpc" || what == "reset_rtpc" || what == "other" {
                    continue;
                }
                if on_bus {
                    if !buses.contains(t) {
                        bad.push(format!("event {e}: bus {t} not found"));
                    }
                    continue;
                }
                if what == "play" || nodes.contains_key(t) {
                    let before = seen.len();
                    walk(
                        t,
                        &nodes,
                        &media,
                        &buses,
                        &mut seen,
                        &mut bad,
                        &format!("event {e}"),
                    );
                    n_reached += seen.len() - before;
                } else if !buses.contains(t) {
                    bad.push(format!("event {e}: {what} target {t} not found"));
                }
            }
        }
    }
    let line = format!(
        "check: {n_events} events, {n_reached} nodes reached, {} unresolved",
        bad.len()
    );
    log(&line);
    sum.lines.push(line);
    sum.check_failures += bad.len();
    sum.problems
        .extend(bad.into_iter().map(|b| format!("check: {b}")));
}
