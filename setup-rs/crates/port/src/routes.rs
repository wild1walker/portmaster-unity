//! The routes (port_template/<route>/route.json), their templates and @@TOKENS@@, and the donors a
//! route's release ships (port.py's routes, render, template_path, route_donors, port_donors).
use std::path::{Path, PathBuf};

use crate::val::{Dict, Val};
use crate::{exit, pyjson, Repo};
use pkgzip::{io_at, Result};

/// {route: route.json} of the templates present, each with its folder ("dir"), in name order.
pub fn routes(repo: &Repo) -> Result<Vec<(String, Dict)>> {
    let t = repo.templates();
    let mut names: Vec<String> = match std::fs::read_dir(&t) {
        Ok(rd) => rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect(),
        Err(_) => return Ok(vec![]),
    };
    names.sort();
    let mut out = Vec::new();
    for n in names {
        let p = t.join(&n).join("route.json");
        if p.is_file() {
            let text = read_text(&p)?;
            let Val::Dict(mut r) = pyjson::loads(&text)? else {
                return Err(pkgzip::Error::Fail(format!("{}: not an object", p.display())));
            };
            r.set("dir", Val::Str(t.join(&n).to_string_lossy().into_owned()));
            out.push((n, r));
        }
    }
    Ok(out)
}

pub fn route(repo: &Repo, name: &str) -> Result<Option<Dict>> {
    Ok(routes(repo)?.into_iter().find(|(n, _)| n == name).map(|(_, r)| r))
}

/// A file read as Python's open(path).read() reads it: UTF-8, newlines as '\n'.
pub fn read_text(p: &Path) -> Result<String> {
    let b = std::fs::read(p).map_err(io_at(p))?;
    let s = String::from_utf8(b).map_err(|e| {
        pkgzip::Error::Fail(format!("UnicodeDecodeError: 'utf-8' codec can't decode {}: {e}", p.display()))
    })?;
    Ok(universal_newlines(s))
}

/// Text mode's newline translation on reading: "\r\n" and "\r" become "\n".
pub fn universal_newlines(s: String) -> String {
    if !s.contains('\r') {
        return s;
    }
    s.replace("\r\n", "\n").replace('\r', "\n")
}

/// render(text, values, where): @@TOKEN@@ replaced; an unknown one stops.
pub fn render(text: &str, values: &[(&str, String)], at: &str) -> Result<String> {
    let mut out = String::with_capacity(text.len());
    let mut rest = text;
    while let Some(i) = rest.find("@@") {
        let after = &rest[i + 2..];
        let n = after.bytes().take_while(|c| c.is_ascii_uppercase() || c.is_ascii_digit() || *c == b'_').count();
        if n > 0 && after[n..].starts_with("@@") {
            let k = &after[..n];
            let v = values
                .iter()
                .find(|(x, _)| *x == k)
                .ok_or_else(|| exit(format!("{at}: unknown template value @@{k}@@")))?;
            out.push_str(&rest[..i]);
            out.push_str(&v.1);
            rest = &after[n + 2..];
        } else {
            out.push_str(&rest[..i + 1]);
            rest = &rest[i + 1..];
        }
    }
    out.push_str(rest);
    Ok(out)
}

/// A route's template file, else the common one (port_template/common/).
pub fn template_path(repo: &Repo, route: &Dict, name: &str) -> Result<PathBuf> {
    let dir = PathBuf::from(route.str("dir"));
    for d in [dir.clone(), repo.templates().join("common")] {
        let p = d.join(name);
        if p.is_file() {
            return Ok(p);
        }
    }
    Err(exit(format!("template {name} is in neither {} nor port_template/common", dir.display())))
}

/// donor_name: donor-<version>-<backend>-<armv7|arm64>.apk
pub fn donor_name(version: &str, backend: &str, abi: &str) -> String {
    let a = match abi {
        "armeabi-v7a" => "armv7",
        "arm64-v8a" => "arm64",
        x => x,
    };
    format!("donor-{version}-{backend}-{a}.apk")
}

/// DONOR_BUILD: tools/donor/build.sh's name for a donor's build.
pub fn donor_build(backend: &str, abi: &str) -> &'static str {
    match (backend, abi) {
        ("mono", "armeabi-v7a") => "MonoArmv7",
        ("il2cpp", "arm64-v8a") => "Il2cppArm64",
        _ => "?",
    }
}

/// The donor file names a route's setup reads (route.json "donors": "player" = the port's
/// player.backend/abi, or "<backend>/<abi>"), for each Unity version.
pub fn route_donors(route: &Dict, versions: &[Val], backend: &str, abi: &str) -> Vec<String> {
    let mut specs: Vec<(String, String)> = Vec::new();
    for x in route.list("donors") {
        let x = x.py_str();
        let ba = if x == "player" {
            (backend.to_string(), abi.to_string())
        } else {
            // (tuple(x.split('/')): a spec of two parts)
            let mut it = x.splitn(2, '/');
            (it.next().unwrap_or("").to_string(), it.next().unwrap_or("").to_string())
        };
        if !specs.contains(&ba) {
            specs.push(ba);
        }
    }
    let mut out = Vec::new();
    for v in versions {
        for (b, a) in &specs {
            out.push(donor_name(&v.py_str(), b, a));
        }
    }
    out
}

/// The donors a port's release ships: its route's for each of game.unity_versions, and those
/// player.donors names besides.
pub fn port_donors(cfg: &Dict, route: &Dict) -> Vec<Val> {
    let pl = cfg.dict("player");
    let get = |k: &str, d: &str| pl.get(k).map_or(d.to_string(), Val::py_str);
    let need = route_donors(route, cfg.dict("game").list("unity_versions"), &get("backend", "mono"), &get("abi", "armeabi-v7a"));
    let mut out: Vec<Val> = need.iter().map(|n| Val::s(n)).collect();
    for n in pl.list("donors") {
        if !need.iter().any(|m| n.eq(&Val::s(m))) {
            out.push(n.clone());
        }
    }
    out
}
