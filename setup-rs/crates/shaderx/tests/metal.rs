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
