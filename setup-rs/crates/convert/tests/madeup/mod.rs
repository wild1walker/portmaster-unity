//! Made-up Unity data for the convert tests (no Unity or game files): serialized files of Unity
//! 2017.4 written from the class database's type trees (with the trees embedded, or without as
//! a player build has them), bundles laid out as Unity writes them, and a whole data folder with
//! what Cuphead's config converts: player settings, audio clips and scenes' preload lists,
//! animation clips, shaders (translatable, untranslatable, already GLES, without OpenGL Core) in
//! files and bundles. The class database is UnityPy's lzma.tpk ($PMU_TPK).
#![allow(dead_code)]
use shaderx::blob::{self, Entry, PlatformBlob, ShaderBlobs, Slot};
use std::collections::BTreeMap;
use std::path::Path;
use std::sync::Arc;
use unityfs::bin::Writer;
use unityfs::typetree::{CommonStrings, Kind};
use unityfs::{ClassDb, Struct, TypeTree, UnityVersion, Value};

pub const VERSION: &str = "2017.4.9f1";

/// The class database, or None (tests that need it are skipped) without $PMU_TPK.
pub fn db() -> Option<Arc<ClassDb>> {
    std::env::var_os("PMU_TPK")?;
    Some(Arc::new(ClassDb::default_db().expect("PMU_TPK")))
}

/// A class's type tree for Unity 2017.4.
pub fn tree(db: &ClassDb, class_id: i32) -> Arc<TypeTree> {
    db.typetree(class_id, UnityVersion::parse(VERSION).unwrap())
        .unwrap()
}

/// The default value of a node: zeros, empty strings and arrays.
pub fn default_of(t: &TypeTree, i: usize) -> Value {
    let n = &t.nodes[i];
    match n.kind {
        Kind::Bool => Value::Bool(false),
        Kind::I8 => Value::I8(0),
        Kind::U8 => Value::U8(0),
        Kind::I16 => Value::I16(0),
        Kind::U16 => Value::U16(0),
        Kind::I32 => Value::I32(0),
        Kind::U32 => Value::U32(0),
        Kind::I64 => Value::I64(0),
        Kind::U64 => Value::U64(0),
        Kind::F32 => Value::F32(0.0),
        Kind::F64 => Value::F64(0.0),
        Kind::Str => Value::Str(Vec::new()),
        Kind::Bytes => Value::Bytes(Vec::new()),
        Kind::Pair => Value::Pair(Box::new((
            default_of(t, n.children[0]),
            default_of(t, n.children[1]),
        ))),
        _ if n
            .children
            .first()
            .is_some_and(|&c| &*t.nodes[c].type_name == "Array") =>
        {
            Value::Array(Vec::new())
        }
        _ => Value::Struct(Struct {
            fields: n
                .children
                .iter()
                .map(|&c| (t.nodes[c].name.clone(), default_of(t, c)))
                .collect(),
        }),
    }
}

/// The node of a field path below the root ("m_Axes" -> the vector; elements: `element`).
fn node_of(t: &TypeTree, path: &[&str]) -> usize {
    let mut i = 0;
    for p in path {
        i = *t.nodes[i]
            .children
            .iter()
            .find(|&&c| &*t.nodes[c].name == *p)
            .unwrap_or_else(|| panic!("no field {p} in {}", t.class_name()));
    }
    i
}

/// The default element of an array field (path to the vector).
pub fn element(t: &TypeTree, path: &[&str]) -> Value {
    let v = node_of(t, path);
    let arr = t.nodes[v].children[0];
    default_of(t, t.nodes[arr].children[1])
}

pub fn object(db: &ClassDb, class_id: i32) -> Value {
    default_of(&tree(db, class_id), 0)
}

pub fn set(v: &mut Value, path: &[&str], x: Value) {
    let (last, head) = path.split_last().unwrap();
    v.path_mut(head).unwrap().set(last, x).unwrap();
}

pub fn pptr(file: i32, pid: i64) -> Value {
    Value::Struct(Struct {
        fields: vec![
            ("m_FileID".into(), Value::I32(file)),
            ("m_PathID".into(), Value::I64(pid)),
        ],
    })
}

/// One object of a made-up file: (class id, path id, value).
pub struct Obj {
    pub class_id: i32,
    pub path_id: i64,
    pub value: Value,
}

/// A serialized file (format 17, Unity 2017.4, little endian) holding the objects, written with
/// the class database's trees (embedded when `type_trees`), referring to `externals`.
pub fn serialized(db: &ClassDb, objs: &[Obj], externals: &[&str], type_trees: bool) -> Vec<u8> {
    let mut classes: Vec<i32> = Vec::new();
    for o in objs {
        if !classes.contains(&o.class_id) {
            classes.push(o.class_id);
        }
    }
    let mut m = Writer::new(false);
    m.cstr(VERSION.as_bytes());
    m.i32(2); // StandaloneOSX
    m.u8(type_trees as u8);
    m.i32(classes.len() as i32);
    for &c in &classes {
        m.i32(c);
        m.u8(0);
        m.i16(-1);
        m.bytes(&(c as u32).to_le_bytes().repeat(4));
        if type_trees {
            let mut t = (*tree(db, c)).clone();
            for (i, n) in t.nodes.iter_mut().enumerate() {
                n.index = Some(i as i32);
                n.type_flags = Some((&*n.type_name == "Array") as i32);
                n.meta_flag = Some(n.meta_flag.unwrap_or(0));
            }
            t.dump_blob(&mut m, 17, &CommonStrings::default()).unwrap();
        }
    }
    let mut data = Vec::new();
    let mut table = Vec::new();
    for o in objs {
        let t = tree(db, o.class_id);
        let b = unityfs::value::write(&t, &o.value, false, None).unwrap();
        while data.len() % 8 != 0 {
            data.push(0);
        }
        let ti = classes.iter().position(|&c| c == o.class_id).unwrap();
        table.push((o.path_id, data.len() as u32, b.len() as u32, ti as i32));
        data.extend_from_slice(&b);
    }
    m.i32(table.len() as i32);
    for (pid, off, size, ti) in &table {
        while (m.len() + 20) % 4 != 0 {
            m.u8(0);
        }
        m.i64(*pid);
        m.u32(*off);
        m.u32(*size);
        m.i32(*ti);
    }
    m.i32(0); // script types
    m.i32(externals.len() as i32);
    for (k, e) in externals.iter().enumerate() {
        m.cstr(b"");
        m.bytes(&[k as u8 + 1; 16]);
        m.i32(if e.starts_with("library/") { 0 } else { 2 });
        m.cstr(e.as_bytes());
    }
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

/// A bundle laid out as Unity 2017.4 writes them: format 6, blocks info LZ4 HC right after the
/// header (flags 0x43), LZ4 HC blocks of `block` bytes. Nodes: (name, flags, bytes).
pub fn bundle(nodes: &[(&str, u32, Vec<u8>)], block: usize) -> Vec<u8> {
    use unityfs::compress::lz4hc;
    let mut data = Vec::new();
    for n in nodes {
        data.extend_from_slice(&n.2);
    }
    let mut blocks = Vec::new();
    let mut packed = Vec::new();
    for c in data.chunks(block) {
        let z = lz4hc(c, 9).unwrap();
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
    let ci = lz4hc(&info.buf, 9).unwrap();
    let mut h = Writer::new(true);
    h.cstr(b"UnityFS");
    h.u32(6);
    h.cstr(b"5.x.x");
    h.cstr(VERSION.as_bytes());
    let total = h.len() + 20 + ci.len() + packed.len();
    h.i64(total as i64);
    h.u32(ci.len() as u32);
    h.u32(info.len() as u32);
    h.u32(0x43);
    h.bytes(&ci);
    h.bytes(&packed);
    h.buf
}

// ---- objects

pub fn build_settings(db: &ClassDb) -> Value {
    let mut v = object(db, 141);
    set(&mut v, &["m_Version"], Value::str(VERSION));
    set(&mut v, &["usesOnMouseEvents"], Value::Bool(true));
    set(
        &mut v,
        &["m_GraphicsAPIs"],
        Value::Array(vec![Value::I32(17), Value::I32(4)]),
    );
    v
}

pub fn input_manager(db: &ClassDb, axes: &[&str]) -> Value {
    let t = tree(db, 13);
    let mut v = default_of(&t, 0);
    let list = axes
        .iter()
        .enumerate()
        .map(|(k, n)| {
            let mut a = element(&t, &["m_Axes"]);
            a.set("m_Name", Value::str(n)).unwrap();
            a.set("axis", Value::I32(k as i32 % 8)).unwrap();
            a
        })
        .collect();
    set(&mut v, &["m_Axes"], Value::Array(list));
    v
}

pub fn audio_manager(db: &ClassDb) -> Value {
    let mut v = object(db, 11);
    set(&mut v, &["m_SampleRate"], Value::I32(0));
    set(&mut v, &["m_RealVoiceCount"], Value::I32(32));
    v
}

/// An AudioClip: name, load type (0 DecompressOnLoad, 1 CompressedInMemory, 2 Streaming),
/// compression format (1 Vorbis, 0 PCM), length (s), frequency, channels.
pub fn audio_clip(
    db: &ClassDb,
    name: &str,
    load: i32,
    fmt: i32,
    length: f32,
    rate: i32,
    ch: i32,
) -> Value {
    let mut v = object(db, 83);
    set(&mut v, &["m_Name"], Value::str(name));
    set(&mut v, &["m_LoadType"], Value::I32(load));
    set(&mut v, &["m_CompressionFormat"], Value::I32(fmt));
    set(&mut v, &["m_Length"], Value::F32(length));
    set(&mut v, &["m_Frequency"], Value::I32(rate));
    set(&mut v, &["m_Channels"], Value::I32(ch));
    set(&mut v, &["m_BitsPerSample"], Value::I32(16));
    set(
        &mut v,
        &["m_Resource", "m_Source"],
        Value::str("archive:/x.resource"),
    );
    set(&mut v, &["m_Resource", "m_Size"], Value::U64(1000));
    v
}

pub fn preload_data(db: &ClassDb, assets: &[(i32, i64)]) -> Value {
    let mut v = object(db, 150);
    set(
        &mut v,
        &["m_Assets"],
        Value::Array(assets.iter().map(|&(f, p)| pptr(f, p)).collect()),
    );
    v
}

/// An AnimationClip with bindings (path, typeID) and `events` animation events.
pub fn animation_clip(db: &ClassDb, name: &str, binds: &[(u32, i32)], events: usize) -> Value {
    let t = tree(db, 74);
    let mut v = default_of(&t, 0);
    set(&mut v, &["m_Name"], Value::str(name));
    let b = binds
        .iter()
        .map(|&(path, typ)| {
            let mut e = element(&t, &["m_ClipBindingConstant", "genericBindings"]);
            e.set("path", Value::U32(path)).unwrap();
            e.set("typeID", Value::I32(typ)).unwrap();
            e
        })
        .collect();
    set(
        &mut v,
        &["m_ClipBindingConstant", "genericBindings"],
        Value::Array(b),
    );
    let ev = (0..events)
        .map(|k| {
            let mut e = element(&t, &["m_Events"]);
            e.set("functionName", Value::str(&format!("OnEvent{k}")))
                .unwrap();
            e
        })
        .collect();
    set(&mut v, &["m_Events"], Value::Array(ev));
    v
}

/// A Texture2D: name, size, format (10 DXT1, 12 DXT5, 4 RGBA32), its pixels inline or in a
/// stream (offset, size, path), readable or not.
pub fn texture(
    db: &ClassDb,
    name: &str,
    (w, h): (i32, i32),
    format: i32,
    inline: &[u8],
    stream: Option<(u32, u32, &str)>,
) -> Value {
    let mut v = object(db, 28);
    set(&mut v, &["m_Name"], Value::str(name));
    set(&mut v, &["m_Width"], Value::I32(w));
    set(&mut v, &["m_Height"], Value::I32(h));
    set(&mut v, &["m_TextureFormat"], Value::I32(format));
    set(&mut v, &["m_MipCount"], Value::I32(1));
    set(&mut v, &["m_ImageCount"], Value::I32(1));
    set(&mut v, &["m_TextureDimension"], Value::I32(2));
    set(
        &mut v,
        &["m_CompleteImageSize"],
        Value::I32(inline.len().max(stream.map_or(0, |s| s.1 as usize)) as i32),
    );
    set(&mut v, &["image data"], Value::Bytes(inline.to_vec()));
    if let Some((off, size, path)) = stream {
        set(&mut v, &["m_StreamData", "offset"], Value::U32(off));
        set(&mut v, &["m_StreamData", "size"], Value::U32(size));
        set(&mut v, &["m_StreamData", "path"], Value::str(path));
    }
    v
}

fn rect(t: &TypeTree, base: &[&str], vals: &[(&str, f32)]) -> Value {
    let mut r = default_of(t, node_of(t, base));
    for (k, x) in vals {
        r.set(k, Value::F32(*x)).unwrap();
    }
    r
}

/// A Sprite drawn from texture `tex` (path id in the same file): its rectangle and border in the
/// texture's pixels.
pub fn sprite(db: &ClassDb, name: &str, tex: i64) -> Value {
    let t = tree(db, 213);
    let mut v = default_of(&t, 0);
    set(&mut v, &["m_Name"], Value::str(name));
    set(
        &mut v,
        &["m_Rect"],
        rect(
            &t,
            &["m_Rect"],
            &[("x", 3.0), ("y", 5.0), ("width", 101.0), ("height", 77.0)],
        ),
    );
    set(
        &mut v,
        &["m_Offset"],
        rect(&t, &["m_Offset"], &[("x", 0.5), ("y", -1.25)]),
    );
    set(
        &mut v,
        &["m_Border"],
        rect(
            &t,
            &["m_Border"],
            &[("x", 1.0), ("y", 2.0), ("z", 3.0), ("w", 4.1)],
        ),
    );
    set(&mut v, &["m_PixelsToUnits"], Value::F32(100.0));
    set(&mut v, &["m_RD", "texture"], pptr(0, tex));
    set(
        &mut v,
        &["m_RD", "textureRect"],
        rect(
            &t,
            &["m_RD", "textureRect"],
            &[("x", 3.0), ("y", 5.0), ("width", 101.0), ("height", 77.0)],
        ),
    );
    set(
        &mut v,
        &["m_RD", "textureRectOffset"],
        rect(
            &t,
            &["m_RD", "textureRectOffset"],
            &[("x", 0.1), ("y", 0.2)],
        ),
    );
    set(
        &mut v,
        &["m_RD", "atlasRectOffset"],
        rect(
            &t,
            &["m_RD", "atlasRectOffset"],
            &[("x", -1.0), ("y", -1.0)],
        ),
    );
    set(
        &mut v,
        &["m_RD", "uvTransform"],
        rect(
            &t,
            &["m_RD", "uvTransform"],
            &[("x", 100.0), ("y", 53.3), ("z", 100.0), ("w", 41.7)],
        ),
    );
    set(&mut v, &["m_RD", "downscaleMultiplier"], Value::F32(1.0));
    v
}

/// A SpriteAtlas whose render data entries point at the given textures (path ids in its file).
pub fn sprite_atlas(db: &ClassDb, name: &str, textures: &[i64]) -> Value {
    let t = tree(db, 687078895);
    let mut v = default_of(&t, 0);
    set(&mut v, &["m_Name"], Value::str(name));
    let entries = textures
        .iter()
        .enumerate()
        .map(|(k, &tex)| {
            let Value::Pair(mut p) = element(&t, &["m_RenderDataMap"]) else {
                unreachable!()
            };
            p.1.set("texture", pptr(0, tex)).unwrap();
            p.1.set(
                "downscaleMultiplier",
                Value::F32(if k == 0 { 1.0 } else { 0.75 }),
            )
            .unwrap();
            p.0 = Value::Pair(Box::new((default_of_first(&t), Value::I64(k as i64))));
            Value::Pair(p)
        })
        .collect();
    set(&mut v, &["m_RenderDataMap"], Value::Array(entries));
    v
}

fn default_of_first(t: &TypeTree) -> Value {
    let map = node_of(t, &["m_RenderDataMap"]);
    let pair = t.nodes[t.nodes[map].children[0]].children[1];
    let key = t.nodes[pair].children[0];
    default_of(t, t.nodes[key].children[0])
}

pub const VS: &str = "#ifdef VERTEX\n#version 150\n#extension GL_ARB_explicit_attrib_location : require\nin vec4 in_POSITION0;\nvoid main()\n{\n    gl_Position = in_POSITION0;\n    return;\n}\n#endif\n";

pub fn fs(body: &str) -> String {
    format!("#ifdef FRAGMENT\n#version 150\nuniform sampler2D _MainTex;\nlayout(location = 0) out vec4 SV_Target0;\nvoid main()\n{{\n{body}}}\n#endif\n")
}

fn entry(code: &str, keywords: &[&str]) -> Option<Slot> {
    Some(Slot::Program(Entry {
        version: 201708220,
        gpu_type: blob::GPU_GLCORE32 as i32,
        stats: vec![0, 0, 0, 0],
        keywords: keywords.iter().map(|k| k.to_string()).collect(),
        local_keywords: None,
        code: code.as_bytes().to_vec(),
        rest: vec![0; 8],
        guessed: false,
    }))
}

/// What a made-up shader holds.
#[derive(Clone, Copy, PartialEq)]
pub enum ShaderKind {
    /// GLCore programs that translate (one with a keyword variant)
    Good,
    /// a fragment program with no ES equivalent (gl_FragColor): skipped
    Bad,
    /// textureGather: a GLES 3.1 program
    Es31,
    /// already has GLES3 programs (platforms 15 and 9)
    HasGles,
    /// Direct3D 11 programs only
    D3d11,
}

/// A Shader object (Unity 2017.4 layout: one GLCore blob, m_SubPrograms per stage).
pub fn shader(db: &ClassDb, name: &str, kind: ShaderKind) -> Value {
    let t = tree(db, 48);
    let mut v = default_of(&t, 0);
    set(&mut v, &["m_Name"], Value::str(name));
    set(&mut v, &["m_ParsedForm", "m_Name"], Value::str(name));
    let body = match kind {
        ShaderKind::Bad => "    gl_FragColor = vec4(1.0);\n",
        ShaderKind::Es31 => "    SV_Target0 = textureGather(_MainTex, vec2(0.5));\n",
        _ => "    SV_Target0 = texture(_MainTex, vec2(0.5));\n",
    };
    let fs1 = format!("{VS}{}", fs(body));
    let fs2 = format!(
        "{VS}{}",
        fs("    SV_Target0 = vec4(0.25, 0.5, 0.75, 1.0);\n")
    );
    let entries = vec![
        entry(&fs1, &[]),
        entry("", &[]),
        entry(&fs2, &["FOG_LINEAR"]),
        entry("", &["FOG_LINEAR"]),
    ];
    let mut plats = BTreeMap::new();
    let platform = if kind == ShaderKind::D3d11 {
        blob::PLATFORM_D3D11
    } else {
        blob::PLATFORM_GLCORE
    };
    plats.insert(platform, PlatformBlob::One(blob::build_blob(&entries)));
    if kind == ShaderKind::HasGles {
        plats.insert(
            blob::PLATFORM_GLES3PLUS,
            PlatformBlob::One(blob::build_blob(&entries)),
        );
    }
    let fields = ShaderBlobs {
        nested: false,
        platforms: plats,
        stage_counts: None,
    }
    .store()
    .unwrap();
    let ints = |l: &[i64]| Value::Array(l.iter().map(|&x| Value::U32(x as u32)).collect());
    let flat = |l: &blob::Lengths| match l {
        blob::Lengths::Flat(x) => ints(x),
        blob::Lengths::Nested(_) => unreachable!(),
    };
    set(&mut v, &["platforms"], ints(&fields.platforms));
    set(&mut v, &["offsets"], flat(&fields.offsets));
    set(
        &mut v,
        &["compressedLengths"],
        flat(&fields.compressed_lengths),
    );
    set(
        &mut v,
        &["decompressedLengths"],
        flat(&fields.decompressed_lengths),
    );
    set(
        &mut v,
        &["compressedBlob"],
        Value::Bytes(fields.compressed_blob),
    );
    // one SubShader, one pass: vertex 0 / fragment 1, and the FOG_LINEAR variant 2 / 3
    let gpu = if kind == ShaderKind::D3d11 {
        1
    } else {
        blob::GPU_GLCORE32 as i8
    };
    let st = sub_program_tree(&t);
    let sub = |blob_index: u32, keywords: &[u16]| {
        let mut e = default_of(&t, st);
        e.set("m_BlobIndex", Value::U32(blob_index)).unwrap();
        e.set("m_GpuProgramType", Value::I8(gpu)).unwrap();
        e.set(
            "m_KeywordIndices",
            Value::Array(keywords.iter().map(|&k| Value::U16(k)).collect()),
        )
        .unwrap();
        e
    };
    let pass_node = pass_tree(&t);
    let mut pass = default_of(&t, pass_node);
    let prog = |subs: Vec<Value>| {
        let mut p = pass.get("progVertex").unwrap().clone();
        p.set("m_SubPrograms", Value::Array(subs)).unwrap();
        p
    };
    let vertex = prog(vec![sub(0, &[]), sub(2, &[3])]);
    let fragment = prog(vec![sub(1, &[]), sub(3, &[3])]);
    pass.set("progVertex", vertex).unwrap();
    pass.set("progFragment", fragment).unwrap();
    let mut ss = default_of(&t, sub_shader_tree(&t));
    ss.set("m_Passes", Value::Array(vec![pass])).unwrap();
    set(
        &mut v,
        &["m_ParsedForm", "m_SubShaders"],
        Value::Array(vec![ss]),
    );
    v
}

/// The element node of an array field below node `i`.
fn array_element(t: &TypeTree, i: usize) -> usize {
    let arr = t.nodes[i].children[0];
    t.nodes[arr].children[1]
}

fn child(t: &TypeTree, i: usize, name: &str) -> usize {
    *t.nodes[i]
        .children
        .iter()
        .find(|&&c| &*t.nodes[c].name == name)
        .unwrap()
}

fn sub_shader_tree(t: &TypeTree) -> usize {
    array_element(t, child(t, child(t, 0, "m_ParsedForm"), "m_SubShaders"))
}

fn pass_tree(t: &TypeTree) -> usize {
    array_element(t, child(t, sub_shader_tree(t), "m_Passes"))
}

fn sub_program_tree(t: &TypeTree) -> usize {
    array_element(
        t,
        child(t, child(t, pass_tree(t), "progVertex"), "m_SubPrograms"),
    )
}

// ---- a data folder

fn write(dir: &Path, rel: &str, data: &[u8]) {
    let p = dir.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, data).unwrap();
}

/// A made-up game's data folder for Cuphead's config (the textures step's files apart).
pub fn game(db: &ClassDb, dir: &Path) {
    let o = |class_id, path_id, value| Obj {
        class_id,
        path_id,
        value,
    };
    // globalgamemanagers: no type trees, as a player build has it
    let ggm = serialized(
        db,
        &[
            o(141, 1, build_settings(db)),
            o(
                13,
                2,
                input_manager(
                    db,
                    &[
                        "Horizontal",
                        "Joy1Axis3",
                        "Vertical",
                        "Joy12Axis10",
                        "Joy1Axis3x",
                        "Joy2Axis",
                    ],
                ),
            ),
            o(11, 3, audio_manager(db)),
        ],
        &["library/unity default resources"],
        false,
    );
    write(dir, "globalgamemanagers", &ggm);
    // resources.assets: clips no scene's own file has, animation clips, shaders
    let res = serialized(
        db,
        &[
            o(83, 1, audio_clip(db, "music_long", 0, 1, 95.5, 44100, 2)),
            o(83, 2, audio_clip(db, "sfx_short", 0, 1, 0.75, 44100, 1)),
            o(83, 3, audio_clip(db, "ambience", 0, 1, 12.0, 22050, 2)),
            o(83, 4, audio_clip(db, "streamed", 2, 1, 200.0, 44100, 2)),
            o(
                74,
                10,
                animation_clip(db, "idle", &[(11, 212), (12, 212)], 0),
            ),
            o(
                74,
                11,
                animation_clip(db, "run", &[(11, 212), (77, 4), (78, 4)], 0),
            ),
            o(
                74,
                12,
                animation_clip(db, "attack", &[(11, 212), (77, 4), (3, 23)], 1),
            ),
            o(74, 13, animation_clip(db, "idle", &[(90, 4)], 0)),
            o(74, 14, animation_clip(db, "", &[], 0)),
            o(48, 20, shader(db, "Custom/Good", ShaderKind::Good)),
            o(48, 21, shader(db, "Custom/Bad", ShaderKind::Bad)),
            o(48, 22, shader(db, "Custom/HasGles", ShaderKind::HasGles)),
            o(48, 23, shader(db, "Custom/Gather", ShaderKind::Es31)),
        ],
        &["library/unity default resources"],
        true,
    );
    write(dir, "resources.assets", &res);
    // scenes 0 - 2: their own clips and what their PreloadData lists from resources.assets
    let s0 = serialized(
        db,
        &[
            o(83, 1, audio_clip(db, "title_loop", 0, 1, 30.0, 44100, 2)),
            o(150, 2, preload_data(db, &[(0, 1), (1, 3), (2, 7)])),
            o(74, 3, animation_clip(db, "menu_blink", &[(5, 212)], 0)),
        ],
        &["resources.assets", "library/unity default resources"],
        true,
    );
    write(dir, "sharedassets0.assets", &s0);
    let s1 = serialized(
        db,
        &[
            o(83, 1, audio_clip(db, "boss_theme", 0, 1, 150.0, 48000, 2)),
            o(83, 2, audio_clip(db, "boss_hit", 0, 1, 2.5, 44100, 2)),
            o(83, 3, audio_clip(db, "boss_pcm", 0, 0, 40.0, 44100, 2)),
            o(150, 4, preload_data(db, &[(0, 1), (0, 2), (1, 1), (1, 2)])),
        ],
        &["resources.assets"],
        true,
    );
    write(dir, "sharedassets1.assets", &s1);
    let s2 = serialized(
        db,
        &[
            o(83, 1, audio_clip(db, "level_music", 0, 1, 120.0, 44100, 2)),
            o(150, 2, preload_data(db, &[(0, 1)])),
        ],
        &[],
        true,
    );
    write(dir, "sharedassets2.assets", &s2);
    // a scene file with a shader, and Unity's built-in extra resources
    let l0 = serialized(
        db,
        &[o(48, 5, shader(db, "Level/Shader", ShaderKind::Good))],
        &[],
        true,
    );
    write(dir, "level0", &l0);
    let extra = serialized(
        db,
        &[
            o(48, 7, shader(db, "Hidden/Builtin", ShaderKind::Good)),
            o(48, 8, shader(db, "Hidden/D3D", ShaderKind::D3d11)),
        ],
        &[],
        false,
    );
    write(dir, "Resources/unity_builtin_extra", &extra);
    // bundles: font shaders (rewritten), one with two serialized files (left as it is), one
    // whose shaders need nothing, one the regex leaves out, a manifest
    let font = serialized(
        db,
        &[o(48, 1, shader(db, "TMP/Distance", ShaderKind::Good))],
        &[],
        true,
    );
    write(
        dir,
        "StreamingAssets/AssetBundles/tmpfont_main",
        &bundle(
            &[
                ("CAB-0123456789abcdef0123456789abcdef", 4, font.clone()),
                (
                    "CAB-0123456789abcdef0123456789abcdef.resS",
                    0,
                    vec![7u8; 3000],
                ),
            ],
            1 << 12,
        ),
    );
    let gles = serialized(
        db,
        &[o(48, 1, shader(db, "TMP/Ready", ShaderKind::HasGles))],
        &[],
        true,
    );
    write(
        dir,
        "StreamingAssets/AssetBundles/tmpfont_ready",
        &bundle(
            &[("CAB-feedfeedfeedfeedfeedfeedfeedfeed", 4, gles)],
            1 << 17,
        ),
    );
    write(
        dir,
        "StreamingAssets/AssetBundles/tmpfont_scenes",
        &bundle(
            &[
                ("BuildPlayer-a", 4, font.clone()),
                ("BuildPlayer-a.sharedAssets", 4, font.clone()),
            ],
            1 << 17,
        ),
    );
    write(
        dir,
        "StreamingAssets/AssetBundles/atlas_other",
        &bundle(&[("CAB-aaaa", 4, font)], 1 << 17),
    );
    write(
        dir,
        "StreamingAssets/AssetBundles/tmpfont_main.manifest",
        b"ManifestFileVersion: 0\n",
    );
    write(
        dir,
        "StreamingAssets/AssetBundles/tmpfont_text",
        b"not a bundle\n",
    );
}

/// Cuphead's config, as portmaster-cuphead's convert/cuphead.json has it.
pub const CUPHEAD_JSON: &str = r#"{
  "steps": ["shaders", "settings", "textures", "audio", "animclips"],
  "shaders": {"precision": "default-samplers", "bundles": "^tmpfont_", "glcore4x": false, "compute": false},
  "settings": {"graphics_apis": [11, 17], "mouse_events": false, "trim_joystick_axes": true},
  "textures": {"scale": 0.375, "block": [4, 4], "scale_sharp": 0.5, "sharp_block": [4, 4],
               "sharp_names": "text|font|glyph|title_card|_loc\\b|achievement", "keep_max": 64,
               "min_side": 16, "min_convert": 4, "quality": "fast", "inline_streams": true,
               "mips": "drop", "inline": false,
               "files": "^StreamingAssets/AssetBundles/(atlas_|tex_)|^[^/]*\\.assets$"},
  "audio": {"compressed_min_s": 2.0}
}
"#;
