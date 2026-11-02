//! grab_fetch's check and rewrite of a GLES3 program (its symbolic evaluation of the vertex
//! stage's screen position and the fragment stage's _GrabTexture reads).
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let Ok(code) = std::str::from_utf8(data) else {
        return;
    };
    let _ = shaderx::grabfetch::reads_grab(code);
    let _ = shaderx::grabfetch::rewrite_program(code);
});
