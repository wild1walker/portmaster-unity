//! AndroidManifest.xml as apk.py reads it: Android's binary XML (AXML, an APK's) and aapt2's
//! protobuf XmlNode (an App Bundle's), into a small element tree, and the fields the runtime needs
//! (Manifest). Attribute names without namespace; stripped or obfuscated names by their android:
//! resource ids. Damaged input is an error where the Python raises (its traceback: status 1).
use crate::Fail;

/// android: attribute resource ids (apk.py ATTR_IDS).
pub fn attr_id(id: u64) -> Option<&'static str> {
    Some(match id {
        0x01010003 => "name",
        0x01010024 => "value",
        0x01010025 => "resource",
        0x0101021b => "versionCode",
        0x0101021c => "versionName",
        0x0101020c => "minSdkVersion",
        0x01010270 => "targetSdkVersion",
        0x01010271 => "maxSdkVersion",
        0x01010281 => "glEsVersion",
        0x0101028e => "required",
        0x010104ea => "extractNativeLibs",
        0x0101000c => "hasCode",
        0x01010001 => "label",
        0x01010572 => "compileSdkVersion",
        0x01010591 => "isSplitRequired",
        0x0101055e => "isFeatureSplit",
        0x01010018 => "screenOrientation",
        0x0101001f => "configChanges",
        0x01010000 => "theme",
        _ => return None,
    })
}

/// An element: its tag, its attributes (a dict: a name set again keeps its first place and takes
/// the new value; None is TYPE_NULL), its children.
#[derive(Debug, Default, Clone)]
pub struct Element {
    pub tag: String,
    pub attrs: Vec<(String, Option<String>)>,
    pub children: Vec<Element>,
}

impl Element {
    fn set(&mut self, k: String, v: Option<String>) {
        match self.attrs.iter_mut().find(|(n, _)| *n == k) {
            Some(e) => e.1 = v,
            None => self.attrs.push((k, v)),
        }
    }

    /// el.get(name): the value, None when missing or null.
    pub fn get(&self, name: &str) -> Option<&str> {
        self.attrs
            .iter()
            .find(|(n, _)| n == name)
            .and_then(|(_, v)| v.as_deref())
    }

    /// el.iter(tag): this element and its descendants in document order, those of the tag.
    pub fn iter<'a>(&'a self, tag: Option<&'a str>) -> Vec<&'a Element> {
        let mut out = Vec::new();
        self.walk(tag, &mut out);
        out
    }

    fn walk<'a>(&'a self, tag: Option<&str>, out: &mut Vec<&'a Element>) {
        if tag.is_none_or(|t| t == self.tag) {
            out.push(self);
        }
        for c in &self.children {
            c.walk(tag, out);
        }
    }
}

fn short(what: &str) -> Fail {
    Fail::Error(format!("struct.error: {what}"))
}

fn u16_at(b: &[u8], i: usize) -> Result<u16, Fail> {
    b.get(i..i.checked_add(2).ok_or_else(|| short("offset"))?)
        .map(|s| u16::from_le_bytes([s[0], s[1]]))
        .ok_or_else(|| {
            short(&format!(
                "unpack_from requires a buffer of at least {} bytes",
                i + 2
            ))
        })
}

fn u32_at(b: &[u8], i: usize) -> Result<u32, Fail> {
    b.get(i..i.checked_add(4).ok_or_else(|| short("offset"))?)
        .map(|s| u32::from_le_bytes(s.try_into().unwrap()))
        .ok_or_else(|| {
            short(&format!(
                "unpack_from requires a buffer of at least {} bytes",
                i + 4
            ))
        })
}

fn byte_at(b: &[u8], i: usize) -> Result<u8, Fail> {
    b.get(i)
        .copied()
        .ok_or_else(|| Fail::Error("IndexError: index out of range".into()))
}

/// Python's bytes.decode('utf-16-le', 'replace').
fn utf16_replace(b: &[u8]) -> String {
    let units = b.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]]));
    let mut s: String = char::decode_utf16(units)
        .map(|r| r.unwrap_or(char::REPLACEMENT_CHARACTER))
        .collect();
    if b.len() % 2 == 1 {
        s.push(char::REPLACEMENT_CHARACTER);
    }
    s
}

/// The string pool chunk at off (apk.py _string_pool).
fn string_pool(b: &[u8], off: usize) -> Result<Vec<String>, Fail> {
    let hsize = u16_at(b, off + 2)? as usize;
    u32_at(b, off + 4)?;
    let count = u32_at(b, off + 8)? as usize;
    u32_at(b, off + 12)?;
    let flags = u32_at(b, off + 16)?;
    let start = u32_at(b, off + 20)? as usize;
    u32_at(b, off + 24)?;
    let utf8 = flags & 0x100 != 0;
    let at = off + hsize;
    if at
        .checked_add(count.checked_mul(4).ok_or_else(|| short("count"))?)
        .is_none_or(|e| e > b.len())
    {
        return Err(short(&format!(
            "unpack_from requires a buffer of at least {} bytes",
            at as u128 + 4 * count as u128
        )));
    }
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let o = u32_at(b, at + 4 * i)? as usize;
        let mut p = off + start + o;
        if utf8 {
            let n = byte_at(b, p)?;
            p += 1;
            if n & 0x80 != 0 {
                p += 1;
            }
            let mut n = byte_at(b, p)? as usize;
            p += 1;
            if n & 0x80 != 0 {
                n = ((n & 0x7f) << 8) | byte_at(b, p)? as usize;
                p += 1;
            }
            let s = &b[p.min(b.len())..(p + n).min(b.len())];
            out.push(String::from_utf8_lossy(s).into_owned());
        } else {
            let mut n = u16_at(b, p)? as usize;
            p += 2;
            if n & 0x8000 != 0 {
                n = ((n & 0x7fff) << 16) | u16_at(b, p)? as usize;
                p += 2;
            }
            let s = &b[p.min(b.len())..(p + 2 * n).min(b.len())];
            out.push(utf16_replace(s));
        }
    }
    Ok(out)
}

/// repr() of a float32 read as Python's float.
fn float_repr(bits: u32) -> String {
    androiddata::pyjson::float_repr(f32::from_bits(bits) as f64)
}

fn value(dtype: u8, data: u32, raw: Option<u32>, strings: &[String]) -> Option<String> {
    Some(match dtype {
        0x03 => strings.get(data as usize).cloned().unwrap_or_default(),
        0x12 => (if data != 0 { "true" } else { "false" }).into(),
        0x10 => (data as i32).to_string(),
        0x11 => format!("0x{data:x}"),
        0x04 => float_repr(data),
        0x01 | 0x07 => format!("@0x{data:08x}"),
        0x00 => return None,
        _ => match raw {
            Some(r) if (r as usize) < strings.len() => strings[r as usize].clone(),
            _ => format!("0x{data:x}"),
        },
    })
}

/// The element tree of a binary AndroidManifest.xml (apk.py parse_axml).
pub fn parse_axml(b: &[u8]) -> Result<Element, Fail> {
    if b.len() < 8 || u16_at(b, 0)? != 0x0003 {
        return Err(Fail::Error(
            "ValueError: not an Android binary XML file".into(),
        ));
    }
    let mut strings: Vec<String> = Vec::new();
    let mut resmap: Vec<u32> = Vec::new();
    let mut root: Option<Element> = None;
    // the open elements: a path of child indices from the root
    let mut stack: Vec<usize> = Vec::new();
    let mut open = 0usize; // how many elements are open (the root counts)
    let hsize = u16_at(b, 2)? as usize;
    let total = u32_at(b, 4)? as usize;
    let mut off = hsize;
    let end = total.min(b.len());
    while off + 8 <= end {
        let ctype = u16_at(b, off)?;
        let chsize = u16_at(b, off + 2)? as usize;
        let csize = u32_at(b, off + 4)? as usize;
        if csize < 8 || off + csize > end {
            break;
        }
        match ctype {
            0x0001 => strings = string_pool(b, off)?,
            0x0180 => {
                if chsize > csize {
                    return Err(short("bad char in struct format"));
                }
                let n = (csize - chsize) / 4;
                resmap = (0..n)
                    .map(|i| u32_at(b, off + chsize + 4 * i))
                    .collect::<Result<_, _>>()?;
            }
            0x0102 => {
                let ext = off + chsize;
                u32_at(b, ext)?;
                let name = u32_at(b, ext + 4)? as usize;
                let astart = u16_at(b, ext + 8)? as usize;
                let asize = u16_at(b, ext + 10)? as usize;
                let acount = u16_at(b, ext + 12)? as usize;
                let mut el = Element {
                    tag: strings.get(name).cloned().unwrap_or_else(|| "?".into()),
                    ..Default::default()
                };
                for i in 0..acount {
                    let a = ext + astart + i * asize;
                    u32_at(b, a)?;
                    let aname = u32_at(b, a + 4)? as usize;
                    let raw = u32_at(b, a + 8)?;
                    u16_at(b, a + 12)?;
                    let dtype = byte_at(b, a + 15)?;
                    let data = u32_at(b, a + 16)?;
                    let key = resmap
                        .get(aname)
                        .and_then(|&id| attr_id(id as u64))
                        .map(str::to_string)
                        .unwrap_or_else(|| match strings.get(aname) {
                            Some(s) => s.clone(),
                            None => format!("attr{aname}"),
                        });
                    let v = value(dtype, data, (raw != 0xffff_ffff).then_some(raw), &strings);
                    el.set(key, v);
                }
                if open > 0 {
                    let parent = node_at(root.as_mut().unwrap(), &stack);
                    parent.children.push(el);
                    stack.push(parent.children.len() - 1);
                    open += 1;
                } else if root.is_none() {
                    root = Some(el);
                    open = 1;
                }
                // (a second top-level element after the root closed is not kept, nor pushed)
            }
            0x0103 if open > 0 => {
                open -= 1;
                if open > 0 {
                    stack.pop();
                }
            }
            _ => {}
        }
        off += csize;
    }
    root.ok_or_else(|| Fail::Error("ValueError: binary XML without elements".into()))
}

fn node_at<'a>(root: &'a mut Element, path: &[usize]) -> &'a mut Element {
    let mut e = root;
    for &i in path {
        e = &mut e.children[i];
    }
    e
}

// ------------------------------------------------------------------ protobuf XML (App Bundles)

/// A protobuf field's value: a varint or fixed number, or bytes.
#[derive(Clone, Copy)]
enum Pb<'a> {
    Int(u128),
    Bytes(&'a [u8]),
}

fn type_error(what: &str) -> Fail {
    Fail::Error(format!("TypeError: {what}"))
}

impl<'a> Pb<'a> {
    fn bytes(self) -> Result<&'a [u8], Fail> {
        match self {
            Pb::Bytes(b) => Ok(b),
            Pb::Int(_) => Err(type_error("object of type 'int' has no len()")),
        }
    }

    fn int(self) -> Result<u128, Fail> {
        match self {
            Pb::Int(v) => Ok(v),
            Pb::Bytes(_) => Err(type_error("bytes where an integer was expected")),
        }
    }
}

/// [(field number, value)] of one message (apk.py _pb_fields).
fn pb_fields(b: &[u8]) -> Result<Vec<(u128, Pb<'_>)>, Fail> {
    let mut out = Vec::new();
    let mut p = 0usize;
    let varint = |p: &mut usize| -> Result<u128, Fail> {
        let (mut v, mut s) = (0u128, 0u32);
        loop {
            let c = byte_at(b, *p)?;
            *p += 1;
            if s < 128 {
                v |= ((c & 0x7f) as u128) << s;
            }
            s += 7;
            if c & 0x80 == 0 {
                return Ok(v);
            }
        }
    };
    while p < b.len() {
        let key = varint(&mut p)?;
        let (f, wt) = (key >> 3, key & 7);
        match wt {
            0 => out.push((f, Pb::Int(varint(&mut p)?))),
            1 => {
                let s = b
                    .get(p..p + 8)
                    .ok_or_else(|| short("unpack_from requires a buffer of at least 8 bytes"))?;
                out.push((
                    f,
                    Pb::Int(u64::from_le_bytes(s.try_into().unwrap()) as u128),
                ));
                p += 8;
            }
            2 => {
                let n = varint(&mut p)?;
                let e = (p as u128).saturating_add(n).min(b.len() as u128) as usize;
                out.push((f, Pb::Bytes(&b[p.min(b.len())..e.max(p.min(b.len()))])));
                p = (p as u128).saturating_add(n).min(usize::MAX as u128) as usize;
            }
            5 => {
                out.push((f, Pb::Int(u32_at(b, p)? as u128)));
                p += 4;
            }
            _ => return Err(Fail::Error(format!("ValueError: protobuf wire type {wt}"))),
        }
    }
    Ok(out)
}

fn utf8_replace(b: &[u8]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

fn pb_prim(b: &[u8]) -> Result<Option<String>, Fail> {
    for (f, v) in pb_fields(b)? {
        match f {
            8 => {
                let truthy = match v {
                    Pb::Int(i) => i != 0,
                    Pb::Bytes(b) => !b.is_empty(),
                };
                return Ok(Some((if truthy { "true" } else { "false" }).into()));
            }
            6 => {
                let v = v.int()?;
                return Ok(Some(if v >= 1 << 63 {
                    (v as i128 - (1i128 << 64)).to_string()
                } else {
                    v.to_string()
                }));
            }
            7 => return Ok(Some(format!("0x{:x}", v.int()?))),
            3 => {
                let v = v.int()?;
                let bits = u32::try_from(v)
                    .map_err(|_| short("'I' format requires 0 <= number <= 4294967295"))?;
                return Ok(Some(float_repr(bits)));
            }
            _ => {}
        }
    }
    Ok(None)
}

fn pb_item(b: &[u8]) -> Result<Option<String>, Fail> {
    for (f, v) in pb_fields(b)? {
        match f {
            7 => return pb_prim(v.bytes()?),
            1 => {
                let mut rid = 0u128;
                for (g, x) in pb_fields(v.bytes()?)? {
                    if g == 1 {
                        rid = x.int()?;
                        break;
                    }
                }
                return Ok(Some(format!("@0x{rid:08x}")));
            }
            2 | 3 => {
                for (g, x) in pb_fields(v.bytes()?)? {
                    if g == 1 {
                        return Ok(Some(utf8_replace(x.bytes()?)));
                    }
                }
                return Ok(Some(String::new()));
            }
            _ => {}
        }
    }
    Ok(None)
}

/// Python's recursion limit, near enough: a manifest nested this deep is not a real one.
const MAX_DEPTH: usize = 900;

fn pb_element(eb: &[u8], depth: usize) -> Result<Element, Fail> {
    if depth > MAX_DEPTH {
        return Err(Fail::Error(
            "RecursionError: maximum recursion depth exceeded".into(),
        ));
    }
    let mut el = Element {
        tag: "?".into(),
        ..Default::default()
    };
    for (f, v) in pb_fields(eb)? {
        match f {
            3 => el.tag = utf8_replace(v.bytes()?),
            4 => {
                let (mut aname, mut aval, mut rid, mut item) =
                    (String::new(), None, Pb::Int(0), None);
                for (g, x) in pb_fields(v.bytes()?)? {
                    match g {
                        2 => aname = utf8_replace(x.bytes()?),
                        3 => aval = Some(utf8_replace(x.bytes()?)),
                        5 => rid = x,
                        6 => item = pb_item(x.bytes()?)?,
                        _ => {}
                    }
                }
                // (ATTR_IDS.get(rid): a resource id of bytes is no key of it)
                let known = match rid {
                    Pb::Int(r) => u64::try_from(r).ok().and_then(attr_id),
                    Pb::Bytes(_) => None,
                };
                let key = match known {
                    Some(k) => k.to_string(),
                    None => aname,
                };
                el.set(key, item.or(aval));
            }
            5 => {
                for (g, x) in pb_fields(v.bytes()?)? {
                    if g == 1 {
                        el.children.push(pb_element(x.bytes()?, depth + 1)?);
                    }
                }
            }
            _ => {}
        }
    }
    Ok(el)
}

/// The element tree of an App Bundle's AndroidManifest.xml (apk.py parse_proto_xml).
pub fn parse_proto_xml(b: &[u8]) -> Result<Element, Fail> {
    for (f, v) in pb_fields(b)? {
        if f == 1 {
            return pb_element(v.bytes()?, 0);
        }
    }
    Err(Fail::Error(
        "ValueError: protobuf XML without a root element".into(),
    ))
}

/// parse_manifest: AXML when it starts so, else protobuf.
pub fn parse_manifest(b: &[u8]) -> Result<Element, Fail> {
    if b.starts_with(b"\x03\x00") {
        parse_axml(b)
    } else {
        parse_proto_xml(b)
    }
}

/// int(v, 0) of a string, None where Python raises (apk.py _int).
pub fn int0(v: Option<&str>) -> Option<i128> {
    let t = pkgzip::py::strip(v?);
    let (neg, t) = match t.as_bytes().first() {
        Some(b'-') => (true, &t[1..]),
        Some(b'+') => (false, &t[1..]),
        _ => (false, t),
    };
    let low = t.get(..2).map(str::to_ascii_lowercase);
    let (radix, body) = match low.as_deref() {
        Some("0x") => (16, &t[2..]),
        Some("0o") => (8, &t[2..]),
        Some("0b") => (2, &t[2..]),
        _ => (10, t),
    };
    if radix == 10 {
        // (base 0: a decimal number may not start with 0 unless it is all zeros)
        let digits: Vec<char> = body.chars().filter(|&c| c != '_').collect();
        if digits.first() == Some(&'0') && digits.iter().any(|&c| c != '0') {
            return None;
        }
        if body.starts_with('_') {
            return None;
        }
    }
    let text = if radix == 10 {
        body.to_string()
    } else {
        // int() takes an underscore right after the prefix
        format!(
            "0{}{}",
            ['x', 'o', 'b'][[16, 8, 2].iter().position(|&r| r == radix).unwrap()],
            body
        )
    };
    let v = pkgzip::py::int(&text, radix)?;
    Some(if neg { -v } else { v })
}

/// What the runtime needs from AndroidManifest.xml (apk.py Manifest).
#[derive(Debug, Clone)]
pub struct Manifest {
    pub root: Element,
    pub package: String,
    pub version_code: i128,
    pub version_name: String,
    pub split: Option<String>,
    pub min_sdk: Option<i128>,
    pub target_sdk: Option<i128>,
    /// application's meta-data: (name, value or resource), a dict's order.
    pub meta: Vec<(String, Option<String>)>,
    pub activities: Vec<Option<String>>,
    pub categories: Vec<String>,
    pub gles: Option<i128>,
    pub features: Vec<(String, bool)>,
}

impl Manifest {
    pub fn new(root: Element) -> Result<Manifest, Fail> {
        if root.tag != "manifest" {
            return Err(Fail::Error(format!(
                "ValueError: manifest root is <{}>",
                root.tag
            )));
        }
        let nonempty = |v: Option<&str>| v.filter(|s| !s.is_empty()).map(str::to_string);
        let package = nonempty(root.get("package")).unwrap_or_default();
        let version_code = int0(root.get("versionCode"))
            .filter(|&v| v != 0)
            .unwrap_or(0);
        let version_name = nonempty(root.get("versionName")).unwrap_or_default();
        let split = root.get("split").map(str::to_string);
        let us = root.iter(Some("uses-sdk")).first().copied();
        let min_sdk = us.and_then(|u| int0(u.get("minSdkVersion")));
        let target_sdk = us.and_then(|u| int0(u.get("targetSdkVersion")));
        let mut meta: Vec<(String, Option<String>)> = Vec::new();
        if let Some(app) = root.iter(Some("application")).first() {
            for m in &app.children {
                let Some(name) = m.get("name").filter(|n| !n.is_empty()) else {
                    continue;
                };
                if m.tag != "meta-data" {
                    continue;
                }
                let v = m
                    .get("value")
                    .or_else(|| m.get("resource"))
                    .map(str::to_string);
                match meta.iter_mut().find(|(k, _)| k == name) {
                    Some(e) => e.1 = v,
                    None => meta.push((name.to_string(), v)),
                }
            }
        }
        let activities = root
            .iter(Some("activity"))
            .iter()
            .map(|a| a.get("name").map(str::to_string))
            .collect();
        let categories = root
            .iter(Some("category"))
            .iter()
            .filter_map(|c| c.get("name").filter(|n| !n.is_empty()).map(str::to_string))
            .collect();
        let (mut gles, mut features) = (None, Vec::new());
        for f in root.iter(Some("uses-feature")) {
            if f.get("glEsVersion").is_some_and(|v| !v.is_empty()) {
                gles = int0(f.get("glEsVersion"));
            }
            if let Some(n) = f.get("name").filter(|n| !n.is_empty()) {
                features.push((n.to_string(), f.get("required") != Some("false")));
            }
        }
        Ok(Manifest {
            package,
            version_code,
            version_name,
            split,
            min_sdk,
            target_sdk,
            meta,
            activities,
            categories,
            gles,
            features,
            root,
        })
    }

    /// mf.meta.get(name).
    pub fn meta_get(&self, name: &str) -> Option<&str> {
        self.meta
            .iter()
            .find(|(k, _)| k == name)
            .and_then(|(_, v)| v.as_deref())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn int0_as_python() {
        for (s, v) in [
            ("7", Some(7)),
            (" -12 ", Some(-12)),
            ("0x1f", Some(31)),
            ("0X_1F", Some(31)),
            ("0o17", Some(15)),
            ("0b101", Some(5)),
            ("0", Some(0)),
            ("00", Some(0)),
            ("0_0", Some(0)),
            ("017", None),
            ("1_000", Some(1000)),
            ("_1", None),
            ("", None),
            ("x", None),
            ("1.5", None),
        ] {
            assert_eq!(int0(Some(s)), v, "{s:?}");
        }
        assert_eq!(int0(None), None);
    }

    #[test]
    fn utf16_odd_and_lone_surrogates() {
        assert_eq!(utf16_replace(b"a\0b"), "a\u{fffd}");
        assert_eq!(utf16_replace(&[0x00, 0xd8, b'a', 0]), "\u{fffd}a");
    }

    #[test]
    fn protobuf_errors() {
        // a truncated varint, an unknown wire type, no root element
        assert!(parse_proto_xml(&[0x80]).is_err());
        assert!(parse_proto_xml(&[0x0b]).is_err());
        assert!(parse_proto_xml(&[]).is_err());
        assert!(parse_axml(b"<manifest/>").is_err());
    }
}
