//! UnityFS bundles (formats 6 and 7), read and written as the Python does: UnityPy's BundleFile
//! rules for the header (the flags by engine version, the 16-byte alignment of format 7 and
//! 2019.4.15+, blocks info at the end or after the header, the padding before the blocks), its
//! blocks read one at a time where a read needs them (bigbundle.Source: never the whole bundle),
//! and the writer of UnityPy's save(packer=...) and bigbundle.pack (the same bytes: header, LZ4
//! HC chunks of 128 KiB, a chunk kept raw when LZ4 does not make it smaller, the blocks info).
use crate::bin::{Reader, Writer};
use crate::compress;
use crate::error::{bad, Error, Result};
use crate::source::{FileSource, SharedSource, Source};
use crate::version::UnityVersion;
use std::collections::VecDeque;
use std::io::{self, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::{Arc, Mutex};

/// UnityPy's LZ4 chunk (COMPRESSION_CHUNK_SIZE_MAP).
pub const CHUNK: usize = 0x20000;
/// UnityPy's save(packer='lz4') and bigbundle.pack: blocks info at the end, directory, LZ4.
pub const LZ4_DATA_FLAG: u32 = 194;
pub const LZ4_BLOCK_FLAG: u32 = 2;
/// Blocks a lazy reader decompresses as reads reach them (bigbundle.LAZY_MAX_BLOCK); a bundle
/// with bigger ones (one LZMA block) is unpacked into files instead.
pub const LAZY_MAX_BLOCK: u32 = 4 << 20;

/// A block of the bundle's data.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BlockInfo {
    pub usize: u32,
    pub csize: u32,
    pub flags: u16,
}

/// A node (a file in the bundle): its range of the uncompressed data.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NodeInfo {
    pub offset: i64,
    pub size: i64,
    pub flags: u32,
    pub name: String,
}

/// A UnityFS bundle's header, blocks and nodes.
#[derive(Clone, Debug)]
pub struct Header {
    pub signature: String,
    pub format: u32,
    pub player: String,
    pub engine: String,
    pub size: i64,
    pub ci_size: u32,
    pub ui_size: u32,
    pub flags: u32,
    /// Engines before 2020.3.34 / 2021.3.2 / 2022.1.1: flag 0x200 is Unity CN's encryption,
    /// not BlockInfoNeedPaddingAtStart (UnityPy's ArchiveFlagsOld).
    pub old_flags: bool,
    /// The header is padded to 16 bytes (format 7+, or 2019.4.15+): UnityPy's _uses_block_alignment.
    pub aligned: bool,
    pub hash: [u8; 16],
    pub blocks: Vec<BlockInfo>,
    pub nodes: Vec<NodeInfo>,
    /// File offset of the first block.
    pub data_start: u64,
}

fn engine_triple(engine: &str) -> (u64, u64, u64) {
    UnityVersion::parse(engine)
        .map(|v| v.triple())
        .unwrap_or((0, 0, 0))
}

/// Whether an engine version uses the old flag set (UnityPy's rule).
pub fn old_flags(engine: &str) -> bool {
    let v = engine_triple(engine);
    v.0 < 2020
        || (v.0 == 2020 && v < (2020, 3, 34))
        || (v.0 == 2021 && v < (2021, 3, 2))
        || (v.0 == 2022 && v < (2022, 1, 1))
}

impl Header {
    /// Reads a bundle's header, blocks info and nodes.
    pub fn read(src: &dyn Source) -> Result<Self> {
        let first = src.read_clipped(0, 4096)?;
        let mut r = Reader::new(&first, true);
        let signature = r.cstring()?;
        if signature != "UnityFS" {
            return Err(Error::Unsupported(format!(
                "bundle signature {signature:?}"
            )));
        }
        let format = r.u32()?;
        let player = r.cstring()?;
        let engine = r.cstring()?;
        let size = r.i64()?;
        let ci_size = r.u32()?;
        let ui_size = r.u32()?;
        let flags = r.u32()?;
        let old = old_flags(&engine);
        if (old && flags & 0x200 != 0) || (!old && flags & 0x400 != 0) {
            return Err(Error::Unsupported("an encrypted bundle".into()));
        }
        let v = engine_triple(&engine);
        let aligned = format >= 7 || (v.0 == 2019 && v >= (2019, 4, 15));
        if aligned {
            r.align(16);
        }
        let start = r.pos as u64;
        let ci = if flags & 0x80 != 0 {
            let len = src.len();
            if (ci_size as u64) > len {
                return bad("bundle: blocks info past the start");
            }
            src.read_vec(len - ci_size as u64, ci_size as usize)?
        } else {
            src.read_vec(start, ci_size as usize)?
        };
        let info = compress::decompress(&ci, ui_size as usize, flags)?;
        let mut ir = Reader::new(&info, true);
        let hash: [u8; 16] = ir.take(16)?.try_into().unwrap();
        let nb = ir.i32()?;
        let mut blocks = Vec::with_capacity(nb.max(0) as usize);
        for _ in 0..nb.max(0) {
            blocks.push(BlockInfo {
                usize: ir.u32()?,
                csize: ir.u32()?,
                flags: ir.u16()?,
            });
        }
        let nn = ir.i32()?;
        let mut nodes = Vec::with_capacity(nn.max(0) as usize);
        for _ in 0..nn.max(0) {
            let offset = ir.i64()?;
            let size = ir.i64()?;
            let nflags = ir.u32()?;
            let name = ir.cstring()?;
            nodes.push(NodeInfo {
                offset,
                size,
                flags: nflags,
                name,
            });
        }
        let mut data_start = if flags & 0x80 != 0 {
            start
        } else {
            start + ci_size as u64
        };
        if !old && flags & 0x200 != 0 {
            data_start += (16 - data_start % 16) % 16;
        }
        Ok(Header {
            signature,
            format,
            player,
            engine,
            size,
            ci_size,
            ui_size,
            flags,
            old_flags: old,
            aligned,
            hash,
            blocks,
            nodes,
            data_start,
        })
    }

    /// The bytes of the nodes (what UnityPy holds in memory: bigbundle.size).
    pub fn nodes_size(&self) -> u64 {
        self.nodes.iter().map(|n| n.size.max(0) as u64).sum()
    }

    /// The uncompressed data's length (the blocks').
    pub fn data_size(&self) -> u64 {
        self.blocks.iter().map(|b| b.usize as u64).sum()
    }

    /// The nodes UnityPy's bigbundle opens as serialized files: flagged 4, else every node that
    /// is not a .resS / .resource stream (bigbundle.serialized_nodes).
    pub fn serialized_nodes(&self) -> Vec<usize> {
        let f: Vec<usize> = (0..self.nodes.len())
            .filter(|&i| self.nodes[i].flags & 4 != 0)
            .collect();
        if !f.is_empty() {
            return f;
        }
        (0..self.nodes.len())
            .filter(|&i| {
                !(self.nodes[i].name.ends_with(".resS")
                    || self.nodes[i].name.ends_with(".resource"))
            })
            .collect()
    }
}

/// Whether a file starts like a UnityFS bundle.
pub fn is_unityfs(path: impl AsRef<Path>) -> bool {
    let mut b = [0u8; 8];
    std::fs::File::open(path)
        .and_then(|mut f| io::Read::read_exact(&mut f, &mut b))
        .is_ok()
        && &b == b"UnityFS\0"
}

/// A bundle's uncompressed data, read where it is: a block is decompressed when a read reaches
/// it and the last few are kept (bigbundle.Source).
pub struct Blocks {
    file: SharedSource,
    /// (uncompressed offset, usize, file offset, csize, flags)
    table: Vec<(u64, u32, u64, u32, u16)>,
    size: u64,
    cache: Mutex<VecDeque<(usize, Arc<Vec<u8>>)>>,
}

impl Blocks {
    /// Blocks kept decompressed.
    pub const KEEP: usize = 4;

    pub fn new(file: SharedSource, h: &Header) -> Self {
        let mut table = Vec::with_capacity(h.blocks.len());
        let (mut u, mut fp) = (0u64, h.data_start);
        for b in &h.blocks {
            table.push((u, b.usize, fp, b.csize, b.flags));
            u += b.usize as u64;
            fp += b.csize as u64;
        }
        Blocks {
            file,
            table,
            size: u,
            cache: Mutex::new(VecDeque::new()),
        }
    }

    /// Block i decompressed (exactly its usize bytes).
    pub fn block(&self, i: usize) -> Result<Arc<Vec<u8>>> {
        {
            let mut c = self.cache.lock().unwrap();
            if let Some(k) = c.iter().position(|(j, _)| *j == i) {
                let e = c.remove(k).unwrap();
                let b = e.1.clone();
                c.push_back(e);
                return Ok(b);
            }
        }
        let (_, usize, fp, csize, flags) = self.table[i];
        let raw = self
            .file
            .read_vec(fp, csize as usize)
            .map_err(|e| Error::Format(format!("block {i} ends early: {e}")))?;
        let b = Arc::new(compress::decompress(&raw, usize as usize, flags as u32)?);
        if b.len() != usize as usize {
            return bad(format!(
                "block {i} decompressed to {} bytes, not {usize}",
                b.len()
            ));
        }
        let mut c = self.cache.lock().unwrap();
        c.push_back((i, b.clone()));
        while c.len() > Self::KEEP {
            c.pop_front();
        }
        Ok(b)
    }

    /// The block holding uncompressed offset off.
    fn block_at(&self, off: u64) -> usize {
        self.table.partition_point(|b| b.0 <= off).saturating_sub(1)
    }

    pub fn block_count(&self) -> usize {
        self.table.len()
    }
}

impl Source for Blocks {
    fn len(&self) -> u64 {
        self.size
    }

    fn read_at(&self, off: u64, buf: &mut [u8]) -> io::Result<()> {
        if off
            .checked_add(buf.len() as u64)
            .is_none_or(|e| e > self.size)
        {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "read past the bundle's data",
            ));
        }
        let mut done = 0usize;
        let mut i = self.block_at(off);
        while done < buf.len() {
            let pos = off + done as u64;
            let (u, usize, ..) = self.table[i];
            if pos >= u + usize as u64 {
                i += 1;
                continue;
            }
            let b = self.block(i)?;
            let lo = (pos - u) as usize;
            let n = (buf.len() - done).min(usize as usize - lo);
            buf[done..done + n].copy_from_slice(&b[lo..lo + n]);
            done += n;
            i += 1;
        }
        Ok(())
    }
}

/// An open bundle: its header and its data read in place.
pub struct Bundle {
    pub header: Header,
    pub file: SharedSource,
    pub data: Arc<Blocks>,
}

impl Bundle {
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let file: SharedSource = Arc::new(FileSource::open(path)?);
        Self::from_source(file)
    }

    pub fn from_source(file: SharedSource) -> Result<Self> {
        let header = Header::read(&*file)?;
        let data = Arc::new(Blocks::new(file.clone(), &header));
        let end = header
            .nodes
            .iter()
            .map(|n| (n.offset.max(0) as u64).saturating_add(n.size.max(0) as u64))
            .max()
            .unwrap_or(0);
        if end > data.len() {
            return bad(format!(
                "the nodes reach past the blocks' {} bytes",
                data.len()
            ));
        }
        Ok(Bundle { header, file, data })
    }

    /// A node's bytes as a source of their own (read from the blocks as needed).
    pub fn node(&self, i: usize) -> SharedSource {
        let n = &self.header.nodes[i];
        Arc::new(crate::source::SubSource {
            inner: self.data.clone(),
            offset: n.offset as u64,
            size: n.size as u64,
        })
    }

    pub fn node_index(&self, name: &str) -> Option<usize> {
        self.header.nodes.iter().position(|n| n.name == name)
    }

    /// Whether every block can be read lazily (bigbundle's lazy rule: blocks up to 4 MB).
    pub fn lazy_ok(&self) -> bool {
        self.header
            .blocks
            .iter()
            .all(|b| b.usize <= LAZY_MAX_BLOCK && (b.flags & 0x3F) <= 3)
    }

    /// Every node written to a file in `dir` (bigbundle.unpack without lazy): the blocks
    /// decompressed one after the other, an LZMA block in pieces as it decompresses; the node
    /// files are named as bigbundle names them. Returns the files, in the nodes' order.
    pub fn unpack(&self, dir: &Path) -> Result<Vec<std::path::PathBuf>> {
        std::fs::create_dir_all(dir)?;
        let h = &self.header;
        let mut paths = Vec::new();
        let mut files = Vec::new();
        for (i, n) in h.nodes.iter().enumerate() {
            let safe = !n.name.is_empty()
                && n.name
                    .bytes()
                    .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
                && n.name != "."
                && n.name != "..";
            let p = dir.join(if safe {
                n.name.clone()
            } else {
                format!("node{i}")
            });
            files.push(io::BufWriter::with_capacity(
                1 << 20,
                std::fs::File::create(&p)?,
            ));
            paths.push(p);
        }
        let mut order: Vec<usize> = (0..h.nodes.len()).collect();
        order.sort_by_key(|&i| h.nodes[i].offset);
        struct Spread<'a> {
            pos: u64,
            nodes: &'a [NodeInfo],
            order: &'a [usize],
            files: &'a mut [io::BufWriter<std::fs::File>],
        }
        impl Write for Spread<'_> {
            fn write(&mut self, data: &[u8]) -> io::Result<usize> {
                let end = self.pos + data.len() as u64;
                for &i in self.order {
                    let (off, sz) = (self.nodes[i].offset as u64, self.nodes[i].size as u64);
                    let lo = off.max(self.pos);
                    let hi = (off + sz).min(end);
                    if lo < hi {
                        self.files[i]
                            .write_all(&data[(lo - self.pos) as usize..(hi - self.pos) as usize])?;
                    }
                }
                self.pos = end;
                Ok(data.len())
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let mut sp = Spread {
            pos: 0,
            nodes: &h.nodes,
            order: &order,
            files: &mut files,
        };
        for (i, b) in h.blocks.iter().enumerate() {
            let start = sp.pos;
            let fp = self.data.table[i].2;
            match b.flags as u32 & 0x3F {
                compress::LZMA if b.usize > LAZY_MAX_BLOCK => {
                    let mut rd = io::BufReader::with_capacity(
                        1 << 20,
                        crate::source::SourceReader {
                            src: &*self.file,
                            pos: fp,
                            end: fp + b.csize as u64,
                        },
                    );
                    compress::lzma_decompress_to(&mut rd, b.usize as u64, &mut sp)?;
                }
                compress::NONE if b.usize > LAZY_MAX_BLOCK => {
                    self.file.copy_to(fp, b.csize as u64, &mut sp)?;
                }
                _ => {
                    let d = self.data.block(i)?;
                    sp.write_all(&d)?;
                }
            }
            if sp.pos - start != b.usize as u64 {
                return bad(format!(
                    "a block decompressed to {} bytes, not {}",
                    sp.pos - start,
                    b.usize
                ));
            }
        }
        for f in files.iter_mut() {
            f.flush()?;
        }
        drop(files);
        for (i, n) in h.nodes.iter().enumerate() {
            let got = std::fs::metadata(&paths[i])?.len();
            if got != n.size as u64 {
                return bad(format!(
                    "node {}: {got} bytes unpacked, not {}",
                    n.name, n.size
                ));
            }
        }
        Ok(paths)
    }
}

/// A node to write into a bundle: its name, flags and bytes.
pub struct PackNode {
    pub name: String,
    pub flags: u32,
    pub src: SharedSource,
}

/// What a written bundle's header says besides the sizes.
#[derive(Clone, Debug)]
pub struct PackHeader {
    pub signature: String,
    pub format: u32,
    pub player: String,
    pub engine: String,
    /// Pad the header to 16 bytes (the source bundle's alignment rule).
    pub aligned: bool,
}

impl PackHeader {
    pub fn of(h: &Header) -> Self {
        PackHeader {
            signature: h.signature.clone(),
            format: h.format,
            player: h.player.clone(),
            engine: h.engine.clone(),
            aligned: h.aligned,
        }
    }
}

/// The nodes' bytes concatenated, in pieces of `size` (the last one shorter).
fn chunks<'a>(nodes: &'a [PackNode], size: usize) -> impl Iterator<Item = Result<Vec<u8>>> + 'a {
    let mut ni = 0usize;
    let mut np = 0u64;
    let mut done = false;
    std::iter::from_fn(move || {
        if done {
            return None;
        }
        let mut buf = Vec::with_capacity(size);
        while buf.len() < size && ni < nodes.len() {
            let n = &nodes[ni];
            let left = n.src.len() - np;
            if left == 0 {
                ni += 1;
                np = 0;
                continue;
            }
            let k = (size - buf.len()).min(left as usize);
            let at = buf.len();
            buf.resize(at + k, 0);
            if let Err(e) = n.src.read_at(np, &mut buf[at..]) {
                done = true;
                return Some(Err(e.into()));
            }
            np += k as u64;
        }
        if buf.is_empty() {
            done = true;
            return None;
        }
        Some(Ok(buf))
    })
}

/// Writes a bundle as UnityPy's save_fs(data_flag, block_info_flag) does (packer='lz4' is
/// LZ4_DATA_FLAG, LZ4_BLOCK_FLAG: bigbundle.pack's layout). LZ4 chunks are compressed on
/// `threads` threads (the same bytes). With blocks info at the end (0x80) the blocks are written
/// as they are compressed, else they are held until the blocks info is written before them.
pub fn pack<W: Write + Seek>(
    out: &mut W,
    h: &PackHeader,
    nodes: &[PackNode],
    data_flag: u32,
    block_flag: u32,
    threads: usize,
) -> Result<()> {
    if data_flag & 0x40 == 0 {
        return Err(Error::Unsupported(
            "UnityPy always writes DirectoryInfo, so data_flag must include 0x40".into(),
        ));
    }
    if h.signature != "UnityFS" {
        return Err(Error::Unsupported(format!(
            "writing a {} bundle",
            h.signature
        )));
    }
    let base = out.stream_position()?;
    let mut head = Writer::new(true);
    head.cstr(h.signature.as_bytes());
    head.u32(h.format);
    head.cstr(h.player.as_bytes());
    head.cstr(h.engine.as_bytes());
    let size_pos = head.len();
    head.i64(0);
    head.u32(0);
    head.u32(0);
    head.u32(data_flag);
    if h.aligned {
        head.align(16);
    }
    let at_end = data_flag & 0x80 != 0;
    let switch = block_flag & 0x3F;
    let mut blocks: Vec<(u32, u32, u32)> = Vec::new();
    // the blocks, to `out` (at the end) or to memory
    let mut held: Vec<u8> = Vec::new();
    out.write_all(&head.buf)?;
    let mut pos = head.len() as u64;
    if at_end && data_flag & 0x200 != 0 {
        let pad = (16 - pos % 16) % 16;
        out.write_all(&vec![0u8; pad as usize])?;
        pos += pad;
    }
    {
        let mut emit = |b: &[u8]| -> Result<()> {
            if at_end {
                out.write_all(b)?;
            } else {
                held.extend_from_slice(b);
            }
            Ok(())
        };
        match switch {
            compress::NONE => {
                // one block of everything (chunk_based_compress)
                let total: u64 = nodes.iter().map(|n| n.src.len()).sum();
                for c in chunks(nodes, 1 << 20) {
                    emit(&c?)?;
                }
                blocks.push((total as u32, total as u32, block_flag));
            }
            compress::LZ4 | compress::LZ4HC => {
                compress::compress_chunks(
                    chunks(nodes, CHUNK),
                    threads,
                    compress::compress_lz4,
                    |c, z| {
                        let raw = if c.len() == CHUNK {
                            z.len() >= CHUNK
                        } else {
                            z.len() > c.len()
                        };
                        if raw {
                            emit(&c)?;
                            blocks.push((c.len() as u32, c.len() as u32, block_flag ^ switch));
                        } else {
                            emit(&z)?;
                            blocks.push((c.len() as u32, z.len() as u32, block_flag));
                        }
                        Ok(())
                    },
                )?;
            }
            k => {
                return Err(Error::Unsupported(format!(
                    "writing bundle blocks of compression {k}"
                )))
            }
        }
    }
    let mut info = Writer::new(true);
    info.bytes(&[0u8; 16]);
    info.i32(blocks.len() as i32);
    for &(u, c, f) in &blocks {
        info.u32(u);
        info.u32(c);
        info.u16(f as u16);
    }
    info.i32(nodes.len() as i32);
    let mut off = 0i64;
    for n in nodes {
        let sz = n.src.len() as i64;
        info.i64(off);
        info.i64(sz);
        info.u32(n.flags);
        info.cstr(n.name.as_bytes());
        off += sz;
    }
    let cinfo = compress::compress(&info.buf, data_flag & 0x3F)?;
    if at_end {
        out.write_all(&cinfo)?;
    } else {
        out.write_all(&cinfo)?;
        pos += cinfo.len() as u64;
        if data_flag & 0x200 != 0 {
            let pad = (16 - pos % 16) % 16;
            out.write_all(&vec![0u8; pad as usize])?;
        }
        out.write_all(&held)?;
    }
    let end = out.stream_position()?;
    let total = end - base;
    let mut sizes = Writer::new(true);
    sizes.i64(total as i64);
    sizes.u32(cinfo.len() as u32);
    sizes.u32(info.len() as u32);
    sizes.u32(data_flag);
    out.seek(SeekFrom::Start(base + size_pos as u64))?;
    out.write_all(&sizes.buf)?;
    out.seek(SeekFrom::Start(end))?;
    Ok(())
}

/// pack() into a file at path.
pub fn pack_file(
    path: &Path,
    h: &PackHeader,
    nodes: &[PackNode],
    data_flag: u32,
    block_flag: u32,
) -> Result<()> {
    let f = std::fs::File::create(path)?;
    let mut w = io::BufWriter::with_capacity(1 << 20, f);
    pack(&mut w, h, nodes, data_flag, block_flag, compress::threads())?;
    w.flush()?;
    Ok(())
}
