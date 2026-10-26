"""Unit tests of guided ASTC encoding (convert/native/guided_astc, textures.mode "guided") on
synthetic images and DXT blocks: the module is built with the system's C compiler into a temporary
folder (skipped without one). python -m unittest tests.test_guided_astc   (from convert/)"""
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'native', 'guided_astc'))
from pmunity import textures as T  # noqa: E402

G = None
_tmp = None


def setUpModule():
    global G, _tmp
    import build
    _tmp = tempfile.mkdtemp(prefix='guided_astc-')
    try:
        sys.path.insert(0, build.build(_tmp, quiet=True))
    except (OSError, subprocess.CalledProcessError) as ex:
        raise unittest.SkipTest(f'guided_astc not built: {ex}')
    import guided_astc
    G = guided_astc


def tearDownModule():
    if _tmp:
        shutil.rmtree(_tmp, ignore_errors=True)


def astc(quality='medium'):
    import astc_encoder as A
    cfg = A.ASTCConfig(A.ASTCProfile.LDR, 4, 4, block_z=1, quality=getattr(A.ASTCQualityPreset, quality.upper()),
                       flags=A.ASTCConfigFlags.USE_DECODE_UNORM8)
    return A.ASTCContext(cfg, threads=2)


def decode(blocks, w, h):
    """astc-encoder's decoder (decode_unorm8, as the encoder assumes): RGBA bytes"""
    import astc_encoder as A
    img = A.ASTCImage(A.ASTCType.U8, w, h, 1)
    astc().decompress(bytes(blocks), img, A.ASTCSwizzle.from_str('RGBA'))
    return bytes(img.data)


def blocks_image(blocks):
    """16-texel blocks (lists of RGBA tuples), 4 across -> (rgba bytes, w, h)"""
    across = 4
    rows = -(-len(blocks) // across)
    w, h = 4 * across, 4 * rows
    out = bytearray(w * h * 4)
    for n, b in enumerate(blocks):
        bx, by = n % across, n // across
        for i, px in enumerate(b):
            x, y = 4 * bx + i % 4, 4 * by + i // 4
            out[4 * (y * w + x):4 * (y * w + x) + 4] = bytes(px)
    return bytes(out), w, h


def sse(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b))


def line_block(rng, alpha):
    a = [rng.randrange(256) for _ in range(4)]
    b = [rng.randrange(256) for _ in range(4)]
    out = []
    for _ in range(16):
        t = rng.random()
        px = [round(a[c] + (b[c] - a[c]) * t) for c in range(4)]
        if not alpha:
            px[3] = 255
        out.append(tuple(px))
    return out


class GuidedModuleTest(unittest.TestCase):
    def test_one_colour_blocks_are_astc_encoders_void_extent(self):
        rng = random.Random(1)
        blocks = [[(r, g, b, a)] * 16 for r, g, b, a in
                  [(0, 0, 0, 0), (255, 255, 255, 255)] + [tuple(rng.randrange(256) for _ in range(4)) for _ in range(6)]]
        data, w, h = blocks_image(blocks)
        out, rests, counts, err = G.encode(data, w, h, 4, 4, 64, 2)
        self.assertEqual(counts, (8, 0, 0, 0, 0, 0))
        self.assertEqual(rests, (b'', b'', b''))
        import astc_encoder as A
        ref = astc().compress(A.ASTCImage(A.ASTCType.U8, w, h, 1, data), A.ASTCSwizzle.from_str('RGBA'))
        self.assertEqual(bytes(out), ref)
        self.assertEqual(decode(out, w, h), data)

    def test_fitted_blocks_decode_with_the_error_reported(self):
        rng = random.Random(2)
        for alpha in (False, True):
            blocks = [line_block(rng, alpha) for _ in range(64)]
            data, w, h = blocks_image(blocks)
            out, rests, counts, err = G.encode(data, w, h, 4, 4, 1 << 29, 2)
            self.assertEqual(counts[1], 64)
            self.assertEqual(sse(decode(out, w, h), data), err)    # the module's decode is astc-encoder's

    def test_classification(self):
        rng = random.Random(3)
        flat = [(10, 20, 30, 255)] * 16
        line = [(100 + 4 * i, 50 + 2 * i, 20, 255) for i in range(16)]           # one gradient: a line
        noise = [tuple(rng.randrange(256) for _ in range(4)) for _ in range(16)]  # nowhere near one
        data, w, h = blocks_image([flat, line, noise, line])
        out, (lines, near, other), counts, err = G.encode(data, w, h, 4, 4, 64, 1, 128, 1024)
        self.assertEqual(counts, (1, 2, 0, 0, 0, 1))
        self.assertEqual(other, (2).to_bytes(4, 'little'))
        # without fitting, the gradient is left to the encoder's one-partition search
        out, (lines, near, other), counts, err = G.encode(data, w, h, 4, 4, -1, 1, 128, 1024)
        self.assertEqual(counts, (1, 0, 0, 2, 0, 1))
        self.assertEqual(lines, (1).to_bytes(4, 'little') + (3).to_bytes(4, 'little'))

    def test_gather_repeats_the_edge_as_the_encoder_does(self):
        w, h = 5, 6                                                       # partial blocks
        data = bytes(i * 7 % 256 for i in range(w * h * 4))
        rest = b''.join(i.to_bytes(4, 'little') for i in (3, 0))          # block (1, 1) then (0, 0)
        sub = G.gather(data, w, h, 4, 4, rest, 2)
        px = lambda x, y: data[4 * (min(y, h - 1) * w + min(x, w - 1)):][:4]
        self.assertEqual(sub[0:4], px(4, 4))                              # block 3's first texel
        self.assertEqual(sub[12:16], px(7, 4))                            # past the right edge: column 4
        self.assertEqual(sub[16:20], px(0, 0))                            # block 0
        self.assertEqual(len(sub), 8 * 4 * 4)


class DxtSeedTest(unittest.TestCase):
    def _dxt1(self, rng, three=False):
        c0, c1 = rng.randrange(1 << 16), rng.randrange(1 << 16)
        if (c0 > c1) == three:
            c0, c1 = c1, c0
        if c0 == c1:
            c0, c1 = (c0 + 1) & 0xFFFF, c0
            if three:
                c0, c1 = min(c0, c1), max(c0, c1)
        idx = [rng.randrange(3 if three else 4) for _ in range(16)]
        bits = sum(i << (2 * n) for n, i in enumerate(idx))
        return c0.to_bytes(2, 'little') + c1.to_bytes(2, 'little') + bits.to_bytes(4, 'little')

    def _check(self, fmt, raw, w, h):
        import texture2ddecoder
        dec = getattr(texture2ddecoder, 'decode_bc1' if fmt == 'DXT1' else 'decode_bc3')(raw, w, h)
        rgba = bytearray(dec)
        for i in range(0, len(rgba), 4):                                  # BGRA -> RGBA
            rgba[i], rgba[i + 2] = rgba[i + 2], rgba[i]
        if fmt == 'DXT1':
            rgba[3::4] = b'\xff' * (len(rgba) // 4)                       # (sampled as RGB)
        out, rests, counts, err = G.encode(bytes(rgba), w, h, 4, 4, 1 << 29, 2, -1, -1, raw, int(fmt == 'DXT5'))
        dec2 = decode(out, w, h)
        return counts, max(abs(a - b) for a, b in zip(dec2, rgba)), err, sse(dec2, rgba)

    def test_dxt1_blocks_transcode_within_two_steps(self):
        rng = random.Random(4)
        raw = b''.join(self._dxt1(rng, three=n % 4 == 3) for n in range(64))
        counts, worst, err, real = self._check('DXT1', raw, 32, 32)
        self.assertEqual(err, real)
        self.assertGreaterEqual(counts[2], 48)                            # the seeds (or a fit as good)
        self.assertLessEqual(worst, 2)

    def test_dxt5_with_one_alpha_per_block(self):
        rng = random.Random(5)
        blocks = []
        for n in range(64):
            a = rng.choice((255, 0, rng.randrange(256)))
            blocks.append(bytes([a, a]) + b'\0' * 6 + self._dxt1(rng))   # alpha: both endpoints the same
        counts, worst, err, real = self._check('DXT5', b''.join(blocks), 32, 32)
        self.assertEqual(err, real)
        self.assertGreaterEqual(counts[2] + counts[0], 48)
        self.assertLessEqual(worst, 2)


class AstcGuidedTest(unittest.TestCase):
    def _image(self, w, h, seed):
        from PIL import Image
        rng = random.Random(seed)
        img = Image.new('RGBA', (w, h))
        px = img.load()
        for y in range(h):
            for x in range(w):
                if (x // 7 + y // 5) % 3 == 0:
                    px[x, y] = (40, 80, 120, 255)                                       # flat
                elif (x // 7 + y // 5) % 3 == 1:
                    px[x, y] = (3 * x % 256, 2 * y % 256, 90, 255 - x % 200)            # smooth
                else:
                    px[x, y] = tuple(rng.randrange(256) for _ in range(4))              # noise
        return img

    def test_without_fits_or_routes_the_blocks_are_the_encoders(self):
        img = self._image(37, 23, 6)                   # partial blocks at both edges
        ref = T.Astc('fast', 2, 'search').encode(img, (4, 4))
        enc = T.Astc('fast', 2, 'guided', max_err=-1, line=-1, near=-1)
        self.assertEqual(enc.mode, 'guided')
        self.assertEqual(enc.encode(img, (4, 4)), ref)
        self.assertGreater(enc.counts[0], 0)
        self.assertEqual(T.Astc('fast', 2, 'guided').encode(img, (6, 6)), T.Astc('fast', 2, 'search').encode(img, (6, 6)))

    def test_guided_is_close_to_search(self):
        img = self._image(64, 64, 7)
        ref = decode(T.Astc('medium', 2, 'search').encode(img, (4, 4)), 64, 64)
        enc = T.Astc('medium', 2, 'guided')
        out = decode(enc.encode(img, (4, 4)), 64, 64)
        from PIL import Image
        src = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes()
        self.assertLessEqual(sse(out, src), sse(ref, src) * 1.05)
        self.assertGreater(enc.counts[1], 0)

    def test_strips_give_the_whole_images_blocks(self):
        from UnityPy.export import Texture2DConverter as C
        rng = random.Random(8)
        w, h = 64, 50
        raw = b''.join(DxtSeedTest._dxt1(None, rng) for _ in range((w // 4) * (-(-h // 4))))
        t = {'m_TextureFormat': 10, 'm_Width': w, 'm_Height': h}
        ch = T.Choice((4, 4), w, h, False, 1)
        enc = T.Astc('fast', 2, 'guided')
        whole = T.texture_image(C.parse_image_data(raw, w, h, 10, (2021, 3, 0, 0), 0, flip=False), 'DXT1')
        old_px = T.STRIP_PX
        T.STRIP_PX = w * 8
        try:
            strips = T.strip_blocks(object(), t, raw, ch, enc, 'DXT1')
        finally:
            T.STRIP_PX = old_px
        self.assertEqual(strips, enc.encode_rows(whole, (4, 4), (raw, False)))
        self.assertGreater(enc.counts[2], 0)

    def test_falls_back_to_search_without_the_module(self):
        logged = []
        old = T.guided_module
        T.guided_module = lambda: None
        try:
            enc = T.Astc('fast', 2, 'guided', log=logged.append)
        finally:
            T.guided_module = old
        self.assertEqual(enc.mode, 'search')
        self.assertEqual(len(logged), 1)
        with self.assertRaises(ValueError):
            T.Astc('fast', 2, 'quick')


if __name__ == '__main__':
    unittest.main()
