//! `pmu video <game dir> [--set KEY=VALUE]...` (pmu's own): convert's `video` step on a game that
//! is already set up, in place, for setups made before the step existed. The resource files of
//! the APK and OBBs (assets/bin/Data/*.resource) that hold WebM clips are taken out with their
//! asset files and the game's globalgamemanagers into <game dir>/.pmunity-video, the step runs on
//! them (its settings: `--set mode=on`, `max_height=540`, ...; mode auto by default: only where
//! MemTotal is at most ram_mb), and each file it changed is written back over its entry: stored
//! and of the same length, so nothing else in the APK or OBB moves (Archive::overwrite_stored).
//! The changed files wait in the work folder, marked ready, until all are written back: a run
//! that is stopped writes them again the next time. Exit 0 when done or nothing needed doing;
//! "video: ..." lines on stdout.
use convert::pyjson::Json;
use pkgzip::zip::Archive;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

const DATA: &str = "assets/bin/Data/";
const USAGE: &str = "usage: pmu video <game dir> [--set KEY=VALUE]...";

fn fail(m: impl std::fmt::Display) -> ExitCode {
    eprintln!("video: {m}");
    ExitCode::from(1)
}

/// The step's settings from KEY=VALUE pairs (a number where the value is one).
fn settings(sets: &[String]) -> Result<Json, String> {
    let mut d = Vec::new();
    for s in sets {
        let (k, v) = s.split_once('=').ok_or_else(|| format!("--set {s}: KEY=VALUE"))?;
        let k = k.strip_prefix("video.").unwrap_or(k);
        let v = match v.parse::<i128>() {
            Ok(n) => Json::Int(n),
            Err(_) => Json::Str(v.to_string()),
        };
        d.push((k.to_string(), v));
    }
    Ok(Json::Dict(vec![("steps".into(), Json::List(vec![Json::Str("video".into())])), ("video".into(), Json::Dict(d))]))
}

/// Whether a resource file holds a WebM clip (an EBML header with the "webm" doc type).
fn has_webm(b: &[u8]) -> bool {
    let magic = [0x1A, 0x45, 0xDF, 0xA3];
    b.windows(4)
        .enumerate()
        .any(|(i, w)| w == magic && b[i..(i + 64).min(b.len())].windows(4).any(|t| t == b"webm"))
}

pub fn run(args: &[String]) -> ExitCode {
    let mut game = None;
    let mut sets = Vec::new();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--set" => match it.next() {
                Some(v) => sets.push(v.clone()),
                None => return fail(USAGE),
            },
            _ if game.is_none() && !a.starts_with('-') => game = Some(PathBuf::from(a)),
            _ => return fail(USAGE),
        }
    }
    let Some(game) = game else { return fail(USAGE) };
    let cfg = match settings(&sets) {
        Ok(c) => c,
        Err(e) => return fail(e),
    };
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
        return Err(format!("{}: no APK or OBB (a game set up for Unity's Android player?)", game.display()));
    }
    let mut zips = Vec::new();
    for p in &paths {
        zips.push((p.to_string_lossy().into_owned(), Archive::open(&p.to_string_lossy()).map_err(|e| e.to_string())?));
    }
    let holder = |name: &str| zips.iter().find(|(_, z)| z.get(&format!("{DATA}{name}")).is_some());
    let work = game.join(".pmunity-video");
    let (data, out) = (work.join("data"), work.join("out"));
    if !out.join(".ready").exists() {
        let _ = std::fs::remove_dir_all(&data);
        std::fs::create_dir_all(&data).map_err(|e| e.to_string())?;
        // the resource files with clips, their asset files, the game's version
        let mut wanted = vec!["globalgamemanagers".to_string()];
        for (_, z) in &zips {
            for n in z.names() {
                let Some(base) = n.strip_prefix(DATA).filter(|b| b.ends_with(".resource") && !b.contains('/')) else { continue };
                if has_webm(&z.read(n).map_err(|e| e.to_string())?) {
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
            let b = z.read(&format!("{DATA}{n}")).map_err(|e| e.to_string())?;
            std::fs::write(data.join(n), b).map_err(|e| e.to_string())?;
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
        let len = std::fs::metadata(out.join(n)).map_err(|e| e.to_string())?.len();
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
        let b = std::fs::read(out.join(n)).map_err(|e| e.to_string())?;
        z.overwrite_stored(path, &format!("{DATA}{n}"), &b).map_err(|e| e.to_string())?;
    }
    if !changed.is_empty() {
        println!("video: {} files of the game's data written back in place", changed.len());
    }
    let _ = std::fs::remove_dir_all(&work);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn webm_found_anywhere() {
        let mut b = vec![0u8; 5000];
        assert!(!has_webm(&b));
        b.extend_from_slice(&[0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81, 0x01, 0x42, 0x82, 0x84]);
        b.extend_from_slice(b"webm");
        assert!(has_webm(&b));
    }

    #[test]
    fn set_values() {
        let c = settings(&["mode=on".into(), "video.max_height=540".into()]).unwrap();
        let v = c.get("video").unwrap();
        assert_eq!(v.get("mode"), Some(&Json::Str("on".into())));
        assert_eq!(v.get("max_height"), Some(&Json::Int(540)));
    }
}
