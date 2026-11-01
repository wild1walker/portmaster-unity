//! The settings file (globalgamemanagers) for the Android player (android_data.py
//! edit_globalgamemanagers): Android platform, GLES3 only, GLES3 shader defines, MonoManager's
//! list, Rewired's joystick axes, PlayerSettings tweaks, retargeted to the player's version; and
//! the build id the OBBs are matched by (build_id).
use crate::android_data::{Ctx, Stop, ANDROID};
use crate::pyjson::Json;
use crate::typetrees;
use crate::uversion::Version;
use std::collections::BTreeSet;
use std::path::Path;
use unityfs::{DataFile, Value};

const SHADER_PLATFORM_GLCORE: i128 = 15;
const SHADER_PLATFORM_GLES3: i128 = 9;
const SHADER_PLATFORM_D3D11: i128 = 4;
pub const BUILTIN_ASSEMBLY_SLOTS: usize = 48;
pub const SWAPPY_FROM: (u32, u32) = (2019, 3);

/// The engine's slots at the start of MonoManager's list (Unity 5.x - 2017.x).
pub fn builtin_assembly_slots(unity: Option<&Version>) -> usize {
    match unity {
        Some(u) if u.major < 2017 || u.line() == (2017, 1) => 2,
        Some(u) if u.line() == (2017, 2) => 45,
        _ => BUILTIN_ASSEMBLY_SLOTS,
    }
}

/// UnityEngine.dll or UnityEngine.<word>Module.dll.
pub fn is_engine_module(name: &str) -> bool {
    if name == "UnityEngine.dll" {
        return true;
    }
    let Some(mid) = name
        .strip_prefix("UnityEngine.")
        .and_then(|r| r.strip_suffix("Module.dll"))
    else {
        return false;
    };
    !mid.is_empty() && mid.chars().all(|c| c.is_alphanumeric() || c == '_')
}

/// (game assemblies, engine modules) of an assembly list: Unity 5.x - 2017 by slot, later by name.
pub fn split_assemblies(names: &[String], unity: Option<&Version>) -> (Vec<String>, Vec<String>) {
    if unity.is_none_or(|u| u.major <= 2017) {
        let n = builtin_assembly_slots(unity);
        let game = names
            .iter()
            .enumerate()
            .filter(|(i, a)| *i >= n && !a.is_empty())
            .map(|(_, a)| a.clone())
            .collect();
        let engine = names
            .iter()
            .enumerate()
            .filter(|(i, a)| *i < n && !a.is_empty())
            .map(|(_, a)| a.clone())
            .collect();
        return (game, engine);
    }
    (
        names
            .iter()
            .filter(|a| !a.is_empty() && !is_engine_module(a))
            .cloned()
            .collect(),
        names
            .iter()
            .filter(|a| !a.is_empty() && is_engine_module(a))
            .cloned()
            .collect(),
    )
}

fn strs(v: Option<&Value>) -> Vec<String> {
    v.and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .map(|x| String::from_utf8_lossy(x.as_bytes().unwrap_or(b"")).into_owned())
                .collect()
        })
        .unwrap_or_default()
}

fn list_repr(v: &[String]) -> String {
    format!(
        "[{}]",
        v.iter()
            .map(|s| pkgzip::py::str_repr(s))
            .collect::<Vec<_>>()
            .join(", ")
    )
}

fn ints_repr(v: &[i128]) -> String {
    format!(
        "[{}]",
        v.iter()
            .map(|i| i.to_string())
            .collect::<Vec<_>>()
            .join(", ")
    )
}

pub const GLES_LEVELS: &[(i64, &str)] = &[(2, "3.0"), (3, "3.1"), (5, "3.2")];

/// PlayerSettings.playerMinOpenGLESVersion to write (--min-gles), None to keep the data's.
pub fn min_gles_level(min_gles: &str, have: Option<i64>) -> Option<i64> {
    let have = have?;
    match min_gles {
        "keep" => None,
        "auto" => (have < 2).then_some(2),
        m => GLES_LEVELS.iter().find(|(_, v)| *v == m).map(|(k, _)| *k),
    }
}

fn pybool(b: bool) -> &'static str {
    if b {
        "True"
    } else {
        "False"
    }
}

/// edit_globalgamemanagers: the file's new bytes and the notes.
pub fn edit(src: &Path, ctx: &mut Ctx) -> Result<(Vec<u8>, Vec<String>), Stop> {
    let a = ctx.args.clone();
    let mut f = DataFile::open(src, &ctx.opts())?;
    let mut notes: Vec<String> = Vec::new();
    let objs: Vec<(i64, String)> =
        f.sf.objects
            .iter()
            .map(|o| (o.path_id, f.sf.class_name(o)))
            .collect();
    for (pid, n) in objs {
        let sf = &mut f.sf;
        match n.as_str() {
            "BuildSettings" => {
                let mut t = sf.read_typetree(pid)?;
                let mut apis = Vec::new();
                for x in a.graphics_apis.split(',') {
                    apis.push(pkgzip::py::int(x, 10).ok_or_else(|| {
                        Stop::Fail(format!(
                            "ValueError: invalid literal for int() with base 10: {}",
                            pkgzip::py::str_repr(x)
                        ))
                    })?);
                }
                let have: Vec<i128> = t
                    .get("m_GraphicsAPIs")
                    .and_then(Value::as_array)
                    .map(|v| v.iter().filter_map(Value::as_int).collect())
                    .ok_or_else(|| Stop::Fail("KeyError: 'm_GraphicsAPIs'".into()))?;
                if have != apis {
                    notes.push(format!(
                        "BuildSettings.m_GraphicsAPIs {} -> {}",
                        ints_repr(&have),
                        ints_repr(&apis)
                    ));
                    t.set(
                        "m_GraphicsAPIs",
                        Value::Array(apis.iter().map(|&x| Value::I64(x as i64)).collect()),
                    )?;
                    sf.save_typetree(pid, &t)?;
                }
            }
            "GraphicsSettings" => {
                let mut t = sf.read_typetree(pid)?;
                let Some(d) = t
                    .get_mut("m_ShaderDefinesPerShaderCompiler")
                    .and_then(Value::as_array_mut)
                else {
                    continue;
                };
                let plat = |e: &Value| e.get("shaderPlatform").and_then(Value::as_int);
                let have: BTreeSet<i128> = d.iter().filter_map(plat).collect();
                let src_plat = [SHADER_PLATFORM_GLCORE, SHADER_PLATFORM_D3D11]
                    .into_iter()
                    .find(|p| have.contains(p));
                if let (false, Some(sp)) = (have.contains(&SHADER_PLATFORM_GLES3), src_plat) {
                    let mut e = d.iter().find(|e| plat(e) == Some(sp)).unwrap().clone();
                    e.set("shaderPlatform", Value::I64(SHADER_PLATFORM_GLES3 as i64))?;
                    d.push(e);
                    notes.push(format!(
                        "GraphicsSettings: GLES3 shader defines = {} defines",
                        if sp == SHADER_PLATFORM_GLCORE {
                            "GLCore"
                        } else {
                            "D3D11"
                        }
                    ));
                    sf.save_typetree(pid, &t)?;
                }
            }
            "MonoManager" => {
                let mut t = sf.read_typetree(pid)?;
                if t.get("m_AssemblyNames").is_none() {
                    continue;
                }
                let names = strs(t.get("m_AssemblyNames"));
                let unity = Some(&ctx.unity);
                let (game, engine) = split_assemblies(&names, unity);
                let mut missing: Vec<String> = game
                    .into_iter()
                    .filter(|x| !ctx.managed_names.contains(x))
                    .collect();
                let slots = builtin_assembly_slots(unity);
                let drop: Vec<usize> = names
                    .iter()
                    .enumerate()
                    .filter(|(i, x)| {
                        missing.contains(x) && a.drop_missing_assembly.contains(x) && *i >= slots
                    })
                    .map(|(i, _)| i)
                    .collect();
                if !drop.is_empty() {
                    let types_len = t
                        .get("m_AssemblyTypes")
                        .and_then(Value::as_array)
                        .map(Vec::len);
                    if types_len.is_some_and(|n| n != names.len()) {
                        return Err(Stop::Exit(
                            "MonoManager: m_AssemblyTypes and m_AssemblyNames differ in length"
                                .into(),
                        ));
                    }
                    let removed: Vec<String> = drop.iter().map(|&i| names[i].clone()).collect();
                    notes.push(format!(
                        "MonoManager: removed {} (not in Managed/)",
                        list_repr(&removed)
                    ));
                    for key in ["m_AssemblyNames", "m_AssemblyTypes"] {
                        if let Some(arr) = t.get_mut(key).and_then(Value::as_array_mut) {
                            let kept: Vec<Value> = arr
                                .iter()
                                .enumerate()
                                .filter(|(i, _)| !drop.contains(i))
                                .map(|(_, v)| v.clone())
                                .collect();
                            *arr = kept;
                        }
                    }
                    sf.save_typetree(pid, &t)?;
                    missing.retain(|x| !a.drop_missing_assembly.contains(x));
                }
                if !missing.is_empty() {
                    return Err(Stop::Exit(format!(
                        "MonoManager lists assemblies missing from Managed/: {}",
                        list_repr(&missing)
                    )));
                }
                let absent: Vec<String> = engine
                    .into_iter()
                    .filter(|x| !ctx.managed_names.contains(x))
                    .collect();
                if !absent.is_empty() {
                    notes.push(format!(
                        "MonoManager: engine modules the player will skip (not in its Managed set): {}",
                        list_repr(&absent)
                    ));
                }
            }
            "InputManager" if ctx.managed_names.contains("Rewired_Core.dll") => {
                let mut t = sf.read_typetree(pid)?;
                let ax = t
                    .get_mut("m_Axes")
                    .and_then(Value::as_array_mut)
                    .ok_or_else(|| Stop::Fail("KeyError: 'm_Axes'".into()))?;
                let name_of = |v: &Value| {
                    v.get("m_Name")
                        .and_then(Value::as_bytes)
                        .map(<[u8]>::to_vec)
                        .unwrap_or_default()
                };
                let have: BTreeSet<Vec<u8>> = ax.iter().map(name_of).collect();
                let proto = ax
                    .first()
                    .cloned()
                    .ok_or_else(|| Stop::Fail("StopIteration".into()))?;
                let mut added = 0;
                for j in 1..=a.rewired_joysticks {
                    for k in 1..=28i64 {
                        let name = format!("Joy{j}Axis{k}");
                        if have.contains(name.as_bytes()) {
                            continue;
                        }
                        let mut e = proto.clone();
                        for (f, v) in [
                            ("m_Name", Value::str(&name)),
                            ("descriptiveName", Value::str("")),
                            ("descriptiveNegativeName", Value::str("")),
                            ("negativeButton", Value::str("")),
                            ("positiveButton", Value::str("")),
                            ("altNegativeButton", Value::str("")),
                            ("altPositiveButton", Value::str("")),
                            ("gravity", Value::F64(0.0)),
                            ("dead", Value::F64(0.0)),
                            ("sensitivity", Value::F64(1.0)),
                            ("snap", Value::Bool(false)),
                            ("invert", Value::Bool(false)),
                            ("type", Value::I64(2)),
                            ("axis", Value::I64(k - 1)),
                            ("joyNum", Value::I64(j)),
                        ] {
                            e.set(f, v)?;
                        }
                        ax.push(e);
                        added += 1;
                    }
                }
                if added > 0 {
                    notes.push(format!(
                        "InputManager: {added} Rewired joystick axes (Joy1-{} Axis1-28) added",
                        a.rewired_joysticks
                    ));
                    sf.save_typetree(pid, &t)?;
                }
            }
            "QualitySettings" if a.async_upload_buffer != 0 => {
                let mut t = sf.read_typetree(pid)?;
                if let Some(qs) = t.get_mut("m_QualitySettings").and_then(Value::as_array_mut) {
                    for q in qs {
                        q.set("asyncUploadBufferSize", Value::I64(a.async_upload_buffer))?;
                    }
                }
                notes.push(format!(
                    "QualitySettings.asyncUploadBufferSize = {} MB",
                    a.async_upload_buffer
                ));
                sf.save_typetree(pid, &t)?;
            }
            "PlayerSettings" => {
                let edits = [
                    "m_MTRendering",
                    "mobileMTRenderingBaked",
                    "androidBlitType",
                    "androidUseSwappy",
                    "playerMinOpenGLESVersion",
                ];
                let (mut t, node) = typetrees::read(sf, pid, &edits)?;
                if node.is_some() {
                    notes.push("PlayerSettings read with a fixed type tree (typetrees.py)".into());
                }
                let mut changed = false;
                let mt = |t: &Value| {
                    t.get("m_MTRendering")
                        .and_then(Value::as_bool)
                        .unwrap_or(false)
                };
                if a.mt_rendering != "keep" {
                    t.set("m_MTRendering", Value::Bool(a.mt_rendering == "on"))?;
                    notes.push(format!("PlayerSettings.m_MTRendering = {}", pybool(mt(&t))));
                    changed = true;
                }
                if let Some(b) = t.get("mobileMTRenderingBaked").and_then(Value::as_bool) {
                    if t.get("m_MTRendering").and_then(Value::as_bool) != Some(b) {
                        let m = mt(&t);
                        t.set("mobileMTRenderingBaked", Value::Bool(m))?;
                        notes.push(format!(
                            "PlayerSettings.mobileMTRenderingBaked = m_MTRendering ({})",
                            pybool(m)
                        ));
                        changed = true;
                    }
                }
                let have = t.get("playerMinOpenGLESVersion").and_then(Value::as_i64);
                if let Some(level) = min_gles_level(&a.min_gles, have) {
                    if have != Some(level) {
                        t.set("playerMinOpenGLESVersion", Value::I64(level))?;
                        let name = GLES_LEVELS.iter().find(|(k, _)| *k == level).unwrap().1;
                        notes.push(format!(
                            "PlayerSettings.playerMinOpenGLESVersion = {level} (OpenGL ES {name})"
                        ));
                        changed = true;
                    }
                }
                if a.blit != "keep" {
                    let v = match a.blit.as_str() {
                        "always" => 0,
                        "never" => 1,
                        _ => 2,
                    };
                    t.set("androidBlitType", Value::I64(v))?;
                    notes.push(format!("PlayerSettings.androidBlitType = {v}"));
                    changed = true;
                }
                if a.swappy != "keep" {
                    if t.get("androidUseSwappy").is_some() {
                        t.set("androidUseSwappy", Value::Bool(a.swappy == "on"))?;
                        notes.push(format!(
                            "PlayerSettings.androidUseSwappy = {}",
                            pybool(a.swappy == "on")
                        ));
                        changed = true;
                    } else if ctx.unity.line() >= SWAPPY_FROM {
                        notes.push("PlayerSettings: no androidUseSwappy, --swappy ignored".into());
                    }
                }
                if changed {
                    typetrees::save(sf, pid, &t, node.as_ref())?;
                }
            }
            _ => {}
        }
    }
    let old = f.sf.target_platform;
    f.sf.target_platform = ANDROID as i32;
    notes.push(format!("target platform {old} -> {ANDROID}"));
    notes.extend(ctx.retarget_sf(&mut f.sf)?);
    let data = f.sf.save()?;
    ctx.log(&format!("globalgamemanagers: {}", notes.join("; ")));
    Ok((data, notes))
}

/// unity.build-id: from BuildSettings.buildGUID (PlayerSettings.productGUID, else a hash of the
/// settings file), as 8-4-4-4-12 hex.
pub fn build_id(path: &Path, ctx: &Ctx) -> Result<String, Stop> {
    let f = DataFile::open(path, &ctx.opts())?;
    let mut h: Option<String> = None;
    for (cls, field) in [
        ("BuildSettings", "buildGUID"),
        ("PlayerSettings", "productGUID"),
    ] {
        for o in &f.sf.objects {
            if f.sf.class_name(o) != cls || h.is_some() {
                continue;
            }
            let (t, _) = typetrees::read(&f.sf, o.path_id, &[])?;
            if let Some(g) = t.get(field) {
                let parts: Vec<u64> = (0..4)
                    .map(|i| {
                        g.get(&format!("data[{i}]"))
                            .and_then(Value::as_u64)
                            .unwrap_or(0)
                    })
                    .collect();
                let any = g.as_struct().is_some_and(|s| {
                    s.fields
                        .iter()
                        .any(|(_, v)| v.as_f64().is_some_and(|x| x != 0.0))
                });
                if any {
                    h = Some(parts.iter().map(|p| format!("{p:08x}")).collect());
                }
            }
        }
    }
    let h = match h {
        Some(h) => h,
        None => {
            let data = std::fs::read(path)?;
            crate::sha1::hex(&data)[..32].to_string()
        }
    };
    Ok(format!(
        "{}-{}-{}-{}-{}",
        &h[..8],
        &h[8..12],
        &h[12..16],
        &h[16..20],
        &h[20..]
    ))
}

/// The assembly lists of a settings file (MonoManager.m_AssemblyNames).
pub fn assembly_lists(path: &Path, ctx: &Ctx) -> Result<Vec<Vec<String>>, Stop> {
    let f = DataFile::open(path, &ctx.opts())?;
    let mut lists = Vec::new();
    for o in &f.sf.objects {
        if f.sf.class_name(o) == "MonoManager" {
            let t = f.sf.read_typetree(o.path_id)?;
            if t.get("m_AssemblyNames").is_some() {
                lists.push(strs(t.get("m_AssemblyNames")));
            }
        }
    }
    Ok(lists)
}

/// A JSON list of strings (ScriptingAssemblies.json "names").
pub fn json_names(j: &Json) -> Vec<String> {
    j.get("names")
        .and_then(Json::as_list)
        .map(|l| l.iter().map(Json::py_str).collect())
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn assemblies() {
        assert!(is_engine_module("UnityEngine.AudioModule.dll"));
        assert!(is_engine_module("UnityEngine.dll"));
        assert!(!is_engine_module("UnityEngine.UI.dll"));
        let mut names: Vec<String> = (0..48)
            .map(|i| format!("UnityEngine.M{i}Module.dll"))
            .collect();
        names[47] = String::new();
        names.push("Assembly-CSharp.dll".into());
        let v = Version::parse("2017.4.9f1").unwrap();
        let (game, engine) = split_assemblies(&names, Some(&v));
        assert_eq!(game, ["Assembly-CSharp.dll"]);
        assert_eq!(engine.len(), 47);
        assert_eq!(min_gles_level("auto", Some(0)), Some(2));
        assert_eq!(min_gles_level("auto", None), None);
        assert_eq!(min_gles_level("3.2", Some(2)), Some(5));
    }
}
