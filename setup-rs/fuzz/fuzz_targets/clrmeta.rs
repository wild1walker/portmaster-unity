//! A managed assembly (a game's DLL) as android-data reads its .NET metadata: the PE headers,
//! the metadata streams and tables, type and method names, P/Invokes, signatures, constants and
//! type layouts.
#![no_main]
use androiddata::clrmeta::Assembly;
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let Ok(a) = Assembly::from_bytes("Assembly-CSharp.dll", data.to_vec()) else {
        return;
    };
    let _ = a.pinvokes();
    let _ = a.field_constants();
    if let Ok(types) = a.types() {
        for row in 1..=types.len().min(64) as u32 {
            let _ = a.typedef_layout(row);
        }
    }
});
