//! il2cpp.py's build: the assembly set il2cpp compiles (the game's, the IL2CPP player's UnityEngine
//! ones, the IL2CPP class libraries of the game's profile), UnityLinker's and il2cpp's command
//! lines as an Android IL2CPP build of the version runs them, then (on the handheld) the steps
//! and the output folder: lib/<abi>/libil2cpp.so, Data/Managed, il2cpp.json.
use crate::native::{self, Native};
use crate::tools::{self, abi_info, Toolchain, BCL_DIR, MAP_FILE_UNTIL, METADATA_MAGIC};
use crate::util::{self, at, join, joins};
use crate::{device, exit, fail, log, mix, Error, Result};
use androiddata::pyjson::{self, Json};
use androiddata::uversion::{self as UV, Version};
use std::collections::HashMap;

/// What `build` is given (il2cpp.build's arguments).
#[derive(Clone, Debug, Default)]
pub struct BuildOpts {
    pub data: String,
    pub out: String,
    pub abi: String,
    pub home: Option<String>,
    pub pkg: Option<String>,
    pub jobs: Option<usize>,
    pub keep_cpp: bool,
    pub dry: bool,
    pub strip: String,
    pub unity: Option<String>,
    pub managed_files: Vec<String>,
    pub asmpatch: Option<String>,
    /// natively on this aarch64 machine (the only route pmu runs)
    pub on_device: bool,
    pub device_tc: Option<String>,
    pub link_xml: Vec<String>,
    pub assembly_patches: Vec<(String, String)>,
    pub streaming_assets: String,
    pub data_path: bool,
    /// the Unity version when `data` has no serialized file (`apk --il2cpp`)
    pub data_version: Option<Version>,
    /// the data's script assemblies (else listed_assemblies(data))
    pub listed: Option<Vec<String>>,
}

impl BuildOpts {
    /// The defaults of il2cpp.build (arm64-v8a, --strip low, StreamingAssets in the OBB).
    pub fn new(data: &str, out: &str) -> BuildOpts {
        BuildOpts {
            data: data.into(),
            out: out.into(),
            abi: "arm64-v8a".into(),
            strip: "low".into(),
            streaming_assets: "obb".into(),
            ..Default::default()
        }
    }
}

/// default_home: $PMUNITY_IL2CPP_HOME, else ~/.cache/pmunity/il2cpp.
pub fn default_home() -> String {
    match std::env::var("PMUNITY_IL2CPP_HOME") {
        Ok(h) if !h.is_empty() => h,
        _ => {
            let home = std::env::var("HOME").unwrap_or_else(|_| "~".into());
            joins(&[&home, ".cache", "pmunity", "il2cpp"])
        }
    }
}

// ------------------------------------------------------------------ assemblies

fn u16le(d: &[u8], p: usize) -> Result<u16> {
    d.get(p..p + 2)
        .map(|b| u16::from_le_bytes(b.try_into().unwrap()))
        .ok_or_else(|| Error::Fail("struct.error: unpack_from requires a buffer of 2 bytes".into()))
}

fn u32le(d: &[u8], p: usize) -> Result<u32> {
    d.get(p..p + 4)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
        .ok_or_else(|| Error::Fail("struct.error: unpack_from requires a buffer of 4 bytes".into()))
}

fn u64le(d: &[u8], p: usize) -> Result<u64> {
    d.get(p..p + 8)
        .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
        .ok_or_else(|| Error::Fail("struct.error: unpack_from requires a buffer of 8 bytes".into()))
}

fn index0(d: &[u8], from: usize) -> Result<usize> {
    d.get(from..)
        .and_then(|r| r.iter().position(|&c| c == 0))
        .map(|i| i + from)
        .ok_or_else(|| Error::Fail("ValueError: subsection not found".into()))
}

/// _AsmRefs: the names of a .NET assembly's AssemblyRef table (a minimal ECMA-335 reader).
pub fn asm_refs(path: &str) -> Result<Vec<String>> {
    let d = util::read(path)?;
    let mut refs = Vec::new();
    let pe = u32le(&d, 0x3c)? as usize;
    if d.get(pe..pe + 4) != Some(b"PE\0\0") {
        return fail(format!("ValueError: {path}: not a PE file"));
    }
    let nsec = u16le(&d, pe + 6)? as usize;
    let optsz = u16le(&d, pe + 20)? as usize;
    let opt = pe + 24;
    let magic = u16le(&d, opt)?;
    let dd = opt + if magic == 0x10b { 96 } else { 112 };
    let cli_rva = u32le(&d, dd + 14 * 8)?;
    let mut secs = Vec::new();
    for i in 0..nsec {
        let o = opt + optsz + 40 * i;
        secs.push((
            u32le(&d, o + 8)? as u64,
            u32le(&d, o + 12)? as u64,
            u32le(&d, o + 16)? as u64,
            u32le(&d, o + 20)? as u64,
        ));
    }
    let off = |rva: u64| -> Result<usize> {
        for &(vsz, va, rsz, ro) in &secs {
            if va <= rva && rva < va + vsz.max(rsz) {
                return Ok((rva - va + ro) as usize);
            }
        }
        fail("ValueError: rva outside sections")
    };
    if cli_rva == 0 {
        return fail(format!("ValueError: {path}: not a .NET assembly"));
    }
    let c = off(cli_rva as u64)?;
    let md_rva = u32le(&d, c + 8)?;
    let m = off(md_rva as u64)?;
    let vlen = u32le(&d, m + 12)? as usize;
    let mut p = m + 16 + vlen;
    let nstreams = u16le(&d, p + 2)?;
    p += 4;
    let mut streams: HashMap<String, (usize, u32)> = HashMap::new();
    for _ in 0..nstreams {
        let so = u32le(&d, p)? as usize;
        let ss = u32le(&d, p + 4)?;
        let e = index0(&d, p + 8)?;
        let nm = String::from_utf8(d[p + 8..e].to_vec())
            .map_err(|e| Error::Fail(format!("UnicodeDecodeError: {e}")))?;
        streams.insert(nm, (m + so, ss));
        p = (e + 4) & !3;
    }
    let t = streams
        .get("#~")
        .or_else(|| streams.get("#-"))
        .ok_or_else(|| Error::Fail("KeyError: '#-'".into()))?
        .0;
    let strings = streams
        .get("#Strings")
        .ok_or_else(|| Error::Fail("KeyError: '#Strings'".into()))?
        .0;
    let hs = *d
        .get(t + 6)
        .ok_or_else(|| Error::Fail("IndexError: index out of range".into()))?;
    let valid = u64le(&d, t + 8)?;
    let mut rows = [0u64; 64];
    let mut q = t + 24;
    for (i, r) in rows.iter_mut().enumerate() {
        if valid >> i & 1 == 1 {
            *r = u32le(&d, q)? as u64;
            q += 4;
        }
    }
    if valid >> 0x23 & 1 == 0 {
        return Ok(refs);
    }
    let si: u64 = if hs & 1 != 0 { 4 } else { 2 };
    let gi: u64 = if hs & 2 != 0 { 4 } else { 2 };
    let bi: u64 = if hs & 4 != 0 { 4 } else { 2 };
    let idx = |tabs: &[usize]| -> u64 {
        if tabs.iter().map(|&x| rows[x]).max().unwrap() >= 1 << 16 {
            4
        } else {
            2
        }
    };
    let coded = |bits: u32, tabs: &[usize]| -> u64 {
        if tabs.iter().map(|&x| rows[x]).max().unwrap() >= 1 << (16 - bits) {
            4
        } else {
            2
        }
    };
    let tdor = coded(2, &[0x02, 0x01, 0x1b]);
    let rs = coded(2, &[0x00, 0x1a, 0x23, 0x01]);
    let mrp = coded(3, &[0x02, 0x01, 0x1a, 0x06, 0x1b]);
    let hc = coded(2, &[0x04, 0x08, 0x17]);
    let hfm = coded(1, &[0x04, 0x08]);
    let hs_ = coded(1, &[0x14, 0x17]);
    let mdor = coded(1, &[0x06, 0x0a]);
    let cat = coded(3, &[0x06, 0x0a]);
    let hca = coded(
        5,
        &[
            0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x00, 0x0e, 0x17, 0x14, 0x11, 0x1a, 0x1b,
            0x20, 0x23, 0x26, 0x27, 0x28, 0x2a, 0x2b, 0x2c,
        ],
    );
    let hds = coded(2, &[0x02, 0x06, 0x20]);
    let impl_ = coded(2, &[0x26, 0x23, 0x27]);
    let mf = coded(1, &[0x04, 0x06]);
    let tom = coded(1, &[0x02, 0x06]);
    let _ = (impl_, tom);
    let sizes: [u64; 0x23] = [
        2 + si + gi * 3,
        rs + si * 2,
        4 + si * 2 + tdor + idx(&[0x04]) + idx(&[0x06]),
        idx(&[0x04]),
        2 + si + bi,
        idx(&[0x06]),
        4 + 2 + 2 + si + bi + idx(&[0x08]),
        idx(&[0x08]),
        2 + 2 + si,
        idx(&[0x02]) + tdor,
        mrp + si + bi,
        2 + hc + bi,
        hca + cat + bi,
        hfm + bi,
        2 + hds + bi,
        2 + 4 + idx(&[0x02]),
        4 + idx(&[0x04]),
        bi,
        idx(&[0x02]) + idx(&[0x14]),
        idx(&[0x14]),
        2 + si + tdor,
        idx(&[0x02]) + idx(&[0x17]),
        idx(&[0x17]),
        2 + si + bi,
        2 + idx(&[0x06]) + hs_,
        idx(&[0x02]) + mdor * 2,
        si,
        bi,
        2 + mf + si + idx(&[0x1a]),
        4 + idx(&[0x04]),
        4 + 4,
        4,
        4 + 2 * 4 + 4 + bi + si * 2,
        4,
        4 * 3,
    ];
    let mut q = q as u64;
    for i in 0..0x23 {
        q += sizes[i] * rows[i];
    }
    let n = rows[0x23];
    let rsz = 2 * 4 + 4 + bi + si + si + bi;
    for k in 0..n {
        let r = q + k * rsz;
        let nm_off = (r + 12 + bi) as usize;
        let name_idx = if si == 4 {
            u32le(&d, nm_off)? as usize
        } else {
            u16le(&d, nm_off)? as usize
        };
        let e = index0(&d, strings + name_idx)?;
        refs.push(String::from_utf8_lossy(&d[strings + name_idx..e]).into_owned());
    }
    Ok(refs)
}

/// assembly_refs: the names (.dll) of the assemblies an assembly references.
pub fn assembly_refs(path: &str) -> Result<Vec<String>> {
    Ok(asm_refs(path)?
        .into_iter()
        .map(|r| format!("{r}.dll"))
        .collect())
}

/// listed_assemblies: the script assemblies the data names (ScriptingAssemblies.json, else
/// MonoManager.m_AssemblyNames in globalgamemanagers); empty when neither can be read.
pub fn listed_assemblies(data: &str) -> Vec<String> {
    let p = join(data, "ScriptingAssemblies.json");
    if util::isfile(&p) {
        let Ok(b) = std::fs::read(&p) else {
            return Vec::new();
        };
        let Ok(j) = pyjson::loads_bytes(&b) else {
            return Vec::new();
        };
        let Json::Obj(_) = j else { return Vec::new() };
        return match j.get("names") {
            Some(Json::List(l)) => l.iter().filter(|n| n.truthy()).map(Json::py_str).collect(),
            _ => Vec::new(),
        };
    }
    let g = join(data, "globalgamemanagers");
    if util::isfile(&g) {
        return monomanager_names(&g);
    }
    Vec::new()
}

/// monomanager_names: MonoManager.m_AssemblyNames of a globalgamemanagers file (empty on any error).
pub fn monomanager_names(path: &str) -> Vec<String> {
    let db = match unityfs::ClassDb::default_db() {
        Ok(db) => std::sync::Arc::new(db),
        Err(_) => return Vec::new(),
    };
    let opts = unityfs::Options {
        db: Some(db),
        ..Default::default()
    };
    let Ok(f) = unityfs::DataFile::open(path, &opts) else {
        return Vec::new();
    };
    for o in &f.sf.objects {
        if f.sf.class_name(o) != "MonoManager" {
            continue;
        }
        let Ok(t) = f.sf.read_typetree(o.path_id) else {
            return Vec::new();
        };
        return t
            .get("m_AssemblyNames")
            .and_then(unityfs::Value::as_array)
            .map(|a| {
                a.iter()
                    .filter_map(|x| x.as_bytes())
                    .filter(|b| !b.is_empty())
                    .map(|b| String::from_utf8_lossy(b).into_owned())
                    .collect()
            })
            .unwrap_or_default();
    }
    Vec::new()
}

pub fn is_engine(name: &str) -> bool {
    if name == "UnityEngine.dll" {
        return true;
    }
    let Some(mid) = name
        .strip_prefix("UnityEngine.")
        .and_then(|r| r.strip_suffix("Module.dll"))
    else {
        return false;
    };
    !mid.is_empty() && mid.chars().all(|c| c.is_alphanumeric() || c == '_')
}

/// An insertion-ordered map (a Python dict of names).
#[derive(Default, Clone)]
pub struct Ordered {
    pub items: Vec<(String, String)>,
    index: HashMap<String, usize>,
}

impl Ordered {
    pub fn get(&self, k: &str) -> Option<&String> {
        self.index.get(k).map(|&i| &self.items[i].1)
    }

    pub fn contains(&self, k: &str) -> bool {
        self.index.contains_key(k)
    }

    pub fn set(&mut self, k: &str, v: String) {
        match self.index.get(k) {
            Some(&i) => self.items[i].1 = v,
            None => {
                self.index.insert(k.to_string(), self.items.len());
                self.items.push((k.to_string(), v));
            }
        }
    }

    pub fn len(&self) -> usize {
        self.items.len()
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }
}

/// stage_assemblies: the assembly set il2cpp compiles, copied into out_dir. Returns {name: source}.
pub fn stage_assemblies(
    data: &str,
    tc: &Toolchain,
    out_dir: &str,
    managed_files: &[String],
    listed: &[String],
) -> Result<Ordered> {
    let gm = join(data, "Managed");
    let eng = joins(&[&tc.pkg, "Variations", "il2cpp", "Managed"]);
    let bcl = join(&tc.editor, &tc.bcl);
    let mut bcl_files: HashMap<String, String> = HashMap::new();
    for w in util::walk(&bcl, false) {
        for n in &w.files {
            if n.ends_with(".dll") {
                bcl_files
                    .entry(n.clone())
                    .or_insert_with(|| join(&w.root, n));
            }
        }
    }
    let mut engine = Ordered::default();
    for n in util::listdir(&eng)? {
        if n.ends_with(".dll") {
            let p = join(&eng, &n);
            engine.set(&n, p);
        }
    }
    let mut over: HashMap<String, String> = HashMap::new();
    for f in managed_files {
        over.insert(util::basename(f).to_string(), util::abspath(f));
    }
    let mut names: Vec<String> = util::listdir(&gm)?;
    names.extend(over.keys().cloned());
    names.sort();
    names.dedup();
    let mut game = Ordered::default();
    for n in names {
        let listed_ = listed.contains(&n);
        if !n.ends_with(".dll")
            || (!over.contains_key(&n)
                && (is_engine(&n) || (bcl_files.contains_key(&n) && !listed_)))
        {
            continue;
        }
        let src = over.get(&n).cloned().unwrap_or_else(|| join(&gm, &n));
        game.set(&n, src);
        if bcl_files.contains_key(&n) && listed_ && !over.contains_key(&n) {
            log(&format!(
                "  {n}: a class library's name, but one of the game's own assemblies (the data lists it): its copy, whole"
            ));
        }
    }
    let mut chosen = engine.clone();
    for (k, v) in &game.items {
        chosen.set(k, v.clone());
    }
    let mut todo: Vec<String> = chosen.items.iter().map(|x| x.0.clone()).collect();
    let mut missing: Vec<(String, String)> = Vec::new();
    while let Some(n) = todo.pop() {
        let src = chosen.get(&n).unwrap().clone();
        for r in assembly_refs(&src)? {
            if chosen.contains(&r) {
                continue;
            }
            let mut s = bcl_files.get(&r).or(engine.get(&r)).cloned();
            if s.is_none() && util::isfile(&join(&gm, &r)) {
                s = Some(join(&gm, &r));
                log(&format!(
                    "  {r}: not in the IL2CPP class libraries; the game's copy is used"
                ));
            }
            match s {
                None => missing.push((r, n.clone())),
                Some(s) => {
                    chosen.set(&r, s);
                    todo.push(r);
                }
            }
        }
    }
    missing.sort();
    missing.dedup();
    for (r, by) in &missing {
        log(&format!(
            "  WARNING: {by} references {r}, which is nowhere (il2cpp may refuse or leave calls to it failing)"
        ));
    }
    if util::isdir(out_dir) {
        std::fs::remove_dir_all(out_dir).map_err(at(out_dir))?;
    }
    util::makedirs(out_dir)?;
    let mut items = chosen.items.clone();
    items.sort();
    for (n, src) in &items {
        util::copyfile(src, &join(out_dir, n))?;
    }
    log(&format!(
        "assemblies: {} of the game, {} UnityEngine (IL2CPP player), {} class libraries ({})",
        game.len(),
        engine.len(),
        chosen.len() as i64 - game.len() as i64 - engine.len() as i64,
        tc.bcl
    ));
    Ok(chosen)
}

/// player_settings: incremental GC (boot.config's gc-max-time-slice): (on, time slice).
pub fn player_settings(data: &str) -> (bool, Option<i64>) {
    let mut out = (false, None);
    if let Ok(b) = std::fs::read(join(data, "boot.config")) {
        let t = String::from_utf8_lossy(&b).into_owned();
        for line in t.split_inclusive('\n') {
            let line = pkgzip::py::strip(line);
            let (k, v) = line.split_once('=').unwrap_or((line, ""));
            let vs = pkgzip::py::strip(v);
            if k == "gc-max-time-slice" && !vs.is_empty() && vs.chars().all(|c| c.is_ascii_digit())
            {
                if let Some(n) = pkgzip::py::int(v, 10) {
                    if n > 0 {
                        out = (true, Some(n as i64));
                    }
                }
            }
        }
    }
    out
}

/// dotnet_level: 2 (.NET 3.5) or 4 (.NET 4.x).
pub fn dotnet_level(data: &str, version: Option<&Version>) -> Result<u8> {
    if version.is_some_and(|v| v.line() >= (2019, 2)) {
        return Ok(4);
    }
    androiddata::android_data::dotnet_level(data).ok_or_else(|| {
        Error::Unsupported(format!(
            "{}: no mscorlib.dll, no Assembly-CSharp.dll that references it: which .NET?",
            join(data, "Managed")
        ))
    })
}

/// subset_profile: a .NET 3.5 game of the old Mono's "unity" profile (".NET 2.0 Subset").
pub fn subset_profile(data: &str) -> Result<bool> {
    let p = joins(&[data, "Managed", "mscorlib.dll"]);
    if !util::isfile(&p) {
        return Ok(false);
    }
    let a = androiddata::clrmeta::Assembly::open(&p).map_err(|e| Error::Fail(format!("{e:?}")))?;
    let types = a.types().map_err(|e| Error::Fail(format!("{e:?}")))?;
    let has = |n: &str| types.iter().any(|t| t.full == n);
    Ok(has("System.MonoTouchAOTHelper") && !has("System.TermInfoDriver"))
}

/// bcl_profile: (il2cpp's --dotnetprofile, the class library folder of Editor/Data).
pub fn bcl_profile(
    version: &Version,
    net: u8,
    subset: bool,
) -> Result<(&'static str, &'static str)> {
    if net == 2 {
        if version.line() >= (2019, 1) {
            return Err(Error::Unsupported(format!(
                "Unity {version}: a .NET 3.5 game (IL2CPP builds of .NET 3.5 games are made for 2017.x - 2018.x; 2019.1 is the last line with that runtime, 2019.2+ have none)"
            )));
        }
        return Ok(if subset {
            ("legacyunity", "Mono/lib/mono/unity")
        } else {
            ("net20", "Mono/lib/mono/2.0")
        });
    }
    if version.line() < (2018, 1) {
        return Ok(("net45", "MonoBleedingEdge/lib/mono/4.5"));
    }
    if version.line() >= (2021, 2) {
        return Ok(("unityaot-linux", "MonoBleedingEdge/lib/mono/unityaot-linux"));
    }
    Ok(("unityaot", BCL_DIR))
}

// ------------------------------------------------------------------ the output's checks

/// _elf_symbols: (data, string table offset, [(name offset, value, size, info, shndx)]) of one
/// symbol table (11: SHT_DYNSYM, 2: SHT_SYMTAB).
pub fn elf_symbols(
    path: &str,
    table: u32,
) -> Result<(Vec<u8>, usize, Vec<(u32, u64, u64, u8, u16)>)> {
    let d = util::read(path)?;
    if d.get(..4) != Some(b"\x7fELF") {
        return fail(format!("ValueError: {path}: not an ELF file"));
    }
    let is64 = d.get(4) == Some(&2);
    let (shoff, shentsize, shnum) = if is64 {
        (
            u64le(&d, 0x28)? as usize,
            u16le(&d, 0x3a)? as usize,
            u16le(&d, 0x3c)? as usize,
        )
    } else {
        (
            u32le(&d, 0x20)? as usize,
            u16le(&d, 0x2e)? as usize,
            u16le(&d, 0x30)? as usize,
        )
    };
    let mut secs = Vec::new();
    for i in 0..shnum {
        let o = shoff + i * shentsize;
        if is64 {
            secs.push((
                u32le(&d, o + 4)?,
                u64le(&d, o + 24)? as usize,
                u64le(&d, o + 32)? as usize,
                u32le(&d, o + 40)? as usize,
                u64le(&d, o + 56)? as usize,
            ));
        } else {
            secs.push((
                u32le(&d, o + 4)?,
                u32le(&d, o + 16)? as usize,
                u32le(&d, o + 20)? as usize,
                u32le(&d, o + 24)? as usize,
                u32le(&d, o + 36)? as usize,
            ));
        }
    }
    let mut syms = Vec::new();
    let mut stro = 0;
    for &(typ, off, size, link, ent) in &secs {
        if typ != table {
            continue;
        }
        stro = secs
            .get(link)
            .ok_or_else(|| Error::Fail("IndexError: list index out of range".into()))?
            .1;
        if ent == 0 {
            return fail("ZeroDivisionError: integer division or modulo by zero");
        }
        for k in 1..size / ent {
            let o = off + k * ent;
            if is64 {
                syms.push((
                    u32le(&d, o)?,
                    u64le(&d, o + 8)?,
                    u64le(&d, o + 16)?,
                    d[o + 4],
                    u16le(&d, o + 6)?,
                ));
            } else {
                syms.push((
                    u32le(&d, o)?,
                    u32le(&d, o + 4)? as u64,
                    u32le(&d, o + 8)? as u64,
                    *d.get(o + 12).unwrap_or(&0),
                    u16le(&d, o + 14)?,
                ));
            }
        }
    }
    Ok((d, stro, syms))
}

/// elf_exports: the defined dynamic symbols' names.
pub fn elf_exports(path: &str) -> Result<std::collections::HashSet<String>> {
    let (d, stro, syms) = elf_symbols(path, 11)?;
    let mut out = std::collections::HashSet::new();
    for (nm, _, _, info, shndx) in syms {
        if shndx == 0 || info >> 4 == 0 {
            continue;
        }
        let s = stro + nm as usize;
        let e = index0(&d, s)?;
        out.insert(String::from_utf8_lossy(&d[s..e]).into_owned());
    }
    Ok(out)
}

/// write_symbol_map: SymbolMap-<arch> from the unstripped library's symbol table.
pub fn write_symbol_map(elf: &str, path: &str, thumb: bool) -> Result<usize> {
    let (_, _, syms) = elf_symbols(elf, 2)?;
    let mut funcs: Vec<(u64, u64)> = syms
        .iter()
        .filter(|s| s.3 & 0xf == 2 && s.2 > 0 && s.4 != 0)
        .map(|s| (if thumb { s.1 & !1 } else { s.1 }, s.2))
        .collect();
    funcs.sort();
    funcs.dedup();
    let mut b = (funcs.len() as i32).to_le_bytes().to_vec();
    for (a, n) in &funcs {
        b.extend(a.to_le_bytes());
        b.extend((*n as u32).to_le_bytes());
    }
    std::fs::write(path, b).map_err(at(path))?;
    Ok(funcs.len())
}

/// il2cpp_api_check: the il2cpp_* functions the player's libunity.so looks up that libil2cpp.so
/// does not export, and how many it looks up.
pub fn il2cpp_api_check(libunity: &str, libil2cpp: &str) -> Result<(Vec<String>, usize)> {
    let d = util::read(libunity)?;
    let rx = regex::bytes::Regex::new(r"\x00(il2cpp_[a-z0-9_]+)\x00").unwrap();
    let mut wanted: Vec<String> = rx
        .captures_iter(&d)
        .map(|c| String::from_utf8_lossy(&c[1]).into_owned())
        .collect();
    wanted.sort();
    wanted.dedup();
    let have = elf_exports(libil2cpp)?;
    let n = wanted.len();
    Ok((
        wanted.into_iter().filter(|w| !have.contains(w)).collect(),
        n,
    ))
}

/// metadata_version: global-metadata.dat's version.
pub fn metadata_version(path: &str) -> Result<i32> {
    let d = crate::fetch::local(path, 0, 8)?;
    let magic = u32le(&d, 0)?;
    let ver = u32le(&d, 4)? as i32;
    if magic != METADATA_MAGIC {
        return fail(format!(
            "ValueError: {path}: not IL2CPP metadata (magic {magic:#x})"
        ));
    }
    Ok(ver)
}

// ------------------------------------------------------------------ command lines

/// RUNTIMECONFIG: the .NET runtime options the tools run with.
pub fn runtimeconfig() -> Json {
    Json::Obj(vec![(
        "runtimeOptions".into(),
        Json::Obj(vec![
            ("tfm".into(), Json::str("net8.0")),
            ("rollForward".into(), Json::str("Major")),
            (
                "framework".into(),
                Json::Obj(vec![
                    ("name".into(), Json::str("Microsoft.NETCore.App")),
                    ("version".into(), Json::str("6.0.0")),
                ]),
            ),
            (
                "configProperties".into(),
                Json::Obj(vec![
                    ("System.Globalization.Invariant".into(), Json::Bool(true)),
                    (
                        "System.Runtime.Serialization.EnableUnsafeBinaryFormatterSerialization"
                            .into(),
                        Json::Bool(true),
                    ),
                    ("System.GC.Concurrent".into(), Json::Bool(false)),
                ]),
            ),
        ]),
    )])
}

/// deploy_tool: a .NET program of Editor/Data/il2cpp/build.
pub fn deploy_tool(editor: &str, name: &str) -> Result<String> {
    let b = joins(&[editor, "il2cpp", "build"]);
    for rel in [format!("deploy/net471/{name}.exe"), format!("{name}.exe")] {
        let p = join(&b, &rel);
        if util::isfile(&p) {
            return Ok(p);
        }
    }
    let d = join(&b, "deploy");
    if util::isdir(&d) {
        let mut subs = util::listdir(&d)?;
        subs.sort();
        for sub in subs {
            let p = joins(&[&d, &sub, &format!("{name}.dll")]);
            if util::isfile(&p) {
                return Ok(p);
            }
        }
    }
    let p = join(&d, &format!("{name}.dll"));
    if util::isfile(&p) {
        return Ok(p);
    }
    if util::isfile(&join(&d, &format!("{name}.exe"))) {
        return exit(format!(
            "{editor}: this Unity's {name} is a native Windows program ({name}.exe without a .NET {name}.dll); the IL2CPP route runs Unity 2017.4 - 2022.3's .NET il2cpp (docs/il2cpp.md)"
        ));
    }
    exit(format!("{editor}: no {name} program under il2cpp/build"))
}

/// has_option: a Unity .NET tool has an option field of that name.
pub fn has_option(tool: &str, field: &str) -> Result<bool> {
    let d = util::read(tool)?;
    let mut pat = vec![0u8];
    pat.extend(field.as_bytes());
    pat.push(0);
    Ok(pkgzip::py::find(&d, &pat).is_some())
}

#[allow(clippy::too_many_arguments)]
pub fn il2cpp_args(
    version: &Version,
    abi: &str,
    managed: &str,
    cpp: &str,
    cache: &str,
    so: &str,
    ndk: &str,
    settings: (bool, Option<i64>),
    pkg: &str,
    profile: &str,
) -> Vec<String> {
    let arch = abi_info(abi).0;
    let mut a: Vec<String> = [
        "--convert-to-cpp",
        "--compile-cpp",
        "--libil2cpp-static",
        "--emit-null-checks",
        "--enable-array-bounds-check",
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    a.push(format!("--dotnetprofile={profile}"));
    a.push("--platform=Android".into());
    a.push(format!("--architecture={arch}"));
    a.push("--configuration=Release".into());
    a.push(format!("--outputpath={so}"));
    a.push(format!("--cachedirectory={cache}"));
    a.push(format!("--generatedcppdir={cpp}"));
    a.push(format!("--directory={managed}"));
    a.push(format!("--tool-chain-path={ndk}"));
    a.push("--verbose".into());
    if version.line() < MAP_FILE_UNTIL {
        a.push("--map-file-parser=/bin/true".into());
    }
    if settings.0 && version.line() >= (2019, 1) {
        a.push(format!(
            "--incremental-g-c-time-slice={}",
            settings.1.unwrap_or(3)
        ));
    }
    let baselib = joins(&[pkg, "Variations", "il2cpp", "Release", "StaticLibs", abi]);
    if util::isfile(&join(&baselib, "baselib.a")) {
        a.push(format!("--baselib-directory={baselib}"));
    }
    a
}

pub fn link_args(
    abi: &str,
    staged: &str,
    linked: &str,
    roots: &[String],
    profile: &str,
) -> Vec<String> {
    let mut a = vec![
        format!("--out={linked}"),
        format!("--search-directory={staged}"),
        format!("--include-directory={staged}"),
        "--dotnetruntime=il2cpp".into(),
        format!("--dotnetprofile={profile}"),
        "--use-editor-options".into(),
        "--rule-set=Conservative".into(),
        "--platform=Android".into(),
        format!("--architecture={}", abi_info(abi).0),
    ];
    let mut r = roots.to_vec();
    r.sort();
    a.extend(
        r.iter()
            .map(|n| format!("--include-unity-root-assembly={}", join(staged, n))),
    );
    a
}

#[allow(clippy::too_many_arguments)]
pub fn link_args_2017(
    net: u8,
    staged: &str,
    linked: &str,
    roots: &[String],
    descriptors: &str,
    subset: bool,
    keep_xml: Option<&str>,
) -> Vec<String> {
    let api = if net == 4 {
        "NET_4_6"
    } else if subset {
        "NET_2_0_Subset"
    } else {
        "NET_2_0"
    };
    let mut a = vec![
        format!("--api={api}"),
        format!("--out={linked}"),
        "--i18n=none".into(),
        "--core-action=link".into(),
        format!("--descriptor-directory={descriptors}"),
        format!("--search-directory={staged}"),
    ];
    if let Some(k) = keep_xml {
        a.push(format!("--include-link-xml={k}"));
    }
    let mut r = roots.to_vec();
    r.sort();
    a.extend(
        r.iter()
            .map(|n| format!("--include-assembly={}", join(staged, n))),
    );
    a
}

/// xml.sax.saxutils.quoteattr
pub fn quoteattr(v: &str) -> String {
    let mut s = v
        .replace('&', "&amp;")
        .replace('>', "&gt;")
        .replace('<', "&lt;");
    s = s
        .replace('\n', "&#10;")
        .replace('\r', "&#13;")
        .replace('\t', "&#9;");
    if s.contains('"') {
        if s.contains('\'') {
            format!("\"{}\"", s.replace('"', "&quot;"))
        } else {
            format!("'{s}'")
        }
    } else {
        format!("\"{s}\"")
    }
}

/// engine_keep_xml: a link.xml keeping the named assemblies whole.
pub fn engine_keep_xml(names: &[String], path: &str) -> Result<String> {
    let mut n = names.to_vec();
    n.sort();
    let mut t = String::from("<linker>\n");
    for x in n {
        t.push_str(&format!(
            "  <assembly fullname={} preserve=\"all\"/>\n",
            quoteattr(&x[..x.len().saturating_sub(4)])
        ));
    }
    t.push_str("</linker>\n");
    util::write_text(path, &t)?;
    Ok(path.to_string())
}

/// linker_descriptors: the folder the 2017.x UnityLinker reads its descriptors from.
pub fn linker_descriptors(editor: &str, profile: &str, work: &str) -> Result<String> {
    let src = joins(&[editor, "il2cpp", "LinkerDescriptors"]);
    if profile != "net45" {
        return Ok(src);
    }
    let dst = join(work, "LinkerDescriptors");
    util::rmtree(&dst);
    util::makedirs(&dst)?;
    let mut names = util::listdir(&src)?;
    names.sort();
    for n in &names {
        if !n.ends_with(".xml")
            || n.ends_with("_mono.xml")
            || (!n.ends_with("45.xml") && names.contains(&format!("{}45.xml", &n[..n.len() - 4])))
        {
            continue;
        }
        let to = if n.ends_with("45.xml") {
            format!("{}.xml", &n[..n.len() - 6])
        } else {
            n.clone()
        };
        util::copyfile(&join(&src, n), &join(&dst, &to))?;
    }
    Ok(dst)
}

/// A program run with its output captured (subprocess.run(capture_output=True, text=True)).
fn run_captured(cmd: &[String]) -> Result<(i32, String, String)> {
    if cmd.is_empty() {
        return fail("IndexError: list index out of range");
    }
    let o = std::process::Command::new(&cmd[0])
        .args(&cmd[1..])
        .stdin(std::process::Stdio::inherit())
        .output()
        .map_err(|e| {
            Error::Fail(match e.raw_os_error() {
                Some(n) => format!(
                    "[Errno {n}] {}: {}",
                    util::strerror(n),
                    pkgzip::py::str_repr(&cmd[0])
                ),
                None => e.to_string(),
            })
        })?;
    use std::os::unix::process::ExitStatusExt;
    let code = o
        .status
        .code()
        .unwrap_or_else(|| -o.status.signal().unwrap_or(0));
    let t = |b: &[u8]| {
        String::from_utf8_lossy(b)
            .replace("\r\n", "\n")
            .replace('\r', "\n")
    };
    Ok((code, t(&o.stdout), t(&o.stderr)))
}

/// run_asmpatch: tools/asmpatch on the game's Managed folder; the assemblies it changed.
pub fn run_asmpatch(command: &str, managed: &str, work: &str) -> Result<Vec<String>> {
    let dst = join(work, "asmpatch");
    util::rmtree(&dst);
    util::makedirs(work)?;
    let mut cmd = util::shlex_split(command)?;
    cmd.push(managed.into());
    cmd.push(dst.clone());
    let (rc, out, err) = run_captured(&cmd)?;
    for line in out.lines() {
        log(&format!("asmpatch: {line}"));
    }
    if rc != 0 {
        return exit(format!("asmpatch failed (exit {rc}):\n{out}{err}"));
    }
    if !util::isdir(&dst) {
        return Ok(Vec::new());
    }
    let mut n = util::listdir(&dst)?;
    n.sort();
    Ok(n.into_iter()
        .filter(|x| x.ends_with(".dll"))
        .map(|x| join(&dst, &x))
        .collect())
}

/// run_assembly_patches: a port's assembly patches ([(name, command)]).
pub fn run_assembly_patches(
    patches: &[(String, String)],
    managed: &str,
    work: &str,
) -> Result<Vec<String>> {
    let dst = join(work, "assembly-patch");
    util::rmtree(&dst);
    util::makedirs(&dst)?;
    let mut out = Vec::new();
    for (name, command) in patches {
        let tmp = join(work, "assembly-patch.tmp");
        util::rmtree(&tmp);
        util::makedirs(&tmp)?;
        let cmd: Vec<String> = util::shlex_split(command)?
            .into_iter()
            .map(|w| w.replace("{managed}", managed).replace("{out}", &tmp))
            .collect();
        let (rc, so, se) = run_captured(&cmd)?;
        for line in so.lines() {
            log(&format!("assembly patch {name}: {line}"));
        }
        if rc != 0 || !util::isfile(&join(&tmp, name)) {
            return exit(format!(
                "assembly patch for {name} failed (exit {rc}{}):\n{so}{se}",
                if rc != 0 {
                    String::new()
                } else {
                    format!(", no {name} written")
                }
            ));
        }
        util::replace(&join(&tmp, name), &join(&dst, name))?;
        util::rmtree(&tmp);
        out.push(join(&dst, name));
    }
    Ok(out)
}

/// folder_patches: asmpatch's opt-in patches for StreamingAssets and the data folder.
pub fn folder_patches(streaming_assets: &str, data_path: bool) -> Result<Vec<String>> {
    if !["obb", "both", "loose"].contains(&streaming_assets) {
        return exit(format!(
            "--streaming-assets {}: one of obb, both, loose",
            pkgzip::py::str_repr(streaming_assets)
        ));
    }
    let mut v: Vec<String> = androiddata::streaming::patch_of(streaming_assets)
        .map(|p| vec![p.to_string()])
        .unwrap_or_default();
    if data_path {
        v.push("data-path".into());
    }
    Ok(v)
}

fn strs(v: &[String]) -> Json {
    Json::List(v.iter().map(Json::str).collect())
}

fn sorted(mut v: Vec<String>) -> Vec<String> {
    v.sort();
    v
}

/// build: the IL2CPP build (natively, `on_device`); il2cpp.json's contents, None for a dry run.
pub fn build(o: &BuildOpts) -> Result<Option<Json>> {
    if !o.on_device {
        return Err(Error::NotPorted(
            "pmu il2cpp: only the build on the handheld (--on-device) is ported; the Docker route stays with `python -m pmunity il2cpp`".into(),
        ));
    }
    let with_patches = folder_patches(&o.streaming_assets, o.data_path)?;
    if !with_patches.is_empty() && o.asmpatch.is_none() {
        return exit(format!(
            "--streaming-assets {}{}: these need --asmpatch (tools/asmpatch's command): its {} patch is what gives the game's code the folder, compiled into libil2cpp.so",
            o.streaming_assets,
            if o.data_path { " --data-path" } else { "" },
            with_patches.join(", ")
        ));
    }
    let data_version = match &o.data_version {
        Some(v) => v.clone(),
        None => UV::of_data(&o.data)?,
    };
    let data_version = UV::check(data_version, false)?;
    let version = tools::tool_version(&data_version, &o.abi, o.unity.as_deref())?;
    let home = util::abspath(&o.home.clone().unwrap_or_else(default_home));
    let out = util::abspath(&o.out);
    let data = util::abspath(&o.data);
    let net = dotnet_level(&data, Some(&data_version))?;
    let subset = net == 2 && subset_profile(&data)?;
    let (profile, bcl) = bcl_profile(&version, net, subset)?;
    log(&format!(
        "data: Unity {data_version}, .NET {}; IL2CPP of {version} for {} (profile {profile}, {bcl})",
        if net == 2 { "3.5" } else { "4.x" },
        o.abi
    ));
    let pkg = o
        .pkg
        .as_deref()
        .filter(|p| !p.is_empty())
        .map(util::abspath);
    let tc = tools::toolchain(&version, &o.abi, &home, pkg.as_deref(), bcl)?;
    if let Some(api) = tc.android_api {
        // (the handheld's commands see it: ndk-clang.sh)
        std::env::set_var("PMUNITY_ANDROID_API", api.to_string());
    }
    let work = join(&out, ".work");
    let staged = join(&work, "Managed-in");
    let linked = join(&work, "Managed");
    let mut managed_files = o.managed_files.clone();
    let mut given: Vec<String> = managed_files
        .iter()
        .map(|f| util::basename(f).to_string())
        .collect();
    given.sort();
    given.dedup();
    let mut patches: Vec<(String, Json)> = vec![("managed_file".into(), strs(&given))];
    if !o.assembly_patches.is_empty() {
        let names: Vec<String> = o.assembly_patches.iter().map(|x| x.0.clone()).collect();
        let mut clash: Vec<String> = names
            .iter()
            .filter(|n| names.iter().filter(|m| m == n).count() > 1 || given.contains(n))
            .cloned()
            .collect();
        clash.sort();
        clash.dedup();
        if !clash.is_empty() {
            return exit(format!(
                "--assembly-patch {}: given twice, or also by --managed-file",
                clash.join(", ")
            ));
        }
        managed_files.extend(run_assembly_patches(
            &o.assembly_patches,
            &join(&data, "Managed"),
            &work,
        )?);
        given.extend(names.iter().cloned());
        patches.push(("assembly_patch".into(), strs(&sorted(names))));
    }
    if let Some(a) = &o.asmpatch {
        let mut cmd = vec![a.clone()];
        cmd.extend(with_patches.iter().map(|p| format!("--with={p}")));
        let patched = run_asmpatch(&cmd.join(" "), &join(&data, "Managed"), &work)?;
        let mut clash: Vec<String> = patched
            .iter()
            .map(|f| util::basename(f).to_string())
            .filter(|n| given.contains(n))
            .collect();
        clash.sort();
        clash.dedup();
        if !clash.is_empty() {
            return exit(format!(
                "asmpatch changed {}, which --managed-file or --assembly-patch also gives: run asmpatch on that assembly when making it instead",
                clash.join(", ")
            ));
        }
        let names = sorted(
            patched
                .iter()
                .map(|f| util::basename(f).to_string())
                .collect(),
        );
        managed_files.extend(patched);
        patches.push(("asmpatch".into(), strs(&names)));
        patches.push(("asmpatch_with".into(), strs(&with_patches)));
    }
    patches.push(("streaming_assets".into(), Json::str(&o.streaming_assets)));
    patches.push(("data_path".into(), Json::Bool(o.data_path)));
    let listed = match &o.listed {
        Some(l) => l.clone(),
        None => listed_assemblies(&data),
    };
    let chosen = stage_assemblies(&data, &tc, &staged, &managed_files, &listed)?;
    let settings = player_settings(&data);
    let cpp = join(&work, "cpp");
    let cache = join(&work, "cache");
    let so = joins(&[&out, "lib", &o.abi, "libil2cpp.so"]);
    util::makedirs(util::dirname(&so))?;
    let rc = join(&work, "tools.runtimeconfig.json");
    util::write_text(&rc, &pyjson::dumps(&runtimeconfig(), None))?;
    let runner = |tool: &str, net471: &str| -> Vec<String> {
        if tool.ends_with(".exe") && net471 == "mono" {
            vec!["mono".into(), "--gc=sgen".into(), tool.into()]
        } else {
            vec![
                "dotnet".into(),
                "exec".into(),
                "--runtimeconfig".into(),
                rc.clone(),
                tool.into(),
            ]
        }
    };
    let device_tc = util::abspath(
        &o.device_tc
            .clone()
            .unwrap_or_else(|| device::default_toolchain(&home)),
    );
    let mut nb = Native::new(&device_tc, o.jobs, &work);
    if util::exists(&join(&work, "cc.log")) {
        util::unlink(&join(&work, "cc.log"))?;
    }
    let bcl_root = format!("{}/", join(&tc.editor, bcl));
    let roots: Vec<String> = chosen
        .items
        .iter()
        .filter(|(_, src)| !src.starts_with(&bcl_root))
        .map(|(n, _)| n.clone())
        .collect();
    let linker = deploy_tool(&tc.editor, "UnityLinker")?;
    let mut largs = if has_option(&linker, "IncludeUnityRootAssembly")? {
        let mut a = link_args(&o.abi, &staged, &linked, &roots, profile);
        if has_option(&linker, "AllowedAssembly")? {
            let mut n = util::listdir(&staged)?;
            n.sort();
            a.extend(
                n.into_iter()
                    .filter(|x| x.ends_with(".dll"))
                    .map(|x| format!("--allowed-assembly={}", join(&staged, &x))),
            );
        }
        a
    } else {
        let engine: Vec<String> = roots.iter().filter(|n| is_engine(n)).cloned().collect();
        util::makedirs(&work)?;
        let others: Vec<String> = roots
            .iter()
            .filter(|n| !engine.contains(n))
            .cloned()
            .collect();
        let desc = linker_descriptors(&tc.editor, profile, &work)?;
        let keep = engine_keep_xml(&engine, &join(&work, "engine-link.xml"))?;
        link_args_2017(net, &staged, &linked, &others, &desc, subset, Some(&keep))
    };
    for (i, x) in o.link_xml.iter().enumerate() {
        util::makedirs(&work)?;
        let dst = join(&work, &format!("port-link-{i}.xml"));
        util::copyfile(x, &dst)?;
        largs.push(format!("--include-link-xml={dst}"));
    }
    let mut lcmd = runner(&linker, "mono");
    lcmd.extend(largs);
    let tool = deploy_tool(&tc.il2cpp_editor, "il2cpp")?;
    let net471 = std::env::var("PMUNITY_IL2CPP_NET471").unwrap_or_else(|_| "dotnet".into());
    let mut icmd = runner(&tool, &net471);
    icmd.extend(il2cpp_args(
        &version, &o.abi, &linked, &cpp, &cache, &so, &tc.ndk, settings, &tc.pkg, profile,
    ));
    if has_option(&tool, "Jobs")? {
        icmd.push(format!("--jobs={}", nb.jobs));
    }
    if let Ok(x) = std::env::var("PMUNITY_IL2CPP_EXTRA_ARGS") {
        icmd.extend(x.split_whitespace().map(str::to_string));
    }
    if o.strip == "none" {
        let from = format!("--directory={linked}");
        let to = format!("--directory={staged}");
        icmd = icmd.into_iter().map(|w| w.replace(&from, &to)).collect();
    }
    if o.strip != "none" {
        log(&format!("UnityLinker: {}", lcmd.join(" ")));
    } else {
        log("UnityLinker: not run (--strip none)");
    }
    log(&format!("il2cpp: {}", icmd.join(" ")));
    if o.dry {
        return Ok(None);
    }
    let tools_ = vec![deploy_tool(&tc.editor, "UnityLinker")?, tool.clone()];
    device::fetch(&device_tc, &device::groups_for(&tools_, &net471))?;
    let secs = native::run_steps(
        &mut nb,
        &lcmd,
        &icmd,
        &work,
        &staged,
        &linked,
        &cpp,
        &cache,
        &tc.il2cpp_editor,
        &o.strip,
    )?;
    let mut more = vec![("on_device".to_string(), native::report(&nb))];
    more.extend(patches);
    let info = finish(&Finish {
        out: &out,
        work: &work,
        cpp: &cpp,
        so: &so,
        version: &version,
        abi: &o.abi,
        tc: &tc,
        chosen: &chosen,
        settings,
        keep_cpp: o.keep_cpp,
        secs,
        strip: &o.strip,
        data_version: &data_version,
        profile,
        more,
    })?;
    Ok(Some(info))
}

/// finish's arguments.
pub struct Finish<'a> {
    pub out: &'a str,
    pub work: &'a str,
    pub cpp: &'a str,
    pub so: &'a str,
    pub version: &'a Version,
    pub abi: &'a str,
    pub tc: &'a Toolchain,
    pub chosen: &'a Ordered,
    pub settings: (bool, Option<i64>),
    pub keep_cpp: bool,
    pub secs: f64,
    pub strip: &'a str,
    pub data_version: &'a Version,
    pub profile: &'a str,
    pub more: Vec<(String, Json)>,
}

/// finish: the data folder il2cpp wrote -> <out>/Data/Managed; il2cpp.json.
pub fn finish(f: &Finish) -> Result<Json> {
    let sodir = util::dirname(f.so).to_string();
    let cands = [
        join(f.cpp, "Data"),
        join(&sodir, "Data"),
        join(f.work, "Data"),
    ];
    let Some(src) = cands
        .iter()
        .find(|c| util::isfile(&joins(&[c, "Metadata", "global-metadata.dat"])))
        .cloned()
    else {
        return exit(format!(
            "il2cpp wrote no Metadata/global-metadata.dat (looked in {}/Data)",
            f.cpp
        ));
    };
    let dst = joins(&[f.out, "Data", "Managed"]);
    if util::isdir(&dst) {
        std::fs::remove_dir_all(&dst).map_err(at(&dst))?;
    }
    util::makedirs(util::dirname(&dst))?;
    if src == join(&sodir, "Data") {
        util::replace(&src, &dst)?;
    } else {
        util::copytree(&src, &dst)?;
    }
    util::rmtree(&join(&sodir, "Data"));
    let mv = metadata_version(&joins(&[&dst, "Metadata", "global-metadata.dat"]))?;
    let libunity = joins(&[
        &f.tc.pkg,
        "Variations",
        "il2cpp",
        "Release",
        "Libs",
        f.abi,
        "libunity.so",
    ]);
    let (missing, nwanted) = if util::isfile(&libunity) {
        il2cpp_api_check(&libunity, f.so)?
    } else {
        (Vec::new(), 0)
    };
    if !missing.is_empty() {
        let first: Vec<String> = missing
            .iter()
            .take(8)
            .map(|m| pkgzip::py::str_repr(m))
            .collect();
        log(&format!(
            "WARNING: the player's libunity.so looks up {} il2cpp_* functions libil2cpp.so lacks: [{}]",
            missing.len(),
            first.join(", ")
        ));
    }
    let syms: Vec<String> = ["libil2cpp.so.debug", "libil2cpp.sym"]
        .iter()
        .map(|n| join(&sodir, n))
        .filter(|p| util::isfile(p))
        .collect();
    let arch = abi_info(f.abi).0;
    if f.version.line() < MAP_FILE_UNTIL && !syms.is_empty() {
        let n = write_symbol_map(
            &syms[0],
            &join(&dst, &format!("SymbolMap-{arch}")),
            f.abi == "armeabi-v7a",
        )?;
        log(&format!("SymbolMap-{arch}: {n} code symbols"));
    }
    for extra in [
        "libil2cpp.dbg.so",
        "libil2cpp.so.debug",
        "libil2cpp.map",
        "libil2cpp.sym.so",
        "libil2cpp.sym",
    ] {
        let p = join(&sodir, extra);
        if util::exists(&p) {
            if extra.ends_with(".dbg.so") || extra.ends_with(".debug") || extra.ends_with(".map") {
                util::unlink(&p)?;
            } else {
                util::replace(&p, &join(f.work, extra))?;
            }
        }
    }
    let mut files: Vec<String> = Vec::new();
    for w in util::walk(&join(f.out, "Data"), false) {
        for n in &w.files {
            files.push(util::relpath(&join(&w.root, n), f.out));
        }
    }
    files.sort();
    let mut assemblies: Vec<String> = f.chosen.items.iter().map(|x| x.0.clone()).collect();
    assemblies.sort();
    let mut info: Vec<(String, Json)> = vec![
        ("unity".into(), Json::str(&f.version.text)),
        ("data_unity".into(), Json::str(&f.data_version.text)),
        ("abi".into(), Json::str(f.abi)),
        ("metadata_version".into(), Json::Int(mv as i128)),
        ("dotnet_profile".into(), Json::str(f.profile)),
        (
            "libil2cpp_bytes".into(),
            Json::Int(util::getsize(f.so)? as i128),
        ),
        (
            "player_api".into(),
            Json::Obj(vec![
                ("looked_up".into(), Json::Int(nwanted as i128)),
                ("missing".into(), strs(&missing)),
            ]),
        ),
        ("ndk".into(), Json::str(&f.tc.ndk_name)),
        (
            "player".into(),
            f.tc.player.get("version").cloned().unwrap_or(Json::Null),
        ),
    ];
    if let Some(c) = &f.tc.converter {
        info.push(("il2cpp_converter".into(), Json::str(c)));
    }
    if let Some(a) = f.tc.android_api {
        info.push(("android_api".into(), Json::Int(a as i128)));
    }
    info.push(("assemblies".into(), strs(&assemblies)));
    info.push(("gc_incremental".into(), Json::Bool(f.settings.0)));
    info.push(("strip".into(), Json::str(f.strip)));
    info.push((
        "build_seconds".into(),
        Json::Int(util::py_round(f.secs) as i128),
    ));
    for (k, v) in &f.more {
        match info.iter_mut().find(|x| x.0 == *k) {
            Some(x) => x.1 = v.clone(),
            None => info.push((k.clone(), v.clone())),
        }
    }
    info.push(("files".into(), strs(&files)));
    let info = Json::Obj(info);
    util::write_text(&join(f.out, "il2cpp.json"), &pyjson::dumps(&info, Some(1)))?;
    if !f.keep_cpp {
        util::rmtree(f.cpp);
        util::rmtree(&join(f.work, "cache"));
    }
    util::rmtree(&join(f.work, "tmp"));
    log(&format!(
        "il2cpp: {} ({:.1} MiB), metadata version {mv}, {} data files -> {dst}",
        f.so,
        util::getsize(f.so)? as f64 / (1u64 << 20) as f64,
        files.len()
    ));
    Ok(info)
}

/// `pmunity il2cpp fetch`: the toolchain (and the handheld's tools); the dict it prints.
pub fn fetch_only(
    version: &str,
    abi: &str,
    home: Option<&str>,
    pkg: Option<&str>,
    net: &str,
    on_device: bool,
    toolchain_dir: Option<&str>,
) -> Result<Json> {
    let home = util::abspath(&home.map(str::to_string).unwrap_or_else(default_home));
    if on_device {
        device::install_ca();
    }
    let v = Version::parse(version).map_err(|e| Error::Fail(format!("ValueError: {e}")))?;
    let n: u8 = if net.starts_with('2') { 2 } else { 4 };
    let bcl = bcl_profile(&v, n, net == "2subset")?.1;
    let tc = tools::toolchain(&v, abi, &home, pkg, bcl)?;
    let mut dtc = None;
    if on_device {
        let t = vec![
            deploy_tool(&tc.editor, "UnityLinker")?,
            deploy_tool(&tc.il2cpp_editor, "il2cpp")?,
        ];
        let net471 = std::env::var("PMUNITY_IL2CPP_NET471").unwrap_or_else(|_| "dotnet".into());
        let dir = toolchain_dir
            .map(str::to_string)
            .unwrap_or_else(|| device::default_toolchain(&home));
        dtc = Some(device::fetch(&dir, &device::groups_for(&t, &net471))?);
    }
    let _ = mix::NATIVE_FROM;
    Ok(tc.json(dtc.as_deref()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn engine_names() {
        assert!(is_engine("UnityEngine.dll"));
        assert!(is_engine("UnityEngine.CoreModule.dll"));
        assert!(!is_engine("UnityEngine.UI.dll"));
        assert!(!is_engine("UnityEngine.Module.dll"));
        assert!(!is_engine("Assembly-CSharp.dll"));
    }

    #[test]
    fn link_xml_and_args() {
        assert_eq!(quoteattr("UnityEngine"), "\"UnityEngine\"");
        assert_eq!(quoteattr("a\"b"), "'a\"b'");
        assert_eq!(quoteattr("a\"b'c&"), "\"a&quot;b'c&amp;\"");
        let v = Version::parse("2017.4.40f1").unwrap();
        let a = il2cpp_args(
            &v,
            "arm64-v8a",
            "/w/M",
            "/w/cpp",
            "/w/c",
            "/o/l.so",
            "/ndk",
            (true, Some(5)),
            "/nonexistent",
            "net20",
        );
        assert!(a.contains(&"--map-file-parser=/bin/true".to_string()));
        assert!(!a.iter().any(|x| x.starts_with("--incremental")));
        let v = Version::parse("2020.3.49f1").unwrap();
        let a = il2cpp_args(
            &v,
            "arm64-v8a",
            "/w/M",
            "/w/cpp",
            "/w/c",
            "/o/l.so",
            "/ndk",
            (true, Some(5)),
            "/nonexistent",
            "unityaot",
        );
        assert!(a.contains(&"--incremental-g-c-time-slice=5".to_string()));
        assert_eq!(a[7], "--architecture=ARM64");
        assert_eq!(
            pyjson::dumps(&runtimeconfig(), None),
            r#"{"runtimeOptions": {"tfm": "net8.0", "rollForward": "Major", "framework": {"name": "Microsoft.NETCore.App", "version": "6.0.0"}, "configProperties": {"System.Globalization.Invariant": true, "System.Runtime.Serialization.EnableUnsafeBinaryFormatterSerialization": true, "System.GC.Concurrent": false}}}"#
        );
        let l = link_args_2017(
            2,
            "/s",
            "/l",
            &["b.dll".into(), "a.dll".into()],
            "/d",
            true,
            Some("/k.xml"),
        );
        assert_eq!(l[0], "--api=NET_2_0_Subset");
        assert_eq!(l[7], "--include-assembly=/s/a.dll");
    }

    #[test]
    fn settings_and_profiles() {
        let d = std::env::temp_dir().join(format!("il2cpp-bc-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        std::fs::write(
            d.join("boot.config"),
            "gfx-enable-gfx-jobs=1\ngc-max-time-slice=3\n",
        )
        .unwrap();
        assert_eq!(player_settings(d.to_str().unwrap()), (true, Some(3)));
        std::fs::write(d.join("boot.config"), "gc-max-time-slice=0\n").unwrap();
        assert_eq!(player_settings(d.to_str().unwrap()), (false, None));
        let _ = std::fs::remove_dir_all(&d);
        let v = Version::parse("2018.4.36f1").unwrap();
        assert_eq!(bcl_profile(&v, 4, false).unwrap(), ("unityaot", BCL_DIR));
        assert_eq!(
            bcl_profile(&v, 2, true).unwrap(),
            ("legacyunity", "Mono/lib/mono/unity")
        );
        assert!(bcl_profile(&Version::parse("2019.1.1f1").unwrap(), 2, false).is_err());
        assert_eq!(
            bcl_profile(&Version::parse("2021.3.16f1").unwrap(), 4, false)
                .unwrap()
                .0,
            "unityaot-linux"
        );
        assert_eq!(
            bcl_profile(&Version::parse("2017.4.40f1").unwrap(), 4, false)
                .unwrap()
                .0,
            "net45"
        );
    }

    /// _AsmRefs against androiddata's .NET reader, on every assembly of the test data's game
    /// ($PMU_TEST_DATA: Cuphead's Managed folder).
    #[test]
    fn assembly_refs_as_clrmeta_reads_them() {
        let Ok(td) = std::env::var("PMU_TEST_DATA") else {
            return;
        };
        let m = format!("{td}/Cuphead.app/Contents/Resources/Data/Managed");
        let Ok(names) = util::listdir(&m) else { return };
        let mut n = 0;
        for x in names.iter().filter(|x| x.ends_with(".dll")) {
            let p = join(&m, x);
            let a = androiddata::clrmeta::Assembly::open(&p).unwrap();
            assert_eq!(asm_refs(&p).unwrap(), a.assembly_refs().unwrap(), "{x}");
            n += 1;
        }
        assert!(n > 10);
    }
}
