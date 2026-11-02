//! A type tree (a serialized file's blob, or the old nested format) and an object's bytes read
//! with it, then written back: nested structs, arrays, pairs, strings, managed references (the
//! tree is its own reference type "A", so a reference can name the tree it is in). The first
//! byte picks the file format and byte order.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::{class_db, Input};
use std::sync::Arc;
use unityfs::bin::{Reader, Writer};
use unityfs::typetree::CommonStrings;
use unityfs::{value, TypeTree};

const FORMATS: [u32; 10] = [9, 10, 11, 12, 15, 17, 19, 21, 22, 23];

fuzz_target!(|data: &[u8]| {
    let mut inp = Input::new(data);
    let sel = inp.u8();
    let version = FORMATS[(sel & 0x7f) as usize % FORMATS.len()];
    let big = sel & 0x80 != 0;
    let rest = inp.rest();
    let common: CommonStrings = class_db()
        .map(|d| d.common_strings().clone())
        .unwrap_or_default();
    let mut r = Reader::new(rest, big);
    let tree = if version >= 12 || version == 10 {
        TypeTree::parse_blob(&mut r, version, &common)
    } else {
        TypeTree::parse_old(&mut r, version)
    };
    let Ok(tree) = tree else {
        return;
    };
    let obj = &rest[r.pos.min(rest.len())..];
    let _ = tree.shape();
    let mut w = Writer::new(big);
    let _ = tree.dump_blob(&mut w, version, &common);
    let _ = tree.with_fields(&[("m_Name", "int", "m_Added", true)]);
    let refs = vec![(
        b"A".to_vec(),
        Vec::new(),
        Vec::new(),
        Some(Arc::new(tree.clone())),
    )];
    if let Ok((v, _)) = value::read(&tree, obj, big, Some(&refs)) {
        let _ = value::write(&tree, &v, big, Some(&refs));
    }
});
