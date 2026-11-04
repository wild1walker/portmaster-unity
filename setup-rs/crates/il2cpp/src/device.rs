//! il2cpp_device.py's tool folder: Debian 12 arm64 packages (clang/lld 14, Mono 6.8, glibc and
//! what they load) and Microsoft's .NET 8 runtime, each checked against its pinned sha256 and
//! unpacked into one folder that holds no links and runs everything through its own glibc loader
//! (`lib/ld-linux-aarch64.so.1 --library-path lib <program>`), with the commands in bin/.
use crate::lock::{self, Package};
use crate::tar::Tar;
use crate::util::{self, at, join, joins};
use crate::{exit, fail, hash, log, Error, Result};
use androiddata::pyjson::{self, Json};
use regex::Regex;
use std::io::{Read, Seek, SeekFrom};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;

pub const LOADER: &str = "ld-linux-aarch64.so.1";
pub const MUXER: &str = "dotnet-muxer";
pub const LAYOUT: i128 = 2;
pub const CLANG_VERSION: &str = "14.0.6";
pub const DEBIAN: &str = "https://deb.debian.org/debian/";

fn snapshot() -> String {
    format!(
        "https://snapshot.debian.org/archive/debian/{}/",
        lock::SNAPSHOT
    )
}

/// (regex on the member path without './', destination)
const PLACE: [(&str, &str); 11] = [
    (
        r"^(?:usr/)?lib/aarch64-linux-gnu/([^/]+\.so(?:\.[0-9]+)*)$",
        "lib/${1}",
    ),
    (r"^usr/lib/llvm-14/lib/(libclang-cpp\.so\.14)$", "lib/${1}"),
    (
        r"^usr/lib/llvm-14/bin/(clang|lld|llvm-objcopy|llvm-ar|llvm-nm)$",
        "llvm/bin/${1}",
    ),
    (
        r"^usr/lib/llvm-14/lib/clang/([^/]+/include/.+)$",
        "llvm/lib/clang/${1}",
    ),
    (r"^usr/bin/(mono-sgen)$", "mono/bin/${1}"),
    (
        r"^usr/lib/(libmono-native)\.so\.0\.0\.0$",
        "mono/lib/${1}.so",
    ),
    (r"^usr/lib/(libMonoPosixHelper\.so)$", "mono/lib/${1}"),
    (
        r"^etc/mono/(config|4\.5/machine\.config|4\.5/settings\.map)$",
        "mono/etc/mono/${1}",
    ),
    (
        r"^usr/lib/mono/4\.5/([^/]+\.dll)$",
        "mono/lib/mono/4.5/${1}",
    ),
    (
        r"^usr/lib/mono/gac/([^/]+/[^/]+/[^/]+\.dll)$",
        "mono/lib/mono/gac/${1}",
    ),
    (
        r"^usr/lib/mono/4\.5/Facades/([^/]+\.dll)$",
        "mono/lib/mono/4.5/Facades/${1}",
    ),
];
const DROP: [&str; 2] = [
    r"^llvm/lib/clang/[^/]+/include/(riscv_vector\.h|hexagon_.*|hvx_hexagon_.*|altivec\.h|vecintrin\.h|htm.*intrin\.h|msa\.h|__clang_cuda_.*|__clang_hip_.*|opencl-c.*\.h|ppc_wrappers/.*|openmp_wrappers/.*|cuda_wrappers/.*)$",
    r"^lib/lib(BrokenLocale|memusage|pcprofile|c_malloc_debug|thread_db|nsl|anl|nss_hesiod|nss_compat|tic|icutest|icutu|icuio|icui18n)\.so.*$",
];

fn place_rx() -> &'static (Vec<(Regex, &'static str)>, Vec<Regex>) {
    static R: OnceLock<(Vec<(Regex, &'static str)>, Vec<Regex>)> = OnceLock::new();
    R.get_or_init(|| {
        (
            PLACE
                .iter()
                .map(|(a, b)| (Regex::new(a).unwrap(), *b))
                .collect(),
            DROP.iter().map(|d| Regex::new(d).unwrap()).collect(),
        )
    })
}

/// place: a path inside a package -> its path in the tool folder, or None.
pub fn place(member: &str) -> Option<String> {
    let name = member.strip_prefix("./").unwrap_or(member);
    let (pl, drop) = place_rx();
    for (rx, dst) in pl {
        if let Some(c) = rx.captures(name) {
            let mut out = String::new();
            c.expand(dst, &mut out);
            return (!drop.iter().any(|d| d.is_match(&out))).then_some(out);
        }
    }
    None
}

/// ROOTS: what the build runs, per group.
fn roots(group: &str) -> &'static [&'static str] {
    match group {
        "llvm" => &[
            "llvm/bin/clang",
            "llvm/bin/lld",
            "llvm/bin/llvm-objcopy",
            "llvm/bin/llvm-ar",
            "llvm/bin/llvm-nm",
        ],
        "mono" => &[
            "mono/bin/mono-sgen",
            "mono/lib/libmono-native.so",
            "mono/lib/libMonoPosixHelper.so",
        ],
        _ => &[],
    }
}

/// DLOPEN: libraries loaded by name.
fn dlopen(group: &str) -> &'static [&'static str] {
    match group {
        "base" => &["libnss_files.so.2", "libnss_dns.so.2"],
        "dotnet" => &["libssl.so.3", "libcrypto.so.3"],
        _ => &[],
    }
}

/// ONLY: packages of which only part is needed.
fn only(name: &str) -> Option<Regex> {
    (name == "mono-devel").then(|| Regex::new(r"^(?:\./)?usr/lib/mono/4\.5/Facades/").unwrap())
}

/// groups_for: the package groups a build needs (tools: the il2cpp/UnityLinker programs).
pub fn groups_for(tools: &[String], net471: &str) -> Vec<String> {
    let mut g: Vec<String> = vec!["base".into(), "llvm".into()];
    let exe: Vec<&String> = tools.iter().filter(|t| t.ends_with(".exe")).collect();
    if !exe.is_empty() {
        g.push("mono".into());
        let mut refs: Vec<String> = Vec::new();
        let mut dirs: Vec<&str> = exe.iter().map(|t| util::dirname(t)).collect();
        dirs.sort();
        dirs.dedup();
        for d in dirs {
            for n in util::listdir(d).unwrap_or_default() {
                if n.ends_with(".dll") || n.ends_with(".exe") {
                    if let Ok(r) = crate::build::assembly_refs(&join(d, &n)) {
                        refs.extend(r);
                    }
                }
            }
        }
        if refs.iter().any(|r| r == "netstandard.dll") {
            g.push("mono-facades".into());
        }
        if refs.iter().any(|r| r == "System.Web.Extensions.dll") {
            g.push("mono-web".into());
        }
    }
    if tools.iter().any(|t| t.ends_with(".dll")) || (!exe.is_empty() && net471 == "dotnet") {
        g.push("dotnet".into());
    }
    g
}

/// elf_dynamic: (soname, [needed]) of a 64-bit little-endian ELF file (None, []) without
/// PT_DYNAMIC; Ok(None) when it is not ELF.
pub fn elf_dynamic(path: &str) -> Result<Option<(Option<String>, Vec<String>)>> {
    let mut f = std::fs::File::open(path).map_err(at(path))?;
    let mut head = Vec::new();
    (&mut f).take(64).read_to_end(&mut head).map_err(at(path))?;
    if head.len() < 64 || &head[..4] != b"\x7fELF" || head[4] != 2 {
        return Ok(None);
    }
    let u16_ = |b: &[u8], p: usize| u16::from_le_bytes(b[p..p + 2].try_into().unwrap());
    let u32_ = |b: &[u8], p: usize| u32::from_le_bytes(b[p..p + 4].try_into().unwrap());
    let u64_ = |b: &[u8], p: usize| u64::from_le_bytes(b[p..p + 8].try_into().unwrap());
    let phoff = u64_(&head, 0x20);
    let (phentsize, phnum) = (u16_(&head, 0x36) as usize, u16_(&head, 0x38) as usize);
    let short = || Error::Fail(format!("struct.error: {path}: short ELF data"));
    let mut ph = vec![0u8; phentsize * phnum];
    f.seek(SeekFrom::Start(phoff)).map_err(at(path))?;
    f.read_exact(&mut ph).map_err(|_| short())?;
    let mut loads = Vec::new();
    let mut dyn_ = None;
    for i in 0..phnum {
        let o = i * phentsize;
        if o + 56 > ph.len() {
            return Err(short());
        }
        let typ = u32_(&ph, o);
        let off = u64_(&ph, o + 8);
        let vaddr = u64_(&ph, o + 16);
        let filesz = u64_(&ph, o + 32);
        if typ == 1 {
            loads.push((vaddr, off, filesz));
        } else if typ == 2 {
            dyn_ = Some((off, filesz));
        }
    }
    let Some((doff, dsz)) = dyn_ else {
        return Ok(Some((None, Vec::new())));
    };
    let mut d = Vec::new();
    f.seek(SeekFrom::Start(doff)).map_err(at(path))?;
    (&mut f).take(dsz).read_to_end(&mut d).map_err(at(path))?;
    let mut tags = Vec::new();
    let mut i = 0;
    while i + 16 <= d.len() {
        tags.push((u64_(&d, i) as i64, u64_(&d, i + 8)));
        i += 16;
    }
    let strtab = tags.iter().find(|t| t.0 == 5).map(|t| t.1);
    let strsz = tags.iter().find(|t| t.0 == 10).map_or(0, |t| t.1);
    let off_of = |va: u64| -> Result<u64> {
        for &(v, o, n) in &loads {
            if v <= va && va < v + n {
                return Ok(va - v + o);
            }
        }
        fail(format!(
            "ValueError: {path}: address {va:#x} outside the file"
        ))
    };
    let Some(strtab) = strtab else {
        return fail(format!("TypeError: {path}: no string table"));
    };
    let so = off_of(strtab)?;
    let mut strs = Vec::new();
    f.seek(SeekFrom::Start(so)).map_err(at(path))?;
    (&mut f)
        .take(strsz)
        .read_to_end(&mut strs)
        .map_err(at(path))?;
    let s = |i: u64| -> Result<String> {
        let i = i as usize;
        let e = strs
            .get(i..)
            .and_then(|r| r.iter().position(|&c| c == 0))
            .ok_or_else(|| Error::Fail("ValueError: subsection not found".into()))?;
        String::from_utf8(strs[i..i + e].to_vec())
            .map_err(|e| Error::Fail(format!("UnicodeDecodeError: {e}")))
    };
    let soname = match tags.iter().find(|t| t.0 == 14) {
        Some(t) => Some(s(t.1)?),
        None => None,
    };
    let needed = tags
        .iter()
        .filter(|t| t.0 == 1)
        .map(|t| s(t.1))
        .collect::<Result<Vec<_>>>()?;
    Ok(Some((soname, needed)))
}

// ------------------------------------------------------------------ downloads

pub const CA_FILES: [&str; 6] = [
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/cert.pem",
    "/etc/ssl/ca-bundle.pem",
    "/etc/pki/tls/cacert.pem",
    "/etc/ssl/certs/ca-bundle.crt",
];

static UNVERIFIED: AtomicBool = AtomicBool::new(false);

/// install_ca: HTTPS with a CA bundle the firmware has where the TLS library does not look
/// (SSL_CERT_FILE); false when the system has none at all.
pub fn install_ca() -> bool {
    if pkgzip::http::have_ca() {
        return true;
    }
    if std::env::var_os("SSL_CERT_FILE").is_none() {
        if let Some(f) = CA_FILES.iter().find(|f| util::isfile(f)) {
            // (before any thread starts: the setup's process environment)
            std::env::set_var("SSL_CERT_FILE", f);
            return true;
        }
    }
    false
}

pub fn no_ca() {
    log("  (no CA certificates on this system: downloading without checking the server; every file is checked against its sha256 instead)");
    UNVERIFIED.store(true, Ordering::SeqCst);
}

fn http_reason(code: u16) -> &'static str {
    match code {
        403 => "Forbidden",
        404 => "Not Found",
        410 => "Gone",
        500 => "Internal Server Error",
        502 => "Bad Gateway",
        503 => "Service Unavailable",
        504 => "Gateway Timeout",
        _ => "",
    }
}

enum DlErr {
    Http(u16, String),
    Other(String),
}

fn download_once(url: &str, dest: &str, sha256: &str) -> std::result::Result<(), DlErr> {
    let mut c = pkgzip::http::Client::default();
    c.max_body = 512 << 20;
    c.insecure = UNVERIFIED.load(Ordering::SeqCst);
    let r = c
        .get_following(url, &[("User-Agent", "pmunity-il2cpp-device")], 10)
        .map_err(|e| DlErr::Other(format!("<urlopen error {e}>")))?;
    if r.status >= 400 {
        return Err(DlErr::Http(
            r.status,
            format!("HTTP Error {}: {}", r.status, http_reason(r.status)),
        ));
    }
    let part = format!("{dest}.part");
    std::fs::write(&part, &r.body).map_err(|e| DlErr::Other(util::oserror(&e, &part)))?;
    let got = hash::sha256_hex(&r.body);
    if got != sha256 {
        return Err(DlErr::Other(format!("checksum mismatch ({got})")));
    }
    std::fs::rename(&part, dest).map_err(|e| DlErr::Other(util::oserror(&e, dest)))
}

/// download: the first of `urls` that serves the file with that sha256 -> dest.
pub fn download(urls: &[String], dest: &str, sha256: &str) -> Result<String> {
    if util::isfile(dest) && hash::sha256_file(dest)? == sha256 {
        return Ok(dest.to_string());
    }
    let tries = 4;
    let mut errors: Vec<String> = Vec::new();
    for url in urls {
        let mut attempt = 0;
        while attempt < tries {
            match download_once(url, dest, sha256) {
                Ok(()) => return Ok(dest.to_string()),
                Err(DlErr::Http(code, m)) => {
                    errors.push(format!("{url}: {m}"));
                    if matches!(code, 403 | 404 | 410) {
                        break;
                    }
                }
                Err(DlErr::Other(m)) => {
                    errors.push(format!("{url}: {m}"));
                    let cert = m.contains("CERTIFICATE_VERIFY_FAILED")
                        || m.contains("UnknownIssuer")
                        || m.contains("no CA certificates");
                    if cert && !UNVERIFIED.load(Ordering::SeqCst) {
                        no_ca();
                        attempt += 1;
                        continue;
                    }
                }
            }
            std::thread::sleep(std::time::Duration::from_secs(2 * (attempt as u64 + 1)));
            attempt += 1;
        }
    }
    let _ = std::fs::remove_file(format!("{dest}.part"));
    let tail: Vec<&String> = errors.iter().rev().take(4).rev().collect();
    exit(format!(
        "download failed (is Wi-Fi on?):\n  {}",
        tail.iter()
            .map(|s| s.as_str())
            .collect::<Vec<_>>()
            .join("\n  ")
    ))
}

fn chmod_quiet(p: &str, mode: u32) {
    let _ = util::chmod(p, mode);
}

/// _write: a stream into path through path.part, then its mode (errors ignored: exFAT).
fn write_stream(path: &str, src: &mut dyn Read, mode: Option<u32>) -> Result<()> {
    util::makedirs(util::dirname(path))?;
    let part = format!("{path}.part");
    let mut f = std::fs::File::create(&part).map_err(at(&part))?;
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = src.read(&mut buf).map_err(|e| Error::Fail(e.to_string()))?;
        if n == 0 {
            break;
        }
        std::io::Write::write_all(&mut f, &buf[..n]).map_err(at(&part))?;
    }
    drop(f);
    util::replace(&part, path)?;
    if let Some(m) = mode {
        chmod_quiet(path, m);
    }
    Ok(())
}

/// deb_data: the data.tar member of a .deb (an ar archive), as (its name's compression, offset,
/// size).
fn deb_data(f: &mut std::fs::File) -> Result<(String, u64, u64)> {
    let mut magic = [0u8; 8];
    if f.read_exact(&mut magic).is_err() || &magic != b"!<arch>\n" {
        return fail("ValueError: not a Debian package");
    }
    loop {
        let mut h = [0u8; 60];
        let mut got = 0;
        while got < 60 {
            let n = f
                .read(&mut h[got..])
                .map_err(|e| Error::Fail(e.to_string()))?;
            if n == 0 {
                break;
            }
            got += n;
        }
        if got < 60 {
            return fail("ValueError: no data.tar in the package");
        }
        let name = String::from_utf8_lossy(&h[..16])
            .trim()
            .trim_end_matches('/')
            .to_string();
        let size: u64 = String::from_utf8_lossy(&h[48..58])
            .trim()
            .parse()
            .map_err(|_| Error::Fail("ValueError: invalid literal for int()".into()))?;
        let start = f
            .stream_position()
            .map_err(|e| Error::Fail(e.to_string()))?;
        if name.starts_with("data.tar") {
            let comp = name.rsplit('.').next().unwrap_or("").to_string();
            if !matches!(comp.as_str(), "xz" | "gz" | "tar") {
                return fail(format!(
                    "ValueError: {name}: compression not supported (Debian packages are xz)"
                ));
            }
            return Ok((comp, start, size));
        }
        f.seek(SeekFrom::Start(start + size + (size & 1)))
            .map_err(|e| Error::Fail(e.to_string()))?;
    }
}

/// unpack_deb: the members select() places -> tc; the relative paths written.
pub fn unpack_deb(
    deb: &str,
    tc: &str,
    select: &dyn Fn(&str) -> Option<String>,
) -> Result<Vec<String>> {
    let mut f = std::fs::File::open(deb).map_err(at(deb))?;
    let (comp, start, size) = deb_data(&mut f)?;
    f.seek(SeekFrom::Start(start)).map_err(at(deb))?;
    let raw = std::io::BufReader::with_capacity(1 << 20, f.take(size));
    let stream: Box<dyn Read> = match comp.as_str() {
        "xz" => Box::new(lzma_rust2::XzReader::new(raw, true)),
        "gz" => Box::new(flate2::read::MultiGzDecoder::new(raw)),
        _ => Box::new(raw),
    };
    unpack_tar(stream, &mut |name| select(name), tc)
}

/// The regular files of a tar stream that pick() places (-> their path under tc), written with
/// mode 755 when any exec bit is set.
fn unpack_tar(
    stream: Box<dyn Read + '_>,
    pick: &mut dyn FnMut(&str) -> Option<String>,
    tc: &str,
) -> Result<Vec<String>> {
    let mut t = Tar::new(stream);
    let mut got = Vec::new();
    while let Some(m) = t.next_member().map_err(|e| Error::Fail(e.to_string()))? {
        if !m.isfile() {
            continue;
        }
        let Some(rel) = pick(&m.name) else {
            continue;
        };
        let exe = (m.mode & 0o111 != 0).then_some(0o755);
        write_stream(&join(tc, &rel), &mut t.data(), exe)?;
        got.push(rel);
    }
    Ok(got)
}

/// unpack_tgz: Microsoft's .NET runtime tarball -> tc/prefix (files only; `dotnet` as the muxer).
pub fn unpack_tgz(path: &str, tc: &str, prefix: &str) -> Result<Vec<String>> {
    let f = std::fs::File::open(path).map_err(at(path))?;
    let gz = flate2::read::MultiGzDecoder::new(std::io::BufReader::with_capacity(1 << 20, f));
    unpack_tar(
        Box::new(gz),
        &mut |name| {
            let name = name.strip_prefix("./").unwrap_or(name);
            if name.starts_with('/') || name.split('/').any(|p| p == "..") {
                return None;
            }
            let name = if name == "dotnet" { MUXER } else { name };
            Some(join(prefix, name))
        },
        tc,
    )
}

// ------------------------------------------------------------------ the tool folder

pub fn default_toolchain(home: &str) -> String {
    join(home, "device-arm64")
}

pub fn lock_packages(groups: &[String]) -> Vec<&'static Package> {
    lock::PACKAGES
        .iter()
        .filter(|p| groups.iter().any(|g| g == p.group))
        .collect()
}

/// lock_id: a digest of the pins the groups use.
pub fn lock_id(groups: &[String]) -> String {
    let mut l: Vec<Json> = lock_packages(groups)
        .iter()
        .map(|p| {
            Json::List(vec![
                Json::str(p.name),
                Json::str(p.version),
                Json::str(p.sha256),
            ])
        })
        .collect();
    if groups.iter().any(|g| g == "dotnet") {
        let d = &lock::DOTNET;
        // (json.dumps(sort_keys=True))
        l.push(Json::Obj(vec![
            ("sha256".into(), Json::str(d.sha256)),
            ("size".into(), Json::Int(d.size as i128)),
            ("url".into(), Json::str(d.url)),
            ("version".into(), Json::str(d.version)),
        ]));
    }
    hash::sha256_hex(pyjson::dumps(&Json::List(l), None).as_bytes())[..16].to_string()
}

/// disk_free: bytes free where `path` is (PMUNITY_DISK_FREE_MB instead, when set).
pub fn disk_free(path: &str) -> Result<u64> {
    if let Ok(v) = std::env::var("PMUNITY_DISK_FREE_MB") {
        if !v.is_empty() && v.bytes().all(|c| c.is_ascii_digit()) {
            return Ok(v.parse::<u64>().unwrap_or(0) << 20);
        }
    }
    let c = std::ffi::CString::new(path).map_err(|e| Error::Fail(e.to_string()))?;
    // SAFETY: a valid C string and a zeroed struct for the call to fill in
    let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statvfs(c.as_ptr(), &mut st) } != 0 {
        return Err(Error::Fail(util::oserror(
            &std::io::Error::last_os_error(),
            path,
        )));
    }
    #[allow(clippy::unnecessary_cast)] // (the types differ from libc to libc)
    Ok(st.f_bavail as u64 * st.f_frsize as u64)
}

fn pkey(p: &Package) -> String {
    format!("{}_{}", p.name, p.version)
}

fn state(tc: &str) -> Json {
    std::fs::read(join(tc, ".state.json"))
        .ok()
        .and_then(|b| pyjson::loads_bytes(&b).ok())
        .filter(|j| matches!(j, Json::Obj(_)))
        .unwrap_or_else(Json::obj)
}

fn save_state(tc: &str, st: &Json) -> Result<()> {
    let p = join(tc, ".state.json");
    let part = format!("{p}.part");
    util::write_text(&part, &pyjson::dumps(st, Some(1)))?;
    util::replace(&part, &p)
}

fn strs(j: Option<&Json>) -> Vec<String> {
    j.and_then(Json::as_list)
        .map(|l| l.iter().map(Json::py_str).collect())
        .unwrap_or_default()
}

fn str_list(v: &[String]) -> Json {
    Json::List(v.iter().map(Json::str).collect())
}

/// du: the bytes of the files under a folder.
pub fn du(path: &str) -> u64 {
    let mut n = 0;
    for w in util::walk(path, false) {
        for x in &w.files {
            if let Ok(m) = std::fs::symlink_metadata(join(&w.root, x)) {
                n += m.len();
            }
        }
    }
    n
}

/// fetch: what `groups` need, downloaded and unpacked into tc (a package once unpacked is not
/// fetched again); the wrappers; every library checked. Returns tc.
pub fn fetch(tc: &str, groups: &[String]) -> Result<String> {
    let tc = util::abspath(tc);
    util::makedirs(&tc)?;
    let mut st = state(&tc);
    if st.as_obj().is_some_and(|o| !o.is_empty())
        && st.get("layout").and_then(Json::as_int) != Some(LAYOUT)
    {
        for n in util::listdir(&tc)? {
            if n != ".download" {
                let p = join(&tc, &n);
                if util::isdir(&p) && !util::islink(&p) {
                    std::fs::remove_dir_all(&p).map_err(at(&p))?;
                } else {
                    util::unlink(&p)?;
                }
            }
        }
        st = Json::obj();
    }
    st.set("layout", Json::Int(LAYOUT));
    let have_groups = strs(st.get("groups"));
    if st.get("ready").and_then(Json::as_str) == Some(lock_id(groups).as_str())
        && groups.iter().all(|g| have_groups.contains(g))
    {
        commands(&tc, &have_groups)?;
        if st.get("path").and_then(Json::as_str) != Some(tc.as_str()) {
            st.set("path", Json::str(&tc));
            save_state(&tc, &st)?;
        }
        return Ok(tc);
    }
    if !install_ca() {
        no_ca();
    }
    let pkgs = lock_packages(groups);
    let mut done: Vec<String> = strs(st.get("unpacked"));
    let todo: Vec<&Package> = pkgs
        .into_iter()
        .filter(|p| !done.contains(&pkey(p)))
        .collect();
    let dl = join(&tc, ".download");
    let want_dotnet = groups.iter().any(|g| g == "dotnet") && !done.iter().any(|d| d == "dotnet");
    if !todo.is_empty() || want_dotnet {
        let mut mb = todo.iter().map(|p| p.size as f64).sum::<f64>() / 1e6;
        if want_dotnet {
            mb += lock::DOTNET.size as f64 / 1e6;
        }
        let need: u64 = 400 << 20;
        if disk_free(&tc)? < need {
            return exit(format!(
                "{tc}: {} MB free; the compiler tools need about {} MB",
                disk_free(&tc)? >> 20,
                need >> 20
            ));
        }
        log(&format!(
            "IL2CPP tools for this handheld: {} packages, {mb:.0} MB to download (Debian 12, Microsoft .NET)",
            todo.len()
        ));
        util::makedirs(&dl)?;
    }
    let total = todo.len();
    for (i, p) in todo.iter().enumerate() {
        log(&format!(
            "  [{}/{total}] {} {} ({:.1} MB)",
            i + 1,
            p.name,
            p.version,
            p.size as f64 / 1e6
        ));
        let deb = join(&dl, util::basename(p.filename));
        download(
            &[
                format!("{DEBIAN}{}", p.filename),
                format!("{}{}", snapshot(), p.filename),
            ],
            &deb,
            p.sha256,
        )?;
        let only = only(p.name);
        let sel = |m: &str| match &only {
            Some(rx) if !rx.is_match(m) => None,
            _ => place(m),
        };
        unpack_deb(&deb, &tc, &sel)?;
        util::unlink(&deb)?;
        done.push(pkey(p));
        done.sort();
        st.set("unpacked", str_list(&done));
        save_state(&tc, &st)?;
    }
    if want_dotnet {
        let d = &lock::DOTNET;
        log(&format!(
            "  .NET runtime {} ({:.1} MB, Microsoft)",
            d.version,
            d.size as f64 / 1e6
        ));
        let tgz = join(&dl, util::basename(d.url));
        download(&[d.url.to_string()], &tgz, d.sha256)?;
        util::rmtree(&join(&tc, "dotnet"));
        unpack_tgz(&tgz, &tc, "dotnet")?;
        util::unlink(&tgz)?;
        done.push("dotnet".into());
        done.sort();
        st.set("unpacked", str_list(&done));
        save_state(&tc, &st)?;
    }
    util::rmtree(&dl);
    finish(&tc, groups)?;
    let mut all: Vec<String> = groups.to_vec();
    for g in strs(st.get("groups")) {
        if !all.contains(&g) {
            all.push(g);
        }
    }
    all.sort();
    all.dedup();
    st.set("ready", Json::str(lock_id(groups)));
    st.set("path", Json::str(&tc));
    st.set("groups", str_list(&all));
    save_state(&tc, &st)?;
    log(&format!(
        "IL2CPP tools ready: {tc} ({:.0} MB)",
        du(&tc) as f64 / 1e6
    ));
    Ok(tc)
}

/// finish: libraries under their sonames, the loader's copies, the wrappers; every library
/// checked.
pub fn finish(tc: &str, groups: &[String]) -> Result<()> {
    let lib = join(tc, "lib");
    let mut names = util::listdir(&lib)?;
    names.sort();
    for n in names {
        let p = join(&lib, &n);
        if n.ends_with(".part") || !util::isfile(&p) {
            continue;
        }
        let so = elf_dynamic(&p)?.and_then(|d| d.0);
        if let Some(so) = so {
            if so != n {
                if util::exists(&join(&lib, &so)) {
                    util::unlink(&p)?;
                } else {
                    util::replace(&p, &join(&lib, &so))?;
                }
            }
        }
    }
    commands(tc, groups)?;
    check(tc, groups)
}

/// commands: the loader's copies programs take their prefix from, and the wrappers in bin/.
pub fn commands(tc: &str, groups: &[String]) -> Result<()> {
    let loader = joins(&[tc, "lib", LOADER]);
    if !util::isfile(&loader) {
        return exit(format!(
            "{tc}: glibc's loader is missing (the libc6 package did not unpack)"
        ));
    }
    chmod_quiet(&loader, 0o755);
    let mut copies = Vec::new();
    if groups.iter().any(|g| g == "mono") {
        copies.push(joins(&[tc, "mono", "bin", "mono"]));
    }
    if groups.iter().any(|g| g == "dotnet") {
        copies.push(joins(&[tc, "dotnet", "dotnet"]));
    }
    let lsize = util::getsize(&loader)?;
    for c in copies {
        if !(util::isfile(&c) && util::getsize(&c)? == lsize) {
            let mut src = std::fs::File::open(&loader).map_err(at(&loader))?;
            write_stream(&c, &mut src, Some(0o755))?;
        }
    }
    write_wrappers(tc, groups)
}

const DOTNET_SKIP: [&str; 3] = [
    "libcoreclrtraceptprovider.so",
    "libmscordbi.so",
    "libmscordaccore.so",
];

/// check: every library the programs need is in lib/ (or next to the one that needs it).
pub fn check(tc: &str, groups: &[String]) -> Result<()> {
    let have: Vec<String> = util::listdir(&join(tc, "lib"))?;
    let mut roots_: Vec<String> = Vec::new();
    for g in groups {
        for r in roots(g) {
            roots_.push(join(tc, r));
        }
    }
    if groups.iter().any(|g| g == "dotnet") {
        let d = join(tc, "dotnet");
        roots_.push(join(&d, MUXER));
        for w in util::walk(&d, false) {
            for n in &w.files {
                if n.ends_with(".so") && !DOTNET_SKIP.contains(&n.as_str()) {
                    roots_.push(join(&w.root, n));
                }
            }
        }
    }
    let mut missing: Vec<(String, String)> = Vec::new();
    let mut seen = std::collections::HashSet::new();
    let mut todo = roots_;
    for g in groups {
        for n in dlopen(g) {
            todo.push(joins(&[tc, "lib", n]));
        }
    }
    while let Some(p) = todo.pop() {
        if !seen.insert(p.clone()) {
            continue;
        }
        if !util::isfile(&p) {
            let b = util::basename(&p).to_string();
            if !missing.iter().any(|m| m.0 == b) {
                missing.push((b, "the toolchain".into()));
            }
            continue;
        }
        let needed = elf_dynamic(&p)?.map(|d| d.1).unwrap_or_default();
        for n in needed {
            if have.contains(&n) {
                todo.push(joins(&[tc, "lib", &n]));
            } else if util::isfile(&join(util::dirname(&p), &n)) {
                todo.push(join(util::dirname(&p), &n));
            } else if !missing.iter().any(|m| m.0 == n) {
                missing.push((n, util::relpath(&p, tc)));
            }
        }
    }
    if !missing.is_empty() {
        missing.sort();
        return exit(format!(
            "{tc}: libraries missing: {}",
            missing
                .iter()
                .map(|(n, by)| format!("{n} (needed by {by})"))
                .collect::<Vec<_>>()
                .join(", ")
        ));
    }
    Ok(())
}

const WRAP: &str = "#!/bin/sh\n# pmunity il2cpp --on-device ({what}): run through the toolchain's own glibc loader\n";

/// NO_OUTLINE_SH: -mno-outline for AArch64 and ARMv7 compiles (identical objects, a third of the peak).
pub const NO_OUTLINE_SH: &str = r#"nool=; c=; a64=
for a do case $a in -c) c=1 ;; aarch64*|--target=aarch64*|armv7*|--target=armv7*) a64=1 ;; esac; done
[ -n "$c" ] && [ -n "$a64" ] && [ -z "$PMUNITY_IL2CPP_OUTLINER" ] && nool=-mno-outline
"#;

/// KILLED_SH: a compiler killed by a signal noted in PMUNITY_IL2CPP_KILLED.
pub const KILLED_SH: &str = r#"if [ "$st" -gt 128 ] && [ -n "$PMUNITY_IL2CPP_KILLED" ]; then
  echo "$st ${src:-?} $(date +%H:%M:%S 2>/dev/null)" >> "$PMUNITY_IL2CPP_KILLED"
fi
"#;

/// JOBS_SH: the job slots (PMUNITY_IL2CPP_SCHED=0).
pub const JOBS_SH: &str = r#"slots=${PMUNITY_JOBS:-0}; jd=$PMUNITY_JOBS_DIR; held=
release() { for s in $held; do rm -rf "$jd/$s"; done; held=; }
memok() {
  [ -z "$PMUNITY_JOB_MEM_KB" ] && return 0
  while read -r k v _; do [ "$k" = MemAvailable: ] && { [ "$v" -ge "$PMUNITY_JOB_MEM_KB" ]; return; }; done < /proc/meminfo
  return 0
}
take() {   # take <slot>: 0 when this process now holds it
  if mkdir "$jd/$1" 2>/dev/null; then echo $$ > "$jd/$1/pid"; held="$held $1"; return 0; fi
  o=$(cat "$jd/$1/pid" 2>/dev/null)
  if [ -n "$o" ] && ! kill -0 "$o" 2>/dev/null; then rm -rf "$jd/$1"; fi
  return 1
}
if [ -n "$jd" ] && [ "$slots" -gt 0 ]; then
  big=; src=
  for a do case $a in *.cpp|*.c|*.cc) src=$a ;; esac; done
  if [ -n "$src" ] && [ -n "$PMUNITY_IL2CPP_BIG_KB" ] && [ -f "$src" ]; then
    kb=$(( $(wc -c < "$src") / 1024 )); [ "$kb" -ge "$PMUNITY_IL2CPP_BIG_KB" ] && big=1
  fi
  trap 'release; exit 130' INT TERM HUP
  if [ -n "$big" ]; then          # one big file at a time; it keeps the slots it got until it has all
    until take big; do sleep 1; done
    i=0; while [ $i -lt "$slots" ]; do if take $i; then i=$((i+1)); else sleep 1; fi; done
  else
    while :; do
      i=0; while [ $i -lt "$slots" ]; do
        if take $i; then
          if [ $i -eq 0 ] || memok; then break 2; fi
          release
        fi
        i=$((i+1))
      done
      sleep 1
    done
  fi
fi
"#;

const SCHED_SH_HEAD: &str = r#"sd=$PMUNITY_SCHED; me=$$; st=0
if [ -n "$sd" ] && [ -d "$sd/q" ]; then
  src=; c=0
  for a do case $a in *.cpp|*.c|*.cc) src=$a ;; -c) c=1 ;; esac; done
  kb=0; [ -n "$src" ] && [ -f "$src" ] && kb=$(( $(wc -c < "$src") / 1024 ))
  release() { rm -f "$sd/q/$me" "$sd/go/$me"; }
  trap 'release; exit 130' INT TERM HUP
  trap 'g=1' USR1
  { echo 500 > /proc/self/oom_score_adj; } 2>/dev/null
  for try in 1 2; do
    if [ $try = 1 ]; then how=-; else how=alone; fi
    g=; echo "$kb $c $how $src" > "$sd/q/$me.t" && mv "$sd/q/$me.t" "$sd/q/$me"
    while [ -z "$g" ] && [ ! -e "$sd/go/$me" ] && [ -d "$sd/q" ]; do sleep 1 & s=$!; wait $s; kill $s 2>/dev/null; done
    @CC@
    st=$?
    release
    { [ "$st" -gt 128 ] && [ "$st" -ne 130 ] && [ $try = 1 ] && [ -d "$sd/q" ]; } || break
    echo "$st ${src:-?}" >> "$sd/retried"
  done
else
"#;

const SCHED_SH_TAIL: &str = r#"
  @CC@
  st=$?
  release
fi
"#;

/// SCHED_SH: the compiler wrapper's turn from the scheduler (else the slots).
pub fn sched_sh() -> String {
    // (JOBS_SH.replace('\n', '\n  ').rstrip())
    let jobs = JOBS_SH.replace('\n', "\n  ");
    format!("{SCHED_SH_HEAD}{}{SCHED_SH_TAIL}", jobs.trim_end())
}

/// write_wrappers: the commands in <tc>/bin (and ld.lld next to clang).
pub fn write_wrappers(tc: &str, groups: &[String]) -> Result<()> {
    let ld = format!("\"{tc}/lib/{LOADER}\" --library-path \"{tc}/lib\"");
    let llvm = format!("{tc}/llvm/bin");
    let wrap = |what: &str| WRAP.replace("{what}", what);
    let mut w: Vec<(String, String)> = Vec::new();
    let sched = sched_sh();
    for (name, mode) in [("clang", ""), ("clang++", " --driver-mode=g++")] {
        let cc = format!(
            "$PMUNITY_IL2CPP_CC_WRAPPER {ld} \"{llvm}/clang\"{mode} -no-canonical-prefixes $nool \"$@\""
        );
        w.push((
            name.into(),
            format!(
                "{}{NO_OUTLINE_SH}{}{KILLED_SH}exit $st\n",
                wrap(name),
                sched.replace("@CC@", &cc)
            ),
        ));
    }
    w.push((
        "ld.lld".into(),
        format!(
            "{}exec {ld} \"{llvm}/lld\" -flavor gnu $PMUNITY_LLD_FLAGS \"$@\"\n",
            wrap("lld")
        ),
    ));
    for (name, real) in [
        ("llvm-objcopy", "llvm-objcopy"),
        ("llvm-strip", "llvm-objcopy"),
        ("llvm-ar", "llvm-ar"),
        ("llvm-ranlib", "llvm-ar"),
        ("llvm-nm", "llvm-nm"),
    ] {
        w.push((
            name.into(),
            format!(
                "{}exec {ld} --argv0 {name} \"{llvm}/{real}\" \"$@\"\n",
                wrap(name)
            ),
        ));
    }
    if groups.iter().any(|g| g == "mono") {
        w.push((
            "mono".into(),
            format!(
                "{}exec \"{tc}/mono/bin/mono\" --library-path \"{tc}/lib\" \"{tc}/mono/bin/mono-sgen\" \"$@\"\n",
                wrap("Mono")
            ),
        ));
    }
    if groups.iter().any(|g| g == "dotnet") {
        w.push((
            "dotnet".into(),
            format!(
                "{}exec \"{tc}/dotnet/dotnet\" --library-path \"{tc}/lib\" --argv0 \"{tc}/dotnet/dotnet\" \"{tc}/dotnet/{MUXER}\" \"$@\"\n",
                wrap(".NET")
            ),
        ));
    }
    let mut files: Vec<(String, String)> =
        w.into_iter().map(|(n, t)| (join("bin", &n), t)).collect();
    let lld = files
        .iter()
        .find(|f| f.0 == "bin/ld.lld")
        .unwrap()
        .1
        .clone();
    files.push(("llvm/bin/ld.lld".into(), lld));
    for (rel, text) in files {
        let p = join(tc, &rel);
        if std::fs::read(&p).is_ok_and(|d| d == text.as_bytes()) {
            continue;
        }
        util::makedirs(util::dirname(&p))?;
        let part = format!("{p}.part");
        util::write_text(&part, &text)?;
        chmod_quiet(&part, 0o755);
        util::replace(&part, &p)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn places() {
        assert_eq!(
            place("./usr/lib/aarch64-linux-gnu/libz.so.1.2.13").as_deref(),
            Some("lib/libz.so.1.2.13")
        );
        assert_eq!(
            place("./lib/aarch64-linux-gnu/libc.so.6").as_deref(),
            Some("lib/libc.so.6")
        );
        assert_eq!(
            place("./usr/lib/llvm-14/bin/clang").as_deref(),
            Some("llvm/bin/clang")
        );
        assert_eq!(place("./usr/lib/llvm-14/bin/clang-14"), None);
        assert_eq!(
            place("./usr/lib/llvm-14/lib/clang/14.0.6/include/arm_neon.h").as_deref(),
            Some("llvm/lib/clang/14.0.6/include/arm_neon.h")
        );
        assert_eq!(
            place("./usr/lib/llvm-14/lib/clang/14.0.6/include/altivec.h"),
            None
        );
        assert_eq!(place("./usr/lib/aarch64-linux-gnu/libthread_db.so.1"), None);
        assert_eq!(
            place("./usr/lib/libmono-native.so.0.0.0").as_deref(),
            Some("mono/lib/libmono-native.so")
        );
        assert_eq!(
            place("./etc/mono/4.5/machine.config").as_deref(),
            Some("mono/etc/mono/4.5/machine.config")
        );
        assert_eq!(
            place("./usr/lib/mono/gac/System/4.0.0.0__b77a5c561934e089/System.dll").as_deref(),
            Some("mono/lib/mono/gac/System/4.0.0.0__b77a5c561934e089/System.dll")
        );
    }

    #[test]
    fn lock_ids() {
        // (the Python's lock_id of the same groups, at the pins of 2026-10-21)
        let g = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(lock_id(&g(&["base", "llvm", "dotnet"])).len(), 16);
        assert_ne!(lock_id(&g(&["base"])), lock_id(&g(&["base", "dotnet"])));
    }

    #[test]
    fn sched_script() {
        let s = sched_sh();
        assert!(s.contains(
            "\nelse\nslots=${PMUNITY_JOBS:-0}; jd=$PMUNITY_JOBS_DIR; held=\n  release()"
        ));
        assert!(s.ends_with("  fi\n  @CC@\n  st=$?\n  release\nfi\n"));
    }

    #[test]
    fn deb_unpacked() {
        let d = std::env::temp_dir().join(format!("il2cpp-deb-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let tar = crate::tar::tests::make(&[
            ("./usr/lib/llvm-14/bin/clang", b'0', b"CLANG", 0o755),
            ("./usr/share/doc/x", b'0', b"doc", 0o644),
            (
                "./usr/lib/aarch64-linux-gnu/libz.so.1.2.13",
                b'0',
                b"Z",
                0o644,
            ),
        ]);
        let mut gz = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::fast());
        std::io::Write::write_all(&mut gz, &tar).unwrap();
        let gz = gz.finish().unwrap();
        let mut deb = b"!<arch>\n".to_vec();
        for (name, data) in [("debian-binary", b"2.0\n".to_vec()), ("data.tar.gz", gz)] {
            let mut h = format!(
                "{name:<16}{:<12}{:<6}{:<6}{:<8}{:<10}`\n",
                0,
                0,
                0,
                100644,
                data.len()
            );
            h.truncate(60);
            deb.extend(h.as_bytes());
            deb.extend(&data);
            if data.len() % 2 == 1 {
                deb.push(b'\n');
            }
        }
        let debp = d.join("x.deb");
        std::fs::write(&debp, deb).unwrap();
        let tc = d.join("tc");
        let got = unpack_deb(debp.to_str().unwrap(), tc.to_str().unwrap(), &place).unwrap();
        assert_eq!(got, vec!["llvm/bin/clang", "lib/libz.so.1.2.13"]);
        assert_eq!(std::fs::read(tc.join("llvm/bin/clang")).unwrap(), b"CLANG");
        assert!(util::executable(
            tc.join("llvm/bin/clang").to_str().unwrap()
        ));
        let _ = std::fs::remove_dir_all(&d);
    }
}
