//! `pmu-dev uatest-sum <out dir>` (was tools/device/test.sh's inline Python): what a handheld run
//! fetched (run.log, mem.log, shot-<n>.ppm) summed up: the frame rate after the first [fps] window, the
//! memory, how the run ended, and the screenshots (written again as PNG, the PPMs removed).
use std::path::Path;

use regex::Regex;

use crate::media::{shots, write_png};
use crate::shaders::read_image;

pub fn summary(out: &Path) -> String {
    let rd = |n: &str| std::fs::read(out.join(n)).map(|b| String::from_utf8_lossy(&b).into_owned());
    let log = rd("run.log").unwrap_or_default();
    let mut o = String::new();
    let fps_rx = Regex::new(r"\[fps\] ([\d.]+) fps, frame [\d.]+ avg / ([\d.]+) p99 .* \((\d+) frames, \d+ total, (\d+) s\)(?: rss (\d+) MB)?").unwrap();
    let fps: Vec<(f64, f64, f64)> = fps_rx
        .captures_iter(&log)
        .map(|c| (c[1].parse().unwrap_or(0.0), c[2].parse().unwrap_or(0.0), c[3].parse().unwrap_or(0.0)))
        .collect();
    if !fps.is_empty() {
        // the first window holds the load: leave it out of the average when there are others
        let w = if fps.len() > 1 { &fps[1..] } else { &fps[..] };
        let frames: f64 = w.iter().map(|f| f.2).sum();
        let secs: f64 = w.iter().filter(|f| f.0 > 0.0).map(|f| f.2 / f.0).sum();
        let lo = w.iter().map(|f| f.0).fold(f64::INFINITY, f64::min);
        let p99 = w.iter().map(|f| f.1).fold(f64::NEG_INFINITY, f64::max);
        let avg = if secs != 0.0 { frames / secs } else { f64::NAN };
        o += &format!("frames:  {avg:.1} fps average after the first window, lowest window {lo:.1} fps, worst p99 {p99:.1} ms ({} windows)\n", fps.len());
    } else {
        o += "frames:  no [fps] lines: the game did not reach its first frames (see run.log)\n";
    }
    let mem = rd("mem.log").unwrap_or_default();
    let nums = |re: &str| -> Vec<i64> { Regex::new(re).unwrap().captures_iter(&mem).filter_map(|c| c[1].parse().ok()).collect() };
    let (avail, rss) = (nums(r"avail=(\d+)M"), nums(r"rss=(\d+)M"));
    if !avail.is_empty() {
        o += &format!(
            "memory:  game up to {} MB resident; the system had {} MB available at the lowest\n",
            rss.iter().max().copied().unwrap_or(0),
            avail.iter().min().unwrap()
        );
    }
    let code = Regex::new(r"(?m)^exit (-?\d+)$").unwrap().captures_iter(&log).last().map_or("?".to_string(), |c| c[1].to_string());
    let crash: Vec<String> = Regex::new(r"(?m)^.*(?:\[crash\]|FORTIFY|abort message|Fatal signal|SIGSEGV).*$").unwrap().find_iter(&log).map(|m| m.as_str().to_string()).collect();
    let ended = if log.contains("LOW MEMORY") {
        "stopped by the memory watchdog: the game needs less memory (e.g. smaller textures, textures.scale)".to_string()
    } else if let Some(c) = crash.first() {
        format!("crashed (exit {code}): {}", c.chars().take(200).collect::<String>())
    } else if log.contains("[uatest] ran the full") {
        "ran the whole time (stopped at the end)".into()
    } else {
        let after = Regex::new(r"exited on its own after (\d+) s").unwrap().captures(&log).map(|c| format!(" after {} s", &c[1])).unwrap_or_default();
        format!("the game exited on its own{after}, exit {code} (see run.log)")
    };
    o += &format!("ended:   {ended}\n");
    let mut names = Vec::new();
    for f in shots(out, ".ppm") {
        let png = f.with_extension("png");
        match read_image(&f).and_then(|(w, h, px)| write_png(&png, w, h, &px)) {
            Ok(()) => {
                let _ = std::fs::remove_file(&f);
                names.push(png.file_name().unwrap().to_string_lossy().into_owned());
            }
            Err(_) => names.push(f.file_name().unwrap().to_string_lossy().into_owned()),
        }
    }
    if !names.is_empty() {
        o += &format!("shots:   {}\n", names.join(" "));
    }
    o
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let [out] = args else { return Err("usage: pmu-dev uatest-sum <out dir>".into()) };
    print!("{}", summary(Path::new(out)));
    Ok(0)
}

#[cfg(test)]
mod tests {
    #[test]
    fn summary() {
        let d = std::env::temp_dir().join(format!("pmu-dev-uatest-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        std::fs::write(
            d.join("run.log"),
            "[fps] 20.0 fps, frame 50.0 avg / 90.0 p99 / 99 worst ms (20 frames, 20 total, 1 s) rss 500 MB\n\
             [fps] 30.0 fps, frame 33.3 avg / 40.0 p99 / 50 worst ms (60 frames, 80 total, 2 s)\n[uatest] ran the full 60 s\nexit 0\n",
        )
        .unwrap();
        std::fs::write(d.join("mem.log"), "t=1 avail=300M rss=400M\nt=2 avail=250M rss=420M\n").unwrap();
        std::fs::write(d.join("shot-10.ppm"), b"P6\n1 1\n255\n\x01\x02\x03").unwrap();
        std::fs::write(d.join("shot-9.ppm"), b"P6\n1 1\n255\n\x01\x02\x03").unwrap();
        let s = super::summary(&d);
        assert!(s.contains("frames:  30.0 fps average after the first window, lowest window 30.0 fps, worst p99 40.0 ms (2 windows)"), "{s}");
        assert!(s.contains("memory:  game up to 420 MB resident; the system had 250 MB available at the lowest"), "{s}");
        assert!(s.contains("ended:   ran the whole time (stopped at the end)") && s.contains("shots:   shot-9.png shot-10.png"), "{s}");
        assert!(d.join("shot-9.png").is_file() && !d.join("shot-9.ppm").exists());
        let _ = std::fs::remove_dir_all(&d);
    }
}
