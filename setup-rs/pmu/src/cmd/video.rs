//! `pmu video <game dir> [--config <convert.json>] [--set KEY=VALUE]...` (pmu's own): convert's
//! `video` step on a game that is already set up, in place, for setups made before the step
//! existed. With the port's convert config the step runs only when its "steps" name it, with its
//! "video" settings (--set over them). The resource files of
//! the APK and OBBs (assets/bin/Data/*.resource) that hold WebM clips are taken out with their
//! asset files and the game's globalgamemanagers into <game dir>/.pmunity-video, the step runs on
//! them (its settings: `--set mode=on`, `max_height=540`, ...; mode auto by default: only where
//! MemTotal is at most ram_mb), and each file it changed is written back over its entry: stored
//! and of the same length, so nothing else in the APK or OBB moves (Archive::overwrite_stored).
//! An entry is looked through, taken out and written back a piece at a time: a game's resource
//! files (its music too) can be hundreds of MB, and the handheld has 1 GB.
//! The changed files wait in the work folder, marked ready, until all are written back: a run
//! that is stopped writes them again the next time. Exit 0 when done or nothing needed doing;
//! "video: ..." lines on stdout.
use convert::pyjson::Json;
use pkgzip::zip::Archive;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

const DATA: &str = "assets/bin/Data/";
const USAGE: &str = "usage: pmu video <game dir> [--config <convert.json>] [--set KEY=VALUE]...";

fn fail(m: impl std::fmt::Display) -> ExitCode {
    eprintln!("video: {m}");
    ExitCode::from(1)
}

/// The step's settings: the config's "video" section (or none), KEY=VALUE pairs over it (a number
/// where the value is one).
fn settings(base: Option<&Json>, sets: &[String]) -> Result<Json, String> {
    let mut d = match base.and_then(|c| c.get("video")) {
        Some(Json::Dict(d)) => d.clone(),
        _ => Vec::new(),
    };
    for s in sets {
        let (k, v) = s
            .split_once('=')
            .ok_or_else(|| format!("--set {s}: KEY=VALUE"))?;
        let k = k.strip_prefix("video.").unwrap_or(k);
        let v = match v.parse::<i128>() {
            Ok(n) => Json::Int(n),
            Err(_) => Json::Str(v.to_string()),
        };
        d.retain(|(x, _)| x != k);
        d.push((k.to_string(), v));
    }
    Ok(Json::Dict(vec![
        ("steps".into(), Json::List(vec![Json::Str("video".into())])),
        ("video".into(), Json::Dict(d)),
    ]))
}

/// How far after an EBML header's first byte its doc type is looked for.
const LOOK: usize = 64;

/// Whether the bytes hold a WebM clip (an EBML header with the "webm" doc type) that begins
/// before `upto`.
fn webm_before(b: &[u8], upto: usize) -> bool {
    let magic = [0x1A, 0x45, 0xDF, 0xA3];
    b.windows(4).take(upto).enumerate().any(|(i, w)| {
        w == magic
            && b[i..(i + LOOK).min(b.len())]
                .windows(4)
                .any(|t| t == b"webm")
    })
}

/// Whether a resource file holds a WebM clip, read a piece at a time (never whole in memory): a
/// header is looked at once the LOOK bytes from it are here, or the file's end.
fn has_webm(r: &mut dyn Read) -> std::io::Result<bool> {
    let mut piece = vec![0u8; 1 << 20];
    let mut b: Vec<u8> = Vec::with_capacity(piece.len() + LOOK);
    loop {
        let got = match r.read(&mut piece) {
            Ok(n) => n,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        };
        b.extend_from_slice(&piece[..got]);
        let upto = if got == 0 {
            b.len()
        } else {
            b.len().saturating_sub(LOOK - 1)
        };
        if webm_before(&b, upto) {
            return Ok(true);
        }
        if got == 0 {
            return Ok(false);
        }
        b.drain(..upto);
    }
}

/// pkgzip's error text of a read's error (a bad CRC keeps its own).
fn zip_err(e: std::io::Error) -> String {
    pkgzip::Error::from(e).to_string()
}

pub fn run(args: &[String]) -> ExitCode {
    let mut game = None;
    let mut sets = Vec::new();
    let mut config = None;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--set" => match it.next() {
                Some(v) => sets.push(v.clone()),
                None => return fail(USAGE),
            },
            "--config" => match it.next() {
                Some(v) => config = Some(v.clone()),
                None => return fail(USAGE),
            },
            _ if game.is_none() && !a.starts_with('-') => game = Some(PathBuf::from(a)),
            _ => return fail(USAGE),
        }
    }
    let Some(game) = game else { return fail(USAGE) };
    let base = match &config {
        Some(p) => match std::fs::read_to_string(p)
            .map_err(|e| e.to_string())
            .and_then(|t| convert::pyjson::loads(&t).map_err(|e| format!("{p}: {e:?}")))
        {
            Ok(j) => Some(j),
            Err(e) => return fail(e),
        },
        None => None,
    };
    if let Some(b) = &base {
        let named = b
            .get("steps")
            .and_then(Json::as_list)
            .is_some_and(|l| l.iter().any(|s| s.as_str() == Some("video")));
        if !named {
            println!("video: not one of this port's convert steps");
            return ExitCode::SUCCESS;
        }
    }
    let cfg = match settings(base.as_ref(), &sets) {
        Ok(c) => c,
        Err(e) => return fail(e),
    };
    match convert::steps::video::applies(&cfg) {
        Ok((false, why)) => {
            println!("video: clips kept as they are ({why})");
            return ExitCode::SUCCESS;
        }
        Ok(_) => {}
        Err(e) => return fail(e),
    }
    match in_place(&game, cfg) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => fail(e),
    }
}

fn in_place(game: &Path, cfg: Json) -> Result<(), String> {
    // the game's archives and which one holds each data file
    let mut paths: Vec<PathBuf> = std::fs::read_dir(game)
        .map_err(|e| format!("{}: {e}", game.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "apk" || x == "obb"))
        .collect();
    paths.sort();
    if paths.is_empty() {
        return Err(format!(
            "{}: no APK or OBB (a game set up for Unity's Android player?)",
            game.display()
        ));
    }
    let mut zips = Vec::new();
    for p in &paths {
        zips.push((
            p.to_string_lossy().into_owned(),
            Archive::open(&p.to_string_lossy()).map_err(|e| e.to_string())?,
        ));
    }
    let holder = |name: &str| {
        zips.iter()
            .find(|(_, z)| z.get(&format!("{DATA}{name}")).is_some())
    };
    let work = game.join(".pmunity-video");
    let (data, out) = (work.join("data"), work.join("out"));
    if !out.join(".ready").exists() {
        let _ = std::fs::remove_dir_all(&data);
        std::fs::create_dir_all(&data).map_err(|e| e.to_string())?;
        // the resource files with clips, their asset files, the game's version
        let mut wanted = vec!["globalgamemanagers".to_string()];
        for (_, z) in &zips {
            for n in z.names() {
                let Some(base) = n
                    .strip_prefix(DATA)
                    .filter(|b| b.ends_with(".resource") && !b.contains('/'))
                else {
                    continue;
                };
                let e = z.info(n).map_err(|e| e.to_string())?;
                let mut r = z.reader(e).map_err(|e| e.to_string())?;
                if has_webm(&mut r).map_err(zip_err)? {
                    wanted.push(base.to_string());
                    wanted.push(format!("{}.assets", base.trim_end_matches(".resource")));
                }
            }
        }
        if wanted.len() == 1 {
            println!("video: no WebM clips in the game's data");
            let _ = std::fs::remove_dir_all(&work);
            return Ok(());
        }
        for n in &wanted {
            let (_, z) = holder(n).ok_or_else(|| format!("{n}: in none of the game's archives"))?;
            let e = z.info(&format!("{DATA}{n}")).map_err(|e| e.to_string())?;
            let mut r = z.reader(e).map_err(|e| e.to_string())?;
            let mut f = std::fs::File::create(data.join(n)).map_err(|e| e.to_string())?;
            pkgzip::zip::copy_entry(&mut r, &mut f).map_err(|e| e.to_string())?;
        }
        let mut job = convert::Job::new(&data, &out, cfg).map_err(|e| e.to_string())?;
        let r = convert::steps::video::run(&mut job);
        job.flush().map_err(|e| e.to_string())?;
        r.map_err(|e| e.to_string())?;
        drop(job);
        std::fs::write(out.join(".ready"), b"").map_err(|e| e.to_string())?;
    }
    // the changed files back over their entries
    let mut changed: Vec<String> = std::fs::read_dir(&out)
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok()?.file_name().into_string().ok())
        .filter(|n| !n.starts_with('.') && out.join(n).is_file())
        .collect();
    changed.sort();
    // every file checked against its entry before any is written: a game is never left with some
    // of a resource's files new and some old
    for n in &changed {
        let (_, z) = holder(n).ok_or_else(|| format!("{n}: in none of the game's archives"))?;
        let e = z.info(&format!("{DATA}{n}")).map_err(|e| e.to_string())?;
        let len = std::fs::metadata(out.join(n))
            .map_err(|e| e.to_string())?
            .len();
        if e.method != 0 || e.file_size != len || e.flag_bits & 0x08 != 0 {
            let _ = std::fs::remove_dir_all(&work);
            return Err(format!(
                "{n}: cannot be written back in place ({len} bytes for an entry of {} bytes, method {}): the game is left as it was",
                e.file_size, e.method
            ));
        }
    }
    for n in &changed {
        let (path, z) = holder(n).ok_or_else(|| format!("{n}: in none of the game's archives"))?;
        let p = out.join(n);
        let len = std::fs::metadata(&p).map_err(|e| e.to_string())?.len();
        let mut f = std::fs::File::open(&p).map_err(|e| e.to_string())?;
        z.overwrite_stored_from(path, &format!("{DATA}{n}"), &mut f, len)
            .map_err(|e| e.to_string())?;
    }
    if !changed.is_empty() {
        println!(
            "video: {} files of the game's data written back in place",
            changed.len()
        );
    }
    let _ = std::fs::remove_dir_all(&work);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A reader that gives its bytes `1` at a time (an entry read in pieces).
    struct Drip<'a>(&'a [u8], usize);

    impl Read for Drip<'_> {
        fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
            let n = self.1.min(out.len()).min(self.0.len());
            out[..n].copy_from_slice(&self.0[..n]);
            self.0 = &self.0[n..];
            Ok(n)
        }
    }

    /// has_webm on the bytes read whole and in pieces of several sizes: one answer.
    fn scan(b: &[u8]) -> bool {
        let whole = webm_before(b, b.len());
        for step in [1, 3, 63, 64, 65, 1000, 1 << 20] {
            assert_eq!(has_webm(&mut Drip(b, step)).unwrap(), whole, "step {step}");
        }
        whole
    }

    const HEAD: [u8; 12] = [
        0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81, 0x01, 0x42, 0x82, 0x84,
    ];

    #[test]
    fn webm_found_anywhere() {
        let mut b = vec![0u8; 5000];
        assert!(!scan(&b));
        assert!(!scan(&[]));
        b.extend_from_slice(&HEAD);
        b.extend_from_slice(b"webm");
        assert!(scan(&b));
    }

    /// The header and its doc type on two sides of a piece's end, wherever that end falls; the
    /// doc type at the last place it counts (60 bytes after the header's first) and one past it.
    #[test]
    fn webm_across_pieces() {
        for lead in [0, 1, 61, 62, 63, 64, 65, 127, 128, 1000] {
            for gap in [0, 8, 56, 57] {
                let mut b = vec![0u8; lead];
                b.extend_from_slice(&[0x1A, 0x45, 0xDF, 0xA3]);
                b.extend(std::iter::repeat(0x20).take(gap));
                b.extend_from_slice(b"webm");
                b.extend(std::iter::repeat(0).take(200));
                assert_eq!(scan(&b), gap <= 56, "lead {lead} gap {gap}");
                // (the file ends right after the doc type)
                b.truncate(lead + 4 + gap + 4);
                assert_eq!(scan(&b), gap <= 56, "lead {lead} gap {gap}, at the end");
            }
        }
        // a header cut off by the file's end, and a doc type with no header before it
        assert!(!scan(&[0x1A, 0x45, 0xDF]));
        assert!(!scan(b"....webm...."));
    }

    #[test]
    fn set_values() {
        let c = settings(None, &["mode=on".into(), "video.max_height=540".into()]).unwrap();
        let v = c.get("video").unwrap();
        assert_eq!(v.get("mode"), Some(&Json::Str("on".into())));
        assert_eq!(v.get("max_height"), Some(&Json::Int(540)));
        // the port's section, --set over it
        let port = convert::pyjson::loads(
            r#"{"steps": ["video"], "video": {"mode": "off", "cq_level": 12}}"#,
        )
        .unwrap();
        let c = settings(Some(&port), &["mode=on".into()]).unwrap();
        let v = c.get("video").unwrap();
        assert_eq!(v.get("mode"), Some(&Json::Str("on".into())));
        assert_eq!(v.get("cq_level"), Some(&Json::Int(12)));
    }
}
