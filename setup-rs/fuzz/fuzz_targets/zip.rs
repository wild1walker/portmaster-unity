//! A zip (APK, OBB, the setup's own zips): the end record and central directory, every entry's
//! local header and data (stored, deflate, bzip2, LZMA) to its CRC, and `zip-extract`.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::{drain, Scratch};

fuzz_target!(|data: &[u8]| {
    let s = Scratch::new();
    let p = s.file("a.zip", data);
    let p = p.to_str().unwrap();
    let Ok(z) = pkgzip::zip::Archive::open(p) else {
        return;
    };
    for e in z.entries.iter().take(256) {
        let _ = z.data_offset(e);
        if let Ok(r) = z.reader(e) {
            let _ = drain(r, 64 << 20);
        }
    }
    if z.entries.len() <= 64 {
        let out = s.path("out");
        let _ = pkgzip::zip::extract_all(p, out.to_str().unwrap());
    }
});
