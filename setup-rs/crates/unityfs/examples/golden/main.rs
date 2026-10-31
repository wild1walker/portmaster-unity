//! The Rust side of unityfs's golden test (setup-rs/tests/golden/unityfs.sh): the same JSON line a
//! file as tests/golden/unityfs_golden.py writes with the Python, from this crate.
//!
//!     golden <data dir> <scratch dir> [--only TEXT] [--no-write] [--no-patch] [--no-objects]
//!
//! The class database comes from $PMU_TPK (UnityPy's resources/lzma.tpk).
mod sha256;

use sha256::Sha256;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use unityfs::bundle::{Bundle, Header};
use unityfs::serialized::looks_serialized;
use unityfs::{compress, platform, ClassDb, DataFile, Options, Result, Source, Value};

/// The canonical form of a value (unityfs_golden.py's canon): numbers by their bits, strings and
/// bytes as bytes, a struct's fields as a dict keeps them (a name twice: its first place, its
/// last value).
fn canon(v: &Value, h: &mut Sha256) {
    match v {
        Value::Bool(b) => h.update(if *b { b"b1" } else { b"b0" }),
        Value::F32(f) => {
            h.update(b"f");
            h.update(&f.to_bits().to_le_bytes());
        }
        Value::F64(f) => {
            h.update(b"d");
            h.update(&f.to_bits().to_le_bytes());
        }
        Value::Str(b) => {
            h.update(format!("s{}:", b.len()).as_bytes());
            h.update(b);
        }
        Value::Bytes(b) => {
            h.update(format!("y{}:", b.len()).as_bytes());
            h.update(b);
        }
        Value::Array(a) => {
            h.update(format!("[{}:", a.len()).as_bytes());
            for x in a {
                canon(x, h);
            }
            h.update(b"]");
        }
        Value::Pair(p) => {
            h.update(b"(");
            canon(&p.0, h);
            canon(&p.1, h);
            h.update(b")");
        }
        Value::Struct(s) => {
            h.update(b"{");
            let mut seen: Vec<&str> = Vec::new();
            for (n, _) in &s.fields {
                if seen.contains(&&**n) {
                    continue;
                }
                seen.push(n);
                h.update(n.as_bytes());
                h.update(b"=");
                canon(s.get(n).unwrap(), h);
                h.update(b",");
            }
            h.update(b"}");
        }
        v => {
            h.update(format!("i{};", v.as_int().unwrap()).as_bytes());
        }
    }
}

fn js(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if (c as u32) < 0x20 || (c as u32) > 0x7e => {
                let mut b = [0u16; 2];
                for u in c.encode_utf16(&mut b) {
                    let _ = write!(o, "\\u{u:04x}");
                }
            }
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

fn sha_file(p: &Path) -> std::io::Result<String> {
    use std::io::Read;
    let mut f = std::fs::File::open(p)?;
    let mut h = Sha256::default();
    let mut b = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut b)?;
        if n == 0 {
            break;
        }
        h.update(&b[..n]);
    }
    Ok(h.hex())
}

fn unity_files(data: &Path) -> Vec<PathBuf> {
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let mut ents: Vec<_> = std::fs::read_dir(d)
            .unwrap()
            .filter_map(|e| e.ok())
            .collect();
        ents.sort_by_key(|e| e.file_name());
        let mut dirs = Vec::new();
        for e in ents {
            let p = e.path();
            let ft = e.file_type().unwrap();
            if ft.is_dir() {
                dirs.push(p);
                continue;
            }
            if ft.is_symlink() {
                continue;
            }
            let n = e.file_name().to_string_lossy().into_owned();
            if [".resS", ".resource", ".config", ".xml", ".dat"]
                .iter()
                .any(|x| n.ends_with(x))
            {
                continue;
            }
            let src = unityfs::FileSource::open(&p).unwrap();
            let first = src.read_clipped(0, 48).unwrap();
            if first.starts_with(b"UnityFS\0") || looks_serialized(&first, src.len()) {
                out.push(p);
            }
        }
        for d in dirs {
            walk(&d, out);
        }
    }
    let mut out = Vec::new();
    walk(data, &mut out);
    out
}

fn opts(db: &Arc<ClassDb>, scratch: &Path) -> Options {
    Options {
        db: Some(db.clone()),
        spill_dir: Some(scratch.join("spill")),
        spill_mb: 64,
        ..Default::default()
    }
}

fn objects(p: &Path, o: &Options, out: &mut String) -> Result<()> {
    let f = DataFile::open(p, o)?;
    let mut classes: Vec<(String, usize)> = Vec::new();
    out.push_str(", \"objects\": [");
    for (k, ob) in f.sf.objects.iter().enumerate() {
        let name = f.class_name(ob);
        match classes.iter_mut().find(|c| c.0 == name) {
            Some(c) => c.1 += 1,
            None => classes.push((name.clone(), 1)),
        }
        let (read, rewrite) = match f.sf.read_typetree(ob.path_id) {
            Ok(v) => {
                let mut h = Sha256::default();
                canon(&v, &mut h);
                let tree = f.sf.typetree(ob)?;
                let b = f.sf.write_value(&tree, &v)?;
                let orig = f.sf.original_bytes(ob.path_id)?;
                (
                    h.hex()[..16].to_string(),
                    if b == orig {
                        "=".to_string()
                    } else {
                        sha256::hex(&b)[..16].to_string()
                    },
                )
            }
            Err(e) => (format!("ERR {e}"), String::new()),
        };
        let _ = write!(
            out,
            "{}[{}, {}, {}, {}, {}]",
            if k > 0 { ", " } else { "" },
            ob.path_id,
            ob.class_id,
            js(&name),
            js(&read),
            if rewrite.is_empty() {
                "null".into()
            } else {
                js(&rewrite)
            }
        );
    }
    out.push_str("], \"classes\": {");
    for (k, (n, c)) in classes.iter().enumerate() {
        let _ = write!(out, "{}{}: {}", if k > 0 { ", " } else { "" }, js(n), c);
    }
    out.push('}');
    Ok(())
}

fn edit_one(f: &mut DataFile) -> Result<Option<i64>> {
    let ids: Vec<i64> = f.sf.objects.iter().map(|o| o.path_id).collect();
    for pid in ids {
        let Ok(mut t) = f.sf.read_typetree(pid) else {
            continue;
        };
        if let Some(Value::Str(s)) = t.get_mut("m_Name") {
            s.push(b'~');
            f.sf.save_typetree(pid, &t)?;
            return Ok(Some(pid));
        }
    }
    Ok(None)
}

fn write(p: &Path, o: &Options, out: &Path, edit: bool, inline: bool) -> Result<String> {
    let mut f = DataFile::open(p, o)?;
    if edit {
        edit_one(&mut f)?;
    }
    f.write_to(out, inline)?;
    drop(f);
    let h = sha_file(out)?;
    std::fs::remove_file(out)?;
    Ok(h)
}

fn blocks(p: &Path) -> Result<String> {
    let b = Bundle::open(p)?;
    let h: &Header = &b.header;
    let (mut d, mut z, mut fz) = (Sha256::default(), Sha256::default(), Sha256::default());
    let mut kinds: Vec<(u16, usize)> = Vec::new();
    for (i, bl) in h.blocks.iter().enumerate() {
        let k = bl.flags & 0x3F;
        match kinds.iter_mut().find(|x| x.0 == k) {
            Some(x) => x.1 += 1,
            None => kinds.push((k, 1)),
        }
        let plain = b.data.block(i)?;
        d.update(&plain);
        z.update(&compress::lz4hc(&plain, 9)?);
        fz.update(&compress::lz4_fast(&plain)?);
    }
    kinds.sort();
    let ks: Vec<String> = kinds
        .iter()
        .map(|(k, n)| format!("{}: {n}", js(&k.to_string())))
        .collect();
    Ok(format!(
        "{{\"plain\": {}, \"hc9\": {}, \"fast\": {}, \"kinds\": {{{}}}}}",
        js(&d.hex()),
        js(&z.hex()),
        js(&fz.hex()),
        ks.join(", ")
    ))
}

fn patch(p: &Path, scratch: &Path) -> Result<String> {
    let cp = scratch.join("patch.copy");
    // (a plain copy: copy_file_range crawls on a shared folder)
    {
        use std::io::{Read, Write};
        let mut i = std::fs::File::open(p)?;
        let mut o = std::fs::File::create(&cp)?;
        let mut b = vec![0u8; 1 << 20];
        loop {
            let n = i.read(&mut b)?;
            if n == 0 {
                break;
            }
            o.write_all(&b[..n])?;
        }
    }
    let res = (|| -> Result<String> {
        let res: Vec<String> = if unityfs::bundle::is_unityfs(p) {
            platform::patch_bundle_platform(&cp, platform::ANDROID)?
                .iter()
                .map(|(n, c, m)| format!("[{}, {c}, {}]", js(n), js(m)))
                .collect()
        } else {
            vec![platform::patch_sf_platform(&cp, platform::ANDROID)?.to_string()]
        };
        Ok(format!(
            "{{\"sha\": {}, \"res\": [{}]}}",
            js(&sha_file(&cp)?),
            res.join(", ")
        ))
    })();
    let _ = std::fs::remove_file(&cp);
    res
}

fn rss_mb() -> u64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| {
            s.lines().find(|l| l.starts_with("VmHWM:")).map(|l| {
                l.split_whitespace()
                    .nth(1)
                    .unwrap_or("0")
                    .parse::<u64>()
                    .unwrap_or(0)
            })
        })
        .unwrap_or(0)
        >> 10
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let flag = |f: &str| args.iter().any(|a| a == f);
    let only = args
        .iter()
        .position(|a| a == "--only")
        .map(|i| args[i + 1].clone());
    let pos: Vec<&String> = args
        .iter()
        .filter(|a| !a.starts_with("--") && Some(*a) != only.as_ref())
        .collect();
    let data = PathBuf::from(pos[0]);
    let scratch = PathBuf::from(pos[1]);
    std::fs::create_dir_all(&scratch).unwrap();
    let db = Arc::new(ClassDb::default_db().expect("class database ($PMU_TPK)"));
    let o = opts(&db, &scratch);
    for p in unity_files(&data) {
        let rel = p
            .strip_prefix(&data)
            .unwrap()
            .to_string_lossy()
            .into_owned();
        if let Some(t) = &only {
            if !rel.contains(t.as_str()) {
                continue;
            }
        }
        let is_bundle = unityfs::bundle::is_unityfs(&p);
        let mut line = format!(
            "{{\"file\": {}, \"kind\": {}",
            js(&rel),
            js(if is_bundle { "bundle" } else { "serialized" })
        );
        let res = (|| -> Result<()> {
            if !flag("--no-objects") {
                objects(&p, &o, &mut line)?;
            }
            if !flag("--no-write") {
                let out = scratch.join("out.bin");
                let _ = write!(
                    line,
                    ", \"save\": {}",
                    js(&write(&p, &o, &out, false, false)?)
                );
                let _ = write!(
                    line,
                    ", \"edit\": {}",
                    js(&write(&p, &o, &out, true, false)?)
                );
                if is_bundle {
                    let _ = write!(
                        line,
                        ", \"inline\": {}",
                        js(&write(&p, &o, &out, false, true)?)
                    );
                }
            }
            if is_bundle {
                let _ = write!(line, ", \"blocks\": {}", blocks(&p)?);
            }
            if !flag("--no-patch") {
                let _ = write!(line, ", \"patch\": {}", patch(&p, &scratch)?);
            }
            Ok(())
        })();
        if let Err(e) = res {
            let _ = write!(line, ", \"error\": {}", js(&e.to_string()));
        }
        let _ = std::fs::remove_dir_all(scratch.join("spill"));
        let _ = write!(line, ", \"rss_mb\": {}}}", rss_mb());
        println!("{line}");
    }
}
