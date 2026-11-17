//! A strings bank's STDT chunk: every path of the project (`event:/...`, `bus:/...`,
//! `snapshot:/...`, `bank:/...`) and its GUID, kept as a radix tree (read from the data; the
//! layout: docs/fmod-player.md, "Strings").
//!
//! u32 version (1); the nodes: X16 count (n * 2 + 1), u16 size (8), each u24 label offset in the
//! text (0xffffff: none), u8 the label's first character, u24 first child (a leaf: its GUID's
//! index), u8 child count (0: a leaf); the GUIDs: X16 count (n * 2 + 1), u16 size (16), the GUIDs;
//! the text: u16 size, NUL-terminated labels. (Then index arrays this reader does not need.)
use crate::bank::Guid;
use crate::rd::{Rd, Result};

fn u24(b: &[u8]) -> u32 {
    b[0] as u32 | (b[1] as u32) << 8 | (b[2] as u32) << 16
}

pub fn read(d: &[u8]) -> Result<Vec<(String, Guid)>> {
    let mut r = Rd::new(d);
    let ver = r.u32()?;
    if ver != 1 {
        return Err(format!("strings table version {ver}"));
    }
    let nn = (r.x16()? >> 1) as usize;
    let size = r.u16()? as usize;
    if size != 8 {
        return Err(format!("string tree nodes of {size} bytes"));
    }
    let nodes: Vec<(u32, u32, u8)> = (0..nn)
        .map(|_| {
            let b = r.take(8)?;
            Ok((u24(&b[0..3]), u24(&b[4..7]), b[7]))
        })
        .collect::<Result<_>>()?;
    let ng = (r.x16()? >> 1) as usize;
    let gs = r.u16()? as usize;
    if ng > 0 && gs < 16 {
        return Err(format!("string GUIDs of {gs} bytes"));
    }
    let guids: Vec<Guid> = (0..ng).map(|_| Ok(r.take(gs)?[..16].try_into().unwrap())).collect::<Result<_>>()?;
    let tl = r.u16()? as usize;
    let text = r.take(tl)?;
    let label = |x: u32| -> Result<&str> {
        if x == 0xff_ffff {
            return Ok("");
        }
        let s = text.get(x as usize..).ok_or("a label past the text")?;
        let e = s.iter().position(|&c| c == 0).ok_or("an unterminated label")?;
        std::str::from_utf8(&s[..e]).map_err(|_| "a label that is not UTF-8".to_string())
    };
    let mut out = Vec::new();
    // depth-first, without recursion (a corrupt tree must not overflow the stack)
    let mut stack = vec![(0usize, String::new(), 0usize)];
    while let Some((i, prefix, depth)) = stack.pop() {
        let (x, first, n) = *nodes.get(i).ok_or("a node past the tree")?;
        if depth > 512 {
            return Err("a string tree too deep".into());
        }
        let full = prefix + label(x)?;
        if n == 0 {
            let g = *guids.get(first as usize).ok_or("a GUID index past the table")?;
            out.push((full, g));
        } else {
            for k in (first as usize..first as usize + n as usize).rev() {
                stack.push((k, full.clone(), depth + 1));
            }
        }
    }
    out.sort();
    Ok(out)
}
