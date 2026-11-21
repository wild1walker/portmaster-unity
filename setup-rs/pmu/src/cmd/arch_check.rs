//! `pmu arch-check <release zip | unpacked release folder> [--list]` (pmu's own, no Python): the
//! aarch64-only check of a port release (pkgzip::arch, docs/arm64.md "Shipping aarch64 only"): no
//! 32-bit ARM (EM_ARM) ELF file anywhere, the donor APKs' entries included, and every piece the
//! 64-bit route needs there, for aarch64. Exit 0 when it passes, 1 with what is wrong on stderr.
//! --list: every ELF file found and its machine, nothing checked.
use std::path::Path;
use std::process::ExitCode;

use pkgzip::arch::{self, machine_name};

const USAGE: &str = "usage: pmu arch-check <release zip | release folder> [--list]";

pub fn run(args: &[String]) -> ExitCode {
    let mut path: Option<&str> = None;
    let mut list = false;
    for a in args {
        match a.as_str() {
            "--list" => list = true,
            "-h" | "--help" => {
                println!("{USAGE}");
                return ExitCode::SUCCESS;
            }
            _ if a.starts_with('-') => {
                eprintln!("{USAGE}\npmu arch-check: unknown option {a}");
                return ExitCode::from(2);
            }
            _ if path.is_none() => path = Some(a),
            _ => {
                eprintln!("{USAGE}");
                return ExitCode::from(2);
            }
        }
    }
    let Some(path) = path else {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    };
    let p = Path::new(path);
    if list {
        let elves = if p.is_dir() {
            arch::scan(p)
        } else {
            arch::scan_archive(path, &pkgzip::py::basename(path).to_string())
        };
        return match elves {
            Ok(v) => {
                for e in v {
                    println!(
                        "{}\t{}-bit\t{}",
                        machine_name(e.machine),
                        if e.class == 2 { 64 } else { 32 },
                        e.path
                    );
                }
                ExitCode::SUCCESS
            }
            Err(e) => {
                eprintln!("pmu arch-check: {e}");
                ExitCode::from(1)
            }
        };
    }
    let r = if p.is_dir() {
        arch::check_release(p)
    } else {
        let tmp = p
            .parent()
            .filter(|d| !d.as_os_str().is_empty())
            .map(Path::to_path_buf)
            .unwrap_or_else(std::env::temp_dir);
        arch::check_release_zip(path, &tmp)
    };
    let r = match r {
        Ok(r) => r,
        Err(e) => {
            eprintln!("pmu arch-check: {e}");
            return ExitCode::from(1);
        }
    };
    let n64 = r
        .elves
        .iter()
        .filter(|e| e.machine == arch::EM_AARCH64)
        .count();
    println!(
        "{}: {} route, {} ELF files ({} aarch64, {} armhf)",
        r.port,
        if r.route.is_empty() { "no" } else { &r.route },
        r.elves.len(),
        n64,
        r.armhf.len()
    );
    for x in &r.armhf {
        eprintln!("armhf: {x}");
    }
    for x in &r.missing {
        eprintln!("missing: {x}");
    }
    if r.ok() {
        println!("aarch64 only: yes");
        ExitCode::SUCCESS
    } else {
        eprintln!(
            "aarch64 only: no ({} armhf files, {} pieces of the 64-bit route missing)",
            r.armhf.len(),
            r.missing.len()
        );
        ExitCode::from(1)
    }
}
