//! A UnityFS bundle: the header, the blocks info (stored, LZ4, LZMA) with its blocks and nodes,
//! the nodes read through the blocks, a bundle of big blocks unpacked into files, and the bundle
//! opened as the steps open one (its serialized file, its objects read, the bundle written back,
//! with and without its textures' streams inlined; every serialized node opened and saved).
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
        ..Default::default()
    };
    if let Ok(mut f) = DataFile::open(&p, &opts) {
        let ids: Vec<i64> = f.sf.objects.iter().take(64).map(|o| o.path_id).collect();
        for pid in ids {
            if let Ok(v) = f.read(pid) {
                let _ = f.sf.save_typetree(pid, &v);
            }
        }
        let _ = f.write_to(&s.path("out"), false);
        let _ = f.write_to(&s.path("inlined"), true);
    }
    if let Ok(bf) = BundleFiles::open(&p, class_db()) {
        let _ = bf.write_to(&s.path("saved"), Some(&s.path("work")));
    }
});
