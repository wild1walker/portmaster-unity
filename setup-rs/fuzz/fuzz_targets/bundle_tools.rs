//! The bundle readers of `unpack` and android-data's platform patching: a compressed build's
//! data.unity3d (its tables, every file extracted) and a bundle's header, blocks and serialized
//! nodes read and patched in place.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::Scratch;
use unityfs::platform::{bundle_platforms, patch_bundle_platform, MiniBundle};
use unityfs::unpack::Archive;

fuzz_target!(|data: &[u8]| {
    let s = Scratch::new();
    let p = s.file("data.unity3d", data);
    if let Ok(a) = Archive::open(&p) {
        let out = s.path("out");
        let _ = std::fs::create_dir_all(&out);
        if a.files.len() <= 64 {
            let _ = a.extract(&out, &mut |_| {});
        }
    }
    if let Ok(b) = MiniBundle::open(&p) {
        if let Ok(mut f) = std::fs::File::open(&p) {
            let _ = b.read_plain(&mut f, 0, 4096);
        }
        let _ = bundle_platforms(&p);
        let _ = patch_bundle_platform(&p, 11);
    }
});
