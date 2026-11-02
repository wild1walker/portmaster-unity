//! A Shader's program blob as its tree gives it: the platforms with their offsets and lengths
//! (one block each, or 2019.3+'s segments), the LZ4 blocks, each blob's entry table and entries
//! (subprograms of any serializer layout, parameter entries), written back as the step stores
//! them; and one entry and one LZ4 block parsed on their own.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::Input;
use shaderx::blob::{BlobFields, Entry, Lengths, ShaderBlobs};
use std::collections::BTreeSet;

fuzz_target!(|data: &[u8]| {
    let mut inp = Input::new(data);
    let sel = inp.u8();
    let nested = sel & 1 != 0;
    let wide = sel & 2 != 0;
    let n = 1 + (sel >> 2 & 3) as usize;
    let num = |inp: &mut Input| if wide { inp.int(8) } else { inp.int(3) };
    let mut platforms = Vec::new();
    let (mut fo, mut fc, mut fd) = (Vec::new(), Vec::new(), Vec::new());
    let (mut no, mut nc, mut nd) = (Vec::new(), Vec::new(), Vec::new());
    for _ in 0..n {
        platforms.push(inp.int(1));
        if nested {
            let segs = inp.u8() as usize % 4;
            let (mut o, mut c, mut d) = (Vec::new(), Vec::new(), Vec::new());
            for _ in 0..segs {
                o.push(num(&mut inp));
                c.push(num(&mut inp));
                d.push(num(&mut inp));
            }
            no.push(o);
            nc.push(c);
            nd.push(d);
        } else {
            fo.push(num(&mut inp));
            fc.push(num(&mut inp));
            fd.push(num(&mut inp));
        }
    }
    let stage_counts = (sel & 0x10 != 0).then(|| (0..n).map(|_| inp.int(1)).collect());
    let params: BTreeSet<i64> = (0..inp.u8() % 4).map(|_| inp.int(1)).collect();
    let programs: Option<BTreeSet<i64>> =
        (sel & 0x20 != 0).then(|| (0..inp.u8() % 4).map(|_| inp.int(1)).collect());
    let size = inp.u16() as usize;
    let blob = inp.rest();
    let _ = Entry::parse(blob);
    let _ = shaderx::lz4::decompress(blob, size);
    let (offsets, compressed_lengths, decompressed_lengths) = if nested {
        (
            Lengths::Nested(no),
            Lengths::Nested(nc),
            Lengths::Nested(nd),
        )
    } else {
        (Lengths::Flat(fo), Lengths::Flat(fc), Lengths::Flat(fd))
    };
    let f = BlobFields {
        platforms: platforms.clone(),
        offsets,
        compressed_lengths,
        decompressed_lengths,
        compressed_blob: blob.to_vec(),
        stage_counts,
    };
    let Ok(mut b) = ShaderBlobs::read(&f) else {
        return;
    };
    for &p in &platforms {
        let segs = b.segments(p);
        if let Ok(entries) = b.entries(p, &params, programs.as_ref()) {
            if let Ok(segs) = segs {
                if segs.len() == entries.len() {
                    b.set_entries(p, &entries, &segs, None);
                }
            }
        }
    }
    let _ = b.store();
});
