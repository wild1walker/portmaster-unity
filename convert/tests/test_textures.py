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


def _sprite(file_id, path_id):
    return {'m_Rect': {'x': 10.0, 'y': 20.0, 'width': 100.0, 'height': 60.0}, 'm_Offset': {'x': 4.0, 'y': 6.0},
            'm_Border': {'x': 2.0, 'y': 4.0, 'z': 6.0, 'w': 8.0}, 'm_PixelsToUnits': 100.0,
            'm_RD': {'texture': {'m_FileID': file_id, 'm_PathID': path_id},
                     'textureRect': {'x': 10.0, 'y': 20.0, 'width': 100.0, 'height': 60.0},
                     'textureRectOffset': {'x': 2.0, 'y': 2.0}, 'atlasRectOffset': {'x': -1.0, 'y': -1.0},
                     'uvTransform': {'x': 100.0, 'y': 60.0, 'z': 100.0, 'w': 50.0}}}


class _Obj:
    def __init__(self, tree):
        self.tree = tree

    def read_typetree(self):
        return self.tree


class CrossFileTest(unittest.TestCase):
    """Sprites and atlas entries whose texture is in another file follow it when it is scaled there."""

    SCALED = {'sharedassets1.assets': {'7': [2048, 1024, 1024, 512]},
              os.path.join('sub', 'sharedassets2.assets'): {'3': [512, 512, 256, 256]}}

    def test_externals_by_file_name(self):
        ext = ['globalgamemanagers.assets', 'Library/unity default resources', 'sharedassets1.assets',
               'sharedassets2.assets']
        self.assertEqual(T.external_sizes(ext, self.SCALED, 'sharedassets5.assets'), {3: {7: (2048, 1024, 1024, 512)}})
        # an external is looked for in the file's own folder; a file's reference to itself is its own textures'
        self.assertEqual(T.external_sizes(ext, self.SCALED, os.path.join('sub', 'sharedassets9.assets')),
                         {4: {3: (512, 512, 256, 256)}})
        self.assertEqual(T.external_sizes(['sharedassets1.assets'], self.SCALED, 'sharedassets1.assets'), {})

    def test_sprites_and_atlas_entries(self):
        ext = {3: {7: (2048, 1024, 1024, 512)}}
        there, own, other = _Obj(_sprite(3, 7)), _Obj(_sprite(0, 7)), _Obj(_sprite(3, 8))
        atlas = _Obj({'m_RenderDataMap': [('a', {'texture': {'m_FileID': 3, 'm_PathID': 7}, 'downscaleMultiplier': 0.5}),
                                          ('b', {'texture': {'m_FileID': 2, 'm_PathID': 7}, 'downscaleMultiplier': 1.0})]})
        stats = {'sprites': 0, 'atlas_entries': 0}
        save = T.fix_cross_references([('Sprite', there), ('Sprite', own), ('Sprite', other), ('SpriteAtlas', atlas),
                                       ('Texture2D', _Obj({}))], ext, stats)
        self.assertEqual([o for o, _ in save], [there, atlas])
        self.assertEqual(stats, {'sprites': 1, 'atlas_entries': 1})
        t = there.tree
        self.assertEqual(t['m_Rect'], {'x': 5.0, 'y': 10.0, 'width': 50.0, 'height': 30.0})
        self.assertEqual(t['m_RD']['textureRect'], {'x': 5.0, 'y': 10.0, 'width': 50.0, 'height': 30.0})
        self.assertEqual(t['m_PixelsToUnits'], 50.0)
        self.assertEqual(t['m_RD']['atlasRectOffset'], {'x': -1.0, 'y': -1.0})
        # the same file's texture 7 (fix_references' own) and another texture of that file: as they were
        self.assertEqual(own.tree, _sprite(0, 7))
        self.assertEqual(other.tree, _sprite(3, 8))
        m = dict(atlas.tree['m_RenderDataMap'])
        self.assertEqual((m['a']['downscaleMultiplier'], m['b']['downscaleMultiplier']), (0.25, 1.0))


if __name__ == '__main__':
    unittest.main()
