//! `pmu-dev il2cpp-lock [--packages Packages.xz] [--dotnet 8.0.x] [--keep-dotnet]` (was
//! tools/il2cpp/device_lock.py): writes setup-rs/crates/il2cpp/src/lock.rs, the pins of what `pmu
//! il2cpp --on-device` fetches on a handheld: the Debian 12 arm64 packages and the .NET runtime, each
//! with its sha256 (Debian's own index gives the package hashes; the .NET tarball is hashed after
//! downloading it once, and checked against Microsoft's published sha512).
//!
//! Run it on the porter's computer when the pins should move (a Debian point release, a .NET 8
//! servicing release); the handheld never resolves anything. Old pins keep working: the setup falls
//! back to snapshot.debian.org (every version Debian ever published) when the pool has moved on.
use std::collections::HashMap;
use std::fmt::Write as _;
use std::io::Read;

use sha2::Digest;

const SUITE: &str = "bookworm";
const DOTNET_META: &str = "https://builds.dotnet.microsoft.com/dotnet/release-metadata/8.0/releases.json";

fn index_url() -> String {
    format!("https://deb.debian.org/debian/dists/{SUITE}/main/binary-arm64/Packages.xz")
}

// what each group needs (no dependency resolution: Debian's dependencies pull in far more than these
// programs load; the setup's check verifies the libraries on the unpacked files)
const GROUPS: &[(&str, &[&str])] = &[
    ("base", &["libc6", "libgcc-s1", "libstdc++6", "zlib1g"]),
    (
        "llvm",
        &[
            "libllvm14", "libedit2", "libbsd0", "libmd0", "libffi8", "libtinfo6", "libxml2", "libicu72", "liblzma5", "libz3-4", "libclang-cpp14",
            "clang-14", "libclang-common-14-dev", "lld-14", "llvm-14",
        ],
    ),
    // Mono and the class libraries il2cpp.exe / UnityLinker.exe (2017.4 - 2019.4) load; libmono-native.so's
    // GSSAPI (System.Net.Security), loaded with it
    (
        "mono",
        &[
            "mono-runtime-sgen", "mono-runtime-common", "libmono-corlib4.5-dll", "libmono-system4.0-cil", "libmono-system-core4.0-cil",
            "libmono-system-xml4.0-cil", "libmono-system-configuration4.0-cil", "libmono-security4.0-cil", "libmono-system-numerics4.0-cil",
            "libmono-system-security4.0-cil", "libmono-posix4.0-cil", "libgssapi-krb5-2", "libkrb5-3", "libk5crypto3", "libkrb5support0",
            "libcom-err2", "libkeyutils1",
        ],
    ),
    // Unity 2019.x's tools: Mono.Cecil built for netstandard (the facades, from mono-devel: only
    // usr/lib/mono/4.5/Facades is unpacked), UnityLinker's JSON (System.Web.Extensions)
    ("mono-facades", &["mono-devel"]),
    ("mono-web", &["libmono-system-web-extensions4.0-cil", "libmono-system-web4.0-cil", "libmono-system-web-applicationservices4.0-cil"]),
    ("dotnet", &["libssl3"]), // (+ Microsoft's runtime: .NET hashes with OpenSSL on Linux)
];

/// Debian's Packages index: {package: {field: value}} (the first stanza of each package).
pub fn packages(text: &str) -> HashMap<String, HashMap<String, String>> {
    let mut out: HashMap<String, HashMap<String, String>> = HashMap::new();
    let mut cur: HashMap<String, String> = HashMap::new();
    let mut key = String::new();
    let flush = |cur: &mut HashMap<String, String>, out: &mut HashMap<String, HashMap<String, String>>| {
        if let Some(p) = cur.get("Package").cloned() {
            out.entry(p).or_insert_with(|| std::mem::take(cur));
        }
        cur.clear();
    };
    for line in text.split('\n') {
        if line.is_empty() {
            flush(&mut cur, &mut out);
            continue;
        }
        if line.starts_with([' ', '\t']) {
            if let Some(v) = cur.get_mut(&key) {
                v.push('\n');
                v.push_str(line);
            }
            continue;
        }
        let (k, v) = line.split_once(':').unwrap_or((line, ""));
        key = k.to_string();
        cur.insert(key.clone(), v.trim().to_string());
    }
    flush(&mut cur, &mut out);
    out
}

fn get(url: &str) -> Result<Vec<u8>, String> {
    let mut c = pkgzip::http::Client::default();
    c.max_body = 1 << 31;
    let r = c.get_following(url, &[("User-Agent", "pmu-dev"), ("Connection", "close")], 10).map_err(|e| format!("{url}: {e}"))?;
    if r.status >= 400 {
        return Err(format!("{url}: HTTP Error {}", r.status));
    }
    Ok(r.body)
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// (version, url, size, sha256) of .NET 8's linux-arm64 runtime (the newest, or `version`)
fn dotnet(version: Option<&str>) -> Result<(String, String, u64, String), String> {
    let meta: serde_json::Value = serde_json::from_slice(&get(DOTNET_META)?).map_err(|e| format!("{DOTNET_META}: {e}"))?;
    let rel = meta["releases"]
        .as_array()
        .and_then(|r| r.iter().find(|r| r["runtime"].is_object() && version.is_none_or(|v| r["runtime"]["version"] == v)))
        .ok_or("no such .NET 8 runtime in Microsoft's release metadata")?;
    let f = rel["runtime"]["files"]
        .as_array()
        .and_then(|fs| fs.iter().find(|f| f["rid"] == "linux-arm64" && f["name"].as_str().is_some_and(|n| n.ends_with(".tar.gz"))))
        .ok_or("no linux-arm64 tarball in the release")?;
    let url = f["url"].as_str().ok_or("no url")?.to_string();
    let data = get(&url)?;
    if hex(&sha2::Sha512::digest(&data)) != f["hash"].as_str().unwrap_or("").to_lowercase() {
        return Err(format!("{url}: sha512 differs from Microsoft's release metadata"));
    }
    Ok((rel["runtime"]["version"].as_str().unwrap_or("").to_string(), url, data.len() as u64, hex(&sha2::Sha256::digest(&data))))
}

fn snapshot_now() -> String {
    // (time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()))
    let s = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs()) as i64;
    let (days, rem) = (s.div_euclid(86400), s.rem_euclid(86400));
    // civil from days (Howard Hinnant)
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + (m <= 2) as i64;
    format!("{y:04}{m:02}{d:02}T{:02}{:02}{:02}Z", rem / 3600, rem % 3600 / 60, rem % 60)
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let usage = "usage: pmu-dev il2cpp-lock [--packages Packages.xz] [--dotnet 8.0.x] [--keep-dotnet] [--out lock.rs]";
    let (mut pk_path, mut want, mut keep, mut out) = (None, None, false, None);
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--packages" => pk_path = args.get(i + 1).cloned(),
            "--dotnet" => want = args.get(i + 1).cloned(),
            "--out" => out = args.get(i + 1).cloned(),
            "--keep-dotnet" => {
                keep = true;
                i += 1;
                continue;
            }
            _ => return Err(usage.into()),
        }
        i += 2;
    }
    let out = out.unwrap_or_else(|| concat!(env!("CARGO_MANIFEST_DIR"), "/../il2cpp/src/lock.rs").to_string());
    let xz = match pk_path {
        Some(p) => std::fs::read(&p).map_err(|e| format!("{p}: {e}"))?,
        None => get(&index_url())?,
    };
    let mut text = String::new();
    lzma_rust2::XzReader::new(&xz[..], true).read_to_string(&mut text).map_err(|e| format!("Packages.xz: {e}"))?;
    let p = packages(&text);
    let (dv, du, ds, dh) = if keep {
        let d = &il2cpp::lock::DOTNET;
        (d.version.to_string(), d.url.to_string(), d.size, d.sha256.to_string())
    } else {
        dotnet(want.as_deref())?
    };
    let mut o = String::new();
    o.push_str(
        "//! The pins of the handheld's tools for `pmu il2cpp --on-device` (written by `pmu-dev il2cpp-lock`,\n\
         //! crates/devtools): Debian bookworm arm64 packages (deb.debian.org, else snapshot.debian.org at\n\
         //! SNAPSHOT) and Microsoft's .NET 8 runtime for linux-arm64 (Unity 2020.1+ only).\n\n\
         /// A Debian package: name, group, version, filename (in the pool), size, sha256.\n\
         pub struct Package {\n    pub name: &'static str,\n    pub group: &'static str,\n    pub version: &'static str,\n    pub filename: &'static str,\n    pub size: u64,\n    pub sha256: &'static str,\n}\n\n\
         /// The .NET runtime: version, url, size, sha256.\n\
         pub struct Dotnet {\n    pub version: &'static str,\n    pub url: &'static str,\n    pub size: u64,\n    pub sha256: &'static str,\n}\n\n",
    );
    writeln!(o, "pub const SUITE: &str = \"{SUITE}\";\npub const SNAPSHOT: &str = \"{}\";\n\npub const PACKAGES: &[Package] = &[", snapshot_now()).unwrap();
    let (mut n, mut total) = (0, 0u64);
    for (group, names) in GROUPS {
        for name in *names {
            let q = p.get(*name).ok_or_else(|| format!("{name}: not in Debian's index"))?;
            let f = |k: &str| q.get(k).cloned().unwrap_or_default();
            let size: u64 = f("Size").parse().map_err(|_| format!("{name}: no Size"))?;
            writeln!(
                o,
                "    Package {{ name: \"{name}\", group: \"{group}\", version: \"{}\", filename: \"{}\", size: {size}, sha256: \"{}\" }},",
                f("Version"),
                f("Filename"),
                f("SHA256")
            )
            .unwrap();
            n += 1;
            total += size;
        }
    }
    writeln!(o, "];\n\npub const DOTNET: Dotnet = Dotnet {{ version: \"{dv}\", url: \"{du}\", size: {ds}, sha256: \"{dh}\" }};").unwrap();
    std::fs::write(&out, o).map_err(|e| format!("{out}: {e}"))?;
    println!("{out}: {n} packages, {:.1} MB; .NET {dv}", total as f64 / 1e6);
    Ok(0)
}

#[cfg(test)]
mod tests {
    #[test]
    fn index() {
        let t = "Package: a\nVersion: 1\nDescription: x\n more\n\nPackage: b\nVersion: 2\nSize: 7\n\nPackage: a\nVersion: 9\n";
        let p = super::packages(t);
        assert_eq!(p["a"]["Version"], "1");
        assert_eq!(p["a"]["Description"], "x\n more");
        assert_eq!(p["b"]["Size"], "7");
    }

    #[test]
    fn same_layout_as_lock_rs() {
        // the file this tool writes is the one il2cpp compiles: its pins are where the tool puts them
        let t = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../il2cpp/src/lock.rs")).unwrap();
        assert!(t.contains("pub const SUITE: &str = \"bookworm\";"));
        assert_eq!(t.matches("    Package { name: ").count(), il2cpp::lock::PACKAGES.len());
        let names: Vec<&str> = super::GROUPS.iter().flat_map(|g| g.1.iter().copied()).collect();
        assert_eq!(names, il2cpp::lock::PACKAGES.iter().map(|p| p.name).collect::<Vec<_>>());
    }
}
