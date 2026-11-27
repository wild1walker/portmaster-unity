//! `pmu convert` on made-up data (crates/convert/tests/madeup: no game files) in the cases of the
//! retired tests/golden/convert_madeup.sh, frozen: each case's exit status, stdout and stderr (the
//! timings and the run's folder replaced) and every file it leaves (path, size, CRC-32, empty folders
//! too) are tests/golden/convert.txt's. That file was written (PMU_CONVERT_BLESS=1) from a run of
//! pmu whose every case convert_madeup.sh had found the same as `python -m pmunity convert`'s, byte
//! for byte (2026-11-27, before the Python converter was removed). Left out: the cases whose prep or
//! reference was the Python's (from_python, edits_from_python, textures_guide's guide, scenes'
//! reference). Needs $PMU_TPK (skipped without).
#[path = "../../crates/convert/tests/madeup/mod.rs"]
mod madeup;

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn put(p: &Path, data: impl AsRef<[u8]>) {
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, data).unwrap();
}

fn copy_tree(src: &Path, dst: &Path) {
    fs::create_dir_all(dst).unwrap();
    let mut ents: Vec<_> = fs::read_dir(src).unwrap().map(|e| e.unwrap()).collect();
    ents.sort_by_key(|e| e.file_name());
    for e in ents {
        let (s, d) = (e.path(), dst.join(e.file_name()));
        if e.file_type().unwrap().is_dir() {
            copy_tree(&s, &d);
        } else {
            fs::copy(&s, &d).unwrap();
        }
    }
}

/// Every file (size, CRC-32) and empty folder under top, sorted.
fn tree(top: &Path, rel: &str, out: &mut Vec<String>) {
    let mut ents: Vec<_> = fs::read_dir(top.join(rel)).unwrap().map(|e| e.unwrap()).collect();
    ents.sort_by_key(|e| e.file_name());
    if ents.is_empty() && !rel.is_empty() {
        out.push(format!("  dir  {rel}/"));
    }
    for e in ents {
        let n = e.file_name().to_string_lossy().into_owned();
        let r = if rel.is_empty() { n } else { format!("{rel}/{n}") };
        if e.file_type().unwrap().is_dir() {
            tree(top, &r, out);
        } else {
            let b = fs::read(e.path()).unwrap();
            out.push(format!("  file {r} {} {:08x}", b.len(), crc32fast::hash(&b)));
        }
    }
}

/// Timings ("[   1.2s]") and the run folder's path out of a run's text.
fn norm(text: &[u8], run: &Path) -> String {
    let t = String::from_utf8_lossy(text).replace(&run.to_string_lossy().into_owned(), "<run>");
    t.lines()
        .map(|l| match l.strip_prefix('[').and_then(|r| r.split_once("s]")) {
            Some((n, rest)) if !n.trim().is_empty() && n.trim().bytes().all(|c| c.is_ascii_digit() || c == b'.') => format!("[t]{rest}"),
            _ => l.to_string(),
        })
        .map(|l| format!("  | {l}\n"))
        .collect()
}

fn words(s: &str) -> Vec<String> {
    s.split_whitespace().map(String::from).collect()
}

struct Run<'a> {
    base: &'a Path,
    src: &'a Path,
    out: String,
}

impl Run<'_> {
    fn pmu(dir: &Path, args: &[String]) -> std::process::Output {
        Command::new(env!("CARGO_BIN_EXE_pmu")).arg("convert").args(args).current_dir(dir).output().unwrap()
    }

    /// case <name> <prep args or none> <args>: <args> name the data folder Data
    fn case(&mut self, name: &str, prep: Option<&str>, args: &str) {
        let d = self.base.join(name);
        let _ = fs::remove_dir_all(&d);
        copy_tree(&self.src.join("Data"), &d.join("Data"));
        if let Some(p) = prep {
            let mut a = vec!["Data".to_string()];
            a.extend(words(p));
            Run::pmu(&d, &a);
        }
        let r = Run::pmu(&d, &words(args));
        self.out += &format!("== {name}\nstatus {}\nstdout\n{}stderr\n{}", r.status.code().unwrap_or(-1), norm(&r.stdout, &d), norm(&r.stderr, &d));
        let mut files = Vec::new();
        for f in ["Data", "Out"] {
            if d.join(f).is_dir() {
                tree(&d, f, &mut files);
            }
        }
        for l in files {
            self.out += &l;
            self.out.push('\n');
        }
    }
}

#[test]
fn convert_golden() {
    let Some(db) = madeup::db() else {
        eprintln!("convert_golden: skipped (PMU_TPK: UnityPy's lzma.tpk)");
        return;
    };
    // (a fixed place: the outputs name it)
    let base = PathBuf::from("/tmp/pmu-convert-golden");
    let _ = fs::remove_dir_all(&base);
    let src = base.join("src");
    madeup::game(&db, &src.join("Data"));
    put(&src.join("cuphead.json"), madeup::CUPHEAD_JSON);
    put(&src.join("animclips.py"), "# pmu's built-in animclips step, named by the file\n");
    put(&src.join("defaults.json"), "{\"steps\": [\"shaders\", \"settings\", \"audio\"]}\n");
    put(
        &src.join("edits.json"),
        r#"{"about": "made-up edits",
 "steps": {
  "tweaks": [
   {"file": "globalgamemanagers", "class": "AudioManager",
    "set": {"m_Volume": 0.5, "Rolloff Scale": 2, "m_RealVoiceCount": 24, "m_DisableAudio": true, "m_SpatializerPlugin": "none"}},
   {"file": "global*", "class": "InputManager", "set": {"m_Axes.0.sensitivity": 2.5, "m_Axes.1.invert": true, "m_Axes.2.m_Name": "Renamed"}},
   {"file": "*", "class": "AudioClip", "name": "*_short", "set": {"m_LoadInBackground": true}},
   {"file": "level[0-9]*", "class": "AudioClip", "set": {"m_Frequency": 32000}},
   {"file": "*", "class": "Shader", "name": "TMP/*", "set": {"m_ParsedForm.m_Name": "TMP/Edited"}}],
  "none": [{"file": "nothing*", "class": "Light", "set": {"m_Intensity": 1}}]}}
"#,
    );
    put(
        &src.join("edits_bad.json"),
        r#"{"steps": {"oops": [{"file": "globalgamemanagers", "class": "AudioManager", "set": {"m_Volume.x": 1}}],
           "kind": [{"file": "globalgamemanagers", "class": "AudioManager", "set": {"m_RealVoiceCount": 1.5}}],
           "range": [{"file": "globalgamemanagers", "class": "AudioManager", "set": {"m_RealVoiceCount": 9999999999}}]}}
"#,
    );
    put(&src.join("edits_mono.json"), "{\"steps\": {\"x\": [{\"file\": \"a\", \"class\": \"MonoBehaviour\", \"set\": {\"m_Enabled\": false}}]}}\n");
    put(&src.join("edits_broken.json"), "{\"steps\": {\"x\": [\n");
    // (the scene bundle stays out, as it did in convert_madeup.sh's cases)
    fs::remove_file(src.join("Data/StreamingAssets/AssetBundles/tmpfont_scenes")).unwrap();
    let s = src.display();
    let c = format!(
        "--config {s}/cuphead.json --plugin {s}/animclips.py --set textures.quality=fast --set textures.guide={s}/astc_guide_fast.bin"
    );
    let steps = "--steps shaders,settings,audio,animclips";
    let e = format!("--config {s}/defaults.json --plugin {s}/edits.json");
    let mut r = Run { base: &base, src: &src, out: String::new() };
    r.case("cuphead", None, &format!("Data {c}"));
    r.case("again", Some(&c), &format!("Data {c}"));
    r.case("resumed", Some(&format!("{c} --steps shaders")), &format!("Data {c}"));
    r.case("from_pmu", Some(&format!("{c} --steps shaders,settings,textures")), &format!("Data {c}"));
    r.case(
        "textures_unscaled",
        None,
        &format!("Data {c} --steps textures --set textures.scale=1.0 --set textures.scale_sharp=1.0 --set textures.inline=true --set textures.inline_streams=false"),
    );
    r.case("textures_search", None, &format!("Data {c} --steps textures --set textures.mode=search --set textures.quality=fastest --set textures.mips=keep"));
    r.case("textures_only", None, &format!("Data {c} --steps textures --only atlas_"));
    r.case("budget_none", None, &format!("Data {c} {steps} --set audio.pcm_budget_mb=null --set audio.compressed_min_s=10"));
    r.case("budget_float", None, &format!("Data {c} {steps} --set audio.pcm_budget_mb=4.5 --set audio.resident_scenes=1"));
    r.case("audio_cfg", Some(&format!("{c} --steps audio")), &format!("Data {c} --steps audio --set audio.compressed_min_s=3"));
    r.case("grab_fetch", None, &format!("Data {c} --steps shaders --set shaders.grab_fetch=always"));
    r.case("grab_fetch_true", None, &format!("Data {c} --steps shaders --set shaders.grab_fetch=true"));
    r.case("grab_fetch_names", None, &format!("Data {c} --steps shaders --set shaders.grab_fetch=always --set shaders.grab_fetch_names=Offset|Screen"));
    r.case(
        "options",
        None,
        &format!("Data {c} {steps} --set shaders.precision=highp --set shaders.glcore4x=true --set settings.audio_sample_rate=24000 --set settings.real_voices=16 --set settings.graphics_apis=null"),
    );
    r.case("defaults", None, &format!("Data --config {s}/defaults.json"));
    r.case("overlay", None, &format!("Data Out {c}"));
    r.case("unknown_step", None, &format!("Data {c} --steps settings,nope"));
    r.case("no_data", None, &format!("Missing {c}"));
    r.case("edits", None, &format!("Data {e} --steps settings,tweaks,none"));
    r.case("edits_again", Some(&format!("{e} --steps tweaks")), &format!("Data {e} --steps tweaks,shaders"));
    r.case("edits_bad_path", None, &format!("Data {e} --plugin {s}/edits_bad.json --steps oops"));
    r.case("edits_bad_kind", None, &format!("Data {e} --plugin {s}/edits_bad.json --steps kind"));
    r.case("edits_bad_range", None, &format!("Data {e} --plugin {s}/edits_bad.json --steps range"));
    r.case("edits_mono", None, &format!("Data --config {s}/defaults.json --plugin {s}/edits_mono.json --steps x"));
    r.case("edits_broken", None, &format!("Data --config {s}/defaults.json --plugin {s}/edits_broken.json --steps x"));
    let got = r.out;
    let file = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/golden/convert.txt");
    if std::env::var_os("PMU_CONVERT_BLESS").is_some_and(|v| !v.is_empty()) {
        put(&file, &got);
        eprintln!("convert_golden: wrote {}", file.display());
        return;
    }
    let want = fs::read_to_string(&file).unwrap_or_default();
    if got != want {
        put(&base.join("convert.txt.new"), &got);
        let w: std::collections::BTreeSet<&str> = want.lines().collect();
        let (mut section, mut diffs) = ("", Vec::new());
        for l in got.lines() {
            if l.starts_with("== ") {
                section = l;
            } else if !w.contains(l) && diffs.len() < 40 {
                diffs.push(format!("{section}: {l}"));
            }
        }
        panic!("pmu convert's outputs are not tests/golden/convert.txt's; the new text is {}/convert.txt.new:\n{}", base.display(), diffs.join("\n"));
    }
    let _ = fs::remove_dir_all(&base);
}
