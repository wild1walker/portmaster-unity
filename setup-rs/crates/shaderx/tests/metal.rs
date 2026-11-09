//! shaderx::metal against the Python's metal.translate_program on hand-written MSL
//! (tests/metal_golden.json, from setup-rs/tests/golden/metal_golden.py): the same GL program text
//! and type, or the same Unsupported message, and the same notes.
use shaderx::metal::{translate_program, Fault};

#[test]
fn metal_golden() {
    let rows: Vec<serde_json::Value> =
        serde_json::from_str(include_str!("metal_golden.json")).unwrap();
    assert!(rows.len() >= 10);
    for row in &rows {
        let name = row["name"].as_str().unwrap();
        let unity = match row["unity"].as_array() {
            Some(v) => (v[0].as_u64().unwrap(), v[1].as_u64().unwrap()),
            None => (0, 0),
        };
        let mut notes = Vec::new();
        let got = translate_program(
            row["vs"].as_str().unwrap(),
            row["fs"].as_str().unwrap(),
            "highp",
            unity,
            &mut notes,
        );
        match (got, row.get("text")) {
            (Ok((text, typ)), Some(want)) => {
                assert_eq!(text, want.as_str().unwrap(), "{name}");
                assert_eq!(typ, row["type"].as_i64().unwrap(), "{name}");
            }
            (Err(Fault::Uns(m) | Fault::Es31(m)), None) => {
                assert_eq!(m, row["error"].as_str().unwrap(), "{name}")
            }
            (got, _) => panic!("{name}: {got:?}, the Python: {row}"),
        }
        let want: Vec<&str> = row["notes"]
            .as_array()
            .unwrap()
            .iter()
            .map(|n| n.as_str().unwrap())
            .collect();
        assert_eq!(notes, want, "{name}");
    }
}

/// Unity 6's variable rasterization rate code (pmu's own; the Python stops at it): the golden
/// `test_metal` fragment stage with its `#if __METAL_VERSION__` block written as Unity 6 writes
/// it, `if (has_vrr_api) { ... }` with Metal's own types in it, and the VRR buffer among the
/// entry's parameters. The block never runs (has_vrr_api is a function constant, false), so the
/// program is the golden one, byte for byte.
#[test]
fn metal_unity6_vrr() {
    let rows: Vec<serde_json::Value> =
        serde_json::from_str(include_str!("metal_golden.json")).unwrap();
    let row = rows.iter().find(|r| r["name"] == "test_metal").unwrap();
    let fs = row["fs"].as_str().unwrap();
    let old_block = "#if __METAL_VERSION__ >= 230
    {
        rasterization_rate_map_decoder mtl_VrrMap(*(constant rasterization_rate_map_data*)(&mtl_VrrBuffer.vrrData));
    }
#endif
";
    let vrr_block = "    if (has_vrr_api)
    {
        rasterization_rate_map_decoder mtl_VrrMap(*(constant rasterization_rate_map_data*)(&mtl_VrrBuffer.vrrData));
        u_xlat0.xy = mtl_VrrMap.map_screen_to_physical_coordinates(u_xlat0.xy * float2(mtl_VrrBuffer.viewportWidth, mtl_VrrBuffer.viewportHeight), 0) / float2(mtl_VrrBuffer.physicalWidth, mtl_VrrBuffer.physicalHeight);
    } // has_vrr_api
";
    let vrr_struct = "constant bool has_vrr_api [[ function_constant(3) ]];
struct Mtl_VrrData
{
    float physicalWidth;
    float physicalHeight;
    float viewportWidth;
    float viewportHeight;
    constant rasterization_rate_map_decoder* vrrData;
};

fragment Mtl_FragmentOut xlatMtlMain(
    constant Mtl_VrrData& mtl_VrrBuffer [[ buffer(27), function_constant(has_vrr_api) ]],
";
    assert!(fs.contains(old_block));
    let fs6 = fs
        .replace(old_block, vrr_block)
        .replace("fragment Mtl_FragmentOut xlatMtlMain(\n", vrr_struct);
    let mut notes = Vec::new();
    let (text, typ) =
        translate_program(row["vs"].as_str().unwrap(), &fs6, "highp", (0, 0), &mut notes)
            .unwrap_or_else(|f| panic!("{f:?}"));
    assert_eq!(text, row["text"].as_str().unwrap());
    assert_eq!(typ, row["type"].as_i64().unwrap());
    assert!(notes.is_empty(), "{notes:?}");
}

/// A compute kernel as HLSLcc writes MSL for one (pmu's own; tests/data/kernel.metal, written by
/// hand): constant buffer members at Metal's offsets (a float after a float3: a padding float),
/// structured, raw and read-only storage buffers bound as Unity binds a GLES3 variant's
/// (writable ones first), threadgroup memory and its barrier, an atomic_int add on a uint word,
/// a compare exchange loop. tests/data/kernel.comp, the expected GLSL, compiles and links on Mesa
/// (llvmpipe, runtime/host/shadercheck).
#[test]
fn metal_kernel() {
    let msl = include_str!("data/kernel.metal");
    let s = |x: &str| x.to_string();
    let (text, iface) = shaderx::metal::translate_kernel(msl, [64, 1, 1], &[s("counts")])
        .unwrap_or_else(|f| panic!("{f:?}"));
    if std::env::var("PMU_PRINT").is_ok() {
        println!("{text}\n{iface:?}");
    }
    assert_eq!(text, include_str!("data/kernel.comp"));
    assert_eq!(
        iface.cbs,
        vec![(
            s("Globals"),
            1,
            0,
            vec![(s("count"), 0), (s("target"), 16), (s("radius"), 32), (s("colors"), 48)]
        )]
    );
    assert_eq!(iface.ins, vec![(s("items"), 2, 0), (s("raw"), 3, 1)]);
    assert_eq!(iface.outs, vec![(s("counts"), 0, 0)]);
    // a read-write buffer the kernel only reads (HLSLcc: const device) is bound with the
    // writable ones, as its records list it, and stays readonly in GLSL
    let (text2, iface2) =
        shaderx::metal::translate_kernel(msl, [64, 1, 1], &[s("counts"), s("items")]).unwrap();
    assert_eq!(iface2.outs, vec![(s("counts"), 0, 0), (s("items"), 2, 1)]);
    assert_eq!(iface2.ins, vec![(s("raw"), 3, 0)]);
    assert!(text2.contains("layout(std430, binding = 1) readonly buffer items {"));
    assert!(text2.contains("layout(std430, binding = 2) readonly buffer raw {"));
    // HLSLcc's bitFieldExtractU helper: its definition left out, the GLSL twin used
    let with_extract = msl
        .replace(
            "kernel void computeMain(",
            "uint bitFieldExtractU(uint width, uint offset, uint src);\nuint bitFieldExtractU(uint width, uint offset, uint src)\n{\n\tbool isWidthZero = (width == 0);\n\treturn select(src >> offset, (uint)0, isWidthZero);\n}; \nkernel void computeMain(",
        )
        .replace(
            "u_xlatu2 = raw[mtl_ThreadID.x]",
            "u_xlatu2 = bitFieldExtractU(0x8u, 0x10u, raw[mtl_ThreadID.x])",
        );
    let (text3, _) = shaderx::metal::translate_kernel(&with_extract, [64, 1, 1], &[]).unwrap();
    assert!(text3.contains("uint pm_bitFieldExtractU(uint width, uint offset, uint src)\n{"));
    assert!(text3.contains("u_xlatu2 = pm_bitFieldExtractU(0x8u, 0x10u, raw_buf[int(gl_GlobalInvocationID.x)])"));
    // what is not translated says why
    let refuse = |from: &str, to: &str, why: &str| {
        let msl = msl.replace(from, to);
        assert_ne!(msl, include_str!("data/kernel.metal"), "{why}");
        match shaderx::metal::translate_kernel(&msl, [64, 1, 1], &[]) {
            Err(Fault::Uns(m)) => assert_eq!(m, why),
            got => panic!("{why}: {got:?}"),
        }
    };
    refuse(
        "    uint count;\n",
        "    bool count;\n",
        "MSL: constant buffer Globals_Type: count of bool",
    );
    refuse(
        "    float4 colors[2];\n",
        "    float colors[2];\n",
        "MSL: constant buffer Globals_Type: colors of float",
    );
    refuse(
        "    const device uint *raw [[ buffer(3) ]],\n",
        "    const device uint *raw [[ buffer(3) ]],\n    device atomic_uint* raw_counter [[ buffer(4) ]],\n",
        "MSL: counter raw_counter (an append or consume buffer)",
    );
    refuse(
        "    const device uint *raw [[ buffer(3) ]],\n",
        "    const device uint *raw [[ buffer(3) ]],\n    texture3d<float, access::write > vol [[ texture(0) ]] ,\n",
        "MSL: kernel parameter texture3d < float , access::write > vol [[ texture(0) ]]",
    );
    refuse(
        "atomic_fetch_add_explicit(reinterpret_cast<device atomic_int *>",
        "atomic_fetch_max_explicit(reinterpret_cast<device atomic_int *>",
        "MSL: atomic_fetch_max_explicit on uint as int",
    );
    // a graphics stage is still refused a kernel
    match translate_program(msl, msl, "highp", (6000, 0), &mut Vec::new()) {
        Err(Fault::Uns(m)) => assert_eq!(m, "MSL: top level 'kernel'"),
        got => panic!("{got:?}"),
    }
}
