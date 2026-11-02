//! What the fuzz targets share: the input cut into fields, a scratch folder for the parsers that
//! read files, a reader drained with a cap, and the class database when $PMU_TPK names one.
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, OnceLock};

/// The input read field by field; past its end every number is 0 and every slice empty.
pub struct Input<'a> {
    pub data: &'a [u8],
}

impl<'a> Input<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Input { data }
    }

    pub fn bytes(&mut self, n: usize) -> &'a [u8] {
        let (a, b) = self.data.split_at(n.min(self.data.len()));
        self.data = b;
        a
    }

    pub fn u8(&mut self) -> u8 {
        self.bytes(1).first().copied().unwrap_or(0)
    }

    pub fn u16(&mut self) -> u16 {
        let b = self.bytes(2);
        b.iter().rev().fold(0, |a, &x| a << 8 | x as u16)
    }

    pub fn u32(&mut self) -> u32 {
        let b = self.bytes(4);
        b.iter().rev().fold(0, |a, &x| a << 8 | x as u32)
    }

    pub fn u64(&mut self) -> u64 {
        let b = self.bytes(8);
        b.iter().rev().fold(0, |a, &x| a << 8 | x as u64)
    }

    /// A number of `n` bytes at most, sign-extended from its top byte (small inputs give small
    /// numbers, negative ones included).
    pub fn int(&mut self, n: usize) -> i64 {
        let b = self.bytes(n.min(8));
        match b.last() {
            None => 0,
            Some(&top) => {
                let mut v: i64 = if top & 0x80 != 0 { -1 } else { 0 };
                for &x in b.iter().rev() {
                    v = v << 8 | x as i64;
                }
                v
            }
        }
    }

    pub fn rest(self) -> &'a [u8] {
        self.data
    }
}

/// A scratch folder of this run, removed when dropped: under $PMU_FUZZ_TMP, else the system's
/// temporary folder (a tmpfs is fastest).
pub struct Scratch(PathBuf);

impl Scratch {
    pub fn new() -> Scratch {
        static N: AtomicU64 = AtomicU64::new(0);
        let base = std::env::var_os("PMU_FUZZ_TMP")
            .map(PathBuf::from)
            .unwrap_or_else(std::env::temp_dir);
        let p = base.join(format!(
            "pmu-fuzz-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).expect("scratch folder");
        Scratch(p)
    }

    pub fn path(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }

    /// A file of the scratch folder with these bytes.
    pub fn file(&self, name: &str, data: &[u8]) -> PathBuf {
        let p = self.path(name);
        std::fs::write(&p, data).expect("scratch file");
        p
    }

    pub fn dir(&self) -> &Path {
        &self.0
    }
}

impl Default for Scratch {
    fn default() -> Self {
        Scratch::new()
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// Reads up to `cap` bytes from r (a stream that gives more is not followed further).
pub fn drain(mut r: impl Read, cap: u64) -> std::io::Result<u64> {
    std::io::copy(&mut (&mut r).take(cap), &mut std::io::sink())
}

/// The class database UnityPy uses (lzma.tpk), when $PMU_TPK names it: read once a process.
pub fn class_db() -> Option<Arc<unityfs::ClassDb>> {
    static DB: OnceLock<Option<Arc<unityfs::ClassDb>>> = OnceLock::new();
    DB.get_or_init(|| {
        std::env::var_os("PMU_TPK")?;
        unityfs::ClassDb::default_db().ok().map(Arc::new)
    })
    .clone()
}

/// Splits the input at NUL bytes into a command line's arguments (lossy UTF-8).
pub fn args(data: &[u8]) -> Vec<String> {
    data.split(|&b| b == 0)
        .map(|a| String::from_utf8_lossy(a).into_owned())
        .collect()
}
