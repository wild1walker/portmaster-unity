//! Golden tests against the Python's own output on a game's data: the folder
//! setup-rs/tests/golden/shaderx_golden.py writes, named by $SHADERX_GOLDEN (skipped without it).
//! Every Shader's report, edit (fields and compressed blob) and GLES3 programs are compared with
//! the step's, and with a convert run's output where the folder has it; every distinct program
//! translation; LZ4HC on made-up inputs; the step's summary line.
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};

use serde_json::{json, Value};
use shaderx::blob::{BlobFields, Lengths};
use shaderx::convert::{self, Pass, Shader, StageProgram, SubProgram, STAGES};
use shaderx::glsl::{self, Es, Optimize};
use shaderx::{lz4, step};

fn golden() -> Option<PathBuf> {
    match std::env::var_os("SHADERX_GOLDEN") {
        Some(d) => Some(PathBuf::from(d)),
        None => {
            eprintln!("SHADERX_GOLDEN not set: golden test skipped");
            None
        }
    }
}

fn ints(v: &Value) -> Vec<i64> {
    v.as_array()
        .unwrap()
        .iter()
        .map(|x| x.as_i64().unwrap())
        .collect()
}

fn lengths(v: &Value) -> Lengths {
    let a = v.as_array().unwrap();
    if a.first().is_some_and(|x| x.is_array()) {
        Lengths::Nested(a.iter().map(ints).collect())
    } else {
        Lengths::Flat(ints(v))
    }
}

fn lengths_json(l: &Lengths) -> Value {
    match l {
        Lengths::Flat(v) => json!(v),
        Lengths::Nested(v) => json!(v),
    }
}

fn sub_program(v: &Value) -> SubProgram {
    let opt = |k: &str| v.get(k).map(ints);
    SubProgram {
        blob_index: v["m_BlobIndex"].as_i64().unwrap(),
        gpu_program_type: v["m_GpuProgramType"].as_i64().unwrap(),
        keyword_indices: opt("m_KeywordIndices"),
        global_keyword_indices: opt("m_GlobalKeywordIndices"),
        local_keyword_indices: opt("m_LocalKeywordIndices"),
        hardware_tier: v.get("m_ShaderHardwareTier").map(|x| x.as_i64().unwrap()),
    }
}

/// The Shader of an obj/<hash>.json (the fields the Python harness saves) and its blob.
fn shader(fields: &Value, blob: Vec<u8>) -> Shader {
    let sub_shaders = fields["subshaders"]
        .as_array()
        .unwrap()
        .iter()
        .map(|ss| {
            ss.as_array()
                .unwrap()
                .iter()
                .map(|p| {
                    let mut pass = Pass {
                        platforms: p.get("m_Platforms").map(ints),
                        ..Default::default()
                    };
                    for (k, st) in STAGES.iter().enumerate() {
                        let Some(prog) = p.get(*st) else { continue };
                        let list = |l: &Value| {
                            l.as_array()
                                .unwrap()
                                .iter()
                                .map(sub_program)
                                .collect::<Vec<_>>()
                        };
                        pass.programs[k] = Some(StageProgram {
                            sub_programs: prog.get("m_SubPrograms").map(list),
                            player_sub_programs: prog
                                .get("m_PlayerSubPrograms")
                                .map(|t| t.as_array().unwrap().iter().map(list).collect()),
                            parameter_blob_indices: prog
                                .get("m_ParameterBlobIndices")
                                .map(|t| t.as_array().unwrap().iter().map(ints).collect()),
                        });
                    }
                    pass
                })
                .collect()
        })
        .collect();
    Shader {
        name: fields["name"].as_str().unwrap().to_string(),
        blob: BlobFields {
            platforms: ints(&fields["platforms"]),
            offsets: lengths(&fields["offsets"]),
            compressed_lengths: lengths(&fields["compressedLengths"]),
            decompressed_lengths: lengths(&fields["decompressedLengths"]),
            compressed_blob: blob,
            stage_counts: fields.get("stageCounts").map(ints),
        },
        sub_shaders,
    }
}

/// The saved fields with an edit applied, as the Python's tree has them after the step.
fn apply_json(fields: &Value, edit: &convert::Edit) -> Value {
    let mut out = fields.clone();
    let b = &edit.blob;
    out["platforms"] = json!(b.platforms);
    out["offsets"] = lengths_json(&b.offsets);
    out["compressedLengths"] = lengths_json(&b.compressed_lengths);
    out["decompressedLengths"] = lengths_json(&b.decompressed_lengths);
    if let Some(sc) = &b.stage_counts {
        out["stageCounts"] = json!(sc);
    }
    for le in &edit.lists {
        let prog = &mut out["subshaders"][le.at.sub_shader][le.at.pass][STAGES[le.at.stage]];
        let old = match le.at.tier {
            None => prog["m_SubPrograms"].clone(),
            Some(t) => prog["m_PlayerSubPrograms"][t].clone(),
        };
        let new: Vec<Value> = le
            .items
            .iter()
            .map(|it| {
                let mut v = old[it.from].clone();
                v["m_GpuProgramType"] = json!(it.gpu_program_type);
                v
            })
            .collect();
        match le.at.tier {
            None => prog["m_SubPrograms"] = json!(new),
            Some(t) => {
                prog["m_PlayerSubPrograms"][t] = json!(new);
                prog["m_ParameterBlobIndices"][t] =
                    json!(le.parameter_blob_indices.clone().unwrap());
            }
        }
    }
    for pp in &edit.pass_platforms {
        out["subshaders"][pp.sub_shader][pp.pass]["m_Platforms"] = json!(pp.platforms);
    }
    out
}

fn load(dir: &Path, hash: &str) -> (Value, Vec<u8>) {
    let p = dir.join("obj").join(hash);
    let js = std::fs::read(p.with_extension("json")).unwrap();
    (
        serde_json::from_slice(&js).unwrap(),
        std::fs::read(p.with_extension("blob")).unwrap(),
    )
}

fn report_json(r: &convert::Report) -> Value {
    json!({
        "name": r.name, "status": r.status.as_str(), "changed": r.changed, "why": r.why, "programs": r.programs,
        "twins": r.twins, "guessed_layout": r.guessed_layout, "notes": r.notes, "dropped": r.dropped, "es31": r.es31,
        "programs4x": r.programs4x, "empty": r.empty,
        "failed": r.failed.iter().map(|(s, w)| json!([s, w])).collect::<Vec<_>>(),
    })
}

#[test]
fn shaders_as_the_step_converts_them() {
    let Some(dir) = golden() else { return };
    let index: Value =
        serde_json::from_slice(&std::fs::read(dir.join("index.json")).unwrap()).unwrap();
    let precision = index["precision"].as_str().unwrap().to_string();
    let optimize = Optimize::from_names(
        &index["optimize"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap())
            .collect::<Vec<_>>(),
    );
    let replace = index["replace"].as_bool().unwrap();
    let mut tr = convert::Translations::new();
    let mut done: HashMap<(String, bool), convert::Outcome> = HashMap::new();
    let mut reports = Vec::new();
    let mut changed_files: BTreeMap<String, bool> = BTreeMap::new();
    let (mut shaders, mut edits, mut programs, mut cf_same, mut cf_rows, mut bad) =
        (0, 0, 0, 0, 0, Vec::new());
    let mut spent = std::time::Duration::ZERO;
    for row in index["rows"].as_array().unwrap() {
        let file = row["file"].as_str().unwrap();
        let Some(hin) = row["in"].as_str() else {
            bad.push(format!(
                "{file} {}: a Shader only the converted data has",
                row["path_id"]
            ));
            continue;
        };
        assert!(
            row.get("unmodeled").is_none(),
            "{file} {}: the step changed fields the harness does not save",
            row["path_id"]
        );
        shaders += 1;
        let gl4 = row["glcore4x"].as_bool().unwrap();
        let (fin, bin) = load(&dir, hin);
        let sh = shader(&fin, bin.clone());
        let opt = convert::Options {
            precision: precision.clone(),
            replace,
            optimize,
            glcore4x: gl4,
        };
        let oc = done
            .entry((hin.to_string(), gl4))
            .or_insert_with(|| {
                let t = std::time::Instant::now();
                let oc = convert::add_gles3(&sh, &opt, &mut tr)
                    .unwrap_or_else(|e| panic!("{file}: {e}"));
                spent += t.elapsed();
                oc
            })
            .clone();
        let what = format!("{file} {} ({})", row["path_id"], sh.name);
        if report_json(&oc.report) != row["result"] {
            bad.push(format!(
                "{what}: report {} != {}",
                report_json(&oc.report),
                row["result"]
            ));
        }
        reports.push(oc.report.clone());
        *changed_files.entry(file.to_string()).or_default() |= oc.edit.is_some();
        let (ours_json, ours_blob) = match (&oc.edit, row["out"].as_str()) {
            (Some(edit), Some(hout)) => {
                edits += 1;
                let (fout, bout) = load(&dir, hout);
                let mine = apply_json(&fin, edit);
                if mine != fout {
                    bad.push(format!("{what}: fields differ from the step's"));
                }
                if edit.blob.compressed_blob != bout {
                    bad.push(format!(
                        "{what}: compressedBlob differs ({} / {} bytes)",
                        edit.blob.compressed_blob.len(),
                        bout.len()
                    ));
                }
                // every GLES3 program's text
                let theirs = convert::gles3_programs(&shader(&fout, bout)).unwrap();
                let ours = convert::gles3_programs(&convert::apply(&sh, edit)).unwrap();
                programs += theirs.len();
                if ours != theirs {
                    let n = ours.iter().zip(&theirs).filter(|(a, b)| a != b).count();
                    bad.push(format!(
                        "{what}: {n} GLES3 programs differ ({} / {})",
                        ours.len(),
                        theirs.len()
                    ));
                }
                (mine, edit.blob.compressed_blob.clone())
            }
            (None, None) => (fin.clone(), bin),
            (e, o) => {
                bad.push(format!(
                    "{what}: edit {} / step's output {}",
                    e.is_some(),
                    o.is_some()
                ));
                continue;
            }
        };
        if let Some(hcf) = row.get("converted").and_then(|h| h.as_str()) {
            cf_rows += 1;
            let (fcf, bcf) = load(&dir, hcf);
            if fcf == ours_json && bcf == ours_blob {
                cf_same += 1;
            } else {
                bad.push(format!(
                    "{what}: differs from the converted data's (fields {}, blob {})",
                    fcf == ours_json,
                    bcf == ours_blob
                ));
            }
        }
    }
    let nfiles = changed_files.values().filter(|c| **c).count();
    let lines = step::summary(&reports, nfiles, &precision);
    let log = std::fs::read_to_string(dir.join("log.txt")).unwrap();
    if lines[0] != log.trim_end() {
        bad.push(format!("summary: {:?} != {:?}", lines[0], log.trim_end()));
    }
    eprintln!(
        "{shaders} Shader objects ({} distinct, add_gles3 {:.2} s), {edits} edits, {programs} GLES3 programs compared; {cf_same} of {cf_rows} as in the converted data; {} distinct translations\n{}",
        done.len(),
        spent.as_secs_f64(),
        tr.len(),
        lines.join("\n")
    );
    for b in bad.iter().take(30) {
        eprintln!("MISMATCH {b}");
    }
    assert!(bad.is_empty(), "{} mismatches", bad.len());
}

#[test]
fn programs_as_glsl_py_translates_them() {
    let Some(dir) = golden() else { return };
    let text = std::fs::read_to_string(dir.join("programs.jsonl")).unwrap();
    let (mut n, mut stages, mut errors, mut bad) = (0, 0, 0, Vec::new());
    for line in text.lines() {
        let rec: Value = serde_json::from_str(line).unwrap();
        let code = rec["code"].as_str().unwrap();
        let precision = rec["precision"].as_str().unwrap();
        let opt = Optimize::from_names(
            &rec["optimize"]
                .as_array()
                .unwrap()
                .iter()
                .map(|x| x.as_str().unwrap())
                .collect::<Vec<_>>(),
        );
        let es = if rec["es"] == "310" {
            Es::Es310
        } else {
            Es::Es300
        };
        let mut notes = Vec::new();
        let res = glsl::program_to_es(code, precision, opt, &mut notes, es);
        n += 1;
        stages += glsl::stage_blocks(code).len();
        let want_notes: Vec<String> = rec["notes"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap().to_string())
            .collect();
        let same = match (&res, rec.get("out"), rec.get("error")) {
            (Ok(out), Some(want), None) => out == want.as_str().unwrap(),
            (Err(e), None, Some(want)) => {
                errors += 1;
                json!([e.class(), e.msg]) == *want
            }
            _ => false,
        };
        if !same || notes != want_notes {
            bad.push(format!(
                "program {n} (es {}): result {same}, notes {:?} / {:?}",
                es.as_str(),
                notes,
                want_notes
            ));
        }
    }
    eprintln!(
        "{n} distinct translations ({stages} stages; {errors} Unsupported / NeedsES31) compared"
    );
    for b in bad.iter().take(20) {
        eprintln!("MISMATCH {b}");
    }
    assert!(bad.is_empty(), "{} of {n} translations differ", bad.len());
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

#[test]
fn lz4hc_as_python_lz4() {
    let Some(dir) = golden() else { return };
    let text = std::fs::read_to_string(dir.join("lz4.jsonl")).unwrap();
    let mut n = 0;
    for line in text.lines() {
        let rec: Value = serde_json::from_str(line).unwrap();
        let (src, want) = (
            unhex(rec["in"].as_str().unwrap()),
            unhex(rec["out"].as_str().unwrap()),
        );
        let z = lz4::compress_hc(&src);
        assert!(
            z == want,
            "case {n} ({} bytes): {} bytes compressed, python-lz4 {}",
            src.len(),
            z.len(),
            want.len()
        );
        assert_eq!(lz4::decompress(&want, src.len()).unwrap(), src);
        n += 1;
    }
    eprintln!("{n} LZ4HC cases identical");
}
