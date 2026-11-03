//! The HIRC chunk of a bank of version 128 (Wwise 2017.2): the objects step 1 plays (sounds,
//! containers, actor-mixers, buses, events and actions, states, the music hierarchy), read field
//! by field. Each object's size is in its header, so an object of another type is skipped, and one
//! that does not read to exactly its size is an error (a layout this reader has wrong).
use crate::rd::{Rd, Result};

/// Wwise's property ids (AkPropID, bank version 128) that step 1 reads.
pub mod prop {
    pub const VOLUME: u8 = 0x00;
    pub const PITCH: u8 = 0x02;
    pub const BUS_VOLUME: u8 = 0x05;
    pub const MAKEUP_GAIN: u8 = 0x06;
    pub const PAN_LR: u8 = 0x0C;
    pub const PAN_FR: u8 = 0x0D;
    pub const DELAY_TIME: u8 = 0x0F;
    pub const TRANSITION_TIME: u8 = 0x10;
    pub const PROBABILITY: u8 = 0x11;
    pub const OUTPUT_BUS_VOLUME: u8 = 0x18;
    pub const LOOP: u8 = 0x3A;
    pub const INITIAL_DELAY: u8 = 0x3B;
    /// Properties whose value is an integer (the others are floats).
    pub fn is_int(id: u8) -> bool {
        matches!(id, 0x07 | 0x0F | 0x10 | 0x2D..=0x35 | 0x37..=0x39 | 0x3A | 0x46)
    }
}

/// A property value: AkPropValue, a float or an integer by the property.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Val {
    F(f32),
    I(i32),
}

impl Val {
    pub fn f(self) -> f64 {
        match self {
            Val::F(x) => x as f64,
            Val::I(x) => x as f64,
        }
    }
}

fn val(id: u8, raw: u32) -> Val {
    if prop::is_int(id) {
        Val::I(raw as i32)
    } else {
        Val::F(f32::from_bits(raw))
    }
}

#[derive(Clone, Debug, Default)]
pub struct Props {
    pub set: Vec<(u8, Val)>,
    pub ranged: Vec<(u8, Val, Val)>,
}

impl Props {
    pub fn get(&self, id: u8) -> Option<Val> {
        self.set.iter().find(|p| p.0 == id).map(|p| p.1)
    }
    pub fn f(&self, id: u8) -> f64 {
        self.get(id).map_or(0.0, Val::f)
    }
    pub fn range(&self, id: u8) -> Option<(f64, f64)> {
        self.ranged
            .iter()
            .find(|p| p.0 == id)
            .map(|p| (p.1.f(), p.2.f()))
    }
}

fn props(r: &mut Rd, ranged: bool) -> Result<Props> {
    let n = r.u8()? as usize;
    let ids = r.take(n)?.to_vec();
    let mut p = Props::default();
    for &id in &ids {
        let raw = r.u32()?;
        p.set.push((id, val(id, raw)));
    }
    if ranged {
        let n = r.u8()? as usize;
        let ids = r.take(n)?.to_vec();
        for &id in &ids {
            let (lo, hi) = (r.u32()?, r.u32()?);
            p.ranged.push((id, val(id, lo), val(id, hi)));
        }
    }
    Ok(p)
}

/// One RTPC curve on an object (InitialRTPC).
#[derive(Clone, Debug)]
pub struct Rtpc {
    pub id: u32,
    pub kind: u8,
    pub param: u32,
    pub scaling: u8,
    pub points: Vec<(f32, f32, u32)>,
}

fn points(r: &mut Rd, n: u32) -> Result<Vec<(f32, f32, u32)>> {
    let n = r.count(n, 12)?;
    (0..n).map(|_| Ok((r.f32()?, r.f32()?, r.u32()?))).collect()
}

fn rtpcs(r: &mut Rd) -> Result<Vec<Rtpc>> {
    let n = r.u16()?;
    let mut v = Vec::new();
    for _ in 0..r.count(n as u32, 12)? {
        let id = r.u32()?;
        let kind = r.u8()?;
        let _accum = r.u8()?;
        let param = r.var()?;
        let _curve = r.u32()?;
        let scaling = r.u8()?;
        let np = r.u16()?;
        v.push(Rtpc {
            id,
            kind,
            param,
            scaling,
            points: points(r, np as u32)?,
        });
    }
    Ok(v)
}

/// A state group's states on an object: (group, sync type, [(state, its CAkState object)]).
pub type StateGroup = (u32, Vec<(u32, u32)>);

fn state_chunk(r: &mut Rd) -> Result<Vec<StateGroup>> {
    let n = r.var()?;
    for _ in 0..r.count(n, 3)? {
        r.var()?; // property id
        r.u8()?; // accumulation
        r.u8()?; // in dB
    }
    let n = r.var()?;
    let mut v = Vec::new();
    for _ in 0..r.count(n, 6)? {
        let g = r.u32()?;
        r.u8()?; // sync type
        let ns = r.var()?;
        let mut s = Vec::new();
        for _ in 0..r.count(ns, 8)? {
            s.push((r.u32()?, r.u32()?));
        }
        v.push((g, s));
    }
    Ok(v)
}

fn positioning(r: &mut Rd) -> Result<()> {
    let bits = r.u8()?;
    if bits & 1 != 0 && (bits >> 4) & 1 != 0 {
        let bits3d = r.u8()?;
        r.u32()?; // attenuation
        if (bits3d >> 6) & 1 != 1 {
            // an automated 3D path
            r.u8()?; // path mode
            r.i32()?; // transition time
            let nv = r.u32()?;
            let nv = r.count(nv, 16)?;
            r.skip(nv * 16)?;
            let ni = r.u32()?;
            let ni = r.count(ni, 8 + 12)?;
            r.skip(ni * 8 + ni * 12)?;
        }
    }
    Ok(())
}

fn aux(r: &mut Rd) -> Result<()> {
    let bits = r.u8()?;
    if (bits >> 3) & 1 != 0 {
        r.skip(16)?;
    }
    Ok(())
}

fn fx(r: &mut Rd) -> Result<()> {
    r.u8()?; // override parent
    let n = r.u8()? as usize;
    if n > 0 {
        r.u8()?; // bypass bits
        r.skip(n * 7)?; // index, fx id, share set, rendered
    }
    Ok(())
}

/// NodeBaseParams: what every node of the actor-mixer and music hierarchies has.
#[derive(Clone, Debug, Default)]
pub struct Base {
    pub bus: u32,
    pub parent: u32,
    pub props: Props,
    pub limit: u16,
    /// at the limit, a new instance is refused (else the oldest stops)
    pub kill_newest: bool,
    pub states: Vec<StateGroup>,
    pub rtpcs: Vec<Rtpc>,
}

fn base(r: &mut Rd) -> Result<Base> {
    fx(r)?;
    r.u8()?; // override attachment params
    let bus = r.u32()?;
    let parent = r.u32()?;
    r.u8()?; // priority bits
    let props = props(r, true)?;
    positioning(r)?;
    aux(r)?;
    // AdvSettingsParams
    let kill_newest = r.u8()? & 1 != 0;
    r.u8()?;
    let limit = r.u16()?;
    r.u8()?;
    r.u8()?;
    let states = state_chunk(r)?;
    let rtpcs = rtpcs(r)?;
    Ok(Base {
        bus,
        parent,
        props,
        limit,
        kill_newest,
        states,
        rtpcs,
    })
}

fn children(r: &mut Rd) -> Result<Vec<u32>> {
    let n = r.u32()?;
    r.ids(n)
}

/// AkBankSourceData: a sound's or a music track's media.
#[derive(Clone, Copy, Debug, Default)]
pub struct Source {
    pub plugin: u32,
    pub stream: u8,
    pub id: u32,
    pub size: u32,
}

fn source(r: &mut Rd) -> Result<Source> {
    let plugin = r.u32()?;
    let stream = r.u8()?;
    let id = r.u32()?;
    let size = r.u32()?;
    r.u8()?; // source bits
    if plugin & 0x0f == 2 {
        let n = r.u32()? as usize;
        r.skip(n)?;
    }
    Ok(Source {
        plugin,
        stream,
        id,
        size,
    })
}

/// A music transition rule (the fades and sync; the rest of the rule is read and dropped).
#[derive(Clone, Debug)]
pub struct Rule {
    pub src: Vec<u32>,
    pub dst: Vec<u32>,
    pub fade_out: i32,
    pub fade_in: i32,
    pub sync: u32,
    pub segment: u32,
}

/// MusicNodeParams (+ MusicTransNodeParams' rules when `trans`).
fn music(r: &mut Rd, trans: bool) -> Result<(Base, Vec<u32>, Vec<Rule>)> {
    r.u8()?; // flags
    let b = base(r)?;
    let ch = children(r)?;
    r.skip(8 + 8 + 4 + 1 + 1)?; // meter
    r.u8()?;
    let ns = r.u32()?;
    let ns = r.count(ns, 24)?;
    r.skip(ns * 24)?; // stingers
    let mut rules = Vec::new();
    if trans {
        let n = r.u32()?;
        for _ in 0..r.count(n, 8)? {
            let ns = r.u32()?;
            let src = r.ids(ns)?;
            let nd = r.u32()?;
            let dst = r.ids(nd)?;
            let fade_out = r.i32()?;
            r.skip(8)?; // curve, offset
            let sync = r.u32()?;
            r.skip(5)?; // cue filter, play post-exit
            let fade_in = r.i32()?;
            r.skip(20)?; // curve, offset, cue filter, jump to, entry type, pre-entry, match cue name
            let mut segment = 0;
            if r.u8()? != 0 {
                segment = r.u32()?;
                r.skip(24 + 2)?;
            }
            rules.push(Rule {
                src,
                dst,
                fade_out,
                fade_in,
                sync,
                segment,
            });
        }
    }
    Ok((b, ch, rules))
}

/// A music playlist's item, a tree.
#[derive(Clone, Debug)]
pub struct Item {
    pub segment: u32,
    pub kind: u32,
    pub loop_count: i16,
    pub weight: u32,
    pub avoid: u16,
    pub shuffle: bool,
    pub children: Vec<Item>,
}

fn item(r: &mut Rd, depth: u32) -> Result<Item> {
    if depth > 64 {
        return Err("a playlist nested too deep".into());
    }
    let segment = r.u32()?;
    r.u32()?; // item id
    let n = r.u32()?;
    let kind = r.u32()?;
    let loop_count = r.i16()?;
    r.skip(4)?; // loop min, max
    let weight = r.u32()?;
    let avoid = r.u16()?;
    r.u8()?; // using weight
    let shuffle = r.u8()? != 0;
    let mut children = Vec::new();
    for _ in 0..r.count(n, 30)? {
        children.push(item(r, depth + 1)?);
    }
    Ok(Item {
        segment,
        kind,
        loop_count,
        weight,
        avoid,
        shuffle,
        children,
    })
}

/// A music switch's decision tree: a node id, or values (0: any) to subtrees.
#[derive(Clone, Debug)]
pub enum Tree {
    Leaf(u32),
    Node(Vec<(u32, Tree)>),
}

fn tree(data: &[u8], depth: u32) -> Result<Tree> {
    let n = data.len() / 12;
    let at = |i: usize| -> (u32, u32, u16, u16) {
        let o = i * 12;
        let u = |k| u32::from_le_bytes(data[o + k..o + k + 4].try_into().unwrap());
        let w = u(4);
        (u(0), w, (w & 0xffff) as u16, (w >> 16) as u16)
    };
    fn walk(
        at: &dyn Fn(usize) -> (u32, u32, u16, u16),
        n: usize,
        i: usize,
        d: u32,
        depth: u32,
    ) -> Result<Tree> {
        let (_, id, idx, cnt) = at(i);
        if d == depth {
            return Ok(Tree::Leaf(id));
        }
        let (idx, cnt) = (idx as usize, cnt as usize);
        if idx + cnt > n || idx <= i {
            return Err(format!(
                "a decision tree node out of range ({idx}+{cnt} of {n})"
            ));
        }
        let mut v = Vec::new();
        for c in idx..idx + cnt {
            v.push((at(c).0, walk(at, n, c, d + 1, depth)?));
        }
        Ok(Tree::Node(v))
    }
    if n == 0 {
        return Err("an empty decision tree".into());
    }
    walk(&at, n, 0, 0, depth)
}

#[derive(Clone, Debug)]
pub struct Clip {
    pub sub: u32,
    pub media: u32,
    pub at: f64,
    pub begin: f64,
    pub end: f64,
    pub src: f64,
}

#[derive(Clone, Debug)]
pub struct TrackSwitch {
    pub state: bool,
    pub group: u32,
    pub default: u32,
    pub assoc: Vec<u32>,
}

/// An action's own parameters, for the types step 1 plays.
#[derive(Clone, Debug, Default)]
pub struct ActionParams {
    pub curve: u8,
    pub except: Vec<(u32, bool)>,
    pub group: u32,
    pub value: u32,
    pub meaning: u8,
    pub amount: f32,
    pub bank: u32,
}

#[derive(Clone, Debug)]
pub enum Obj {
    State {
        props: Vec<(u16, f32)>,
    },
    Sound {
        src: Source,
        base: Base,
    },
    Action {
        ty: u16,
        target: u32,
        is_bus: bool,
        props: Props,
        params: ActionParams,
    },
    Event {
        actions: Vec<u32>,
    },
    RanSeq {
        base: Base,
        loop_count: u16,
        trans_time: f32,
        avoid: u16,
        trans_mode: u8,
        random_mode: u8,
        mode: u8,
        bits: u8,
        children: Vec<u32>,
        playlist: Vec<(u32, i32)>,
    },
    Switch {
        base: Base,
        state: bool,
        group: u32,
        default: u32,
        continuous: bool,
        children: Vec<u32>,
        map: Vec<(u32, Vec<u32>)>,
    },
    Mixer {
        base: Base,
        children: Vec<u32>,
    },
    Layer {
        base: Base,
        children: Vec<u32>,
    },
    Bus {
        parent: u32,
        props: Props,
        rtpcs: Vec<Rtpc>,
        states: Vec<StateGroup>,
    },
    Segment {
        base: Base,
        children: Vec<u32>,
        duration: f64,
        markers: Vec<(u32, f64)>,
    },
    Track {
        base: Base,
        sources: Vec<Source>,
        clips: Vec<Clip>,
        subtracks: u32,
        kind: u8,
        switch: Option<TrackSwitch>,
    },
    MusicSwitch {
        base: Base,
        children: Vec<u32>,
        rules: Vec<Rule>,
        cont: bool,
        args: Vec<(u32, u8)>,
        tree: Tree,
    },
    Playlist {
        base: Base,
        children: Vec<u32>,
        rules: Vec<Rule>,
        items: Option<Item>,
    },
    Other,
}

/// HIRC types (bank version 128).
pub mod ty {
    pub const STATE: u8 = 1;
    pub const SOUND: u8 = 2;
    pub const ACTION: u8 = 3;
    pub const EVENT: u8 = 4;
    pub const RANSEQ: u8 = 5;
    pub const SWITCH: u8 = 6;
    pub const MIXER: u8 = 7;
    pub const BUS: u8 = 8;
    pub const LAYER: u8 = 9;
    pub const SEGMENT: u8 = 10;
    pub const TRACK: u8 = 11;
    pub const MUSIC_SWITCH: u8 = 12;
    pub const PLAYLIST: u8 = 13;
    pub const AUX_BUS: u8 = 18;
}

fn action(r: &mut Rd) -> Result<Obj> {
    let ty = r.u16()?;
    let target = r.u32()?;
    let is_bus = r.u8()? & 1 != 0;
    let props = props(r, true)?;
    let mut p = ActionParams::default();
    let except = |r: &mut Rd| -> Result<Vec<(u32, bool)>> {
        let n = r.var()?;
        let n = r.count(n, 5)?;
        (0..n).map(|_| Ok((r.u32()?, r.u8()? != 0))).collect()
    };
    match ty >> 8 {
        0x01..=0x03 => {
            p.curve = r.u8()? & 0x1f;
            r.u8()?;
            p.except = except(r)?;
        }
        0x04 => {
            p.curve = r.u8()? & 0x1f;
            p.bank = r.u32()?;
        }
        0x08..=0x0F | 0x20 | 0x30 => {
            p.curve = r.u8()? & 0x1f;
            p.meaning = r.u8()?;
            p.amount = r.f32()?;
            r.skip(8)?;
            p.except = except(r)?;
        }
        0x12 | 0x19 => {
            p.group = r.u32()?;
            p.value = r.u32()?;
        }
        0x13 | 0x14 => {
            p.curve = r.u8()? & 0x1f;
            r.u8()?; // bypass transition
            p.meaning = r.u8()?;
            p.amount = r.f32()?;
            r.skip(8)?;
            p.except = except(r)?;
        }
        _ => {
            r.p = r.b.len(); // (not played: the rest is not read)
        }
    }
    Ok(Obj::Action {
        ty,
        target,
        is_bus,
        props,
        params: p,
    })
}

fn bus(r: &mut Rd) -> Result<Obj> {
    let parent = r.u32()?;
    if parent == 0 {
        r.u32()?; // audio device share set
    }
    let props = props(r, false)?;
    positioning(r)?;
    aux(r)?;
    r.skip(1 + 2 + 4 + 1)?; // bits, max instances, channel config, HDR bits
    r.skip(8)?; // recovery time, max duck volume
    let nd = r.u32()?;
    let nd = r.count(nd, 18)?;
    r.skip(nd * 18)?;
    let nfx = r.u8()? as usize;
    if nfx > 0 {
        r.u8()?;
        r.skip(nfx * 7)?;
    }
    r.skip(5)?; // fx 0, share set
    r.u8()?; // override attachment params
    let rtpcs = rtpcs(r)?;
    let states = state_chunk(r)?;
    Ok(Obj::Bus {
        parent,
        props,
        rtpcs,
        states,
    })
}

fn track(r: &mut Rd) -> Result<Obj> {
    r.u8()?; // flags
    let ns = r.u32()?;
    let mut sources = Vec::new();
    for _ in 0..r.count(ns, 14)? {
        sources.push(source(r)?);
    }
    let np = r.u32()?;
    let mut clips = Vec::new();
    let mut subtracks = 0;
    if np > 0 {
        for _ in 0..r.count(np, 40)? {
            let sub = r.u32()?;
            let media = r.u32()?;
            clips.push(Clip {
                sub,
                media,
                at: r.f64()?,
                begin: r.f64()?,
                end: r.f64()?,
                src: r.f64()?,
            });
        }
        subtracks = r.u32()?;
    }
    let na = r.u32()?;
    for _ in 0..r.count(na, 12)? {
        r.skip(8)?;
        let n = r.u32()?;
        points(r, n)?;
    }
    let base = base(r)?;
    let kind = r.u8()?;
    let mut switch = None;
    if kind == 3 {
        let state = r.u8()? == 1;
        let group = r.u32()?;
        let default = r.u32()?;
        let n = r.u32()?;
        let assoc = r.ids(n)?;
        r.skip(12 + 8 + 12)?; // trans params
        switch = Some(TrackSwitch {
            state,
            group,
            default,
            assoc,
        });
    }
    r.i32()?; // look-ahead
    Ok(Obj::Track {
        base,
        sources,
        clips,
        subtracks,
        kind,
        switch,
    })
}

/// One object's body (after its type, size and id).
pub fn parse(t: u8, body: &[u8]) -> Result<Obj> {
    let r = &mut Rd::new(body);
    let o = match t {
        ty::STATE => {
            let n = r.u16()? as usize;
            let mut ids = Vec::new();
            for _ in 0..n {
                ids.push(r.u16()?);
            }
            let mut props = Vec::new();
            for id in ids {
                props.push((id, r.f32()?));
            }
            Obj::State { props }
        }
        ty::SOUND => {
            let src = source(r)?;
            Obj::Sound {
                src,
                base: base(r)?,
            }
        }
        ty::ACTION => action(r)?,
        ty::EVENT => {
            let n = r.var()?;
            Obj::Event { actions: r.ids(n)? }
        }
        ty::RANSEQ => {
            let base = base(r)?;
            let loop_count = r.u16()?;
            r.skip(4)?;
            let trans_time = r.f32()?;
            r.skip(8)?;
            let avoid = r.u16()?;
            let trans_mode = r.u8()?;
            let random_mode = r.u8()?;
            let mode = r.u8()?;
            let bits = r.u8()?;
            let children = children(r)?;
            let n = r.u16()?;
            let mut playlist = Vec::new();
            for _ in 0..r.count(n as u32, 8)? {
                playlist.push((r.u32()?, r.i32()?));
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
            }
        }
        ty::SWITCH => {
            let base = base(r)?;
            let state = r.u8()? == 1;
            let group = r.u32()?;
            let default = r.u32()?;
            let continuous = r.u8()? != 0;
            let children = children(r)?;
            let n = r.u32()?;
            let mut map = Vec::new();
            for _ in 0..r.count(n, 8)? {
                let sw = r.u32()?;
                let k = r.u32()?;
                map.push((sw, r.ids(k)?));
            }
            let n = r.u32()?;
            let n = r.count(n, 14)?;
            r.skip(n * 14)?;
            Obj::Switch {
                base,
                state,
                group,
                default,
                continuous,
                children,
                map,
            }
        }
        ty::MIXER => {
            let base = base(r)?;
            Obj::Mixer {
                base,
                children: children(r)?,
            }
        }
        ty::LAYER => {
            let base = base(r)?;
            let children = children(r)?;
            let n = r.u32()?;
            for _ in 0..r.count(n, 15)? {
                r.u32()?; // layer id
                rtpcs(r)?;
                r.u32()?; // crossfade rtpc
                r.u8()?;
                let na = r.u32()?;
                for _ in 0..r.count(na, 8)? {
                    r.u32()?;
                    let np = r.u32()?;
                    points(r, np)?;
                }
            }
            r.u8()?; // continuous validation
            Obj::Layer { base, children }
        }
        ty::BUS | ty::AUX_BUS => bus(r)?,
        ty::SEGMENT => {
            let (base, children, _) = music(r, false)?;
            let duration = r.f64()?;
            let n = r.u32()?;
            let mut markers = Vec::new();
            for _ in 0..r.count(n, 16)? {
                let id = r.u32()?;
                let pos = r.f64()?;
                let ns = r.u32()? as usize;
                r.skip(ns)?;
                markers.push((id, pos));
            }
            Obj::Segment {
                base,
                children,
                duration,
                markers,
            }
        }
        ty::TRACK => track(r)?,
        ty::MUSIC_SWITCH => {
            let (base, children, rules) = music(r, true)?;
            let cont = r.u8()? != 0;
            let depth = r.u32()?;
            let d = r.count(depth, 5)?;
            let groups = r.ids(d as u32)?;
            let kinds = r.take(d)?.to_vec();
            let size = r.u32()? as usize;
            r.u8()?; // mode
            let data = r.take(size)?;
            let tree = tree(data, depth)?;
            Obj::MusicSwitch {
                base,
                children,
                rules,
                cont,
                args: groups.into_iter().zip(kinds).collect(),
                tree,
            }
        }
        ty::PLAYLIST => {
            let (base, children, rules) = music(r, true)?;
            let n = r.u32()?;
            let items = if n > 0 { Some(item(r, 0)?) } else { None };
            Obj::Playlist {
                base,
                children,
                rules,
                items,
            }
        }
        _ => return Ok(Obj::Other),
    };
    if r.left() != 0 {
        return Err(format!("{} bytes left unread", r.left()));
    }
    Ok(o)
}

/// The objects of a HIRC chunk: (type, id, object or the error reading it).
pub fn objects(hirc: &[u8]) -> Result<Vec<(u8, u32, Result<Obj>)>> {
    let r = &mut Rd::new(hirc);
    let n = r.u32()?;
    let mut v = Vec::new();
    for _ in 0..r.count(n, 9)? {
        let t = r.u8()?;
        let size = r.u32()? as usize;
        let body = r.take(size)?;
        if body.len() < 4 {
            return Err("an object smaller than its id".into());
        }
        let id = u32::from_le_bytes(body[..4].try_into().unwrap());
        v.push((t, id, parse(t, &body[4..])));
    }
    Ok(v)
}
