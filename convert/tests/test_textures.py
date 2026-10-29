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


class StripsTest(unittest.TestCase):
    """strip_blocks: a strip of rows at a time gives the blocks of the whole image at once."""

    def _same(self, fmt_name, fmt_id, w, h, nbytes, block=(4, 4)):
        import random
        from UnityPy.export import Texture2DConverter as C
        raw = bytes(random.Random(w * h).getrandbits(8) for _ in range(nbytes))
        t = {'m_TextureFormat': fmt_id, 'm_Width': w, 'm_Height': h}
        ch = T.Choice(block, w, h, True, 1)
        enc = T.Astc('fastest', 2)
        whole = T.texture_image(C.parse_image_data(raw, w, h, fmt_id, (2021, 3, 0, 0), 0), fmt_name)
        old_px = T.STRIP_PX
        T.STRIP_PX = w * 8                           # strips of a few rows: many of them
        try:
            strips = T.strip_blocks(object(), t, raw, ch, enc, fmt_name)
        finally:
            T.STRIP_PX = old_px
        self.assertEqual(strips, enc.encode(whole, block))

    def test_rgba32(self):
        self._same('RGBA32', 4, 37, 29, 37 * 29 * 4)

    def test_dxt1_height_not_a_multiple_of_4(self):
        self._same('DXT1', 10, 64, 50, (64 // 4) * (52 // 4) * 8)

    def test_bc7_6x6_blocks(self):
        self._same('BC7', 25, 48, 40, (48 // 4) * (40 // 4) * 16, block=(6, 6))

    def test_unexpected_size_left_to_the_whole_image(self):
        t = {'m_TextureFormat': 10, 'm_Width': 64, 'm_Height': 64}
        self.assertIsNone(T.strip_blocks(object(), t, b'\0' * 1001, T.Choice((4, 4), 64, 64, False, 1),
                                         T.Astc('fastest', 1), 'DXT1'))


class DecodeDownscaleTest(unittest.TestCase):
    """decode_downscale: a band of rows at a time, on several threads, gives the image one decode and
    one resize give (tall textures: several bands of about 512 source rows)."""

    def _same(self, fmt_name, fmt_id, w, h, nbytes, scale):
        import random
        from concurrent.futures import ThreadPoolExecutor
        from UnityPy.export import Texture2DConverter as C
        raw = random.Random(w * h).randbytes(nbytes)

        class Tex:
            m_TextureFormat, m_Width, m_Height, m_PlatformBlob = fmt_id, w, h, None

            def get_image_data(self):
                return raw
        nw, nh = round(w * scale), round(h * scale)
        whole = T.downscale(T.texture_image(C.parse_image_data(raw, w, h, fmt_id, (0, 0, 0, 0), 0), fmt_name), nw, nh)
        with ThreadPoolExecutor(3) as pool:
            self.assertEqual(T.decode_downscale(object(), Tex(), nw, nh, fmt_name, pool).tobytes(), whole.tobytes())
        self.assertEqual(T.decode_downscale(object(), Tex(), nw, nh, fmt_name).tobytes(), whole.tobytes())

    def test_bc7_at_cupheads_scale(self):
        self._same('BC7', 25, 64, 4096, (64 // 4) * (4096 // 4) * 16, 0.375)

    def test_dxt5_halved(self):
        self._same('DXT5', 12, 64, 2048, (64 // 4) * (2048 // 4) * 16, 0.5)

    def test_dxt1(self):
        self._same('DXT1', 10, 64, 2048, (64 // 4) * (2048 // 4) * 8, 0.375)


if __name__ == '__main__':
    unittest.main()
