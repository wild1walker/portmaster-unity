//! `pmu lazy <game dir> [--config <convert.json>] [--set KEY=VALUE]...` (pmu's own): convert's `lazy` step on a
//! game that is already set up, in place, for setups made before the step existed. With the port's convert
//! config the step runs only when its "steps" name it, with its "lazy" settings (--set over them). The serialized
//! files of the APK and OBBs (assets/bin/Data/*.assets and globalgamemanagers) are taken out one at a time into
//! <game dir>/.pmunity-lazy: a first pass finds the named objects, a second edits each file's PreloadData and
//! ResourceManager lists (the step's edit: every list keeps its length, so the file does) and keeps each changed
//! file in the work folder. Marked ready, they are written over their entries: stored and of the same length, so
//! nothing else in the APK or OBB moves (Archive::overwrite_stored_from, the CRC-32 in both headers). A run that is
//! stopped before the mark starts again; one stopped after writes them all again. Exit 0 when done or nothing
//! needed doing; "lazy: ..." lines on stdout.
use convert::pyjson::Json;
use convert::steps::lazy;
use pkgzip::zip::Archive;
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

const DATA: &str = "assets/bin/Data/";
const USAGE: &str = "usage: pmu lazy <game dir> [--config <convert.json>] [--set KEY=VALUE]...";

fn fail(m: impl std::fmt::Display) -> ExitCode {
    eprintln!("lazy: {m}");
    ExitCode::from(1)
}

/// The step's settings: the config's "lazy" section (or none), KEY=VALUE pairs over it.
fn settings(base: Option<&Json>, sets: &[String]) -> Result<Json, String> {
    let mut d = match base.and_then(|c| c.get(lazy::STEP)) {
        Some(Json::Dict(d)) => d.clone(),
        _ => Vec::new(),
    };
    for s in sets {
        let (k, v) = s
            .split_once('=')
            .ok_or_else(|| format!("--set {s}: KEY=VALUE"))?;
        let k = k.strip_prefix("lazy.").unwrap_or(k);
        d.retain(|(x, _)| x != k);
        d.push((k.to_string(), Json::Str(v.to_string())));
    }
    Ok(Json::Dict(d))
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
            .is_some_and(|l| l.iter().any(|s| s.as_str() == Some(lazy::STEP)));
        if !named {
            println!("lazy: not one of this port's convert steps");
            return ExitCode::SUCCESS;
        }
    }
    let c = match settings(base.as_ref(), &sets) {
        Ok(c) => c,
        Err(e) => return fail(e),
    };
    let names = lazy::names_of(&c);
    if names.is_empty() {
        println!("lazy: no names (lazy.names): nothing to do");
        return ExitCode::SUCCESS;
    }
    match in_place(&game, &names) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => fail(e),
    }
}

/// One entry taken out into `dir` and opened; the file is removed when the DataFile is dropped by the caller.
fn take_out(z: &Archive, name: &str, dir: &Path) -> Result<PathBuf, String> {
    let e = z.info(&format!("{DATA}{name}")).map_err(|e| e.to_string())?;
    let mut r = z.reader(e).map_err(|e| e.to_string())?;
    let p = dir.join(name);
    let mut f = std::fs::File::create(&p).map_err(|e| format!("{}: {e}", p.display()))?;
    pkgzip::zip::copy_entry(&mut r, &mut f).map_err(|e| e.to_string())?;
    Ok(p)
}

fn in_place(game: &Path, names: &BTreeSet<Vec<u8>>) -> Result<(), String> {
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
    // the serialized files that can hold named objects or preload lists, and the archive of each
    let mut files: Vec<(String, usize)> = Vec::new();
    for (i, (_, z)) in zips.iter().enumerate() {
        for n in z.names() {
            if let Some(b) = n.strip_prefix(DATA).filter(|b| !b.contains('/')) {
                if (b.ends_with(".assets") || b == "globalgamemanagers") && !files.iter().any(|(f, _)| f == b) {
                    files.push((b.to_string(), i));
                }
            }
        }
    }
    files.sort();
    let work = game.join(".pmunity-lazy");
    let (one, out) = (work.join("one"), work.join("out"));
    let db = unityfs::ClassDb::default_db().map_err(|e| e.to_string())?;
    let opts = unityfs::Options { db: Some(std::sync::Arc::new(db)), ..Default::default() };
    if !out.join(".ready").exists() {
        let _ = std::fs::remove_dir_all(&work);
        std::fs::create_dir_all(&one).map_err(|e| e.to_string())?;
        std::fs::create_dir_all(&out).map_err(|e| e.to_string())?;
        // the named objects
        let mut drop: BTreeSet<lazy::Key> = BTreeSet::new();
        let mut found: BTreeSet<Vec<u8>> = BTreeSet::new();
        for (name, i) in &files {
            let p = take_out(&zips[*i].1, name, &one)?;
            if let Ok(f) = unityfs::DataFile::open(&p, &opts) {
                let (keys, n) = lazy::survey(&f, name, names);
                drop.extend(keys);
                found.extend(n);
            }
            let _ = std::fs::remove_file(&p);
        }
        for n in names.difference(&found) {
            println!("lazy: no object named {:?} in the game's data", String::from_utf8_lossy(n));
        }
        if drop.is_empty() {
            let _ = std::fs::remove_dir_all(&work);
            return Ok(());
        }
        // each file's lists without them, kept when changed (the same length, or nothing is written)
        let mut total = 0;
        for (name, i) in &files {
            let p = take_out(&zips[*i].1, name, &one)?;
            let len = std::fs::metadata(&p).map_err(|e| e.to_string())?.len();
            if let Ok(mut f) = unityfs::DataFile::open(&p, &opts) {
                let n = lazy::edit(&mut f, name, &drop).map_err(|e| format!("{name}: {e}"))?;
                if n > 0 {
                    let q = out.join(name);
                    f.write_to(&q, false).map_err(|e| format!("{name}: {e}"))?;
                    let nl = std::fs::metadata(&q).map_err(|e| e.to_string())?.len();
                    if nl != len {
                        let _ = std::fs::remove_dir_all(&work);
                        return Err(format!("{name}: {nl} bytes after the edit, {len} before: the game is left as it was"));
                    }
                    total += n;
                }
            }
            let _ = std::fs::remove_file(&p);
        }
        println!("lazy: {} object(s) named, left out of {total} preload or dependency entries", drop.len());
        let _ = std::fs::remove_dir_all(&one);
        std::fs::write(out.join(".ready"), b"").map_err(|e| e.to_string())?;
    }
    // the changed files back over their entries, every one checked first
    let mut changed: Vec<String> = std::fs::read_dir(&out)
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok()?.file_name().into_string().ok())
        .filter(|n| !n.starts_with('.') && out.join(n).is_file())
        .collect();
    changed.sort();
    let holder = |n: &str| zips.iter().find(|(_, z)| z.get(&format!("{DATA}{n}")).is_some());
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
        let p = out.join(n);
        let len = std::fs::metadata(&p).map_err(|e| e.to_string())?.len();
        let mut f = std::fs::File::open(&p).map_err(|e| e.to_string())?;
        z.overwrite_stored_from(path, &format!("{DATA}{n}"), &mut f, len).map_err(|e| e.to_string())?;
    }
    if !changed.is_empty() {
        println!("lazy: {} files of the game's data written back in place", changed.len());
    }
    let _ = std::fs::remove_dir_all(&work);
    Ok(())
}
