//! The port config values the check suggests (port.py's suggest_config; `port new` starts from
//! them) with the notes that say why, and the [controls] draft from the game's default pad
//! bindings (asmpatch --survey).
use crate::pyx::*;
use crate::survey::{int_of, pinvoke_benign};
use port::val::{Dict, Val};
use regex::Regex;
use std::sync::OnceLock;

fn re(cell: &'static OnceLock<Regex>, pat: &str) -> &'static Regex {
    cell.get_or_init(|| Regex::new(pat).unwrap())
}

const DIRS: [&str; 4] = ["Up", "Down", "Left", "Right"];

/// InControl's controls as a handheld labels its buttons ([controls]): the face buttons by the
/// Nintendo layout most PortMaster handhelds have (A right, B bottom: the game's Action1 is the
/// bottom one)
fn pad_label(c: &str) -> Option<&'static str> {
    Some(match c {
        "Action1" => "B",
        "Action2" => "A",
        "Action3" => "Y",
        "Action4" => "X",
        "LeftBumper" => "L1",
        "RightBumper" => "R1",
        "LeftTrigger" => "L2",
        "RightTrigger" => "R2",
        "LeftStickButton" => "L3",
        "RightStickButton" => "R3",
        "Back" | "Select" | "View" | "Share" | "Minus" | "Create" => "Select",
        "Start" | "Menu" | "Options" | "Pause" | "Plus" => "Start",
        _ => return None,
    })
}

fn swap_label(b: &str) -> Option<&'static str> {
    Some(match b {
        "a" => "B",
        "b" => "A",
        "x" => "Y",
        "y" => "X",
        "l1" => "L1",
        "r1" => "R1",
        "l2" => "L2",
        "r2" => "R2",
        "select" => "Select",
        _ => return None,
    })
}

type Binds = Vec<(String, Vec<String>)>;

fn bind<'a>(b: &'a Binds, c: &str) -> &'a [String] {
    b.iter().find(|(k, _)| k == c).map_or(&[], |x| x.1.as_slice())
}

/// {control: [action, ...]} from asmpatch --survey's "every binding, by control" list, in its order.
pub fn pad_bindings(survey: &str) -> Binds {
    static R: OnceLock<Regex> = OnceLock::new();
    let mut out: Binds = Vec::new();
    let Some(m) = re(&R, r"(?m)^    every binding, by control:\n((?:      .*\n?)*)").captures(survey) else { return out };
    for line in m[1].lines() {
        let line = pkgzip::py::strip(line);
        let Some((c, acts)) = line.split_once(": ") else { continue };
        if c.is_empty() || acts.is_empty() {
            continue;
        }
        let acts: Vec<String> = acts.split(", ").map(String::from).collect();
        match out.iter_mut().find(|(k, _)| k == c) {
            Some(x) => x.1 = acts,
            None => out.push((c.to_string(), acts)),
        }
    }
    out
}

/// re.findall(r'[A-Z]+(?![a-z])|[A-Z]?[a-z0-9]+', n)
fn words(n: &str) -> Vec<String> {
    let c: Vec<char> = n.chars().collect();
    let up = |x: char| x.is_ascii_uppercase();
    let low = |x: char| x.is_ascii_lowercase();
    let lod = |x: char| x.is_ascii_lowercase() || x.is_ascii_digit();
    let (mut out, mut i) = (Vec::new(), 0);
    while i < c.len() {
        if up(c[i]) {
            let mut j = i;
            while j < c.len() && up(c[j]) {
                j += 1;
            }
            if j < c.len() && low(c[j]) {
                j -= 1;
            }
            if j > i {
                out.push(c[i..j].iter().collect());
                i = j;
                continue;
            }
        }
        let st = i;
        let mut j = if up(c[i]) { i + 1 } else { i };
        let body = j;
        while j < c.len() && lod(c[j]) {
            j += 1;
        }
        if j > body {
            out.push(c[st..j].iter().collect());
            i = j;
        } else {
            i += 1;
        }
    }
    out
}

/// GungeonActions.DodgeRollAction -> "Dodge roll" (strip_dir: a trailing Up/Down/Left/Right too).
pub fn action_words(action: &str, strip_dir: bool) -> String {
    let mut n = action.rsplit('.').next().unwrap_or("");
    if let Some(x) = n.strip_suffix("Action").filter(|x| !x.is_empty()) {
        n = x;
    }
    if strip_dir {
        if let Some(x) = DIRS.iter().find_map(|d| n.strip_suffix(d)) {
            n = x;
        }
    }
    let w = words(n);
    let Some(first) = w.first() else { return String::new() };
    let mut out = vec![first.clone()];
    for x in &w[1..] {
        let upper = x.chars().any(|c| c.is_ascii_uppercase()) && !x.chars().any(|c| c.is_ascii_lowercase());
        out.push(if !upper || x.chars().count() == 1 { x.to_lowercase() } else { x.clone() });
    }
    out.join(" ")
}

/// What a d-pad or stick does: an action in all four directions once (no direction: "Move"),
/// others with theirs, e.g. "Move, Select, Drop item (up)".
fn direction_row(binds: &Binds, prefix: &str) -> String {
    let mut acts: Vec<(String, Vec<String>)> = Vec::new();
    for d in DIRS {
        for a in bind(binds, &format!("{prefix}{d}")) {
            let mut base = action_words(a, a.ends_with(d) || a.ends_with(&format!("{d}Action")));
            if base.is_empty() {
                base = "Move".into();
            }
            if !acts.iter().any(|(k, _)| *k == base) {
                acts.push((base.clone(), Vec::new()));
            }
            let ds = &mut acts.iter_mut().find(|(k, _)| *k == base).unwrap().1;
            if !ds.contains(&d.to_lowercase()) {
                ds.push(d.to_lowercase());
            }
        }
    }
    acts.iter().map(|(b, ds)| if ds.len() == 4 { b.clone() } else { format!("{b} ({})", ds.join(", ")) }).collect::<Vec<_>>().join(", ")
}

fn is_right_stick(c: &str) -> bool {
    c.starts_with("RightStick") && c != "RightStickButton"
}

/// Whether every right-stick action has another binding.
fn right_elsewhere(binds: &Binds) -> bool {
    let other: Vec<&String> = binds.iter().filter(|(c, _)| !is_right_stick(c)).flat_map(|x| x.1.iter()).collect();
    binds.iter().filter(|(c, _)| is_right_stick(c)).flat_map(|x| x.1.iter()).all(|a| other.contains(&a))
}

fn dedup(xs: Vec<String>) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for x in xs {
        if !out.contains(&x) {
            out.push(x);
        }
    }
    out
}

/// port.toml's [controls] drafted from the game's default pad bindings.
pub fn controls_draft(binds: &Binds, dpad_stick: bool, swap: Option<&str>) -> Dict {
    let mut rows = Dict::new();
    let (left, right, dpad) = (direction_row(binds, "LeftStick"), direction_row(binds, "RightStick"), direction_row(binds, "DPad"));
    if !left.is_empty() {
        rows.set("Left Stick", s(&left));
    }
    if !right.is_empty() {
        rows.set("Right Stick", s(format!("{right}{}", if right_elsewhere(binds) { "" } else { "; handhelds without a right stick cannot" })));
    }
    if dpad_stick {
        let sl = swap.and_then(swap_label).unwrap_or("");
        rows.set("D-Pad", s(format!("the left stick on handhelds without sticks{}", if !sl.is_empty() && !dpad.is_empty() {
            format!("; {sl} gives the d-pad back: {dpad}")
        } else if !dpad.is_empty() {
            format!(": {dpad}")
        } else {
            String::new()
        })));
    } else if !dpad.is_empty() {
        rows.set("D-Pad", s(&dpad));
    }
    for (c, acts) in binds {
        if let Some(lab) = pad_label(c) {
            let mut had: Vec<String> = rows.str(lab).split(", ").filter(|w| !w.is_empty()).map(String::from).collect();
            had.extend(acts.iter().map(|a| action_words(a, false)));
            rows.set(lab, s(dedup(had).join(", ")));
        }
    }
    if let Some(l) = swap.and_then(swap_label).filter(|_| dpad_stick) {
        rows.set(l, s("the d-pad as the d-pad, again as the left stick (handhelds without sticks; the game no longer gets this button)"));
    }
    rows
}

fn steps_with(cv: &mut Dict, step: &str) {
    if !cv.contains("steps") {
        cv.set("steps", strs(&port::config::DEFAULT_STEPS));
    }
    if !cv.list("steps").iter().any(|x| x.as_str() == Some(step)) {
        push(cv, "steps", s(step));
    }
}

/// Port config values the check suggests (rep['suggest']), and the notes and warnings with them.
pub fn suggest_config(rep: &mut Dict) {
    let sv = rep.dict("code").clone();
    let d = rep.dict("data_survey").clone();
    let mut sug = rep.dict("suggest").clone();
    sug.table("runtime");
    sug.table("android_data");
    sug.table("convert");
    let (mut notes, mut warnings): (Vec<String>, Vec<String>) = Default::default();
    let survey = rep.str("asmpatch_survey").to_string();
    let src = rep.str("shader_source").to_string();
    if ["d3d11", "vulkan", "metal"].contains(&src.as_str()) {
        sug.table("convert").table("shaders").set("from", strs(&[&src]));
    }
    let setdefault = |sug: &mut Dict, k: &str, v: &str| {
        if !sug.table("runtime").contains(k) {
            sug.table("runtime").set(k, s(v));
        }
    };
    if sv.truthy("rewired") {
        setdefault(&mut sug, "UA_PAD_NAME", "Xbox Wireless Controller");
        notes.push(format!("Rewired ({}): asmpatch's rewired-android patch (Rewired then reads the pad through Unity's Android \
                            input); UA_PAD_NAME names the pad as one Rewired's Android definitions know", sv.str("rewired")));
    }
    if sv.truthy("incontrol") {
        static R: OnceLock<Regex> = OnceLock::new();
        let name = re(&R, r#"-> UA_PAD_NAME="([^"]+)""#).captures(&survey).map_or("Microsoft X-Box 360 pad".to_string(), |m| m[1].to_string());
        setdefault(&mut sug, "UA_PAD_NAME", &name);
        notes.push("InControl: it picks its Android profiles at run time by the pad's name; UA_PAD_NAME names the pad as one of \
                    them knows (asmpatch --survey lists them), else the pad is an unknown controller (docs/input.md)".into());
    }
    // asmpatch --survey's pad bindings (InControl's default bindings)
    let binds = pad_bindings(&survey);
    static DS: OnceLock<Regex> = OnceLock::new();
    static OWN: OnceLock<Regex> = OnceLock::new();
    static SW: OnceLock<Regex> = OnceLock::new();
    static RS: OnceLock<Regex> = OnceLock::new();
    let ds = re(&DS, r"the left stick alone \(no d-pad binding\): (.*)\n\s*-> UA_DPAD_STICK=([\w,]+)").captures(&survey);
    let mut swap: Option<String> = None;
    let squeeze = |x: &str| x.split_whitespace().collect::<Vec<_>>().join(" ");
    if let Some(ds) = &ds {
        setdefault(&mut sug, "UA_DPAD_STICK", &ds[2]);
        let own = re(&OWN, r"(?s)its own bindings (.*?) need a UA_DPAD_SWAP").captures(&survey);
        let sw = re(&SW, r"-> UA_DPAD_SWAP=(\w+) \(([^)]*)\)").captures(&survey);
        let mut why = String::new();
        if let (Some(_), Some(sw)) = (&own, &sw) {
            why = squeeze(&sw[2]);
            // (a button the game binds nothing to, or whose actions other buttons also do: nothing is lost)
            if why.contains("binds nothing") || why.contains("other buttons also do") {
                swap = Some(sw[1].to_string());
                setdefault(&mut sug, "UA_DPAD_SWAP", &sw[1]);
            }
        }
        let tail = match &own {
            Some(own) => format!(
                "; the d-pad's own bindings ({}) then need UA_DPAD_SWAP = \"<button>\", a button that gives the d-pad back{}",
                squeeze(&own[1]),
                match &sw {
                    Some(sw) => format!(": UA_DPAD_SWAP = \"{}\" ({why}){}", &sw[1],
                                        if swap.is_some() { "" } else { "; not suggested, as it loses those: choose the button yourself" }),
                    None => String::new(),
                }
            ),
            None => "; the d-pad also still reports as the d-pad (every d-pad binding is also a left stick one)".to_string(),
        };
        notes.push(format!("pad bindings: {} only on the left stick, which a handheld without sticks lacks: UA_DPAD_STICK = \"{}\" \
                            (the d-pad drives the left stick on pads without one; docs/input.md){tail}", pkgzip::py::strip(&ds[1]), &ds[2]));
    }
    if let Some(rs) = re(&RS, r"the right stick alone \(no other binding\): (.*)\n").captures(&survey) {
        let what = dedup(rs[1].split(", ").map(|a| {
            let w = action_words(a, true);
            if w.is_empty() { "it".to_string() } else { w }
        }).collect()).join(", ");
        notes.push(format!("pad bindings: {} only on the right stick: a handheld without one (the RG34XX-SP and other d-pad-only \
                            ones) cannot do those ({what} needs a right stick; nothing in the port stands in for it). Check the \
                            game's own options on such a handheld (aim assist, aiming where it moves, another control scheme), or \
                            pointer mode where it aims at the mouse; the [controls] draft says so", pkgzip::py::strip(&rs[1])));
    }
    if !binds.is_empty() {
        sug.set("controls", Val::Dict(controls_draft(&binds, ds.is_some(), swap.as_deref())));
    }
    if sv.truthy("playmaker") {
        notes.push(format!("PlayMaker ({}): asmpatch's optimizations playmaker-loopcount and playmaker-delayedevents (the same \
                            behaviour, less work every frame for every FSM: tools/asmpatch/README.md, \"Optimizations\")", sv.str("playmaker")));
    }
    sug.table("android_data").set("asmpatch", s(if sv.truthy("rewired") || sv.truthy("input_system") || sv.truthy("playmaker") { "auto" } else { "off" }));
    let verdict = rep.str("input_verdict").to_string();
    // asmpatch --survey: the Input System's Mouse.current used with no null check
    static UNCHECKED: OnceLock<Regex> = OnceLock::new();
    let unchecked = re(&UNCHECKED, r"used with no null check in (\d+) methods( \([^)]*\))?").captures(&survey);
    if let Some(u) = unchecked.as_ref().filter(|_| sug.dict("runtime").str("UA_POINTER") != "touch") {
        sug.table("runtime").set("UA_POINTER", s("1"));
        notes.push(format!("the code uses the Input System's Mouse.current with no null check{}: a handheld has no mouse unless \
                            pointer mode presents one, else those throw NullReferenceException (every frame from an Update): \
                            UA_POINTER=1; if the game draws its own cursor, also UA_POINTER_CURSOR=0", u.get(2).map_or("", |m| m.as_str())));
    } else if verdict == "pointer" {
        sug.table("runtime").set("UA_POINTER", s("1"));
        notes.push("menus look mouse-only (the code reads the mouse and selects no UI element for pad navigation): pointer mode, \
                    UA_POINTER=1; if the game draws its own cursor, also UA_POINTER_CURSOR=0".into());
    } else if verdict == "mixed" {
        notes.push("reads the mouse and also selects UI elements: try the pad first; UA_POINTER=1 (with UA_POINTER_TOGGLE) where \
                    menus need the mouse".into());
    }
    if sv.dict("legacy").truthy("keys") {
        notes.push(format!("reads keyboard keys (Input.GetKey, {} references): map pad buttons with UA_KEYMAP (e.g. start:escape) \
                            where the pad has no binding", int(sv.dict("legacy"), "keys")));
    }
    let active = d.str("active_input");
    if sv.truthy("input_system") || active == "Input System package" || active == "both" {
        notes.push(format!("Input System package ({}): its desktop build has no layout for the Android player's pads: asmpatch's \
                            inputsystem-android patch registers them (docs/input.md)", if active.is_empty() { "Unity.InputSystem.dll" } else { active }));
    }
    let scripts: Vec<&str> = d.dict("scripts_used").keys().collect();
    if sv.truthy("text_input") || scripts.iter().any(|k| k.ends_with("InputField")) {
        let mut all = str_list(&sv, "text_input");
        all.extend(scripts.iter().filter(|k| k.ends_with("InputField") || k.ends_with("TextField")).map(|k| format!("{k} in scenes")));
        notes.push(format!("text input ({}): Unity's soft keyboard opens ua_host's on-screen keyboard (the pad types); UA_OSK = \
                            \"done\" in runtime answers it at once where text does not matter (docs/input.md)", all.join(", ")));
    }
    if d.str("color_space") == "linear" {
        notes.push("linear colour space: with the SDL display, UA_DISPLAY_SRGB=1 (docs/runtime.md)".into());
    }
    let has_video = |x: &str| sv.list("video").iter().any(|v| v.as_str() == Some(x));
    if d.truthy("video_clips") || d.truthy("video_players") {
        notes.push(format!("video: {} VideoPlayers in the main data, clips {}: android-data adds Hidden/VideoDecodeAndroid (keep \
                            android_data.video_shader on); VP8/VP9 and H.264 decode built in, AAC/HEVC/MP3 only with the system's \
                            libavcodec (else silent)", d.get("video_players").map_or("0".into(), Val::py_str),
                           if d.truthy("video_clips") { d.get("video_clips").unwrap().repr() } else { "none there".into() }));
    } else if has_video("VideoPlayer") {
        notes.push("the code uses VideoPlayer, but the main data has no clips or players (videos from bundles or URLs, or none \
                    at all): keep android_data.video_shader on".into());
    }
    if d.truthy("movie_textures") || has_video("MovieTexture") {
        warnings.push("MovieTexture (the legacy movie player): Unity's Android player has none; those videos do not play".into());
    }
    let pl: Vec<String> = rep.list("plugins").iter().filter_map(Val::as_list).filter(|p| p.len() == 2)
        .map(|p| format!("{}: {}", basename(&p[0].py_str()), p[1].py_str())).collect();
    if !pl.is_empty() {
        notes.push(format!("native plugins: {}", pl.join("; ")));
    }
    let pinv: Vec<String> = sv.dict("pinvoke").iter().filter(|(m, _)| !pinvoke_benign(m))
        .map(|(m, a)| format!("{m} ({})", a.as_list().unwrap_or(&[]).iter().map(Val::py_str).collect::<Vec<_>>().join(", "))).collect();
    if !pinv.is_empty() {
        notes.push(format!("P/Invoke into {}: each needs an ARM library or a stub", pinv.join(", ")));
    }
    if sv.truthy("services") {
        notes.push(format!("services: {}", str_list(&sv, "services").join(", ")));
    }
    // textures: the main data's and the StreamingAssets bundles'
    let st = rep.dict("streaming_survey").clone();
    let mut tex: Vec<(String, i64, i64)> = Vec::new();
    for src in [d.dict("textures"), st.dict("textures")] {
        for (k, v) in src.iter() {
            let Some(l) = v.as_list().filter(|l| l.len() == 2) else { continue };
            match tex.iter_mut().find(|t| t.0 == k) {
                Some(t) => {
                    t.1 += int_of(&l[0]);
                    t.2 += int_of(&l[1]);
                }
                None => tex.push((k.to_string(), int_of(&l[0]), int_of(&l[1]))),
            }
        }
    }
    let place = if st.truthy("textures") { "main data and StreamingAssets bundles" } else { "main data" };
    let bc: i64 = tex.iter().filter(|t| t.0.starts_with("DXT") || t.0.starts_with("BC")).map(|t| t.2).sum();
    let crunched: i64 = tex.iter().filter(|t| t.0.ends_with("Crunched")).map(|t| t.1).sum();
    if bc > 64 << 20 {
        notes.push(format!("{:.0} MB of BC/DXT textures ({place}): the handheld GPU cannot sample them, so Unity decodes them to \
                            RGBA when loading (4-8x the memory). The textures step (convert.steps += \"textures\") re-encodes \
                            them as ASTC: less memory, slightly different pixels, a longer setup; off by default", bc as f64 / 1048576.0));
    }
    // uncompressed textures stay as they are in the GPU's memory
    let raw: Vec<&(String, i64, i64)> = tex.iter().filter(|t| ["RGBA32", "ARGB32", "BGRA32", "RGB24"].contains(&t.0.as_str())).collect();
    let raw_mb = raw.iter().map(|t| t.2).sum::<i64>() as f64 / 1048576.0;
    if raw_mb > 256.0 {
        warnings.push(format!(
            "{raw_mb:.0} MB of uncompressed textures ({}; {place}): on a handheld with 1 GB of RAM the game, the system and these \
             share it, and the scenes that load many of them may not fit. The textures step (convert.steps += \"textures\"; \
             docs/porting.md, \"Small handhelds (1 GB)\") re-encodes them as ASTC 4x4, a quarter of RGBA32's memory (slightly \
             different pixels, a longer setup); try the port on a 1 GB handheld first, watching log.txt's memory",
            raw.iter().map(|t| format!("{} {}", t.0, t.1)).collect::<Vec<_>>().join(", ")));
    }
    if crunched != 0 {
        notes.push(format!("{crunched} crunched textures: as ASTC (the textures step) they take their full size on the card \
                            (several times the crunched size)"));
    }
    let decoded = float(&d, "decoded_mb") + float(&st, "decoded_mb");
    if decoded > 128.0 {
        steps_with(sug.table("convert"), "textures");
        notes.push(format!("BC/DXT textures decode to about {decoded:.0} MB of RGBA on the handheld (its GPU cannot sample them): \
                            more than a 1 GB device can hold next to the game. The textures step is suggested (convert.steps), \
                            re-encoding them as ASTC at the GPU's own size; remove it only for a device with the memory to spare"));
    }
    if rep.list("asmpatch_would").iter().any(|x| x.as_str() == Some("blasphemous-catalogs")) {
        steps_with(sug.table("convert"), "fmodcatalogs");
        let ad = sug.table("android_data");
        let mut a: Vec<Val> = match ad.get("asmpatch") {
            Some(Val::List(l)) => l.clone(),
            Some(Val::Str(x)) if x == "auto" => vec![s("auto")],
            _ => Vec::new(),
        };
        a.push(s("blasphemous-catalogs"));
        ad.set("asmpatch", Val::List(a));
        ad.set("data_path", Val::Bool(true));
        notes.push("Blasphemous loads every enemy's sound catalog at startup, and each catalog references its enemy (all their \
                    sprite sheets: more than a 1 GB handheld has): the fmodcatalogs step (convert.steps) lists them and \
                    asmpatch's blasphemous-catalogs loads each level's own when it needs them (android_data.asmpatch, data_path \
                    = true for the list in Game_Data/)".into());
    }
    let size = int(rep, "size_mb");
    if size > 600 {
        notes.push(format!("{size} MB: the handheld's setup needs about 2-3 times that free while it runs"));
    }
    rep.set("suggest", Val::Dict(sug));
    // (the Python appends to the report's lists as it goes: notes and warnings are separate lists)
    for n in notes {
        push(rep, "notes", s(n));
    }
    for w in warnings {
        push(rep, "warnings", s(w));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn action_names() {
        assert_eq!(action_words("GungeonActions.DodgeRollAction", false), "Dodge roll");
        assert_eq!(action_words("SelectUp", true), "Select");
        assert_eq!(action_words("UseUIItem2", false), "Use UI item2");
        assert_eq!(action_words("A", false), "A");
        assert_eq!(action_words("MoveXAxis", false), "Move x axis");
        assert_eq!(words("HTTPServer_x"), ["HTTP", "Server", "x"]);
        assert_eq!(action_words("Action", false), "Action");
    }
}
