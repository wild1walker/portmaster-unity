//! The names a .NET assembly defines and references (port.py's Assembly, on androiddata's
//! clrmeta): type definitions, type and member references, methods, P/Invoke modules. Read only.
use androiddata::clrmeta::{Assembly, IMPLMAP, METHODDEF, MODULEREF, TYPEDEF, TYPEREF};

pub struct Names {
    /// (namespace, name)
    pub typedefs: Vec<(String, String)>,
    pub typerefs: Vec<(String, String)>,
    pub methods: Vec<String>,
    /// (class namespace, class name, member)
    pub memberrefs: Vec<(String, String, String)>,
    /// (module, entry)
    pub pinvokes: Vec<(String, String)>,
}

const MEMBERREF: u8 = 0x0A;

/// The assembly's names, or why it is not one (the Python's NotAssembly / struct.error text is
/// its own; this one is clrmeta's).
pub fn names(path: &str) -> Result<Names, String> {
    let d = std::fs::read(path).map_err(|e| e.to_string())?;
    // (the Python's own words for what is no assembly at all)
    if d.len() < 0x40 || &d[..2] != b"MZ" {
        return Err("not a PE file".into());
    }
    let pe = u32::from_le_bytes(d[0x3C..0x40].try_into().unwrap()) as usize;
    if d.get(pe..pe + 4) != Some(b"PE\0\0") {
        return Err("no PE signature".into());
    }
    let dd = pe + 24 + if d.get(pe + 24..pe + 26) == Some(&[0x0B, 0x01]) { 96 } else { 112 } + 14 * 8;
    if d.get(dd..dd + 4).is_some_and(|r| r == [0, 0, 0, 0]) {
        return Err("no CLI header (a native library)".into());
    }
    let a = Assembly::from_bytes(path, d).map_err(|e| e.to_string())?;
    let e = |x: androiddata::clrmeta::Error| x.to_string();
    let st = |i: u32| a.string(i).unwrap_or_default();
    let typedefs: Vec<(String, String)> = a.rows(TYPEDEF).map_err(e)?.iter().map(|r| (st(r[2]), st(r[1]))).collect();
    let typerefs: Vec<(String, String)> = a.rows(TYPEREF).map_err(e)?.iter().map(|r| (st(r[2]), st(r[1]))).collect();
    let methods: Vec<String> = a.rows(METHODDEF).map_err(e)?.iter().map(|r| st(r[3])).collect();
    let modulerefs: Vec<String> = a.rows(MODULEREF).map_err(e)?.iter().map(|r| st(r[0])).collect();
    let mut memberrefs = Vec::new();
    for r in a.rows(MEMBERREF).map_err(e)?.iter() {
        let (tag, idx) = (r[0] & 7, (r[0] >> 3) as usize);
        let owner = match tag {
            1 if idx > 0 && idx <= typerefs.len() => typerefs[idx - 1].clone(),
            0 if idx > 0 && idx <= typedefs.len() => typedefs[idx - 1].clone(),
            2 => (
                "<module>".to_string(),
                if idx > 0 && idx <= modulerefs.len() { modulerefs[idx - 1].clone() } else { String::new() },
            ),
            _ => (String::new(), String::new()),
        };
        memberrefs.push((owner.0, owner.1, st(r[1])));
    }
    let pinvokes = a
        .rows(IMPLMAP)
        .map_err(e)?
        .iter()
        .map(|r| {
            let m = r[3] as usize;
            (if m > 0 && m <= modulerefs.len() { modulerefs[m - 1].clone() } else { "?".to_string() }, st(r[2]))
        })
        .collect();
    Ok(Names { typedefs, typerefs, methods, memberrefs, pinvokes })
}

/// Unity's and the class libraries' assemblies (the rest is the game's own code and the libraries
/// it ships): the same rule as tools/asmpatch.
pub fn is_platform_assembly(name: &str) -> bool {
    name == "mscorlib"
        || name == "netstandard"
        || ["System", "Mono.", "UnityEngine.", "Unity.", "Microsoft."].iter().any(|p| name.starts_with(p))
        || name == "UnityEngine"
}
