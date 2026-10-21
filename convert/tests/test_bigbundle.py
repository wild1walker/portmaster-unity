"""bigbundle.py: bundles unpacked to node files and written back one chunk at a time, the same
bytes UnityPy's save(packer='lz4') writes."""
import os
import random
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from pmunity import bigbundle  # noqa: E402


def _read(p):
    with open(p, 'rb') as f:
        return f.read()


def _nodes(d):
    rnd = random.Random(7)
    text = (b'm_Name texture atlas sprite ' * 20000)[:300_001]          # compressible, odd size
    noise = bytes(rnd.getrandbits(8) for _ in range(0x20000 * 3 + 777))   # raw chunks, a short tail
    out = []
    for name, flags, data in (('CAB-0123abcd', 4, text), ('CAB-0123abcd.resS', 0, noise)):
        p = os.path.join(d, name)
        with open(p, 'wb') as f:
            f.write(data)
        out.append(bigbundle.Node(name, flags, p))
    return out


class BigBundleTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.d)

    def _roundtrip(self, version, engine, aligned):
        src = os.path.join(self.d, 'src')
        os.makedirs(src)
        nodes = _nodes(src)
        b = bigbundle.Bundle(None, version, '5.x.x', engine, aligned, nodes, 0)
        path = os.path.join(self.d, 'bundle')
        bigbundle.pack(b, nodes, path)
        self.assertFalse(os.path.exists(path + '.data'))

        # UnityPy reads it, node for node
        import UnityPy
        env = UnityPy.load(path)
        f = env.file
        self.assertEqual(list(f.files), [n.name for n in nodes])
        for n in nodes:
            got = f.files[n.name]
            data = got.bytes if hasattr(got, 'bytes') else bytes(got.view)
            self.assertEqual(data, _read(n.path), n.name)
        # ... and writes the same bytes
        self.assertEqual(f.save(packer='lz4'), _read(path))

        # unpacked again: the same nodes, and packed again: the same file
        u = bigbundle.unpack(path, os.path.join(self.d, 'work'))
        self.assertEqual([(n.name, n.flags) for n in u.nodes], [(n.name, n.flags) for n in nodes])
        for a, c in zip(u.nodes, nodes):
            self.assertEqual(_read(a.path), _read(c.path))
        self.assertEqual(u.size, sum(os.path.getsize(n.path) for n in nodes))
        self.assertEqual(bigbundle.size(path), u.size)
        again = os.path.join(self.d, 'again')
        bigbundle.pack(u, u.nodes, again)
        self.assertEqual(_read(again), _read(path))

    def test_v6_unaligned(self):
        self._roundtrip(6, '2017.4.9f1', False)

    def test_v7_aligned(self):
        self._roundtrip(7, '2020.3.48f1', True)

    def test_lzma_one_block_in_pieces(self):
        # Unity's default bundle compression: the block info and all data in one LZMA block
        src = os.path.join(self.d, 'src')
        os.makedirs(src)
        nodes = _nodes(src)
        lz4 = os.path.join(self.d, 'lz4')
        bigbundle.pack(bigbundle.Bundle(None, 6, '5.x.x', '2017.4.9f1', False, nodes, 0), nodes, lz4)
        import UnityPy
        lzma_path = os.path.join(self.d, 'lzma')
        with open(lzma_path, 'wb') as f:
            f.write(UnityPy.load(lz4).file.save(packer='lzma'))
        h = bigbundle.header(lzma_path)
        self.assertEqual(len(h[5]), 1)                        # one block
        old = bigbundle.MAX_BLOCK
        bigbundle.MAX_BLOCK = 1 << 16                         # so it is read in pieces
        try:
            u = bigbundle.unpack(lzma_path, os.path.join(self.d, 'work'))
        finally:
            bigbundle.MAX_BLOCK = old
        for a, c in zip(u.nodes, nodes):
            self.assertEqual(_read(a.path), _read(c.path))
        # written back as chunked LZ4: the bundle UnityPy's lz4 packer makes of the same nodes
        out = os.path.join(self.d, 'out')
        bigbundle.pack(u, u.nodes, out)
        self.assertEqual(_read(out), _read(lz4))

    def test_not_a_bundle(self):
        p = os.path.join(self.d, 'x.assets')
        with open(p, 'wb') as f:
            f.write(b'\0' * 64)
        self.assertEqual(bigbundle.size(p), 0)
        self.assertIsNone(bigbundle.unpack(p, os.path.join(self.d, 'w')))


if __name__ == '__main__':
    unittest.main()
