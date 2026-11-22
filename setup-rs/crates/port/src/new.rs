//! `port new`: a port's source folder from port.toml (or .json) and the route's templates
//! (port.py `new`, with the route hooks' port_new: route.py's box64 part, il2cpp.py, mono64.py,
//! apk.py).
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

use crate::config::{aarch64_only_route_rep, build_config_rep, dump_toml, load_config, merge, validate};
use crate::routes::{port_donors, read_text, render, routes, template_path};
use crate::text::{self, *};
use crate::val::{Dict, Val};
use crate::{abspath, exit, mac_junk, ps, pyjson, Repo};
use pkgzip::{io_at, Result};

pub const GITIGNORE: &str = "# build output (dotnet's obj/ holds local paths)
out/
obj/
bin/
__pycache__/
*.pyc
# never commit game files, Unity's player, logs or the rig's screenshots
*.apk
*.obb
*.so
*.dll
*.assets
*.resS
*.resource
*.bnk
*_Data/
*.app/
*.log
*.ppm
shots/
.DS_Store
._*
";

/// What `port new` is given (port.py's new() arguments).
#[derive(Default)]
pub struct NewArgs {
    pub name: String,
    pub data: Option<String>,
    pub config: Option<String>,
    pub out: Option<String>,
    pub route: Option<String>,
    pub title: Option<String>,
    pub player: Option<String>,
    pub force: bool,
}

/// What a port_new hook gets: the config, the output folder, write().
pub struct NewCtx {
    pub cfg: Dict,
    pub out: PathBuf,
    pub written: Vec<String>,
    /// files.own: the porter's own versions of files port new would write
    pub own: Vec<String>,
    /// what write() says, for the log (in order)
    pub said: Vec<String>,
}

impl NewCtx {
    pub fn write(&mut self, rel: &str, data: &[u8], mode: u32) -> Result<()> {
        let p = self.out.join(rel);
        if self.own.iter().any(|o| o == rel) && p.is_file() {
            self.said.push(format!("{rel}: the port's own (files.own), left as it is"));
            self.written.push(rel.to_string());
            return Ok(());
        }
        if let Some(d) = p.parent() {
            fs::create_dir_all(d).map_err(io_at(d))?;
        }
        write_file(&p, data)?;
        fs::set_permissions(&p, fs::Permissions::from_mode(mode)).map_err(io_at(&p))?;
        self.written.push(rel.to_string());
        Ok(())
    }
}

/// open(p, 'w').write(data): the file truncated or made (its mode kept or the default).
pub fn write_file(p: &Path, data: &[u8]) -> Result<()> {
    fs::write(p, data).map_err(io_at(p))
}

/// os.access(p, os.X_OK)
fn is_exec(p: &Path) -> bool {
    use std::ffi::CString;
    let Ok(c) = CString::new(p.as_os_str().as_encoded_bytes()) else { return false };
    unsafe { libc::access(c.as_ptr(), libc::X_OK) == 0 }
}

/// Writes a port's source folder; returns its path.
pub fn new(repo: &Repo, a: &NewArgs, log: &mut dyn FnMut(&str)) -> Result<PathBuf> {
    new_with(repo, a, None, log)
}

/// The check of `--data <game>`: (the game, asmpatch's command) -> the report (crates/check's; this
/// crate does not read games).
pub type Checker<'a> = &'a mut dyn FnMut(&str) -> Result<Dict>;

/// What the check suggests that the porter's port.toml lacks or sets otherwise: a key it lacks is
/// used all the same (merged in; port.toml should say so), a value it has wins. For android_data,
/// only where port.toml has the default.
pub fn suggest_gaps(user: &Dict, sug: &Dict) -> Vec<String> {
    use crate::config::{defaults, toml_key, toml_value};
    let tv = |v: &Val| toml_value(v).unwrap_or_else(|_| v.repr());
    let mut out = Vec::new();
    for (k, v) in sug.dict("runtime").iter() {
        match user.dict("runtime").get(k) {
            None | Some(Val::Null) => out.push(format!("[runtime] {k} = {}: used, not in port.toml (add it there)", tv(v))),
            Some(have) if have.py_str() != v.py_str() => {
                out.push(format!("[runtime] {k} = {} in port.toml; the check suggests {}", tv(have), tv(v)))
            }
            _ => {}
        }
    }
    let dflt = defaults();
    let dflt = dflt.dict("android_data");
    for (k, v) in sug.dict("android_data").iter() {
        let d = dflt.get(k).cloned().unwrap_or(Val::Null);
        match user.dict("android_data").get(k) {
            None | Some(Val::Null) => {
                if !v.eq(&d) {
                    out.push(format!("[android_data] {k} = {}: used, not in port.toml (add it there)", tv(v)));
                }
            }
            Some(have) => {
                if have.eq(&d) && !have.eq(v) {
                    out.push(format!("[android_data] {k} = {} in port.toml (the default); the check suggests {}", tv(have), tv(v)));
                }
            }
        }
    }
    if sug.dict("wwise").truthy("banks") {
        let banks = sug.dict("wwise").get("banks").unwrap();
        match user.dict("wwise").get("banks") {
            None | Some(Val::Null) => out.push(format!(
                "[wwise] banks = {}: used (the setup converts them for ua_host's Wwise player), not in port.toml (add it there)",
                tv(banks)
            )),
            Some(have) if !have.eq(banks) => out.push(format!(
                "[wwise] banks = {} in port.toml; the check suggests {}{}",
                tv(have),
                tv(banks),
                if have.truthy() { "" } else { " (empty: the game runs silent)" }
            )),
            _ => {}
        }
    }
    let steps = sug.dict("convert").list("steps");
    if let (false, Some(Val::List(have))) = (steps.is_empty(), user.dict("convert").get("steps")) {
        let more: Vec<Val> = steps.iter().filter(|x| !have.iter().any(|h| h.eq(x))).cloned().collect();
        if !more.is_empty() {
            out.push(format!("[convert] steps lacks {}, which the check suggests", tv(&Val::List(more))));
        }
    }
    if sug.truthy("controls") && !user.truthy("controls") {
        out.push(format!(
            "[controls] is empty: the READMEs use the check's draft from the game's pad bindings: {}",
            sug.dict("controls").iter().map(|(k, v)| format!("{} = {}", toml_key(k), tv(v))).collect::<Vec<_>>().join("; ")
        ));
    }
    out
}

/// `port new`, with the check `--data` asks for.
pub fn new_with(repo: &Repo, a: &NewArgs, check: Option<Checker>, log: &mut dyn FnMut(&str)) -> Result<PathBuf> {
    let mut config = a.config.clone();
    let mut user = match &config {
        Some(c) => load_config(c)?,
        None => Dict::new(),
    };
    let out = abspath(Path::new(a.out.as_deref().unwrap_or(&a.name)));
    let own_toml = out.join("port.toml");
    if config.is_none() && own_toml.is_file() {
        user = load_config(&ps(&own_toml))?;
        config = Some(ps(&own_toml));
        log(&format!("using {}", own_toml.display()));
    }
    let repo_files = [".git", ".github", ".gitignore", ".gitattributes", "LICENSE", "LICENSE.md", "LICENSE.txt", "README.md"];
    if out.exists() && !a.force && !out.join("port.build.json").is_file() {
        let other = fs::read_dir(&out)
            .map_err(io_at(&out))?
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .any(|n| !mac_junk(&n) && !repo_files.contains(&n.as_str()));
        if other {
            return Err(exit(format!(
                "{} exists and is not a port made by `port new` (--force to write into it)",
                out.display()
            )));
        }
    }
    let mut rep: Option<Dict> = None;
    if let Some(data) = a.data.as_deref().filter(|d| !d.is_empty()) {
        let Some(check) = check else {
            return Err(exit("--data <game>: this build of the packager has no check of a game (crates/check)"));
        };
        log(&format!("checking {data}"));
        let r = check(data)?;
        if !r.truthy("route") && a.route.as_deref().is_none_or(str::is_empty) && !user.dict("port").truthy("route") {
            return Err(exit(format!("no route fits this game: {}", r.get("unsupported").map_or("None".into(), Val::py_str))));
        }
        rep = Some(r);
    } else if !user.dict("game").truthy("unity_versions") {
        return Err(exit("--data <game> (or a config with game.unity_versions) is needed"));
    }
    if std::env::var("PMUNITY_PORT_HOOKS").is_ok_and(|h| h.split(',').any(|m| !m.is_empty())) {
        return Err(exit("PMUNITY_PORT_HOOKS: pmu port runs no Python hook modules (run `python -m pmunity port new`)"));
    }
    let mut over = Dict::new();
    if let Some(r) = a.route.as_deref().filter(|r| !r.is_empty()) {
        over.table("port").set("route", Val::s(r));
    }
    if let Some(t) = a.title.as_deref().filter(|t| !t.is_empty()) {
        over.table("port").set("title", Val::s(t));
    }
    let mut cfg = build_config_rep(&a.name, rep.as_ref(), &merge(&user, &over));
    aarch64_only_route_rep(&mut cfg, &mut user, rep.as_ref(), log)?;
    let rts = routes(repo)?;
    let route_name = cfg.dict("port").get("route").map(Val::py_str).unwrap_or_default();
    let r = rts.iter().find(|(n, _)| *n == route_name).map(|(_, r)| r.clone());
    // the route's own defaults (route.json "defaults"), where port.toml says nothing
    if let Some(r) = &r {
        for (sec, vals) in r.dict("defaults").iter() {
            if let Val::Dict(vals) = vals {
                for (k, v) in vals.iter() {
                    if !user.dict(sec).contains(k) {
                        cfg.table(sec).set(k, v.clone());
                    }
                }
            }
        }
    }
    // a .NET 3.5 game on the mono-arm64 route: Mono's 4.x class libraries in place of its 2.0 ones
    if cfg.dict("port").str("route") == "mono-arm64"
        && !user.dict("player").contains("mono64_classlibs")
        && rep.as_ref().is_some_and(|r| r.truthy("scripting_runtime") && r.get("scripting_runtime").unwrap().py_str().contains("3.5"))
    {
        cfg.table("player").set("mono64_classlibs", Val::Bool(true));
        log("a .NET 3.5 game: the release ships Mono's 4.x class libraries for it (player.mono64_classlibs)");
    }
    if let Some(player) = a.player.as_deref().filter(|p| !p.is_empty()) {
        if !cfg.dict("player").truthy("sha256") {
            let pl = cfg.dict("player");
            let libs = Path::new(player)
                .join("Variations")
                .join(pl.get("backend").map_or("None".into(), Val::py_str))
                .join("Release")
                .join("Libs")
                .join(pl.get("abi").map_or("None".into(), Val::py_str));
            let mut sums = Vec::new();
            for n in ["libmain.so", "libunity.so"] {
                let f = libs.join(n);
                if f.is_file() {
                    let data = fs::read(&f).map_err(io_at(&f))?;
                    sums.push((n, sha256_hex(&data)));
                }
            }
            let sha = cfg.table("player").table("sha256");
            for (n, h) in sums {
                sha.set(n, Val::Str(h));
            }
        }
    }
    let Some(r) = r else {
        let have: Vec<&str> = rts.iter().map(|(n, _)| n.as_str()).collect();
        let have = if have.is_empty() { "none".to_string() } else { have.join(", ") };
        return Err(exit(format!(
            "route {} has no templates (templates/{route_name}/route.json; routes with templates: {have})",
            Val::s(&route_name).repr()
        )));
    };
    let donors = port_donors(&cfg, &r);
    cfg.table("player").set("donors", Val::List(donors));
    validate(&cfg)?;
    fs::create_dir_all(&out).map_err(io_at(&out))?;
    let own: Vec<String> = cfg.dict("files").list("own").iter().filter_map(|x| x.as_str().map(String::from)).collect();
    let mut ctx = NewCtx { cfg: cfg.clone(), out: out.clone(), written: Vec::new(), own, said: Vec::new() };
    let p = cfg.dict("port");
    let g = cfg.dict("game");
    let pn = p.get("name").map_or("None".into(), Val::py_str);
    let pt = p.get("title").map_or("None".into(), Val::py_str);
    let route = route_name.as_str();
    let int0 = |k: &str| Val::int_or0(g.get(k)).unwrap_or(0);
    let space = int0("setup_space_mb");
    let size_gb = std::cmp::max(1, ((if space != 0 { space } else { 3 * int0("size_mb") }) + 1023).div_euclid(1024));
    let join = |l: &[Val], sep: &str| l.iter().map(Val::py_str).collect::<Vec<_>>().join(sep);
    let vs = join(g.list("unity_versions"), ", ");
    let porter = join(p.list("porter"), ", ");
    let values: Vec<(&str, String)> = vec![
        ("PORTDIR", pn.clone()),
        ("TITLE", pt.clone()),
        ("ZIP", format!("{pn}.zip")),
        ("LAUNCHER", format!("{pt}.sh")),
        ("SETUP_TIME", p.get("setup_time").map_or("None".into(), Val::py_str)),
        ("GAME_WHAT", g.get("what").map_or("None".into(), Val::py_str)),
        ("GAME_FORMS", game_forms(&g.get("build").map_or(String::new(), Val::py_str)).into()),
        ("ROUTE", route.into()),
        ("ROUTE_TEXT", route_text(route).map_or(route.to_string(), String::from)),
        ("UNITY_VERSIONS", if vs.is_empty() { "any the converter supports".into() } else { vs }),
        ("SIZE_GB", size_gb.to_string()),
        ("PORTER", if porter.is_empty() { "not named yet (port.toml: `port.porter`)".into() } else { porter }),
        ("READY", p.get("ready_marker").filter(|v| v.truthy()).map_or(".setup_done".into(), Val::py_str)),
        ("PLAYER_FROM", player_from(&cfg, &r)),
        ("THANKS", thanks_sentence(&cfg)),
        ("CONTROLS", controls_markdown(&cfg)),
        ("ROUTE_NEEDS", text::route_needs(route, &cfg)),
        ("BUILD_STEPS", build_steps(&cfg, &r)),
        ("PLAYER_ON", player_on(route).into()),
        ("HANDHELD_NEEDS", handheld_needs(route).into()),
    ];
    // the files a route renders: route.json "files": {output path: template name}
    for (dst, spec) in r.dict("files").iter() {
        let (tname, mode) = match spec {
            Val::Str(s) => (s.clone(), None),
            Val::Dict(d) => (d.get("template").map_or("None".into(), Val::py_str), d.get("mode").filter(|m| m.truthy()).map(Val::py_str)),
            x => (x.py_str(), None),
        };
        let tpath = template_path(repo, &r, &tname)?;
        let t = read_text(&tpath)?;
        let dst_r = render(dst, &values, "route.json")?;
        let mode = match mode {
            Some(m) => u32::from_str_radix(&m, 8).map_err(|_| exit(format!("route.json: mode {m:?}")))?,
            None => 0o644,
        };
        ctx.write(&dst_r, render(&t, &values, &tname)?.as_bytes(), mode)?;
    }
    let pd = pn.clone();
    let sections: Option<Vec<Val>> = r.get("config").and_then(Val::as_list).map(<[Val]>::to_vec);
    ctx.write(&format!("{pd}/port.json"), (pyjson::dumps(&port_json(&cfg), Some(4), true) + "\n").as_bytes(), 0o644)?;
    ctx.write(&format!("{pd}/port.env"), port_env(&cfg).as_bytes(), 0o644)?;
    ctx.write(&format!("{pd}/setup/setup.env"), setup_env(&cfg, &[], sections.as_deref()).as_bytes(), 0o644)?;
    let has_convert = match r.get("config") {
        Some(c) if c.truthy() => c.as_list().unwrap_or(&[]).iter().any(|x| x.eq(&Val::s("convert"))),
        _ => true,
    };
    if has_convert {
        let conv = Val::Dict(cfg.dict("convert").clone());
        ctx.write(&format!("{pd}/setup/convert.json"), (pyjson::dumps(&conv, Some(1), true) + "\n").as_bytes(), 0o644)?;
    }
    if !cfg.list("questions").is_empty() {
        ctx.write(&format!("{pd}/setup/questions.lua"), questions_lua(cfg.list("questions")).as_bytes(), 0o644)?;
    }
    ctx.write(&format!("{pd}/setup.txt"), setup_txt(&cfg).as_bytes(), 0o644)?;
    // the porter's files
    let base = match &config {
        Some(c) => abspath(Path::new(c)).parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("/")),
        None => std::env::current_dir()?,
    };
    let fl = cfg.dict("files");
    let mut files: Vec<String> = fl.list("plugins").iter().chain(fl.list("managed")).map(Val::py_str).collect();
    for ap in fl.list("assembly_patches") {
        if let Some(d) = ap.as_dict() {
            files.extend(d.list("files").iter().map(Val::py_str));
        }
    }
    let src_of = |rel: &str| PathBuf::from(pkgzip::py::join(&ps(&base), rel));
    for rel in &files {
        let src = src_of(rel);
        if !src.is_file() {
            return Err(exit(format!("files: {} not found", src.display())));
        }
        let mode = if is_exec(&src) { 0o755 } else { 0o644 };
        ctx.write(&format!("{pd}/setup/files/{}", pkgzip::py::basename(rel)), &fs::read(&src).map_err(io_at(&src))?, mode)?;
    }
    for (point, rel) in fl.dict("hooks").iter() {
        let src = src_of(&rel.py_str());
        if !src.is_file() {
            return Err(exit(format!("files.hooks.{point}: {} not found", src.display())));
        }
        ctx.write(&format!("{pd}/setup/hooks/{point}"), &fs::read(&src).map_err(io_at(&src))?, 0o644)?;
    }
    for rel in fl.list("extra") {
        let rel = rel.py_str();
        let src = src_of(&rel);
        if !src.is_file() {
            return Err(exit(format!("files.extra: {} not found", src.display())));
        }
        ctx.write(&format!("{pd}/{}", pkgzip::py::basename(&rel)), &fs::read(&src).map_err(io_at(&src))?, 0o644)?;
    }
    port_new_hooks(repo, &mut ctx)?;
    // a .gitignore for the port's repository, once
    let gi = out.join(".gitignore");
    if !gi.exists() {
        write_file(&gi, GITIGNORE.as_bytes())?;
    }
    // PortMaster's metadata from the final port.json (route hooks adjust its texts)
    let pj = pyjson::loads(&read_text(&out.join(&pd).join("port.json"))?)?;
    let attr = pj.as_dict().map(|d| d.dict("attr").clone()).unwrap_or_default();
    let image = gameinfo_image(&cfg, &ps(&out));
    ctx.write(&format!("{pd}/gameinfo.xml"), gameinfo_xml(&cfg, &image, attr.get("desc")).as_bytes(), 0o644)?;
    let inst = attr.get("inst").filter(|v| v.truthy()).map_or(String::new(), Val::py_str);
    ctx.write("testing_thread.txt", testing_thread_txt(&cfg, &inst).as_bytes(), 0o644)?;
    if config.is_none() {
        let drafted: &[&str] = if rep.as_ref().is_some_and(|r| r.dict("suggest").truthy("controls")) { &["controls"] } else { &[] };
        ctx.write("port.toml", dump_toml(&cfg, &r, drafted)?.as_bytes(), 0o644)?;
    }
    let mut route_out = Dict::new();
    for (k, v) in r.iter() {
        if k != "dir" {
            route_out.set(k, v.clone());
        }
    }
    let mut written = ctx.written.clone();
    written.sort();
    written.dedup();
    let mut build = crate::dict! {
        "made_by" => Val::s("pmunity port new"),
        "config" => Val::Dict(cfg.clone()),
        "route" => Val::Dict(route_out),
        "files" => Val::strs(&written),
    };
    if let Some(rep) = rep.as_ref().filter(|r| !r.is_empty()) {
        let mut c = Dict::new();
        for k in ["build", "arch", "backend", "unity_version", "scripting_runtime", "compressed", "input_verdict", "route", "notes", "warnings"] {
            c.set(k, rep.get(k).cloned().unwrap_or(Val::Null));
        }
        build.set("check", Val::Dict(c));
    }
    let build = Val::Dict(build);
    ctx.write("port.build.json", (pyjson::dumps(&build, Some(1), true) + "\n").as_bytes(), 0o644)?;
    // remove files an earlier run wrote that this one did not
    let old = out.join(".port_files");
    if old.is_file() {
        for rel in read_text(&old)?.split('\n') {
            if !rel.is_empty() && !ctx.written.iter().any(|w| w == rel) && rel != "port.toml" && out.join(rel).is_file() {
                fs::remove_file(out.join(rel)).map_err(io_at(out.join(rel)))?;
            }
        }
    }
    let mut all = ctx.written.clone();
    all.sort();
    all.dedup();
    write_file(&old, (all.join("\n") + "\n").as_bytes())?;
    for m in std::mem::take(&mut ctx.said) {
        log(&m);
    }
    log(&format!("port {pn} ({route} route) -> {}", out.display()));
    let donors = cfg.dict("player").list("donors");
    if !donors.is_empty() {
        log(&format!(
            "donors the release ships (Unity's player; `port package --donors DIR`, built with tools/donor/build.sh): {}",
            join(donors, ", ")
        ));
    } else if r.truthy("donors") {
        log("no donors: game.unity_versions names no Unity version, so the release would ship no player for the setup");
    }
    if let Some(rep) = &rep {
        for w in rep.list("warnings") {
            log(&format!("warning: {}", w.py_str()));
        }
        for n in rep.list("notes") {
            log(&format!("- {}", n.py_str()));
        }
        if let Some(c) = &config {
            let gaps = suggest_gaps(&user, rep.dict("suggest"));
            if !gaps.is_empty() {
                log(&format!("{} and what this check suggests ({}; add them, or keep yours on purpose):", pkgzip::py::basename(c), gaps.len()));
                for x in gaps {
                    log(&format!("  - {x}"));
                }
            }
        }
    }
    let todo = submission_warnings(&cfg, &ps(&out), Some(&pj), None, None);
    if !todo.is_empty() {
        log(&format!("for a PortMaster submission ({}; docs/porting.md, \"Submitting to PortMaster\"):", todo.len()));
        for x in todo {
            log(&format!("  - {x}"));
        }
    }
    Ok(out)
}

/// The route hooks' port_new, in port.py's HOOK_MODULES order (route, il2cpp, mono64, apk).
pub fn port_new_hooks(repo: &Repo, ctx: &mut NewCtx) -> Result<()> {
    box64_port_new(repo, ctx)?;
    il2cpp_port_new(ctx)?;
    mono64_port_new(ctx)?;
    apk_port_new(ctx)
}

fn s(d: &Dict, k: &str) -> String {
    d.get(k).map_or("None".into(), Val::py_str)
}

/// port.json of the port being written, when there is one: (its path, the parsed file).
fn read_port_json(ctx: &NewCtx, name: &str) -> Result<Option<Val>> {
    let p = ctx.out.join(name).join("port.json");
    if !p.is_file() {
        return Ok(None);
    }
    Ok(Some(pyjson::loads(&read_text(&p)?)?))
}

fn no_armhf(a: &mut Dict) {
    let reqs: Vec<Val> = a.list("reqs").iter().filter(|r| !r.eq(&Val::s("armhf"))).cloned().collect();
    a.set("reqs", Val::List(reqs));
}

fn write_port_json(ctx: &mut NewCtx, name: &str, pj: &Val) -> Result<()> {
    ctx.write(&format!("{name}/port.json"), (pyjson::dumps(pj, Some(4), true) + "\n").as_bytes(), 0o644)
}

/// str.splitlines()
fn splitlines(t: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let mut it = t.char_indices().peekable();
    while let Some((i, c)) = it.next() {
        if matches!(c, '\n' | '\r' | '\u{b}' | '\u{c}' | '\u{1c}' | '\u{1d}' | '\u{1e}' | '\u{85}' | '\u{2028}' | '\u{2029}') {
            out.push(&t[start..i]);
            let mut end = i + c.len_utf8();
            if c == '\r' {
                if let Some(&(_, '\n')) = it.peek() {
                    it.next();
                    end += 1;
                }
            }
            start = end;
        }
    }
    if start < t.len() {
        out.push(&t[start..]);
    }
    out
}

/// route.py port_new: a box64 port's shader config, port.json as PortMaster should list it, and
/// port.env as the box64 launcher reads it.
fn box64_port_new(repo: &Repo, ctx: &mut NewCtx) -> Result<()> {
    let p = ctx.cfg.dict("port").clone();
    if p.str("route") != "box64" {
        return Ok(());
    }
    let name = s(&p, "name");
    let gles = read_text(&repo.root.join("tools").join("box64").join("gles.json"))?;
    ctx.write(&format!("{name}/setup/gles.json"), gles.as_bytes(), 0o644)?;
    let Some(mut pj) = read_port_json(ctx, &name)? else {
        return Ok(());
    };
    {
        let a = pj.as_dict_mut().unwrap().table("attr");
        if !p.truthy("desc") {
            a.set("desc", Val::Str(format!("{}: the game's own Linux build under box64.", s(&p, "title"))));
        }
        if !p.truthy("inst") {
            a.set(
                "inst",
                Val::Str(format!(
                    "Requires {}: put it in ports/{name}/gamedata. The first launch prepares it ({}).",
                    s(ctx.cfg.dict("game"), "what"),
                    s(&p, "setup_time")
                )),
            );
        }
        let mut rt: Vec<String> = a.list("runtime").iter().map(Val::py_str).collect();
        rt.push("weston_pkg_0.2.squashfs".into());
        rt.sort();
        rt.dedup();
        a.set("runtime", Val::strs(&rt));
        no_armhf(a);
        a.set("min_glibc", Val::s("2.29"));
    }
    write_port_json(ctx, &name, &pj)?;
    let env = ctx.out.join(&name).join("port.env");
    let kept: Vec<String> = if env.is_file() {
        splitlines(&read_text(&env)?).into_iter().filter(|l| !l.is_empty() && !l.starts_with('#')).map(String::from).collect()
    } else {
        vec![]
    };
    let mut lines: Vec<String> = vec![
        "# The game's options for the box64 launcher (docs/box64-route.md in portmaster-unity), sourced by it;".into(),
        "# a variable already set in the environment wins. Uncomment to change:".into(),
    ];
    lines.extend(kept);
    lines.extend(
        [
            "#PMU_GL=gles          # gles | mesa | gl4es (default: what the setup found, in game.env)",
            "#PMU_BOX64=default    # safe | default | fast (box64.env)",
            "#PMU_MAXCPU=2         # CPUs box64 reports to the game (Unity sizes its job threads by it)",
            "#PMU_ARGS=\"-force-gfx-direct\"   # more arguments for the game",
            "#PMU_DYNACACHE=0      # box64 translates the game again at every start",
        ]
        .map(String::from),
    );
    ctx.write(&format!("{name}/port.env"), (lines.join("\n") + "\n").as_bytes(), 0o644)
}

/// il2cpp.py port_new: port.json for the il2cpp-arm64 route.
fn il2cpp_port_new(ctx: &mut NewCtx) -> Result<()> {
    let p = ctx.cfg.dict("port").clone();
    if p.str("route") != "il2cpp-arm64" {
        return Ok(());
    }
    let name = s(&p, "name");
    let Some(mut pj) = read_port_json(ctx, &name)? else {
        return Ok(());
    };
    {
        let a = pj.as_dict_mut().unwrap().table("attr");
        no_armhf(a);
        if !p.truthy("desc") {
            a.set(
                "desc",
                Val::Str(format!(
                    "{} on Unity's 64-bit Android player (its code compiled with IL2CPP on the handheld).",
                    s(&p, "title")
                )),
            );
        }
        if !p.truthy("inst") {
            a.set(
                "inst",
                Val::Str(format!(
                    "Requires {}: put it in ports/{name}/gamedata. The first launch builds the game on the handheld and compiles its code with Unity's IL2CPP ({}, plus one to a few hours for the code; about 1.6 GB free; Wi-Fi for Unity's and Debian's files).",
                    s(ctx.cfg.dict("game"), "what"),
                    s(&p, "setup_time")
                )),
            );
        }
    }
    write_port_json(ctx, &name, &pj)
}

/// mono64.py port_new: port.json for the mono-arm64 route.
fn mono64_port_new(ctx: &mut NewCtx) -> Result<()> {
    let p = ctx.cfg.dict("port").clone();
    if p.str("route") != "mono-arm64" {
        return Ok(());
    }
    let name = s(&p, "name");
    let Some(mut pj) = read_port_json(ctx, &name)? else {
        return Ok(());
    };
    {
        let a = pj.as_dict_mut().unwrap().table("attr");
        no_armhf(a);
        if !p.truthy("desc") {
            a.set(
                "desc",
                Val::Str(format!(
                    "{} on Unity's 64-bit Android player (its own code on Unity's Mono, shipped with the port).",
                    s(&p, "title")
                )),
            );
        }
    }
    write_port_json(ctx, &name, &pj)
}

/// apk.py port_new: port.json for the apk route.
fn apk_port_new(ctx: &mut NewCtx) -> Result<()> {
    let p = ctx.cfg.dict("port").clone();
    if p.str("route") != "apk" {
        return Ok(());
    }
    let name = s(&p, "name");
    let Some(mut pj) = read_port_json(ctx, &name)? else {
        return Ok(());
    };
    let pl = ctx.cfg.dict("player");
    let abis: Vec<Val> = match pl.get("abis") {
        Some(v) if v.truthy() => v.as_list().map(<[Val]>::to_vec).unwrap_or_default(),
        _ => Val::strs(&["arm64-v8a", "armeabi-v7a"]).as_list().unwrap().to_vec(),
    };
    let il2cpp = pl.truthy("il2cpp");
    let what = s(ctx.cfg.dict("game"), "what");
    {
        let a = pj.as_dict_mut().unwrap().table("attr");
        let only_v7 = abis.len() == 1 && abis[0].eq(&Val::s("armeabi-v7a"));
        let mut reqs: Vec<Val> = if only_v7 && !il2cpp { vec![Val::s("armhf")] } else { vec![] };
        reqs.extend(a.list("reqs").iter().filter(|r| !r.eq(&Val::s("armhf"))).cloned());
        a.set("reqs", Val::List(reqs));
        if !p.truthy("desc") {
            a.set("desc", Val::Str(format!("{}: the game's own Android build, run on the handheld by ua_host.", s(&p, "title"))));
        }
        if !p.truthy("inst") {
            let t = s(&p, "setup_time");
            a.set(
                "inst",
                Val::Str(if !il2cpp {
                    format!("Requires {what}: put it in ports/{name}/gamedata. The first launch installs it ({t}; nothing is downloaded).")
                } else {
                    format!("Requires {what}: put it in ports/{name}/gamedata. The first launch installs it ({t}); on a handheld without a 32-bit userland it also compiles the game's code with Unity's IL2CPP (one to a few hours; about 1.6 GB free; Wi-Fi for Unity's and Debian's files).")
                }),
            );
        }
    }
    write_port_json(ctx, &name, &pj)
}

/// hashlib.sha256(data).hexdigest()
pub fn sha256_hex(data: &[u8]) -> String {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];
    let mut h: [u32; 8] =
        [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    let mut msg = data.to_vec();
    let bits = (data.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bits.to_be_bytes());
    for chunk in msg.chunks(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(chunk[i * 4..i * 4 + 4].try_into().unwrap());
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let mut v = h;
        for i in 0..64 {
            let s1 = v[4].rotate_right(6) ^ v[4].rotate_right(11) ^ v[4].rotate_right(25);
            let ch = (v[4] & v[5]) ^ (!v[4] & v[6]);
            let t1 = v[7].wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = v[0].rotate_right(2) ^ v[0].rotate_right(13) ^ v[0].rotate_right(22);
            let maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            let t2 = s0.wrapping_add(maj);
            v = [t1.wrapping_add(t2), v[0], v[1], v[2], v[3].wrapping_add(t1), v[4], v[5], v[6]];
        }
        for i in 0..8 {
            h[i] = h[i].wrapping_add(v[i]);
        }
    }
    h.iter().map(|x| format!("{x:08x}")).collect()
}

#[cfg(test)]
mod tests {
    #[test]
    fn sha256() {
        assert_eq!(super::sha256_hex(b"abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert_eq!(super::sha256_hex(b""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }

    #[test]
    fn splitlines() {
        assert_eq!(super::splitlines("a\nb\r\nc\rd\n"), vec!["a", "b", "c", "d"]);
        assert_eq!(super::splitlines(""), Vec::<&str>::new());
    }
}
