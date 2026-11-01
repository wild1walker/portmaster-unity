//! `pmu patch-obb new|ggm|check` (patch_obb.py): an add-on's asset bundles (Cuphead's DLC) beside
//! an installed game: which of them the game lacks, the game's globalgamemanagers (its Unity
//! version, for `convert`), and whether a copy of them stopped early. (`pack`, which rewrites the
//! bundles, comes with the Unity-file crate.)
use std::collections::BTreeSet;
use std::fs::{self, File};
use std::io::{Read, Write};

use serde_json::Value;

use crate::zip::Archive;
use crate::{fail, io_at, py, Error, Result};

pub const PREFIX: &str = "assets/AssetBundles/";

/// The first two paragraphs of patch_obb.py's docstring: what it prints on wrong arguments.
pub const USAGE: &str = "`python -m pmunity patch-obb`: asset bundles added to an installed game as its patch OBB.
    python -m pmunity patch-obb new <bundles dir> <game dir>     the bundles the game does not have yet
    python -m pmunity patch-obb ggm <game dir> <out file>        the game's globalgamemanagers (its
                                                                 Unity version, for `convert`)
    python -m pmunity patch-obb pack <bundles dir> <game dir>    patch.<version>.<package>.obb
    python -m pmunity patch-obb check <names file> <bundles dir>... [--game <game dir>]
                                                                 exits 3 when a copy stopped early: a
                                                                 bundle shorter than its header says,
                                                                 or some of the add-on's bundles (the
                                                                 names file) there and some not";

/// The built game's android_package.json.
pub fn package(game: &str) -> Result<Value> {
    let p = py::join(game, "android_package.json");
    let text = fs::read(&p).map_err(io_at(&p))?;
    serde_json::from_slice(&text)
        .map_err(|e| Error::Fail(format!("json.decoder.JSONDecodeError: {e}")))
}

/// str() of a JSON value in an f-string, as Python prints it.
fn pystr(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Bool(b) => (if *b { "True" } else { "False" }).into(),
        Value::Null => "None".into(),
        v => v.to_string(),
    }
}

fn key<'a>(pk: &'a Value, k: &str) -> Result<&'a Value> {
    pk.get(k)
        .ok_or_else(|| Error::Fail(format!("KeyError: {}", py::str_repr(k))))
}

/// A file name the package names (a path part: a string, or the TypeError of os.path.join).
fn name_of(v: &Value) -> Result<String> {
    match v {
        Value::String(s) => Ok(s.clone()),
        _ => fail("TypeError: join() argument must be str, bytes, or os.PathLike object"),
    }
}

fn is_file(p: &str) -> bool {
    fs::metadata(p).is_ok_and(|m| m.is_file())
}

/// The installed game's archives: its APK, main OBB and patch OBB, those that are there.
pub fn archives(game: &str) -> Result<Vec<String>> {
    let pk = package(game)?;
    let apk = pk.get("apk").map_or(Ok(String::new()), name_of)?;
    let main = pk.get("main_obb").map_or(Ok(String::new()), name_of)?;
    let patch = format!(
        "patch.{}.{}.obb",
        pystr(key(&pk, "version_code")?),
        pystr(key(&pk, "package")?)
    );
    Ok([apk, main, patch]
        .into_iter()
        .filter(|n| !n.is_empty() && is_file(&py::join(game, n)))
        .map(|n| py::join(game, &n))
        .collect())
}

/// The bundle names the installed game's archives hold.
pub fn have(game: &str) -> Result<BTreeSet<String>> {
    let mut got = BTreeSet::new();
    for a in archives(game)? {
        let z = Archive::open(&a)?;
        got.extend(
            z.names()
                .filter_map(|n| n.strip_prefix(PREFIX))
                .map(str::to_string),
        );
    }
    Ok(got)
}

/// A folder's names, sorted (sorted(os.listdir(d))).
fn listdir(d: &str) -> Result<Vec<String>> {
    let mut v = Vec::new();
    for e in fs::read_dir(d).map_err(io_at(d))? {
        v.push(e?.file_name().to_string_lossy().into_owned());
    }
    v.sort();
    Ok(v)
}

/// The bundles the game does not have yet.
pub fn new(bundles: &str, game: &str) -> Result<Vec<String>> {
    let got = have(game)?;
    Ok(listdir(bundles)?
        .into_iter()
        .filter(|n| !n.starts_with('.') && !got.contains(n) && is_file(&py::join(bundles, n)))
        .collect())
}

/// (size, the size its header gives) of a UnityFS bundle cut short, else None.
pub fn short(path: &str) -> Result<Option<(u64, i64)>> {
    let have = fs::metadata(path).map_err(io_at(path))?.len();
    let mut head = Vec::new();
    File::open(path)
        .map_err(io_at(path))?
        .take(512)
        .read_to_end(&mut head)?;
    let mut need: i64 = 1; // (an empty file)
    if head.starts_with(b"UnityFS\0") {
        let cstr_end = |p: usize| {
            head.get(p..)
                .and_then(|t| t.iter().position(|&c| c == 0))
                .map(|i| p + i + 1)
        };
        need = cstr_end(12)
            .and_then(cstr_end)
            .and_then(|p| head.get(p..p + 8))
            .map_or(head.len() as i64 + 1, |b| {
                i64::from_be_bytes(b.try_into().unwrap())
            });
    }
    Ok(((have as i128) < need as i128).then_some((have, need)))
}

/// What a download or copy that stopped early left out: (the add-on's bundles missing, though some
/// of them are there; (name, size, header size) of the bundles cut short). The add-on's bundles
/// count where the installed game already has them.
pub type Cut = (String, u64, i64);

pub fn incomplete(
    names: &[String],
    dirs: &[String],
    game: Option<&str>,
) -> Result<(Vec<String>, Vec<Cut>)> {
    let mut got = match game {
        Some(g) if !g.is_empty() => have(g)?,
        _ => BTreeSet::new(),
    };
    let mut cut = Vec::new();
    for d in dirs {
        for n in listdir(d)? {
            let p = py::join(d, &n);
            if n.starts_with('.') || !is_file(&p) {
                continue;
            }
            got.insert(n.clone());
            if let Some((h, s)) = short(&p)? {
                cut.push((n, h, s));
            }
        }
    }
    let all: BTreeSet<&String> = names.iter().collect();
    let missing: Vec<String> = all
        .iter()
        .filter(|n| !got.contains(**n))
        .map(|n| n.to_string())
        .collect();
    Ok((
        if missing.len() < all.len() {
            missing
        } else {
            Vec::new()
        },
        cut,
    ))
}

/// The game's globalgamemanagers, out of its APK, into out (created before the APK is read, as the
/// Python opens it).
pub fn ggm(game: &str, out: &str) -> Result<()> {
    let pk = package(game)?;
    let apk = py::join(game, &name_of(key(&pk, "apk")?)?);
    let z = Archive::open(&apk)?;
    let mut f = File::create(out).map_err(io_at(out))?;
    let data = z.read("assets/bin/Data/globalgamemanagers")?;
    f.write_all(&data).map_err(io_at(out))?;
    Ok(())
}

/// The lines of a text file as Python's text mode gives them (\n, \r\n or \r ends a line).
fn lines(text: &str) -> Vec<&str> {
    let mut v = Vec::new();
    let mut rest = text;
    while !rest.is_empty() {
        let i = rest.find(['\n', '\r']).map_or(rest.len(), |i| i + 1);
        let i = if rest[..i].ends_with('\r') && rest[i..].starts_with('\n') {
            i + 1
        } else {
            i
        };
        v.push(&rest[..i]);
        rest = &rest[i..];
    }
    v
}

/// patch_obb.main for new, ggm and check: what to print on stdout. Wrong arguments: Error::Exit
/// with the usage; an incomplete copy: Error::Incomplete (exit 3) with its message.
pub fn main(argv: &[String]) -> Result<String> {
    let a: Vec<&str> = argv.iter().map(String::as_str).collect();
    match a.as_slice() {
        ["new", b, g] => Ok(new(b, g)?.join("\n") + "\n"),
        ["ggm", g, o] => ggm(g, o).map(|_| String::new()),
        ["check", _, _, ..] => {
            let mut a: Vec<&str> = a[1..].to_vec();
            let mut game = None;
            if let Some(i) = a.iter().position(|x| *x == "--game") {
                let Some(g) = a.get(i + 1) else {
                    return fail("IndexError: list index out of range");
                };
                game = Some(g.to_string());
                a.remove(i);
                a.remove(i);
            }
            let Some(names_file) = a.first() else {
                return fail("IndexError: list index out of range");
            };
            let raw = fs::read(names_file).map_err(io_at(names_file))?;
            let text = String::from_utf8(raw)
                .map_err(|e| Error::Fail(format!("UnicodeDecodeError: {e}")))?;
            let names: Vec<String> = lines(&text)
                .into_iter()
                .map(py::strip)
                .filter(|n| !n.is_empty())
                .map(str::to_string)
                .collect();
            let dirs: Vec<String> = a[1..].iter().map(|s| s.to_string()).collect();
            let (missing, cut) = incomplete(&names, &dirs, game.as_deref())?;
            if missing.is_empty() && cut.is_empty() {
                return Ok(String::new());
            }
            let mut say = Vec::new();
            if !missing.is_empty() {
                let more = if missing.len() > 5 { ", ..." } else { "" };
                say.push(format!(
                    "{} of the {} add-on bundles missing ({}{more})",
                    missing.len(),
                    names.len(),
                    missing[..missing.len().min(5)].join(", ")
                ));
            }
            say.extend(
                cut.iter()
                    .take(5)
                    .map(|(n, h, s)| format!("{n} cut short: {h} of {s} bytes")),
            );
            Err(Error::Incomplete(format!(
                "incomplete (a download or copy that stopped early): {}",
                say.join("; ")
            )))
        }
        _ => Err(Error::Exit(USAGE.into())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::zipwrite::{write_zip, Src};

    fn bundle(size: usize) -> Vec<u8> {
        let mut h = b"UnityFS\0".to_vec();
        h.extend(6u32.to_be_bytes());
        h.extend(b"5.x.x\0");
        h.extend(b"2017.4.9f1\0");
        h.extend((size as i64).to_be_bytes());
        h.extend([0u8; 12]);
        h.resize(size, 0);
        h
    }

    struct Tmp(String);
    impl Tmp {
        fn new(tag: &str) -> Tmp {
            let p = std::env::temp_dir().join(format!("pkgzip-obb-{tag}-{}", std::process::id()));
            let _ = fs::remove_dir_all(&p);
            fs::create_dir_all(&p).unwrap();
            Tmp(p.to_string_lossy().into_owned())
        }
        fn folder(&self, name: &str, files: &[(&str, Vec<u8>)]) -> String {
            let d = py::join(&self.0, name);
            fs::create_dir_all(&d).unwrap();
            for (n, data) in files {
                fs::write(py::join(&d, n), data).unwrap();
            }
            d
        }
    }
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn names() -> Vec<String> {
        ["dlc_a", "dlc_b", "dlc_c"].map(String::from).to_vec()
    }

    #[test]
    fn complete_and_no_addon() {
        let t = Tmp::new("complete");
        let d = t.folder(
            "b",
            &[
                ("dlc_a", bundle(200)),
                ("dlc_b", bundle(200)),
                ("dlc_c", bundle(200)),
                ("game_x", bundle(200)),
            ],
        );
        assert_eq!(incomplete(&names(), &[d], None).unwrap(), (vec![], vec![]));
        let d = t.folder(
            "c",
            &[("game_x", bundle(200)), ("notes.txt", b"x".to_vec())],
        );
        assert_eq!(incomplete(&names(), &[d], None).unwrap(), (vec![], vec![]));
    }

    #[test]
    fn some_missing_and_cut_short() {
        let t = Tmp::new("cut");
        let d = t.folder(
            "b",
            &[
                ("dlc_a", bundle(200)),
                ("dlc_b", bundle(300)[..150].to_vec()),
                (".DS_Store", vec![]),
            ],
        );
        assert_eq!(
            incomplete(&names(), &[d.clone()], None).unwrap(),
            (
                vec!["dlc_c".to_string()],
                vec![("dlc_b".to_string(), 150, 300)]
            )
        );
        let e = main(&[
            "check".into(),
            t.folder(
                "n",
                &[("names.txt", b"dlc_a\r\n dlc_b \rdlc_c\n\n".to_vec())],
            ) + "/names.txt",
            d,
        ])
        .unwrap_err();
        assert_eq!(e.status(), 3);
        assert_eq!(
            e.to_string(),
            "incomplete (a download or copy that stopped early): 1 of the 3 add-on bundles missing (dlc_c); \
             dlc_b cut short: 150 of 300 bytes"
        );
    }

    #[test]
    fn empty_and_header_only() {
        let t = Tmp::new("empty");
        let d = t.folder(
            "b",
            &[
                ("dlc_a", vec![]),
                ("dlc_b", bundle(300)[..20].to_vec()),
                ("dlc_c", bundle(200)),
            ],
        );
        let (missing, cut) = incomplete(&names(), &[d], None).unwrap();
        assert!(missing.is_empty());
        assert_eq!(
            cut,
            [("dlc_a".to_string(), 0, 1), ("dlc_b".to_string(), 20, 21)]
        );
    }

    #[test]
    fn installed_game_counts_and_new_and_ggm() {
        let t = Tmp::new("game");
        let game = t.folder(
            "game",
            &[(
                "android_package.json",
                br#"{"apk": "g.apk", "package": "com.x", "version_code": 1}"#.to_vec(),
            )],
        );
        let entries = vec![
            (format!("{PREFIX}dlc_c"), Src::Bytes(vec![])),
            (
                "assets/bin/Data/globalgamemanagers".to_string(),
                Src::Bytes(b"ggm".to_vec()),
            ),
        ];
        write_zip(&py::join(&game, "g.apk"), &entries, &mut |_| {}, 4).unwrap();
        let d = t.folder(
            "b",
            &[
                ("dlc_a", bundle(200)),
                ("dlc_b", bundle(200)),
                ("dlc_c", bundle(200)),
            ],
        );
        assert_eq!(
            incomplete(&names(), &[d.clone()], Some(&game)).unwrap(),
            (vec![], vec![])
        );
        assert_eq!(
            main(&["new".into(), d, game.clone()]).unwrap(),
            "dlc_a\ndlc_b\n"
        );
        let out = py::join(&t.0, "ggm");
        main(&["ggm".into(), game, out.clone()]).unwrap();
        assert_eq!(fs::read(out).unwrap(), b"ggm");
    }

    #[test]
    fn usage() {
        let e = main(&["new".into()]).unwrap_err();
        assert_eq!(e.status(), 1);
        assert!(e.to_string().starts_with("`python -m pmunity patch-obb`"));
    }
}
