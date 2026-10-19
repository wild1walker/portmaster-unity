"""Unity's compiled shader programs: the per-platform program blob of a Shader asset.

A Shader (Unity 5.5 and later) keeps its compiled programs in `compressedBlob`: one LZ4 block per
shader compiler platform (`platforms`, with `offsets`, `compressedLengths`, `decompressedLengths`).
Decompressed, a platform's blob is

    int32 count
    count x (int32 offset, int32 length)       entries, offsets from the blob's start
    entries

and every entry is one serialized subprogram:

    int32 version                              the serializer's date, e.g. 201708220 (2017.3/2017.4)
    int32 gpu program type                     6 GLCore32, 4 GLES3, 23/24 Metal, ...
    int32 x N                                  statistics (ALU, TEX, flow, temp registers)
    string[] keywords                          int32 count, then each: int32 length, bytes, pad to 4
    string code                                int32 length, bytes, pad to 4 (GL: GLSL source)
    bytes                                      binding info: parameters, buffers, samplers (kept as is)

N is 4 for the serializer versions in LAYOUTS (checked on real data: every GL entry writes back
byte for byte). For another version the GL entries are tried with each N in turn; a layout is only
taken when the keywords decode, the code is GLSL and the entry writes back to the same bytes, and
the converter says so (Entry.guessed). The SerializedSubProgram records in m_ParsedForm point into
a platform's blob by m_BlobIndex. Only GL platforms are parsed here; Metal and others are kept as
they are.
"""
import struct

import lz4.block

# shader compiler platforms (ShaderCompilerPlatform)
PLATFORM_GLES20, PLATFORM_GLES3PLUS, PLATFORM_METAL, PLATFORM_GLCORE, PLATFORM_VULKAN = 5, 9, 14, 15, 18
# GPU program types (ShaderGpuProgramType, Unity 5.5 - 2018.x)
GPU_GLES31AEP, GPU_GLES31, GPU_GLES3, GPU_GLES = 2, 3, 4, 5
GPU_GLCORE32, GPU_GLCORE41, GPU_GLCORE43 = 6, 7, 8

# serializer version -> number of statistics ints (verified versions only)
LAYOUTS = {
    201708220: 4,       # Unity 2017.3 / 2017.4 (Cuphead 1.3.4: 5914 GLCore entries)
}
_STATS_CANDIDATES = (4, 3, 5, 2)      # for versions not in LAYOUTS, most likely first


def _pad4(n):
    return (n + 3) & ~3


class Entry:
    """One subprogram of a platform blob."""

    __slots__ = ('version', 'gpu_type', 'stats', 'keywords', 'code', 'rest', 'guessed')

    def __init__(self, version, gpu_type, stats, keywords, code, rest):
        self.version, self.gpu_type, self.stats = version, gpu_type, list(stats)
        self.keywords, self.code, self.rest = list(keywords), code, rest
        self.guessed = False

    @classmethod
    def parse(cls, data):
        version, gpu_type = struct.unpack_from('<ii', data)
        if version in LAYOUTS:
            e = cls._try(data, version, gpu_type, LAYOUTS[version])
            if e is None:
                raise ValueError(f'shader program entry (serializer {version}, gpu type {gpu_type}) does not match its layout')
            return e
        for n in _STATS_CANDIDATES:
            e = cls._try(data, version, gpu_type, n)
            if e is not None:
                e.guessed = True
                return e
        raise ValueError(f'shader program entry (serializer {version}, gpu type {gpu_type}): unknown layout')

    @classmethod
    def _try(cls, data, version, gpu_type, nstats):
        try:
            p = 8
            stats = struct.unpack_from('<%di' % nstats, data, p); p += 4 * nstats
            nkw, = struct.unpack_from('<i', data, p); p += 4
            if not 0 <= nkw < 4096:
                return None
            kws = []
            for _ in range(nkw):
                n, = struct.unpack_from('<i', data, p)
                if not 0 < n < 1024 or p + 4 + n > len(data):
                    return None
                kws.append(bytes(data[p + 4:p + 4 + n]).decode("ascii"))
                p = _pad4(p + 4 + n)
            n, = struct.unpack_from('<i', data, p)
            if not 0 <= n <= len(data) - p - 4:
                return None
            code = bytes(data[p + 4:p + 4 + n])
            if code and not code.lstrip()[:1] in (b'#', b'/'):
                return None
            code.decode('utf-8')
            e = cls(version, gpu_type, stats, kws, code, bytes(data[_pad4(p + 4 + n):]))
        except (struct.error, UnicodeDecodeError):
            return None
        return e if e.serialize() == bytes(data) else None

    def serialize(self):
        out = bytearray(struct.pack('<ii', self.version, self.gpu_type))
        out += struct.pack('<%di' % len(self.stats), *self.stats)
        out += struct.pack('<i', len(self.keywords))
        for k in self.keywords:
            b = k.encode('ascii')
            out += struct.pack('<i', len(b)) + b + b'\0' * (_pad4(len(b)) - len(b))
        out += struct.pack('<i', len(self.code)) + self.code + b'\0' * (_pad4(len(self.code)) - len(self.code))
        out += self.rest
        return bytes(out)

    def copy(self):
        e = Entry(self.version, self.gpu_type, self.stats, self.keywords, self.code, self.rest)
        e.guessed = self.guessed
        return e


def parse_blob(data):
    """A decompressed platform blob -> list of Entry (None for an empty slot)."""
    count, = struct.unpack_from('<i', data)
    out = []
    for i in range(count):
        off, length = struct.unpack_from('<ii', data, 4 + 8 * i)
        out.append(Entry.parse(bytes(data[off:off + length])) if length else None)
    return out


def build_blob(entries):
    """List of Entry -> decompressed platform blob (entries 4-aligned, in order)."""
    head = 4 + 8 * len(entries)
    body = bytearray()
    table = []
    for e in entries:
        b = e.serialize() if e is not None else b''
        table.append((head + len(body), len(b)))
        body += b
        body += b'\0' * (_pad4(len(body)) - len(body))
    out = bytearray(struct.pack('<i', len(entries)))
    for off, n in table:
        out += struct.pack('<ii', off, n)
    return bytes(out + body)


class ShaderBlobs:
    """The per-platform blobs of one Shader typetree (dict from UnityPy read_typetree); the tree's
    other fields are not touched until store()."""

    def __init__(self, tree):
        self.tree = tree
        raw = bytes(tree['compressedBlob'])
        self.platforms = {}
        for i, plat in enumerate(tree['platforms']):
            off, clen, dlen = tree['offsets'][i], tree['compressedLengths'][i], tree['decompressedLengths'][i]
            self.platforms[plat] = lz4.block.decompress(raw[off:off + clen], uncompressed_size=dlen)

    def entries(self, platform):
        return parse_blob(self.platforms[platform])

    def set_entries(self, platform, entries):
        self.platforms[platform] = build_blob(entries)

    def remove(self, platform):
        self.platforms.pop(platform, None)

    def store(self):
        """Write the blobs back into the tree, platforms in ascending order as Unity writes them."""
        blob = bytearray()
        plats, offs, clens, dlens = [], [], [], []
        for plat in sorted(self.platforms):
            data = self.platforms[plat]
            c = lz4.block.compress(data, mode='high_compression', store_size=False)
            plats.append(plat); offs.append(len(blob)); clens.append(len(c)); dlens.append(len(data))
            blob += c
        t = self.tree
        t['platforms'], t['offsets'], t['compressedLengths'], t['decompressedLengths'] = plats, offs, clens, dlens
        t['compressedBlob'] = bytes(blob)
