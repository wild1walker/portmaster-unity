"""Big UnityFS bundles in little memory.

UnityPy reads a bundle by joining every decompressed block into one buffer, and saves one by
building the whole file in memory: a bundle with 500 MB of textures needs about twice that. On a
1 GB handheld the kernel then kills the setup (Cuphead's largest atlases: 994 MB peak for one
bundle). Here a bundle is unpacked one block at a time into its nodes, each a file in a work
folder; the textures step loads the serialized file from its file and reads a texture's pixels
from the .resS file when it converts that texture. The new bundle is written from node files the
same way, one chunk at a time. Memory: about one chunk, whatever the bundle's size.

The bundle written is the one UnityPy's save(packer='lz4') writes (the same layout, flags, 128 KiB
LZ4 chunks and raw-chunk rule), so the player reads both the same.

    b = unpack(path, work_dir)          # None: a layout this module leaves to UnityPy
    b.nodes                             # [Node(name, flags, path)], in the bundle's order
    pack(b, nodes, out_path)            # nodes: the same, with new files for the changed ones
"""
import os
import re
import struct
from collections import namedtuple

Node = namedtuple('Node', 'name flags path')
CHUNK = 0x20000                  # UnityPy's LZ4 chunk (COMPRESSION_CHUNK_SIZE_MAP)
MAX_BLOCK = 64 << 20             # a bigger block (an LZMA bundle is one block) stays with UnityPy
DATA_FLAG, BLOCK_FLAG = 194, 2   # UnityPy's packer='lz4': blocks info at the end, directory, LZ4


class Bundle:
    def __init__(self, path, version, player, engine, aligned, nodes, size):
        self.path, self.version, self.player, self.engine = path, version, player, engine
        self.aligned, self.nodes, self.size = aligned, nodes, size       # size: the nodes' bytes


def _cstr(f):
    b = bytearray()
    while True:
        c = f.read(1)
        if not c or c == b'\0':
            return bytes(b)
        b += c


def _engine(s):
    m = re.match(r'(\d+)\.(\d+)\.(\d+)', s)
    return tuple(int(x) for x in m.groups()) if m else (0, 0, 0)


def _decompress(data, usize, flag):
    from UnityPy.helpers import CompressionHelper as C
    kind = flag & 0x3F
    if kind == 0:
        return data
    if kind in (2, 3):
        return C.decompress_lz4(data, usize)
    if kind == 1:
        return C.decompress_lzma(data, usize)
    raise ValueError(f'compression {kind}')


def header(path):
    """(version, player, engine, flags, aligned, blocks, nodes, data start) of a UnityFS file, or None
    for any other file and for layouts left to UnityPy (encrypted, unknown flags)."""
    with open(path, 'rb') as f:
        if _cstr(f) != b'UnityFS':
            return None
        version = struct.unpack('>I', f.read(4))[0]
        player, engine = _cstr(f).decode(), _cstr(f).decode()
        _, csize, usize, flags = struct.unpack('>qIII', f.read(20))
        ev = _engine(engine)
        # UnityPy's rules: flag 0x200 is Unity CN's encryption before the alignment fix, then
        # BlockInfoNeedPaddingAtStart (and 0x400 encryption)
        old = ev < (2020,) or (ev[0] == 2020 and ev < (2020, 3, 34)) or \
            (ev[0] == 2021 and ev < (2021, 3, 2)) or (ev[0] == 2022 and ev < (2022, 1, 1))
        if flags & ~0x3FF or (old and flags & 0x200) or (not old and flags & 0x100):
            return None
        pad = not old and bool(flags & 0x200)
        aligned = version >= 7 or (ev[0] == 2019 and ev >= (2019, 4, 15))
        if aligned:
            f.seek(-f.tell() % 16, 1)
        start = f.tell()
        if flags & 0x80:
            f.seek(-csize, 2)
        info = _decompress(f.read(csize), usize, flags)
        if not flags & 0x80:
            start = f.tell()
        if pad:
            start += -start % 16
        n = struct.unpack_from('>i', info, 16)[0]
        p = 20
        blocks = []
        for _ in range(n):
            blocks.append(struct.unpack_from('>IIH', info, p))
            p += 10
        n = struct.unpack_from('>i', info, p)[0]
        p += 4
        nodes = []
        for _ in range(n):
            off, size, nflags = struct.unpack_from('>qqI', info, p)
            e = info.index(b'\0', p + 20)
            nodes.append((off, size, nflags, info[p + 20:e].decode()))
            p = e + 1
        return version, player, engine, flags, aligned, blocks, nodes, start


def size(path):
    """The bytes of a UnityFS bundle's nodes (what UnityPy holds in memory), or 0."""
    try:
        h = header(path)
    except (OSError, ValueError, struct.error):
        return 0
    return sum(n[1] for n in h[6]) if h else 0


def unpack(path, work_dir):
    """The bundle's nodes as files in work_dir (one block in memory at a time), or None when this
    module leaves the bundle to UnityPy (a layout it does not read, or a block over MAX_BLOCK)."""
    h = header(path)
    if h is None:
        return None
    version, player, engine, flags, aligned, blocks, nodes, start = h
    if any(b[0] > MAX_BLOCK for b in blocks):
        return None
    os.makedirs(work_dir, exist_ok=True)
    order = sorted(range(len(nodes)), key=lambda i: nodes[i][0])
    out = []
    for i, (off, sz, nflags, name) in enumerate(nodes):
        safe = re.fullmatch(r'[A-Za-z0-9._-]+', name) and name not in ('.', '..')
        out.append(Node(name, nflags, os.path.join(work_dir, name if safe else f'node{i}')))
    files = {i: open(out[i].path, 'wb') for i in range(len(nodes))}
    try:
        with open(path, 'rb') as f:
            f.seek(start)
            pos = 0                                      # in the uncompressed data
            for usize, csize, bflags in blocks:
                data = memoryview(_decompress(f.read(csize), usize, bflags))
                if len(data) != usize:
                    raise ValueError(f'{path}: a block decompressed to {len(data)} bytes, not {usize}')
                end = pos + usize
                for i in order:
                    off, sz = nodes[i][0], nodes[i][1]
                    lo, hi = max(off, pos), min(off + sz, end)
                    if lo < hi:
                        files[i].write(data[lo - pos:hi - pos])
                pos = end
    finally:
        for fh in files.values():
            fh.close()
    for i, (off, sz, _, name) in enumerate(nodes):
        if os.path.getsize(out[i].path) != sz:
            raise ValueError(f'{path}: node {name}: {os.path.getsize(out[i].path)} bytes unpacked, not {sz}')
    return Bundle(path, version, player, engine, aligned, out, sum(n[1] for n in nodes))


def _chunks(paths):
    """The files' bytes, concatenated, in CHUNK-sized pieces (the last one shorter)."""
    buf = bytearray()
    for p in paths:
        with open(p, 'rb') as f:
            while True:
                b = f.read(CHUNK - len(buf))
                if not b:
                    break
                buf += b
                if len(buf) == CHUNK:
                    yield bytes(buf)
                    buf.clear()
    if buf:
        yield bytes(buf)


def pack(bundle, nodes, out_path):
    """Writes the bundle with these node files to out_path, as UnityPy's save(packer='lz4') does."""
    from UnityPy.helpers import CompressionHelper as C
    lz4 = C.COMPRESSION_MAP[BLOCK_FLAG & 0x3F]
    data_path = out_path + '.data'
    blocks = []
    with open(data_path, 'wb') as d:
        for chunk in _chunks([n.path for n in nodes]):
            c = lz4(chunk)
            # UnityPy keeps a chunk raw when LZ4 does not make it smaller (a full chunk: >=, the
            # last one: >)
            raw = len(c) >= CHUNK if len(chunk) == CHUNK else len(c) > len(chunk)
            if raw:
                d.write(chunk)
                blocks.append((len(chunk), len(chunk), BLOCK_FLAG ^ (BLOCK_FLAG & 0x3F)))
            else:
                d.write(c)
                blocks.append((len(chunk), len(c), BLOCK_FLAG))
    info = bytearray(16) + struct.pack('>i', len(blocks))
    for u, c, fl in blocks:
        info += struct.pack('>IIH', u, c, fl)
    info += struct.pack('>i', len(nodes))
    off = 0
    for n in nodes:
        sz = os.path.getsize(n.path)
        info += struct.pack('>qqI', off, sz, n.flags) + n.name.encode() + b'\0'
        off += sz
    cinfo = C.COMPRESSION_MAP[DATA_FLAG & 0x3F](bytes(info))
    with open(out_path, 'wb') as o:
        o.write(b'UnityFS\0' + struct.pack('>I', bundle.version) + bundle.player.encode() + b'\0' +
                bundle.engine.encode() + b'\0')
        size_pos = o.tell()
        o.write(struct.pack('>qIII', 0, len(cinfo), len(info), DATA_FLAG))
        if bundle.aligned:
            o.write(b'\0' * (-o.tell() % 16))
        # DATA_FLAG has 0x80 (blocks info at the end) and not 0x200 (no padding)
        with open(data_path, 'rb') as d:
            while True:
                b = d.read(1 << 20)
                if not b:
                    break
                o.write(b)
        o.write(cinfo)
        total = o.tell()
        o.seek(size_pos)
        o.write(struct.pack('>q', total))
    os.remove(data_path)
