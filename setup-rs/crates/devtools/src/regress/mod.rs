//! `pmu-dev regress` (was tools/regress/regress.py): the regression run. Takes the games of
//! tools/regress/games.toml through the tools as a porter would (every step by pmu) and compares what
//! comes out with the list's expectations and with the recorded baseline (tools/regress/README.md).
//!
//!   regress fast [--only REGEX] [--update] [--keep] [--checkout DIR]   check + convert + pack + checks
//!   regress rig  [--only REGEX] [--update] [--keep] [--checkout DIR]   the games in the emulated rig
//!   regress list [--only REGEX]                                        the list: games x levels x checks
//!   regress bisect GOOD BAD --level fast|rig --only REGEX [--build CMD] the first commit at which they fail
//!   regress recheck <results folder>                                   a finished run judged again
//!
//! It runs where pmu runs (setup-rs/tools/pmu-dev.sh: in Docker, the folders of local.toml mounted), and
//! what needs the computer (the emulated rig's Docker, asmpatch's .NET) through pkgzip's host requests.
//! Exit status: 0 when nothing failed, 1 when a check failed, 2 for a usage or setup error (125 for
//! git bisect when nothing could be judged).
pub mod checks;
pub mod digest;

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use regex::Regex;
use serde_json::{json, Map, Value};

use checks::{show, truthy, Check};

const ROUTES: [&str; 6] = ["android", "apk", "il2cpp", "box64", "shaders", "prebuilt"];
const PACK: [&str; 3] = ["none", "tree", "all"];
const LIBS: &str = "Variations/{backend}/Release/Libs/{abi}";
const MONOLIBS: &str = "Variations/mono/Release/MonoLibs/armeabi-v7a";
const APK_FACTS: [&str; 12] =
    ["abi", "abis", "backend", "min_sdk", "target_sdk", "unity_version", "source", "obbs", "il2cpp", "swappy", "graphics_apis", "target"];

enum Stop {
    Setup(String),
    Skip(String),
    Fail(String),
}

type R<T> = Result<T, Stop>;

fn fail<T>(m: impl Into<String>) -> R<T> {
    Err(Stop::Fail(m.into()))
}
fn skip<T>(m: impl Into<String>) -> R<T> {
    Err(Stop::Skip(m.into()))
}

fn here() -> PathBuf {
    // (tools/regress of the checkout this pmu-dev comes from, else the given one: $REGRESS_HERE)
    std::env::var_os("REGRESS_HERE").map(PathBuf::from).unwrap_or_else(|| {
        let p = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tools/regress");
        if p.is_dir() {
            p
        } else {
            std::env::current_dir().unwrap_or_default().join("tools/regress")
        }
    })
}

fn toml_file(p: &Path) -> Result<Value, String> {
    let text = fs::read_to_string(p).map_err(|e| format!("{}: {e}", p.display()))?;
    let v = port::toml::loads(&text).map_err(|e| format!("{}: {e}", p.display()))?;
    serde_json::from_str(&port::pyjson::dumps(&v, None, false)).map_err(|e| e.to_string())
}

fn deep_merge(a: &Value, b: &Value) -> Value {
    let mut out = a.as_object().cloned().unwrap_or_default();
    for (k, v) in b.as_object().into_iter().flatten() {
        let m = match (out.get(k), v) {
            (Some(x @ Value::Object(_)), Value::Object(_)) => deep_merge(x, v),
            _ => v.clone(),
        };
        out.insert(k.clone(), m);
    }
    Value::Object(out)
}

#[derive(Default, Clone)]
struct Opts {
    level: String,
    rest: Vec<String>,
    bisect_level: String,
    build: Option<String>,
    bisect_run: bool,
    only: Option<String>,
    list: String,
    config: Option<String>,
    checkout: Option<String>,
    update: bool,
    keep: bool,
    pack: Option<String>,
    offline: bool,
}

const USAGE: &str = "usage: pmu-dev regress {fast,rig,list,bisect,recheck} [rest...] [--only REGEX] [--list FILE] [--config FILE] \
[--checkout DIR] [--update] [--keep] [--pack none|tree|all] [--offline] [--level fast|rig] [--build CMD]";

fn parse(args: &[String]) -> Result<Opts, String> {
    let mut o = Opts { bisect_level: "fast".into(), list: here().join("games.toml").to_string_lossy().into_owned(), ..Default::default() };
    let mut pos = Vec::new();
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        let (k, inline) = match a.split_once('=') {
            Some((k, v)) if k.starts_with("--") => (k.to_string(), Some(v.to_string())),
            _ => (a.clone(), None),
        };
        let mut val = || -> Result<String, String> {
            if let Some(v) = &inline {
                return Ok(v.clone());
            }
            i += 1;
            args.get(i).cloned().ok_or_else(|| format!("{USAGE}\n{k}: expected one argument"))
        };
        match k.as_str() {
            "--level" => o.bisect_level = val()?,
            "--build" => o.build = Some(val()?),
            "--only" => o.only = Some(val()?),
            "--list" => o.list = val()?,
            "--config" => o.config = Some(val()?),
            "--checkout" => o.checkout = Some(val()?),
            "--pack" => o.pack = Some(val()?),
            "--bisect-run" => o.bisect_run = true,
            "--update" => o.update = true,
            "--keep" => o.keep = true,
            "--offline" => o.offline = true,
            x if x.starts_with("--") => return Err(format!("{USAGE}\nunrecognized arguments: {x}")),
            _ => pos.push(a.clone()),
        }
        i += 1;
    }
    let Some(level) = pos.first() else { return Err(USAGE.into()) };
    if !["fast", "rig", "list", "bisect", "recheck"].contains(&level.as_str()) {
        return Err(format!("{USAGE}\nlevel {level}: one of fast, rig, list, bisect, recheck (`pmu` went with the Python)"));
    }
    if o.pack.as_deref().is_some_and(|p| !PACK.contains(&p)) || !["fast", "rig"].contains(&o.bisect_level.as_str()) {
        return Err(USAGE.into());
    }
    o.level = level.clone();
    o.rest = pos[1..].to_vec();
    Ok(o)
}

/// The machine's own settings (local.toml, gitignored; $REGRESS_CONFIG names another file); environment
/// variables REGRESS_<KEY> override single keys.
fn load_local(path: Option<&str>) -> Result<Value, String> {
    let p = path
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("REGRESS_CONFIG").map(PathBuf::from))
        .unwrap_or_else(|| here().join("local.toml"));
    let mut cfg = if p.is_file() { toml_file(&p)? } else { json!({}) };
    for k in ["corpus", "scratch", "il2cpp_home", "hostbin", "hostbin64", "asmpatch", "pmhlslcc", "pmspirvcross", "dotnet", "box64_bin", "pmu"] {
        if let Ok(v) = std::env::var(format!("REGRESS_{}", k.to_uppercase())) {
            if !v.is_empty() {
                cfg[k] = json!(v);
            }
        }
    }
    if let Ok(v) = std::env::var("REGRESS_PLAYERS") {
        if !v.is_empty() {
            cfg["players"] = json!(v.split(':').collect::<Vec<_>>());
        }
    }
    cfg["_path"] = json!(p.to_string_lossy());
    Ok(cfg)
}

fn load_list(path: &str) -> Result<Vec<Value>, String> {
    let doc = toml_file(Path::new(path))?;
    let mut games: Vec<Value> = doc.get("game").and_then(Value::as_array).cloned().unwrap_or_default();
    let mut seen = std::collections::HashSet::new();
    for g in &mut games {
        for k in ["slug", "route"] {
            if g.get(k).is_none() {
                return Err(format!("{path}: a [[game]] without {k}: {g}"));
            }
        }
        if g.get("id").is_none() {
            g["id"] = g["slug"].clone();
        }
        let id = show(&g["id"]);
        if !seen.insert(id.clone()) {
            return Err(format!("{path}: {id} twice (give one an id)"));
        }
        let route = show(&g["route"]);
        if !ROUTES.contains(&route.as_str()) {
            return Err(format!("{path}: {}: route '{route}' (one of {})", show(&g["slug"]), ROUTES.join(", ")));
        }
        if g.get("levels").is_none() {
            g["levels"] = json!(["fast"]);
        }
        if g.get("covers").is_none() {
            g["covers"] = json!([]);
        }
        let pack = g.get("pack").map_or("none".to_string(), show);
        if !PACK.contains(&pack.as_str()) {
            return Err(format!("{path}: {}: pack '{pack}' (one of {})", show(&g["slug"]), PACK.join(", ")));
        }
    }
    Ok(games)
}

fn s(v: &Value, k: &str) -> String {
    v.get(k).map(show).unwrap_or_default()
}
fn strs(v: &Value, k: &str) -> Vec<String> {
    v.get(k).and_then(Value::as_array).map_or(vec![], |a| a.iter().map(show).collect())
}
fn abspath(p: &str) -> String {
    let pb = Path::new(p);
    let full = if pb.is_absolute() { pb.to_path_buf() } else { std::env::current_dir().unwrap_or_default().join(pb) };
    port::abspath(&full).to_string_lossy().into_owned()
}

struct Ctx {
    a: Opts,
    local: Value,
    checkout: String,
    pmu: String,
    scratch: String,
    corpus: Option<String>,
    players: Vec<String>,
    fetch_players: String,
    il2cpp_home: Option<String>,
    hostbin: String,
    hostbin64: String,
    asmpatch: Option<String>,
    games: Value,
    prebuilt: Value,
    env: Vec<(String, String)>,
    run_id: String,
    results: String,
}

fn utc_stamp() -> String {
    let t = unsafe { libc::time(std::ptr::null_mut()) };
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::gmtime_r(&t, &mut tm) };
    format!("{:04}{:02}{:02}T{:02}{:02}{:02}Z", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)
}

impl Ctx {
    fn new(a: Opts, local: Value) -> R<Ctx> {
        let checkout = abspath(a.checkout.as_deref().unwrap_or(&here().join("../..").to_string_lossy()));
        if !Path::new(&checkout).join("setup-rs/templates").is_dir() {
            return Err(Stop::Setup(format!("{checkout}: no setup-rs/templates (not a portmaster-unity checkout)")));
        }
        let pmu = local.get("pmu").map(show).unwrap_or_else(|| format!("{checkout}/tools/port/out/pmu/pmu"));
        if !Path::new(&pmu).is_file() {
            return Err(Stop::Setup(format!("no pmu at {pmu} (setup-rs/tools/build_pmu.sh {checkout}/tools/port/out/pmu, or pmu in the local config)")));
        }
        let scratch = local.get("scratch").map(show).filter(|x| !x.is_empty()).ok_or_else(|| {
            Stop::Setup(format!("no scratch folder: set scratch in {} or REGRESS_SCRATCH", s(&local, "_path")))
        })?;
        let scratch = abspath(&scratch);
        let hostbin = abspath(&local.get("hostbin").map(show).unwrap_or_else(|| format!("{checkout}/runtime/host/out")));
        let hostbin64 = abspath(&local.get("hostbin64").map(show).unwrap_or_else(|| format!("{hostbin}/arm64")));
        let dotnet = local.get("dotnet").map(show).unwrap_or_else(|| "dotnet".into());
        let asmpatch = local.get("asmpatch").map(show).or_else(|| {
            let dll = format!("{checkout}/tools/asmpatch/out/any/asmpatch.dll");
            Path::new(&dll).is_file().then(|| format!("{dotnet} {dll}"))
        });
        let mut env = Vec::new();
        for (k, e) in [("pmhlslcc", "PMHLSLCC"), ("pmspirvcross", "PMSPIRVCROSS")] {
            if let Some(v) = local.get(k) {
                env.push((e.to_string(), show(v)));
            }
        }
        if let Some(a) = &asmpatch {
            env.push(("PMUNITY_ASMPATCH".into(), a.clone()));
        }
        env.push(("PMUNITY_REPO".into(), checkout.clone()));
        let run_id = utc_stamp();
        let results = format!("{scratch}/results/{}-{run_id}", a.level);
        Ok(Ctx {
            corpus: local.get("corpus").map(show),
            players: strs(&local, "players").into_iter().map(|p| abspath(&p)).collect(),
            fetch_players: format!("{scratch}/players"),
            il2cpp_home: local.get("il2cpp_home").map(show),
            games: local.get("games").cloned().unwrap_or(json!({})),
            prebuilt: local.get("prebuilt").cloned().unwrap_or(json!({})),
            a,
            local,
            checkout,
            pmu,
            scratch,
            hostbin,
            hostbin64,
            asmpatch,
            env,
            run_id,
            results,
        })
    }

    /// Machine paths replaced in what goes into a baseline (commands of a dry run).
    fn paths_to_hide(&self) -> Vec<(String, String)> {
        let mut out = vec![(self.scratch.clone(), "<scratch>".to_string()), (self.checkout.clone(), "<checkout>".to_string())];
        if let Some(h) = &self.il2cpp_home {
            out.push((abspath(h), "<home>".into()));
        }
        if let Some(c) = &self.corpus {
            out.push((abspath(c), "<corpus>".into()));
        }
        if let Ok(h) = std::env::var("HOME") {
            out.push((h, "<home-dir>".into()));
        }
        out.sort_by_key(|p| std::cmp::Reverse(p.0.len()));
        out
    }
}

fn shq(s: &str) -> String {
    if !s.is_empty() && s.bytes().all(|c| c.is_ascii_alphanumeric() || b"@%+=:,./-_".contains(&c)) {
        s.to_string()
    } else {
        format!("'{}'", s.replace('\'', "'\"'\"'"))
    }
}

fn append(log: &Path, text: &str) {
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(log) {
        let _ = std::io::Write::write_all(&mut f, text.as_bytes());
    }
}

/// Runs cmd with its output appended to the file log; returns the exit status (124 on a timeout).
fn run(cmd: &[String], log: &Path, cwd: Option<&str>, env: &[(String, String)], timeout: Option<u64>) -> i32 {
    append(log, &format!("$ {}\n", cmd.iter().map(|c| shq(c)).collect::<Vec<_>>().join(" ")));
    let Ok(f) = fs::OpenOptions::new().append(true).open(log) else { return 1 };
    let Ok(f2) = f.try_clone() else { return 1 };
    let mut c = Command::new(&cmd[0]);
    c.args(&cmd[1..]).stdout(f).stderr(f2).envs(env.iter().map(|(k, v)| (k, v)));
    if let Some(d) = cwd {
        c.current_dir(d);
    }
    let Ok(mut ch) = c.spawn() else {
        append(log, &format!("(could not start {})\n", cmd[0]));
        return 127;
    };
    let t0 = Instant::now();
    loop {
        if let Ok(Some(st)) = ch.try_wait() {
            return st.code().unwrap_or(1);
        }
        if timeout.is_some_and(|t| t0.elapsed() > Duration::from_secs(t)) {
            let _ = ch.kill();
            let _ = ch.wait();
            append(log, &format!("(timed out after {} s)\n", timeout.unwrap()));
            return 124;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// A command that prints JSON on stdout (its log on stderr, into the file log): (status, object).
fn run_json(cmd: &[String], log: &Path, env: &[(String, String)]) -> (i32, Option<Value>) {
    append(log, &format!("$ {}\n", cmd.iter().map(|c| shq(c)).collect::<Vec<_>>().join(" ")));
    let Ok(f) = fs::OpenOptions::new().append(true).open(log) else { return (1, None) };
    let o = Command::new(&cmd[0]).args(&cmd[1..]).envs(env.iter().map(|(k, v)| (k, v))).stderr(f).stdin(Stdio::null()).output();
    let Ok(o) = o else { return (127, None) };
    let code = o.status.code().unwrap_or(1);
    match serde_json::from_slice::<Value>(&o.stdout) {
        Ok(v) => (code, Some(v)),
        Err(_) => {
            let t = String::from_utf8_lossy(&o.stdout);
            append(log, &t[t.len().saturating_sub(2000)..]);
            (if code == 0 { 1 } else { code }, None)
        }
    }
}

fn tail(path: &Path, n: usize) -> String {
    let t = fs::read(path).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
    let lines: Vec<&str> = t.lines().filter(|l| !l.trim().is_empty()).collect();
    let s = lines[lines.len().saturating_sub(n)..].iter().map(|l| l.trim()).collect::<Vec<_>>().join(" | ");
    s.chars().rev().take(400).collect::<Vec<_>>().into_iter().rev().collect()
}

fn read_lines(path: &Path) -> Vec<String> {
    fs::read(path).map(|b| String::from_utf8_lossy(&b).lines().map(String::from).collect()).unwrap_or_default()
}

fn rmtree(p: &Path) {
    if p.is_symlink() || p.is_file() {
        let _ = fs::remove_file(p);
    } else if p.is_dir() {
        let _ = fs::remove_dir_all(p);
    }
}

fn copyfile(a: &Path, b: &Path) {
    let _ = fs::copy(a, b);
}

/// Python's str.replace of every machine path (a dry run's commands).
fn hide_paths(text: &str, pairs: &[(String, String)]) -> String {
    let mut t = text.to_string();
    for (p, n) in pairs {
        if !p.is_empty() {
            t = t.replace(p.as_str(), n);
        }
    }
    t
}

/// A data file's name as the baseline keeps it: the converter's generic Unity names as they are,
/// anything else (a game's own bundle names) as a short hash.
pub fn hide_name(rel: &str) -> String {
    use sha2::Digest;
    let generic = Regex::new(r"^(Resources/)?(level\d+|sharedassets\d+\.assets|resources\.assets|globalgamemanagers(\.assets)?|unity_builtin_extra|unity default resources)$").unwrap();
    if generic.is_match(rel) {
        return rel.to_string();
    }
    format!("f-{}", &digest::hex(&sha2::Sha256::digest(rel.as_bytes()))[..12])
}

/// The game's folder: the corpus' games/<slug>/game, or a path the local config gives under [games].
fn game_root(ctx: &Ctx, g: &Value) -> R<String> {
    let slug = s(g, "slug");
    if let Some(p) = ctx.games.get(&slug) {
        let p = show(p);
        if !Path::new(&p).exists() {
            return skip(format!("{p} (local config [games].{slug}) not found"));
        }
        return Ok(abspath(&p));
    }
    let Some(c) = &ctx.corpus else { return skip("no corpus (corpus in the local config, or REGRESS_CORPUS)") };
    let p = format!("{c}/games/{slug}/game");
    if !Path::new(&p).is_dir() {
        return skip(format!("not in the corpus (games/{slug}/game)"));
    }
    Ok(p)
}

/// The Unity data folder in a game folder, as the corpus scripts find it.
fn find_data(root: &str, g: &Value) -> R<String> {
    if let Some(d) = g.get("data") {
        let p = format!("{root}/{}", show(d));
        if !Path::new(&p).is_dir() {
            return skip(format!("data folder {} not in the game folder", show(d)));
        }
        return Ok(p);
    }
    let r = Path::new(root);
    if r.join("globalgamemanagers").is_file() || r.join("data.unity3d").is_file() {
        return Ok(root.to_string());
    }
    let mut found: Vec<String> = Vec::new();
    fn walk(d: &Path, depth: usize, found: &mut Vec<String>) {
        if depth > 5 || d.components().any(|c| c.as_os_str() == "__MACOSX") {
            return;
        }
        let s = d.to_string_lossy();
        if s.ends_with("_Data") || s.ends_with(".app/Contents/Resources/Data") {
            found.push(s.into_owned());
            return;
        }
        let mut subs: Vec<PathBuf> = fs::read_dir(d).map(|r| r.filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| p.is_dir()).collect()).unwrap_or_default();
        subs.sort();
        for x in subs {
            walk(&x, depth + 1, found);
        }
    }
    walk(r, 0, &mut found);
    found.sort_by_key(|p| (p.matches('/').count(), p.clone()));
    found.into_iter().next().ok_or(Stop::Skip("no Unity data folder in the game folder".into()))
}

fn mono_etc(data: &str) -> Option<String> {
    let d = Path::new(data);
    for base in [Some(d), d.parent(), d.parent().and_then(Path::parent)].into_iter().flatten() {
        for m in ["MonoBleedingEdge", "Mono"] {
            let p = base.join(m).join("etc");
            if p.is_dir() {
                return Some(p.to_string_lossy().into_owned());
            }
        }
    }
    None
}

/// A folder of symlinks to every file of src, each overlay folder's files replacing src's (an
/// unpacked data.unity3d, a convert overlay); nothing is copied.
fn view_dir(src: &str, overlays: &[&str], dst: &Path) -> R<String> {
    rmtree(dst);
    fs::create_dir_all(dst).map_err(|e| Stop::Fail(e.to_string()))?;
    let mut layers = vec![src.to_string()];
    layers.extend(overlays.iter().filter(|o| Path::new(o).is_dir()).map(|o| o.to_string()));
    for (li, base) in layers.iter().enumerate() {
        fn walk(base: &Path, rel: &Path, dst: &Path, first: bool) {
            let d = base.join(rel);
            let _ = fs::create_dir_all(dst.join(rel));
            let Ok(rd) = fs::read_dir(&d) else { return };
            for e in rd.filter_map(|e| e.ok()) {
                let n = e.file_name().to_string_lossy().into_owned();
                if n.starts_with(".pmunity") {
                    continue;
                }
                let p = e.path();
                if p.is_dir() {
                    walk(base, &rel.join(&n), dst, first);
                } else {
                    if first && n == "data.unity3d" {
                        continue;
                    }
                    let t = dst.join(rel).join(&n);
                    let _ = fs::remove_file(&t);
                    let real = fs::canonicalize(&p).unwrap_or(p);
                    let _ = std::os::unix::fs::symlink(real, &t);
                }
            }
        }
        walk(Path::new(base), Path::new(""), dst, li == 0);
    }
    Ok(dst.to_string_lossy().into_owned())
}

/// `docker ps` names, on the computer (pkgzip::host when this runs in pmu-dev.sh's container).
fn docker_names() -> Vec<String> {
    match pkgzip::host::output(Command::new("docker").args(["ps", "--format", "{{.Names}}"])) {
        Ok(o) => String::from_utf8_lossy(&o.stdout).split_whitespace().map(String::from).collect(),
        Err(_) => vec![],
    }
}

/// At most one container of this run at a time (names regress*), and none started while two or more
/// others run (the Mac's Docker VM is shared).
fn docker_gate(log: &Path) {
    let mut waited = 0;
    loop {
        let names = docker_names();
        let mine: Vec<&String> = names.iter().filter(|n| n.starts_with("regress")).collect();
        // (pmu-dev.sh's own container is one of the others)
        let own = std::env::var_os("PMU_HOST_EXEC").is_some() as usize;
        let others = (names.len() - mine.len()).saturating_sub(own);
        if mine.is_empty() && others < 2 {
            if waited != 0 {
                append(log, &format!("(waited {waited} s for Docker: {others} other containers)\n"));
            }
            return;
        }
        if waited % 300 == 0 {
            println!("    waiting for Docker: ours {mine:?}, {others} others");
        }
        std::thread::sleep(Duration::from_secs(20));
        waited += 20;
    }
}

struct Game<'a> {
    ctx: &'a Ctx,
    g: Value,
    level: String,
    slug: String,
    id: String,
    work: PathBuf,
    out: PathBuf,
    log: PathBuf,
    values: Map<String, Value>,
    checks: Vec<Check>,
    notes: Vec<String>,
    timing: Map<String, Value>,
    root: String,
    data: String,
    version: String,
    names: BTreeMap<String, String>,
}

impl<'a> Game<'a> {
    fn new(ctx: &'a Ctx, g: &Value, level: &str) -> Game<'a> {
        let id = s(g, "id");
        let out = PathBuf::from(&ctx.results).join(&id);
        Game {
            ctx,
            g: g.clone(),
            level: level.into(),
            slug: s(g, "slug"),
            work: PathBuf::from(&ctx.scratch).join("work").join(format!("{level}-{id}")),
            log: out.join("run.txt"),
            out,
            id,
            values: Map::new(),
            checks: vec![],
            notes: vec![],
            timing: Map::new(),
            root: String::new(),
            data: String::new(),
            version: String::new(),
            names: BTreeMap::new(),
        }
    }

    fn pmu(&self, args: &[String], log: &Path, timeout: Option<u64>, env: &[(String, String)]) -> i32 {
        let mut cmd = vec![self.ctx.pmu.clone()];
        cmd.extend(args.iter().cloned());
        let mut e = self.ctx.env.clone();
        e.extend(env.iter().cloned());
        run(&cmd, log, Some(&self.ctx.checkout), &e, timeout)
    }

    fn pmu_json(&self, args: &[String], log: &Path) -> (i32, Option<Value>) {
        let mut cmd = vec![self.ctx.pmu.clone()];
        cmd.extend(args.iter().cloned());
        run_json(&cmd, log, &self.ctx.env)
    }

    /// A timed step: its name in the log and the wall seconds it took (timing).
    fn step<T>(&mut self, name: &str, f: impl FnOnce(&mut Self) -> R<T>) -> R<T> {
        let t = Instant::now();
        println!("    {name}");
        append(&self.log, &format!("\n== {name}\n"));
        let r = f(self);
        self.timing.insert(name.into(), json!((t.elapsed().as_secs_f64() * 10.0).round() / 10.0));
        r
    }

    fn route(&self) -> String {
        s(&self.g, "route")
    }

    fn anyver(&self) -> Vec<String> {
        if self.g.get("any_unity_version").is_some_and(truthy) {
            vec!["--any-unity-version".into()]
        } else {
            vec![]
        }
    }

    fn locate(&mut self) -> R<()> {
        if self.route() == "prebuilt" {
            return Ok(());
        }
        self.root = game_root(self.ctx, &self.g)?;
        if self.route() == "apk" {
            return Ok(());
        }
        self.data = find_data(&self.root, &self.g)?;
        self.version = self.g.get("unity").map(show).unwrap_or_else(|| {
            let p = format!("{}/games/{}/source.json", self.ctx.corpus.clone().unwrap_or_default(), self.slug);
            fs::read(&p).ok().and_then(|b| serde_json::from_slice::<Value>(&b).ok()).map(|v| s(&v, "version")).unwrap_or_default()
        });
        Ok(())
    }

    /// port check --json (android, il2cpp, apk) or route --json (box64): the verdict.
    fn check(&mut self) -> R<Option<Value>> {
        let j = self.out.join("check.log");
        if self.route() == "box64" {
            let root = self.root.clone();
            return self.step("route", |g| {
                let (rc, rep) = g.pmu_json(&["route".into(), root, "--json".into()], &j);
                match rep {
                    Some(r) if rc == 0 => {
                        g.values.insert("route".into(), checks::route_verdict(&r));
                        Ok(None)
                    }
                    _ => fail(format!("pmu route failed: {}", tail(&j, 4))),
                }
            });
        }
        let root = self.root.clone();
        self.step("port check", |g| {
            let mut args = vec!["port".to_string(), "check".into(), "--json".into()];
            if let Some(a) = &g.ctx.asmpatch {
                args.push("--asmpatch".into());
                args.push(a.clone());
            }
            args.push(root);
            let (rc, rep) = g.pmu_json(&args, &j);
            let Some(rep) = rep.filter(|_| rc == 0 || rc == 1) else {
                return fail(format!("port check failed (exit {rc}): {}", tail(&j, 4)));
            };
            let mut v = checks::check_verdict(&rep);
            v["exit"] = json!(rc);
            g.values.insert("check".into(), v);
            let uv = s(&rep, "unity_version");
            if let Some(want) = g.g.get("unity").map(show) {
                if !uv.is_empty() && uv != want {
                    g.checks.push(("unity".into(), false, format!("data is Unity {uv}, the list says {want}")));
                }
            }
            if !uv.is_empty() {
                g.version = uv;
            }
            Ok(Some(rep))
        })
    }

    /// The data to convert: the game's own folder, or for a compressed build a view of it with the
    /// archive unpacked into the work folder.
    fn source(&mut self) -> R<String> {
        let d = Path::new(&self.data);
        if d.join("globalgamemanagers").is_file() {
            return Ok(self.data.clone());
        }
        if !d.join("data.unity3d").is_file() {
            return fail("no globalgamemanagers and no data.unity3d in the data folder");
        }
        self.step("unpack", |g| {
            let up = g.work.join("unpacked").to_string_lossy().into_owned();
            let log = g.log.clone();
            if g.pmu(&["unpack".into(), g.data.clone(), "--out".into(), up.clone()], &log, None, &[]) != 0 {
                return fail(format!("unpack failed: {}", tail(&log, 4)));
            }
            g.notes.push("compressed build: unpacked".into());
            view_dir(&g.data, &[&up], &g.work.join("src"))
        })
    }

    fn convert_config(&self, rep: Option<&Value>) -> Value {
        let mut cfg = json!({"steps": ["shaders", "settings"], "settings": {"graphics_apis": [11]}});
        let build = rep.map(|r| s(r, "build")).filter(|b| !b.is_empty()).or_else(|| self.g.get("build").map(show));
        if build.as_deref() == Some("windows") {
            cfg["shaders"] = json!({"from": ["d3d11"]});
        }
        // of what the check suggests only the shaders' source (metal, vulkan): its other steps change a
        // port's picture or memory, which a port opts into; a game of the list gets them through its own
        // convert table
        if let Some(sh) = rep.and_then(|r| r.get("suggest")).and_then(|x| x.get("convert")).and_then(|x| x.get("shaders")) {
            cfg = deep_merge(&cfg, &json!({"shaders": sh}));
        }
        if self.route() == "box64" {
            let p = format!("{}/tools/box64/gles.json", self.ctx.checkout);
            if let Some(mut b) = fs::read(&p).ok().and_then(|b| serde_json::from_slice::<Value>(&b).ok()) {
                if let Some(o) = b.as_object_mut() {
                    o.remove("comment");
                }
                cfg = deep_merge(&cfg, &b);
            }
        }
        cfg = deep_merge(&cfg, self.g.get("convert").unwrap_or(&json!({})));
        if cfg.get("version").is_none() {
            cfg["version"] = json!(1);
        }
        cfg
    }

    /// The shaders step: `pmu convert --steps shaders` into the overlay folder, its files digested;
    /// real = false: the overlay is removed afterwards (android-data does not read it).
    fn shaders(&mut self, src: &str, cfg: &Value, real: bool) -> R<String> {
        let cfgp = self.work.join("convert.json");
        fs::write(&cfgp, serde_json::to_string_pretty(cfg).unwrap()).map_err(|e| Stop::Fail(e.to_string()))?;
        let conv = self.work.join("conv").to_string_lossy().into_owned();
        let name = if real { "shaders" } else { "shaders (digested, not kept)" };
        let src = src.to_string();
        self.step(name, |g| {
            let clog = g.work.join("convert.log");
            let mut args = vec!["convert".into(), src.clone(), conv.clone(), "--config".into(), cfgp.to_string_lossy().into_owned(), "--steps".into(), "shaders".into()];
            args.extend(g.anyver());
            let rc = g.pmu(&args, &clog, Some(7200), &[]);
            copyfile(&clog, &g.out.join("convert-shaders.log"));
            if rc != 0 {
                return fail(format!("convert (shaders) failed: {}", tail(&clog, 4)));
            }
            let lines = read_lines(&clog);
            let d = digest::files(Path::new(&conv), &[]);
            let files = d["files"].as_object().cloned().unwrap_or_default();
            g.values.insert("shaders".into(), checks::shader_counts(&lines));
            let per: Map<String, Value> = files.iter().map(|(k, f)| (hide_name(k), json!(f["file"].as_str().unwrap_or("").chars().take(16).collect::<String>()))).collect();
            let sum = |k: &str| files.values().map(|f| f[k].as_i64().unwrap_or(0)).sum::<i64>();
            g.values.insert(
                "shaders_digest".into(),
                json!({"total": d["total"], "files": files.len(), "shaders": sum("shaders"), "compute": sum("compute"), "per_file": per}),
            );
            g.names = files.keys().map(|k| (hide_name(k), k.clone())).collect();
            if !real {
                // (the converted files are digested; what the next steps read is the game's own data)
                rmtree(Path::new(&conv));
            }
            Ok(conv.clone())
        })
    }

    /// The other convert steps (settings, ...) into the overlay; their written files' digests.
    fn settings(&mut self, src: &str, conv: &str, cfg: &Value) -> R<()> {
        let steps: Vec<String> = strs(cfg, "steps").into_iter().filter(|x| x != "shaders").collect();
        if steps.is_empty() {
            return Ok(());
        }
        let before = if Path::new(conv).is_dir() { digest::tree(Path::new(conv), &[], None)["files"].clone() } else { json!({}) };
        let (src, conv) = (src.to_string(), conv.to_string());
        self.step(&format!("convert {}", steps.join(",")), |g| {
            let clog = g.work.join("convert2.log");
            let mut args = vec!["convert".into(), src.clone(), conv.clone(), "--config".into(), g.work.join("convert.json").to_string_lossy().into_owned(), "--steps".into(), steps.join(",")];
            args.extend(g.anyver());
            let rc = g.pmu(&args, &clog, Some(7200), &[]);
            copyfile(&clog, &g.out.join("convert-steps.log"));
            if rc != 0 {
                return fail(format!("convert ({}) failed: {}", steps.join(","), tail(&clog, 4)));
            }
            let after = digest::tree(Path::new(&conv), &[], None)["files"].clone();
            let mut written = BTreeMap::new();
            for (k, v) in after.as_object().into_iter().flatten() {
                if before.get(k) != Some(v) {
                    written.insert(hide_name(k), json!(v[1].as_str().unwrap_or("").chars().take(16).collect::<String>()));
                }
            }
            g.values.insert("steps".into(), json!({"steps": steps, "files": written.len(), "per_file": written}));
            Ok(())
        })
    }

    /// Unity's Android player of that version: a `pmu player fetch` folder in one of the local
    /// config's player folders (<dir>/<version>/pkg or <dir>/<version>), else fetched into <scratch>/players.
    fn player_pkg(&mut self, version: &str) -> R<String> {
        let ctx = self.ctx;
        for d in ctx.players.iter().chain(std::iter::once(&ctx.fetch_players)) {
            for p in [format!("{d}/{version}/pkg"), format!("{d}/{version}")] {
                if Path::new(&p).join("player.json").is_file() {
                    return Ok(p);
                }
            }
        }
        if ctx.a.offline {
            return skip(format!("no Android player {version} here (--offline: not fetched)"));
        }
        let p = format!("{}/{version}/pkg", ctx.fetch_players);
        let log = self.log.clone();
        let rc = self.pmu(&["player".into(), "fetch".into(), version.into(), p.clone()], &log, Some(1800), &[]);
        if rc != 0 || !Path::new(&p).join("player.json").is_file() {
            return fail(format!("player fetch {version} failed: {}", tail(&log, 2)));
        }
        Ok(p)
    }

    /// android-data on the converted data (a view: the data's files + the overlay's).
    fn android_data(&mut self, src: &str, conv: &str, rep: Option<&Value>, pack: &str, il2cpp: Option<&str>, pkg: Option<String>) -> R<String> {
        let pkg = match pkg {
            Some(p) => p,
            None => {
                let v = self.version.clone();
                self.player_pkg(&v)?
            }
        };
        let view = view_dir(src, &[conv], &self.work.join("view"))?;
        let out = self.work.join("android").to_string_lossy().into_owned();
        let slug: String = self.slug.chars().filter(|c| c.is_ascii_lowercase() || c.is_ascii_digit()).collect();
        let mut args: Vec<String> = ["android-data", "--src", &view, "--pkg", &pkg, "--out", &out, "--name", "Game", "--package", &format!("com.regress.{slug}"), "--pack", if pack == "tree" { "none" } else { pack }, "--force"]
            .iter()
            .map(|x| x.to_string())
            .collect();
        let sug = rep.and_then(|r| r.get("suggest")).and_then(|x| x.get("android_data")).cloned().unwrap_or(json!({}));
        let sam = self.g.get("streaming_assets").or(sug.get("streaming_assets")).filter(|v| truthy(v)).map(show);
        if let Some(sm) = &sam {
            args.extend(["--streaming-assets".into(), sm.clone()]);
        }
        let asm = self.g.get("asmpatch").or(sug.get("asmpatch")).cloned().unwrap_or(json!("off"));
        match il2cpp {
            None => {
                let off = asm.is_null() || asm == json!("off") || asm == json!(false);
                if !off {
                    let Some(a) = &self.ctx.asmpatch else {
                        return skip("the check suggests asmpatch: no asmpatch (tools/asmpatch build, asmpatch in the local config)");
                    };
                    let cmd = if asm == json!("auto") || asm == json!(true) {
                        a.clone()
                    } else {
                        let only: Vec<String> = asm.as_array().map_or(vec![show(&asm)], |x| x.iter().map(show).collect());
                        format!("{a} --only={}", only.join(","))
                    };
                    args.extend(["--asmpatch".into(), cmd]);
                } else if sam.as_deref().is_some_and(|x| x != "obb") {
                    if let Some(a) = &self.ctx.asmpatch {
                        args.extend(["--asmpatch".into(), format!("{a} --only=")]);
                    }
                }
            }
            Some(i) => args.extend(["--il2cpp".into(), i.to_string()]),
        }
        if let Some(etc) = mono_etc(&self.data) {
            let d = Path::new(&self.data);
            if !d.join("MonoBleedingEdge/etc").is_dir() && !d.join("Mono/etc").is_dir() {
                args.extend(["--mono-etc".into(), etc]);
            }
        }
        if rep.is_some_and(|r| s(r, "build") == "windows") && il2cpp.is_none() {
            args.extend(self.winbcl(&view, &pkg)?);
        }
        args.extend(strs(&self.g, "android_data"));
        let out2 = out.clone();
        self.step(&format!("android-data --pack {pack}"), |g| {
            let alog = g.work.join("pack.log");
            let rc = g.pmu(&args, &alog, Some(7200), &[]);
            copyfile(&alog, &g.out.join("android-data.log"));
            if rc != 0 {
                return fail(format!("android-data failed (exit {rc}): {}", tail(&alog, 4)));
            }
            let report: Value = fs::read(Path::new(&out2).join("android_data_report.json"))
                .ok()
                .and_then(|b| serde_json::from_slice(&b).ok())
                .ok_or(Stop::Fail("android-data wrote no report".into()))?;
            let mut p = checks::pack_summary(&report, &read_lines(&alog));
            let skip_globs = ["android_data_report.json".to_string(), "make_android_data.log".into(), "*.log".into()];
            let t = digest::tree(Path::new(&out2), &skip_globs, Some(&Path::new(&out2).join(".android_data_state.json")));
            let zips: Map<String, Value> = t["zips"]
                .as_object()
                .into_iter()
                .flatten()
                .map(|(k, z)| (k.clone(), json!({"members": z["members"].as_str().unwrap_or("").chars().take(16).collect::<String>(), "entries": z["entries"]})))
                .collect();
            p["tree"] = json!({"total": t["total"], "files": t["count"], "mb": t["bytes"].as_u64().unwrap_or(0) >> 20, "zips": zips});
            g.values.insert("pack".into(), p);
            Ok(())
        })?;
        Ok(out)
    }

    /// A Windows build's Unix class libraries (pmu winbcl: range reads from Unity's installer), cached in
    /// <scratch>/cache/winbcl/<slug> once read.
    fn winbcl(&mut self, data: &str, pkg: &str) -> R<Vec<String>> {
        let cache = format!("{}/cache/winbcl/{}", self.ctx.scratch, self.slug);
        let opts = format!("{cache}/options.txt");
        if !Path::new(&opts).is_file() {
            if self.ctx.a.offline {
                return skip("Windows build: winbcl not cached (--offline)");
            }
            let (data, pkg) = (data.to_string(), pkg.to_string());
            self.step("winbcl", |g| {
                let _ = fs::create_dir_all(&cache);
                let o = Command::new(&g.ctx.pmu)
                    .args(["winbcl", &data, &format!("{cache}/bcl"), "--player", &pkg])
                    .envs(g.ctx.env.iter().map(|(k, v)| (k, v)))
                    .output()
                    .map_err(|e| Stop::Fail(e.to_string()))?;
                let err = String::from_utf8_lossy(&o.stderr).into_owned();
                append(&g.log, &err[err.len().saturating_sub(4000)..]);
                if !o.status.success() {
                    return fail(format!("winbcl failed: {}", err.trim().chars().rev().take(300).collect::<Vec<_>>().into_iter().rev().collect::<String>()));
                }
                fs::write(&opts, String::from_utf8_lossy(&o.stdout).trim()).map_err(|e| Stop::Fail(e.to_string()))
            })?;
        }
        Ok(fs::read_to_string(&opts).unwrap_or_default().split_whitespace().map(String::from).collect())
    }

    /// The emulated device root: lib/ with the player (and only the Mono runtime the data needs, as
    /// an Android build ships one), data/ for saves.
    fn emu_root(&self, pkg: &str, backend: &str, abi: &str, runtime: &str, il2cpp_lib: Option<&str>) -> R<PathBuf> {
        let root = self.work.join("emu");
        rmtree(&root);
        let lib = root.join("lib");
        fs::create_dir_all(&lib).map_err(|e| Stop::Fail(e.to_string()))?;
        let copy_so = |dir: &Path, except: &str| {
            for e in fs::read_dir(dir).into_iter().flatten().filter_map(|e| e.ok()) {
                let n = e.file_name().to_string_lossy().into_owned();
                if n.ends_with(".so") && n != except {
                    let _ = fs::copy(e.path(), lib.join(&n));
                }
            }
        };
        copy_so(&Path::new(pkg).join(LIBS.replace("{backend}", backend).replace("{abi}", abi)), "");
        if backend == "mono" {
            let other = if runtime.contains("bdwgc") { "libmono.so" } else { "libmonobdwgc-2.0.so" };
            copy_so(&Path::new(pkg).join(MONOLIBS), other);
        }
        if let Some(l) = il2cpp_lib {
            let _ = fs::copy(l, lib.join("libil2cpp.so"));
        }
        Ok(root)
    }

    /// tools/emu/emu.sh (on the computer: Docker) with screenshots at the list's times, counted from
    /// ua_host's first log line.
    fn rig_run(&mut self, root: &Path, zipdir: &str, arch: &str, env_extra: &Map<String, Value>) -> R<()> {
        let ctx = self.ctx;
        let r = self.g.get("rig").cloned().unwrap_or(json!({}));
        let secs = r.get("seconds").and_then(Value::as_i64).unwrap_or(300);
        let mut shots: Vec<i64> = r.get("shots").and_then(Value::as_array).map_or(vec![secs / 4, secs / 2, secs * 9 / 10], |a| a.iter().filter_map(Value::as_i64).collect());
        shots.sort();
        let hostbin = if arch == "arm64" { &ctx.hostbin64 } else { &ctx.hostbin };
        if !Path::new(hostbin).join("ua_host").is_file() {
            return skip(format!("no ua_host in {hostbin} (build the runtime, or hostbin in the local config)"));
        }
        let mut env: Vec<(String, String)> = vec![
            ("EMU_ROOT".into(), root.to_string_lossy().into_owned()),
            ("EMU_ZIP".into(), zipdir.into()),
            ("EMU_ARCH".into(), arch.into()),
            ("EMU_HOSTBIN".into(), hostbin.clone()),
            ("EMU_NAME".into(), format!("regress-{}", self.id)),
        ];
        if let Some(i) = self.g.get("input") {
            env.push(("UA_INPUT_SCRIPT".into(), show(i)));
        }
        for (k, v) in env_extra {
            env.push((k.clone(), show(v)));
        }
        // the feature probe (Mono games; uaprobe.dll beside ua_host), halfway through the run at most a
        // minute in: its failures go into the baseline (checks::compare_rig)
        if !env.iter().any(|(k, _)| k == "UA_PROBE") {
            env.push(("UA_PROBE".into(), (secs / 2).clamp(10, 60).to_string()));
        }
        let mut prefixes: Vec<String> = env_extra
            .keys()
            .filter(|k| !k.starts_with("UA_") && !k.starts_with("GC_") && !k.starts_with("ANDROID_"))
            .map(|k| format!("{}_", k.split('_').next().unwrap_or(k)))
            .collect();
        prefixes.sort();
        prefixes.dedup();
        if !prefixes.is_empty() {
            env.push(("EMU_ENV".into(), prefixes.join(" ")));
        }
        if fs::canonicalize(root).ok() == fs::canonicalize(zipdir).ok() {
            return Err(Stop::Setup("EMU_ROOT and EMU_ZIP are the same folder".into()));
        }
        let runlog = root.join("run.log");
        docker_gate(&self.log);
        let others = docker_names().iter().filter(|n| !n.starts_with("regress")).count();
        if others != 0 {
            self.notes.push(format!("{others} other container{} running at the start", if others > 1 { "s" } else { "" }));
        }
        let emu_out = self.work.join("emu.out");
        let rc_file = self.work.join("emu.rc");
        let mut taken: Vec<i64> = Vec::new();
        let id = self.id.clone();
        self.step(&format!("rig {arch} {secs} s"), |g| {
            let shown: Vec<String> = env.iter().map(|(k, v)| format!("{k}={v}")).collect();
            append(&g.log, &format!("env: {}\n", shown.join(" ")));
            // (started detached on the computer: its end is emu.rc)
            let script = format!(
                "cd {} && bash tools/emu/emu.sh run.log {secs} release > {} 2>&1; echo $? > {}",
                shq(&ctx.checkout),
                shq(&emu_out.to_string_lossy()),
                shq(&rc_file.to_string_lossy())
            );
            let mut c = Command::new("/bin/sh");
            c.arg("-c").arg(format!("nohup sh -c {} > /dev/null 2>&1 &", shq(&script))).envs(env.iter().map(|(k, v)| (k, v)));
            pkgzip::host::status(&mut c).map_err(|e| Stop::Fail(format!("the rig did not start: {e}")))?;
            let done = || rc_file.is_file();
            let deadline = Instant::now() + Duration::from_secs(secs as u64 + 1800);
            let mut t0 = None;
            while !done() && Instant::now() < deadline {
                if fs::metadata(&runlog).is_ok_and(|m| m.len() > 0) {
                    t0 = Some(Instant::now());
                    break;
                }
                std::thread::sleep(Duration::from_secs(2));
            }
            let req = root.join("shot.req");
            for t in &shots {
                while !done() && t0.is_some_and(|t0| t0.elapsed().as_secs() < *t as u64) {
                    std::thread::sleep(Duration::from_secs(1));
                }
                if done() {
                    break;
                }
                let _ = fs::write(&req, "");
                taken.push(*t);
                // ua_host takes it with the next frame (and removes the request): a slow frame delays
                // the shot, never merges it with the next one
                while !done() && req.exists() && t0.is_some_and(|t0| t0.elapsed().as_secs() < secs as u64) {
                    std::thread::sleep(Duration::from_secs(1));
                }
            }
            let end = Instant::now() + Duration::from_secs((secs as u64 + 600).max(60));
            while !done() && Instant::now() < end {
                std::thread::sleep(Duration::from_secs(2));
            }
            if !done() {
                let _ = pkgzip::host::status(Command::new("docker").args(["kill", &format!("regress-{id}")]));
                g.notes.push("the rig did not stop by itself: killed".into());
            }
            Ok(())
        })?;
        let lines = read_lines(&runlog);
        if emu_out.is_file() {
            copyfile(&emu_out, &self.out.join("emu.out"));
        }
        if !runlog.is_file() {
            let t = tail(&emu_out, 4);
            return fail(format!("the rig did not start the game (no run log): {}", if t.is_empty() { "no output".into() } else { t }));
        }
        copyfile(&runlog, &self.out.join("emu-run.log"));
        let runv = checks::run_log(&lines);
        let files = crate::media::shots(root, ".ppm");
        let mut stats = Vec::new();
        for (t, f) in taken.iter().zip(&files) {
            let mut st = match crate::shaders::read_image(f) {
                Ok((w, h, rgb)) => checks::picture_stats(w, h, &rgb, 4),
                Err(_) => json!({"content": false, "dark": 1.0, "colours": 0, "top": 1.0}),
            };
            st["t"] = json!(t);
            stats.push(st);
        }
        for t in taken.iter().skip(files.len()) {
            stats.push(json!({"t": t, "content": false, "dark": 1.0, "colours": 0, "top": 1.0, "missing": true}));
        }
        // the shots as PNGs in the results folder (for a look; never in the repository)
        for f in &files {
            if let Ok((w, h, px)) = crate::shaders::read_image(f) {
                let n = f.file_name().unwrap().to_string_lossy().replace(".ppm", ".png");
                let _ = crate::media::write_png(&self.out.join(n), w, h, &px);
            }
        }
        let mut run = Map::new();
        for k in ["frames", "crash", "exit", "fatal", "exceptions", "fps_lines"] {
            run.insert(k.into(), runv[k].clone());
        }
        // (the probe too: compare_rig reads it from the baseline)
        run.insert("probe".into(), runv["probe"].clone());
        self.values.insert("run".into(), Value::Object(run));
        self.values.insert("shots".into(), json!(stats));
        self.checks.extend(checks::expectations_rig(&r, &runv, &stats, &lines.join("\n")));
        Ok(())
    }
}

fn suggest_env(g: &Game, rep: Option<&Value>) -> Map<String, Value> {
    let mut env = if g.g.get("suggested_env").is_none_or(truthy) {
        rep.and_then(|r| r.get("suggest")).and_then(|x| x.get("runtime")).and_then(Value::as_object).cloned().unwrap_or_default()
    } else {
        Map::new()
    };
    for (k, v) in g.g.get("env").and_then(Value::as_object).into_iter().flatten() {
        env.insert(k.clone(), v.clone());
    }
    env
}

// ---- per route

fn fast_android(g: &mut Game) -> R<()> {
    let rep = g.check()?;
    let src = g.source()?;
    let cfg = g.convert_config(rep.as_ref());
    let pack = g.ctx.a.pack.clone().unwrap_or_else(|| g.g.get("pack").map_or("none".into(), show));
    let conv = g.shaders(&src, &cfg, pack != "none")?;
    g.settings(&src, &conv, &cfg)?;
    if pack != "none" {
        g.android_data(&src, &conv, rep.as_ref(), &pack, None, None)?;
    }
    Ok(())
}

fn fast_box64(g: &mut Game) -> R<()> {
    g.check()?;
    let src = g.source()?;
    let cfg = g.convert_config(None);
    let conv = g.shaders(&src, &cfg, false)?;
    g.settings(&src, &conv, &cfg)
}

fn fast_shaders(g: &mut Game) -> R<()> {
    let src = g.source()?;
    let cfg = g.convert_config(None);
    let conv = g.shaders(&src, &cfg, false)?;
    g.settings(&src, &conv, &cfg)
}

fn apk_inputs(g: &Game) -> R<Vec<String>> {
    let mut ins: Vec<String> = fs::read_dir(&g.root)
        .into_iter()
        .flatten()
        .filter_map(|e| e.ok())
        .map(|e| e.path().to_string_lossy().into_owned())
        .filter(|f| [".apk", ".obb", ".xapk", ".apks", ".aab"].iter().any(|x| f.ends_with(x)))
        .collect();
    ins.sort();
    if ins.is_empty() {
        return skip("no APK in the game folder");
    }
    Ok(ins)
}

fn fast_apk(g: &mut Game) -> R<(String, Value)> {
    let ins = apk_inputs(g)?;
    let a = g.g.get("apk").cloned().unwrap_or(json!({}));
    let out = g.work.join("zip").to_string_lossy().into_owned();
    let out2 = out.clone();
    let rep = g.step("pmu apk", |g| {
        let j = g.out.join("apk.log");
        let mut args = vec!["apk".to_string()];
        args.extend(ins);
        args.extend([out2.clone(), "--json".into(), "--target".into(), a.get("target").map_or("auto".into(), show)]);
        args.extend(strs(&a, "args"));
        let (rc, rep) = g.pmu_json(&args, &j);
        if rc != 0 {
            return fail(format!("pmu apk failed (exit {rc}): {}", tail(&j, 4)));
        }
        let rep = rep.unwrap_or(json!({}));
        let t = digest::tree(Path::new(&out2), &["*.log".to_string()], None);
        let facts: Map<String, Value> = APK_FACTS.iter().filter_map(|k| rep.get(*k).map(|v| (k.to_string(), v.clone()))).collect();
        let zips: Map<String, Value> = t["zips"]
            .as_object()
            .into_iter()
            .flatten()
            .map(|(k, z)| (hide_name(k), json!({"members": z["members"].as_str().unwrap_or("").chars().take(16).collect::<String>(), "entries": z["entries"]})))
            .collect();
        g.values.insert(
            "apk".into(),
            json!({"facts": facts, "tree": {"total": t["total"], "files": t["count"], "mb": t["bytes"].as_u64().unwrap_or(0) >> 20, "zips": zips}}),
        );
        Ok(rep)
    })?;
    Ok((out, rep))
}

fn il2cpp_args(g: &Game, rep: Option<&Value>) -> R<Vec<String>> {
    let i = g.g.get("il2cpp").cloned().unwrap_or(json!({}));
    let mut args = vec!["--abi".to_string(), i.get("abi").map_or("arm64-v8a".into(), show)];
    let sug = rep.and_then(|r| r.get("suggest")).and_then(|x| x.get("android_data")).cloned().unwrap_or(json!({}));
    let asm = g.g.get("asmpatch").or(sug.get("asmpatch")).cloned().unwrap_or(json!("off"));
    let sam = g.g.get("streaming_assets").or(sug.get("streaming_assets")).filter(|v| truthy(v)).map(show);
    let off = asm.is_null() || asm == json!("off") || asm == json!(false);
    if let (false, Some(a)) = (off, &g.ctx.asmpatch) {
        let cmd = if asm == json!("auto") || asm == json!(true) {
            a.clone()
        } else {
            format!("{a} --only={}", asm.as_array().map_or(show(&asm), |x| x.iter().map(show).collect::<Vec<_>>().join(",")))
        };
        args.extend(["--asmpatch".into(), cmd]);
    } else if let (Some(sm), Some(a)) = (&sam, &g.ctx.asmpatch) {
        if sm != "obb" {
            args.extend(["--asmpatch".into(), format!("{a} --only=")]); // (the streaming-assets patch alone)
        }
    }
    if let Some(sm) = sam.filter(|x| x != "obb") {
        if g.ctx.asmpatch.is_none() {
            return skip(format!("streaming_assets {sm} on the IL2CPP route needs asmpatch (none here)"));
        }
        args.extend(["--streaming-assets".into(), sm]);
    }
    args.extend(strs(&i, "args"));
    Ok(args)
}

/// pmu il2cpp --on-device --dry-run: the tools fetched, the assemblies staged, the commands it would
/// run (machine paths replaced): what changes when the IL2CPP route's staging or options change.
fn fast_il2cpp(g: &mut Game) -> R<()> {
    let Some(home) = g.ctx.il2cpp_home.clone() else { return skip("no il2cpp_home in the local config") };
    let rep = g.check()?;
    let src = g.source()?;
    let out = g.work.join("il2cpp").to_string_lossy().into_owned();
    let extra = il2cpp_args(g, rep.as_ref())?;
    g.step("il2cpp --dry-run", |g| {
        let dlog = g.work.join("dry.log");
        let mut args = vec!["il2cpp".to_string(), src.clone(), out.clone(), "--dry-run".into(), "--on-device".into(), "--home".into(), home.clone()];
        args.extend(extra);
        let rc = g.pmu(&args, &dlog, Some(3600), &[]);
        copyfile(&dlog, &g.out.join("il2cpp-dry-run.log"));
        if rc != 0 {
            return fail(format!("il2cpp --dry-run failed (exit {rc}): {}", tail(&dlog, 4)));
        }
        let text = String::from_utf8_lossy(&fs::read(&dlog).unwrap_or_default()).replace(&g.work.to_string_lossy().into_owned(), "<work>");
        let text = hide_paths(&text, &g.ctx.paths_to_hide());
        // what it decided and would run; not what it fetched first (a first run fetches the tools)
        let line = Regex::new(r"^((data|assemblies|asmpatch|UnityLinker|il2cpp|mix\w*|bee\w*|Bee\w*): |\S+\.(h|c|cpp): )").unwrap();
        let cmds: Vec<&str> = text.lines().skip(1).filter(|l| line.is_match(l)).collect();
        let _ = fs::write(g.out.join("il2cpp-dry-run.log"), &text);
        let staged = Path::new(&out).join(".work/Managed-in");
        let st = if staged.is_dir() { digest::tree(&staged, &[], None) } else { json!({"total": null, "count": 0}) };
        use sha2::Digest;
        let head = cmds.first().copied().unwrap_or("");
        g.values.insert(
            "il2cpp_dry_run".into(),
            json!({
                "summary": head.strip_prefix("data: ").unwrap_or(head).chars().take(200).collect::<String>(),
                "assemblies": cmds.iter().find(|l| l.starts_with("assemblies:")),
                "commands": &digest::hex(&sha2::Sha256::digest(cmds.join("\n").as_bytes()))[..16],
                "lines": cmds.len(),
                "staged": {"total": st["total"].as_str().unwrap_or("").chars().take(16).collect::<String>(), "files": st["count"]},
            }),
        );
        Ok(())
    })
}

fn rig_android(g: &mut Game) -> R<()> {
    let rep = g.check()?;
    let src = g.source()?;
    let cfg = g.convert_config(rep.as_ref());
    let conv = g.shaders(&src, &cfg, true)?;
    g.settings(&src, &conv, &cfg)?;
    let v = g.version.clone();
    let pkg = g.player_pkg(&v)?;
    let out = g.android_data(&src, &conv, rep.as_ref(), "all", None, Some(pkg.clone()))?;
    rmtree(&Path::new(&out).join("Game")); // the zip folder has it all now
    rmtree(&g.work.join("view"));
    let runtime = g.values.get("pack").map(|p| s(p, "scripting_runtime")).unwrap_or_default();
    let root = g.emu_root(&pkg, "mono", "armeabi-v7a", &runtime, None)?;
    let env = suggest_env(g, rep.as_ref());
    g.rig_run(&root, &format!("{out}/zip"), "armhf", &env)
}

fn rig_apk(g: &mut Game) -> R<()> {
    let a = g.g.get("apk").cloned().unwrap_or(json!({}));
    let default = if a.get("target").map_or("arm64".into(), show) != "armhf" { "arm64" } else { "armhf" };
    let arch = g.g.get("rig").and_then(|r| r.get("arch")).map_or(default.to_string(), show);
    let (zipdir, _) = fast_apk(g)?;
    let root = g.work.join("emu");
    rmtree(&root);
    let _ = fs::create_dir_all(&root);
    let env = g.g.get("env").and_then(Value::as_object).cloned().unwrap_or_default();
    g.rig_run(&root, &zipdir, &arch, &env)
}

/// The whole IL2CPP route on the rig: pmu il2cpp --on-device (in this container: the handheld's build,
/// natively), convert, android-data --il2cpp, the arm64 rig.
fn rig_il2cpp(g: &mut Game) -> R<()> {
    let Some(home) = g.ctx.il2cpp_home.clone() else { return skip("no il2cpp_home in the local config") };
    let rep = g.check()?;
    let src = g.source()?;
    let abi = g.g.get("il2cpp").and_then(|i| i.get("abi")).map_or("arm64-v8a".into(), show);
    let out = g.work.join("il2cpp").to_string_lossy().into_owned();
    let extra = il2cpp_args(g, rep.as_ref())?;
    let lib = format!("{out}/lib/{abi}/libil2cpp.so");
    let lib2 = lib.clone();
    g.step("il2cpp build", |g| {
        let blog = g.work.join("il2cpp.log");
        let mut args = vec!["il2cpp".to_string(), src.clone(), out.clone(), "--on-device".into(), "--home".into(), home.clone()];
        args.extend(extra);
        let rc = g.pmu(&args, &blog, Some(3 * 3600), &[]);
        copyfile(&blog, &g.out.join("il2cpp.log"));
        if rc != 0 {
            return fail(format!("pmu il2cpp failed (exit {rc}): {}", tail(&blog, 4)));
        }
        let Ok(md) = fs::metadata(&lib2) else { return fail("pmu il2cpp wrote no libil2cpp.so") };
        let mv = Regex::new(r"metadata version (\d+)").unwrap();
        let meta = read_lines(&blog).iter().find_map(|l| mv.captures(l).map(|c| c[1].to_string()));
        g.values.insert("il2cpp".into(), json!({"libil2cpp_mb": (md.len() as f64 / 1048576.0 * 10.0).round() / 10.0, "metadata": meta}));
        Ok(())
    })?;
    let cfg = g.convert_config(rep.as_ref());
    let conv = g.shaders(&src, &cfg, true)?;
    g.settings(&src, &conv, &cfg)?;
    let pkg = format!("{home}/{}/player-{abi}", g.version);
    if !Path::new(&pkg).join("player.json").is_file() {
        return skip(format!("no IL2CPP player in {pkg}"));
    }
    let adir = g.android_data(&src, &conv, rep.as_ref(), "all", Some(&out), Some(pkg.clone()))?;
    rmtree(&Path::new(&adir).join("Game"));
    rmtree(&g.work.join("view"));
    let root = g.emu_root(&pkg, "il2cpp", &abi, "", Some(&lib))?;
    let env = suggest_env(g, rep.as_ref());
    g.rig_run(&root, &format!("{adir}/zip"), if abi == "arm64-v8a" { "arm64" } else { "armhf" }, &env)
}

/// A game packaged before (its zip folder and lib/ from the local config's [prebuilt.<slug>]): the
/// runtime under test on the same data.
fn rig_prebuilt(g: &mut Game) -> R<()> {
    let Some(pb) = g.ctx.prebuilt.get(&g.slug).cloned() else { return skip(format!("no [prebuilt.{}] in the local config", g.slug)) };
    let (zipdir, lib) = (s(&pb, "zip"), s(&pb, "lib"));
    if !Path::new(&zipdir).is_dir() || !Path::new(&lib).is_dir() {
        return skip(format!("[prebuilt.{}]: zip or lib folder missing", g.slug));
    }
    let root = g.work.join("emu");
    rmtree(&root);
    let st = Command::new("cp").args(["-R", &lib, &root.join("lib").to_string_lossy()]).status();
    if !st.is_ok_and(|s| s.success()) {
        let _ = fs::create_dir_all(root.join("lib"));
    }
    for f in strs(&pb, "root_files") {
        let _ = fs::copy(&f, root.join(Path::new(&f).file_name().unwrap_or_default()));
    }
    let env = g.g.get("env").and_then(Value::as_object).cloned().unwrap_or_default();
    let arch = pb.get("arch").map_or("armhf".into(), show);
    g.rig_run(&root, &zipdir, &arch, &env)
}

fn rig_box64(g: &mut Game) -> R<()> {
    let b = g.ctx.local.get("box64_bin").map(show);
    if !b.is_some_and(|b| Path::new(&b).join("box64/box64").is_file()) {
        return skip("no box64 build (tools/box64/build.sh; box64_bin in the local config)");
    }
    skip("box64 rig runs are not wired into tools/regress yet (tools/box64/rig.sh)")
}

// ---- the run

fn baseline_path(level: &str) -> PathBuf {
    here().join("baseline").join(format!("{level}.json"))
}

fn load_baseline(level: &str) -> Map<String, Value> {
    fs::read(baseline_path(level)).ok().and_then(|b| serde_json::from_slice::<Value>(&b).ok()).and_then(|v| v.as_object().cloned()).unwrap_or_default()
}

/// json.dumps(..., indent=1, sort_keys=True) + "\n"
fn sorted(v: &Value) -> Value {
    match v {
        Value::Object(o) => {
            let b: BTreeMap<&String, Value> = o.iter().map(|(k, v)| (k, sorted(v))).collect();
            serde_json::to_value(b).unwrap()
        }
        Value::Array(a) => Value::Array(a.iter().map(sorted).collect()),
        x => x.clone(),
    }
}

fn dump(v: &Value) -> String {
    let mut buf = Vec::new();
    let mut ser = serde_json::Serializer::with_formatter(&mut buf, serde_json::ser::PrettyFormatter::with_indent(b" "));
    serde::Serialize::serialize(&sorted(v), &mut ser).unwrap();
    String::from_utf8(buf).unwrap()
}

fn save_baseline(level: &str, data: &Map<String, Value>) {
    let p = baseline_path(level);
    let _ = fs::create_dir_all(p.parent().unwrap());
    let _ = fs::write(&p, dump(&Value::Object(data.clone())) + "\n");
}

fn short(v: &Value) -> String {
    let s = match v {
        Value::String(s) => s.clone(),
        o => o.to_string(),
    };
    if s.chars().count() <= 40 {
        s
    } else {
        s.chars().take(37).collect::<String>() + "..."
    }
}

/// The run's values against the baseline: the fast level exactly (counts and digests of the tools'
/// output), the rig with tolerance (checks::compare_rig).
fn compare(g: &mut Game, golden: Option<&Value>) {
    let Some(golden) = golden.filter(|x| truthy(x)) else {
        g.notes.push("no baseline yet".into());
        return;
    };
    let values = Value::Object(g.values.clone());
    if g.level == "fast" {
        let diffs = checks::compare_exact(golden, &values);
        if diffs.is_empty() {
            g.checks.push(("baseline".into(), true, "same as the baseline".into()));
        } else {
            let mut shown = Vec::new();
            for (k, a, b) in diffs.iter().take(6) {
                if k.ends_with("per_file") {
                    let (c, add, rem, names) = checks::changed_files(a, b);
                    let real: Vec<String> = names.iter().take(4).map(|n| g.names.get(n).cloned().unwrap_or(n.clone())).collect();
                    shown.push(format!("{k}: {c} changed, {add} new, {rem} gone ({})", real.join(", ")));
                } else {
                    shown.push(format!("{k}: {} -> {}", short(a), short(b)));
                }
            }
            g.checks.push(("baseline".into(), false, format!("{} values differ from the baseline: {}", diffs.len(), shown.join("; "))));
        }
    } else {
        let shots: Vec<Value> = g.values.get("shots").and_then(Value::as_array).cloned().unwrap_or_default();
        let run = g.values.get("run").cloned().unwrap_or(json!({}));
        g.checks.extend(checks::compare_rig(golden, &run, &shots, 0.3));
        for part in ["pack", "shaders_digest", "apk"] {
            if let (Some(a), Some(b)) = (golden.get(part), g.values.get(part)) {
                let d = checks::compare_exact(a, b);
                let detail = if d.is_empty() {
                    "same as the baseline".to_string()
                } else {
                    format!("{} values differ: {}", d.len(), d.iter().take(4).map(|(k, a, b)| format!("{k}: {} -> {}", short(a), short(b))).collect::<Vec<_>>().join("; "))
                };
                g.checks.push((format!("baseline:{part}"), d.is_empty(), detail));
            }
        }
    }
}

fn run_game(ctx: &Ctx, gv: &Value, level: &str, golden: Option<&Value>) -> Value {
    let mut g = Game::new(ctx, gv, level);
    rmtree(&g.work);
    let _ = fs::create_dir_all(&g.work);
    let _ = fs::create_dir_all(&g.out);
    let t0 = Instant::now();
    let (mut status, mut reason, mut step_failed) = ("pass".to_string(), String::new(), false);
    let r: R<()> = (|| {
        if !strs(gv, "levels").contains(&level.to_string()) {
            return skip(format!("not in the {level} level"));
        }
        let route = g.route();
        g.locate()?;
        if level == "fast" {
            match route.as_str() {
                "android" => fast_android(&mut g)?,
                "box64" => fast_box64(&mut g)?,
                "shaders" => fast_shaders(&mut g)?,
                "apk" => {
                    fast_apk(&mut g)?;
                }
                "il2cpp" => fast_il2cpp(&mut g)?,
                r => return skip(format!("route {r} has no {level} level")),
            }
            let exp = gv.get("expect").cloned().unwrap_or(json!({}));
            let c = checks::expectations_fast(&exp, &Value::Object(g.values.clone()));
            g.checks.extend(c);
        } else {
            match route.as_str() {
                "android" => rig_android(&mut g)?,
                "apk" => rig_apk(&mut g)?,
                "il2cpp" => rig_il2cpp(&mut g)?,
                "prebuilt" => rig_prebuilt(&mut g)?,
                "box64" => rig_box64(&mut g)?,
                r => return skip(format!("route {r} has no {level} level")),
            }
        }
        compare(&mut g, golden);
        if ctx.a.update {
            // (the baseline is being recorded: its differences are news, not faults)
            let (base, rest): (Vec<Check>, Vec<Check>) = g.checks.drain(..).partition(|c| c.0.starts_with("baseline") && !c.1);
            g.checks = rest;
            for c in base {
                g.notes.push(format!("baseline updated: {}", c.2));
            }
        }
        Ok(())
    })();
    match r {
        Ok(()) => {
            let bad: Vec<&Check> = g.checks.iter().filter(|c| !c.1).collect();
            if !bad.is_empty() {
                status = "fail".into();
                reason = bad.iter().map(|c| c.2.clone()).collect::<Vec<_>>().join("; ");
            }
        }
        Err(Stop::Skip(m)) => {
            status = "skip".into();
            reason = m;
        }
        Err(Stop::Fail(m)) | Err(Stop::Setup(m)) => {
            status = "fail".into();
            reason = m;
            step_failed = true;
        }
    }
    if !ctx.a.keep {
        rmtree(&g.work);
    }
    let res = json!({
        "slug": g.id, "route": g.route(), "level": level, "covers": gv.get("covers").cloned().unwrap_or(json!([])),
        "status": status, "reason": reason, "notes": g.notes, "seconds": t0.elapsed().as_secs(), "timing": g.timing,
        "values": g.values, "checks": g.checks.iter().map(|c| json!([c.0, c.1, c.2])).collect::<Vec<_>>(), "step_failed": step_failed,
    });
    let _ = fs::write(g.out.join("result.json"), serde_json::to_string_pretty(&res).unwrap());
    res
}

fn fmt_secs(s: u64) -> String {
    if s >= 60 {
        format!("{}:{:02}", s / 60, s % 60)
    } else {
        format!("{s} s")
    }
}

fn table(results: &[Value]) -> String {
    let mut rows = vec!["| game | route | covers | status | time | reason, notes |".to_string(), "|---|---|---|---|---|---|".to_string()];
    for r in results {
        let notes = strs(r, "notes");
        let reason = s(r, "reason");
        let mut why = if !reason.is_empty() { reason.clone() } else { notes.join(", ") };
        if !reason.is_empty() && !notes.is_empty() {
            why += &format!(" ({})", notes.join(", "));
        }
        rows.push(format!(
            "| {} | {} | {} | {} | {} | {} |",
            s(r, "slug"),
            s(r, "route"),
            strs(r, "covers").join(", "),
            s(r, "status"),
            fmt_secs(r["seconds"].as_u64().unwrap_or(0)),
            why.replace('|', "/")
        ));
    }
    let n = |st: &str| results.iter().filter(|r| s(r, "status") == st).count();
    let total: u64 = results.iter().map(|r| r["seconds"].as_u64().unwrap_or(0)).sum();
    rows.push(String::new());
    rows.push(format!("{} pass, {} fail, {} skip; {} in all", n("pass"), n("fail"), n("skip"), fmt_secs(total)));
    rows.join("\n")
}

fn selected(games: &[Value], only: Option<&str>) -> Result<Vec<Value>, String> {
    let Some(o) = only else { return Ok(games.to_vec()) };
    let rx = Regex::new(o).map_err(|e| format!("--only: {e}"))?;
    Ok(games
        .iter()
        .filter(|g| rx.is_match(&s(g, "id")) || rx.is_match(&s(g, "slug")) || strs(g, "covers").iter().any(|c| rx.is_match(c)) || rx.is_match(&s(g, "route")))
        .cloned()
        .collect())
}

fn cmd_list(a: &Opts, games: &[Value]) -> Result<u8, String> {
    println!("| game | route | Unity | covers | levels | fast checks | rig checks |");
    println!("|---|---|---|---|---|---|---|");
    for g in selected(games, a.only.as_deref())? {
        let route = s(&g, "route");
        let pack = g.get("pack").map_or("none".into(), show);
        let mut fast = match route.as_str() {
            "android" => format!("check, shaders, settings{}", if pack != "none" { format!(", android-data ({pack})") } else { String::new() }),
            "box64" => "route, shaders (GLES mode), settings".into(),
            "shaders" => "shaders, settings".into(),
            "apk" => "pmu apk".into(),
            "il2cpp" => "check, il2cpp --dry-run".into(),
            _ => "-".into(),
        };
        if let Some(e) = g.get("expect").and_then(Value::as_object).filter(|e| !e.is_empty()) {
            fast += &format!("; {}", e.iter().map(|(k, v)| format!("{k} {}", show(v))).collect::<Vec<_>>().join(", "));
        }
        let mut rig = "-".to_string();
        if strs(&g, "levels").contains(&"rig".to_string()) {
            let r = g.get("rig").cloned().unwrap_or(json!({}));
            let mut parts = vec![format!("{} s", r.get("seconds").map_or("300".into(), show)), format!("shots {}", strs(&r, "shots").join("/"))];
            let content = strs(&r, "content");
            if !content.is_empty() {
                parts.push(format!("picture at {}", content.join("/")));
            }
            if let Some(f) = r.get("frames_min").filter(|x| truthy(x)) {
                parts.push(format!("frames >= {}", show(f)));
            }
            parts.push("no crash".into());
            parts.extend(strs(&r, "log").iter().map(|x| format!("log /{x}/")));
            if g.get("input").is_some() {
                parts.push("input script".into());
            }
            rig = parts.join(", ");
        }
        println!("| {} | {route} | {} | {} | {} | {fast} | {rig} |", s(&g, "id"), s(&g, "unity"), strs(&g, "covers").join(", "), strs(&g, "levels").join(", "));
    }
    Ok(0)
}

fn git(args: &[&str]) -> std::process::Output {
    Command::new("git").args(args).stdin(Stdio::null()).output().unwrap_or_else(|_| std::process::Output {
        status: std::os::unix::process::ExitStatusExt::from_raw(1 << 8),
        stdout: vec![],
        stderr: vec![],
    })
}

/// git bisect over a worktree of this repository (in the scratch folder): at each commit this
/// pmu-dev, its list and baseline judge the worktree's tools.
fn cmd_bisect(a: &Opts, local: &Value) -> Result<u8, String> {
    let [good, bad] = &a.rest[..] else { return Err("bisect GOOD BAD --level fast|rig --only REGEX [--build CMD]".into()) };
    let Some(only) = &a.only else { return Err("bisect GOOD BAD --level fast|rig --only REGEX [--build CMD]".into()) };
    let scratch = local.get("scratch").map(show).ok_or("no scratch folder in the local config")?;
    let here_s = here().to_string_lossy().into_owned();
    let repo = String::from_utf8_lossy(&git(&["-C", &here_s, "rev-parse", "--show-toplevel"]).stdout).trim().to_string();
    let wt = format!("{}/bisect-worktree", abspath(&scratch));
    if Path::new(&wt).exists() {
        git(&["-C", &repo, "worktree", "remove", "--force", &wt]);
        rmtree(Path::new(&wt));
    }
    if !git(&["-C", &repo, "worktree", "add", "--detach", &wt, bad]).status.success() {
        return Err(format!("git worktree add {wt} {bad} failed"));
    }
    let exe = std::env::current_exe().map_err(|e| e.to_string())?.to_string_lossy().into_owned();
    let mut cmd = vec![exe, "regress".into(), a.bisect_level.clone(), "--only".into(), only.clone(), "--checkout".into(), wt.clone(), "--bisect-run".into(), "--list".into(), a.list.clone()];
    if let Some(c) = &a.config {
        cmd.extend(["--config".into(), c.clone()]);
    }
    if a.offline {
        cmd.push("--offline".into());
    }
    if let Some(p) = &a.pack {
        cmd.extend(["--pack".into(), p.clone()]);
    }
    if let Some(b) = &a.build {
        // (run by each step before its games, on the computer: a build script's Docker)
        cmd.extend(["--build".into(), b.clone()]);
    }
    let ok = git(&["-C", &wt, "bisect", "start", bad, good]).status.success();
    if ok {
        let mut c = Command::new("git");
        c.args(["-C", &wt, "bisect", "run"]).args(&cmd);
        let _ = c.status();
        let log = String::from_utf8_lossy(&git(&["-C", &wt, "bisect", "log"]).stdout).into_owned();
        println!("{log}");
        if let Some(m) = Regex::new(r"(?m)^# first bad commit: \[(\w+)\] (.*)$").unwrap().captures(&log) {
            println!("first bad commit: {} {}", &m[1][..12.min(m[1].len())], &m[2]);
        }
    }
    git(&["-C", &wt, "bisect", "reset"]);
    git(&["-C", &repo, "worktree", "remove", "--force", &wt]);
    Ok(0)
}

/// A finished run's results (its results.json) judged again by the list and baseline as they are now.
fn cmd_recheck(a: &Opts, games: &[Value]) -> Result<u8, String> {
    let [d] = &a.rest[..] else { return Err("recheck <results folder of a run>".into()) };
    let run_: Value = serde_json::from_slice(&fs::read(Path::new(d).join("results.json")).map_err(|e| format!("{d}: {e}"))?).map_err(|e| e.to_string())?;
    let level = s(&run_, "level");
    let base = load_baseline(&level);
    let mut out = Vec::new();
    for r in run_["results"].as_array().cloned().unwrap_or_default() {
        let id = s(&r, "slug");
        let Some(g) = games.iter().find(|g| s(g, "id") == id) else {
            out.push(r);
            continue;
        };
        if s(&r, "status") == "skip" || !truthy(&r["values"]) {
            out.push(r);
            continue;
        }
        let v = &r["values"];
        let mut cks: Vec<Check> = Vec::new();
        if level == "fast" {
            cks = checks::expectations_fast(g.get("expect").unwrap_or(&json!({})), v);
            if let Some(golden) = base.get(&id).filter(|x| truthy(x)) {
                let diffs = checks::compare_exact(golden, v);
                let detail = if diffs.is_empty() {
                    "same as the baseline".to_string()
                } else {
                    format!("{} values differ from the baseline: {}", diffs.len(), diffs.iter().take(6).map(|(k, x, y)| format!("{k}: {} -> {}", short(x), short(y))).collect::<Vec<_>>().join("; "))
                };
                cks.push(("baseline".into(), diffs.is_empty(), detail));
            }
        } else {
            let log = read_lines(&Path::new(d).join(&id).join("emu-run.log"));
            let runv = checks::run_log(&log);
            let shots: Vec<Value> = v.get("shots").and_then(Value::as_array).cloned().unwrap_or_default();
            cks = checks::expectations_rig(g.get("rig").unwrap_or(&json!({})), &runv, &shots, &log.join("\n"));
            cks.extend(checks::compare_rig(base.get(&id).unwrap_or(&Value::Null), &runv, &shots, 0.3));
        }
        // what failed before the checks (a step) stays failed
        let step_failed = r.get("step_failed").map_or(
            s(&r, "status") == "fail" && !r["checks"].as_array().is_some_and(|c| c.iter().any(|x| x[1] == json!(false))),
            truthy,
        );
        let mut r = r.clone();
        r["checks"] = json!(cks.iter().map(|c| json!([c.0, c.1, c.2])).collect::<Vec<_>>());
        let has_base = base.get(&id).is_some_and(truthy);
        r["notes"] = json!(strs(&r, "notes").into_iter().filter(|n| !n.starts_with("baseline updated") && !(n == "no baseline yet" && has_base)).collect::<Vec<_>>());
        if !step_failed {
            let bad: Vec<&Check> = cks.iter().filter(|c| !c.1).collect();
            if bad.is_empty() {
                r["status"] = json!("pass");
                r["reason"] = json!("");
            } else {
                r["status"] = json!("fail");
                r["reason"] = json!(bad.iter().map(|c| c.2.clone()).collect::<Vec<_>>().join("; "));
            }
        }
        out.push(r);
    }
    println!("recheck of {d} ({level}, {})\n", s(&run_, "checkout"));
    println!("{}", table(&out));
    Ok(if out.iter().any(|r| s(r, "status") == "fail") { 1 } else { 0 })
}

pub fn run_cli(args: &[String]) -> Result<u8, String> {
    let a = match parse(args) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{e}");
            return Ok(2);
        }
    };
    let bisect_code = |c: u8| if a.bisect_run { 125 } else { c };
    let setup_err = |m: String| -> Result<u8, String> {
        eprintln!("regress: {m}");
        Ok(bisect_code(2))
    };
    let games = match load_list(&a.list) {
        Ok(g) => g,
        Err(e) => return setup_err(e),
    };
    match a.level.as_str() {
        "list" => return cmd_list(&a, &games),
        "recheck" => return cmd_recheck(&a, &games),
        _ => {}
    }
    let local = match load_local(a.config.as_deref()) {
        Ok(l) => l,
        Err(e) => return setup_err(e),
    };
    if a.level == "bisect" {
        return cmd_bisect(&a, &local);
    }
    if let (true, Some(b), Some(wt)) = (a.bisect_run, &a.build, &a.checkout) {
        let mut c = Command::new("/bin/sh");
        c.arg("-c").arg(format!("cd {} && ({b}) >/dev/null 2>&1", shq(wt)));
        if !pkgzip::host::status(&mut c).is_ok_and(|s| s.success()) {
            eprintln!("regress: the build ({b}) failed at this commit");
            return Ok(125);
        }
    }
    let ctx = match Ctx::new(a.clone(), local) {
        Ok(c) => c,
        Err(Stop::Setup(m)) | Err(Stop::Fail(m)) | Err(Stop::Skip(m)) => return setup_err(m),
    };
    let games: Vec<Value> = selected(&games, a.only.as_deref())?.into_iter().filter(|g| strs(g, "levels").contains(&a.level)).collect();
    if games.is_empty() {
        return setup_err("no game of the list for this level and --only".into());
    }
    let _ = fs::create_dir_all(&ctx.results);
    let mut base = load_baseline(&a.level);
    let head = String::from_utf8_lossy(&git(&["-C", &ctx.checkout, "log", "-1", "--format=%h %s"]).stdout).trim().to_string();
    println!("regress {}: {} games, checkout {} ({head}); results {}", a.level, games.len(), ctx.checkout, ctx.results);
    let t0 = Instant::now();
    let mut results = Vec::new();
    for (i, g) in games.iter().enumerate() {
        println!("[{}/{}] {} ({})", i + 1, games.len(), s(g, "id"), s(g, "route"));
        let id = s(g, "id");
        let r = run_game(&ctx, g, &a.level, base.get(&id));
        println!("    -> {} {} {}", s(&r, "status"), fmt_secs(r["seconds"].as_u64().unwrap_or(0)), s(&r, "reason").chars().take(300).collect::<String>());
        if a.update && s(&r, "status") != "skip" && truthy(&r["values"]) && !truthy(&r["step_failed"]) {
            base.insert(id, r["values"].clone());
            save_baseline(&a.level, &base);
        } else if a.update && truthy(&r["step_failed"]) {
            println!("    (baseline not updated: a step failed)");
        }
        results.push(r);
    }
    let out = table(&results);
    let summary = json!({"level": a.level, "checkout": head, "run": ctx.run_id, "seconds": t0.elapsed().as_secs(), "results": results});
    let _ = fs::write(Path::new(&ctx.results).join("results.json"), serde_json::to_string_pretty(&summary).unwrap());
    let _ = fs::write(Path::new(&ctx.results).join("results.md"), format!("# regress {}, {head}, {}\n\n{out}\n", a.level, ctx.run_id));
    println!("\n{out}");
    if a.update {
        let n = results.iter().filter(|r| s(r, "status") != "skip" && truthy(&r["values"]) && !truthy(&r["step_failed"])).count();
        println!("baseline: {} updated for {n} games", baseline_path(&a.level).display());
    }
    if a.bisect_run && results.iter().all(|r| s(r, "status") == "skip") {
        return Ok(125); // (git bisect: this commit cannot be judged)
    }
    Ok(if results.iter().any(|r| s(r, "status") == "fail") { 1 } else { 0 })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_and_lists() {
        assert_eq!(hide_name("level3"), "level3");
        assert_eq!(hide_name("Resources/unity_builtin_extra"), "Resources/unity_builtin_extra");
        assert!(hide_name("StreamingAssets/my_bundle").starts_with("f-") && hide_name("x").len() == 14);
        let d = std::env::temp_dir().join(format!("pmu-dev-regress-{}", std::process::id()));
        let _ = fs::create_dir_all(&d);
        let p = d.join("games.toml");
        fs::write(&p, "[[game]]\nslug = \"a\"\nroute = \"android\"\ncovers = [\"2019.4\"]\n[[game]]\nslug = \"a\"\nid = \"a-rig\"\nroute = \"apk\"\nlevels = [\"rig\"]\n").unwrap();
        let g = load_list(&p.to_string_lossy()).unwrap();
        assert_eq!(g.len(), 2);
        assert_eq!(g[0]["levels"], json!(["fast"]));
        assert_eq!(selected(&g, Some("2019")).unwrap().len(), 1);
        assert_eq!(selected(&g, Some("apk")).unwrap().len(), 1);
        fs::write(&p, "[[game]]\nslug = \"a\"\nroute = \"nope\"\n").unwrap();
        assert!(load_list(&p.to_string_lossy()).unwrap_err().contains("route 'nope'"));
        let _ = fs::remove_dir_all(&d);
        assert_eq!(fmt_secs(75), "1:15");
        assert_eq!(fmt_secs(5), "5 s");
        assert_eq!(dump(&json!({"b": 1, "a": [1, 2]})), "{\n \"a\": [\n  1,\n  2\n ],\n \"b\": 1\n}");
    }

    #[test]
    fn merge() {
        let m = deep_merge(&json!({"a": {"x": 1, "y": 2}, "b": 1}), &json!({"a": {"y": 3}, "c": 4}));
        assert_eq!(m, json!({"a": {"x": 1, "y": 3}, "b": 1, "c": 4}));
    }
}
