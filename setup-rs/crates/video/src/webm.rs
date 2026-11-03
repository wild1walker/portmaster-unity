//! WebM (Matroska's subset) read and written as far as the video step needs: the EBML header and
//! the Segment's Info, Tags and the like kept byte for byte, the Tracks with the video track's
//! picture size changeable, and every Cluster's blocks; the SeekHead, Cues and Void elements are
//! left out (their positions change; players, ours included, find the clusters by scanning).
//! Unity's clips (its transcoder writes them with libwebm) have one VP8 track and Vorbis tracks.

pub const ID_EBML: u32 = 0x1A45DFA3;
pub const ID_SEGMENT: u32 = 0x18538067;
pub const ID_SEEKHEAD: u32 = 0x114D9B74;
pub const ID_INFO: u32 = 0x1549A966;
pub const ID_TIMECODESCALE: u32 = 0x2AD7B1;
pub const ID_TRACKS: u32 = 0x1654AE6B;
pub const ID_TRACKENTRY: u32 = 0xAE;
pub const ID_TRACKNUMBER: u32 = 0xD7;
pub const ID_TRACKTYPE: u32 = 0x83;
pub const ID_CODECID: u32 = 0x86;
pub const ID_VIDEO: u32 = 0xE0;
pub const ID_PIXELWIDTH: u32 = 0xB0;
pub const ID_PIXELHEIGHT: u32 = 0xBA;
pub const ID_DISPLAYWIDTH: u32 = 0x54B0;
pub const ID_DISPLAYHEIGHT: u32 = 0x54BA;
pub const ID_CLUSTER: u32 = 0x1F43B675;
pub const ID_TIMECODE: u32 = 0xE7;
pub const ID_POSITION: u32 = 0xA7;
pub const ID_PREVSIZE: u32 = 0xAB;
pub const ID_SIMPLEBLOCK: u32 = 0xA3;
pub const ID_BLOCKGROUP: u32 = 0xA0;
pub const ID_BLOCK: u32 = 0xA1;
pub const ID_REFERENCEBLOCK: u32 = 0xFB;
pub const ID_CUES: u32 = 0x1C53BB6B;
pub const ID_VOID: u32 = 0xEC;

pub type Result<T> = std::result::Result<T, String>;

/// An element's id (with its marker bits, as Matroska writes ids) and the length of it.
fn read_id(b: &[u8], pos: usize) -> Result<(u32, usize)> {
    let first = *b.get(pos).ok_or("webm: truncated id")?;
    let len = first.leading_zeros() as usize + 1;
    if len > 4 || pos + len > b.len() {
        return Err(format!("webm: bad element id at {pos}"));
    }
    Ok((b[pos..pos + len].iter().fold(0u32, |a, &x| (a << 8) | x as u32), len))
}

/// A size (None: unknown, all value bits set) and the length of it.
fn read_size(b: &[u8], pos: usize) -> Result<(Option<u64>, usize)> {
    let first = *b.get(pos).ok_or("webm: truncated size")?;
    let len = first.leading_zeros() as usize + 1;
    if len > 8 || pos + len > b.len() {
        return Err(format!("webm: bad element size at {pos}"));
    }
    let mask = 0xFFu64 >> len;
    let mut v = first as u64 & mask;
    let mut ones = v == mask;
    for &x in &b[pos + 1..pos + len] {
        v = (v << 8) | x as u64;
        ones &= x == 0xFF;
    }
    Ok((if ones { None } else { Some(v) }, len))
}

/// One element: (id, the range of its payload, the range of the whole element).
#[derive(Clone, Copy, Debug)]
struct El {
    id: u32,
    body: (usize, usize),
    all: (usize, usize),
}

/// The elements in b[start..end], one level; an unknown size runs to `end` (or, for a Cluster,
/// to the next top-level element of the Segment).
fn children(b: &[u8], start: usize, end: usize) -> Result<Vec<El>> {
    let mut out = Vec::new();
    let mut pos = start;
    while pos < end {
        let (id, il) = read_id(b, pos)?;
        let (size, sl) = read_size(b, pos + il)?;
        let body_start = pos + il + sl;
        let body_end = match size {
            Some(s) => body_start.checked_add(s as usize).filter(|&e| e <= end).ok_or_else(|| {
                format!("webm: element {id:#x} at {pos} runs past its parent")
            })?,
            None if id == ID_CLUSTER => {
                // up to the next element that cannot be in a cluster
                let mut q = body_start;
                while q < end {
                    let (cid, cl) = read_id(b, q)?;
                    if matches!(cid, ID_CLUSTER | ID_CUES | ID_TAGS_ID | ID_SEEKHEAD) {
                        break;
                    }
                    let (cs, csl) = read_size(b, q + cl)?;
                    q += cl + csl + cs.ok_or("webm: unknown size inside a cluster")? as usize;
                }
                q.min(end)
            }
            None => end,
        };
        out.push(El { id, body: (body_start, body_end), all: (pos, body_end) });
        pos = body_end;
    }
    Ok(out)
}
const ID_TAGS_ID: u32 = 0x1254C367;

/// Whether a run of elements (a BlockGroup's other children) has one with this id.
pub fn has_child(b: &[u8], id: u32) -> bool {
    children(b, 0, b.len()).is_ok_and(|c| c.iter().any(|e| e.id == id))
}

fn uint(b: &[u8]) -> u64 {
    b.iter().fold(0u64, |a, &x| (a << 8) | x as u64)
}

/// An id's bytes (as stored, marker bits included).
fn put_id(out: &mut Vec<u8>, id: u32) {
    let n = 4 - (id.leading_zeros() as usize / 8);
    out.extend_from_slice(&id.to_be_bytes()[4 - n.max(1)..]);
}

/// A size in the shortest form (never the all-ones "unknown" value).
fn put_size(out: &mut Vec<u8>, v: u64) {
    let mut len = 1;
    while len < 8 && v >= (1u64 << (7 * len)) - 1 {
        len += 1;
    }
    let marked = v | (1u64 << (7 * len));
    out.extend_from_slice(&marked.to_be_bytes()[8 - len..]);
}

pub fn element(id: u32, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(payload.len() + 12);
    put_id(&mut out, id);
    put_size(&mut out, payload.len() as u64);
    out.extend_from_slice(payload);
    out
}

fn put_uint(v: u64) -> Vec<u8> {
    let n = ((64 - v.leading_zeros() as usize).div_ceil(8)).max(1);
    v.to_be_bytes()[8 - n..].to_vec()
}

/// A block's frame data and where it was.
#[derive(Clone, Debug)]
pub struct Block {
    pub track: u64,
    /// time relative to the cluster's (in the segment's TimecodeScale units)
    pub rel: i16,
    /// SimpleBlock flags (0x80 key frame, 0x06 lacing, 0x08 invisible, 0x01 discardable)
    pub flags: u8,
    /// everything after the block header (the frame, or laced frames)
    pub payload: Vec<u8>,
}

#[derive(Clone, Debug)]
pub enum Item {
    Simple(Block),
    /// a BlockGroup: its Block, and the group's other children as they were
    Group(Block, Vec<u8>),
    /// anything else in a cluster, kept as it was (Position and PrevSize are dropped)
    Raw(Vec<u8>),
}

impl Item {
    pub fn block(&self) -> Option<&Block> {
        match self {
            Item::Simple(b) | Item::Group(b, _) => Some(b),
            Item::Raw(_) => None,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Cluster {
    pub timecode: u64,
    pub items: Vec<Item>,
}

#[derive(Clone, Debug)]
pub struct Track {
    pub number: u64,
    pub kind: u64,
    pub codec: String,
    /// the TrackEntry's children as they were: (id, payload)
    pub entries: Vec<(u32, Vec<u8>)>,
}

impl Track {
    /// The video track's picture size (PixelWidth, PixelHeight).
    pub fn size(&self) -> Option<(u64, u64)> {
        let v = &self.entries.iter().find(|e| e.0 == ID_VIDEO)?.1;
        let els = children(v, 0, v.len()).ok()?;
        let get = |id| els.iter().find(|e| e.id == id).map(|e| uint(&v[e.body.0..e.body.1]));
        Some((get(ID_PIXELWIDTH)?, get(ID_PIXELHEIGHT)?))
    }

    /// Sets the video picture size; a display size that was the picture's follows it.
    pub fn set_size(&mut self, w: u64, h: u64) -> Result<()> {
        let (ow, oh) = self.size().ok_or("webm: the track has no picture size")?;
        let v = &mut self.entries.iter_mut().find(|e| e.0 == ID_VIDEO).ok_or("webm: no Video element")?.1;
        let els = children(v, 0, v.len())?;
        let mut out = Vec::new();
        for e in els {
            let old = uint(&v[e.body.0..e.body.1]);
            let new = match e.id {
                ID_PIXELWIDTH => Some(w),
                ID_PIXELHEIGHT => Some(h),
                ID_DISPLAYWIDTH if old == ow => Some(w),
                ID_DISPLAYHEIGHT if old == oh => Some(h),
                _ => None,
            };
            match new {
                Some(n) => out.extend(element(e.id, &put_uint(n))),
                None => out.extend_from_slice(&v[e.all.0..e.all.1]),
            }
        }
        *v = out;
        Ok(())
    }

    fn write(&self) -> Vec<u8> {
        let mut p = Vec::new();
        for (id, body) in &self.entries {
            p.extend(element(*id, body));
        }
        element(ID_TRACKENTRY, &p)
    }
}

#[derive(Clone, Debug)]
pub enum Part {
    Raw(Vec<u8>),
    Tracks(Vec<Track>),
    Cluster(Cluster),
}

/// A WebM file: its EBML header and the Segment's parts in their order.
#[derive(Clone, Debug)]
pub struct WebM {
    pub header: Vec<u8>,
    pub parts: Vec<Part>,
    pub timecode_scale: u64,
}

fn parse_block(b: &[u8]) -> Result<Block> {
    let first = *b.first().ok_or("webm: empty block")?;
    let len = first.leading_zeros() as usize + 1;
    if len > 8 || b.len() < len + 3 {
        return Err("webm: bad block header".into());
    }
    let track = b[1..len].iter().fold((first as u64) & (0xFF >> len) as u64, |a, &x| (a << 8) | x as u64);
    let rel = i16::from_be_bytes([b[len], b[len + 1]]);
    Ok(Block { track, rel, flags: b[len + 2], payload: b[len + 3..].to_vec() })
}

fn block_bytes(k: &Block, flags: u8) -> Vec<u8> {
    let mut out = Vec::with_capacity(k.payload.len() + 4);
    put_size(&mut out, k.track); // a track number is written as a size is
    out.extend_from_slice(&k.rel.to_be_bytes());
    out.push(flags);
    out.extend_from_slice(&k.payload);
    out
}

impl WebM {
    pub fn parse(b: &[u8]) -> Result<WebM> {
        let top = children(b, 0, b.len())?;
        let header = top.iter().find(|e| e.id == ID_EBML).ok_or("webm: no EBML header")?;
        let seg = top.iter().find(|e| e.id == ID_SEGMENT).ok_or("webm: no Segment")?;
        let mut parts = Vec::new();
        let mut timecode_scale = 1_000_000;
        for e in children(b, seg.body.0, seg.body.1)? {
            match e.id {
                ID_SEEKHEAD | ID_CUES | ID_VOID => {}
                ID_INFO => {
                    for i in children(b, e.body.0, e.body.1)? {
                        if i.id == ID_TIMECODESCALE {
                            timecode_scale = uint(&b[i.body.0..i.body.1]);
                        }
                    }
                    parts.push(Part::Raw(b[e.all.0..e.all.1].to_vec()));
                }
                ID_TRACKS => {
                    let mut tracks = Vec::new();
                    for t in children(b, e.body.0, e.body.1)?.into_iter().filter(|t| t.id == ID_TRACKENTRY) {
                        let mut tr = Track { number: 0, kind: 0, codec: String::new(), entries: Vec::new() };
                        for c in children(b, t.body.0, t.body.1)? {
                            let body = &b[c.body.0..c.body.1];
                            match c.id {
                                ID_TRACKNUMBER => tr.number = uint(body),
                                ID_TRACKTYPE => tr.kind = uint(body),
                                ID_CODECID => tr.codec = String::from_utf8_lossy(body).trim_end_matches('\0').to_string(),
                                _ => {}
                            }
                            tr.entries.push((c.id, body.to_vec()));
                        }
                        tracks.push(tr);
                    }
                    parts.push(Part::Tracks(tracks));
                }
                ID_CLUSTER => {
                    let mut cl = Cluster { timecode: 0, items: Vec::new() };
                    for c in children(b, e.body.0, e.body.1)? {
                        let body = &b[c.body.0..c.body.1];
                        match c.id {
                            ID_TIMECODE => cl.timecode = uint(body),
                            ID_POSITION | ID_PREVSIZE | ID_VOID => {}
                            ID_SIMPLEBLOCK => cl.items.push(Item::Simple(parse_block(body)?)),
                            ID_BLOCKGROUP => {
                                let mut block = None;
                                let mut rest = Vec::new();
                                for g in children(b, c.body.0, c.body.1)? {
                                    if g.id == ID_BLOCK {
                                        block = Some(parse_block(&b[g.body.0..g.body.1])?);
                                    } else {
                                        rest.extend_from_slice(&b[g.all.0..g.all.1]);
                                    }
                                }
                                cl.items.push(Item::Group(block.ok_or("webm: a BlockGroup without a Block")?, rest));
                            }
                            _ => cl.items.push(Item::Raw(b[c.all.0..c.all.1].to_vec())),
                        }
                    }
                    parts.push(Part::Cluster(cl));
                }
                _ => parts.push(Part::Raw(b[e.all.0..e.all.1].to_vec())),
            }
        }
        Ok(WebM { header: b[header.all.0..header.all.1].to_vec(), parts, timecode_scale })
    }

    pub fn tracks(&self) -> Vec<&Track> {
        self.parts.iter().flat_map(|p| match p {
            Part::Tracks(t) => t.iter().collect(),
            _ => Vec::new(),
        }).collect()
    }

    pub fn tracks_mut(&mut self) -> Vec<&mut Track> {
        self.parts.iter_mut().flat_map(|p| match p {
            Part::Tracks(t) => t.iter_mut().collect(),
            _ => Vec::new(),
        }).collect()
    }

    pub fn write(&self) -> Vec<u8> {
        let mut seg = Vec::new();
        for p in &self.parts {
            match p {
                Part::Raw(r) => seg.extend_from_slice(r),
                Part::Tracks(ts) => {
                    let mut body = Vec::new();
                    for t in ts {
                        body.extend(t.write());
                    }
                    seg.extend(element(ID_TRACKS, &body));
                }
                Part::Cluster(c) => {
                    let mut body = element(ID_TIMECODE, &put_uint(c.timecode));
                    for it in &c.items {
                        match it {
                            Item::Simple(k) => body.extend(element(ID_SIMPLEBLOCK, &block_bytes(k, k.flags))),
                            Item::Group(k, rest) => {
                                // a Block's flags byte keeps only lacing and invisible
                                let mut g = element(ID_BLOCK, &block_bytes(k, k.flags & 0x0E));
                                g.extend_from_slice(rest);
                                body.extend(element(ID_BLOCKGROUP, &g));
                            }
                            Item::Raw(r) => body.extend_from_slice(r),
                        }
                    }
                    seg.extend(element(ID_CLUSTER, &body));
                }
            }
        }
        let mut out = self.header.clone();
        out.extend(element(ID_SEGMENT, &seg));
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes_and_ids_round_trip() {
        for v in [0u64, 1, 126, 127, 128, 16382, 16383, 1 << 20, (1 << 35) + 5] {
            let mut b = Vec::new();
            put_size(&mut b, v);
            assert_eq!(read_size(&b, 0).unwrap(), (Some(v), b.len()), "{v}");
        }
        // 127 is all ones in one byte: it must take two
        let mut b = Vec::new();
        put_size(&mut b, 127);
        assert_eq!(b, vec![0x40, 0x7F]);
        for id in [ID_EBML, ID_SEGMENT, ID_TRACKENTRY, ID_PIXELWIDTH, ID_DISPLAYWIDTH, ID_TIMECODESCALE] {
            let mut b = Vec::new();
            put_id(&mut b, id);
            assert_eq!(read_id(&b, 0).unwrap(), (id, b.len()));
        }
        assert_eq!(read_size(&[0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF], 0).unwrap().0, None);
    }

    /// A small file as libwebm lays one out: header, Segment with SeekHead, Void, Info, Tracks (a
    /// VP8 and a Vorbis track), a Cluster with SimpleBlocks and a BlockGroup, Cues.
    pub fn sample() -> Vec<u8> {
        let mut seg = element(ID_SEEKHEAD, &[0x4D, 0xBB, 0x80]);
        seg.extend(element(ID_VOID, &[0; 5]));
        let mut info = element(ID_TIMECODESCALE, &put_uint(1_000_000));
        info.extend(element(0x4489, &1000.0f64.to_be_bytes()));
        seg.extend(element(ID_INFO, &info));
        let mut video = element(ID_PIXELWIDTH, &put_uint(64));
        video.extend(element(ID_PIXELHEIGHT, &put_uint(48)));
        video.extend(element(ID_DISPLAYWIDTH, &put_uint(64)));
        video.extend(element(ID_DISPLAYHEIGHT, &put_uint(48)));
        let mut t1 = element(ID_TRACKNUMBER, &[1]);
        t1.extend(element(ID_TRACKTYPE, &[1]));
        t1.extend(element(ID_CODECID, b"V_VP8"));
        t1.extend(element(ID_VIDEO, &video));
        let mut t2 = element(ID_TRACKNUMBER, &[2]);
        t2.extend(element(ID_TRACKTYPE, &[2]));
        t2.extend(element(ID_CODECID, b"A_VORBIS"));
        t2.extend(element(0x63A2, &[2, 1, 2, 9, 9, 9]));
        let mut tracks = element(ID_TRACKENTRY, &t1);
        tracks.extend(element(ID_TRACKENTRY, &t2));
        seg.extend(element(ID_TRACKS, &tracks));
        let mut cl = element(ID_TIMECODE, &put_uint(0));
        cl.extend(element(ID_SIMPLEBLOCK, &[0x81, 0, 0, 0x80, 0xAA, 0xBB]));
        cl.extend(element(ID_SIMPLEBLOCK, &[0x82, 0, 5, 0x80, 1, 2, 3]));
        let mut g = element(ID_BLOCK, &[0x81, 0, 33, 0x00, 0xCC]);
        g.extend(element(ID_REFERENCEBLOCK, &[0xDF]));
        cl.extend(element(ID_BLOCKGROUP, &g));
        seg.extend(element(ID_CLUSTER, &cl));
        seg.extend(element(ID_CUES, &[0xBB, 0x80]));
        let mut f = element(ID_EBML, &element(0x4282, b"webm"));
        f.extend(element(ID_SEGMENT, &seg));
        f
    }

    #[test]
    fn parse_change_write_parse() {
        let mut w = WebM::parse(&sample()).unwrap();
        assert_eq!(w.timecode_scale, 1_000_000);
        let ts = w.tracks();
        assert_eq!(ts.len(), 2);
        assert_eq!((ts[0].number, ts[0].kind, ts[0].codec.as_str()), (1, 1, "V_VP8"));
        assert_eq!(ts[0].size(), Some((64, 48)));
        assert_eq!(ts[1].codec, "A_VORBIS");
        w.tracks_mut()[0].set_size(32, 24).unwrap();
        let out = w.write();
        let back = WebM::parse(&out).unwrap();
        assert_eq!(back.tracks()[0].size(), Some((32, 24)));
        // the display size followed the picture's, the audio track is the same
        let v = &back.tracks()[0].entries.iter().find(|e| e.0 == ID_VIDEO).unwrap().1;
        assert!(v.windows(4).any(|x| x == [0x54, 0xB0, 0x81, 32]));
        assert_eq!(back.tracks()[1].entries, w.tracks()[1].entries);
        let Part::Cluster(c) = back.parts.iter().find(|p| matches!(p, Part::Cluster(_))).unwrap() else { panic!() };
        let blocks: Vec<&Block> = c.items.iter().filter_map(Item::block).collect();
        assert_eq!(blocks.len(), 3);
        assert_eq!((blocks[0].track, blocks[0].rel, blocks[0].flags, blocks[0].payload.clone()), (1, 0, 0x80, vec![0xAA, 0xBB]));
        assert_eq!((blocks[2].track, blocks[2].rel, blocks[2].payload.clone()), (1, 33, vec![0xCC]));
        assert!(matches!(&c.items[2], Item::Group(_, rest) if rest == &element(ID_REFERENCEBLOCK, &[0xDF])));
        // no SeekHead, Void or Cues; written again, the same bytes
        assert!(!back.parts.iter().any(|p| matches!(p, Part::Raw(r) if r.starts_with(&[0x11, 0x4D]))));
        assert_eq!(back.write(), out);
    }
}
