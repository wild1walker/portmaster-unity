//! The video step's work on one clip (convert's `video`): a WebM clip's VP8 track decoded,
//! halved until it is at most `max_height` tall (2x2 means), encoded again with libvpx (constant
//! quality, key frames where the clip had them, several token partitions so a player can decode
//! on more than one thread), and written back with every other track's blocks as they were.
//! Decoding and encoding run on two threads.
pub mod scale;
pub mod vpx;
pub mod webm;

use std::sync::mpsc;
use webm::{Item, Part, WebM};

/// An 8-bit 4:2:0 picture, planes without padding (chroma ceil(w / 2) x ceil(h / 2)).
#[derive(Clone, Debug)]
pub struct Picture {
    pub w: usize,
    pub h: usize,
    pub y: Vec<u8>,
    pub u: Vec<u8>,
    pub v: Vec<u8>,
}

/// How a clip is made smaller.
#[derive(Clone, Copy, Debug)]
pub struct Settings {
    /// halve while the picture is taller than this
    pub max_height: u64,
    pub encode: vpx::EncodeSettings,
}

/// What became of a clip.
#[derive(Clone, Debug, PartialEq)]
pub struct Outcome {
    pub from: (u64, u64),
    pub to: (u64, u64),
    pub frames: usize,
}

/// The size a picture of w x h gets (halved while taller than max_height).
pub fn target(w: u64, h: u64, max_height: u64) -> (u64, u64) {
    let (mut w, mut h) = (w, h);
    while h > max_height && h > 1 {
        w = w.div_ceil(2);
        h = h.div_ceil(2);
    }
    (w, h)
}

/// The clip made smaller (None: its VP8 picture is not taller than max_height, or it has none).
pub fn reencode(clip: &[u8], s: &Settings) -> Result<Option<(Vec<u8>, Outcome)>, String> {
    let mut w = WebM::parse(clip)?;
    let video: Vec<(u64, (u64, u64))> = w
        .tracks()
        .iter()
        .filter(|t| t.kind == 1 && t.codec == "V_VP8")
        .filter_map(|t| t.size().map(|sz| (t.number, sz)))
        .collect();
    let [(track, (vw, vh))] = video[..] else {
        return Ok(None);
    };
    let (tw, th) = target(vw, vh, s.max_height);
    if (tw, th) == (vw, vh) {
        return Ok(None);
    }
    let halvings = {
        let (mut n, mut h) = (0, vh);
        while h > th {
            h = h.div_ceil(2);
            n += 1;
        }
        n
    };
    // the video blocks in file order, with their times (ms) and key flags
    let scale_ns = w.timecode_scale;
    let mut frames: Vec<(Vec<u8>, i64, bool)> = Vec::new();
    for p in &w.parts {
        if let Part::Cluster(c) = p {
            for it in &c.items {
                if let Some(k) = it.block().filter(|k| k.track == track) {
                    if k.flags & 0x06 != 0 {
                        return Err("video: a laced VP8 block".into());
                    }
                    let t = (c.timecode as i64 + k.rel as i64) * scale_ns as i64 / 1_000_000;
                    let key = matches!(it, Item::Simple(_)) && k.flags & 0x80 != 0
                        || matches!(it, Item::Group(_, rest) if !webm::has_child(rest, webm::ID_REFERENCEBLOCK));
                    frames.push((k.payload.clone(), t, key));
                }
            }
        }
    }
    let n = frames.len();
    let times: Vec<i64> = frames.iter().map(|f| f.1).collect();
    // decoding on this thread, encoding on another; the encoder's packets come back in order
    let (tx, rx) = mpsc::sync_channel::<(Option<Picture>, i64, u64, bool)>(4);
    let enc_settings = s.encode;
    let encoder = std::thread::spawn(move || -> Result<Vec<Option<webm::Block>>, String> {
        let mut enc: Option<vpx::Encoder> = None;
        let mut out = Vec::new();
        for (pic, t, dur, key) in rx {
            let Some(pic) = pic else {
                out.push(None); // a frame that showed no picture (alt-ref): its block goes
                continue;
            };
            let e = match &mut enc {
                Some(e) => e,
                None => enc.insert(vpx::Encoder::new(pic.w, pic.h, &enc_settings)?),
            };
            let pk = e.encode(&pic, t, dur, key)?;
            if pk.len() != 1 {
                return Err(format!(
                    "video: the encoder gave {} frames for one picture",
                    pk.len()
                ));
            }
            let p = &pk[0];
            out.push(Some(webm::Block {
                track: 0,
                rel: 0,
                flags: if p.key { 0x80 } else { 0 },
                payload: p.data.clone(),
            }));
        }
        if let Some(e) = &mut enc {
            if !e.finish()?.is_empty() {
                return Err("video: the encoder held frames back".into());
            }
        }
        Ok(out)
    });
    let mut dec = vpx::Decoder::new(1)?;
    let mut size = None;
    let feed = (|| -> Result<(), String> {
        for (i, (data, t, key)) in frames.iter().enumerate() {
            let pic = dec.decode(data)?.map(|mut p| {
                for _ in 0..halvings {
                    p = scale::half(&p);
                }
                p
            });
            if let Some(p) = &pic {
                size.get_or_insert((p.w as u64, p.h as u64));
            }
            let dur = times.get(i + 1).map_or(33, |&nt| (nt - t).max(1)) as u64;
            if tx.send((pic, *t, dur, *key)).is_err() {
                break; // the encoder stopped: its error is below
            }
        }
        Ok(())
    })();
    drop(tx);
    let encoded = encoder
        .join()
        .map_err(|_| "video: the encoder thread panicked")??;
    feed?;
    if encoded.len() != n {
        return Err(format!("video: {} of {n} frames encoded", encoded.len()));
    }
    let (ow, oh) = size.ok_or("video: no picture decoded")?;
    // the new frames into the blocks they came from (as SimpleBlocks); a frame without a
    // picture leaves its block out
    let mut next = encoded.into_iter();
    for p in w.parts.iter_mut() {
        if let Part::Cluster(c) = p {
            let mut items = Vec::with_capacity(c.items.len());
            for it in c.items.drain(..) {
                match it.block() {
                    Some(k) if k.track == track => {
                        if let Some(mut nb) = next.next().flatten() {
                            nb.track = k.track;
                            nb.rel = k.rel;
                            items.push(Item::Simple(nb));
                        }
                    }
                    _ => items.push(it),
                }
            }
            c.items = items;
        }
    }
    for t in w.tracks_mut() {
        if t.number == track {
            t.set_size(ow, oh)?;
        }
    }
    Ok(Some((
        w.write(),
        Outcome {
            from: (vw, vh),
            to: (ow, oh),
            frames: n,
        },
    )))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn target_sizes() {
        assert_eq!(target(1920, 1080, 720), (960, 540));
        assert_eq!(target(1920, 988, 720), (960, 494));
        assert_eq!(target(1198, 1080, 720), (599, 540));
        assert_eq!(target(1280, 720, 720), (1280, 720));
        assert_eq!(target(1920, 1080, 400), (480, 270));
    }
}
