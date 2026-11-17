//! Made-up pieces of an FMOD bank (no FMOD data): the strings tree, an FSB5 header, an event with its
//! timeline, instruments and a sheet whose box is a condition, through `bank::parse`.

fn x16(n: u32) -> Vec<u8> {
    (n as u16).to_le_bytes().to_vec()
}

fn list(elems: &[Vec<u8>]) -> Vec<u8> {
    if elems.is_empty() {
        return x16(1);
    }
    let mut v = x16(elems.len() as u32 * 2 + 1);
    v.extend_from_slice(&(elems[0].len() as u16).to_le_bytes());
    for e in elems {
        v.extend_from_slice(e);
    }
    v
}

fn k(v: &[u8]) -> [u8; 16] {
    v.try_into().unwrap()
}

fn g(n: u8) -> Vec<u8> {
    let mut v = vec![0u8; 16];
    v[0] = n;
    v[15] = 0x80;
    v
}

fn chunk(id: &[u8; 4], body: &[u8]) -> Vec<u8> {
    let mut v = id.to_vec();
    v.extend_from_slice(&(body.len() as u32).to_le_bytes());
    v.extend_from_slice(body);
    if body.len() % 2 == 1 {
        v.push(0);
    }
    v
}

fn list_chunk(typ: &[u8; 4], parts: &[Vec<u8>]) -> Vec<u8> {
    let mut body = typ.to_vec();
    for p in parts {
        body.extend_from_slice(p);
    }
    chunk(b"LIST", &body)
}

fn bank(parts: &[Vec<u8>]) -> Vec<u8> {
    let mut body = b"FEV ".to_vec();
    let mut fmt = 0x67u32.to_le_bytes().to_vec();
    fmt.extend_from_slice(&0x63u32.to_le_bytes());
    body.extend_from_slice(&chunk(b"FMT ", &fmt));
    body.extend_from_slice(&list_chunk(b"PROJ", parts));
    chunk(b"RIFF", &body)
}

fn u24(x: u32) -> [u8; 3] {
    [x as u8, (x >> 8) as u8, (x >> 16) as u8]
}

#[test]
fn strings_tree() {
    // root -> "event:/" -> {"a" (leaf 0), "b" -> {"" (leaf 1), "c" (leaf 2)}}
    let text = b"event:/\0a\0b\0c\0";
    let node = |label: u32, c: u8, first: u32, n: u8| {
        let mut v = u24(label).to_vec();
        v.push(c);
        v.extend_from_slice(&u24(first));
        v.push(n);
        v
    };
    let nodes = [
        node(0xff_ffff, 0, 1, 1),
        node(0, b'e', 2, 2),
        node(8, b'a', 0, 0),
        node(10, b'b', 4, 2),
        node(0xff_ffff, 0, 1, 0),
        node(12, b'c', 2, 0),
    ];
    let mut d = 1u32.to_le_bytes().to_vec();
    d.extend_from_slice(&list(&nodes.to_vec()));
    d.extend_from_slice(&list(&[g(1), g(2), g(3)]));
    d.extend_from_slice(&(text.len() as u16).to_le_bytes());
    d.extend_from_slice(text);
    let s = fmod::strings::read(&d).unwrap();
    let names: Vec<(&str, u8)> = s.iter().map(|(p, gd)| (p.as_str(), gd[0])).collect();
    assert_eq!(names, vec![("event:/a", 1), ("event:/b", 2), ("event:/bc", 3)]);
}

#[test]
fn fsb5_sample_headers() {
    // two samples, stored out of their data's order; a chunk 11 with the setup header's id
    let mut f = b"FSB5".to_vec();
    let raw = |rate: u64, chcode: u64, off: u64, samples: u64, more: u64| more | rate << 1 | chcode << 5 | (off / 32) << 7 | samples << 34;
    let mut sh = Vec::new();
    sh.extend_from_slice(&raw(9, 1, 64, 1000, 1).to_le_bytes());
    sh.extend_from_slice(&((4u32 << 1) | (11 << 25)).to_le_bytes());
    sh.extend_from_slice(&0xabcd_ef01u32.to_le_bytes());
    sh.extend_from_slice(&raw(9, 0, 0, 500, 0).to_le_bytes());
    let data = vec![7u8; 100];
    for x in [1u32, 2, sh.len() as u32, 0, data.len() as u32, 15] {
        f.extend_from_slice(&x.to_le_bytes());
    }
    f.resize(0x3c, 0);
    f.extend_from_slice(&sh);
    f.extend_from_slice(&data);
    let r = fmod::fsb::read(&f, 0, f.len()).unwrap();
    assert_eq!(r.codec, 15);
    assert_eq!((r.samples[0].channels, r.samples[0].rate, r.samples[0].samples), (2, 48000, 1000));
    assert_eq!(r.samples[0].setup_id, Some(0xabcd_ef01));
    assert_eq!(r.samples[1].channels, 1);
    let base = 0x3c + sh.len();
    assert_eq!(r.samples[0].data, (base + 64, 36));
    assert_eq!(r.samples[1].data, (base, 64));
}

#[test]
fn event_timeline_and_conditions() {
    let (ev, tl, inst, wav, multi, param, lay, master, track) = (g(1), g(2), g(3), g(4), g(5), g(6), g(7), g(8), g(9));
    let mut evtb = [ev.clone(), vec![0; 16], tl.clone(), g(10), master.clone()].concat();
    for x in [5i32, 128] {
        evtb.extend_from_slice(&x.to_le_bytes());
    }
    evtb.push(0);
    evtb.extend_from_slice(&1i32.to_le_bytes());
    evtb.extend_from_slice(&list(&[]));
    evtb.extend_from_slice(&x16(1));
    evtb.extend_from_slice(&x16(1));
    for x in [0f32.to_bits(), 3, 0f32.to_bits(), 1] {
        evtb.extend_from_slice(&x.to_le_bytes());
    }
    let tbox = |i: &Vec<u8>, s: u32, l: u32| [i.clone(), s.to_le_bytes().to_vec(), l.to_le_bytes().to_vec()].concat();
    let mut tlnb = [tl.clone(), ev.clone()].concat();
    tlnb.extend_from_slice(&list(&[tbox(&multi, 0, 48000)]));
    tlnb.extend_from_slice(&list(&[]));
    tlnb.extend_from_slice(&list(&[]));
    // a named marker (its own size), a tempo marker
    let mut mk = [g(11), 96000u32.to_le_bytes().to_vec()].concat();
    mk.extend_from_slice(&x16(6));
    mk.extend_from_slice(b"Attack");
    let mut markers = x16(3);
    markers.extend_from_slice(&(mk.len() as u16).to_le_bytes());
    markers.extend_from_slice(&mk);
    tlnb.extend_from_slice(&markers);
    let tempo = [g(12), (4u64 | 4u64 << 32).to_le_bytes().to_vec(), 0u32.to_le_bytes().to_vec(), 90f32.to_le_bytes().to_vec()].concat();
    tlnb.extend_from_slice(&list(&[tempo]));
    let mut inst_body = [g(0)].concat();
    for x in [(-3f32).to_bits(), 0f32.to_bits(), 0] {
        inst_body.extend_from_slice(&x.to_le_bytes());
    }
    inst_body.push(0);
    for x in [0f32, 100.0, 0.0, 0.0] {
        inst_body.extend_from_slice(&x.to_le_bytes());
    }
    inst_body.extend_from_slice(&3u32.to_le_bytes());
    inst_body.extend_from_slice(&[0; 16]);
    for x in [0f32, 0.0] {
        inst_body.extend_from_slice(&x.to_le_bytes());
    }
    inst_body.extend_from_slice(&0i32.to_le_bytes());
    inst_body.extend_from_slice(&26u16.to_le_bytes());
    inst_body.extend_from_slice(&track);
    inst_body.extend_from_slice(&[0; 8]);
    for x in [0u32, 0, 0, 0] {
        inst_body.extend_from_slice(&x.to_le_bytes());
    }
    let mut plst = [0i32.to_le_bytes(), 1i32.to_le_bytes()].concat();
    plst.extend_from_slice(&list(&[[inst.clone(), 1f32.to_le_bytes().to_vec()].concat()]));
    plst.push(0);
    let wavb = [wav.clone(), 12u16.to_le_bytes().to_vec(), 0i32.to_le_bytes().to_vec(), 7i32.to_le_bytes().to_vec(), 0u32.to_le_bytes().to_vec()].concat();
    let mut prmb = param.clone();
    prmb.push(0);
    prmb.extend_from_slice(&0u32.to_le_bytes());
    prmb.extend_from_slice(&x16(5));
    prmb.extend_from_slice(b"Phase");
    for x in [0f32, 3.0, 0.0, 0.0, 0.3] {
        prmb.extend_from_slice(&x.to_le_bytes());
    }
    prmb.push(0);
    prmb.extend_from_slice(&0.1f32.to_le_bytes());
    prmb.push(0);
    let mut pmlb = [lay.clone(), param.clone(), ev.clone()].concat();
    pmlb.extend_from_slice(&list(&[[multi.clone(), 2f32.to_le_bytes().to_vec(), 0f32.to_le_bytes().to_vec()].concat()]));
    let d = bank(&[
        list_chunk(b"EVTS", &[list_chunk(b"EVNT", &[chunk(b"EVTB", &evtb)])]),
        list_chunk(b"TLNS", &[list_chunk(b"TMLN", &[chunk(b"TLNB", &tlnb)])]),
        list_chunk(b"PMLS", &[list_chunk(b"PMLO", &[chunk(b"PMLB", &pmlb)])]),
        list_chunk(b"PRMS", &[list_chunk(b"PARM", &[chunk(b"PRMB", &prmb)])]),
        list_chunk(b"MUIS", &[list_chunk(b"MUIT", &[chunk(b"MUIB", &multi), chunk(b"PLST", &plst), chunk(b"INST", &inst_body)])]),
        list_chunk(b"WAIS", &[list_chunk(b"WAIT", &[chunk(b"WAIB", &[inst.clone(), wav.clone()].concat()), chunk(b"INST", &inst_body)])]),
        list_chunk(b"WAVS", &[chunk(b"WAV ", &wavb)]),
    ]);
    let b = fmod::bank::parse(&d).unwrap();
    assert!(b.problems.is_empty(), "{:?}", b.problems);
    let e = &b.events[&k(&ev)];
    assert_eq!((e.max_poly, e.steal), (5, 3));
    let t = &b.timelines[&k(&tl)];
    assert_eq!((t.async_boxes.len(), t.async_boxes[0].len), (1, 48000));
    assert_eq!((t.markers[0].pos, t.markers[0].name.as_str()), (96000, "Attack"));
    assert_eq!((t.tempo[0].bpm, t.tempo[0].upper, t.tempo[0].lower), (90.0, 4, 4));
    let m = &b.insts[&k(&multi)];
    assert_eq!(m.playlist.as_ref().unwrap().2.len(), 1);
    assert_eq!(m.body.as_ref().unwrap().vol, -3.0);
    assert_eq!(b.wavs[&k(&wav)].subsound, 7);
    assert_eq!(b.params[&k(&param)].name, "Phase");
    assert_eq!(b.layouts[0].boxes[0].1, 2.0);
}
