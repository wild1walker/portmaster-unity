//! LZ4 blocks as the Python's `lz4.block` (python-lz4 with liblz4 1.9.4) reads and writes them:
//! [`decompress`] and [`compress_hc`], `lz4.block.compress(data, mode='high_compression',
//! store_size=False)`, which is liblz4's `LZ4_compress_HC` at its default level 9: the hash chain
//! search of lz4hc.c (256 attempts, with its pattern analysis), byte for byte. Only what one
//! block compressed on its own reaches (no dictionary, no output limit) is ported.

const MINMATCH: usize = 4;
const MFLIMIT: usize = 12;
const LASTLITERALS: usize = 5;
const MIN_LENGTH: usize = MFLIMIT + 1;
const ML_BITS: u32 = 4;
const ML_MASK: usize = (1 << ML_BITS) - 1;
const RUN_MASK: usize = (1 << (8 - ML_BITS)) - 1;
const DISTANCE_MAX: u32 = 65535;
const OPTIMAL_ML: isize = (ML_MASK - 1 + MINMATCH) as isize;
const HASH_LOG: u32 = 15;
/// Level 9: attempts per search (more than 128: with the pattern analysis).
const NB_SEARCHES: i32 = 256;
/// lz4hc's index of the first byte of a fresh context's input (its 64 KB start offset).
const BASE: u32 = 65536;

/// The decompressed bytes of an LZ4 block (`size`: the expected length, as `uncompressed_size`).
pub fn decompress(src: &[u8], size: usize) -> Result<Vec<u8>, String> {
    let bad = || "LZ4 block: corrupt input or insufficient space in the destination".to_string();
    let mut out: Vec<u8> = Vec::with_capacity(size);
    let mut i = 0;
    loop {
        let token = *src.get(i).ok_or_else(bad)?;
        i += 1;
        let mut lit = (token >> ML_BITS) as usize;
        if lit == RUN_MASK {
            loop {
                let b = *src.get(i).ok_or_else(bad)?;
                i += 1;
                lit += b as usize;
                if b != 255 {
                    break;
                }
            }
        }
        let end = i
            .checked_add(lit)
            .filter(|&e| e <= src.len())
            .ok_or_else(bad)?;
        if out.len() + lit > size {
            return Err(bad());
        }
        out.extend_from_slice(&src[i..end]);
        i = end;
        if i == src.len() {
            break; // the last sequence: literals only
        }
        let off = u16::from_le_bytes([
            *src.get(i).ok_or_else(bad)?,
            *src.get(i + 1).ok_or_else(bad)?,
        ]) as usize;
        i += 2;
        let mut ml = (token & ML_MASK as u8) as usize;
        if ml == ML_MASK {
            loop {
                let b = *src.get(i).ok_or_else(bad)?;
                i += 1;
                ml += b as usize;
                if b != 255 {
                    break;
                }
            }
        }
        ml += MINMATCH;
        if off == 0 || off > out.len() || out.len() + ml > size {
            return Err(bad());
        }
        let from = out.len() - off;
        for k in 0..ml {
            let b = out[from + k];
            out.push(b);
        }
    }
    if out.len() != size {
        return Err(format!(
            "LZ4 block: {} bytes decompressed, {size} expected",
            out.len()
        ));
    }
    Ok(out)
}

/// A match: length, offset, and how far it starts before the position searched (<= 0).
#[derive(Clone, Copy)]
struct Match {
    len: isize,
    off: isize,
    back: isize,
}

const NOMATCH: Match = Match {
    len: 0,
    off: 0,
    back: 0,
};

/// lz4hc's context for one block: hash table, chain table (deltas), the next position to insert.
struct Hc<'a> {
    src: &'a [u8],
    hash: Vec<u32>,
    chain: Vec<u16>,
    next_to_update: u32,
}

#[derive(PartialEq)]
enum Repeat {
    Untested,
    Not,
    Confirmed,
}

impl<'a> Hc<'a> {
    fn new(src: &'a [u8]) -> Self {
        Hc {
            src,
            hash: vec![0; 1 << HASH_LOG],
            chain: vec![0; 1 << 16],
            next_to_update: BASE,
        }
    }

    fn read32(&self, p: usize) -> u32 {
        u32::from_le_bytes(self.src[p..p + 4].try_into().unwrap())
    }

    fn read16(&self, p: usize) -> u16 {
        u16::from_le_bytes(self.src[p..p + 2].try_into().unwrap())
    }

    fn hash_at(&self, p: usize) -> usize {
        (self.read32(p).wrapping_mul(2654435761) >> (MINMATCH as u32 * 8 - HASH_LOG)) as usize
    }

    fn chain_at(&self, idx: u32) -> u32 {
        self.chain[idx as u16 as usize] as u32
    }

    /// LZ4HC_Insert: every position before `ip` into the tables.
    fn insert(&mut self, ip: usize) {
        let target = ip as u32 + BASE;
        let mut idx = self.next_to_update;
        while idx < target {
            let h = self.hash_at((idx - BASE) as usize);
            let delta = (idx.wrapping_sub(self.hash[h])).min(DISTANCE_MAX);
            self.chain[idx as u16 as usize] = delta as u16;
            self.hash[h] = idx;
            idx += 1;
        }
        self.next_to_update = target;
    }

    fn read64(&self, p: usize) -> u64 {
        u64::from_le_bytes(self.src[p..p + 8].try_into().unwrap())
    }

    /// LZ4_count: the bytes equal at `a` and `b`, up to `limit` (for `a`).
    fn count(&self, a: usize, b: usize, limit: usize) -> usize {
        let mut n = 0;
        while a + n + 8 <= limit {
            let x = self.read64(a + n) ^ self.read64(b + n);
            if x != 0 {
                return n + (x.trailing_zeros() / 8) as usize;
            }
            n += 8;
        }
        while a + n < limit && self.src[a + n] == self.src[b + n] {
            n += 1;
        }
        n
    }

    /// LZ4HC_countBack: minus the bytes equal before `ip` and `m`, down to `imin` / `mmin`.
    fn count_back(&self, ip: usize, m: usize, imin: usize, mmin: usize) -> isize {
        let min = (imin as isize - ip as isize).max(mmin as isize - m as isize);
        let mut back = 0isize;
        while back > min
            && self.src[(ip as isize + back - 1) as usize]
                == self.src[(m as isize + back - 1) as usize]
        {
            back -= 1;
        }
        back
    }

    /// LZ4HC_countPattern: the bytes from `p` (up to `end`) that repeat `pattern` (little-endian).
    fn count_pattern(&self, p: usize, end: usize, pattern: u32) -> usize {
        let wide = pattern as u64 | (pattern as u64) << 32;
        let mut n = 0;
        while p + n + 8 <= end {
            let x = self.read64(p + n) ^ wide;
            if x != 0 {
                return n + (x.trailing_zeros() / 8) as usize;
            }
            n += 8;
        }
        while p + n < end && self.src[p + n] == (pattern >> (8 * (n % 4))) as u8 {
            n += 1;
        }
        n
    }

    /// LZ4HC_reverseCountPattern: the bytes before `p` (down to `low`) that repeat `pattern`.
    fn reverse_count_pattern(&self, p: usize, low: usize, pattern: u32) -> usize {
        let mut n = 0;
        while p - n >= low + 4 && self.read32(p - n - 4) == pattern {
            n += 4;
        }
        while p - n > low && self.src[p - n - 1] == (pattern >> (8 * (3 - n % 4))) as u8 {
            n += 1;
        }
        n
    }

    /// LZ4HC_InsertAndGetWiderMatch for one block without a dictionary: the longest match at `ip`
    /// (starting no earlier than `low`, ending no later than `high`) longer than `longest`.
    fn wider(&mut self, ip: usize, low: usize, high: usize, mut longest: isize) -> Match {
        let prefix_idx = BASE;
        let ip_index = ip as u32 + BASE;
        let within_start = BASE + (DISTANCE_MAX + 1) > ip_index;
        let lowest = if within_start {
            BASE
        } else {
            ip_index - DISTANCE_MAX
        };
        let look_back = (ip - low) as isize;
        let mut attempts = NB_SEARCHES;
        let pattern = self.read32(ip);
        let mut repeat = Repeat::Untested;
        let mut src_pattern_len = 0usize;
        let (mut offset, mut s_back) = (0isize, 0isize);
        self.insert(ip);
        let mut match_index = self.hash[self.hash_at(ip)];
        while match_index >= lowest && attempts > 0 {
            attempts -= 1;
            // (every index from `lowest` on is in the block itself: no external dictionary)
            let mp = (match_index - prefix_idx) as usize;
            let a = (low as isize + longest - 1) as usize;
            let b = (mp as isize - look_back + longest - 1) as usize;
            if self.read16(a) == self.read16(b) && self.read32(mp) == pattern {
                let back = if look_back != 0 {
                    self.count_back(ip, mp, low, 0)
                } else {
                    0
                };
                let len =
                    (MINMATCH + self.count(ip + MINMATCH, mp + MINMATCH, high)) as isize - back;
                if len > longest {
                    longest = len;
                    offset = (ip_index - match_index) as isize;
                    s_back = back;
                }
            }
            let dist_next = self.chain_at(match_index);
            if dist_next == 1 {
                // pattern analysis (levels 9+): a run of one byte is skipped through in one step
                let cand = match_index - 1;
                if repeat == Repeat::Untested {
                    if (pattern & 0xFFFF) == (pattern >> 16) && (pattern & 0xFF) == (pattern >> 24)
                    {
                        repeat = Repeat::Confirmed;
                        src_pattern_len = self.count_pattern(ip + 4, high, pattern) + 4;
                    } else {
                        repeat = Repeat::Not;
                    }
                }
                if repeat == Repeat::Confirmed
                    && cand >= lowest
                    && protect_dict_end(prefix_idx, cand)
                {
                    let mp = (cand - prefix_idx) as usize;
                    if self.read32(mp) == pattern {
                        let forward = self.count_pattern(mp + 4, high, pattern) + 4;
                        let mut back_len = self.reverse_count_pattern(mp, 0, pattern);
                        back_len =
                            (cand - (cand.wrapping_sub(back_len as u32)).max(lowest)) as usize;
                        let segment = back_len + forward;
                        if segment >= src_pattern_len && forward <= src_pattern_len {
                            let new = cand
                                .wrapping_add(forward as u32)
                                .wrapping_sub(src_pattern_len as u32);
                            match_index = if protect_dict_end(prefix_idx, new) {
                                new
                            } else {
                                prefix_idx
                            };
                        } else {
                            let new = cand.wrapping_sub(back_len as u32);
                            if !protect_dict_end(prefix_idx, new) {
                                match_index = prefix_idx;
                            } else {
                                match_index = new;
                                if look_back == 0 {
                                    let max_ml = segment.min(src_pattern_len);
                                    if (longest as usize) < max_ml {
                                        if ip as u32 + prefix_idx - match_index > DISTANCE_MAX {
                                            break;
                                        }
                                        longest = max_ml as isize;
                                        offset = (ip_index - match_index) as isize;
                                    }
                                    let dist = self.chain_at(match_index);
                                    if dist > match_index {
                                        break;
                                    }
                                    match_index -= dist;
                                }
                            }
                        }
                        continue;
                    }
                }
            }
            match_index = match_index.wrapping_sub(self.chain_at(match_index));
        }
        Match {
            len: longest,
            off: offset,
            back: s_back,
        }
    }
}

/// LZ4HC_protectDictEnd: whether a match index is not in the last 3 bytes before the prefix.
fn protect_dict_end(dict_limit: u32, idx: u32) -> bool {
    (dict_limit.wrapping_sub(1)).wrapping_sub(idx) >= 3
}

/// LZ4HC_encodeSequence: literals from `anchor` to `ip`, then a match; `ip` and `anchor` move past it.
fn encode(
    out: &mut Vec<u8>,
    src: &[u8],
    ip: &mut usize,
    anchor: &mut usize,
    ml: isize,
    off: isize,
) {
    let lit = *ip - *anchor;
    let tok = out.len();
    if lit >= RUN_MASK {
        out.push((RUN_MASK << ML_BITS) as u8);
        let mut len = lit - RUN_MASK;
        while len >= 255 {
            out.push(255);
            len -= 255;
        }
        out.push(len as u8);
    } else {
        out.push((lit << ML_BITS) as u8);
    }
    out.extend_from_slice(&src[*anchor..*ip]);
    out.extend_from_slice(&(off as u16).to_le_bytes());
    let mut len = ml as usize - MINMATCH;
    if len >= ML_MASK {
        out[tok] += ML_MASK as u8;
        len -= ML_MASK;
        while len >= 510 {
            out.extend_from_slice(&[255, 255]);
            len -= 510;
        }
        if len >= 255 {
            len -= 255;
            out.push(255);
        }
        out.push(len as u8);
    } else {
        out[tok] += len as u8;
    }
    *ip += ml as usize;
    *anchor = *ip;
}

/// One LZ4 block of `src`, as `lz4.block.compress(src, mode='high_compression', store_size=False)`
/// writes it (LZ4HC_compress_hashChain at level 9).
pub fn compress_hc(src: &[u8]) -> Vec<u8> {
    let n = src.len();
    let mut out = Vec::with_capacity(n + n / 255 + 16);
    let (mut ip, mut anchor) = (0usize, 0usize);
    if n >= MIN_LENGTH {
        let mflimit = n - MFLIMIT;
        let matchlimit = n - LASTLITERALS;
        let mut hc = Hc::new(src);
        'main: while ip <= mflimit {
            let mut m1 = hc.wider(ip, ip, matchlimit, MINMATCH as isize - 1);
            if m1.len < MINMATCH as isize {
                ip += 1;
                continue;
            }
            // saved, in case the first match is skipped too far
            let (mut start0, mut m0) = (ip, m1);
            'search2: loop {
                let (mut start2, mut m2);
                if ip as isize + m1.len <= mflimit as isize {
                    let s = ip + m1.len as usize - 2;
                    m2 = hc.wider(s, ip, matchlimit, m1.len);
                    start2 = (s as isize + m2.back) as usize;
                } else {
                    m2 = NOMATCH;
                    start2 = 0;
                }
                if m2.len <= m1.len {
                    encode(&mut out, src, &mut ip, &mut anchor, m1.len, m1.off);
                    continue 'main;
                }
                if start0 < ip && (start2 as isize) < ip as isize + m0.len {
                    ip = start0; // squeezing ML1 between ML0 and ML2: back to the first match
                    m1 = m0;
                }
                if (start2 as isize - ip as isize) < 3 {
                    ip = start2; // first match too short: dropped
                    m1 = m2;
                    continue 'search2;
                }
                loop {
                    // _Search3
                    if (start2 as isize - ip as isize) < OPTIMAL_ML {
                        let mut new_ml = m1.len.min(OPTIMAL_ML);
                        if ip as isize + new_ml > start2 as isize + m2.len - MINMATCH as isize {
                            new_ml = (start2 as isize - ip as isize) + m2.len - MINMATCH as isize;
                        }
                        let correction = new_ml - (start2 as isize - ip as isize);
                        if correction > 0 {
                            start2 = (start2 as isize + correction) as usize;
                            m2.len -= correction;
                        }
                    }
                    let (start3, m3);
                    if start2 as isize + m2.len <= mflimit as isize {
                        let s = start2 + m2.len as usize - 3;
                        m3 = hc.wider(s, start2, matchlimit, m2.len);
                        start3 = (s as isize + m3.back) as usize;
                    } else {
                        m3 = NOMATCH;
                        start3 = 0;
                    }
                    if m3.len <= m2.len {
                        // no better match: ML1 and ML2
                        if (start2 as isize) < ip as isize + m1.len {
                            m1.len = start2 as isize - ip as isize;
                        }
                        encode(&mut out, src, &mut ip, &mut anchor, m1.len, m1.off);
                        ip = start2;
                        encode(&mut out, src, &mut ip, &mut anchor, m2.len, m2.off);
                        continue 'main;
                    }
                    if (start3 as isize) < ip as isize + m1.len + 3 {
                        // not enough room for match 2: removed
                        if start3 as isize >= ip as isize + m1.len {
                            // ML1 can be written now; ML3 becomes ML1
                            if (start2 as isize) < ip as isize + m1.len {
                                let correction = ip as isize + m1.len - start2 as isize;
                                start2 = (start2 as isize + correction) as usize;
                                m2.len -= correction;
                                if m2.len < MINMATCH as isize {
                                    start2 = start3;
                                    m2 = m3;
                                }
                            }
                            encode(&mut out, src, &mut ip, &mut anchor, m1.len, m1.off);
                            ip = start3;
                            m1 = m3;
                            start0 = start2;
                            m0 = m2;
                            continue 'search2;
                        }
                        start2 = start3;
                        m2 = m3;
                        continue;
                    }
                    // three ascending matches: ML1 written, its length decided here
                    if (start2 as isize) < ip as isize + m1.len {
                        if (start2 as isize - ip as isize) < OPTIMAL_ML {
                            if m1.len > OPTIMAL_ML {
                                m1.len = OPTIMAL_ML;
                            }
                            if ip as isize + m1.len > start2 as isize + m2.len - MINMATCH as isize {
                                m1.len =
                                    (start2 as isize - ip as isize) + m2.len - MINMATCH as isize;
                            }
                            let correction = m1.len - (start2 as isize - ip as isize);
                            if correction > 0 {
                                start2 = (start2 as isize + correction) as usize;
                                m2.len -= correction;
                            }
                        } else {
                            m1.len = start2 as isize - ip as isize;
                        }
                    }
                    encode(&mut out, src, &mut ip, &mut anchor, m1.len, m1.off);
                    ip = start2;
                    m1 = m2;
                    start2 = start3;
                    m2 = m3;
                }
            }
        }
    }
    // last literals
    let lit = n - anchor;
    if lit >= RUN_MASK {
        out.push((RUN_MASK << ML_BITS) as u8);
        let mut acc = lit - RUN_MASK;
        while acc >= 255 {
            out.push(255);
            acc -= 255;
        }
        out.push(acc as u8);
    } else {
        out.push((lit << ML_BITS) as u8);
    }
    out.extend_from_slice(&src[anchor..]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips() {
        let mut cases: Vec<Vec<u8>> = vec![vec![], b"a".to_vec(), vec![0; 13], vec![7; 70000]];
        let mut x = 1u32;
        let mut v = Vec::new();
        for i in 0..200000u32 {
            x = x.wrapping_mul(1103515245).wrapping_add(12345);
            v.push(if i % 1000 < 500 {
                (x >> 24) as u8 & 3
            } else {
                b"float4 "[(i % 7) as usize]
            });
        }
        cases.push(v);
        for c in cases {
            let z = compress_hc(&c);
            assert_eq!(decompress(&z, c.len()).unwrap(), c);
        }
    }

    #[test]
    fn short_inputs_are_literals() {
        assert_eq!(compress_hc(b""), vec![0]);
        assert_eq!(compress_hc(b"abc"), b"\x30abc".to_vec());
        // 12 bytes: under LZ4's minimum for a match, all literals
        assert_eq!(compress_hc(&[0; 12])[0], 0xc0);
    }

    #[test]
    fn rejects_corrupt_blocks() {
        assert!(decompress(&[0x10, b'a', 9, 0], 10).is_err()); // offset past the start
        assert!(decompress(&[0x40, b'a'], 4).is_err()); // literals past the end
        assert!(decompress(&[0x10, b'a'], 2).is_err()); // shorter than expected
    }
}
