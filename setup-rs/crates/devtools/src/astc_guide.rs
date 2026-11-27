//! `pmu-dev astc-guide <data> <guide> --config game.json [--set textures.k=v] [--target fast|medium|thorough]
//! [--tol 0] [--only REGEX]` (was `python -m pmunity astc-guide`, convert/pmunity/astc_guide.py):
//! an ASTC guide file, the encoder's effort chosen per block ahead of time, on a PC.
//!
//! The texture step's guided mode writes the blocks that need no search itself and hands the others
//! (the "hard" blocks) to astc-encoder at one preset. Most hard blocks do not need all of that search:
//! for each one a smaller search (a faster preset, or one or two partitions) finds a block as good as
//! the target preset's. A guide holds that choice, a tier number per hard block (per texture level:
//! the number of its hard blocks, a CRC of their positions and a byte per block, zlib), and the
//! handheld encodes each hard block at its tier only. It holds no pixels.
//!
//! Made by running pmu's texture step itself (the same files, order, decoding and scaling) with a
//! recorder (texcodec::guide::Recorder) that encodes each level's hard blocks at every tier, measures
//! each block's error against its pixels and keeps the cheapest tier that reaches the target's. The
//! file is byte for byte what astc_guide.py wrote for the same data and settings.
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use convert::pyjson::{self, Json};
use texcodec::guide::{Recorder, KEYS, TIERS};

const MAGIC: &[u8] = b"PMUNITY-ASTC-GUIDE\n";
const USAGE: &str = "usage: pmu-dev astc-guide <data> <guide> --config CONFIG [--set textures.<key>=<json value>] \
[--target fast|medium|thorough] [--tol T] [--only REGEX]";

/// json.dumps(..., sort_keys=True): every dict's keys sorted
fn sorted(v: Json) -> Json {
    match v {
        Json::Dict(mut d) => {
            d.sort_by(|a, b| a.0.cmp(&b.0));
            Json::Dict(d.into_iter().map(|(k, v)| (k, sorted(v))).collect())
        }
        Json::List(l) => Json::List(l.into_iter().map(sorted).collect()),
        x => x,
    }
}

// (zlib's compress2 and compressBound, from libz-sys's static zlib, which binds neither)
use libz_sys as _;
extern "C" {
    fn compress2(dest: *mut u8, dest_len: *mut std::os::raw::c_ulong, source: *const u8, source_len: std::os::raw::c_ulong, level: std::os::raw::c_int) -> std::os::raw::c_int;
    fn compressBound(source_len: std::os::raw::c_ulong) -> std::os::raw::c_ulong;
}

/// zlib.compress(data, 9): zlib's own deflate (crates/apk's reason: zlib-rs's output differs)
fn zlib9(data: &[u8]) -> Result<Vec<u8>, String> {
    let mut len = unsafe { compressBound(data.len() as _) };
    let mut out = vec![0u8; len as usize];
    // SAFETY: out holds compressBound bytes; len is set to what was written.
    let rc = unsafe { compress2(out.as_mut_ptr(), &mut len, data.as_ptr(), data.len() as _, 9) };
    if rc != 0 {
        return Err(format!("zlib compress2: {rc}"));
    }
    out.truncate(len as usize);
    Ok(out)
}

fn tiers_json() -> Json {
    Json::List(
        TIERS
            .iter()
            .map(|(p, l)| {
                let mut kw = vec![];
                if let Some(i) = l.index2 {
                    kw.push(("tune_2partition_index_limit".to_string(), Json::Int(i as i128)));
                }
                if let Some(p) = l.partitions {
                    kw.push(("tune_partition_count_limit".to_string(), Json::Int(p as i128)));
                }
                Json::List(vec![Json::Str(p.to_string()), Json::Dict(kw)])
            })
            .collect(),
    )
}

/// Recorder.save: the header (JSON, sorted keys) and each level's zlib'd codes in key order.
pub fn save(path: &Path, rec: &Recorder, c: &Json) -> Result<u64, String> {
    let mut index = vec![];
    let mut blobs: Vec<u8> = vec![];
    for (k, (n, crc, codes)) in &rec.entries {
        let z = zlib9(codes)?;
        index.push((k.clone(), Json::List(vec![Json::Int(blobs.len() as i128), Json::Int(z.len() as i128), Json::Int(*n as i128), Json::Int(*crc as i128)])));
        blobs.extend_from_slice(&z);
    }
    let settings = Json::Dict(KEYS.iter().map(|k| (k.to_string(), c.get(k).cloned().unwrap_or(Json::Null))).collect());
    let head = sorted(Json::Dict(vec![
        ("version".into(), Json::Int(1)),
        ("target".into(), Json::Str(rec.target.clone())),
        ("tol".into(), Json::Float(rec.tol)),
        ("settings".into(), settings),
        ("tiers".into(), tiers_json()),
        ("index".into(), Json::Dict(index)),
    ]))
    .dumps();
    let mut out = MAGIC.to_vec();
    out.extend_from_slice(&(head.len() as u32).to_le_bytes());
    out.extend_from_slice(head.as_bytes());
    out.extend_from_slice(&blobs);
    let tmp = PathBuf::from(format!("{}.tmp", path.display()));
    std::fs::write(&tmp, &out).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(out.len() as u64)
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let (mut config, mut target, mut tol, mut only) = (None, "medium".to_string(), 0.0f64, None);
    let mut sets = vec![];
    let mut pos = vec![];
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let (k, inline) = match a.split_once('=') {
            Some((k, v)) if k.starts_with("--") => (k.to_string(), Some(v.to_string())),
            _ => (a.clone(), None),
        };
        let mut val = || inline.clone().or_else(|| it.next().cloned()).ok_or_else(|| format!("{USAGE}\n{k}: expected one argument"));
        match k.as_str() {
            "--config" => config = Some(val()?),
            "--set" => sets.push(val()?),
            "--target" => target = val()?,
            "--tol" => tol = val()?.parse().map_err(|_| format!("{USAGE}\n--tol: a number"))?,
            "--only" => only = Some(val()?),
            "-h" | "--help" => {
                println!("{USAGE}");
                return Ok(0);
            }
            x if x.starts_with("--") => return Err(format!("{USAGE}\nunrecognized arguments: {x}")),
            _ => pos.push(a.clone()),
        }
    }
    let ([data, guide], Some(config)) = (&pos[..], config) else { return Err(USAGE.into()) };
    let text = std::fs::read_to_string(&config).map_err(|e| format!("{config}: {e}"))?;
    let mut cfg = pyjson::loads(&text).map_err(|e| format!("{config}: {e:?}"))?;
    cfg.setdefault("version", Json::Int(1));
    for kv in &sets {
        let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
        let v = pyjson::loads(v).unwrap_or_else(|_| Json::Str(v.to_string()));
        let (sec, key) = k.split_once('.').unwrap_or((k, ""));
        cfg.setdefault(sec, Json::dict());
        if let Some(s) = cfg.get_mut(sec) {
            s.set(key, v);
        }
    }
    cfg.setdefault("textures", Json::dict());
    if let Some(tc) = cfg.get_mut("textures") {
        tc.set("mode", Json::Str("guided".into()));
        tc.pop("guide");
    }
    let rec = Arc::new(Mutex::new(Recorder::new(&target, tol).map_err(|e| e.to_string())?));
    let work = std::env::temp_dir().join(format!("astc-guide-{}", std::process::id()));
    let c = {
        let mut job = convert::job::Job::new(Path::new(data), &work, cfg).map_err(|e| e.to_string())?;
        let r = convert::steps::textures::run_recording(&mut job, &convert::steps::Args { only }, rec.clone());
        drop(job);
        let _ = std::fs::remove_dir_all(&work);
        r.map_err(|e| e.to_string())?
    };
    let rec = rec.lock().map_err(|_| "recorder poisoned".to_string())?;
    let size = save(Path::new(guide), &rec, &c)?;
    let total = rec.picked.iter().sum::<u64>().max(1);
    let parts: Vec<String> = TIERS
        .iter()
        .zip(rec.picked)
        .map(|((p, l), k)| {
            let lim = if *l != texcodec::guide::Limits::NONE { format!("/{}p", l.partitions.map_or("None".to_string(), |p| p.to_string())) } else { String::new() };
            format!("{p}{lim} {} %", pyjson::format_f(k as f64 * 100.0 / total as f64, 1))
        })
        .collect();
    eprintln!("astc-guide: {} texture levels, {total} hard blocks, {} KB; tiers: {}", rec.entries.len(), size >> 10, parts.join(", "));
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_and_blobs() {
        assert_eq!(
            tiers_json().dumps(),
            "[[\"fastest\", {\"tune_partition_count_limit\": 1}], [\"fast\", {\"tune_partition_count_limit\": 1}], [\"fastest\", {}], \
[\"fast\", {\"tune_2partition_index_limit\": 8, \"tune_partition_count_limit\": 2}], [\"fast\", {\"tune_partition_count_limit\": 2}], \
[\"medium\", {\"tune_partition_count_limit\": 1}], [\"fast\", {}], [\"medium\", {\"tune_2partition_index_limit\": 16, \"tune_partition_count_limit\": 2}], \
[\"medium\", {\"tune_partition_count_limit\": 2}], [\"medium\", {}], [\"thorough\", {}]]"
        );
        // zlib.compress(bytes([2, 0, 9]), 9) in Python
        assert_eq!(zlib9(&[2, 0, 9]).unwrap(), [120, 218, 99, 98, 224, 4, 0, 0, 18, 0, 12]);
        let mut rec = Recorder::new("medium", 0.0).unwrap();
        let rest: Vec<u8> = [1u32, 4].iter().flat_map(|v| v.to_le_bytes()).collect();
        let mut errs = vec![vec![100i64, 5]; 11];
        errs[9] = vec![50, 5];
        errs[0] = vec![60, 4];
        assert_eq!(rec.choose("a|1|0", &rest, &errs), vec![9, 0]);
        let d = std::env::temp_dir().join(format!("pmu-dev-guide-{}", std::process::id()));
        let c = Json::Dict(vec![("scale".into(), Json::Float(1.0)), ("mode".into(), Json::Str("guided".into()))]);
        save(&d, &rec, &c).unwrap();
        let b = std::fs::read(&d).unwrap();
        let _ = std::fs::remove_file(&d);
        let n = u32::from_le_bytes(b[19..23].try_into().unwrap()) as usize;
        let head = std::str::from_utf8(&b[23..23 + n]).unwrap();
        assert!(head.starts_with("{\"index\": {\"a|1|0\": [0, "), "{head}");
        assert!(head.contains("\"settings\": {\"block\": null, ") && head.contains("\"scale\": 1.0") && head.ends_with("\"target\": \"medium\", \"tiers\": [[\"fastest\", {\"tune_partition_count_limit\": 1}], [\"fast\", {\"tune_partition_count_limit\": 1}], [\"fastest\", {}], [\"fast\", {\"tune_2partition_index_limit\": 8, \"tune_partition_count_limit\": 2}], [\"fast\", {\"tune_partition_count_limit\": 2}], [\"medium\", {\"tune_partition_count_limit\": 1}], [\"fast\", {}], [\"medium\", {\"tune_2partition_index_limit\": 16, \"tune_partition_count_limit\": 2}], [\"medium\", {\"tune_partition_count_limit\": 2}], [\"medium\", {}], [\"thorough\", {}]], \"tol\": 0.0, \"version\": 1}"), "{head}");
    }
}
