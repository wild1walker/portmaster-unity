"""Build the data tree Unity's Android player (2017.4, Mono, armeabi-v7a) reads, from a game's
converted desktop (macOS / Linux) data: `python -m pmunity android-data --src <Game>_Data --pkg
<player package> ...`. See docs/android-data.md for the why of each step.

Output (default ./android-out/):
  <name>/                                    the "APK" root = Application.dataPath; hand this
    assets/bin/Data/                         directory to the player as the package code path
      globalgamemanagers                     UnityPy-edited (platform, GLES3 only, see below)
      globalgamemanagers.assets, level*, sharedassets*.assets, resources.assets
                                             clones, header target platform byte set to Android
      *.resource                             clones (FSB audio / video data, unchanged)
                                             (*.resS: texture pixels moved into the textures, see
                                             inline_texture_streams; bundles likewise)
      unity default resources                the Android player package's own copy
      Resources/unity_builtin_extra          clone, platform byte set (+ Hidden/VideoDecodeAndroid,
                                             video.py, when the data has Hidden/VideoDecode)
      Managed/*.dll                          Android UnityEngine*.dll + the game's assemblies
      Managed/etc/mono/...                   Mono config (dllmaps), read relative to the cwd
      boot.config                            read through Java AssetManager "bin/Data/boot.config"
    assets/AssetBundles/*, assets/Logo.png   StreamingAssets (jar:file://<root>!/assets);
                                             bundle clones whose first LZ4 block is re-encoded
                                             to the same length with the CAB platform set (a few
                                             incompressible ones are rewritten)
  --beside files                             beside the root (default goggame-*.info: a GOG build's
                                             DLC checks read Application.dataPath + "/../goggame-<id>.info")
  plugins.map                                the native-plugin map for ua_host (plugins.py; also in zip/)
  android_data_report.json                   (with the plugin report under "plugins")

Big files are APFS clones (clonefile(2): no extra space; a 1-byte patch copies one block).
On a filesystem without clones, unchanged files are hard links and patched files copies.
Re-running only redoes files whose source changed (state in <out>/.android_data_state.json).

  zip/ (--pack apk|obb|all)                  the same tree as archives for the stock Mount path:
                                             <name>.apk = assets/bin/Data/**, main.<code>.<pkg>.obb
                                             = the rest + assets/<build-id>; STORE, zip32, < 2 GiB each;
                                             --loose-assets files (FMOD banks) in zip/assets/ instead

Game-specific additions are options: --managed-file (an assembly to ship instead of the data's
copy, e.g. a port's helper), --assembly-patch (a command that writes a patched assembly from the
original game's ones), --asmpatch (tools/asmpatch's generic patches, e.g. Rewired on Android),
--root-file (files the game reads from Application.dataPath).
"""
import argparse
import ctypes
import errno
import fnmatch
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from . import unity_version as UV
from . import video

DEF_OUT = 'android-out'

ANDROID = 13            # BuildTarget.Android; SerializedFile::ReadMetadata rejects anything else
GLES3 = 11              # GfxDeviceRenderer kGfxRendererOpenGLES3x
SHADER_PLATFORM_GLCORE, SHADER_PLATFORM_GLES3, SHADER_PLATFORM_D3D11 = 15, 9, 4
STATE_VERSION = 10      # bump when an action's output changes

# --------------------------------------------------------------------------- file placement

_libc = ctypes.CDLL(None, use_errno=True)
_clonefile = getattr(_libc, 'clonefile', None)


def clone(src, dst, allow_hardlink):
    """dst = a copy-on-write clone of src (APFS); else a hard link (unchanged files only) or a copy.
    Never leaves dst sharing an inode with src when the caller will patch it (allow_hardlink=False)."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.unlink(dst)
    if _clonefile is not None:
        if _clonefile(os.fsencode(src), os.fsencode(dst), 0) == 0:
            return 'clone'
        e = ctypes.get_errno()
        if e not in (errno.ENOTSUP, errno.EOPNOTSUPP, errno.EXDEV, errno.ENOSYS):
            raise OSError(e, 'clonefile failed', src)
    if allow_hardlink:
        try:
            os.link(src, dst)
            return 'hardlink'
        except OSError:
            pass
    shutil.copyfile(src, dst)
    shutil.copymode(src, dst)
    return 'copy'


def consumable(args, src, act):
    """--consume-src: the game data's big files are moved into the tree (or deleted once rewritten)
    instead of copied; assemblies and Mono's config stay (an assembly patch reads them)."""
    if not args.consume_src or act not in ('link', 'sfpatch', 'bundle', 'sfinline'):
        return False
    rel = os.path.relpath(os.path.realpath(src), os.path.realpath(args.src))
    return not rel.startswith(('..', 'Managed' + os.sep, 'Mono' + os.sep))


def move(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    os.replace(src, dst)
    return 'moved'


def write_bytes(dst, data):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, dst)


# --------------------------------------------------------------------------- SerializedFile header

def sf_platform_offset(head):
    """(offset of the uint32 target platform, byteorder, current value, unity version) for a
    SerializedFile header: formats 9..21 (Unity 5 - 2019.4) have a 20-byte big-endian header,
    format 22 (2020.1+) 48 bytes (64-bit sizes); then the metadata: unity version cstring, platform."""
    fmt, uv, off, big = UV.sf_header(head)
    bo = '>' if big else '<'
    if not re.match(r'^\d+\.\d+\.\d+', uv):
        raise ValueError(f'bad unity version string {uv!r}')
    cur, = struct.unpack_from(bo + 'I', head, off)
    return off, bo, cur, uv


def patch_sf_platform(path, platform=ANDROID):
    with open(path, 'r+b') as f:
        head = f.read(128)
        off, bo, cur, uv = sf_platform_offset(head)
        if cur != platform:
            f.seek(off)
            f.write(struct.pack(bo + 'I', platform))
    return cur


# --------------------------------------------------------------------------- UnityFS bundles

def _cstr(b, p):
    e = b.index(b'\0', p)
    return b[p:e], e + 1


def lz4_decompress(comp, usize):
    import lz4.block
    return lz4.block.decompress(comp, uncompressed_size=usize)


def lz4_literal_pos(comp, target):
    """Position in the raw LZ4 block comp of the literal byte that becomes plaintext[target],
    or None when that byte comes from a match."""
    ip = op = 0
    n = len(comp)
    while ip < n:
        token = comp[ip]; ip += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = comp[ip]; ip += 1; lit += b
                if b != 255:
                    break
        if op <= target < op + lit:
            return ip + (target - op)
        ip += lit; op += lit
        if ip >= n:
            break
        ip += 2                                             # match offset
        ml = token & 15
        if ml == 15:
            while True:
                b = comp[ip]; ip += 1; ml += b
                if b != 255:
                    break
        ml += 4
        if op <= target < op + ml:
            return None
        op += ml
    return None


def _lz4_sequences(comp):
    """Raw LZ4 block -> [[literals, offset, matchlen]] (the last one has offset None) and plaintext."""
    seqs, out = [], bytearray()
    ip, n = 0, len(comp)
    while ip < n:
        token = comp[ip]; ip += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = comp[ip]; ip += 1; lit += b
                if b != 255:
                    break
        lits = comp[ip:ip + lit]; ip += lit
        out += lits
        if ip >= n:
            seqs.append([bytes(lits), None, 0])
            break
        off = comp[ip] | comp[ip + 1] << 8; ip += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = comp[ip]; ip += 1; ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        for j in range(ml):                                 # byte-wise: matches may overlap
            out.append(out[start + j])
        seqs.append([bytes(lits), off, ml, bytes(out[-ml:])])
    return seqs, bytes(out)


def _ext(n):
    return 0 if n < 15 else 1 + (n - 15) // 255


def _seq_size(q):
    s = 1 + _ext(len(q[0])) + len(q[0])
    if q[1] is not None:
        s += 2 + _ext(q[2] - 4)
    return s


def _lz4_emit(seqs):
    o = bytearray()
    for q in seqs:
        lits, off = q[0], q[1]
        ml = q[2] - 4 if off is not None else 0
        L = len(lits)
        o.append((min(L, 15) << 4) | (min(ml, 15) if off is not None else 0))
        if L >= 15:
            r = L - 15
            while r >= 255:
                o.append(255); r -= 255
            o.append(r)
        o += lits
        if off is not None:
            o += bytes((off & 255, off >> 8))
            if ml >= 15:
                r = ml - 15
                while r >= 255:
                    o.append(255); r -= 255
                o.append(r)
    return bytes(o)


def _lz4_decode_trace(comp):
    """Plaintext and [(out_start, matchlen, position of the 2 offset bytes in comp)] per match."""
    out, matches = bytearray(), []
    ip, n = 0, len(comp)
    while ip < n:
        token = comp[ip]; ip += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = comp[ip]; ip += 1; lit += b
                if b != 255:
                    break
        out += comp[ip:ip + lit]; ip += lit
        if ip >= n:
            break
        opos = ip
        off = comp[ip] | comp[ip + 1] << 8; ip += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = comp[ip]; ip += 1; ml += b
                if b != 255:
                    break
        ml += 4
        start, s0 = len(out), len(out) - off
        if s0 < 0:
            return None, None
        for j in range(ml):
            out.append(out[s0 + j])
        matches.append((start, ml, opos))
    return bytes(out), matches


def lz4_repoint(comp, expect, tries=64):
    """comp (a raw LZ4 block whose literal was patched) made to decode to expect without changing
    its length: every match that copied the old byte gets the offset of an identical earlier run."""
    comp = bytearray(comp)
    for _ in range(tries):
        out, matches = _lz4_decode_trace(bytes(comp))
        if out is None or len(out) != len(expect):
            return None
        if out == expect:
            return bytes(comp)
        bad = next(i for i in range(len(out)) if out[i] != expect[i])
        m = next((m for m in matches if m[0] <= bad < m[0] + m[1]), None)
        if m is None:
            return None                                     # a literal differs: not ours to fix
        start, ml, opos = m
        target = expect[start:start + ml]
        q = expect.rfind(target, max(0, start - 65535), start + ml - 1)
        if q < 0 or q >= start or start - q > 65535:
            return None
        comp[opos], comp[opos + 1] = (start - q) & 255, (start - q) >> 8
    return None


def lz4_exact(plain, size):
    """A raw LZ4 block of plain that is exactly size bytes long, or None: compress tighter than
    needed, then turn matches back into literals (each one costs a few bytes) until the size fits.
    Keeps a bundle's block table and every later byte of the file where they were."""
    import lz4.block
    for mode, level in (('high_compression', 12), ('high_compression', 9), ('high_compression', 4), ('default', 0)):
        c = lz4.block.compress(plain, mode=mode, compression=level, store_size=False) if mode != 'default' \
            else lz4.block.compress(plain, store_size=False)
        if len(c) > size:
            continue
        seqs, check = _lz4_sequences(c)
        if check != plain:
            continue
        deficit = size - len(c)
        for _ in range(8):
            if deficit == 0:
                break
            out, cur = [], seqs[0]
            for nxt in seqs[1:]:
                if deficit and cur[1] is not None:
                    merged = [cur[0] + cur[3] + nxt[0]] + nxt[1:]
                    d = _seq_size(merged) - _seq_size(cur) - _seq_size(nxt)
                    if 0 < d <= deficit:
                        cur = merged; deficit -= d
                        continue
                out.append(cur); cur = nxt
            out.append(cur)
            seqs = out
        if deficit:
            continue
        res = _lz4_emit(seqs)
        if len(res) == size and lz4.block.decompress(res, uncompressed_size=len(plain)) == plain:
            return res
    return None


class Bundle:
    """Minimal UnityFS (format 6/7) reader: header, blocks, nodes, block file offsets."""

    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            head = f.read(4096)
            sig, p = _cstr(head, 0)
            if sig != b'UnityFS':
                raise ValueError('not a UnityFS bundle')
            self.format, = struct.unpack_from('>I', head, p); p += 4
            self.uv, p = _cstr(head, p)
            self.ur, p = _cstr(head, p)
            self.size, self.ci_size, self.ui_size, self.flags = struct.unpack_from('>qIII', head, p)
            p += 20
            if self.format >= 7:
                p = (p + 15) & ~15
            self.header_end = p
            if self.flags & 0x80:                           # blocks info at the end of the file
                f.seek(self.size - self.ci_size)
                ci = f.read(self.ci_size)
                self.data_start = p
            else:
                f.seek(p)
                ci = f.read(self.ci_size)
                self.data_start = p + self.ci_size
        comp = self.flags & 0x3f
        if comp == 0:
            ub = ci
        elif comp in (2, 3):
            ub = lz4_decompress(ci, self.ui_size)
        else:                                               # LZMA blocks info: not used by these bundles
            raise ValueError(f'blocks info compression {comp}')
        self.hash = ub[:16]
        n, = struct.unpack_from('>i', ub, 16)
        self.blocks = [list(struct.unpack_from('>IIH', ub, 20 + 10 * i)) for i in range(n)]
        p = 20 + 10 * n
        nn, = struct.unpack_from('>i', ub, p); p += 4
        self.nodes = []
        for _ in range(nn):
            off, sz, fl = struct.unpack_from('>qqI', ub, p); p += 20
            name, p = _cstr(ub, p)
            self.nodes.append((off, sz, fl, name.decode()))
        # file offset and uncompressed offset of every block
        self.block_pos = []
        fpos, upos = self.data_start, 0
        for usz, csz, bfl in self.blocks:
            self.block_pos.append((fpos, upos))
            fpos += csz; upos += usz

    def block_of(self, uoff):
        for i, (usz, csz, bfl) in enumerate(self.blocks):
            fpos, upos = self.block_pos[i]
            if upos <= uoff < upos + usz:
                return i
        raise ValueError('offset beyond data')

    def read_block(self, f, i):
        usz, csz, bfl = self.blocks[i]
        fpos, upos = self.block_pos[i]
        f.seek(fpos)
        raw = f.read(csz)
        c = bfl & 0x3f
        if c == 0:
            return raw, raw
        if c in (2, 3):
            return raw, lz4_decompress(raw, usz)
        raise ValueError(f'block compression {c} not handled')

    def read_plain(self, f, uoff, n):
        out = b''
        while n > 0:
            i = self.block_of(uoff)
            raw, plain = self.read_block(f, i)
            s = uoff - self.block_pos[i][1]
            take = plain[s:s + n]
            out += take; n -= len(take); uoff += len(take)
        return out


def patch_bundle_platform(path, platform=ANDROID):
    """Sets the target platform of every serialized file (CAB-...) inside a bundle, in place:
    the platform byte is a literal in the (uncompressed or LZ4) block, so one byte changes and
    nothing moves. Returns a list of (node, old platform, method). Raises if a byte is not a
    literal (then the bundle must be rewritten; see rewrite_bundle_block)."""
    b = Bundle(path)
    done = []
    with open(path, 'r+b') as f:
        for off, sz, fl, name in b.nodes:
            if not fl & 4:
                continue
            head = b.read_plain(f, off, 128)
            poff, bo, cur, uv = sf_platform_offset(head)
            if cur == platform:
                done.append((name, cur, 'already'))
                continue
            new = struct.pack(bo + 'I', platform)
            old = struct.pack(bo + 'I', cur)
            method = 'literal'
            for k in range(4):
                if old[k] == new[k]:
                    continue
                u = off + poff + k
                i = b.block_of(u)
                raw, plain = b.read_block(f, i)
                rel = u - b.block_pos[i][1]
                c = b.blocks[i][2] & 0x3f
                if c == 0:
                    pos = rel
                else:
                    pos = lz4_literal_pos(raw, rel)
                    if pos is None:
                        rewrite_bundle_block(path, b, i, rel, new[k])
                        done.append((name, cur, 'rewrite'))
                        return done + patch_bundle_platform(path, platform)
                    patched = bytearray(raw); patched[pos] = new[k]
                    expect = bytearray(plain); expect[rel] = new[k]
                    if lz4_decompress(bytes(patched), b.blocks[i][0]) != bytes(expect):
                        # later matches copy the byte: point them elsewhere, else re-encode
                        # the block to the same length
                        enc = lz4_repoint(bytes(patched), bytes(expect)) or lz4_exact(bytes(expect), len(raw))
                        if enc is None:
                            rewrite_bundle_block(path, b, i, rel, new[k])
                            done.append((name, cur, 'rewrite'))
                            return done + patch_bundle_platform(path, platform)
                        f.seek(b.block_pos[i][0])
                        f.write(enc)
                        method = 'reencode'
                        continue
                f.seek(b.block_pos[i][0] + pos)
                f.write(bytes([new[k]]))
            done.append((name, cur, method))
    return done


def rewrite_bundle_block(path, b, i, rel, value):
    """Fallback: re-encode block i with plaintext[rel] = value and rewrite the bundle (other blocks
    are copied as they are; header and blocks info get the new sizes)."""
    import lz4.block
    with open(path, 'rb') as f:
        raw, plain = b.read_block(f, i)
        p = bytearray(plain); p[rel] = value
        newraw = lz4.block.compress(bytes(p), mode='high_compression', store_size=False)
        blocks = [list(x) for x in b.blocks]
        blocks[i][1] = len(newraw)
        blocks[i][2] = (blocks[i][2] & ~0x3f) | 3
        bi = bytearray(b.hash) + struct.pack('>i', len(blocks))
        for usz, csz, bfl in blocks:
            bi += struct.pack('>IIH', usz, csz, bfl)
        bi += struct.pack('>i', len(b.nodes))
        for off, sz, fl, name in b.nodes:
            bi += struct.pack('>qqI', off, sz, fl) + name.encode() + b'\0'
        ci = lz4.block.compress(bytes(bi), mode='high_compression', store_size=False)
        flags = (b.flags & ~0x3f) | 3
        f.seek(0)
        head = bytearray(f.read(b.header_end))
        tmp = path + '.tmp'
        with open(tmp, 'wb') as o:
            o.write(b'\0' * b.header_end)
            if not flags & 0x80:
                o.write(ci)
            for j, (usz, csz, bfl) in enumerate(b.blocks):
                if j == i:
                    o.write(newraw)
                else:
                    f.seek(b.block_pos[j][0])
                    o.write(f.read(csz))
            if flags & 0x80:
                o.write(ci)
            total = o.tell()
            p0 = len(b'UnityFS\0') + 4 + len(b.uv) + 1 + len(b.ur) + 1
            struct.pack_into('>qIII', head, p0, total, len(ci), len(bi), flags)
            o.seek(0); o.write(head)
    os.replace(tmp, path)


def bundle_platforms(path):
    b = Bundle(path)
    res = []
    with open(path, 'rb') as f:
        for off, sz, fl, name in b.nodes:
            if fl & 4:
                res.append((name, sf_platform_offset(b.read_plain(f, off, 128))[2]))
    return res


# --------------------------------------------------------------------------- texture data inline

def inline_texture_streams(objects, read_stream):
    """Moves streamed texture pixels (m_StreamData -> offset/size in a .resS) into the texture
    objects themselves ("image data"), returning (textures inlined, .resS names still referenced).

    Why: Unity 2017.4's Android player creates an asynchronous upload for every streamed texture
    but never runs one; its per-frame step (AsyncUploadManager::TimeSlicedUpdate, the player
    loop's Initialization.AsyncUploadTimeSlicedUpdate) is compiled out. Only a synchronous load
    finishes such a texture, so LoadAssetAsync / LoadSceneAsync of anything holding one waits
    forever (Cuphead: atlas_playerfx at the start screen). Inline pixels are read like any other
    field, the way a texture without stream data always loads."""
    done, still = 0, set()
    for o in objects:
        if o.type.name == 'Mesh':
            # Unity 2018+ meshes can keep their vertex data in the .resS too: left there
            sd = o.read_typetree().get('m_StreamData')
            if sd and sd.get('path') and sd.get('size'):
                still.add(sd['path'].split('/')[-1])
            continue
        if o.type.name not in ('Texture2D', 'Cubemap', 'Texture2DArray', 'Texture3D'):
            continue
        t = o.read_typetree()
        sd = t.get('m_StreamData')
        if not sd or not sd.get('path'):
            continue
        if 'image data' not in t:
            still.add(sd['path'].split('/')[-1])
            continue
        data = read_stream(sd['path'], sd['offset'], sd['size'])
        if len(data) != sd['size']:
            raise SystemExit(f'{t.get("m_Name")}: stream {sd} gave {len(data)} bytes')
        t['image data'] = data
        t['m_StreamData'] = {'offset': 0, 'size': 0, 'path': ''}
        o.save_typetree(t)
        done += 1
    return done, still


def inline_bundle(src, dst):
    """Bundle with .resS nodes: textures inlined, the .resS nodes dropped once unreferenced, every
    CAB's target platform set to Android, saved as chunked LZ4 like the original."""
    import UnityPy
    env = UnityPy.load(src)
    bf = env.file
    cache = {}

    def read_stream(path, off, size):
        name = path.split('/')[-1]
        if name not in cache:
            f = bf.files[name]
            cache[name] = bytes(f.bytes) if hasattr(f, 'bytes') else bytes(f.save())
        return cache[name][off:off + size]

    done, still = inline_texture_streams(env.objects, read_stream)
    dropped = [n for n in list(bf.files) if n.endswith('.resS') and n not in still]
    for n in dropped:
        del bf.files[n]
    for f in bf.files.values():
        if hasattr(f, '_m_target_platform'):
            f._m_target_platform = ANDROID
    write_bytes(dst, bf.save(packer='lz4'))
    return done, dropped


def inline_serialized(src, dst):
    """Serialized file with a <name>.resS beside it: textures inlined, target platform Android.
    Returns (textures inlined, names of .resS files still read by other objects)."""
    import UnityPy
    env = UnityPy.load(src)
    sf = env.file
    d = os.path.dirname(src)

    def read_stream(path, off, size):
        with open(os.path.join(d, path.split('/')[-1]), 'rb') as f:
            f.seek(off)
            return f.read(size)

    done, still = inline_texture_streams(sf.objects.values(), read_stream)
    sf._m_target_platform = ANDROID
    write_bytes(dst, sf.save())
    return done, still


# --------------------------------------------------------------------------- globalgamemanagers

def edit_globalgamemanagers(src, managed_names, args, log):
    """UnityPy edit of the settings file: Android platform in the header, GLES3 as the only
    graphics API, GLES3 shader defines (copied from GLCore, whose programs the GLES3 ones were
    converted from); MonoManager's standalone-only engine modules the Android player
    lacks are left in place (see BUILTIN_ASSEMBLY_SLOTS), optional PlayerSettings tweaks."""
    import UnityPy
    env = UnityPy.load(src)
    sf = env.file
    notes = []
    for pid, o in sf.objects.items():
        n = o.type.name
        if n == 'BuildSettings':
            t = o.read_typetree()
            apis = [int(x) for x in args.graphics_apis.split(',')]
            if t['m_GraphicsAPIs'] != apis:
                notes.append(f'BuildSettings.m_GraphicsAPIs {t["m_GraphicsAPIs"]} -> {apis}')
                t['m_GraphicsAPIs'] = apis
                o.save_typetree(t)
        elif n == 'GraphicsSettings':
            # the GLES3 programs were made from the GLCore (Windows builds: D3D11) ones, compiled
            # with that platform's built-in defines: the GLES3 platform gets the same (the entry
            # has the same layout in Unity 2017.4 - 2022.3)
            t = o.read_typetree()
            d = t['m_ShaderDefinesPerShaderCompiler']
            have = {e['shaderPlatform'] for e in d}
            src_plat = next((p for p in (SHADER_PLATFORM_GLCORE, SHADER_PLATFORM_D3D11) if p in have), None)
            if SHADER_PLATFORM_GLES3 not in have and src_plat is not None:
                src_e = next(e for e in d if e['shaderPlatform'] == src_plat)
                e = dict(src_e); e['shaderPlatform'] = SHADER_PLATFORM_GLES3
                d.append(e)
                notes.append('GraphicsSettings: GLES3 shader defines = ' + ('GLCore' if src_plat == SHADER_PLATFORM_GLCORE else 'D3D11') + ' defines')
                o.save_typetree(t)
        elif n == 'MonoManager':
            t = o.read_typetree()
            if 'm_AssemblyNames' not in t:
                continue                # Unity 2020.2+: the list is ScriptingAssemblies.json (checked in plan)
            names = t['m_AssemblyNames']
            game, engine = split_assemblies(names, getattr(args, 'unity', None))
            missing = [a for a in game if a not in managed_names]
            if missing:
                raise SystemExit(f'MonoManager lists assemblies missing from Managed/: {missing}')
            absent = [a for a in engine if a not in managed_names]
            if absent:
                notes.append(f'MonoManager: engine modules the player will skip (not in its Managed set): {absent}')
        elif n == 'InputManager' and 'Rewired_Core.dll' in managed_names:
            # Rewired reading pads through Unity input (its Android platform) asks for axes named
            # Joy<j>Axis<a> (UnityTools.GetUnityInputAxisName: joysticks 1..11, axes 1..28); Rewired's
            # setup adds them to a project's InputManager, but a desktop build that never used Unity
            # input for pads (Cuphead: only Joy9-11 buttons) lacks them: buttons work, sticks and
            # d-pad read 0. Added like Rewired's installer does (joystick axis, no dead zone), for the
            # first --rewired-joysticks joysticks (default 4): Input.GetAxis searches the whole axis
            # list on every call and Rewired asks for 28 axes of each pad every frame.
            t = o.read_typetree()
            ax = t['m_Axes']
            have = {a['m_Name'] for a in ax}
            proto = next(a for a in ax if a['m_Name'].startswith('Joy') or a['type'] == 2 or True)
            added = 0
            for j in range(1, args.rewired_joysticks + 1):
                for a in range(1, 29):
                    name = f'Joy{j}Axis{a}'
                    if name in have:
                        continue
                    e = dict(proto)
                    e.update({'m_Name': name, 'descriptiveName': '', 'descriptiveNegativeName': '',
                              'negativeButton': '', 'positiveButton': '', 'altNegativeButton': '',
                              'altPositiveButton': '', 'gravity': 0.0, 'dead': 0.0, 'sensitivity': 1.0,
                              'snap': False, 'invert': False, 'type': 2, 'axis': a - 1, 'joyNum': j})
                    ax.append(e)
                    added += 1
            if added:
                notes.append(f'InputManager: {added} Rewired joystick axes (Joy1-{args.rewired_joysticks} Axis1-28) added')
                o.save_typetree(t)
        elif n == 'QualitySettings' and args.async_upload_buffer:
            t = o.read_typetree()
            for q in t['m_QualitySettings']:
                q['asyncUploadBufferSize'] = args.async_upload_buffer
            notes.append(f'QualitySettings.asyncUploadBufferSize = {args.async_upload_buffer} MB')
            o.save_typetree(t)
        elif n == 'ScriptMapper' and getattr(args, 'video_shader_pid', None):
            video.map_shader(sf, args.video_shader_pid, notes)    # Hidden/VideoDecodeAndroid
        elif n == 'PlayerSettings':
            t = o.read_typetree()
            changed = False
            if args.mt_rendering != 'keep':
                t['m_MTRendering'] = args.mt_rendering == 'on'
                notes.append(f'PlayerSettings.m_MTRendering = {t["m_MTRendering"]}')
                changed = True
            # mobile players (Android) take multithreaded rendering from mobileMTRenderingBaked, which
            # the editor fills from the mobile platform's setting at build time; a standalone build
            # leaves it false (no render thread, and async texture uploads from bundles are then
            # never processed). Carry the game's own choice over.
            if 'mobileMTRenderingBaked' in t and t['mobileMTRenderingBaked'] != t['m_MTRendering']:
                t['mobileMTRenderingBaked'] = t['m_MTRendering']
                notes.append(f'PlayerSettings.mobileMTRenderingBaked = m_MTRendering ({t["m_MTRendering"]})')
                changed = True
            if args.blit != 'keep':
                t['androidBlitType'] = {'always': 0, 'never': 1, 'auto': 2}[args.blit]
                notes.append(f'PlayerSettings.androidBlitType = {t["androidBlitType"]}')
                changed = True
            if args.swappy != 'keep':
                # Android Frame Pacing (Unity 2019.3+): a desktop build never ran it; off by default
                # (--swappy), as the runtime keeps Swappy off and Unity then cannot present with it on
                if 'androidUseSwappy' in t:
                    t['androidUseSwappy'] = args.swappy == 'on'
                    notes.append(f'PlayerSettings.androidUseSwappy = {t["androidUseSwappy"]}')
                    changed = True
                elif args.unity.line >= SWAPPY_FROM:
                    notes.append('PlayerSettings: no androidUseSwappy, --swappy ignored')
            if changed:
                o.save_typetree(t)
    old = sf._m_target_platform
    sf._m_target_platform = ANDROID
    notes.append(f'target platform {old} -> {ANDROID}')
    data = sf.save()
    log('globalgamemanagers: ' + '; '.join(notes))
    return data, notes


# MonoManager, Unity 2017.x: slots 0..47 of m_AssemblyNames are the engine's (modules, then one
# reserved slot); the game's assemblies start at 48. The list is kept as is: the slot index
# matters. MonoManager::GetAvailableDllAssemblyMask loads slots 0..46 whose file exists, never 47,
# then 48+, so an engine module the Android player lacks (UnityEngine.Cluster*Module.dll) is simply
# not loaded; removing it would shift the game's assemblies into engine slots.
# From Unity 2018 (2018.4 - 2022.3 disassembled) no slot is reserved: every listed assembly that
# loaded is taken, filtered by its flags in m_AssemblyTypes, so missing engine modules are
# skipped wherever they are. From 2020.2 the list is Data/ScriptingAssemblies.json
# ({"names": [...], "types": [...]}), which the player reads instead of MonoManager fields.
BUILTIN_ASSEMBLY_SLOTS = 48


def is_engine_module(name):
    return name == 'UnityEngine.dll' or bool(re.match(r'^UnityEngine\.\w+Module\.dll$', name))


def split_assemblies(names, unity=None):
    """(game assemblies, engine modules) of an assembly list (MonoManager.m_AssemblyNames or
    ScriptingAssemblies.json names): Unity 2017 by slot, later by name."""
    if unity is None or unity.major <= 2017:
        return ([a for i, a in enumerate(names) if i >= BUILTIN_ASSEMBLY_SLOTS and a],
                [a for i, a in enumerate(names) if i < BUILTIN_ASSEMBLY_SLOTS and a])
    return [a for a in names if a and not is_engine_module(a)], [a for a in names if a and is_engine_module(a)]


# --------------------------------------------------------------------------- plan

MONO_CONFIG_EXTRA = [
    # a port helper P/Invoking the runtime by its Linux name (libmono.so.0); on Android it is libmono.so
    ('libmono.so.0', 'libmono.so'),
]
# a macOS build's Mono config names the Mac libraries; the Android player runs on Linux (the
# runtime's loader maps libc.so.6 to the host's glibc)
DYLIB_TO_SO = [
    (r'libc\.dylib', 'libc.so.6'), (r'libintl\.dylib', 'libc.so.6'),
    (r'lib([\w.+-]+?)\.(\d+)\.dylib', r'lib\1.so.\2'), (r'lib([\w.+-]+?)\.dylib', r'lib\1.so'),
]


def mono_config_text(txt, extra):
    """Mono's etc/mono/config for the Android player: Mac library names -> Linux ones, plus the
    extra dllmaps [(dll, target)] the port asks for (--dllmap)."""
    lines = txt.splitlines(keepends=True)
    for i, l in enumerate(lines):
        if 'os="osx"' in l:
            continue                                   # macOS-only entries never apply
        for rx, to in DYLIB_TO_SO:
            l = re.sub(r'target="' + rx + '"', lambda m: 'target="' + m.expand(to) + '"', l)
        lines[i] = l
    txt = ''.join(lines)
    for dll, target in list(extra) + list(MONO_CONFIG_EXTRA):
        line = f'\t<dllmap dll="{dll}" target="{target}" os="!windows"/>\n'
        if f'dll="{dll}"' not in txt:
            txt = txt.replace('</configuration>', line + '</configuration>')
    return txt


# read by the player from the data folder (Unity 2020.2+): the assembly list MonoManager had
# before, and the [RuntimeInitializeOnLoadMethod]s to call
DATA_JSON = ('ScriptingAssemblies.json', 'RuntimeInitializeOnLoads.json')


def mono_etc_dir(src):
    """Mono's etc folder of a desktop build: <Game>_Data/Mono/etc (.NET 3.5 runtime) or
    <Game>_Data/MonoBleedingEdge/etc (.NET 4.x); a Windows build has MonoBleedingEdge next to
    <Game>_Data, a macOS build in Contents/, two levels up from Contents/Resources/Data."""
    up = os.path.dirname(os.path.abspath(src))
    for d in (src, up, os.path.dirname(up)):
        for m in ('Mono', 'MonoBleedingEdge'):
            p = os.path.join(d, m, 'etc')
            if os.path.isdir(p):
                return p
    return os.path.join(src, 'Mono', 'etc')


def plan(args):
    """[(dst_rel, src, action)] action: link | sfpatch | bundle | ggm | text:<content> | monoconfig"""
    src, pkg = args.src, args.pkg
    data = os.path.join(args.name, 'assets', 'bin', 'Data')
    items = []
    for n in sorted(os.listdir(src)):
        p = os.path.join(src, n)
        if not os.path.isfile(p) or n.startswith('.'):
            continue
        if n == 'globalgamemanagers':
            items.append((os.path.join(data, n), p, 'ggm'))
        elif n in ('globalgamemanagers.assets', 'resources.assets') or re.match(r'^(level\d+|sharedassets\d+\.assets)$', n):
            inline = args.inline_textures and os.path.exists(p + '.resS')
            items.append((os.path.join(data, n), p, 'sfinline' if inline else 'sfpatch'))
        elif n.endswith('.resS') and args.inline_textures and os.path.exists(p[:-5]):
            args.inlined_ress.append(n)                    # moved into its serialized file
        elif n.endswith('.resS') or n.endswith('.resource'):
            items.append((os.path.join(data, n), p, 'link'))
        elif n == 'boot.config' or n in DATA_JSON:
            items.append((os.path.join(data, n), p, 'link'))
        elif n in args.root_file:                           # read by the game from Application.dataPath/<n>
            items.append((os.path.join(args.name, n), p, 'link'))
        else:
            args.skipped.append(n)
    # realtime GI data (Enlighten: GI/level<n>/..), read by scene loads as in an Android build's Data
    gi = os.path.join(src, 'GI')
    for r, ds, fs in os.walk(gi):
        ds.sort()
        for f in sorted(fs):
            if not f.startswith('.'):
                items.append((os.path.join(data, 'GI', os.path.relpath(os.path.join(r, f), gi)), os.path.join(r, f), 'link'))
    items.append((os.path.join(data, 'unity default resources'), os.path.join(pkg, 'Data', 'unity default resources'), 'link'))
    bex = os.path.join(src, 'Resources', 'unity_builtin_extra')
    # the Android VideoPlayer's shader, made from the desktop one (video.py)
    args.video_shader_pid = video.path_id_for(bex) if args.video_shader and video.has_template(bex) else None
    args.video_bt709 = bool(args.video_shader_pid) and video.wants_bt709(src, print)
    act = ('sfvideo709' if args.video_bt709 else 'sfvideo') if args.video_shader_pid else 'sfpatch'
    items.append((os.path.join(data, 'Resources', 'unity_builtin_extra'), bex, act))
    # Managed: the Android engine assemblies, the game's own and Unity's extension/BCL assemblies
    amanaged = os.path.join(pkg, 'Variations', 'mono', 'Managed')
    names = set()
    for n in sorted(os.listdir(amanaged)):
        if n.endswith('.dll'):
            items.append((os.path.join(data, 'Managed', n), os.path.join(amanaged, n), 'link'))
            names.add(n)
    gmanaged = os.path.join(src, 'Managed')
    override = {os.path.basename(f): os.path.abspath(f) for f in args.managed_file}
    patched = dict(args.assembly_patch)
    for n in sorted(set(os.listdir(gmanaged)) | set(override)):
        if not n.endswith('.dll'):
            continue                                        # *.dll.so: x86-64 AOT images, useless on ARM
        if is_engine_module(n):
            if n not in names:
                args.dropped_modules.append(n)              # standalone-only (Cluster*): not shipped
            continue
        if n in override:
            # e.g. the port's runtime helper, built separately (newer than the converted data's copy)
            items.append((os.path.join(data, 'Managed', n), override[n], 'link'))
        elif n in patched:
            # written by the port's patch command from the original game's assemblies
            orig = os.path.join(args.game_managed, n)
            items.append((os.path.join(data, 'Managed', n), orig, 'patch:' + n + ':' + _cmd_key(patched[n])))
        else:
            items.append((os.path.join(data, 'Managed', n), os.path.join(gmanaged, n), 'link'))
        names.add(n)
    args.managed_names = names
    # Mono config tree (dllmaps); libunity passes "assets/bin/Data/Managed/etc" (relative) as config dir
    metc = args.mono_etc or mono_etc_dir(src)
    for r, ds, fs in os.walk(metc):
        for n in fs:
            s = os.path.join(r, n)
            rel = os.path.relpath(s, metc)
            act = 'monoconfig' if rel == os.path.join('mono', 'config') else 'link'
            items.append((os.path.join(data, 'Managed', 'etc', rel), s, act))
    # StreamingAssets -> assets/
    sa = os.path.join(src, 'StreamingAssets')
    for r, ds, fs in os.walk(sa):
        ds.sort()
        for n in sorted(fs):
            if n.startswith('.'):
                continue
            s = os.path.join(r, n)
            rel = os.path.relpath(s, sa)
            act = 'bundle' if rel.startswith('AssetBundles' + os.sep) and is_unityfs(s) else 'link'
            items.append((os.path.join(args.name, 'assets', rel), s, act))
    # files from the game's folder beside the root (GOG .info files: DLC detection)
    gd = os.path.dirname(src)
    for n in sorted(os.listdir(gd)):
        if any(fnmatch.fnmatch(n, g) for g in args.beside):
            items.append((n, os.path.join(gd, n), 'link'))
    return items


def player_version(pkg):
    """The Unity version of an Android player package: its player.json (`pmunity player fetch`), its
    ivy.xml (2017.x, 2018.x), else the version string in its Mono libunity.so, else that of its
    "unity default resources" (the Unity line only: that file can be older than the player)."""
    pj = os.path.join(pkg, 'player.json')
    if os.path.isfile(pj):
        try:
            with open(pj) as f:
                return UV.Version(json.load(f)['version'])
        except (ValueError, KeyError):
            pass
    ivy = os.path.join(pkg, 'ivy.xml')
    if os.path.isfile(ivy):
        m = re.search(r'unityVersion="([^"]+)"', open(ivy, encoding='utf-8', errors='replace').read())
        if m:
            return UV.Version(m.group(1))
    for name in ('modules.asset', 'version.txt'):           # (installer extras, when kept)
        p = os.path.join(pkg, name)
        if os.path.isfile(p):
            m = re.search(r'\b(20\d\d\.\d+\.\d+[abfp]\d+)\b', open(p, encoding='utf-8', errors='replace').read())
            if m:
                return UV.Version(m.group(1))
    for abi in ('armeabi-v7a', 'arm64-v8a'):
        lib = os.path.join(pkg, 'Variations', 'mono', 'Release', 'Libs', abi, 'libunity.so')
        if os.path.isfile(lib):
            with open(lib, 'rb') as f:
                found = re.findall(rb'\b(20\d\d\.\d+\.\d+[abfp]\d+)\x00', f.read())
            if found:
                best = max(set(found), key=found.count)
                return UV.Version(best.decode())
    udr = os.path.join(pkg, 'Data', 'unity default resources')
    if os.path.isfile(udr):
        v = UV.of_file(udr)
        v.exact = False                 # the file is often from an earlier patch of the line
        return v
    return None


def scripting_runtime(src, unity):
    """'.NET 3.5 (libmono)' or '.NET 4.x (libmonobdwgc-2.0)': which Mono runtime of the Android
    player the game needs (Unity 2017.x / 2018.x had both; 2019.1+ only the second)."""
    if unity.major >= 2019:
        return '.NET 4.x (libmonobdwgc-2.0)'
    etc = mono_etc_dir(src)
    if os.path.basename(os.path.dirname(etc)) == 'MonoBleedingEdge':
        return '.NET 4.x (libmonobdwgc-2.0)'
    if os.path.basename(os.path.dirname(etc)) == 'Mono':
        return '.NET 3.5 (libmono)'
    return None


def check_source(args):
    """The data's Unity version (supported range, the player's Unity line) and build kind: a
    compressed build (data.unity3d) must be unpacked first; an IL2CPP build has no assemblies for
    the Mono player."""
    src = args.src
    args.warnings = []
    if not os.path.isfile(os.path.join(src, 'globalgamemanagers')) and os.path.isfile(os.path.join(src, 'data.unity3d')):
        raise UV.Unsupported(f'{src}: a compressed build (data.unity3d): unpack it first with '
                             f'`python -m pmunity unpack {src}`')
    args.unity = UV.check(UV.of_data(src), args.any_unity_version)
    gm = os.path.join(src, 'Managed')
    if not os.path.isdir(gm) or not any(n.endswith('.dll') for n in os.listdir(gm)):
        il2cpp = [n for n in ('il2cpp_data', 'Il2CppData', '../GameAssembly.so', '../GameAssembly.dll', '../GameAssembly.dylib')
                  if os.path.exists(os.path.join(src, n))]
        raise UV.Unsupported(f'{src}: no managed assemblies (Managed/*.dll)' +
                             (f': an IL2CPP build ({", ".join(il2cpp)}), which the Mono Android player cannot run'
                              if il2cpp else ''))
    args.player_unity = player_version(args.pkg) if os.path.isdir(args.pkg) else None
    if args.player_unity is not None:
        if args.player_unity.line != args.unity.line and not args.any_unity_version:
            raise UV.Unsupported(f'the data is Unity {args.unity}, the Android player Unity {args.player_unity}: '
                                 f'use the Android player of Unity {args.unity.major}.{args.unity.minor} '
                                 f'(serialized data has no type trees: the player must read the same layouts)')
        if getattr(args.player_unity, 'exact', True) and \
                (args.player_unity.patch, args.player_unity.build) != (args.unity.patch, args.unity.build):
            args.warnings.append(f'data of Unity {args.unity}, Android player {args.player_unity}: same line, other patch '
                                 f'(layouts rarely change within a line; the same version is safest)')
    args.runtime = scripting_runtime(src, args.unity)
    # native plugins: what the managed code P/Invokes and what the port gives each (plugins.py)
    from . import plugins as PL
    try:
        args.plugins = PL.survey(src)
        pm = getattr(args, 'plugin_map', None)
        port_map = json.load(open(pm)) if pm else {}
        args.plugin_entries = PL.merge_map(args.plugins, port_map)
    except (OSError, ValueError) as ex:
        raise UV.Unsupported(f'plugins: {ex}')
    args.warnings += ['plugin ' + line for line in PL.summary(args.plugins)]
    if getattr(args, 'loose_assets', None) is None:   # FMOD reads its banks with plain file I/O: beside the APK
        args.loose_assets = ['*.bank'] if args.plugins.get('fmod') else []


def run_asmpatch(args, log):
    """--asmpatch: tools/asmpatch on the game's Managed folder writes the assemblies its patches
    change into <out>/.asmpatch; they are shipped like --managed-file ones."""
    src = args.game_managed or os.path.join(args.src, 'Managed')
    work = os.path.join(args.out, '.asmpatch')
    shutil.rmtree(work, ignore_errors=True)
    r = subprocess.run(shlex.split(args.asmpatch) + [src, work], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        log('asmpatch: ' + line)
    if r.returncode != 0:
        raise SystemExit(f'asmpatch failed (exit {r.returncode}):\n{r.stdout}{r.stderr}')
    written = sorted(n for n in os.listdir(work) if n.endswith('.dll')) if os.path.isdir(work) else []
    clash = [n for n in written if n in dict(args.assembly_patch) or n in {os.path.basename(f) for f in args.managed_file}]
    if clash:
        raise SystemExit(f'asmpatch changed {", ".join(clash)}, which --assembly-patch/--managed-file also provide: '
                         f'run asmpatch on that assembly inside the patch command instead')
    args.managed_file += [os.path.join(work, n) for n in written]
    return [line for line in r.stdout.splitlines() if line.startswith(('ok', 'done', 'FAIL'))]


def _cmd_key(cmd):
    """State key of an assembly patch command: its text and the modification times of the files it names."""
    parts = [cmd]
    for w in shlex.split(cmd):
        if os.path.isfile(w):
            parts.append(str(os.stat(w).st_mtime_ns))
    return '|'.join(parts)


def is_unityfs(p):
    with open(p, 'rb') as f:
        return f.read(8) == b'UnityFS\0'


# --------------------------------------------------------------------------- run

def main(argv=None):
    ap = argparse.ArgumentParser(prog='pmunity android-data', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, help="the game's converted <Game>_Data folder (read only)")
    ap.add_argument('--pkg', default=os.environ.get('PMUNITY_ANDROID_PLAYER'),
                    help="Unity's Android player package, unpacked from the editor's Android Build Support installer (the "
                         "folder with Variations/ and Data/; env PMUNITY_ANDROID_PLAYER)")
    ap.add_argument('--out', default=DEF_OUT)
    ap.add_argument('--name', default=None, help='name of the APK-root directory (default: the game name from --src; must not end in .apk/.obb/.zip/.jar)')
    ap.add_argument('--platform-patch', choices=['data', 'none'], default='data',
                    help="data: set every serialized file's target platform to Android (13); none: leave "
                         "them (only if libunity's check in SerializedFile::ReadMetadata is patched)")
    ap.add_argument('--graphics-apis', default=str(GLES3), help='BuildSettings.m_GraphicsAPIs (11 = GLES3, 8 = GLES2)')
    ap.add_argument('--mt-rendering', choices=['keep', 'on', 'off'], default='keep', help='PlayerSettings.m_MTRendering (game: on)')
    ap.add_argument('--blit', choices=['keep', 'always', 'never', 'auto'], default='never',
                    help="PlayerSettings.androidBlitType. Default never: Unity draws straight into the window instead of "
                         "an offscreen buffer copied to it every frame (same pixels, one full-screen pass less; the "
                         "game's own value, Android's default, is always)")
    ap.add_argument('--swappy', choices=['keep', 'off', 'on'], default='off',
                    help="PlayerSettings.androidUseSwappy (Unity 2019.3+: Android Frame Pacing, never used by a desktop "
                         "build). Default off: Unity paces frames itself, as on the desktop. The runtime keeps Swappy off, "
                         "and with the setting on Unity presents nothing (2019.4) or stops after its first frames "
                         "(2021.3+); keep/on only for a runtime that runs Swappy")
    ap.add_argument('--any-unity-version', action='store_true',
                    help='convert data of a Unity version outside 2017.3 - 2022.3, or for an Android player of another '
                         'Unity line (untested; the formats are then guessed)')
    ap.add_argument('--async-upload-buffer', type=int, default=0, help='QualitySettings.asyncUploadBufferSize in MB (0 = keep)')
    ap.add_argument('--no-inline-textures', dest='inline_textures', action='store_false',
                    help="keep textures' pixels in .resS streams (the Android player never finishes "
                         "an asynchronous load of a streamed texture; see inline_texture_streams)")
    ap.add_argument('--no-video-shader', dest='video_shader', action='store_false',
                    help="do not add Hidden/VideoDecodeAndroid (the Android VideoPlayer's shader, made from the data's "
                         "Hidden/VideoDecode; without it no video frame shows)")
    ap.add_argument('--rewired-joysticks', type=int, default=4, help="joysticks given Rewired's Unity-input axes (Joy<j>Axis1-28; Rewired supports 11)")
    ap.add_argument('--managed-file', action='append', default=[], metavar='DLL',
                    help='an assembly shipped instead of the data\'s copy of the same name (or added)')
    ap.add_argument('--assembly-patch', action='append', default=[], nargs=2, metavar=('NAME', 'COMMAND'),
                    help='NAME (e.g. Assembly-CSharp.dll) is written by COMMAND, which gets {managed} (the original '
                         'game\'s Managed folder, --game-managed) and {out} (a folder to write NAME into)')
    ap.add_argument('--game-managed', default=None, help="the original game's Managed folder (input of --assembly-patch)")
    ap.add_argument('--asmpatch', default=None, metavar='COMMAND',
                    help="tools/asmpatch's command (its path and options, e.g. 'asmpatch --only=rewired-android' or "
                         "'dotnet asmpatch.dll'): run on the game's Managed folder (--game-managed, else <src>/Managed); "
                         "the assemblies it changes are shipped instead of the game's copies")
    ap.add_argument('--root-file', action='append', default=[], metavar='NAME',
                    help='a file of --src the game reads from Application.dataPath (placed at the APK root)')
    ap.add_argument('--mono-etc', default=None, metavar='DIR',
                    help="Mono's etc folder (default <src>/Mono/etc; a macOS build has it in <Game>.app/Contents/Mono/etc, "
                         "its .dylib names become Linux ones)")
    ap.add_argument('--dllmap', action='append', default=[], metavar='DLL=TARGET',
                    help='an extra dllmap line in the Mono config (P/Invoke DLL -> library)')
    ap.add_argument('--plugin-map', default=None, metavar='JSON',
                    help='the port\'s plugin map over the defaults (a {"<DllImport name>": {"use": lib|host|stub|none, ...}} '
                         'object, or a port config with a "plugins" one): written as plugins.map for ua_host (docs/plugins.md)')
    ap.add_argument('--loose-assets', action='append', default=None, metavar='GLOB',
                    help='StreamingAssets files kept beside the APK (assets/<path>) instead of in the OBB, for libraries '
                         'that open them with plain file I/O (default: *.bank when the game uses FMOD)')
    ap.add_argument('--beside', action='append', default=None, metavar='GLOB',
                    help="files of the game's folder (the parent of --src) put beside the APK root (default goggame-*.info)")
    ap.add_argument('--force', action='store_true', help='redo every file')
    ap.add_argument('--consume-src', action='store_true',
                    help="move the data's files into the tree instead of copying them (and delete what is "
                         "rewritten): for a first-run setup on a file system without clones or hard links. "
                         "--src is used up; re-running continues from the tree")
    ap.add_argument('--verify-bundles', type=int, default=6, help='bundles fully loaded with UnityPy in the check (0 = none, -1 = all)')
    ap.add_argument('--no-verify', action='store_true')
    ap.add_argument('--pack', choices=['none', 'apk', 'obb', 'all'], default='none',
                    help='also pack the tree into zip archives for the stock (unpatched) Mount path: '
                         '<out>/zip/<name>.apk (assets/bin/Data) and main.<code>.<package>.obb (the rest)')
    ap.add_argument('--package', default=None, help='package name for the OBB file name (default com.pmunity.<name>)')
    ap.add_argument('--version-code', type=int, default=1, help='versionCode for the OBB file name')
    ap.add_argument('--zip-dir', default=None, help='where the archives go (default <out>/zip)')
    args = ap.parse_args(argv)
    if not args.pkg:
        sys.exit('--pkg: the unpacked Unity Android player package is needed (or set PMUNITY_ANDROID_PLAYER)')
    if args.name is None:
        base = os.path.basename(os.path.normpath(args.src))
        args.name = base[:-5] if base.endswith('_Data') else os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(args.src))))).replace('.app', '')
    if args.package is None:
        args.package = 'com.pmunity.' + re.sub(r'[^a-z0-9]', '', args.name.lower())
    if args.beside is None:
        args.beside = ['goggame-*.info']
    if args.assembly_patch and not args.game_managed:
        sys.exit('--assembly-patch needs --game-managed')
    if re.search(r'\.(apk|obb|zip|jar)$', args.name, re.I):
        sys.exit('--name must not end in .apk/.obb/.zip/.jar: the player would try to open paths under it as a zip')
    args.src, args.pkg, args.out = map(os.path.abspath, (args.src, args.pkg, args.out))
    real_out = os.path.realpath(args.out)
    for ro in [args.src] + ([args.game_managed] if args.game_managed else []):
        if real_out.startswith(os.path.realpath(ro)):
            sys.exit(f'refusing to write inside {ro}')
    args.skipped, args.dropped_modules, args.inlined_ress = [], [], []
    try:
        check_source(args)
    except UV.Unsupported as ex:
        sys.exit(str(ex))
    os.makedirs(args.out, exist_ok=True)
    t0 = time.time()
    logf = open(os.path.join(args.out, 'make_android_data.log'), 'a')

    def log(msg):
        print(msg)
        logf.write(time.strftime('%H:%M:%S ') + msg + '\n'); logf.flush()

    log(f'data: Unity {args.unity}' + (f'; Android player: Unity {args.player_unity}' if args.player_unity else '') +
        (f'; scripting runtime: {args.runtime}' if args.runtime else ''))
    for w in args.warnings:
        log('WARNING: ' + w)

    statep = os.path.join(args.out, '.android_data_state.json')
    state, placed_before = {}, set()
    if os.path.exists(statep):
        try:
            state = json.load(open(statep))
            placed_before = {d for d in state if not d.startswith('_')}   # for removing stale files
            if args.force or state.get('_version') != STATE_VERSION or state.get('_opts') != opts_key(args):
                state = {}
        except Exception:
            state = {}
    asmpatch_notes = run_asmpatch(args, log) if args.asmpatch else []
    items = plan(args)
    log(f'{len(items)} files planned from {args.src}')
    stats = {}
    report = {'bundles': {}, 'files': {}, 'notes': ['asmpatch: ' + n for n in asmpatch_notes]}
    newstate = {'_version': STATE_VERSION, '_opts': opts_key(args)}
    kept = set()                                    # .resS files placed beside their inlined file
    for k, (dst_rel, src, act) in enumerate(items):
        dst = os.path.join(args.out, dst_rel)
        if not os.path.exists(src) and dst_rel in state and os.path.exists(dst):
            newstate[dst_rel] = state[dst_rel]          # moved here by an earlier --consume-src run
            stats['unchanged'] = stats.get('unchanged', 0) + 1
            continue
        st = os.stat(src)
        key = [st.st_size, int(st.st_mtime_ns), act]
        if state.get(dst_rel) == key and os.path.exists(dst):
            newstate[dst_rel] = key
            for kr in state.get('_kept', {}).get(dst_rel, ()):
                newstate.setdefault('_kept', {}).setdefault(dst_rel, []).append(kr)
                newstate[kr] = state.get(kr, key)
                kept.add(kr)
            stats['unchanged'] = stats.get('unchanged', 0) + 1
            continue
        mv = consumable(args, src, act)
        if act == 'link' or (act in ('sfpatch', 'bundle') and args.platform_patch == 'none'):
            how = move(src, dst) if mv else clone(src, dst, allow_hardlink=True)
        elif act == 'sfpatch':
            how = move(src, dst) if mv else clone(src, dst, allow_hardlink=False)
            old = patch_sf_platform(dst)
            report['files'][dst_rel] = f'platform {old} -> {ANDROID}'
        elif act.startswith('patch:'):
            name = act.split(':')[1]
            with tempfile.TemporaryDirectory() as td:
                cmd = [w.replace('{managed}', args.game_managed).replace('{out}', td)
                       for w in shlex.split(dict(args.assembly_patch)[name])]
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode != 0:
                    raise SystemExit(f'assembly patch for {name} failed:\n{r.stdout}{r.stderr}')
                write_bytes(dst, open(os.path.join(td, name), 'rb').read())
            report['notes'].append(f'{name}: ' + ', '.join(l[5:] for l in r.stdout.splitlines() if l.startswith('ok')))
            how = 'patched'
        elif act == 'sfinline':
            n, still = inline_serialized(src, dst)
            report['files'][dst_rel] = f'platform -> {ANDROID}, {n} textures inlined'
            how = 'inlined'
            for name in sorted(still):          # e.g. mesh data: the .resS goes along
                s_res, d_res = os.path.join(os.path.dirname(src), name), os.path.join(os.path.dirname(dst), name)
                move(s_res, d_res) if mv else clone(s_res, d_res, allow_hardlink=True)
                kr = os.path.join(os.path.dirname(dst_rel), name)
                kept.add(kr)
                newstate[kr] = key
                newstate.setdefault('_kept', {}).setdefault(dst_rel, []).append(kr)
                report['files'][dst_rel] += f'; {name} kept (still read by meshes)'
            if mv:
                os.remove(src)
                if os.path.exists(src + '.resS') and os.path.basename(src) + '.resS' not in still:
                    os.remove(src + '.resS')
        elif act == 'bundle' and args.inline_textures and any(nm.endswith('.resS') for _, _, _, nm in Bundle(src).nodes):
            n, dropped = inline_bundle(src, dst)
            report['bundles'][os.path.basename(dst)] = f'{n} textures inlined, dropped {dropped}'
            how = 'inlined'
            if mv:
                os.remove(src)
        elif act == 'bundle':
            how = move(src, dst) if mv else clone(src, dst, allow_hardlink=False)
            res = patch_bundle_platform(dst)
            report['bundles'][os.path.basename(dst)] = res
            if any(m == 'rewrite' for _, _, m in res):
                log(f'  {dst_rel}: bundle rewritten (no same-size encoding of the patched block)')
        elif act == 'ggm':
            data, notes = edit_globalgamemanagers(src, args.managed_names, args, log)
            write_bytes(dst, data)
            report['notes'] += notes
            how = 'unitypy'
        elif act in ('sfvideo', 'sfvideo709'):
            import UnityPy
            sf = UnityPy.load(src).file
            pid = video.add_to_builtin_extra(sf, log, act == 'sfvideo709')
            if pid != args.video_shader_pid:
                raise SystemExit(f'video: shader got path id {pid}, globalgamemanagers maps {args.video_shader_pid}')
            sf._m_target_platform = ANDROID
            write_bytes(dst, sf.save())
            report['files'][dst_rel] = f'platform -> {ANDROID}, {video.NAME} added (path id {pid})'
            how = 'video'
        elif act == 'monoconfig':
            txt = mono_config_text(open(src).read(), [d.split('=', 1) for d in args.dllmap])
            write_bytes(dst, txt.encode())
            how = 'written'
        else:
            raise ValueError(act)
        stats[how] = stats.get(how, 0) + 1
        newstate[dst_rel] = key
        if (k + 1) % 100 == 0:
            log(f'  {k + 1}/{len(items)} ({time.time() - t0:.0f} s)')
    # remove files from earlier runs that are no longer planned
    planned = {d for d, s, a in items} | kept
    for d in sorted(placed_before):
        if d not in planned:
            p = os.path.join(args.out, d)
            if args.consume_src and d in state and os.path.lexists(p):
                newstate[d] = state[d]                  # its source was moved here by an earlier run
                continue
            if os.path.lexists(p):
                os.unlink(p)
                log(f'removed stale {d}')
    json.dump(newstate, open(statep, 'w'), indent=0)
    log(f'placed: {stats}; skipped source files: {args.skipped}; engine modules not shipped: {args.dropped_modules}; '
        f'.resS moved into their serialized files: {len(args.inlined_ress)}')
    report['stats'] = stats
    report['unity'] = {'data': str(args.unity), 'player': str(args.player_unity) if args.player_unity else None,
                       'scripting_runtime': args.runtime, 'warnings': args.warnings}
    report['skipped'] = args.skipped
    report['dropped_modules'] = args.dropped_modules
    report['plugins'] = {k: v for k, v in args.plugins.items() if k not in ('sigs', 'map')}
    report['plugins']['map'] = args.plugin_entries
    report['plugins']['float_signatures'] = len(args.plugins['sigs'])
    write_plugin_map(args, args.out, log)
    if not args.no_verify:
        report['verify'] = verify(args, items, log)
    if args.pack != 'none':
        report['pack'] = pack(args, log)
    json.dump(report, open(os.path.join(args.out, 'android_data_report.json'), 'w'), indent=1, default=str)
    log(f'done in {time.time() - t0:.0f} s -> {os.path.join(args.out, args.name)}')


# --------------------------------------------------------------------------- zip archives

ZIP_LIMIT = 2 ** 31 - 1     # libunity reads archives with fopen/fseek(long)/ftell: 32-bit offsets


def build_id(args):
    """unity.build-id: an OBB is mounted only if it holds an entry "assets/<build-id>"; the
    player gets the id from the manifest meta-data (fake PackageManager). Derived from
    BuildSettings.buildGUID so it is stable (Unity 2022 has none there: PlayerSettings.productGUID,
    else a hash of the settings file)."""
    import hashlib
    import UnityPy
    path = os.path.join(args.out, args.name, 'assets', 'bin', 'Data', 'globalgamemanagers')
    env = UnityPy.load(path)
    guid = lambda g: ''.join('%08x' % g[f'data[{i}]'] for i in range(4))
    h = None
    for cls, field in (('BuildSettings', 'buildGUID'), ('PlayerSettings', 'productGUID')):
        for o in env.file.objects.values():
            if o.type.name == cls and h is None:
                t = o.read_typetree()
                if field in t and any(t[field].values()):
                    h = guid(t[field])
    if h is None:
        with open(path, 'rb') as f:
            h = hashlib.sha1(f.read()).hexdigest()[:32]
    return f'{h[:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:]}'


# Unity 2021+: an OBB is matched by its GUID file (libunity 2022.3: "Application OBB has mismatching
# GUID"; 2017.4 - 2020.3 use the unity.build-id marker): the APK's assets/unity_obb_guid (2021.3:
# AssetManager.open("unity_obb_guid"), 2022.3: <apk>/assets/unity_obb_guid) must equal the OBB's
# unity_obb_guid at the root of the archive (2021.3: ZipFile.getEntry("unity_obb_guid"), 2022.3: its
# own zip reader, which reads the archive's first entry when the name is missing). Both hold the build id.
OBB_GUID_FROM = (2021, 1)
SWAPPY_FROM = (2019, 3)          # PlayerSettings.androidUseSwappy (Optimized Frame Pacing)
OBB_GUID_FILE = 'assets/unity_obb_guid'
OBB_GUID_FILE_IN_OBB = 'unity_obb_guid'


def _write_zip(path, entries, log, align=4):
    """Uncompressed (STORE) zip, no zip64, every file's data 4-byte aligned (zipalign-style
    extra field), written to path.tmp then renamed. entries: [(arcname, src path or bytes)]."""
    import zipfile
    total = sum(30 + 46 + 2 * len(a.encode()) + 8 + (len(s) if isinstance(s, bytes) else os.path.getsize(s)) for a, s in entries) + 22
    if total > ZIP_LIMIT:
        raise SystemExit(f'{path}: {total} bytes would exceed the 2 GiB limit')
    tmp = path + '.tmp'
    with zipfile.ZipFile(tmp, 'w', compression=zipfile.ZIP_STORED, allowZip64=False) as z:
        for arc, src in entries:
            zi = zipfile.ZipInfo(arc, date_time=(2018, 8, 2, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0o644 << 16
            off = z.fp.tell() + 30 + len(arc.encode())
            pad = (-(off + 4)) % align
            zi.extra = struct.pack('<HH', 0xD935, pad) + b'\0' * pad
            zi.file_size = len(src) if isinstance(src, bytes) else os.path.getsize(src)
            if isinstance(src, bytes):
                z.writestr(zi, src)
            else:
                with open(src, 'rb') as fi, z.open(zi, 'w') as fo:
                    shutil.copyfileobj(fi, fo, 1 << 20)
    size = os.path.getsize(tmp)
    if size > ZIP_LIMIT:
        os.unlink(tmp)
        raise SystemExit(f'{path}: {size} bytes exceeds the 2 GiB limit')
    os.replace(tmp, path)
    log(f'pack: {path}: {len(entries)} entries, {size / 2**20:.1f} MiB')
    return size


def verify_zip(path, entries, log, sample=40):
    """Central directory as libunity's reader needs it (zip32, STORE) and data in place."""
    import zipfile, random
    with zipfile.ZipFile(path) as z, open(path, 'rb') as f:
        infos = z.infolist()
        assert len(infos) == len(entries), (len(infos), len(entries))
        bad = [i.filename for i in infos if i.compress_type != zipfile.ZIP_STORED or i.file_size != i.compress_size]
        assert not bad, bad[:5]
        src = dict(entries)
        rnd = random.Random(1)
        for i in rnd.sample(infos, min(sample, len(infos))) + infos[-3:]:
            f.seek(i.header_offset)
            h = f.read(30)
            n, e = struct.unpack_from('<HH', h, 26)
            data_off = i.header_offset + 30 + n + e
            assert data_off % 4 == 0, i.filename
            f.seek(data_off)
            got = f.read(min(i.file_size, 1 << 16))
            s = src[i.filename]
            want = s[:len(got)] if isinstance(s, bytes) else open(s, 'rb').read(len(got))
            assert got == want, i.filename
    log(f'pack: {os.path.basename(path)} verified (zip32, stored, aligned, data in place)')


def pack(args, log):
    """APK = assets/bin/Data/** (what Unity reads first: Mono assemblies, settings, scenes);
    main OBB = assets/AssetBundles/**, assets/Logo.png and the build-id marker; a patch OBB takes
    the overflow if the main one would pass 2 GiB. The player searches every mounted archive
    (OBBs first) for a path under the APK, so the split is free."""
    zdir = os.path.abspath(args.zip_dir or os.path.join(args.out, 'zip'))
    os.makedirs(zdir, exist_ok=True)
    root = os.path.join(args.out, args.name)
    files = []
    for r, ds, fs in os.walk(root):
        ds.sort()
        for n in sorted(fs):
            p = os.path.join(r, n)
            files.append((os.path.relpath(p, root).replace(os.sep, '/'), p))
    bid = build_id(args)
    apk_entries = [(a, p) for a, p in files if a.startswith('assets/bin/')]
    obb_entries = [(a, p) for a, p in files if a.startswith('assets/') and not a.startswith('assets/bin/')]
    loose = [(a, p) for a, p in obb_entries if any(fnmatch.fnmatch(a[len('assets/'):], g) or
                                                   fnmatch.fnmatch(os.path.basename(a), g) for g in args.loose_assets)]
    obb_entries = [e for e in obb_entries if e not in loose]
    beside = [(a, p) for a, p in files if not a.startswith('assets/')]   # read with plain file I/O (--root-file)
    marker = ('assets/' + bid, b'')
    res = {'build_id': bid, 'package': args.package, 'version_code': args.version_code}
    if args.unity.line >= OBB_GUID_FROM:
        apk_entries.append((OBB_GUID_FILE, bid.encode('ascii')))
        marker = [marker, (OBB_GUID_FILE_IN_OBB, bid.encode('ascii'))]
        res['unity_obb_guid'] = bid
    else:
        marker = [marker]
    apk = os.path.join(zdir, args.name + '.apk')
    main_obb = os.path.join(zdir, f'main.{args.version_code}.{args.package}.obb')
    patch_obb = os.path.join(zdir, f'patch.{args.version_code}.{args.package}.obb')
    if args.pack in ('apk', 'all'):
        _write_zip(apk, apk_entries, log)
        verify_zip(apk, apk_entries, log)
        res['apk'] = apk
    if args.pack in ('obb', 'all'):
        need = sum(os.path.getsize(p) for a, p in obb_entries)
        st = os.statvfs(zdir)
        free = st.f_bavail * st.f_frsize
        if free < need + (1 << 30):
            raise SystemExit(f'pack: OBB needs {need / 2**30:.2f} GiB, only {free / 2**30:.2f} GiB free (keeping 1 GiB spare)')
        main, rest, size = list(marker), [], 0
        for a, p in obb_entries:
            sz = os.path.getsize(p) + 30 + 46 + 2 * len(a) + 8
            if size + sz < ZIP_LIMIT - (16 << 20) and not rest:
                main.append((a, p)); size += sz
            else:
                rest.append((a, p))
        _write_zip(main_obb, main, log)
        verify_zip(main_obb, main, log)
        res['main_obb'] = main_obb
        if rest:
            _write_zip(patch_obb, marker + rest, log)
            verify_zip(patch_obb, marker + rest, log)
            res['patch_obb'] = patch_obb
    # what lives outside the archives: Mono reads its config with plain fopen relative to the
    # cwd ("assets/bin/Data/Managed/etc"), and the --beside files sit beside the APK
    for a, p in files:
        if a.startswith('assets/bin/Data/Managed/etc/'):
            clone(p, os.path.join(zdir, a), allow_hardlink=True)
    for n in os.listdir(args.out):
        if any(fnmatch.fnmatch(n, g) for g in args.beside):
            clone(os.path.join(args.out, n), os.path.join(zdir, n), allow_hardlink=True)
    # files the game reads beside its data (Application.dataPath/.. on Android: beside the APK/OBB)
    for a, p in beside:
        clone(p, os.path.join(zdir, a), allow_hardlink=True)
    # StreamingAssets read with plain file I/O (FMOD banks): ua_host gives their jar:file:// paths
    # these files (runtime/host/plugins.c)
    for a, p in loose:
        clone(p, os.path.join(zdir, a), allow_hardlink=True)
    if loose:
        res['loose_assets'] = [a for a, p in loose]
        log(f'pack: {len(loose)} StreamingAssets files beside the APK ({", ".join(args.loose_assets)})')
    write_plugin_map(args, zdir, log)
    # read by ua_host from beside the APK (package, build id, APK name): file names only, so the
    # folder can be copied anywhere
    pj = {k: (os.path.basename(v) if k in ('apk', 'main_obb', 'patch_obb') else v) for k, v in res.items()}
    json.dump(dict(pj, meta_data={'unity.build-id': bid}), open(os.path.join(zdir, 'android_package.json'), 'w'), indent=1)
    log(f'pack: build-id {bid}; host: getPackageCodePath -> {apk}, getObbDir -> {zdir}, '
        f'PackageInfo.versionCode {args.version_code}, package {args.package}')
    return res


def write_plugin_map(args, where, log):
    """plugins.map (runtime/host/plugins.c) from the plugin survey and the port's map."""
    from . import plugins as PL
    txt = PL.map_text(args.plugins, args.plugin_entries)
    with open(os.path.join(where, 'plugins.map'), 'w') as f:
        f.write(txt)
    n = sum(1 for line in txt.splitlines() if line.startswith('map '))
    log(f'plugins.map: {n} libraries, {sum(1 for line in txt.splitlines() if line.startswith("sig "))} float signatures -> {where}')


def opts_key(args):
    key = [args.platform_patch, args.graphics_apis, args.mt_rendering, args.blit, args.rewired_joysticks, args.name, args.async_upload_buffer,
           args.inline_textures]
    if not args.video_shader:
        key.append('no-video-shader')
    if args.swappy != 'keep' and args.unity.line >= SWAPPY_FROM:     # the setting exists from 2019.3
        key.append('swappy=' + args.swappy)
    return key


def verify(args, items, log):
    """Loads the result with UnityPy: every plain serialized file (platform, object count), every
    globalgamemanagers object (type tree read), every bundle's CAB header (platform), and
    --verify-bundles bundles completely (all objects parsed)."""
    import UnityPy
    from UnityPy.files import SerializedFile
    want = ANDROID if args.platform_patch == 'data' else None
    bad = []
    n_sf = 0
    for dst_rel, src, act in items:
        dst = os.path.join(args.out, dst_rel)
        if act in ('sfpatch', 'sfinline', 'ggm', 'sfvideo', 'sfvideo709'):
            env = UnityPy.load(dst)
            sf = env.file
            if not isinstance(sf, SerializedFile):
                bad.append((dst_rel, 'not a serialized file')); continue
            env0 = UnityPy.load(src) if os.path.exists(src) else None      # (gone with --consume-src)
            added = 1 if act.startswith('sfvideo') else 0                 # Hidden/VideoDecodeAndroid
            if env0 is not None and len(sf.objects) != len(env0.file.objects) + added:
                bad.append((dst_rel, f'{len(sf.objects)} objects, source {len(env0.file.objects)}'))
            if want is not None and sf._m_target_platform != want:
                bad.append((dst_rel, f'platform {sf._m_target_platform}'))
            if act == 'ggm':
                for o in sf.objects.values():
                    o.read_typetree()
            n_sf += 1
    log(f'verify: {n_sf} serialized files load in UnityPy')
    bundles = [(d, s) for d, s, a in items if a == 'bundle']
    for d, s in bundles:
        for name, plat in bundle_platforms(os.path.join(args.out, d)):
            if want is not None and plat != want:
                bad.append((d, f'{name} platform {plat}'))
    log(f'verify: {len(bundles)} bundles: CAB headers checked')
    nb = len(bundles) if args.verify_bundles < 0 else args.verify_bundles
    if nb and bundles:
        by_size = sorted(bundles, key=lambda x: os.path.getsize(os.path.join(args.out, x[0])))
        pick = by_size if nb >= len(by_size) else [by_size[int(i * (len(by_size) - 1) / max(1, nb - 1))] for i in range(nb)]
        for d, s in pick:
            env = UnityPy.load(os.path.join(args.out, d))
            cnt = 0
            for f in env.file.files.values():
                if isinstance(f, SerializedFile):
                    if want is not None and f._m_target_platform != want:
                        bad.append((d, f'UnityPy platform {f._m_target_platform}'))
                    for o in f.objects.values():
                        o.read_typetree(); cnt += 1
            log(f'verify: {d}: {cnt} objects parsed')
            env = None
    # the Managed set must cover what MonoManager loads
    ggm = os.path.join(args.out, args.name, 'assets', 'bin', 'Data', 'globalgamemanagers')
    env = UnityPy.load(ggm)
    lists = []
    for o in env.file.objects.values():
        if o.type.name == 'MonoManager':
            t = o.read_typetree()
            if 'm_AssemblyNames' in t:
                lists.append(('MonoManager', t['m_AssemblyNames']))
    sa = os.path.join(args.out, args.name, 'assets', 'bin', 'Data', 'ScriptingAssemblies.json')
    if os.path.exists(sa):
        lists.append(('ScriptingAssemblies.json', json.load(open(sa, encoding='utf-8'))['names']))
    if not lists:
        bad.append(('Managed', 'no assembly list (MonoManager.m_AssemblyNames, ScriptingAssemblies.json)'))
    for what, names in lists:
        mdir = os.path.join(args.out, args.name, 'assets', 'bin', 'Data', 'Managed')
        have = lambda a: os.path.exists(os.path.join(mdir, a))
        game, engine = split_assemblies(names, args.unity)
        miss = [a for a in game if not have(a)]
        if miss:
            bad.append(('Managed', f'missing {miss}'))
        skip = [a for a in engine if not have(a)]
        log(f'verify: {what}: {len(engine) - len(skip)} engine modules (skipped, not in Managed: {skip}), '
            f'{len(game)} game assemblies' + (f', MISSING {miss}' if miss else ', all present'))
    log('verify: OK' if not bad else f'verify: {len(bad)} PROBLEMS: {bad[:20]}')
    return {'problems': bad}


if __name__ == '__main__':
    main()
