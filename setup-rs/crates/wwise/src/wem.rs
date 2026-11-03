//! A WEM (Wwise's RIFF media): its format, and its data as the player's pak holds it: Wwise Vorbis
//! rebuilt as standard Ogg Vorbis (ww2ogg's method, no decoding; its pages packed again, ogg.rs),
//! IMA ADPCM and PCM as they are.
use std::io::Cursor;

use crate::rd::{Rd, Result};

#[derive(Clone, Debug, Default)]
pub struct Wem {
    pub format: u16,
    pub channels: u16,
    pub rate: u32,
    pub block_align: u16,
    pub bits: u16,
    pub fmt_size: u32,
    /// the data chunk: offset, size
    pub data: (usize, usize),
    /// loop start, end (exclusive), in samples
    pub loop_: Option<(u32, u32)>,
    /// Vorbis: the sample count in the header
    pub vorbis_samples: Option<u32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Codec {
    Vorbis,
    Adpcm,
    Pcm,
}

impl Codec {
    pub fn name(self) -> &'static str {
        match self {
            Codec::Vorbis => "vorbis",
            Codec::Adpcm => "adpcm",
            Codec::Pcm => "pcm",
        }
    }
}

pub fn parse(b: &[u8]) -> Result<Wem> {
    let r = &mut Rd::new(b);
    if r.take(4)? != b"RIFF" {
        return Err("not a RIFF WEM (RIFX and others are not read)".into());
    }
    r.u32()?;
    if r.take(4)? != b"WAVE" {
        return Err("not a WAVE".into());
    }
    let mut w = Wem::default();
    let mut have_fmt = false;
    let mut have_data = false;
    while r.left() >= 8 {
        let id: [u8; 4] = r.take(4)?.try_into().unwrap();
        let size = r.u32()? as usize;
        let at = r.p;
        let body = if size <= r.left() {
            &b[at..at + size]
        } else {
            &b[at..]
        };
        match &id {
            b"fmt " => {
                let f = &mut Rd::new(body);
                w.format = f.u16()?;
                w.channels = f.u16()?;
                w.rate = f.u32()?;
                f.u32()?;
                w.block_align = f.u16()?;
                w.bits = f.u16()?;
                w.fmt_size = size as u32;
                if w.format == 0xFFFF && size == 0x42 {
                    let v = &mut Rd::new(body.get(0x18..).ok_or("a short fmt chunk")?);
                    w.vorbis_samples = Some(v.u32()?);
                }
                have_fmt = true;
            }
            b"vorb" => {
                w.vorbis_samples = Some(Rd::new(body).u32()?);
            }
            b"smpl" => {
                let s = &mut Rd::new(body);
                if body.len() >= 0x34 {
                    s.skip(0x1c)?;
                    let n = s.u32()?;
                    s.skip(4 + 4)?;
                    let kind = s.u32()?;
                    let (a, e) = (s.u32()?, s.u32()?);
                    if n == 1 && kind == 0 {
                        w.loop_ = Some((a, e.saturating_add(1)));
                    }
                }
            }
            b"data" => {
                if size > r.left() {
                    return Err(format!(
                        "data chunk of {size} bytes past the end ({} left): a prefetch?",
                        r.left()
                    ));
                }
                w.data = (at, size);
                have_data = true;
            }
            _ => {}
        }
        if size > r.left() {
            break;
        }
        r.skip(size)?;
    }
    if !have_fmt || !have_data {
        return Err("no fmt or data chunk".into());
    }
    if w.channels == 0 {
        return Err("no channels".into());
    }
    Ok(w)
}

impl Wem {
    pub fn codec(&self) -> Result<Codec> {
        match self.format {
            0xFFFF => Ok(Codec::Vorbis),
            0x0002 if self.bits == 4 && self.block_align as u32 == 0x24 * self.channels as u32 => {
                Ok(Codec::Adpcm)
            }
            0x0001 | 0xFFFE if self.bits == 16 => Ok(Codec::Pcm),
            f => Err(format!(
                "codec 0x{f:04x} ({} bits, block {}) is not one the player reads",
                self.bits, self.block_align
            )),
        }
    }

    /// Samples per channel: from the header (Vorbis) or the data's size.
    pub fn samples(&self) -> Result<u64> {
        let size = self.data.1 as u64;
        let ch = self.channels as u64;
        Ok(match self.codec()? {
            Codec::Vorbis => self
                .vorbis_samples
                .ok_or("no sample count in the Vorbis header")? as u64,
            Codec::Adpcm => {
                let ba = self.block_align as u64;
                let mut n = size / ba * 64;
                let rest = size % ba;
                if rest > 4 * ch {
                    n += (rest - 4 * ch) * 2 / ch;
                }
                n
            }
            Codec::Pcm => size / (2 * ch),
        })
    }
}

/// The media's bytes as the pak holds them. Vorbis needs the codebook libraries of the game's
/// Wwise (codebooks.rs); with several, the first whose stream decodes (its headers and first
/// packets) is taken.
pub fn convert(
    wem: &[u8],
    w: &Wem,
    out: &mut Vec<u8>,
    libs: &[crate::codebooks::Library],
) -> Result<()> {
    out.clear();
    match w.codec()? {
        Codec::Adpcm | Codec::Pcm => {
            out.extend_from_slice(&wem[w.data.0..w.data.0 + w.data.1]);
            Ok(())
        }
        Codec::Vorbis => {
            let mut last = String::from("no codebooks (the game's Wwise library)");
            for lib in libs {
                let cb =
                    ww2ogg::CodebookLibrary::from_bytes(&lib.packed).map_err(|e| e.to_string())?;
                out.clear();
                let r = ww2ogg::WwiseRiffVorbis::new(Cursor::new(wem), cb)
                    .and_then(|mut c| c.generate_ogg(&mut *out));
                match r {
                    Ok(()) => {
                        *out = crate::ogg::repack(out)?;
                        if libs.len() == 1 || plays(out, 16) {
                            return Ok(());
                        }
                        last = format!("does not decode with the codebooks of {}", lib.source);
                    }
                    Err(e) => last = e.to_string(),
                }
            }
            Err(format!("Vorbis: {last}"))
        }
    }
}

/// Whether a Vorbis stream's headers and first `n` audio packets decode (lewton).
fn plays(ogg: &[u8], n: usize) -> bool {
    use lewton::{audio, header};
    let Ok((p, _)) = crate::ogg::packets(ogg) else {
        return false;
    };
    if p.len() < 3 {
        return false;
    }
    let Ok(id) = header::read_header_ident(&p[0].0) else {
        return false;
    };
    let Ok(setup) =
        header::read_header_setup(&p[2].0, id.audio_channels, (id.blocksize_0, id.blocksize_1))
    else {
        return false;
    };
    let mut pwr = audio::PreviousWindowRight::new();
    p[3..]
        .iter()
        .take(n)
        .all(|(pk, _)| audio::read_audio_packet(&id, &setup, pk, &mut pwr).is_ok())
}

/// Decodes an Ogg Vorbis stream's packets one after another (lewton) and gives the samples per
/// channel they make before a decoder trims the end to the stream's length, its channel count, its
/// last page's granule position (that length) and a long block's size: Wwise's streams end with up
/// to a long block past their length, which a decoder drops (libvorbis and stb_vorbis give exactly
/// the length; lewton's own Ogg reader trims by its own packet split, which differs at a long block
/// followed by short ones, hence the packets here).
pub fn decode_count(ogg: &[u8]) -> Result<(u64, u16, u64, u64)> {
    use lewton::{audio, header};
    let (p, _) = crate::ogg::packets(ogg)?;
    if p.len() < 3 {
        return Err("no Vorbis headers".into());
    }
    let e = |x: &dyn std::fmt::Display| format!("lewton: {x}");
    let id = header::read_header_ident(&p[0].0).map_err(|x| e(&x))?;
    header::read_header_comment(&p[1].0).map_err(|x| e(&x))?;
    let setup =
        header::read_header_setup(&p[2].0, id.audio_channels, (id.blocksize_0, id.blocksize_1))
            .map_err(|x| e(&x))?;
    let mut pwr = audio::PreviousWindowRight::new();
    let mut n = 0u64;
    for (pk, _) in &p[3..] {
        let d: Vec<Vec<i16>> =
            audio::read_audio_packet(&id, &setup, pk, &mut pwr).map_err(|x| e(&x))?;
        n += d.first().map_or(0, |c| c.len() as u64);
    }
    Ok((
        n,
        id.audio_channels as u16,
        last_granule(ogg)?,
        1u64 << id.blocksize_1,
    ))
}

/// The granule position of an Ogg stream's last page.
pub fn last_granule(ogg: &[u8]) -> Result<u64> {
    let (mut i, mut g) = (0usize, None);
    while i + 27 <= ogg.len() {
        if &ogg[i..i + 4] != b"OggS" {
            return Err(format!("no Ogg page at {i}"));
        }
        let ns = ogg[i + 26] as usize;
        let len: usize = ogg
            .get(i + 27..i + 27 + ns)
            .ok_or("a truncated Ogg page")?
            .iter()
            .map(|&x| x as usize)
            .sum();
        g = Some(u64::from_le_bytes(ogg[i + 6..i + 14].try_into().unwrap()));
        i += 27 + ns + len;
    }
    if i != ogg.len() {
        return Err("a truncated Ogg stream".into());
    }
    g.ok_or_else(|| "no Ogg pages".into())
}
