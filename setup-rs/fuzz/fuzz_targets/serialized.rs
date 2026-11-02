//! A serialized file (.assets, a level, globalgamemanagers): the header, the metadata (types with
//! their type trees, objects, scripts, externals, reference types), every object read with its
//! type tree (the file's, else the class database's when $PMU_TPK names it) and written back, the
//! texture streams' plan, the file saved, and the platform patched in place.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::{class_db, Scratch};
use std::sync::Arc;
use unityfs::serialized::{head_len, looks_serialized, platform_offset};
use unityfs::{MemSource, SerializedFile};

fuzz_target!(|data: &[u8]| {
    let _ = head_len(data);
    let _ = looks_serialized(&data[..data.len().min(48)], data.len() as u64);
    let _ = platform_offset(data);
    let src = Arc::new(MemSource(data.to_vec()));
    if let Ok(mut sf) =
        SerializedFile::open("sharedassets0.assets", src, class_db(), Some("2017.4.9f1"))
    {
        let ids: Vec<i64> = sf.objects.iter().take(256).map(|o| o.path_id).collect();
        for pid in ids {
            let _ = sf.typetree_of(pid);
            if let Ok(v) = sf.read_typetree(pid) {
                let _ = sf.save_typetree(pid, &v);
            }
        }
        let _ = unityfs::inline::plan(&sf);
        let _ = sf.metadata_in_place();
        let _ = sf.save();
    }
    let s = Scratch::new();
    let p = s.file("level0", data);
    let _ = unityfs::platform::patch_sf_platform(&p, 11);
});
