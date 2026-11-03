//! `inline_bundle <src> <dst>`: android-data's inlining of one bundle's streamed textures
//! (inline::inline_bundle_with, android_data.py's inline_bundle), printed as the Python's
//! function returns it: `(textures inlined, [dropped nodes])`. For tests/golden/inline_scenes.sh.
//! Needs $PMU_TPK for files without type trees.
fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() != 2 {
        eprintln!("usage: inline_bundle <src> <dst>");
        std::process::exit(2);
    }
    let db = unityfs::ClassDb::default_db().ok().map(std::sync::Arc::new);
    match androiddata::inline::inline_bundle_with(db, &a[0], &a[1]) {
        Ok((n, dropped)) => {
            let d: Vec<String> = dropped.iter().map(|x| format!("'{x}'")).collect();
            println!("({n}, [{}])", d.join(", "));
        }
        Err(e) => {
            eprintln!("{e:?}");
            std::process::exit(e.status() as i32);
        }
    }
}
