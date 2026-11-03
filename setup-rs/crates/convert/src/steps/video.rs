//! Step `video` (pmu only, no Python counterpart): the game's VP8 clips made smaller on small
//! handhelds (cfg['video']). A 1920x1080 VP8 clip costs a Cortex-A53 about 31 ms a frame to
//! decode alone and ~40 in the game, more than a 30 fps clip has (Hollow Knight's cinematics
//! played in slow motion against their music); halved to 960x540 it is a quarter of the work and
//! looks the same on a 720x480 panel (luma ~56 dB against the original halved the same way).
//! Each VideoClip whose VP8 picture is taller than `max_height` is halved until it is not and
//! encoded again (crates/video: libvpx at constant quality `cq_level`, key frames where the clip
//! had them, four token partitions for a multi-threaded decoder), and the new clip goes into its
//! .resource file where the old one was (it is smaller; the rest of its old place is zeroed), so
//! no other object of the file moves; the VideoClip gets the new size (m_ExternalResources.m_Size,
//! Width, Height). Other codecs, and clips inside bundles, are left as they are.
//! `mode`: auto (where MemTotal is at most `ram_mb`), on, off. A resource file and the asset files
//! whose clips it holds are written together and journalled as done together.
use super::{field, field_mut, objects, section, text};
use crate::error::{Error, Result};
use crate::job::Job;
use crate::pyjson::Json;
use std::collections::BTreeMap;
use std::io::Read;
use unityfs::Value;

fn defaults() -> Vec<(&'static str, Json)> {
    vec![
        ("mode", Json::Str("auto".into())),
        ("ram_mb", Json::Int(1024)),
        ("max_height", Json::Int(720)),
        ("cpu_used", Json::Int(4)),
        ("cq_level", Json::Int(10)),
        ("max_kbps", Json::Int(4000)),
        ("threads", Json::Int(0)),
    ]
}

fn int_opt(c: &Json, key: &str) -> Result<i128> {
    c.get(key)
        .and_then(Json::as_int)
        .ok_or_else(|| Error::Value(format!("video.{key}: not an integer")))
}

/// (on, why)
fn mode_of(c: &Json) -> Result<(bool, String)> {
    match c.get("mode").and_then(Json::as_str) {
        Some("on") => Ok((true, "video.mode = on".into())),
        Some("off") => Ok((false, "video.mode = off".into())),
        Some("auto") => {
            let mem = super::quality::mem_total_mb();
            let ram = int_opt(c, "ram_mb")?;
            Ok((mem <= ram, format!("{mem} MB of RAM ({} {ram})", if mem <= ram { "at most" } else { "more than" })))
        }
        _ => Err(Error::Value(format!(
            "video.mode: {} (auto, on or off)",
            c.get("mode").map_or("None".into(), Json::repr)
        ))),
    }
}

/// One VideoClip: (asset file, path id, offset, size).
#[derive(Clone, Debug)]
struct Clip {
    rel: String,
    pid: i64,
    offset: u64,
    size: u64,
}

fn is_level(n: &str) -> bool {
    n.strip_prefix("level").is_some_and(|d| !d.is_empty() && d.chars().all(|c| c.is_ascii_digit()))
}

/// Sets a number field to n in the type it has.
fn set_num(v: &mut Value, n: u64) -> Result<()> {
    *v = match v {
        Value::U64(_) => Value::U64(n),
        Value::I64(_) => Value::I64(n as i64),
        Value::U32(_) => Value::U32(u32::try_from(n).map_err(|_| Error::Value(format!("{n}: out of range")))?),
        Value::I32(_) => Value::I32(i32::try_from(n).map_err(|_| Error::Value(format!("{n}: out of range")))?),
        other => return Err(Error::Value(format!("video: a number field holds {other:?}"))),
    };
    Ok(())
}

pub fn run(job: &mut Job) -> Result<()> {
    let c = section(&job.cfg, "video", &defaults())?;
    let (on, why) = mode_of(&c)?;
    if !on {
        job.log(&format!("video: clips kept as they are ({why})"));
        return Ok(());
    }
    if !video::vpx::AVAILABLE {
        return Err(Error::Value("video: this pmu has no VP8 encoder (libvpx is built into arm64 builds)".into()));
    }
    let threads = match int_opt(&c, "threads")? {
        n if n > 0 => n as usize,
        _ => std::thread::available_parallelism().map_or(4, |n| n.get()),
    };
    let s = video::Settings {
        max_height: int_opt(&c, "max_height")? as u64,
        encode: video::vpx::EncodeSettings {
            // the decoder and the conversion take one core
            threads: threads.saturating_sub(1).max(1),
            log2_partitions: 2,
            cpu_used: int_opt(&c, "cpu_used")? as i32,
            cq_level: int_opt(&c, "cq_level")? as i32,
            max_kbps: int_opt(&c, "max_kbps")? as u32,
        },
    };
    // the clips by the resource file that holds them
    let files = job.list("", |n| n.ends_with(".assets") || is_level(n));
    let mut by_res: BTreeMap<String, Vec<Clip>> = BTreeMap::new();
    for rel in &files {
        let f = job.open(&job.path_in(rel), Default::default())?;
        for (pid, class) in objects(&f) {
            if class != "VideoClip" {
                continue;
            }
            let t = f.sf.read_typetree(pid)?;
            let er = field(&t, "m_ExternalResources")?;
            let source = String::from_utf8_lossy(&text(er, "m_Source")?).into_owned();
            let num = |k: &str| field(er, k).ok().and_then(Value::as_u64).unwrap_or(0);
            // a clip of the player's data: its resource file beside the asset file (a clip inside
            // a bundle, "archive:/...", is left as it is)
            if source.is_empty() || source.contains(':') {
                continue;
            }
            let res = source.rsplit('/').next().unwrap_or_default().to_string();
            if !job.path_in(&res).is_file() {
                continue;
            }
            by_res.entry(res).or_default().push(Clip { rel: rel.clone(), pid, offset: num("m_Offset"), size: num("m_Size") });
        }
    }
    let (mut done_clips, mut kept, mut before, mut after) = (0usize, 0usize, 0u64, 0u64);
    for (res, clips) in by_res {
        let mut rels: Vec<String> = clips.iter().map(|c| c.rel.clone()).collect();
        rels.sort();
        rels.dedup();
        if rels.iter().all(|r| job.is_done("video", r)) {
            continue;
        }
        let mut data = Vec::new();
        std::fs::File::open(job.path_in(&res))?.read_to_end(&mut data)?;
        // (asset file, path id) -> (new size, width, height)
        let mut changed: BTreeMap<(String, i64), (u64, u64, u64)> = BTreeMap::new();
        for c in &clips {
            let (a, b) = (c.offset as usize, (c.offset + c.size) as usize);
            if b > data.len() {
                return Err(Error::Value(format!("video: {res}: a clip at {a} runs past the file's end")));
            }
            let t0 = std::time::Instant::now();
            let r = video::reencode(&data[a..b], &s).map_err(|e| Error::Value(format!("video: {res} @{a}: {e}")))?;
            let Some((new, o)) = r else { continue };
            if new.len() as u64 > c.size {
                job.log(&format!("video: {res} @{a}: the smaller picture's clip is not smaller ({} > {} bytes): kept", new.len(), c.size));
                kept += 1;
                continue;
            }
            data[a..a + new.len()].copy_from_slice(&new);
            data[a + new.len()..b].fill(0);
            changed.insert((c.rel.clone(), c.pid), (new.len() as u64, o.to.0, o.to.1));
            job.log(&format!(
                "video: {res} @{a}: {}x{} -> {}x{}, {} frames, {:.1} -> {:.1} MB in {:.0} s",
                o.from.0, o.from.1, o.to.0, o.to.1, o.frames,
                c.size as f64 / 1048576.0, new.len() as f64 / 1048576.0, t0.elapsed().as_secs_f64()
            ));
            done_clips += 1;
            before += c.size;
            after += new.len() as u64;
        }
        if changed.is_empty() {
            for r in &rels {
                job.mark_done("video", r)?;
            }
            continue;
        }
        // the resource file and its asset files, written together
        let work = job.work_dir("video")?;
        let mut out = vec![(res.clone(), work.join(&res))];
        std::fs::write(work.join(&res), &data)?;
        drop(data);
        for rel in &rels {
            let path = job.path_in(rel);
            let mut f = job.open(&path, Default::default())?;
            // each changed object spliced into the file as it was (its numbers keep their width,
            // so it keeps its length and nothing else in the file moves: `pmu video` writes an
            // installed game's file back over its APK entry); a whole new file if one did not
            let mut file = std::fs::read(&path)?;
            let mut spliced = true;
            for ((r, pid), (size, w, h)) in &changed {
                if r != rel {
                    continue;
                }
                let mut t = f.sf.read_typetree(*pid)?;
                set_num(field_mut(field_mut(&mut t, "m_ExternalResources")?, "m_Size")?, *size)?;
                for (k, v) in [("Width", *w), ("Height", *h)] {
                    if let Some(x) = t.get_mut(k) {
                        set_num(x, v)?;
                    }
                }
                f.sf.save_typetree(*pid, &t)?;
                let (old, new) = (f.sf.original_bytes(*pid)?, f.sf.bytes(*pid)?);
                let o = f.sf.object(*pid).ok_or_else(|| Error::Value(format!("video: {rel}: no object {pid}")))?;
                let at = o.byte_start as usize; // (the file's offset: the data offset included)
                if new.len() == old.len() && file.get(at..at + old.len()) == Some(&old[..]) {
                    file[at..at + new.len()].copy_from_slice(&new);
                } else {
                    spliced = false;
                }
            }
            let p = work.join(rel.replace('/', "_"));
            if spliced {
                std::fs::write(&p, &file)?;
            } else {
                f.write_to(&p, false)?;
            }
            out.push((rel.clone(), p));
        }
        job.commit_then_mark(&out)?;
        for r in &rels {
            job.mark_done("video", r)?;
        }
        job.end_work(&work);
    }
    if done_clips > 0 || kept > 0 {
        job.log(&format!(
            "video: {done_clips} clips at most {} pixels tall ({:.1} -> {:.1} MB){}",
            s.max_height, before as f64 / 1048576.0, after as f64 / 1048576.0,
            if kept > 0 { format!(", {kept} kept") } else { String::new() }
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_keep_their_type() {
        let mut v = Value::U32(1080);
        set_num(&mut v, 540).unwrap();
        assert!(matches!(v, Value::U32(540)));
        let mut v = Value::U64(9809172);
        set_num(&mut v, 4765853).unwrap();
        assert!(matches!(v, Value::U64(4765853)));
        assert!(set_num(&mut Value::U32(0), 1 << 40).is_err());
    }

    #[test]
    fn modes() {
        let c = |m: &str| Json::Dict(vec![("mode".into(), Json::Str(m.into())), ("ram_mb".into(), Json::Int(1024))]);
        assert!(mode_of(&c("on")).unwrap().0);
        assert!(!mode_of(&c("off")).unwrap().0);
        assert!(mode_of(&c("sometimes")).is_err());
        assert!(mode_of(&c("auto")).unwrap().1.contains("MB of RAM"));
    }
}
