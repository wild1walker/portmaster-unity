"""Unit tests of the textures step's handling of Windows builds' formats (BC4, BC5, BC7) on
hand-made blocks (no Unity or game files): python -m unittest tests.test_textures   (from convert/)"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import textures as T


def bc4_block(r0, r1, indices=(0,) * 16):
    """A BC4 (one channel) block: two endpoints, sixteen 3-bit indices."""
    bits = 0
    for i, ix in enumerate(indices):
        bits |= ix << (3 * i)
    return bytes([r0, r1]) + bits.to_bytes(6, 'little')


def bc7_mode6_block(r, g, b, a, p):
    """A BC7 mode 6 block of one colour: both endpoints (7 bits per channel) the same, p-bits p,
    every index 0. The texels are (e << 1 | p) per channel."""
    bits, at = 1 << 6, 7                    # mode 6: bit 6
    for e in (r, r, g, g, b, b, a, a):
        bits |= e << at
        at += 7
    bits |= p << at | p << (at + 1)         # P0, P1; the 63 index bits stay 0
    return bits.to_bytes(16, 'little')


def decode(fmt, data, w=4, h=4):
    """UnityPy's decoding (Pillow's 'bcn' codec, as UnityPy uses it), then the step's texture_image."""
    from PIL import Image
    mode, n = {'BC4': ('L', 4), 'BC5': ('RGB', 5), 'BC7': ('RGBA', 7)}[fmt]
    return T.texture_image(Image.frombytes(mode, (w, h), data, 'bcn', n), fmt)


class WindowsFormatsTest(unittest.TestCase):
    def test_bc4_samples_as_red(self):
        img = decode('BC4', bc4_block(200, 10, [0] * 8 + [1] * 8))
        px = img.load()
        self.assertEqual(img.mode, 'RGBA')
        self.assertEqual(px[0, 0], (200, 0, 0, 255))        # (r, 0, 0, 1), as a GPU samples BC4
        self.assertEqual(px[0, 3], (10, 0, 0, 255))

    def test_bc5_samples_as_red_green(self):
        img = decode('BC5', bc4_block(200, 10) + bc4_block(60, 5))
        self.assertEqual(img.load()[1, 1], (200, 60, 0, 255))

    def test_bc7(self):
        img = decode('BC7', bc7_mode6_block(50, 100, 15, 127, 1))
        self.assertEqual(img.load()[2, 3], (101, 201, 31, 255))

    def test_dxt_unchanged(self):
        from PIL import Image
        img = Image.new('RGB', (4, 4), (1, 2, 3))
        self.assertEqual(T.texture_image(img, 'DXT1').load()[0, 0], (1, 2, 3, 255))

    def test_choose(self):
        c = dict(T.DEFAULTS)
        tree = {'m_Width': 64, 'm_Height': 64, 'm_Name': 'n', 'm_MipCount': 7}
        for fmt, alpha in (('BC4', False), ('BC5', False), ('BC7', True)):
            ch = T.choose(tree, fmt, c, None)
            self.assertEqual((ch.width, ch.height, ch.alpha, ch.mips), (64, 64, alpha, 7), fmt)
        self.assertIsNone(T.choose(tree, 'BC6H', c, None))   # HDR: ASTC LDR cannot hold it


if __name__ == '__main__':
    unittest.main()
