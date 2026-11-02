//! A UnityFS bundle: the header, the blocks info (stored, LZ4, LZMA) with its blocks and nodes,
//! the nodes read through the blocks, a bundle of big blocks unpacked into files, and the bundle
//! opened as the steps open one (its serialized file, its objects read, the bundle written back,
//! with and without its textures' streams inlined; with several serialized files, each selected,
//! its PPtrs' files resolved; every serialized node opened and saved).
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::{class_db, Scratch};
use std::sync::Arc;
use unityfs::bundle::Bundle;
use unityfs::data::BundleFiles;
use unityfs::{DataFile, MemSource, Options, SharedSource};

fuzz_target!(|data: &[u8]| {
    let src: SharedSource = Arc::new(MemSource(data.to_vec()));
    let Ok(b) = Bundle::from_source(src) else {
        return;
    };
    let _ = b.lazy_ok();
    for i in 0..b.header.nodes.len().min(64) {
        let n = b.node(i);
        let _ = n.read_clipped(0, 1 << 16);
        let _ = n.read_clipped(n.len().saturating_sub(1), 1);
    }
    let s = Scratch::new();
    let _ = b.unpack(&s.path("nodes"));
    let p = s.file("bundle", data);
    let opts = Options {
        db: class_db(),
        spill_dir: Some(s.path("spill")),
        spill_mb: 1,
        all_serialized: data.first().is_some_and(|b| b & 1 == 1),
        ..Default::default()
    };
    if let Ok(mut f) = DataFile::open(&p, &opts) {
        for k in (0..f.file_count().min(8)).rev() {
            if f.select(k).is_err() {
                break;
            }
            for fid in 0..4 {
                let _ = f.file_of(fid);
            }
            let ids: Vec<i64> = f.sf.objects.iter().take(64).map(|o| o.path_id).collect();
            for pid in ids {
                if let Ok(v) = f.read(pid) {
                    let _ = f.sf.save_typetree(pid, &v);
                }
            }
        }
        let _ = f.write_to(&s.path("out"), false);
        let _ = f.write_to(&s.path("inlined"), true);
    }
    if let Ok(bf) = BundleFiles::open(&p, class_db()) {
        let _ = bf.write_to(&s.path("saved"), Some(&s.path("work")));
        let names: Vec<String> = bf.bundle.header.nodes.iter().map(|n| n.name.clone()).collect();
        let drop: std::collections::HashSet<String> =
            names.iter().filter(|n| n.ends_with(".resS")).cloned().collect();
        let _ = bf.write_with(&s.path("dropped"), None, &drop, |_, sf, o| {
            Ok(unityfs::inline::inlined(sf, o.path_id, |_, _, n| Ok(vec![0; n.min(1 << 16)])).ok())
        });
    }
});
