//! Unpacks macOS installer packages (.pkg, as GOG ships its macOS games) without macOS tools:
//! `pmu macpkg <root> <installer.pkg>...` (macpkg.py).
//!
//! A .pkg is a xar archive; each component package in it holds `PackageInfo` (the install
//! location) and `Payload`, a cpio archive (gzip or pbzx, or plain) whose files land at
//! <root>/<install location>/<path>. GOG's installers carry the game in the component's Scripts
//! archive under payload/ instead, which a postinstall script copies to
//! <install location>/<gog_bundle_name>; that folder is unpacked the same way. Everything is
//! streamed; symbolic links are skipped and counted, `._*` (macOS metadata) left out.
use std::collections::BTreeMap;
use std::fs;
use std::io::{Read, Write};

use crate::cpio::Cpio;
use crate::streams::{payload_stream, Ahead};
use crate::xar::{Entry, Xar};
use crate::{io_at, py, Error, Result};

/// keep(path) -> bool: which Payload paths (relative to /) to unpack.
pub type Keep<'a> = Option<&'a dyn Fn(&str) -> bool>;

const S_IFMT: u32 = 0o170000;
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;

/// Totals of one package: files written, their bytes, symbolic links skipped.
#[derive(Default, Debug, Clone, Copy, PartialEq)]
pub struct Totals {
    pub files: u64,
    pub bytes: u64,
    pub links: u64,
}

/// Extracts every component package of one .pkg under root (macpkg.extract). The log gets the
/// lines the Python prints for each component.
pub fn extract(pkg: &str, root: &str, log: &mut dyn FnMut(&str), keep: Keep) -> Result<Totals> {
    let x = Xar::open(pkg)?;
    let mut files: BTreeMap<&str, &Entry> = BTreeMap::new();
    for e in &x.entries {
        files.insert(&e.path, e);
    }
    let mut comps: Vec<&str> = files
        .keys()
        .filter(|p| matches!(py::basename(p), "Payload" | "Scripts" | "PackageInfo"))
        .map(|p| p.rsplit_once('/').map_or("", |(c, _)| c))
        .collect();
    comps.sort();
    comps.dedup();
    let name = py::basename(pkg);
    let mut t = Totals::default();
    for c in comps {
        let pre = if c.is_empty() {
            String::new()
        } else {
            format!("{c}/")
        };
        let label = if c.is_empty() { "package" } else { c };
        let mut loc = "/".to_string();
        if let Some(info) = files.get(format!("{pre}PackageInfo").as_str()) {
            let text = x.read(info)?;
            loc = install_location(&text)?
                .filter(|l| !l.is_empty())
                .unwrap_or(loc);
        }
        if let Some(p) = files
            .get(format!("{pre}Payload").as_str())
            .filter(|e| e.data.is_some())
        {
            let before = t.files;
            let s = Ahead::new(payload_stream(x.reader(p)?)?);
            unpack(&mut Cpio::new(s), root, &loc, None, keep, &mut t, None)?;
            log(&format!(
                "{name}: {label} Payload -> {loc}: {} files",
                t.files - before
            ));
        }
        if let Some(sc) = files
            .get(format!("{pre}Scripts").as_str())
            .filter(|e| e.data.is_some())
        {
            // GOG: payload/ goes to <install location>/<gog_bundle_name> (the postinstall script's
            // `mv payload/*` or, for a DLC, `cp -rf payload/*` into the game's .app); without such
            // a script, to the install location itself. Unpacked into a staging folder first (the
            // script comes after the payload in the archive), then moved into place.
            let stage = py::join(&py::join(root, ".pmunity-payload"), label);
            let before = t.files;
            let mut scripts = BTreeMap::new();
            let s = Ahead::new(payload_stream(x.reader(sc)?)?);
            unpack(
                &mut Cpio::new(s),
                &stage,
                "/",
                Some("payload"),
                None,
                &mut t,
                Some(&mut scripts),
            )?;
            if t.files > before {
                let post = scripts
                    .get("postinstall")
                    .map(|b| String::from_utf8_lossy(b).into_owned());
                let lstrip = loc.trim_start_matches('/');
                let dest = match post.as_deref().and_then(gog_bundle_name) {
                    Some(b) => py::join(lstrip, b),
                    None => lstrip.to_string(),
                };
                merge(&stage, &py::join(root, &dest))?;
                log(&format!(
                    "{name}: {label} Scripts/payload -> /{dest}: {} files",
                    t.files - before
                ));
            }
            let _ = fs::remove_dir_all(&stage);
        }
    }
    let _ = fs::remove_dir_all(py::join(root, ".pmunity-payload"));
    if t.files == 0 {
        let mut held = Vec::new();
        for (p, e) in &files {
            held.push(match &e.data {
                None => format!("{p} (dir)"),
                Some(d) => {
                    let s = d.size.as_deref().filter(|s| !s.is_empty()).unwrap_or("0");
                    let v = py::int(s, 10).ok_or_else(|| {
                        Error::Fail(format!(
                            "ValueError: invalid literal for int() with base 10: {}",
                            py::str_repr(s)
                        ))
                    })?;
                    format!("{p} ({} KB)", v >> 10)
                }
            });
        }
        let list: String = held.join(", ").chars().take(2000).collect();
        log(&format!(
            "{name}: no files found; the package holds: {list}"
        ));
    }
    Ok(t)
}

/// PackageInfo's install-location attribute (of its root element).
fn install_location(xml: &[u8]) -> Result<Option<String>> {
    let text = String::from_utf8_lossy(xml);
    let opts = roxmltree::ParsingOptions {
        allow_dtd: true,
        ..Default::default()
    };
    let doc = roxmltree::Document::parse_with_options(&text, opts)
        .map_err(|e| Error::Fail(format!("xml.etree.ElementTree.ParseError: {e}")))?;
    Ok(doc
        .root_element()
        .attribute("install-location")
        .map(str::to_string))
}

/// re.search(r'gog_bundle_name="([^"/]+)"', text).group(1).
fn gog_bundle_name(text: &str) -> Option<&str> {
    const KEY: &str = "gog_bundle_name=\"";
    let mut from = 0;
    while let Some(i) = text[from..].find(KEY) {
        let start = from + i + KEY.len();
        let rest = &text[start..];
        let n = rest.find(['"', '/']).unwrap_or(rest.len());
        if n > 0 && rest[n..].starts_with('"') {
            return Some(&rest[..n]);
        }
        from = from + i + 1;
    }
    None
}

/// Moves the tree src into dst (files replace files of the same name), then removes src
/// (macpkg._merge).
fn merge(src: &str, dst: &str) -> Result<()> {
    fn walk(d: &str, target: &str) -> Result<()> {
        fs::create_dir_all(target).map_err(io_at(target))?;
        // (the whole listing first, as os.walk, then the moves)
        let listing = fs::read_dir(d)
            .map_err(io_at(d))?
            .collect::<std::io::Result<Vec<_>>>()?;
        let mut dirs = Vec::new();
        for e in listing {
            let n = e.file_name().to_string_lossy().into_owned();
            if fs::metadata(e.path()).is_ok_and(|m| m.is_dir()) {
                dirs.push(n);
            } else {
                let to = py::join(target, &n);
                fs::rename(e.path(), &to).map_err(io_at(&to))?;
            }
        }
        for n in dirs {
            walk(&py::join(d, &n), &py::normpath(&py::join(target, &n)))?;
        }
        Ok(())
    }
    if fs::metadata(src).is_ok_and(|m| m.is_dir()) {
        walk(src, &py::normpath(dst))?;
    }
    let _ = fs::remove_dir_all(src);
    Ok(())
}

/// Writes the members of one cpio stream under root/<loc> (macpkg._unpack). strip: only members
/// inside that top folder, with the folder taken off their paths; scripts gets the small
/// top-level files outside it (postinstall, preinstall) by name.
fn unpack(
    s: &mut Cpio<impl Read>,
    root: &str,
    loc: &str,
    strip: Option<&str>,
    keep: Keep,
    t: &mut Totals,
    mut scripts: Option<&mut BTreeMap<String, Vec<u8>>>,
) -> Result<()> {
    let mut buf = vec![0u8; 1 << 20];
    while let Some(m) = s.next_member()? {
        let mut nm = py::normpath(&m.name);
        if let Some(strip) = strip {
            let inside = nm
                .strip_prefix(strip)
                .and_then(|r| r.strip_prefix('/'))
                .map(str::to_string);
            match inside {
                Some(r) if nm != strip => nm = r,
                _ => {
                    match scripts.as_deref_mut() {
                        Some(sc)
                            if !nm.contains('/')
                                && m.mode & S_IFMT == S_IFREG
                                && m.size < (1 << 20) =>
                        {
                            let b = crate::cpio::exact(
                                s.data(),
                                m.size as usize,
                                "cpio archive ends early",
                            )?;
                            sc.insert(nm, b);
                        }
                        _ => s.skip(m.size)?,
                    }
                    continue;
                }
            }
        }
        let mut rel = py::normpath(&py::join(loc.trim_start_matches('/'), &nm));
        if rel == "." {
            rel.clear();
        }
        let kind = m.mode & S_IFMT;
        if rel.starts_with("..")
            || rel.starts_with('/')
            || py::basename(&rel).starts_with("._")
            || keep.is_some_and(|k| !k(&format!("/{rel}")))
        {
            s.skip(m.size)?;
            continue;
        }
        let dst = py::join(root, &rel);
        match kind {
            S_IFDIR => makedirs(&dst)?,
            S_IFLNK => {
                s.skip(m.size)?;
                t.links += 1;
            }
            S_IFREG => {
                makedirs(py::dirname(&dst))?;
                let tmp = format!("{dst}.part");
                let mut f = fs::File::create(&tmp).map_err(io_at(&tmp))?;
                let mut left = m.size;
                while left > 0 {
                    let k = left.min(buf.len() as u64) as usize;
                    let n = s.data().read(&mut buf[..k])?;
                    if n == 0 {
                        return Err(Error::Fail(format!(
                            "EOFError: {}: archive ends early",
                            m.name
                        )));
                    }
                    f.write_all(&buf[..n]).map_err(io_at(&tmp))?;
                    left -= n as u64;
                }
                drop(f);
                fs::rename(&tmp, &dst).map_err(io_at(&dst))?;
                t.files += 1;
                t.bytes += m.size;
            }
            _ => s.skip(m.size)?,
        }
    }
    Ok(())
}

/// os.makedirs(path, exist_ok=True).
pub fn makedirs(path: &str) -> Result<()> {
    if path.is_empty() {
        return Err(Error::Fail(
            "FileNotFoundError: [Errno 2] No such file or directory: ''".into(),
        ));
    }
    fs::create_dir_all(path).map_err(io_at(path))
}

/// `pmu macpkg <root> <installer.pkg>...`: the Python's lines on stdout; an incomplete installer
/// stops it with its message on stderr (status 3).
pub fn main(argv: &[String]) -> Result<()> {
    if argv.len() < 2 {
        return Err(Error::Exit(
            "usage: python -m pmunity macpkg <root> <installer.pkg>...".into(),
        ));
    }
    let root = &argv[0];
    makedirs(root)?;
    for pkg in &argv[1..] {
        let mut out = std::io::stdout();
        let t = extract(
            pkg,
            root,
            &mut |m| {
                let _ = writeln!(out, "{m}");
            },
            None,
        )?;
        let links = if t.links > 0 {
            format!(", {} symbolic links skipped", t.links)
        } else {
            String::new()
        };
        println!(
            "{}: {} files, {} MB{links}",
            py::basename(pkg),
            t.files,
            t.bytes >> 20
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cpio::tests::{newc, odc};
    use flate2::write::{GzEncoder, ZlibEncoder};
    use flate2::Compression;

    fn zlib(d: &[u8]) -> Vec<u8> {
        let mut e = ZlibEncoder::new(Vec::new(), Compression::default());
        e.write_all(d).unwrap();
        e.finish().unwrap()
    }

    fn gzip(d: &[u8]) -> Vec<u8> {
        let mut e = GzEncoder::new(Vec::new(), Compression::default());
        e.write_all(d).unwrap();
        e.finish().unwrap()
    }

    /// A xar archive of (path, data), each stored zlib-encoded as pkgs do (test_macpkg_synthetic.xar).
    pub fn xar(members: &[(&str, &[u8])]) -> Vec<u8> {
        let mut heap = Vec::new();
        let mut files = BTreeMap::new();
        for (p, d) in members {
            let z = zlib(d);
            files.insert(p.to_string(), (heap.len(), z.len(), d.len()));
            heap.extend(z);
        }
        fn tree(prefix: &str, files: &BTreeMap<String, (usize, usize, usize)>) -> String {
            let mut names: Vec<&str> = files
                .keys()
                .filter_map(|p| p.strip_prefix(prefix))
                .map(|r| r.split('/').next().unwrap())
                .collect();
            names.dedup();
            let mut xml = String::new();
            for nm in names {
                let p = format!("{prefix}{nm}");
                if let Some((off, ln, size)) = files.get(&p) {
                    xml += &format!(
                        "<file><name>{nm}</name><type>file</type><data><offset>{off}</offset><length>{ln}</length>\
                         <size>{size}</size><encoding style=\"application/x-gzip\"/></data></file>"
                    );
                } else {
                    xml += &format!(
                        "<file><name>{nm}</name><type>directory</type>{}</file>",
                        tree(&format!("{p}/"), files)
                    );
                }
            }
            xml
        }
        let toc = format!(
            "<?xml version=\"1.0\"?><xar><toc>{}</toc></xar>",
            tree("", &files)
        );
        let tz = zlib(toc.as_bytes());
        let mut out = b"xar!".to_vec();
        out.extend(28u16.to_be_bytes());
        out.extend(1u16.to_be_bytes());
        out.extend((tz.len() as u64).to_be_bytes());
        out.extend((toc.len() as u64).to_be_bytes());
        out.extend(0u32.to_be_bytes());
        out.extend(tz);
        out.extend(heap);
        out
    }

    const INFO: &[u8] = b"<pkg-info install-location=\"/Applications\" identifier=\"test\"/>";

    fn gog_pkg(kind: &str, files: &[(&str, &[u8])]) -> Vec<u8> {
        let post =
            format!("#!/bin/bash\ngog_bundle_name=\"Game.app\"\ngog_installer_type=\"{kind}\"\n");
        let paths: Vec<String> = files
            .iter()
            .map(|(p, _)| format!("./payload/{p}"))
            .collect();
        let mut e: Vec<(&str, Option<&[u8]>)> = vec![
            (".", None),
            ("./.DS_Store", Some(b"x")),
            ("./payload", None),
        ];
        e.extend(
            paths
                .iter()
                .zip(files)
                .map(|(p, (_, d))| (p.as_str(), Some(*d))),
        );
        e.push(("./postinstall", Some(post.as_bytes())));
        let scripts = gzip(&odc(&e));
        xar(&[
            ("Distribution", b"<installer-gui-script/>"),
            ("package.pkg/PackageInfo", INFO),
            ("package.pkg/Scripts", &scripts),
        ])
    }

    struct Tmp(String);
    impl Tmp {
        fn new(tag: &str) -> Tmp {
            let p =
                std::env::temp_dir().join(format!("pkgzip-macpkg-{tag}-{}", std::process::id()));
            let _ = fs::remove_dir_all(&p);
            fs::create_dir_all(&p).unwrap();
            Tmp(p.to_string_lossy().into_owned())
        }
        fn write(&self, name: &str, data: &[u8]) -> String {
            let p = py::join(&self.0, name);
            fs::write(&p, data).unwrap();
            p
        }
        fn read(&self, rel: &str) -> Vec<u8> {
            fs::read(py::join(&self.0, rel)).unwrap()
        }
    }
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn quiet() -> impl FnMut(&str) {
        |_| {}
    }

    #[test]
    fn payload() {
        let tmp = Tmp::new("payload");
        let cp = odc(&[
            (".", None),
            ("./Game.app/Contents/Info.plist", Some(b"plist")),
            ("./Game.app/._Info.plist", Some(b"meta")),
        ]);
        let pkg = tmp.write(
            "a.pkg",
            &xar(&[("PackageInfo", INFO), ("Payload", &gzip(&cp))]),
        );
        let root = py::join(&tmp.0, "root");
        let t = extract(&pkg, &root, &mut quiet(), None).unwrap();
        assert_eq!((t.files, t.bytes), (1, 5));
        assert_eq!(
            tmp.read("root/Applications/Game.app/Contents/Info.plist"),
            b"plist"
        );
        assert!(!fs::exists(py::join(&root, "Applications/Game.app/._Info.plist")).unwrap());
    }

    #[test]
    fn gog_scripts_payload_with_dlc() {
        let tmp = Tmp::new("gog");
        let root = py::join(&tmp.0, "root");
        let game = tmp.write(
            "game.pkg",
            &gog_pkg(
                "game",
                &[
                    ("Contents/Data/a", b"game a"),
                    ("Contents/Data/b", b"game b"),
                ],
            ),
        );
        let dlc = tmp.write(
            "dlc.pkg",
            &gog_pkg(
                "dlc",
                &[("Contents/Data/b", b"dlc b"), ("Contents/Data/c", b"dlc c")],
            ),
        );
        let mut lines = Vec::new();
        for pkg in [&game, &dlc] {
            extract(pkg, &root, &mut |m| lines.push(m.to_string()), None).unwrap();
        }
        assert_eq!(
            tmp.read("root/Applications/Game.app/Contents/Data/a"),
            b"game a"
        );
        assert_eq!(
            tmp.read("root/Applications/Game.app/Contents/Data/b"),
            b"dlc b"
        ); // (the DLC's copy wins, as cp -rf)
        assert_eq!(
            tmp.read("root/Applications/Game.app/Contents/Data/c"),
            b"dlc c"
        );
        let top: Vec<_> = fs::read_dir(&root)
            .unwrap()
            .map(|e| e.unwrap().file_name())
            .collect();
        assert_eq!(top, ["Applications"]); // (no staging or .DS_Store left)
        assert_eq!(
            lines[0],
            "game.pkg: package.pkg Scripts/payload -> /Applications/Game.app: 2 files"
        );
    }

    #[test]
    fn scripts_payload_without_gog_postinstall() {
        let tmp = Tmp::new("nogog");
        let root = py::join(&tmp.0, "root");
        let cp = odc(&[
            ("./payload", None),
            ("./payload/x", Some(b"x")),
            ("./postinstall", Some(b"#!/bin/sh\n")),
        ]);
        let pkg = tmp.write(
            "s.pkg",
            &xar(&[("PackageInfo", INFO), ("Scripts", &gzip(&cp))]),
        );
        extract(&pkg, &root, &mut quiet(), None).unwrap();
        assert_eq!(tmp.read("root/Applications/x"), b"x"); // (no bundle name: the install location)
    }

    #[test]
    fn newc_links_and_no_files() {
        let tmp = Tmp::new("newc");
        let root = py::join(&tmp.0, "root");
        let cp = newc(&[
            ("a/f", 0o100644, b"data"),
            ("a/l", 0o120777, b"f"),
            ("../up", 0o100644, b"no"),
        ]);
        let pkg = tmp.write("n.pkg", &xar(&[("Payload", &cp)]));
        let t = extract(&pkg, &root, &mut quiet(), None).unwrap();
        assert_eq!(
            t,
            Totals {
                files: 1,
                bytes: 4,
                links: 1
            }
        );
        assert_eq!(tmp.read("root/a/f"), b"data");
        let empty = tmp.write("e.pkg", &xar(&[("Distribution", b"<x/>")]));
        let mut said = String::new();
        extract(&empty, &root, &mut |m| said = m.to_string(), None).unwrap();
        assert_eq!(
            said,
            "e.pkg: no files found; the package holds: Distribution (0 KB)"
        );
    }

    #[test]
    fn incomplete_installer() {
        let tmp = Tmp::new("cut");
        let data = gog_pkg("dlc", &[("Contents/Data/a", &b"dlc a".repeat(1000))]);
        let pkg = tmp.write("dlc.pkg", &data[..data.len() - 200]);
        let e = extract(&pkg, &py::join(&tmp.0, "root"), &mut quiet(), None).unwrap_err();
        assert_eq!(e.status(), 3);
        assert!(e.to_string().starts_with("dlc.pkg is incomplete: "), "{e}");
        let e = main(&[py::join(&tmp.0, "root"), pkg]).unwrap_err();
        assert_eq!(e.status(), 3);
    }

    #[test]
    fn bundle_name() {
        assert_eq!(
            gog_bundle_name("a gog_bundle_name=\"x/y\" gog_bundle_name=\"Game.app\""),
            Some("Game.app")
        );
        assert_eq!(gog_bundle_name("gog_bundle_name=\"\""), None);
    }
}
