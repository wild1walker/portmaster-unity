//! il2cpp_device.py's build on the handheld: the steps run natively with the tool folder
//! (`Native`: PATH, HOME and TMPDIR in the work folder, CPU affinity, the process tree's peak
//! memory and CPU time, progress lines), the compiler wrappers given their turns by a `Scheduler`
//! (one compile per CPU as the memory allows), and the steps resumable (`run_steps`).
use crate::util::{self, at, join};
use crate::{bee, exit, log, Error, Result};
use androiddata::pyjson::{self, Json};
use std::collections::HashMap;
use std::io::{BufRead, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

fn env_int(k: &str, default: i64) -> i64 {
    std::env::var(k)
        .ok()
        .and_then(|v| v.trim().parse().ok())
        .unwrap_or(default)
}

/// JOB_MB, RESERVE_MB, BIG_KB (the job slots), SCHED_RESERVE_MB (the scheduler).
pub fn job_mb() -> i64 {
    env_int("PMUNITY_IL2CPP_JOB_MB", 200)
}
pub fn reserve_mb() -> i64 {
    env_int("PMUNITY_IL2CPP_RESERVE_MB", 250)
}
pub fn big_kb() -> i64 {
    env_int("PMUNITY_IL2CPP_BIG_KB", 4096)
}
pub fn sched_reserve_mb() -> f64 {
    env_int("PMUNITY_IL2CPP_SCHED_RESERVE_MB", 160) as f64
}

pub const SCHED_GEN: (f64, f64) = (40.0, 26.0);
pub const SCHED_META: (f64, f64) = (40.0, 37.0);
pub const SCHED_LINK_MB: f64 = 400.0;

/// meminfo: /proc/meminfo in MB.
pub fn meminfo() -> HashMap<String, i64> {
    let mut out = HashMap::new();
    if let Ok(t) = std::fs::read_to_string("/proc/meminfo") {
        for line in t.lines() {
            if let Some((k, v)) = line.split_once(':') {
                if let Some(n) = v
                    .split_whitespace()
                    .next()
                    .and_then(|x| x.parse::<i64>().ok())
                {
                    out.insert(k.to_string(), n / 1024);
                }
            }
        }
    }
    out
}

/// cgroup_limits: (CPUs, MB of memory left) a cgroup v2 limit allows.
pub fn cgroup_limits() -> (Option<i64>, Option<i64>) {
    let mut cpus = None;
    let mut mem = None;
    if let Ok(t) = std::fs::read_to_string("/sys/fs/cgroup/cpu.max") {
        let p: Vec<&str> = t.split_whitespace().collect();
        if p.len() == 2 && p[0] != "max" {
            if let (Ok(q), Ok(per)) = (p[0].parse::<i64>(), p[1].parse::<i64>()) {
                if per != 0 {
                    cpus = Some((q.div_euclid(per)).max(1));
                }
            }
        }
    }
    let lim = std::fs::read_to_string("/sys/fs/cgroup/memory.max").ok();
    if let Some(lim) = lim.map(|l| l.trim().to_string()).filter(|l| l != "max") {
        let cur = std::fs::read_to_string("/sys/fs/cgroup/memory.current")
            .ok()
            .and_then(|c| c.trim().parse::<i64>().ok());
        let mut reclaim = 0i64;
        let mut ok = true;
        match std::fs::read_to_string("/sys/fs/cgroup/memory.stat") {
            Ok(t) => {
                for line in t.lines() {
                    let p: Vec<&str> = line.split_whitespace().collect();
                    if p.len() != 2 {
                        ok = false;
                        break;
                    }
                    if p[0] == "inactive_file" || p[0] == "active_file" {
                        reclaim += p[1].parse::<i64>().unwrap_or(0);
                    }
                }
            }
            Err(_) => ok = false,
        }
        if let (Ok(l), Some(c), true) = (lim.parse::<i64>(), cur, ok) {
            mem = Some((l - c + reclaim).max(0) >> 20);
        }
    }
    (cpus, mem)
}

/// dotnet_heap_limit: .NET's heap limit under a cgroup memory limit (the limit plus the swap the
/// cgroup may use, less 64 MB, at least 3/4 of the limit), else None.
pub fn dotnet_heap_limit(root: &str) -> Option<i64> {
    let num = |name: &str| -> Option<i64> {
        let v = std::fs::read_to_string(join(root, name)).ok()?;
        let v = v.trim();
        if v == "max" || v.is_empty() || !v.bytes().all(|c| c.is_ascii_digit()) {
            return None;
        }
        v.parse().ok()
    };
    let mem = num("memory.max")?;
    let swap = num("memory.swap.max")
        .unwrap_or_else(|| meminfo().get("SwapFree").copied().unwrap_or(0) << 20);
    Some((mem * 3 / 4).max(mem + swap - (64 << 20)))
}

/// The CPUs this process may run on (sched_getaffinity), in order.
pub fn affinity() -> Vec<usize> {
    // SAFETY: a zeroed cpu_set_t filled in by the call
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        if libc::sched_getaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &mut set) != 0 {
            return (0..std::thread::available_parallelism().map_or(1, |n| n.get())).collect();
        }
        (0..libc::CPU_SETSIZE as usize)
            .filter(|&i| libc::CPU_ISSET(i, &set))
            .collect()
    }
}

/// cpu_count: the CPUs this process may use (a cgroup's quota too).
pub fn cpu_count() -> i64 {
    let n = affinity().len() as i64;
    match cgroup_limits().0 {
        Some(q) => n.min(q),
        None => n,
    }
}

/// available_mb: MemAvailable (and a cgroup's limit).
pub fn available_mb() -> i64 {
    let mem = meminfo();
    let avail = mem
        .get("MemAvailable")
        .or(mem.get("MemFree"))
        .copied()
        .unwrap_or(0);
    match cgroup_limits().1 {
        Some(l) => avail.min(l),
        None => avail,
    }
}

/// auto_jobs: compiler jobs that fit (the job slots' count).
pub fn auto_jobs(cpus: i64) -> i64 {
    let avail = available_mb();
    let fit = if avail != 0 {
        (avail - reserve_mb()).div_euclid(job_mb())
    } else {
        1
    };
    1.max(cpus.min(fit))
}

fn proc_ppid_mem(pid: &str) -> Option<(i32, i64, i64)> {
    let st = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    let rest = &st[st.rfind(')')? + 2..];
    let ppid: i32 = rest.split_whitespace().nth(1)?.parse().ok()?;
    let status = std::fs::read_to_string(format!("/proc/{pid}/status")).ok()?;
    let (mut anon, mut filed) = (0i64, 0i64);
    for line in status.lines() {
        let v = || {
            line.split_whitespace()
                .nth(1)
                .and_then(|x| x.parse::<i64>().ok())
        };
        if line.starts_with("RssAnon:") {
            anon = v()?;
        } else if line.starts_with("RssFile:") || line.starts_with("RssShmem:") {
            filed += v()?;
        }
    }
    Some((ppid, anon, filed))
}

/// tree_rss_kb: a process and its descendants' anonymous memory plus the largest file-backed
/// share of one of them (kB).
pub fn tree_rss_kb(root: i32) -> i64 {
    let mut kids: HashMap<i32, Vec<i32>> = HashMap::new();
    let mut mem: HashMap<i32, (i64, i64)> = HashMap::new();
    if let Ok(rd) = std::fs::read_dir("/proc") {
        for e in rd.flatten() {
            let d = e.file_name().to_string_lossy().into_owned();
            if !d.bytes().all(|c| c.is_ascii_digit()) || d.is_empty() {
                continue;
            }
            if let Some((ppid, a, f)) = proc_ppid_mem(&d) {
                let pid: i32 = d.parse().unwrap_or(0);
                mem.insert(pid, (a, f));
                kids.entry(ppid).or_default().push(pid);
            }
        }
    }
    let (mut anon, mut filed) = (0, 0);
    let mut todo = vec![root];
    while let Some(p) = todo.pop() {
        let (a, f) = mem.get(&p).copied().unwrap_or((0, 0));
        anon += a;
        filed = filed.max(f);
        if let Some(k) = kids.get(&p) {
            todo.extend(k);
        }
    }
    anon + filed
}

/// sched_root: a RAM file system for the scheduler's request files (/dev/shm, /tmp), else None.
pub fn sched_root() -> Option<String> {
    let mounts = std::fs::read_to_string("/proc/mounts").unwrap_or_default();
    for d in ["/dev/shm", "/tmp"] {
        let free = crate::device::disk_free(d).unwrap_or(0);
        let c = std::ffi::CString::new(d).unwrap();
        // SAFETY: a valid C string
        let writable = unsafe { libc::access(c.as_ptr(), libc::W_OK) == 0 };
        if !writable || free <= 4 << 20 {
            continue;
        }
        if mounts.lines().any(|l| {
            let p: Vec<&str> = l.split_whitespace().collect();
            p.len() > 2 && p[1] == d && (p[2] == "tmpfs" || p[2] == "ramfs")
        }) {
            return Some(d.into());
        }
    }
    None
}

/// children: a process's children (from /proc/<pid>/task/*/children); None without such files.
pub fn children(pid: i32) -> Option<Vec<i32>> {
    let Ok(tasks) = std::fs::read_dir(format!("/proc/{pid}/task")) else {
        return Some(Vec::new());
    };
    let (mut out, mut seen) = (Vec::new(), false);
    for t in tasks.flatten() {
        let p = t.path().join("children");
        if let Ok(s) = std::fs::read_to_string(&p) {
            let v: std::result::Result<Vec<i32>, _> =
                s.split_whitespace().map(str::parse).collect();
            if let Ok(v) = v {
                out.extend(v);
                seen = true;
            }
        }
    }
    seen.then_some(out)
}

pub fn anon_kb(pid: i32) -> i64 {
    std::fs::read_to_string(format!("/proc/{pid}/status"))
        .ok()
        .and_then(|s| {
            s.lines()
                .find(|l| l.starts_with("RssAnon:"))
                .and_then(|l| l.split_whitespace().nth(1)?.parse().ok())
        })
        .unwrap_or(0)
}

fn alive(pid: i32) -> bool {
    // SAFETY: signal 0 only checks
    let r = unsafe { libc::kill(pid, 0) };
    r == 0 || std::io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
}

/// mkdtemp(prefix='pmunity-cc-', dir=root)
fn mkdtemp(root: &str) -> Result<String> {
    const CH: &[u8] = b"abcdefghijklmnopqrstuvwxyz0123456789_";
    let mut seed = (util::now() * 1e9) as u64 ^ (std::process::id() as u64) << 32;
    for _ in 0..100 {
        let mut name = String::from("pmunity-cc-");
        for _ in 0..8 {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            name.push(CH[(seed % CH.len() as u64) as usize] as char);
        }
        let p = join(root, &name);
        match std::fs::create_dir(&p) {
            Ok(()) => {
                let _ = util::chmod(&p, 0o700);
                return Ok(p);
            }
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(Error::Fail(util::oserror(&e, &p))),
        }
    }
    crate::fail("FileExistsError: no temporary folder name left")
}

/// A running compile: [predicted MB, kind, raw prediction, peak anon kB, started]
struct Run {
    pred: f64,
    kind: &'static str,
    raw: f64,
    peak: i64,
}

struct SchedState {
    running: Vec<(i32, Run)>,
    scale: [(&'static str, f64); 3],
    compiles: i64,
    most_at_once: i64,
    waited: f64,
}

/// The compiles' turns (Scheduler): up to `jobs` at once, the largest waiting first, a further one
/// only while the memory the running ones are predicted to reach leaves room for it.
pub struct Scheduler {
    pub dir: String,
    jobs: usize,
    reserve: f64,
    tick: f64,
    /// MB available (available_mb; a test's own)
    pub available: fn() -> i64,
    st: Mutex<SchedState>,
}

impl Scheduler {
    pub fn new(jobs: usize, root: Option<String>) -> Result<Scheduler> {
        let root = root.or_else(sched_root).unwrap_or_else(|| {
            std::env::var("TMPDIR")
                .ok()
                .filter(|t| !t.is_empty())
                .unwrap_or_else(|| "/tmp".into())
        });
        let dir = mkdtemp(&root)?;
        util::makedirs(&join(&dir, "q"))?;
        util::makedirs(&join(&dir, "go"))?;
        Ok(Scheduler {
            dir,
            jobs: jobs.max(1),
            reserve: sched_reserve_mb(),
            tick: 0.1,
            available: available_mb,
            st: Mutex::new(SchedState {
                running: Vec::new(),
                scale: [("gen", 1.0), ("meta", 1.0), ("link", 1.0)],
                compiles: 0,
                most_at_once: 0,
                waited: 0.0,
            }),
        })
    }

    pub fn retried(&self) -> Vec<Vec<String>> {
        std::fs::read_to_string(join(&self.dir, "retried"))
            .map(|t| {
                t.lines()
                    .filter(|l| !l.trim().is_empty())
                    .map(|l| l.split_whitespace().map(str::to_string).collect())
                    .collect()
            })
            .unwrap_or_default()
    }

    pub fn predict(kb: i64, compile: bool, src: &str) -> (&'static str, f64) {
        if !compile {
            return ("link", SCHED_LINK_MB);
        }
        let meta = util::basename(src).starts_with("Il2Cpp");
        let (base, per) = if meta { SCHED_META } else { SCHED_GEN };
        (
            if meta { "meta" } else { "gen" },
            base + per * kb as f64 / 1024.0,
        )
    }

    fn requests(&self) -> Vec<(i32, (i64, bool, bool, String))> {
        let q = join(&self.dir, "q");
        let mut out = Vec::new();
        let Ok(rd) = std::fs::read_dir(&q) else {
            return out;
        };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n.is_empty() || !n.bytes().all(|c| c.is_ascii_digit()) {
                continue;
            }
            let Ok(t) = std::fs::read_to_string(join(&q, &n)) else {
                continue;
            };
            // (f.read().split(None, 3) + ['', '', '', ''])[:4]
            let mut parts: Vec<String> = Vec::new();
            let mut rest = t.trim_start();
            while parts.len() < 3 && !rest.is_empty() {
                let end = rest.find(char::is_whitespace).unwrap_or(rest.len());
                parts.push(rest[..end].to_string());
                rest = rest[end..].trim_start();
            }
            if !rest.is_empty() {
                parts.push(rest.to_string());
            }
            while parts.len() < 4 {
                parts.push(String::new());
            }
            let Ok(kb) = parts[0].parse::<i64>() else {
                continue;
            };
            let Ok(pid) = n.parse::<i32>() else { continue };
            out.push((
                pid,
                (
                    kb,
                    parts[1] == "1",
                    parts[2] == "alone",
                    parts[3].trim().to_string(),
                ),
            ));
        }
        out
    }

    fn finish_stale(&self, pid: i32) {
        for d in ["q", "go"] {
            let _ = std::fs::remove_file(crate::util::joins(&[&self.dir, d, &pid.to_string()]));
        }
    }

    fn finish(&self, st: &mut SchedState, pid: i32) {
        let i = st.running.iter().position(|r| r.0 == pid).unwrap();
        let (_, r) = st.running.remove(i);
        if r.peak != 0 && r.raw != 0.0 {
            let k = st.scale.iter_mut().find(|s| s.0 == r.kind).unwrap();
            k.1 = k.1.max(r.peak as f64 / 1024.0 / r.raw * 1.1);
        }
        st.compiles += 1;
        if !alive(pid) {
            self.finish_stale(pid);
        }
    }

    pub fn step(&self) -> Result<()> {
        let mut st = self.st.lock().unwrap();
        let go = join(&self.dir, "go");
        let pids: Vec<i32> = st.running.iter().map(|r| r.0).collect();
        for pid in pids {
            if !util::exists(&join(&go, &pid.to_string())) || !alive(pid) {
                self.finish(&mut st, pid);
                continue;
            }
            let kb: i64 = children(pid)
                .unwrap_or_default()
                .into_iter()
                .map(anon_kb)
                .sum();
            let r = st.running.iter_mut().find(|r| r.0 == pid).unwrap();
            r.1.peak = r.1.peak.max(kb);
        }
        let mut reqs: Vec<(i32, (i64, bool, bool, String))> = self
            .requests()
            .into_iter()
            .filter(|(p, _)| !st.running.iter().any(|r| r.0 == *p))
            .collect();
        let dead: Vec<i32> = reqs.iter().map(|r| r.0).filter(|&p| !alive(p)).collect();
        for p in &dead {
            self.finish_stale(*p);
        }
        reqs.retain(|r| !dead.contains(&r.0));
        if reqs.is_empty() || st.running.len() >= self.jobs {
            return Ok(());
        }
        // (not alone, -raw * scale, pid, kind, raw, alone), sorted
        let mut order: Vec<(bool, f64, i32, &'static str, f64, bool)> = reqs
            .iter()
            .map(|(p, (kb, c, alone, src))| {
                let (kind, raw) = Self::predict(*kb, *c, src);
                let sc = st.scale.iter().find(|s| s.0 == kind).unwrap().1;
                (!alone, -raw * sc, *p, kind, raw, *alone)
            })
            .collect();
        order.sort_by(|a, b| {
            (a.0, a.1, a.2)
                .partial_cmp(&(b.0, b.1, b.2))
                .unwrap_or(std::cmp::Ordering::Equal)
        });
        let avail = (self.available)() as f64;
        let mut growth: f64 = st
            .running
            .iter()
            .map(|r| (r.1.pred - r.1.peak as f64 / 1024.0).max(0.0))
            .sum();
        for (_, neg, p, kind, raw, alone) in order {
            let pred = -neg;
            if st.running.len() >= self.jobs || (alone && !st.running.is_empty()) {
                break;
            }
            if !st.running.is_empty() && pred + growth + self.reserve > avail {
                st.waited += self.tick;
                break;
            }
            let g = join(&go, &p.to_string());
            if std::fs::File::create(&g).is_err() {
                break;
            }
            if util::exists(&crate::util::joins(&[&self.dir, "q", &p.to_string()])) {
                // SAFETY: a signal to the waiting wrapper
                if unsafe { libc::kill(p, libc::SIGUSR1) } != 0 {
                    break;
                }
            }
            st.running.push((
                p,
                Run {
                    pred,
                    kind,
                    raw,
                    peak: 0,
                },
            ));
            growth += pred;
            st.most_at_once = st.most_at_once.max(st.running.len() as i64);
            if alone {
                break;
            }
        }
        Ok(())
    }

    /// Runs `f` with the scheduler's thread looking every tick; then the last compiles counted
    /// and the folder removed (a wrapper still waiting then runs).
    pub fn with<R>(&self, f: impl FnOnce() -> R) -> R {
        let stop = AtomicBool::new(false);
        let r = std::thread::scope(|s| {
            s.spawn(|| {
                while !stop.load(Ordering::SeqCst) {
                    std::thread::sleep(std::time::Duration::from_secs_f64(self.tick));
                    if stop.load(Ordering::SeqCst) {
                        break;
                    }
                    if let Err(e) = self.step() {
                        log(&format!(
                            "il2cpp: compile scheduler: {e}; the compiles now run without it"
                        ));
                        util::rmtree(&self.dir);
                        return;
                    }
                }
            });
            let r = f();
            stop.store(true, Ordering::SeqCst);
            r
        });
        let _ = self.step();
        util::rmtree(&self.dir);
        r
    }
}

/// A command's output as Python's text mode reads it: UTF-8 with replacement, universal newlines.
fn text(b: &[u8]) -> String {
    String::from_utf8_lossy(b)
        .replace("\r\n", "\n")
        .replace('\r', "\n")
}

/// os.waitstatus_to_exitcode
fn exit_code(status: libc::c_int) -> i32 {
    if libc::WIFEXITED(status) {
        libc::WEXITSTATUS(status)
    } else if libc::WIFSIGNALED(status) {
        -libc::WTERMSIG(status)
    } else {
        status
    }
}

/// A child spawned with its stderr on its stdout pipe, in a new session, optionally on `cpus`.
fn spawn(
    cmd: &[String],
    env: &[(String, String)],
    cwd: &str,
    cpus: Option<Vec<usize>>,
) -> Result<(i32, std::io::PipeReader)> {
    use std::os::unix::process::CommandExt;
    let (r, w) = std::io::pipe().map_err(|e| Error::Fail(e.to_string()))?;
    let w2 = w.try_clone().map_err(|e| Error::Fail(e.to_string()))?;
    let mut c = std::process::Command::new(&cmd[0]);
    c.args(&cmd[1..])
        .env_clear()
        .envs(env.iter().map(|(a, b)| (a.as_str(), b.as_str())))
        .current_dir(cwd)
        .stdout(w)
        .stderr(w2);
    let set = cpus.map(|cs| {
        // SAFETY: a zeroed cpu_set_t with the chosen CPUs set
        unsafe {
            let mut set: libc::cpu_set_t = std::mem::zeroed();
            for c in cs {
                libc::CPU_SET(c, &mut set);
            }
            set
        }
    });
    // SAFETY: only async-signal-safe calls between fork and exec
    unsafe {
        c.pre_exec(move || {
            libc::setsid();
            if let Some(s) = &set {
                if libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), s) != 0 {
                    return Err(std::io::Error::last_os_error());
                }
            }
            Ok(())
        });
    }
    let child = c.spawn().map_err(|e| {
        Error::Fail(match e.raw_os_error() {
            Some(n) => format!(
                "[Errno {n}] {}: {}",
                util::strerror(n),
                pkgzip::py::str_repr(&cmd[0])
            ),
            None => e.to_string(),
        })
    })?;
    drop(c);
    Ok((child.id() as i32, r))
}

/// wait4: (exit code, CPU seconds of the process and what it waited for)
fn wait4(pid: i32) -> (i32, f64) {
    let mut status: libc::c_int = 0;
    // SAFETY: a zeroed rusage the call fills in
    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    loop {
        let r = unsafe { libc::wait4(pid, &mut status, 0, &mut ru) };
        if r == pid {
            break;
        }
        if r < 0 && std::io::Error::last_os_error().kind() != std::io::ErrorKind::Interrupted {
            return (-1, 0.0);
        }
    }
    let secs = ru.ru_utime.tv_sec as f64
        + ru.ru_utime.tv_usec as f64 / 1e6
        + ru.ru_stime.tv_sec as f64
        + ru.ru_stime.tv_usec as f64 / 1e6;
    (exit_code(status), secs)
}

/// The progress line a step prints every 30 s (seconds since its start -> a line).
pub type Progress<'a> = &'a mut (dyn FnMut(f64) -> Option<String> + Send);

/// Native: the build's commands run on this machine with the tool folder.
pub struct Native {
    pub tc: String,
    pub work: String,
    pub sched: bool,
    pub jobs: usize,
    pub threads: usize,
    pub cpus: usize,
    /// (compiles, most at once, waited for memory s, retried alone, memory scale) once set
    pub sched_stats: Option<(i64, i64, i64, i64, [(&'static str, f64); 3])>,
    /// step log name -> peak MB / CPU seconds, in the order the steps ran
    pub peaks: Vec<(String, i64)>,
    pub cpu: Vec<(String, i64)>,
    pub killed_log: String,
}

fn get(v: &[(String, i64)], k: &str) -> Option<i64> {
    v.iter().find(|x| x.0 == k).map(|x| x.1)
}

fn put(v: &mut Vec<(String, i64)>, k: &str, n: i64) {
    match v.iter_mut().find(|x| x.0 == k) {
        Some(x) => x.1 = n,
        None => v.push((k.to_string(), n)),
    }
}

impl Native {
    pub fn new(tc: &str, jobs: Option<usize>, work: &str) -> Native {
        let cpus = cpu_count().max(1) as usize;
        let sched = std::env::var("PMUNITY_IL2CPP_SCHED").map_or(true, |v| v != "0");
        let jobs_ = match jobs.filter(|&j| j != 0) {
            Some(j) => j.min(cpus).max(1),
            None if sched => cpus,
            None => auto_jobs(cpus as i64) as usize,
        };
        Native {
            tc: tc.into(),
            work: work.into(),
            sched,
            jobs: jobs_,
            threads: if sched { 8.max(3 * cpus) } else { jobs_ },
            cpus,
            sched_stats: None,
            peaks: Vec::new(),
            cpu: Vec::new(),
            killed_log: join(work, "killed.log"),
        }
    }

    pub fn peak(&self, k: &str) -> Option<i64> {
        get(&self.peaks, k)
    }

    pub fn cpu_of(&self, k: &str) -> Option<i64> {
        get(&self.cpu, k)
    }

    /// killed: compilers killed by a signal so far (killed.log's lines).
    pub fn killed(&self) -> Vec<Vec<String>> {
        std::fs::read_to_string(&self.killed_log)
            .map(|t| {
                t.lines()
                    .filter(|l| !l.trim().is_empty())
                    .map(|l| l.split_whitespace().map(str::to_string).collect())
                    .collect()
            })
            .unwrap_or_default()
    }

    /// env: the steps' environment.
    pub fn env(&self) -> Result<Vec<(String, String)>> {
        let tmp = join(&self.work, "tmp");
        util::makedirs(&tmp)?;
        let drop_ = [
            "LD_LIBRARY_PATH",
            "LD_PRELOAD",
            "PYTHONHOME",
            "MONO_PATH",
            "MONO_CFG_DIR",
            "DOTNET_ROOT",
        ];
        let mut e: Vec<(String, String)> = std::env::vars_os()
            .map(|(k, v)| {
                (
                    k.to_string_lossy().into_owned(),
                    v.to_string_lossy().into_owned(),
                )
            })
            .filter(|(k, _)| !drop_.contains(&k.as_str()))
            .collect();
        let path = format!(
            "{}:{}",
            join(&self.tc, "bin"),
            e.iter()
                .find(|x| x.0 == "PATH")
                .map_or("/usr/bin:/bin".to_string(), |x| x.1.clone())
        );
        let set = |e: &mut Vec<(String, String)>, k: &str, v: String| match e
            .iter_mut()
            .find(|x| x.0 == k)
        {
            Some(x) => x.1 = v,
            None => e.push((k.into(), v)),
        };
        set(&mut e, "PATH", path);
        for (k, v) in [
            ("HOME", tmp.clone()),
            ("TMPDIR", tmp.clone()),
            ("DOTNET_CLI_HOME", tmp.clone()),
            ("DOTNET_CLI_TELEMETRY_OPTOUT", "1".into()),
            ("DOTNET_NOLOGO", "1".into()),
            ("DOTNET_ROLL_FORWARD", "Major".into()),
            ("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1".into()),
            ("DOTNET_gcServer", "0".into()),
            ("DOTNET_GCgen0size", "0x2000000".into()),
            ("DOTNET_GCConserveMemory", "9".into()),
            ("DOTNET_TieredCompilation", "0".into()),
            ("PMUNITY_LLVM", join(&self.tc, "bin")),
            ("PMUNITY_IL2CPP_CCLOG", join(&self.work, "cc.log")),
            ("PMUNITY_JOBS", self.jobs.to_string()),
            ("PMUNITY_JOBS_DIR", join(&self.work, "jobs")),
            (
                "PMUNITY_JOB_MEM_KB",
                ((job_mb() + reserve_mb().div_euclid(2)) * 1024).to_string(),
            ),
            ("PMUNITY_IL2CPP_BIG_KB", big_kb().to_string()),
            ("PMUNITY_IL2CPP_KILLED", self.killed_log.clone()),
        ] {
            set(&mut e, k, v);
        }
        if let Some(hard) = dotnet_heap_limit("/sys/fs/cgroup").filter(|&h| h != 0) {
            if std::env::var_os("DOTNET_GCHeapHardLimit").is_none() {
                set(&mut e, "DOTNET_GCHeapHardLimit", format!("{hard:#x}"));
            }
        }
        Ok(e)
    }

    /// Runs `f` with the scheduler's environment additions (when `on` and the scheduler is used);
    /// its figures go to sched_stats.
    /// (A step that fails leaves the figures as they were, as the Python's context manager.)
    fn scheduled<T>(
        &mut self,
        on: bool,
        f: impl FnOnce(&mut Self, Vec<(String, String)>) -> Result<T>,
    ) -> Result<T> {
        if !(on && self.sched) {
            return f(self, Vec::new());
        }
        let s = Scheduler::new(self.jobs, None)?;
        let extra = vec![
            ("PMUNITY_SCHED".to_string(), s.dir.clone()),
            (
                "DOTNET_PROCESSOR_COUNT".to_string(),
                self.threads.to_string(),
            ),
        ];
        let r = s.with(|| f(self, extra))?;
        let st = s.st.lock().unwrap();
        let (c, m, w, ra, _) = self.sched_stats.unwrap_or((0, 0, 0, 0, [("gen", 1.0); 3]));
        // (the Python reads `retried` after the folder is gone)
        let retried = s.retried();
        let scale = [
            (st.scale[0].0, util::py_round2(st.scale[0].1)),
            (st.scale[1].0, util::py_round2(st.scale[1].1)),
            (st.scale[2].0, util::py_round2(st.scale[2].1)),
        ];
        self.sched_stats = Some((
            c + st.compiles,
            m.max(st.most_at_once),
            util::py_round(w as f64 + st.waited),
            ra + retried.len() as i64,
            scale,
        ));
        if !retried.is_empty() {
            log(&format!(
                "il2cpp: {} compile(s) killed (out of memory, most likely) and run again alone",
                retried.len()
            ));
        }
        Ok(r)
    }

    /// run: a step's command, its output in <work>/<logname>; SystemExit with the output's last
    /// lines when it fails. Returns the seconds it took.
    pub fn run(
        &mut self,
        cmd: &[String],
        logname: &str,
        affinity: Option<usize>,
        sched: bool,
        progress: Option<Progress>,
    ) -> Result<f64> {
        self.scheduled(sched, |me, extra| {
            me.run_inner(cmd, logname, affinity, extra, progress)
        })
    }

    fn run_inner(
        &mut self,
        cmd: &[String],
        logname: &str,
        affinity: Option<usize>,
        extra: Vec<(String, String)>,
        progress: Option<Progress>,
    ) -> Result<f64> {
        let jd = join(&self.work, "jobs");
        util::rmtree(&jd);
        util::makedirs(&jd)?;
        let path = join(&self.work, logname);
        let n = affinity.unwrap_or(if extra.is_empty() {
            self.jobs
        } else {
            self.cpus
        });
        let cpus: Vec<usize> = affinity_list().into_iter().take(n).collect();
        let mut env = self.env()?;
        for (k, v) in extra {
            match env.iter_mut().find(|x| x.0 == k) {
                Some(x) => x.1 = v,
                None => env.push((k, v)),
            }
        }
        let t0 = util::now();
        let mut lf = std::fs::File::create(&path).map_err(at(&path))?;
        let (pid, reader) = spawn(cmd, &env, &self.work, (!cpus.is_empty()).then_some(cpus))?;
        let peak = Arc::new(Mutex::new(0i64));
        let stop = AtomicBool::new(false);
        let mut tail: Vec<String> = Vec::new();
        let (code, cpu) = std::thread::scope(|s| {
            let peak2 = peak.clone();
            let stop = &stop;
            let mut progress = progress;
            s.spawn(move || {
                let mut last = util::now();
                loop {
                    for _ in 0..20 {
                        if stop.load(Ordering::SeqCst) {
                            return;
                        }
                        std::thread::sleep(std::time::Duration::from_millis(100));
                    }
                    let kb = tree_rss_kb(pid);
                    let mut p = peak2.lock().unwrap();
                    *p = (*p).max(kb);
                    drop(p);
                    if let Some(pr) = progress.as_mut() {
                        if util::now() - last >= 30.0 {
                            last = util::now();
                            if let Some(m) = pr(util::now() - t0) {
                                log(&m);
                            }
                        }
                    }
                }
            });
            let mut r = std::io::BufReader::new(reader);
            let mut line = Vec::new();
            loop {
                line.clear();
                match r.read_until(b'\n', &mut line) {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {
                        let t = text(&line);
                        let _ = lf.write_all(t.as_bytes());
                        let _ = lf.flush();
                        for piece in t.split_inclusive('\n') {
                            tail.push(piece.to_string());
                            if tail.len() > 40 {
                                tail.remove(0);
                            }
                        }
                    }
                }
            }
            let r = wait4(pid);
            stop.store(true, Ordering::SeqCst);
            r
        });
        let secs = util::now() - t0;
        put(&mut self.peaks, logname, *peak.lock().unwrap() / 1024);
        put(&mut self.cpu, logname, util::py_round(cpu));
        if code != 0 {
            eprint!("{}", tail.concat());
            let prog = cmd
                .iter()
                .find(|w| w.ends_with(".exe") || w.ends_with(".dll"))
                .map(|w| util::basename(w).to_string())
                .unwrap_or_else(|| cmd[0].clone());
            let mut hint = if code == -9 || code == 137 {
                " (killed: out of memory? see docs/il2cpp.md, \"On the handheld\")".to_string()
            } else {
                String::new()
            };
            let k = self.killed();
            if let Some(l) = k.last() {
                hint.push_str(&format!(
                    "; {} compiler(s) were killed by a signal ({}, the last on {}): out of memory, most likely",
                    k.len(),
                    l.first().map_or("", |s| s.as_str()),
                    util::basename(l.get(1).map_or("", |s| s.as_str()))
                ));
            }
            return exit(format!(
                "{prog} failed (exit {code}){hint}; full log: {path}"
            ));
        }
        Ok(secs)
    }

    /// shell: many commands (the Bee graph's), from several threads; their peak memory and CPU
    /// time are the 'build.log' step's.
    pub fn shell<T>(&mut self, f: impl FnOnce(&NativeShell) -> Result<T>) -> Result<T> {
        self.scheduled(true, |me, extra| -> Result<T> {
            let jd = join(&me.work, "jobs");
            util::rmtree(&jd);
            util::makedirs(&jd)?;
            let mut base = me.env()?;
            for (k, v) in extra {
                match base.iter_mut().find(|x| x.0 == k) {
                    Some(x) => x.1 = v,
                    None => base.push((k, v)),
                }
            }
            let sh = NativeShell {
                base,
                pids: Mutex::new(Vec::new()),
                cpu: Mutex::new(0.0),
            };
            let stop = AtomicBool::new(false);
            let peak = Mutex::new(0i64);
            let r = std::thread::scope(|s| {
                s.spawn(|| loop {
                    for _ in 0..20 {
                        if stop.load(Ordering::SeqCst) {
                            return;
                        }
                        std::thread::sleep(std::time::Duration::from_millis(100));
                    }
                    let now: Vec<i32> = sh.pids.lock().unwrap().clone();
                    let kb: i64 = now.into_iter().map(tree_rss_kb).sum();
                    let mut p = peak.lock().unwrap();
                    *p = (*p).max(kb);
                });
                let r = f(&sh);
                stop.store(true, Ordering::SeqCst);
                r
            });
            let pk = get(&me.peaks, "build.log")
                .unwrap_or(0)
                .max(*peak.lock().unwrap() / 1024);
            put(&mut me.peaks, "build.log", pk);
            let c =
                get(&me.cpu, "build.log").unwrap_or(0) + util::py_round(*sh.cpu.lock().unwrap());
            put(&mut me.cpu, "build.log", c);
            r
        })
    }
}

fn affinity_list() -> Vec<usize> {
    affinity()
}

/// The shell Native.shell gives the Bee graph.
pub struct NativeShell {
    base: Vec<(String, String)>,
    pids: Mutex<Vec<i32>>,
    cpu: Mutex<f64>,
}

impl bee::Shell for NativeShell {
    fn sh(&self, command: &str, cwd: &str, env: &[(String, String)]) -> Result<(i32, String)> {
        let mut e = self.base.clone();
        for (k, v) in env {
            match e.iter_mut().find(|x| x.0 == *k) {
                Some(x) => x.1 = v.clone(),
                None => e.push((k.clone(), v.clone())),
            }
        }
        let cmd = vec!["/bin/sh".to_string(), "-c".to_string(), command.to_string()];
        let (pid, mut reader) = spawn(&cmd, &e, cwd, None)?;
        self.pids.lock().unwrap().push(pid);
        let mut out = Vec::new();
        let _ = std::io::Read::read_to_end(&mut reader, &mut out);
        let (code, secs) = wait4(pid);
        let mut p = self.pids.lock().unwrap();
        p.retain(|&x| x != pid);
        *self.cpu.lock().unwrap() += secs;
        Ok((code, text(&out)))
    }
}

// ------------------------------------------------------------------ the steps, resumable

/// _digest: a command and the contents of folders.
pub fn digest(cmd: &[String], dirs: &[&str]) -> Result<String> {
    let mut h = crate::hash::Sha256::default();
    h.update(pyjson::dumps(&strs_json(cmd), None).as_bytes());
    for d in dirs {
        for w in util::walk(d, true) {
            for n in &w.files {
                let p = join(&w.root, n);
                h.update(util::relpath(&p, d).as_bytes());
                h.update_file(&p)?;
            }
        }
    }
    Ok(h.hexdigest())
}

pub fn strs_json(v: &[String]) -> Json {
    Json::List(v.iter().map(Json::str).collect())
}

fn done(work: &str, step: &str, id: &str) -> bool {
    std::fs::read_to_string(join(work, &format!(".done-{step}"))).is_ok_and(|t| t.trim() == id)
}

fn mark(work: &str, step: &str, id: &str) -> Result<()> {
    util::write_text(&join(work, &format!(".done-{step}")), id)
}

pub const FILE_COST: u64 = 300 << 10;

fn size(p: &str) -> u64 {
    std::fs::metadata(p).map_or(0, |m| m.len())
}

/// sources_estimate: (files, cost) of what il2cpp's builder compiles.
pub fn sources_estimate(cpp: &str, editor: &str) -> (usize, u64) {
    let is_src = |x: &str| x.ends_with(".cpp") || x.ends_with(".c");
    let mut files: Vec<String> = Vec::new();
    for w in util::walk(cpp, false) {
        files.extend(
            w.files
                .iter()
                .filter(|x| is_src(x))
                .map(|x| join(&w.root, x)),
        );
    }
    let lib = util::joins(&[editor, "il2cpp", "libil2cpp"]);
    for w in util::walk(&lib, false) {
        files.extend(
            w.files
                .iter()
                .filter(|x| is_src(x))
                .map(|x| join(&w.root, x)),
        );
    }
    for gc in ["boehmgc", "bdwgc"] {
        let extra = util::joins(&[editor, "il2cpp", "external", gc, "extra"]);
        if util::isdir(&extra) {
            for x in util::listdir(&extra).unwrap_or_default() {
                if x.ends_with(".c") {
                    files.push(join(&extra, &x));
                }
            }
        }
    }
    let cost = files.iter().map(|f| size(f) + FILE_COST).sum();
    (files.len(), cost)
}

/// CompileLog: the compiles started so far, from cc.log.
pub struct CompileLog {
    path: String,
    pos: u64,
    pub n: u64,
    pub cost: u64,
    rx: regex::Regex,
}

impl CompileLog {
    pub fn new(path: &str) -> Self {
        CompileLog {
            path: path.into(),
            pos: 0,
            n: 0,
            cost: 0,
            rx: regex::Regex::new(r" (\S+\.(?:cpp|c|cc)) -o ").unwrap(),
        }
    }

    pub fn read(&mut self) {
        use std::io::{Read, Seek, SeekFrom};
        let Ok(mut f) = std::fs::File::open(&self.path) else {
            return;
        };
        if f.seek(SeekFrom::Start(self.pos)).is_err() {
            return;
        }
        let mut data = Vec::new();
        if f.read_to_end(&mut data).is_err() {
            return;
        }
        let end = data.iter().rposition(|&c| c == b'\n').map_or(0, |i| i + 1);
        self.pos += end as u64;
        for line in String::from_utf8_lossy(&data[..end]).lines() {
            if let Some(c) = self.rx.captures(line) {
                self.n += 1;
                self.cost += size(&c[1]) + FILE_COST;
            }
        }
    }
}

pub fn count_objects(cache: &str) -> usize {
    util::walk(cache, false)
        .iter()
        .map(|w| w.files.iter().filter(|x| x.ends_with(".o")).count())
        .sum()
}

fn mb(path: &str) -> f64 {
    if util::isdir(path) {
        crate::device::du(path) as f64 / (1u64 << 20) as f64
    } else {
        0.0
    }
}

/// on_mono: an il2cpp.exe command run on .NET 8 -> the same on Mono.
pub fn on_mono(cmd: &[String]) -> Option<Vec<String>> {
    if cmd.len() > 4
        && cmd[0] == "dotnet"
        && cmd[1] == "exec"
        && cmd[2] == "--runtimeconfig"
        && cmd[4].ends_with(".exe")
    {
        let mut v = vec!["mono".to_string(), "--gc=sgen".to_string()];
        v.extend(cmd[4..].iter().cloned());
        return Some(v);
    }
    None
}

fn outputpath(cmd: &[String]) -> Option<String> {
    cmd.iter()
        .find_map(|w| w.strip_prefix("--outputpath=").map(str::to_string))
}

pub const SIDE_FILES: [&str; 2] = ["libil2cpp.so.debug", "libil2cpp.sym"];

/// run_bee_graph: Unity 2021.2+: the graph il2cpp wrote, run here. Returns the seconds spent.
pub fn run_bee_graph(
    nb: &mut Native,
    editor: &str,
    cache: &str,
    work: &str,
    progress: &mut dyn FnMut(usize, usize, f64) -> Option<String>,
) -> Result<f64> {
    if !bee::uses_bee(editor) {
        return Ok(0.0);
    }
    let dag = bee::graph_path(cache);
    if !util::isfile(&dag) {
        return exit(format!("il2cpp wrote no build graph ({dag})"));
    }
    let t0 = util::now();
    let threads = nb.threads;
    let logf = join(work, "build.log");
    let (ran, before) =
        nb.shell(|sh| bee::run_graph(&dag, sh, threads, &|m| log(m), &logf, None, progress, 30.0))?;
    let t = util::now() - t0;
    log(&format!(
        "il2cpp: build graph run, {ran} commands ({before} done before) in {t:.0} s"
    ));
    Ok(t)
}

/// clear_bee_graph: an earlier run's graph removed before il2cpp writes this one.
pub fn clear_bee_graph(editor: &str, cache: &str) -> Result<()> {
    if bee::uses_bee(editor) {
        for n in ["bee.dag.json", bee::ARGS_FILE] {
            let p = util::joins(&[cache, "buildstate", n]);
            if util::lexists(&p) {
                util::unlink(&p)?;
            }
        }
    }
    Ok(())
}

/// run_steps: UnityLinker, the conversion to C++, then the build of libil2cpp.so, each skipped
/// when a run before finished it with the same inputs. Returns the seconds spent.
#[allow(clippy::too_many_arguments)]
pub fn run_steps(
    nb: &mut Native,
    lcmd: &[String],
    icmd: &[String],
    work: &str,
    staged: &str,
    linked: &str,
    cpp: &str,
    cache: &str,
    editor: &str,
    strip: &str,
) -> Result<f64> {
    let mut secs = 0.0;
    let free = crate::device::disk_free(work)?;
    let need: u64 = 1536 << 20;
    if free < need && !util::isdir(cpp) {
        return exit(format!(
            "{work}: {} MB free; the build needs about {} MB (generated C++ and objects, removed when it is done)",
            free >> 20,
            need >> 20
        ));
    }
    let mem = meminfo();
    let jobs = if nb.sched {
        format!("up to {} compiles at a time as the memory allows", nb.jobs)
    } else {
        format!("{} compiler job(s)", nb.jobs)
    };
    log(&format!(
        "IL2CPP build on this machine: {} CPUs, {} MB RAM ({} MB available, swap {} MB free), {jobs}; it continues where it stopped if interrupted",
        nb.cpus,
        mem.get("MemTotal").copied().unwrap_or(0),
        available_mb(),
        mem.get("SwapFree").copied().unwrap_or(0)
    ));
    let mut link_id = "none".to_string();
    if strip != "none" {
        link_id = digest(lcmd, &[staged])?;
        if done(work, "unitylinker", &link_id) && util::isdir(linked) {
            log("UnityLinker: done before");
        } else {
            log("UnityLinker: removing the unused parts of the class libraries (a few minutes)");
            util::rmtree(linked);
            let t = nb.run(lcmd, "unitylinker.log", Some(1), false, None)?;
            secs += t;
            let kept = util::listdir(linked)?
                .into_iter()
                .filter(|n| n.ends_with(".dll"))
                .count();
            log(&format!(
                "UnityLinker: {kept} assemblies, {:.1} MiB, in {:.1} min (CPU {:.1} min, peak {} MB)",
                mb(linked),
                t / 60.0,
                nb.cpu_of("unitylinker.log").unwrap_or(0) as f64 / 60.0,
                nb.peak("unitylinker.log").unwrap_or(0)
            ));
            mark(work, "unitylinker", &link_id)?;
        }
    }
    let mut icmd: Vec<String> = icmd.to_vec();
    let conv_of = |icmd: &[String]| -> Vec<String> {
        icmd.iter()
            .filter(|w| *w != "--compile-cpp")
            .cloned()
            .collect()
    };
    let conv_id_of = |conv: &[String]| -> String {
        let j = Json::List(vec![strs_json(conv), Json::str(&link_id)]);
        crate::hash::sha256_hex(pyjson::dumps(&j, None).as_bytes())
    };
    let mut conv = conv_of(&icmd);
    let mut conv_id = conv_id_of(&conv);
    if done(work, "convert", &conv_id) && util::isdir(cpp) {
        log("il2cpp: C++ written before");
    } else {
        log("il2cpp: converting the game's code to C++ (one CPU: 10 minutes to an hour on a handheld)");
        util::rmtree(cpp);
        let cpp_s = cpp.to_string();
        let mut prog = move |el: f64| -> Option<String> {
            Some(format!(
                "  converting: {:.0} min, {:.0} MiB of C++ so far",
                el / 60.0,
                mb(&cpp_s)
            ))
        };
        let t = match nb.run(&conv, "il2cpp-convert.log", Some(1), false, Some(&mut prog)) {
            Ok(t) => t,
            Err(Error::Exit(m)) => {
                let peak = nb.peak("il2cpp-convert.log").unwrap_or(0) as f64;
                if on_mono(&conv).is_none() || peak > available_mb() as f64 * 0.9 {
                    return Err(Error::Exit(m));
                }
                log("il2cpp: il2cpp.exe failed on .NET 8: converting again on Mono (as Unity's Linux and macOS editors run it)");
                icmd = on_mono(&icmd).unwrap_or(icmd);
                conv = conv_of(&icmd);
                conv_id = conv_id_of(&conv);
                util::rmtree(cpp);
                nb.run(&conv, "il2cpp-convert.log", Some(1), false, Some(&mut prog))?
            }
            Err(e) => return Err(e),
        };
        secs += t;
        log(&format!(
            "il2cpp: {:.0} MiB of C++ in {:.1} min (CPU {:.1} min, peak {} MB)",
            mb(cpp),
            t / 60.0,
            nb.cpu_of("il2cpp-convert.log").unwrap_or(0) as f64 / 60.0,
            nb.peak("il2cpp-convert.log").unwrap_or(0)
        ));
        mark(work, "convert", &conv_id)?;
    }
    let (total, cost) = sources_estimate(cpp, editor);
    let before = count_objects(cache);
    let mut comp: Vec<String> = icmd
        .iter()
        .filter(|w| *w != "--convert-to-cpp")
        .cloned()
        .collect();
    if nb.sched {
        let th = nb.threads;
        comp = comp
            .into_iter()
            .map(|w| {
                if w.starts_with("--jobs=") {
                    format!("--jobs={th}")
                } else {
                    w
                }
            })
            .collect();
    }
    let at_ = if nb.sched {
        format!("up to {}", nb.jobs)
    } else {
        nb.jobs.to_string()
    };
    if bee::uses_bee(editor) {
        log(&format!(
            "il2cpp: compiling ({at_} at a time), then linking (the long part: half an hour to a few hours on a 4-core ARM handheld)"
        ));
    } else {
        log(&format!(
            "il2cpp: compiling about {total} files ({before} done before), {at_} at a time, then linking (the long part: half an hour to a few hours on a 4-core ARM handheld)"
        ));
    }
    let left0 = cost as f64 * (total.saturating_sub(before)) as f64 / total.max(1) as f64;
    let so = outputpath(&comp);
    for n in SIDE_FILES {
        if let Some(so) = &so {
            let p = join(util::dirname(so), n);
            if util::exists(&p) {
                util::unlink(&p)?;
            }
        }
    }
    let killed = nb.killed().len();
    let cache_s = cache.to_string();
    let cclog = Arc::new(Mutex::new(CompileLog::new(&join(work, "cc.log"))));
    let compile = |nb: &mut Native| -> Result<f64> {
        clear_bee_graph(editor, cache)?;
        let cl = cclog.clone();
        let cache2 = cache_s.clone();
        let mut prog = move |el: f64| -> Option<String> {
            let n = count_objects(&cache2);
            let mut c = cl.lock().unwrap();
            c.read();
            let mut eta = String::new();
            if c.n >= 5 && c.cost != 0 {
                eta = format!(
                    ", about {:.0} min left",
                    (el / c.cost as f64 * (left0 - c.cost as f64) / 60.0).max(1.0)
                );
            }
            let m = n.min(total);
            Some(format!(
                "  compiled {m} of ~{total} files ({}%), {:.0} min{eta}",
                100 * m / total.max(1),
                el / 60.0
            ))
        };
        let a = nb.run(&comp, "il2cpp.log", None, true, Some(&mut prog))?;
        let mut bee_progress = |n: usize, total: usize, el: f64| -> Option<String> {
            let eta = if n >= 5 {
                format!(
                    ", about {:.0} min left",
                    (el / n as f64 * (total as f64 - n as f64) / 60.0).max(1.0)
                )
            } else {
                String::new()
            };
            Some(format!(
                "  compiled {n} of {total} ({}%), {:.0} min{eta}",
                100 * n / total.max(1),
                el / 60.0
            ))
        };
        Ok(a + run_bee_graph(nb, editor, cache, work, &mut bee_progress)?)
    };
    let t = match compile(nb) {
        Ok(t) => t,
        Err(Error::Exit(m)) => {
            if nb.killed().len() == killed || nb.jobs == 1 {
                return Err(Error::Exit(m));
            }
            log("il2cpp: a compiler was killed (out of memory, most likely): going on with one job at a time");
            nb.jobs = 1;
            compile(nb)?
        }
        Err(e) => return Err(e),
    };
    secs += t;
    log(&format!(
        "il2cpp: compiled and linked in {:.1} min (CPU {:.1} min, peak {} MB)",
        t / 60.0,
        (nb.cpu_of("il2cpp.log").unwrap_or(0) + nb.cpu_of("build.log").unwrap_or(0)) as f64 / 60.0,
        nb.peak("il2cpp.log")
            .unwrap_or(0)
            .max(nb.peak("build.log").unwrap_or(0))
    ));
    for n in SIDE_FILES {
        if let Some(so) = &so {
            let p = join(util::dirname(so), n);
            if util::exists(&p) {
                if n.ends_with(".debug") {
                    util::unlink(&p)?;
                } else {
                    util::replace(&p, &join(work, n))?;
                }
            }
        }
    }
    Ok(secs)
}

/// report: il2cpp.json's on_device.
pub fn report(nb: &Native) -> Json {
    let ints = |v: &[(String, i64)]| {
        Json::Obj(
            v.iter()
                .map(|(k, n)| (k.clone(), Json::Int(*n as i128)))
                .collect(),
        )
    };
    let sched = match (&nb.sched_stats, nb.sched) {
        (Some((c, m, w, r, sc)), true) => Json::Obj(vec![
            ("compiles".into(), Json::Int(*c as i128)),
            ("most_at_once".into(), Json::Int(*m as i128)),
            ("waited_for_memory_s".into(), Json::Int(*w as i128)),
            ("retried_alone".into(), Json::Int(*r as i128)),
            (
                "memory_scale".into(),
                Json::Obj(
                    sc.iter()
                        .map(|(k, v)| (k.to_string(), Json::Float(*v)))
                        .collect(),
                ),
            ),
        ]),
        (None, true) => Json::obj(),
        _ => Json::Null,
    };
    Json::Obj(vec![
        ("jobs".into(), Json::Int(nb.jobs as i128)),
        ("cpus".into(), Json::Int(nb.cpus as i128)),
        ("scheduler".into(), sched),
        ("peak_mb".into(), ints(&nb.peaks)),
        ("cpu_seconds".into(), ints(&nb.cpu)),
        (
            "mem_total_mb".into(),
            meminfo()
                .get("MemTotal")
                .map_or(Json::Null, |&m| Json::Int(m as i128)),
        ),
    ])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn predictions() {
        assert_eq!(Scheduler::predict(0, false, ""), ("link", 400.0));
        assert_eq!(
            Scheduler::predict(2048, true, "/x/Il2CppAttributes.cpp"),
            ("meta", 114.0)
        );
        assert_eq!(
            Scheduler::predict(1024, true, "/x/Bulk_0.cpp"),
            ("gen", 66.0)
        );
        assert_eq!(
            on_mono(&[
                "dotnet".into(),
                "exec".into(),
                "--runtimeconfig".into(),
                "rc".into(),
                "/a/il2cpp.exe".into(),
                "--x".into()
            ]),
            Some(vec![
                "mono".into(),
                "--gc=sgen".into(),
                "/a/il2cpp.exe".into(),
                "--x".into()
            ])
        );
        assert_eq!(
            on_mono(&[
                "dotnet".into(),
                "exec".into(),
                "--runtimeconfig".into(),
                "rc".into(),
                "/a/il2cpp.dll".into()
            ]),
            None
        );
    }

    /// The turns: the largest request first, a second one only while the memory holds it, a
    /// request "alone" with nothing else running, a gone wrapper's turn ended.
    #[test]
    fn scheduler_turns() {
        let root = std::env::temp_dir().join(format!("il2cpp-sched-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let mut s = Scheduler::new(4, Some(root.to_str().unwrap().into())).unwrap();
        s.reserve = 100.0;
        s.available = || 900;
        let sleeper = || {
            std::process::Command::new("/bin/sh")
                .args(["-c", "trap '' USR1; sleep 30"])
                .spawn()
                .unwrap()
        };
        let mut kids: Vec<std::process::Child> = (0..3).map(|_| sleeper()).collect();
        let q = |c: &std::process::Child, text: &str| {
            std::fs::write(format!("{}/q/{}", s.dir, c.id()), text).unwrap()
        };
        // 600 MB predicted (meta, 15 MiB), 300 MB (gen, ~10 MiB), 300 MB
        q(&kids[0], "15155 1 - /x/Il2CppAttributes.cpp");
        q(&kids[1], "10240 1 - /x/Bulk_0.cpp");
        q(&kids[2], "10240 1 - /x/Bulk_1.cpp");
        s.step().unwrap();
        let go = |c: &std::process::Child| {
            std::path::Path::new(&format!("{}/go/{}", s.dir, c.id())).exists()
        };
        // the largest went; 588 + 300 + 100 reserve > 900: the next waits
        assert!(go(&kids[0]) && !go(&kids[1]) && !go(&kids[2]));
        s.step().unwrap();
        assert!(!go(&kids[1]));
        // the first one done (its go file removed by its wrapper): the next two fit
        std::fs::remove_file(format!("{}/go/{}", s.dir, kids[0].id())).unwrap();
        std::fs::remove_file(format!("{}/q/{}", s.dir, kids[0].id())).unwrap();
        s.step().unwrap();
        assert!(go(&kids[1]) && go(&kids[2]));
        assert_eq!(s.st.lock().unwrap().compiles, 1);
        assert_eq!(s.st.lock().unwrap().most_at_once, 2);
        for k in kids.iter_mut() {
            let _ = k.kill();
            let _ = k.wait();
        }
        // gone wrappers: their turns end, their files go
        s.step().unwrap();
        assert_eq!(s.st.lock().unwrap().running.len(), 0);
        assert_eq!(
            std::fs::read_dir(format!("{}/go", s.dir)).unwrap().count(),
            0
        );
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn runs_a_step() {
        let d = std::env::temp_dir().join(format!("il2cpp-native-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let work = d.to_str().unwrap();
        let mut nb = Native::new(work, Some(2), work);
        let cmd: Vec<String> = ["/bin/sh", "-c", "echo out; echo err >&2; printf 'a\\r\\nb'"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        nb.run(&cmd, "t.log", Some(1), false, None).unwrap();
        let t = std::fs::read_to_string(d.join("t.log")).unwrap();
        assert!(
            t.contains("out\n") && t.contains("err\n") && t.ends_with("a\nb"),
            "{t:?}"
        );
        assert!(nb.peak("t.log").is_some() && nb.cpu_of("t.log").is_some());
        let bad: Vec<String> = ["/bin/sh", "-c", "echo boom; exit 3"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let e = nb.run(&bad, "b.log", None, false, None).unwrap_err();
        assert!(
            e.to_string()
                .starts_with("/bin/sh failed (exit 3); full log: "),
            "{e}"
        );
        // the scheduler hands out a turn to a waiting request
        let r = nb
            .shell(|sh| bee::Shell::sh(sh, "echo $PMUNITY_SCHED", work, &[]))
            .unwrap();
        assert_eq!(r.0, 0);
        assert!(r.1.contains("pmunity-cc-"), "{:?}", r.1);
        assert!(nb.sched_stats.is_some() || !nb.sched);
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn digests_follow_inputs() {
        let d = std::env::temp_dir().join(format!("il2cpp-digest-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("s")).unwrap();
        std::fs::write(d.join("a.dll"), b"1").unwrap();
        std::fs::write(d.join("s/b.dll"), b"2").unwrap();
        let p = d.to_str().unwrap();
        let a = digest(&["x".into()], &[p]).unwrap();
        // json.dumps(['x']) + 'a.dll' + b'1' + 's/b.dll' + b'2'
        assert_eq!(a, crate::hash::sha256_hex(b"[\"x\"]a.dll1s/b.dll2"));
        std::fs::write(d.join("s/b.dll"), b"3").unwrap();
        assert_ne!(a, digest(&["x".into()], &[p]).unwrap());
        let _ = std::fs::remove_dir_all(&d);
    }
}
