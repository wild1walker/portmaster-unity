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
