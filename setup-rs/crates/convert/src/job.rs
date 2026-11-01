//! The conversion job (job.py): where files are read from and written to, the resume journal and
//! the log.
//!
//! In place (the device's setup: <out> is the data folder itself), every file is written to
//! <file>.tmp first, synced and renamed over the original, and each finished file is recorded in
//! the journal (<out>/.pmunity.json), so a setup that is killed resumes where it stopped and
//! never leaves a half-written file. Files that must change together (a serialized file and its
//! .resS) are listed in the journal as `pending` before they are renamed, and a new run finishes
//! the renames first. As an overlay (<out> another folder), converted files go there and later
//! steps read them from there.
//!
//! The journal is the Python's, byte for byte: `{"version": ..., "done": {step: [file, ...]},
//! "scaled": {file: {path id: [old w, old h, new w, new h]}}, ...}` as json.dump writes it, so
//! either implementation continues the other's run.
use crate::error::{Error, Result};
use crate::pyjson::{self, Json};
use std::io::Write as _;
use std::path::{Component, Path, PathBuf};
use std::sync::{Arc, OnceLock};
use std::time::Instant;
use unityfs::ClassDb;

/// The journal's file name in the output folder.
pub const STATE: &str = ".pmunity.json";

pub struct Job {
    pub src: PathBuf,
    pub out: PathBuf,
    /// The config (a dict), with --set applied.
    pub cfg: Json,
    pub inplace: bool,
    t0: Instant,
    state_path: PathBuf,
    /// The journal (a dict).
    pub state: Json,
    db: OnceLock<std::result::Result<Arc<ClassDb>, String>>,
}

/// os.path.abspath: absolute, '.' and '..' resolved lexically (symlinks left alone).
pub fn abspath(p: &Path) -> PathBuf {
    let joined = if p.is_absolute() {
        p.to_path_buf()
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("/"))
            .join(p)
    };
    let mut out = PathBuf::from("/");
    for c in joined.components() {
        match c {
            Component::ParentDir => {
                out.pop();
            }
            Component::Normal(x) => out.push(x),
            _ => {}
        }
    }
    out
}

/// A path as the text the Python's str paths have (paths here are UTF-8).
pub fn text(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}

/// `<path>.<suffix>` (the Python's path + '.tmp').
pub fn with_suffix(p: &Path, suffix: &str) -> PathBuf {
    let mut s = p.as_os_str().to_os_string();
    s.push(suffix);
    PathBuf::from(s)
}

fn fsync(path: &Path) -> std::io::Result<()> {
    std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)?
        .sync_all()
}

/// A folder's entries (a rename in it) on the card (a file system that cannot sync a folder: as
/// it is).
fn fsync_dir(dir: &Path) {
    if let Ok(d) = std::fs::File::open(dir) {
        let _ = d.sync_all();
    }
}

/// A journal's text as the state, when it is one (a dict); None for a damaged one (e.g. empty: a
/// handheld that restarted before a journal written without a sync reached the card).
fn read_journal(path: &Path) -> Option<Json> {
    let text = std::fs::read_to_string(path).ok()?;
    pyjson::loads(&text).ok().filter(|j| matches!(j, Json::Dict(_)))
}

/// Status of a conversion whose journal is damaged: the files converted in place before cannot be
/// told from the others, so the setup starts again from the game's files (its message).
pub const DAMAGED_JOURNAL: u8 = 5;

/// The files a journal's steps have finished, 0 when it cannot be read (resume.txt's
/// journal_files: a setup reruns a killed conversion as long as this grows).
pub fn journal_files(path: &Path) -> usize {
    let Ok(text) = std::fs::read_to_string(path) else {
        return 0;
    };
    match pyjson::loads(&text)
        .ok()
        .as_ref()
        .and_then(|j| j.get("done"))
    {
        Some(Json::Dict(d)) => d
            .iter()
            .map(|(_, v)| match v {
                Json::List(l) => l.len(),
                Json::Dict(d) => d.len(),
                Json::Str(s) => s.chars().count(),
                _ => 0,
            })
            .sum(),
        _ => 0,
    }
}

impl Job {
    /// Job(src, out, cfg): the journal read when there is one, renames a killed run left
    /// pending finished.
    pub fn new(src: &Path, out: &Path, cfg: Json) -> Result<Job> {
        let src = abspath(src);
        let out = abspath(out);
        std::fs::create_dir_all(&out)?;
        let real = |p: &Path| std::fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf());
        let inplace = real(&src) == real(&out);
        let state_path = out.join(STATE);
        let version = cfg.get("version").cloned().unwrap_or(Json::Int(1));
        let mut job = Job {
            src,
            out,
            cfg,
            inplace,
            t0: Instant::now(),
            state_path,
            state: Json::Dict(vec![
                ("version".into(), version),
                ("done".into(), Json::dict()),
                ("scaled".into(), Json::dict()),
            ]),
            db: OnceLock::new(),
        };
        // the journal, or the copy a save had synced but not yet renamed over it
        let tmp = with_suffix(&job.state_path, ".tmp");
        let found = job.state_path.exists() || tmp.exists();
        let state = read_journal(&job.state_path).or_else(|| read_journal(&tmp));
        if found && state.is_none() {
            println!(
                "ERROR: {}: the conversion's journal is damaged (the handheld restarted or lost power \
                 while it was written): the files already converted cannot be told apart, so the \
                 setup has to start again from the game's files",
                job.state_path.display()
            );
            return Err(Error::Status(DAMAGED_JOURNAL));
        }
        if let Some(state) = state {
            job.state = state;
            if read_journal(&job.state_path).is_none() {
                job.save_state()?;
            }
            job.state.setdefault("done", Json::dict());
            job.state.setdefault("scaled", Json::dict());
            if job.state.get("pending").is_some_and(Json::truthy) {
                job.finish_pending()?;
            }
        }
        Ok(job)
    }

    /// A log line with the time since the job started, on stdout (job.log).
    pub fn log(&self, msg: &str) {
        let mut out = std::io::stdout().lock();
        let _ = writeln!(out, "[{:7.1}s] {msg}", self.t0.elapsed().as_secs_f64());
        let _ = out.flush();
    }

    /// The class database (UnityPy's lzma.tpk: embedded at build time or $PMU_TPK), for files
    /// without type trees; read once, the first time a file needs it.
    pub fn db(&self) -> Result<Arc<ClassDb>> {
        self.db
            .get_or_init(|| {
                ClassDb::default_db()
                    .map(Arc::new)
                    .map_err(|e| e.to_string())
            })
            .clone()
            .map_err(Error::Exit)
    }

    /// Opens a data file as Loaded does (the class database given to files without type trees).
    pub fn open(&self, path: &Path, mut opts: unityfs::Options) -> Result<unityfs::DataFile> {
        if opts.db.is_none() {
            opts.db = self.db().ok();
        }
        let f = unityfs::DataFile::open(path, &opts)?;
        if !f.sf.enable_type_tree && opts.db.is_none() {
            self.db()?;
        }
        Ok(f)
    }

    // ---- paths

    /// The newest version of a data file: the converted one when a step already wrote it.
    pub fn path_in(&self, rel: &str) -> PathBuf {
        let p = self.out.join(rel);
        if p.exists() {
            p
        } else {
            self.src.join(rel)
        }
    }

    /// Relative paths of the files in one folder of the source data whose names pred accepts,
    /// sorted (job.list).
    pub fn list(&self, dir: &str, pred: impl Fn(&str) -> bool) -> Vec<String> {
        let d = self.src.join(dir);
        let Ok(rd) = std::fs::read_dir(&d) else {
            return Vec::new();
        };
        let mut out: Vec<String> = rd
            .filter_map(|e| e.ok()?.file_name().into_string().ok())
            .filter(|n| pred(n) && d.join(n).is_file())
            .map(|n| {
                if dir.is_empty() {
                    n
                } else {
                    format!("{dir}/{n}")
                }
            })
            .collect();
        out.sort();
        out
    }

    /// os.walk(<src>/<dir>) top-down as the Python walks it: each folder (relative to the
    /// source) with its file names, unsorted, in the order the file system lists them; sub
    /// folders in that order too (symlinked ones not entered).
    pub fn walk(&self, dir: &str) -> Vec<(String, Vec<String>)> {
        fn rec(abs: &Path, rel: &str, out: &mut Vec<(String, Vec<String>)>) {
            let Ok(rd) = std::fs::read_dir(abs) else {
                return;
            };
            let (mut dirs, mut files) = (Vec::new(), Vec::new());
            for e in rd {
                let Ok(e) = e else { break };
                let Ok(name) = e.file_name().into_string() else {
                    continue;
                };
                if e.path().is_dir() {
                    dirs.push(name);
                } else {
                    files.push(name);
                }
            }
            out.push((rel.to_string(), files));
            for d in dirs {
                let p = abs.join(&d);
                if !p.is_symlink() {
                    rec(&p, &format!("{rel}/{d}"), out);
                }
            }
        }
        let mut out = Vec::new();
        rec(&self.src.join(dir), dir, &mut out);
        out
    }

    // ---- writing

    /// Writes a converted file atomically (job.write: <file>.tmp, synced in place, renamed).
    pub fn write(&self, rel: &str, data: &[u8]) -> Result<()> {
        self.write_with(rel, |tmp| Ok(std::fs::write(tmp, data)?))
    }

    /// job.write with the file's bytes written by `f` into the path it is given (a big file
    /// streamed there, never whole in memory).
    pub fn write_with(&self, rel: &str, f: impl FnOnce(&Path) -> Result<()>) -> Result<()> {
        let dst = self.out.join(rel);
        if let Some(d) = dst.parent() {
            std::fs::create_dir_all(d)?;
        }
        let tmp = with_suffix(&dst, ".tmp");
        f(&tmp)?;
        if self.inplace {
            fsync(&tmp)?;
        }
        std::fs::rename(&tmp, &dst)?;
        Ok(())
    }

    /// Puts a converted file written to `path` (on the output's file system) in place.
    pub fn write_file(&mut self, rel: &str, path: &Path) -> Result<()> {
        self.commit(&[(rel.to_string(), path.to_path_buf())])
    }

    /// Puts several converted files [(rel, written path)] in place together: the journal lists
    /// them first, so a job killed between two renames finishes them when it starts again.
    pub fn commit(&mut self, files: &[(String, PathBuf)]) -> Result<()> {
        let mut moves = Vec::new();
        for (rel, path) in files {
            let dst = self.out.join(rel);
            if let Some(d) = dst.parent() {
                std::fs::create_dir_all(d)?;
            }
            if self.inplace {
                fsync(path)?;
            }
            moves.push((path.clone(), dst));
        }
        if moves.len() > 1 {
            let pending = moves
                .iter()
                .map(|(p, d)| Json::List(vec![Json::Str(text(p)), Json::Str(text(d))]))
                .collect();
            self.state.set("pending", Json::List(pending));
            self.save_state()?;
        }
        for (p, d) in &moves {
            std::fs::rename(p, d)?;
        }
        if self.inplace {
            // the renames on the card before the journal says the files are done
            let mut dirs: Vec<&Path> = moves.iter().filter_map(|(_, d)| d.parent()).collect();
            dirs.dedup();
            for d in dirs {
                fsync_dir(d);
            }
        }
        if self.state.pop("pending").is_some_and(|p| p.truthy()) {
            self.save_state()?;
        }
        Ok(())
    }

    fn finish_pending(&mut self) -> Result<()> {
        if let Some(Json::List(moves)) = self.state.pop("pending") {
            for m in moves {
                let pair = m.as_list().map(|l| (l.first(), l.get(1)));
                if let Some((Some(Json::Str(p)), Some(Json::Str(d)))) = pair {
                    if Path::new(p).exists() {
                        std::fs::rename(p, d)?;
                    }
                }
            }
        }
        self.save_state()
    }

    /// An empty scratch folder for a step (<out>/.pmunity-work/<name>: a dot folder, which
    /// packing leaves out).
    pub fn work_dir(&self, name: &str) -> Result<PathBuf> {
        let d = self.out.join(".pmunity-work").join(name);
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d)?;
        Ok(d)
    }

    /// A step's scratch folder removed, and .pmunity-work with it when nothing else is there.
    pub fn end_work(&self, work: &Path) {
        let _ = std::fs::remove_dir_all(work);
        if let Some(p) = work.parent() {
            let _ = std::fs::remove_dir(p);
        }
    }

    // ---- journal

    pub fn is_done(&self, step: &str, rel: &str) -> bool {
        self.state
            .get("done")
            .and_then(|d| d.get(step))
            .and_then(Json::as_list)
            .is_some_and(|l| l.iter().any(|x| x.as_str() == Some(rel)))
    }

    pub fn mark_done(&mut self, step: &str, rel: &str) -> Result<()> {
        let done = self
            .state
            .setdefault("done", Json::dict())
            .and_then(|d| d.setdefault(step, Json::List(Vec::new())));
        match done {
            Some(Json::List(l)) => l.push(Json::Str(rel.to_string())),
            _ => return Err(Error::Value(format!("{STATE}: done.{step} is not a list"))),
        }
        self.save_state()
    }

    /// Writes the journal (json.dump, through <journal>.tmp and a rename), synced before and after
    /// the rename: a handheld that restarts or loses power finds the journal before or after
    /// this save, never an empty one.
    pub fn save_state(&self) -> Result<()> {
        let tmp = with_suffix(&self.state_path, ".tmp");
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(self.state.dumps().as_bytes())?;
        f.sync_all()?;
        drop(f);
        std::fs::rename(&tmp, &self.state_path)?;
        if let Some(d) = self.state_path.parent() {
            fsync_dir(d);
        }
        Ok(())
    }

    /// Stops with status 3 when the output's file system has less than `nbytes` free.
    pub fn need_space(&self, nbytes: u64, what: &str) -> Result<()> {
        let st = rustix::fs::statvfs(&self.out).map_err(std::io::Error::from)?;
        let free = st.f_bavail.saturating_mul(st.f_frsize);
        if free < nbytes {
            self.log(&format!(
                "ERROR: not enough free space to {what}: {} MB free, {} MB needed",
                free >> 20,
                nbytes >> 20
            ));
            return Err(Error::Status(3));
        }
        Ok(())
    }
}
