"""pmunity.nsis: the four installer layouts Unity's Android support installers use, on hand-made
installers (offline): non-solid NSIS (up to 2020.2), the large-installer variant (2020.3 - 2023.2),
and Unity 6's chunked LZMA, per-file (6000.1, 6000.2) and solid (6000.0, 6000.3+)."""
import lzma
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import nsis as N

SIG = b'\xef\xbe\xad\xdeNullsoftInst'
DICT = 1 << 20


def lzma_block(data):
    """An NSIS LZMA stream: properties byte, dictionary size, raw LZMA1 data."""
    raw = lzma.compress(data, format=lzma.FORMAT_RAW,
                        filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': DICT, 'lc': 3, 'lp': 0, 'pb': 2, 'preset': 0}])
    return b'\x5d' + struct.pack('<I', DICT) + raw


def chunks(data, size=N.CHUNK):
    """Unity 6's chunked LZMA: per chunk a 3-byte compressed length and an LZMA stream."""
    out = b''
    for i in range(0, len(data), size):
        b = lzma_block(data[i:i + size])
        out += len(b).to_bytes(3, 'little') + b
    return out


def ustr(s):
    return s.encode('utf-16-le') + b'\0\0'


def header(files, wide):
    """An NSIS script header (Unicode, NSIS 3 string codes) that creates $INSTDIR\\sub and extracts
    each (name, data offset) there; returns the header bytes."""
    strings = b'\0\0'                                   # string 0: empty
    def add(t):
        nonlocal strings
        off = len(strings) // 2
        strings += t
        return off
    var_instdir = struct.pack('<HH', 3, 0x8080 | 21)     # NSIS 3: code 3 (VAR), $INSTDIR
    sub = add(var_instdir + '\\sub'.encode('utf-16-le') + b'\0\0')
    entsize = 36 if wide else 28
    np = entsize // 4 - 1
    ents = b''
    def ent(op, *pa):
        return struct.pack('<%dI' % (np + 1), op, *(list(pa) + [0] * (np - len(pa))))
    ents += ent(N.EW_CREATEDIR, sub, 1)
    for name, off in files:
        s = add(ustr(name))
        if wide:
            ents += ent(N.EW_EXTRACTFILE, 0, s, off & 0xffffffff, off >> 32)
        else:
            ents += ent(N.EW_EXTRACTFILE, 0, s, off)
    n_ent = 1 + len(files)
    eo = 4 + 8 * 8
    so = eo + len(ents)
    lo = so + len(strings)
    blocks = [(0, 0), (0, 0), (eo, n_ent), (so, 0), (lo, 0), (lo, 0), (0, 0), (0, 0)]
    h = struct.pack('<I', 0) + b''.join(struct.pack('<II', *b) for b in blocks) + ents + strings + b'\0' * 8
    return h


def first_header(flags, hdr_len, total):
    return struct.pack('<I', flags) + SIG + struct.pack('<II', hdr_len, total)


def installer(layout, files):
    """files: [(name, bytes)] -> installer bytes in the given layout."""
    pre = b'MZ' + b'\0' * 510                           # the first header sits on a 512-byte boundary
    if layout in ('classic', 'large'):
        data, offs = b'', []
        for name, d in files:
            offs.append((name, len(data)))
            b = lzma_block(d)
            data += struct.pack('<I', 0x80000000 | len(b)) + b
        h = header(offs, wide=layout == 'large')
        hb = lzma_block(h)
        if layout == 'classic':
            body = struct.pack('<I', 0x80000000 | len(hb)) + hb + data
            fh = first_header(0, len(h), 28 + len(body))
        else:
            body = bytes(8) + struct.pack('<I', 0x80000000 | len(hb)) + hb + data   # 36-byte first header
            fh = first_header(0x30, len(h), 28 + len(body))
        return pre + fh + body
    if layout == 'chunked':
        data, offs = b'', []
        for name, d in files:
            offs.append((name, len(data)))
            b = chunks(d) + bytes(3)                    # the chunks end with a zero length
            data += struct.pack('<Q', (1 << 63) | len(b)) + b
        h = header(offs, wide=True)
        hb = chunks(h) + bytes(3)
        body = struct.pack('<Q', 0) + struct.pack('<Q', (1 << 63) | len(hb)) + hb + data
        return pre + first_header(0x70, len(h), 28 + len(body)) + body
    if layout == 'solid':
        stream, offs = b'', []
        for name, d in files:
            offs.append((name, len(stream)))
            stream += struct.pack('<Q', len(d)) + d
        h = header(offs, wide=True)
        hb = lzma_block(struct.pack('<Q', len(h)) + h)
        body = struct.pack('<Q', len(stream)) + len(hb).to_bytes(3, 'little') + hb + chunks(stream)
        return pre + first_header(0x70, len(h), 28 + len(body)) + body
    raise ValueError(layout)


def sample_files():
    # a file crossing 2 MiB chunk boundaries (solid layout), small ones before and after it
    big = bytes((i * 7 + (i >> 11)) & 0xff for i in range(5 << 19))      # 2.5 MiB
    return [('a.txt', b'hello'), ('libunity.so', big), ('z.bin', bytes(range(256)) * 9)]


class NsisLayoutTest(unittest.TestCase):
    def check(self, layout):
        files = sample_files()
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'setup.exe')
            with open(p, 'wb') as f:
                f.write(installer(layout, files))
            ns = N.Nsis(p)
            try:
                got = {path: off for path, off, _ in ns.files()}
                self.assertEqual(sorted(got), sorted('$INSTDIR/sub/' + n for n, _ in files))
                for name, data in reversed(files):      # out of order: chunks are found as needed
                    self.assertEqual(ns.extract(got['$INSTDIR/sub/' + name]), data, f'{layout} {name}')
            finally:
                ns.close()
            return ns

    def test_classic(self):
        ns = self.check('classic')
        self.assertFalse(ns.chunked)

    def test_large(self):
        self.check('large')

    def test_unity6_chunked(self):
        ns = self.check('chunked')
        self.assertTrue(ns.chunked)
        self.assertFalse(ns.solid)

    def test_unity6_solid(self):
        ns = self.check('solid')
        self.assertTrue(ns.solid)

    def test_dechunk(self):
        data = os.urandom(1000) * 3
        self.assertEqual(N.dechunk(chunks(data, 700) + bytes(3)), data)


if __name__ == '__main__':
    unittest.main()
