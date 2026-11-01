//! Step `audio` (audio.py): the game's DecompressOnLoad clips kept where their PCM fits a 1 GB
//! handheld (cfg['audio']). A long Vorbis clip marked DecompressOnLoad is switched to
//! CompressedInMemory only to keep a scene's PCM within `pcm_budget_mb`: the scene's own clips
//! before the resident ones (the first `resident_scenes` scenes count as loaded everywhere), the
//! largest first; clips no scene uses keep the length rule (`compressed_min_s`). Without a
//! budget (None), every clip of the length rule is switched.
//!
//! The decision needs every file's clips, so it is made once, before any file is written, and
//! kept in the journal (`audio`: the settings, the clips to switch by file, the summary): a
//! setup that is stopped and started again goes on with the same list.
use super::{float, int, objects, opt, section, text};
use crate::error::{Error, Result};
use crate::job::Job;
use crate::pyjson::{format_f, json_g, Json};
use std::collections::{BTreeMap, BTreeSet, HashMap};
use unityfs::Value;

pub const DECOMPRESS_ON_LOAD: i128 = 0;
pub const COMPRESSED_IN_MEMORY: i128 = 1;
pub const VORBIS: i128 = 1;

fn defaults() -> Vec<(&'static str, Json)> {
    vec![
        ("pcm_budget_mb", Json::Int(16)),
        ("compressed_min_s", Json::Float(2.0)),
        ("resident_scenes", Json::Int(2)),
    ]
}

/// One clip: (name, load type, compression format, length in s, frequency, channels).
#[derive(Clone, Debug)]
pub struct Clip {
    pub name: Vec<u8>,
    pub load: i128,
    pub fmt: i128,
    pub length: f64,
    pub rate: i128,
    pub channels: i128,
}

impl Clip {
    /// Bytes of the 16-bit PCM Unity decodes it to (`round` rounds half to even, as Python's).
    pub fn pcm(&self) -> i128 {
        (self.length * self.rate as f64).round_ties_even() as i128 * self.channels * 2
    }
}

/// A clip: (file, path id).
pub type Key = (String, i64);

/// sharedassets<n>.assets -> n (scene n's own assets and its PreloadData), else None.
pub fn scene_of(rel: &str) -> Option<i64> {
    let n = rel.strip_prefix("sharedassets")?.strip_suffix(".assets")?;
    if !n.is_empty() && n.chars().all(|c| c.is_ascii_digit()) {
        n.parse().ok()
    } else {
        None
    }
}

/// Which clips to keep compressed in memory (audio.plan): the keys to switch, and the PCM each
/// scene holds after (empty without a budget).
pub fn plan(
    clips: &HashMap<Key, Clip>,
    scenes: &BTreeMap<i64, BTreeSet<Key>>,
    budget: Option<i128>,
    min_s: f64,
    resident: i64,
) -> (BTreeSet<Key>, BTreeMap<i64, i128>) {
    let switchable = |k: &Key| {
        let c = &clips[k];
        c.load == DECOMPRESS_ON_LOAD && c.fmt == VORBIS && c.length >= min_s
    };
    let used: BTreeSet<&Key> = scenes.values().flatten().collect();
    let mut switch: BTreeSet<Key> = clips
        .keys()
        .filter(|k| switchable(k) && (budget.is_none() || !used.contains(k)))
        .cloned()
        .collect();
    let Some(budget) = budget else {
        return (switch, BTreeMap::new());
    };
    let base: BTreeSet<Key> = scenes
        .iter()
        .filter(|(i, _)| **i < resident)
        .flat_map(|(_, s)| s.iter().cloned())
        .collect();
    let mut left = BTreeMap::new();
    for (&i, keys) in scenes {
        let held: Vec<&Key> = keys
            .union(&base)
            .filter(|k| clips[*k].load == DECOMPRESS_ON_LOAD && !switch.contains(*k))
            .collect();
        let mut total: i128 = held.iter().map(|k| clips[*k].pcm()).sum();
        let mut order: Vec<&Key> = held.iter().copied().filter(|k| switchable(k)).collect();
        order.sort_by(|a, b| {
            let ka = (i >= resident && base.contains(*a), -clips[*a].pcm(), *a);
            let kb = (i >= resident && base.contains(*b), -clips[*b].pcm(), *b);
            ka.cmp(&kb)
        });
        for k in order {
            if total <= budget {
                break;
            }
            switch.insert(k.clone());
            total -= clips[k].pcm();
        }
        left.insert(i, total);
    }
    (switch, left)
}

/// One file's clips by path id, and its PreloadData's references [(file, path id)].
type FileClips = (Vec<(i64, Clip)>, Vec<Key>);

/// The clips of one file and its PreloadData's references (audio.read).
fn read(f: &unityfs::DataFile, rel: &str) -> Result<FileClips> {
    let ext: Vec<String> =
        f.sf.externals
            .iter()
            .map(|e| {
                let p = String::from_utf8_lossy(&e.path).into_owned();
                p.rsplit('/').next().unwrap_or("").to_string()
            })
            .collect();
    let (mut clips, mut refs) = (Vec::new(), Vec::new());
    for (pid, class) in objects(f) {
        if class == "AudioClip" {
            let t = f.sf.read_typetree(pid)?;
            clips.push((
                pid,
                Clip {
                    name: text(&t, "m_Name")?,
                    load: int(&t, "m_LoadType")?,
                    fmt: int(&t, "m_CompressionFormat")?,
                    length: float(&t, "m_Length")?,
                    rate: int(&t, "m_Frequency")?,
                    channels: int(&t, "m_Channels")?,
                },
            ));
        } else if class == "PreloadData" {
            let t = f.sf.read_typetree(pid)?;
            if let Some(assets) = t.get("m_Assets").and_then(Value::as_array) {
                for p in assets {
                    let fid = int(p, "m_FileID")?;
                    if 0 <= fid && fid <= ext.len() as i128 {
                        let file = if fid == 0 {
                            rel.to_string()
                        } else {
                            ext[fid as usize - 1].clone()
                        };
                        let pid = int(p, "m_PathID")?;
                        refs.push((file, pid as i64));
                    }
                }
            }
        }
    }
    Ok((clips, refs))
}

type Survey = (HashMap<Key, Clip>, Vec<Key>, BTreeMap<i64, BTreeSet<Key>>);

/// Every file's clips (and their order) and what each scene keeps loaded.
fn survey(job: &Job, files: &[String]) -> Result<Survey> {
    let mut clips = HashMap::new();
    let mut order = Vec::new();
    let mut scenes: BTreeMap<i64, BTreeSet<Key>> = BTreeMap::new();
    let mut refs: BTreeMap<i64, Vec<Key>> = BTreeMap::new();
    for rel in files {
        let f = job.open(&job.path_in(rel), Default::default())?;
        let (c, r) = read(&f, rel)?;
        drop(f);
        let n = scene_of(rel);
        if let Some(n) = n {
            scenes.insert(n, c.iter().map(|(pid, _)| (rel.clone(), *pid)).collect());
            refs.insert(n, r);
        }
        for (pid, clip) in c {
            let k = (rel.clone(), pid);
            if clips.insert(k.clone(), clip).is_none() {
                order.push(k);
            }
        }
    }
    for (n, r) in refs {
        let s = scenes.get_mut(&n).unwrap();
        s.extend(r.into_iter().filter(|k| clips.contains_key(k)));
    }
    Ok((clips, order, scenes))
}

fn mib(clips: &HashMap<Key, Clip>, keys: &[&Key]) -> f64 {
    keys.iter().map(|k| clips[*k].pcm()).sum::<i128>() as f64 / (1u64 << 20) as f64
}

pub fn run(job: &mut Job) -> Result<()> {
    let c = section(&job.cfg, "audio", &defaults())?;
    let cfg = Json::Dict(
        defaults()
            .iter()
            .map(|(k, _)| (k.to_string(), opt(&c, k).clone()))
            .collect(),
    );
    let files = job.list("", |n| n.ends_with(".assets"));
    let st = match job.state.get("audio") {
        None => {
            let (clips, order, scenes) = survey(job, &files)?;
            let budget = match opt(&c, "pcm_budget_mb") {
                Json::Null => None,
                Json::Float(f) => Some((f * (1u64 << 20) as f64).trunc() as i128),
                v => Some(
                    v.as_int()
                        .ok_or_else(|| Error::Value("audio.pcm_budget_mb: not a number".into()))?
                        << 20,
                ),
            };
            let min_s = opt(&c, "compressed_min_s")
                .as_f64()
                .ok_or_else(|| Error::Value("audio.compressed_min_s: not a number".into()))?;
            let resident = opt(&c, "resident_scenes")
                .as_int()
                .ok_or_else(|| Error::Value("audio.resident_scenes: not an integer".into()))?
                as i64;
            let (switch, left) = plan(&clips, &scenes, budget, min_s, resident);
            let kept: Vec<&Key> = order
                .iter()
                .filter(|k| {
                    let x = &clips[*k];
                    x.load == DECOMPRESS_ON_LOAD && x.fmt == VORBIS && !switch.contains(*k)
                })
                .collect();
            // (the Python's dict of files follows its set's hash order, which changes from run
            // to run; here the files are in order)
            let mut by_file: BTreeMap<&str, Vec<i64>> = BTreeMap::new();
            for (rel, pid) in &switch {
                by_file.entry(rel).or_default().push(*pid);
            }
            let top = left
                .iter()
                .fold(None, |best: Option<(i64, i128)>, (&i, &t)| match best {
                    Some((_, b)) if b >= t => best,
                    _ => Some((i, t)),
                });
            let sw: Vec<&Key> = switch.iter().collect();
            let rule = match top {
                Some((scene, held)) => format!(
                    "budget {} MiB a scene, the most a scene holds {} MiB (scene {scene})",
                    json_g(opt(&c, "pcm_budget_mb")),
                    format_f(held as f64 / (1u64 << 20) as f64, 1)
                ),
                None => format!(
                    "every clip of {} s or longer",
                    json_g(opt(&c, "compressed_min_s"))
                ),
            };
            let summary = format!(
                "{} clips kept compressed in memory ({} MiB of PCM), {} decompressed on load as the game has them ({} MiB); {rule}",
                switch.len(),
                format_f(mib(&clips, &sw), 1),
                kept.len(),
                format_f(mib(&clips, &kept), 1),
            );
            let st = Json::Dict(vec![
                ("cfg".into(), cfg.clone()),
                (
                    "switch".into(),
                    Json::Dict(
                        by_file
                            .into_iter()
                            .map(|(rel, mut p)| {
                                p.sort();
                                (
                                    rel.to_string(),
                                    Json::List(
                                        p.into_iter().map(|x| Json::Int(x as i128)).collect(),
                                    ),
                                )
                            })
                            .collect(),
                    ),
                ),
                ("summary".into(), Json::Str(summary)),
            ]);
            job.state.set("audio", st.clone());
            job.save_state()?;
            st
        }
        Some(st) => {
            let st = st.clone();
            if st.get("cfg") != Some(&cfg) {
                job.log(&format!(
                    "audio: the clips were chosen with {} before; kept (files may be converted already)",
                    st.get("cfg").unwrap_or(&Json::Null).repr()
                ));
            }
            st
        }
    };
    let switch = st
        .get("switch")
        .ok_or_else(|| Error::Value("KeyError: 'switch'".into()))?;
    for rel in &files {
        let pids: BTreeSet<i128> = switch
            .get(rel)
            .and_then(Json::as_list)
            .map(|l| l.iter().filter_map(Json::as_int).collect())
            .unwrap_or_default();
        if pids.is_empty() || job.is_done("audio", rel) {
            continue;
        }
        let mut f = job.open(&job.path_in(rel), Default::default())?;
        for (pid, class) in objects(&f) {
            if class != "AudioClip" || !pids.contains(&(pid as i128)) {
                continue;
            }
            let mut t = f.sf.read_typetree(pid)?;
            if int(&t, "m_LoadType")? == DECOMPRESS_ON_LOAD {
                *super::field_mut(&mut t, "m_LoadType")? = Value::I64(COMPRESSED_IN_MEMORY as i64);
                f.sf.save_typetree(pid, &t)?;
            }
        }
        job.write_with(rel, |tmp| Ok(f.write_to(tmp, false)?))?;
        drop(f);
        job.mark_done("audio", rel)?;
    }
    let summary = st
        .get("summary")
        .ok_or_else(|| Error::Value("KeyError: 'summary'".into()))?
        .to_str();
    job.log(&format!("audio: {summary}"));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn clip(load: i128, length: f64, rate: i128) -> Clip {
        Clip {
            name: b"c".to_vec(),
            load,
            fmt: VORBIS,
            length,
            rate,
            channels: 2,
        }
    }

    #[test]
    fn pcm_rounds_half_to_even() {
        assert_eq!(clip(0, 0.5, 5).pcm(), 2 * 2 * 2); // 2.5 -> 2
        assert_eq!(clip(0, 0.5, 7).pcm(), 4 * 2 * 2); // 3.5 -> 4
    }

    #[test]
    fn plan_as_python() {
        let mut clips = HashMap::new();
        let k = |f: &str, p: i64| (f.to_string(), p);
        clips.insert(k("sharedassets0.assets", 1), clip(0, 60.0, 44100)); // resident, 10 MiB
        clips.insert(k("sharedassets2.assets", 1), clip(0, 30.0, 44100)); // 5 MiB
        clips.insert(k("sharedassets2.assets", 2), clip(0, 1.0, 44100)); // short
        clips.insert(k("resources.assets", 9), clip(0, 10.0, 44100)); // unused
        let mut scenes = BTreeMap::new();
        scenes.insert(0, BTreeSet::from([k("sharedassets0.assets", 1)]));
        scenes.insert(
            2,
            BTreeSet::from([k("sharedassets2.assets", 1), k("sharedassets2.assets", 2)]),
        );
        let (sw, left) = plan(&clips, &scenes, Some(12 << 20), 2.0, 2);
        // scene 2 holds 10 + 5 + 0.17 MiB: its own long clip goes first
        assert!(sw.contains(&k("sharedassets2.assets", 1)));
        assert!(!sw.contains(&k("sharedassets0.assets", 1)));
        // (a clip no scene uses: the length rule)
        assert!(sw.contains(&k("resources.assets", 9)));
        assert_eq!(left[&0], clips[&k("sharedassets0.assets", 1)].pcm());
        let (sw, left) = plan(&clips, &scenes, None, 2.0, 2);
        assert_eq!(sw.len(), 3);
        assert!(left.is_empty());
    }
}
