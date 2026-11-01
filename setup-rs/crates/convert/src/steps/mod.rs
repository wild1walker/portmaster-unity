//! The conversion steps a game's config names (__main__.BUILTIN_STEPS, OPTIONAL_STEPS and the
//! plugins' STEPS), each `run(job)` as the Python's: it skips the files the journal has as done,
//! writes each converted file through the job and journals it.
pub mod animclips;
pub mod audio;
pub mod settings;
pub mod shaders;
pub mod textures;

use crate::error::{Error, Result};
use crate::job::Job;
use crate::pyjson::Json;
use unityfs::Value;

/// What a step gets besides the job: convert's --only (textures: only matching files).
#[derive(Clone, Debug, Default)]
pub struct Args {
    pub only: Option<String>,
}

pub type StepFn = fn(&mut Job, &Args) -> Result<()>;

/// The steps by name, in the order the Python's dict has them (the built-in steps, `quality`,
/// then the plugins' in the order they were given).
pub fn builtin() -> Vec<(String, StepFn)> {
    vec![
        ("shaders".into(), shaders::run as StepFn),
        ("textures".into(), textures::run),
        ("settings".into(), |job, _| settings::run(job)),
        ("audio".into(), |job, _| audio::run(job)),
        ("quality".into(), |_, _| {
            Err(Error::NotPorted("the quality step".into()))
        }),
    ]
}

/// The steps a plugin file gives (`--plugin <path>`): the plugins pmu has built in, by the
/// file's name without .py (Cuphead's convert/animclips.py: `animclips`), so a setup can pass
/// the same path it gives the Python, or just the name.
pub fn plugin(path: &str) -> Result<Vec<(String, StepFn)>> {
    let name = std::path::Path::new(path)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    match name.strip_suffix(".py").unwrap_or(&name) {
        "animclips" => Ok(vec![(
            animclips::STEP.into(),
            (|job, _| animclips::run(job)) as StepFn,
        )]),
        _ => Err(Error::NotPorted(format!("the plugin {path}"))),
    }
}

/// `dict(DEFAULTS, **job.cfg.get(section, {}))`: a step's settings, the config's over the
/// defaults (the config's extra keys kept).
pub fn section(cfg: &Json, name: &str, defaults: &[(&str, Json)]) -> Result<Json> {
    let mut c = Json::Dict(
        defaults
            .iter()
            .map(|(k, v)| (k.to_string(), v.clone()))
            .collect(),
    );
    match cfg.get(name) {
        None => {}
        Some(Json::Dict(d)) => {
            for (k, v) in d {
                c.set(k, v.clone());
            }
        }
        Some(other) => {
            return Err(Error::Value(format!(
                "config {name}: not a dict ({})",
                other.repr()
            )))
        }
    }
    Ok(c)
}

/// `c[key]` of a step's settings.
pub fn opt<'a>(c: &'a Json, key: &str) -> &'a Json {
    c.get(key).unwrap_or(&Json::Null)
}

/// `t[name]` of an object's tree (KeyError when missing).
pub fn field<'a>(t: &'a Value, name: &str) -> Result<&'a Value> {
    t.get(name)
        .ok_or_else(|| Error::Value(format!("KeyError: '{name}'")))
}

pub fn field_mut<'a>(t: &'a mut Value, name: &str) -> Result<&'a mut Value> {
    t.get_mut(name)
        .ok_or_else(|| Error::Value(format!("KeyError: '{name}'")))
}

/// An integer field.
pub fn int(t: &Value, name: &str) -> Result<i128> {
    field(t, name)?
        .as_int()
        .ok_or_else(|| Error::Value(format!("{name}: not an integer")))
}

/// A number field as Python's float (a float32 widened exactly).
pub fn float(t: &Value, name: &str) -> Result<f64> {
    field(t, name)?
        .as_f64()
        .ok_or_else(|| Error::Value(format!("{name}: not a number")))
}

/// A field's text (UTF-8; the Python decodes with surrogateescape, which gives the same bytes).
pub fn text(t: &Value, name: &str) -> Result<Vec<u8>> {
    field(t, name)?
        .as_bytes()
        .map(<[u8]>::to_vec)
        .ok_or_else(|| Error::Value(format!("{name}: not a string")))
}

/// A config value as an object field's value (the type tree gives it its width when written).
pub fn json_value(v: &Json) -> Result<Value> {
    Ok(match v {
        Json::Null => return Err(Error::Value("None is not a field value".into())),
        Json::Bool(b) => Value::Bool(*b),
        Json::Int(i) => {
            Value::I64(i64::try_from(*i).map_err(|_| Error::Value(format!("{i}: out of range")))?)
        }
        Json::Float(f) => Value::F64(*f),
        Json::Str(s) => Value::str(s),
        Json::List(l) => Value::Array(l.iter().map(json_value).collect::<Result<_>>()?),
        Json::Dict(_) => return Err(Error::Value("a dict is not a field value".into())),
    })
}

/// The path ids of a file's objects with their class names, in the file's order (lf.objects()).
pub fn objects(f: &unityfs::DataFile) -> Vec<(i64, String)> {
    f.sf.objects
        .iter()
        .map(|o| (o.path_id, f.class_name(o)))
        .collect()
}
