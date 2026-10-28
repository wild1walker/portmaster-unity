"""ASTC guide files (pmunity/astc_guide.py): written entries read back, used only for the same hard
blocks, split by tier in their order (no Unity or game files): python -m unittest tests.test_astc_guide
(from convert/)"""
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from pmunity import astc_guide as AG
from pmunity import textures as TX


def rest_of(*idx):
    return struct.pack(f'<{len(idx)}i', *idx)


class GuideFile(unittest.TestCase):
    def setUp(self):
        self.c = dict(TX.DEFAULTS, mode='guided')
        self.d = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.d.name, 'guide.bin')
        rec = AG.Recorder('medium')
        rest = rest_of(3, 7, 9, 12)
        rec.entries['a|1|0'] = (4, AG.rest_crc(rest), zlib.compress(bytes([0, 9, 2, 0])))
        rec.save(self.path, self.c)
        self.rest = rest

    def tearDown(self):
        self.d.cleanup()

    def test_read_back(self):
        r = AG.Reader(self.path, self.c)
        self.assertTrue(r.ok)
        self.assertEqual(r.codes('a|1|0', self.rest), bytes([0, 9, 2, 0]))
        r.close()

    def test_other_blocks_or_key(self):
        r = AG.Reader(self.path, self.c)
        self.assertIsNone(r.codes('a|1|0', rest_of(3, 7, 9, 13)))      # other hard blocks
        self.assertIsNone(r.codes('a|1|0', rest_of(3, 7, 9)))          # fewer
        self.assertIsNone(r.codes('a|2|0', self.rest))                 # not in it
        self.assertEqual((r.used, r.missed), (0, 3))
        r.close()

    def test_other_settings(self):
        r = AG.Reader(self.path, dict(self.c, scale=0.5))
        self.assertFalse(r.ok)
        self.assertIsNone(r.codes('a|1|0', self.rest))
        r.close()

    def test_split(self):
        parts = AG.split(self.rest, bytes([0, 9, 2, 0]))
        self.assertEqual(len(parts), len(AG.TIERS))
        self.assertEqual(parts[0], rest_of(3, 12))
        self.assertEqual(parts[2], rest_of(9))
        self.assertEqual(parts[9], rest_of(7))
        self.assertEqual(sum(len(p) for p in parts), len(self.rest))


if __name__ == '__main__':
    unittest.main()
