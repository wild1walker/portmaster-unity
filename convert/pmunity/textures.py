"""Conversion step `textures`: desktop texture formats -> ASTC, optionally downscaled for a small
screen.

Mali and most other handheld GPUs cannot sample BC1-BC7 (DXT, BPTC, RGTC) textures, so Unity's Android player
would decompress every one of them to RGBA32 on the CPU when it loads (a 4096x4096 atlas page: 16
MB on disk becomes 64 MB of RAM and GPU memory). This step decodes each such texture (UnityPy),
optionally scales it (Lanczos on premultiplied alpha, so transparent texels do not bleed into the
edges), and encodes it as ASTC LDR (ARM's astc-encoder), which the GPU samples directly.

What changes in the data:
  * the Texture2D: format, size, mip count, image size, and its pixels (in the .resS stream file
    next to the serialized file or inside the bundle; textures with inline pixels too, unless
    `inline` is False);
  * where a texture was scaled, what points into it in pixels, so everything keeps its size on
    screen: sprite atlas entries get Unity's own `downscaleMultiplier` (the mechanism the editor
    uses for atlas pages shrunk to a maximum size), and sprites that own their texture get their
    rect, offset, border and pixels-per-unit scaled with it.

Settings (cfg['textures'], defaults in DEFAULTS): scale, block, quality, per-name overrides
(sharp_names with scale_sharp / sharp_block), and the sizes left alone.
"""
import os
import re
import shutil

from . import bigbundle
from .unity_io import Loaded

DEFAULTS = {
    'scale': 1.0,                # texture size factor (1.0 = only re-encode)
    'block': [4, 4],             # ASTC block footprint (4x4 = 8 bits per texel)
    'quality': 'medium',         # astc-encoder preset: fastest | fast | medium | thorough
    'sharp_names': '',           # regex: textures given scale_sharp / sharp_block instead
    'scale_sharp': 1.0,
    'sharp_block': [4, 4],
    'keep_max': 64,              # textures whose longest side is at most this are never scaled
    'min_side': 16,              # nor scaled below this
    'min_convert': 4,            # smaller textures are left as they are
    'mips': 'keep',              # keep: a texture with mipmaps gets a full ASTC mip chain; drop: one level
    'inline': True,              # also textures whose pixels are inside the serialized file
    'files': r'.',               # regex on the relative path: which files to look at
    'threads': 0,                # encoder threads, 0 = all cores
    'pipeline': 'auto',          # decode the next texture while one is encoded: true | false | auto
                                 # (on, but on a system with under 2 GB of RAM a texture of more than
                                 # 16 MB decoded is decoded and encoded alone)
    'spill_mb': 64,              # bundles with more than this (uncompressed) are unpacked to files
                                 # and rewritten from them (bigbundle.py): little memory, whatever
                                 # their size; 0 = always in memory
}

# Unity texture formats this step converts: name -> has alpha. Windows builds also use BC7 (RGBA),
# BC4 (one channel, sampled as (r, 0, 0, 1)) and BC5 (two, (r, g, 0, 1)); each is decoded to the
# RGBA a GPU samples from it (texture_image) and encoded like the others. BC6H (HDR half floats)
# is left as it is: ASTC LDR cannot hold it.
CONVERTIBLE = {
    'DXT1': False, 'DXT1Crunched': False, 'DXT5': True, 'DXT5Crunched': True, 'BC7': True,
    'BC4': False, 'BC5': False,
    'RGB24': False, 'RGBA32': True, 'ARGB32': True, 'BGRA32': True,
}


# ---------------------------------------------------------------- pixels

def downscale(img, w, h):
    """PIL RGBA image -> w x h, filtered with premultiplied alpha."""
    from PIL import Image
    if img.size == (w, h):
        return img
    return img.convert('RGBa').resize((w, h), Image.Resampling.LANCZOS).convert('RGBA')


class Astc:
    """astc-encoder contexts, one per block footprint."""

    PRESETS = ('fastest', 'fast', 'medium', 'thorough')

    def __init__(self, quality, threads):
        if quality not in self.PRESETS:
            raise ValueError(f'ASTC quality {quality!r}: one of {", ".join(self.PRESETS)}')
        self.quality, self.threads = quality, threads or os.cpu_count() or 4
        self._ctx = {}

    def _context(self, block):
        import astc_encoder as A
        if block not in self._ctx:
            cfg = A.ASTCConfig(A.ASTCProfile.LDR, block[0], block[1], block_z=1,
                               quality=getattr(A.ASTCQualityPreset, self.quality.upper()),
                               flags=A.ASTCConfigFlags.USE_DECODE_UNORM8)
            self._ctx[block] = A.ASTCContext(cfg, threads=self.threads)
        return self._ctx[block]

    def encode(self, img, block):
        """PIL RGBA image (top row first) -> ASTC blocks, bottom row first as Unity stores textures."""
        import astc_encoder as A
        from PIL import Image
        flipped = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        src = A.ASTCImage(A.ASTCType.U8, flipped.width, flipped.height, 1, flipped.tobytes())
        return self._context(block).compress(src, A.ASTCSwizzle.from_str('RGBA'))


def astc_format_id(block, alpha, unity=None):
    """Unity's TextureFormat of ASTC LDR with this footprint. Up to Unity 2018 there are RGB (48-53)
    and RGBA (54-59) formats; from 2019.1 only ASTC_<n>x<n> (48-53, alpha included: ASTC blocks
    carry it either way), the RGBA numbers being deprecated. unity: the data's version string."""
    from UnityPy.enums import TextureFormat
    if unity and int(unity.split('.')[0]) >= 2019:
        alpha = False
    return int(TextureFormat[f'ASTC_{"RGBA" if alpha else "RGB"}_{block[0]}x{block[1]}'])


def mip_chain(img, levels):
    """[img, img/2, ...] down to `levels` images (each side halved, at least 1)."""
    out = [img]
    while len(out) < levels:
        w, h = out[-1].size
        if w == 1 and h == 1:
            break
        out.append(downscale(out[-1], max(1, w // 2), max(1, h // 2)))
    return out


# ---------------------------------------------------------------- decisions

class Choice:
    __slots__ = ('block', 'width', 'height', 'alpha', 'mips')

    def __init__(self, block, width, height, alpha, mips):
        self.block, self.width, self.height, self.alpha, self.mips = block, width, height, alpha, mips


def choose(tree, fmt_name, c, sharp_rx):
    """What to do with one Texture2D (its typetree): a Choice, or None to leave it."""
    w, h = tree['m_Width'], tree['m_Height']
    if fmt_name not in CONVERTIBLE:
        return None
    if tree.get('m_IsReadable'):                       # scripts read its pixels (GetPixels): keep the format
        return None
    if tree.get('m_TextureDimension', 2) != 2 or tree.get('m_ImageCount', 1) != 1:
        return None
    if min(w, h) < c['min_convert']:
        return None
    sharp = bool(sharp_rx and sharp_rx.search(tree['m_Name']))
    s = c['scale_sharp'] if sharp else c['scale']
    if max(w, h) <= c['keep_max'] or min(w, h) * s < c['min_side']:
        s = 1.0
    nw, nh = max(1, round(w * s)), max(1, round(h * s))
    mips = tree.get('m_MipCount', 1) if c['mips'] == 'keep' else 1
    if mips > 1:
        mips = min(mips, max(nw, nh).bit_length())
    return Choice(tuple(c['sharp_block'] if sharp else c['block']), nw, nh, CONVERTIBLE[fmt_name], mips)


# ---------------------------------------------------------------- what points into a texture

def _mul(d, keys, f):
    for k in keys:
        d[k] *= f


def scale_render_data(rd, sx, sy):
    """A SpriteRenderData's pixel-space fields (its rectangle in the texture)."""
    _mul(rd['textureRect'], ('x', 'width'), sx)
    _mul(rd['textureRect'], ('y', 'height'), sy)
    rd['textureRectOffset']['x'] *= sx
    rd['textureRectOffset']['y'] *= sy
    aro = rd['atlasRectOffset']
    if (aro['x'], aro['y']) != (-1, -1):               # -1, -1: not set
        aro['x'] *= sx
        aro['y'] *= sy
    uv = rd['uvTransform']                             # (x, y) scale/offset in pixels, (z, w) likewise
    _mul(uv, ('x', 'y'), sx)
    _mul(uv, ('z', 'w'), sy)


def scale_own_sprite(sp, sx, sy):
    """A sprite drawn from its own (scaled) texture: rect, offset and border are in that texture's
    pixels; pixels-per-unit scale with it, so the sprite keeps its size in the world."""
    _mul(sp['m_Rect'], ('x', 'width'), sx)
    _mul(sp['m_Rect'], ('y', 'height'), sy)
    sp['m_Offset']['x'] *= sx
    sp['m_Offset']['y'] *= sy
    _mul(sp['m_Border'], ('x', 'z'), sx)              # left, right
    _mul(sp['m_Border'], ('y', 'w'), sy)              # bottom, top
    sp['m_PixelsToUnits'] *= sx
    scale_render_data(sp['m_RD'], sx, sy)


def fix_references(lf, sizes, stats):
    """Sprite atlases and sprites of one file that use its scaled textures. sizes: {path id of a
    texture: (old w, old h, new w, new h)}. Returns [(object, tree)] to save. (A packed sprite
    keeps its render data in the atlas's pixels: the atlas entry's multiplier covers it.)"""
    out = []
    for _, o in lf.objects('SpriteAtlas'):
        t = o.read_typetree()
        n = 0
        for _key, rd in t['m_RenderDataMap']:
            ref = rd['texture']
            if ref['m_FileID'] == 0 and ref['m_PathID'] in sizes:
                ow, _, nw, _ = sizes[ref['m_PathID']]
                rd['downscaleMultiplier'] = rd.get('downscaleMultiplier', 1.0) * nw / ow
                n += 1
        if n:
            out.append((o, t))
            stats['atlas_entries'] += n
    for _, o in lf.objects('Sprite'):
        t = o.read_typetree()
        ref = t['m_RD']['texture']
        if ref['m_FileID'] == 0 and ref['m_PathID'] in sizes:
            ow, oh, nw, nh = sizes[ref['m_PathID']]
            scale_own_sprite(t, nw / ow, nh / oh)
            out.append((o, t))
            stats['sprites'] += 1
    return out


# ---------------------------------------------------------------- files

class StreamFile:
    """The new contents of a texture stream file (.resS): blobs, each 16-byte aligned."""

    def __init__(self):
        self.data = bytearray()

    def add(self, blob):
        self.data += b'\0' * (-len(self.data) % 16)
        off = len(self.data)
        self.data += blob
        return off


def _format_name(tree):
    from UnityPy.enums import TextureFormat
    try:
        return TextureFormat(tree['m_TextureFormat']).name
    except ValueError:
        return None


def texture_image(img, fmt_name):
    """A decoded texture (UnityPy's PIL image) -> the RGBA image a GPU samples from it. UnityPy
    gives BC4 as luminance (r, r, r); the GPU samples it as (r, 0, 0, 1) (and BC5 as (r, g, 0, 1),
    which UnityPy's RGB already is)."""
    if fmt_name == 'BC4':
        from PIL import Image
        r = img.convert('L')
        zero = Image.new('L', r.size, 0)
        return Image.merge('RGBA', (r, zero, zero, Image.new('L', r.size, 255)))
    return img.convert('RGBA')


def mem_total_mb():
    """The system's RAM in MB (Linux; elsewhere: plenty)."""
    try:
        with open('/proc/meminfo') as f:
            for line in f:
                if line.startswith('MemTotal:'):
                    return int(line.split()[1]) >> 10
    except OSError:
        pass
    return 1 << 20


def _prepare(o, ch, fmt_name=None):
    """Decoded and scaled mip levels of one texture (runs on the worker thread)."""
    img = texture_image(o.read().image, fmt_name)
    return mip_chain(downscale(img, ch.width, ch.height), ch.mips)


def convert_file(lf, rel, c, enc, sharp_rx, read_stream, stats, log):
    """Converts the textures of one opened file. Returns (stream name, new stream bytes or None,
    {path id: sizes}) or None when nothing changed.

    Decoding and scaling a texture (one core each) run on a worker thread while the previous
    texture is encoded (ASTC on every core), one texture ahead at most. Everything else, and every
    other read of the file, happens with the worker idle, in the textures' order, so the output is
    the same as one texture after the other (c['pipeline'] = False does exactly that)."""
    unity = getattr(lf.sf, 'unity_version', None)
    from concurrent.futures import ThreadPoolExecutor
    stream_name = None
    stream = StreamFile()
    sizes, save, converted = {}, [], 0
    items = [(pid, o, o.read_typetree()) for pid, o in lf.objects('Texture2D')]   # (no reads later)
    pool = ThreadPoolExecutor(max_workers=1) if c.get('pipeline', True) else None
    solo_px = (16 << 20) // 4 if c.get('pipeline', True) == 'auto' and mem_total_mb() < 2048 else None
    pending = []

    def finish():
        nonlocal converted
        while pending:
            pid, o, t, sd, streamed, ch, fut = pending.pop(0)
            levels = fut.result() if pool else _prepare(o, ch, _format_name(t))
            blob = b''.join(enc.encode(level, ch.block) for level in levels)
            nlevels = len(levels)
            del levels
            old = (t['m_Width'], t['m_Height'])
            if (ch.width, ch.height) != old:
                sizes[pid] = (old[0], old[1], ch.width, ch.height)
            stats['in'] += sd.get('size') or len(t.get('image data') or b'')
            stats['out'] += len(blob)
            t['m_TextureFormat'] = astc_format_id(ch.block, ch.alpha, unity)
            t['m_Width'], t['m_Height'], t['m_MipCount'] = ch.width, ch.height, nlevels
            t['m_CompleteImageSize'] = len(blob)
            if streamed:
                t['m_StreamData'] = dict(sd, offset=stream.add(blob), size=len(blob))
                t['image data'] = b''
            else:
                t['image data'] = blob
            save.append((o, t))
            converted += 1

    try:
        for pid, o, t in items:
            sd = t.get('m_StreamData') or {}
            streamed = bool(sd.get('size'))
            if streamed:
                name = sd['path']
                if stream_name not in (None, name):
                    raise ValueError(f'{rel}: textures stream from more than one file ({stream_name}, {name})')
                stream_name = name
            elif not c['inline'] or not t.get('image data'):
                continue
            ch = choose(t, _format_name(t), c, sharp_rx)
            if ch is None:
                if streamed:                               # copied into the new stream file unchanged
                    finish()                               # (the worker is idle while the file is read)
                    blob = read_stream(sd['path'], sd['offset'], sd['size'])
                    t['m_StreamData'] = dict(sd, offset=stream.add(blob))
                    save.append((o, t))
                continue
            solo = pool and solo_px and t['m_Width'] * t['m_Height'] > solo_px
            if solo:                                       # a big one on a small system: alone
                finish()
            fut = pool.submit(_prepare, o, ch, _format_name(t)) if pool else None
            finish()                                       # the previous one, while this one is prepared
            pending.append((pid, o, t, sd, streamed, ch, fut))
            if solo:
                finish()
        finish()
    finally:
        if pool:
            pool.shutdown(wait=True)
    if not converted:
        return None
    if stream_name:
        # everything else that streams from the same file goes along (Unity 2018+ meshes; cubemaps,
        # texture arrays, 3D textures): the stream file is rewritten
        for _, o in lf.objects():
            if o.type.name not in OTHER_STREAMED:
                continue
            t = o.read_typetree()
            sd = t.get('m_StreamData') or {}
            if sd.get('size') and sd.get('path') == stream_name:
                t['m_StreamData'] = dict(sd, offset=stream.add(read_stream(sd['path'], sd['offset'], sd['size'])))
                save.append((o, t))
    stats['textures'] += converted
    save += fix_references(lf, sizes, stats)
    for o, t in save:
        o.save_typetree(t)
    return stream_name, (bytes(stream.data) if stream_name else None), sizes


# classes besides Texture2D whose data can be in a .resS stream (StreamingInfo m_StreamData)
OTHER_STREAMED = ('Mesh', 'Cubemap', 'CubemapArray', 'Texture2DArray', 'Texture3D')


def texture_files(job, files_rx):
    rx = re.compile(files_rx)
    out = job.list('', lambda n: (n.endswith('.assets') or n == 'globalgamemanagers' or re.match(r'level\d+$', n)))
    for d, _, names in os.walk(os.path.join(job.src, 'StreamingAssets')):
        rel = os.path.relpath(d, job.src)
        for n in sorted(names):
            p = os.path.join(d, n)
            try:
                with open(p, 'rb') as f:
                    if f.read(8) == b'UnityFS\0':
                        out.append(os.path.join(rel, n))
            except OSError:
                pass
    return [r for r in out if rx.search(r)]


def run(job, only=None):
    c = dict(DEFAULTS, **job.cfg.get('textures', {}))
    sharp_rx = re.compile('(?i)' + c['sharp_names']) if c['sharp_names'] else None
    enc = Astc(c['quality'], c['threads'])
    stats = dict(textures=0, sprites=0, atlas_entries=0, **{'in': 0, 'out': 0})
    files = texture_files(job, c['files'])
    if only:
        files = [f for f in files if re.search(only, f)]
    for i, rel in enumerate(files, 1):
        if job.is_done('textures', rel):
            continue
        src = job.path_in(rel)
        spilled = c['spill_mb'] and bigbundle.size(src) > c['spill_mb'] << 20
        job.need_space(os.path.getsize(src) // 2 + (64 << 20) + (bigbundle.size(src) * 2 if spilled else 0),
                       f'convert {rel}')
        before = (stats['in'], stats['out'])
        res_dir = os.path.dirname(os.path.join(job.src, rel))
        handles = {}
        b = sf_node = None
        if spilled:
            work = job.work_dir('textures')
            b = bigbundle.unpack(src, work)
            sfs = [n for n in b.nodes if not n.name.endswith(('.resS', '.resource'))] if b else []
            if len(sfs) == 1:
                sf_node = sfs[0]
            else:
                b = None
                job.log(f'textures: {rel}: a bundle layout left to UnityPy (read in memory)')
        if b:
            node_files = {n.name: n.path for n in b.nodes}
            lf = Loaded(sf_node.path, streams={n.name: n.path for n in b.nodes if n is not sf_node})
        else:
            lf = Loaded(src, res_dir)

        def read_stream(path, off, size):
            name = os.path.basename(path)
            if b:
                p = node_files[name]
            elif lf.is_bundle:
                node = lf.bundle_res(name)
                return bytes(node[off:off + size])
            else:
                p = job.path_in(os.path.join(os.path.dirname(rel), name))
                if not os.path.exists(p):
                    p = os.path.join(res_dir, name)
            if p not in handles:
                handles[p] = open(p, 'rb')
            handles[p].seek(off)
            return handles[p].read(size)

        try:
            r = convert_file(lf, rel, c, enc, sharp_rx, read_stream, stats, job.log)
        finally:
            for h in handles.values():
                h.close()
        if r is not None and b:
            # the bundle again from its node files: the serialized file and the stream rewritten
            stream_name, stream_bytes, sizes = r
            new = {sf_node.name: lf.save()}
            if stream_bytes is not None:
                new[os.path.basename(stream_name)] = stream_bytes
            nodes = []
            for n in b.nodes:
                if n.name in new:
                    p = n.path + '.new'
                    with open(p, 'wb') as f:
                        f.write(new.pop(n.name))
                    n = n._replace(path=p)
                nodes.append(n)
            lf.close()
            del new, stream_bytes
            r = True                                       # (its bytes freed: only "changed" is left)
            out = os.path.join(work, 'bundle.new')
            bigbundle.pack(b, nodes, out)
            job.write_file(rel, out)
            if sizes:
                job.state['scaled'][rel] = {str(k): list(v) for k, v in sizes.items()}
        elif r is not None:
            stream_name, stream_bytes, sizes = r
            if lf.is_bundle:
                if stream_bytes is not None:
                    lf.set_bundle_res(os.path.basename(stream_name), stream_bytes)
                job.write(rel, lf.save())
            else:
                if stream_bytes is not None:     # the stream first: a resumed run redoes the pair
                    job.write(os.path.join(os.path.dirname(rel), os.path.basename(stream_name)), stream_bytes)
                job.write(rel, lf.save())
            if sizes:
                job.state['scaled'][rel] = {str(k): list(v) for k, v in sizes.items()}
        lf.close()
        if spilled:
            shutil.rmtree(work, ignore_errors=True)
            try:
                os.rmdir(os.path.dirname(work))
            except OSError:
                pass
        job.mark_done('textures', rel)
        if r is not None:
            job.log(f'textures: {rel}: {(stats["in"] - before[0]) >> 20} -> {(stats["out"] - before[1]) >> 20} MB ({i}/{len(files)})')
    job.log(f'textures: {stats["textures"]} converted ({stats["in"] >> 20} -> {stats["out"] >> 20} MB), '
            f'{stats["atlas_entries"]} atlas entries and {stats["sprites"]} sprites follow their scaled textures')
    return stats
