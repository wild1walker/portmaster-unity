//! android-data's command line, read as its argparse parser reads it: long options anywhere,
//! `--opt=value`, unique prefixes, appended and two-value options, int and choice values;
//! a wrong argument stops with argparse's message (exit 2).
use std::collections::HashMap;

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Value,
    Int,
    Choice(&'static [&'static str]),
    True,
    False,
    Append,
    Append2,
    Help,
}

struct Opt {
    flag: &'static str,
    dest: &'static str,
    kind: Kind,
}

const fn o(flag: &'static str, dest: &'static str, kind: Kind, _metavar: &'static str) -> Opt {
    Opt { flag, dest, kind }
}

const OPTS: &[Opt] = &[
    o("--help", "help", Kind::Help, ""),
    o("--src", "src", Kind::Value, "SRC"),
    o("--pkg", "pkg", Kind::Value, "PKG"),
    o("--out", "out", Kind::Value, "OUT"),
    o("--name", "name", Kind::Value, "NAME"),
    o(
        "--platform-patch",
        "platform_patch",
        Kind::Choice(&["data", "none"]),
        "",
    ),
    o(
        "--graphics-apis",
        "graphics_apis",
        Kind::Value,
        "GRAPHICS_APIS",
    ),
    o(
        "--mt-rendering",
        "mt_rendering",
        Kind::Choice(&["keep", "on", "off"]),
        "",
    ),
    o(
        "--blit",
        "blit",
        Kind::Choice(&["keep", "always", "never", "auto"]),
        "",
    ),
    o(
        "--swappy",
        "swappy",
        Kind::Choice(&["keep", "off", "on"]),
        "",
    ),
    o("--any-unity-version", "any_unity_version", Kind::True, ""),
    o(
        "--data-version",
        "data_version",
        Kind::Choice(&["player", "keep"]),
        "",
    ),
    o(
        "--async-upload-buffer",
        "async_upload_buffer",
        Kind::Int,
        "ASYNC_UPLOAD_BUFFER",
    ),
    o("--no-inline-textures", "inline_textures", Kind::False, ""),
    o("--no-video-shader", "video_shader", Kind::False, ""),
    o(
        "--rewired-joysticks",
        "rewired_joysticks",
        Kind::Int,
        "REWIRED_JOYSTICKS",
    ),
    o("--managed-file", "managed_file", Kind::Append, "DLL"),
    o(
        "--assembly-patch",
        "assembly_patch",
        Kind::Append2,
        "NAME COMMAND",
    ),
    o(
        "--drop-missing-assembly",
        "drop_missing_assembly",
        Kind::Append,
        "DLL",
    ),
    o(
        "--game-managed",
        "game_managed",
        Kind::Value,
        "GAME_MANAGED",
    ),
    o("--asmpatch", "asmpatch", Kind::Value, "COMMAND"),
    o("--root-file", "root_file", Kind::Append, "NAME"),
    o("--mono-etc", "mono_etc", Kind::Value, "DIR"),
    o("--dllmap", "dllmap", Kind::Append, "DLL=TARGET"),
    o("--plugin-map", "plugin_map", Kind::Value, "JSON"),
    o("--loose-assets", "loose_assets", Kind::Append, "GLOB"),
    o(
        "--streaming-assets",
        "streaming_assets",
        Kind::Choice(&["obb", "both", "loose"]),
        "",
    ),
    o(
        "--min-gles",
        "min_gles",
        Kind::Choice(&["auto", "keep", "3.0", "3.1", "3.2"]),
        "",
    ),
    o("--min-sdk", "min_sdk", Kind::Value, "API"),
    o(
        "--realtime-gi",
        "realtime_gi",
        Kind::Choice(&["neon", "as-is"]),
        "",
    ),
    o("--data-path", "data_path", Kind::True, ""),
    o("--beside", "beside", Kind::Append, "GLOB"),
    o("--mono64", "mono64", Kind::True, ""),
    o("--il2cpp", "il2cpp", Kind::Value, "DIR"),
    o("--force", "force", Kind::True, ""),
    o("--consume-src", "consume_src", Kind::True, ""),
    o(
        "--verify-bundles",
        "verify_bundles",
        Kind::Int,
        "VERIFY_BUNDLES",
    ),
    o("--no-verify", "no_verify", Kind::True, ""),
    o(
        "--pack",
        "pack",
        Kind::Choice(&["none", "apk", "obb", "all"]),
        "",
    ),
    o("--package", "package", Kind::Value, "PACKAGE"),
    o("--version-code", "version_code", Kind::Int, "VERSION_CODE"),
    o("--zip-dir", "zip_dir", Kind::Value, "ZIP_DIR"),
];

/// The parsed options (argparse's namespace).
#[derive(Clone, Debug)]
pub struct Args {
    pub src: String,
    pub pkg: Option<String>,
    pub out: String,
    pub name: Option<String>,
    pub platform_patch: String,
    pub graphics_apis: String,
    pub mt_rendering: String,
    pub blit: String,
    pub swappy: String,
    pub any_unity_version: bool,
    pub data_version: String,
    pub async_upload_buffer: i64,
    pub inline_textures: bool,
    pub video_shader: bool,
    pub rewired_joysticks: i64,
    pub managed_file: Vec<String>,
    pub assembly_patch: Vec<(String, String)>,
    pub drop_missing_assembly: Vec<String>,
    pub game_managed: Option<String>,
    pub asmpatch: Option<String>,
    pub root_file: Vec<String>,
    pub mono_etc: Option<String>,
    pub dllmap: Vec<String>,
    pub plugin_map: Option<String>,
    pub loose_assets: Option<Vec<String>>,
    pub streaming_assets: String,
    pub min_gles: String,
    pub min_sdk: String,
    pub realtime_gi: String,
    pub data_path: bool,
    pub beside: Option<Vec<String>>,
    pub mono64: bool,
    pub il2cpp: Option<String>,
    pub force: bool,
    pub consume_src: bool,
    pub verify_bundles: i64,
    pub no_verify: bool,
    pub pack: String,
    pub package: Option<String>,
    pub version_code: i64,
    pub zip_dir: Option<String>,
}

pub const USAGE: &str =
    "usage: pmunity android-data [-h] --src SRC [--pkg PKG] [--out OUT] [--name NAME]
                             [--platform-patch {data,none}]
                             [--graphics-apis GRAPHICS_APIS]
                             [--mt-rendering {keep,on,off}]
                             [--blit {keep,always,never,auto}]
                             [--swappy {keep,off,on}] [--any-unity-version]
                             [--data-version {player,keep}]
                             [--async-upload-buffer ASYNC_UPLOAD_BUFFER]
                             [--no-inline-textures] [--no-video-shader]
                             [--rewired-joysticks REWIRED_JOYSTICKS]
                             [--managed-file DLL] [--assembly-patch NAME COMMAND]
                             [--drop-missing-assembly DLL]
                             [--game-managed GAME_MANAGED] [--asmpatch COMMAND]
                             [--root-file NAME] [--mono-etc DIR]
                             [--dllmap DLL=TARGET] [--plugin-map JSON]
                             [--loose-assets GLOB]
                             [--streaming-assets {obb,both,loose}]
                             [--min-gles {auto,keep,3.0,3.1,3.2}] [--min-sdk API]
                             [--realtime-gi {neon,as-is}] [--data-path]
                             [--beside GLOB] [--mono64] [--il2cpp DIR] [--force]
                             [--consume-src] [--verify-bundles VERIFY_BUNDLES]
                             [--no-verify] [--pack {none,apk,obb,all}]
                             [--package PACKAGE] [--version-code VERSION_CODE]
                             [--zip-dir ZIP_DIR]";

/// A command line error: argparse's usage and message, exit 2 (Err), or the help (Ok(None)).
pub fn parse(argv: &[String]) -> Result<Option<Args>, String> {
    let err = |m: String| Err(format!("{USAGE}\npmunity android-data: error: {m}"));
    let mut vals: HashMap<&str, Vec<Vec<String>>> = HashMap::new();
    let is_option_like = |a: &str| -> bool {
        // argparse's _parse_optional: what is not a value
        if !a.starts_with('-') || a.len() == 1 {
            return false;
        }
        if OPTS.iter().any(|o| o.flag == a || a == "-h") {
            return true;
        }
        if let Some((k, _)) = a.split_once('=') {
            if OPTS.iter().any(|o| o.flag == k) {
                return true;
            }
        }
        if a.starts_with("--")
            && OPTS
                .iter()
                .any(|o| o.flag.starts_with(a.split('=').next().unwrap()))
        {
            return true;
        }
        let neg = a[1..].parse::<f64>().is_ok() && !a.contains(' ');
        if neg {
            return false;
        }
        !a.contains(' ')
    };
    let mut i = 0;
    let mut extra: Vec<String> = Vec::new();
    while i < argv.len() {
        let a = &argv[i];
        i += 1;
        if a == "--" {
            extra.extend(argv[i..].iter().cloned());
            break;
        }
        if !is_option_like(a) {
            extra.push(a.clone());
            continue;
        }
        let (flag, inline) = match a.split_once('=') {
            Some((k, v)) if a.starts_with("--") => (k.to_string(), Some(v.to_string())),
            _ => (a.clone(), None),
        };
        let opt = if flag == "-h" {
            &OPTS[0]
        } else if let Some(o) = OPTS.iter().find(|o| o.flag == flag) {
            o
        } else {
            let hits: Vec<&Opt> = OPTS.iter().filter(|o| o.flag.starts_with(&flag)).collect();
            match hits.as_slice() {
                [o] => *o,
                [] => {
                    extra.push(a.clone());
                    continue;
                }
                many => {
                    let names: Vec<&str> = many.iter().map(|o| o.flag).collect();
                    return err(format!(
                        "ambiguous option: {flag} could match {}",
                        names.join(", ")
                    ));
                }
            }
        };
        let name = opt.flag;
        let nargs = match opt.kind {
            Kind::True | Kind::False | Kind::Help => 0,
            Kind::Append2 => 2,
            _ => 1,
        };
        let mut got: Vec<String> = Vec::new();
        if nargs == 0 {
            if inline.is_some() {
                return err(format!(
                    "argument {name}: ignored explicit argument '{}'",
                    inline.unwrap()
                ));
            }
        } else if let Some(v) = inline {
            if nargs != 1 {
                return err(format!("argument {name}: expected {nargs} arguments"));
            }
            got.push(v);
        } else {
            while got.len() < nargs
                && i < argv.len()
                && !is_option_like(&argv[i])
                && argv[i] != "--"
            {
                got.push(argv[i].clone());
                i += 1;
            }
            if got.len() < nargs {
                return err(if nargs == 1 {
                    format!("argument {name}: expected one argument")
                } else {
                    format!("argument {name}: expected {nargs} arguments")
                });
            }
        }
        match opt.kind {
            Kind::Help => return Ok(None),
            Kind::Int => {
                if pkgzip::py::int(&got[0], 10).is_none() {
                    return err(format!(
                        "argument {name}: invalid int value: {}",
                        pkgzip::py::str_repr(&got[0])
                    ));
                }
            }
            Kind::Choice(cs) => {
                if !cs.contains(&got[0].as_str()) {
                    let list: Vec<String> = cs.iter().map(|c| pkgzip::py::str_repr(c)).collect();
                    return err(format!(
                        "argument {name}: invalid choice: {} (choose from {})",
                        pkgzip::py::str_repr(&got[0]),
                        list.join(", ")
                    ));
                }
            }
            _ => {}
        }
        vals.entry(opt.dest).or_default().push(got);
    }
    if !extra.is_empty() {
        return err(format!("unrecognized arguments: {}", extra.join(" ")));
    }
    if !vals.contains_key("src") {
        return err("the following arguments are required: --src".into());
    }
    let last = |d: &str| vals.get(d).and_then(|v| v.last()).map(|v| v[0].clone());
    let flag = |d: &str| vals.contains_key(d);
    let list = |d: &str| -> Vec<String> {
        vals.get(d)
            .map(|v| v.iter().map(|x| x[0].clone()).collect())
            .unwrap_or_default()
    };
    let int = |d: &str, def: i64| last(d).map_or(def, |v| pkgzip::py::int(&v, 10).unwrap() as i64);
    let s = |d: &str, def: &str| last(d).unwrap_or_else(|| def.to_string());
    Ok(Some(Args {
        src: last("src").unwrap(),
        pkg: last("pkg").or_else(|| std::env::var("PMUNITY_ANDROID_PLAYER").ok()),
        out: s("out", "android-out"),
        name: last("name"),
        platform_patch: s("platform_patch", "data"),
        graphics_apis: s("graphics_apis", "11"),
        mt_rendering: s("mt_rendering", "keep"),
        blit: s("blit", "never"),
        swappy: s("swappy", "off"),
        any_unity_version: flag("any_unity_version"),
        data_version: s("data_version", "player"),
        async_upload_buffer: int("async_upload_buffer", 0),
        inline_textures: !flag("inline_textures"),
        video_shader: !flag("video_shader"),
        rewired_joysticks: int("rewired_joysticks", 4),
        managed_file: list("managed_file"),
        assembly_patch: vals
            .get("assembly_patch")
            .map(|v| v.iter().map(|x| (x[0].clone(), x[1].clone())).collect())
            .unwrap_or_default(),
        drop_missing_assembly: list("drop_missing_assembly"),
        game_managed: last("game_managed"),
        asmpatch: last("asmpatch"),
        root_file: list("root_file"),
        mono_etc: last("mono_etc"),
        dllmap: list("dllmap"),
        plugin_map: last("plugin_map"),
        loose_assets: vals
            .contains_key("loose_assets")
            .then(|| list("loose_assets")),
        streaming_assets: s("streaming_assets", "obb"),
        min_gles: s("min_gles", "auto"),
        min_sdk: s("min_sdk", "auto"),
        realtime_gi: s("realtime_gi", "neon"),
        data_path: flag("data_path"),
        beside: vals.contains_key("beside").then(|| list("beside")),
        mono64: flag("mono64"),
        il2cpp: last("il2cpp"),
        force: flag("force"),
        consume_src: flag("consume_src"),
        verify_bundles: int("verify_bundles", 6),
        no_verify: flag("no_verify"),
        pack: s("pack", "none"),
        package: last("package"),
        version_code: int("version_code", 1),
        zip_dir: last("zip_dir"),
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(a: &[&str]) -> Vec<String> {
        a.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn setup_invocation() {
        let a = parse(&v(&[
            "--src",
            "/d",
            "--pkg",
            "/p",
            "--name",
            "Cuphead",
            "--assembly-patch",
            "Assembly-CSharp.dll",
            "env X=1 '/s/codepatch' --target=android {managed} {out}",
            "--managed-file",
            "a.dll",
            "--managed-file=b.dll",
            "--consume",
            "--verify-bundles",
            "2",
            "--pack",
            "all",
            "--no-video-shader",
            "--dllmap",
            "SDL2=x",
        ]))
        .unwrap()
        .unwrap();
        assert_eq!(a.src, "/d");
        assert_eq!(
            a.assembly_patch[0].1,
            "env X=1 '/s/codepatch' --target=android {managed} {out}"
        );
        assert_eq!(a.managed_file, ["a.dll", "b.dll"]);
        assert!(a.consume_src && !a.video_shader && a.inline_textures);
        assert_eq!(
            (a.verify_bundles, a.pack.as_str(), a.rewired_joysticks),
            (2, "all", 4)
        );
        assert!(a.loose_assets.is_none() && a.beside.is_none());
    }

    #[test]
    fn errors() {
        assert!(parse(&v(&["--pkg", "x"]))
            .unwrap_err()
            .ends_with("required: --src"));
        assert!(parse(&v(&["--src", "x", "--no-v"]))
            .unwrap_err()
            .contains("ambiguous option"));
        assert!(parse(&v(&["--src", "x", "--pack", "zip"]))
            .unwrap_err()
            .contains("invalid choice: 'zip'"));
        assert!(parse(&v(&["--src", "x", "--verify-bundles", "a"]))
            .unwrap_err()
            .contains("invalid int value"));
        assert!(parse(&v(&["--src"]))
            .unwrap_err()
            .contains("expected one argument"));
        assert!(parse(&v(&["--src", "x", "--help"])).unwrap().is_none());
    }
}
