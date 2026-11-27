//! `pmu-dev shaders-export <data> <dir> [--bundles REGEX]` (was `python -m pmunity shaders-export`): a
//! converted game's GLES3 programs as one file a stage, for compile checks and GPU cost tests:
//! <dir>/<nnnn>.vert and <nnnn>.frag (each the source Unity hands the driver for that stage; identical
//! programs once), <nnnn>.comp for each GLES3 compute kernel and <dir>/index.tsv (number, shader, file,
//! blob index or kernel, keywords). runtime/host/out/shadercheck compiles every pair and kernel with the
//! device's own driver (docs/shaders.md). --bundles: which StreamingAssets bundles (default all).
pub fn run(args: &[String]) -> Result<u8, String> {
    let usage = "usage: pmu-dev shaders-export <data> <dir> [--bundles REGEX]";
    let (mut pos, mut bundles) = (vec![], ".".to_string());
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--bundles" => {
                bundles = args.get(i + 1).cloned().ok_or(usage)?;
                i += 1;
            }
            a if a.starts_with("--bundles=") => bundles = a["--bundles=".len()..].to_string(),
            a if a.starts_with('-') => return Err(usage.into()),
            a => pos.push(a.to_string()),
        }
        i += 1;
    }
    let [data, dir] = &pos[..] else { return Err(usage.into()) };
    let cfg = convert::pyjson::Json::Dict(vec![("version".into(), convert::pyjson::Json::Int(1))]);
    let p = std::path::Path::new(data);
    let job = convert::Job::new(p, p, cfg).map_err(|e| e.to_string())?;
    convert::steps::shaders::export(&job, std::path::Path::new(dir), &bundles).map_err(|e| e.to_string())?;
    Ok(0)
}
