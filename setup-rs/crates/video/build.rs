//! Compiles libvpx's VP8 encoder and decoder (vendor/libvpx: Debian 12's libvpx 1.12.0 source,
//! configured for arm64 with NEON; VENDOR.txt) with the flags its own build uses, and the C
//! interface to them (csrc/vpx_capi.c). The vendored configuration is arm64's: on another
//! architecture nothing is built and the crate's encoder and decoder report that they are absent
//! (cfg `no_vpx`).
use std::path::PathBuf;

fn main() {
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let vpx = root.join("../../vendor/libvpx");
    println!("cargo:rustc-check-cfg=cfg(no_vpx)");
    println!("cargo:rerun-if-changed=csrc/vpx_capi.c");
    println!("cargo:rerun-if-changed=../../vendor/libvpx/SOURCES.txt");
    if std::env::var("CARGO_CFG_TARGET_ARCH").unwrap() != "aarch64" {
        println!("cargo:rustc-cfg=no_vpx");
        return;
    }
    let sources =
        std::fs::read_to_string(vpx.join("SOURCES.txt")).expect("vendor/libvpx/SOURCES.txt");
    let mut b = cc::Build::new();
    b.include(&vpx)
        .opt_level(3)
        .debug(false)
        .warnings(false)
        .define("NDEBUG", None)
        .define("_FILE_OFFSET_BITS", "64")
        .flag("-march=armv8-a")
        .flag("-std=gnu89")
        .flag("-U_FORTIFY_SOURCE")
        .flag("-D_FORTIFY_SOURCE=0");
    for f in sources.lines().map(str::trim).filter(|l| !l.is_empty()) {
        b.file(vpx.join(f));
    }
    b.file(root.join("csrc/vpx_capi.c"));
    b.compile("vpx");
}
