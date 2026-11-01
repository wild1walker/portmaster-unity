//! ASTC guide files (astc_guide.py): per texture level, the effort tier each of its hard blocks is
//! searched at. Only the reading side the textures step uses: the Reader and split().

use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use serde_json::Value;

use crate::Error;

const MAGIC: &[u8] = b"PMUNITY-ASTC-GUIDE\n";
const VERSION: i64 = 1;

/// The tiers (astc_guide.TIERS): a preset and the limits lowered for it, cheapest first.
pub const TIERS: [(&str, Limits); 11] = [
    (
        "fastest",
        Limits {
            partitions: Some(1),
            index2: None,
        },
    ),
    (
        "fast",
        Limits {
            partitions: Some(1),
            index2: None,
        },
    ),
    ("fastest", Limits::NONE),
    (
        "fast",
        Limits {
            partitions: Some(2),
            index2: Some(8),
        },
    ),
    (
        "fast",
        Limits {
            partitions: Some(2),
            index2: None,
        },
    ),
    (
        "medium",
        Limits {
            partitions: Some(1),
            index2: None,
        },
    ),
    ("fast", Limits::NONE),
    (
        "medium",
        Limits {
            partitions: Some(2),
            index2: Some(16),
        },
    ),
    (
        "medium",
        Limits {
            partitions: Some(2),
            index2: None,
        },
    ),
    ("medium", Limits::NONE),
    ("thorough", Limits::NONE),
];

/// Limits put on an astc-encoder config after its preset (each lowered to the value, never raised):
/// tune_partition_count_limit and tune_2partition_index_limit.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Limits {
    pub partitions: Option<u32>,
    pub index2: Option<u32>,
}

impl Limits {
    pub const NONE: Limits = Limits {
        partitions: None,
        index2: None,
    };

    /// As the Python's limits dict ({} or tune_partition_count_limit / tune_2partition_index_limit).
    fn as_json(&self) -> Value {
        let mut m = serde_json::Map::new();
        if let Some(p) = self.partitions {
            m.insert("tune_partition_count_limit".into(), p.into());
        }
        if let Some(i) = self.index2 {
            m.insert("tune_2partition_index_limit".into(), i.into());
        }
        Value::Object(m)
    }
}

/// The texture settings a guide's blocks depend on (astc_guide.KEYS).
pub const KEYS: [&str; 13] = [
    "scale",
    "scale_sharp",
    "sharp_names",
    "block",
    "sharp_block",
    "keep_max",
    "min_side",
    "min_convert",
    "mips",
    "mode",
    "guided_max_err",
    "guided_line",
    "guided_near",
];

/// zlib.crc32 of the hard blocks' positions.
pub fn rest_crc(rest: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &b in rest {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & (crc & 1).wrapping_neg());
        }
    }
    !crc
}

/// A guide opened for the textures step (astc_guide.Reader): codes(key, rest) -> one tier byte a
/// hard block, or None (no entry, or not these blocks).
pub struct Guide {
    f: File,
    base: u64,
    index: HashMap<String, [u64; 4]>,
    /// The guide was made for these settings and tiers (else it is never used).
    pub ok: bool,
    /// Texture levels guided, and those not in it.
    pub used: u64,
    pub missed: u64,
}

impl Guide {
    /// Opens a guide; `settings` is the textures settings (DEFAULTS and the config's), as JSON.
    pub fn open(path: &Path, settings: &Value) -> Result<Guide, Error> {
        let mut f = File::open(path)?;
        let mut magic = vec![0u8; MAGIC.len()];
        if f.read_exact(&mut magic).is_err() || magic != MAGIC {
            return Err(Error::Guide(format!(
                "{}: not an ASTC guide",
                path.display()
            )));
        }
        let mut n = [0u8; 4];
        f.read_exact(&mut n)?;
        let mut head = vec![0u8; u32::from_le_bytes(n) as usize];
        f.read_exact(&mut head)?;
        let head: Value = serde_json::from_slice(&head)
            .map_err(|e| Error::Guide(format!("{}: {e}", path.display())))?;
        let base = f.stream_position()?;
        let mut index = HashMap::new();
        if let Some(m) = head.get("index").and_then(Value::as_object) {
            for (k, v) in m {
                let e = v.as_array().filter(|a| a.len() >= 4).and_then(|a| {
                    let n: Vec<u64> = a.iter().take(4).filter_map(Value::as_u64).collect();
                    (n.len() == 4).then(|| [n[0], n[1], n[2], n[3]])
                });
                if let Some(e) = e {
                    index.insert(k.clone(), e);
                }
            }
        } else {
            return Err(Error::Guide(format!("{}: no index", path.display())));
        }
        let want: serde_json::Map<String, Value> = KEYS
            .iter()
            .map(|k| {
                (
                    k.to_string(),
                    settings.get(*k).cloned().unwrap_or(Value::Null),
                )
            })
            .collect();
        let tiers = Value::Array(
            TIERS
                .iter()
                .map(|(p, l)| Value::Array(vec![(*p).into(), l.as_json()]))
                .collect(),
        );
        let ok = head
            .get("version")
            .is_some_and(|v| py_eq(v, &VERSION.into()))
            && head
                .get("settings")
                .is_some_and(|s| py_eq(s, &Value::Object(want)))
            && py_eq(head.get("tiers").unwrap_or(&Value::Array(vec![])), &tiers);
        Ok(Guide {
            f,
            base,
            index,
            ok,
            used: 0,
            missed: 0,
        })
    }

    /// The tier of each hard block of the level `key` (rest: their positions, int32 each), or None.
    pub fn codes(&mut self, key: &str, rest: &[u8]) -> Result<Option<Vec<u8>>, Error> {
        if !self.ok {
            return Ok(None);
        }
        let n = (rest.len() / 4) as u64;
        let Some(&e) = self
            .index
            .get(key)
            .filter(|e| e[2] == n && e[3] == rest_crc(rest) as u64)
        else {
            self.missed += 1;
            return Ok(None);
        };
        self.f.seek(SeekFrom::Start(self.base + e[0]))?;
        let mut z = vec![0u8; e[1] as usize];
        let got = read_upto(&mut self.f, &mut z)?;
        z.truncate(got);
        let codes = miniz_oxide::inflate::decompress_to_vec_zlib(&z)
            .map_err(|e| Error::Guide(format!("Error -3 while decompressing data: {e:?}")))?;
        if codes.len() as u64 != n || codes.iter().any(|&c| c as usize >= TIERS.len()) {
            self.missed += 1;
            return Ok(None);
        }
        self.used += 1;
        Ok(Some(codes))
    }
}

fn read_upto(f: &mut File, buf: &mut [u8]) -> std::io::Result<usize> {
    let mut got = 0;
    while got < buf.len() {
        match f.read(&mut buf[got..])? {
            0 => break,
            k => got += k,
        }
    }
    Ok(got)
}

/// The hard blocks' positions (int32 bytes) of each tier, in their order (astc_guide.split).
pub fn split(rest: &[u8], codes: &[u8]) -> Vec<Vec<u8>> {
    let mut out = vec![Vec::new(); TIERS.len()];
    for (pos, &c) in rest.chunks_exact(4).zip(codes) {
        out[c as usize].extend_from_slice(pos);
    }
    out
}

/// Python's == on values read from JSON: numbers by value (1 == 1.0, True == 1), lists item by
/// item, dicts key by key.
pub fn py_eq(a: &Value, b: &Value) -> bool {
    fn num(v: &Value) -> Option<f64> {
        match v {
            Value::Bool(b) => Some(*b as u8 as f64),
            Value::Number(n) => n.as_f64(),
            _ => None,
        }
    }
    match (a, b) {
        (Value::Null, Value::Null) => true,
        (Value::String(x), Value::String(y)) => x == y,
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y).all(|(p, q)| py_eq(p, q))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len() && x.iter().all(|(k, v)| y.get(k).is_some_and(|w| py_eq(v, w)))
        }
        (Value::Number(x), Value::Number(y)) if x.is_i64() && y.is_i64() => {
            x.as_i64() == y.as_i64()
        }
        (Value::Number(x), Value::Number(y)) if x.is_u64() && y.is_u64() => {
            x.as_u64() == y.as_u64()
        }
        _ => match (num(a), num(b)) {
            (Some(x), Some(y)) => x == y,
            _ => false,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn crc_matches_zlib() {
        assert_eq!(rest_crc(b""), 0);
        assert_eq!(rest_crc(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn split_keeps_order() {
        let rest: Vec<u8> = [5u32, 9, 2, 7]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect();
        let parts = split(&rest, &[3, 0, 3, 10]);
        assert_eq!(
            parts[3],
            [5u32, 2]
                .iter()
                .flat_map(|v| v.to_le_bytes())
                .collect::<Vec<_>>()
        );
        assert_eq!(parts[0], 9u32.to_le_bytes());
        assert_eq!(parts[10], 7u32.to_le_bytes());
        assert!(parts[1].is_empty());
    }

    #[test]
    fn python_equality() {
        assert!(py_eq(&json!(1), &json!(1.0)));
        assert!(py_eq(&json!(true), &json!(1)));
        assert!(py_eq(
            &json!({"a": [4, 4], "b": null}),
            &json!({"b": null, "a": [4.0, 4]})
        ));
        assert!(!py_eq(&json!("1"), &json!(1)));
        assert!(!py_eq(&json!([1]), &json!([1, 2])));
    }

    #[test]
    fn guide_round_trip() {
        let dir = std::env::temp_dir().join(format!("texcodec-guide-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("g.bin");
        let settings = json!({"scale": 0.5, "block": [4, 4], "mode": "guided", "keep_max": 64});
        let want: serde_json::Map<String, Value> = KEYS
            .iter()
            .map(|k| {
                (
                    k.to_string(),
                    settings.get(*k).cloned().unwrap_or(Value::Null),
                )
            })
            .collect();
        let rest: Vec<u8> = [1u32, 4, 8].iter().flat_map(|v| v.to_le_bytes()).collect();
        let z = miniz_oxide::deflate::compress_to_vec_zlib(&[2, 0, 9], 9);
        let tiers: Vec<Value> = TIERS.iter().map(|(p, l)| json!([p, l.as_json()])).collect();
        let head = json!({"version": 1, "settings": want, "tiers": tiers,
                          "index": {"a|1|0": [0, z.len(), 3, rest_crc(&rest)]}});
        let head = serde_json::to_vec(&head).unwrap();
        let mut bytes = MAGIC.to_vec();
        bytes.extend_from_slice(&(head.len() as u32).to_le_bytes());
        bytes.extend_from_slice(&head);
        bytes.extend_from_slice(&z);
        std::fs::write(&path, bytes).unwrap();
        let mut g = Guide::open(&path, &settings).unwrap();
        assert!(g.ok);
        assert_eq!(g.codes("a|1|0", &rest).unwrap(), Some(vec![2, 0, 9]));
        assert_eq!(g.codes("a|1|0", &rest[..8]).unwrap(), None);
        assert_eq!(g.codes("b|1|0", &rest).unwrap(), None);
        assert_eq!((g.used, g.missed), (1, 2));
        let other = Guide::open(&path, &json!({"scale": 0.375})).unwrap();
        assert!(!other.ok);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
