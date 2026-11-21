//! The textures step's checkpoint inside a big file (`textures.checkpoint_mb`): the converted textures of
//! the file being converted, kept beside the journal as they are finished (`.pmunity-texmemo`), so a setup
//! that is stopped part way through a file that takes minutes (Blasphemous's `resources.assets`: 972
//! textures, more than six minutes on a handheld) does not encode its textures again when it goes on.
//! The journal still marks whole files only: the run that goes on reads the file from its start and
//! takes each texture's blocks from here instead of from the encoder, in the same order, so the file it
//! writes is the one an uninterrupted run writes.
//!
//! The file: a header with a key (the data file, its size and time, the step's settings, the tools),
//! then one record a texture: path id, a hash of what was converted (the texture's size and format, its
//! data's length, the choice), the converted texture's fields, its blocks, and two checksums (the
//! record's head, the blocks). Records are only appended, and synced every few MB. A run that goes on
//! takes the records up to the first whose head does not check (a tail that never reached the card),
//! and a record whose blocks do not check is encoded again. Any other key: the file starts empty. It is
//! best effort throughout: a card with no room for it, or any error writing it, ends the checkpoint and
//! removes the file; the conversion goes on without.
use super::textures::{Choice, Converted, Tex};
use crate::job::Job;
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

/// The checkpoint's file name in the output folder (a dot file: no step lists it, packing leaves it out).
pub const FILE: &str = ".pmunity-texmemo";

const MAGIC: &[u8; 12] = b"PMUTEXMEMO1\n";
const REC: u32 = 0x4d58_4554;
const HEAD: usize = 52;
/// Synced each time this much more is written (what a power cut can lose of it).
const SYNC_EVERY: u64 = 4 << 20;
/// The free space is looked at each time this much more is written.
const SPACE_EVERY: u64 = 16 << 20;

struct Rec {
    meta: u64,
    format: i32,
    width: u32,
    height: u32,
    mips: u32,
    at: u64,
    len: u64,
    crc: u32,
}

pub struct Memo {
    path: PathBuf,
    dir: PathBuf,
    have: HashMap<i64, Rec>,
    reader: Option<std::fs::File>,
    writer: Option<unityfs::bgwrite::BgWriter>,
    /// the room the file's conversion needs on the card: the checkpoint never takes of it
    reserve: u64,
    since_space: u64,
    /// textures taken from the checkpoint, and their bytes (the step's log)
    pub taken: u64,
    pub taken_bytes: u64,
}

fn fnv(b: &[u8]) -> u64 {
    b.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, &x| {
        (h ^ x as u64).wrapping_mul(0x0000_0100_0000_01b3)
    })
}

/// What a record was converted from: another texture, size, format, data or choice is not this one.
pub fn meta(t: &Tex, raw_len: usize, ch: &Choice) -> u64 {
    let s = format!(
        "{:?} {}x{} f{} m{:?} r{} d{:?} i{:?} raw{raw_len} -> {ch:?}",
        t.name, t.width, t.height, t.format, t.mip_count, t.is_readable, t.dimension, t.image_count
    );
    fnv(s.as_bytes())
}

/// A file's size and time, for a key ("-" when it cannot be read).
pub fn file_id(p: &Path) -> String {
    match std::fs::metadata(p) {
        Ok(m) => {
            let t = m
                .modified()
                .ok()
                .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                .map_or(0, |d| d.as_nanos());
            format!("{}.{t}", m.len())
        }
        Err(_) => "-".into(),
    }
}

/// The running program (pmu), for a key: a checkpoint or an index another build made is not used.
pub fn tool_id() -> String {
    std::env::current_exe()
        .map(|p| file_id(&p))
        .unwrap_or_else(|_| "-".into())
}

fn free_bytes(dir: &Path) -> Option<u64> {
    let st = rustix::fs::statvfs(dir).ok()?;
    Some(st.f_bavail.saturating_mul(st.f_frsize))
}

impl Memo {
    /// The checkpoint for the file `key` names: the records a stopped run left for the same key, then
    /// the new ones appended. None: it cannot be kept (the conversion goes on without).
    pub fn open(job: &Job, key: &str, reserve: u64) -> Option<Memo> {
        let path = job.out.join(FILE);
        let mut m = Memo {
            path: path.clone(),
            dir: job.out.clone(),
            have: HashMap::new(),
            reader: None,
            writer: None,
            reserve,
            since_space: 0,
            taken: 0,
            taken_bytes: 0,
        };
        let end = m.scan(key).unwrap_or(0);
        let made = (|| -> std::io::Result<unityfs::bgwrite::BgWriter> {
            let mut f = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .truncate(end == 0)
                .open(&path)?;
            if end == 0 {
                let mut head = MAGIC.to_vec();
                head.extend((key.len() as u32).to_le_bytes());
                head.extend(key.as_bytes());
                head.extend(crc32fast::hash(key.as_bytes()).to_le_bytes());
                f.write_all(&head)?;
            } else {
                // (a tail that does not check is cut off: the new records follow the good ones)
                f.set_len(end)?;
                f.seek(SeekFrom::Start(end))?;
            }
            let mut w = unityfs::bgwrite::BgWriter::new(f)?;
            w.sync_every(SYNC_EVERY)?;
            Ok(w)
        })();
        match made {
            Ok(w) => m.writer = Some(w),
            Err(_) => {
                // (no checkpoint can be written; what a stopped run left is still used)
                if m.have.is_empty() {
                    let _ = std::fs::remove_file(&path);
                    return None;
                }
            }
        }
        if !m.have.is_empty() {
            m.reader = std::fs::File::open(&path).ok();
        }
        Some(m)
    }

    /// The records of a file with this key: where the good ones end (None: no such file, another key).
    fn scan(&mut self, key: &str) -> Option<u64> {
        let mut f = std::fs::File::open(&self.path).ok()?;
        let size = f.metadata().ok()?.len();
        let mut head = vec![0u8; MAGIC.len() + 4];
        f.read_exact(&mut head).ok()?;
        if &head[..MAGIC.len()] != MAGIC {
            return None;
        }
        let n = u32::from_le_bytes(head[MAGIC.len()..].try_into().ok()?) as usize;
        if n != key.len() {
            return None;
        }
        let mut k = vec![0u8; n + 4];
        f.read_exact(&mut k).ok()?;
        if &k[..n] != key.as_bytes()
            || u32::from_le_bytes(k[n..].try_into().ok()?) != crc32fast::hash(&k[..n])
        {
            return None;
        }
        let mut at = (MAGIC.len() + 4 + n + 4) as u64;
        loop {
            let mut h = [0u8; HEAD];
            if at + HEAD as u64 > size
                || f.seek(SeekFrom::Start(at)).is_err()
                || f.read_exact(&mut h).is_err()
            {
                break;
            }
            let u32_at = |i: usize| u32::from_le_bytes(h[i..i + 4].try_into().unwrap());
            let u64_at = |i: usize| u64::from_le_bytes(h[i..i + 8].try_into().unwrap());
            if u32_at(0) != REC || u32_at(HEAD - 4) != crc32fast::hash(&h[..HEAD - 4]) {
                break;
            }
            let len = u64_at(36);
            let data = at + HEAD as u64;
            if data.checked_add(len).is_none_or(|e| e > size) {
                break;
            }
            self.have.insert(
                u64_at(4) as i64,
                Rec {
                    meta: u64_at(12),
                    format: u32_at(20) as i32,
                    width: u32_at(24),
                    height: u32_at(28),
                    mips: u32_at(32),
                    at: data,
                    len,
                    crc: u32_at(44),
                },
            );
            at = data + len;
        }
        Some(at)
    }

    /// The texture as a stopped run converted it, when it is here whole and was made from the same.
    pub fn take(&mut self, pid: i64, meta: u64) -> Option<Converted> {
        let r = self.have.get(&pid).filter(|r| r.meta == meta)?;
        let f = self.reader.as_mut()?;
        let mut data = vec![0u8; usize::try_from(r.len).ok()?];
        f.seek(SeekFrom::Start(r.at)).ok()?;
        f.read_exact(&mut data).ok()?;
        if crc32fast::hash(&data) != r.crc {
            return None;
        }
        self.taken += 1;
        self.taken_bytes += r.len;
        Some(Converted {
            format: r.format,
            width: r.width as usize,
            height: r.height as usize,
            mip_count: r.mips as usize,
            data,
        })
    }

    /// A texture just converted, appended (best effort: an error or a card with no room ends the
    /// checkpoint).
    pub fn put(&mut self, pid: i64, meta: u64, c: &Converted) {
        let Some(w) = self.writer.as_mut() else {
            return;
        };
        let size = (HEAD + c.data.len()) as u64;
        self.since_space += size;
        if self.since_space >= SPACE_EVERY || size >= SPACE_EVERY {
            self.since_space = 0;
            // (None: a file system that does not say; as with room)
            if free_bytes(&self.dir).is_some_and(|f| f < self.reserve.saturating_add(2 * size)) {
                self.stop();
                return;
            }
        }
        let mut h = Vec::with_capacity(HEAD);
        h.extend(REC.to_le_bytes());
        h.extend(pid.to_le_bytes());
        h.extend(meta.to_le_bytes());
        h.extend(c.format.to_le_bytes());
        h.extend((c.width as u32).to_le_bytes());
        h.extend((c.height as u32).to_le_bytes());
        h.extend((c.mip_count as u32).to_le_bytes());
        h.extend((c.data.len() as u64).to_le_bytes());
        h.extend(crc32fast::hash(&c.data).to_le_bytes());
        let crc = crc32fast::hash(&h);
        h.extend(crc.to_le_bytes());
        if w.write_all(&h).and_then(|_| w.write_all(&c.data)).is_err() {
            self.stop();
        }
    }

    /// The checkpoint ended and its file removed (its room given back to the conversion).
    fn stop(&mut self) {
        self.writer = None;
        self.reader = None;
        self.have.clear();
        let _ = std::fs::remove_file(&self.path);
    }
}

/// The checkpoint's file removed (its file is done and marked, or the step ends).
pub fn remove(job: &Job) {
    let _ = std::fs::remove_file(job.out.join(FILE));
}
