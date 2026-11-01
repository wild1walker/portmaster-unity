//! add_gles3 on small made-up shaders: the blob, twins, platforms and report of each case.
use std::collections::{BTreeMap, BTreeSet};

use shaderx::blob::{self, BlobFields, Entry, Lengths, PlatformBlob, ShaderBlobs, Slot};
use shaderx::convert::{
    self, Options, Pass, Shader, StageProgram, Status, SubProgram, Translations,
};
use shaderx::glsl::Optimize;

const VS: &str = "#ifdef VERTEX\n#version 150\n#extension GL_ARB_explicit_attrib_location : require\nin vec4 in_POSITION0;\nvoid main()\n{\n    gl_Position = in_POSITION0;\n    return;\n}\n#endif\n";

fn fs(body: &str) -> String {
    format!("#ifdef FRAGMENT\n#version 150\nuniform sampler2D _MainTex;\nlayout(location = 0) out vec4 SV_Target0;\nvoid main()\n{{\n{body}}}\n#endif\n")
}

fn entry(code: &str, keywords: &[&str], local: Option<&[&str]>) -> Option<Slot> {
    Some(Slot::Program(Entry {
        version: 201708220,
        gpu_type: blob::GPU_GLCORE32 as i32,
        stats: vec![0, 0, 0, 0],
        keywords: keywords.iter().map(|k| k.to_string()).collect(),
        local_keywords: local.map(|l| l.iter().map(|k| k.to_string()).collect()),
        code: code.as_bytes().to_vec(),
        rest: vec![0; 8],
        guessed: false,
    }))
}

fn sp(blob_index: i64, gpu: i64, keywords: &[i64]) -> SubProgram {
    SubProgram {
        blob_index,
        gpu_program_type: gpu,
        keyword_indices: Some(keywords.to_vec()),
        hardware_tier: Some(0),
        ..Default::default()
    }
}

/// A 2017-style shader: one GLCore blob, m_SubPrograms per stage program.
fn shader(
    entries: Vec<Option<Slot>>,
    vertex: Vec<SubProgram>,
    fragment: Vec<SubProgram>,
    platforms: Option<Vec<i64>>,
) -> Shader {
    let sb = ShaderBlobs {
        nested: false,
        platforms: BTreeMap::from([(
            blob::PLATFORM_GLCORE,
            PlatformBlob::One(blob::build_blob(&entries)),
        )]),
        stage_counts: None,
    };
    let prog = |l: Vec<SubProgram>| {
        Some(StageProgram {
            sub_programs: Some(l),
            ..Default::default()
        })
    };
    let mut pass = Pass {
        platforms,
        ..Default::default()
    };
    pass.programs[0] = prog(vertex);
    pass.programs[1] = prog(fragment);
    for k in 2..5 {
        pass.programs[k] = prog(vec![]);
    }
    Shader {
        name: "Test/Shader".into(),
        blob: sb.store().unwrap(),
        sub_shaders: vec![vec![pass]],
    }
}

fn opts() -> Options {
    Options {
        precision: "highp".into(),
        replace: false,
        optimize: Optimize::ALL,
        glcore4x: false,
    }
}

fn gles3_entries(s: &Shader) -> Vec<Option<Slot>> {
    ShaderBlobs::read(&s.blob)
        .unwrap()
        .entries(blob::PLATFORM_GLES3PLUS, &BTreeSet::new(), None)
        .unwrap()
}

#[test]
fn twins_and_blob() {
    let code = format!(
        "{VS}{}",
        fs("    SV_Target0 = texture(_MainTex, vec2(0.5));\n    return;\n")
    );
    let s = shader(
        vec![entry(&code, &[], None), entry("", &[], None)],
        vec![sp(0, 6, &[])],
        vec![sp(1, 6, &[])],
        Some(vec![15]),
    );
    let mut tr = Translations::new();
    let oc = convert::add_gles3(&s, &opts(), &mut tr).unwrap();
    let r = &oc.report;
    assert_eq!(
        (r.status, r.programs, r.twins, r.why.as_str()),
        (Status::Converted, 1, 2, "")
    );
    assert_eq!(r.platforms, vec![9, 15]);
    let edit = oc.edit.unwrap();
    assert_eq!(edit.blob.platforms, vec![9, 15]);
    assert_eq!(edit.lists.len(), 2);
    assert_eq!(
        edit.lists[0].items,
        vec![
            convert::Item {
                from: 0,
                gpu_program_type: 6
            },
            convert::Item {
                from: 0,
                gpu_program_type: 4
            }
        ]
    );
    assert_eq!(edit.pass_platforms[0].platforms, vec![9, 15]);
    let out = convert::apply(&s, &edit);
    let es = gles3_entries(&out);
    let Some(Slot::Program(e)) = &es[0] else {
        panic!()
    };
    assert_eq!(e.gpu_type, 4);
    assert!(String::from_utf8_lossy(&e.code).contains("#version 300 es\n"));
    let Some(Slot::Program(e)) = &es[1] else {
        panic!()
    };
    assert_eq!((e.gpu_type, e.code.len()), (4, 0));
    // the GLCore blob is the same after the round trip (compressed again)
    let glc = ShaderBlobs::read(&out.blob).unwrap();
    assert_eq!(
        glc.platforms[&15],
        ShaderBlobs::read(&s.blob).unwrap().platforms[&15]
    );
    assert_eq!(convert::gles3_programs(&out).unwrap().len(), 1);
    // a shader that already has GLES3 programs is left alone
    let again = convert::add_gles3(&out, &opts(), &mut tr).unwrap();
    assert_eq!(
        (
            again.report.status,
            again.report.why.as_str(),
            again.edit.is_none()
        ),
        (Status::Unchanged, "already has GLES3 programs", true)
    );
    // with replace, they are rebuilt: the same output
    let re = convert::add_gles3(
        &out,
        &Options {
            replace: true,
            ..opts()
        },
        &mut tr,
    )
    .unwrap();
    assert_eq!(re.report.why, "(old GLES programs removed)");
    assert_eq!(convert::apply(&out, &re.edit.unwrap()), out);
}

#[test]
fn untranslatable_vr_and_es31() {
    let bad = format!("{VS}{}", fs("    gl_FragColor = vec4(1.0);\n"));
    let good = format!("{VS}{}", fs("    SV_Target0 = vec4(1.0);\n"));
    let gather = format!(
        "{VS}{}",
        fs("    SV_Target0 = textureGather(_MainTex, vec2(0.5));\n")
    );
    let mut tr = Translations::new();
    // a GLCore32 program with no ES equivalent: the whole shader skipped
    let s = shader(
        vec![entry(&bad, &[], None), entry("", &[], None)],
        vec![sp(0, 6, &[])],
        vec![sp(1, 6, &[])],
        None,
    );
    let oc = convert::add_gles3(&s, &opts(), &mut tr).unwrap();
    assert_eq!(oc.report.status, Status::Skipped);
    assert_eq!(
        oc.report.why,
        "program 0: FRAGMENT: fixed-function built-in 'gl_FragColor'"
    );
    assert!(oc.edit.is_none());
    assert_eq!(
        oc.report.to_string(),
        "Test/Shader: skipped program 0: FRAGMENT: fixed-function built-in 'gl_FragColor'"
    );
    // the same program in a VR-only variant: left out, the rest translated
    let s = shader(
        vec![
            entry(&good, &[], None),
            entry("", &[], None),
            entry(&bad, &["STEREO_INSTANCING_ON"], None),
            entry("", &[], None),
        ],
        vec![sp(0, 6, &[]), sp(2, 6, &[3])],
        vec![sp(1, 6, &[]), sp(3, 6, &[3])],
        None,
    );
    let oc = convert::add_gles3(&s, &opts(), &mut tr).unwrap();
    let r = &oc.report;
    assert_eq!(
        (r.status, r.dropped, r.programs, r.twins),
        (Status::Converted, 1, 1, 2)
    );
    let es = gles3_entries(&convert::apply(&s, oc.edit.as_ref().unwrap()));
    let Some(Slot::Program(e)) = &es[2] else {
        panic!()
    };
    assert_eq!(e.gpu_type, 6); // kept as it was, nothing names it
                               // texture gather: a GLES 3.1 program, its variant's subprograms typed GLES 3.1
    let s = shader(
        vec![entry(&gather, &[], None), entry("", &[], None)],
        vec![sp(0, 6, &[])],
        vec![sp(1, 6, &[])],
        None,
    );
    let oc = convert::add_gles3(&s, &opts(), &mut tr).unwrap();
    assert_eq!((oc.report.status, oc.report.es31), (Status::Converted, 1));
    let edit = oc.edit.unwrap();
    assert!(edit.lists.iter().all(|l| l.items[1].gpu_program_type == 3));
    let es = gles3_entries(&convert::apply(&s, &edit));
    for e in es.iter().flatten() {
        let Slot::Program(e) = e else { panic!() };
        assert_eq!(e.gpu_type, 3);
    }
    let lines = shaderx::step::summary(&[oc.report.clone()], 1, "highp");
    assert_eq!(lines[0], "shaders: 1 shaders given GLES3 programs (1 programs, 2 subprograms) in 1 files; 0 not translatable; precision highp; rewrites: 0 stages (none)");
    assert!(lines[1].starts_with("shaders: 1 programs need GLSL ES 3.10"));
}

#[test]
fn no_programs() {
    let mut tr = Translations::new();
    let s = shader(vec![], vec![], vec![], None);
    let oc = convert::add_gles3(&s, &opts(), &mut tr).unwrap();
    assert_eq!(
        (oc.report.status, oc.report.why.as_str()),
        (Status::Unchanged, "no GLCore32 programs")
    );
    // glcore4x: an empty GLES3 blob
    let oc = convert::add_gles3(
        &s,
        &Options {
            glcore4x: true,
            ..opts()
        },
        &mut tr,
    )
    .unwrap();
    assert_eq!(
        (oc.report.status, oc.report.empty),
        (Status::Converted, true)
    );
    assert_eq!(oc.edit.unwrap().blob.platforms, vec![9, 15]);
    // no GLCore platform at all
    let mut m = s.clone();
    m.blob.platforms = vec![];
    m.blob.offsets = Lengths::Flat(vec![]);
    let oc = convert::add_gles3(&m, &opts(), &mut tr).unwrap();
    assert_eq!(oc.report.why, "no OpenGL Core programs");
    assert!(shaderx::step::nothing_to_translate(&[oc.report], &["glcore"]).is_none());
}

#[test]
fn segmented_blob_with_parameter_entries() {
    // 2021.2+: segments, a parameter entry per program, m_PlayerSubPrograms with their indices
    let code = format!("{VS}{}", fs("    SV_Target0 = vec4(0.0);\n"));
    let entries = vec![
        entry(&code, &[], None),
        Some(Slot::Params {
            version: 202012090,
            data: vec![1, 2, 3, 4],
        }),
    ];
    let segs = blob::build_segments(&entries, &[0, 1]);
    let sb = ShaderBlobs {
        nested: true,
        platforms: BTreeMap::from([(blob::PLATFORM_GLCORE, PlatformBlob::Segments(segs))]),
        stage_counts: Some(BTreeMap::from([(blob::PLATFORM_GLCORE, 1)])),
    };
    let mut pass = Pass {
        platforms: Some(vec![15]),
        ..Default::default()
    };
    pass.programs[0] = Some(StageProgram {
        sub_programs: Some(vec![]),
        player_sub_programs: Some(vec![vec![], vec![sp(0, 6, &[])]]),
        parameter_blob_indices: Some(vec![vec![], vec![1]]),
    });
    let s = Shader {
        name: "Seg".into(),
        blob: sb.store().unwrap(),
        sub_shaders: vec![vec![pass]],
    };
    assert!(matches!(s.blob.offsets, Lengths::Nested(_)));
    let oc = convert::add_gles3(&s, &opts(), &mut Translations::new()).unwrap();
    let edit = oc.edit.unwrap();
    assert_eq!(edit.blob.stage_counts, Some(vec![1, 1]));
    assert_eq!(edit.lists.len(), 1);
    assert_eq!(edit.lists[0].parameter_blob_indices, Some(vec![1, 1]));
    let out = convert::apply(&s, &edit);
    let read = ShaderBlobs::read(&out.blob).unwrap();
    let PlatformBlob::Segments(g) = &read.platforms[&9] else {
        panic!()
    };
    assert_eq!(g.len(), 2);
    let es = read.entries(9, &BTreeSet::from([1]), None).unwrap();
    assert_eq!(es[1], entries[1]);
    let BlobFields {
        decompressed_lengths: Lengths::Nested(d),
        ..
    } = &out.blob
    else {
        panic!()
    };
    assert_eq!(d.len(), 2);
}
