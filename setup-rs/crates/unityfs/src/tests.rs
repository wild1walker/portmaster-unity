//! Unit tests on small files made up here (no Unity or game files): type trees, values, a
//! serialized file, bundles (written by this crate and laid out as Unity writes them), the
//! class database format, platform patching and LZMA blocks.
use crate::bin::Writer;
use crate::bundle::{self, Bundle, PackHeader, PackNode};
use crate::compress;
use crate::data::{DataFile, NodeData, Options};
use crate::serialized::SerializedFile;
use crate::source::{MemSource, SharedSource};
use crate::tpk::ClassDb;
use crate::typetree::{CommonStrings, Kind, Node, TypeTree, ALIGN};
use crate::value::{self, Value};
use crate::version::UnityVersion;
use std::path::PathBuf;
use std::sync::Arc;

fn node(level: i32, t: &str, n: &str, size: i32, meta: i32) -> Node {
    let mut x = Node::new(level, t, n, size, 1);
    x.type_flags = Some(0);
    x.meta_flag = Some(meta);
    x
}

/// A class with the shapes the Python's rules treat differently.
fn test_tree() -> TypeTree {
    let mut l = vec![
        node(0, "TestClass", "Base", -1, 0),
        node(1, "int", "m_A", 4, 0),
        node(1, "bool", "m_B", 1, ALIGN),
        node(1, "string", "m_Name", -1, 0x8000),
        node(2, "Array", "Array", -1, ALIGN),
        node(3, "int", "size", 4, 0),
        node(3, "char", "data", 1, 0),
        node(1, "vector", "m_F", -1, 0),
        node(2, "Array", "Array", -1, ALIGN),
        node(3, "int", "size", 4, 0),
        node(3, "float", "data", 4, 0),
        node(1, "vector", "m_Bytes", -1, 0),
        node(2, "Array", "Array", -1, ALIGN),
        node(3, "int", "size", 4, 0),
        node(3, "UInt8", "data", 1, 0),
        node(1, "map", "m_Map", -1, 0),
        node(2, "Array", "Array", -1, ALIGN),
        node(3, "int", "size", 4, 0),
        node(3, "pair", "data", -1, 0),
        node(4, "SInt16", "first", 2, 0),
        node(4, "string", "second", -1, 0),
        node(5, "Array", "Array", -1, ALIGN),
        node(6, "int", "size", 4, 0),
        node(6, "char", "data", 1, 0),
        node(1, "PPtr<GameObject>", "m_GameObject", 12, 0),
        node(2, "int", "m_FileID", 4, 0),
        node(2, "SInt64", "m_PathID", 8, 0),
        node(1, "UInt8", "m_Small", 1, 0),
        node(1, "TypelessData", "image data", -1, 0),
        node(1, "double", "m_D", 8, 0),
        node(1, "vector", "m_Names", -1, 0),
        node(2, "Array", "Array", -1, ALIGN),
        node(3, "int", "size", 4, 0),
        node(3, "string", "data", -1, 0),
        node(4, "Array", "Array", -1, ALIGN),
        node(5, "int", "size", 4, 0),
        node(5, "char", "data", 1, 0),
    ];
    for (i, n) in l.iter_mut().enumerate() {
        n.index = Some(i as i32);
    }
    TypeTree::from_list(l).unwrap()
}

fn test_value(name: &str, nan_bits: u32) -> Value {
    let s = |fields: Vec<(&str, Value)>| {
        Value::Struct(crate::value::Struct {
            fields: fields.into_iter().map(|(n, v)| (n.into(), v)).collect(),
        })
    };
    s(vec![
        ("m_A", Value::I32(-7)),
        ("m_B", Value::Bool(true)),
        ("m_Name", Value::str(name)),
        (
            "m_F",
            Value::Array(vec![Value::F32(1.5), Value::F32(f32::from_bits(nan_bits))]),
        ),
        ("m_Bytes", Value::Bytes(vec![1, 2, 3])),
        (
            "m_Map",
            Value::Array(vec![Value::Pair(Box::new((
                Value::I16(-2),
                Value::str("x"),
            )))]),
        ),
        (
            "m_GameObject",
            s(vec![
                ("m_FileID", Value::I32(0)),
                ("m_PathID", Value::I64(-5)),
            ]),
        ),
        ("m_Small", Value::U8(9)),
        ("image data", Value::Bytes(vec![0xAA; 5])),
        ("m_D", Value::F64(-0.0)),
        (
            "m_Names",
            Value::Array(vec![Value::str("ab"), Value::str("")]),
        ),
    ])
}

#[test]
fn value_layout_and_round_trip() {
    let t = test_tree();
    let v = test_value("abc", 0x7FA0_0001);
    let b = value::write(&t, &v, false, None).unwrap();
    // the layout, from the object's start
    let mut w = Vec::new();
    w.extend_from_slice(&(-7i32).to_le_bytes()); // 0 m_A
    w.extend_from_slice(&[1, 0, 0, 0]); // 4 m_B, aligned
    w.extend_from_slice(&3i32.to_le_bytes()); // 8 m_Name
    w.extend_from_slice(b"abc\0"); // a string aligns after its bytes
    w.extend_from_slice(&2i32.to_le_bytes()); // 16 m_F
    w.extend_from_slice(&1.5f32.to_le_bytes());
    w.extend_from_slice(&0x7FA0_0001u32.to_le_bytes()); // a signalling NaN's bits as they are
    w.extend_from_slice(&3i32.to_le_bytes()); // 28 m_Bytes
    w.extend_from_slice(&[1, 2, 3, 0]); // its Array node aligns
    w.extend_from_slice(&1i32.to_le_bytes()); // 36 m_Map
    w.extend_from_slice(&(-2i16).to_le_bytes()); // 40 first
    w.extend_from_slice(&1i32.to_le_bytes()); // 42 second: its length unaligned
    w.extend_from_slice(b"x\0"); // 46: aligned to 48
    w.extend_from_slice(&0i32.to_le_bytes()); // 48 m_GameObject
    w.extend_from_slice(&(-5i64).to_le_bytes());
    w.push(9); // 60 m_Small
    w.extend_from_slice(&5i32.to_le_bytes()); // 61 image data
    w.extend_from_slice(&[0xAA; 5]);
    w.extend_from_slice(&(-0.0f64).to_le_bytes()); // 70 m_D
    w.extend_from_slice(&2i32.to_le_bytes()); // 78 m_Names
    w.extend_from_slice(&2i32.to_le_bytes()); // 82
    w.extend_from_slice(b"ab"); // 86 -> 88
    w.extend_from_slice(&0i32.to_le_bytes()); // 88 an empty string
    assert_eq!(w.len(), 92);
    assert_eq!(b, w);
    let (back, n) = value::read(&t, &b, false, None).unwrap();
    assert_eq!(n, b.len());
    // (NaN != NaN: compare the bytes they write)
    assert_eq!(value::write(&t, &back, false, None).unwrap(), b);
    let f = back.path(&["m_F"]).unwrap().as_array().unwrap()[1].clone();
    assert!(matches!(f, Value::F32(x) if x.to_bits() == 0x7FA0_0001));
    assert_eq!(back.get("m_Name").unwrap().as_str(), Some("abc"));
    assert_eq!(
        back.get("m_Bytes").unwrap().as_bytes(),
        Some(&[1u8, 2, 3][..])
    );
    // big-endian too
    let be = value::write(&t, &v, true, None).unwrap();
    assert_eq!(be.len(), b.len());
    assert_eq!(&be[..4], &(-7i32).to_be_bytes());
    let (back, _) = value::read(&t, &be, true, None).unwrap();
    assert_eq!(value::write(&t, &back, true, None).unwrap(), be);
    // too few bytes: an error; extra bytes: ReadSize
    assert!(value::read_object(&t, &b[..b.len() - 1], false, None).is_err());
    let mut more = b.clone();
    more.extend_from_slice(&[0; 4]);
    match value::read_object(&t, &more, false, None) {
        Err(crate::Error::ReadSize { expected, read }) => {
            assert_eq!((expected, read), (more.len(), b.len()))
        }
        r => panic!("{r:?}"),
    }
}

#[test]
fn value_errors_and_conversions() {
    let t = test_tree();
    let mut v = test_value("n", 0);
    // a field missing: the Python's KeyError
    v.as_struct_mut()
        .unwrap()
        .fields
        .retain(|(n, _)| &**n != "m_D");
    assert!(value::write(&t, &v, false, None).is_err());
    // ints are written by the node's type, in range
    let mut v = test_value("n", 0);
    v.set("m_Small", Value::I64(200)).unwrap();
    assert!(value::write(&t, &v, false, None).is_ok());
    v.set("m_Small", Value::I64(300)).unwrap();
    assert!(value::write(&t, &v, false, None).is_err());
    // a negative array length is an error when read
    let mut b = vec![0u8; 12];
    b[8..12].copy_from_slice(&(-1i32).to_le_bytes());
    let mut l = vec![
        node(0, "C", "Base", -1, 0),
        node(1, "int", "a", 4, 0),
        node(1, "int", "b", 4, 0),
        node(1, "vector", "v", -1, 0),
        node(2, "Array", "Array", -1, 0),
        node(3, "int", "size", 4, 0),
        node(3, "int", "data", 4, 0),
    ];
    for (i, n) in l.iter_mut().enumerate() {
        n.index = Some(i as i32);
    }
    let t2 = TypeTree::from_list(l).unwrap();
    assert!(value::read(&t2, &b, false, None).is_err());
}

#[test]
fn aligned_number_arrays() {
    // a bool array whose element node is aligned: read aligns once after the elements, write
    // after each (UnityPyBoost and UnityPy's write_value)
    let mut l = vec![
        node(0, "C", "Base", -1, 0),
        node(1, "vector", "v", -1, 0),
        node(2, "Array", "Array", -1, 0),
        node(3, "int", "size", 4, 0),
        node(3, "bool", "data", 1, ALIGN),
    ];
    for (i, n) in l.iter_mut().enumerate() {
        n.index = Some(i as i32);
    }
    let t = TypeTree::from_list(l).unwrap();
    let b = [2, 0, 0, 0, 1, 0, 0, 0];
    let (v, n) = value::read(&t, &b, false, None).unwrap();
    assert_eq!(n, 8);
    let w = value::write(&t, &v, false, None).unwrap();
    assert_eq!(w, [2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]);
}

fn common() -> CommonStrings {
    let mut off = 0;
    let mut e = Vec::new();
    for s in [
        "AABB", "Array", "Base", "char", "data", "float", "int", "map", "pair", "size", "string",
        "vector", "first", "second", "bool",
    ] {
        e.push((off, Arc::from(s)));
        off += s.len() as u32 + 1;
    }
    CommonStrings::new(e)
}

#[test]
fn blob_round_trip() {
    let t = test_tree();
    let c = common();
    for version in [17u32, 19, 23] {
        let mut t = t.clone();
        if version >= 19 {
            for n in t.nodes.iter_mut() {
                n.ref_type_hash = Some(0x1122334455667788);
            }
        }
        let mut w = Writer::new(false);
        t.dump_blob(&mut w, version, &c).unwrap();
        let mut r = crate::bin::Reader::new(&w.buf, false);
        let back = TypeTree::parse_blob(&mut r, version, &c).unwrap();
        assert_eq!(r.pos, w.buf.len());
        assert_eq!(back.nodes.len(), t.nodes.len());
        for (a, b) in t.nodes.iter().zip(&back.nodes) {
            assert_eq!(
                (
                    a.level,
                    &a.type_name,
                    &a.name,
                    a.byte_size,
                    a.index,
                    a.meta_flag,
                    a.ref_type_hash,
                    &a.children
                ),
                (
                    b.level,
                    &b.type_name,
                    &b.name,
                    b.byte_size,
                    b.index,
                    b.meta_flag,
                    b.ref_type_hash,
                    &b.children
                )
            );
        }
        // the tree's own strings: only those not common ("AABB" at offset 0 never counts)
        let mut w2 = Writer::new(false);
        back.dump_blob(&mut w2, version, &c).unwrap();
        assert_eq!(w2.buf, w.buf);
    }
    // a string buffer of its own: TestClass, m_A, ... first use order
    let mut w = Writer::new(false);
    t.dump_blob(&mut w, 17, &c).unwrap();
    let n = t.nodes.len();
    let strs = &w.buf[8 + 24 * n..];
    assert!(strs.starts_with(b"TestClass\0m_A\0m_B\0m_Name\0"));
    // node 1 (int m_A): its type a common string, its name the buffer's second
    assert_eq!(
        &w.buf[8 + 24 + 4..8 + 24 + 8],
        &(0x8000_0000u32 | 32).to_le_bytes()
    );
    assert_eq!(&w.buf[8 + 24 + 8..8 + 24 + 12], &10u32.to_le_bytes());
}

#[test]
fn from_list_levels() {
    // a level jump and a second root: the jump is a child, the second root is dropped
    let l = vec![
        node(0, "A", "Base", 0, 0),
        node(1, "int", "x", 4, 0),
        node(3, "int", "y", 4, 0),
        node(1, "int", "z", 4, 0),
        node(0, "B", "Base", 0, 0),
    ];
    let t = TypeTree::from_list(l).unwrap();
    assert_eq!(t.nodes.len(), 4);
    assert_eq!(t.nodes[0].children, vec![1, 3]);
    assert_eq!(t.nodes[1].children, vec![2]);
    assert_eq!(t.preorder().collect::<Vec<_>>(), vec![0, 1, 2, 3]);
    assert_eq!(Kind::of("PPtr<Texture2D>"), Kind::PPtr);
}

/// A format 17 serialized file built as Unity lays it out (objects in another order, 8-aligned,
/// with a gap), with the test tree as its one type.
fn serialized_bytes(names: &[(i64, &str)]) -> Vec<u8> {
    let t = test_tree();
    let c = CommonStrings::default();
    let mut data = Vec::new();
    let mut table = Vec::new();
    for (k, (pid, name)) in names.iter().enumerate() {
        let b = value::write(&t, &test_value(name, k as u32), false, None).unwrap();
        while data.len() % 16 != 0 {
            data.push(0);
        }
        table.push((*pid, data.len() as u32, b.len() as u32));
        data.extend_from_slice(&b);
    }
    let mut m = Writer::new(false);
    m.cstr(b"2017.4.9f1");
    m.i32(2);
    m.u8(1);
    m.i32(1);
    m.i32(1); // class id
    m.u8(0);
    m.i16(-1);
    m.bytes(&[0x11; 16]);
    t.dump_blob(&mut m, 17, &c).unwrap();
    m.i32(table.len() as i32);
    for (pid, off, size) in &table {
        // (aligned in the file: the metadata starts at 20)
        while (m.len() + 20) % 4 != 0 {
            m.u8(0);
        }
        m.i64(*pid);
        m.u32(*off);
        m.u32(*size);
        m.i32(0);
    }
    m.i32(0); // script types
    m.i32(1);
    m.cstr(b"");
    m.bytes(&[0x22; 16]);
    m.i32(0);
    m.cstr(b"library/unity default resources");
    m.cstr(b"");
    let data_offset = (20 + m.len()).div_ceil(16) * 16 + 16; // (Unity pads more than UnityPy)
    let mut f = Writer::new(true);
    f.u32(m.len() as u32);
    f.u32((data_offset + data.len()) as u32);
    f.u32(17);
    f.u32(data_offset as u32);
    f.bytes(&[0, 0, 0, 0]);
    f.bytes(&m.buf);
    f.buf.resize(data_offset, 0);
    f.bytes(&data);
    f.buf
}

fn mem(b: Vec<u8>) -> SharedSource {
    Arc::new(MemSource(b))
}

#[test]
fn serialized_file_save() {
    let raw = serialized_bytes(&[(9, "nine"), (-3, "minus three"), (4, "four")]);
    let sf = SerializedFile::open("test", mem(raw.clone()), None, None).unwrap();
    assert_eq!(sf.header.version, 17);
    assert_eq!(sf.version, UnityVersion::parse("2017.4.9f1").unwrap());
    assert_eq!(
        sf.objects.iter().map(|o| o.path_id).collect::<Vec<_>>(),
        vec![9, -3, 4]
    );
    assert_eq!(sf.externals[0].name(), "unity default resources");
    for o in &sf.objects {
        let v = sf.read_typetree(o.path_id).unwrap();
        assert_eq!(
            sf.write_value(&sf.typetree(o).unwrap(), &v).unwrap(),
            sf.original_bytes(o.path_id).unwrap()
        );
    }
    assert_eq!(
        sf.read_typetree(-3)
            .unwrap()
            .get("m_Name")
            .unwrap()
            .as_str(),
        Some("minus three")
    );
    let out = sf.save().unwrap();
    // UnityPy's layout: objects by path id, each 8-aligned, data 16-aligned after the metadata
    let back = SerializedFile::open("test", mem(out.clone()), None, None).unwrap();
    let order: Vec<i64> = back.objects.iter().map(|o| o.path_id).collect();
    assert_eq!(order, vec![-3, 4, 9]);
    assert_eq!(back.header.data_offset % 16, 0);
    let mut expect_start = back.header.data_offset;
    for o in &back.objects {
        assert_eq!(o.byte_start, expect_start);
        expect_start += (o.byte_size as u64).div_ceil(8) * 8;
        assert_eq!(
            back.original_bytes(o.path_id).unwrap(),
            sf.original_bytes(o.path_id).unwrap()
        );
    }
    assert_eq!(back.header.file_size as usize, out.len());
    assert_eq!(
        back.save().unwrap(),
        out,
        "saving a saved file changes nothing"
    );
    // an edit, saved in memory and streamed: the same bytes
    let mut sf = sf;
    let mut v = sf.read_typetree(4).unwrap();
    v.set("m_Name", Value::str("a longer name than before"))
        .unwrap();
    sf.save_typetree(4, &v).unwrap();
    assert_eq!(
        sf.current_typetree(4)
            .unwrap()
            .get("m_Name")
            .unwrap()
            .as_str(),
        Some("a longer name than before")
    );
    assert_eq!(
        sf.read_typetree(4).unwrap().get("m_Name").unwrap().as_str(),
        Some("four"),
        "read_typetree reads the file's bytes"
    );
    let mem_out = sf.save().unwrap();
    let mut cur = std::io::Cursor::new(Vec::new());
    sf.save_to(&mut cur, |_, _| Ok(None)).unwrap();
    assert_eq!(cur.into_inner(), mem_out);
    // a prepare hook's bytes for one write only
    let mut cur = std::io::Cursor::new(Vec::new());
    let other = sf.bytes(9).unwrap();
    sf.save_to(&mut cur, |_, o| {
        Ok(if o.path_id == 4 {
            Some(other.clone())
        } else {
            None
        })
    })
    .unwrap();
    let b2 = SerializedFile::open("t", mem(cur.into_inner()), None, None).unwrap();
    assert_eq!(b2.original_bytes(4).unwrap(), other);
    assert_eq!(
        sf.bytes(4).unwrap(),
        sf.write_value(&sf.typetree_of(4).unwrap(), &v).unwrap()
    );
}

fn temp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("unityfs-test-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// A bundle laid out as Unity 2017.4 writes them: format 6, blocks info LZ4 HC right after the
/// header (flags 0x43), LZ4 HC blocks of `block` bytes.
fn unity_bundle(nodes: &[(&str, u32, Vec<u8>)], block: usize) -> Vec<u8> {
    let mut data = Vec::new();
    for n in nodes {
        data.extend_from_slice(&n.2);
    }
    let mut blocks = Vec::new();
    let mut packed = Vec::new();
    for c in data.chunks(block) {
        let z = compress::lz4hc(c, 9).unwrap();
        blocks.push((c.len() as u32, z.len() as u32, 3u16));
        packed.extend_from_slice(&z);
    }
    let mut info = Writer::new(true);
    info.bytes(&[7; 16]);
    info.i32(blocks.len() as i32);
    for b in &blocks {
        info.u32(b.0);
        info.u32(b.1);
        info.u16(b.2);
    }
    info.i32(nodes.len() as i32);
    let mut off = 0i64;
    for n in nodes {
        info.i64(off);
        info.i64(n.2.len() as i64);
        info.u32(n.1);
        info.cstr(n.0.as_bytes());
        off += n.2.len() as i64;
    }
    let ci = compress::lz4hc(&info.buf, 9).unwrap();
    let mut h = Writer::new(true);
    h.cstr(b"UnityFS");
    h.u32(6);
    h.cstr(b"5.x.x");
    h.cstr(b"2017.4.9f1");
    let total = h.len() + 20 + ci.len() + packed.len();
    h.i64(total as i64);
    h.u32(ci.len() as u32);
    h.u32(info.len() as u32);
    h.u32(0x43);
    h.bytes(&ci);
    h.bytes(&packed);
    h.buf
}

#[test]
fn bundles() {
    let sf = serialized_bytes(&[(1, "one"), (2, "two")]);
    let mut x = 0x2545_F491_4F6C_DD1Du64;
    let res: Vec<u8> = (0..300_000)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            (x >> 32) as u8
        })
        .collect();
    let nodes = vec![
        ("CAB-test", 4u32, sf.clone()),
        ("CAB-test.resS", 0u32, res.clone()),
    ];
    let ub = unity_bundle(&nodes, 0x20000);
    let b = Bundle::from_source(mem(ub.clone())).unwrap();
    assert_eq!(b.header.flags, 0x43);
    assert!(!b.header.aligned);
    assert_eq!(b.header.nodes.len(), 2);
    assert_eq!(b.node(0).read_vec(0, sf.len()).unwrap(), sf);
    assert_eq!(b.node(1).read_vec(0, res.len()).unwrap(), res);
    assert_eq!(
        b.node(1).read_vec(140_000, 10).unwrap(),
        res[140_000..140_010]
    );
    assert_eq!(b.header.serialized_nodes(), vec![0]);

    // UnityPy's packer='lz4': blocks info at the end, LZ4 HC chunks, a chunk LZ4 cannot shrink raw
    let pn: Vec<PackNode> = nodes
        .iter()
        .map(|n| PackNode {
            name: n.0.into(),
            flags: n.1,
            src: mem(n.2.clone()),
        })
        .collect();
    let ph = PackHeader::of(&b.header);
    let mut one = std::io::Cursor::new(Vec::new());
    bundle::pack(
        &mut one,
        &ph,
        &pn,
        bundle::LZ4_DATA_FLAG,
        bundle::LZ4_BLOCK_FLAG,
        1,
    )
    .unwrap();
    let mut many = std::io::Cursor::new(Vec::new());
    bundle::pack(
        &mut many,
        &ph,
        &pn,
        bundle::LZ4_DATA_FLAG,
        bundle::LZ4_BLOCK_FLAG,
        4,
    )
    .unwrap();
    let (one, many) = (one.into_inner(), many.into_inner());
    assert_eq!(one, many, "threads give the same bytes");
    let p = Bundle::from_source(mem(one.clone())).unwrap();
    assert_eq!(p.header.flags, 194);
    assert_eq!(p.header.size as usize, one.len());
    assert!(p
        .header
        .blocks
        .iter()
        .all(|x| x.usize as usize == bundle::CHUNK
            || x.usize as usize == (sf.len() + res.len()) % bundle::CHUNK));
    assert!(
        p.header
            .blocks
            .iter()
            .any(|x| x.flags == 0 && x.csize == x.usize),
        "the noise is kept raw"
    );
    assert!(p.header.blocks.iter().any(|x| x.flags == 2));
    assert_eq!(p.node(1).read_vec(0, res.len()).unwrap(), res);
    // uncompressed (packer='none'), and blocks info before the data
    for (df, bf) in [(64u32, 64u32), (0x43, 3)] {
        let mut c = std::io::Cursor::new(Vec::new());
        bundle::pack(&mut c, &ph, &pn, df, bf, 2).unwrap();
        let q = Bundle::from_source(mem(c.into_inner())).unwrap();
        assert_eq!(q.node(0).read_vec(0, sf.len()).unwrap(), sf);
        assert_eq!(q.node(1).read_vec(0, res.len()).unwrap(), res);
    }

    // a data file of it: written back as the Python writes it
    let dir = temp("bundles");
    let path = dir.join("b");
    std::fs::write(&path, &ub).unwrap();
    for spill in [false, true] {
        let opts = Options {
            spill_dir: if spill { Some(dir.join("spill")) } else { None },
            spill_mb: if spill { 0 } else { 64 },
            ..Default::default()
        };
        let mut f = DataFile::open(&path, &opts).unwrap();
        assert!(f.is_bundle);
        assert_eq!(f.cab_name.as_deref(), Some("CAB-test"));
        assert_eq!(
            f.read_node("archive:/CAB-test/CAB-test.resS", 10, 5)
                .unwrap(),
            res[10..15]
        );
        let saved = f.save().unwrap();
        let out = dir.join("out");
        f.write_to(&out, false).unwrap();
        assert_eq!(std::fs::read(&out).unwrap(), saved);
        // the same as packing the re-saved serialized file with the stream
        let sf2 = SerializedFile::open("x", mem(sf.clone()), None, None)
            .unwrap()
            .save()
            .unwrap();
        let pn2 = vec![
            PackNode {
                name: "CAB-test".into(),
                flags: 4,
                src: mem(sf2),
            },
            PackNode {
                name: "CAB-test.resS".into(),
                flags: 0,
                src: mem(res.clone()),
            },
        ];
        let mut c = std::io::Cursor::new(Vec::new());
        bundle::pack(
            &mut c,
            &ph,
            &pn2,
            bundle::LZ4_DATA_FLAG,
            bundle::LZ4_BLOCK_FLAG,
            2,
        )
        .unwrap();
        assert_eq!(c.into_inner(), saved);
        // a changed stream and a dropped one
        f.set_bundle_res("CAB-test.resS", NodeData::Bytes(vec![5; 10]))
            .unwrap();
        assert_eq!(f.read_node("CAB-test.resS", 0, 100).unwrap(), vec![5; 10]);
        f.remove_node("CAB-test.resS").unwrap();
        f.write_to(&out, false).unwrap();
        let q = Bundle::open(&out).unwrap();
        assert_eq!(q.header.nodes.len(), 1);
    }
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn platform_patch_in_place() {
    let sf = serialized_bytes(&[(1, "one")]);
    let res = vec![3u8; 1000];
    let dir = temp("platform");
    let path = dir.join("b");
    std::fs::write(
        &path,
        unity_bundle(&[("CAB-p", 4, sf.clone()), ("CAB-p.resS", 0, res)], 0x20000),
    )
    .unwrap();
    assert_eq!(
        crate::platform::bundle_platforms(&path).unwrap(),
        vec![("CAB-p".to_string(), 2)]
    );
    let before = std::fs::metadata(&path).unwrap().len();
    let r = crate::platform::patch_bundle_platform(&path, 13).unwrap();
    assert_eq!(r.len(), 1);
    assert_eq!(r[0].1, 2);
    assert_eq!(
        crate::platform::bundle_platforms(&path).unwrap(),
        vec![("CAB-p".to_string(), 13)]
    );
    if r[0].2 != "rewrite" {
        assert_eq!(
            std::fs::metadata(&path).unwrap().len(),
            before,
            "nothing moves"
        );
    }
    let b = Bundle::open(&path).unwrap();
    let got = b.node(0).read_vec(0, sf.len()).unwrap();
    let (off, ..) = crate::serialized::platform_offset(&sf).unwrap();
    assert_eq!(got[..off], sf[..off]);
    assert_eq!(got[off..off + 4], 13u32.to_le_bytes());
    assert_eq!(got[off + 4..], sf[off + 4..]);
    assert_eq!(
        crate::platform::patch_bundle_platform(&path, 13).unwrap()[0].2,
        "already"
    );
    // a plain file
    let p2 = dir.join("sf");
    std::fs::write(&p2, &sf).unwrap();
    assert_eq!(crate::platform::patch_sf_platform(&p2, 13).unwrap(), 2);
    assert_eq!(
        crate::serialized::platform_offset(&std::fs::read(&p2).unwrap())
            .unwrap()
            .2,
        13
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn lzma_block() {
    // made with Python's lzma (FORMAT_RAW, LZMA1, lc 3 lp 0 pb 2, 64 KiB dictionary)
    let hex = "5d00000100002a9b8927786125f45d0f6931417683b8209575112b8f492c7b2b145252fbe3b70bd66aac0d8a171c172c5b2df28787c89e4802ca60d9f387266a80bed94d58592737ed48f953deb7cc14e4b0b821af9750752ced6b67643fdc0747cbb77d606b32868edc1c6dba5c026c87c42411be84c4bb27df8111e17f6be26dd5fefe1654aeb1176fc41c8b9fc5a065c90af0e9490e748d9cfe804d00d9bd9f7438a6e20cfecfb5c904d379a60892db721880a0f1f9e45c3bea1f30685031337c7f221a79fc6efb729616c913e0530e18ef46540484ae32714b5692aa2c58c69efc349e7d4cdc8d21403627aa08587231a12d4fa29afb4d5ca28f9aea47a1ff891ef898083014eda1fffe358000";
    let z: Vec<u8> = (0..hex.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
        .collect();
    let mut want = b"UnityFS LZMA test block ".repeat(20);
    want.extend(0..=255u8);
    assert_eq!(compress::lzma_decompress(&z, want.len()).unwrap(), want);
    assert_eq!(compress::decompress(&z, want.len(), 1).unwrap(), want);
    assert!(compress::lzma_decompress(&z[..100], want.len()).is_err());
}

#[test]
fn class_database() {
    // a TPK (version 2, uncompressed): class 7 from 5.0.0 (tree: Thing { int a; string s }),
    // a second tree from 2017.1, gone in 2020.1
    let strings = [
        "Thing", "int", "a", "string", "s", "Base", "Array", "char", "data", "size", "float", "f",
    ];
    let si = |s: &str| strings.iter().position(|x| *x == s).unwrap() as u16;
    let mut b = Writer::new(false);
    b.i64(0);
    b.i32(0);
    b.i32(1);
    b.i32(7);
    b.i32(3);
    let v = |s: &str| UnityVersion::parse(s).unwrap().0;
    for (ver, root) in [
        (v("5.0.0f1"), Some(0u16)),
        (v("2017.1.0f1"), Some(6)),
        (v("2020.1.0f1"), None),
    ] {
        b.u64(ver);
        match root {
            Some(r) => {
                b.u8(1);
                b.u16(si("Thing"));
                b.u16(0);
                b.u8(128);
                b.u16(r);
            }
            None => b.u8(0),
        }
    }
    // common strings: one version
    b.i32(1);
    b.u64(v("5.0.0f1"));
    b.i32(2);
    for (o, s) in [(0u16, "AABB"), (5, "Array")] {
        b.u16(o);
        b.u16(if s == "AABB" { si("Base") } else { si(s) });
    }
    // nodes: 0 Thing{1 a, 2 s{3 Array{4 size, 5 data}}}, 6 Thing{1 a, 7 f}
    let nodes: [(&str, &str, i32, u32, &[u16]); 8] = [
        ("Thing", "Base", -1, 0, &[1, 2]),
        ("int", "a", 4, 0, &[]),
        ("string", "s", -1, 0x8000, &[3]),
        ("Array", "Array", -1, 0x4000, &[4, 5]),
        ("int", "size", 4, 0, &[]),
        ("char", "data", 1, 0, &[]),
        ("Thing", "Base", -1, 0, &[1, 7]),
        ("float", "f", 4, 0, &[]),
    ];
    b.i32(nodes.len() as i32);
    for (t, n, size, meta, sub) in nodes {
        b.u16(si(t));
        b.u16(si(n));
        b.i32(size);
        b.i16(1);
        b.u8(0);
        b.u32(meta);
        b.u16(sub.len() as u16);
        for &x in sub {
            b.u16(x);
        }
    }
    b.i32(strings.len() as i32);
    for s in strings {
        b.u8(s.len() as u8);
        b.bytes(s.as_bytes());
    }
    let mut f = Writer::new(false);
    f.u32(0x2A4B_5054);
    f.u8(2);
    f.u8(0);
    f.u8(0);
    f.u8(0);
    f.u32(0);
    f.u32(b.len() as u32);
    f.u32(b.len() as u32);
    f.bytes(&b.buf);
    let db = ClassDb::parse(&f.buf).unwrap();
    let t = db
        .typetree(7, UnityVersion::parse("2017.4.9f1").unwrap())
        .unwrap();
    assert_eq!(
        t.nodes
            .iter()
            .map(|n| (&*n.type_name, &*n.name))
            .collect::<Vec<_>>(),
        vec![("Thing", "Base"), ("int", "a"), ("float", "f")]
    );
    assert_eq!(
        t.nodes.iter().map(|n| n.index).collect::<Vec<_>>(),
        vec![Some(0), Some(1), Some(2)]
    );
    assert!(t.nodes.iter().all(|n| n.type_flags.is_none()));
    let t = db
        .typetree(7, UnityVersion::parse("5.6.0f3").unwrap())
        .unwrap();
    assert_eq!(t.nodes.len(), 6);
    assert!(t.nodes[3].aligned());
    assert!(db
        .typetree(7, UnityVersion::parse("4.7.0f1").unwrap())
        .is_err());
    assert!(db
        .typetree(7, UnityVersion::parse("2021.3.0f1").unwrap())
        .is_err());
    assert_eq!(
        db.class_name(7, UnityVersion::parse("2017.4.9f1").unwrap()),
        Some("Thing")
    );
    assert_eq!(db.common_strings().get(5), Some("Array"));
    assert_eq!(db.common_strings().offset_of("Base"), Some(0));
    // an object of a file without type trees is read with it
    let mut tf = Writer::new(false);
    tf.i32(42);
    tf.u32(1.25f32.to_bits());
    let (val, _) = value::read(
        &db.typetree(7, UnityVersion::parse("2017.4.9f1").unwrap())
            .unwrap(),
        &tf.buf,
        false,
        None,
    )
    .unwrap();
    assert_eq!(val.get("a"), Some(&Value::I32(42)));
}

#[test]
fn class_names() {
    assert_eq!(crate::classes::class_name(28), "Texture2D");
    assert_eq!(crate::classes::class_name(1001), "PrefabInstance");
    assert_eq!(crate::classes::class_name(52), "Unknown (52)");
    assert_eq!(crate::classes::class_id("Shader"), Some(48));
}

/// A Texture2D-like class (class id 28): name, stream data, inline pixels.
fn texture_tree() -> TypeTree {
    let mut l = vec![
        node(0, "Texture2D", "Base", -1, 0),
        node(1, "string", "m_Name", -1, 0x8000),
        node(2, "Array", "Array", -1, ALIGN),
        node(3, "int", "size", 4, 0),
        node(3, "char", "data", 1, 0),
        node(1, "TypelessData", "image data", -1, 0),
        node(2, "int", "size", 4, 0),
        node(2, "UInt8", "data", 1, 0),
        node(1, "StreamingInfo", "m_StreamData", -1, 0),
        node(2, "unsigned int", "offset", 4, 0),
        node(2, "unsigned int", "size", 4, 0),
        node(2, "string", "path", -1, 0),
        node(3, "Array", "Array", -1, ALIGN),
        node(4, "int", "size", 4, 0),
        node(4, "char", "data", 1, 0),
    ];
    for (i, n) in l.iter_mut().enumerate() {
        n.index = Some(i as i32);
    }
    TypeTree::from_list(l).unwrap()
}

fn texture_file(textures: &[(i64, &str, u32, u32, &str)]) -> Vec<u8> {
    let t = texture_tree();
    let mut data = Vec::new();
    let mut table = Vec::new();
    for (pid, name, off, size, path) in textures {
        let s = |f: Vec<(&str, Value)>| {
            Value::Struct(crate::value::Struct {
                fields: f.into_iter().map(|(n, v)| (n.into(), v)).collect(),
            })
        };
        let v = s(vec![
            ("m_Name", Value::str(name)),
            ("image data", Value::Bytes(Vec::new())),
            (
                "m_StreamData",
                s(vec![
                    ("offset", Value::U32(*off)),
                    ("size", Value::U32(*size)),
                    ("path", Value::str(path)),
                ]),
            ),
        ]);
        let b = value::write(&t, &v, false, None).unwrap();
        while data.len() % 8 != 0 {
            data.push(0);
        }
        table.push((*pid, data.len() as u32, b.len() as u32));
        data.extend_from_slice(&b);
    }
    let mut m = Writer::new(false);
    m.cstr(b"2017.4.9f1");
    m.i32(2);
    m.u8(1);
    m.i32(1);
    m.i32(28);
    m.u8(0);
    m.i16(-1);
    m.bytes(&[0x33; 16]);
    t.dump_blob(&mut m, 17, &CommonStrings::default()).unwrap();
    m.i32(table.len() as i32);
    for (pid, off, size) in &table {
        while (m.len() + 20) % 4 != 0 {
            m.u8(0);
        }
        m.i64(*pid);
        m.u32(*off);
        m.u32(*size);
        m.i32(0);
    }
    m.i32(0);
    m.i32(0);
    m.cstr(b"");
    let data_offset = (20 + m.len()).div_ceil(16) * 16;
    let mut f = Writer::new(true);
    f.u32(m.len() as u32);
    f.u32((data_offset + data.len()) as u32);
    f.u32(17);
    f.u32(data_offset as u32);
    f.bytes(&[0, 0, 0, 0]);
    f.bytes(&m.buf);
    f.buf.resize(data_offset, 0);
    f.bytes(&data);
    f.buf
}

#[test]
fn inline_streams() {
    let res: Vec<u8> = (0..5000u32).map(|i| (i % 253) as u8).collect();
    let sf = texture_file(&[
        (3, "a", 100, 50, "archive:/CAB-t/CAB-t.resS"),
        (1, "b", 0, 0, ""),
        (2, "c", 1000, 4000, "archive:/CAB-t/CAB-t.resS"),
    ]);
    let dir = temp("inline");
    let path = dir.join("tex");
    std::fs::write(
        &path,
        unity_bundle(&[("CAB-t", 4, sf), ("CAB-t.resS", 0, res.clone())], 0x8000),
    )
    .unwrap();
    let mut f = DataFile::open(
        &path,
        &Options {
            spill_dir: Some(dir.join("spill")),
            ..Default::default()
        },
    )
    .unwrap();
    assert_eq!(f.path_ids("Texture2D"), vec![3, 1, 2]);
    let (plan, still) = crate::inline::plan(&f.sf).unwrap();
    assert_eq!(plan.len(), 2);
    assert!(still.is_empty());
    let out = dir.join("out");
    f.write_to(&out, true).unwrap();
    let g = DataFile::open(&out, &Options::default()).unwrap();
    assert_eq!(
        g.node_names(),
        vec!["CAB-t".to_string()],
        "the stream nothing reads is dropped"
    );
    let a = g.sf.read_typetree(3).unwrap();
    assert_eq!(
        a.get("image data").unwrap().as_bytes().unwrap(),
        &res[100..150]
    );
    assert_eq!(a.path(&["m_StreamData", "size"]), Some(&Value::U32(0)));
    assert_eq!(
        a.path(&["m_StreamData", "path"]).unwrap().as_str(),
        Some("")
    );
    assert_eq!(
        g.sf.read_typetree(2)
            .unwrap()
            .get("image data")
            .unwrap()
            .as_bytes()
            .unwrap(),
        &res[1000..5000]
    );
    // the same bytes as inlining in memory and saving
    let mut h = DataFile::open(&path, &Options::default()).unwrap();
    let rd = |p: &str, o: u64, n: usize| f.read_node(p, o, n);
    let mut sf2 = SerializedFile::open(
        "CAB-t",
        h.bundle_res("CAB-t").unwrap(),
        None,
        Some("2017.4.9f1"),
    )
    .unwrap();
    assert_eq!(
        crate::inline::inline_texture_streams(&mut sf2, rd)
            .unwrap()
            .0,
        2
    );
    h.sf = sf2;
    h.remove_node("CAB-t.resS").unwrap();
    assert_eq!(h.save().unwrap(), std::fs::read(&out).unwrap());
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn bundle_files() {
    let a = serialized_bytes(&[(1, "one")]);
    let b = serialized_bytes(&[(1, "uno"), (2, "dos")]);
    let dir = temp("multi");
    let path = dir.join("scene");
    std::fs::write(
        &path,
        unity_bundle(
            &[
                ("BuildPlayer-a", 4, a),
                ("BuildPlayer-a.sharedAssets", 4, b),
                ("x.resS", 0, vec![1; 10]),
            ],
            0x20000,
        ),
    )
    .unwrap();
    assert!(
        DataFile::open(&path, &Options::default()).is_err(),
        "two serialized files: not a Loaded"
    );
    let mut bf = crate::data::BundleFiles::open(&path, None).unwrap();
    assert_eq!(bf.files.len(), 2);
    for (_, sf) in bf.files.iter_mut() {
        sf.target_platform = 13;
    }
    let out = dir.join("out");
    bf.write_to(&out, None).unwrap();
    let out2 = dir.join("out2");
    bf.write_to(&out2, Some(&dir.join("work"))).unwrap();
    assert_eq!(std::fs::read(&out).unwrap(), std::fs::read(&out2).unwrap());
    let back = crate::data::BundleFiles::open(&out, None).unwrap();
    assert!(back.files.iter().all(|(_, sf)| sf.target_platform == 13));
    assert_eq!(
        back.files[1]
            .1
            .read_typetree(2)
            .unwrap()
            .get("m_Name")
            .unwrap()
            .as_str(),
        Some("dos")
    );
    let _ = std::fs::remove_dir_all(&dir);
}
