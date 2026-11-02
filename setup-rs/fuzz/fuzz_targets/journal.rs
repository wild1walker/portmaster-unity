//! The convert journal (`.pmunity.json`) and the JSON pmu reads: convert's and android-data's
//! json.loads, dumps of what they read, `journal-files`, and a conversion job started on the
//! journal (resumed, a damaged one refused, a file marked done). A journal with `pending` renames
//! is parsed but no job is started on it (its renames name any path).
#![no_main]
use convert::pyjson::{loads, Json};
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::Scratch;

fuzz_target!(|data: &[u8]| {
    let parsed = std::str::from_utf8(data).ok().and_then(|t| loads(t).ok());
    if let Some(j) = &parsed {
        let _ = j.dumps();
        let _ = j.repr();
    }
    if let Ok(j) = androiddata::pyjson::loads_bytes(data) {
        let _ = androiddata::pyjson::dumps(&j, Some(1));
    }
    let s = Scratch::new();
    let out = s.path("out");
    std::fs::create_dir_all(&out).unwrap();
    let journal = s.file("out/.pmunity.json", data);
    let _ = convert::job::journal_files(&journal);
    if parsed.as_ref().is_some_and(|j| j.get("pending").is_some()) {
        return;
    }
    let cfg = Json::Dict(vec![("version".into(), Json::Int(1))]);
    if let Ok(mut job) = convert::Job::new(&s.path("src"), &out, cfg) {
        let _ = job.is_done("textures", "sharedassets0.assets");
        let _ = job.mark_done("textures", "sharedassets0.assets");
    }
});
