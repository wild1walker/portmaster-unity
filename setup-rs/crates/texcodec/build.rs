//! Compiles the C and C++ the Python's texture path runs, with the flags its wheels were built with
//! where they decide the output (floating-point contraction, optimisation level):
//!   * astc-encoder (vendor/astc-encoder, as astc_encoder_py 0.1.12 builds it: C++14, -O3,
//!     -ffp-contract=fast, NEON on aarch64) and the C interface to it (csrc/astcenc_capi.cpp, with
//!     the few C++ runtime functions it needs: no libstdc++ is linked);
//!   * sinf / cosf / sincosf from Arm's optimized-routines (vendor/optimized-routines): glibc's,
//!     which the Python's astc-encoder calls, rather than the C library's;
//!   * texture2ddecoder's crunch unpackers (vendor/texture2ddecoder) and csrc/crunch_capi.cpp;
//!   * Pillow's Resample.c (vendor/pillow, -O3 as Pillow's wheel) with csrc/pillow_shim.c;
//!   * guided_astc (vendor/guided_astc, -O2 as its wheel).
use std::path::PathBuf;

fn main() {
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let vendor = root.join("../../vendor");
    let csrc = root.join("csrc");
    let arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap();

    // astc-encoder: always g++ (also for the musl target: the code needs only libc, pthreads and
    // the runtime functions in astcenc_capi.cpp)
    let src = vendor.join("astc-encoder/Source");
    let mut b = cc::Build::new();
    b.cpp(true)
        .compiler(std::env::var("TC_CXX").unwrap_or_else(|_| "g++".into()))
        .cpp_link_stdlib(None)
        .opt_level(3)
        .debug(false)
        .warnings(false)
        .include(&src)
        .define("NDEBUG", None)
        .flag("-std=c++14")
        .flag("-fwrapv")
        .flag("-ffp-contract=fast")
        .flag("-fno-exceptions")
        .flag("-fno-rtti")
        .flag("-fno-threadsafe-statics")
        .flag("-pthread");
    let neon = if arch == "aarch64" { "1" } else { "0" };
    for (k, v) in [
        ("ASTCENC_NEON", neon),
        ("ASTCENC_SVE", "0"),
        ("ASTCENC_SSE", "0"),
        ("ASTCENC_AVX", "0"),
        ("ASTCENC_POPCNT", "0"),
        ("ASTCENC_F16C", "0"),
    ] {
        b.define(k, v);
    }
    for f in ASTCENC_SOURCES {
        b.file(src.join(f));
    }
    b.file(csrc.join("astcenc_capi.cpp"));
    b.compile("tc_astcenc");

    // sinf, cosf, sincosf as glibc computes them (astc-encoder's angular tables; musl's differ in
    // the last bit for about 1 % of inputs): Arm's optimized-routines, which glibc's are, linked
    // ahead of the C library's. Same results as glibc for every float input (GCC's default
    // contraction, as glibc builds them).
    let aor = vendor.join("optimized-routines/math");
    let mut m = cc::Build::new();
    m.opt_level(2).debug(false).warnings(false).include(&aor);
    for f in ["sinf.c", "cosf.c", "sincosf.c", "sincosf_data.c", "math_errf.c"] {
        m.file(aor.join(f));
    }
    m.compile("tc_libm");

    // texture2ddecoder's crunch unpackers (C++17 as its wheel; crn_decomp.h asks for
    // -fno-strict-aliasing)
    let t2d = vendor.join("texture2ddecoder/src/Texture2DDecoder");
    cc::Build::new()
        .cpp(true)
        .compiler(std::env::var("TC_CXX").unwrap_or_else(|_| "g++".into()))
        .cpp_link_stdlib(None)
        .opt_level(3)
        .debug(false)
        .warnings(false)
        .include(&t2d)
        .define("NDEBUG", None)
        .flag("-std=c++17")
        .flag("-fms-extensions")
        .flag("-fwrapv")
        .flag("-fno-strict-aliasing")
        .flag("-fno-exceptions")
        .flag("-fno-rtti")
        .flag("-fno-threadsafe-statics")
        .file(t2d.join("crunch.cpp"))
        .file(t2d.join("unitycrunch.cpp"))
        .file(csrc.join("crunch_capi.cpp"))
        .compile("tc_crunch");

    // Pillow's resampling
    cc::Build::new()
        .opt_level(3)
        .debug(false)
        .warnings(false)
        .include(csrc.join("pillow"))
        .define("NDEBUG", None)
        .flag("-fwrapv")
        .file(vendor.join("pillow/libImaging/Resample.c"))
        .file(csrc.join("pillow_shim.c"))
        .compile("tc_pillow");

    // guided_astc
    cc::Build::new()
        .opt_level(2)
        .debug(false)
        .warnings(false)
        .define("NDEBUG", None)
        .flag("-fwrapv")
        .flag("-pthread")
        .file(vendor.join("guided_astc/guided_astc.c"))
        .compile("tc_guided");

    println!("cargo:rerun-if-changed=csrc");
    println!("cargo:rerun-if-changed=../../vendor");
    println!("cargo:rerun-if-env-changed=TC_CXX");
}

/// astc_encoder_py's ASTC_ENCODER_SOURCES (its setup.py).
const ASTCENC_SOURCES: &[&str] = &[
    "astcenc_averages_and_directions.cpp",
    "astcenc_block_sizes.cpp",
    "astcenc_color_quantize.cpp",
    "astcenc_color_unquantize.cpp",
    "astcenc_compress_symbolic.cpp",
    "astcenc_compute_variance.cpp",
    "astcenc_decompress_symbolic.cpp",
    "astcenc_diagnostic_trace.cpp",
    "astcenc_entry.cpp",
    "astcenc_find_best_partitioning.cpp",
    "astcenc_ideal_endpoints_and_weights.cpp",
    "astcenc_image.cpp",
    "astcenc_integer_sequence.cpp",
    "astcenc_mathlib.cpp",
    "astcenc_mathlib_softfloat.cpp",
    "astcenc_partition_tables.cpp",
    "astcenc_percentile_tables.cpp",
    "astcenc_pick_best_endpoint_format.cpp",
    "astcenc_quantization.cpp",
    "astcenc_symbolic_physical.cpp",
    "astcenc_weight_align.cpp",
    "astcenc_weight_quant_xfer_tables.cpp",
];
