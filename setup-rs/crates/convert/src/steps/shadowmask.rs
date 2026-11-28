//! Step `shadowmask` (pmu's own, exact): baked shadowmask lightmaps stored as ARGB4444 whose occlusion is in one
//! channel (a scene with one mixed light: its occlusion mask channel 0, red; green, blue and alpha 0 in every texel)
//! become R8, half the memory. A shadowmask is only ever read through `dot(mask, unity_OcclusionMaskSelector)` (the
//! selector one-hot on the light's channel; deferred passes copy it to a G-buffer target that deferred lighting reads
//! the same way), so the channels R8 does not keep (green and blue 0 either way, alpha 0 -> 1) drop out, and red is the
//! same value: a 4-bit n is n/15, its R8 byte n*17 is n*17/255 = n/15. Every mip is converted the same way (the
//! data's own mips, texel by texel). A texture with any texel's other channels not 0 is left as it is (logged).
//! Bomb Rush Cyberfunk: 2048^2 and 4096^2 shadowmasks of 10.7 and 42.7 MB, 22 - 85 MB a stage on a 1 GB handheld.
//!
//!   names   a regex on the texture's m_Name (default "_comp_shadowmask$", Unity's name for a baked shadowmask)
use super::{edits, objects, section};
use crate::error::{Error, Result};
use crate::job::Job;
use crate::pyjson::Json;
use unityfs::Value;

pub const STEP: &str = "shadowmask";

const ARGB4444: i64 = 2;
const R8: i64 = 63;

/// A field's new integer value of the old one's kind.
fn same_kind(old: &Value, n: i64) -> Value {
    match old {
        Value::I32(_) => Value::I32(n as i32),
        Value::U32(_) => Value::U32(n as u32),
        Value::I64(_) => Value::I64(n),
        Value::U64(_) => Value::U64(n as u64),
        _ => Value::I32(n as i32),
    }
}

fn int(t: &Value, k: &str) -> i64 {
    t.get(k).and_then(Value::as_int).map_or(0, |v| v as i64)
}

/// The R8 bytes of ARGB4444 texels (little-endian u16: A 12-15, R 8-11, G 4-7, B 0-3), or None when a texel has a
/// non-zero alpha, green or blue (then R8 would not be exact).
pub fn to_r8(px: &[u8]) -> Option<Vec<u8>> {
    if px.len() % 2 != 0 {
        return None;
    }
    let mut out = Vec::with_capacity(px.len() / 2);
    for p in px.chunks_exact(2) {
        let v = u16::from_le_bytes([p[0], p[1]]);
        if v & 0xF0FF != 0 {
            return None;
        }
        out.push(((v >> 8) & 15) as u8 * 17);
    }
    Some(out)
}

pub fn run(job: &mut Job) -> Result<()> {
    let c = section(&job.cfg, STEP, &[("names", Json::Str("_comp_shadowmask$".into()))])?;
    let rx = regex::Regex::new(c.get("names").map(Json::to_str).unwrap_or_default().as_str())
        .map_err(|e| Error::Value(format!("{STEP}.names: {e}")))?;
    let files: Vec<String> = edits::data_files(&job.src)
        .into_iter()
        .filter(|rel| edits::unity_file(&job.path_in(rel)).unwrap_or(false))
        .collect();
    let (mut n, mut before, mut after, mut kept) = (0usize, 0u64, 0u64, Vec::new());
    for rel in &files {
        if job.is_done(STEP, rel) {
            continue;
        }
        let o = unityfs::Options { all_serialized: true, ..Default::default() };
        let Ok(mut f) = job.open(&job.path_in(rel), o) else {
            job.mark_done(STEP, rel)?;
            continue;
        };
        let mut changed = 0;
        for k in 0..f.file_count() {
            f.select(k)?;
            for (pid, class) in objects(&f) {
                if class != "Texture2D" {
                    continue;
                }
                let Ok(mut t) = f.sf.read_typetree(pid) else { continue };
                if int(&t, "m_TextureFormat") != ARGB4444 {
                    continue;
                }
                let name = t.get("m_Name").and_then(Value::as_str).unwrap_or("").to_string();
                if !rx.is_match(&name) {
                    continue;
                }
                let size = int(&t, "m_CompleteImageSize").max(0) as usize * int(&t, "m_ImageCount").max(1) as usize;
                let mut px = t.get("image data").and_then(Value::as_bytes).map(<[u8]>::to_vec).unwrap_or_default();
                if px.len() < size {
                    let Some(sd) = t.get("m_StreamData") else { continue };
                    let path = sd.get("path").and_then(Value::as_str).unwrap_or("").to_string();
                    let off = sd.get("offset").and_then(Value::as_u64).unwrap_or(0);
                    px = match f.read_node(&path, off, size) {
                        Ok(b) if b.len() == size => b,
                        _ => {
                            kept.push(format!("{rel}: {name}: its data not read"));
                            continue;
                        }
                    };
                }
                let Some(r8) = to_r8(&px[..size]) else {
                    kept.push(format!("{rel}: {name}: alpha, green or blue not 0 in some texel"));
                    continue;
                };
                before += size as u64;
                after += r8.len() as u64;
                let half = (int(&t, "m_CompleteImageSize") / 2) as i64;
                let old = t.get("m_TextureFormat").cloned().unwrap_or(Value::I32(0));
                let _ = t.set("m_TextureFormat", same_kind(&old, R8));
                let old = t.get("m_CompleteImageSize").cloned().unwrap_or(Value::U32(0));
                let _ = t.set("m_CompleteImageSize", same_kind(&old, half));
                let _ = t.set("image data", Value::Bytes(r8));
                if let Some(sd) = t.get_mut("m_StreamData") {
                    for (k, v) in [("offset", 0i64), ("size", 0)] {
                        if let Some(old) = sd.get(k).cloned() {
                            let _ = sd.set(k, same_kind(&old, v));
                        }
                    }
                    let _ = sd.set("path", Value::str(""));
                }
                f.sf.save_typetree(pid, &t)?;
                changed += 1;
            }
        }
        if changed > 0 {
            job.write_with(rel, |tmp| Ok(f.write_to(tmp, false)?))?;
            n += changed;
        }
        drop(f);
        job.mark_done(STEP, rel)?;
    }
    for k in &kept {
        job.log(&format!("{STEP}: kept: {k}"));
    }
    job.log(&format!(
        "{STEP}: {n} shadowmasks ARGB4444 -> R8 ({:.1} -> {:.1} MB)",
        before as f64 / 1048576.0,
        after as f64 / 1048576.0
    ));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::to_r8;

    #[test]
    fn red_only_is_exact() {
        // red n in bits 8-11 -> n*17; n/15 == n*17/255 for every n
        let px: Vec<u8> = (0u16..16).flat_map(|n| (n << 8).to_le_bytes()).collect();
        let r = to_r8(&px).unwrap();
        for n in 0..16u32 {
            assert_eq!(r[n as usize] as u32, n * 17);
            assert_eq!(n as f64 / 15.0, (n * 17) as f64 / 255.0);
        }
    }

    #[test]
    fn other_channels_refuse() {
        for bad in [0x1000u16, 0x0010, 0x0001] {
            assert!(to_r8(&bad.to_le_bytes()).is_none());
        }
        assert!(to_r8(&[0u8]).is_none());
    }
}
