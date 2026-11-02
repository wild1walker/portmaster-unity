//! A shader program's GLSL (OpenGL Core, as Unity compiles it) through the shaders step's
//! rewriting to GLSL ES: the stage blocks, comments, the header, precision, the const-array and
//! loop-unrolling rewrites. The first byte picks the precision, the ES version and the rewrites.
#![no_main]
use libfuzzer_sys::fuzz_target;
use shaderx::glsl::{
    const_arrays, precision_block, program_to_es, stage_blocks, strip_comments, unroll, Es,
    Optimize, UNROLL_MAX, UNROLL_STATEMENTS,
};

const PRECISIONS: [&str; 4] = ["highp", "mediump", "default-samplers", "lowp"];

fuzz_target!(|data: &[u8]| {
    let Some((&sel, rest)) = data.split_first() else {
        return;
    };
    let Ok(code) = std::str::from_utf8(rest) else {
        return;
    };
    let es = if sel & 0x10 != 0 {
        Es::Es310
    } else {
        Es::Es300
    };
    let opt = Optimize {
        const_arrays: sel & 0x20 != 0,
        unroll: sel & 0x40 != 0,
    };
    let mut notes = Vec::new();
    let _ = program_to_es(code, PRECISIONS[sel as usize % 4], opt, &mut notes, es);
    let _ = stage_blocks(code);
    let _ = strip_comments(code);
    let _ = const_arrays(code);
    let _ = unroll(code, UNROLL_MAX, UNROLL_STATEMENTS);
    let _ = precision_block(code, "highp", sel & 0x80 != 0);
});
