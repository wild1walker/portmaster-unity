//! xar archives (macOS installer packages), as macpkg.Xar reads them: the zlib-compressed XML
//! table of contents, the files it lists, and their data in the heap after it.
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::sync::Arc;

use flate2::{Decompress, FlushDecompress, Status};

use crate::streams::{self, Region, Stream};
use crate::{fail, io_at, py, Error, Result};

/// A file's <data>: where it is in the heap and how it is encoded.
pub struct Data {
    pub offset: u64,
    pub length: u64,
    /// The <size> text (the size unpacked), as the TOC gives it.
    pub size: Option<String>,
    /// The <encoding> element's style: None without the element (stored as is).
    pub encoding: Option<Option<String>>,
}

/// One file of the archive: its path (the names of the folders it is in, joined with '/').
pub struct Entry {
    pub path: String,
    pub data: Option<Data>,
}

pub struct Xar {
    file: Arc<File>,
    heap: u64,
    /// Every file, folders first, in the TOC's order (macpkg.Xar.files()).
    pub entries: Vec<Entry>,
}

/// zlib.decompress: the whole stream, or the error of one cut short.
fn zlib_decompress(data: &[u8]) -> Result<Vec<u8>> {
    let mut z = Decompress::new(true);
    let mut out = Vec::with_capacity(data.len() * 4);
    loop {
        let pos = z.total_in() as usize;
        if out.len() == out.capacity() {
            out.reserve(out.len().max(1 << 16));
        }
        let st = z
            .decompress_vec(&data[pos..], &mut out, FlushDecompress::Finish)
            .map_err(|e| {
                Error::Fail(format!(
                    "zlib.error: Error -3 while decompressing data: {e}"
                ))
            })?;
        if st == Status::StreamEnd {
            return Ok(out); // (what follows the stream is ignored, as zlib.decompress does)
        }
        if z.total_in() as usize == data.len() && out.len() < out.capacity() {
            return fail(
                "zlib.error: Error -5 while decompressing data: incomplete or truncated stream",
            );
        }
    }
}

fn is(n: &roxmltree::Node, tag: &str) -> bool {
    n.is_element() && n.tag_name().name() == tag && n.tag_name().namespace().is_none()
}

/// ElementTree's find(tag): the first child element of that name.
fn find<'a, 'i>(n: roxmltree::Node<'a, 'i>, tag: &str) -> Option<roxmltree::Node<'a, 'i>> {
    n.children().find(|c| is(c, tag))
}

/// ElementTree's findtext(tag): the child's text ('' when it has none), None without the child.
fn findtext(n: roxmltree::Node, tag: &str) -> Option<String> {
    find(n, tag).map(|c| c.text().unwrap_or("").to_string())
}

/// int(d.findtext(tag)), with the Python's errors.
fn int_of(n: roxmltree::Node, tag: &str) -> Result<i128> {
    match findtext(n, tag) {
        None => fail("TypeError: int() argument must be a string, a bytes-like object or a real number, not 'NoneType'"),
        Some(t) => py::int(&t, 10)
            .ok_or_else(|| Error::Fail(format!("ValueError: invalid literal for int() with base 10: {}", py::str_repr(&t)))),
    }
}

fn u64_of(v: i128, what: &str) -> Result<u64> {
    u64::try_from(v).map_err(|_| Error::Fail(format!("ValueError: negative {what} {v}")))
}

impl Xar {
    /// Reads the TOC; Error::Incomplete when the file is shorter than the data the TOC lists (a
    /// download or copy that stopped early).
    pub fn open(path: &str) -> Result<Xar> {
        let mut f = File::open(path).map_err(io_at(path))?;
        let head = streams::read_upto(&mut f, 28)?;
        if !head.starts_with(b"xar!") {
            return Err(Error::Fail(format!(
                "ValueError: {path}: not a macOS installer package (xar)"
            )));
        }
        if head.len() < 24 {
            return fail("struct.error: unpack requires a buffer of 20 bytes");
        }
        let hsize = u16::from_be_bytes([head[4], head[5]]) as u64;
        let toc_c = u64::from_be_bytes(head[8..16].try_into().unwrap());
        f.seek(SeekFrom::Start(hsize))?;
        let mut toc = Vec::new();
        (&mut f).take(toc_c).read_to_end(&mut toc)?;
        let xml = zlib_decompress(&toc)?;
        let text = String::from_utf8(xml)
            .map_err(|e| Error::Fail(format!("xml.etree.ElementTree.ParseError: {e}")))?;
        let opts = roxmltree::ParsingOptions {
            allow_dtd: true,
            ..Default::default()
        };
        let doc = roxmltree::Document::parse_with_options(&text, opts)
            .map_err(|e| Error::Fail(format!("xml.etree.ElementTree.ParseError: {e}")))?;
        let root = doc.root_element();
        let heap = hsize + toc_c;
        // a download or copy that stopped early: the table of contents is whole but the data it
        // points to is not (GOG's installers keep PackageInfo after the game, at the very end)
        let mut end: i128 = 0;
        for d in root.descendants().filter(|n| is(n, "data")) {
            end = end.max(int_of(d, "offset")? + int_of(d, "length")?);
        }
        let need = heap as i128 + end;
        let have = f.metadata()?.len() as i128;
        if have < need {
            return Err(Error::Incomplete(format!(
                "{} is incomplete: {have} of {need} bytes (a download or copy that stopped early); \
                 download or copy it again",
                py::basename(path)
            )));
        }
        let Some(toc_el) = find(root, "toc") else {
            return fail("AttributeError: 'NoneType' object has no attribute 'findall'");
        };
        let mut entries = Vec::new();
        walk(toc_el, "", &mut entries)?;
        Ok(Xar {
            file: Arc::new(f),
            heap,
            entries,
        })
    }

    /// A reader of a file's data, decoded (Xar.open).
    pub fn reader(&self, e: &Entry) -> Result<Stream> {
        let Some(d) = &e.data else {
            return fail("AttributeError: 'NoneType' object has no attribute 'findtext'");
        };
        let raw = Region::new(self.file.clone(), self.heap + d.offset, d.length);
        match &d.encoding {
            None => Ok(Box::new(raw)),
            Some(Some(s)) if s == "application/octet-stream" => Ok(Box::new(raw)),
            Some(Some(s)) if s == "application/x-gzip" => Ok(Box::new(streams::Inflate::new(raw))),
            Some(Some(s)) if s == "application/x-bzip2" => Ok(Box::new(streams::Bzip2::new(raw))),
            Some(Some(s)) => fail(format!("ValueError: xar encoding {s} not supported")),
            Some(None) => fail("ValueError: xar encoding None not supported"),
        }
    }

    /// A file's data, decoded, whole (Xar.read).
    pub fn read(&self, e: &Entry) -> Result<Vec<u8>> {
        let mut v = Vec::new();
        self.reader(e)?.read_to_end(&mut v)?;
        Ok(v)
    }
}

fn walk(el: roxmltree::Node, prefix: &str, out: &mut Vec<Entry>) -> Result<()> {
    for fe in el.children().filter(|c| is(c, "file")) {
        let p = format!("{prefix}{}", findtext(fe, "name").unwrap_or_default());
        let data = match find(fe, "data") {
            None => None,
            Some(d) => Some(Data {
                offset: u64_of(int_of(d, "offset")?, "offset")?,
                length: u64_of(int_of(d, "length")?, "length")?,
                size: findtext(d, "size"),
                encoding: find(d, "encoding").map(|e| e.attribute("style").map(str::to_string)),
            }),
        };
        out.push(Entry {
            path: p.clone(),
            data,
        });
        walk(fe, &format!("{p}/"), out)?;
    }
    Ok(())
}
