//! apk.py player_settings: the fields of PlayerSettings, BuildSettings and MonoManager the runtime
//! cares about, read from the build's globalgamemanagers (or data.unity3d, mainData) in the APK or
//! an OBB, as `UnityPy.load(z.read(n))` and `env.objects` read them (unityfs, with UnityPy's class
//! database for the type trees player builds leave out). Anything that fails leaves a note and the
//! fields read before it, as the Python's `except Exception` does.
use std::fs::File;
use std::io;
use std::os::unix::fs::FileExt;
use std::sync::Arc;

use pkgzip::zip::{Archive, STORED};
use unityfs::{ClassDb, MemSource, SerializedFile, SharedSource, Source, Value};

const PLAYER_SETTINGS: i32 = 129;
const BUILD_SETTINGS: i32 = 141;
const MONO_MANAGER: i32 = 116;

/// What was read (each field as `out` has it: missing when not read).
#[derive(Debug, Default, Clone)]
pub struct Settings {
    pub assemblies: Option<Vec<Vec<u8>>>,
    pub graphics_apis: Option<Vec<i128>>,
    pub product_name: Option<Value>,
    pub company_name: Option<Value>,
    pub android_use_swappy: Option<Value>,
    pub default_orientation: Option<Value>,
}

/// Python's truthiness of a value read from a type tree.
pub fn truthy(v: &Value) -> bool {
    match v {
        Value::Str(b) | Value::Bytes(b) => !b.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::F32(f) => *f != 0.0,
        Value::F64(f) => *f != 0.0,
        Value::Struct(_) | Value::Pair(_) => true,
        other => other.as_int().is_some_and(|i| i != 0),
    }
}

/// A stored zip entry read in place.
struct Region {
    f: File,
    off: u64,
    len: u64,
}

impl Source for Region {
    fn len(&self) -> u64 {
        self.len
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> io::Result<()> {
        if off
            .checked_add(buf.len() as u64)
            .is_none_or(|e| e > self.len)
        {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "read past the entry's end",
            ));
        }
        self.f.read_exact_at(buf, self.off + off)
    }
}

/// The entry's bytes as a source (`z.read(n)`: its CRC checked): a stored entry in place, else
/// decompressed into memory, as the Python holds it.
fn entry_source(zip_path: &str, z: &Archive, n: &str) -> Result<SharedSource, String> {
    let e = z.info(n).map_err(|e| e.to_string())?;
    // (read whole once: zipfile's read checks the CRC; for a stored entry the bytes are then
    // read again in place rather than held)
    let mut r = z.reader(e).map_err(|e| e.to_string())?;
    if e.method == STORED {
        io::copy(&mut r, &mut io::sink()).map_err(|e| pkgzip::Error::from(e).to_string())?;
        let off = z.data_offset(e).map_err(|e| e.to_string())?;
        let f = File::open(zip_path).map_err(|e| e.to_string())?;
        return Ok(Arc::new(Region {
            f,
            off,
            len: e.file_size,
        }));
    }
    let mut v = Vec::new();
    io::Read::read_to_end(&mut r, &mut v).map_err(|e| pkgzip::Error::from(e).to_string())?;
    Ok(Arc::new(MemSource(v)))
}

fn class_db() -> Result<Arc<ClassDb>, String> {
    static DB: std::sync::OnceLock<Result<Arc<ClassDb>, String>> = std::sync::OnceLock::new();
    DB.get_or_init(|| {
        ClassDb::default_db()
            .map(Arc::new)
            .map_err(|e| e.to_string())
    })
    .clone()
}

/// The serialized files UnityPy.load gives for the bytes (check_file_type): one, a bundle's, or
/// none (a file UnityPy takes for a resource).
fn files(src: SharedSource, db: Arc<ClassDb>) -> Result<Vec<SerializedFile>, String> {
    let first = src.read_clipped(0, 64).map_err(|e| e.to_string())?;
    if src.len() < 20 {
        return Ok(Vec::new());
    }
    let sig_end = first
        .iter()
        .take(20)
        .position(|&c| c == 0)
        .unwrap_or(20.min(first.len()));
    let sig = &first[..sig_end];
    if sig == b"UnityFS" {
        let b = unityfs::bundle::Bundle::from_source(src).map_err(|e| e.to_string())?;
        let mut out = Vec::new();
        for i in unityfs::data::unitypy_serialized(&b).map_err(|e| e.to_string())? {
            let name = b.header.nodes[i].name.clone();
            out.push(
                SerializedFile::open(&name, b.node(i), Some(db.clone()), Some(&b.header.engine))
                    .map_err(|e| e.to_string())?,
            );
        }
        return Ok(out);
    }
    if [
        &b"UnityWeb"[..],
        b"UnityRaw",
        b"\xfa\xfa\xfa\xfa\xfa\xfa\xfa\xfa",
    ]
    .contains(&sig)
    {
        return Err(format!(
            "a {} bundle (not read here)",
            String::from_utf8_lossy(sig)
        ));
    }
    if !unityfs::serialized::looks_serialized(&first, src.len()) {
        return Ok(Vec::new());
    }
    Ok(vec![
        SerializedFile::open("", src, Some(db), None).map_err(|e| e.to_string())?
    ])
}

/// player_settings(z, n, log).
pub fn read(zip_path: &str, z: &Archive, n: &str, log: &mut dyn FnMut(&str)) -> Settings {
    let mut out = Settings::default();
    if let Err(e) = read_into(zip_path, z, n, &mut out) {
        log(&format!("note: could not read PlayerSettings ({e})"));
    }
    out
}

fn read_into(zip_path: &str, z: &Archive, n: &str, out: &mut Settings) -> Result<(), String> {
    let src = entry_source(zip_path, z, n)?;
    let db = class_db()?;
    for sf in files(src, db)? {
        for o in &sf.objects {
            if ![PLAYER_SETTINGS, BUILD_SETTINGS, MONO_MANAGER].contains(&o.class_id) {
                continue;
            }
            let t = sf.read_typetree(o.path_id).map_err(|e| e.to_string())?;
            match o.class_id {
                MONO_MANAGER => {
                    // (up to 2020.1; then ScriptingAssemblies.json)
                    if let Some(a) = t.get("m_AssemblyNames").and_then(Value::as_array) {
                        if !a.is_empty() {
                            out.assemblies = Some(
                                a.iter()
                                    .filter_map(|x| x.as_bytes())
                                    .filter(|b| !b.is_empty())
                                    .map(<[u8]>::to_vec)
                                    .collect(),
                            );
                        }
                    }
                }
                BUILD_SETTINGS => {
                    out.graphics_apis = Some(
                        t.get("m_GraphicsAPIs")
                            .and_then(Value::as_array)
                            .map(|a| a.iter().filter_map(Value::as_int).collect())
                            .unwrap_or_default(),
                    );
                }
                _ => {
                    for (k, slot) in [
                        ("productName", &mut out.product_name),
                        ("companyName", &mut out.company_name),
                        ("androidUseSwappy", &mut out.android_use_swappy),
                        ("defaultScreenOrientation", &mut out.default_orientation),
                    ] {
                        if let Some(v) = t.get(k) {
                            *slot = Some(v.clone());
                        }
                    }
                }
            }
        }
    }
    Ok(())
}
