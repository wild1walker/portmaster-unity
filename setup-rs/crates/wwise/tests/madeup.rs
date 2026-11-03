//! `wwise::run` on made-up banks (version 128) written byte by byte here: every object type step 1
//! reads, ADPCM and PCM media in the bank and a streamed one beside it, an object that does not read
//! (left out), a bank of another version (refused), the reference check. (Vorbis needs Wwise's
//! encoder: it is checked on a game's banks, docs/wwise-player.md.)
use serde_json::Value;
use std::path::{Path, PathBuf};

#[derive(Default)]
struct B(Vec<u8>);
impl B {
    fn u8(&mut self, x: u8) -> &mut Self {
        self.0.push(x);
        self
    }
    fn u16(&mut self, x: u16) -> &mut Self {
        self.0.extend_from_slice(&x.to_le_bytes());
        self
    }
    fn u32(&mut self, x: u32) -> &mut Self {
        self.0.extend_from_slice(&x.to_le_bytes());
        self
    }
    fn f32(&mut self, x: f32) -> &mut Self {
        self.u32(x.to_bits())
    }
    fn f64(&mut self, x: f64) -> &mut Self {
        self.0.extend_from_slice(&x.to_le_bytes());
        self
    }
    fn bytes(&mut self, b: &[u8]) -> &mut Self {
        self.0.extend_from_slice(b);
        self
    }
}

/// NodeBaseParams: no effects, the bus and parent, props (id, f32 or int bits), one ranged
/// pitch, no positioning or aux, a limit, optional state group (group, state, State object), one
/// volume RTPC when `rtpc`.
fn base(
    b: &mut B,
    bus: u32,
    parent: u32,
    props: &[(u8, u32)],
    state: Option<(u32, u32, u32)>,
    rtpc: Option<u32>,
) {
    b.u8(0).u8(0).u8(0).u32(bus).u32(parent).u8(0);
    b.u8(props.len() as u8);
    for p in props {
        b.u8(p.0);
    }
    for p in props {
        b.u32(p.1);
    }
    b.u8(1).u8(0x02).f32(-50.0).f32(50.0); // ranged pitch
    b.u8(0); // positioning: none
    b.u8(0); // aux: none
    b.u8(1).u8(0).u16(3).u8(0).u8(0); // adv settings: limit 3, kill newest
    b.u8(0); // state props
    match state {
        Some((g, s, inst)) => {
            b.u8(1).u32(g).u8(0).u8(1).u32(s).u32(inst);
        }
        None => {
            b.u8(0);
        }
    }
    match rtpc {
        Some(id) => {
            b.u16(1).u32(id).u8(0).u8(2).u8(0).u32(77).u8(2).u16(2);
            b.f32(0.0).f32(-1.0).u32(4).f32(100.0).f32(0.0).u32(4);
        }
        None => {
            b.u16(0);
        }
    }
}

fn obj(h: &mut Vec<(u8, Vec<u8>)>, t: u8, id: u32, body: B) {
    let mut v = id.to_le_bytes().to_vec();
    v.extend(body.0);
    h.push((t, v));
}

fn music_head(b: &mut B, parent: u32, children: &[u32]) {
    b.u8(0);
    base(b, 0, parent, &[], None, None);
    b.u32(children.len() as u32);
    for c in children {
        b.u32(*c);
    }
    b.f64(500.0).f64(0.0).f32(120.0).u8(4).u8(4).u8(0).u32(0); // meter, no stingers
}

fn rule(b: &mut B) {
    b.u32(1).u32(u32::MAX).u32(1).u32(u32::MAX); // any -> any
    b.u32(250).u32(4).u32(0).u32(7).u32(0).u8(1); // src: fade out 250, exit cue
    b.u32(100).u32(4).u32(0).u32(0).u32(0).u16(0).u8(1).u8(0); // dst: fade in 100
    b.u8(0);
}

fn wem_adpcm(ch: u16, blocks: usize, loop_: Option<(u32, u32)>) -> Vec<u8> {
    let mut f = B::default();
    f.u16(2)
        .u16(ch)
        .u32(32000)
        .u32(0)
        .u16(0x24 * ch)
        .u16(4)
        .u16(2)
        .u16(64);
    let mut d = B::default();
    for i in 0..blocks * ch as usize {
        d.u16(i as u16).u8(10).u8(0).bytes(&[0x17; 32]);
    }
    riff(&f.0, &d.0, loop_)
}

/// A Wwise Vorbis WEM's header (fmt 0xFFFF of 0x42 bytes, its sample count) over made-up data.
fn wem_vorbis() -> Vec<u8> {
    let mut f = B::default();
    f.u16(0xFFFF)
        .u16(1)
        .u32(44100)
        .u32(0)
        .u16(0)
        .u16(0)
        .u16(0x30)
        .u16(0)
        .u32(4);
    f.u32(1000).bytes(&[0; 0x42 - 0x1c]);
    riff(&f.0, &[0x5a; 64], None)
}

/// Made-up codebooks in Wwise's packed form, `n` back to back (each: one dimension, two entries
/// of length 1, no lookup: 4 bytes), between other bytes: a stand-in for a game's Wwise library.
fn wwise_library(n: usize) -> Vec<u8> {
    let mut b: Vec<u8> = (0..999u32).map(|i| (i * 7 + 3) as u8).collect();
    for _ in 0..n {
        b.extend_from_slice(&[0x21, 0x00, 0x08, 0x00]);
    }
    b.extend((0..500u32).map(|i| (i * 13) as u8));
    b
}

fn wem_pcm(ch: u16, frames: usize) -> Vec<u8> {
    let mut f = B::default();
    f.u16(0xFFFE)
        .u16(ch)
        .u32(48000)
        .u32(0)
        .u16(2 * ch)
        .u16(16)
        .u16(6)
        .u16(0)
        .u32(3);
    riff(&f.0, &vec![0x11; frames * 2 * ch as usize], None)
}

fn riff(fmt: &[u8], data: &[u8], loop_: Option<(u32, u32)>) -> Vec<u8> {
    let mut c = B::default();
    c.bytes(b"WAVEfmt ").u32(fmt.len() as u32).bytes(fmt);
    if let Some((a, e)) = loop_ {
        let mut s = B::default();
        s.bytes(&[0; 0x1c])
            .u32(1)
            .u32(0)
            .u32(0)
            .u32(0)
            .u32(a)
            .u32(e)
            .u32(0)
            .u32(0);
        c.bytes(b"smpl").u32(s.0.len() as u32).bytes(&s.0);
    }
    c.bytes(b"data").u32(data.len() as u32).bytes(data);
    let mut r = B::default();
    r.bytes(b"RIFF").u32(c.0.len() as u32).bytes(&c.0);
    r.0
}

fn bank(
    path: &Path,
    version: u32,
    id: u32,
    media: &[(u32, Vec<u8>)],
    hirc: &[(u8, Vec<u8>)],
    stmg: Option<Vec<u8>>,
) {
    let mut b = B::default();
    b.bytes(b"BKHD")
        .u32(20)
        .u32(version)
        .u32(id)
        .u32(0)
        .u32(0)
        .u32(1138);
    if !media.is_empty() {
        let mut didx = B::default();
        let mut data = B::default();
        for (mid, w) in media {
            while data.0.len() % 16 != 0 {
                data.u8(0);
            }
            didx.u32(*mid).u32(data.0.len() as u32).u32(w.len() as u32);
            data.bytes(w);
        }
        b.bytes(b"DIDX").u32(didx.0.len() as u32).bytes(&didx.0);
        b.bytes(b"DATA").u32(data.0.len() as u32).bytes(&data.0);
    }
    let mut h = B::default();
    h.u32(hirc.len() as u32);
    for (t, body) in hirc {
        h.u8(*t).u32(body.len() as u32).bytes(body);
    }
    b.bytes(b"HIRC").u32(h.0.len() as u32).bytes(&h.0);
    if let Some(s) = stmg {
        b.bytes(b"STMG").u32(s.len() as u32).bytes(&s);
    }
    std::fs::write(path, b.0).unwrap();
}

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("wwise-test-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(d.join("banks")).unwrap();
    d
}

const MASTER: u32 = 3803692087;
const MUSIC_BUS: u32 = 1000;
const STATE_GROUP: u32 = 500;
const STATE_A: u32 = 501;
const SWITCH_GROUP: u32 = 600;
const SW_A: u32 = 601;
const SW_B: u32 = 602;

fn init_bank(dir: &Path) {
    let mut h = Vec::new();
    let mut b = B::default();
    b.u32(0).u32(0); // master: no parent, its device
    b.u8(1).u8(0x00).f32(-2.0); // props: volume -2
    b.u8(0).u8(0); // positioning, aux
    b.u8(0).u16(0).u32(0).u8(0); // bits, max instances, channel config, HDR
    b.u32(0).f32(0.0).u32(0); // recovery, max duck, no ducks
    b.u8(0).u32(0).u8(0).u8(0); // fx none, fx 0, share set, attachment
    b.u16(0); // rtpc
    b.u8(0).u8(0); // state chunk
    obj(&mut h, 8, MASTER, b);
    let mut b = B::default();
    b.u32(MASTER).u8(2).u8(0x00).u8(0x05).f32(-3.0).f32(-1.5);
    b.u8(0).u8(0).u8(0).u16(0).u32(0).u8(0).u32(0).f32(0.0);
    b.u32(1).u32(MASTER).f32(-6.0).u32(100).u32(200).u8(4).u8(0); // a duck
    b.u8(1).u8(0).u8(0).u32(99).u8(1).u8(0); // one effect
    b.u32(0).u8(0).u8(0);
    b.u16(1)
        .u32(4242)
        .u8(0)
        .u8(0)
        .u8(0x05)
        .u32(1)
        .u8(0)
        .u16(1)
        .f32(0.0)
        .f32(-3.0)
        .u32(9); // bus volume rtpc
    b.u8(0)
        .u8(1)
        .u32(STATE_GROUP)
        .u8(0)
        .u8(1)
        .u32(STATE_A)
        .u32(900);
    obj(&mut h, 8, MUSIC_BUS, b);
    let mut b = B::default();
    b.u16(2).u16(0).u16(2).f32(-12.0).f32(100.0); // State object: volume -12, pitch 100
    obj(&mut h, 1, 900, b);
    let mut s = B::default();
    s.f32(-80.0).u16(100).u16(50);
    s.u32(1)
        .u32(STATE_GROUP)
        .u32(300)
        .u32(1)
        .u32(0)
        .u32(STATE_A)
        .u32(1000);
    s.u32(1)
        .u32(SWITCH_GROUP)
        .u32(4242)
        .u8(0)
        .u32(2)
        .f32(0.0)
        .u32(SW_A)
        .u32(9)
        .f32(50.0)
        .u32(SW_B)
        .u32(9);
    s.u32(1).u32(4242).f32(25.0).u32(0).f32(0.0).f32(0.0).u8(0);
    s.u32(0); // acoustic textures
    bank(
        &dir.join("banks/Init.bnk"),
        128,
        1355168291,
        &[],
        &h,
        Some(s.0),
    );
}

/// The SFX bank: an event playing a switch of a random container (weights) and a sequence, a
/// layer, an actor-mixer, a streamed sound; a music event: playlist -> segment -> track; a stop,
/// a set-state, a bus volume action; one object that does not read.
fn sfx_bank(dir: &Path, broken: bool) {
    let mut h = Vec::new();
    let sound = |h: &mut Vec<(u8, Vec<u8>)>,
                 id: u32,
                 media: u32,
                 parent: u32,
                 stream: u8,
                 props: &[(u8, u32)]| {
        let mut b = B::default();
        b.u32(0x0002_0001).u8(stream).u32(media).u32(0).u8(0);
        base(&mut b, 0, parent, props, None, None);
        obj(h, 2, id, b);
    };
    let mut b = B::default();
    b.u16(2).u16(0).u16(2).f32(-12.0).f32(100.0); // the State object the mixer's state uses
    obj(&mut h, 1, 900, b);
    // mixer 10 (bus master, vol -1) > switch 20 > random 30 (sounds 31, 32) / sequence 40 (sound 41)
    let mut b = B::default();
    base(
        &mut b,
        MASTER,
        0,
        &[(0x00, (-1.0f32).to_bits())],
        Some((STATE_GROUP, STATE_A, 900)),
        Some(4242),
    );
    b.u32(1).u32(20);
    obj(&mut h, 7, 10, b);
    let mut b = B::default();
    base(&mut b, 0, 10, &[], None, None);
    b.u8(0)
        .u32(SWITCH_GROUP)
        .u32(SW_A)
        .u8(1)
        .u32(2)
        .u32(30)
        .u32(40);
    b.u32(2).u32(SW_A).u32(1).u32(30).u32(SW_B).u32(1).u32(40);
    b.u32(1).u32(30).u8(0).u8(0).u32(0).u32(0);
    obj(&mut h, 6, 20, b);
    let mut b = B::default();
    base(&mut b, 0, 20, &[(0x02, 120.0f32.to_bits())], None, None);
    b.u16(1)
        .u16(0)
        .u16(0)
        .f32(1000.0)
        .f32(0.0)
        .f32(0.0)
        .u16(1)
        .u8(0)
        .u8(1)
        .u8(0)
        .u8(0x01 | 0x02);
    b.u32(2)
        .u32(31)
        .u32(32)
        .u16(2)
        .u32(31)
        .u32(70_000)
        .u32(32)
        .u32(30_000);
    obj(&mut h, 5, 30, b);
    sound(
        &mut h,
        31,
        1001,
        30,
        0,
        &[(0x3A, 0), (0x3B, 0.25f32.to_bits())],
    );
    sound(&mut h, 32, 1002, 30, 0, &[(0x00, (-6.0f32).to_bits())]);
    let mut b = B::default();
    base(&mut b, 0, 20, &[], None, None);
    b.u16(0)
        .u16(0)
        .u16(0)
        .f32(500.0)
        .f32(0.0)
        .f32(0.0)
        .u16(0)
        .u8(3)
        .u8(0)
        .u8(1)
        .u8(0x08);
    b.u32(2)
        .u32(41)
        .u32(42)
        .u16(2)
        .u32(42)
        .u32(50_000)
        .u32(41)
        .u32(50_000);
    obj(&mut h, 5, 40, b);
    sound(&mut h, 41, 1003, 40, 0, &[]);
    sound(&mut h, 42, 2000, 40, 2, &[]); // streamed: 2000.wem beside the banks
    let mut b = B::default();
    base(&mut b, 0, 10, &[], None, None);
    b.u32(1).u32(41).u32(0).u8(0);
    obj(&mut h, 9, 50, b);
    // music: playlist 60 > segment 70 > track 80 (media 1002 at 100 ms)
    let mut b = B::default();
    music_head(&mut b, 0, &[70]);
    b.u32(1);
    rule(&mut b);
    b.u32(2);
    b.u32(0)
        .u32(1)
        .u32(1)
        .u32(0)
        .u16(0)
        .u16(0)
        .u16(0)
        .u32(100_000)
        .u16(0)
        .u8(0)
        .u8(0);
    b.u32(70)
        .u32(2)
        .u32(0)
        .u32(0)
        .u16(2)
        .u16(0)
        .u16(0)
        .u32(100_000)
        .u16(0)
        .u8(0)
        .u8(0);
    obj(&mut h, 13, 60, b);
    let mut b = B::default();
    music_head(&mut b, 60, &[80]);
    b.f64(2000.0)
        .u32(2)
        .u32(43573010)
        .f64(10.0)
        .u32(0)
        .u32(1539036744)
        .f64(1900.0)
        .u32(5)
        .bytes(b"exit!");
    obj(&mut h, 10, 70, b);
    let mut b = B::default();
    b.u8(0).u32(1).u32(0x0002_0001).u8(0).u32(1002).u32(0).u8(0);
    b.u32(1)
        .u32(0)
        .u32(1002)
        .f64(100.0)
        .f64(0.0)
        .f64(-5.0)
        .f64(1500.0)
        .u32(1);
    b.u32(0);
    base(&mut b, MUSIC_BUS, 70, &[], None, None);
    b.u8(0).u32(0);
    obj(&mut h, 11, 80, b);
    // music switch 90: state group -> playlist 60, any -> nothing
    let mut b = B::default();
    music_head(&mut b, 0, &[60]);
    b.u32(0).u8(1).u32(1).u32(STATE_GROUP).u8(1).u32(36).u8(0);
    b.u32(0).u16(1).u16(2).u16(50).u16(100);
    b.u32(0).u32(0).u16(50).u16(100);
    b.u32(STATE_A).u32(60).u16(50).u16(100);
    obj(&mut h, 12, 90, b);
    // actions and events
    let action = |h: &mut Vec<(u8, Vec<u8>)>,
                  id: u32,
                  ty: u16,
                  target: u32,
                  bus: bool,
                  tail: &dyn Fn(&mut B)| {
        let mut b = B::default();
        b.u16(ty)
            .u32(target)
            .u8(bus as u8)
            .u8(1)
            .u8(0x0F)
            .u32(30)
            .u8(0);
        tail(&mut b);
        obj(h, 3, id, b);
    };
    action(&mut h, 100, 0x0403, 10, false, &|b| {
        b.u8(4).u32(393239870);
    });
    action(&mut h, 101, 0x0403, 90, false, &|b| {
        b.u8(4).u32(393239870);
    });
    action(&mut h, 102, 0x0103, 60, false, &|b| {
        b.u8(6).u8(0).u8(1).u32(41).u8(0);
    });
    action(&mut h, 103, 0x1204, STATE_A, false, &|b| {
        b.u32(STATE_GROUP).u32(STATE_A);
    });
    action(&mut h, 104, 0x0C03, MUSIC_BUS, true, &|b| {
        b.u8(4).u8(2).f32(-4.5).f32(0.0).f32(0.0).u8(0);
    });
    action(&mut h, 105, 0x0E03, MUSIC_BUS, true, &|b| {
        b.u8(4).u8(2).f32(-4.5).f32(0.0).f32(0.0).u8(0);
    });
    let mut b = B::default();
    b.u8(2).u32(100).u32(105);
    obj(&mut h, 4, 7000, b);
    let mut b = B::default();
    b.u8(4).u32(101).u32(102).u32(103).u32(104);
    obj(&mut h, 4, 7001, b);
    if broken {
        let mut b = B::default();
        b.u32(0x0002_0001).u8(0); // a sound cut short
        obj(&mut h, 2, 666, b);
    }
    let media = vec![
        (1001, wem_adpcm(1, 3, Some((10, 99)))),
        (1002, wem_pcm(2, 100)),
        (1003, wem_adpcm(2, 2, None)),
        (1004, wem_vorbis()),
    ];
    bank(&dir.join("banks/SFX.bnk"), 128, 393239870, &media, &h, None);
    std::fs::write(dir.join("banks/2000.wem"), wem_adpcm(1, 1, None)).unwrap();
}

fn load(p: &Path) -> Value {
    serde_json::from_slice(&std::fs::read(p).unwrap()).unwrap()
}

#[test]
fn madeup_banks() {
    let d = tmp("madeup");
    init_bank(&d);
    sfx_bank(&d, true);
    let mut log = Vec::new();
    let s = wwise::run(
        &d.join("banks"),
        &d.join("out"),
        &wwise::Opts {
            check: true,
            ..Default::default()
        },
        &mut |l| log.push(l.to_string()),
    )
    .unwrap();
    // no Wwise library near the made-up banks: the Vorbis media is left out, the rest converts
    assert_eq!(s.problems.len(), 2, "{:?}", s.problems);
    assert!(s.problems[0].contains("object 666"), "{:?}", s.problems);
    assert!(
        s.problems[1].contains("1 Vorbis media left out (those sounds stay silent): no codebooks"),
        "{:?}",
        s.problems
    );
    assert_eq!(s.check_failures, 0, "{:?}", s.problems);
    assert!(
        log[0].starts_with("Vorbis codebooks: no Plugins folder"),
        "{log:?}"
    );
    assert!(
        log[1].starts_with("Init.bnk (version 128): 0 media"),
        "{log:?}"
    );
    assert!(
        log[2].contains("4 media (0 vorbis, 3 adpcm, 1 pcm")
            && log[2].contains("1 vorbis left out (no codebooks)"),
        "{log:?}"
    );
    assert!(
        log[3].contains("2 events") && log[3].contains("0 unresolved"),
        "{log:?}"
    );

    let init = load(&d.join("out/Init.uaww.json"));
    assert_eq!(init["bank"]["id"], "1355168291");
    assert_eq!(init["buses"][MASTER.to_string()]["vol"], -2.0);
    let mb = &init["buses"][MUSIC_BUS.to_string()];
    assert_eq!(mb["parent"], MASTER.to_string());
    assert_eq!(mb["vol"], -4.5);
    assert_eq!(
        mb["states"][STATE_GROUP.to_string()][STATE_A.to_string()]["vol"],
        -12.0
    );
    assert_eq!(init["rtpc"]["4242"][0]["target"], MUSIC_BUS.to_string());
    assert_eq!(
        init["rtpc"]["4242"][0]["curve"][0],
        serde_json::json!([0.0, -3.0, 9])
    );
    let df = &init["defaults"];
    assert_eq!(df["rtpc"]["4242"], 25.0);
    assert_eq!(df["state_groups"][STATE_GROUP.to_string()]["fade_ms"], 300);
    assert_eq!(
        df["switch_rtpc"][SWITCH_GROUP.to_string()]["curve"][1],
        serde_json::json!([50.0, SW_B.to_string()])
    );

    let sfx = load(&d.join("out/SFX.uaww.json"));
    let n = &sfx["nodes"];
    let m = &n["10"];
    assert_eq!(
        (m["type"].as_str(), m["bus"].as_str(), m["vol"].as_f64()),
        (Some("mixer"), Some("3803692087"), Some(-1.0))
    );
    assert_eq!(m["limit"], 3);
    assert_eq!(m["limit_newest"], true);
    assert_eq!(m["rand_pitch"], serde_json::json!([-50.0, 50.0]));
    assert_eq!(
        m["states"][STATE_GROUP.to_string()][STATE_A.to_string()]["pitch"],
        100.0
    );
    assert_eq!(sfx["rtpc"]["4242"][0]["scaling"], "db");
    let sw = &n["20"];
    assert_eq!(sw["type"], "switch");
    assert_eq!(sw["map"][SW_B.to_string()], serde_json::json!(["40"]));
    assert_eq!(sw["default"], SW_A.to_string());
    assert_eq!(sw["continuous"], true);
    let r = &n["30"];
    assert_eq!(r["type"], "random");
    assert_eq!(r["children"], serde_json::json!(["31", "32"]));
    assert_eq!(r["weights"], serde_json::json!([70.0, 30.0]));
    assert_eq!(
        (
            r["pitch"].as_f64(),
            r["avoid_repeat"].as_u64(),
            r["shuffle"].as_bool()
        ),
        (Some(120.0), Some(1), Some(true))
    );
    assert_eq!(r["reset"], true);
    let q = &n["40"];
    assert_eq!(q["type"], "sequence");
    assert_eq!(q["children"], serde_json::json!(["42", "41"]));
    assert_eq!(
        (
            q["continuous"].as_bool(),
            q["loop"].as_u64(),
            q["transition"].as_str()
        ),
        (Some(true), Some(0), Some("delay"))
    );
    assert_eq!(q["transition_ms"], 500.0);
    assert_eq!(n["31"]["loop"], 0);
    assert_eq!(n["31"]["delay_ms"], 250.0);
    assert_eq!(n["32"]["vol"], -6.0);
    assert_eq!(n["42"]["media"], "2000");
    assert_eq!(n["50"]["type"], "layer");
    let pl = &n["60"];
    assert_eq!(pl["type"], "playlist");
    assert_eq!(
        pl["items"],
        serde_json::json!({"kind": "sequence", "loop": 0, "children": [{"segment": "70", "loop": 2}]})
    );
    assert_eq!(
        pl["rules"][0],
        serde_json::json!({"src": ["-1"], "dst": ["-1"], "fade_out_ms": 250, "fade_in_ms": 100, "sync": 7})
    );
    let sg = &n["70"];
    assert_eq!(
        (
            sg["duration_ms"].as_f64(),
            sg["entry_ms"].as_f64(),
            sg["exit_ms"].as_f64()
        ),
        (Some(2000.0), Some(10.0), Some(1900.0))
    );
    let tr = &n["80"];
    assert_eq!(
        tr["clips"],
        serde_json::json!([{"media": "1002", "at_ms": 100.0, "begin_ms": 0.0, "end_ms": -5.0, "src_ms": 1500.0}])
    );
    assert_eq!(tr["bus"], MUSIC_BUS.to_string());
    let ms = &n["90"];
    assert_eq!(
        ms["args"],
        serde_json::json!([{"group": STATE_GROUP.to_string(), "state": true}])
    );
    assert_eq!(
        ms["tree"],
        serde_json::json!({"0": "0", STATE_A.to_string(): "60"})
    );
    assert!(n.get("666").is_none());

    let e = &sfx["events"];
    assert_eq!(
        e["7000"][0],
        serde_json::json!({"action": "play", "type": 0x0403, "target": "10", "delay_ms": 30.0, "bank": "393239870"})
    );
    assert_eq!(e["7000"][1]["action"], "other");
    let e1 = &e["7001"];
    assert_eq!(
        e1[1],
        serde_json::json!({"action": "stop", "type": 0x0103, "target": "60", "object": true, "except": ["41"], "curve": 6, "delay_ms": 30.0})
    );
    assert_eq!(
        e1[2],
        serde_json::json!({"action": "set_state", "type": 0x1204, "group": STATE_GROUP.to_string(), "value": STATE_A.to_string(), "delay_ms": 30.0})
    );
    assert_eq!(e1[3]["action"], "set_bus_volume");
    assert_eq!(
        (
            e1[3]["amount"].as_f64(),
            e1[3]["meaning"].as_str(),
            e1[3]["bus"].as_bool()
        ),
        (Some(-4.5), Some("offset"), Some(true))
    );

    // the media: ADPCM's data as it is, PCM's, the streamed one from its .wem; 16-byte aligned
    let md = &sfx["media"];
    let pak = std::fs::read(d.join("out/SFX.uaww.pak")).unwrap();
    let a = &md["1001"];
    assert_eq!(
        (
            a["codec"].as_str(),
            a["samples"].as_u64(),
            a["block_align"].as_u64()
        ),
        (Some("adpcm"), Some(3 * 64), Some(36))
    );
    assert_eq!(a["loop"], serde_json::json!([10, 100]));
    let a2 = &md["1003"];
    assert_eq!(
        (
            a2["channels"].as_u64(),
            a2["samples"].as_u64(),
            a2["len"].as_u64()
        ),
        (Some(2), Some(2 * 64), Some(2 * 72))
    );
    let p = &md["1002"];
    assert_eq!(
        (
            p["codec"].as_str(),
            p["samples"].as_u64(),
            p["rate"].as_u64()
        ),
        (Some("pcm"), Some(100), Some(48000))
    );
    let (off, len) = (
        p["off"].as_u64().unwrap() as usize,
        p["len"].as_u64().unwrap() as usize,
    );
    assert!(pak[off..off + len].iter().all(|&x| x == 0x11));
    assert_eq!(md["2000"]["samples"], 64);
    for v in md.as_object().unwrap().values() {
        assert_eq!(v["off"].as_u64().unwrap() % 16, 0);
    }
    let first = md["1001"]["off"].as_u64().unwrap() as usize;
    assert_eq!(&pak[first..first + 4], &[0, 0, 10, 0]);

    // the same banks again: the same bytes
    let s2 = wwise::run(
        &d.join("banks"),
        &d.join("out2"),
        &wwise::Opts::default(),
        &mut |_| {},
    )
    .unwrap();
    assert_eq!(s2.problems.len(), 2);
    for f in ["SFX.uaww.json", "SFX.uaww.pak", "Init.uaww.json"] {
        assert_eq!(
            std::fs::read(d.join("out").join(f)).unwrap(),
            std::fs::read(d.join("out2").join(f)).unwrap(),
            "{f}"
        );
    }
    let _ = std::fs::remove_dir_all(&d);
}

#[test]
fn check_finds_what_is_missing() {
    let d = tmp("missing");
    sfx_bank(&d, false); // no Init: its buses are missing
    std::fs::remove_file(d.join("banks/2000.wem")).unwrap();
    let s = wwise::run(
        &d.join("banks"),
        &d.join("out"),
        &wwise::Opts {
            check: true,
            ..Default::default()
        },
        &mut |_| {},
    )
    .unwrap();
    assert!(
        s.problems.iter().any(|p| p.contains("streamed media 2000")),
        "{:?}",
        s.problems
    );
    assert!(
        s.problems
            .iter()
            .any(|p| p.contains("media 2000 not found")),
        "{:?}",
        s.problems
    );
    assert!(
        s.problems.iter().any(|p| p.contains("bus 1000 not found")),
        "{:?}",
        s.problems
    );
    assert!(s.check_failures >= 3);
    let _ = std::fs::remove_dir_all(&d);
}

#[test]
fn other_versions_are_refused() {
    let d = tmp("version");
    bank(&d.join("banks/Init.bnk"), 134, 1, &[], &[], None);
    match wwise::run(
        &d.join("banks"),
        &d.join("out"),
        &wwise::Opts::default(),
        &mut |_| {},
    ) {
        Err(wwise::Error::Unsupported(e)) => {
            assert!(e.contains("version 134 (Wwise 2019.1)"), "{e}")
        }
        r => panic!("{r:?}"),
    }
    assert!(!d.join("out").exists());
    std::fs::write(d.join("banks/Init.bnk"), b"RIFF....").unwrap();
    assert!(matches!(
        wwise::run(
            &d.join("banks"),
            &d.join("out"),
            &wwise::Opts::default(),
            &mut |_| {}
        ),
        Err(wwise::Error::Fail(_))
    ));
    let _ = std::fs::remove_dir_all(&d);
}

#[test]
fn vorbis_codebooks_from_the_games_library() {
    // the game's Wwise library in the Plugins folder above the banks: its codebooks are found
    // (and given to ww2ogg, which then stops at the made-up Vorbis data: that media is left out)
    let d = tmp("codebooks");
    sfx_bank(&d, false);
    let lib = d.join("Plugins/x86_64/libAkSoundEngine.so");
    std::fs::create_dir_all(lib.parent().unwrap()).unwrap();
    std::fs::write(&lib, wwise_library(70)).unwrap();
    std::fs::write(d.join("Plugins/x86_64/libAkFlanger.so"), wwise_library(80)).unwrap();
    let mut log = Vec::new();
    let s = wwise::run(
        &d.join("banks"),
        &d.join("out"),
        &wwise::Opts::default(),
        &mut |l| log.push(l.to_string()),
    )
    .unwrap();
    assert!(
        log[0].starts_with(&format!(
            "Vorbis codebooks: 70 from {} (at 0x3e7; sha256 ",
            lib.display()
        )),
        "{log:?}"
    );
    assert!(
        s.problems
            .iter()
            .any(|p| p.contains("media 1004 left out: Vorbis: ")),
        "{:?}",
        s.problems
    );
    assert!(
        !s.problems.iter().any(|p| p.contains("no codebooks")),
        "{:?}",
        s.problems
    );
    // --wwise-lib: a library without codebooks (too short a run) gives none
    std::fs::write(&lib, wwise_library(10)).unwrap();
    let mut log = Vec::new();
    let o = wwise::Opts {
        wwise_lib: Some(lib.clone()),
        ..Default::default()
    };
    let s = wwise::run(&d.join("banks"), &d.join("out"), &o, &mut |l| {
        log.push(l.to_string())
    })
    .unwrap();
    assert!(
        log[0].starts_with("Vorbis codebooks: none found in "),
        "{log:?}"
    );
    assert!(
        s.problems
            .iter()
            .any(|p| p.contains("1 Vorbis media left out")),
        "{:?}",
        s.problems
    );
    let _ = std::fs::remove_dir_all(&d);
}
