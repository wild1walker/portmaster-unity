//! On the handheld (the aarch64 musl build), until the process exits (were tools/device/profcpu.py and
//! memprof.py):
//!
//! `pmu-dev profcpu <pid> [interval s] [top]`: per-thread CPU use of a running process, every <interval>
//! s (100% = one core), and a "[therm]" line with temperatures, CPU/GPU clocks and thermal throttle steps.
//! `pmu-dev memprof <pid> [interval s] [top]`: where its memory is, from /proc/<pid>/smaps: resident and
//! swapped MB by mapping (anonymous memory, the GPU driver's /dev/mali0 mappings, each file), plus the
//! system's MemAvailable and swap use.
use std::collections::HashMap;
use std::io::Write;
use std::time::{Duration, Instant};

fn rd(p: &str) -> Option<String> {
    std::fs::read_to_string(p).ok().map(|s| s.trim().to_string())
}

/// the local time of day, %T
fn clock() -> String {
    let t = unsafe { libc::time(std::ptr::null_mut()) };
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&t, &mut tm) };
    format!("{:02}:{:02}:{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec)
}

fn args3(args: &[String], what: &str) -> Result<(u32, f64, usize), String> {
    let pid = args.first().and_then(|p| p.parse().ok()).ok_or(format!("usage: pmu-dev {what} <pid> [interval] [top]"))?;
    let iv = args.get(1).and_then(|v| v.parse().ok()).unwrap_or(10.0);
    let top = args.get(2).and_then(|v| v.parse().ok()).unwrap_or(if what == "profcpu" { 6 } else { 8 });
    Ok((pid, iv, top))
}

fn sorted_dir(d: &str) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(d).map(|r| r.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect()).unwrap_or_default();
    v.sort();
    v
}

/// temperatures (C), CPU and GPU clocks (MHz), throttle steps: '[therm] cpu 61 gpu 63 | cpu 1512/1512 gpu 504 | cool 1/0'
fn thermal() -> String {
    let mut t = Vec::new();
    for z in sorted_dir("/sys/class/thermal").iter().filter(|z| z.starts_with("thermal_zone")) {
        if let (Some(typ), Some(v)) = (rd(&format!("/sys/class/thermal/{z}/type")), rd(&format!("/sys/class/thermal/{z}/temp"))) {
            let kind = typ.split('_').next().unwrap_or("").to_string();
            if typ.ends_with("_thermal_zone") && ["cpu", "gpu", "ddr"].contains(&kind.as_str()) {
                t.push(format!("{kind} {}", v.parse::<i64>().unwrap_or(0).div_euclid(1000)));
            }
        }
    }
    let cpu: Vec<String> = (0..4)
        .filter_map(|c| rd(&format!("/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_cur_freq")))
        .filter(|x| !x.is_empty())
        .map(|x| (x.parse::<i64>().unwrap_or(0) / 1000).to_string())
        .collect();
    let g = rd("/sys/class/devfreq/gpu/cur_freq").filter(|x| !x.is_empty()).map_or("?".to_string(), |g| (g.parse::<i64>().unwrap_or(0) / 1_000_000).to_string());
    let cool: Vec<String> = sorted_dir("/sys/class/thermal")
        .iter()
        .filter(|c| c.starts_with("cooling_device"))
        .map(|c| rd(&format!("/sys/class/thermal/{c}/cur_state")).filter(|x| !x.is_empty()).unwrap_or_else(|| "?".into()))
        .collect();
    format!("[therm] {} | cpu {} gpu {g} | cool {}", t.join(" "), cpu.join("/"), cool.join("/"))
}

/// {tid: (name, utime + stime)}, or None when the process is gone (or a zombie)
fn sample(pid: u32) -> Option<HashMap<String, (String, i64)>> {
    let st = rd(&format!("/proc/{pid}/stat"))?;
    if st.get(st.rfind(')')? + 2..)?.starts_with('Z') {
        return None;
    }
    let mut r = HashMap::new();
    for t in std::fs::read_dir(format!("/proc/{pid}/task")).ok()?.filter_map(|e| e.ok()) {
        let t = t.file_name().to_string_lossy().into_owned();
        let Ok(st) = std::fs::read_to_string(format!("/proc/{pid}/task/{t}/stat")) else { continue };
        let (Some(a), Some(b)) = (st.find('('), st.rfind(')')) else { continue };
        let f: Vec<&str> = st[b + 2..].split_whitespace().collect();
        if f.len() > 12 {
            r.insert(t, (st[a + 1..b].to_string(), f[11].parse::<i64>().unwrap_or(0) + f[12].parse::<i64>().unwrap_or(0)));
        }
    }
    Some(r)
}

pub fn profcpu(args: &[String]) -> Result<u8, String> {
    let (pid, iv, top) = args3(args, "profcpu")?;
    let hz = unsafe { libc::sysconf(libc::_SC_CLK_TCK) } as f64;
    let (mut prev, mut t0) = (sample(pid), Instant::now());
    while let Some(p) = prev {
        std::thread::sleep(Duration::from_secs_f64(iv));
        let (Some(cur), t1) = (sample(pid), Instant::now()) else { break };
        let secs = (t1 - t0).as_secs_f64();
        let mut names: Vec<String> = Vec::new();
        let mut use_: HashMap<String, i64> = HashMap::new();
        for (t, (name, ticks)) in &cur {
            let d = ticks - p.get(t).map_or(*ticks, |x| x.1);
            if !use_.contains_key(name) {
                names.push(name.clone());
            }
            *use_.entry(name.clone()).or_default() += d;
        }
        let total: i64 = use_.values().sum();
        let mut rows: Vec<(String, i64)> = names.into_iter().map(|n| (n.clone(), use_[&n])).collect();
        rows.sort_by_key(|r| -r.1);
        let parts: Vec<String> = rows.iter().take(top).map(|(n, d)| format!("{n} {:.0}%", 100.0 * *d as f64 / hz / secs)).collect();
        println!("[cpu] {} total {:.0}%  {}", clock(), 100.0 * total as f64 / hz / secs, parts.join("  "));
        println!("{}", thermal());
        let _ = std::io::stdout().flush();
        prev = Some(cur);
        t0 = t1;
    }
    Ok(0)
}

fn smaps(pid: u32) -> Option<(Vec<(String, i64)>, HashMap<String, i64>)> {
    let text = std::fs::read_to_string(format!("/proc/{pid}/smaps")).ok()?;
    let hdr = regex::Regex::new(r"^[0-9a-f]+-[0-9a-f]+ \S+ \S+ \S+ \d+\s*(.*)$").unwrap();
    let (mut rss, mut swap): (Vec<(String, i64)>, HashMap<String, i64>) = (Vec::new(), HashMap::new());
    let mut cur = String::new();
    for line in text.lines() {
        if let Some(m) = hdr.captures(line) {
            let mut n = m[1].trim().to_string();
            if n.is_empty() || n.starts_with("[anon:") || n.starts_with("[stack") {
                n = "[anon]".into();
            }
            cur = if n.starts_with('/') && !n.starts_with("/dev/") { n.rsplit('/').next().unwrap_or("").to_string() } else { n };
            continue;
        }
        let kb = || line.split_whitespace().nth(1).and_then(|x| x.parse::<i64>().ok()).unwrap_or(0);
        if line.starts_with("Rss:") {
            match rss.iter_mut().find(|r| r.0 == cur) {
                Some(r) => r.1 += kb(),
                None => rss.push((cur.clone(), kb())),
            }
        } else if line.starts_with("Swap:") {
            *swap.entry(cur.clone()).or_default() += kb();
        }
    }
    Some((rss, swap))
}

pub fn memprof(args: &[String]) -> Result<u8, String> {
    let (pid, iv, top) = args3(args, "memprof")?;
    while let Some((rss, swap)) = smaps(pid) {
        let mi: HashMap<String, i64> = std::fs::read_to_string("/proc/meminfo")
            .unwrap_or_default()
            .lines()
            .filter_map(|l| {
                let (k, v) = l.split_once(':')?;
                Some((k.to_string(), v.split_whitespace().next()?.parse().ok()?))
            })
            .collect();
        let g = |k: &str| mi.get(k).copied().unwrap_or(0);
        let tot = rss.iter().map(|r| r.1).sum::<i64>() >> 10;
        let tsw = swap.values().sum::<i64>() >> 10;
        let mut rows = rss.clone();
        rows.sort_by_key(|r| -r.1);
        let parts: Vec<String> = rows
            .iter()
            .take(top)
            .map(|(n, v)| {
                let sw = swap.get(n).copied().unwrap_or(0);
                format!("{n} {}{}", v >> 10, if sw >= 1024 { format!("+{}sw", sw >> 10) } else { String::new() })
            })
            .collect();
        println!(
            "[mem] {} rss {tot} MB swap {tsw} MB | avail {} MB, swap used {} MB | {}",
            clock(),
            g("MemAvailable") >> 10,
            (g("SwapTotal") - g("SwapFree")) >> 10,
            parts.join("  ")
        );
        let _ = std::io::stdout().flush();
        std::thread::sleep(Duration::from_secs_f64(iv));
    }
    Ok(0)
}
