//! Streamed texture pixels moved into the textures (android_data.py inline_serialized,
//! inline_bundle): a serialized file with a .resS beside it, or a bundle with .resS nodes, written
//! with every texture's pixels inline, one texture in memory at a time.
use crate::android_data::{Ctx, Stop, ANDROID};
use pkgzip::py;
use std::collections::HashSet;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::Arc;
use unityfs::bundle::{self, Bundle, PackHeader, PackNode};
use unityfs::{FileSource, SerializedFile, SharedSource};

/// A stream's bytes: (path, offset, size).
type ReadStream<'a> = dyn Fn(&str, u64, usize) -> unityfs::Result<Vec<u8>> + 'a;

/// Whether a bundle has a .resS node (android_data.Bundle's node names).
pub fn has_ress_node(src: &str) -> Result<bool, Stop> {
    let b = unityfs::platform::MiniBundle::open(Path::new(src))?;
    Ok(b.nodes.iter().any(|n| n.3.ends_with(".resS")))
}

fn save_inlined(
    sf: &SerializedFile,
    plan: &HashSet<i64>,
    read: &ReadStream<'_>,
    out: &Path,
) -> Result<(), Stop> {
    let f = std::fs::File::create(out)?;
    let mut w = std::io::BufWriter::with_capacity(1 << 20, f);
    sf.save_to(&mut w, |sf, o| {
        if !plan.contains(&o.path_id) {
            return Ok(None);
        }
        unityfs::inline::inlined(sf, o.path_id, read).map(Some)
    })?;
    w.flush()?;
    Ok(())
}

/// A serialized file with a <name>.resS beside it: textures inlined, target platform Android,
/// retargeted to the player's version. Returns (textures inlined, .resS names still read).
pub fn inline_serialized(
    ctx: &Ctx,
    src: &str,
    dst: &str,
) -> Result<(usize, HashSet<String>), Stop> {
    let mut f = unityfs::DataFile::open(src, &ctx.opts())?;
    let d = py::dirname(src).to_string();
    let read = move |path: &str, off: u64, size: usize| -> unityfs::Result<Vec<u8>> {
        let name = path.rsplit('/').next().unwrap_or(path);
        let mut fh = std::fs::File::open(py::join(&d, name))?;
        fh.seek(SeekFrom::Start(off))?;
        let mut v = Vec::with_capacity(size);
        fh.take(size as u64).read_to_end(&mut v)?;
        Ok(v)
    };
    let (plan, still) = unityfs::inline::plan(&f.sf)?;
    f.sf.target_platform = ANDROID as i32;
    ctx.retarget_sf(&mut f.sf)?;
    std::fs::create_dir_all(py::dirname(dst))?;
    let tmp = format!("{dst}.tmp");
    save_inlined(&f.sf, &plan, &read, Path::new(&tmp))?;
    drop(f);
    std::fs::rename(&tmp, dst)?;
    Ok((plan.len(), still))
}

/// A bundle with .resS nodes: textures inlined, the .resS nodes nothing reads any more dropped,
/// the serialized file's platform Android, written as chunked LZ4 (bigbundle.pack). Returns
/// (textures inlined, the nodes dropped).
pub fn inline_bundle(ctx: &Ctx, src: &str, dst: &str) -> Result<(usize, Vec<String>), Stop> {
    inline_bundle_with(Some(ctx.db.clone()), src, dst)
}

/// inline_bundle with a class database (or none: bundles carry their type trees).
pub fn inline_bundle_with(
    db: Option<Arc<unityfs::ClassDb>>,
    src: &str,
    dst: &str,
) -> Result<(usize, Vec<String>), Stop> {
    let b = Bundle::open(src)?;
    let sfs: Vec<usize> = (0..b.header.nodes.len())
        .filter(|&i| b.header.nodes[i].flags & 4 != 0)
        .collect();
    if sfs.len() != 1 {
        return Err(Stop::NotPorted(format!(
            "{src}: a bundle with {} serialized files and streamed textures (UnityPy's in-memory inlining)",
            sfs.len()
        )));
    }
    let work = format!("{dst}.work");
    let _ = std::fs::remove_dir_all(&work);
    let res = (|| -> Result<(usize, Vec<String>), Stop> {
        // the nodes read in place, block by block (or, big LZMA blocks, unpacked first)
        let srcs: Vec<SharedSource> = if b.lazy_ok() {
            (0..b.header.nodes.len()).map(|i| b.node(i)).collect()
        } else {
            b.unpack(Path::new(&work))?
                .iter()
                .map(|p| FileSource::open(p).map(|s| Arc::new(s) as SharedSource))
                .collect::<std::io::Result<_>>()?
        };
        let i = sfs[0];
        let node = &b.header.nodes[i];
        let mut sf = SerializedFile::open(
            &node.name,
            srcs[i].clone(),
            db.clone(),
            Some(&b.header.engine),
        )?;
        let names: Vec<String> = b.header.nodes.iter().map(|n| n.name.clone()).collect();
        let read = |path: &str, off: u64, size: usize| -> unityfs::Result<Vec<u8>> {
            let name = path.rsplit('/').next().unwrap_or(path);
            let k = names
                .iter()
                .position(|n| n == name)
                .ok_or_else(|| unityfs::Error::Value(format!("KeyError: '{name}'")))?;
            Ok(srcs[k].read_clipped(off, size)?)
        };
        let (plan, still) = unityfs::inline::plan(&sf)?;
        sf.target_platform = ANDROID as i32;
        std::fs::create_dir_all(&work)?;
        let cab = Path::new(&work).join("cab.new");
        save_inlined(&sf, &plan, &read, &cab)?;
        drop(sf);
        let dropped: Vec<String> = names
            .iter()
            .filter(|n| n.ends_with(".resS") && !still.contains(*n))
            .cloned()
            .collect();
        let cab_src: SharedSource = Arc::new(FileSource::open(&cab)?);
        let nodes: Vec<PackNode> = b
            .header
            .nodes
            .iter()
            .enumerate()
            .filter(|(_, n)| !dropped.contains(&n.name))
            .map(|(k, n)| PackNode {
                name: n.name.clone(),
                flags: n.flags,
                src: if k == i {
                    cab_src.clone()
                } else {
                    srcs[k].clone()
                },
            })
            .collect();
        std::fs::create_dir_all(py::dirname(dst))?;
        let tmp = format!("{dst}.tmp");
        bundle::pack_file(
            Path::new(&tmp),
            &PackHeader::of(&b.header),
            &nodes,
            bundle::LZ4_DATA_FLAG,
            bundle::LZ4_BLOCK_FLAG,
        )?;
        std::fs::rename(&tmp, dst)?;
        Ok((plan.len(), dropped))
    })();
    let _ = std::fs::remove_dir_all(&work);
    res
}
