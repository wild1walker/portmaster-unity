//! A cpio archive (a package's Payload or Scripts): odc and newc headers, names, padding, data.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pkgzip::cpio::{exact, Cpio};

fuzz_target!(|data: &[u8]| {
    let mut c = Cpio::new(data);
    while let Ok(Some(m)) = c.next_member() {
        let r = if m.size < 1 << 20 {
            exact(c.data(), m.size as usize, "cpio archive ends early").map(|_| ())
        } else {
            c.skip(m.size)
        };
        if r.is_err() {
            break;
        }
    }
});
