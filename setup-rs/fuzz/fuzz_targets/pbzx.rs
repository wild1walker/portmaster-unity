//! The payload streams macpkg reads: pbzx (its chunks, each xz or stored), gzip members, xar's
//! zlib and bzip2 encodings, and lzma.decompress of a chunk (xz and .lzma streams). The first
//! byte picks the reader.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pkgzip::streams::{payload_stream, xz_decompress, Bzip2, Gzip, Inflate};
use pmu_fuzz::drain;

const CAP: u64 = 64 << 20;

fuzz_target!(|data: &[u8]| {
    let Some((&which, rest)) = data.split_first() else {
        return;
    };
    match which % 5 {
        0 => {
            let _ = drain(Inflate::new(rest), CAP);
        }
        1 => {
            let _ = drain(Bzip2::new(rest), CAP);
        }
        2 => {
            let _ = drain(Gzip::new(rest), CAP);
        }
        3 => {
            let _ = xz_decompress(rest);
        }
        _ => {
            if let Ok(s) = payload_stream(Box::new(std::io::Cursor::new(rest.to_vec()))) {
                let _ = drain(s, CAP);
            }
        }
    }
});
