"""Compressed builds: a game built with LZ4 compression keeps its serialized files (globalgamemanagers,
level*, sharedassets*, resources.assets and their .resS / .resource) in one UnityFS archive,
<Game>_Data/data.unity3d. The converter works on plain files, and an Android player reads them
from assets/bin/Data the same way it reads an uncompressed build, so the archive is unpacked
first:

    python -m pmunity unpack <Game>_Data [--out DIR] [--remove]

Each file of the archive is written as <DIR>/<its name> (default DIR: the data folder itself,
next to the archive), one block at a time (memory: one block, 128 KiB in Unity's archives).
--remove deletes data.unity3d afterwards (the player would otherwise read the archive's files
first). Blocks may be stored, LZ4 or LZMA compressed.
"""
import argparse
import lzma
import os
import struct
import sys

ARCHIVE = 'data.unity3d'


def _cstr(b, p):
    e = b.index(b'\0', p)
    return b[p:e], e + 1


def _lzma(data, usize):
    """Unity's LZMA: 5 property bytes (lc/lp/pb, dictionary size), then the raw stream."""
    props, dict_size = data[0], struct.unpack_from('<I', data, 1)[0]
    lc, lp, pb = props % 9, (props // 9) % 5, props // 45
    dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': dict_size,
                                                           'lc': lc, 'lp': lp, 'pb': pb}])
    return dec.decompress(data[5:], usize)


def _decompress(data, usize, comp):
    if comp == 0:
        return data
    if comp in (2, 3):
        import lz4.block
        return lz4.block.decompress(data, uncompressed_size=usize)
    if comp == 1:
        return _lzma(data, usize)
    raise ValueError(f'unknown block compression {comp}')


class Archive:
    """A UnityFS archive's block and file tables (formats 6 - 8)."""

    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            head = f.read(4096)
            sig, p = _cstr(head, 0)
            if sig != b'UnityFS':
                raise ValueError(f'{path}: not a UnityFS archive')
            self.format, = struct.unpack_from('>I', head, p); p += 4
            _, p = _cstr(head, p)                           # "5.x.x"
            self.unity, p = _cstr(head, p)                  # the engine version
            size, ci_size, ui_size, flags = struct.unpack_from('>qIII', head, p); p += 20
            if self.format >= 7:
                p = (p + 15) & ~15
            if flags & 0x80:                                # block table at the end of the file
                f.seek(size - ci_size)
                ci = f.read(ci_size)
                data_start = p
            else:
                f.seek(p)
                ci = f.read(ci_size)
                data_start = p + ci_size
            if flags & 0x200:                               # (2020+) padding before the first block
                data_start = (data_start + 15) & ~15
        bi = _decompress(ci, ui_size, flags & 0x3f)
        n, = struct.unpack_from('>i', bi, 16)
        self.blocks = []                                    # (file offset, compressed, uncompressed, flags)
        q, fpos = 20, data_start
        for _ in range(n):
            usz, csz, bfl = struct.unpack_from('>IIH', bi, q); q += 10
            self.blocks.append((fpos, csz, usz, bfl))
            fpos += csz
        nn, = struct.unpack_from('>i', bi, q); q += 4
        self.files = []                                     # (offset, size, flags, name)
        for _ in range(nn):
            off, sz, fl = struct.unpack_from('>qqI', bi, q); q += 20
            name, q = _cstr(bi, q)
            self.files.append((off, sz, fl, name.decode('utf-8')))

    def extract(self, out_dir, log=None):
        """Writes every file of the archive under out_dir; returns their names."""
        root = os.path.realpath(out_dir)
        names = []
        outs = []
        for off, sz, fl, name in self.files:
            dst = os.path.realpath(os.path.join(out_dir, name))
            if not dst.startswith(root + os.sep):
                raise ValueError(f'{self.path}: file name {name!r} leaves the folder')
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            outs.append((off, off + sz, dst, open(dst + '.tmp', 'wb')))
            names.append(name)
        try:
            upos = 0
            with open(self.path, 'rb') as f:
                for fpos, csz, usz, bfl in self.blocks:
                    f.seek(fpos)
                    plain = _decompress(f.read(csz), usz, bfl & 0x3f)
                    for a, b, dst, fo in outs:
                        lo, hi = max(a, upos), min(b, upos + usz)
                        if lo < hi:
                            fo.write(plain[lo - upos:hi - upos])
                    upos += usz
        finally:
            for _, _, _, fo in outs:
                fo.close()
        for a, b, dst, _ in outs:
            if os.path.getsize(dst + '.tmp') != b - a:
                raise ValueError(f'{self.path}: {dst} came out short')
            os.replace(dst + '.tmp', dst)
            if log:
                log(f'unpack: {os.path.relpath(dst, root)} ({b - a} bytes)')
        return names


def unpack(data_dir, out_dir=None, remove=False, log=print):
    src = os.path.join(data_dir, ARCHIVE)
    if not os.path.isfile(src):
        raise SystemExit(f'{data_dir}: no {ARCHIVE} (not a compressed build)')
    names = Archive(src).extract(out_dir or data_dir, log)
    if remove:
        os.remove(src)
    log(f'unpack: {len(names)} files from {src}' + (' (archive removed)' if remove else ''))
    return names


def main(argv=None):
    ap = argparse.ArgumentParser(prog='pmunity unpack', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('data', help='the data folder holding data.unity3d')
    ap.add_argument('--out', help='where the files go (default: the data folder)')
    ap.add_argument('--remove', action='store_true', help='delete data.unity3d once unpacked')
    a = ap.parse_args(argv)
    unpack(a.data, a.out, a.remove)


if __name__ == '__main__':
    main(sys.argv[1:])
