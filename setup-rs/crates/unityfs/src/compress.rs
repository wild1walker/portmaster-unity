//! Block compression as the Python does it: LZ4 and LZ4 HC through the C library python-lz4
//! bundles (lz4 1.9.4, so the compressed bytes are the same), LZMA decompression (Unity's 5-byte
//! header: properties and dictionary size, then the raw stream).
use crate::error::{bad, Error, Result};
use std::io::Write;
use std::os::raw::{c_char, c_int, c_void};

/// UnityPy's compression kinds (a block's or the blocks info's flags & 0x3F).
pub const NONE: u32 = 0;
pub const LZMA: u32 = 1;
pub const LZ4: u32 = 2;
pub const LZ4HC: u32 = 3;

/// The LZ4 HC level UnityPy's compress_lz4 uses (lz4.block.compress's default).
pub const HC_LEVEL: i32 = 9;

// python-lz4 4.4.5 (lz4/block/_block.c) compresses through the streaming API, a fresh stream for
// each call: LZ4_resetStream + LZ4_compress_fast_continue, LZ4_resetStreamHC + LZ4_compress_HC_continue.
// (Its output differs from LZ4_compress_default's for inputs under 64 KiB: another hash table.)
// lz4-sys does not declare these; they are in the same library.
extern "C" {
    fn LZ4_createStream() -> *mut c_void;
    fn LZ4_freeStream(s: *mut c_void) -> c_int;
    fn LZ4_resetStream(s: *mut c_void);
    fn LZ4_compress_fast_continue(
        s: *mut c_void,
        src: *const c_char,
        dst: *mut c_char,
        src_size: c_int,
        dst_cap: c_int,
        acceleration: c_int,
    ) -> c_int;
    fn LZ4_createStreamHC() -> *mut c_void;
    fn LZ4_freeStreamHC(s: *mut c_void) -> c_int;
    fn LZ4_resetStreamHC(s: *mut c_void, level: c_int);
    fn LZ4_compress_HC_continue(
        s: *mut c_void,
        src: *const c_char,
        dst: *mut c_char,
        src_size: c_int,
        dst_cap: c_int,
    ) -> c_int;
}

fn bound(n: usize) -> Result<usize> {
    if n > 0x7E00_0000 {
        return Err(Error::Unsupported(format!(
            "LZ4: {n} bytes is more than one block takes"
        )));
    }
    Ok(n + n / 255 + 16)
}

/// lz4.block.compress(data, mode='high_compression', compression=level, store_size=False).
pub fn lz4hc(data: &[u8], level: i32) -> Result<Vec<u8>> {
    let cap = bound(data.len())?;
    let mut out = vec![0u8; cap];
    // SAFETY: FFI into the bundled lz4 1.9.4: a stream of its own, freed here; the pointers cover
    // `data` and `out`, whose sizes are passed.
    let n = unsafe {
        let st = LZ4_createStreamHC();
        if st.is_null() {
            return bad("LZ4 HC: no memory for a stream");
        }
        LZ4_resetStreamHC(st, level as c_int);
        let n = LZ4_compress_HC_continue(
            st,
            data.as_ptr() as *const c_char,
            out.as_mut_ptr() as *mut c_char,
            data.len() as c_int,
            cap as c_int,
        );
        LZ4_freeStreamHC(st);
        n
    };
    if n <= 0 && !data.is_empty() {
        return bad("LZ4 HC compression failed");
    }
    out.truncate(n.max(0) as usize);
    Ok(out)
}

/// lz4.block.compress(data, store_size=False): LZ4's fast mode, acceleration 1.
pub fn lz4_fast(data: &[u8]) -> Result<Vec<u8>> {
    let cap = bound(data.len())?;
    let mut out = vec![0u8; cap];
    // SAFETY: as in lz4hc.
    let n = unsafe {
        let st = LZ4_createStream();
        if st.is_null() {
            return bad("LZ4: no memory for a stream");
        }
        LZ4_resetStream(st);
        let n = LZ4_compress_fast_continue(
            st,
            data.as_ptr() as *const c_char,
            out.as_mut_ptr() as *mut c_char,
            data.len() as c_int,
            cap as c_int,
            1,
        );
        LZ4_freeStream(st);
        n
    };
    if n <= 0 && !data.is_empty() {
        return bad("LZ4 compression failed");
    }
    out.truncate(n.max(0) as usize);
    Ok(out)
}

/// UnityPy's compress_lz4 (both LZ4 and LZ4 HC blocks): HC level 9.
pub fn compress_lz4(data: &[u8]) -> Result<Vec<u8>> {
    lz4hc(data, HC_LEVEL)
}

/// lz4.block.decompress(data, uncompressed_size=usize): exactly usize bytes, else an error.
pub fn lz4_decompress(data: &[u8], usize: usize) -> Result<Vec<u8>> {
    let mut out = vec![0u8; usize];
    if usize > 0x7FFF_FFFF || data.len() > 0x7FFF_FFFF {
        return Err(Error::Unsupported("LZ4 block over 2 GB".into()));
    }
    // SAFETY: as in lz4hc; LZ4_decompress_safe never writes past the capacity given.
    let n = unsafe {
        lz4_sys::LZ4_decompress_safe(
            data.as_ptr() as *const c_char,
            out.as_mut_ptr() as *mut c_char,
            data.len() as c_int,
            usize as c_int,
        )
    };
    if n < 0 {
        return bad("LZ4 block corrupt");
    }
    if n as usize != usize {
        return bad(format!("LZ4 block decompressed to {n} bytes, not {usize}"));
    }
    Ok(out)
}

/// A Unity LZMA block (5 bytes of properties, then the raw stream) decompressed into `out`,
/// usize bytes of it, as they come (in pieces: a big block is never held whole).
pub fn lzma_decompress_to(
    data: &mut impl std::io::BufRead,
    usize: u64,
    out: &mut impl Write,
) -> Result<()> {
    let opts = lzma_rs::decompress::Options {
        unpacked_size: lzma_rs::decompress::UnpackedSize::UseProvided(Some(usize)),
        memlimit: None,
        allow_incomplete: false,
    };
    lzma_rs::lzma_decompress_with_options(data, out, &opts).map_err(|e| match e {
        lzma_rs::error::Error::IoError(e) => Error::Io(e),
        e => Error::Format(format!("LZMA: {e:?}")),
    })
}

/// A Unity LZMA block decompressed whole.
pub fn lzma_decompress(data: &[u8], usize: usize) -> Result<Vec<u8>> {
    let mut out = Vec::with_capacity(usize);
    lzma_decompress_to(&mut &data[..], usize as u64, &mut out)?;
    if out.len() != usize {
        return bad(format!(
            "LZMA block decompressed to {} bytes, not {usize}",
            out.len()
        ));
    }
    Ok(out)
}

/// A block decompressed by its flags' kind (UnityPy's DECOMPRESSION_MAP).
pub fn decompress(data: &[u8], usize: usize, flags: u32) -> Result<Vec<u8>> {
    match flags & 0x3F {
        NONE => Ok(data.to_vec()),
        LZMA => lzma_decompress(data, usize),
        LZ4 | LZ4HC => lz4_decompress(data, usize),
        k => Err(Error::Unsupported(format!("compression {k}"))),
    }
}

/// A block compressed by kind (UnityPy's COMPRESSION_MAP; LZMA is not written by the setup).
pub fn compress(data: &[u8], kind: u32) -> Result<Vec<u8>> {
    match kind & 0x3F {
        NONE => Ok(data.to_vec()),
        LZ4 | LZ4HC => compress_lz4(data),
        k => Err(Error::Unsupported(format!("writing compression {k}"))),
    }
}

/// Threads for compressing chunks: the cores this process may use (bigbundle.threads).
pub fn threads() -> usize {
    std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1)
}

/// (chunk, compressed chunk) for each chunk, in order, compressed on n threads with two chunks a
/// thread in flight (bigbundle.compress_chunks): each chunk is compressed on its own, so the
/// bytes are the ones one thread makes. `sink` gets them in order; an error stops the run.
pub fn compress_chunks<I, F>(
    chunks: I,
    n: usize,
    compress: F,
    mut sink: impl FnMut(Vec<u8>, Vec<u8>) -> Result<()>,
) -> Result<()>
where
    I: Iterator<Item = Result<Vec<u8>>>,
    F: Fn(&[u8]) -> Result<Vec<u8>> + Sync,
{
    if n <= 1 {
        for c in chunks {
            let c = c?;
            let z = compress(&c)?;
            sink(c, z)?;
        }
        return Ok(());
    }
    use std::collections::BTreeMap;
    use std::sync::{mpsc, Mutex};
    type Done = BTreeMap<usize, (Vec<u8>, Result<Vec<u8>>)>;
    let (jtx, jrx) = mpsc::channel::<(usize, Vec<u8>)>();
    let jrx = Mutex::new(jrx);
    let (rtx, rrx) = mpsc::channel::<(usize, Vec<u8>, Result<Vec<u8>>)>();
    let compress = &compress;
    let jrx = &jrx;
    std::thread::scope(move |s| -> Result<()> {
        for _ in 0..n {
            let rtx = rtx.clone();
            s.spawn(move || loop {
                let job = jrx.lock().unwrap().recv();
                let Ok((i, c)) = job else { return };
                let z = compress(&c);
                if rtx.send((i, c, z)).is_err() {
                    return;
                }
            });
        }
        drop(rtx);
        // (an early return drops jtx, which ends the threads)
        let jtx = jtx;
        let mut done: Done = BTreeMap::new();
        let take = |done: &mut Done, want: usize| -> Result<(Vec<u8>, Vec<u8>)> {
            loop {
                if let Some((c, z)) = done.remove(&want) {
                    return Ok((c, z?));
                }
                let (i, c, z) = rrx
                    .recv()
                    .map_err(|_| Error::Format("compression thread ended".into()))?;
                done.insert(i, (c, z));
            }
        };
        let (mut sent, mut next) = (0usize, 0usize);
        for c in chunks {
            jtx.send((sent, c?))
                .map_err(|_| Error::Format("compression thread ended".into()))?;
            sent += 1;
            while sent - next >= 2 * n {
                let (c, z) = take(&mut done, next)?;
                next += 1;
                sink(c, z)?;
            }
        }
        drop(jtx);
        while next < sent {
            let (c, z) = take(&mut done, next)?;
            next += 1;
            sink(c, z)?;
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lz4_round_trip() {
        let data: Vec<u8> = (0..300_000u32)
            .map(|i| (i % 251) as u8 ^ (i / 1000) as u8)
            .collect();
        for z in [
            lz4hc(&data, 9).unwrap(),
            lz4hc(&data, 12).unwrap(),
            lz4_fast(&data).unwrap(),
        ] {
            assert!(z.len() < data.len());
            assert_eq!(lz4_decompress(&z, data.len()).unwrap(), data);
        }
        assert!(lz4_decompress(&lz4hc(&data, 9).unwrap(), data.len() - 1).is_err());
        assert_eq!(lz4hc(b"", 9).unwrap(), vec![0]);
    }

    #[test]
    fn chunks_in_order() {
        let chunks: Vec<Vec<u8>> = (0..50u8).map(|i| vec![i; 1000 + i as usize]).collect();
        for n in [1, 4] {
            let mut got = Vec::new();
            compress_chunks(
                chunks.clone().into_iter().map(Ok),
                n,
                compress_lz4,
                |c, z| {
                    got.push((c, z));
                    Ok(())
                },
            )
            .unwrap();
            assert_eq!(got.len(), 50);
            for (i, (c, z)) in got.iter().enumerate() {
                assert_eq!(c, &chunks[i]);
                assert_eq!(z, &compress_lz4(&chunks[i]).unwrap());
            }
        }
    }
}
