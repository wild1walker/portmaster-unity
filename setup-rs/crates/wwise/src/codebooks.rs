//! Wwise's Vorbis codebooks, read from the game's own Wwise library. Wwise Vorbis media leave out
//! the codebooks of their setup header and name them by index in a library the engine carries:
//! one block of codebooks in Wwise's packed form (the layout ww2ogg reads), back to back, in the
//! library's read-only data (Wwise 2017.2's libAkSoundEngine.so: 598 codebooks, about 72 KB, with
//! an array of pointers to them elsewhere). Nothing of Wwise's is in this repository: the setup
//! finds the block in the copy of the game the player has.
//!
//! The block is found by its structure, in any binary (ELF, PE, Mach-O alike): every packed
//! codebook is checked as a Vorbis codebook (dimensions and entries set, codeword lengths that
//! make a complete Huffman tree, the value lookup's size) and must take exactly `bits / 8 + 1`
//! bytes, the next one starting there; a run of at least `MIN_RUN` of them is a library. The
//! codeword lengths' tree is what tells codebooks from other data: Wwise 2017.2's libraries have
//! no other run of `MIN_RUN`, and none of 20 starts in the 4 KB before their block.
use std::path::{Path, PathBuf};

/// The fewest codebooks back to back taken for a library.
pub const MIN_RUN: usize = 64;

/// A codebook library: the codebooks in ww2ogg's packed layout (the codebooks, then each one's
/// offset as u32, then the offset of that table), where it was found and its SHA-256.
#[derive(Clone, Debug)]
pub struct Library {
    pub packed: Vec<u8>,
    pub count: usize,
    pub source: String,
    pub offset: usize,
    pub sha256: String,
}

struct Bits<'a> {
    d: &'a [u8],
    bit: usize,
}

impl Bits<'_> {
    fn get(&mut self, k: u32) -> Option<u32> {
        let mut v = 0u32;
        for i in 0..k {
            let byte = *self.d.get(self.bit >> 3)?;
            v |= ((byte >> (self.bit & 7)) as u32 & 1) << i;
            self.bit += 1;
        }
        Some(v)
    }
}

fn ilog(v: u32) -> u32 {
    32 - v.leading_zeros()
}

/// Vorbis's `_book_maptype1_quantvals`: the largest n with n^dims <= entries.
fn quantvals(entries: u32, dims: u32) -> Option<u64> {
    let bits = ilog(entries);
    let mut vals = (entries >> ((bits - 1) * (dims - 1) / dims)) as u64;
    for _ in 0..64 {
        let (mut acc, mut acc1) = (1u64, 1u64);
        for _ in 0..dims {
            acc = acc.saturating_mul(vals);
            acc1 = acc1.saturating_mul(vals + 1);
        }
        if acc <= entries as u64 && acc1 > entries as u64 {
            return Some(vals);
        }
        if acc > entries as u64 {
            vals = vals.checked_sub(1)?;
        } else {
            vals += 1;
        }
    }
    None
}

/// The size in bytes of a valid packed codebook at `p`, or None.
pub fn codebook_len(d: &[u8], p: usize) -> Option<usize> {
    let b = &mut Bits { d, bit: p * 8 };
    let dims = b.get(4)?;
    let entries = b.get(14)?;
    if dims == 0 || entries == 0 {
        return None;
    }
    // the codeword lengths (stored minus one) must fill a Huffman tree exactly: the sum of
    // 2^(32 - length) is 2^32 (one used entry alone is allowed)
    const FULL: u64 = 1 << 32;
    let (mut kraft, mut used) = (0u64, 0u32);
    if b.get(1)? == 1 {
        let mut len = b.get(5)? + 1;
        let mut cur = 0u32;
        while cur < entries {
            let n = b.get(ilog(entries - cur))?;
            cur += n;
            if n > 0 {
                if len > 32 {
                    return None;
                }
                kraft += (n as u64) << (32 - len);
                used += n;
                if kraft > FULL {
                    return None;
                }
            }
            len += 1;
        }
        if cur > entries {
            return None;
        }
    } else {
        let cll = b.get(3)?;
        let sparse = b.get(1)? == 1;
        if cll == 0 || cll > 5 {
            return None;
        }
        for _ in 0..entries {
            if !sparse || b.get(1)? == 1 {
                kraft += 1u64 << (31 - b.get(cll)?);
                used += 1;
                if kraft > FULL {
                    return None;
                }
            }
        }
    }
    if kraft != FULL && used != 1 {
        return None;
    }
    if b.get(1)? == 1 {
        b.get(32)?;
        b.get(32)?;
        let vl = b.get(4)? as u64 + 1;
        b.get(1)?;
        let q = quantvals(entries, dims)?;
        b.bit = b.bit.checked_add((q * vl) as usize)?;
        if (b.bit - 1) >> 3 >= d.len() {
            return None;
        }
    }
    Some((b.bit - p * 8) / 8 + 1)
}

fn sha256(b: &[u8]) -> String {
    use sha2::Digest;
    sha2::Sha256::digest(b)
        .iter()
        .map(|x| format!("{x:02x}"))
        .collect()
}

/// The codebook libraries in a binary (normally one).
pub fn find(bin: &[u8], source: &str) -> Vec<Library> {
    let mut out = Vec::new();
    let mut p = 0;
    while p < bin.len() {
        if codebook_len(bin, p).is_some() {
            let mut sizes = Vec::new();
            let mut q = p;
            while let Some(l) = codebook_len(bin, q) {
                sizes.push(l);
                q += l;
            }
            if sizes.len() >= MIN_RUN {
                let mut packed = bin[p..q].to_vec();
                let mut off = 0u32;
                for s in &sizes {
                    packed.extend_from_slice(&off.to_le_bytes());
                    off += *s as u32;
                }
                packed.extend_from_slice(&off.to_le_bytes());
                let sha256 = sha256(&packed);
                out.push(Library {
                    packed,
                    count: sizes.len(),
                    source: source.to_string(),
                    offset: p,
                    sha256,
                });
                p = q;
                continue;
            }
        }
        p += 1;
    }
    out
}

/// Wwise's sound engine among a game's files: (lib)AkSoundEngine.so / .dll / .dylib, and the
/// binary of an AkSoundEngine.bundle. `path` is the library, or a folder searched (a few levels).
pub fn engine_files(path: &Path) -> Vec<PathBuf> {
    fn is_engine(p: &Path) -> bool {
        let n = p
            .file_name()
            .map(|n| n.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        let stem = n.split('.').next().unwrap_or("");
        let ext = n.rsplit_once('.').map(|x| x.1).unwrap_or("");
        (stem == "aksoundengine" || stem == "libaksoundengine")
            && matches!(ext, "so" | "dll" | "dylib")
            || n == "aksoundengine"
                && p.to_string_lossy()
                    .to_ascii_lowercase()
                    .contains("aksoundengine.bundle")
    }
    fn walk(d: &Path, depth: u32, out: &mut Vec<PathBuf>) {
        let Ok(rd) = std::fs::read_dir(d) else { return };
        let mut v: Vec<PathBuf> = rd.filter_map(|e| e.ok().map(|e| e.path())).collect();
        v.sort();
        for p in v {
            if p.is_dir() {
                if depth > 0 {
                    walk(&p, depth - 1, out);
                }
            } else if is_engine(&p) {
                out.push(p);
            }
        }
    }
    if path.is_file() {
        return vec![path.to_path_buf()];
    }
    let mut out = Vec::new();
    walk(path, 5, &mut out);
    out
}

/// Where a game's plugins are, from its soundbank folder: the nearest folder above it with a
/// `Plugins` folder (`<Game>_Data/Plugins`, a macOS app's `Contents/Plugins`).
pub fn plugins_near(banks: &Path) -> Option<PathBuf> {
    let mut d = if banks.is_file() {
        banks.parent()?.to_path_buf()
    } else {
        banks.to_path_buf()
    };
    for _ in 0..8 {
        let p = d.join("Plugins");
        if p.is_dir() {
            return Some(p);
        }
        d = d.parent()?.to_path_buf();
    }
    None
}

/// The codebook libraries of the Wwise engine files under `path` (identical ones once).
pub fn from_path(path: &Path) -> (Vec<PathBuf>, Vec<Library>) {
    let files = engine_files(path);
    let mut libs: Vec<Library> = Vec::new();
    for f in &files {
        let Ok(bin) = std::fs::read(f) else { continue };
        for l in find(&bin, &f.display().to_string()) {
            if !libs.iter().any(|x| x.packed == l.packed) {
                libs.push(l);
            }
        }
    }
    (files, libs)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Writes bits LSB first.
    #[derive(Default)]
    struct W {
        v: Vec<u8>,
        bit: usize,
    }
    impl W {
        fn put(&mut self, x: u32, k: u32) {
            for i in 0..k {
                if self.bit % 8 == 0 {
                    self.v.push(0);
                }
                if x >> i & 1 == 1 {
                    *self.v.last_mut().unwrap() |= 1 << (self.bit % 8);
                }
                self.bit += 1;
            }
        }
        /// as ww2ogg sizes a codebook: bits / 8 + 1 bytes
        fn done(mut self) -> Vec<u8> {
            let n = self.bit / 8 + 1;
            self.v.resize(n, 0);
            self.v
        }
    }

    /// A made-up codebook: `dims`, lengths 1, 2, .., n-1, n-1 (a complete tree), unordered,
    /// with a value lookup when `lookup`.
    fn codebook(dims: u32, n: u32, lookup: bool) -> Vec<u8> {
        let mut w = W::default();
        w.put(dims, 4);
        w.put(n, 14);
        w.put(0, 1);
        w.put(5, 3);
        w.put(0, 1);
        for i in 0..n {
            w.put(if i + 1 < n { i } else { n - 2 }, 5);
        }
        w.put(lookup as u32, 1);
        if lookup {
            w.put(0x1234_5678, 32);
            w.put(0x0abc_def0, 32);
            w.put(3, 4);
            w.put(0, 1);
            for i in 0..quantvals(n, dims).unwrap() {
                w.put(i as u32 & 15, 4);
            }
        }
        w.done()
    }

    fn ordered(n: u32) -> Vec<u8> {
        // lengths: 1 entry of 1, 1 of 2, ..., two of n-1
        let mut w = W::default();
        w.put(1, 4);
        w.put(n, 14);
        w.put(1, 1);
        w.put(0, 5);
        let mut cur = 0;
        for len in 1..n {
            let k = if len + 1 < n { 1 } else { 2 };
            w.put(k, ilog(n - cur));
            cur += k;
        }
        w.put(0, 1);
        w.done()
    }

    #[test]
    fn finds_a_block_of_codebooks() {
        let mut bin: Vec<u8> = (0..5000u32)
            .map(|i| (i.wrapping_mul(2654435761) >> 13) as u8)
            .collect();
        let at = bin.len();
        let mut sizes = Vec::new();
        for i in 0..100u32 {
            let c = match i % 3 {
                0 => codebook(1 + i % 4, 2 + i % 30, i % 2 == 0),
                1 => ordered(3 + i % 9),
                _ => codebook(2, 16, true),
            };
            assert_eq!(codebook_len(&c, 0), Some(c.len()), "{i}");
            sizes.push(c.len());
            bin.extend(c);
        }
        let end = bin.len();
        bin.extend((0..3000u32).map(|i| (i.wrapping_mul(40503) >> 7) as u8));
        let libs = find(&bin, "test");
        assert_eq!(libs.len(), 1);
        let l = &libs[0];
        assert_eq!((l.offset, l.count), (at, 100));
        // ww2ogg's layout: the codebooks, their offsets, the table's offset
        let t = end - at;
        assert_eq!(&l.packed[..t], &bin[at..end]);
        assert_eq!(
            u32::from_le_bytes(l.packed[l.packed.len() - 4..].try_into().unwrap()) as usize,
            t
        );
        assert_eq!(l.packed.len(), t + 4 * 101);
        assert_eq!(
            u32::from_le_bytes(l.packed[t + 4..t + 8].try_into().unwrap()) as usize,
            sizes[0]
        );
        assert!(ww2ogg::CodebookLibrary::from_bytes(&l.packed).is_ok());
        // too short a run is not a library; an incomplete tree is not a codebook
        assert!(find(&bin[..at + 200], "short").is_empty());
        let mut bad = W::default();
        bad.put(1, 4);
        bad.put(3, 14);
        bad.put(0, 1);
        bad.put(5, 3);
        bad.put(0, 1);
        for _ in 0..3 {
            bad.put(1, 5); // three of length 2: a tree with a hole
        }
        bad.put(0, 1);
        assert_eq!(codebook_len(&bad.done(), 0), None);
    }

    #[test]
    fn engine_files_by_name() {
        let d = std::env::temp_dir().join(format!("wwise-cb-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        for f in [
            "Plugins/x86_64/libAkSoundEngine.so",
            "Plugins/x86/AkSoundEngine.dll",
            "Plugins/AkSoundEngine.bundle/Contents/MacOS/AkSoundEngine",
            "Plugins/x86/libAkFlanger.so",
        ] {
            let p = d.join(f);
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(&p, b"x").unwrap();
        }
        std::fs::create_dir_all(d.join("StreamingAssets/Audio/GeneratedSoundBanks/Linux")).unwrap();
        let pl = plugins_near(&d.join("StreamingAssets/Audio/GeneratedSoundBanks/Linux")).unwrap();
        assert_eq!(pl, d.join("Plugins"));
        let names: Vec<String> = engine_files(&pl)
            .iter()
            .map(|p| p.strip_prefix(&pl).unwrap().display().to_string())
            .collect();
        assert_eq!(
            names,
            [
                "AkSoundEngine.bundle/Contents/MacOS/AkSoundEngine",
                "x86/AkSoundEngine.dll",
                "x86_64/libAkSoundEngine.so"
            ]
        );
        let (files, libs) = from_path(&pl);
        assert_eq!((files.len(), libs.len()), (3, 0));
        let _ = std::fs::remove_dir_all(&d);
    }
}
