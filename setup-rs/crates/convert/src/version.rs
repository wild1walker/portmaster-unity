//! The Unity version a game's data was built with, read from the data (unity_version.py), and the
//! check `convert` makes before anything else (__main__._check_data).
use crate::error::{Error, Result};
use std::path::Path;

/// The versions the converter supports, both included (major, minor).
pub const FIRST: (u64, u64) = (5, 6);
pub const LAST: (u64, u64) = (6000, 6);

/// A Unity version string: its text and (major, minor).
#[derive(Clone, Debug, PartialEq)]
pub struct Version {
    pub text: String,
    pub line: (u64, u64),
}

impl Version {
    /// `^(\d+)\.(\d+)\.(\d+)([abfpx]?)(\d*)` as Version() parses it.
    pub fn parse(text: &str) -> Result<Version> {
        let bad = || {
            Error::Value(format!(
                "not a Unity version: {}",
                crate::pyjson::str_repr(text)
            ))
        };
        let mut parts = text.splitn(3, '.');
        let num = |s: Option<&str>| -> Option<u64> {
            let s = s?;
            let n = s.bytes().take_while(u8::is_ascii_digit).count();
            if n == 0 {
                None
            } else {
                s[..n].parse().ok()
            }
        };
        let (a, b) = (parts.next(), parts.next());
        fn whole(s: Option<&str>) -> Option<u64> {
            s.filter(|s| !s.is_empty() && s.bytes().all(|c| c.is_ascii_digit()))?
                .parse()
                .ok()
        }
        let major = whole(a).ok_or_else(bad)?;
        let minor = whole(b).ok_or_else(bad)?;
        num(parts.next()).ok_or_else(bad)?;
        Ok(Version {
            text: text.to_string(),
            line: (major, minor),
        })
    }
}

/// Unity's version as a serialized file's header names it (sf_header: formats 9 - 23).
fn sf_version(head: &[u8]) -> Result<String> {
    if head.len() < 40 {
        return Err(Error::Value("short file".into()));
    }
    let fmt = u32::from_be_bytes(head[8..12].try_into().unwrap());
    if !(9..=23).contains(&fmt) {
        return Err(Error::Value(format!(
            "unsupported SerializedFile format {fmt}"
        )));
    }
    let p = if fmt >= 22 { 48 } else { 20 };
    let end = head[p..]
        .iter()
        .position(|&c| c == 0)
        .ok_or_else(|| Error::Value("subsection not found".into()))?;
    ascii(&head[p..p + end])
}

fn ascii(b: &[u8]) -> Result<String> {
    if b.is_ascii() {
        Ok(String::from_utf8(b.to_vec()).unwrap())
    } else {
        Err(Error::Value(
            "'ascii' codec can't decode the version".into(),
        ))
    }
}

/// The Unity version of a serialized file or UnityFS bundle (of_file).
pub fn of_file(path: &Path) -> Result<Version> {
    use std::io::Read;
    let mut head = Vec::with_capacity(256);
    std::fs::File::open(path)?
        .take(256)
        .read_to_end(&mut head)?;
    if head.starts_with(b"UnityFS\0") {
        let parts: Vec<&[u8]> = head[12.min(head.len())..].split(|&c| c == 0).collect();
        let v = parts
            .get(1)
            .ok_or_else(|| Error::Value("list index out of range".into()))?;
        return Version::parse(&ascii(v)?);
    }
    Version::parse(&sf_version(&head)?)
}

/// The Unity version of a data folder: globalgamemanagers, else mainData, else data.unity3d.
pub fn of_data(data: &Path) -> Result<Version> {
    for n in ["globalgamemanagers", "mainData", "data.unity3d"] {
        let p = data.join(n);
        if p.is_file() {
            return of_file(&p);
        }
    }
    Err(Error::Exit(format!(
        "{}: no globalgamemanagers, mainData or data.unity3d (not a Unity data folder?)",
        data.display()
    )))
}

/// _check_data: the data's version when the converter handles it, else why not (sys.exit).
pub fn check_data(data: &Path, force: bool) -> Result<Version> {
    if !data.join("globalgamemanagers").is_file() && data.join("data.unity3d").is_file() {
        return Err(Error::Exit(format!(
            "{d}: a compressed build (data.unity3d): unpack it first with `python -m pmunity unpack {d}`",
            d = data.display()
        )));
    }
    let v = of_data(data)?;
    if (FIRST <= v.line && v.line <= LAST) || force {
        return Ok(v);
    }
    Err(Error::Exit(format!(
        "Unity {}: the converter supports data of Unity {}.{} - {}.{} (their shader program and data formats are known; see docs/compat/README.md). Use --any-unity-version to try anyway (shader layouts are then guessed and checked by round trip).",
        v.text, FIRST.0, FIRST.1, LAST.0, LAST.1
    )))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions() {
        assert_eq!(Version::parse("2017.4.9f1").unwrap().line, (2017, 4));
        assert_eq!(Version::parse("6000.0.1").unwrap().line, (6000, 0));
        assert!(Version::parse("2017.4").is_err());
        assert!(Version::parse("x.4.1").is_err());
        assert!(Version::parse("5.x.x").is_err());
        let mut h = vec![0u8; 20];
        h[11] = 17;
        h.extend_from_slice(b"2017.4.9f1\0");
        h.resize(64, 0);
        assert_eq!(sf_version(&h).unwrap(), "2017.4.9f1");
        assert!(sf_version(&h[..30]).is_err());
    }
}
