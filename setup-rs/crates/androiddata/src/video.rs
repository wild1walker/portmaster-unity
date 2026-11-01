//! Hidden/VideoDecodeAndroid for the Android player (video.py): the shader Unity's Android
//! VideoPlayer draws video frames with, built on the data's own Hidden/VideoDecode (its layout is
//! that Unity version's): name and passes replaced, the two passes made from its
//! FLIP_RGBA_TO_RGBA and FLIP_RGBASPLIT_TO_RGBA, one GLES3 program each (our own GLSL,
//! video_decode_android.shader, the copy of convert/pmunity's), no other platform. It goes into
//! unity_builtin_extra under a free path id, and globalgamemanagers' ScriptMapper gets its name.
//! The YCbCr conversion follows the game's clips (BT.601 for VP8 / VP9, BT.709 for H.264 / HEVC).
use crate::android_data::{Ctx, Stop};
use shaderx::blob::{self, Slot};
use shaderx::convert::STAGES;
use shaderx::tree;
use std::collections::BTreeMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use unityfs::{DataFile, SerializedFile, Value};

pub const NAME: &str = "Hidden/VideoDecodeAndroid";
pub const TEMPLATE: &str = "Hidden/VideoDecode";
/// (pass of the new shader, desktop pass it is made from)
const PASSES: [(&str, &str); 2] = [
    ("RGBAExternal_To_RGBA", "FLIP_RGBA_TO_RGBA"),
    ("RGBASplitExternal_To_RGBA", "FLIP_RGBASPLIT_TO_RGBA"),
];
const GLSL: &str = include_str!("video_decode_android.shader");
const PREFERRED_PATH_ID: i64 = 16001; // next to Hidden/VideoDecode's 16000 (any free id works)
const BT709_OFF: &str = "const bool UM_BT709 = false;";

fn value_err(m: impl Into<String>) -> Stop {
    Stop::Fail(format!("ValueError: {}", m.into()))
}

/// {pass name: GLSL program source} with the YCbCr conversion of VP8 / VP9 clips (bt709 false)
/// or H.264 / HEVC ones (true).
pub fn programs(bt709: bool) -> Result<BTreeMap<String, String>, Stop> {
    let path = "video_decode_android.shader";
    if GLSL.matches(BT709_OFF).count() != PASSES.len() {
        return Err(value_err(format!(
            "{path}: expected one \"{BT709_OFF}\" per pass"
        )));
    }
    let text = if bt709 {
        GLSL.replace(BT709_OFF, "const bool UM_BT709 = true;")
    } else {
        GLSL.to_string()
    };
    // re.finditer(r'^//// pass (\S+)\n(.*?)(?=^//// pass |\Z)', text, re.S | re.M)
    let starts: Vec<usize> = (0..text.len())
        .filter(|&i| {
            (i == 0 || text.as_bytes()[i - 1] == b'\n') && text[i..].starts_with("//// pass ")
        })
        .collect();
    let mut out = BTreeMap::new();
    for (k, &a) in starts.iter().enumerate() {
        let head = &text[a + "//// pass ".len()..];
        let name: String = head.chars().take_while(|c| !c.is_whitespace()).collect();
        if name.is_empty() || !head[name.len()..].starts_with('\n') {
            continue;
        }
        let body_start = a + "//// pass ".len() + name.len() + 1;
        let end = starts
            .get(k + 1)
            .copied()
            .unwrap_or(text.len())
            .max(body_start);
        let body = shaderx::py::strip(&text[body_start..end]);
        out.insert(name, format!("{body}\n"));
    }
    let missing: Vec<&str> = PASSES
        .iter()
        .map(|p| p.0)
        .filter(|p| !out.contains_key(*p))
        .collect();
    if !missing.is_empty() {
        return Err(value_err(format!("{path}: no program for {missing:?}")));
    }
    Ok(out)
}

fn text(v: Option<&Value>) -> String {
    v.and_then(Value::as_bytes)
        .map(|b| String::from_utf8_lossy(b).into_owned())
        .unwrap_or_default()
}

/// _pass_name: the pass's state name, else its own name.
fn pass_name(p: &Value) -> String {
    let st = text(p.get("m_State").and_then(|s| s.get("m_Name")));
    if !st.is_empty() {
        return st;
    }
    text(p.get("m_Name"))
}

fn field<'a>(t: &'a Value, name: &str) -> Result<&'a Value, Stop> {
    t.get(name)
        .ok_or_else(|| Stop::Fail(format!("KeyError: '{name}'")))
}

fn field_mut<'a>(t: &'a mut Value, name: &str) -> Result<&'a mut Value, Stop> {
    t.get_mut(name)
        .ok_or_else(|| Stop::Fail(format!("KeyError: '{name}'")))
}

fn list_mut<'a>(v: &'a mut Value, what: &str) -> Result<&'a mut Vec<Value>, Stop> {
    v.as_array_mut()
        .ok_or_else(|| Stop::Fail(format!("{what}: not a list")))
}

fn int(v: &Value, what: &str) -> Result<i64, Stop> {
    v.as_i64()
        .ok_or_else(|| Stop::Fail(format!("{what}: not an integer")))
}

fn truthy(v: Option<&Value>) -> bool {
    match v {
        None => false,
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Struct(s)) => !s.fields.is_empty(),
        Some(Value::Str(b)) => !b.is_empty(),
        Some(_) => true,
    }
}

/// The new blob index of the template's entry i (copied once, retyped GLES3).
fn take(
    i: i64,
    src: &[Option<Slot>],
    entries: &mut Vec<Option<Slot>>,
    remap: &mut BTreeMap<i64, i64>,
) -> Result<i64, Stop> {
    if let Some(&j) = remap.get(&i) {
        return Ok(j);
    }
    let mut e = usize::try_from(i)
        .ok()
        .and_then(|k| src.get(k))
        .cloned()
        .ok_or_else(|| Stop::Fail("IndexError: list index out of range".into()))?;
    if let Some(Slot::Program(p)) = &mut e {
        p.gpu_type = blob::GPU_GLES3 as i32;
    }
    let j = entries.len() as i64;
    remap.insert(i, j);
    entries.push(e);
    Ok(j)
}

/// The Hidden/VideoDecodeAndroid Shader tree made from a Hidden/VideoDecode one.
pub fn build(template: &Value, bt709: bool) -> Result<Value, Stop> {
    let mut t = template.clone();
    let name = text(field(&t, "m_ParsedForm")?.get("m_Name"));
    if name != TEMPLATE {
        return Err(value_err(format!("template is {name}, not {TEMPLATE}")));
    }
    // a Windows build's has GLES3 programs translated from Direct3D 11 (the shaders step)
    // instead, laid out as Unity's GL programs of that version
    let platforms: Vec<i64> = field(&t, "platforms")?
        .as_array()
        .map(|a| a.iter().filter_map(Value::as_i64).collect())
        .unwrap_or_default();
    let Some((plat, typ)) = [
        (blob::PLATFORM_GLCORE, blob::GPU_GLCORE32),
        (blob::PLATFORM_GLES3PLUS, blob::GPU_GLES3),
    ]
    .into_iter()
    .find(|(p, _)| platforms.contains(p)) else {
        return Err(value_err(format!(
            "{TEMPLATE} has no OpenGL Core (or translated GLES3) programs to take the layout from"
        )));
    };
    let progs = programs(bt709)?;
    let mut blobs = tree::blobs(&t).map_err(Stop::Fail)?;
    let src = tree::platform_entries(&t, plat, &blobs).map_err(Stop::Fail)?;
    let ss0 = field(field(&t, "m_ParsedForm")?, "m_SubShaders")?
        .as_array()
        .and_then(|a| a.first())
        .cloned()
        .ok_or_else(|| Stop::Fail("IndexError: list index out of range".into()))?;
    let mut ss = ss0;
    let mut by_name: BTreeMap<String, Value> = BTreeMap::new();
    for p in field(&ss, "m_Passes")?
        .as_array()
        .cloned()
        .unwrap_or_default()
    {
        by_name.insert(pass_name(&p).to_uppercase(), p);
    }
    let (mut new_passes, mut entries, mut remap) = (Vec::new(), Vec::new(), BTreeMap::new());
    for (new, old) in PASSES {
        let Some(p) = by_name.get(old) else {
            return Err(value_err(format!("{TEMPLATE} has no pass {old}")));
        };
        let mut p = p.clone();
        field_mut(&mut p, "m_State")?.set("m_Name", Value::str(&new.to_uppercase()))?;
        if matches!(p.get("m_Name"), Some(Value::Str(b)) if !b.is_empty()) {
            p.set("m_Name", Value::str(&new.to_uppercase()))?;
        }
        let mut code_entries = std::collections::BTreeSet::new();
        for st in STAGES {
            let Some(prog) = p.get_mut(st) else { continue };
            if !truthy(Some(prog)) {
                continue;
            }
            // the lists: m_SubPrograms, then each tier's (with its parameter blob indices)
            let mut lists: Vec<(Option<usize>, Vec<Value>)> = Vec::new();
            if let Some(l) = prog.get("m_SubPrograms") {
                lists.push((None, l.as_array().cloned().unwrap_or_default()));
            }
            if truthy(prog.get("m_PlayerSubPrograms")) {
                let players = field(prog, "m_PlayerSubPrograms")?
                    .as_array()
                    .cloned()
                    .unwrap_or_default();
                for (tier, l) in players.into_iter().enumerate() {
                    lists.push((Some(tier), l.as_array().cloned().unwrap_or_default()));
                }
            }
            for (tier, lst) in lists {
                let mut keep = Vec::new();
                for (k, sp) in lst.iter().enumerate() {
                    if int(field(sp, "m_GpuProgramType")?, "m_GpuProgramType")? == typ {
                        keep.push(k);
                    }
                }
                let mut subs = Vec::new();
                for &k in &keep {
                    let mut sp = lst[k].clone();
                    sp.set("m_GpuProgramType", Value::I64(blob::GPU_GLES3))?;
                    let bi = take(
                        int(field(&lst[k], "m_BlobIndex")?, "m_BlobIndex")?,
                        &src,
                        &mut entries,
                        &mut remap,
                    )?;
                    sp.set("m_BlobIndex", Value::I64(bi))?;
                    if st == "progVertex" {
                        code_entries.insert(bi);
                    }
                    subs.push(sp);
                }
                match tier {
                    None => *field_mut(prog, "m_SubPrograms")? = Value::Array(subs),
                    Some(tier) => {
                        let pbis = list_mut(
                            field_mut(prog, "m_ParameterBlobIndices")?,
                            "m_ParameterBlobIndices",
                        )?;
                        let slot = pbis.get_mut(tier).ok_or_else(|| {
                            Stop::Fail("IndexError: list index out of range".into())
                        })?;
                        let pbi = slot.as_array().cloned().unwrap_or_default();
                        let mut new_pbi = Vec::new();
                        for &k in &keep {
                            let v = pbi.get(k).ok_or_else(|| {
                                Stop::Fail("IndexError: list index out of range".into())
                            })?;
                            let x = int(v, "m_ParameterBlobIndices")?;
                            new_pbi.push(Value::I64(if x >= 0 {
                                take(x, &src, &mut entries, &mut remap)?
                            } else {
                                x
                            }));
                        }
                        *slot = Value::Array(new_pbi);
                        let players = list_mut(
                            field_mut(prog, "m_PlayerSubPrograms")?,
                            "m_PlayerSubPrograms",
                        )?;
                        players[tier] = Value::Array(subs);
                    }
                }
            }
        }
        if code_entries.is_empty() {
            return Err(value_err(format!(
                "{TEMPLATE} pass {old}: no OpenGL Core 3.2 program"
            )));
        }
        for &i in &code_entries {
            match entries.get_mut(i as usize) {
                Some(Some(Slot::Program(e))) => {
                    e.code = progs[new].as_bytes().to_vec();
                    e.keywords = Vec::new();
                    if e.local_keywords.is_some() {
                        e.local_keywords = Some(Vec::new());
                    }
                }
                _ => {
                    return Err(value_err(format!(
                        "{TEMPLATE} pass {old}: program entry {i} does not parse"
                    )))
                }
            }
        }
        if matches!(p.get("m_Platforms"), Some(Value::Array(_))) {
            p.set(
                "m_Platforms",
                Value::Array(vec![Value::I64(blob::PLATFORM_GLES3PLUS)]),
            )?;
        }
        new_passes.push(p);
    }
    ss.set("m_Passes", Value::Array(new_passes))?;
    let pf = field_mut(&mut t, "m_ParsedForm")?;
    pf.set("m_SubShaders", Value::Array(vec![ss]))?;
    pf.set("m_Name", Value::str(NAME))?;
    if matches!(t.get("m_Name"), Some(Value::Str(b)) if !b.is_empty()) {
        t.set("m_Name", Value::str(NAME))?;
    }
    let plats: Vec<i64> = blobs.platforms.keys().copied().collect();
    for p in plats {
        blobs.remove(p);
    }
    let n = entries.len();
    blobs.set_entries(blob::PLATFORM_GLES3PLUS, &entries, &vec![0; n], Some(1));
    let fields = blobs.store().map_err(|e| Stop::Fail(e.to_string()))?;
    tree::set_blob(&mut t, &fields).map_err(Stop::Fail)?;
    Ok(t)
}

/// The Shader objects of a file: (path id, m_ParsedForm.m_Name), in the file's order.
fn shader_names(sf: &SerializedFile) -> Result<Vec<(i64, String)>, Stop> {
    let mut out = Vec::new();
    for o in &sf.objects {
        if sf.class_name(o) != "Shader" {
            continue;
        }
        let t = sf.read_typetree(o.path_id)?;
        out.push((o.path_id, text(field(&t, "m_ParsedForm")?.get("m_Name"))));
    }
    Ok(out)
}

fn free_path_id(sf: &SerializedFile) -> i64 {
    let mut pid = PREFERRED_PATH_ID;
    while sf.object(pid).is_some() {
        pid += 1;
    }
    pid
}

/// add_to_builtin_extra: Hidden/VideoDecodeAndroid added to a loaded unity_builtin_extra that
/// has Hidden/VideoDecode; its path id, or None (no template, or already there).
pub fn add_to_builtin_extra(
    sf: &mut SerializedFile,
    ctx: &mut Ctx,
    bt709: bool,
) -> Result<Option<i64>, Stop> {
    let mut template = None;
    for (pid, name) in shader_names(sf)? {
        if name == NAME {
            return Ok(None);
        }
        if name == TEMPLATE {
            template = Some(pid);
        }
    }
    let Some(tp) = template else {
        return Ok(None);
    };
    let t = build(&sf.read_typetree(tp)?, bt709)?;
    let pid = free_path_id(sf);
    let tt = sf.typetree_of(tp)?;
    let data = sf.write_value(&tt, &t)?;
    sf.add_object(tp, pid, data)?;
    ctx.log(&format!(
        "video: {NAME} added to unity_builtin_extra (path id {pid}, passes {}; {})",
        PASSES.map(|p| p.0).join(", "),
        if bt709 {
            "BT.709 (H.264 / HEVC clips)"
        } else {
            "BT.601 (VP8 / VP9 clips)"
        }
    ));
    Ok(Some(pid))
}

/// map_shader: globalgamemanagers' ScriptMapper maps the name to the shader in
/// unity_builtin_extra. Returns whether it changed.
pub fn map_shader(
    sf: &mut SerializedFile,
    path_id: i64,
    notes: &mut Vec<String>,
) -> Result<bool, Stop> {
    let Some(fid) = sf
        .externals
        .iter()
        .position(|e| {
            String::from_utf8_lossy(&e.path)
                .to_lowercase()
                .ends_with("unity_builtin_extra")
        })
        .map(|i| i as i64 + 1)
    else {
        notes.push(format!(
            "video: globalgamemanagers does not reference unity_builtin_extra; {NAME} not mapped"
        ));
        return Ok(false);
    };
    let Some(sm) = sf
        .objects
        .iter()
        .find(|o| sf.class_name(o) == "ScriptMapper")
        .map(|o| o.path_id)
    else {
        notes.push(format!(
            "video: no ScriptMapper in globalgamemanagers; {NAME} not mapped"
        ));
        return Ok(false);
    };
    let mut t = sf.read_typetree(sm)?;
    let m = field_mut(&mut t, "m_Shaders")?;
    let lst = match m {
        Value::Struct(_) => list_mut(field_mut(m, "m_ObjectToName")?, "m_ObjectToName")?,
        v => list_mut(v, "m_Shaders")?,
    };
    let ptr = Value::Struct(unityfs::value::Struct {
        fields: vec![
            ("m_FileID".into(), Value::I64(fid)),
            ("m_PathID".into(), Value::I64(path_id)),
        ],
    });
    let have = lst.iter().position(|e| match e {
        Value::Pair(p) => p.1.as_bytes() == Some(NAME.as_bytes()),
        _ => false,
    });
    match have {
        Some(i) => {
            // a desktop build lists the name with a null pointer (the editor maps every built-in
            // shader name; the other platforms' ones point nowhere)
            let Value::Pair(p) = &lst[i] else {
                unreachable!()
            };
            let nonzero = |k: &str| p.0.get(k).and_then(Value::as_int).is_some_and(|x| x != 0);
            if nonzero("m_FileID") || nonzero("m_PathID") {
                return Ok(false);
            }
            lst[i] = Value::Pair(Box::new((ptr, Value::str(NAME))));
        }
        None => lst.push(Value::Pair(Box::new((ptr, Value::str(NAME))))),
    }
    sf.save_typetree(sm, &t)?;
    notes.push(format!(
        "ScriptMapper: {NAME} -> unity_builtin_extra:{path_id}"
    ));
    Ok(true)
}

/// path_id_for: the path id add_to_builtin_extra gives the shader in that file (None: it adds
/// none).
pub fn path_id_for(path: &str, ctx: &Ctx) -> Result<Option<i64>, Stop> {
    let f = DataFile::open(path, &ctx.opts())?;
    let names = shader_names(&f.sf)?;
    if names.iter().any(|(_, n)| n == NAME) || !names.iter().any(|(_, n)| n == TEMPLATE) {
        return Ok(None);
    }
    Ok(Some(free_path_id(&f.sf)))
}

/// has_template: whether a unity_builtin_extra file has Hidden/VideoDecode (a byte search).
pub fn has_template(path: &str) -> bool {
    std::fs::read(path).is_ok_and(|b| b.windows(TEMPLATE.len()).any(|w| w == TEMPLATE.as_bytes()))
}

// ---------------------------------------------------------------- the game's clips

fn contains(h: &[u8], tag: &[u8]) -> bool {
    h.windows(tag.len()).any(|w| w == tag)
}

/// sniff: the video codec of the clip at [offset, offset + size) of path: vp8, vp9, h264, hevc,
/// or the container (webm, mp4) when the codec is not seen, or None.
fn sniff(path: &Path, offset: u64, size: u64) -> Option<&'static str> {
    let read = || -> std::io::Result<(Vec<u8>, Vec<u8>)> {
        let mut f = std::fs::File::open(path)?;
        f.seek(SeekFrom::Start(offset))?;
        let mut head = Vec::new();
        (&mut f).take(size.min(1 << 16)).read_to_end(&mut head)?;
        let mut tail = Vec::new();
        if size > 1 << 16 {
            f.seek(SeekFrom::Start(
                offset + size.saturating_sub(1 << 20).max(1 << 16),
            ))?;
            (&mut f)
                .take((size - (1 << 16)).min(1 << 20))
                .read_to_end(&mut tail)?;
        }
        Ok((head, tail))
    };
    let (head, tail) = read().ok()?;
    if head.starts_with(b"\x1a\x45\xdf\xa3") {
        for (tag, codec) in [
            (&b"V_VP8"[..], "vp8"),
            (b"V_VP9", "vp9"),
            (b"V_MPEG4/ISO/AVC", "h264"),
            (b"V_MPEGH/ISO/HEVC", "hevc"),
        ] {
            if contains(&head, tag) {
                return Some(codec);
            }
        }
        return Some("webm");
    }
    if head.len() >= 8
        && [&b"ftyp"[..], b"moov", b"mdat", b"free", b"wide", b"skip"].contains(&&head[4..8])
    {
        for (tag, codec) in [
            (&b"avc1"[..], "h264"),
            (b"avc3", "h264"),
            (b"hvc1", "hevc"),
            (b"hev1", "hevc"),
        ] {
            if contains(&head, tag) || contains(&tail, tag) {
                return Some(codec);
            }
        }
        return Some("mp4");
    }
    None
}

/// clip_codecs: {codec: count} of the VideoClips in a game's data folder, in the order the
/// Python's dict has them.
fn clip_codecs(data_dir: &str, ctx: &Ctx) -> Result<Vec<(String, usize)>, Stop> {
    let mut names: Vec<String> = std::fs::read_dir(data_dir)
        .map_err(|e| Stop::Fail(e.to_string()))?
        .filter_map(|e| e.ok().map(|e| e.file_name().to_string_lossy().into_owned()))
        .collect();
    names.sort();
    let mut out: Vec<(String, usize)> = Vec::new();
    for n in names {
        let level = n
            .strip_prefix("level")
            .is_some_and(|d| !d.is_empty() && d.chars().all(|c| c.is_ascii_digit()));
        if !(n.ends_with(".assets") || level) {
            continue;
        }
        let Ok(f) = DataFile::open(Path::new(data_dir).join(&n), &ctx.opts()) else {
            continue;
        };
        for o in &f.sf.objects {
            if f.sf.class_name(o) != "VideoClip" {
                continue;
            }
            let t = f.sf.read_typetree(o.path_id)?;
            let er = t.get("m_ExternalResources");
            let source = text(er.and_then(|e| e.get("m_Source")));
            let src = source.rsplit('/').next().unwrap_or("").to_string();
            let num = |k: &str| {
                er.and_then(|e| e.get(k))
                    .and_then(Value::as_u64)
                    .unwrap_or(0)
            };
            let codec = if src.is_empty() {
                None
            } else {
                sniff(
                    &Path::new(data_dir).join(&src),
                    num("m_Offset"),
                    num("m_Size"),
                )
            };
            let c = codec.unwrap_or("unknown");
            match out.iter_mut().find(|(k, _)| k == c) {
                Some((_, v)) => *v += 1,
                None => out.push((c.to_string(), 1)),
            }
        }
    }
    Ok(out)
}

/// wants_bt709: whether the game's clips are H.264 / HEVC (macOS / Windows builds: Unity's
/// desktop players convert those with BT.709) rather than VP8 / VP9 (Linux builds: BT.601).
pub fn wants_bt709(data_dir: &str, ctx: &Ctx) -> Result<bool, Stop> {
    let codecs = clip_codecs(data_dir, ctx)?;
    let get = |k: &str| codecs.iter().find(|(c, _)| c == k).map_or(0, |x| x.1);
    let hd = get("h264") + get("hevc");
    let sw = get("vp8") + get("vp9");
    if !codecs.is_empty() {
        let d: Vec<String> = codecs
            .iter()
            .map(|(k, v)| format!("{}: {v}", pkgzip::py::str_repr(k)))
            .collect();
        println!(
            "video: clips {{{}}}: {} conversion{}",
            d.join(", "),
            if hd > sw { "BT.709" } else { "BT.601" },
            if hd > 0 && sw > 0 {
                " (clips of both kinds: the more common one; the others are converted with it too)"
            } else {
                ""
            }
        );
    }
    Ok(hd > sw)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn programs_of_the_shader_file() {
        let p = programs(false).unwrap();
        assert_eq!(
            p.keys().collect::<Vec<_>>(),
            ["RGBAExternal_To_RGBA", "RGBASplitExternal_To_RGBA"]
        );
        for v in p.values() {
            assert!(v.contains("#ifdef VERTEX") && v.contains(BT709_OFF) && v.ends_with("\n"));
            assert!(!v.contains("//// pass"));
        }
        let p = programs(true).unwrap();
        assert!(p
            .values()
            .all(|v| v.contains("const bool UM_BT709 = true;")));
    }

    fn st(fields: Vec<(&str, Value)>) -> Value {
        Value::Struct(unityfs::value::Struct {
            fields: fields.into_iter().map(|(k, v)| (k.into(), v)).collect(),
        })
    }

    fn sub(blob_index: u32, gpu: i8) -> Value {
        st(vec![
            ("m_BlobIndex", Value::U32(blob_index)),
            ("m_GpuProgramType", Value::I8(gpu)),
        ])
    }

    fn pass(name: &str, vs: u32, fs: u32) -> Value {
        let prog = |i| {
            st(vec![(
                "m_SubPrograms",
                Value::Array(vec![sub(i, 6), sub(i + 10, 1)]),
            )])
        };
        st(vec![
            ("m_State", st(vec![("m_Name", Value::str(name))])),
            ("progVertex", prog(vs)),
            ("progFragment", prog(fs)),
        ])
    }

    /// A made-up Hidden/VideoDecode (Unity 2017.4's layout: one GLCore blob) -> the Android one.
    #[test]
    fn video_shader_from_the_desktop_one() {
        let e = |code: &str, kw: &[&str]| {
            Some(Slot::Program(shaderx::blob::Entry {
                version: 201708220,
                gpu_type: blob::GPU_GLCORE32 as i32,
                stats: vec![0; 4],
                keywords: kw.iter().map(|k| k.to_string()).collect(),
                local_keywords: None,
                code: code.as_bytes().to_vec(),
                rest: vec![0; 8],
                guessed: false,
            }))
        };
        let entries = vec![
            e(
                "#ifdef VERTEX\n#version 150\nvoid main() {}\n#endif\n",
                &["FLIP"],
            ),
            e("", &["FLIP"]),
            e(
                "#ifdef VERTEX\n#version 150\nvoid main() { }\n#endif\n",
                &[],
            ),
            e("", &[]),
            e(
                "#ifdef VERTEX\n#version 150\nvoid main() {  }\n#endif\n",
                &[],
            ),
            e("", &[]),
        ];
        let mut plats = BTreeMap::new();
        plats.insert(
            blob::PLATFORM_GLCORE,
            shaderx::blob::PlatformBlob::One(blob::build_blob(&entries)),
        );
        let f = shaderx::blob::ShaderBlobs {
            nested: false,
            platforms: plats,
            stage_counts: None,
        }
        .store()
        .unwrap();
        let mut t = st(vec![
            ("m_Name", Value::str("")),
            (
                "m_ParsedForm",
                st(vec![
                    ("m_Name", Value::str(TEMPLATE)),
                    (
                        "m_SubShaders",
                        Value::Array(vec![st(vec![(
                            "m_Passes",
                            Value::Array(vec![
                                pass("COPY", 4, 5),
                                pass("FLIP_RGBASPLIT_TO_RGBA", 2, 3),
                                pass("flip_rgba_to_rgba", 0, 1),
                            ]),
                        )])]),
                    ),
                ]),
            ),
        ]);
        tree::set_blob(&mut t, &f).unwrap();
        let v = build(&t, false).unwrap();
        let pf = v.get("m_ParsedForm").unwrap();
        assert_eq!(text(pf.get("m_Name")), NAME);
        assert_eq!(text(v.get("m_Name")), ""); // (an empty object name stays empty)
        let passes = pf.path(&["m_SubShaders"]).unwrap().as_array().unwrap()[0]
            .get("m_Passes")
            .unwrap()
            .as_array()
            .unwrap()
            .clone();
        let names: Vec<String> = passes.iter().map(pass_name).collect();
        assert_eq!(names, ["RGBAEXTERNAL_TO_RGBA", "RGBASPLITEXTERNAL_TO_RGBA"]);
        let subs = |p: &Value, st: &str| -> Vec<(i64, i64)> {
            p.path(&[st, "m_SubPrograms"])
                .unwrap()
                .as_array()
                .unwrap()
                .iter()
                .map(|s| {
                    (
                        s.get("m_BlobIndex").unwrap().as_i64().unwrap(),
                        s.get("m_GpuProgramType").unwrap().as_i64().unwrap(),
                    )
                })
                .collect()
        };
        // the GLCore subprograms only, retyped GLES3, their entries copied in the order taken
        assert_eq!(subs(&passes[0], "progVertex"), [(0, 4)]);
        assert_eq!(subs(&passes[0], "progFragment"), [(1, 4)]);
        assert_eq!(subs(&passes[1], "progVertex"), [(2, 4)]);
        let b = tree::blobs(&v).unwrap();
        assert_eq!(
            b.platforms.keys().collect::<Vec<_>>(),
            [&blob::PLATFORM_GLES3PLUS]
        );
        let got = b
            .entries(blob::PLATFORM_GLES3PLUS, &Default::default(), None)
            .unwrap();
        assert_eq!(got.len(), 4);
        let progs = programs(false).unwrap();
        match &got[0] {
            Some(Slot::Program(p)) => {
                assert_eq!(p.code, progs["RGBAExternal_To_RGBA"].as_bytes());
                assert!(p.keywords.is_empty());
                assert_eq!(p.gpu_type, blob::GPU_GLES3 as i32);
            }
            _ => panic!("entry 0"),
        }
        match &got[2] {
            Some(Slot::Program(p)) => {
                assert_eq!(p.code, progs["RGBASplitExternal_To_RGBA"].as_bytes())
            }
            _ => panic!("entry 2"),
        }
        // a template without the passes: the Python's ValueError
        let mut t2 = t.clone();
        t2.path_mut(&["m_ParsedForm", "m_SubShaders"])
            .unwrap()
            .as_array_mut()
            .unwrap()[0]
            .get_mut("m_Passes")
            .unwrap()
            .as_array_mut()
            .unwrap()
            .truncate(1);
        assert!(
            matches!(build(&t2, false), Err(Stop::Fail(m)) if m == "ValueError: Hidden/VideoDecode has no pass FLIP_RGBA_TO_RGBA")
        );
    }

    #[test]
    fn codecs_by_their_headers() {
        let d = std::env::temp_dir().join(format!("pmu-video-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let p = d.join("clips.resource");
        let mut data = vec![0u8; 100];
        data.extend_from_slice(b"\x1a\x45\xdf\xa3....webm....V_VP8....");
        data.extend_from_slice(b"\0\0\0\x18ftypisom....avc1");
        std::fs::write(&p, &data).unwrap();
        assert_eq!(sniff(&p, 100, 25), Some("vp8"));
        assert_eq!(sniff(&p, 125, 20), Some("h264"));
        assert_eq!(sniff(&p, 125, 16), Some("mp4"));
        assert_eq!(sniff(&p, 0, 10), None);
        assert_eq!(sniff(&d.join("none"), 0, 10), None);
        let _ = std::fs::remove_dir_all(&d);
    }
}
