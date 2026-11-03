//! Ogg pages packed again: ww2ogg writes one page a packet (some 28 bytes a packet of a few
//! hundred: about a tenth of a Wwise Vorbis bank); this puts the audio packets into pages of up to
//! 4 KB, as Vorbis encoders do. The packets and their granule positions stay as they are; the
//! three header packets keep the pages the specification wants (the first alone on its page, the
//! audio starting on a fresh page).
use crate::rd::Result;

const PAGE: usize = 4096;

fn crc_table() -> [u32; 256] {
    let mut t = [0u32; 256];
    for (i, e) in t.iter_mut().enumerate() {
        let mut r = (i as u32) << 24;
        for _ in 0..8 {
            r = if r & 0x8000_0000 != 0 {
                (r << 1) ^ 0x04c1_1db7
            } else {
                r << 1
            };
        }
        *e = r;
    }
    t
}

/// The packets of an Ogg stream (one logical stream): (bytes, granule of the page it ends on),
/// and the stream's serial number.
pub fn packets(ogg: &[u8]) -> Result<(Vec<(Vec<u8>, u64)>, u32)> {
    let mut out = Vec::new();
    let mut cur = Vec::new();
    let (mut i, mut serial) = (0usize, 0u32);
    while i < ogg.len() {
        if i + 27 > ogg.len() || &ogg[i..i + 4] != b"OggS" {
            return Err(format!("no Ogg page at {i}"));
        }
        serial = u32::from_le_bytes(ogg[i + 14..i + 18].try_into().unwrap());
        let g = u64::from_le_bytes(ogg[i + 6..i + 14].try_into().unwrap());
        let ns = ogg[i + 26] as usize;
        let lace = ogg.get(i + 27..i + 27 + ns).ok_or("a truncated Ogg page")?;
        let mut p = i + 27 + ns;
        for &l in lace {
            let seg = ogg.get(p..p + l as usize).ok_or("a truncated Ogg page")?;
            cur.extend_from_slice(seg);
            p += l as usize;
            if l < 255 {
                out.push((std::mem::take(&mut cur), g));
            }
        }
        i = p;
    }
    if !cur.is_empty() {
        return Err("an unfinished Ogg packet at the end".into());
    }
    Ok((out, serial))
}

struct Writer {
    out: Vec<u8>,
    seq: u32,
    serial: u32,
    crc: [u32; 256],
}

impl Writer {
    /// Writes one page: its lacing values and data, the granule, the flags (1 continued, 2 first,
    /// 4 last).
    fn page(&mut self, lace: &[u8], data: &[u8], granule: u64, flags: u8) {
        let start = self.out.len();
        self.out.extend_from_slice(b"OggS\0");
        self.out.push(flags);
        self.out.extend_from_slice(&granule.to_le_bytes());
        self.out.extend_from_slice(&self.serial.to_le_bytes());
        self.out.extend_from_slice(&self.seq.to_le_bytes());
        self.out.extend_from_slice(&[0; 4]);
        self.out.push(lace.len() as u8);
        self.out.extend_from_slice(lace);
        self.out.extend_from_slice(data);
        let mut c = 0u32;
        for &b in &self.out[start..] {
            c = (c << 8) ^ self.crc[((c >> 24) as u8 ^ b) as usize];
        }
        self.out[start + 22..start + 26].copy_from_slice(&c.to_le_bytes());
        self.seq += 1;
    }

    /// Writes packets on pages of up to PAGE bytes (a packet may span pages); the last page gets
    /// the `last` flag. A page on which no packet ends has granule -1.
    fn pages(&mut self, packets: &[(Vec<u8>, u64)], first: bool, last: bool) {
        let (mut lace, mut data): (Vec<u8>, Vec<u8>) = (Vec::new(), Vec::new());
        let (mut granule, mut continued, mut first) = (u64::MAX, false, first);
        let mut flush = |w: &mut Writer,
                         lace: &mut Vec<u8>,
                         data: &mut Vec<u8>,
                         granule: &mut u64,
                         cont: bool,
                         end: bool| {
            let fl = cont as u8 | if first { 2 } else { 0 } | if end { 4 } else { 0 };
            w.page(lace, data, *granule, fl);
            first = false;
            lace.clear();
            data.clear();
            *granule = u64::MAX;
        };
        for (k, (p, g)) in packets.iter().enumerate() {
            let mut rest = &p[..];
            loop {
                let mut done = false;
                while lace.len() < 255 && data.len() < PAGE {
                    let n = rest.len().min(255);
                    lace.push(n as u8);
                    data.extend_from_slice(&rest[..n]);
                    rest = &rest[n..];
                    if n < 255 {
                        done = true;
                        granule = *g;
                        break;
                    }
                }
                let full = lace.len() == 255 || data.len() >= PAGE;
                if done {
                    if full {
                        flush(
                            self,
                            &mut lace,
                            &mut data,
                            &mut granule,
                            continued,
                            last && k + 1 == packets.len(),
                        );
                        continued = false;
                    }
                    break;
                }
                flush(self, &mut lace, &mut data, &mut granule, continued, false);
                continued = true;
            }
        }
        if !lace.is_empty() {
            flush(self, &mut lace, &mut data, &mut granule, continued, last);
        }
    }
}

/// The stream with its audio packets packed into pages of up to 4 KB.
pub fn repack(ogg: &[u8]) -> Result<Vec<u8>> {
    let (p, serial) = packets(ogg)?;
    if p.len() < 4 {
        return Ok(ogg.to_vec());
    }
    let mut w = Writer {
        out: Vec::with_capacity(ogg.len()),
        seq: 0,
        serial,
        crc: crc_table(),
    };
    let hdr = |x: &(Vec<u8>, u64)| (x.0.clone(), 0u64);
    w.pages(&[hdr(&p[0])], true, false);
    w.pages(&[hdr(&p[1]), hdr(&p[2])], false, false);
    w.pages(&p[3..], false, true);
    Ok(w.out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packets_survive_repacking() {
        // made-up packets: three headers, then audio of many sizes (one spans pages)
        let mut ps: Vec<(Vec<u8>, u64)> =
            vec![(vec![1; 30], 0), (vec![3; 300], 0), (vec![5; 3000], 0)];
        let mut g = 0;
        for i in 0..400usize {
            g += 64 + (i as u64 % 3) * 448;
            let n = [0, 1, 254, 255, 256, 510, 7000][i % 7] + i % 13;
            ps.push(((0..n).map(|k| (k + i) as u8).collect(), g));
        }
        let mut w = Writer {
            out: Vec::new(),
            seq: 0,
            serial: 7,
            crc: crc_table(),
        };
        for (k, p) in ps.iter().enumerate() {
            w.pages(std::slice::from_ref(p), k == 0, k + 1 == ps.len());
        }
        let one_a_page = w.out;
        let packed = repack(&one_a_page).unwrap();
        assert!(packed.len() < one_a_page.len());
        let (back, serial) = packets(&packed).unwrap();
        assert_eq!(serial, 7);
        assert_eq!(back.len(), ps.len());
        for (a, b) in back.iter().zip(&ps) {
            assert_eq!(a.0, b.0);
        }
        // every page's granule is that of its last finished packet, the last page's the stream's
        let mut i = 0;
        let mut pages = 0;
        while i < packed.len() {
            let ns = packed[i + 26] as usize;
            let len: usize = packed[i + 27..i + 27 + ns]
                .iter()
                .map(|&x| x as usize)
                .sum();
            let flags = packed[i + 5];
            if i + 27 + ns + len == packed.len() {
                assert_eq!(flags & 4, 4);
                assert_eq!(
                    u64::from_le_bytes(packed[i + 6..i + 14].try_into().unwrap()),
                    g
                );
            }
            assert!(len <= PAGE + 255 * 255);
            i += 27 + ns + len;
            pages += 1;
        }
        assert!(pages < ps.len() / 2, "{pages} pages");
        assert_eq!(super::super::wem::last_granule(&packed).unwrap(), g);
    }
}
