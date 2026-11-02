//! A macOS installer package (GOG's): the xar header and its zlib TOC, every file's data
//! (stored, zlib, bzip2), and `macpkg` unpacking it (Payload and Scripts: gzip, pbzx, cpio).
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::{drain, Scratch};

fuzz_target!(|data: &[u8]| {
    let s = Scratch::new();
    let p = s.file("game.pkg", data);
    let p = p.to_str().unwrap();
    let Ok(x) = pkgzip::xar::Xar::open(p) else {
        return;
    };
    for e in x.entries.iter().take(256) {
        if let Ok(r) = x.reader(e) {
            let _ = drain(r, 64 << 20);
        }
    }
    let root = s.path("root");
    let _ = pkgzip::macpkg::extract(p, root.to_str().unwrap(), &mut |_| {}, None);
});
