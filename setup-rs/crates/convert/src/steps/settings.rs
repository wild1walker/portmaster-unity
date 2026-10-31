//! Step `settings` (settings.py): player settings in globalgamemanagers (cfg['settings']).
//!
//!   graphics_apis       BuildSettings.m_GraphicsAPIs, e.g. [11] (GLES3) or [11, 17]; None = keep
//!   mouse_events        False: BuildSettings.usesOnMouseEvents off (no OnMouse* raycasts)
//!   trim_joystick_axes  True: InputManager axes named Joy<j>Axis<a> dropped
//!   audio_sample_rate   AudioManager output rate (0 = keep); real_voices: its real voices
use super::{field, field_mut, json_value, objects, opt, section};
use crate::error::{Error, Result};
use crate::job::Job;
use crate::pyjson::Json;
use unityfs::Value;

fn defaults() -> Vec<(&'static str, Json)> {
    vec![
        ("graphics_apis", Json::Null),
        ("mouse_events", Json::Bool(true)),
        ("trim_joystick_axes", Json::Bool(false)),
        ("audio_sample_rate", Json::Int(0)),
        ("real_voices", Json::Int(0)),
    ]
}

/// `re.match(r'Joy\d+Axis\d+$', name)`.
fn joystick_axis(name: &[u8]) -> bool {
    static RX: std::sync::OnceLock<regex::Regex> = std::sync::OnceLock::new();
    let rx = RX.get_or_init(|| regex::Regex::new(r"^Joy\d+Axis\d+\n?\z").unwrap());
    rx.is_match(&String::from_utf8_lossy(name))
}

pub fn run(job: &mut Job) -> Result<()> {
    let rel = "globalgamemanagers";
    if job.is_done("settings", rel) {
        return Ok(());
    }
    let c = section(&job.cfg, "settings", &defaults())?;
    let mut f = job.open(&job.path_in(rel), Default::default())?;
    let mut done: Vec<String> = Vec::new();
    for (pid, class) in objects(&f) {
        match class.as_str() {
            "BuildSettings" => {
                let mut t = f.sf.read_typetree(pid)?;
                let apis = opt(&c, "graphics_apis");
                if !matches!(apis, Json::Null) {
                    let list = match apis {
                        Json::List(_) => json_value(apis)?,
                        other => {
                            return Err(Error::Value(format!(
                                "settings.graphics_apis: not a list ({})",
                                other.repr()
                            )))
                        }
                    };
                    t.set("m_GraphicsAPIs", list)?;
                    done.push(format!("graphics APIs {}", apis.to_str()));
                }
                if !opt(&c, "mouse_events").truthy()
                    && t.get("usesOnMouseEvents")
                        .and_then(Value::as_bool)
                        .unwrap_or(false)
                {
                    t.set("usesOnMouseEvents", Value::Bool(false))?;
                    done.push("mouse events off".into());
                }
                f.sf.save_typetree(pid, &t)?;
            }
            "InputManager" if opt(&c, "trim_joystick_axes").truthy() => {
                let mut t = f.sf.read_typetree(pid)?;
                let axes = field(&t, "m_Axes")?
                    .as_array()
                    .ok_or_else(|| Error::Value("m_Axes: not a list".into()))?;
                let mut kept = Vec::new();
                for a in axes {
                    if !joystick_axis(&super::text(a, "m_Name")?) {
                        kept.push(a.clone());
                    }
                }
                if kept.len() != axes.len() {
                    done.push(format!("input axes {} -> {}", axes.len(), kept.len()));
                    *field_mut(&mut t, "m_Axes")? = Value::Array(kept);
                    f.sf.save_typetree(pid, &t)?;
                }
            }
            "AudioManager" if opt(&c, "audio_sample_rate").truthy() => {
                let mut t = f.sf.read_typetree(pid)?;
                let rate = opt(&c, "audio_sample_rate");
                t.set("m_SampleRate", json_value(rate)?)?;
                let voices = opt(&c, "real_voices");
                if voices.truthy() {
                    t.set("m_RealVoiceCount", json_value(voices)?)?;
                }
                f.sf.save_typetree(pid, &t)?;
                done.push(format!("audio {} Hz", rate.to_str()));
            }
            _ => {}
        }
    }
    job.write_with(rel, |tmp| Ok(f.write_to(tmp, false)?))?;
    drop(f);
    job.mark_done("settings", rel)?;
    let what = if done.is_empty() {
        "nothing to change".to_string()
    } else {
        done.join(", ")
    };
    job.log(&format!("settings: {what}"));
    Ok(())
}
