//! The regression run's checks (was tools/regress/checks.py): what the tools' logs and outputs say, and
//! the comparisons with the list's expectations and the recorded baseline. Pure functions over text,
//! JSON and pictures (no game data is kept: counts, sizes and hashes only).
use std::collections::{BTreeMap, BTreeSet, HashMap};

use regex::Regex;
use serde_json::{json, Map, Value};

/// (check, ok, detail)
pub type Check = (String, bool, String);

fn rx(s: &str) -> Regex {
    Regex::new(s).unwrap()
}

fn n(s: &str) -> i64 {
    s.parse().unwrap_or(0)
}

/// The shaders step's summary (its log lines) as counts. Per-shader lines (which name the game's
/// shaders) are not looked at.
pub fn shader_counts(lines: &[String]) -> Value {
    let shaders = rx(r"shaders: (\d+) shaders given GLES3 programs \((\d+) programs, (\d+) subprograms\) in (\d+) files; (\d+) not translatable; precision (\S+); rewrites: (\d+) stages");
    let es31 = rx(r"shaders: (\d+) programs need GLSL ES 3\.10");
    let gl4x = rx(r"shaders: GLCore41/43 programs translated too: (\d+) programs; (\d+) SubShaders left out \(no ES equivalent\); (\d+) shaders without programs");
    let vr = rx(r"shaders: (\d+) VR-only OpenGL Core variants not translatable");
    let compute = rx(r"shaders: (\d+) of (\d+) compute shaders given GLES3 kernels \((\d+) kernels, (\d+) kernel variants.*?\); (\d+) kernels left out; (\d+) without OpenGL Core kernels");
    let from = rx(r"shaders: from (Metal|Vulkan|Direct3D 11): (\d+) of (\d+) shaders \((\d+) programs\); (\d+) SubShaders left out; (\d+) VR-only(?: variants)?(?: and|,) (\d+) GPU instancing(?: variants left out| and (\d+) GPU sprite skinning)?");
    let mut c = Map::new();
    let mut set = |c: &mut Map<String, Value>, k: &str, v: Value| {
        c.insert(k.to_string(), v);
    };
    for line in lines {
        if let Some(m) = shaders.captures(line) {
            for (k, i) in [("shaders", 1), ("programs", 2), ("subprograms", 3), ("files", 4), ("not_translatable", 5)] {
                set(&mut c, k, json!(n(&m[i])));
            }
            set(&mut c, "precision", json!(&m[6]));
            set(&mut c, "rewrites", json!(n(&m[7])));
            continue;
        }
        if let Some(m) = es31.captures(line) {
            set(&mut c, "es31_programs", json!(n(&m[1])));
            continue;
        }
        if let Some(m) = gl4x.captures(line) {
            set(&mut c, "gl4x_programs", json!(n(&m[1])));
            set(&mut c, "gl4x_subshaders_left_out", json!(n(&m[2])));
            set(&mut c, "gl4x_empty", json!(n(&m[3])));
            continue;
        }
        if line.contains("GLCore41/43 programs not translated (glcore4x off)") {
            set(&mut c, "gl4x", json!("off"));
            continue;
        }
        if let Some(m) = vr.captures(line) {
            set(&mut c, "vr_left_out", json!(n(&m[1])));
            continue;
        }
        if let Some(m) = compute.captures(line) {
            for (k, i) in [("compute", 1), ("compute_total", 2), ("kernels", 3), ("kernel_variants", 4), ("kernels_left_out", 5), ("compute_without_glcore", 6)] {
                set(&mut c, k, json!(n(&m[i])));
            }
            continue;
        }
        if let Some(m) = from.captures(line) {
            let k = match &m[1] {
                "Metal" => "metal",
                "Vulkan" => "vulkan",
                _ => "d3d11",
            };
            let mut d = json!({"converted": n(&m[2]), "total": n(&m[3]), "programs": n(&m[4]), "subshaders_left_out": n(&m[5]), "vr": n(&m[6]), "instancing": n(&m[7])});
            if let Some(s) = m.get(8) {
                d["skinning"] = json!(n(s.as_str()));
            }
            set(&mut c, k, d);
        }
    }
    Value::Object(c)
}

fn get<'a>(v: &'a Value, k: &str) -> &'a Value {
    v.get(k).unwrap_or(&Value::Null)
}

fn len_of(v: &Value) -> usize {
    match v {
        Value::Array(a) => a.len(),
        Value::Object(o) => o.len(),
        _ => 0,
    }
}

/// `v or {}`
fn obj_or(v: &Value) -> Value {
    match v {
        Value::Object(o) if !o.is_empty() => v.clone(),
        _ => json!({}),
    }
}

/// What `pmu port check --json` decided, without anything that names or describes the game.
pub fn check_verdict(rep: &Value) -> Value {
    let s = get(rep, "suggest");
    let mut ok: Vec<Value> = get(rep, "routes").as_array().map_or(vec![], |r| {
        r.iter().filter(|x| x.get("ok").is_some_and(truthy)).map(|x| get(x, "route").clone()).collect()
    });
    ok.sort_by_key(|a| a.as_str().unwrap_or("").to_string());
    json!({
        "route": get(rep, "route"),
        "routes_ok": ok,
        "build": get(rep, "build"),
        "backend": get(rep, "backend"),
        "compressed": get(rep, "compressed"),
        "unity_version": get(rep, "unity_version"),
        "unity_supported": get(rep, "unity_supported"),
        "scripting_runtime": get(rep, "scripting_runtime"),
        "shader_source": get(rep, "shader_source"),
        "suggest": {"runtime": obj_or(get(s, "runtime")), "android_data": obj_or(get(s, "android_data")), "convert": obj_or(get(s, "convert"))},
        "warnings": len_of(get(rep, "warnings")),
        "notes": len_of(get(rep, "notes")),
    })
}

/// `pmu route --json` reduced the same way.
pub fn route_verdict(rep: &Value) -> Value {
    let mut out = json!({"unity": get(rep, "unity"), "platform": get(rep, "platform"), "backend": get(rep, "backend"), "compressed": get(rep, "compressed")});
    for k in ["android", "box64"] {
        let r = get(rep, k);
        out[k] = json!({"fits": get(r, "fits"), "blockers": len_of(get(r, "blockers")), "caveats": len_of(get(r, "caveats"))});
    }
    for k in ["route", "recommended", "gl_mode", "gl"] {
        if let Some(v) = rep.get(k).filter(|v| !v.is_object() && !v.is_array()) {
            out[k] = v.clone();
        }
    }
    out
}

pub fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().is_some_and(|x| x != 0.0),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

const PLACEMENT: [&str; 5] = ["clone", "copy", "link", "hardlink", "symlink"];

pub fn placement_stats(stats: &Value) -> Value {
    let mut out: BTreeMap<String, Value> = BTreeMap::new();
    let mut placed = 0i64;
    if let Some(o) = stats.as_object() {
        for (k, v) in o {
            if PLACEMENT.contains(&k.as_str()) {
                placed += v.as_i64().unwrap_or(0);
            } else {
                out.insert(k.clone(), v.clone());
            }
        }
    }
    if placed != 0 {
        out.insert("placed".into(), json!(placed));
    }
    json!(out)
}

/// android-data's report (android_data_report.json) and log: what it placed and checked.
pub fn pack_summary(report: &Value, log: &[String]) -> Value {
    let (v, u) = (get(report, "verify"), get(report, "unity"));
    let verify = rx(r"^verify: OK");
    let packed = rx(r"^pack: \S+ verified");
    json!({
        "stats": placement_stats(get(report, "stats")),
        "changed_files": len_of(get(report, "files")),
        "bundles": len_of(get(report, "bundles")),
        "verify_problems": len_of(get(v, "problems")),
        "warnings": len_of(get(u, "warnings")),
        "dropped_modules": len_of(get(report, "dropped_modules")),
        "player": get(u, "player"),
        "scripting_runtime": get(u, "scripting_runtime"),
        "plugins_map": len_of(get(get(report, "plugins"), "map")),
        "verify_ok": log.iter().any(|l| verify.is_match(l)),
        "pack_verified": log.iter().filter(|l| packed.is_match(l)).count(),
    })
}

/// ua_host's log: frames, fps, crash, exit, shots taken, exceptions (counts only).
pub fn run_log(lines: &[String]) -> Value {
    let fps = rx(r"^\[fps\] ([\d.]+) fps, .*\((\d+) frames, (\d+) total, ([\d.]+) s\)");
    let shot = rx(r"^\[frames\] screenshot \S*shot-(\d+)\.ppm");
    let crash = rx(r"^\[crash\] signal (\d+)");
    let fatal = rx(r"FORTIFY|abort message|Fatal signal|Assertion .* failed|^\* Assertion|SIGSEGV");
    let exit = rx(r"^exit (\d+)$");
    let probe = rx(r"\[probe\] done: (\d+) ok, (\d+) failed(?: \((.*)\))?$");
    let exception = rx(r"\b\w*Exception\b(?::|$)");
    let quit = rx(r"\] up: quit$");
    let unity_exit = rx(r"UnityMain thread exits after (\d+) frames");
    let mut r = json!({"frames": 0, "fps_lines": 0, "last_fps_at": null, "fps_last": null, "crash": null, "fatal": 0,
        "exit": null, "shots_logged": 0, "exceptions": 0, "unity_exit_frames": null, "lines": lines.len(),
        "stopped": false, "probe": null});
    let int = |v: &Value| v.as_i64().unwrap_or(0);
    for line in lines {
        if let Some(m) = probe.captures(line.trim_end()) {
            let mut failed: Vec<String> = m.get(3).map_or("", |x| x.as_str()).split(',').map(|x| x.trim().to_string()).filter(|x| !x.is_empty()).collect();
            failed.sort();
            r["probe"] = json!({"ok": n(&m[1]), "failed": failed});
            continue;
        }
        if quit.is_match(line) {
            r["stopped"] = json!(true); // the rig's time was up: ua_host was asked to stop (SIGINT)
        }
        if let Some(m) = fps.captures(line) {
            r["frames"] = json!(int(&r["frames"]).max(n(&m[3])));
            r["fps_lines"] = json!(int(&r["fps_lines"]) + 1);
            r["last_fps_at"] = json!(m[4].parse::<f64>().unwrap_or(0.0));
            r["fps_last"] = json!(m[1].parse::<f64>().unwrap_or(0.0));
            continue;
        }
        if shot.is_match(line) {
            r["shots_logged"] = json!(int(&r["shots_logged"]) + 1);
            continue;
        }
        if let Some(m) = crash.captures(line) {
            if r["crash"].is_null() {
                r["crash"] = json!(n(&m[1]));
                continue;
            }
        }
        if let Some(m) = exit.captures(line.trim()) {
            r["exit"] = json!(n(&m[1]));
            continue;
        }
        if let Some(m) = unity_exit.captures(line) {
            r["unity_exit_frames"] = json!(n(&m[1]));
            r["frames"] = json!(int(&r["frames"]).max(n(&m[1])));
        }
        if fatal.is_match(line) {
            r["fatal"] = json!(int(&r["fatal"]) + 1);
        }
        if exception.is_match(line) {
            r["exceptions"] = json!(int(&r["exceptions"]) + 1);
        }
    }
    r
}

/// Is there a picture: of every step-th pixel in both directions, how many are dark (all channels
/// below 24), how many distinct colours (4 bits a channel), and the most common colour's share.
/// content = neither (almost) all dark nor (almost) all one colour.
pub fn picture_stats(w: usize, h: usize, rgb: &[u8], step: usize) -> Value {
    let (mut count, mut dark) = (0usize, 0usize);
    let mut counts: HashMap<(u8, u8, u8), usize> = HashMap::new();
    for y in (0..h).step_by(step) {
        for x in (0..w).step_by(step) {
            let i = (y * w + x) * 3;
            let Some(p) = rgb.get(i..i + 3) else { continue };
            count += 1;
            if p[0] < 24 && p[1] < 24 && p[2] < 24 {
                dark += 1;
            }
            *counts.entry((p[0] >> 4, p[1] >> 4, p[2] >> 4)).or_default() += 1;
        }
    }
    if count == 0 {
        return json!({"content": false, "dark": 1.0, "colours": 0, "top": 1.0});
    }
    let top = *counts.values().max().unwrap() as f64 / count as f64;
    let dark_f = dark as f64 / count as f64;
    let r3 = |x: f64| (x * 1000.0).round() / 1000.0;
    json!({"content": dark_f < 0.995 && top < 0.995, "dark": r3(dark_f), "colours": counts.len(), "top": r3(top)})
}

pub fn flatten(d: &Value, prefix: &str, out: &mut BTreeMap<String, Value>) {
    if let Some(o) = d.as_object() {
        for (k, v) in o {
            let key = format!("{prefix}{k}");
            if v.as_object().is_some_and(|x| !x.is_empty()) && k != "per_file" {
                flatten(v, &format!("{key}."), out);
            } else {
                out.insert(key, v.clone());
            }
        }
    }
}

/// Keys whose value differs from the baseline: [(key, baseline, now)].
pub fn compare_exact(golden: &Value, values: &Value) -> Vec<(String, Value, Value)> {
    let (mut g, mut v) = (BTreeMap::new(), BTreeMap::new());
    flatten(golden, "", &mut g);
    flatten(values, "", &mut v);
    let keys: BTreeSet<&String> = g.keys().chain(v.keys()).collect();
    keys.into_iter()
        .filter_map(|k| {
            let (a, b) = (g.get(k).cloned().unwrap_or(Value::Null), v.get(k).cloned().unwrap_or(Value::Null));
            (!num_eq(&a, &b)).then(|| (k.clone(), a, b))
        })
        .collect()
}

/// Python's ==: 1 == 1.0
fn num_eq(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Number(x), Value::Number(y)) => x.as_f64() == y.as_f64(),
        _ => a == b,
    }
}

/// Which per-file digests differ: (changed, added, removed) counts and the changed names.
pub fn changed_files(a: &Value, b: &Value) -> (usize, usize, usize, Vec<String>) {
    let e = Map::new();
    let (a, b) = (a.as_object().unwrap_or(&e), b.as_object().unwrap_or(&e));
    let mut changed: Vec<String> = a.keys().filter(|k| b.get(*k).is_some_and(|x| Some(x) != a.get(*k))).cloned().collect();
    changed.sort();
    let added = b.keys().filter(|k| !a.contains_key(*k)).count();
    let removed = a.keys().filter(|k| !b.contains_key(*k)).count();
    (changed.len(), added, removed, changed)
}

fn gi(v: &Value, k: &str) -> i64 {
    v.get(k).and_then(Value::as_i64).unwrap_or(0)
}

/// The list's expectations for the fast level.
pub fn expectations_fast(exp: &Value, v: &Value) -> Vec<Check> {
    let mut out = Vec::new();
    let sc = get(v, "shaders");
    if let Some(m) = exp.get("shaders_min").and_then(Value::as_i64) {
        let k = gi(sc, "shaders");
        out.push(("shaders_min".into(), k >= m, format!("{k} shaders given GLES3 programs (at least {m})")));
    }
    if let Some(m) = exp.get("not_translatable_max").and_then(Value::as_i64) {
        let k = gi(sc, "not_translatable");
        out.push(("not_translatable_max".into(), k <= m, format!("{k} not translatable (at most {m})")));
    }
    if let Some(m) = exp.get("compute_min").and_then(Value::as_i64) {
        let k = gi(sc, "compute");
        out.push(("compute_min".into(), k >= m, format!("{k} compute shaders with GLES3 kernels (at least {m})")));
    }
    for src in ["d3d11", "metal", "vulkan"] {
        let key = format!("{src}_min");
        if let Some(m) = exp.get(&key).and_then(Value::as_i64) {
            let k = gi(get(sc, src), "converted");
            out.push((key, k >= m, format!("{k} shaders from {src} (at least {m})")));
        }
    }
    let s = |x: &Value| match x {
        Value::Null => "None".to_string(),
        Value::String(s) => s.clone(),
        Value::Bool(b) => if *b { "True".into() } else { "False".into() },
        o => o.to_string(),
    };
    if let Some(want) = exp.get("route") {
        let r = Some(get(get(v, "check"), "route")).filter(|x| truthy(x)).cloned().unwrap_or_else(|| get(get(v, "route"), "route").clone());
        out.push(("route".into(), &r == want, format!("route {} (expected {})", s(&r), s(want))));
    }
    for (key, part) in [("android_fits", "android"), ("box64_fits", "box64")] {
        if let Some(want) = exp.get(key) {
            let r = get(get(get(v, "route"), part), "fits");
            out.push((key.into(), r == want, format!("{part} route fits: {}", s(r))));
        }
    }
    if let Some(p) = v.get("pack").filter(|p| !p.is_null()) {
        let mx = gi(exp, "verify_problems_max");
        let k = gi(p, "verify_problems");
        let vok = get(p, "verify_ok").as_bool().unwrap_or(false);
        let ok = k <= mx && (vok || k > 0);
        let known = if mx != 0 { format!(" (at most {mx}: known)") } else { String::new() };
        out.push(("pack_verify".into(), ok, format!("android-data verify: {}, {k} problems{known}", if vok { "OK" } else { "not OK" })));
    }
    out
}

fn pct0(x: f64) -> String {
    format!("{:.0}%", x * 100.0)
}

/// The list's expectations for the rig. shots: [{'t': s, 'content': ..}].
pub fn expectations_rig(exp: &Value, run: &Value, shots: &[Value], log_text: &str) -> Vec<Check> {
    let mut out = Vec::new();
    if exp.get("no_crash").is_none_or(truthy) {
        let ok = get(run, "crash").is_null() && !truthy(get(run, "fatal"));
        out.push(("no_crash".into(), ok, if ok { "no crash".into() } else { format!("crash (signal {}), {} fatal lines", show(get(run, "crash")), show(get(run, "fatal"))) }));
    }
    let allowed: Vec<i64> = exp.get("exit").and_then(Value::as_array).map_or(vec![124], |a| a.iter().filter_map(Value::as_i64).collect());
    let ex = get(run, "exit");
    if ex.as_i64() == Some(137) && truthy(get(run, "stopped")) && allowed.contains(&124) {
        // asked to stop when the time was up, the engine did not pause in time and the rig's timeout
        // killed it: a slow shutdown on a busy machine, not a fault of the run
        out.push(("exit".into(), true, "exit 137: still running at the end, killed when its shutdown took over 20 s".into()));
    } else if !ex.is_null() || !allowed.is_empty() {
        let ok = ex.as_i64().is_some_and(|e| allowed.contains(&e));
        let a: Vec<String> = allowed.iter().map(|x| x.to_string()).collect();
        out.push(("exit".into(), ok, format!("exit {} (expected {}: 124 = still running at the end)", show(ex), a.join(" or "))));
    }
    if let Some(m) = exp.get("frames_min").and_then(Value::as_i64) {
        let f = gi(run, "frames");
        out.push(("frames_min".into(), f >= m, format!("{f} frames (at least {m})")));
    }
    for t in exp.get("content").and_then(Value::as_array).map_or(&[][..], |a| a.as_slice()) {
        let ti = t.as_i64().unwrap_or(-1);
        let key = format!("content@{ti}");
        match shots.iter().find(|s| s.get("t").and_then(Value::as_i64) == Some(ti)) {
            None => out.push((key, false, format!("no shot at {ti} s (the run ended before it?)"))),
            Some(s) if truthy(get(s, "missing")) => out.push((key, false, format!("no shot at {ti} s: no frame came after the request (the game drew nothing more)"))),
            Some(s) => {
                let c = truthy(get(s, "content"));
                out.push((
                    key,
                    c,
                    format!(
                        "shot at {ti} s: {} (dark {}, {} colours)",
                        if c { "a picture" } else { "black or one colour" },
                        pct0(get(s, "dark").as_f64().unwrap_or(1.0)),
                        gi(s, "colours")
                    ),
                ));
            }
        }
    }
    let multiline = |r: &str| Regex::new(&format!("(?m){r}"));
    for r in exp.get("log").and_then(Value::as_array).map_or(&[][..], |a| a.as_slice()).iter().filter_map(Value::as_str) {
        let ok = multiline(r).is_ok_and(|x| x.is_match(log_text));
        out.push((format!("log:{r}"), ok, format!("{} in the run log: /{r}/", if ok { "found" } else { "not found" })));
    }
    for r in exp.get("log_absent").and_then(Value::as_array).map_or(&[][..], |a| a.as_slice()).iter().filter_map(Value::as_str) {
        let ok = !multiline(r).is_ok_and(|x| x.is_match(log_text));
        out.push((format!("no-log:{r}"), ok, format!("{} in the run log: /{r}/", if ok { "absent" } else { "present" })));
    }
    if let Some(m) = exp.get("exceptions_max").and_then(Value::as_i64) {
        let e = gi(run, "exceptions");
        out.push(("exceptions_max".into(), e <= m, format!("{e} exception lines (at most {m})")));
    }
    out
}

/// Python's str() of a JSON value (None for null).
pub fn show(v: &Value) -> String {
    match v {
        Value::Null => "None".into(),
        Value::String(s) => s.clone(),
        Value::Bool(b) => if *b { "True".into() } else { "False".into() },
        o => o.to_string(),
    }
}

/// The rig against its baseline: every shot time that had a picture has one now, the run is not far
/// slower (frames at least frames_ratio of the baseline's), no crash where there was none, no probe
/// check failing that passed.
pub fn compare_rig(golden: &Value, run: &Value, shots: &[Value], frames_ratio: f64) -> Vec<Check> {
    let mut out = Vec::new();
    if !truthy(golden) {
        return out;
    }
    let gs: Vec<&Value> = golden.get("shots").and_then(Value::as_array).map_or(vec![], |a| a.iter().collect());
    for s in shots {
        let t = get(s, "t").as_i64();
        if let Some(g) = gs.iter().find(|g| get(g, "t").as_i64() == t) {
            if truthy(get(g, "content")) && !truthy(get(s, "content")) {
                let now = if truthy(get(s, "missing")) { "no frame came after the request" } else { "black or one colour" };
                let t = t.unwrap_or(0);
                out.push((format!("baseline:content@{t}"), false, format!("shot at {t} s had a picture in the baseline, now {now}")));
            }
        }
    }
    let grun = get(golden, "run");
    let gf = gi(grun, "frames");
    if gf != 0 {
        let f = gi(run, "frames");
        let ok = f as f64 >= frames_ratio * gf as f64;
        let more = if ok { String::new() } else { format!(" (under {}: slower or stuck)", pct0(frames_ratio)) };
        out.push(("baseline:frames".into(), ok, format!("{f} frames, baseline {gf}{more}")));
    }
    if get(grun, "crash").is_null() && !get(run, "crash").is_null() {
        out.push(("baseline:crash".into(), false, format!("crash (signal {}); none in the baseline", show(get(run, "crash")))));
    }
    let (gp, p) = (get(grun, "probe"), get(run, "probe"));
    if truthy(gp) && truthy(p) {
        let names = |v: &Value| -> BTreeSet<String> { get(v, "failed").as_array().map_or(BTreeSet::new(), |a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect()) };
        let new: Vec<String> = names(p).difference(&names(gp)).cloned().collect();
        let failed: Vec<String> = get(p, "failed").as_array().map_or(vec![], |a| a.iter().map(show).collect());
        let f = if failed.is_empty() { "none".to_string() } else { format!("[{}]", failed.iter().map(|x| format!("'{x}'")).collect::<Vec<_>>().join(", ")) };
        let tail = if new.is_empty() { String::new() } else { format!("; new since the baseline: {}", new.join(", ")) };
        out.push(("baseline:probe".into(), new.is_empty(), format!("probe: {} ok, failed {f}{tail}", gi(p, "ok"))));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lines(s: &str) -> Vec<String> {
        s.lines().map(String::from).collect()
    }

    #[test]
    fn shader_log() {
        let c = shader_counts(&lines("[  1.0s] shaders: 12 shaders given GLES3 programs (40 programs, 80 subprograms) in 3 files; 2 not translatable; precision mediump; rewrites: 5 stages\nshaders: 3 programs need GLSL ES 3.10\nshaders: from Direct3D 11: 4 of 5 shaders (9 programs); 1 SubShaders left out; 0 VR-only variants, 2 GPU instancing variants left out"));
        assert_eq!(c["shaders"], 12);
        assert_eq!(c["precision"], "mediump");
        assert_eq!(c["es31_programs"], 3);
        assert_eq!(c["d3d11"]["converted"], 4);
        assert_eq!(c["d3d11"]["instancing"], 2);
        assert!(c["d3d11"].get("skinning").is_none());
    }

    #[test]
    fn run_and_pictures() {
        let r = run_log(&lines("[fps] 30.0 fps, frame 33 avg (30 frames, 90 total, 3.0 s)\n[crash] signal 11\nNullReferenceException: x\nexit 124\n[probe] done: 5 ok, 2 failed (b, a)"));
        assert_eq!(r["frames"], 90);
        assert_eq!(r["crash"], 11);
        assert_eq!(r["exit"], 124);
        assert_eq!(r["exceptions"], 1);
        assert_eq!(r["probe"]["failed"], json!(["a", "b"]));
        let black = vec![0u8; 8 * 8 * 3];
        assert_eq!(picture_stats(8, 8, &black, 4)["content"], false);
        let mut img = vec![0u8; 8 * 8 * 3];
        for (i, p) in img.chunks_mut(3).enumerate() {
            p[0] = (i * 37 % 256) as u8;
            p[1] = (i * 91 % 256) as u8;
        }
        assert_eq!(picture_stats(8, 8, &img, 1)["content"], true);
    }

    #[test]
    fn comparisons() {
        let g = json!({"shaders": {"shaders": 3, "per_file": {"a": "1"}}, "x": 1});
        let v = json!({"shaders": {"shaders": 4, "per_file": {"a": "2", "b": "3"}}, "x": 1.0});
        let d = compare_exact(&g, &v);
        assert_eq!(d.iter().map(|x| x.0.as_str()).collect::<Vec<_>>(), ["shaders.per_file", "shaders.shaders"]);
        assert_eq!(changed_files(&d[0].1, &d[0].2), (1, 1, 0, vec!["a".to_string()]));
        let e = expectations_fast(&json!({"shaders_min": 4, "route": "android"}), &json!({"shaders": {"shaders": 3}, "check": {"route": "android"}}));
        assert_eq!(e[0].1, false);
        assert_eq!(e[1], ("route".to_string(), true, "route android (expected android)".to_string()));
        let rig = expectations_rig(&json!({"content": [10], "frames_min": 5}), &json!({"crash": null, "fatal": 0, "exit": 124, "frames": 9}), &[json!({"t": 10, "content": true, "dark": 0.2, "colours": 40})], "");
        assert!(rig.iter().all(|c| c.1), "{rig:?}");
        let cr = compare_rig(&json!({"run": {"frames": 100, "crash": null}, "shots": [{"t": 10, "content": true}]}), &json!({"frames": 20, "crash": null}), &[json!({"t": 10, "content": false})], 0.3);
        assert_eq!(cr.len(), 2);
        assert!(!cr[0].1 && !cr[1].1);
    }
}
