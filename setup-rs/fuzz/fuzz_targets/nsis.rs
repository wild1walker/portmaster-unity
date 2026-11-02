//! An NSIS installer (Unity's): the first header, the script header (LZMA, deflate, Unity 6's
//! chunks), its entries and strings, and the files extracted from their blocks.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::Scratch;

fuzz_target!(|data: &[u8]| {
    let s = Scratch::new();
    let p = s.file("setup.exe", data);
    let Ok(mut n) = pkgzip::nsis::Nsis::open(p.to_str().unwrap(), Box::new(|_| {})) else {
        return;
    };
    let Ok(files) = n.files() else {
        return;
    };
    for (_, off, _) in files.iter().take(64) {
        let _ = n.extract(*off);
    }
});
