//! `pmu-dev benchsum [--syms DIR] [--top N] [--brief] <run folder>...` (was tools/analysis/benchsum.py):
//! tools/device/profbench.sh runs summed up.
//!
//! For each run folder (run.log, snap-0.txt / snap-1.txt, freq.log, prof-*.txt; tools/device/profrun.sh
//! writes them): the frame rate over the measurement, CPU and GPU clocks (residency from cpufreq's
//! time_in_state and devfreq's trans_stat, or the per-second samples), each core's use, the game's
//! threads by CPU time and their wait for a CPU (schedstat), memory and swap traffic, temperatures and
//! throttling, other processes that took CPU time, and the profile's summary (profsum). With more than
//! one run, a table compares them with the first (the baseline: run it first and last to see the drift).
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fmt::Write as _;
use std::path::Path;

use regex::Regex;

use crate::profsum;
use crate::syms::ElfNames;

fn read(p: &Path) -> Option<String> {
    std::fs::read(p).ok().map(|b| String::from_utf8_lossy(&b).into_owned())
}

/// {section: [lines]} of a profrun.sh snapshot, in the file's order
#[derive(Default)]
struct Snap {
    order: Vec<String>,
    secs: HashMap<String, Vec<String>>,
}

impl Snap {
    fn read(p: &Path) -> Snap {
        let mut s = Snap::default();
        let Some(text) = read(p) else { return s };
        let mut cur: Option<String> = None;
        for line in text.lines() {
            if let Some(k) = line.strip_prefix("=== ") {
                if !s.secs.contains_key(k) {
                    s.order.push(k.to_string());
                }
                s.secs.insert(k.to_string(), vec![]);
                cur = Some(k.to_string());
            } else if let Some(c) = &cur {
                s.secs.get_mut(c).unwrap().push(line.to_string());
            }
        }
        s
    }
    fn get(&self, k: &str) -> Option<&Vec<String>> {
        self.secs.get(k)
    }
    fn lines(&self, k: &str) -> &[String] {
        self.secs.get(k).map_or(&[], |v| v.as_slice())
    }
    fn first(&self, k: &str) -> Option<&String> {
        self.secs.get(k).and_then(|v| v.first())
    }
    fn is_empty(&self) -> bool {
        self.order.is_empty()
    }
    fn has(&self, k: &str) -> bool {
        self.secs.contains_key(k)
    }
}

fn kv(lines: &[String]) -> HashMap<String, i64> {
    let mut d = HashMap::new();
    for l in lines {
        let r = l.replace(':', " ");
        let p: Vec<&str> = r.split_whitespace().collect();
        if p.len() >= 2 {
            if let Ok(v) = p[1].parse() {
                d.insert(p[0].to_string(), v);
            }
        }
    }
    d
}

fn procstat(lines: &[String]) -> BTreeMap<String, Vec<i64>> {
    let mut d = BTreeMap::new();
    for l in lines {
        let p: Vec<&str> = l.split_whitespace().collect();
        if p.first().is_some_and(|x| x.starts_with("cpu")) {
            d.insert(p[0].to_string(), p[1..p.len().min(8)].iter().map(|x| x.parse().unwrap_or(0)).collect());
        }
    }
    d
}

fn isdigit(s: &str) -> bool {
    !s.is_empty() && s.chars().all(|c| c.is_ascii_digit())
}

/// [(tid, (name, run_ns, wait_ns, slices, utime, stime))] in the snapshot's order
fn tasks(s: &Snap) -> Vec<(i64, (String, i64, i64, i64, i64, i64))> {
    let mut t: Vec<(i64, (String, i64, i64, i64, i64, i64))> = Vec::new();
    for k in &s.order {
        if !k.starts_with("task ") {
            continue;
        }
        let p: Vec<&str> = k.splitn(3, ' ').collect();
        if p.len() < 2 || !isdigit(p[1]) {
            continue;
        }
        let tid: i64 = p[1].parse().unwrap();
        let name = p.get(2).unwrap_or(&"?").replace(' ', "_");
        let (mut run, mut wait, mut sl, mut ut, mut st) = (0, 0, 0, 0, 0);
        for l in s.lines(k) {
            let f: Vec<&str> = l.split_whitespace().collect();
            if f.len() == 3 && f.iter().all(|x| isdigit(x)) {
                run = f[0].parse().unwrap();
                wait = f[1].parse().unwrap();
                sl = f[2].parse().unwrap();
            } else if let Some(i) = l.rfind(')') {
                let r: Vec<&str> = l.get(i + 2..).unwrap_or("").split_whitespace().collect();
                if r.len() > 12 {
                    ut = r[11].parse().unwrap_or(0);
                    st = r[12].parse().unwrap_or(0);
                }
            }
        }
        let v = (name, run, wait, sl, ut, st);
        match t.iter_mut().find(|x| x.0 == tid) {
            Some(x) => x.1 = v,
            None => t.push((tid, v)),
        }
    }
    t
}

/// {pid: (name, utime + stime)}
fn procs(s: &Snap) -> HashMap<i64, (String, i64)> {
    let mut d = HashMap::new();
    for l in s.lines("procs") {
        let (Some(a), Some(b)) = (l.find('('), l.rfind(')')) else { continue };
        let Some(pid) = l.split_whitespace().next().and_then(|x| x.parse().ok()) else { continue };
        let name = l.get(a + 1..b).unwrap_or("").to_string();
        let r: Vec<&str> = l.get(b + 2..).unwrap_or("").split_whitespace().collect();
        if r.len() > 12 {
            d.insert(pid, (name, r[11].parse::<i64>().unwrap_or(0) + r[12].parse::<i64>().unwrap_or(0)));
        }
    }
    d
}

/// {freq: time} from cpufreq time_in_state ('freq ticks') or devfreq trans_stat (last column, ms)
fn residency(lines: &[String]) -> Vec<(i64, i64)> {
    let mut d: Vec<(i64, i64)> = Vec::new();
    let mut set = |d: &mut Vec<(i64, i64)>, f: i64, t: i64| match d.iter_mut().find(|x| x.0 == f) {
        Some(x) => x.1 = t,
        None => d.push((f, t)),
    };
    for l in lines {
        let r = l.replace('*', " ");
        let p: Vec<&str> = r.split_whitespace().collect();
        if p.len() == 2 && isdigit(p[0]) && isdigit(p[1]) {
            set(&mut d, p[0].parse().unwrap(), p[1].parse().unwrap());
        } else if l.contains(':') {
            let mut parts = l.split(':');
            let left = parts.next().unwrap().replace('*', "");
            let left = left.trim();
            if isdigit(left) {
                let nums: Vec<&str> = parts.next().unwrap_or("").split_whitespace().collect();
                if let Some(last) = nums.last().filter(|x| isdigit(x)) {
                    set(&mut d, left.parse().unwrap(), last.parse().unwrap());
                }
            }
        }
    }
    d
}

fn delta_res(a: &[(i64, i64)], b: &[(i64, i64)]) -> Vec<(i64, i64)> {
    b.iter()
        .filter_map(|(f, t)| {
            let x = t - a.iter().find(|y| y.0 == *f).map_or(0, |y| y.1);
            (x > 0).then_some((*f, x))
        })
        .collect()
}

fn fmt_res(d: &[(i64, i64)], unit: f64) -> String {
    let tot: i64 = d.iter().map(|x| x.1).sum();
    if tot == 0 {
        return "-".into();
    }
    let avg = d.iter().map(|(f, t)| (*f as f64) * (*t as f64)).fold(0.0f64, |a, b| a + b) / tot as f64 / unit;
    let mut top = d.to_vec();
    top.sort_by_key(|x| -x.1);
    top.truncate(3);
    top.sort_by_key(|x| -x.0);
    let parts: Vec<String> = top.iter().map(|(f, t)| format!("{:.0}: {:.0}%", *f as f64 / unit, 100.0 * *t as f64 / tot as f64)).collect();
    format!("{avg:.0} MHz avg ({})", parts.join(", "))
}

struct Run {
    dir: std::path::PathBuf,
    name: String,
    result: String,
    fps: Vec<[f64; 8]>,
    notes: Vec<String>,
    s0: Snap,
    s1: Snap,
    secs: f64,
    profiles: Vec<String>,
}

impl Run {
    fn new(d: &str) -> Run {
        let dir = Path::new(d).to_path_buf();
        let name = Path::new(d.trim_end_matches('/')).file_name().map_or(d.to_string(), |n| n.to_string_lossy().into_owned());
        let result = read(&dir.join("result")).map_or("no result file".to_string(), |r| r.trim().to_string());
        let fps_rx = Regex::new(
            r"^\[fps\] ([\d.]+) fps, frame ([\d.]+) avg / ([\d.]+) p99 / ([\d.]+) worst ms, late (\d+) hitch (\d+), gc (-?\d+).*\((\d+) frames",
        )
        .unwrap();
        let skip_env = Regex::new(r"=(dummy|sdl)$|UA_FPS=|ANDROID_WINDOW|UA_EGL_PBUFFER|UATEST").unwrap();
        let (mut inside, mut fps, mut notes) = (false, Vec::new(), Vec::new());
        for l in read(&dir.join("run.log")).unwrap_or_default().lines() {
            if l.starts_with("[bench] measure start") {
                inside = true;
            } else if l.starts_with("[bench] measure end") {
                inside = false;
            }
            if let Some(m) = fps_rx.captures(l).filter(|_| inside) {
                let mut v = [0.0; 8];
                for (k, x) in v.iter_mut().enumerate() {
                    *x = m[k + 1].parse().unwrap_or(0.0);
                }
                fps.push(v);
            }
            if l.starts_with("[bench] env ") && !skip_env.is_match(l) {
                notes.push(l["[bench] env ".len()..].to_string());
            }
            if ["JobSystem:", "[host] mono options", "[host] frame rate", "gfx-threading", "[prof] prof-"].iter().any(|s| l.contains(s)) {
                notes.push(l.trim().chars().take(200).collect());
            }
        }
        let (s0, s1) = (Snap::read(&dir.join("snap-0.txt")), Snap::read(&dir.join("snap-1.txt")));
        let up = |s: &Snap| s.first("/proc/uptime").and_then(|l| l.split_whitespace().next()?.parse::<f64>().ok());
        let secs = match (up(&s0), up(&s1)) {
            (Some(a), Some(b)) => b - a,
            _ => 0.0,
        };
        let mut profiles: Vec<String> = std::fs::read_dir(&dir)
            .map(|r| {
                r.filter_map(|e| e.ok())
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .filter(|n| n.starts_with("prof-") && n.ends_with(".txt"))
                    .map(|n| dir.join(n).to_string_lossy().into_owned())
                    .collect()
            })
            .unwrap_or_default();
        profiles.sort();
        Run { dir, name, result, fps, notes, s0, s1, secs, profiles }
    }

    fn fps_avg(&self) -> Option<f64> {
        if self.fps.is_empty() {
            return None;
        }
        let frames: f64 = self.fps.iter().map(|f| f[7]).sum();
        let secs: f64 = self.fps.iter().filter(|f| f[0] > 0.0).map(|f| f[7] / f[0]).sum();
        (secs != 0.0).then(|| frames / secs)
    }

    fn main_cpu(&self) -> (Option<f64>, Option<f64>) {
        let (t0, t1) = (tasks(&self.s0), tasks(&self.s1));
        for (tid, v) in &t1 {
            if let Some((_, a)) = t0.iter().find(|x| x.0 == *tid).filter(|_| v.0 == "UnityMain" && self.secs != 0.0) {
                return (Some(100.0 * (v.1 - a.1) as f64 / 1e9 / self.secs), Some(100.0 * (v.2 - a.2) as f64 / 1e9 / self.secs));
            }
        }
        (None, None)
    }

    fn clocks(&self) -> (String, String) {
        let mut cpu = "-".to_string();
        for k in &self.s1.order {
            if k.ends_with("time_in_state") && self.s0.has(k) {
                cpu = fmt_res(&delta_res(&residency(self.s0.lines(k)), &residency(self.s1.lines(k))), 1000.0);
                break;
            }
        }
        let out: Vec<String> = self
            .s1
            .order
            .iter()
            .filter(|k| k.starts_with("/sys/class/devfreq/") && k.ends_with("trans_stat") && self.s0.has(k))
            .map(|k| {
                let dev = k.split('/').nth(4).unwrap_or("");
                format!("{dev} {}", fmt_res(&delta_res(&residency(self.s0.lines(k)), &residency(self.s1.lines(k))), 1e6))
            })
            .collect();
        (cpu, if out.is_empty() { "-".into() } else { out.join("; ") })
    }

    fn freq_samples(&self) -> (Vec<i64>, Vec<i64>) {
        let rx = Regex::new(r"cpu=([\d,]+) gpu=([\d,]+)").unwrap();
        let (mut cpu, mut gpu) = (vec![], vec![]);
        for l in read(&self.dir.join("freq.log")).unwrap_or_default().lines() {
            if let Some(m) = rx.captures(l) {
                cpu.extend(m[1].split(',').filter_map(|x| x.parse::<i64>().ok()));
                gpu.extend(m[2].split(',').take(1).filter_map(|x| x.parse::<i64>().ok()));
            }
        }
        (cpu, gpu)
    }

    /// each thermal zone's per-second temperatures (C) from freq.log (profrun.sh since 2026-10-23)
    fn temp_samples(&self) -> Vec<Vec<i64>> {
        let rx = Regex::new(r"temp=([\d,]+)").unwrap();
        let mut zones: Vec<Vec<i64>> = vec![];
        for l in read(&self.dir.join("freq.log")).unwrap_or_default().lines() {
            if let Some(m) = rx.captures(l) {
                let v: Vec<i64> = m[1].split(',').filter_map(|x| x.parse().ok()).collect();
                while zones.len() < v.len() {
                    zones.push(vec![]);
                }
                for (i, x) in v.into_iter().enumerate() {
                    zones[i].push(x);
                }
            }
        }
        zones
    }

    fn report(&self, o: &mut String, syms: &[String], top: usize, brief: bool) {
        writeln!(o, "\n######## {}: {}", self.name, self.result).unwrap();
        for n in &self.notes {
            writeln!(o, "  {n}").unwrap();
        }
        match self.fps_avg().filter(|x| *x != 0.0) {
            Some(fa) => {
                let f = &self.fps;
                let mn = f.iter().map(|x| x[0]).fold(f64::INFINITY, f64::min);
                let p99 = f.iter().map(|x| x[2]).fold(f64::NEG_INFINITY, f64::max);
                let worst = f.iter().map(|x| x[3]).fold(f64::NEG_INFINITY, f64::max);
                writeln!(
                    o,
                    "  frames: {fa:.1} fps over {} s (lowest second {mn:.1}), p99 up to {p99:.1} ms, worst {worst:.1} ms, {:.0} frames over 25 ms, {:.0} over 50 ms, {:.0} GCs",
                    f.len(),
                    f.iter().map(|x| x[4]).fold(0.0f64, |a, b| a + b),
                    f.iter().map(|x| x[5]).fold(0.0f64, |a, b| a + b),
                    f.iter().map(|x| x[6].max(0.0)).fold(0.0f64, |a, b| a + b)
                )
                .unwrap();
            }
            None => o.push_str("  frames: no [fps] lines in the measurement\n"),
        }
        if self.s1.is_empty() {
            return;
        }
        let (cpu, gpu) = self.clocks();
        let (fc, fg) = self.freq_samples();
        let mut l = format!("  CPU clock: {cpu}");
        if !fc.is_empty() {
            write!(
                l,
                "; per-second samples {}-{} MHz, avg {:.0}",
                fc.iter().min().unwrap(),
                fc.iter().max().unwrap(),
                fc.iter().sum::<i64>() as f64 / fc.len() as f64
            )
            .unwrap();
        }
        writeln!(o, "{l}").unwrap();
        let mut l = format!("  GPU/devfreq: {gpu}");
        if !fg.is_empty() {
            write!(l, "; per-second samples {}-{} MHz", fg.iter().min().unwrap(), fg.iter().max().unwrap()).unwrap();
        }
        writeln!(o, "{l}").unwrap();
        let s1 = &self.s1;
        let gov: BTreeSet<&String> = s1.order.iter().filter(|k| k.ends_with("scaling_governor")).filter_map(|k| s1.first(k)).collect();
        let mx: BTreeSet<i64> = s1
            .order
            .iter()
            .filter(|k| k.ends_with("scaling_max_freq"))
            .filter_map(|k| s1.first(k))
            .filter_map(|v| v.trim().parse::<i64>().ok())
            .map(|v| v / 1000)
            .collect();
        let dgov: BTreeSet<String> = s1
            .order
            .iter()
            .filter(|k| k.starts_with("/sys/class/devfreq/") && k.ends_with("governor"))
            .filter_map(|k| s1.first(k).map(|v| format!("{}={v}", k.split('/').nth(4).unwrap_or(""))))
            .collect();
        if !gov.is_empty() || !dgov.is_empty() {
            let g: Vec<&str> = gov.iter().map(|s| s.as_str()).collect();
            let m: Vec<String> = mx.iter().map(|x| x.to_string()).collect();
            let d: Vec<&str> = dgov.iter().map(|s| s.as_str()).collect();
            writeln!(o, "  governors: cpu {} (max {} MHz); {}", g.join(","), m.join(","), d.join(" ")).unwrap();
        }
        let (p0, p1) = (procstat(self.s0.lines("/proc/stat")), procstat(s1.lines("/proc/stat")));
        let mut cores = vec![];
        for (c, b) in &p1 {
            let Some(a) = p0.get(c).filter(|_| c != "cpu") else { continue };
            let d: Vec<i64> = a.iter().zip(b).map(|(a, b)| b - a).collect();
            let tot: i64 = d.iter().sum();
            if tot != 0 && d.len() > 4 {
                cores.push(format!("{c} {:.0}%", 100.0 * (tot - d[3] - d[4]) as f64 / tot as f64));
            }
        }
        if !cores.is_empty() {
            writeln!(o, "  cores busy: {}", cores.join(", ")).unwrap();
        }
        let (t0, t1) = (tasks(&self.s0), tasks(s1));
        let mut rows: Vec<(f64, f64, String, i64, i64)> = vec![];
        for (tid, v) in &t1 {
            if let Some((_, a)) = t0.iter().find(|x| x.0 == *tid).filter(|_| self.secs != 0.0) {
                rows.push((
                    100.0 * (v.1 - a.1) as f64 / 1e9 / self.secs,
                    100.0 * (v.2 - a.2) as f64 / 1e9 / self.secs,
                    v.0.clone(),
                    (v.4 - a.4) * 10,
                    (v.5 - a.5) * 10,
                ));
            }
        }
        rows.sort_by(|a, b| b.partial_cmp(a).unwrap_or(std::cmp::Ordering::Equal));
        if !rows.is_empty() {
            let parts: Vec<String> = rows.iter().take(8).map(|(r, w, n, u, k)| format!("{n} {r:.0}/{w:.0}% ({u}, {k})")).collect();
            writeln!(o, "  threads (% of a core on a CPU / waiting for one; user, kernel ms): {}", parts.join(", ")).unwrap();
        }
        let (v0, v1) = (kv(self.s0.lines("/proc/vmstat")), kv(s1.lines("/proc/vmstat")));
        let m1 = kv(s1.lines("/proc/meminfo"));
        let rss = s1.order.iter().find(|k| k.ends_with("/status")).and_then(|k| kv(s1.lines(k)).get("VmRSS").copied());
        if !v1.is_empty() {
            let g = |d: &HashMap<String, i64>, k: &str| d.get(k).copied().unwrap_or(0);
            writeln!(
                o,
                "  memory: available {} MB at the end, swap used {} MB, game rss {} MB; during the measurement: swap-ins {}, swap-outs {}, major faults {}",
                g(&m1, "MemAvailable") >> 10,
                (g(&m1, "SwapTotal") - g(&m1, "SwapFree")) >> 10,
                rss.unwrap_or(0) >> 10,
                g(&v1, "pswpin") - g(&v0, "pswpin"),
                g(&v1, "pswpout") - g(&v0, "pswpout"),
                g(&v1, "pgmajfault") - g(&v0, "pgmajfault")
            )
            .unwrap();
        }
        let mut sorted: Vec<&String> = s1.order.iter().collect();
        sorted.sort();
        let mut temps = vec![];
        for k in &sorted {
            if k.ends_with("/type") && k.contains("thermal_zone") {
                let tk = format!("{}/temp", &k[..k.len() - 5]);
                if let (Some(a), Some(b), Some(t)) = (self.s0.first(&tk), s1.first(&tk), s1.first(k)) {
                    let (a, b) = (a.trim().parse::<i64>().unwrap_or(0), b.trim().parse::<i64>().unwrap_or(0));
                    temps.push(format!("{t} {}->{} C", a.div_euclid(1000), b.div_euclid(1000)));
                }
            }
        }
        let cool: Vec<String> = sorted
            .iter()
            .filter(|k| k.contains("cooling_device") && k.ends_with("/type"))
            .filter_map(|k| {
                let cs = s1.first(&format!("{}/cur_state", &k[..k.len() - 5]))?;
                Some(format!("{}={cs}", s1.first(k)?))
            })
            .collect();
        if !temps.is_empty() || !cool.is_empty() {
            let c = if cool.is_empty() { String::new() } else { format!("; cooling {}", cool.join(", ")) };
            writeln!(o, "  thermal: {}{c}", temps.join(", ")).unwrap();
        }
        let zs = self.temp_samples();
        if !zs.is_empty() && !fg.is_empty() {
            let mut zk: Vec<&String> =
                s1.order.iter().filter(|k| k.starts_with("/sys/class/thermal/thermal_zone") && k.ends_with("/type") && s1.first(k).is_some()).collect();
            let zone_no = |k: &String| -> i64 {
                let d: String = k.split('/').nth(4).unwrap_or("").chars().filter(|c| c.is_ascii_digit()).collect();
                d.parse().unwrap_or(0)
            };
            zk.sort_by_key(|k| zone_no(k));
            let names: Vec<&String> = zk.iter().filter_map(|k| s1.first(k)).collect();
            let per: Vec<String> = zs
                .iter()
                .enumerate()
                .filter(|(_, z)| !z.is_empty())
                .map(|(i, z)| {
                    let n = names.get(i).map_or(i.to_string(), |s| s.to_string());
                    format!("{n} {}-{} (avg {:.1})", z.iter().min().unwrap(), z.iter().max().unwrap(), z.iter().sum::<i64>() as f64 / z.len() as f64)
                })
                .collect();
            let mut cnt: BTreeMap<i64, i64> = BTreeMap::new();
            for c in &fg {
                *cnt.entry(*c).or_default() += 1;
            }
            let clk: Vec<String> = cnt.iter().rev().map(|(c, n)| format!("{c} MHz {n} s")).collect();
            writeln!(o, "  per second: {} C; GPU {}", per.join(", "), clk.join(", ")).unwrap();
        }
        let (q0, q1) = (procs(&self.s0), procs(s1));
        let gp: BTreeSet<i64> =
            s1.order.iter().filter(|k| k.starts_with("/proc/") && k.ends_with("/status")).filter_map(|k| k.split('/').nth(2)?.parse().ok()).collect();
        let mut other: Vec<(i64, String)> = q1
            .iter()
            .filter_map(|(p, (n, t))| {
                let (_, t0) = q0.get(p)?;
                (!gp.contains(p) && t > t0).then(|| (t - t0, n.clone()))
            })
            .collect();
        other.sort_by(|a, b| b.cmp(a));
        other.truncate(5);
        if !other.is_empty() && self.secs != 0.0 {
            let parts: Vec<String> = other.iter().map(|(t, n)| format!("{n} {:.0}%", *t as f64 / self.secs)).collect();
            writeln!(o, "  other processes: {}", parts.join(", ")).unwrap();
        }
        if let Some(text) = read(&self.dir.join("gpucnt.txt")) {
            // GPUCNT=1 runs: the GPU's counters over the measurement (runtime/host/gpucnt.c)
            let lines: Vec<&str> = text.lines().collect();
            if lines.concat().contains("# derived") {
                let i = lines.iter().position(|l| l.starts_with("# derived")).map_or(lines.len(), |i| i + 1);
                writeln!(o, "  GPU counters: {}", lines[i.min(lines.len())..].join("\n                ")).unwrap();
            } else {
                let t = lines[lines.len().saturating_sub(2)..].join(" ");
                writeln!(o, "  GPU counters: {}", t.chars().take(200).collect::<String>()).unwrap();
            }
        }
        if brief {
            return;
        }
        for p in &self.profiles {
            writeln!(o, "  --- {}", Path::new(p).file_name().unwrap().to_string_lossy()).unwrap();
            let mut args = vec![p.clone(), "--top".into(), top.to_string()];
            for s in syms {
                args.push("--syms".into());
                args.push(s.clone());
            }
            let text = match profsum::parse_args(&args) {
                Ok(a) => {
                    let prof = read(Path::new(p)).unwrap_or_default();
                    profsum::summary(&profsum::Profile::parse(&prof), &a, ElfNames::new(&a.syms, &a.lib, true))
                }
                Err(e) => e,
            };
            for l in text.lines() {
                writeln!(o, "  {l}").unwrap();
            }
        }
    }
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let usage = "usage: pmu-dev benchsum [--syms DIR] [--top N] [--brief] <run folder>...";
    let (mut syms, mut top, mut brief, mut runs) = (vec![], 12usize, false, vec![]);
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        let (k, inline) = match a.split_once('=') {
            Some((k, v)) if k.starts_with("--") => (k, Some(v.to_string())),
            _ => (a.as_str(), None),
        };
        let mut val = || -> Result<String, String> {
            if let Some(v) = &inline {
                return Ok(v.clone());
            }
            i += 1;
            args.get(i).cloned().ok_or_else(|| usage.to_string())
        };
        match k {
            "--syms" => syms.push(val()?),
            "--top" => top = val()?.parse().map_err(|_| usage.to_string())?,
            "--brief" => brief = true,
            x if x.starts_with('-') && x.len() > 1 => return Err(format!("{usage}\nunrecognized arguments: {x}")),
            _ => runs.push(a.clone()),
        }
        i += 1;
    }
    if runs.is_empty() {
        return Err(usage.into());
    }
    print!("{}", report(&runs, &syms, top, brief));
    Ok(0)
}

pub fn report(dirs: &[String], syms: &[String], top: usize, brief: bool) -> String {
    let runs: Vec<Run> = dirs.iter().map(|d| Run::new(d)).collect();
    let mut o = String::new();
    for r in &runs {
        r.report(&mut o, syms, top, brief);
    }
    if runs.len() > 1 {
        o.push_str("\n== compared with the first run\n");
        writeln!(o, "  {:<20} {:>6} {:>7} {:>8} {:>6}  result", "run", "fps", "vs 1st", "main CPU", "waits").unwrap();
        let base = runs[0].fps_avg();
        for r in &runs {
            let f = r.fps_avg();
            let (mc, mw) = r.main_cpu();
            let d = match (f.filter(|x| *x != 0.0), base.filter(|x| *x != 0.0)) {
                (Some(f), Some(b)) => format!("{:+.1}%", 100.0 * (f - b) / b),
                _ => "-".into(),
            };
            writeln!(o, "  {:<20} {:6.1} {d:>7} {:7.0}% {:5.0}%  {}", r.name, f.unwrap_or(0.0), mc.unwrap_or(0.0), mw.unwrap_or(0.0), r.result).unwrap();
        }
    }
    o
}

#[cfg(test)]
mod tests {
    use super::*;

    #[allow(clippy::too_many_arguments)]
    fn snap(uptime: f64, cpu0: [i64; 7], main_run: i64, main_wait: i64, swpin: i64, tis: (i64, i64), gpu_ms: (i64, i64), temp: i64, es: i64) -> String {
        format!(
            "=== /proc/uptime
{uptime:.2} 100.00
=== /proc/stat
cpu  {} 0 0 0 0 0 0
cpu0 {}
=== /proc/meminfo
MemTotal:         973000 kB
MemAvailable:      61440 kB
SwapTotal:        498688 kB
SwapFree:         400000 kB
=== /proc/vmstat
pswpin {swpin}
pswpout 10
pgmajfault {}
=== /proc/4242/status
Name:\tua_host
VmRSS:\t  800000 kB
=== /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
schedutil
=== /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
1512000
=== /sys/devices/system/cpu/cpu0/cpufreq/stats/time_in_state
1512000 {}
1008000 {}
=== /sys/class/devfreq/gpu/governor
simple_ondemand
=== /sys/class/devfreq/gpu/trans_stat
     From  :   To
           : 252000000 600000000   time(ms)
  252000000:         0         5   {}
* 600000000:         4         0   {}
Total transition : 9
=== /sys/class/thermal/thermal_zone0/type
cpu_thermal_zone
=== /sys/class/thermal/thermal_zone0/temp
{temp}
=== task 4243 UnityMain
{main_run} {main_wait} 1000
4243 (UnityMain) R 1 2 3 4 5 6 7 8 9 10 {} 5 0 0
=== task 4244 UnityGfxDeviceW
1000000000 0 500
4244 (UnityGfxDeviceW) S 1 2 3 4 5 6 7 8 9 10 50 50 0 0
=== procs
4242 (ua_host) S 1 2 3 4 5 6 7 8 9 10 1000 10 0 0
77 (emulationstation) S 1 2 3 4 5 6 7 8 9 10 {es} 0 0 0
",
            cpu0.iter().sum::<i64>(),
            cpu0.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(" "),
            swpin * 2,
            tis.0,
            tis.1,
            gpu_ms.0,
            gpu_ms.1,
            main_run / 10_000_000
        )
    }

    fn make(base: &Path, name: &str, fps: f64) -> String {
        let d = base.join(name);
        std::fs::create_dir_all(&d).unwrap();
        std::fs::write(d.join("result"), "ok\n").unwrap();
        let mut lines = vec![
            "[bench] name x".to_string(),
            "[bench] env UA_MONO_OPTIONS=-O=abcrem".into(),
            "[fps] 59.0 fps, frame 16.9 avg / 18.0 p99 / 20.0 worst ms, late 0 hitch 0, gc 0 heap 64 MB (59 frames, 100 total, 10 s) rss 700 MB".into(),
            "[bench] measure start (2 s)".into(),
        ];
        for _ in 0..2 {
            lines.push(format!(
                "[fps] {fps:.1} fps, frame {:.1} avg / 35.0 p99 / 36.0 worst ms, late 30 hitch 0, gc 0 heap 64 MB ({} frames, 200 total, 12 s) rss 700 MB",
                1000.0 / fps,
                fps as i64
            ));
        }
        lines.push("[bench] measure end".into());
        std::fs::write(d.join("run.log"), lines.join("\n") + "\n").unwrap();
        std::fs::write(d.join("snap-0.txt"), snap(100.0, [100, 0, 50, 850, 0, 0, 0], 10_000_000_000, 1_000_000_000, 5, (1000, 500), (100, 200), 60000, 100)).unwrap();
        std::fs::write(d.join("snap-1.txt"), snap(102.0, [250, 0, 100, 850, 0, 0, 0], 11_900_000_000, 1_100_000_000, 25, (1150, 550), (300, 2000), 63000, 104)).unwrap();
        std::fs::write(
            d.join("freq.log"),
            "t=1 cpu=1512,1512,1512,1512 gpu=600 avail=60M swapfree=390M temp=61,60 cool=1,0\nt=2 cpu=1008,1512,1512,1512 gpu=252 avail=60M swapfree=390M temp=63,60 cool=1,1\n",
        )
        .unwrap();
        d.to_string_lossy().into_owned()
    }

    #[test]
    fn report_and_compare() {
        let t = std::env::temp_dir().join(format!("pmu-dev-bench-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let (a, b) = (make(&t, "base", 32.0), make(&t, "mono-abcrem", 34.0));
        let o = report(&[a, b], &[], 12, true);
        for x in [
            "######## base: ok", "frames: 32.0 fps over 2 s", "UA_MONO_OPTIONS=-O=abcrem", "CPU clock: 1386 MHz avg (1512: 75%, 1008: 25%)",
            "gpu 565 MHz avg (600: 90%, 252: 10%)", "governors: cpu schedutil (max 1512 MHz); gpu=simple_ondemand", "cores busy: cpu0 100%",
            "UnityMain 95/5%", "swap-ins 20", "cpu_thermal_zone 60->63 C", "per second: cpu_thermal_zone 61-63 (avg 62.0)", "GPU 600 MHz 1 s, 252 MHz 1 s",
            "emulationstation 2%", "mono-abcrem            34.0   +6.2%",
        ] {
            assert!(o.contains(x), "{x}\n{o}");
        }
        let _ = std::fs::remove_dir_all(&t);
    }
}
