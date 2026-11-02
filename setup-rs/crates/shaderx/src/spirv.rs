//! SPIR-V modules as shaders/spirv.py reads and edits them: the instructions, names, decorations,
//! types, constants, global variables and the entry point; set_name / set_member_name replace a
//! target's OpName / OpMemberName (or add one at the end of the debug names), serialize gives the
//! words back.
use std::collections::BTreeMap;

pub const MAGIC: u32 = 0x0723_0203;
pub const OP_SOURCE: u32 = 3;
pub const OP_NAME: u32 = 5;
pub const OP_MEMBER_NAME: u32 = 6;
pub const OP_ENTRY_POINT: u32 = 15;
pub const OP_CAPABILITY: u32 = 17;
pub const OP_CONSTANT: u32 = 43;
pub const OP_VARIABLE: u32 = 59;
pub const OP_DECORATE: u32 = 71;
pub const OP_MEMBER_DECORATE: u32 = 72;
pub const OP_DPDX: u32 = 207;
pub const OP_DPDY: u32 = 208;
pub const OP_FWIDTH: u32 = 209;
pub const CAP_DERIVATIVE_CONTROL: u32 = 51;
const BEFORE_NAMES: [u32; 13] = [17, 10, 11, 14, 15, 16, 331, 2, 3, 4, 7, 8, 317];

pub const DEC_BUFFER_BLOCK: u32 = 3;
pub const DEC_BUILTIN: u32 = 11;
pub const DEC_NOPERSPECTIVE: u32 = 13;
pub const DEC_FLAT: u32 = 14;
pub const DEC_CENTROID: u32 = 16;
pub const DEC_SAMPLE: u32 = 17;
pub const DEC_LOCATION: u32 = 30;
pub const DEC_COMPONENT: u32 = 31;
pub const DEC_INDEX: u32 = 32;
pub const DEC_BINDING: u32 = 33;
pub const DEC_DESCRIPTOR_SET: u32 = 34;
pub const DEC_OFFSET: u32 = 35;

pub const SC_UNIFORM_CONSTANT: u32 = 0;
pub const SC_INPUT: u32 = 1;
pub const SC_UNIFORM: u32 = 2;
pub const SC_OUTPUT: u32 = 3;
pub const SC_PUSH_CONSTANT: u32 = 9;
pub const SC_STORAGE_BUFFER: u32 = 12;

/// Why a module does not read (spirv.SpirvError, a ValueError).
#[derive(Clone, Debug)]
pub struct SpirvError(pub String);

/// A SPIR-V type (spirv.Type).
#[derive(Clone, Debug, Default)]
pub struct Type {
    pub id: u32,
    pub kind: &'static str,
    pub width: u32,
    pub signed: bool,
    pub elem: u32,
    pub count: u32,
    pub members: Vec<u32>,
    pub storage: u32,
    pub dim: u32,
    pub depth: u32,
    pub sampled: u32,
}

fn string(words: &[u32]) -> Result<(String, usize), SpirvError> {
    let b: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    let Some(end) = b.iter().position(|&x| x == 0) else {
        return Err(SpirvError("unterminated string".into()));
    };
    Ok((String::from_utf8_lossy(&b[..end]).into_owned(), end / 4 + 1))
}

fn string_words(s: &str) -> Vec<u32> {
    let mut b = s.as_bytes().to_vec();
    b.push(0);
    while b.len() % 4 != 0 {
        b.push(0);
    }
    b.chunks(4)
        .map(|c| u32::from_le_bytes(c.try_into().unwrap()))
        .collect()
}

/// A module (spirv.Module).
#[derive(Clone, Debug)]
pub struct Module {
    header: Vec<u32>,
    pub insts: Vec<(u32, Vec<u32>)>,
    pub decorations: BTreeMap<u32, BTreeMap<u32, Vec<u32>>>,
    pub member_decorations: BTreeMap<(u32, u32), BTreeMap<u32, Vec<u32>>>,
    pub types: BTreeMap<u32, Type>,
    pub constants: BTreeMap<u32, u32>,
    /// id -> (type id, storage class), in declaration order
    pub variables: Vec<(u32, (u32, u32))>,
    /// (execution model, name, function id, interface ids)
    pub entry: Option<(u32, String, u32, Vec<u32>)>,
    pub source_language: Option<u32>,
}

impl Module {
    pub fn new(data: &[u8]) -> Result<Module, SpirvError> {
        if data.len() < 20 || data.len() % 4 != 0 {
            return Err(SpirvError("not a SPIR-V module (size)".into()));
        }
        let words: Vec<u32> = data
            .chunks(4)
            .map(|c| u32::from_le_bytes(c.try_into().unwrap()))
            .collect();
        if words[0] != MAGIC {
            return Err(SpirvError("not a SPIR-V module (magic)".into()));
        }
        let mut insts = Vec::new();
        let mut i = 5;
        while i < words.len() {
            let (n, op) = ((words[i] >> 16) as usize, words[i] & 0xffff);
            if n == 0 || i + n > words.len() {
                return Err(SpirvError(format!("bad instruction length at word {i}")));
            }
            insts.push((op, words[i + 1..i + n].to_vec()));
            i += n;
        }
        let mut m = Module {
            header: words[..5].to_vec(),
            insts,
            decorations: BTreeMap::new(),
            member_decorations: BTreeMap::new(),
            types: BTreeMap::new(),
            constants: BTreeMap::new(),
            variables: Vec::new(),
            entry: None,
            source_language: None,
        };
        m.read()?;
        Ok(m)
    }

    fn read(&mut self) -> Result<(), SpirvError> {
        let short = || SpirvError("tuple index out of range".into());
        for (op, a) in &self.insts {
            let g = |k: usize| a.get(k).copied().ok_or_else(short);
            let t = |kind: &'static str| Type {
                id: a.first().copied().unwrap_or(0),
                kind,
                ..Default::default()
            };
            match *op {
                OP_NAME | OP_MEMBER_NAME => {}
                OP_DECORATE => {
                    let (id, dec) = (g(0)?, g(1)?);
                    self.decorations
                        .entry(id)
                        .or_default()
                        .insert(dec, a[2..].to_vec());
                }
                OP_MEMBER_DECORATE => {
                    let (id, m, dec) = (g(0)?, g(1)?, g(2)?);
                    self.member_decorations
                        .entry((id, m))
                        .or_default()
                        .insert(dec, a[3..].to_vec());
                }
                OP_ENTRY_POINT if self.entry.is_none() => {
                    let (name, used) = string(a.get(2..).ok_or_else(short)?)?;
                    self.entry = Some((
                        g(0)?,
                        name,
                        g(1)?,
                        a.get(2 + used..).unwrap_or(&[]).to_vec(),
                    ));
                }
                OP_SOURCE => self.source_language = Some(g(0)?),
                19 => {
                    self.types.insert(g(0)?, t("void"));
                }
                20 => {
                    self.types.insert(g(0)?, t("bool"));
                }
                21 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            width: g(1)?,
                            signed: g(2)? != 0,
                            ..t("int")
                        },
                    );
                }
                22 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            width: g(1)?,
                            ..t("float")
                        },
                    );
                }
                23 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            elem: g(1)?,
                            count: g(2)?,
                            ..t("vector")
                        },
                    );
                }
                24 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            elem: g(1)?,
                            count: g(2)?,
                            ..t("matrix")
                        },
                    );
                }
                25 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            elem: g(1)?,
                            dim: g(2)?,
                            depth: g(3)?,
                            sampled: g(6)?,
                            ..t("image")
                        },
                    );
                    g(7)?;
                }
                26 => {
                    self.types.insert(g(0)?, t("sampler"));
                }
                27 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            elem: g(1)?,
                            ..t("sampled_image")
                        },
                    );
                }
                28 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            elem: g(1)?,
                            count: g(2)?,
                            ..t("array")
                        },
                    );
                }
                29 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            elem: g(1)?,
                            ..t("runtime_array")
                        },
                    );
                }
                30 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            members: a[1..].to_vec(),
                            ..t("struct")
                        },
                    );
                }
                32 => {
                    self.types.insert(
                        g(0)?,
                        Type {
                            storage: g(1)?,
                            elem: g(2)?,
                            ..t("pointer")
                        },
                    );
                }
                OP_CONSTANT if a.len() >= 3 => {
                    self.constants.insert(a[1], a[2]);
                }
                OP_VARIABLE => {
                    let id = g(1)?;
                    let v = (g(0)?, g(2)?);
                    match self.variables.iter_mut().find(|(x, _)| *x == id) {
                        Some(e) => e.1 = v,
                        None => self.variables.push((id, v)),
                    }
                }
                _ => {}
            }
        }
        Ok(())
    }

    /// The stage of the entry point: VERTEX, FRAGMENT, ...
    pub fn stage(&self) -> Option<&'static str> {
        self.entry.as_ref().and_then(|e| match e.0 {
            0 => Some("VERTEX"),
            1 => Some("HULL"),
            2 => Some("DOMAIN"),
            3 => Some("GEOMETRY"),
            4 => Some("FRAGMENT"),
            5 => Some("COMPUTE"),
            _ => None,
        })
    }

    /// A decoration's first operand (1 for one without operands), or None.
    pub fn decoration(&self, id: u32, dec: u32) -> Option<u32> {
        self.decorations
            .get(&id)
            .and_then(|d| d.get(&dec))
            .map(|v| v.first().copied().unwrap_or(1))
    }

    pub fn member_decoration(&self, sid: u32, m: u32, dec: u32) -> Option<u32> {
        self.member_decorations
            .get(&(sid, m))
            .and_then(|d| d.get(&dec))
            .map(|v| v.first().copied().unwrap_or(1))
    }

    pub fn array_length(&self, t: &Type) -> Option<u32> {
        self.constants.get(&t.count).copied()
    }

    fn var(&self, id: u32) -> Option<(u32, u32)> {
        self.variables
            .iter()
            .find(|(x, _)| *x == id)
            .map(|(_, v)| *v)
    }

    /// The type a global variable points to, and its storage class.
    pub fn pointee(&self, var: u32) -> Result<(Type, u32), SpirvError> {
        let key = || SpirvError(format!("KeyError: {var}"));
        let (tid, sc) = self.var(var).ok_or_else(key)?;
        let p = self.types.get(&tid).ok_or_else(key)?;
        Ok((self.types.get(&p.elem).ok_or_else(key)?.clone(), sc))
    }

    /// The entry point's interface variables of a storage class, not built-ins.
    pub fn interface(&self, storage: u32) -> Result<Vec<u32>, SpirvError> {
        let mut out = Vec::new();
        let ids = self.entry.as_ref().map(|e| e.3.clone()).unwrap_or_default();
        for v in ids {
            let Some((_, sc)) = self.var(v) else { continue };
            if sc != storage || self.decoration(v, DEC_BUILTIN).is_some() {
                continue;
            }
            let (t, _) = self.pointee(v)?;
            if t.kind == "struct"
                && (0..t.members.len() as u32)
                    .any(|m| self.member_decoration(t.id, m, DEC_BUILTIN).is_some())
            {
                continue;
            }
            out.push(v);
        }
        Ok(out)
    }

    /// Global variables of the storage classes, in declaration order.
    pub fn globals(&self, storage: &[u32]) -> Vec<u32> {
        self.variables
            .iter()
            .filter(|(_, (_, sc))| storage.contains(sc))
            .map(|(v, _)| *v)
            .collect()
    }

    /// A short GLSL-like name of a type.
    pub fn describe(&self, tid: u32) -> String {
        let Some(t) = self.types.get(&tid) else {
            return format!("?{tid}");
        };
        match t.kind {
            "void" | "bool" => t.kind.to_string(),
            "int" => format!(
                "{}{}",
                if t.signed { "int" } else { "uint" },
                if t.width == 32 {
                    String::new()
                } else {
                    t.width.to_string()
                }
            ),
            "float" => {
                if t.width == 32 {
                    "float".into()
                } else {
                    format!("float{}", t.width)
                }
            }
            "vector" => {
                let e = self.describe(t.elem);
                let p = match e.as_str() {
                    "float" => "",
                    "int" => "i",
                    "uint" => "u",
                    "bool" => "b",
                    x => return format!("{x}vec{}", t.count),
                };
                format!("{p}vec{}", t.count)
            }
            "matrix" => {
                let col = self.types.get(&t.elem).map_or(0, |c| c.count);
                if col == t.count {
                    format!("mat{}", t.count)
                } else {
                    format!("mat{}x{col}", t.count)
                }
            }
            "array" => format!(
                "{}[{}]",
                self.describe(t.elem),
                self.array_length(t)
                    .map_or("None".into(), |n| n.to_string())
            ),
            k => k.to_string(),
        }
    }

    pub fn set_name(&mut self, id: u32, name: &str) {
        self.replace_name(OP_NAME, &[id], name);
    }

    pub fn set_member_name(&mut self, sid: u32, m: u32, name: &str) {
        self.replace_name(OP_MEMBER_NAME, &[sid, m], name);
    }

    fn replace_name(&mut self, op: u32, key: &[u32], name: &str) {
        self.insts
            .retain(|(o, a)| !(*o == op && a.len() >= key.len() && a[..key.len()] == *key));
        let at = self
            .insts
            .iter()
            .position(|(o, _)| !BEFORE_NAMES.contains(o) && *o != OP_NAME && *o != OP_MEMBER_NAME)
            .unwrap_or(self.insts.len());
        let mut a = key.to_vec();
        a.extend(string_words(name));
        self.insts.insert(at, (op, a));
    }

    /// Rewrites instructions of opcodes in the mapping (old -> new); how many.
    pub fn replace_opcodes(&mut self, mapping: &[(u32, u32)]) -> usize {
        let mut n = 0;
        for (op, _) in self.insts.iter_mut() {
            if let Some(&(_, new)) = mapping.iter().find(|(o, _)| o == op) {
                *op = new;
                n += 1;
            }
        }
        n
    }

    pub fn remove_capability(&mut self, cap: u32) {
        self.insts
            .retain(|(o, a)| !(*o == OP_CAPABILITY && a.first() == Some(&cap)));
    }

    pub fn serialize(&self) -> Vec<u8> {
        let mut out: Vec<u32> = self.header.clone();
        for (op, a) in &self.insts {
            out.push(((a.len() as u32 + 1) << 16) | op);
            out.extend(a);
        }
        out.iter().flat_map(|w| w.to_le_bytes()).collect()
    }
}
