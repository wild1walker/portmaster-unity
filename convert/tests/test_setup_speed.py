"""The setup's speed-ups give the bytes they replace (docs/setup-speed.md): bundles read in place
(bigbundle's lazy nodes), chunks compressed on several threads, UnityPy's chunked compression and
type-tree array writes, serialized files written in one pass, installer payloads read ahead.

    python -m unittest tests.test_setup_speed            (from convert/)
    PMUNITY_GAMES=<Game>_Data[:...] python -m unittest tests.test_setup_speed   (also: real files)
"""
import os
import random
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from pmunity import bigbundle, unity_io  # noqa: E402

GAMES = [p for p in os.environ.get('PMUNITY_GAMES', '').split(os.pathsep) if p]


def _read(p):
    with open(p, 'rb') as f:
        return f.read()


def _bundle(d):
    """A chunked LZ4 bundle of a serialized-file node and a stream node (with raw chunks)."""
    rnd = random.Random(11)
    text = (b'm_Name sprite atlas texture ' * 30000)[:400_003]
    noise = bytes(rnd.getrandbits(8) for _ in range(0x20000 * 4 + 1234))
    nodes = []
    for name, flags, data in (('CAB-00ff', 4, text), ('CAB-00ff.resS', 0, noise)):
        p = os.path.join(d, name)
        with open(p, 'wb') as f:
            f.write(data)
        nodes.append(bigbundle.Node(name, flags, p))
    path = os.path.join(d, 'bundle')
    bigbundle.pack(bigbundle.Bundle(None, 6, '5.x.x', '2017.4.9f1', False, nodes, 0), nodes, path)
    return path, text, noise


class LazyBundle(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.d)

    def test_streams_stay_in_the_bundle(self):
        path, text, noise = _bundle(self.d)
        work = os.path.join(self.d, 'work')
        b = bigbundle.unpack(path, work, lazy=True)
        try:
            cab, res = b.nodes
            self.assertEqual(_read(cab.path), text)                   # the serialized file: a file
            self.assertIsInstance(res.path, bigbundle.Part)           # the stream: read in place
            self.assertEqual(sorted(os.listdir(work)), ['CAB-00ff'])
            self.assertEqual(len(res.path), len(noise))
            rnd = random.Random(3)
            for _ in range(200):                                      # slices across block edges
                a = rnd.randrange(len(noise))
                n = rnd.choice((1, 17, 0x20000, 0x20001, 300_000, len(noise)))
                self.assertEqual(res.path[a:a + n], noise[a:a + n])
            r = res.path.open()
            r.seek(0x1FFF0)
            self.assertEqual(r.read(40), noise[0x1FFF0:0x20018])
            self.assertEqual(r.tell(), 0x20018)
            r.seek(-10, 2)
            self.assertEqual(r.read(), noise[-10:])
            self.assertEqual(r.read(5), b'')
            self.assertEqual(bigbundle.node_size(res), len(noise))
            # packed again from the file and the Part: the same bundle
            again = os.path.join(self.d, 'again')
            bigbundle.pack(b, b.nodes, again)
            self.assertEqual(_read(again), _read(path))
        finally:
            b.close()

    def test_unitypy_reads_a_part(self):
        from UnityPy.streams import EndianBinaryReader
        path, text, noise = _bundle(self.d)
        b = bigbundle.unpack(path, os.path.join(self.d, 'work'), lazy=True)
        try:
            r = EndianBinaryReader(b.nodes[1].path.open())
            r.Position = 0x3FFFE
            self.assertEqual(r.read_bytes(4), noise[0x3FFFE:0x40002])
            self.assertEqual(r.Length, len(noise))
        finally:
            b.close()

    def test_big_blocks_are_written_out(self):
        # a bundle of one LZMA block is not read in place: every node becomes a file, as before
        path, text, noise = _bundle(self.d)
        import UnityPy
        lzma = os.path.join(self.d, 'lzma')
        with open(lzma, 'wb') as f:
            f.write(UnityPy.load(path).file.save(packer='lzma'))
        old = bigbundle.LAZY_MAX_BLOCK
        bigbundle.LAZY_MAX_BLOCK = 1 << 16
        try:
            b = bigbundle.unpack(lzma, os.path.join(self.d, 'w2'), lazy=True)
        finally:
            bigbundle.LAZY_MAX_BLOCK = old
        self.assertIsNone(b.source)
        self.assertEqual([_read(n.path) for n in b.nodes], [text, noise])

    def test_threads_give_the_same_bundle(self):
        path, text, noise = _bundle(self.d)
        b = bigbundle.unpack(path, os.path.join(self.d, 'work'))
        lz4 = __import__('UnityPy.helpers.CompressionHelper', fromlist=['x']).COMPRESSION_MAP[2]
        chunks = list(bigbundle._chunks(b.nodes))
        one = [c for _, c in bigbundle.compress_chunks(iter(chunks), lz4, 1)]
        four = [c for _, c in bigbundle.compress_chunks(iter(chunks), lz4, 4)]
        self.assertEqual(one, four)
        self.assertEqual(one, [lz4(c) for c in chunks])


class UnityPyPatches(unittest.TestCase):
    def test_chunk_based_compress(self):
        unity_io._patch_unitypy()
        from UnityPy.helpers import CompressionHelper as C
        self.assertIs(C.chunk_based_compress, unity_io._chunk_based_compress)
        orig = unity_io._orig_chunk_based_compress
        rnd = random.Random(5)
        noise = bytes(rnd.getrandbits(8) for _ in range(0x20000 * 3))
        text = b'abc' * 100_000
        for data in (b'', text, noise, noise[:0x20000 * 2], text + noise[:777], bytearray(text)):
            for flag in (2, 3, 0x42, 0):
                self.assertEqual(C.chunk_based_compress(data, flag), orig(data, flag), (len(data), flag))

    def test_array_writes(self):
        unity_io._patch_unitypy()
        from UnityPy.helpers import TypeTreeHelper as TT
        from UnityPy.streams import EndianBinaryWriter

        class Node:                                  # the fields write_value reads
            def __init__(self, t, flag=0, children=()):
                self.m_Type, self.m_MetaFlag, self.m_Children = t, flag, list(children)
                self.m_Name = self._clean_name = 'x'

        def one_by_one(value, sub, endian):
            w = EndianBinaryWriter(endian=endian)
            for v in value:
                TT.write_value(v, sub, w, TT.TypeTreeConfig(True, None, False))
            return w.bytes

        cases = [('UInt8', bytes(range(256)) * 3), ('UInt8', list(range(200))), ('SInt8', [-5, 0, 127]),
                 ('float', [0.1, -2.5, float('inf'), 1e-40]), ('double', [1 / 3, -0.0]), ('int', [1, -2, 1 << 30]),
                 ('UInt16', [0, 65535]), ('SInt64', [-(1 << 62)]), ('bool', [True, False, 1, 0]),
                 ('unsigned int', []), ('char', b'xyz')]
        for t, value in cases:
            for endian in ('<', '>'):
                sub = Node(t)
                w = EndianBinaryWriter(endian=endian)
                unity_io._write_items(value, sub, w, TT.TypeTreeConfig(True, None, False), TT.write_value)
                self.assertEqual(w.bytes, one_by_one(value, sub, endian), (t, endian))
        # elements aligned one by one (or strings) are written one by one, as UnityPy does
        for sub, value in ((Node('UInt8', 0x4000), b'ab'), (Node('string'), ['a', 'bcd'])):
            w = EndianBinaryWriter(endian='<')
            unity_io._write_items(value, sub, w, TT.TypeTreeConfig(True, None, False), TT.write_value)
            self.assertEqual(w.bytes, one_by_one(value, sub, '<'))


@unittest.skipUnless(GAMES, 'set PMUNITY_GAMES to data folders to check real serialized files')
class StreamedSave(unittest.TestCase):
    """save_serialized_to writes what sf.save() returns (real files: their objects, as read, and
    with a changed object)."""

    def test_same_bytes(self):
        import UnityPy
        d = tempfile.mkdtemp()
        try:
            for data in GAMES:
                names = sorted(n for n in os.listdir(data) if n.endswith('.assets') or n.startswith('level'))
                for n in names[:3] + names[-3:]:
                    p = os.path.join(data, n)
                    sf = UnityPy.load(p).file
                    want = sf.save()
                    out = os.path.join(d, n)
                    self.assertTrue(unity_io.save_serialized_to(sf, out))
                    self.assertEqual(_read(out), want, p)
                    # a texture given other bytes for this write only (as android-data inlines one)
                    sf = UnityPy.load(p).file
                    tex = next((o for o in sf.objects.values() if o.type.name == 'Texture2D'), None)
                    if tex is None:
                        continue
                    t = tex.read_typetree()
                    t['m_Name'] = t['m_Name'] + '-x' * 7

                    def prepare(o):
                        if o.path_id != tex.path_id:
                            return False
                        o.save_typetree(t)
                        return True
                    unity_io.save_serialized_to(sf, out, prepare)
                    sf2 = UnityPy.load(p).file
                    sf2.objects[tex.path_id].save_typetree(t)
                    self.assertEqual(_read(out), sf2.save(), p + ' (changed)')
        finally:
            shutil.rmtree(d)


if __name__ == '__main__':
    unittest.main()
