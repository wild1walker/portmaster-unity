//! What shaders/step.py says about the shaders it saw, for the OpenGL Core source: its summary
//! lines and the error for data without the programs it translates.
use crate::blob;
use crate::convert::{Report, Status};

const NO_SOURCE: [&str; 4] = [
    "no OpenGL Core programs",
    "no Direct3D 11 programs",
    "no Vulkan programs",
    "no Metal programs",
];

/// Shader.platforms values (Unity's ShaderCompilerPlatform) as names, for messages.
fn platform_name(p: i64) -> String {
    match p {
        0 => "OpenGL (legacy)",
        1 => "Direct3D 9",
        4 => "Direct3D 11",
        5 => "OpenGL ES 2",
        8 => "Direct3D 11 9.x",
        9 => "OpenGL ES 3",
        14 => "Metal",
        15 => "OpenGL Core",
        18 => "Vulkan",
        20 => "Direct3D 12 (Xbox)",
        25 => "WebGPU",
        _ => return format!("platform {p}"),
    }
    .to_string()
}

fn source_name(s: &str) -> &str {
    match s {
        "glcore" => "OpenGL Core",
        "d3d11" => "Direct3D 11",
        "vulkan" => "Vulkan",
        "metal" => "Metal",
        _ => s,
    }
}

/// step.nothing_to_translate: the error for data whose shaders all lack the source platforms'
/// programs (a Vulkan- or Metal-only build), else None.
pub fn nothing_to_translate(results: &[Report], sources: &[&str]) -> Option<String> {
    if results.is_empty() || results.iter().any(|r| !NO_SOURCE.contains(&r.why.as_str())) {
        return None;
    }
    let plats: std::collections::BTreeSet<i64> = results
        .iter()
        .flat_map(|r| r.platforms.iter().copied())
        .collect();
    if plats.is_empty() {
        return None;
    }
    let names: Vec<String> = plats.iter().map(|&p| platform_name(p)).collect();
    let hint = if plats.contains(&blob::PLATFORM_VULKAN) && !sources.contains(&"vulkan") {
        ". They have Vulkan programs: translate those with `\"shaders\": {\"from\": [\"vulkan\"]}` (or `--from vulkan`; tools/spirvcross/build.sh). HDRP does not run on OpenGL ES"
    } else if plats.contains(&blob::PLATFORM_METAL) && !sources.contains(&"metal") {
        ". They have Metal programs: translate those with `\"shaders\": {\"from\": [\"metal\"]}` (or `--from metal`)"
    } else {
        ""
    };
    let srcs: Vec<&str> = sources.iter().map(|s| source_name(s)).collect();
    Some(format!(
        "shaders: none of the {} shaders has {} programs (their programs: {}), so the Android player would draw nothing{hint}. A build made without OpenGL Core (Vulkan or Metal only: HDRP's Linux and macOS builds, recent macOS builds) cannot be converted from it; another build of the game may have OpenGL Core or Direct3D 11 programs",
        results.len(),
        srcs.join(" or "),
        names.join(", ")
    ))
}

/// The step's summary lines for shaders translated from OpenGL Core (results: every shader the
/// step saw; changed_files: the files it rewrote).
pub fn summary(results: &[Report], changed_files: usize, precision: &str) -> Vec<String> {
    let conv: Vec<&Report> = results
        .iter()
        .filter(|r| r.status == Status::Converted)
        .collect();
    let kinds: std::collections::BTreeSet<&str> = conv
        .iter()
        .flat_map(|r| r.notes.iter())
        .map(|n| {
            let after = n.split_once(' ').map_or("", |x| x.1);
            after.rsplit_once(' ').map_or(after, |x| x.0)
        })
        .collect();
    let kinds = if kinds.is_empty() {
        "none".to_string()
    } else {
        kinds.into_iter().collect::<Vec<_>>().join(", ")
    };
    let mut out = vec![format!(
        "shaders: {} shaders given GLES3 programs ({} programs, {} subprograms) in {changed_files} files; {} not translatable; precision {precision}; rewrites: {} stages ({kinds})",
        conv.len(),
        conv.iter().map(|r| r.programs).sum::<usize>(),
        conv.iter().map(|r| r.twins).sum::<usize>(),
        results.iter().filter(|r| r.status == Status::Skipped).count(),
        conv.iter().map(|r| r.notes.len()).sum::<usize>(),
    )];
    let es31: usize = conv.iter().map(|r| r.es31).sum();
    if es31 > 0 {
        out.push(format!(
            "shaders: {es31} programs need GLSL ES 3.10 (multisampled textures, texture gather, bit-field functions, storage buffers, binding points, images): GLES 3.1 programs, used where the device has OpenGL ES 3.1+"
        ));
    }
    let gl4: Vec<&Report> = results.iter().filter(|r| r.glcore4x).collect();
    if !gl4.is_empty() {
        out.push(format!(
            "shaders: GLCore41/43 programs translated too: {} programs; {} SubShaders left out (no ES equivalent); {} shaders without programs (stripped) given an empty GLES3 blob",
            gl4.iter().filter(|r| r.status == Status::Converted).map(|r| r.programs4x).sum::<usize>(),
            gl4.iter().map(|r| r.failed.len()).sum::<usize>(),
            gl4.iter().filter(|r| r.empty).count(),
        ));
    } else if !results.is_empty() {
        out.push("shaders: GLCore41/43 programs not translated (glcore4x off)".into());
    }
    let vr: usize = conv.iter().map(|r| r.dropped).sum();
    if vr > 0 {
        out.push(format!("shaders: {vr} VR-only OpenGL Core variants not translatable (left out, never used on a handheld)"));
    }
    out
}
