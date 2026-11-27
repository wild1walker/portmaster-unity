//! `pmu-dev memlog <mem.log>` (was tools/port/memlog.py): what a rehearsed setup's memory sampler saw
//! (tools/port/rehearse.sh, REHEARSE_MEMLOG=1): the container's peak (all of its memory, and the
//! anonymous part, which a handheld without swap cannot give back), each program's peak RSS and
//! anonymous memory with the step it was in, and each step's peak. The log's lines, tab-separated,
//! sizes in MB:
//!
//!     <time> <the container's> <its anonymous> <its page cache> <name/pid RSS/anonymous/peak RSS ...> <step> <last line>
use std::fmt::Write as _;

pub fn run(args: &[String]) -> Result<u8, String> {
    let [path] = args else { return Err("usage: pmu-dev memlog <mem.log>".into()) };
    let text = String::from_utf8_lossy(&std::fs::read(path).map_err(|e| format!("{path}: {e}"))?).into_owned();
    print!("{}", summary(path, &text));
    Ok(0)
}

pub fn summary(path: &str, text: &str) -> String {
    let mut o = String::new();
    let (mut peak_cg, mut peak_anon) = ((0i64, String::new(), String::new()), (0i64, String::new(), String::new()));
    // name -> [peak anonymous, peak RSS, peak of the peak RSS, step, last line]
    let mut progs: Vec<(String, (i64, i64, i64, String, String))> = vec![];
    // step -> [the container's anonymous memory, the program with the most, its anonymous]
    let mut steps: Vec<(String, (i64, String, i64))> = vec![];
    let mut n = 0;
    for line in text.split('\n') {
        let col: Vec<&str> = line.split('\t').collect();
        if col.len() < 7 {
            continue;
        }
        let (Ok(cg), Ok(anon)) = (col[1].trim().parse::<i64>(), col[2].trim().parse::<i64>()) else { continue };
        let procs: Vec<&str> = col[4].split_whitespace().collect();
        let step = if col[5].trim().is_empty() { "(start)".to_string() } else { col[5].trim().to_string() };
        let last = col[6].trim().to_string();
        n += 1;
        if cg > peak_cg.0 {
            peak_cg = (cg, step.clone(), last.clone());
        }
        if anon > peak_anon.0 {
            peak_anon = (anon, step.clone(), last.clone());
        }
        let si = match steps.iter().position(|s| s.0 == step) {
            Some(i) => i,
            None => {
                steps.push((step.clone(), (0, String::new(), 0)));
                steps.len() - 1
            }
        };
        steps[si].1 .0 = steps[si].1 .0.max(anon);
        let mut i = 0;
        while i + 1 < procs.len() {
            let name = procs[i].rsplit_once('/').map_or(procs[i], |x| x.0).to_string();
            let v: Vec<Result<i64, _>> = procs[i + 1].split('/').map(|x| x.parse::<i64>()).collect();
            i += 2;
            let [Ok(rss), Ok(a), Ok(hwm)] = v[..] else { continue };
            let pi = match progs.iter().position(|p| p.0 == name) {
                Some(i) => i,
                None => {
                    progs.push((name.clone(), (0, 0, 0, String::new(), String::new())));
                    progs.len() - 1
                }
            };
            let p = &mut progs[pi].1;
            if a > p.0 {
                p.0 = a;
                p.3 = step.clone();
                p.4 = last.clone();
            }
            p.1 = p.1.max(rss);
            p.2 = p.2.max(hwm);
            let s = &mut steps[si].1;
            if a > s.2 {
                s.1 = name;
                s.2 = a;
            }
        }
    }
    if n == 0 {
        writeln!(o, "memlog: no samples in {path}").unwrap();
        return o;
    }
    writeln!(o, "memory ({n} samples, {path}):").unwrap();
    writeln!(o, "  the container at most {} MB (page cache included), in \"{}\"", peak_cg.0, peak_cg.1).unwrap();
    writeln!(o, "  its anonymous memory at most {} MB, in \"{}\": {}", peak_anon.0, peak_anon.1, peak_anon.2).unwrap();
    let mut sorted = progs.clone();
    sorted.sort_by_key(|p| -p.1 .0);
    for (name, (a, rss, hwm, step, last)) in sorted.iter().take(8) {
        let most = if *a != 0 { format!("; most in \"{step}\": {last}") } else { String::new() };
        writeln!(o, "  {name}: anonymous {a} MB, RSS {rss} MB (peak RSS {hwm} MB){most}").unwrap();
    }
    o.push_str("  each step: the container's anonymous memory at most (the program with the most)\n");
    for (step, (a, name, pa)) in &steps {
        let w = if name.is_empty() { String::new() } else { format!(" ({name} {pa} MB)") };
        writeln!(o, "    {a:5} MB  {step}{w}").unwrap();
    }
    o
}

#[cfg(test)]
mod tests {
    #[test]
    fn peaks() {
        let log = "1\t300\t200\t100\tpmu/12 250/180/260 sh/3 2/1/2\tconvert\tshaders: 3 of 9\n2\t500\t420\t80\tpmu/12 450/400/460\ttextures\tatlas\nbad\n";
        let o = super::summary("mem.log", log);
        assert!(o.contains("memory (2 samples, mem.log):"), "{o}");
        assert!(o.contains("the container at most 500 MB (page cache included), in \"textures\""), "{o}");
        assert!(o.contains("  pmu: anonymous 400 MB, RSS 450 MB (peak RSS 460 MB); most in \"textures\": atlas"), "{o}");
        assert!(o.contains("      200 MB  convert (pmu 180 MB)\n      420 MB  textures (pmu 400 MB)"), "{o}");
    }
}
