//! `shaders.fused_post` on the Shader objects (shaders/fusedpost.py's apply): a chain's fused pass
//! (shaderx::fusedpost writes its program) appended to the chain's last shader, its GLES3
//! subprograms pointing to new blob entries with the binding info Unity's GL compiler would give
//! the fused program (shaderx::bindings::gl_bindings, first checked to reproduce the last pass's
//! own).
use super::grabfetch::gles_only;
use crate::error::{Error, Result};
use crate::pyjson::Json;
use shaderx::bindings::{self, Unsupported};
use shaderx::blob::{self, Slot};
use shaderx::convert::STAGES;
use shaderx::fusedpost::{self as fp, NotFusable};
use shaderx::tree;
use std::collections::{BTreeSet, HashMap};
use unityfs::Value;

const GLES3_TYPES: [i128; 2] = [blob::GPU_GLES3 as i128, blob::GPU_GLES31 as i128];

/// One chain of the config (name, [(shader, pass)], quantize).
#[derive(Clone, Debug)]
pub struct Chain {
    pub name: String,
    pub items: Vec<(String, usize)>,
    pub quantize: String,
}

/// The chains of a config's shaders.fused_post, checked (fusedpost.chains_of).
pub fn chains_of(cfg: &Json) -> Result<Vec<Chain>> {
    let v = cfg.get("shaders").and_then(|s| s.get("fused_post"));
    let Some(v) = v.filter(|v| v.truthy()) else {
        return Ok(Vec::new());
    };
    let Json::List(l) = v else {
        return Err(Error::Value(
            "shaders.fused_post: a list of {\"name\", \"chain\", \"quantize\"} objects".into(),
        ));
    };
    if !l.iter().all(|c| matches!(c, Json::Dict(_))) {
        return Err(Error::Value(
            "shaders.fused_post: a list of {\"name\", \"chain\", \"quantize\"} objects".into(),
        ));
    }
    let mut out = Vec::new();
    for c in l {
        let chain = c.get("chain").cloned().unwrap_or(Json::Null);
        let names: Option<Vec<String>> = match &chain {
            Json::List(x) if x.len() >= 2 => x
                .iter()
                .map(|s| match s {
                    Json::Str(s) if !s.is_empty() => Some(s.clone()),
                    _ => None,
                })
                .collect(),
            _ => None,
        };
        let Some(names) = names else {
            return Err(Error::Value(format!(
                "shaders.fused_post: \"chain\" is a list of two or more shader names ({})",
                chain.repr()
            )));
        };
        let q = c
            .get("quantize")
            .cloned()
            .unwrap_or(Json::Str("rgba8".into()));
        let quantize = match &q {
            Json::Str(s) if fp::QUANTIZE.contains(&s.as_str()) => s.clone(),
            _ => {
                return Err(Error::Value(format!(
                    "shaders.fused_post: quantize {} (one of {})",
                    c.get("quantize").map_or("None".into(), Json::repr),
                    fp::QUANTIZE.join(", ")
                )))
            }
        };
        let name = match c.get("name") {
            Some(n) if n.truthy() => n.clone(),
            _ => Json::Str(fp::DEFAULT_NAME.into()),
        };
        let ok = matches!(&name, Json::Str(s) if {
            let mut ch = s.chars();
            ch.next().is_some_and(|f| f.is_ascii_alphabetic() || f == '_')
                && ch.all(|x| x.is_ascii_alphanumeric() || x == '_')
        });
        if !ok {
            return Err(Error::Value(format!(
                "shaders.fused_post: name {} (a C identifier)",
                c.get("name").map_or("None".into(), Json::repr)
            )));
        }
        let items = names
            .iter()
            .map(|s| {
                // ("Name" or "Name:<pass>": re.fullmatch(r'(.*?)(?::(\d+))?', s))
                match s.rsplit_once(':') {
                    Some((n, p)) if !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()) => {
                        (n.to_string(), p.parse().unwrap_or(usize::MAX))
                    }
                    _ => (s.clone(), 0),
                }
            })
            .collect();
        out.push(Chain {
            name: name.to_str(),
            items,
            quantize,
        });
    }
    Ok(out)
}

/// Every shader name the chains name.
pub fn chain_names(chains: &[Chain]) -> BTreeSet<String> {
    chains
        .iter()
        .flat_map(|c| c.items.iter().map(|(n, _)| n.clone()))
        .collect()
}

/// What apply did (fusedpost.Result).
#[derive(Clone, Debug)]
pub struct FpResult {
    pub status: &'static str,
    pub why: String,
    pub shader: String,
    pub pass_index: Option<usize>,
    pub programs: usize,
}

impl FpResult {
    pub fn changed(&self) -> bool {
        self.status == "changed"
    }
}

impl std::fmt::Display for FpResult {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.status, self.why)
    }
}

/// Why a chain is left as it is, or a fault the Python does not catch.
enum Stop {
    Skip(String),
    Fail(Error),
}

impl From<NotFusable> for Stop {
    fn from(e: NotFusable) -> Stop {
        Stop::Skip(e.0)
    }
}

impl From<Unsupported> for Stop {
    fn from(e: Unsupported) -> Stop {
        Stop::Skip(e.0)
    }
}

fn skip<T>(m: String) -> std::result::Result<T, Stop> {
    Err(Stop::Skip(m))
}

/// convert.unity_of: (major, minor) of the data, from its version string, else from the layout.
fn unity_of(t: &Value, version: &str) -> (u64, u64) {
    if !version.is_empty() {
        let mut it = version.split('.');
        if let (Some(a), Some(b)) = (it.next(), it.next()) {
            if let (Ok(a), Ok(b)) = (a.trim().parse(), b.trim().parse()) {
                return (a, b);
            }
        }
    }
    let subs = t
        .path(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let passes: Vec<&Value> = subs
        .iter()
        .flat_map(|ss| {
            ss.get("m_Passes")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
        })
        .collect();
    let progs: Vec<&Value> = passes
        .iter()
        .flat_map(|p| STAGES.iter().filter_map(|st| p.get(st)))
        .filter(|p| truthy(Some(p)))
        .collect();
    if progs.iter().any(|p| truthy(p.get("m_PlayerSubPrograms"))) {
        return if passes
            .iter()
            .any(|p| p.get("m_SerializedKeywordStateMask").is_some())
        {
            (2022, 1)
        } else {
            (2021, 2)
        };
    }
    if progs.iter().any(|p| p.get("m_CommonParameters").is_some()) {
        return (2020, 1);
    }
    if progs.iter().any(|p| {
        p.get("m_SubPrograms")
            .and_then(Value::as_array)
            .is_some_and(|l| {
                l.iter()
                    .any(|sp| sp.get("m_GlobalKeywordIndices").is_some())
            })
    }) {
        return (2019, 1);
    }
    (2018, 4)
}

fn truthy(v: Option<&Value>) -> bool {
    match v {
        None => false,
        Some(Value::Struct(s)) => !s.fields.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Bytes(b) | Value::Str(b)) => !b.is_empty(),
        Some(v) => v.as_f64().is_some_and(|x| x != 0.0),
    }
}

/// A stage program's subprogram lists with their parameter indices (convert._tier_lists).
fn tier_lists(prog: Option<&Value>) -> Vec<(&Vec<Value>, Option<&Vec<Value>>)> {
    let Some(prog) = prog.filter(|p| truthy(Some(p))) else {
        return Vec::new();
    };
    if truthy(prog.get("m_PlayerSubPrograms")) {
        let players = prog
            .get("m_PlayerSubPrograms")
            .and_then(Value::as_array)
            .unwrap();
        let pbis = prog
            .get("m_ParameterBlobIndices")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        return players
            .iter()
            .zip(0..pbis.len())
            .filter_map(|(l, k)| {
                let pbi = prog
                    .get("m_ParameterBlobIndices")?
                    .as_array()?
                    .get(k)?
                    .as_array();
                Some((l.as_array()?, pbi))
            })
            .collect();
    }
    static EMPTY: Vec<Value> = Vec::new();
    vec![(
        prog.get("m_SubPrograms")
            .and_then(Value::as_array)
            .unwrap_or(&EMPTY),
        None,
    )]
}

/// (its one GLES3 program's code, [blob indices], parameter entry index or None, entries,
/// blobs) of SubShader 0's pass (fusedpost._pass_program).
#[allow(clippy::type_complexity)]
fn pass_program(
    t: &Value,
    pass: usize,
    label: &str,
) -> std::result::Result<
    (
        String,
        Vec<i128>,
        Option<i128>,
        Vec<Option<Slot>>,
        blob::ShaderBlobs,
    ),
    Stop,
> {
    let subs = t
        .path(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array)
        .ok_or_else(|| Stop::Fail(Error::Value("KeyError: 'm_SubShaders'".into())))?;
    let passes = subs
        .first()
        .and_then(|s| s.get("m_Passes"))
        .and_then(Value::as_array);
    let Some(p) = passes.and_then(|ps| ps.get(pass)) else {
        return skip(format!("{label}: no such pass"));
    };
    let typ = p.get("m_Type").and_then(Value::as_int);
    if typ != Some(0) {
        return skip(format!(
            "{label}: not a normal pass (type {})",
            typ.map_or("None".into(), |x| x.to_string())
        ));
    }
    let plats = t
        .get("platforms")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    if !plats
        .iter()
        .any(|x| x.as_int() == Some(blob::PLATFORM_GLES3PLUS as i128))
    {
        return skip(format!("{label}: no GLES3 programs"));
    }
    let blobs = tree::blobs(t).map_err(Stop::Skip)?;
    let entries =
        tree::platform_entries(t, blob::PLATFORM_GLES3PLUS, &blobs).map_err(Stop::Skip)?;
    let mut sps = Vec::new();
    for st in STAGES {
        for (lst, pbi) in tier_lists(p.get(st)) {
            for (j, sp) in lst.iter().enumerate() {
                let ty = sp
                    .get("m_GpuProgramType")
                    .and_then(Value::as_int)
                    .unwrap_or(-1);
                if GLES3_TYPES.contains(&ty) {
                    sps.push((sp, pbi, j));
                }
            }
        }
    }
    if sps.is_empty() {
        return skip(format!("{label}: no GLES3 subprogram"));
    }
    let mut codes = BTreeSet::new();
    let mut params: BTreeSet<Option<i128>> = BTreeSet::new();
    for (sp, pbi, j) in sps {
        if [
            "m_KeywordIndices",
            "m_GlobalKeywordIndices",
            "m_LocalKeywordIndices",
        ]
        .iter()
        .any(|k| truthy(sp.get(k)))
        {
            return skip(format!("{label}: keyword variants"));
        }
        let bi = sp.get("m_BlobIndex").and_then(Value::as_int).unwrap_or(-1);
        let e = usize::try_from(bi)
            .ok()
            .and_then(|i| entries.get(i))
            .cloned()
            .flatten();
        match e {
            Some(Slot::Program(e)) if !e.code.is_empty() => {}
            _ => return skip(format!("{label}: empty program entry")),
        }
        codes.insert(bi);
        params.insert(match pbi {
            Some(pbi) => Some(pbi.get(j).and_then(Value::as_int).ok_or_else(|| {
                Stop::Fail(Error::Value("IndexError: list index out of range".into()))
            })?),
            None => None,
        });
    }
    let mut texts = BTreeSet::new();
    for &i in &codes {
        if let Some(Slot::Program(e)) = &entries[i as usize] {
            texts.insert(
                String::from_utf8(e.code.clone())
                    .map_err(|_| Stop::Skip("'utf-8' codec can't decode the program".into()))?,
            );
        }
    }
    if texts.len() != 1 || params.len() != 1 {
        return skip(format!("{label}: more than one GLES3 program"));
    }
    Ok((
        texts.into_iter().next().unwrap(),
        codes.into_iter().collect(),
        params.into_iter().next().unwrap(),
        entries,
        blobs,
    ))
}

/// Appends a chain's fused pass to its last shader (fusedpost.apply). shaders: {name: tree} of
/// the file's chain shaders (the last one's tree edited in place); unity: the file's version.
pub fn apply(shaders: &mut HashMap<String, Value>, chain: &Chain, unity: &str) -> Result<FpResult> {
    let mut r = FpResult {
        status: "unchanged",
        why: String::new(),
        shader: chain.items.last().unwrap().0.clone(),
        pass_index: None,
        programs: 0,
    };
    let missing: Vec<&str> = chain
        .items
        .iter()
        .filter(|(n, _)| !shaders.contains_key(n))
        .map(|(n, _)| n.as_str())
        .collect();
    if !missing.is_empty() {
        r.status = "skipped";
        r.why = format!("not in this file: {}", missing.join(", "));
        return Ok(r);
    }
    let labels: Vec<String> = chain
        .items
        .iter()
        .map(|(n, i)| format!("\"{n}\" pass {i}"))
        .collect();
    let plan = (|| -> std::result::Result<Option<_>, Stop> {
        let mut codes = Vec::new();
        for ((n, i), label) in chain.items.iter().zip(&labels) {
            codes.push(pass_program(&shaders[n], *i, label)?.0);
        }
        let last_tree = &shaders[&r.shader];
        let last = chain.items.last().unwrap();
        let (_, blob_indices, param_index, entries, blobs) =
            pass_program(last_tree, last.1, labels.last().unwrap())?;
        let mark = fp::MARK.as_bytes();
        if entries.iter().any(|e| {
            matches!(e, Some(Slot::Program(e)) if e.code.windows(mark.len()).any(|w| w == mark))
        }) {
            return Ok(Some(Err("already has a fused pass (fused_post done before)".to_string())));
        }
        let named = last_tree
            .path(&["m_ParsedForm", "m_SubShaders"])
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .flat_map(|ss| {
                ss.get("m_Passes")
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
            })
            .any(|p| {
                p.path(&["m_State", "m_Name"]).and_then(Value::as_bytes)
                    == Some(chain.name.as_bytes())
            });
        if named {
            return Ok(Some(Err(format!(
                "already has a pass named {}",
                chain.name
            ))));
        }
        let fused = fp::fuse_program(&codes, &labels, &chain.quantize)?;
        let uv = unity_of(last_tree, unity);
        let Some(Slot::Program(e0)) = entries[blob_indices[0] as usize].clone() else {
            unreachable!()
        };
        let e0_code = String::from_utf8(e0.code.clone()).unwrap();
        let (new_param, rest) = if let Some(pi) = param_index {
            let Some(Some(Slot::Params { version, data })) =
                usize::try_from(pi).ok().and_then(|i| entries.get(i))
            else {
                return skip(format!("{}: no parameter entry", labels.last().unwrap()));
            };
            let own = bindings::gl_bindings(&e0_code, uv, 0)?;
            if own.params_bytes() != *data {
                return skip(format!(
                    "{}: its binding info is not as this writes it (layout not understood)",
                    labels.last().unwrap()
                ));
            }
            let info = bindings::gl_bindings(&fused, uv, 0)?;
            (
                Some(Slot::Params {
                    version: *version,
                    data: info.params_bytes(),
                }),
                e0.rest.clone(),
            )
        } else {
            let own = bindings::entry_bindings(&e0, None)?;
            let info = bindings::gl_bindings(&fused, uv, own.source_map)?;
            if own.serialize() != bindings::gl_bindings(&e0_code, uv, own.source_map)?.serialize() {
                return skip(format!(
                    "{}: its binding info is not as this writes it (layout not understood)",
                    labels.last().unwrap()
                ));
            }
            (None, info.serialize())
        };
        Ok(Some(Ok((
            fused,
            blob_indices,
            param_index,
            entries,
            blobs,
            new_param,
            rest,
        ))))
    })();
    let (fused, blob_indices, param_index, entries, mut blobs, new_param, rest) = match plan {
        Err(Stop::Skip(why)) => {
            r.status = "skipped";
            r.why = why;
            return Ok(r);
        }
        Err(Stop::Fail(e)) => return Err(e),
        Ok(Some(Err(why))) => {
            r.why = why;
            return Ok(r);
        }
        Ok(Some(Ok(x))) => x,
        Ok(None) => unreachable!(),
    };
    let mut out = entries.clone();
    let mut segs = blobs
        .segments(blob::PLATFORM_GLES3PLUS)
        .map_err(|e| Error::Value(e.to_string()))?;
    let mut moved: HashMap<i128, usize> = HashMap::new();
    for &bi in &blob_indices {
        let Some(Slot::Program(mut e)) = out[bi as usize].clone() else {
            unreachable!()
        };
        e.code = fused.clone().into_bytes();
        e.rest = rest.clone();
        moved.insert(bi, out.len());
        out.push(Some(Slot::Program(e)));
        segs.push(segs[bi as usize]);
        r.programs += 1;
    }
    let mut new_pi = None;
    if let Some(np) = new_param {
        new_pi = Some(out.len());
        out.push(Some(np));
        segs.push(segs[param_index.unwrap() as usize]);
    }
    let last = chain.items.last().unwrap();
    let t = shaders.get_mut(&r.shader).unwrap();
    let mut cp = t
        .path(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array)
        .and_then(|s| s.first())
        .and_then(|s| s.get("m_Passes"))
        .and_then(Value::as_array)
        .and_then(|ps| ps.get(last.1))
        .cloned()
        .unwrap();
    gles_only(&mut cp, Some(&moved), false)?;
    if let Some(npi) = new_pi {
        for st in STAGES {
            let Some(prog) = cp.get_mut(st) else { continue };
            if !truthy(Some(prog)) || !truthy(prog.get("m_PlayerSubPrograms")) {
                continue;
            }
            let lens: Vec<usize> = prog
                .get("m_PlayerSubPrograms")
                .and_then(Value::as_array)
                .unwrap()
                .iter()
                .map(|l| l.as_array().map_or(0, Vec::len))
                .collect();
            let pbis = prog
                .get_mut("m_ParameterBlobIndices")
                .and_then(Value::as_array_mut)
                .ok_or_else(|| Error::Value("KeyError: 'm_ParameterBlobIndices'".into()))?;
            for (tier, n) in lens.into_iter().enumerate() {
                let slot = pbis.get_mut(tier).ok_or_else(|| {
                    Error::Value("IndexError: list assignment index out of range".into())
                })?;
                *slot = Value::Array((0..n).map(|_| Value::I64(npi as i64)).collect());
            }
        }
    }
    if let Some(st @ Value::Struct(_)) = cp.get_mut("m_State") {
        st.set("m_Name", Value::str(&chain.name))?;
    }
    blobs.set_entries(blob::PLATFORM_GLES3PLUS, &out, &segs, None);
    let fields = blobs.store().map_err(|e| Error::Value(e.to_string()))?;
    tree::set_blob(t, &fields).map_err(Error::Value)?;
    let passes = t
        .path_mut(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array_mut)
        .and_then(|s| s.first_mut())
        .and_then(|s| s.get_mut("m_Passes"))
        .and_then(Value::as_array_mut)
        .unwrap();
    passes.push(cp);
    r.pass_index = Some(passes.len() - 1);
    r.status = "changed";
    r.why = format!(
        "\"{}\" pass {} ({}) draws {} as one ({} program{}; quantize {})",
        r.shader,
        passes.len() - 1,
        chain.name,
        labels.join(", "),
        r.programs,
        if r.programs != 1 { "s" } else { "" },
        chain.quantize
    );
    Ok(r)
}

/// The step's summary line of the chains.
pub fn summary(chains: &[Chain], results: &[FpResult]) -> String {
    let pc: Vec<&FpResult> = results.iter().filter(|p| p.changed()).collect();
    let mut s = format!(
        "shaders: fused_post: {} of {} chains given one pass that draws them",
        pc.len(),
        chains.len()
    );
    if !pc.is_empty() {
        s += &format!(
            " ({})",
            pc.iter()
                .map(|p| format!(
                    "\"{}\" pass {}",
                    p.shader,
                    p.pass_index.map_or("None".into(), |x| x.to_string())
                ))
                .collect::<Vec<_>>()
                .join("; ")
        );
    }
    if results.iter().any(|p| !p.changed()) {
        s += &format!(
            "; left as they are: {}",
            results
                .iter()
                .filter(|p| !p.changed())
                .map(|p| format!("\"{}\": {}", p.shader, p.why))
                .collect::<Vec<_>>()
                .join("; ")
        );
    }
    if results.len() < chains.len() {
        s += &format!(
            "; {} chains whose last shader no file has",
            chains.len() - results.len()
        );
    }
    s
}
