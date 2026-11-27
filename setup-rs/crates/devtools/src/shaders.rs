//! Shader overrides for measuring a scene (was tools/device/flatshaders.py and deadfrags.py).
//!
//! `pmu-dev flatshaders [--notex] <dump dir> <override dir> [<hash> ...]`: fragment shaders that write a
//! constant colour, for measuring a scene without the GPU's work. <dump dir> holds a run's shader
//! sources (UA_SHADER_DUMP=<dir>: <fnv64>.glsl each); every fragment shader among them (no gl_Position)
//! is written to <override dir> under the same name with its main() replaced by one that writes grey to
//! each of its outputs. Run the game with UA_SHADER_OVERRIDE=<override dir> (a folder outside the
//! bench's run folders: profrun.sh empties <bench>/<run name>): the draws, passes, blending and every
//! CPU-side step stay, the fragment work is gone, and the frame rate is what the CPU side allows. A
//! measurement, never a setting: the picture is grey. The files are made from the game's own shaders:
//! keep them out of any repository.
//! --notex: instead, every texture lookup of each fragment shader is replaced by an expression of its
//! coordinate (the arithmetic before and after it stays, the fetch goes): with UA_GL_TRACE's per-pass GPU
//! times against the shaders as they are, whether a pass is arithmetic-bound (as long without its
//! fetches; the Mali's arithmetic and texture units run side by side) or fetch-bound. A shader whose
//! branches or loops depend on what it fetched takes other paths: read such a pass from the flat run.
//! Hashes after the folders: only those fragment shaders (default: all).
//!
//! `pmu-dev deadfrags shaders <dump dir> <trace log> <override dir> [--copy H,..] [--mark H,..] [--hide
//! H,..] [--only H,..]` and `pmu-dev deadfrags sum <run dir>... [--rows A:B] [--skip X0,Y0,X1,Y1]`: a
//! frame's 'dead' fragments, those whose texture sample has alpha exactly 0, which a sprite drawn with
//! alpha blending cannot change the picture with (a measurement, never a setting).
//! `shaders`: <dump dir> holds a run's shader sources (UA_SHADER_DUMP), <trace log> the run.log of a
//! UA_GL_TRACE_DRAWS run (each draw's fragment shader hash and blend state). Every fragment shader of the
//! traced frame that samples _MainTex at a plain varying (`texture(_MainTex, vs_X.xy)`) is written to
//! <override dir> with a main() that adds 1/255 to red when that sample's alpha is exactly 0 and to green
//! otherwise, in a way its blend state turns into an exact count:
//!   blend 1,1-sa      writes (d, l, 0, 0) / 255         dst + src
//!   blend sa,1-sa     writes (d, l, 0, 1/255)           dst * 254/255 + src / 255: +1 code where src is 1,
//!                                                       the others kept (counts under 127)
//!   blend sa,1        writes ((d, l, 0) / 255, 1)       dst + src
//! Other blend states are left out (listed). The roles of the frame's other fragment shaders are given by
//! hash: --copy (post effects between the scene and the window: an exact copy of _MainTex, texelFetch at
//! the fragment, so the counts reach the screenshot), --mark (adds 1/255 to blue: an opaque background
//! copy, a full-screen overlay; blend off or 1,1-sa), --hide (writes nothing: rgb and alpha 0, e.g. the
//! HUD's own shaders), --only (count only these; the other counting shaders hide). Run the game with
//! UA_SHADER_OVERRIDE=<override dir> and a plan's `shot` steps.
//! `sum`: adds the counters up over each run's shot-*.ppm / shot-*.png: dead and live fragments, as
//! full-screen layers of the picture (--rows A:B: the picture's rows in the window, e.g. 15:465 for a
//! 720x450 picture in a 720x480 window; --skip: a box left out, e.g. a HUD drawn with the same shaders).
//! The override files are made from the game's own shaders: keep them out of any repository.
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::io::Read;
use std::path::Path;

use regex::Regex;

const OUT_RX: &str = r"layout\s*\(\s*location\s*=\s*\d+\s*\)\s*out\s+(?:\w+\s+)?vec4\s+(\w+)\s*;";

/// the shader with a constant main(), or None (not a fragment shader, no main)
pub fn flatten(src: &str) -> Option<String> {
    if src.contains("gl_Position") {
        return None;
    }
    let i = src.find("void main()")?;
    let head = &src[..i];
    let outs: Vec<String> = Regex::new(OUT_RX).unwrap().captures_iter(head).map(|c| c[1].to_string()).collect();
    let grey = "vec4(0.5, 0.5, 0.5, 1.0)";
    let body = if !outs.is_empty() {
        outs.iter().map(|o| format!("{o} = {grey};")).collect::<Vec<_>>().join(" ")
    } else if head.contains("FRAG_COLOR") {
        format!("FRAG_COLOR = {grey};")
    } else if src.contains("gl_FragData") {
        format!("gl_FragData[0] = {grey};")
    } else {
        format!("gl_FragColor = {grey};")
    };
    Some(format!("{head}void main()\n{{\n    {body}\n}}\n"))
}

fn split_args(s: &str) -> Vec<String> {
    let (mut out, mut depth, mut cur) = (Vec::new(), 0i32, String::new());
    for ch in s.chars() {
        depth += match ch {
            '(' | '[' => 1,
            ')' | ']' => -1,
            _ => 0,
        };
        if ch == ',' && depth == 0 {
            out.push(cur.trim().to_string());
            cur.clear();
        } else {
            cur.push(ch);
        }
    }
    out.push(cur.trim().to_string());
    out
}

/// the shader with each texture lookup in main() replaced by a value made from its coordinate, or None
pub fn notex(src: &str) -> Option<String> {
    if src.contains("gl_Position") || !src.contains("void main()") {
        return None;
    }
    let lookup = Regex::new(
        r"\b(texture|textureLod|textureProj|textureProjLod|textureGrad|textureOffset|textureLodOffset|texelFetch|texelFetchOffset|textureGather|textureGatherOffset|texture2D|texture2DLod|textureCube|shadow2D|shadow2DEXT)\s*\(",
    )
    .unwrap();
    let types: HashMap<String, String> = Regex::new(r"uniform\s+(?:(?:highp|mediump|lowp)\s+)?(\w*sampler\w+)\s+(\w+)\s*(?:\[[^\]]*\])?\s*;")
        .unwrap()
        .captures_iter(src)
        .map(|c| (c[2].to_string(), c[1].to_string()))
        .collect();
    let (head, body) = src.split_once("void main()").unwrap();
    let b = body.as_bytes();
    let (mut out, mut i) = (String::new(), 0usize);
    let idx = Regex::new(r"\[.*").unwrap();
    loop {
        let Some(m) = lookup.captures_at(body, i) else {
            return Some(format!("{head}void main(){out}{}", &body[i..]));
        };
        let all = m.get(0).unwrap();
        let fname = m[1].to_string();
        out += &body[i..all.start()];
        let (mut j, mut depth) = (all.end(), 1i32);
        while depth != 0 {
            if j >= b.len() {
                return None;
            }
            depth += match b[j] {
                b'(' => 1,
                b')' => -1,
                _ => 0,
            };
            j += 1;
        }
        let a = split_args(&body[all.end()..j - 1]);
        let st = types.get(idx.replace(&a[0], "").as_ref()).cloned().unwrap_or_else(|| "sampler2D".into());
        let coord = a.get(1).cloned().unwrap_or_else(|| "0.0".into());
        let rest: String = if fname == "texelFetch" || fname == "texelFetchOffset" {
            String::new()
        } else {
            a.iter().skip(2).map(|x| format!(" + float({x}) * 0.0")).collect()
        };
        let val = format!("(float(({coord}).x){rest}) * 0.5 + 0.25");
        if st.contains("Shadow") || fname.starts_with("shadow2D") {
            out += &if fname.starts_with("textureGather") { format!("vec4({val})") } else { format!("({val})") };
        } else if st.starts_with("usampler") {
            out += &format!("uvec4(uint({val} * 8.0))");
        } else if st.starts_with("isampler") {
            out += &format!("ivec4(int({val} * 8.0))");
        } else {
            out += &format!("vec4({val})");
        }
        i = j;
    }
}

pub fn flat_run(args: &[String]) -> Result<u8, String> {
    let nt = args.iter().any(|a| a == "--notex");
    let rest: Vec<&String> = args.iter().filter(|a| *a != "--notex").collect();
    if rest.len() < 2 {
        println!("usage: pmu-dev flatshaders [--notex] <dump dir> <override dir> [<hash> ...]");
        return Ok(2);
    }
    let (src, dst) = (rest[0], rest[1]);
    let only: HashSet<&str> = rest[2..].iter().map(|s| s.as_str()).collect();
    std::fs::create_dir_all(dst).map_err(|e| format!("{dst}: {e}"))?;
    let mut names: Vec<String> = std::fs::read_dir(src)
        .map_err(|e| format!("{src}: {e}"))?
        .filter_map(|e| e.ok().map(|e| e.file_name().to_string_lossy().into_owned()))
        .collect();
    names.sort();
    let mut n = 0;
    for f in names {
        let Some(stem) = f.strip_suffix(".glsl") else { continue };
        if !only.is_empty() && !only.contains(stem) {
            continue;
        }
        let text = String::from_utf8_lossy(&std::fs::read(Path::new(src).join(&f)).map_err(|e| e.to_string())?).into_owned();
        let Some(out) = (if nt { notex(&text) } else { flatten(&text) }) else { continue };
        std::fs::write(Path::new(dst).join(&f), out).map_err(|e| e.to_string())?;
        n += 1;
    }
    println!("{n} fragment shaders written to {dst}{}", if nt { " (texture lookups replaced)" } else { "" });
    Ok(0)
}

const BLENDS: [&str; 3] = ["1,1-sa", "sa,1-sa", "sa,1"];

/// fragment shader hash -> the blend states it was drawn with in the traced frame
pub fn blends(log: &str) -> BTreeMap<String, BTreeSet<String>> {
    let mut out: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
    for c in Regex::new(r" fs ([0-9a-f]{16}) .*?\| .*?blend (\S+)").unwrap().captures_iter(log) {
        out.entry(c[1].to_string()).or_default().insert(c[2].to_string());
    }
    out
}

/// the statements of the new main(), or None when the role does not fit the blend state
fn body(role: &str, blend: &str, o: &str, uv: Option<&str>) -> Option<Vec<String>> {
    match role {
        "copy" => Some(vec![format!("{o} = texelFetch(_MainTex, ivec2(gl_FragCoord.xy), 0);")]),
        "mark" => (blend == "off" || blend == "1,1-sa").then(|| vec![format!("{o} = vec4(0.0, 0.0, 1.0 / 255.0, 0.0);")]),
        "hide" => BLENDS.contains(&blend).then(|| vec![format!("{o} = vec4(0.0);")]),
        "count" => {
            let mut v = vec![format!("float ua_a = texture(_MainTex, {}).w;", uv.unwrap_or("")), "float ua_d = (ua_a == 0.0) ? 1.0 : 0.0;".to_string()];
            v.push(match blend {
                "1,1-sa" => format!("{o} = vec4(ua_d, 1.0 - ua_d, 0.0, 0.0) * (1.0 / 255.0);"),
                "sa,1-sa" => format!("{o} = vec4(ua_d, 1.0 - ua_d, 0.0, 1.0 / 255.0);"),
                "sa,1" => format!("{o} = vec4(vec3(ua_d, 1.0 - ua_d, 0.0) * (1.0 / 255.0), 1.0);"),
                _ => return None,
            });
            Some(v)
        }
        _ => None,
    }
}

/// (new source, role) or Err(reason)
pub fn rewrite(src: &str, role: &str, blend: &str) -> Result<(String, String), String> {
    if src.contains("gl_Position") {
        return Err("vertex shader".into());
    }
    let Some(i) = src.find("void main()") else { return Err("no main()".into()) };
    let head = &src[..i];
    let o = Regex::new(r"layout\s*\(\s*location\s*=\s*0\s*\)\s*out\s+(?:\w+\s+)?vec4\s+(\w+)\s*;")
        .unwrap()
        .captures(head)
        .map(|c| c[1].to_string())
        .ok_or("no layout(location = 0) vec4 output")?;
    let uv = Regex::new(r"texture\(_MainTex,\s*(vs_\w+)\.xy\)").unwrap().captures(src).map(|c| format!("{}.xy", &c[1]));
    if role == "count" && uv.is_none() {
        return Err("no _MainTex sample at a varying".into());
    }
    let b = body(role, blend, &o, uv.as_deref()).ok_or_else(|| format!("blend {blend}"))?;
    Ok((format!("{head}void main()\n{{\n    {}\n}}\n", b.join("\n    ")), role.to_string()))
}

fn take_opt(args: &mut Vec<String>, name: &str) -> Option<String> {
    let k = args.iter().position(|a| a == name)?;
    let v = args.get(k + 1).cloned();
    args.drain(k..(k + 2).min(args.len()));
    v
}

fn cmd_shaders(mut args: Vec<String>) -> Result<u8, String> {
    let mut set = |n: &str| -> HashSet<String> { take_opt(&mut args, n).map(|v| v.split(',').map(String::from).collect()).unwrap_or_default() };
    let (copy, mark, hide, only) = (set("--copy"), set("--mark"), set("--hide"), set("--only"));
    let [dump, log, dst] = &args[..] else {
        println!("usage: pmu-dev deadfrags shaders <dump dir> <trace log> <override dir> [--copy H,..] [--mark H,..] [--hide H,..] [--only H,..]");
        return Ok(2);
    };
    std::fs::create_dir_all(dst).map_err(|e| format!("{dst}: {e}"))?;
    let bl = blends(&String::from_utf8_lossy(&std::fs::read(log).map_err(|e| format!("{log}: {e}"))?));
    let mut n = 0;
    for (fs, bs) in &bl {
        let path = Path::new(dump).join(format!("{fs}.glsl"));
        if !path.exists() {
            println!("{fs}: not in {dump}");
            continue;
        }
        if bs.len() != 1 {
            println!("{fs}: left (drawn with several blend states: {})", bs.iter().cloned().collect::<Vec<_>>().join(", "));
            continue;
        }
        let blend = bs.iter().next().unwrap();
        let mut role = if copy.contains(fs) {
            "copy"
        } else if mark.contains(fs) {
            "mark"
        } else if hide.contains(fs) {
            "hide"
        } else {
            "count"
        };
        if role == "count" && !only.is_empty() && !only.contains(fs) {
            role = "hide";
        }
        let text = String::from_utf8_lossy(&std::fs::read(&path).map_err(|e| e.to_string())?).into_owned();
        match rewrite(&text, role, blend) {
            Err(why) => println!("{fs}: left ({why})"),
            Ok((out, what)) => {
                std::fs::write(Path::new(dst).join(format!("{fs}.glsl")), out).map_err(|e| e.to_string())?;
                n += 1;
                println!("{fs}: {what} (blend {blend})");
            }
        }
    }
    println!("{n} fragment shaders written to {dst}");
    Ok(0)
}

/// An RGB picture: (width, height, rgb bytes) of a binary PPM (P6, 8-bit) or an 8-bit PNG.
pub fn read_image(p: &Path) -> Result<(usize, usize, Vec<u8>), String> {
    let d = std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
    if d.starts_with(b"P6") {
        let mut f = Vec::new();
        let mut i = 2;
        while f.len() < 3 {
            while i < d.len() && d[i].is_ascii_whitespace() {
                i += 1;
            }
            if i < d.len() && d[i] == b'#' {
                while i < d.len() && d[i] != b'\n' {
                    i += 1;
                }
                continue;
            }
            let s = i;
            while i < d.len() && d[i].is_ascii_digit() {
                i += 1;
            }
            f.push(std::str::from_utf8(&d[s..i]).ok().and_then(|x| x.parse::<usize>().ok()).ok_or("a bad PPM header")?);
        }
        let (w, h) = (f[0], f[1]);
        let px = d.get(i + 1..i + 1 + w * h * 3).ok_or("a short PPM")?;
        return Ok((w, h, px.to_vec()));
    }
    if !d.starts_with(b"\x89PNG\r\n\x1a\n") {
        return Err(format!("{}: not a PPM or PNG picture", p.display()));
    }
    let (mut w, mut h, mut depth, mut ctype, mut idat) = (0, 0, 0, 0, Vec::new());
    let mut i = 8;
    while i + 8 <= d.len() {
        let n = u32::from_be_bytes(d[i..i + 4].try_into().unwrap()) as usize;
        let kind = &d[i + 4..i + 8];
        let data = d.get(i + 8..i + 8 + n).ok_or("a short PNG")?;
        match kind {
            b"IHDR" => {
                w = u32::from_be_bytes(data[0..4].try_into().unwrap()) as usize;
                h = u32::from_be_bytes(data[4..8].try_into().unwrap()) as usize;
                depth = data[8];
                ctype = data[9];
                if data[12] != 0 {
                    return Err("an interlaced PNG".into());
                }
            }
            b"IDAT" => idat.extend_from_slice(data),
            b"IEND" => break,
            _ => {}
        }
        i += 12 + n;
    }
    let ch = match ctype {
        0 => 1,
        2 => 3,
        4 => 2,
        6 => 4,
        _ => return Err(format!("PNG colour type {ctype}")),
    };
    if depth != 8 {
        return Err(format!("a {depth}-bit PNG"));
    }
    let mut raw = Vec::new();
    flate2::read::ZlibDecoder::new(&idat[..]).read_to_end(&mut raw).map_err(|e| e.to_string())?;
    let stride = w * ch;
    let mut px = vec![0u8; stride * h];
    for y in 0..h {
        let f = raw[y * (stride + 1)];
        let line = &raw[y * (stride + 1) + 1..(y + 1) * (stride + 1)];
        for x in 0..stride {
            let a = if x >= ch { px[y * stride + x - ch] as i32 } else { 0 };
            let b = if y > 0 { px[(y - 1) * stride + x] as i32 } else { 0 };
            let c = if x >= ch && y > 0 { px[(y - 1) * stride + x - ch] as i32 } else { 0 };
            let pred = match f {
                0 => 0,
                1 => a,
                2 => b,
                3 => (a + b) / 2,
                4 => {
                    let p = a + b - c;
                    let (pa, pb, pc) = ((p - a).abs(), (p - b).abs(), (p - c).abs());
                    if pa <= pb && pa <= pc {
                        a
                    } else if pb <= pc {
                        b
                    } else {
                        c
                    }
                }
                _ => return Err(format!("PNG filter {f}")),
            };
            px[y * stride + x] = (line[x] as i32 + pred) as u8;
        }
    }
    let rgb: Vec<u8> = px
        .chunks(ch)
        .flat_map(|p| match ch {
            1 | 2 => [p[0], p[0], p[0]],
            _ => [p[0], p[1], p[2]],
        })
        .collect();
    Ok((w, h, rgb))
}

/// (dead, live, marked, pixels) of one screenshot
pub fn counts(img: &(usize, usize, Vec<u8>), rows: Option<(usize, usize)>, skip: Option<(usize, usize, usize, usize)>) -> (u64, u64, u64, u64) {
    let (w, h, px) = img;
    let mut r = (0u64, 0u64, 0u64, 0u64);
    for y in 0..*h {
        if let Some((a, b)) = rows {
            if y < a || y >= b {
                continue;
            }
        }
        for x in 0..*w {
            if let Some((x0, y0, x1, y1)) = skip {
                if (y0..y1).contains(&y) && (x0..x1).contains(&x) {
                    continue;
                }
            }
            let p = &px[(y * w + x) * 3..(y * w + x) * 3 + 3];
            r.0 += p[0] as u64;
            r.1 += p[1] as u64;
            r.2 += p[2] as u64;
            r.3 += 1;
        }
    }
    r
}

fn cmd_sum(mut args: Vec<String>) -> Result<u8, String> {
    let nums = |s: String, sep: char| -> Vec<usize> { s.split(sep).filter_map(|v| v.trim().parse().ok()).collect() };
    let rows = take_opt(&mut args, "--rows").map(|v| nums(v, ':')).filter(|v| v.len() == 2).map(|v| (v[0], v[1]));
    let skip = take_opt(&mut args, "--skip").map(|v| nums(v, ',')).filter(|v| v.len() == 4).map(|v| (v[0], v[1], v[2], v[3]));
    for d in &args {
        let mut fs: Vec<String> = std::fs::read_dir(d)
            .map(|r| {
                r.filter_map(|e| e.ok().map(|e| e.file_name().to_string_lossy().into_owned()))
                    .filter(|n| n.starts_with("shot-") && (n.ends_with(".ppm") || n.ends_with(".png")))
                    .collect()
            })
            .unwrap_or_default();
        fs.sort();
        let mut res = Vec::new();
        for f in &fs {
            res.push(counts(&read_image(&Path::new(d).join(f))?, rows, skip));
        }
        if res.is_empty() {
            println!("{d}: no shots");
            continue;
        }
        let k = res.len() as f64;
        let avg = |i: usize| res.iter().map(|r| [r.0, r.1, r.2, r.3][i] as f64).fold(0.0f64, |a, b| a + b) / k;
        let (dead, live, mark, px) = (avg(0), avg(1), avg(2), avg(3));
        let base = Path::new(d.trim_end_matches('/')).file_name().map_or(d.clone(), |b| b.to_string_lossy().into_owned());
        println!(
            "{base}: {} shots, {px:.0} pixels; sprite fragments {:.2} layers, dead {:.2} ({:.1} %), marked {:.2}",
            res.len(),
            (dead + live) / px,
            dead / px,
            dead / (dead + live).max(1.0) * 100.0,
            mark / px
        );
    }
    Ok(0)
}

pub fn dead_run(args: &[String]) -> Result<u8, String> {
    match args.first().map(String::as_str) {
        Some("shaders") => cmd_shaders(args[1..].to_vec()),
        Some("sum") => cmd_sum(args[1..].to_vec()),
        _ => {
            println!("usage: pmu-dev deadfrags shaders <dump dir> <trace log> <override dir> [...] | sum <run dir>... [--rows A:B] [--skip X0,Y0,X1,Y1]");
            Ok(2)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FS: &str = "#version 300 es\nprecision mediump float;\nuniform sampler2D _MainTex;\nuniform mediump sampler2DShadow _Sh;\nin vec2 vs_TEXCOORD0;\nlayout(location = 0) out vec4 SV_Target0;\nvoid main()\n{\n    vec4 c = texture(_MainTex, vs_TEXCOORD0.xy);\n    float s = texture(_Sh, vec3(vs_TEXCOORD0, 0.5));\n    SV_Target0 = c * s + texelFetch(_MainTex, ivec2(1, 2), 0);\n}\n";

    #[test]
    fn flat_and_notex() {
        assert_eq!(
            flatten(FS).unwrap().rsplit_once("void main()").unwrap().1,
            "\n{\n    SV_Target0 = vec4(0.5, 0.5, 0.5, 1.0);\n}\n"
        );
        assert!(flatten("void main() { gl_Position = vec4(0.0); }").is_none());
        let n = notex(FS).unwrap();
        assert!(n.contains("vec4 c = vec4((float((vs_TEXCOORD0.xy).x)) * 0.5 + 0.25);"), "{n}");
        assert!(n.contains("float s = ((float((vec3(vs_TEXCOORD0, 0.5)).x)) * 0.5 + 0.25);"), "{n}");
        assert!(n.contains("+ vec4((float((ivec2(1, 2)).x)) * 0.5 + 0.25);"), "{n}");
    }

    #[test]
    fn dead_rewrite() {
        let log = "draw 3 fs 00000000000000aa vs x | tex 1 blend sa,1-sa\ndraw 4 fs 00000000000000bb vs x | blend off\ndraw 5 fs 00000000000000bb vs | blend sa,1\n";
        let b = blends(log);
        assert_eq!(b["00000000000000aa"].iter().collect::<Vec<_>>(), ["sa,1-sa"]);
        assert_eq!(b["00000000000000bb"].len(), 2);
        let (out, role) = rewrite(FS, "count", "sa,1-sa").unwrap();
        assert_eq!(role, "count");
        assert!(out.contains("float ua_a = texture(_MainTex, vs_TEXCOORD0.xy).w;\n    float ua_d = (ua_a == 0.0) ? 1.0 : 0.0;\n    SV_Target0 = vec4(ua_d, 1.0 - ua_d, 0.0, 1.0 / 255.0);"), "{out}");
        assert_eq!(rewrite(FS, "mark", "sa,1").unwrap_err(), "blend sa,1");
    }

    #[test]
    fn png_counts() {
        // 2x1 RGB, filter 1 (sub): (2,1,0) then (2,1,0) again
        let mut z = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
        std::io::Write::write_all(&mut z, &[1, 2, 1, 0, 0, 0, 0]).unwrap();
        let idat = z.finish().unwrap();
        let chunk = |k: &[u8], d: &[u8]| {
            let mut c = (d.len() as u32).to_be_bytes().to_vec();
            c.extend_from_slice(k);
            c.extend_from_slice(d);
            c.extend_from_slice(&[0; 4]);
            c
        };
        let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
        png.extend(chunk(b"IHDR", &[0, 0, 0, 2, 0, 0, 0, 1, 8, 2, 0, 0, 0]));
        png.extend(chunk(b"IDAT", &idat));
        png.extend(chunk(b"IEND", b""));
        let p = std::env::temp_dir().join(format!("pmu-dev-shot-{}.png", std::process::id()));
        std::fs::write(&p, png).unwrap();
        let img = read_image(&p).unwrap();
        assert_eq!(img.2, [2, 1, 0, 2, 1, 0]);
        assert_eq!(counts(&img, None, None), (4, 2, 0, 2));
        assert_eq!(counts(&img, None, Some((1, 0, 2, 1))), (2, 1, 0, 1));
        let _ = std::fs::remove_file(p);
    }
}
