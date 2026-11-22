//! The GL state a draw depends on, followed call by call: buffers' bytes, vertex arrays, programs'
//! uniforms, texture bindings and a version of each texture's contents, framebuffer attachments,
//! the fixed-function state. A draw is then described by what it reads, not by the calls that set it.
use crate::trace::{h64, hmix, Blob, Rec};
use std::collections::{BTreeMap, HashMap};
use std::rc::Rc;

pub const ARRAY_BUFFER: u32 = 0x8892;
pub const ELEMENT_ARRAY_BUFFER: u32 = 0x8893;
const FRAMEBUFFER: u32 = 0x8D40;
const READ_FRAMEBUFFER: u32 = 0x8CA8;
const COLOR_ATTACHMENT0: u32 = 0x8CE0;
const DEPTH_ATTACHMENT: u32 = 0x8D00;
const STENCIL_ATTACHMENT: u32 = 0x8D20;
const DEPTH_STENCIL_ATTACHMENT: u32 = 0x821A;
pub const BLEND: u32 = 0x0BE2;
pub const DEPTH_TEST: u32 = 0x0B71;
pub const STENCIL_TEST: u32 = 0x0B90;
pub const SCISSOR_TEST: u32 = 0x0C11;
pub const CULL_FACE: u32 = 0x0B44;

// kinds of fixed-function state (the first half of a key; the second: a capability, a face, a unit)
pub const K_CAP: u32 = 1;
pub const K_BLENDFUNC: u32 = 2;
pub const K_BLENDEQ: u32 = 3;
pub const K_DEPTHFUNC: u32 = 4;
pub const K_DEPTHMASK: u32 = 5;
pub const K_COLORMASK: u32 = 6;
pub const K_STENCILFUNC: u32 = 7;
pub const K_STENCILOP: u32 = 8;
pub const K_STENCILMASK: u32 = 9;
pub const K_VIEWPORT: u32 = 10;
pub const K_SCISSOR: u32 = 11;
pub const K_FRONTFACE: u32 = 12;
pub const K_CULLFACE: u32 = 13;
pub const K_OTHER: u32 = 14; // (second half: the entry point's number)
pub const K_SAMPLER: u32 = 15;

pub fn kind_name(k: (u32, u32), names: &[String]) -> String {
    match k.0 {
        K_CAP => format!("glEnable/Disable(0x{:04x})", k.1),
        K_BLENDFUNC => "blend function".into(),
        K_BLENDEQ => "blend equation".into(),
        K_DEPTHFUNC => "depth function".into(),
        K_DEPTHMASK => "depth mask".into(),
        K_COLORMASK => "colour mask".into(),
        K_STENCILFUNC => "stencil function".into(),
        K_STENCILOP => "stencil operation".into(),
        K_STENCILMASK => "stencil mask".into(),
        K_VIEWPORT => "viewport".into(),
        K_SCISSOR => "scissor".into(),
        K_FRONTFACE => "front face".into(),
        K_CULLFACE => "cull face".into(),
        K_SAMPLER => format!("sampler object, unit {}", k.1),
        _ => names.get(k.1 as usize).cloned().unwrap_or_default(),
    }
}

#[derive(Clone, Copy, Default)]
pub struct Attr {
    pub enabled: bool,
    pub size: u32,
    pub ty: u32,
    pub norm: u32,
    pub stride: u32,
    pub ptr: u64,
    pub buf: u32,
    pub integer: bool,
}
#[derive(Default)]
pub struct Vao {
    pub elem: u32,
    pub attr: [Attr; 16],
}
#[derive(Default)]
pub struct Tex {
    pub version: u64,
    pub w: u32,
    pub h: u32,
    pub samples: u32,
    pub phash: u64,
    params: HashMap<u32, u64>,
    regions: HashMap<u64, u64>,
}
pub struct UVal {
    pub op: u32,
    pub data: Vec<u8>,
    pub hash: u64,
}
#[derive(Default)]
pub struct Program {
    pub uni: BTreeMap<i32, Rc<UVal>>,
    pub names: HashMap<String, i32>,
    pub attr_names: HashMap<String, u32>,
    pub shaders: Vec<u32>,
    /// the sources as they were at the link (shader names are used again after it): (type, source)
    pub srcs: Vec<(u32, Rc<String>)>,
    /// sampler uniforms: (name, location, texture target), from the sources
    pub samplers: Option<Rc<Vec<(String, i32, u32)>>>,
    pub label: Option<Rc<String>>,
    pub vs_has_pos: bool,
    pub fs_discards: bool,
}
impl Program {
    pub fn name_of(&self, loc: i32) -> String {
        self.names.iter().find(|(_, l)| **l == loc).map(|(n, _)| n.clone()).unwrap_or_else(|| format!("location {loc}"))
    }
    fn loc(&self, n: &str) -> Option<i32> {
        self.names.get(n).or_else(|| self.names.get(&format!("{n}[0]"))).copied().filter(|l| *l >= 0)
    }
    fn mat(&self, n: &str) -> Option<[f32; 16]> {
        let v = self.uni.get(&self.loc(n)?)?;
        if v.data.len() < 64 {
            return None;
        }
        let mut m = [0f32; 16];
        for (i, c) in v.data.chunks_exact(4).take(16).enumerate() {
            m[i] = f32::from_le_bytes(c.try_into().unwrap());
        }
        Some(m)
    }
}
#[derive(Default, Clone, Copy)]
pub struct Fbo {
    pub color: u32,
    pub color_rb: bool,
    pub depth: u32,
    pub msaa: bool,
}
pub type FixedSnap = Rc<Vec<((u32, u32), u64)>>;
#[derive(Default)]
pub struct Ctx {
    active: u32,
    units: HashMap<(u32, u32), u32>,
    bufs: HashMap<u32, u32>,
    vao: u32,
    pub prog: u32,
    pub dfbo: u32,
    rfbo: u32,
    pub fixed: BTreeMap<(u32, u32), Vec<u64>>,
    snap: Option<FixedSnap>,
    gattr: [[u8; 16]; 16],
    maps: HashMap<u32, u64>,
}
impl Ctx {
    fn set(&mut self, k: (u32, u32), v: Vec<u64>) {
        if self.fixed.get(&k) != Some(&v) {
            self.fixed.insert(k, v);
            self.snap = None;
        }
    }
    pub fn cap(&self, c: u32) -> bool {
        self.fixed.get(&(K_CAP, c)).map(|v| v[0] != 0).unwrap_or(false)
    }
    pub fn get(&self, k: (u32, u32)) -> Option<&Vec<u64>> {
        self.fixed.get(&k)
    }
    pub fn snapshot(&mut self) -> FixedSnap {
        if self.snap.is_none() {
            let v: Vec<_> = self.fixed.iter().map(|(k, v)| {
                let mut h = 0u64;
                for x in v {
                    h = hmix(h, *x);
                }
                (*k, h)
            }).collect();
            self.snap = Some(Rc::new(v));
        }
        self.snap.clone().unwrap()
    }
}

pub struct TexRef {
    pub loc: i32,
    /// the texture's contents (its uploads, or the pass that drew it) and size: not its name, a
    /// pooled render texture comes back under another
    pub version: u64,
    pub phash: u64,
}
pub struct DrawInfo {
    pub prog: u32,
    pub label: Rc<String>,
    pub params: u64,
    pub count: u32,
    pub h_idx: u64,
    pub h_pos: u64,
    pub h_attr: u64,
    pub uni: Vec<(i32, Rc<UVal>)>,
    pub tex: Vec<TexRef>,
    pub fixed: FixedSnap,
    /// fragments rasterized, estimated from the triangles (None: the vertex transform is not known)
    pub weight: Option<f64>,
    pub how: u8, // 0 by the matrices, 1 whole viewport (no position attribute), 2 unknown
    /// no triangle can rasterize: each has two identical corners or lies wholly outside one clip plane
    pub empty: bool,
    pub bbox: [f32; 4],
    pub blend: bool,
    pub depth_write: bool,
    pub stencil_write: bool,
    pub opaque_state: bool,
    pub discards: bool,
}
pub enum Event {
    None,
    Draw(Box<DrawInfo>),
    Clear { mask: u32, extra: u64, full: bool },
    Blit { src: u64, args: u64, area: f64, full_w: i64, full_h: i64 },
    BindDrawFbo { old: u32 },
    Unmodelled,
}

#[derive(Default)]
pub struct State {
    pub ctx: HashMap<u8, Ctx>,
    pub bufs: HashMap<u32, Vec<u8>>,
    vaos: HashMap<(u8, u32), Vao>,
    pub tex: HashMap<u32, Tex>,
    pub progs: HashMap<u32, Program>,
    shaders: HashMap<u32, (u32, Rc<String>)>, // (type, source)
    pub fbos: HashMap<u32, Fbo>,
    rbs: HashMap<u32, (u32, u32, u32, u64)>,
    fb0_version: u64,
    uniq: u64,
    pub msaa_seen: u64,
    pub ubo_seen: u64,
    pub tex_uploads: u64,
    pub flush_nodata: u64,
    pub tex_uploads_nopixels: u64,
}

fn type_size(t: u32) -> usize {
    match t {
        0x1400 | 0x1401 => 1,
        0x1402 | 0x1403 | 0x140B => 2,
        _ => 4,
    }
}
fn mul(a: &[f32; 16], b: &[f32; 16]) -> [f32; 16] {
    // column-major: (a*b)[c][r] = sum_k a[k][r] * b[c][k]
    let mut m = [0f32; 16];
    for c in 0..4 {
        for r in 0..4 {
            let mut s = 0f32;
            for k in 0..4 {
                s += a[k * 4 + r] * b[c * 4 + k];
            }
            m[c * 4 + r] = s;
        }
    }
    m
}
fn xf(m: &[f32; 16], p: [f32; 4]) -> [f64; 4] {
    let mut o = [0f64; 4];
    for r in 0..4 {
        o[r] = (m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r] * p[3]) as f64;
    }
    o
}
/// a triangle in clip space cut to the view volume, then to window coordinates
fn clip_tri(t: [[f64; 4]; 3], vp: [f64; 4]) -> Vec<[f64; 2]> {
    let mut poly: Vec<[f64; 4]> = t.to_vec();
    for pl in 0..6 {
        let d = |v: &[f64; 4]| match pl {
            0 => v[3] + v[0],
            1 => v[3] - v[0],
            2 => v[3] + v[1],
            3 => v[3] - v[1],
            4 => v[3] + v[2],
            _ => v[3] - v[2],
        };
        let mut out = Vec::with_capacity(poly.len() + 2);
        for i in 0..poly.len() {
            let (a, b) = (&poly[i], &poly[(i + 1) % poly.len()]);
            let (da, db) = (d(a), d(b));
            if da >= 0.0 {
                out.push(*a);
            }
            if (da >= 0.0) != (db >= 0.0) {
                let s = da / (da - db);
                out.push([a[0] + s * (b[0] - a[0]), a[1] + s * (b[1] - a[1]), a[2] + s * (b[2] - a[2]), a[3] + s * (b[3] - a[3])]);
            }
        }
        poly = out;
        if poly.len() < 3 {
            return Vec::new();
        }
    }
    poly.iter().filter(|v| v[3] > 1e-9).map(|v| [vp[0] + (v[0] / v[3] + 1.0) * 0.5 * vp[2], vp[1] + (v[1] / v[3] + 1.0) * 0.5 * vp[3]]).collect()
}
fn clip_rect(poly: Vec<[f64; 2]>, r: [f64; 4]) -> Vec<[f64; 2]> {
    let mut poly = poly;
    for e in 0..4 {
        let d = |v: &[f64; 2]| match e {
            0 => v[0] - r[0],
            1 => r[0] + r[2] - v[0],
            2 => v[1] - r[1],
            _ => r[1] + r[3] - v[1],
        };
        let mut out = Vec::with_capacity(poly.len() + 2);
        for i in 0..poly.len() {
            let (a, b) = (&poly[i], &poly[(i + 1) % poly.len()]);
            let (da, db) = (d(a), d(b));
            if da >= 0.0 {
                out.push(*a);
            }
            if (da >= 0.0) != (db >= 0.0) {
                let s = da / (da - db);
                out.push([a[0] + s * (b[0] - a[0]), a[1] + s * (b[1] - a[1])]);
            }
        }
        poly = out;
        if poly.len() < 3 {
            return Vec::new();
        }
    }
    poly
}
fn area(p: &[[f64; 2]]) -> f64 {
    let mut s = 0.0;
    for i in 0..p.len() {
        let (a, b) = (p[i], p[(i + 1) % p.len()]);
        s += a[0] * b[1] - b[0] * a[1];
    }
    s * 0.5
}

impl State {
    /// a program's shader sources (for --src)
    pub fn sources(&self, prog: u32) -> Vec<Rc<String>> {
        self.progs.get(&prog).map(|p| p.srcs.iter().map(|x| x.1.clone()).collect()).unwrap_or_default()
    }
    fn uniq(&mut self) -> u64 {
        self.uniq += 1;
        hmix(0x51ed, self.uniq)
    }
    fn buf_of(&self, th: u8, target: u32) -> u32 {
        let c = &self.ctx[&th];
        if target == ELEMENT_ARRAY_BUFFER {
            self.vaos.get(&(th, c.vao)).map(|v| v.elem).unwrap_or(0)
        } else {
            c.bufs.get(&target).copied().unwrap_or(0)
        }
    }
    fn write(&mut self, buf: u32, off: u64, b: Option<&Blob>) {
        let (Some(b), Some(d)) = (b, self.bufs.get_mut(&buf)) else { return };
        let off = off as usize;
        let n = b.len();
        if off + n > d.len() {
            d.resize(off + n, 0);
        }
        match b {
            Blob::Data(s) => d[off..off + n].copy_from_slice(s),
            Blob::Zero(_) => d[off..off + n].fill(0),
        }
    }
    fn fbo_of(&self, th: u8, target: u32) -> u32 {
        let c = &self.ctx[&th];
        if target == READ_FRAMEBUFFER { c.rfbo } else { c.dfbo }
    }
    pub fn target_version(&self, fbo: u32) -> u64 {
        if fbo == 0 {
            return self.fb0_version;
        }
        let f = self.fbos.get(&fbo).copied().unwrap_or_default();
        if f.color_rb {
            self.rbs.get(&f.color).map(|r| r.3).unwrap_or(0)
        } else {
            self.tex.get(&f.color).map(|t| t.version).unwrap_or(0)
        }
    }
    pub fn set_target_version(&mut self, fbo: u32, v: u64) {
        if fbo == 0 {
            self.fb0_version = v;
            return;
        }
        let f = self.fbos.get(&fbo).copied().unwrap_or_default();
        if f.color_rb {
            if let Some(r) = self.rbs.get_mut(&f.color) {
                r.3 = v;
            }
        } else if let Some(t) = self.tex.get_mut(&f.color) {
            t.version = v;
        }
    }
    pub fn target_size(&self, fbo: u32) -> Option<(u32, u32, bool)> {
        let f = self.fbos.get(&fbo)?;
        if f.color_rb {
            self.rbs.get(&f.color).map(|r| (r.0, r.1, f.msaa || r.2 > 1))
        } else {
            self.tex.get(&f.color).map(|t| (t.w, t.h, f.msaa || t.samples > 1))
        }
    }
    fn bound_tex(&mut self, th: u8, target: u32) -> u32 {
        let t = if (0x8515..=0x851A).contains(&target) { 0x8513 } else { target };
        let c = &self.ctx[&th];
        c.units.get(&(c.active, t)).copied().unwrap_or(0)
    }
    fn program_meta(&mut self, id: u32) {
        let Some(p) = self.progs.get(&id) else { return };
        if p.samplers.is_some() {
            return;
        }
        let mut samplers = Vec::new();
        let (mut vs_has_pos, mut fs_discards, mut fsh) = (false, false, 0u64);
        for (ty, src) in &p.srcs {
            if *ty == 0x8B31 {
                vs_has_pos = src.contains("in_POSITION0");
            } else {
                fs_discards = src.contains("discard");
                fsh = h64(0, src.as_bytes());
            }
            for st in src.split(';') {
                let st = st.trim();
                let Some(u) = st.rfind("uniform ") else { continue };
                let w: Vec<&str> = st[u..].split_whitespace().collect();
                let Some(ty) = w.iter().find(|x| x.contains("sampler")) else { continue };
                let target = if ty.contains("2DArray") { 0x8C1A } else if ty.contains("Cube") { 0x8513 } else if ty.contains("3D") { 0x806F } else if ty.contains("External") { 0x8D65 } else if ty.contains("2DMS") { 0x9100 } else { 0x0DE1 };
                let name = w.last().unwrap().split('[').next().unwrap().to_string();
                if let Some(l) = p.loc(&name) {
                    if !samplers.iter().any(|(_, x, _)| *x == l) {
                        samplers.push((name, l, target));
                    }
                }
            }
        }
        samplers.sort_by_key(|s| s.1);
        let label = format!("prog {} ({}; fs {:04x})", id, samplers.iter().map(|s| s.0.as_str()).collect::<Vec<_>>().join(","), fsh & 0xffff);
        let p = self.progs.get_mut(&id).unwrap();
        p.samplers = Some(Rc::new(samplers));
        p.label = Some(Rc::new(label));
        p.vs_has_pos = vs_has_pos;
        p.fs_discards = fs_discards;
    }

    /// one call: the state changed, and for the render thread's calls what it was
    pub fn apply(&mut self, r: &Rec, name: &str, describe: bool) -> Event {
        let th = r.thread;
        self.ctx.entry(th).or_default();
        macro_rules! c {
            () => {
                self.ctx.get_mut(&th).unwrap()
            };
        }
        match name {
            "glBindBuffer" => {
                let (t, b) = (r.u(0), r.u(1));
                if t == ELEMENT_ARRAY_BUFFER {
                    let v = c!().vao;
                    self.vaos.entry((th, v)).or_default().elem = b;
                } else {
                    c!().bufs.insert(t, b);
                }
                if b != 0 {
                    self.bufs.entry(b).or_default();
                }
            }
            "glBindBufferBase" | "glBindBufferRange" => {
                c!().bufs.insert(r.u(0), r.u(2));
                self.bufs.entry(r.u(2)).or_default();
                if r.u(0) == 0x8A11 && r.u(2) != 0 {
                    self.ubo_seen += 1;
                }
            }
            "glBufferData" => {
                let b = self.buf_of(th, r.u(0));
                let n = r.i(1).max(0) as usize;
                self.bufs.insert(b, vec![0; n]);
                self.write(b, 0, r.blob(2));
            }
            "glBufferSubData" => {
                let b = self.buf_of(th, r.u(0));
                self.write(b, r.a[1], r.blob(3));
            }
            "glMapBufferRange" => {
                // (by buffer: a buffer stays mapped while others are mapped and unmapped on its target)
                let b = self.buf_of(th, r.u(0));
                c!().maps.insert(b, r.a[1]);
            }
            "glFlushMappedBufferRange" => {
                let b = self.buf_of(th, r.u(0));
                if r.blob(3).is_none() {
                    self.flush_nodata += 1; // (a trace written before glrec.c kept its maps by buffer)
                }
                if let Some(off) = c!().maps.get(&b).copied() {
                    self.write(b, off + r.a[1], r.blob(3));
                }
            }
            "glUnmapBuffer" => {
                let b = self.buf_of(th, r.u(0));
                if let Some(off) = c!().maps.remove(&b) {
                    self.write(b, off, r.blob(1));
                }
            }
            "glDeleteBuffers" => {
                if let Some(d) = r.bytes(1) {
                    for id in d.chunks_exact(4) {
                        self.bufs.remove(&u32::from_le_bytes(id.try_into().unwrap()));
                    }
                }
            }
            "glBindVertexArray" | "glBindVertexArrayOES" => c!().vao = r.u(0),
            "glEnableVertexAttribArray" | "glDisableVertexAttribArray" => {
                let v = c!().vao;
                if let Some(a) = self.vaos.entry((th, v)).or_default().attr.get_mut(r.u(0) as usize) {
                    a.enabled = name.starts_with("glEnable");
                }
            }
            "glVertexAttribPointer" | "glVertexAttribIPointer" => {
                let int = name == "glVertexAttribIPointer";
                let buf = self.buf_of(th, ARRAY_BUFFER);
                let v = c!().vao;
                if let Some(a) = self.vaos.entry((th, v)).or_default().attr.get_mut(r.u(0) as usize) {
                    let en = a.enabled;
                    *a = if int {
                        Attr { enabled: en, size: r.u(1), ty: r.u(2), norm: 0, stride: r.u(3), ptr: r.a[4], buf, integer: true }
                    } else {
                        Attr { enabled: en, size: r.u(1), ty: r.u(2), norm: r.u(3), stride: r.u(4), ptr: r.a[5], buf, integer: false }
                    };
                }
            }
            "glVertexAttrib4fv" => {
                if let (Some(d), true) = (r.bytes(1), r.u(0) < 16) {
                    c!().gattr[r.u(0) as usize].copy_from_slice(&d[..16]);
                }
            }
            "glUseProgram" => c!().prog = r.u(0),
            "glCreateShader" => {
                self.shaders.insert(r.ret.unwrap_or(0) as u32, (r.u(0), Rc::new(String::new())));
            }
            "glShaderSource" => {
                if let Some(d) = r.bytes(1) {
                    let s = String::from_utf8_lossy(d).into_owned();
                    let e = self.shaders.entry(r.u(0)).or_insert((0, Rc::new(String::new())));
                    if e.0 == 0 {
                        e.0 = if s.contains("gl_Position") { 0x8B31 } else { 0x8B30 };
                    }
                    e.1 = Rc::new(s);
                }
            }
            "glCreateProgram" => {
                self.progs.insert(r.ret.unwrap_or(0) as u32, Program::default());
            }
            "glDeleteProgram" => {
                self.progs.remove(&r.u(0));
            }
            "glAttachShader" => self.progs.entry(r.u(0)).or_default().shaders.push(r.u(1)),
            "glDetachShader" => self.progs.entry(r.u(0)).or_default().shaders.retain(|s| *s != r.u(1)),
            "glLinkProgram" => {
                let srcs: Vec<(u32, Rc<String>)> = self.progs.get(&r.u(0)).map(|p| p.shaders.iter().filter_map(|s| self.shaders.get(s).cloned()).collect()).unwrap_or_default();
                let p = self.progs.entry(r.u(0)).or_default();
                p.uni.clear();
                p.names.clear();
                p.samplers = None;
                p.srcs = srcs;
            }
            "glGetUniformLocation" => {
                if let (Some(d), Some(l)) = (r.bytes(1), r.ret) {
                    let n = String::from_utf8_lossy(d).trim_end_matches('\0').to_string();
                    let p = self.progs.entry(r.u(0)).or_default();
                    p.names.insert(n, l as i32);
                    p.samplers = None;
                }
            }
            "glBindAttribLocation" => {
                if let Some(d) = r.bytes(2) {
                    let n = String::from_utf8_lossy(d).trim_end_matches('\0').to_string();
                    self.progs.entry(r.u(0)).or_default().attr_names.insert(n, r.u(1));
                }
            }
            "glGetAttribLocation" => {
                if let (Some(d), Some(l)) = (r.bytes(1), r.ret) {
                    let n = String::from_utf8_lossy(d).trim_end_matches('\0').to_string();
                    if (l as i32) >= 0 {
                        self.progs.entry(r.u(0)).or_default().attr_names.insert(n, l as u32);
                    }
                }
            }
            "glActiveTexture" => c!().active = r.u(0).wrapping_sub(0x84C0),
            "glBindTexture" => {
                let a = c!().active;
                c!().units.insert((a, r.u(0)), r.u(1));
                if r.u(1) != 0 {
                    self.tex.entry(r.u(1)).or_default();
                }
            }
            "glDeleteTextures" => {
                if let Some(d) = r.bytes(1) {
                    for id in d.chunks_exact(4) {
                        self.tex.remove(&u32::from_le_bytes(id.try_into().unwrap()));
                    }
                }
            }
            "glBindSampler" => c!().set((K_SAMPLER, r.u(0)), vec![r.a[1]]),
            "glTexParameteri" | "glTexParameterf" | "glTexParameteriv" | "glTexParameterfv" => {
                let id = self.bound_tex(th, r.u(0));
                let v = match r.bytes(2) {
                    Some(d) => h64(1, d),
                    None => r.a[2],
                };
                if let Some(t) = self.tex.get_mut(&id) {
                    let k = r.u(1);
                    if let Some(old) = t.params.insert(k, v) {
                        t.phash ^= hmix(k as u64, old);
                    }
                    t.phash ^= hmix(k as u64, v);
                }
            }
            "glGenerateMipmap" => {}
            _ if name.starts_with("glTexImage") || name.starts_with("glTexSubImage") || name.starts_with("glCompressedTex") || name.starts_with("glTexStorage") || name.starts_with("glCopyTex") => {
                let id = self.bound_tex(th, r.u(0));
                self.tex_uploads += 1;
                // the region (every argument but the data) and the data's hash; a copy from the
                // framebuffer: the read framebuffer's contents
                let mut region = h64(7, name.as_bytes());
                let mut data = 0u64;
                let mut nopix = false;
                for (i, v) in r.a.iter().enumerate() {
                    match r.blob(i) {
                        Some(Blob::Data(d)) => data = h64(data, d),
                        Some(Blob::Zero(_)) => nopix = true,
                        None => region = hmix(region, *v),
                    }
                }
                if name.starts_with("glCopyTex") {
                    let rf = self.ctx[&th].rfbo;
                    data = self.target_version(rf);
                }
                if nopix {
                    self.tex_uploads_nopixels += 1;
                    data = self.uniq();
                }
                let define = !name.contains("Sub");
                let (w, h, s) = match name {
                    "glTexImage2D" | "glCompressedTexImage2D" => (r.u(3), r.u(4), 0),
                    "glTexStorage2D" => (r.u(3), r.u(4), 0),
                    "glTexStorage2DMultisample" => (r.u(3), r.u(4), r.u(1)),
                    "glTexImage3D" | "glTexStorage3D" | "glCompressedTexImage3D" => (r.u(3), r.u(4), 0),
                    _ => (0, 0, 0),
                };
                if s > 1 {
                    self.msaa_seen += 1;
                }
                if let Some(t) = self.tex.get_mut(&id) {
                    if define && (name.contains("Storage") || r.u(1) == 0) {
                        t.w = w;
                        t.h = h;
                        t.samples = s;
                        t.regions.clear();
                        t.version = 0;
                    }
                    let e = hmix(region, data);
                    if let Some(old) = t.regions.insert(region, e) {
                        t.version ^= old;
                    }
                    t.version ^= e;
                }
            }
            "glBindRenderbuffer" => {
                c!().bufs.insert(0x8D41, r.u(1));
            }
            "glRenderbufferStorage" | "glRenderbufferStorageMultisample" | "glRenderbufferStorageMultisampleEXT" => {
                let id = c!().bufs.get(&0x8D41).copied().unwrap_or(0);
                let ms = name != "glRenderbufferStorage";
                let o = if ms { 1 } else { 0 };
                let s = if ms { r.u(1) } else { 0 };
                if s > 1 {
                    self.msaa_seen += 1;
                }
                let v = self.uniq();
                self.rbs.insert(id, (r.u(2 + o), r.u(3 + o), s, v));
            }
            "glBindFramebuffer" => {
                let (t, id) = (r.u(0), r.u(1));
                let c = c!();
                let old = c.dfbo;
                if t == FRAMEBUFFER || t == READ_FRAMEBUFFER {
                    c.rfbo = id;
                }
                if t != READ_FRAMEBUFFER {
                    c.dfbo = id;
                    self.fbos.entry(id).or_default();
                    if old != id {
                        return Event::BindDrawFbo { old };
                    }
                }
            }
            "glFramebufferTexture2D" | "glFramebufferTexture2DMultisampleEXT" | "glFramebufferTextureLayer" | "glFramebufferRenderbuffer" | "glFramebufferTexture" => {
                let fb = self.fbo_of(th, r.u(0));
                let rb = name == "glFramebufferRenderbuffer";
                let obj = if rb || name == "glFramebufferTexture2D" || name == "glFramebufferTexture2DMultisampleEXT" { r.u(3) } else { r.u(2) };
                let ms = name == "glFramebufferTexture2DMultisampleEXT" && r.u(5) > 1;
                if ms {
                    self.msaa_seen += 1;
                }
                let f = self.fbos.entry(fb).or_default();
                match r.u(1) {
                    COLOR_ATTACHMENT0 => {
                        f.color = obj;
                        f.color_rb = rb;
                        f.msaa = ms;
                    }
                    DEPTH_ATTACHMENT | STENCIL_ATTACHMENT | DEPTH_STENCIL_ATTACHMENT => f.depth = obj,
                    _ => {}
                }
            }
            "glEnable" | "glDisable" => c!().set((K_CAP, r.u(0)), vec![(name == "glEnable") as u64]),
            "glBlendFunc" => c!().set((K_BLENDFUNC, 0), vec![r.a[0], r.a[1], r.a[0], r.a[1]]),
            "glBlendFuncSeparate" => c!().set((K_BLENDFUNC, 0), r.a.clone()),
            "glBlendEquation" => c!().set((K_BLENDEQ, 0), vec![r.a[0], r.a[0]]),
            "glBlendEquationSeparate" => c!().set((K_BLENDEQ, 0), r.a.clone()),
            "glDepthFunc" => c!().set((K_DEPTHFUNC, 0), r.a.clone()),
            "glDepthMask" => c!().set((K_DEPTHMASK, 0), vec![r.a[0] & 0xff]),
            "glColorMask" => c!().set((K_COLORMASK, 0), r.a.iter().map(|x| x & 0xff).collect()),
            "glViewport" => c!().set((K_VIEWPORT, 0), r.a.clone()),
            "glScissor" => c!().set((K_SCISSOR, 0), r.a.clone()),
            "glFrontFace" => c!().set((K_FRONTFACE, 0), r.a.clone()),
            "glCullFace" => c!().set((K_CULLFACE, 0), r.a.clone()),
            "glStencilFunc" | "glStencilOp" | "glStencilMask" | "glStencilFuncSeparate" | "glStencilOpSeparate" | "glStencilMaskSeparate" => {
                let sep = name.ends_with("Separate");
                let face = if sep { r.u(0) } else { 0x0408 };
                let v: Vec<u64> = r.a[sep as usize..].to_vec();
                let k = if name.starts_with("glStencilFunc") { K_STENCILFUNC } else if name.starts_with("glStencilOp") { K_STENCILOP } else { K_STENCILMASK };
                if face != 0x0405 {
                    c!().set((k, 0x0404), v.clone());
                }
                if face != 0x0404 {
                    c!().set((k, 0x0405), v);
                }
            }
            "glClearColor" | "glClearDepthf" | "glClearStencil" | "glBlendColor" | "glPolygonOffset" | "glDepthRangef" | "glLineWidth" | "glSampleCoverage" | "glDrawBuffers" | "glReadBuffer" | "glPatchParameteri" | "glMinSampleShading" => {
                let mut v = r.a.clone();
                for (i, x) in v.iter_mut().enumerate() {
                    if let Some(d) = r.bytes(i) {
                        *x = h64(3, d);
                    }
                }
                c!().set((K_OTHER, r.op), v);
            }
            "glClear" => {
                let m = r.u(0);
                return Event::Clear { mask: m, extra: 0, full: m & 0x4000 != 0 && self.clear_is_full(th) };
            }
            "glClearBufferfv" | "glClearBufferiv" | "glClearBufferuiv" | "glClearBufferfi" => {
                let col = r.u(0) == 0x1800;
                let mut e = hmix(r.a[0], r.a[1]);
                for i in 2..r.a.len() {
                    e = match r.bytes(i) {
                        Some(d) => h64(e, d),
                        None => hmix(e, r.a[i]),
                    };
                }
                return Event::Clear { mask: if col { 0x4000 } else { 0x500 }, extra: e, full: col && self.clear_is_full(th) };
            }
            "glInvalidateFramebuffer" | "glDiscardFramebufferEXT" | "glInvalidateSubFramebuffer" => {
                // the colour attachment given up: what follows does not depend on what the target held
                let col = r.bytes(2).map(|d| d.chunks_exact(4).any(|c| matches!(u32::from_le_bytes(c.try_into().unwrap()), 0x8CE0 | 0x1800))).unwrap_or(false);
                if col && r.u(0) != READ_FRAMEBUFFER && name != "glInvalidateSubFramebuffer" {
                    return Event::Clear { mask: 0, extra: 0x1447, full: true };
                }
            }
            "glBlitFramebuffer" => {
                let c = &self.ctx[&th];
                let src = self.target_version(c.rfbo);
                let mut h = 0;
                for v in &r.a {
                    h = hmix(h, *v);
                }
                let (w, hh) = ((r.i(6) - r.i(4)).abs(), (r.i(7) - r.i(5)).abs());
                return Event::Blit { src, args: h, area: if r.u(8) & 0x4000 != 0 { (w * hh) as f64 } else { 0.0 }, full_w: w, full_h: hh };
            }
            _ if name.starts_with("glUniform") && name != "glUniformBlockBinding" => {
                let p = c!().prog;
                let data: Vec<u8> = match r.a.iter().enumerate().find_map(|(i, _)| r.bytes(i)) {
                    Some(d) => d.to_vec(),
                    None => r.a[1..].iter().flat_map(|x| x.to_le_bytes()).collect(),
                };
                let hash = h64(r.op as u64, &data);
                self.progs.entry(p).or_default().uni.insert(r.i(0) as i32, Rc::new(UVal { op: r.op, data, hash }));
            }
            _ if name.starts_with("glProgramUniform") => {
                let data: Vec<u8> = match r.a.iter().enumerate().find_map(|(i, _)| r.bytes(i)) {
                    Some(d) => d.to_vec(),
                    None => r.a[2..].iter().flat_map(|x| x.to_le_bytes()).collect(),
                };
                let hash = h64(r.op as u64, &data);
                self.progs.entry(r.u(0)).or_default().uni.insert(r.i(1) as i32, Rc::new(UVal { op: r.op, data, hash }));
            }
            _ if name.starts_with("glDrawElements") || name.starts_with("glDrawRangeElements") || name.starts_with("glDrawArrays") => {
                if !describe {
                    return Event::None;
                }
                return Event::Draw(Box::new(self.draw(r, name)));
            }
            _ => return Event::Unmodelled,
        }
        Event::None
    }

    /// the scissor test off, or its rectangle over the whole target
    pub fn scissor_off(&self, th: u8) -> bool {
        let c = &self.ctx[&th];
        if !c.cap(SCISSOR_TEST) {
            return true;
        }
        match (c.get((K_SCISSOR, 0)), self.target_size(c.dfbo)) {
            (Some(s), Some((w, h, _))) => s[0] as i64 <= 0 && s[1] as i64 <= 0 && s[0] as i64 + s[2] as i64 >= w as i64 && s[1] as i64 + s[3] as i64 >= h as i64,
            _ => false,
        }
    }
    fn clear_is_full(&self, th: u8) -> bool {
        let c = &self.ctx[&th];
        self.scissor_off(th) && c.get((K_COLORMASK, 0)).map(|m| m.iter().all(|x| *x != 0)).unwrap_or(true)
    }

    fn draw(&mut self, r: &Rec, name: &str) -> DrawInfo {
        let th = r.thread;
        let prog = self.ctx[&th].prog;
        self.program_meta(prog);
        let fixed = self.ctx.get_mut(&th).unwrap().snapshot();
        let c = &self.ctx[&th];
        let empty = Program::default();
        let p = self.progs.get(&prog).unwrap_or(&empty);
        let vao = self.vaos.get(&(th, c.vao));
        // the call: mode, count, index type, where the indices are, the base vertex
        let arrays = name.starts_with("glDrawArrays");
        let o = if name.contains("Range") { 2 } else { 0 };
        let mode = r.u(0);
        let (count, ity, first) = if arrays { (r.u(2), 0, r.u(1)) } else { (r.u(1 + o), r.u(2 + o), 0) };
        let base = if name.contains("BaseVertex") { r.i(if name.contains("Instanced") { 5 } else { 4 + o }) } else { 0 };
        let mut params = hmix(hmix(mode as u64, count as u64), ity as u64);
        if name.contains("Instanced") {
            params = hmix(params, r.a[if arrays { 3 } else { 4 }]);
        }
        let mut idx: Vec<u32> = Vec::with_capacity(count as usize);
        if arrays {
            idx.extend(first..first + count);
        } else {
            let sz = type_size(ity);
            let slot = 3 + o;
            let (data, off): (&[u8], usize) = match r.bytes(slot) {
                Some(d) => (d, 0),
                None => (vao.and_then(|v| self.bufs.get(&v.elem)).map(|d| d.as_slice()).unwrap_or(&[]), r.a[slot] as usize),
            };
            for i in 0..count as usize {
                let at = off + i * sz;
                if at + sz > data.len() {
                    break;
                }
                let v = match sz {
                    1 => data[at] as u32,
                    2 => u16::from_le_bytes([data[at], data[at + 1]]) as u32,
                    _ => u32::from_le_bytes(data[at..at + 4].try_into().unwrap()),
                };
                idx.push((v as i64 + base) as u32);
            }
        }
        let (lo, hi) = idx.iter().fold((u32::MAX, 0u32), |(a, b), x| (a.min(*x), b.max(*x)));
        let mut h_idx = idx.len() as u64;
        for x in &idx {
            h_idx = hmix(h_idx, (*x - lo) as u64);
        }
        // the vertex data the indices reach, each buffer range once, and each array's layout in it
        let mut h_attr = 0u64;
        if let (Some(v), false) = (vao, idx.is_empty()) {
            let mut groups: Vec<(u32, u32, u64, u64)> = Vec::new(); // buffer, stride, lowest pointer, widest end
            for a in v.attr.iter().filter(|a| a.enabled) {
                let w = a.size as u64 * type_size(a.ty) as u64;
                let st = if a.stride == 0 { w as u32 } else { a.stride };
                match groups.iter_mut().find(|g| g.0 == a.buf && g.1 == st) {
                    Some(g) => {
                        let end = g.2.max(a.ptr) + w;
                        g.2 = g.2.min(a.ptr);
                        g.3 = g.3.max(end);
                    }
                    None => groups.push((a.buf, st, a.ptr, a.ptr + w)),
                }
            }
            for (i, a) in v.attr.iter().enumerate() {
                if a.enabled {
                    let w = a.size as u64 * type_size(a.ty) as u64;
                    let st = if a.stride == 0 { w as u32 } else { a.stride };
                    let g = groups.iter().position(|g| g.0 == a.buf && g.1 == st).unwrap();
                    for x in [i as u64, a.size as u64, a.ty as u64, a.norm as u64, a.integer as u64, st as u64, g as u64, a.ptr - groups[g].2] {
                        h_attr = hmix(h_attr, x);
                    }
                } else if p.attr_names.values().any(|x| *x as usize == i) {
                    h_attr = h64(hmix(h_attr, i as u64), &c.gattr[i]);
                }
            }
            for g in &groups {
                let d = self.bufs.get(&g.0).map(|d| d.as_slice()).unwrap_or(&[]);
                let st = g.1.max(1) as usize;
                let a = (g.2 as usize + lo as usize * st).min(d.len());
                let b = (g.2 as usize + hi as usize * st + (g.3 - g.2) as usize).min(d.len());
                h_attr = h64(h_attr, &d[a..b.max(a)]);
            }
        }
        // uniforms and textures
        let uni: Vec<(i32, Rc<UVal>)> = p.uni.iter().map(|(l, v)| (*l, v.clone())).collect();
        let mut tex = Vec::new();
        if let Some(s) = &p.samplers {
            for (_, loc, target) in s.iter() {
                let unit = p.uni.get(loc).map(|v| i32::from_le_bytes(v.data[..4].try_into().unwrap())).unwrap_or(0) as u32;
                let id = c.units.get(&(unit, *target)).copied().unwrap_or(0);
                let (version, phash) = self.tex.get(&id).map(|t| (hmix(t.version, (t.w as u64) << 32 | t.h as u64), t.phash)).unwrap_or((0, 0));
                tex.push(TexRef { loc: *loc, version, phash });
            }
        }
        // fragments: the triangles through the program's matrices, cut to the viewport and the scissor
        let vp: [f64; 4] = c.get((K_VIEWPORT, 0)).map(|v| [v[0] as i64 as f64, v[1] as i64 as f64, v[2] as i64 as f64, v[3] as i64 as f64]).unwrap_or([0.0; 4]);
        let sc: Option<[f64; 4]> = if c.cap(SCISSOR_TEST) { c.get((K_SCISSOR, 0)).map(|v| [v[0] as i64 as f64, v[1] as i64 as f64, v[2] as i64 as f64, v[3] as i64 as f64]) } else { None };
        let cull = if c.cap(CULL_FACE) { c.get((K_CULLFACE, 0)).map(|v| v[0] as u32).unwrap_or(0x0405) } else { 0 };
        let ccw = c.get((K_FRONTFACE, 0)).map(|v| v[0] as u32 != 0x0900).unwrap_or(true);
        let vpm = p.mat("hlslcc_mtx4x4unity_MatrixVP");
        let o2w = p.mat("hlslcc_mtx4x4unity_ObjectToWorld");
        let m = match (vpm, o2w) {
            (Some(a), Some(b)) => Some(mul(&a, &b)),
            (Some(a), None) => Some(a),
            _ => None,
        };
        let mut h_pos = 0u64;
        let mut bbox = [f32::MAX, f32::MAX, f32::MIN, f32::MIN];
        let pa = p.attr_names.get("in_POSITION0").copied().unwrap_or(0) as usize;
        let mut pos: Vec<[f32; 4]> = Vec::new();
        if let Some(a) = vao.map(|v| v.attr[pa]).filter(|a| a.enabled && a.ty == 0x1406 && a.size >= 2) {
            let d = self.bufs.get(&a.buf).map(|d| d.as_slice()).unwrap_or(&[]);
            let st = if a.stride == 0 { a.size as usize * 4 } else { a.stride as usize };
            for i in &idx {
                let at = a.ptr as usize + *i as usize * st;
                let mut v = [0.0, 0.0, 0.0, 1.0f32];
                if at + a.size as usize * 4 <= d.len() {
                    for k in 0..a.size as usize {
                        v[k] = f32::from_le_bytes(d[at + k * 4..at + k * 4 + 4].try_into().unwrap());
                        h_pos = hmix(h_pos, v[k].to_bits() as u64);
                    }
                }
                pos.push(v);
            }
        }
        let mut empty = false;
        let (weight, how) = if !p.vs_has_pos && p.samplers.is_some() && m.is_none() {
            let full = match sc {
                Some(s) => s[2] * s[3],
                None => vp[2] * vp[3],
            };
            bbox = [vp[0] as f32, vp[1] as f32, (vp[0] + vp[2]) as f32, (vp[1] + vp[3]) as f32];
            (Some(full), 1)
        } else if let (Some(m), true, true) = (m, pos.len() == idx.len(), mode >= 4 && mode <= 6) {
            let cl: Vec<[f64; 4]> = pos.iter().map(|v| xf(&m, *v)).collect();
            let mut total = 0.0;
            let n = cl.len();
            let mut live = false;
            let mut tri = |a: usize, b: usize, c: usize| {
                let same = |i: usize, j: usize| pos[i].iter().zip(pos[j].iter()).all(|(x, y)| x.to_bits() == y.to_bits());
                let out = (0..6).any(|pl| [a, b, c].iter().all(|i| {
                    let v = &cl[*i];
                    (match pl { 0 => v[3] + v[0], 1 => v[3] - v[0], 2 => v[3] + v[1], 3 => v[3] - v[1], 4 => v[3] + v[2], _ => v[3] - v[2] }) < 0.0
                }));
                if !(same(a, b) || same(b, c) || same(a, c) || out) {
                    live = true;
                }
                let mut poly = clip_tri([cl[a], cl[b], cl[c]], vp);
                if let Some(s) = sc {
                    poly = clip_rect(poly, s);
                }
                if poly.len() < 3 {
                    return;
                }
                let s = area(&poly);
                let front = (s > 0.0) == ccw;
                if (cull == 0x0405 && !front) || (cull == 0x0404 && front) || cull == 0x0408 {
                    return;
                }
                total += s.abs();
                for q in &poly {
                    bbox = [bbox[0].min(q[0] as f32), bbox[1].min(q[1] as f32), bbox[2].max(q[0] as f32), bbox[3].max(q[1] as f32)];
                }
            };
            match mode {
                4 => (0..n / 3).for_each(|t| tri(t * 3, t * 3 + 1, t * 3 + 2)),
                5 => (0..n.saturating_sub(2)).for_each(|t| if t % 2 == 0 { tri(t, t + 1, t + 2) } else { tri(t + 1, t, t + 2) }),
                _ => (1..n.saturating_sub(1)).for_each(|t| tri(0, t, t + 1)),
            }
            empty = !live;
            (Some(total), 0)
        } else {
            (None, 2)
        };
        let blend = c.cap(BLEND);
        let depth_write = c.cap(DEPTH_TEST) && c.get((K_DEPTHMASK, 0)).map(|v| v[0] != 0).unwrap_or(true);
        let st_keep = |f: u32| c.get((K_STENCILOP, f)).map(|v| v.iter().all(|x| *x == 0x1E00)).unwrap_or(true) || c.get((K_STENCILMASK, f)).map(|v| v[0] & 0xff == 0).unwrap_or(false);
        let stencil_write = c.cap(STENCIL_TEST) && !(st_keep(0x0404) && st_keep(0x0405));
        let depth_passes = !c.cap(DEPTH_TEST) || c.get((K_DEPTHFUNC, 0)).map(|v| v[0] == 0x0207).unwrap_or(false);
        let opaque_state = !blend && !c.cap(STENCIL_TEST) && depth_passes && self.scissor_off(th) && c.get((K_COLORMASK, 0)).map(|m| m.iter().all(|x| *x != 0)).unwrap_or(true);
        DrawInfo {
            prog,
            label: p.label.clone().unwrap_or_else(|| Rc::new(format!("prog {prog}"))),
            params,
            count,
            h_idx,
            h_pos,
            h_attr,
            uni,
            tex,
            fixed,
            weight,
            how,
            empty,
            bbox,
            blend,
            depth_write,
            stencil_write,
            opaque_state,
            discards: p.fs_discards,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const ID: [f32; 16] = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0];
    fn tri_area(t: [[f32; 4]; 3], vp: [f64; 4]) -> f64 {
        let c = [xf(&ID, t[0]), xf(&ID, t[1]), xf(&ID, t[2])];
        area(&clip_tri(c, vp)).abs()
    }
    #[test]
    fn a_triangle_inside_keeps_its_area() {
        let a = tri_area([[-0.5, -0.5, 0.0, 1.0], [0.5, -0.5, 0.0, 1.0], [-0.5, 0.5, 0.0, 1.0]], [0.0, 0.0, 200.0, 100.0]);
        assert!((a - 0.5 * 100.0 * 50.0).abs() < 1e-6);
    }
    #[test]
    fn a_triangle_over_the_edge_is_cut() {
        // the right half of a quad's triangle pair reaches x = 3: only up to x = 1 counts
        let v = [0.0, 0.0, 200.0, 100.0];
        let a = tri_area([[-1.0, -1.0, 0.0, 1.0], [3.0, -1.0, 0.0, 1.0], [-1.0, 3.0, 0.0, 1.0]], v);
        assert!((a - 200.0 * 100.0).abs() < 1e-6);
        assert_eq!(tri_area([[2.0, 2.0, 0.0, 1.0], [3.0, 2.0, 0.0, 1.0], [2.0, 3.0, 0.0, 1.0]], v), 0.0);
    }
    #[test]
    fn behind_the_eye_is_cut() {
        let a = tri_area([[-1.0, -1.0, 0.0, -1.0], [1.0, -1.0, 0.0, -1.0], [0.0, 1.0, 0.0, -1.0]], [0.0, 0.0, 100.0, 100.0]);
        assert_eq!(a, 0.0);
    }
    #[test]
    fn a_scissor_cuts_the_window_polygon() {
        let p = clip_rect(vec![[0.0, 0.0], [100.0, 0.0], [100.0, 100.0], [0.0, 100.0]], [10.0, 20.0, 30.0, 40.0]);
        assert!((area(&p).abs() - 1200.0).abs() < 1e-9);
    }
    #[test]
    fn matrices_multiply_column_major() {
        let mut t = ID;
        t[12] = 2.0; // a translation by x
        let mut s = ID;
        s[0] = 3.0; // a scale of x
        let m = mul(&t, &s); // scale, then translate
        let v = xf(&m, [1.0, 0.0, 0.0, 1.0]);
        assert_eq!(v[0], 5.0);
    }
}
