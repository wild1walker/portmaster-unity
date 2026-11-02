//! il2cpp_mix.py: Unity 2023.1+ (Unity 6 included), whose il2cpp is a native program, built with a
//! Unity 2022.3 release's .NET il2cpp (the converter) for the data version's own libil2cpp, from a
//! folder that is the data version's il2cpp/ with the converter's build/ in it; what both
//! libil2cpps must agree on is checked first.
use crate::util::{self, join, joins};
use crate::{bee, fetch, log, tools, Error, Result};
use androiddata::pyjson::{self, Json};
use androiddata::uversion::Version;
use regex::Regex;

pub const NATIVE_FROM: (u32, u32) = (2023, 1);
/// the .NET il2cpp that converts, by the metadata version the data's libil2cpp reads
pub const CONVERTERS: [(u32, &str); 2] = [(31, "2022.3.62f3"), (29, "2022.3.23f1")];
pub const RECIPE: i128 = 1;
pub const EMITTED: [&str; 24] = [
    "Il2CppCodeRegistration",
    "Il2CppMetadataRegistration",
    "Il2CppCodeGenModule",
    "Il2CppCodeGenOptions",
    "Il2CppTokenRangePair",
    "Il2CppRange",
    "Il2CppTokenIndexMethodTuple",
    "Il2CppTokenAdjustorThunkPair",
    "Il2CppRGCTXDefinition",
    "Il2CppRGCTXDefinitionData",
    "Il2CppRGCTXConstrainedData",
    "Il2CppRGCTXDataType",
    "Il2CppMethodSpec",
    "Il2CppGenericMethodFunctionsDefinitions",
    "Il2CppGenericMethodIndices",
    "Il2CppGenericInst",
    "Il2CppGenericClass",
    "Il2CppGenericContext",
    "Il2CppType",
    "Il2CppArrayType",
    "Il2CppTypeEnum",
    "Il2CppTypeDefinitionSizes",
    "Il2CppInteropData",
    "Il2CppWindowsRuntimeFactoryTableEntry",
];
pub const HEADERS: [&str; 5] = [
    "il2cpp-class-internals.h",
    "il2cpp-runtime-metadata.h",
    "il2cpp-metadata.h",
    "il2cpp-blob.h",
    "il2cpp-pinvoke-support.h",
];
pub const METADATA_HEADER: &str = "vm/GlobalMetadataFileInternals.h";
pub const LINES: [(u32, u32); 5] = [(2023, 1), (2023, 2), (6000, 0), (6000, 1), (6000, 2)];
pub const LATER_METADATA: [((u32, u32), u32); 4] = [
    ((6000, 3), 39),
    ((6000, 4), 39),
    ((6000, 5), 107),
    ((6000, 6), 108),
];

/// needed: the version's il2cpp is a native program (2023.1+).
pub fn needed(v: &Version) -> bool {
    v.line() >= NATIVE_FROM
}

/// refusal: why a 2023.1+ version cannot be built mixed, or None.
pub fn refusal(v: &Version) -> Option<String> {
    if !needed(v) || LINES.contains(&v.line()) {
        return None;
    }
    let mv = LATER_METADATA
        .iter()
        .find(|(l, _)| *l == v.line())
        .map(|x| x.1);
    let what = match mv {
        Some(m) => format!(
            "its libil2cpp reads global-metadata.dat version {m}, which no .NET il2cpp (Unity 2022.3) writes"
        ),
        None => "its libil2cpp has not been compared with 2022.3's il2cpp yet".into(),
    };
    Some(format!(
        "Unity {v}: its il2cpp is a native program, and {what}; the IL2CPP route covers 2017.4 - 2022.3, and 2023.1 - 6000.2 with 2022.3's il2cpp (docs/il2cpp.md)"
    ))
}

/// _code: C source without comments, whitespace runs made one space, stripped.
pub fn code(text: &str) -> String {
    let t = Regex::new(r"(?s)/\*.*?\*/").unwrap().replace_all(text, " ");
    let t = Regex::new(r"//[^\n]*").unwrap().replace_all(&t, " ");
    let t = Regex::new(r"\s+").unwrap().replace_all(&t, " ");
    t.trim_matches(pkgzip::py::is_space).to_string()
}

/// definitions: {name: normalized definition} of the struct, union and enum definitions.
pub fn definitions(text: &str) -> Vec<(String, String)> {
    let code = code(text);
    let b = code.as_bytes();
    let mut out: Vec<(String, String)> = Vec::new();
    let head = Regex::new(r"\b(?:typedef\s+)?(struct|union|enum)\s*([A-Za-z_]\w*)?\s*\{").unwrap();
    let tail_rx = Regex::new(r"^\s*([A-Za-z_]\w*)\s*;").unwrap();
    for m in head.captures_iter(&code) {
        let all = m.get(0).unwrap();
        let mut depth = 0i64;
        let mut i = all.end() - 1;
        while i < b.len() {
            if b[i] == b'{' {
                depth += 1;
            } else if b[i] == b'}' {
                depth -= 1;
                if depth == 0 {
                    break;
                }
            }
            i += 1;
        }
        let Some(end) = code[i.min(code.len())..].find(';').map(|k| k + i) else {
            continue;
        };
        let body = code[all.start()..end + 1].to_string();
        let tail = tail_rx
            .captures(&code[(i + 1).min(end + 1)..end + 1])
            .map(|c| c[1].to_string());
        let mut names: Vec<String> = Vec::new();
        if let Some(g) = m.get(2) {
            names.push(g.as_str().to_string());
        }
        if let Some(t) = tail {
            if !names.contains(&t) {
                names.push(t);
            }
        }
        for n in names {
            if !out.iter().any(|(k, _)| *k == n) {
                out.push((n, body.clone()));
            }
        }
    }
    out
}

/// metadata_version: the global-metadata.dat version a libil2cpp source tree reads.
pub fn metadata_version(libil2cpp: &str) -> Option<u32> {
    let p = joins(&[libil2cpp, "vm", "GlobalMetadata.cpp"]);
    let t = std::fs::read(&p).ok()?;
    let t = String::from_utf8_lossy(&t);
    Regex::new(r"GlobalMetadataHeader->version\s*==\s*(\d+)")
        .unwrap()
        .captures(&t)
        .and_then(|c| c[1].parse().ok())
}

fn defs(libil2cpp: &str, names: &[&str]) -> Vec<(String, String)> {
    let mut d: Vec<(String, String)> = Vec::new();
    for n in names {
        let p = join(libil2cpp, n);
        if let Ok(t) = std::fs::read(&p) {
            for (k, v) in definitions(&String::from_utf8_lossy(&t)) {
                if !d.iter().any(|(x, _)| *x == k) {
                    d.push((k, v));
                }
            }
        }
    }
    d
}

fn get<'a>(d: &'a [(String, String)], k: &str) -> Option<&'a String> {
    d.iter().find(|(x, _)| x == k).map(|x| &x.1)
}

/// compat: what differs between the two libil2cpps that a mixed build depends on.
pub fn compat(conv_lib: &str, data_lib: &str) -> Vec<String> {
    let mut why = Vec::new();
    let (mc, md) = (metadata_version(conv_lib), metadata_version(data_lib));
    let opt = |v: Option<u32>| v.map_or("None".to_string(), |x| x.to_string());
    match (mc, md) {
        (Some(c), Some(d)) if c != d => why.push(format!(
            "global-metadata.dat version: the converter writes {c}, this libil2cpp reads {d}"
        )),
        (Some(_), Some(_)) => {}
        _ => why.push(format!(
            "metadata version not found (converter {}, data {})",
            opt(mc),
            opt(md)
        )),
    }
    let (fc, fd) = (
        defs(conv_lib, &[METADATA_HEADER]),
        defs(data_lib, &[METADATA_HEADER]),
    );
    if fd.is_empty() {
        why.push(format!(
            "no {METADATA_HEADER} in the data version's libil2cpp"
        ));
    }
    let mut names: Vec<&String> = fc.iter().chain(fd.iter()).map(|(k, _)| k).collect();
    names.sort();
    names.dedup();
    let changed: Vec<String> = names
        .into_iter()
        .filter(|n| get(&fc, n) != get(&fd, n))
        .cloned()
        .collect();
    if !changed.is_empty() {
        why.push(format!(
            "global-metadata.dat tables laid out differently: {}",
            changed.join(", ")
        ));
    }
    let (hc, hd) = (defs(conv_lib, &HEADERS), defs(data_lib, &HEADERS));
    let mut changed: Vec<&str> = EMITTED
        .iter()
        .copied()
        .filter(|n| get(&hc, n) != get(&hd, n))
        .collect();
    changed.sort();
    if !changed.is_empty() {
        why.push(format!(
            "structures the generated C++ fills in laid out differently: {}",
            changed.join(", ")
        ));
    }
    why
}

/// converter_wanted: il2cpp/build and libil2cpp's headers and GlobalMetadata.cpp.
pub fn converter_wanted(rel: &str) -> bool {
    if rel.starts_with("il2cpp/build/") {
        return true;
    }
    rel.starts_with("il2cpp/libil2cpp/")
        && (rel.ends_with(".h") || rel.ends_with("vm/GlobalMetadata.cpp"))
}

pub fn converter_for(data_lib: &str) -> Option<&'static str> {
    let mv = metadata_version(data_lib)?;
    CONVERTERS.iter().find(|(m, _)| *m == mv).map(|x| x.1)
}

/// fetch_converter: the converter's il2cpp/build and libil2cpp headers (or a full editor of that
/// version in the home). Returns the folder holding il2cpp/.
pub fn fetch_converter(home: &str, conv: &str) -> Result<String> {
    let full = joins(&[home, conv, "editor"]);
    if util::isfile(&join(&full, ".complete"))
        && util::isfile(&joins(&[&full, "il2cpp", "build", "deploy", "il2cpp.dll"]))
    {
        return Ok(full);
    }
    let dest = joins(&[home, conv, "converter"]);
    if util::isfile(&join(&dest, ".complete")) {
        return Ok(dest);
    }
    let installer = tools::editor_installer(conv)?;
    log(&format!(
        "converter {conv}: il2cpp/build and libil2cpp's headers from {installer}"
    ));
    let mut ns = fetch::open_nsis(&installer)?;
    let mut deps: Vec<(String, Vec<String>)> = Vec::new();
    let rels = fetch::nsis_fetch(
        &mut ns,
        &|p| {
            tools::editor_rel(p)
                .filter(|r| r.starts_with("il2cpp/build/") && r.ends_with(".deps.json"))
        },
        &dest,
    )?;
    for rel in rels {
        let b = util::read(&join(&dest, &rel))?;
        let b = b.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&b);
        let j = pyjson::loads_bytes(b)
            .map_err(|e| Error::Fail(format!("json.decoder.JSONDecodeError: {e}")))?;
        let d = util::dirname(&rel).to_string();
        let files = tools::runtime_pack_files(&j);
        match deps.iter_mut().find(|x| x.0 == d) {
            Some(x) => x.1.extend(files),
            None => deps.push((d, files)),
        }
    }
    let deps_ref = &deps;
    let select = |p: &str| -> Option<String> {
        let r = tools::editor_rel(p)?;
        if !converter_wanted(&r) || r.ends_with(".deps.json") {
            return None;
        }
        let name = r.rsplit('/').next().unwrap_or(&r).to_lowercase();
        let d = util::dirname(&r);
        if deps_ref.iter().any(|(k, v)| k == d && v.contains(&name))
            || r.ends_with(".pdb")
            || r.ends_with(".mdb")
        {
            return None;
        }
        Some(r)
    };
    let got = fetch::nsis_fetch(&mut ns, &select, &dest)?;
    if !got.iter().any(|g| g == "il2cpp/build/deploy/il2cpp.dll") {
        return crate::exit(format!(
            "{installer}: no il2cpp/build/deploy/il2cpp.dll (not a .NET il2cpp)"
        ));
    }
    for (d, _) in &deps {
        for n in util::listdir(&join(&dest, d))? {
            if n.ends_with(".deps.json") {
                let p = joins(&[&dest, d, &n]);
                util::replace(&p, &format!("{p}.orig"))?;
            }
        }
    }
    let info = Json::Obj(vec![
        ("version".into(), Json::str(conv)),
        ("installer".into(), Json::str(&installer)),
        ("files".into(), Json::Int(got.len() as i128)),
    ]);
    util::write_text(&join(&dest, ".complete"), &pyjson::dumps(&info, Some(0)))?;
    Ok(dest)
}

/// _link_tree: dst = src's files, hard links where the file system allows, else copies.
fn link_tree(src: &str, dst: &str, skip: &[&str]) -> Result<()> {
    walk_link(src, src, dst, skip)
}

fn walk_link(src: &str, root: &str, dst: &str, skip: &[&str]) -> Result<()> {
    let rel = util::relpath(root, src);
    if skip.contains(&rel.split('/').next().unwrap_or("")) {
        return Ok(());
    }
    let out = if rel == "." {
        dst.to_string()
    } else {
        join(dst, &rel)
    };
    util::makedirs(&out)?;
    let Ok(rd) = std::fs::read_dir(root) else {
        return Ok(());
    };
    let mut dirs = Vec::new();
    for e in rd.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        let s = join(root, &n);
        if util::isdir(&s) {
            dirs.push(s);
            continue;
        }
        let d = util::normpath(&join(&out, &n));
        if util::lexists(&d) {
            util::unlink(&d)?;
        }
        if std::fs::hard_link(&s, &d).is_err() {
            util::copy2(&s, &d)?;
        }
    }
    for d in dirs {
        if !util::islink(&d) {
            walk_link(src, &d, dst, skip)?;
        }
    }
    Ok(())
}

/// prepare: the mixed folder (made once): (an Editor/Data-like folder, the converter's version).
pub fn prepare(
    version: &Version,
    editor: &str,
    home: &str,
    conv: Option<&str>,
) -> Result<(String, String)> {
    let data_lib = joins(&[editor, "il2cpp", "libil2cpp"]);
    let conv = match conv.or_else(|| converter_for(&data_lib)) {
        Some(c) => c.to_string(),
        None => {
            let mut cs: Vec<(u32, &str)> = CONVERTERS.to_vec();
            cs.sort();
            return Err(Error::Unsupported(format!(
                "Unity {version}: its il2cpp is a native program, and its libil2cpp reads global-metadata.dat version {}, which no .NET il2cpp (Unity 2022.3: {}) writes",
                metadata_version(&data_lib).map_or("None".into(), |v| v.to_string()),
                cs.iter().map(|(m, v)| format!("{v} {m}")).collect::<Vec<_>>().join(", ")
            )));
        }
    };
    let conv_dir = fetch_converter(home, &conv)?;
    let conv_lib = joins(&[&conv_dir, "il2cpp", "libil2cpp"]);
    let why = compat(&conv_lib, &data_lib);
    if !why.is_empty() {
        return Err(Error::Unsupported(format!(
            "Unity {version}: its il2cpp is a native program, and Unity {conv}'s .NET il2cpp cannot convert for its libil2cpp: {}",
            why.join("; ")
        )));
    }
    let mix = joins(&[home, &version.text, &format!("mix-{conv}")]);
    let stamp = Json::Obj(vec![
        ("recipe".into(), Json::Int(RECIPE)),
        ("unity".into(), Json::str(&version.text)),
        ("converter".into(), Json::str(&conv)),
        (
            "from".into(),
            Json::List(vec![
                Json::Str(util::relpath(editor, home)),
                Json::Str(util::relpath(&conv_dir, home)),
            ]),
        ),
    ]);
    let mark = join(&mix, ".complete");
    let done = std::fs::read(&mark)
        .ok()
        .and_then(|b| pyjson::loads_bytes(&b).ok())
        .is_some_and(|j| j == stamp);
    if !done {
        log(&format!(
            "il2cpp: Unity {version} has a native il2cpp: converting with Unity {conv}'s .NET il2cpp for {version}'s libil2cpp (metadata {}) -> {mix}",
            metadata_version(&data_lib).map_or("None".into(), |v| v.to_string())
        ));
        util::rmtree(&mix);
        link_tree(&join(editor, "il2cpp"), &join(&mix, "il2cpp"), &["build"])?;
        link_tree(
            &joins(&[&conv_dir, "il2cpp", "build"]),
            &joins(&[&mix, "il2cpp", "build"]),
            &[],
        )?;
        for d in ["linux-x64", "linux-arm64"] {
            let p = joins(&[
                &mix,
                "il2cpp",
                "build",
                "deploy",
                "bee_backend",
                d,
                "bee_backend",
            ]);
            if util::lexists(&p) {
                util::unlink(&p)?;
            }
        }
        let p = joins(&[&mix, "il2cpp", "build", "deploy", "il2cpp"]);
        if util::lexists(&p) {
            util::unlink(&p)?;
        }
        util::write_text(&mark, &pyjson::dumps(&stamp, None))?;
    }
    bee::install(&mix)?;
    Ok((mix, conv))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defs_and_compat() {
        let a = "/* x */ typedef struct Il2CppType { void* data; // c\n uint32_t bits; } Il2CppType;\n\
                 typedef enum { A = 1, B } Il2CppTypeEnum;\nstruct Il2CppRange { int32_t start; int32_t length; };";
        let d = definitions(a);
        let names: Vec<&str> = d.iter().map(|x| x.0.as_str()).collect();
        assert_eq!(names, vec!["Il2CppType", "Il2CppTypeEnum", "Il2CppRange"]);
        assert_eq!(
            d[0].1,
            "typedef struct Il2CppType { void* data; uint32_t bits; } Il2CppType;"
        );
        let base = std::env::temp_dir().join(format!("il2cpp-mix-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        for (side, extra) in [("conv", ""), ("data", " int32_t more;")] {
            let lib = base.join(side);
            std::fs::create_dir_all(lib.join("vm")).unwrap();
            std::fs::write(
                lib.join("vm/GlobalMetadata.cpp"),
                "IL2CPP_ASSERT(s_GlobalMetadataHeader->version == 31);",
            )
            .unwrap();
            std::fs::write(
                lib.join("vm/GlobalMetadataFileInternals.h"),
                "typedef struct Il2CppGlobalMetadataHeader { int32_t sanity; } Il2CppGlobalMetadataHeader;",
            )
            .unwrap();
            std::fs::write(
                lib.join("il2cpp-runtime-metadata.h"),
                format!("typedef struct Il2CppRange {{ int32_t start;{extra} }} Il2CppRange;"),
            )
            .unwrap();
        }
        let why = compat(
            base.join("conv").to_str().unwrap(),
            base.join("data").to_str().unwrap(),
        );
        assert_eq!(
            why,
            vec!["structures the generated C++ fills in laid out differently: Il2CppRange"]
        );
        assert_eq!(
            converter_for(base.join("data").to_str().unwrap()),
            Some("2022.3.62f3")
        );
        let _ = std::fs::remove_dir_all(&base);
        assert!(refusal(&Version::parse("6000.3.1f1").unwrap())
            .unwrap()
            .contains("version 39"));
        assert!(refusal(&Version::parse("6000.0.58f2").unwrap()).is_none());
    }
}
