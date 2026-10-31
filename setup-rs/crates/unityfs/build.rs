//! Embeds the class database when PMU_TPK names one at build time (UnityPy's resources/lzma.tpk:
//! Unity-derived data, never in the repository); else an empty placeholder, and the database is
//! read at run time (ClassDb::open, or $PMU_TPK).
use std::path::PathBuf;

fn main() {
    let out = PathBuf::from(std::env::var_os("OUT_DIR").unwrap()).join("classdb.tpk");
    println!("cargo:rerun-if-env-changed=PMU_TPK");
    let data = match std::env::var_os("PMU_TPK") {
        Some(p) if !p.is_empty() => {
            println!("cargo:rerun-if-changed={}", PathBuf::from(&p).display());
            std::fs::read(&p)
                .unwrap_or_else(|e| panic!("PMU_TPK {}: {e}", PathBuf::from(&p).display()))
        }
        _ => Vec::new(),
    };
    if std::fs::read(&out).ok().as_deref() != Some(&data[..]) {
        std::fs::write(&out, &data).unwrap();
    }
}
