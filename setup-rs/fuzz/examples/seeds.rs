//! `seeds <corpus>`: writes made-up seed inputs for every fuzz target to <corpus>/<target>/:
//! zips, installers, packages, cpio archives and compressed streams built here; Unity files
//! from the convert tests' made-up data folder (tests/madeup, which needs $PMU_TPK: UnityPy's
//! lzma.tpk) and what is in them (type trees, shader blobs and programs, textures); HTTP
//! responses, journals and command lines. Nothing is read from a game or a Unity install.
#[path = "../../crates/convert/tests/madeup/mod.rs"]
mod madeup;

use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use unityfs::bin::Writer;
use unityfs::bundle::{pack, PackHeader, PackNode};
use unityfs::typetree::CommonStrings;
use unityfs::{DataFile, MemSource, Options, SerializedFile, SharedSource, Value};

struct Seeds {
    root: PathBuf,
    n: usize,
}

impl Seeds {
    fn put(&mut self, target: &str, data: &[u8]) {
        let d = self.root.join(target);
        std::fs::create_dir_all(&d).unwrap();
        std::fs::write(d.join(format!("seed-{:04}", self.n)), data).unwrap();
        self.n += 1;
    }
}

fn le16(v: &mut Vec<u8>, x: u16) {
    v.extend_from_slice(&x.to_le_bytes());
}

fn le32(v: &mut Vec<u8>, x: u32) {
    v.extend_from_slice(&x.to_le_bytes());
}

fn deflate(data: &[u8]) -> Vec<u8> {
    let mut e = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::default());
    e.write_all(data).unwrap();
    e.finish().unwrap()
}

fn zlib(data: &[u8]) -> Vec<u8> {
    let mut e = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
    e.write_all(data).unwrap();
    e.finish().unwrap()
}

fn gzip(data: &[u8]) -> Vec<u8> {
    let mut e = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::default());
    e.write_all(data).unwrap();
    e.finish().unwrap()
}

fn bzip2(data: &[u8]) -> Vec<u8> {
    let mut e = bzip2::write::BzEncoder::new(Vec::new(), bzip2::Compression::default());
    e.write_all(data).unwrap();
    e.finish().unwrap()
}

fn xz(data: &[u8]) -> Vec<u8> {
    let mut opts = lzma_rust2::XzOptions::with_preset(1);
    opts.lzma_options.dict_size = 1 << 16;
    let mut w = lzma_rust2::XzWriter::new(Vec::new(), opts).unwrap();
    w.write_all(data).unwrap();
    w.finish().unwrap()
}

/// A raw LZMA stream (with an end marker or not), its properties byte and dictionary size.
fn lzma_raw(data: &[u8], eos: bool) -> (u8, u32, Vec<u8>) {
    let mut opts = lzma_rust2::LzmaOptions::with_preset(1);
    opts.dict_size = 1 << 16;
    let mut w = lzma_rust2::LzmaWriter::new_no_header(Vec::new(), &opts, eos).unwrap();
    let props = w.props();
    w.write_all(data).unwrap();
    (props, opts.dict_size, w.finish().unwrap())
}

/// An .lzma (LZMA_Alone) file: properties, dictionary size, the size unknown, the stream.
fn lzma_alone(data: &[u8]) -> Vec<u8> {
    let (props, dict, s) = lzma_raw(data, true);
    let mut v = vec![props];
    le32(&mut v, dict);
    v.extend_from_slice(&u64::MAX.to_le_bytes());
    v.extend_from_slice(&s);
    v
}

/// NSIS's and Unity's LZMA block: properties, dictionary size, the stream (Unity's without an
/// end marker).
fn lzma_block(data: &[u8], eos: bool) -> Vec<u8> {
    let (props, dict, s) = lzma_raw(data, eos);
    let mut v = vec![props];
    le32(&mut v, dict);
    v.extend_from_slice(&s);
    v
}

fn text(seed: u32, n: usize) -> Vec<u8> {
    (0..n as u32)
        .map(|i| b"abcdefgh ijklmnop\n"[((i * 7 + seed) % 18) as usize])
        .collect()
}

// ---- zip

/// A zip of (name, method, data): local headers, the central directory (zip64 sizes for the
/// entries flagged), the end record; data before the archive and a comment.
fn zip(entries: &[(&str, u16, &[u8], bool)], prefix: &[u8], comment: &[u8]) -> Vec<u8> {
    let mut out = prefix.to_vec();
    let mut cd = Vec::new();
    for &(name, method, data, z64) in entries {
        let (packed, flags) = match method {
            8 => (deflate(data), 0u16),
            12 => (bzip2(data), 0),
            14 => {
                let (props, dict, s) = lzma_raw(data, true);
                let mut v = vec![9, 4, 5, 0, props];
                le32(&mut v, dict);
                v.extend_from_slice(&s);
                (v, 2)
            }
            _ => (data.to_vec(), 0),
        };
        let crc = crc32fast::hash(data);
        let at = (out.len() - prefix.len()) as u32;
        let mut h = b"PK\x03\x04".to_vec();
        le16(&mut h, if method == 14 { 63 } else { 20 });
        le16(&mut h, flags);
        le16(&mut h, method);
        h.extend([0, 0, 0x21, 0]);
        le32(&mut h, crc);
        le32(&mut h, packed.len() as u32);
        le32(&mut h, data.len() as u32);
        le16(&mut h, name.len() as u16);
        le16(&mut h, 0);
        let mut c = b"PK\x01\x02".to_vec();
        c.extend([20, 3]);
        c.extend(&h[4..18]);
        if z64 {
            le32(&mut c, u32::MAX);
            le32(&mut c, u32::MAX);
        } else {
            c.extend(&h[18..26]);
        }
        le16(&mut c, name.len() as u16);
        le16(&mut c, if z64 { 28 } else { 0 });
        c.extend([0; 6]);
        le32(&mut c, 0o100644 << 16);
        le32(&mut c, if z64 { u32::MAX } else { at });
        c.extend(name.as_bytes());
        if z64 {
            le16(&mut c, 1);
            le16(&mut c, 24);
            c.extend((data.len() as u64).to_le_bytes());
            c.extend((packed.len() as u64).to_le_bytes());
            c.extend((at as u64).to_le_bytes());
        }
        cd.extend(c);
        out.extend(h);
        out.extend(name.as_bytes());
        out.extend(packed);
    }
    let start = (out.len() - prefix.len()) as u32;
    let n = entries.len() as u16;
    let size = cd.len() as u32;
    out.extend(cd);
    out.extend(b"PK\x05\x06\0\0\0\0");
    le16(&mut out, n);
    le16(&mut out, n);
    le32(&mut out, size);
    le32(&mut out, start);
    le16(&mut out, comment.len() as u16);
    out.extend(comment);
    out
}

fn zips(s: &mut Seeds) {
    let big = text(3, 70_000);
    s.put("zip", &zip(&[("a.txt", 0, b"stored", false)], b"", b""));
    s.put(
        "zip",
        &zip(
            &[
                ("d/", 0, b"", false),
                ("d/big", 8, &big, false),
                ("../up.txt", 0, b"up", false),
                ("b.bz2", 12, &text(1, 3000), false),
                ("c.lzma", 14, &text(2, 3000), false),
                ("dup", 0, b"one", false),
                ("dup", 0, b"two", false),
            ],
            b"a stub before the archive",
            b"comment",
        ),
    );
    s.put(
        "zip",
        &zip(&[("z64.bin", 8, &text(5, 5000), true)], b"", b""),
    );
}

// ---- NSIS

const EW_CREATEDIR: u32 = 11;
const EW_EXTRACTFILE: u32 = 20;

/// The script header of an installer with two files under $INSTDIR\Editor\Data: its entries
/// (28 bytes each), strings (ANSI) and empty tables. offs: the files' block offsets.
fn nsis_header(offs: [u32; 2]) -> Vec<u8> {
    let strings = b"\0$INSTDIR\\Editor\\Data\0a.txt\0b.bin\0";
    let eo = 4 + 64u32;
    let so = eo + 3 * 28;
    let lo = so + strings.len() as u32;
    let mut h = Vec::new();
    le32(&mut h, 0);
    for (off, num) in [
        (0, 0),
        (0, 0),
        (eo, 3),
        (so, 0),
        (lo, 0),
        (lo, 0),
        (lo, 0),
        (lo, 0),
    ] {
        le32(&mut h, off);
        le32(&mut h, num);
    }
    for w in [
        [EW_CREATEDIR, 1, 1, 0, 0, 0, 0],
        [EW_EXTRACTFILE, 0, 22, offs[0], 0, 0, 0],
        [EW_EXTRACTFILE, 0, 28, offs[1], 0, 0, 0],
    ] {
        for x in w {
            le32(&mut h, x);
        }
    }
    h.extend_from_slice(strings);
    h.extend_from_slice(&[0; 16]);
    h
}

/// An installer with per-file blocks: the first header at `stub` (a multiple of 512), the
/// script header compressed (LZMA or deflate), one file stored and one compressed.
fn nsis(stub: usize, lz: bool, bcj: bool) -> Vec<u8> {
    let comp = |d: &[u8]| {
        if lz {
            let b = lzma_block(d, true);
            if bcj {
                [&[1u8][..], &b].concat()
            } else {
                b
            }
        } else {
            deflate(d)
        }
    };
    let a = b"stored file".to_vec();
    let b = comp(&text(9, 4000));
    let mut data = Vec::new();
    le32(&mut data, a.len() as u32);
    data.extend(&a);
    let off_b = data.len() as u32;
    le32(&mut data, b.len() as u32 | 0x8000_0000);
    data.extend(&b);
    let h = nsis_header([0, off_b]);
    let hb = comp(&h);
    let mut out = vec![0u8; stub];
    if stub >= 2 {
        out[..2].copy_from_slice(b"MZ");
    }
    let fh = out.len();
    le32(&mut out, 0);
    le32(&mut out, 0xDEAD_BEEF);
    out.extend(b"NullsoftInst");
    le32(&mut out, h.len() as u32);
    let total_at = out.len();
    le32(&mut out, 0);
    le32(&mut out, hb.len() as u32 | 0x8000_0000);
    out.extend(&hb);
    out.extend(&data);
    let total = (out.len() - fh - 28) as u32;
    out[total_at..total_at + 4].copy_from_slice(&total.to_le_bytes());
    out
}

/// Unity 6's layout: 64-bit block words, each block LZMA chunks (3-byte lengths, up to 0).
fn nsis_chunked() -> Vec<u8> {
    let chunks = |d: &[u8]| {
        let mut v = Vec::new();
        for c in d.chunks(1500) {
            let b = lzma_block(c, true);
            v.extend(&(b.len() as u32).to_le_bytes()[..3]);
            v.extend(b);
        }
        v.extend([0, 0, 0]);
        v
    };
    let a = b"stored file".to_vec();
    let b = chunks(&text(4, 4000));
    let mut data = Vec::new();
    data.extend((a.len() as u64).to_le_bytes());
    data.extend(&a);
    let off_b = data.len() as u32;
    data.extend((b.len() as u64 | 1 << 63).to_le_bytes());
    data.extend(&b);
    let h = nsis_header([0, off_b]);
    let hb = chunks(&h);
    let mut out = Vec::new();
    le32(&mut out, 0x40);
    le32(&mut out, 0xDEAD_BEEF);
    out.extend(b"NullsoftInst");
    le32(&mut out, h.len() as u32);
    le32(&mut out, 0);
    out.extend(0u64.to_le_bytes());
    out.extend((hb.len() as u64 | 1 << 63).to_le_bytes());
    out.extend(&hb);
    out.extend(&data);
    let total = (out.len() - 36) as u32;
    out[24..28].copy_from_slice(&total.to_le_bytes());
    out
}

// ---- cpio, xar, streams

/// odc cpio of (name, data or None for a folder).
fn odc(entries: &[(&str, Option<&[u8]>)]) -> Vec<u8> {
    let mut out = Vec::new();
    for (name, data) in entries
        .iter()
        .copied()
        .chain([("TRAILER!!!", Some(&b""[..]))])
    {
        let mode = if data.is_none() { 0o040755 } else { 0o100644 };
        let data = data.unwrap_or(b"");
        let n = name.len() + 1;
        out.extend(
            format!(
                "070707{:06o}{:06o}{mode:06o}{:06o}{:06o}{:06o}{:06o}{:011o}{n:06o}{:011o}",
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                data.len()
            )
            .bytes(),
        );
        out.extend(name.bytes());
        out.push(0);
        out.extend(data);
    }
    out
}

/// newc cpio of (name, mode, data).
fn newc(entries: &[(&str, u32, &[u8])]) -> Vec<u8> {
    let mut out = Vec::new();
    for (name, mode, data) in entries.iter().copied().chain([("TRAILER!!!", 0, &b""[..])]) {
        let n = name.len() + 1;
        out.extend(b"070701");
        for v in [0, mode as usize, 0, 0, 1, 0, data.len(), 0, 0, 0, 0, n, 0] {
            out.extend(format!("{v:08X}").bytes());
        }
        out.extend(name.bytes());
        out.push(0);
        while out.len() % 4 != 0 {
            out.push(0);
        }
        out.extend(data);
        while out.len() % 4 != 0 {
            out.push(0);
        }
    }
    out
}

/// Apple's pbzx: 'pbzx', the chunk size, then (size, length, data) chunks, xz or stored.
fn pbzx(data: &[u8], chunk: usize) -> Vec<u8> {
    let mut out = b"pbzx".to_vec();
    out.extend((chunk as u64).to_be_bytes());
    for (i, c) in data.chunks(chunk).enumerate() {
        let body = if i % 2 == 0 { xz(c) } else { c.to_vec() };
        out.extend((c.len() as u64).to_be_bytes());
        out.extend((body.len() as u64).to_be_bytes());
        out.extend(body);
    }
    out
}

/// A file of a xar's TOC: its name, its data (bytes, encoding style) or none, its files.
struct XFile {
    name: &'static str,
    data: Option<(Vec<u8>, &'static str)>,
    files: Vec<XFile>,
}

fn xf(name: &'static str, data: Option<(Vec<u8>, &'static str)>, files: Vec<XFile>) -> XFile {
    XFile { name, data, files }
}

/// A xar: the header, the zlib TOC, the heap (each file's data encoded by its style).
fn xar(files: &[XFile]) -> Vec<u8> {
    fn walk(f: &XFile, id: &mut u32, heap: &mut Vec<u8>, xml: &mut String) {
        *id += 1;
        xml.push_str(&format!("<file id=\"{id}\"><name>{}</name>", f.name));
        if let Some((d, style)) = &f.data {
            let enc = match *style {
                "application/x-gzip" => zlib(d),
                "application/x-bzip2" => bzip2(d),
                _ => d.clone(),
            };
            xml.push_str(&format!(
                "<data><offset>{}</offset><length>{}</length><size>{}</size>\
                 <encoding style=\"{style}\"/></data><type>file</type>",
                heap.len(),
                enc.len(),
                d.len()
            ));
            heap.extend(enc);
        } else {
            xml.push_str("<type>directory</type>");
        }
        for c in &f.files {
            walk(c, id, heap, xml);
        }
        xml.push_str("</file>");
    }
    let mut xml = String::from("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xar><toc>");
    let (mut id, mut heap) = (0, Vec::new());
    for f in files {
        walk(f, &mut id, &mut heap, &mut xml);
    }
    xml.push_str("</toc></xar>");
    let toc = zlib(xml.as_bytes());
    let mut out = b"xar!".to_vec();
    out.extend(28u16.to_be_bytes());
    out.extend(1u16.to_be_bytes());
    out.extend((toc.len() as u64).to_be_bytes());
    out.extend((xml.len() as u64).to_be_bytes());
    out.extend(0u32.to_be_bytes());
    out.extend(toc);
    out.extend(heap);
    out
}

fn packages(s: &mut Seeds) {
    let odc1 = odc(&[
        (".", None),
        ("./Game.app", None),
        ("./Game.app/Contents", None),
        ("./Game.app/Contents/Info.plist", Some(b"<plist/>")),
        ("./Game.app/Contents/._Info.plist", Some(b"meta")),
    ]);
    let newc1 = newc(&[
        ("payload", 0o040755, b""),
        ("payload/Game.app", 0o040755, b""),
        ("payload/Game.app/data.bin", 0o100644, &text(6, 3000)),
        ("payload/link", 0o120777, b"Game.app"),
        (
            "postinstall",
            0o100755,
            b"#!/bin/sh\ngog_bundle_name=\"Game.app\"\nmv payload/* \"$2\"\n",
        ),
    ]);
    s.put("cpio", &odc1);
    s.put("cpio", &newc1);
    let info = |loc: &str| {
        format!("<?xml version=\"1.0\"?><pkg-info install-location=\"{loc}\" identifier=\"x\"/>")
            .into_bytes()
    };
    s.put(
        "macpkg",
        &xar(&[
            xf(
                "PackageInfo",
                Some((info("/Applications"), "application/octet-stream")),
                vec![],
            ),
            xf(
                "Payload",
                Some((gzip(&odc1), "application/octet-stream")),
                vec![],
            ),
        ]),
    );
    s.put(
        "macpkg",
        &xar(&[xf(
            "game.pkg",
            None,
            vec![
                xf(
                    "PackageInfo",
                    Some((info("/"), "application/x-gzip")),
                    vec![],
                ),
                xf(
                    "Payload",
                    Some((pbzx(&odc1, 200), "application/octet-stream")),
                    vec![],
                ),
                xf(
                    "Scripts",
                    Some((gzip(&newc1), "application/x-bzip2")),
                    vec![],
                ),
            ],
        )]),
    );
    s.put("pbzx", &[&[0u8][..], &zlib(&text(1, 2000))].concat());
    s.put(
        "pbzx",
        &[&[1u8][..], &bzip2(&text(2, 2000)), &bzip2(b"two")].concat(),
    );
    s.put(
        "pbzx",
        &[&[2u8][..], &gzip(b"one "), &gzip(b"two"), &[0; 4]].concat(),
    );
    s.put(
        "pbzx",
        &[&[3u8][..], &xz(&text(3, 2000)), &xz(b"again")].concat(),
    );
    s.put("pbzx", &[&[3u8][..], &lzma_alone(&text(4, 2000))].concat());
    s.put("pbzx", &[&[4u8][..], &pbzx(&newc1, 300)].concat());
    s.put("pbzx", &[&[4u8][..], &gzip(&odc1)].concat());
    s.put("pbzx", &[&[4u8][..], &odc1].concat());
}

// ---- HTTP

fn http(s: &mut Seeds) {
    for r in [
        &b"HTTP/1.1 206 Partial Content\r\nContent-Length: 5\r\nContent-Range: bytes 0-4/100\r\n\r\nhello"[..],
        b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;x=y\r\n world\r\n0\r\nTrailer: t\r\n\r\n",
        b"HTTP/1.1 302 Found\r\nLocation: ../other/Setup.exe?x=1\r\nContent-Length: 0\r\n\r\n",
        b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n",
        b"HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\nto the end",
        b"HTTP/1.1 301 Moved\r\nLocation: //cdn.example.com/x\r\n\r\n",
    ] {
        s.put("http", r);
    }
}

// ---- Unity files

/// A bundle of the nodes written by unityfs's packer: the blocks info at the end or after the
/// header, LZ4 or stored blocks.
fn packed(nodes: &[(&str, u32, Vec<u8>)], data_flag: u32, block_flag: u32, format: u32) -> Vec<u8> {
    let h = PackHeader {
        signature: "UnityFS".into(),
        format,
        player: "5.x.x".into(),
        engine: if format >= 7 {
            "2021.3.5f1"
        } else {
            madeup::VERSION
        }
        .into(),
        aligned: format >= 7,
    };
    let nodes: Vec<PackNode> = nodes
        .iter()
        .map(|(n, f, d)| PackNode {
            name: n.to_string(),
            flags: *f,
            src: Arc::new(MemSource(d.clone())) as SharedSource,
        })
        .collect();
    let mut out = std::io::Cursor::new(Vec::new());
    pack(&mut out, &h, &nodes, data_flag, block_flag, 1).unwrap();
    out.into_inner()
}

/// A bundle of one LZMA block (Unity's 5-byte header), its blocks info LZ4 after the header.
fn lzma_bundle(nodes: &[(&str, u32, Vec<u8>)]) -> Vec<u8> {
    let data: Vec<u8> = nodes.iter().flat_map(|n| n.2.clone()).collect();
    let z = lzma_block(&data, false);
    let mut info = Writer::new(true);
    info.bytes(&[0; 16]);
    info.i32(1);
    info.u32(data.len() as u32);
    info.u32(z.len() as u32);
    info.u16(1);
    info.i32(nodes.len() as i32);
    let mut off = 0i64;
    for n in nodes {
        info.i64(off);
        info.i64(n.2.len() as i64);
        info.u32(n.1);
        info.cstr(n.0.as_bytes());
        off += n.2.len() as i64;
    }
    let ci = unityfs::compress::lz4hc(&info.buf, 9).unwrap();
    let mut h = Writer::new(true);
    h.cstr(b"UnityFS");
    h.u32(6);
    h.cstr(b"5.x.x");
    h.cstr(madeup::VERSION.as_bytes());
    h.i64((h.len() + 20 + ci.len() + z.len()) as i64);
    h.u32(ci.len() as u32);
    h.u32(info.len() as u32);
    h.u32(0x43);
    h.bytes(&ci);
    h.bytes(&z);
    h.buf
}

/// The old nested type tree format (formats below 10, and 11): each node, then its children.
fn old_tree(t: &unityfs::TypeTree, i: usize, w: &mut Writer) {
    let n = &t.nodes[i];
    w.cstr(n.type_name.as_bytes());
    w.cstr(n.name.as_bytes());
    w.i32(n.byte_size);
    w.i32(n.index.unwrap_or(0));
    w.i32(n.type_flags.unwrap_or(0));
    w.i32(n.version);
    w.i32(n.meta_flag.unwrap_or(0));
    w.i32(n.children.len() as i32);
    for &c in &n.children {
        old_tree(t, c, w);
    }
}

/// What a serialized file gives the other targets: each type's tree with an object of it
/// (typetree), its shaders' blobs and programs (shader_blob, glsl, grabfetch), its textures.
fn contents(s: &mut Seeds, sf: &SerializedFile, f: Option<&DataFile>) {
    let mut seen = std::collections::HashSet::new();
    for o in &sf.objects {
        let Ok(t) = sf.typetree(o) else { continue };
        let Ok(bytes) = sf.original_bytes(o.path_id) else {
            continue;
        };
        if seen.insert(o.class_id) {
            let mut t2 = (*t).clone();
            for (i, n) in t2.nodes.iter_mut().enumerate() {
                n.index = Some(i as i32);
                n.type_flags.get_or_insert(0);
                n.meta_flag.get_or_insert(0);
                n.ref_type_hash.get_or_insert(0);
            }
            // formats 17 (index 5) and 22 (index 8), little endian; the old format 9 (index 0)
            for (sel, version) in [(5u8, 17u32), (8, 22)] {
                let mut w = Writer::new(false);
                if t2
                    .dump_blob(&mut w, version, &CommonStrings::default())
                    .is_ok()
                {
                    s.put("typetree", &[&[sel][..], &w.buf, &bytes].concat());
                }
            }
            let mut w = Writer::new(false);
            old_tree(&t2, 0, &mut w);
            s.put("typetree", &[&[0u8][..], &w.buf, &bytes].concat());
        }
        let Ok(v) = sf.read_typetree(o.path_id) else {
            continue;
        };
        match o.class_id {
            48 => shader_seeds(s, &v),
            28 => texture_seeds(s, &v, f),
            _ => {}
        }
    }
}

fn int3(v: &mut Vec<u8>, x: i64) {
    v.extend_from_slice(&x.to_le_bytes()[..3]);
}

fn shader_seeds(s: &mut Seeds, v: &Value) {
    let Ok(f) = shaderx::tree::blob_fields(v) else {
        return;
    };
    if let (
        shaderx::blob::Lengths::Flat(o),
        shaderx::blob::Lengths::Flat(c),
        shaderx::blob::Lengths::Flat(d),
    ) = (&f.offsets, &f.compressed_lengths, &f.decompressed_lengths)
    {
        let n = f.platforms.len().min(4);
        let mut b = vec![((n.max(1) - 1) << 2) as u8];
        for i in 0..n {
            b.push(f.platforms[i] as u8);
            int3(&mut b, o[i]);
            int3(&mut b, c[i]);
            int3(&mut b, d[i]);
        }
        b.push(0); // no parameter entries
        b.extend(0u16.to_le_bytes());
        b.extend(&f.compressed_blob);
        s.put("shader_blob", &b);
    }
    let Ok(blobs) = shaderx::tree::blobs(v) else {
        return;
    };
    for p in blobs.platforms.keys() {
        let Ok(entries) = blobs.entries(*p, &Default::default(), None) else {
            continue;
        };
        for e in entries.into_iter().flatten() {
            let shaderx::blob::Slot::Program(e) = e else {
                continue;
            };
            if e.code.is_empty() {
                continue;
            }
            // (one platform of empty lengths, no parameters: the entry on its own)
            s.put("shader_blob", &[&[0u8; 14][..], &e.serialize()].concat());
            let code = String::from_utf8_lossy(&e.code).into_owned();
            s.put("glsl", &[&[0x60u8][..], code.as_bytes()].concat());
            let mut notes = Vec::new();
            if let Ok(es) = shaderx::glsl::program_to_es(
                &code,
                "highp",
                shaderx::glsl::Optimize::ALL,
                &mut notes,
                shaderx::glsl::Es::Es300,
            ) {
                s.put("grabfetch", es.as_bytes());
            }
        }
    }
}

fn texture_seeds(s: &mut Seeds, v: &Value, f: Option<&DataFile>) {
    let int = |k: &str| v.get(k).and_then(Value::as_i64).unwrap_or(0);
    let (w, h, fmt) = (int("m_Width"), int("m_Height"), int("m_TextureFormat"));
    let mut data = v
        .get("image data")
        .and_then(Value::as_bytes)
        .unwrap_or(&[])
        .to_vec();
    if data.is_empty() {
        let sd = |k: &str| v.path(&["m_StreamData", k]);
        let path = sd("path").and_then(Value::as_bytes).unwrap_or(b"");
        let off = sd("offset").and_then(Value::as_u64).unwrap_or(0);
        let size = sd("size").and_then(Value::as_u64).unwrap_or(0) as usize;
        if let Some(f) = f {
            data = f
                .read_node(&String::from_utf8_lossy(path), off, size)
                .unwrap_or_default();
        }
    }
    texture_input(s, w, h, fmt as i32, &data);
}

const TEX_FORMATS: [i32; 15] = [1, 3, 4, 5, 10, 11, 12, 14, 24, 25, 26, 27, 28, 29, 48];
const ENC_FORMATS: [i32; 8] = [1, 3, 4, 10, 12, 26, 27, 25];

fn texture_input(s: &mut Seeds, w: i64, h: i64, fmt: i32, data: &[u8]) {
    let data = &data[..data.len().min(64 << 10)];
    if let Some(i) = TEX_FORMATS.iter().position(|&f| f == fmt) {
        if (1..=64).contains(&w) && (1..=64).contains(&h) {
            s.put(
                "texture",
                &[&[i as u8, w as u8, h as u8, 2, 1][..], data].concat(),
            );
        }
        let mut b = vec![0x80 | i as u8];
        b.extend(w.to_le_bytes());
        b.extend(h.to_le_bytes());
        b.extend([2, 1]);
        b.extend(data);
        s.put("texture", &b);
    }
    if let Some(i) = ENC_FORMATS.iter().position(|&f| f == fmt) {
        if (1..=32).contains(&w) && (1..=32).contains(&h) {
            s.put(
                "texture_encode",
                &[&[i as u8, (w - 1) as u8, (h - 1) as u8, 0, 1, 3][..], data].concat(),
            );
        }
    }
}

fn unity(s: &mut Seeds, work: &Path) {
    let Some(db) = madeup::db() else {
        eprintln!(
            "seeds: PMU_TPK is not set: no Unity files (serialized, bundle, typetree, shaders)"
        );
        return;
    };
    let data = work.join("Data");
    madeup::game(&db, &data);
    let mut files = Vec::new();
    let mut stack = vec![data.clone()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).unwrap().flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else {
                files.push(p);
            }
        }
    }
    files.sort();
    let mut sfs: Vec<Vec<u8>> = Vec::new();
    for p in &files {
        let b = std::fs::read(p).unwrap();
        if b.starts_with(b"UnityFS\0") {
            s.put("bundle", &b);
            s.put("bundle_tools", &b);
            let opts = Options {
                db: Some(db.clone()),
                ..Default::default()
            };
            if let Ok(f) = DataFile::open(p, &opts) {
                contents(s, &f.sf, Some(&f));
            }
        } else if unityfs::serialized::looks_serialized(&b[..b.len().min(48)], b.len() as u64) {
            s.put("serialized", &b);
            let opts = Options {
                db: Some(db.clone()),
                ..Default::default()
            };
            if let Ok(f) = DataFile::open(p, &opts) {
                contents(s, &f.sf, Some(&f));
            }
            sfs.push(b);
        }
    }
    // the same serialized files in other bundle layouts
    for (k, sf) in sfs.iter().take(3).enumerate() {
        let nodes = [
            (format!("CAB-{k:032x}"), 4u32, sf.clone()),
            (format!("CAB-{k:032x}.resS"), 0u32, text(k as u32, 600)),
        ];
        let nodes: Vec<(&str, u32, Vec<u8>)> = nodes
            .iter()
            .map(|(n, f, d)| (n.as_str(), *f, d.clone()))
            .collect();
        for (df, bf, fmt) in [
            (194, 2, 7),
            (0x43, 3, 6),
            (0x40, 0, 6),
            (0x240 | 0x80 | 2, 2, 7),
        ] {
            let b = packed(&nodes, df, bf, fmt);
            s.put("bundle", &b);
            s.put("bundle_tools", &b);
        }
        let b = lzma_bundle(&nodes);
        s.put("bundle", &b);
        s.put("bundle_tools", &b);
        s.put("bundle", &madeup::bundle(&nodes, 1000));
    }
    // textures of every format the targets read
    for (i, &fmt) in TEX_FORMATS.iter().enumerate() {
        let (w, h) = (16 + i, 12 + i);
        let n = match fmt {
            1 => w * h,
            3 => w * h * 3,
            4 | 5 | 14 => w * h * 4,
            10 | 26 => w.div_ceil(4) * h.div_ceil(4) * 8,
            _ => w.div_ceil(4) * h.div_ceil(4) * 16,
        };
        let px = if fmt == 10 || fmt == 12 {
            madeup::dxt(i as u64, w, h, fmt == 12)
        } else {
            madeup::noise(i as u64, n)
        };
        texture_input(s, w as i64, h as i64, fmt, &px[..n.min(px.len())]);
    }
}

// ---- a managed assembly

/// A minimal .NET assembly (as androiddata's clrmeta tests make one): a PE with one section, the
/// CLI header, metadata with #~, #Strings and #Blob; a type with a P/Invoke method. Columns are
/// 2 bytes wide but the 4-byte flags and RVAs.
fn dll() -> Vec<u8> {
    let strings = b"\0<Module>\0Ns\0Glue\0DoIt\0do_it\0libfoo\0\0\0\0";
    let blob = [0u8, 5, 0x00, 2, 0x01, 0x0c, 0x08, 0];
    let w2 = |v: &mut Vec<u8>, xs: &[u16]| xs.iter().for_each(|x| v.extend(x.to_le_bytes()));
    let mut tb = vec![0u8; 24];
    let valid: u64 = 1 | 1 << 2 | 1 << 6 | 1 << 0x1A | 1 << 0x1C;
    tb[8..16].copy_from_slice(&valid.to_le_bytes());
    for n in [1u32, 2, 1, 1, 1] {
        tb.extend(n.to_le_bytes());
    }
    w2(&mut tb, &[0, 1, 0, 0, 0]); // Module: <Module>
    for (name, ns, extends) in [(1u16, 0u16, 0u16), (13, 10, 0)] {
        tb.extend(0u32.to_le_bytes()); // TypeDef: <Module>, Ns.Glue
        w2(&mut tb, &[name, ns, extends, 1, 1]);
    }
    tb.extend(0u32.to_le_bytes()); // MethodDef: DoIt, a P/Invoke
    w2(&mut tb, &[0, 0x2000, 18, 1, 1]);
    w2(&mut tb, &[29]); // ModuleRef: libfoo
    w2(&mut tb, &[0, 1 << 1 | 1, 23, 1]); // ImplMap: DoIt -> do_it in libfoo
    while tb.len() % 4 != 0 {
        tb.push(0);
    }
    let mut md = b"BSJB".to_vec();
    md.extend([1, 0, 1, 0, 0, 0, 0, 0, 4, 0, 0, 0]);
    md.extend(b"v4\0\0");
    md.extend([0, 0, 3, 0]);
    let hdr = md.len() + 12 + 20 + 16;
    let mut off = hdr;
    for (name, len) in [
        (&b"#~\0\0"[..], tb.len()),
        (b"#Strings\0\0\0\0", strings.len()),
        (b"#Blob\0\0\0", blob.len()),
    ] {
        md.extend((off as u32).to_le_bytes());
        md.extend((len as u32).to_le_bytes());
        md.extend(name);
        off += len;
    }
    md.extend(&tb);
    md.extend(strings);
    md.extend(blob);
    let mut body = vec![0u8; 0x48];
    body[8..12].copy_from_slice(&0x2048u32.to_le_bytes());
    body.extend(&md);
    let mut d = vec![0u8; 0x200];
    d[..2].copy_from_slice(b"MZ");
    d[0x3c..0x40].copy_from_slice(&0x80u32.to_le_bytes());
    let (pe, opt) = (0x80, 0x80 + 24);
    d[pe..pe + 4].copy_from_slice(b"PE\0\0");
    d[pe + 6..pe + 8].copy_from_slice(&1u16.to_le_bytes());
    d[pe + 20..pe + 22].copy_from_slice(&224u16.to_le_bytes());
    d[opt..opt + 2].copy_from_slice(&0x10bu16.to_le_bytes());
    let (dirs, sec) = (opt + 96, opt + 224);
    let size = body.len() as u32;
    for (at, v) in [(8, size), (12, 0x2000), (16, size), (20, 0x200)] {
        d[sec + at..sec + at + 4].copy_from_slice(&v.to_le_bytes());
    }
    d[dirs + 112..dirs + 116].copy_from_slice(&0x2000u32.to_le_bytes());
    d[dirs + 116..dirs + 120].copy_from_slice(&0x48u32.to_le_bytes());
    d.extend(body);
    d
}

// ---- text inputs

fn programs(s: &mut Seeds) {
    let vs = madeup::VS;
    for body in [
        "    SV_Target0 = texture(_MainTex, vec2(0.5));\n",
        "    gl_FragColor = vec4(1.0);\n",
        "    SV_Target0 = textureGather(_MainTex, vec2(0.5));\n",
        "    u = 0.0;\n    for(int i = 0 ; i<3 ; i++)\n    {\n        u += w[i] * texture(_MainTex, uv + o[i]).x;\n    }\n    SV_Target0 = vec4(u);\n",
    ] {
        s.put("glsl", format!("\x60{vs}{}", madeup::fs(body)).as_bytes());
    }
    s.put("glsl", b"\x70#ifdef VERTEX\n#version 410\n#extension GL_ARB_shading_language_420pack : require\nprecise vec4 p;\nlayout(location = 0) out vec2 vs_TEXCOORD0;\nvoid main()\n{\n    vs_TEXCOORD0 = vec2(0.0);\n}\n#endif\n");
    s.put("glsl", b"\x20#ifdef FRAGMENT\n#version 150\nvec4 ImmCB_0_0_0[3];\nuniform vec4 x;\nout vec4 o;\nvoid main()\n{\n    ImmCB_0_0_0[0] = vec4(1.0, 0.0, 0.0, 0.0);\n    ImmCB_0_0_0[1] = vec4(0.5, 0.0, 0.0, 0.0);\n    ImmCB_0_0_0[2] = vec4(0.25, 0.0, 0.0, 0.0);\n    o = ImmCB_0_0_0[int(x.x)]; /* a comment */ // and another\n}\n#endif\n");
}

fn journals(s: &mut Seeds) {
    for j in [
        &br#"{"version": 1, "done": {"shaders": ["sharedassets0.assets", "level0"], "textures": []}, "scaled": {"sharedassets0.assets": {"7": [256, 256, 96, 96]}}}"#[..],
        br#"{"version": 2, "done": {}, "scaled": {}, "audio": {"switch": ["a", "b"]}, "x": [1.5e3, -0.25, NaN, Infinity, -Infinity, true, false, null, "@u00e9@ud83d@ude00\n\t\"\\/"]}"#,
        br#"{"version": 1, "done": {"textures": ["a"]}, "pending": [["/nonexistent/a.tmp", "/nonexistent/a"]]}"#,
        br#"{"version": 1, "done": {"shaders": "abc"}}"#,
        br#"{"version": 1, "done": {"shaders": ["a"], "#,
        br#"[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]"#,
        b"",
    ] {
        // ('@' stands for a backslash: \u escapes, a surrogate pair among them)
        let j: Vec<u8> = j
            .iter()
            .map(|&c| if c == b'@' { b'\\' } else { c })
            .collect();
        s.put("journal", &j);
    }
}

fn command_lines(s: &mut Seeds) {
    for a in [
        &[
            "convert",
            "Data",
            "--config",
            "cuphead.json",
            "--steps",
            "shaders,textures",
            "--plugin",
            "animclips.py",
            "--set",
            "shaders.precision=highp",
            "--set=textures.scale=0.375",
            "--only",
            "level.*",
            "--any-unity-version",
        ][..],
        &["Data", "out", "--conf=c.json", "--", "-x"],
        &[
            "--src",
            "Data",
            "--pkg",
            "player",
            "--out",
            "android",
            "--name",
            "Game",
            "--package",
            "com.example.game",
            "--pack",
            "all",
            "--min-gles",
            "3.1",
            "--assembly-patch",
            "Assembly-CSharp.dll",
            "cmd",
            "--dllmap",
            "a.dll=b",
            "--version-code",
            "3",
            "--force",
        ],
        &[
            "data",
            "--out",
            "o",
            "--remove",
            "--version",
            "2021.3.5f1",
            "--no-f",
            "--json=j.json",
            "--apk",
            "a.apk",
            "--apk=b.apk",
            "--back",
            "il2cpp",
            "-h",
        ],
    ] {
        s.put("args", a.join("\0").as_bytes());
    }
}

fn main() {
    let root = PathBuf::from(
        std::env::args()
            .nth(1)
            .expect("usage: seeds <corpus folder>"),
    );
    let mut s = Seeds { root, n: 0 };
    zips(&mut s);
    for (stub, lz, bcj) in [(0, false, false), (512, true, false), (1024, true, true)] {
        s.put("nsis", &nsis(stub, lz, bcj));
    }
    s.put("nsis", &nsis_chunked());
    packages(&mut s);
    http(&mut s);
    programs(&mut s);
    s.put("clrmeta", &dll());
    journals(&mut s);
    command_lines(&mut s);
    let work = std::env::temp_dir().join(format!("pmu-fuzz-seeds-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&work);
    unity(&mut s, &work);
    let _ = std::fs::remove_dir_all(&work);
    println!("{} seeds in {}", s.n, s.root.display());
}
