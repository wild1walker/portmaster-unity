//! GLES 3.1 kernels for ComputeShader assets that only carry OpenGL Core (and Vulkan, Metal, ...)
//! ones (shaders/compute.py; its docstring has the layouts): add_gles3 gives such a ComputeShader
//! a copy of its GLCore variant with renderer 11, the GLES level and every kernel's code
//! translated to GLSL ES 3.10 (shaderx's glsl::kernel_to_es), the formats of write-only images
//! taken from the Vulkan variant's SPIR-V of the same kernel.
use crate::error::{Error, Result};
use shaderx::glsl::{self, Optimize};
use shaderx::py;
use std::collections::HashMap;
use unityfs::Value;

pub const RENDERER_GLES3: i128 = 11;
pub const RENDERER_GLCORE: i128 = 17;
pub const RENDERER_VULKAN: i128 = 21;

/// (major, minor) of a Unity version string, (0, 0) when it does not parse (compute._major_minor).
fn major_minor(unity: &str) -> (i64, i64) {
    let mut it = unity.split('.');
    match (it.next(), it.next()) {
        (Some(a), Some(b)) => match (py_int(a), py_int(b)) {
            (Some(a), Some(b)) => (a, b),
            _ => (0, 0),
        },
        _ => (0, 0),
    }
}

/// int(s) of a version part (surrounding whitespace allowed, as Python's int).
fn py_int(s: &str) -> Option<i64> {
    let t = py::strip(s);
    let digits = t.strip_prefix(['+', '-']).unwrap_or(t);
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit() || b == b'_') {
        return None;
    }
    t.replace('_', "").parse().ok()
}

/// The targetLevel Unity gives GLES3 compute variants: 3 up to 2022.3, 2 from 2023.1.
pub fn gles_level(unity: &str) -> i64 {
    if major_minor(unity) >= (2023, 1) {
        2
    } else {
        3
    }
}

/// What add_gles3 did to one ComputeShader (compute.Result).
#[derive(Clone, Debug, Default)]
pub struct ComputeResult {
    pub name: String,
    /// converted | unchanged | skipped
    pub status: &'static str,
    pub why: String,
    pub kernels: usize,
    pub variants: usize,
    pub failed: Vec<(String, String)>,
    pub notes: Vec<String>,
}

impl std::fmt::Display for ComputeResult {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(py::rstrip(&format!(
            "{}: {} {}",
            self.name, self.status, self.why
        )))
    }
}

fn int_of(v: &Value, name: &str) -> Result<i128> {
    v.get(name)
        .ok_or_else(|| Error::Value(format!("KeyError: '{name}'")))?
        .as_int()
        .ok_or_else(|| Error::Value(format!("{name}: not an integer")))
}

fn kernels_of(v: &Value) -> Result<&Vec<Value>> {
    v.get("kernels")
        .ok_or_else(|| Error::Value("KeyError: 'kernels'".into()))?
        .as_array()
        .ok_or_else(|| Error::Value("kernels: not a list".into()))
}

/// The variant of a renderer that has kernels, of the highest level: the first of the highest
/// (bool(kernels), targetLevel), as sorted(..., reverse=True)[0] picks it.
fn best_variant(variants: &[Value], renderer: i128) -> Result<Option<usize>> {
    let mut best: Option<(usize, (bool, i128))> = None;
    for (i, v) in variants.iter().enumerate() {
        if int_of(v, "targetRenderer")? != renderer {
            continue;
        }
        let key = (!kernels_of(v)?.is_empty(), int_of(v, "targetLevel")?);
        if best.is_none_or(|(_, k)| key > k) {
            best = Some((i, key));
        }
    }
    Ok(best.map(|(i, _)| i))
}

/// The keys of a kernel's tree, sorted (for the unknown-layout message).
fn keys(k: &Value) -> Vec<String> {
    let mut out: Vec<String> = k
        .as_struct()
        .map(|s| s.names().map(String::from).collect())
        .unwrap_or_default();
    out.sort();
    out
}

fn kernel_name(k: &Value) -> String {
    k.get("name")
        .and_then(Value::as_bytes)
        .map(|b| String::from_utf8_lossy(b).into_owned())
        .unwrap_or_default()
}

fn present(k: &Value, key: &str) -> bool {
    k.get(key).is_some()
}

/// The layout of a kernel's code (compute.kernel_variants): 0 the kernel itself ('code'), 1
/// variantMap's values, 2 uniqueVariants.
fn layout(k: &Value) -> Result<u8> {
    if present(k, "code") {
        Ok(0)
    } else if present(k, "variantMap") {
        Ok(1)
    } else if present(k, "uniqueVariants") {
        Ok(2)
    } else {
        let n = kernel_name(k);
        Err(Error::Value(format!(
            "kernel {}: unknown layout (keys [{}])",
            py::repr(&n),
            keys(k)
                .iter()
                .map(|x| py::repr(x))
                .collect::<Vec<_>>()
                .join(", ")
        )))
    }
}

/// The values holding a kernel's code, in their list order.
fn kernel_variants(k: &Value) -> Result<Vec<&Value>> {
    Ok(match layout(k)? {
        0 => vec![k],
        1 => k
            .get("variantMap")
            .and_then(Value::as_array)
            .ok_or_else(|| Error::Value("variantMap: not a list".into()))?
            .iter()
            .map(|p| match p {
                Value::Pair(p) => Ok(&p.1),
                _ => Err(Error::Value("variantMap: not a list of pairs".into())),
            })
            .collect::<Result<_>>()?,
        _ => k
            .get("uniqueVariants")
            .and_then(Value::as_array)
            .ok_or_else(|| Error::Value("uniqueVariants: not a list".into()))?
            .iter()
            .collect(),
    })
}

fn kernel_variants_mut(k: &mut Value) -> Result<Vec<&mut Value>> {
    Ok(match layout(k)? {
        0 => vec![k],
        1 => k
            .get_mut("variantMap")
            .and_then(Value::as_array_mut)
            .ok_or_else(|| Error::Value("variantMap: not a list".into()))?
            .iter_mut()
            .map(|p| match p {
                Value::Pair(p) => Ok(&mut p.1),
                _ => Err(Error::Value("variantMap: not a list of pairs".into())),
            })
            .collect::<Result<_>>()?,
        _ => k
            .get_mut("uniqueVariants")
            .and_then(Value::as_array_mut)
            .ok_or_else(|| Error::Value("uniqueVariants: not a list".into()))?
            .iter_mut()
            .collect(),
    })
}

/// A keyword set's member as the Python hashes it (a str's bytes; anything else by its debug form).
fn keyword(v: &Value) -> Vec<u8> {
    match v.as_bytes() {
        Some(b) => b.to_vec(),
        None => format!("{v:?}").into_bytes(),
    }
}

/// For each of kernel_variants(kernel): the keyword sets that select it.
fn variant_keywords(k: &Value) -> Result<Vec<Vec<Vec<u8>>>> {
    Ok(match layout(k)? {
        0 => vec![vec![Vec::new()]],
        1 => k
            .get("variantMap")
            .and_then(Value::as_array)
            .ok_or_else(|| Error::Value("variantMap: not a list".into()))?
            .iter()
            .map(|p| match p {
                Value::Pair(p) => Ok(vec![keyword(&p.0)]),
                _ => Err(Error::Value("variantMap: not a list of pairs".into())),
            })
            .collect::<Result<_>>()?,
        _ => {
            let n = k
                .get("uniqueVariants")
                .and_then(Value::as_array)
                .map_or(0, Vec::len);
            let mut out: Vec<Vec<Vec<u8>>> = vec![Vec::new(); n];
            if let Some(Value::Array(idx)) = k.get("variantIndices") {
                for p in idx {
                    let Value::Pair(p) = p else { continue };
                    let Some(i) = p.1.as_int() else { continue };
                    if i >= 0 && (i as usize) < n {
                        let kw = keyword(&p.0);
                        let set = &mut out[i as usize];
                        if !set.contains(&kw) {
                            set.push(kw);
                        }
                    }
                }
            }
            out
        }
    })
}

/// A code field's bytes (`bytes(c)`).
fn code_bytes(kv: &Value) -> Result<Vec<u8>> {
    let c = kv
        .get("code")
        .ok_or_else(|| Error::Value("KeyError: 'code'".into()))?;
    match c {
        Value::Bytes(b) | Value::Str(b) => Ok(b.clone()),
        Value::Array(a) => a
            .iter()
            .map(|x| {
                x.as_int()
                    .and_then(|v| u8::try_from(v).ok())
                    .ok_or_else(|| Error::Value("code: bytes must be in range(0, 256)".into()))
            })
            .collect(),
        _ => Err(Error::Value("code: not bytes".into())),
    }
}

/// kv['code'] = data, as bytes when it was bytes, else as a list.
fn set_code(kv: &mut Value, data: Vec<u8>) -> Result<()> {
    let like_bytes = matches!(kv.get("code"), Some(Value::Bytes(_) | Value::Str(_)));
    kv.set(
        "code",
        if like_bytes {
            Value::Bytes(data)
        } else {
            Value::Array(data.into_iter().map(Value::U8).collect())
        },
    )?;
    Ok(())
}

/// SPIR-V image formats (compute._SPV_FORMATS).
fn spv_format(f: u32) -> String {
    const F: [&str; 40] = [
        "",
        "rgba32f",
        "rgba16f",
        "r32f",
        "rgba8",
        "rgba8_snorm",
        "rg32f",
        "rg16f",
        "r11f_g11f_b10f",
        "r16f",
        "rgba16",
        "rgb10_a2",
        "rg16",
        "rg8",
        "r16",
        "r8",
        "rgba16_snorm",
        "rg16_snorm",
        "rg8_snorm",
        "r16_snorm",
        "r8_snorm",
        "rgba32i",
        "rgba16i",
        "rgba8i",
        "r32i",
        "rg32i",
        "rg16i",
        "rg8i",
        "r16i",
        "r8i",
        "rgba32ui",
        "rgba16ui",
        "rgba8ui",
        "r32ui",
        "rgb10_a2ui",
        "rg32ui",
        "rg16ui",
        "rg8ui",
        "r16ui",
        "r8ui",
    ];
    match F.get(f as usize) {
        Some(s) if f > 0 => s.to_string(),
        _ => format!("spirv-format-{f}"),
    }
}

/// {variable name: format} of the image variables in a SPIR-V module (formats given, not Unknown),
/// in the order the Python's dict ends up with (a later variable of the same name replaces one).
pub fn spirv_image_formats(code: &[u8]) -> Vec<(String, String)> {
    if code.len() < 20 || u32::from_le_bytes(code[..4].try_into().unwrap()) != 0x0723_0203 {
        return Vec::new();
    }
    let w: Vec<u32> = code
        .chunks_exact(4)
        .map(|c| u32::from_le_bytes(c.try_into().unwrap()))
        .collect();
    let mut names: HashMap<u32, String> = HashMap::new();
    let mut images: HashMap<u32, u32> = HashMap::new();
    let mut pointers: HashMap<u32, u32> = HashMap::new();
    let mut variables: Vec<(u32, u32)> = Vec::new();
    let mut i = 5;
    while i < w.len() {
        let (op, n) = (w[i] & 0xffff, (w[i] >> 16) as usize);
        if n == 0 {
            break;
        }
        let a = &w[(i + 1).min(w.len())..(i + n).min(w.len())];
        match op {
            5 if a.len() >= 2 => {
                let bytes: Vec<u8> = a[1..].iter().flat_map(|x| x.to_le_bytes()).collect();
                let s = bytes.split(|&b| b == 0).next().unwrap_or(&[]);
                names.insert(a[0], String::from_utf8_lossy(s).into_owned());
            }
            25 if a.len() >= 8 => {
                images.insert(a[0], a[7]);
            }
            32 if a.len() >= 3 => {
                pointers.insert(a[0], a[2]);
            }
            59 if a.len() >= 3 => variables.push((a[1], a[0])),
            _ => {}
        }
        i += n;
    }
    let mut out: Vec<(String, String)> = Vec::new();
    for (vid, tid) in variables {
        let fmt = pointers.get(&tid).and_then(|t| images.get(t)).copied();
        if let (Some(fmt), Some(name)) = (fmt.filter(|&f| f != 0), names.get(&vid)) {
            let v = spv_format(fmt);
            match out.iter_mut().find(|(k, _)| k == name) {
                Some(e) => e.1 = v,
                None => out.push((name.clone(), v)),
            }
        }
    }
    out
}

/// Image formats of the Vulkan kernel's variant of the same keywords (else at the same index), by
/// GLSL name ('_origX0X' dropped).
fn vulkan_formats(
    vk: Option<&Value>,
    keywords: &[Vec<u8>],
    index: usize,
) -> Result<HashMap<String, String>> {
    let Some(vk) = vk else {
        return Ok(HashMap::new());
    };
    let vs = kernel_variants(vk)?;
    let vkw = variant_keywords(vk)?;
    let m = vkw
        .iter()
        .position(|kws| kws.iter().any(|k| keywords.contains(k)))
        .or((index < vs.len()).then_some(index));
    let Some(m) = m else {
        return Ok(HashMap::new());
    };
    let mut out = HashMap::new();
    for (k, v) in spirv_image_formats(&code_bytes(vs[m])?) {
        let k = k.strip_suffix("_origX0X").map(String::from).unwrap_or(k);
        out.insert(k, v);
    }
    Ok(out)
}

/// Python's UnicodeDecodeError message for bytes that are not UTF-8.
fn decode_error(b: &[u8], e: std::str::Utf8Error) -> String {
    let pos = e.valid_up_to();
    let byte = b[pos];
    let why = match e.error_len() {
        None => "unexpected end of data",
        Some(_) if (0x80..0xc2).contains(&byte) || byte >= 0xf5 => "invalid start byte",
        Some(_) => "invalid continuation byte",
    };
    format!("'utf-8' codec can't decode byte 0x{byte:02x} in position {pos}: {why}")
}

/// One kernel's variants translated: their new codes, or why the kernel is left out.
fn translate_kernel(
    k: &Value,
    vkk: Option<&Value>,
    precision: &str,
    optimize: Optimize,
    notes: &mut Vec<String>,
) -> Result<std::result::Result<Vec<Vec<u8>>, String>> {
    let kws = variant_keywords(k)?;
    let mut done = Vec::new();
    for (vi, kv) in kernel_variants(k)?.into_iter().enumerate() {
        let code = code_bytes(kv)?;
        let end = code.iter().rposition(|&b| b != 0).map_or(0, |p| p + 1);
        let text = match std::str::from_utf8(&code[..end]) {
            Ok(t) => t,
            Err(e) => return Ok(Err(decode_error(&code[..end], e))),
        };
        let formats = if glsl::image_names(text).is_empty() {
            None
        } else {
            Some(vulkan_formats(
                vkk,
                &kws[vi.min(kws.len().saturating_sub(1))],
                vi,
            )?)
        };
        match glsl::kernel_to_es(text, precision, formats.as_ref(), optimize, notes) {
            Ok(s) => {
                let mut out = s.into_bytes();
                if code.last() == Some(&0) {
                    out.push(0);
                }
                done.push(out);
            }
            Err(u) => return Ok(Err(u.msg)),
        }
    }
    Ok(Ok(done))
}

fn failed_list(failed: &[(String, String)]) -> String {
    failed
        .iter()
        .take(3)
        .map(|(n, w)| format!("{n}: {w}"))
        .collect::<Vec<_>>()
        .join("; ")
}

/// Adds a GLES3 variant to one ComputeShader tree in place (compute.add_gles3). unity: the
/// file's version string; replace: rebuild a GLES3 variant the shader has from its GLCore one.
pub fn add_gles3(
    tree: &mut Value,
    unity: &str,
    precision: &str,
    replace: bool,
    optimize: Optimize,
) -> Result<ComputeResult> {
    let mut r = ComputeResult {
        name: tree
            .get("m_Name")
            .and_then(Value::as_bytes)
            .map_or("?".into(), |b| String::from_utf8_lossy(b).into_owned()),
        status: "unchanged",
        ..Default::default()
    };
    let variants = tree
        .get_mut("variants")
        .ok_or_else(|| Error::Value("KeyError: 'variants'".into()))?
        .as_array_mut()
        .ok_or_else(|| Error::Value("variants: not a list".into()))?;
    let Some(gi) = best_variant(variants, RENDERER_GLCORE)? else {
        r.why = "no OpenGL Core kernels".into();
        return Ok(r);
    };
    let gl = variants[gi].clone();
    let mut removed_before = 0;
    let has_old = variants
        .iter()
        .map(|v| int_of(v, "targetRenderer"))
        .collect::<Result<Vec<_>>>()?
        .contains(&RENDERER_GLES3);
    if has_old {
        if !replace {
            r.why = "already has GLES3 kernels".into();
            return Ok(r);
        }
        let mut keep = Vec::with_capacity(variants.len());
        for (i, v) in variants.drain(..).enumerate() {
            if int_of(&v, "targetRenderer")? != RENDERER_GLES3 {
                keep.push(v);
            } else if i < gi {
                removed_before += 1;
            }
        }
        *variants = keep;
        r.status = "converted";
        r.why = "(old GLES3 kernels removed)".into();
    }
    let mut vk_kernels: Vec<(String, Vec<Value>)> = Vec::new();
    if let Some(vi) = best_variant(variants, RENDERER_VULKAN)? {
        for k in kernels_of(&variants[vi])? {
            let n = kernel_name(k);
            match vk_kernels.iter_mut().find(|(x, _)| *x == n) {
                Some(e) => e.1.push(k.clone()),
                None => vk_kernels.push((n, vec![k.clone()])),
            }
        }
    }
    let mut es = gl.clone();
    es.set("targetRenderer", Value::I64(RENDERER_GLES3 as i64))?;
    es.set("targetLevel", Value::I64(gles_level(unity)))?;
    let mut kept: Vec<Value> = Vec::new();
    let mut seen: HashMap<String, usize> = HashMap::new();
    for mut k in kernels_of(&es)?.clone() {
        let name = kernel_name(&k);
        let nth = *seen.get(&name).unwrap_or(&0);
        seen.insert(name.clone(), nth + 1);
        let vkk = vk_kernels
            .iter()
            .find(|(x, _)| *x == name)
            .and_then(|(_, l)| l.get(nth));
        match translate_kernel(&k, vkk, precision, optimize, &mut r.notes)? {
            Err(why) => {
                r.failed.push((name, why));
                continue;
            }
            Ok(codes) => {
                let n = codes.len();
                for (kv, c) in kernel_variants_mut(&mut k)?.into_iter().zip(codes) {
                    set_code(kv, c)?;
                }
                r.variants += n;
                kept.push(k);
            }
        }
    }
    if kept.is_empty() {
        if r.status != "converted" {
            r.status = if r.failed.is_empty() {
                "unchanged"
            } else {
                "skipped"
            };
        }
        let head = if r.failed.is_empty() {
            "no OpenGL Core kernels".to_string()
        } else {
            format!("no kernel translates: {}", failed_list(&r.failed))
        };
        r.why = if r.why.is_empty() {
            head
        } else {
            format!("{head} {}", r.why)
        };
        return Ok(r);
    }
    r.kernels = kept.len();
    es.set("kernels", Value::Array(kept))?;
    // where Unity puts it: up to 2021.x right after the GLCore variant, from 2022.1 in renderer order
    let at = if major_minor(unity) >= (2022, 1) {
        let mut at = variants.len();
        for (i, v) in variants.iter().enumerate() {
            if int_of(v, "targetRenderer")? > RENDERER_GLES3 {
                at = i;
                break;
            }
        }
        at
    } else {
        // (list.index: the first variant equal to it; the one it was where values do not compare
        // equal to themselves, a NaN)
        let removed = if has_old { removed_before } else { 0 };
        variants
            .iter()
            .position(|v| *v == gl)
            .unwrap_or(gi - removed)
            + 1
    };
    variants.insert(at, es);
    r.status = "converted";
    if !r.failed.is_empty() {
        r.why = py::strip(&format!(
            "{} of {} kernels left out: {} {}",
            r.failed.len(),
            r.failed.len() + r.kernels,
            failed_list(&r.failed),
            r.why
        ))
        .to_string();
    }
    Ok(r)
}

/// The step's summary line of the compute shaders (step.run's).
pub fn summary(results: &[ComputeResult]) -> String {
    let cc: Vec<&ComputeResult> = results.iter().filter(|r| r.status == "converted").collect();
    format!(
        "shaders: {} of {} compute shaders given GLES3 kernels ({} kernels, {} kernel variants, GLSL ES 3.10: used where \
         the device has OpenGL ES 3.1+); {} kernels left out; {} without OpenGL Core kernels",
        cc.len(),
        results.len(),
        cc.iter().map(|r| r.kernels).sum::<usize>(),
        cc.iter().map(|r| r.variants).sum::<usize>(),
        results.iter().map(|r| r.failed.len()).sum::<usize>(),
        results
            .iter()
            .filter(|r| r.why.starts_with("no OpenGL Core"))
            .count()
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn levels_and_versions() {
        assert_eq!(gles_level("2022.3.36f1"), 3);
        assert_eq!(gles_level("2023.1.8f1"), 2);
        assert_eq!(gles_level("0.0.0"), 3);
        assert_eq!(major_minor("6000.0.58f2"), (6000, 0));
        assert_eq!(major_minor("x"), (0, 0));
    }

    #[test]
    fn spirv_formats() {
        // header, OpName %7 "img_origX0X", OpTypeImage %5 (format 3: r32f), OpTypePointer %6 -> %5,
        // OpVariable %6 %7
        let mut w: Vec<u32> = vec![0x0723_0203, 0x10000, 0, 8, 0];
        let name = b"img_origX0X\0";
        let mut nw: Vec<u32> = name
            .chunks(4)
            .map(|c| {
                let mut b = [0u8; 4];
                b[..c.len()].copy_from_slice(c);
                u32::from_le_bytes(b)
            })
            .collect();
        w.push(((2 + nw.len() as u32) << 16) | 5);
        w.push(7);
        w.append(&mut nw);
        w.extend([(9 << 16) | 25, 5, 1, 1, 0, 0, 0, 2, 3]);
        w.extend([(4 << 16) | 32, 6, 0, 5]);
        w.extend([(4 << 16) | 59, 6, 7, 0]);
        let code: Vec<u8> = w.iter().flat_map(|x| x.to_le_bytes()).collect();
        assert_eq!(
            spirv_image_formats(&code),
            vec![("img_origX0X".to_string(), "r32f".to_string())]
        );
        assert!(spirv_image_formats(b"not spir-v at all....").is_empty());
    }
}
