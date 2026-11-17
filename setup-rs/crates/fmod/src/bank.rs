//! An FMOD Studio bank (RIFF "FEV ", format 0x67 = FMOD Studio 1.10): its chunks and the objects
//! the player needs, read as the format has them (the chunk layouts: docs/fmod-player.md, "The
//! bank format"). Objects are keyed by their GUID; a chunk that belongs to another object (an
//! instrument's INST, a bus's BUS, a playlist, a controller list) is joined to the object whose
//! body opens the same LIST.
use std::collections::HashMap;

use crate::rd::{Rd, Result};

pub type Guid = [u8; 16];
pub const ZERO: Guid = [0; 16];

/// The bank format versions this reader knows.
pub const VERSIONS: &[u32] = &[0x67];

pub fn hex(g: &Guid) -> String {
    g.iter().map(|b| format!("{b:02x}")).collect()
}

#[derive(Clone, Debug, Default)]
pub struct Event {
    pub id: Guid,
    pub snapshot: Guid,
    pub timeline: Guid,
    pub input_bus: Guid,
    pub master: Guid,
    pub max_poly: i32,
    pub priority: i32,
    pub sched: i32,
    pub layouts: Vec<Guid>,
    pub steal: i32,
    pub cooldown: f32,
    pub flags: u32,
    pub user_float: Vec<(String, f32)>,
    pub user_string: Vec<(String, String)>,
}

/// A box on a timeline: the instrument, its start and length (timeline units, 48 kHz samples).
#[derive(Clone, Copy, Debug)]
pub struct Box_ {
    pub inst: Guid,
    pub start: u32,
    pub len: u32,
}

#[derive(Clone, Debug, Default)]
pub struct Marker {
    pub id: Guid,
    pub pos: u32,
    pub name: String,
}

#[derive(Clone, Debug, Default)]
pub struct Tempo {
    pub pos: u32,
    pub bpm: f32,
    pub upper: u32,
    pub lower: u32,
}

#[derive(Clone, Debug, Default)]
pub struct Transition {
    pub id: Guid,
    pub dest: Guid,
    pub start: u32,
    pub end: u32,
    pub cond: Vec<(Guid, f32, f32)>,
    pub quant: (u32, i32),
    pub chance: f32,
    pub flags: u32,
}

#[derive(Clone, Debug, Default)]
pub struct Timeline {
    pub id: Guid,
    /// list 1: asynchronous instruments (triggered when the cursor enters, they play on)
    pub async_boxes: Vec<Box_>,
    /// list 2: time-locked instruments (their position follows the cursor, cut at the box's end)
    pub locked: Vec<Box_>,
    pub markers: Vec<Marker>,
    pub tempo: Vec<Tempo>,
    pub transitions: Vec<Transition>,
    pub controllers: Vec<Guid>,
}

/// A parameter sheet: its parameter, the event it belongs to, instruments on the parameter's axis
/// (start, length in the parameter's units) and the controllers the parameter drives.
#[derive(Clone, Debug, Default)]
pub struct Layout {
    pub id: Guid,
    pub param: Guid,
    pub event: Guid,
    pub boxes: Vec<(Guid, f32, f32)>,
    pub controllers: Vec<Guid>,
}

#[derive(Clone, Debug, Default)]
pub struct Param {
    pub id: Guid,
    pub kind: u32,
    pub name: String,
    pub min: f32,
    pub max: f32,
    pub default: f32,
    pub velocity: f32,
    pub seek_up: f32,
    pub seek_down: f32,
}

#[derive(Clone, Debug, PartialEq)]
pub enum InstKind {
    Wave { wav: Guid },
    Multi,
    Programmer { name: String },
    Event { event: Guid },
    Scatterer,
    Other(&'static str),
}

/// An instrument's common properties (its INST chunk).
#[derive(Clone, Debug, Default)]
pub struct InstBody {
    pub vol: f32,
    pub pitch: f32,
    pub loop_count: i32,
    pub flags: u8,
    pub chance: f32,
    pub delay: (f32, f32),
    pub quant: (u32, i32),
    pub control_param: Guid,
    pub seek: f32,
    pub max_poly: i32,
    pub out: Guid,
    pub steal: i32,
    pub left_trim: u32,
    pub seek_pct: f32,
}

#[derive(Clone, Debug)]
pub struct Inst {
    pub id: Guid,
    pub kind: InstKind,
    pub body: Option<InstBody>,
    /// a multi instrument's playlist: play mode (0 sequential, 1 random, 2 shuffle, 3 global
    /// sequential, 4 instance sequential), selection mode, entries (instrument, weight)
    pub playlist: Option<(i32, i32, Vec<(Guid, f32)>)>,
}

#[derive(Clone, Debug)]
pub struct Wav {
    pub id: Guid,
    pub sound_bank: i32,
    pub subsound: i32,
    pub mode: u32,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BusKind {
    Input,
    Group,
    Master,
    Return,
}

#[derive(Clone, Debug)]
pub struct Bus {
    pub id: Guid,
    pub kind: BusKind,
    pub out: Guid,
    pub vol: f32,
    pub pitch: f32,
    pub pre: Vec<Guid>,
    pub post: Vec<Guid>,
}

#[derive(Clone, Debug)]
pub struct Effect {
    pub id: Guid,
    pub dsp: u32,
    pub bypass: bool,
}

#[derive(Clone, Debug)]
pub struct Controller {
    pub id: Guid,
    pub owner: Guid,
    pub curve: Guid,
    pub prop: i32,
}

/// A curve point: x, y, the segment's shape, its type.
#[derive(Clone, Copy, Debug)]
pub struct Point {
    pub x: f32,
    pub y: f32,
    pub shape: f32,
    pub kind: u32,
}

#[derive(Clone, Debug)]
pub struct Curve {
    pub id: Guid,
    pub owner: Guid,
    pub points: Vec<Point>,
}

#[derive(Clone, Debug)]
pub enum ModKind {
    /// AHDSR: initial, peak, sustain values, attack, hold, decay, release times (s), 3 shapes
    Ahdsr([f32; 10]),
    Random(f32),
    Other(i32),
}

#[derive(Clone, Debug)]
pub struct Modulator {
    pub id: Guid,
    pub owner: Guid,
    pub prop: i32,
    pub kind: ModKind,
}

/// The sound table of a bank of programmer sounds (a VoiceOver bank): sorted 64-bit key hashes
/// and their subsound indices.
#[derive(Clone, Debug, Default)]
pub struct SoundTable {
    pub sound_bank: i32,
    pub keys: Vec<u64>,
    pub index: Vec<u32>,
}

/// Where the bank's FSB5 sound data is: (offset in the file, length).
#[derive(Clone, Debug, Default)]
pub struct Bank {
    pub version: u32,
    pub id: Guid,
    pub events: HashMap<Guid, Event>,
    pub timelines: HashMap<Guid, Timeline>,
    pub layouts: Vec<Layout>,
    pub params: HashMap<Guid, Param>,
    pub insts: HashMap<Guid, Inst>,
    pub wavs: HashMap<Guid, Wav>,
    pub buses: HashMap<Guid, Bus>,
    pub effects: HashMap<Guid, Effect>,
    pub controllers: HashMap<Guid, Controller>,
    pub curves: Vec<Curve>,
    pub modulators: Vec<Modulator>,
    pub sound_table: Option<SoundTable>,
    pub strings: Option<Vec<(String, Guid)>>,
    pub fsb: Vec<(usize, usize)>,
    /// chunks that did not read (kind, why): left out
    pub problems: Vec<String>,
}

struct Chunk<'a> {
    id: [u8; 4],
    data: &'a [u8],
    /// the data's offset in the file
    at: usize,
}

/// The chunks of a RIFF body, in order: (path of LIST types, chunk); a LIST is entered.
fn walk<'a>(d: &'a [u8], mut off: usize, end: usize, path: &mut Vec<[u8; 4]>, out: &mut Vec<(Vec<[u8; 4]>, Chunk<'a>)>) -> Result<()> {
    while off + 8 <= end {
        let id: [u8; 4] = d[off..off + 4].try_into().unwrap();
        let size = u32::from_le_bytes(d[off + 4..off + 8].try_into().unwrap()) as usize;
        let body = off + 8;
        let stop = body.checked_add(size).filter(|&e| e <= end).ok_or_else(|| format!("chunk {} at {off} runs past its parent", String::from_utf8_lossy(&id)))?;
        if &id == b"LIST" || &id == b"RIFF" {
            if size < 4 {
                return Err(format!("a LIST at {off} without a type"));
            }
            let typ: [u8; 4] = d[body..body + 4].try_into().unwrap();
            path.push(typ);
            out.push((path.clone(), Chunk { id: *b"LIST", data: &d[body + 4..stop], at: body + 4 }));
            walk(d, body + 4, stop, path, out)?;
            path.pop();
        } else {
            out.push((path.clone(), Chunk { id, data: &d[body..stop], at: body }));
        }
        off = stop + (size & 1);
    }
    Ok(())
}

fn quant(r: &mut Rd) -> Result<(u32, i32)> {
    let u = r.u32()?;
    Ok((u, if u <= 2 { r.i32()? } else { 0 }))
}

fn tbox(r: &mut Rd) -> Result<Box_> {
    Ok(Box_ { inst: r.guid()?, start: r.u32()?, len: r.u32()? })
}

/// A routable's output: (u16 size) GUID, channel layout, channel mask.
fn routable(r: &mut Rd) -> Result<Guid> {
    r.u16()?;
    let g = r.guid()?;
    r.u32()?;
    r.u32()?;
    Ok(g)
}

fn guids(r: &mut Rd) -> Result<Vec<Guid>> {
    r.list(|r| r.guid())
}

/// The bank's version (FMT chunk), or an error for a file that is not an FMOD Studio bank.
pub fn version(d: &[u8]) -> Result<u32> {
    if d.len() < 28 || &d[0..4] != b"RIFF" || &d[8..12] != b"FEV " || &d[12..16] != b"FMT " {
        return Err("not an FMOD Studio bank (RIFF FEV FMT)".into());
    }
    Ok(u32::from_le_bytes(d[20..24].try_into().unwrap()))
}

pub fn parse(d: &[u8]) -> Result<Bank> {
    let version = version(d)?;
    let mut chunks = Vec::new();
    let riff_end = (u32::from_le_bytes(d[4..8].try_into().unwrap()) as usize + 8).min(d.len());
    walk(d, 12, riff_end, &mut Vec::new(), &mut chunks)?;
    let mut b = Bank { version, ..Default::default() };
    // the object whose body opened the current LIST (by the LIST's depth), for chunks that join it
    let mut owner: Vec<(usize, Guid, [u8; 4])> = Vec::new();
    let mut sound_heads: Vec<(u32, u32)> = Vec::new();
    for (path, c) in &chunks {
        let depth = path.len();
        if &c.id == b"LIST" {
            owner.retain(|o| o.0 < depth);
            continue;
        }
        owner.retain(|o| o.0 <= depth);
        let parent = owner.iter().rev().find(|o| o.0 == depth).map(|o| (o.1, o.2));
        let mut r = Rd::new(c.data);
        let kind = String::from_utf8_lossy(&c.id).into_owned();
        if c.data.is_empty() {
            continue;
        }
        let res: Result<Option<(Guid, [u8; 4])>> = (|| {
            Ok(match &c.id {
                b"BNKI" => {
                    b.id = r.guid()?;
                    None
                }
                b"EVTB" => {
                    let mut e = Event { id: r.guid()?, snapshot: r.guid()?, timeline: r.guid()?, input_bus: r.guid()?, master: r.guid()?, ..Default::default() };
                    e.max_poly = r.i32()?;
                    e.priority = r.i32()?;
                    r.u8()?;
                    e.sched = r.i32()?;
                    e.layouts = guids(&mut r)?;
                    e.user_float = r.vlist(|r| Ok((r.string()?, r.f32()?)))?;
                    e.user_string = r.vlist(|r| Ok((r.string()?, r.string()?)))?;
                    r.f32()?; // doppler
                    e.steal = r.i32()?;
                    e.cooldown = r.f32()?;
                    e.flags = r.u32()?;
                    let id = e.id;
                    b.events.insert(id, e);
                    Some((id, c.id))
                }
                b"TLNB" => {
                    let id = r.guid()?;
                    r.guid()?; // (the event, before 0x6d)
                    let mut t = Timeline { id, ..Default::default() };
                    t.async_boxes = r.list(tbox)?;
                    t.locked = r.list(tbox)?;
                    r.list(|r| r.u32())?;
                    t.markers = r.vlist(|r| Ok(Marker { id: r.guid()?, pos: r.u32()?, name: r.string()? }))?;
                    t.tempo = r.list(|r| {
                        r.guid()?;
                        let sig = r.u64()?;
                        let pos = r.u32()?;
                        let bpm = r.f32()?;
                        Ok(Tempo { pos, bpm, upper: sig as u32, lower: (sig >> 32) as u32 })
                    })?;
                    b.timelines.insert(id, t);
                    Some((id, c.id))
                }
                b"TRNB" => {
                    let mut t = Transition { id: r.guid()?, dest: r.guid()?, start: r.u32()?, end: r.u32()?, ..Default::default() };
                    t.cond = r.list(|r| Ok((r.guid()?, r.f32()?, r.f32()?)))?;
                    t.quant = quant(&mut r)?;
                    t.chance = r.f32()?;
                    t.flags = r.u32()?;
                    r.u8()?;
                    r.u8()?;
                    // a transition region lies inside its timeline's LIST
                    if let Some(tl) = owner.iter().rev().find(|o| &o.2 == b"TLNB").and_then(|o| b.timelines.get_mut(&o.1)) {
                        tl.transitions.push(t);
                    }
                    None
                }
                b"TRTL" => None, // (transition timelines: fades and lead-ins, not played)
                b"CTRO" => {
                    let list = guids(&mut r)?;
                    match parent {
                        Some((g, k)) if &k == b"TLNB" => {
                            if let Some(t) = b.timelines.get_mut(&g) {
                                t.controllers.extend(list);
                            }
                        }
                        Some((g, k)) if &k == b"PMLB" => {
                            if let Some(l) = b.layouts.iter_mut().rev().find(|l| l.id == g) {
                                l.controllers.extend(list);
                            }
                        }
                        _ => {}
                    }
                    None
                }
                b"PMLB" => {
                    let mut l = Layout { id: r.guid()?, param: r.guid()?, event: r.guid()?, ..Default::default() };
                    l.boxes = r.list(|r| Ok((r.guid()?, r.f32()?, r.f32()?)))?;
                    let id = l.id;
                    b.layouts.push(l);
                    Some((id, c.id))
                }
                b"PRMB" => {
                    let id = r.guid()?;
                    r.u8()?;
                    let mut p = Param { id, kind: r.u32()?, name: r.string()?, ..Default::default() };
                    p.min = r.f32()?;
                    p.max = r.f32()?;
                    p.default = r.f32()?;
                    p.velocity = r.f32()?;
                    p.seek_up = r.f32()?;
                    r.u8()?;
                    p.seek_down = r.f32()?;
                    r.u8()?;
                    b.params.insert(id, p);
                    Some((id, c.id))
                }
                b"WAIB" | b"MUIB" | b"PRIB" | b"EVIB" | b"SPIB" | b"SIIB" | b"EFIB" | b"CMIB" => {
                    let id = r.guid()?;
                    let kind = match &c.id {
                        b"WAIB" => InstKind::Wave { wav: r.guid()? },
                        b"MUIB" => InstKind::Multi,
                        b"PRIB" => InstKind::Programmer { name: r.string()? },
                        b"EVIB" => InstKind::Event { event: r.guid()? },
                        b"SPIB" => InstKind::Scatterer,
                        b"SIIB" => InstKind::Other("silence"),
                        b"EFIB" => InstKind::Other("effect"),
                        _ => InstKind::Other("command"),
                    };
                    b.insts.insert(id, Inst { id, kind, body: None, playlist: None });
                    Some((id, c.id))
                }
                b"PLST" => {
                    let play = r.i32()?;
                    let sel = r.i32()?;
                    let e = r.list(|r| Ok((r.guid()?, r.f32()?)))?;
                    if r.left() == 1 {
                        r.u8()?; // (0x65 - 0x67: a flag)
                    }
                    if let Some(i) = parent.and_then(|p| b.insts.get_mut(&p.0)) {
                        i.playlist = Some((play, sel, e));
                    }
                    None
                }
                b"INST" => {
                    r.guid()?; // (the timeline it is on)
                    let mut i = InstBody { vol: r.f32()?, pitch: r.f32()?, loop_count: r.i32()?, flags: r.u8()?, ..Default::default() };
                    r.f32()?; // 3D offset distance
                    i.chance = r.f32()?;
                    i.delay = (r.f32()?, r.f32()?);
                    i.quant = quant(&mut r)?;
                    i.control_param = r.guid()?;
                    r.f32()?; // auto pitch reference
                    i.seek = r.f32()?;
                    i.max_poly = r.i32()?;
                    i.out = routable(&mut r)?;
                    i.steal = r.i32()?;
                    i.left_trim = r.u32()?;
                    i.seek_pct = r.f32()?;
                    r.f32()?; // auto pitch at minimum
                    if let Some(x) = parent.and_then(|p| b.insts.get_mut(&p.0)) {
                        x.body = Some(i);
                    }
                    None
                }
                b"WAV " => {
                    let id = r.guid()?;
                    r.u16()?;
                    let w = Wav { id, sound_bank: r.i32()?, subsound: r.i32()?, mode: r.u32()? };
                    b.wavs.insert(id, w);
                    None
                }
                b"IBSB" | b"GBSB" | b"MBSB" | b"RBSB" => {
                    let id = r.guid()?;
                    let out = routable(&mut r)?;
                    let kind = match &c.id {
                        b"IBSB" => BusKind::Input,
                        b"GBSB" => BusKind::Group,
                        b"MBSB" => BusKind::Master,
                        _ => BusKind::Return,
                    };
                    b.buses.insert(id, Bus { id, kind, out, vol: 0.0, pitch: 0.0, pre: vec![], post: vec![] });
                    Some((id, c.id))
                }
                b"BUS " => {
                    r.u8()?;
                    r.u32()?;
                    let pre = guids(&mut r)?;
                    let post = guids(&mut r)?;
                    r.u16()?;
                    let vol = r.f32()?;
                    let pitch = r.f32()?;
                    r.p = r.b.len(); // (polyphony, channel layouts: not used)
                    if let Some(x) = parent.and_then(|p| b.buses.get_mut(&p.0)) {
                        x.vol = vol;
                        x.pitch = pitch;
                        x.pre = pre;
                        x.post = post;
                    }
                    None
                }
                b"BEFB" => {
                    let e = Effect { id: r.guid()?, dsp: r.u32()?, bypass: r.u8()? != 0 };
                    let id = e.id;
                    b.effects.insert(id, e);
                    Some((id, c.id))
                }
                b"CTRL" => {
                    let x = Controller { id: r.guid()?, owner: r.guid()?, curve: r.guid()?, prop: r.i32()? };
                    b.controllers.insert(x.id, x);
                    None
                }
                b"CURV" => {
                    let id = r.guid()?;
                    let owner = r.guid()?;
                    let points = r.list(|r| Ok(Point { x: r.f32()?, y: r.f32()?, shape: r.f32()?, kind: r.u32()? }))?;
                    b.curves.push(Curve { id, owner, points });
                    None
                }
                b"MODB" => {
                    r.u16()?;
                    let id = r.guid()?;
                    let owner = r.guid()?;
                    let prop = r.i32()?;
                    let t = r.i32()?;
                    r.i32()?; // property type
                    let kind = match t {
                        0 => {
                            let mut a = [0f32; 10];
                            for x in a.iter_mut() {
                                *x = r.f32()?;
                            }
                            ModKind::Ahdsr(a)
                        }
                        1 => ModKind::Random(r.f32()?),
                        _ => {
                            r.p = r.b.len();
                            ModKind::Other(t)
                        }
                    };
                    b.modulators.push(Modulator { id, owner, prop, kind });
                    None
                }
                b"STBL" => {
                    r.u32()?;
                    let sound_bank = r.i32()?;
                    let n = r.x16()? as usize;
                    let keys = (0..n).map(|_| r.u64()).collect::<Result<Vec<_>>>()?;
                    let m = r.x16()? as usize;
                    let index = (0..m)
                        .map(|_| {
                            let t = r.take(3)?;
                            Ok(t[0] as u32 | (t[1] as u32) << 8 | (t[2] as u32) << 16)
                        })
                        .collect::<Result<Vec<_>>>()?;
                    b.sound_table = Some(SoundTable { sound_bank, keys, index });
                    None
                }
                b"STDT" => {
                    b.strings = Some(crate::strings::read(c.data)?);
                    r.p = r.b.len();
                    None
                }
                b"SNDH" => {
                    sound_heads = r.list(|r| Ok((r.u32()?, r.u32()?)))?;
                    None
                }
                b"SND " => {
                    let k = b.fsb.len();
                    // the FSB5 data starts where the header says (after alignment padding)
                    let at = sound_heads.get(k).map(|h| h.0 as usize).unwrap_or(c.at);
                    let len = sound_heads.get(k).map(|h| h.1 as usize).unwrap_or(c.data.len());
                    if at < c.at || at + len > c.at + c.data.len() {
                        return Err(format!("sound data {k} outside its chunk"));
                    }
                    b.fsb.push((at, len));
                    r.p = r.b.len();
                    None
                }
                _ => {
                    r.p = r.b.len();
                    None
                }
            })
        })();
        match res {
            Ok(Some((g, k))) => owner.push((depth, g, k)),
            Ok(None) => {}
            Err(e) => b.problems.push(format!("{kind} at {}: {e}", c.at)),
        }
        // a chunk the reader knows but did not read to its end: its layout is not this one
        if r.p != c.data.len() && !b.problems.last().is_some_and(|p| p.starts_with(&kind)) {
            if matches!(&c.id, b"EVTB" | b"TLNB" | b"PMLB" | b"PRMB" | b"INST" | b"WAV " | b"BUS " | b"CTRL" | b"CURV" | b"TRNB" | b"PLST" | b"MODB") {
                b.problems.push(format!("{kind} at {}: {} of {} bytes read", c.at, r.p, c.data.len()));
            }
        }
    }
    Ok(b)
}
