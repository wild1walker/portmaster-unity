//! `--macho`: what the Android player's own code asks the game's code for by name, checked
//! against the macOS build. Unity's engine modules (UnityEngine.*.dll) differ a little by
//! platform; the player finds classes and methods of its own modules by name
//! (`[RequiredByNativeCode]` in the player package's `Variations/il2cpp/Managed`), and a macOS
//! build has the macOS modules' code. The members marked so, and not as optional, that the
//! build's metadata lacks are listed here: the runtime's bridge stands in for those it knows
//! (runtime/macho/bridge/bridge.c, ua_standin_space), and the player stops with the name of any
//! other it asks for (docs/macho-route.md). Only modules the build has an image of are compared:
//! a module it has no image of at all is one whose code the player does not reach. A member
//! listed is not always one the macOS modules lack: a build's stripping takes the required
//! members of a module the game does not use as well (the macOS player was built without that
//! module's code and never asks), and the build does not say which it was. So the list is what
//! to look at when the player stops, not a verdict.
use crate::clrmeta::{Assembly, Coded, FIELD, METHODDEF, TYPEDEF, TYPEREF};
use crate::il2cppmeta::Metadata;
use std::collections::BTreeSet;

const MEMBERREF: u8 = 0x0A;
const CUSTOMATTRIBUTE: u8 = 0x0C;
const ATTRIBUTE: &str = "UnityEngine.Scripting.RequiredByNativeCodeAttribute";

/// The bridge's stand-ins: (image, type, method). As bridge.c's ua_standin_space (the test
/// below reads it).
pub const STAND_INS: &[(&str, &str, &str)] = &[
    (
        "UnityEngine.AudioModule.dll",
        "UnityEngine.AudioSettings/Mobile",
        "InvokeIsStopAudioOutputOnMuteEnabled",
    ),
    (
        "UnityEngine.AudioModule.dll",
        "UnityEngine.AudioSettings/Mobile",
        "InvokeOnMuteStateChanged",
    ),
];

/// A type (`name` empty), or a method or field of it.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Member {
    pub image: String,
    pub ty: String,
    pub name: String,
    pub field: bool,
}

impl Member {
    pub fn label(&self) -> String {
        if self.name.is_empty() {
            format!("{}: {}", self.image, self.ty)
        } else {
            format!("{}: {}::{}", self.image, self.ty, self.name)
        }
    }

    fn stood_in(&self) -> bool {
        STAND_INS.iter().any(|(i, t, m)| {
            *i == self.image && *t == self.ty && !self.field && (self.name.is_empty() || *m == self.name)
        })
    }
}

#[derive(Debug, Default)]
pub struct Census {
    /// members marked as required in the modules the build has an image of
    pub required: usize,
    /// modules of the player package the build has an image of
    pub modules: usize,
    pub stood_in: Vec<Member>,
    pub absent: Vec<Member>,
}

// An ECMA-335 compressed unsigned integer: (value, bytes).
fn cuint(b: &[u8], o: usize) -> Option<(usize, usize)> {
    let a = *b.get(o)? as usize;
    if a & 0x80 == 0 {
        Some((a, 1))
    } else if a & 0xC0 == 0x80 {
        Some((((a & 0x3F) << 8) | *b.get(o + 1)? as usize, 2))
    } else {
        Some((
            ((a & 0x1F) << 24) | (*b.get(o + 1)? as usize) << 16 | (*b.get(o + 2)? as usize) << 8 | *b.get(o + 3)? as usize,
            4,
        ))
    }
}

// A SerString: (its text, None for null; bytes).
fn ser_string(b: &[u8], o: usize) -> Option<(Option<String>, usize)> {
    if *b.get(o)? == 0xFF {
        return Some((None, 1));
    }
    let (n, k) = cuint(b, o)?;
    let s = b.get(o + k..o + k + n)?;
    Some((Some(String::from_utf8_lossy(s).into_owned()), k + n))
}

/// Whether an attribute's value says `optional` (the constructor's bool argument, or the named
/// `Optional`). `sig`: the constructor's signature; `value`: the attribute's blob. None: a value
/// this does not read.
fn optional(sig: &[u8], value: &[u8]) -> Option<bool> {
    if sig.first()? & 0x20 == 0 || value.get(..2)? != [1, 0] {
        return None;
    }
    let (count, k) = cuint(sig, 1)?;
    let params = sig.get(1 + k + 1..1 + k + 1 + count)?;
    let (mut o, mut opt) = (2, false);
    for &p in params {
        match p {
            0x02 => {
                opt |= *value.get(o)? != 0;
                o += 1;
            }
            0x0E => o += ser_string(value, o)?.1,
            _ => return None,
        }
    }
    let named = u16::from_le_bytes(value.get(o..o + 2)?.try_into().ok()?);
    o += 2;
    for _ in 0..named {
        let (kind, ty) = (*value.get(o)?, *value.get(o + 1)?);
        if kind != 0x53 && kind != 0x54 {
            return None;
        }
        let (name, k) = ser_string(value, o + 2)?;
        o += 2 + k;
        match ty {
            0x02 => {
                if name.as_deref() == Some("Optional") {
                    opt |= *value.get(o)? != 0;
                }
                o += 1;
            }
            0x0E => o += ser_string(value, o)?.1,
            _ => return None,
        }
    }
    Some(opt)
}

/// The members one module marks as required (not optional).
pub fn of_module(a: &Assembly, image: &str) -> Result<Vec<Member>, String> {
    let e = |e: crate::clrmeta::Error| format!("{image}: {e}");
    let mut out = Vec::new();
    let (methods, fields, refs) = (a.rows(METHODDEF).map_err(e)?, a.rows(FIELD).map_err(e)?, a.rows(MEMBERREF).map_err(e)?);
    let row = |rows: &[Vec<u32>], r: u32| -> Result<Vec<u32>, String> {
        rows.get((r as usize).wrapping_sub(1)).cloned().ok_or_else(|| format!("{image}: row {r} past its table"))
    };
    for ca in a.rows(CUSTOMATTRIBUTE).map_err(e)?.iter() {
        // the attribute's type, through its constructor
        let (tab, r) = Assembly::coded(Coded::CustomAttributeType, ca[1]).map_err(e)?;
        let (ty, sig) = match tab {
            Some(METHODDEF) => {
                let m = row(&methods, r)?;
                (a.type_of_method(r).map_err(e)?.map(|t| t.full.clone()).unwrap_or_default(), m[4])
            }
            Some(MEMBERREF) => {
                let m = row(&refs, r)?;
                let (ptab, prow) = Assembly::coded(Coded::MemberRefParent, m[0]).map_err(e)?;
                let name = match ptab {
                    Some(TYPEDEF) | Some(TYPEREF) => a.type_name(ptab, prow).map_err(e)?,
                    _ => String::new(),
                };
                (name, m[2])
            }
            _ => continue,
        };
        if ty != ATTRIBUTE {
            continue;
        }
        // (a value this does not read: counted as required)
        if optional(&a.blob(sig).map_err(e)?, &a.blob(ca[2]).map_err(e)?) == Some(true) {
            continue;
        }
        let (ptab, prow) = Assembly::coded(Coded::HasCustomAttribute, ca[0]).map_err(e)?;
        let member = match ptab {
            Some(TYPEDEF) => Some((a.type_name(ptab, prow).map_err(e)?, String::new(), false)),
            Some(METHODDEF) => {
                let name = a.string(row(&methods, prow)?[3]).map_err(e)?;
                a.type_of_method(prow).map_err(e)?.map(|t| (t.full.clone(), name, false))
            }
            Some(FIELD) => {
                let name = a.string(row(&fields, prow)?[1]).map_err(e)?;
                a.type_of_field(prow).map_err(e)?.map(|t| (t.full.clone(), name, true))
            }
            _ => None,
        };
        if let Some((ty, name, field)) = member {
            out.push(Member { image: image.to_string(), ty, name, field });
        }
    }
    Ok(out)
}

/// The required members of the player package's modules (`dir`: its Variations/il2cpp/Managed)
/// that the build's metadata lacks, for the modules the build has an image of.
pub fn census(dir: &str, meta: &Metadata) -> Result<Census, String> {
    let have = meta.members()?;
    let mut names: Vec<String> = std::fs::read_dir(dir)
        .map_err(|e| format!("{dir}: {e}"))?
        .filter_map(|e| e.ok().map(|e| e.file_name().to_string_lossy().into_owned()))
        .filter(|n| n.ends_with(".dll"))
        .collect();
    names.sort();
    let mut c = Census::default();
    let mut absent = BTreeSet::new();
    for image in names {
        let Some(types) = have.get(&image) else { continue };
        let a = Assembly::open(format!("{dir}/{image}")).map_err(|e| format!("{dir}/{image}: {e}"))?;
        c.modules += 1;
        for m in of_module(&a, &image)? {
            c.required += 1;
            let there = match types.get(&m.ty) {
                None => false,
                Some(_) if m.name.is_empty() => true,
                Some((methods, fields)) => (if m.field { fields } else { methods }).contains(&m.name),
            };
            if !there {
                absent.insert(m);
            }
        }
    }
    for m in absent {
        if m.stood_in() {
            c.stood_in.push(m);
        } else {
            c.absent.push(m);
        }
    }
    Ok(c)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn attribute_values() {
        // [RequiredByNativeCode]
        assert_eq!(optional(&[0x20, 0, 1], &[1, 0, 0, 0]), Some(false));
        // [RequiredByNativeCode(true)], [RequiredByNativeCode("Name", false)]
        assert_eq!(optional(&[0x20, 1, 1, 2], &[1, 0, 1, 0, 0]), Some(true));
        assert_eq!(optional(&[0x20, 2, 1, 0x0E, 2], &[1, 0, 2, b'a', b'b', 0, 0, 0]), Some(false));
        // [RequiredByNativeCode(Optional = true)], [RequiredByNativeCode(GenerateProxy = true, Name = null)]
        let mut v = vec![1, 0, 1, 0, 0x54, 2, 8];
        v.extend_from_slice(b"Optional");
        v.push(1);
        assert_eq!(optional(&[0x20, 0, 1], &v), Some(true));
        let mut v = vec![1, 0, 2, 0, 0x54, 2, 13];
        v.extend_from_slice(b"GenerateProxy");
        v.extend_from_slice(&[1, 0x54, 0x0E, 4]);
        v.extend_from_slice(b"Name");
        v.push(0xFF);
        assert_eq!(optional(&[0x20, 0, 1], &v), Some(false));
        // not read: an enum argument, a cut value
        assert_eq!(optional(&[0x20, 1, 1, 0x11], &[1, 0, 0, 0, 0, 0]), None);
        assert_eq!(optional(&[0x20, 1, 1, 2], &[1, 0]), None);
    }

    // The table here names what the bridge stands in for: every class and method of its
    // ua_standin_space, when the runtime's sources are beside this crate.
    #[test]
    fn stand_ins_as_the_bridge() {
        let c = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../runtime/macho/bridge/bridge.c");
        let Ok(text) = std::fs::read_to_string(c) else { return };
        let table = &text[text.find("ua_standin_space __attribute__").expect("the table")..];
        let table = &table[..table.find("#define SCLASSES").expect("the table's end")];
        let (classes, methods) = table.split_at(table.find(".methods").expect("methods"));
        let strings = |line: &str| -> Vec<String> {
            line.split('"').skip(1).step_by(2).map(str::to_string).collect()
        };
        let entries = |part: &str| -> Vec<String> {
            part.lines().map(str::trim).filter(|l| l.starts_with("{ ")).map(str::to_string).collect()
        };
        let classes: Vec<(String, String)> = entries(classes)
            .iter()
            .map(|l| {
                let s = strings(l);
                (s[0].clone(), format!("{}.{}/{}", s[1], s[2], s[3]))
            })
            .collect();
        let mut theirs = Vec::new();
        for l in entries(methods) {
            let k: usize = l[2..l.find(',').unwrap()].trim().parse().unwrap();
            theirs.push((classes[k].0.clone(), classes[k].1.clone(), strings(&l)[0].clone()));
        }
        let ours: Vec<(String, String, String)> =
            STAND_INS.iter().map(|(i, t, m)| (i.to_string(), t.to_string(), m.to_string())).collect();
        assert_eq!(ours, theirs);
    }
}
