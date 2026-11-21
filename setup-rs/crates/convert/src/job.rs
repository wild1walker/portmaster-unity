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
use texcodec::prof::{Stage, Timer};
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
    /// When the journal was last written, and whether it holds marks not written yet (see
    /// mark_unchanged).
    saved: std::cell::Cell<Option<Instant>>,
    unsaved: std::cell::Cell<bool>,
    /// Converted files written but not yet put in place (write_deferred), with the marks of the
    /// files after them, in order.
    deferred: Deferred,
    /// The output folder, locked while the job runs (lock_dir).
    _lock: Arc<std::fs::File>,
}

/// Locks a folder for this process (flock on the folder itself: no file is made, and the lock
/// goes with the process, also when it is killed); None when another process holds it. Jobs of
/// one process on the same folder share the lock.
pub fn lock_dir(dir: &Path) -> std::io::Result<Option<Arc<std::fs::File>>> {
    use std::collections::HashMap;
    use std::sync::{Mutex, Weak};
    static HELD: Mutex<Option<HashMap<PathBuf, Weak<std::fs::File>>>> = Mutex::new(None);
    let key = std::fs::canonicalize(dir).unwrap_or_else(|_| dir.to_path_buf());
    let mut held = HELD.lock().unwrap();
    let held = held.get_or_insert_with(HashMap::new);
    if let Some(f) = held.get(&key).and_then(Weak::upgrade) {
        return Ok(Some(f));
    }
    let f = std::fs::File::open(dir)?;
    match rustix::fs::flock(&f, rustix::fs::FlockOperation::NonBlockingLockExclusive) {
        Ok(()) => {}
        Err(rustix::io::Errno::WOULDBLOCK) => return Ok(None),
        // (a file system without locks: as without the lock)
        Err(_) => {}
    }
    let f = Arc::new(f);
    held.insert(key, Arc::downgrade(&f));
    Ok(Some(f))
}

/// How long marks of files a step left unchanged may wait before the journal is written.
const LAZY_SAVE: std::time::Duration = std::time::Duration::from_secs(2);

/// write_deferred's batch: put in place together once it holds DEFER_FILES files, DEFER_BYTES of
/// them (the card holds them beside the files they replace until then), or its first file is
/// DEFER_AGE old (the most a killed run has to convert again).
const DEFER_FILES: usize = 16;
const DEFER_BYTES: u64 = 64 << 20;
const DEFER_AGE: std::time::Duration = std::time::Duration::from_secs(15);
/// Where the deferred files wait (in the scratch folder, `.pmunity-work`).
const DEFER_DIR: &str = ".pmunity-work/commit";

/// A file of the batch (its sync on the syncer thread: the result comes on `synced`), or the mark
/// of a file a step left as it was (kept in its place among the batch's files: the journal lists
/// the files in the order the step finished them).
enum Entry {
    File {
        step: String,
        rel: String,
        tmp: PathBuf,
        dst: PathBuf,
        synced: std::sync::mpsc::Receiver<std::io::Result<()>>,
    },
    Mark {
        step: String,
        rel: String,
    },
}

type SyncReq = (PathBuf, std::sync::mpsc::SyncSender<std::io::Result<()>>);

#[derive(Default)]
struct Deferred {
    entries: Vec<Entry>,
    files: usize,
    bytes: u64,
    since: Option<Instant>,
    /// the thread that syncs the batch's files as they come (fsync), while the step goes on
    syncer: Option<(
        std::sync::mpsc::Sender<SyncReq>,
        std::thread::JoinHandle<()>,
    )>,
}

impl Drop for Deferred {
    fn drop(&mut self) {
        if let Some((tx, t)) = self.syncer.take() {
            drop(tx);
            let _ = t.join();
        }
    }
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

fn fsync(path: &Path) -> Result<()> {
    std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .and_then(|f| f.sync_all())
        .map_err(crate::error::at(path))
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
    pyjson::loads(&text)
        .ok()
        .filter(|j| matches!(j, Json::Dict(_)))
}

/// Status of a conversion whose journal is damaged: the files converted in place before cannot be
/// told from the others, so the setup starts again from the game's files (its message).
pub const DAMAGED_JOURNAL: u8 = 5;

/// What a file's work left in a step's scratch folder (work_dir_lazy) removed, when anything.
pub fn drop_spill(work: &Path) {
    let _t = Timer::new(Stage::WorkDir);
    if work.exists() {
        let _ = std::fs::remove_dir_all(work);
    }
}

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
        // (a second setup started while one runs: two conversions of the same files would
        // undo each other's)
        let Some(lock) = lock_dir(&out)? else {
            return Err(Error::Exit(format!(
                "{}: another pmu convert is converting this folder (a setup still running?): \
                 wait for it to end, then launch again",
                out.display()
            )));
        };
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
            saved: std::cell::Cell::new(None),
            unsaved: std::cell::Cell::new(false),
            deferred: Deferred::default(),
            _lock: lock,
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
            if job.state.get("once").is_some_and(Json::truthy) {
                job.finish_once()?;
            }
        }
        // (deferred files of a killed run that never went in place: converted again)
        let dir = job.out.join(DEFER_DIR);
        if dir.exists() {
            let _ = std::fs::remove_dir_all(&dir);
            if let Some(p) = dir.parent() {
                let _ = std::fs::remove_dir(p);
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
        let f = texcodec::prof::time(Stage::Open, || unityfs::DataFile::open(path, &opts))?;
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
        texcodec::prof::time(Stage::Write, || f(&tmp)).map_err(|e| match e {
            Error::Io(e) => crate::error::at(&tmp)(e),
            Error::Unity(unityfs::Error::Io(e)) => crate::error::at(&tmp)(e),
            e => e,
        })?;
        let _t = Timer::new(Stage::Commit);
        if self.inplace {
            fsync(&tmp)?;
        }
        std::fs::rename(&tmp, &dst).map_err(crate::error::at(&dst))?;
        Ok(())
    }

    /// job.write_with then mark_done(step, rel), in place without waiting for the card: the file
    /// is written to <file>.tmp, synced on a thread of its own while the step goes on, and put in
    /// place with the batch it is in (commit_deferred), the journal written twice for the batch
    /// instead of for each file. The same guarantees: a file is renamed into place only once it
    /// is synced, the renames of a batch are listed in the journal (`pending`) before they are
    /// made, and a file is marked done only after its rename. A run killed before a batch is put
    /// in place converts its files again (their .tmp files written anew). As an overlay: as
    /// write_with and mark_done.
    pub fn write_deferred(
        &mut self,
        step: &str,
        rel: &str,
        f: impl FnOnce(&Path) -> Result<()>,
    ) -> Result<()> {
        if !self.inplace {
            self.write_with(rel, f)?;
            return self.mark_done(step, rel);
        }
        let dst = self.out.join(rel);
        if let Some(d) = dst.parent() {
            std::fs::create_dir_all(d)?;
        }
        // (in the scratch folder, which no step lists: a killed run's leave nothing among the
        // game's files; Job::new removes them)
        let dir = self.out.join(DEFER_DIR);
        std::fs::create_dir_all(&dir)?;
        let name = Path::new(rel)
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        let tmp = dir.join(format!("{}-{name}", self.deferred.files));
        texcodec::prof::time(Stage::Write, || f(&tmp)).map_err(|e| match e {
            Error::Io(e) => crate::error::at(&tmp)(e),
            Error::Unity(unityfs::Error::Io(e)) => crate::error::at(&tmp)(e),
            e => e,
        })?;
        let size = std::fs::metadata(&tmp).map_or(0, |m| m.len());
        let d = &mut self.deferred;
        if d.syncer.is_none() {
            let (tx, rx) = std::sync::mpsc::channel::<SyncReq>();
            let t = std::thread::Builder::new()
                .name("pmu-sync".into())
                .spawn(move || {
                    for (p, reply) in rx {
                        let r = std::fs::OpenOptions::new()
                            .read(true)
                            .write(true)
                            .open(&p)
                            .and_then(|f| f.sync_all());
                        let _ = reply.send(r);
                    }
                })?;
            d.syncer = Some((tx, t));
        }
        let (reply, synced) = std::sync::mpsc::sync_channel(1);
        d.syncer
            .as_ref()
            .unwrap()
            .0
            .send((tmp.clone(), reply))
            .map_err(|_| Error::Value("the sync thread stopped".into()))?;
        d.entries.push(Entry::File {
            step: step.to_string(),
            rel: rel.to_string(),
            tmp,
            dst,
            synced,
        });
        d.files += 1;
        d.bytes += size;
        let since = *d.since.get_or_insert_with(Instant::now);
        if d.files >= DEFER_FILES || d.bytes >= DEFER_BYTES || since.elapsed() >= DEFER_AGE {
            self.commit_deferred()?;
        }
        Ok(())
    }

    /// The bytes of the deferred files waiting beside the files they replace (room they take on
    /// the card until their batch is put in place).
    pub fn deferred_bytes(&self) -> u64 {
        self.deferred.bytes
    }

    /// The deferred batch put in place: its files' syncs waited for, the renames listed in the
    /// journal, made, their folders synced, then the marks (the files' and those of the files left
    /// as they were between them) in order, and the journal written.
    pub fn commit_deferred(&mut self) -> Result<()> {
        if self.deferred.entries.is_empty() {
            return Ok(());
        }
        let _t = Timer::new(Stage::Commit);
        let entries = std::mem::take(&mut self.deferred.entries);
        self.deferred.files = 0;
        self.deferred.bytes = 0;
        self.deferred.since = None;
        let mut moves = Vec::new();
        for e in &entries {
            if let Entry::File {
                tmp, dst, synced, ..
            } = e
            {
                synced
                    .recv()
                    .unwrap_or_else(|_| Err(std::io::Error::other("the sync thread stopped")))
                    .map_err(crate::error::at(tmp))?;
                moves.push((tmp.clone(), dst.clone()));
            }
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
            std::fs::rename(p, d).map_err(crate::error::at(d))?;
        }
        let mut dirs: Vec<&Path> = moves.iter().filter_map(|(_, d)| d.parent()).collect();
        dirs.sort();
        dirs.dedup();
        for d in dirs {
            fsync_dir(d);
        }
        self.state.pop("pending");
        for e in &entries {
            let (Entry::File { step, rel, .. } | Entry::Mark { step, rel }) = e;
            self.add_mark(step, rel)?;
        }
        self.save_state()?;
        // (the scratch folder as a step leaves it: removed when nothing else is there)
        let dir = self.out.join(DEFER_DIR);
        if std::fs::remove_dir(&dir).is_ok() {
            if let Some(p) = dir.parent() {
                let _ = std::fs::remove_dir(p);
            }
        }
        Ok(())
    }

    /// Puts a converted file written to `path` (on the output's file system) in place.
    pub fn write_file(&mut self, rel: &str, path: &Path) -> Result<()> {
        self.commit(&[(rel.to_string(), path.to_path_buf())])
    }

    /// Puts several converted files [(rel, written path)] in place together: the journal lists
    /// them first, so a job killed between two renames finishes them when it starts again.
    pub fn commit(&mut self, files: &[(String, PathBuf)]) -> Result<()> {
        self.commit_inner(files, true)
    }

    /// commit, the journal's end of `pending` written with the caller's next mark_done (one write
    /// of the journal fewer, two syncs on a card): a run killed before that mark finds the
    /// renames done and the file not marked, as it would between commit's last write and the
    /// mark.
    pub fn commit_then_mark(&mut self, files: &[(String, PathBuf)]) -> Result<()> {
        self.commit_inner(files, false)
    }

    fn commit_inner(&mut self, files: &[(String, PathBuf)], save: bool) -> Result<()> {
        let _t = Timer::new(Stage::Commit);
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
            std::fs::rename(p, d).map_err(crate::error::at(d))?;
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
            if save {
                self.save_state()?;
            } else {
                self.unsaved.set(true);
            }
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

    /// write_with and mark_done(step, rel) for a change that must not be made twice (values
    /// scaled in place: a file found changed and not marked would be scaled again). The journal
    /// names the written file before it is put in place (`once`), so a job killed anywhere on
    /// the way ends with the file changed once and marked when it starts again.
    pub fn write_once(
        &mut self,
        step: &str,
        rel: &str,
        f: impl FnOnce(&Path) -> Result<()>,
    ) -> Result<()> {
        self.commit_deferred()?;
        let dst = self.out.join(rel);
        if let Some(d) = dst.parent() {
            std::fs::create_dir_all(d)?;
        }
        let tmp = with_suffix(&dst, ".tmp");
        texcodec::prof::time(Stage::Write, || f(&tmp)).map_err(|e| match e {
            Error::Io(e) => crate::error::at(&tmp)(e),
            Error::Unity(unityfs::Error::Io(e)) => crate::error::at(&tmp)(e),
            e => e,
        })?;
        if self.inplace {
            fsync(&tmp)?;
        }
        let once = [text(&tmp), text(&dst), step.to_string(), rel.to_string()];
        self.state
            .set("once", Json::List(once.into_iter().map(Json::Str).collect()));
        self.save_state()?;
        self.finish_once()
    }

    fn finish_once(&mut self) -> Result<()> {
        let Some(Json::List(l)) = self.state.pop("once") else {
            return self.save_state();
        };
        let s: Vec<&str> = l.iter().filter_map(Json::as_str).collect();
        let [tmp, dst, step, rel] = s[..] else {
            return Err(Error::Value(format!("{STATE}: once is not four strings")));
        };
        let _t = Timer::new(Stage::Commit);
        if Path::new(tmp).exists() {
            std::fs::rename(tmp, dst).map_err(crate::error::at(Path::new(dst)))?;
            if self.inplace {
                if let Some(d) = Path::new(dst).parent() {
                    fsync_dir(d);
                }
            }
        }
        let (step, rel) = (step.to_string(), rel.to_string());
        self.add_mark(&step, &rel)?;
        self.save_state()
    }

    /// An empty scratch folder for a step (<out>/.pmunity-work/<name>: a dot folder, which
    /// packing leaves out).
    pub fn work_dir(&self, name: &str) -> Result<PathBuf> {
        let _t = Timer::new(Stage::WorkDir);
        let d = self.out.join(".pmunity-work").join(name);
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d)?;
        Ok(d)
    }

    /// The step's scratch folder (<out>/.pmunity-work/<name>) as work_dir names it, emptied of
    /// what a killed run left but not made: what spills into it makes it (drop_spill empties it
    /// after each file, end_work removes it). On a handheld's card, making and removing a folder
    /// for each of a thousand files cost more than the files that need one.
    pub fn work_dir_lazy(&self, name: &str) -> Result<PathBuf> {
        let _t = Timer::new(Stage::WorkDir);
        let d = self.out.join(".pmunity-work").join(name);
        if d.exists() {
            std::fs::remove_dir_all(&d)?;
        }
        Ok(d)
    }

    /// A step's scratch folder removed, and .pmunity-work with it when nothing else is there.
    pub fn end_work(&self, work: &Path) {
        let _t = Timer::new(Stage::WorkDir);
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
        // (after the files of a deferred batch, which come before it)
        self.commit_deferred()?;
        self.add_mark(step, rel)?;
        self.save_state()
    }

    /// mark_done for a file the step looked at and left as it was: the journal is written with
    /// the next mark of a changed file, at the end of the step (flush), or when the last write is
    /// LAZY_SAVE old. On a handheld's card each write of the journal costs two syncs; a step that
    /// looks at a thousand files and changes ten no longer waits for two thousand. A run killed
    /// before such a mark is written looks at that file again and leaves it again: the same
    /// files, the same journal at the end.
    pub fn mark_unchanged(&mut self, step: &str, rel: &str) -> Result<()> {
        if !self.deferred.entries.is_empty() {
            // (its place among the deferred files: marked with them)
            self.deferred.entries.push(Entry::Mark {
                step: step.to_string(),
                rel: rel.to_string(),
            });
            return Ok(());
        }
        self.add_mark(step, rel)?;
        self.unsaved.set(true);
        match self.saved.get() {
            Some(t) if t.elapsed() < LAZY_SAVE => Ok(()),
            _ => self.save_state(),
        }
    }

    /// Writes the journal when it holds marks not written yet (the end of a step).
    pub fn flush(&self) -> Result<()> {
        if self.unsaved.get() {
            self.save_state()?;
        }
        Ok(())
    }

    fn add_mark(&mut self, step: &str, rel: &str) -> Result<()> {
        let done = self
            .state
            .setdefault("done", Json::dict())
            .and_then(|d| d.setdefault(step, Json::List(Vec::new())));
        match done {
            Some(Json::List(l)) => l.push(Json::Str(rel.to_string())),
            _ => return Err(Error::Value(format!("{STATE}: done.{step} is not a list"))),
        }
        Ok(())
    }

    /// Writes the journal (json.dump, through <journal>.tmp and a rename), synced before and after
    /// the rename: a handheld that restarts or loses power finds the journal before or after
    /// this save, never an empty one.
    pub fn save_state(&self) -> Result<()> {
        let _t = Timer::new(Stage::Journal);
        let tmp = with_suffix(&self.state_path, ".tmp");
        std::fs::File::create(&tmp)
            .and_then(|mut f| {
                f.write_all(self.state.dumps().as_bytes())?;
                f.sync_all()
            })
            .map_err(crate::error::at(&tmp))?;
        std::fs::rename(&tmp, &self.state_path).map_err(crate::error::at(&self.state_path))?;
        if let Some(d) = self.state_path.parent() {
            fsync_dir(d);
        }
        self.saved.set(Some(Instant::now()));
        self.unsaved.set(false);
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

#[cfg(test)]
mod tests {
    use rustix::fs::{flock, FlockOperation};

    #[test]
    fn the_folder_is_locked_while_a_job_holds_it() {
        let d = std::env::temp_dir().join(format!("pmu-lock-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let held = super::lock_dir(&d).unwrap().unwrap();
        // (another job of this process shares it)
        assert!(super::lock_dir(&d).unwrap().is_some());
        // another open of the folder, as another process's: refused while held
        let other = std::fs::File::open(&d).unwrap();
        assert_eq!(
            flock(&other, FlockOperation::NonBlockingLockExclusive),
            Err(rustix::io::Errno::WOULDBLOCK)
        );
        drop(held);
        assert!(flock(&other, FlockOperation::NonBlockingLockExclusive).is_ok());
        let _ = std::fs::remove_dir_all(&d);
    }

    /// write_deferred in place: nothing in place or journaled until the batch is put in place; then
    /// the files renamed and the marks (with an unchanged file's between them, and one marked
    /// done after them) in the order the step made them, the journal as one file at a time makes it.
    #[test]
    fn deferred_files_go_in_place_together_in_order() {
        use super::*;
        let d = std::env::temp_dir().join(format!("pmu-defer-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("sub")).unwrap();
        for f in ["a", "b", "sub/c", "e"] {
            std::fs::write(d.join(f), b"old").unwrap();
        }
        let journal = |d: &Path| std::fs::read_to_string(d.join(STATE)).unwrap_or_default();
        {
            let mut job = Job::new(&d, &d, Json::dict()).unwrap();
            job.write_deferred("s", "a", |t| Ok(std::fs::write(t, b"new a")?)).unwrap();
            job.mark_unchanged("s", "x").unwrap();
            job.write_deferred("s", "sub/c", |t| Ok(std::fs::write(t, b"new c")?)).unwrap();
            assert_eq!(std::fs::read(d.join("a")).unwrap(), b"old");
            assert!(!journal(&d).contains("\"a\""));
            assert_eq!(job.deferred_bytes(), 10);
            // (a killed run: the batch dropped, nothing in place, its files converted again)
            assert!(d.join(DEFER_DIR).is_dir());
        }
        assert_eq!(std::fs::read(d.join("a")).unwrap(), b"old");
        assert!(!journal(&d).contains("\"a\""));
        let mut job = Job::new(&d, &d, Json::dict()).unwrap();
        assert!(!d.join(".pmunity-work").exists());
        job.write_deferred("s", "a", |t| Ok(std::fs::write(t, b"new a")?)).unwrap();
        job.mark_unchanged("s", "x").unwrap();
        job.write_deferred("s", "sub/c", |t| Ok(std::fs::write(t, b"new c")?)).unwrap();
        job.write_with("e", |t| Ok(std::fs::write(t, b"new e")?)).unwrap();
        job.mark_done("s", "e").unwrap();
        assert_eq!(std::fs::read(d.join("a")).unwrap(), b"new a");
        assert_eq!(std::fs::read(d.join("sub/c")).unwrap(), b"new c");
        assert!(!d.join(".pmunity-work").exists());
        assert_eq!(
            journal(&d),
            r#"{"version": 1, "done": {"s": ["a", "x", "sub/c", "e"]}, "scaled": {}}"#
        );
        // the end of a step puts a batch in place too
        job.write_deferred("t", "b", |t| Ok(std::fs::write(t, b"new b")?)).unwrap();
        job.commit_deferred().unwrap();
        assert_eq!(std::fs::read(d.join("b")).unwrap(), b"new b");
        assert!(job.is_done("t", "b"));
        drop(job);
        let _ = std::fs::remove_dir_all(&d);
    }

    /// write_once: the file changed and marked together; a job killed after the journal named
    /// the written file (before or after its rename) ends with the file changed once and marked
    /// when it starts again; one killed before that finds the file as it was, not marked.
    #[test]
    fn a_file_written_once_is_marked_with_its_change() {
        use super::*;
        let d = std::env::temp_dir().join(format!("pmu-once-{}", std::process::id()));
        let journal = |d: &Path| std::fs::read_to_string(d.join(STATE)).unwrap_or_default();
        let done = r#"{"version": 1, "done": {"refs": ["a"]}, "scaled": {}}"#;
        let fresh = || {
            let _ = std::fs::remove_dir_all(&d);
            std::fs::create_dir_all(&d).unwrap();
            std::fs::write(d.join("a"), b"old").unwrap();
        };
        fresh();
        {
            let mut job = Job::new(&d, &d, Json::dict()).unwrap();
            job.write_once("refs", "a", |t| Ok(std::fs::write(t, b"new")?)).unwrap();
            assert!(job.is_done("refs", "a"));
        }
        assert_eq!(std::fs::read(d.join("a")).unwrap(), b"new");
        assert!(!d.join("a.tmp").exists());
        assert_eq!(journal(&d), done);
        for renamed in [false, true] {
            fresh();
            {
                let mut job = Job::new(&d, &d, Json::dict()).unwrap();
                std::fs::write(d.join("a.tmp"), b"new").unwrap();
                let once = [text(&d.join("a.tmp")), text(&d.join("a")), "refs".into(), "a".into()];
                job.state
                    .set("once", Json::List(once.into_iter().map(Json::Str).collect()));
                job.save_state().unwrap();
                if renamed {
                    std::fs::rename(d.join("a.tmp"), d.join("a")).unwrap();
                }
            }
            let job = Job::new(&d, &d, Json::dict()).unwrap();
            assert!(job.is_done("refs", "a"), "renamed: {renamed}");
            assert_eq!(std::fs::read(d.join("a")).unwrap(), b"new");
            assert!(!d.join("a.tmp").exists());
            drop(job);
            assert_eq!(journal(&d), done);
        }
        fresh();
        {
            let job = Job::new(&d, &d, Json::dict()).unwrap();
            job.save_state().unwrap();
            std::fs::write(d.join("a.tmp"), b"half").unwrap();
        }
        let job = Job::new(&d, &d, Json::dict()).unwrap();
        assert!(!job.is_done("refs", "a"));
        assert_eq!(std::fs::read(d.join("a")).unwrap(), b"old");
        drop(job);
        let _ = std::fs::remove_dir_all(&d);
    }
}
