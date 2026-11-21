//! `port package`: the release zip of a port folder made by `port new` (port.py `package`, its
//! components, add_donors, slim_donor, armhf_files, write_submission, and route.py's box64
//! port_package).
use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::SystemTime;

use crate::config::AARCH64_ROUTES;
use crate::new::write_file;
use crate::routes::{donor_build, port_donors, read_text};
use crate::text::*;
use crate::val::{Dict, Val};
use crate::zipw::{Info, ZipOut};
use crate::{abspath, exit, mac_junk, ps, pyjson, relpath, rx, Repo};
use pkgzip::{io_at, Error, Result};

pub const EM_ARM: u16 = 40;
pub const EM_AARCH64: u16 = 183;
const ARCHIVE_EXT: [&str; 6] = [".apk", ".zip", ".obb", ".aab", ".xapk", ".apks"];

pub const DONOR_LICENSE: &str = "donor/*.apk: small games built with Unity (Unity Personal) for this port: a game of pong (one
scene, one script; portmaster-unity's tools/donor). Each carries Unity's Android player of its Unity
version (the Unity runtime), which the setup uses to run the player's own copy of the game. The Unity
runtime is Unity Technologies' software, distributed as part of these Made-with-Unity applications
under Unity's terms of service: https://unity.com/legal/terms-of-service
";

pub const CECIL_LICENSE: &str = "Mono.Cecil 0.11.6 (in asmpatch)

Copyright (c) 2008 - 2015 Jb Evain
Copyright (c) 2008 - 2011 Novell, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the \"Software\"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
";

/// What `port package` is given besides the port folder (port.py's package() options; the
/// *_arm64 and mono64_bridge ones only through the API, as in the Python).
#[derive(Clone, Debug, Default)]
pub struct PackOpts {
    pub out: Option<String>,
    pub runtime: Option<String>,
    pub donors: Option<String>,
    pub ua_host: Option<String>,
    pub ua_host_arm64: Option<String>,
    pub media_licenses: Option<String>,
    pub media_licenses_arm64: Option<String>,
    pub pybundle: Option<String>,
    pub pmu: Option<String>,
    pub asmpatch_bin: Option<String>,
    pub pmhlslcc_bin: Option<String>,
    pub pmspirvcross_bin: Option<String>,
    pub mono64_work: Option<String>,
    pub mono64_bridge: Option<String>,
    pub no_build: bool,
    pub submission: bool,
}

/// opts.get(k) or os.environ.get(env): a value that is not empty.
fn given(v: &Option<String>, env: Option<&str>) -> Option<String> {
    v.clone()
        .filter(|x| !x.is_empty())
        .or_else(|| env.and_then(|e| std::env::var(e).ok()).filter(|x| !x.is_empty()))
}

/// What the components and hooks get: add(), license(), the config and the folders.
pub struct PackCtx<'a> {
    pub repo: &'a Repo,
    pub cfg: Dict,
    pub port_src: PathBuf,
    pub zipdir: PathBuf,
    pub runtime: PathBuf,
    pub name: String,
    pub port_dir: PathBuf,
    pub licenses: PathBuf,
    /// {path in the zip: mode}: the files add() was given a mode for.
    pub modes: BTreeMap<String, u32>,
    /// builds older than their sources, said at the end
    pub stale: Vec<String>,
    pub log: &'a mut dyn FnMut(&str),
}

/// shutil.copyfile: the bytes, into a file made with the default mode (or the one it had).
pub fn copyfile(src: &Path, dst: &Path) -> Result<()> {
    let mut i = fs::File::open(src).map_err(io_at(src))?;
    let mut o = fs::File::create(dst).map_err(io_at(dst))?;
    std::io::copy(&mut i, &mut o).map_err(io_at(dst))?;
    Ok(())
}

/// shutil.copytree(src, dst, dirs_exist_ok=True, ignore=...): files with their modes (copy2),
/// symbolic links followed; `skip(dir, name)` leaves a name out.
pub fn copytree(src: &Path, dst: &Path, skip: &dyn Fn(&Path, &str) -> bool) -> Result<()> {
    fs::create_dir_all(dst).map_err(io_at(dst))?;
    let mut names: Vec<String> = fs::read_dir(src)
        .map_err(io_at(src))?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    for n in names {
        if skip(src, &n) {
            continue;
        }
        let (s, d) = (src.join(&n), dst.join(&n));
        if s.is_dir() {
            copytree(&s, &d, skip)?;
        } else {
            copyfile(&s, &d)?;
            let m = fs::metadata(&s).map_err(io_at(&s))?.permissions().mode() & 0o7777;
            let _ = fs::set_permissions(&d, fs::Permissions::from_mode(m));
        }
    }
    let m = fs::metadata(src).map_err(io_at(src))?.permissions().mode() & 0o7777;
    let _ = fs::set_permissions(dst, fs::Permissions::from_mode(m));
    Ok(())
}

fn ignore_patterns(_: &Path, n: &str) -> bool {
    n == "__pycache__" || n.ends_with(".pyc") || mac_junk(n)
}

impl PackCtx<'_> {
    fn log(&mut self, m: &str) {
        (self.log)(m)
    }

    /// Copy src (a file or folder) to <port folder>/rel; mode: a file's Unix mode in the release.
    pub fn add(&mut self, src: &Path, rel: &str, mode: Option<u32>) -> Result<PathBuf> {
        let dst = PathBuf::from(pkgzip::py::join(&ps(&self.port_dir), rel));
        let parent = PathBuf::from(pkgzip::py::dirname(&ps(&dst)));
        fs::create_dir_all(&parent).map_err(io_at(&parent))?;
        if src.is_dir() {
            copytree(src, &dst, &ignore_patterns)?;
        } else {
            copyfile(src, &dst)?;
            let key = relpath(&dst, &self.zipdir);
            self.modes.remove(&key);
            if let Some(m) = mode {
                let _ = fs::set_permissions(&dst, fs::Permissions::from_mode(m));
                self.modes.insert(key, m);
            }
        }
        Ok(dst)
    }

    pub fn license_file(&mut self, name: &str, path: &Path) -> Result<()> {
        if mac_junk(name) {
            return Ok(());
        }
        fs::create_dir_all(&self.licenses).map_err(io_at(&self.licenses))?;
        copyfile(path, &self.licenses.join(name))
    }

    pub fn license_text(&mut self, name: &str, text: &str) -> Result<()> {
        if mac_junk(name) {
            return Ok(());
        }
        fs::create_dir_all(&self.licenses).map_err(io_at(&self.licenses))?;
        write_file(&self.licenses.join(name), text.as_bytes())
    }

    fn run(&mut self, what: &str, cmd: &mut Command) -> Result<()> {
        self.log(what);
        let st = cmd.status().map_err(|e| Error::Fail(format!("{:?}: {e}", cmd.get_program())))?;
        if !st.success() {
            return Err(Error::Fail(format!(
                "subprocess.CalledProcessError: Command '{:?}' returned non-zero exit status {}.",
                cmd.get_program(),
                st.code().unwrap_or(-1)
            )));
        }
        Ok(())
    }
}

// ---- builds older than their sources

pub fn build_sources(what: &str) -> &'static [&'static str] {
    match what {
        "pmu" => &["setup-rs/Cargo.toml", "setup-rs/Cargo.lock", "setup-rs/pmu", "setup-rs/crates", "setup-rs/vendor"],
        "ua_host" => &[
            "runtime/build.sh", "runtime/host", "runtime/jni", "runtime/loader", "runtime/android", "runtime/media",
            "runtime/thirdparty",
        ],
        "asmpatch" => &["tools/asmpatch/build.sh", "tools/asmpatch/src"],
        "mono64" => &["runtime/mono64"],
        _ => &[],
    }
}

const SOURCES_SKIP: [&str; 6] = ["out", "test", "tests", "target", "docker", "__pycache__"];

fn mtime(p: &Path) -> Option<SystemTime> {
    fs::metadata(p).ok()?.modified().ok()
}

/// The files of `sources` (under root) changed after `built` was made, the newest first.
pub fn newer_sources(built: &Path, root: &Path, sources: &[&str]) -> Vec<String> {
    let Some(t) = mtime(built) else { return vec![] };
    let mut hits: Vec<(SystemTime, String)> = Vec::new();
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = fs::read_dir(d) else { return };
        let mut ents: Vec<(String, bool, bool)> = rd
            .filter_map(|e| e.ok())
            .map(|e| {
                let p = e.path();
                let isdir = p.is_dir();
                let link = e.file_type().map(|t| t.is_symlink()).unwrap_or(false);
                (e.file_name().to_string_lossy().into_owned(), isdir, link)
            })
            .collect();
        ents.sort();
        for (n, isdir, _) in &ents {
            if !isdir && !mac_junk(n) && !n.starts_with('.') {
                out.push(d.join(n));
            }
        }
        for (n, isdir, link) in &ents {
            if *isdir && !link && !SOURCES_SKIP.contains(&n.as_str()) && !n.starts_with('.') {
                walk(&d.join(n), out);
            }
        }
    }
    for rel in sources {
        let top = PathBuf::from(pkgzip::py::join(&ps(root), rel));
        let mut files = Vec::new();
        if top.is_file() {
            files.push(top.clone());
        }
        walk(&top, &mut files);
        for f in files {
            if let Some(m) = mtime(&f) {
                if m > t {
                    hits.push((m, ps(&f)));
                }
            }
        }
    }
    hits.sort();
    hits.reverse();
    hits.into_iter().map(|(_, f)| relpath(Path::new(&f), root)).collect()
}

/// Notes a build of the checkout older than a file it is made from.
pub fn stale_build(stale: &mut Vec<String>, what: &str, built: &Path, root: &Path, rebuild: &str) {
    let newer = newer_sources(built, root, build_sources(what));
    if !newer.is_empty() {
        let more = if newer.len() > 1 { format!(" and {} more", newer.len() - 1) } else { String::new() };
        stale.push(format!(
            "{} is older than {}{more}: the release has it as it was built; build it again ({rebuild}) and package again",
            relpath(built, root),
            newer[0]
        ));
    }
}

/// One text with the licences inside the setup's Python bundle.
pub fn pybundle_licenses(tar_path: &str) -> Result<String> {
    let mut parts: Vec<String> = vec![
        "The setup's Python (removed from the handheld when the setup is done): python-build-standalone's".into(),
        "CPython and these packages. Their licences follow.".into(),
    ];
    for m in crate::tar::members(tar_path)? {
        if !m.is_file || !rx::pybundle_license(&m.name) {
            continue;
        }
        let rel = match m.name.strip_prefix("python/lib/python3.") {
            Some(r) => {
                let d = r.bytes().take_while(u8::is_ascii_digit).count();
                if d > 0 && r[d..].starts_with('/') { r[d + 1..].to_string() } else { m.name.clone() }
            }
            None => m.name.clone(),
        };
        parts.push(format!("\n\n==== {rel} ====\n"));
        parts.push(String::from_utf8_lossy(&m.data).into_owned());
    }
    Ok(parts.join("\n"))
}

// ---- the components a route's release takes (route.json "package")

fn ua_host_part(ctx: &mut PackCtx, o: &UaOpts, dst: &str, suffix: &str) -> Result<()> {
    let p = o.ua_host.clone().map(PathBuf::from).unwrap_or_else(|| ctx.runtime.join("runtime/host/out/ua_host"));
    if !p.is_file() {
        if o.no_build {
            return Err(exit(format!("{}: build the runtime first (runtime/build.sh)", p.display())));
        }
        let build = ctx.runtime.join("runtime").join("build.sh");
        ctx.run("building the runtime (runtime/build.sh, Docker)", &mut Command::new(build))?;
    }
    if o.ua_host.is_none() || o.ua_host_default {
        let arm = ps(&p).contains("/arm64/");
        let mut st = std::mem::take(&mut ctx.stale);
        stale_build(&mut st, "ua_host", &p, &ctx.runtime, &format!("{}runtime/build.sh", if arm { "ARCH=arm64 " } else { "" }));
        ctx.stale = st;
    }
    ctx.add(&p, dst, Some(0o755))?;
    if dst == "ua_host" && uses_flipbook(&ctx.cfg) {
        let mut fb = p.parent().unwrap_or(Path::new("/")).join("uaflipbook.dll");
        if !fb.is_file() {
            fb = ctx.runtime.join("runtime/host/out/uaflipbook.dll");
        }
        if !fb.is_file() {
            return Err(exit(format!(
                "{}: the port sets UA_FLIPBOOK: build it (runtime/host/managed/build.sh <donor APK>)",
                fb.display()
            )));
        }
        ctx.add(&fb, "uaflipbook.dll", None)?;
    }
    let mut hyb = ctx.runtime.join("runtime/thirdparty/libhybris/LICENSE.Apache2");
    if !hyb.is_file() {
        hyb = ctx.repo.root.join("runtime/thirdparty/libhybris/LICENSE.Apache2");
    }
    ctx.license_file("LICENSE.libhybris.txt", &hyb)?;
    let ml = match &o.media_licenses {
        Some(m) => PathBuf::from(m),
        None if o.ua_host.is_some() => {
            let up3 = p.parent().and_then(Path::parent).and_then(Path::parent).unwrap_or(Path::new("/"));
            up3.join("media/out/licenses")
        }
        None => ctx.runtime.join("runtime/media/out/licenses"),
    };
    let mut lic: Vec<String> = match fs::read_dir(&ml) {
        Ok(rd) if ml.is_dir() => rd
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| (n.ends_with(".copyright") || n.ends_with(".LICENSE")) && !mac_junk(n))
            .collect(),
        _ => vec![],
    };
    lic.sort();
    if lic.is_empty() {
        return Err(exit(format!(
            "no decoder licences in {} (ua_host links the video decoders: build the runtime, or --media-licenses DIR)",
            ml.display()
        )));
    }
    for n in lic {
        let name = format!("LICENSE.{}.txt", n.rsplit_once('.').map_or(n.as_str(), |x| x.0));
        let old = ctx.licenses.join(&name);
        let mut text = read_text(&ml.join(&n))?;
        if !suffix.is_empty() && old.is_file() {
            let prev = read_text(&old)?;
            if prev != text {
                text = format!("{prev}\n\n==== {dst} ({}) ====\n\n{text}", suffix.trim_matches('.'));
            }
        }
        ctx.license_text(&name, &text)?;
    }
    Ok(())
}

/// The options _component_ua_host reads (opts, or the arm64 part's own).
struct UaOpts {
    ua_host: Option<String>,
    ua_host_default: bool,
    media_licenses: Option<String>,
    no_build: bool,
}

fn ua_host_arm64_part(ctx: &mut PackCtx, opts: &PackOpts, dst: &str, suffix: &str) -> Result<()> {
    let rt = ctx.runtime.join("runtime");
    let own = given(&opts.ua_host_arm64, None);
    let o = UaOpts {
        ua_host: Some(own.clone().unwrap_or_else(|| ps(&rt.join("host/out/arm64/ua_host")))),
        ua_host_default: own.is_none(),
        media_licenses: Some(
            given(&opts.media_licenses_arm64, None)
                .or_else(|| if suffix.is_empty() { given(&opts.media_licenses, None) } else { None })
                .unwrap_or_else(|| ps(&rt.join("media/out/arm64/licenses"))),
        ),
        no_build: opts.no_build,
    };
    if !Path::new(o.ua_host.as_ref().unwrap()).is_file() && !opts.no_build {
        ctx.run(
            "building the 64-bit runtime (runtime/build.sh, Docker)",
            Command::new(rt.join("build.sh")).env("ARCH", "arm64"),
        )?;
    }
    ua_host_part(ctx, &o, dst, suffix)?;
    // the code layout lists (runtime/relayout/<build id>.txt), beside every 64-bit ua_host
    let rl = ctx.runtime.join("runtime/relayout");
    if rl.is_dir() {
        let mut names: Vec<String> = fs::read_dir(&rl)
            .map_err(io_at(&rl))?
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| n.ends_with(".txt") && !mac_junk(n))
            .collect();
        names.sort();
        for n in names {
            ctx.add(&rl.join(&n), &format!("relayout/{n}"), Some(0o644))?;
        }
    }
    Ok(())
}

fn component(ctx: &mut PackCtx, c: &str, opts: &PackOpts) -> Result<bool> {
    match c {
        "ua_host" => {
            let o = UaOpts {
                ua_host: given(&opts.ua_host, None),
                ua_host_default: false,
                media_licenses: given(&opts.media_licenses, None),
                no_build: opts.no_build,
            };
            ua_host_part(ctx, &o, "ua_host", "")?;
        }
        "ua_host_arm64" => ua_host_arm64_part(ctx, opts, "ua_host", "")?,
        "ua_host64" => ua_host_arm64_part(ctx, opts, "ua_host64", ".ua_host64")?,
        "pmunity" => pmunity_part(ctx)?,
        "pybundle" => pybundle_part(ctx, opts)?,
        "pmu" => pmu_part(ctx, opts)?,
        "asmpatch" => asmpatch_part(ctx, opts)?,
        "pmhlslcc" => tool_part(ctx, opts, "pmhlslcc")?,
        "pmspirvcross" => tool_part(ctx, opts, "pmspirvcross")?,
        "mono64" => mono64_part(ctx, opts)?,
        _ => return Ok(false),
    }
    Ok(true)
}

fn is_component(c: &str) -> bool {
    matches!(
        c,
        "ua_host" | "ua_host_arm64" | "ua_host64" | "pmunity" | "pybundle" | "pmu" | "asmpatch" | "pmhlslcc" | "pmspirvcross" | "mono64"
    )
}

/// The converter (convert/pmunity's .py and .shader files) as setup/pmunity, and the NDK stand-in.
fn pmunity_part(ctx: &mut PackCtx) -> Result<()> {
    let src = ctx.repo.pmunity();
    let top = src.parent().unwrap().to_path_buf();
    fn walk(d: &Path, out: &mut Vec<PathBuf>) -> Result<()> {
        let mut ents: Vec<(String, bool)> = fs::read_dir(d)
            .map_err(io_at(d))?
            .filter_map(|e| e.ok())
            .map(|e| (e.file_name().to_string_lossy().into_owned(), e.path().is_dir()))
            .collect();
        ents.sort();
        for (n, isdir) in &ents {
            if !isdir && (n.ends_with(".py") || n.ends_with(".shader")) && !mac_junk(n) {
                out.push(d.join(n));
            }
        }
        for (n, isdir) in &ents {
            if *isdir && n != "__pycache__" && n != "port_template" {
                walk(&d.join(n), out)?;
            }
        }
        Ok(())
    }
    let mut files = Vec::new();
    walk(&src, &mut files)?;
    for f in files {
        let rel = relpath(&f, &top);
        ctx.add(&f, &format!("setup/{rel}"), None)?;
    }
    let ndk = ctx.repo.root.join("tools/il2cpp/ndk-clang.sh");
    ctx.add(&ndk, "setup/tools/il2cpp/ndk-clang.sh", Some(0o755))?;
    Ok(())
}

fn pmu_part(ctx: &mut PackCtx, opts: &PackOpts) -> Result<()> {
    let own = given(&opts.pmu, Some("PMU_DIR"));
    let d = match &own {
        Some(d) => PathBuf::from(d),
        None => {
            let d = ctx.repo.root.join("tools/port/out/pmu");
            if !d.join("pmu").is_file() {
                if opts.no_build {
                    return Err(exit("the setup's tools: --pmu DIR (setup-rs/tools/build_pmu.sh builds it)"));
                }
                let b = ctx.repo.root.join("setup-rs/tools/build_pmu.sh");
                ctx.run("building the setup's tools, pmu (setup-rs/tools/build_pmu.sh, Docker)", Command::new(b).arg(&d))?;
            }
            d
        }
    };
    if !d.join("pmu").is_file() || !d.join("licenses").is_dir() {
        return Err(exit(format!("{}: no pmu and licenses/ (setup-rs/tools/build_pmu.sh {})", d.display(), d.display())));
    }
    if own.is_none() {
        let rebuild = format!("setup-rs/tools/build_pmu.sh {}", relpath(&d, &ctx.repo.root));
        let root = ctx.repo.root.clone();
        stale_build(&mut ctx.stale, "pmu", &d.join("pmu"), &root, &rebuild);
    }
    ctx.add(&d.join("pmu"), "setup/pmu", Some(0o755))?;
    let mut names: Vec<String> = fs::read_dir(d.join("licenses"))
        .map_err(io_at(d.join("licenses")))?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    for n in names {
        ctx.license_file(&n, &d.join("licenses").join(&n))?;
    }
    Ok(())
}

fn pybundle_part(ctx: &mut PackCtx, opts: &PackOpts) -> Result<()> {
    let p = match given(&opts.pybundle, Some("PYBUNDLE")) {
        Some(p) => PathBuf::from(p),
        None => {
            let p = ctx.repo.root.join("tools/port/out/python.tar.gz");
            if !p.is_file() {
                if opts.no_build {
                    return Err(exit("the setup's Python: --pybundle FILE (tools/port/build_pybundle.sh builds it)"));
                }
                let b = ctx.repo.root.join("tools/port/build_pybundle.sh");
                ctx.run("building the setup's Python (tools/port/build_pybundle.sh)", Command::new(b).arg(&p))?;
            }
            p
        }
    };
    ctx.add(&p, "setup/python.tar.gz", None)?;
    let text = pybundle_licenses(&ps(&p))?;
    ctx.license_text("LICENSE.setup-python.txt", &text)
}

fn version_key(v: &str) -> Vec<u64> {
    v.split(['.', '-']).map(|x| if !x.is_empty() && x.bytes().all(|c| c.is_ascii_digit()) { x.parse().unwrap_or(0) } else { 0 }).collect()
}

fn asmpatch_part(ctx: &mut PackCtx, opts: &PackOpts) -> Result<()> {
    if !needs_asmpatch(&ctx.cfg) {
        return Ok(());
    }
    let own = given(&opts.asmpatch_bin, None);
    let p = own.clone().map(PathBuf::from).unwrap_or_else(|| ctx.runtime.join("tools/asmpatch/out/linux-arm64/asmpatch"));
    if !p.is_file() {
        if opts.no_build {
            return Err(exit(format!("{}: build asmpatch first (tools/asmpatch/build.sh)", p.display())));
        }
        let b = ctx.runtime.join("tools/asmpatch/build.sh");
        ctx.run("building asmpatch (tools/asmpatch/build.sh, .NET 8 SDK)", &mut Command::new(b))?;
    }
    if own.is_none() {
        let rt = ctx.runtime.clone();
        stale_build(&mut ctx.stale, "asmpatch", &p, &rt, "tools/asmpatch/build.sh");
    }
    ctx.add(&p, "setup/asmpatch", Some(0o755))?;
    let nuget = std::env::var("NUGET_PACKAGES")
        .ok()
        .filter(|x| !x.is_empty())
        .unwrap_or_else(|| format!("{}/.nuget/packages", std::env::var("HOME").unwrap_or_default()));
    let rt = Path::new(&nuget).join("microsoft.netcore.app.runtime.linux-arm64");
    let mut vers: Vec<String> = match fs::read_dir(&rt) {
        Ok(rd) => rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|v| v.starts_with("8.")).collect(),
        Err(_) => vec![],
    };
    vers.sort_by_key(|v| version_key(v));
    let Some(v) = vers.last() else {
        return Err(exit(format!(
            "{}: the .NET runtime pack (its LICENSE.TXT and THIRD-PARTY-NOTICES.TXT go with asmpatch)",
            rt.display()
        )));
    };
    let text = format!(
        "{}\n\n{}",
        read_text(&rt.join(v).join("LICENSE.TXT"))?,
        read_text(&rt.join(v).join("THIRD-PARTY-NOTICES.TXT"))?
    );
    ctx.license_text("LICENSE.dotnet-runtime.txt", &text)?;
    ctx.license_text("LICENSE.mono-cecil.txt", CECIL_LICENSE)
}

/// pmhlslcc (HLSLcc) or pmspirvcross (SPIRV-Cross, SMOL-V) for linux-arm64, with its licence.
fn tool_part(ctx: &mut PackCtx, opts: &PackOpts, tool: &str) -> Result<()> {
    let (own, dir, what, lic_name, lic_what) = if tool == "pmhlslcc" {
        (&opts.pmhlslcc_bin, "hlslcc", "pmhlslcc for the handheld", "LICENSE.pmhlslcc-HLSLcc.txt", "HLSLcc's licence goes")
    } else {
        (
            &opts.pmspirvcross_bin,
            "spirvcross",
            "pmspirvcross for the handheld",
            "LICENSE.pmspirvcross-SPIRV-Cross-SMOL-V.txt",
            "SPIRV-Cross's and SMOL-V's licences go",
        )
    };
    let p = given(own, None)
        .map(PathBuf::from)
        .unwrap_or_else(|| ctx.runtime.join(format!("tools/{dir}/out/linux-arm64/{tool}")));
    if !p.is_file() {
        if opts.no_build {
            return Err(exit(format!("{}: build {what} first (ARCH=arm64 tools/{dir}/build.sh)", p.display())));
        }
        let b = ctx.runtime.join(format!("tools/{dir}/build.sh"));
        let label = if tool == "pmhlslcc" {
            "building pmhlslcc for linux-arm64 (ARCH=arm64 tools/hlslcc/build.sh, Docker)"
        } else {
            "building pmspirvcross for linux-arm64 (ARCH=arm64 tools/spirvcross/build.sh, Docker)"
        };
        ctx.run(label, Command::new(b).arg(p.parent().unwrap()).env("ARCH", "arm64"))?;
    }
    let lic = p.parent().unwrap_or(Path::new("/")).join(format!("{tool}.LICENSE.txt"));
    if !lic.is_file() {
        let writes = if tool == "pmhlslcc" { "(tools/hlslcc/build.sh writes it)" } else { "(tools/spirvcross/build.sh writes them)" };
        return Err(exit(format!("{}: {lic_what} with {tool} {writes}", lic.display())));
    }
    ctx.add(&p, &format!("setup/{tool}"), Some(0o755))?;
    ctx.license_file(lic_name, &lic)
}

fn mono64_part(ctx: &mut PackCtx, opts: &PackOpts) -> Result<()> {
    let lines = mono64_lines(&ctx.cfg);
    if lines.is_empty() {
        return Err(exit("game.unity_versions: the mono-arm64 route ships the Mono runtime of the game's Unity line"));
    }
    let rt = ctx.runtime.clone();
    let own = given(&opts.mono64_bridge, None);
    let bridge = own.clone().map(PathBuf::from).unwrap_or_else(|| rt.join("runtime/mono64/out/arm64/libil2cpp_mono.so"));
    if !bridge.is_file() {
        if opts.no_build {
            return Err(exit(format!("{}: build the Mono bridge first (runtime/mono64/build.sh)", bridge.display())));
        }
        ctx.run("building the Mono bridge (runtime/mono64/build.sh, Docker)", &mut Command::new(rt.join("runtime/mono64/build.sh")))?;
    }
    if own.is_none() {
        stale_build(&mut ctx.stale, "mono64", &bridge, &rt, "runtime/mono64/build.sh");
    }
    let work = given(&opts.mono64_work, Some("PMUNITY_MONO64_WORK")).map(PathBuf::from).unwrap_or_else(|| rt.join("tools/mono64/work"));
    for line in lines {
        let branch = format!("unity-{line}-mbe");
        let out = work.join("out").join(&branch);
        if !out.join("libmonobdwgc-2.0.so").is_file() {
            if opts.no_build {
                return Err(exit(format!(
                    "{}: build Unity's Mono for {line} first (tools/mono64/build-mono.sh {line} {})",
                    out.display(),
                    work.display()
                )));
            }
            ctx.run(
                &format!("building Unity's Mono {branch} for aarch64 Linux (tools/mono64/build-mono.sh, Docker)"),
                Command::new(rt.join("tools/mono64/build-mono.sh")).arg(&line).arg(&work),
            )?;
        }
        ctx.add(&bridge, &format!("setup/mono64/{line}/libil2cpp.so"), Some(0o755))?;
        for n in ["libmonobdwgc-2.0.so", "libMonoPosixHelper.so", "libmono-native.so"] {
            if out.join(n).is_file() {
                ctx.add(&out.join(n), &format!("setup/mono64/{line}/{n}"), Some(0o755))?;
            }
        }
        let src = work.join("src").join(&branch);
        let pat = ["PATENTS.TXT", "Microsoft Patent Promise.TXT"]
            .iter()
            .map(|n| src.join(n))
            .find(|p| p.is_file())
            .ok_or_else(|| Error::Fail(format!("StopIteration: no PATENTS.TXT in {}", src.display())))?;
        let text = format!("{}\n\n{}", read_text(&src.join("LICENSE"))?, read_text(&pat)?);
        ctx.license_text(&format!("LICENSE.mono-{line}.txt"), &text)?;
        ctx.license_file(&format!("LICENSE.bdwgc-{line}.txt"), &src.join("external/bdwgc/README.QUICK"))?;
        let cf = src.join("external/corefx/LICENSE.TXT");
        if out.join("libmono-native.so").is_file() && cf.is_file() {
            ctx.license_file(&format!("LICENSE.corefx-{line}.txt"), &cf)?;
        }
        if ctx.cfg.dict("player").truthy("mono64_classlibs") {
            mono64_classlibs(ctx, opts, &line, &work, &out)?;
        }
    }
    Ok(())
}

/// A .NET 3.5 game's class libraries for the mono-arm64 route (player.mono64_classlibs): Mono's 4.x
/// ones (tools/mono64/build-classlibs.sh) as setup/mono64/<line>/classlibs/, with Mono's config as
/// setup/mono64/<line>/etc/.
pub fn mono64_classlibs(ctx: &mut PackCtx, opts: &PackOpts, line: &str, work: &Path, out: &Path) -> Result<()> {
    use crate::config::{MONO64_CLASSLIBS, MONO64_NET35_LINES};
    if !MONO64_NET35_LINES.contains(&line) {
        return Err(exit(format!(
            "player.mono64_classlibs: Unity {line} (a .NET 3.5 game on Mono's 4.x class libraries: {})",
            MONO64_NET35_LINES.join(", ")
        )));
    }
    let cl = out.join("classlibs");
    if !cl.join("mscorlib.dll").is_file() {
        if opts.no_build {
            return Err(exit(format!(
                "{}: build Mono's 4.x class libraries first (tools/mono64/build-classlibs.sh {line} {})",
                cl.display(),
                work.display()
            )));
        }
        let b = ctx.runtime.join("tools/mono64/build-classlibs.sh");
        ctx.run(
            &format!("building Mono's 4.x class libraries of unity-{line}-mbe (tools/mono64/build-classlibs.sh, Docker)"),
            Command::new(b).arg(line).arg(work),
        )?;
    }
    let names: Vec<String> = match ctx.cfg.dict("player").get("mono64_classlibs") {
        Some(Val::List(l)) => l.iter().map(Val::py_str).collect(),
        _ => MONO64_CLASSLIBS.iter().map(|s| s.to_string()).collect(),
    };
    let missing: Vec<&str> = names.iter().filter(|n| !cl.join(n).is_file()).map(String::as_str).collect();
    if !missing.is_empty() {
        return Err(exit(format!(
            "{} has no {} (tools/mono64/build-classlibs.sh {line} {} builds the whole profile)",
            cl.display(),
            missing.join(", "),
            work.display()
        )));
    }
    for n in &names {
        ctx.add(&cl.join(n), &format!("setup/mono64/{line}/classlibs/{n}"), Some(0o644))?;
    }
    for rel in ["mono/config", "mono/4.5/machine.config"] {
        let sp = cl.join("etc").join(rel);
        if !sp.is_file() {
            return Err(exit(format!(
                "{}: missing (tools/mono64/build-classlibs.sh writes Mono's config beside the libraries)",
                sp.display()
            )));
        }
        ctx.add(&sp, &format!("setup/mono64/{line}/etc/{rel}"), Some(0o644))?;
    }
    if cl.join("LICENSE.txt").is_file() {
        ctx.license_file(&format!("LICENSE.mono-class-libraries-{line}.txt"), &cl.join("LICENSE.txt"))?;
    }
    Ok(())
}

/// route.py port_package: the box64 route's box64, glxes, gl4es, box64.env, unityprefs.sh.
fn box64_port_package(ctx: &mut PackCtx) -> Result<()> {
    if ctx.cfg.dict("port").str("route") != "box64" {
        return Ok(());
    }
    let out = std::env::var("BOX64_OUT")
        .ok()
        .filter(|x| !x.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| ctx.runtime.join("tools/box64/out"));
    let need = ["box64/box64", "box64/x64lib", "glxes/libGL.so.1", "gl4es/libGL.so.1", "licenses"];
    let missing: Vec<&str> = need.iter().filter(|n| !out.join(n).exists()).copied().collect();
    if !missing.is_empty() {
        return Err(exit(format!(
            "box64 route: {} lacks {} (run tools/box64/build.sh, or set BOX64_OUT)",
            out.display(),
            missing.join(", ")
        )));
    }
    ctx.add(&out.join("box64/box64"), "box64/box64", Some(0o755))?;
    ctx.add(&out.join("box64/x64lib"), "box64/x64lib", None)?;
    ctx.add(&out.join("glxes/libGL.so.1"), "glxes/libGL.so.1", None)?;
    ctx.add(&out.join("gl4es/libGL.so.1"), "gl4es/libGL.so.1", None)?;
    let tools = ctx.runtime.join("tools/box64");
    for n in ["box64.env", "unityprefs.sh"] {
        ctx.add(&tools.join(n), &format!("box64/{n}"), None)?;
    }
    let mut names: Vec<String> = fs::read_dir(out.join("licenses"))
        .map_err(io_at(out.join("licenses")))?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    for n in names {
        ctx.license_file(&n, &out.join("licenses").join(&n))?;
    }
    Ok(())
}

// ---- ELF files

/// An ELF file's e_machine from its first 20 bytes.
pub fn elf_machine(head: &[u8]) -> Option<u16> {
    if head.len() < 20 || &head[..4] != b"\x7fELF" {
        return None;
    }
    let b = [head[18], head[19]];
    Some(if head[5] == 2 { u16::from_be_bytes(b) } else { u16::from_le_bytes(b) })
}

fn head(p: &Path, n: usize) -> Vec<u8> {
    let mut v = Vec::new();
    if let Ok(f) = fs::File::open(p) {
        let _ = f.take(n as u64).read_to_end(&mut v);
    }
    v
}

/// The 32-bit ARM ELF files under root, and in its archives' entries, relative to rel_to
/// (`<archive>!<entry>`).
pub fn armhf_files(root: &Path, rel_to: &Path) -> Result<Vec<String>> {
    let mut out = Vec::new();
    fn walk(d: &Path, rel_to: &Path, out: &mut Vec<String>) -> Result<()> {
        let Ok(rd) = fs::read_dir(d) else { return Ok(()) };
        let mut ents: Vec<(String, bool, bool)> = rd
            .filter_map(|e| e.ok())
            .map(|e| {
                let link = e.file_type().map(|t| t.is_symlink()).unwrap_or(false);
                (e.file_name().to_string_lossy().into_owned(), e.path().is_dir(), link)
            })
            .collect();
        ents.sort();
        for (n, isdir, _) in &ents {
            if *isdir {
                continue;
            }
            let p = d.join(n);
            let rel = relpath(&p, rel_to);
            let h = head(&p, 20);
            if elf_machine(&h) == Some(EM_ARM) {
                out.push(rel);
            } else if h.starts_with(b"PK\x03\x04") || ARCHIVE_EXT.iter().any(|e| n.to_lowercase().ends_with(e)) {
                let z = pkgzip::zip::Archive::open(&ps(&p))?;
                for e in &z.entries {
                    if !e.is_dir() && e.file_size >= 20 {
                        let mut b = Vec::new();
                        z.reader(e)?.take(20).read_to_end(&mut b)?;
                        if elf_machine(&b) == Some(EM_ARM) {
                            out.push(format!("{rel}!{}", e.name));
                        }
                    }
                }
            }
        }
        for (n, isdir, link) in &ents {
            if *isdir && !link {
                walk(&d.join(n), rel_to, out)?;
            }
        }
        Ok(())
    }
    walk(root, rel_to, &mut out)?;
    Ok(out)
}

/// The bytes of an ELF file's .debug_* sections; 0 for a stripped file or no ELF file.
pub fn elf_debug_bytes(path: &Path) -> u64 {
    fn go(path: &Path) -> Option<u64> {
        use std::io::{Seek, SeekFrom};
        let mut f = fs::File::open(path).ok()?;
        let mut h = Vec::new();
        (&mut f).take(64).read_to_end(&mut h).ok()?;
        if h.len() < 52 || &h[..4] != b"\x7fELF" || h[5] != 1 {
            return Some(0);
        }
        let wide = h[4] == 2;
        if wide && h.len() < 64 {
            return None;
        }
        let u16a = |i: usize| u16::from_le_bytes([h[i], h[i + 1]]) as usize;
        let (shoff, entsize, num, strndx) = if wide {
            (u64::from_le_bytes(h[0x28..0x30].try_into().unwrap()), u16a(0x3A), u16a(0x3C), u16a(0x3E))
        } else {
            (u32::from_le_bytes(h[0x20..0x24].try_into().unwrap()) as u64, u16a(0x2E), u16a(0x30), u16a(0x32))
        };
        if shoff == 0 || num == 0 || strndx >= num || entsize < (if wide { 64 } else { 40 }) || num > 4096 {
            return Some(0);
        }
        f.seek(SeekFrom::Start(shoff)).ok()?;
        let mut sh = Vec::new();
        (&mut f).take((entsize * num) as u64).read_to_end(&mut sh).ok()?;
        if sh.len() < entsize * num {
            return Some(0);
        }
        let size_at = if wide { 0x20 } else { 0x14 };
        let off_at = if wide { 0x18 } else { 0x10 };
        let word = |i: usize| -> u64 {
            if wide {
                u64::from_le_bytes(sh[i..i + 8].try_into().unwrap())
            } else {
                u32::from_le_bytes(sh[i..i + 4].try_into().unwrap()) as u64
            }
        };
        let size = |i: usize| word(i * entsize + size_at);
        let off = word(strndx * entsize + off_at);
        f.seek(SeekFrom::Start(off)).ok()?;
        let mut names = Vec::new();
        (&mut f).take(size(strndx).min(1 << 20)).read_to_end(&mut names).ok()?;
        let mut total = 0;
        for i in 0..num {
            let n = u32::from_le_bytes(sh[i * entsize..i * entsize + 4].try_into().unwrap()) as usize;
            if names.get(n..n + 7) == Some(b".debug_") {
                total += size(i);
            }
        }
        Some(total)
    }
    go(path).unwrap_or(0)
}

// ---- donors

/// A donor APK without its 32-bit player: every entry but those in lib/<abi>/ for another ABI
/// than arm64-v8a, written again as zipfile writes it. Returns the names left out.
pub fn slim_donor(src: &Path, dst: &Path) -> Result<Vec<String>> {
    let mut dropped = Vec::new();
    let tmp = PathBuf::from(format!("{}.tmp", ps(dst)));
    let zin = pkgzip::zip::Archive::open(&ps(src))?;
    let mut zout = ZipOut::create(&ps(&tmp))?;
    for e in &zin.entries {
        let parts: Vec<&str> = e.name.split('/').collect();
        if parts[0] == "lib" && parts.len() > 2 && parts[1] != "arm64-v8a" {
            dropped.push(e.name.clone());
            continue;
        }
        let mut data = Vec::new();
        zin.reader(e)?.read_to_end(&mut data)?;
        let info = Info {
            name: e.name.clone(),
            time: e.time,
            date: e.date,
            method: e.method,
            level: None,
            create_version: e.create_version,
            create_system: e.create_system,
            extract_version: e.extract_version,
            internal_attr: e.internal_attr,
            external_attr: e.external_attr,
            extra: e.extra.clone(),
            comment: e.comment.clone(),
        };
        zout.add(info, &data)?;
    }
    zout.finish()?;
    drop(zin);
    fs::rename(&tmp, dst).map_err(io_at(dst))?;
    Ok(dropped)
}

pub fn add_donors(ctx: &mut PackCtx, names: &[Val], src: Option<String>, aarch64_only: bool) -> Result<()> {
    if names.is_empty() {
        return Ok(());
    }
    let names: Vec<String> = names.iter().map(Val::py_str).collect();
    let how: Vec<String> = names
        .iter()
        .filter_map(|n| {
            rx::donor(n).map(|(v, b, a)| {
                let abi = if a == "armv7" { "armeabi-v7a" } else { "arm64-v8a" };
                format!("{n}: tools/donor/build.sh <Unity {v} editor> {} <dir>", donor_build(b, abi))
            })
        })
        .collect();
    let how = how.join("; ");
    let Some(src) = src else {
        return Err(exit(format!(
            "this port's setup takes Unity's player from donors, and no donor folder was given: --donors DIR (or $DONORS) with {} ({how}; docs/porting.md, \"Donors\")",
            names.join(", ")
        )));
    };
    let missing: Vec<&String> = names.iter().filter(|n| !Path::new(&pkgzip::py::join(&src, n)).is_file()).collect();
    if !missing.is_empty() {
        return Err(exit(format!(
            "{src} has no {}: the release would ship no player for them ({how}; docs/porting.md, \"Donors\")",
            missing.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(", ")
        )));
    }
    let mut bad = Vec::new();
    for n in &names {
        if !pkgzip::zip::is_zipfile(&pkgzip::py::join(&src, n))? {
            bad.push(n.as_str());
        }
    }
    if !bad.is_empty() {
        return Err(exit(format!("{src}: {} is not an APK", bad.join(", "))));
    }
    ctx.log(&format!("== donors: {}", names.join(", ")));
    for n in &names {
        let d = ctx.add(Path::new(&pkgzip::py::join(&src, n)), &format!("donor/{n}"), Some(0o644))?;
        if aarch64_only {
            let gone = slim_donor(&d, &d)?;
            if !gone.is_empty() {
                ctx.log(&format!("== donor/{n}: without its 32-bit player ({} files of lib/; aarch64 only)", gone.len()));
            }
        }
    }
    ctx.license_text("LICENSE.unity-donor.txt", DONOR_LICENSE)
}

// ---- the zip

/// packed_mode: 0o755 for an ELF file, else 0o644.
pub fn packed_mode(data: &[u8]) -> u32 {
    if data.starts_with(b"\x7fELF") { 0o755 } else { 0o644 }
}

pub fn is_program(head: &[u8]) -> bool {
    head.starts_with(b"\x7fELF") || head.starts_with(b"#!")
}

/// The files of root/<items>: a folder's files by name, then its folders by name (os.walk),
/// macOS's own files left out.
fn zip_paths(root: &Path, items: &[String]) -> Vec<PathBuf> {
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = fs::read_dir(d) else { return };
        let mut ents: Vec<(String, bool, bool)> = rd
            .filter_map(|e| e.ok())
            .map(|e| {
                let link = e.file_type().map(|t| t.is_symlink()).unwrap_or(false);
                (e.file_name().to_string_lossy().into_owned(), e.path().is_dir(), link)
            })
            .collect();
        ents.sort();
        for (n, isdir, _) in &ents {
            if !isdir && !mac_junk(n) {
                out.push(d.join(n));
            }
        }
        for (n, isdir, link) in &ents {
            if *isdir && !mac_junk(n) && !link {
                walk(&d.join(n), out);
            }
        }
    }
    let mut out = Vec::new();
    for item in items {
        let top = root.join(item);
        if top.is_file() {
            out.push(top.clone());
        }
        walk(&top, &mut out);
    }
    out
}

/// _write_zip: a zip of root/<items> with Unix modes and fixed timestamps, deflated at level 9.
pub fn write_zip(zpath: &Path, root: &Path, items: &[String], modes: &BTreeMap<String, u32>) -> Result<()> {
    let tmp = PathBuf::from(format!("{}.tmp", ps(zpath)));
    let mut z = ZipOut::create(&ps(&tmp))?;
    for p in zip_paths(root, items) {
        let rel = relpath(&p, root);
        let data = fs::read(&p).map_err(io_at(&p))?;
        let mode = modes.get(&rel).copied().filter(|m| *m != 0).unwrap_or_else(|| packed_mode(&data));
        let _ = fs::set_permissions(&p, fs::Permissions::from_mode(mode));
        z.add(Info::file(&rel, mode, 9), &data)?;
    }
    z.finish()?;
    fs::rename(&tmp, zpath).map_err(io_at(zpath))?;
    Ok(())
}

/// <out>/submission/<name>/: the port as PortMaster's repository holds it.
fn write_submission(ctx: &PackCtx, out: &Path, launcher: &str, imgs: &[(&str, Option<(String, String)>)]) -> Result<PathBuf> {
    let sub = out.join("submission").join(&ctx.name);
    let _ = fs::remove_dir_all(&sub);
    let mut top: Vec<String> = vec!["port.json".into(), "gameinfo.xml".into()];
    top.extend(imgs.iter().filter_map(|(_, v)| v.as_ref().map(|x| x.1.clone())));
    let port_dir = abspath(&ctx.port_dir);
    let skip = |d: &Path, n: &str| mac_junk(n) || (abspath(d) == port_dir && top.iter().any(|t| t == n));
    copytree(&ctx.port_dir, &sub.join(&ctx.name), &skip)?;
    for n in ["port.json", "gameinfo.xml"] {
        if ctx.port_dir.join(n).is_file() {
            copyfile(&ctx.port_dir.join(n), &sub.join(n))?;
        }
    }
    for (_, v) in imgs {
        if let Some((path, shipped)) = v {
            copyfile(Path::new(path), &sub.join(shipped))?;
        }
    }
    if ctx.port_src.join("README.md").is_file() {
        copyfile(&ctx.port_src.join("README.md"), &sub.join("README.md"))?;
    }
    copyfile(&ctx.zipdir.join(launcher), &sub.join(launcher))?;
    fs::set_permissions(sub.join(launcher), fs::Permissions::from_mode(0o644)).map_err(io_at(sub.join(launcher)))?;
    Ok(sub)
}

/// The release zip of a port folder made by `port new`; returns its path.
pub fn package(repo: &Repo, port_src: &str, opts: &PackOpts, log: &mut dyn FnMut(&str)) -> Result<PathBuf> {
    let port_src = abspath(Path::new(port_src));
    if std::env::var("PMUNITY_PORT_HOOKS").is_ok_and(|h| h.split(',').any(|m| !m.is_empty())) {
        return Err(exit("PMUNITY_PORT_HOOKS: pmu port runs no Python hook modules (run `python -m pmunity port package`)"));
    }
    let bj = port_src.join("port.build.json");
    if !bj.is_file() {
        return Err(exit(format!("{}: no port.build.json (make the port with `pmunity port new` first)", port_src.display())));
    }
    let build = pyjson::loads(&read_text(&bj)?)?;
    let bd = build.as_dict().cloned().unwrap_or_default();
    let cfg = bd.dict("config").clone();
    let route = bd.dict("route").clone();
    let name = cfg.dict("port").get("name").map_or("None".into(), Val::py_str);
    let title = cfg.dict("port").get("title").map_or("None".into(), Val::py_str);
    let out = abspath(Path::new(&opts.out.clone().filter(|x| !x.is_empty()).unwrap_or_else(|| ps(&port_src.join("out")))));
    let runtime = match opts.runtime.as_deref().filter(|x| !x.is_empty()) {
        Some(r) => abspath(Path::new(r)),
        None => repo.root.clone(),
    };
    let a64 = cfg.dict("port").truthy("aarch64_only");
    let rname = cfg.dict("port").get("route").map_or("None".into(), Val::py_str);
    if a64 && !AARCH64_ROUTES.contains(&rname.as_str()) {
        return Err(exit(format!(
            "port.aarch64_only: this port folder was made for the {rname} route (an armhf setup and player): run `port new` again, which takes the mono-arm64 route"
        )));
    }
    let zipdir = out.join("zip");
    let _ = fs::remove_dir_all(&zipdir);
    fs::create_dir_all(&zipdir).map_err(io_at(&zipdir))?;
    let port_dir = zipdir.join(&name);
    let mut ctx = PackCtx {
        repo,
        cfg: cfg.clone(),
        port_src: port_src.clone(),
        zipdir: zipdir.clone(),
        runtime,
        name: name.clone(),
        licenses: port_dir.join("licenses"),
        port_dir,
        modes: BTreeMap::new(),
        stale: Vec::new(),
        log,
    };
    let bld = cfg.dict("build");
    let build_dir = out.join("build");
    if bld.truthy("commands") {
        fs::create_dir_all(&build_dir).map_err(io_at(&build_dir))?;
        for cmd in bld.list("commands") {
            let cmd = cmd.py_str();
            ctx.log(&format!("== build: {cmd}"));
            let st = Command::new("/bin/sh")
                .arg("-c")
                .arg(&cmd)
                .current_dir(&port_src)
                .env("PMUNITY", &repo.root)
                .env("PORT_SRC", &port_src)
                .env("BUILD", &build_dir)
                .status()
                .map_err(|e| Error::Fail(format!("/bin/sh: {e}")))?;
            if !st.success() {
                return Err(Error::Fail(format!(
                    "subprocess.CalledProcessError: Command '{cmd}' returned non-zero exit status {}.",
                    st.code().unwrap_or(-1)
                )));
            }
        }
    }
    let launcher = format!("{title}.sh");
    copyfile(&port_src.join(&launcher), &zipdir.join(&launcher))?;
    fs::set_permissions(zipdir.join(&launcher), fs::Permissions::from_mode(0o644)).map_err(io_at(zipdir.join(&launcher)))?;
    ctx.add(&port_src.join(&name), ".", None)?;
    let donors = port_donors(&cfg, &route);
    add_donors(&mut ctx, &donors, given(&opts.donors, Some("DONORS")), a64)?;
    for c in route.list("package") {
        let c = c.py_str();
        let opt = c.ends_with('?');
        let c = c.trim_end_matches('?');
        if !is_component(c) {
            continue;
        }
        if (opt || c == "asmpatch") && !wants(c, &cfg) {
            continue;
        }
        ctx.log(&format!("== {c}"));
        component(&mut ctx, c, opts)?;
    }
    let bstr = ps(&build_dir);
    for (dst, src) in bld.dict("outputs").iter() {
        let sp = PathBuf::from(pkgzip::py::join(&ps(&port_src), &src.py_str().replace("{build}", &bstr)));
        if !sp.exists() {
            return Err(exit(format!("build.outputs: {} was not made", sp.display())));
        }
        if a64 && sp.is_file() && elf_machine(&head(&sp, 20)) == Some(EM_ARM) {
            ctx.log(&format!("build.outputs: {dst} not shipped: armhf, and the port is aarch64 only"));
            continue;
        }
        let mode = if sp.is_file() && is_program(&head(&sp, 4)) { Some(0o755) } else { None };
        let d = ctx.add(&sp, dst, mode)?;
        if a64 && d.is_dir() {
            let pd = ctx.port_dir.clone();
            for x in armhf_files(&d, &pd)? {
                ctx.log(&format!("build.outputs: {dst}: {x} not shipped: armhf, and the port is aarch64 only"));
                fs::remove_file(pd.join(&x)).map_err(io_at(pd.join(&x)))?;
            }
        }
    }
    for (n, src) in bld.dict("licenses").iter() {
        let p = PathBuf::from(pkgzip::py::join(&ps(&port_src), &src.py_str().replace("{build}", &bstr)));
        ctx.license_file(n, &p)?;
    }
    box64_port_package(&mut ctx)?;
    let pmlic = format!(
        "{}\n\n{}",
        read_text(&repo.root.join("LICENSE"))?,
        read_text(&repo.root.join("THIRD_PARTY.md"))?
    );
    ctx.license_text("LICENSE.portmaster-unity.txt", &pmlic)?;
    for n in ["LICENSE", "LICENSE.md", "LICENSE.txt"] {
        if port_src.join(n).is_file() {
            ctx.license_file("LICENSE.port.txt", &port_src.join(n))?;
            break;
        }
    }
    let imgs = port_images(&cfg, &ps(&port_src));
    for (_, v) in &imgs {
        if let Some((path, shipped)) = v {
            ctx.add(Path::new(path), shipped, Some(0o644))?;
        }
    }
    if a64 {
        let bad = armhf_files(&zipdir, &zipdir)?;
        if !bad.is_empty() {
            return Err(exit(format!(
                "port.aarch64_only: the release would carry armhf (32-bit ARM) files: {}",
                bad.join(", ")
            )));
        }
        ctx.log("aarch64 only: no armhf file in the release");
    }
    let zpath = out.join(format!("{name}.zip"));
    let mut modes = ctx.modes.clone();
    modes.insert(launcher.clone(), 0o644);
    write_zip(&zpath, &zipdir, &[launcher.clone(), name.clone()], &modes)?;
    let mb = fs::metadata(&zpath).map_err(io_at(&zpath))?.len() as f64 / 1048576.0;
    ctx.log(&format!("built {} ({mb:.1} MB): unzip it into the handheld's ports folder", zpath.display()));
    for x in std::mem::take(&mut ctx.stale) {
        ctx.log(&format!("warning: {x}"));
    }
    let tt = port_src.join("testing_thread.txt");
    if tt.is_file() {
        copyfile(&tt, &out.join("testing_thread.txt"))?;
    }
    if opts.submission {
        let sub = write_submission(&ctx, &out, &launcher, &imgs)?;
        ctx.log(&format!("PortMaster repository layout: {}", sub.display()));
    }
    let pj = pyjson::loads(&read_text(&ctx.port_dir.join("port.json"))?)?;
    let gi = ctx.port_dir.join("gameinfo.xml");
    let gtext = if gi.is_file() { Some(read_text(&gi)?) } else { None };
    let ltext = read_text(&zipdir.join(&launcher))?;
    let mut todo = submission_warnings(&cfg, &ps(&port_src), Some(&pj), Some(&ltext), gtext.as_deref());
    if !gi.is_file() {
        todo.push("no gameinfo.xml in the port folder: run `port new` again".into());
    }
    let mut big: Vec<(String, u64)> = zip_all_files(&zipdir)
        .into_iter()
        .map(|p| (relpath(&p, &zipdir), fs::metadata(&p).map(|m| m.len()).unwrap_or(0)))
        .collect();
    big.sort();
    for (n, s) in &big {
        if *s > BIG_FILE_MB << 20 {
            todo.push(format!(
                "{n} is {:.0} MB: PortMaster's repository keeps files over {BIG_FILE_MB} MB in pieces (its tools/build_data.py)",
                *s as f64 / 1048576.0
            ));
        }
    }
    for (n, s) in &big {
        let d = if *s > 1 << 20 { elf_debug_bytes(&zipdir.join(n)) } else { 0 };
        if d > 1 << 20 {
            todo.push(format!(
                "{n} ships with debug info ({:.1} MB of {:.1} MB): rebuild the runtime (runtime/build.sh strips ua_host; an out/ folder built before that does not) or strip the file",
                d as f64 / 1048576.0,
                *s as f64 / 1048576.0
            ));
        }
    }
    if !todo.is_empty() {
        ctx.log(&format!("for a PortMaster submission ({}; docs/porting.md, \"Submitting to PortMaster\"):", todo.len()));
        for x in todo {
            ctx.log(&format!("  - {x}"));
        }
    }
    Ok(zpath)
}

/// Every file under d but macOS's own (os.walk, symbolic links to folders not entered).
fn zip_all_files(d: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    fn walk(d: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = fs::read_dir(d) else { return };
        for e in rd.filter_map(|e| e.ok()) {
            let n = e.file_name().to_string_lossy().into_owned();
            let p = e.path();
            let link = e.file_type().map(|t| t.is_symlink()).unwrap_or(false);
            if p.is_dir() {
                if !link {
                    walk(&p, out);
                }
            } else if !mac_junk(&n) {
                out.push(p);
            }
        }
    }
    walk(d, &mut out);
    out
}
