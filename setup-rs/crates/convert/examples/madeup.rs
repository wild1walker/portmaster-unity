//! `madeup <dir>`: writes the convert tests' made-up data folder (tests/madeup) to <dir>/Data
//! and Cuphead's config to <dir>/cuphead.json, for tests/golden/convert_madeup.sh (the Python
//! and pmu run on copies of it). Needs $PMU_TPK (UnityPy's lzma.tpk).
#[path = "../tests/madeup/mod.rs"]
mod madeup;

fn main() {
    let dir = std::path::PathBuf::from(std::env::args().nth(1).expect("usage: madeup <dir>"));
    let db = madeup::db().expect("set PMU_TPK to UnityPy's lzma.tpk");
    std::fs::create_dir_all(&dir).unwrap();
    madeup::game(&db, &dir.join("Data"));
    std::fs::write(dir.join("cuphead.json"), madeup::CUPHEAD_JSON).unwrap();
}
