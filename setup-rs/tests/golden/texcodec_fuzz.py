"""Made-up data for texcodec's library-level golden test (setup-rs/crates/texcodec/tests/libs.rs):
what the Python's libraries do with it, beyond one game's textures.

  texcodec_fuzz.py <out dir> [--seed N]

  * decode: random texture data of every format the textures step converts (and Alpha8), odd sizes
    included, through UnityPy's parse_image_data (Pillow's decoders) and textures.texture_image;
  * resize: random premultiplied images resized with Pillow's Lanczos, with and without a box,
    and textures.downscale on random RGBA (premultiply, resize, back);
  * astc: structured random images (gradients, edges, noise, flat areas, alpha) through
    astc_encoder at every preset textures.py and the guide use, each tier's limits, several block
    footprints; and textures.Astc in guided mode (guided_astc), with DXT seeds.

<out dir>/cases.jsonl lists the cases (inputs in <out dir>/in/, outputs as SHA-256). Run with the
reference Python (PYTHONPATH=<worktree>/convert:/opt/pylibs).
"""
import hashlib
import json
import os
import random
import sys


def sha(b):
    return hashlib.sha256(b).hexdigest()


def structured(rng, w, h, alpha=True):
    """An RGBA image with what textures have: smooth gradients, hard edges, flat areas, noise."""
    out = bytearray(w * h * 4)
    kind = rng.randrange(5)
    c0 = [rng.randrange(256) for _ in range(4)]
    c1 = [rng.randrange(256) for _ in range(4)]
    edge = rng.randrange(1, max(2, w))
    noise = rng.choice((0, 2, 8, 40, 255))
    for y in range(h):
        for x in range(w):
            if kind == 0:
                t = (x + y) / max(1, w + h - 2)
            elif kind == 1:
                t = 1.0 if (x + y // 2) >= edge else 0.0
            elif kind == 2:
                t = ((x // 3) % 2) * 0.5 + ((y // 5) % 2) * 0.5
            elif kind == 3:
                t = 0.0
            else:
                t = ((x * 7 + y * 13) % 17) / 16
            i = 4 * (y * w + x)
            for c in range(4):
                v = c0[c] + (c1[c] - c0[c]) * t
                if noise:
                    v += rng.randrange(-noise, noise + 1)
                out[i + c] = max(0, min(255, int(v)))
            if not alpha:
                out[i + 3] = 255
    return bytes(out)


def main(argv):
    from PIL import Image
    from UnityPy.enums import TextureFormat as TF
    from UnityPy.export import Texture2DConverter as C
    import astc_encoder as A
    from pmunity import textures as T
    from pmunity.astc_guide import TIERS as GT
    out = argv[0]
    seed = int(argv[argv.index('--seed') + 1]) if '--seed' in argv else 1
    rng = random.Random(seed)
    os.makedirs(os.path.join(out, 'in'), exist_ok=True)
    cases = open(os.path.join(out, 'cases.jsonl'), 'w')
    n = [0]

    def put(data):
        p = f'in/{n[0]}.bin'
        n[0] += 1
        open(os.path.join(out, p), 'wb').write(data)
        return p

    # decode
    sizes = [(4, 4), (8, 4), (37, 29), (64, 64), (130, 6), (5, 3), (1, 1)]
    bpp = {TF.DXT1: 8, TF.DXT3: 16, TF.DXT5: 16, TF.BC4: 8, TF.BC5: 16, TF.BC7: 16}
    raw = {TF.RGB24: 3, TF.RGBA32: 4, TF.ARGB32: 4, TF.BGRA32: 4, TF.Alpha8: 1}
    for fmt in list(bpp) + list(raw):
        for w, h in sizes:
            for k in range(3):
                if fmt in bpp:
                    nb = -(-w // 4) * -(-h // 4)
                    if fmt == TF.BC7 and k == 2:
                        data = b''.join(bytes([1 << rng.randrange(8)]) + bytes(rng.randrange(256) for _ in range(15))
                                        for _ in range(nb))
                    else:
                        data = bytes(rng.randrange(256) for _ in range(nb * bpp[fmt]))
                else:
                    data = bytes(rng.randrange(256) for _ in range(w * h * raw[fmt]))
                img = C.parse_image_data(data, w, h, fmt, (2017, 4, 9, 1), 2, None, flip=False)
                rgba = T.texture_image(img, fmt.name)
                cases.write(json.dumps(dict(kind='decode', format=int(fmt), width=w, height=h, input=put(data),
                                            out=sha(rgba.tobytes()))) + '\n')

    # resize (premultiplied images as they are, with boxes) and downscale
    for k in range(60):
        w, h = rng.randrange(1, 300), rng.randrange(1, 300)
        if k % 10 == 9:
            w, h = rng.randrange(1, 4), rng.randrange(200, 500)       # very tall: Pillow's two steps
        data = structured(rng, w, h)
        nw, nh = rng.randrange(1, w + 3), rng.randrange(1, h + 3)
        box = None
        if k % 3 == 1:
            x0, y0 = rng.uniform(0, w / 2), rng.uniform(0, h / 2)
            box = (x0, y0, rng.uniform(x0, w), rng.uniform(y0, h))
        elif k % 3 == 2:
            y0 = rng.randrange(0, h)
            box = (0, y0, w, rng.randrange(y0, h + 1))
        im = Image.frombytes('RGBa', (w, h), data)
        try:
            r = im.resize((nw, nh), Image.Resampling.LANCZOS, box=box).tobytes()
            res = sha(r)
        except ValueError:
            res = 'error'
        cases.write(json.dumps(dict(kind='resize', width=w, height=h, input=put(data), to=[nw, nh], box=box,
                                    out=res)) + '\n')
        d = T.downscale(Image.frombytes('RGBA', (w, h), data), max(1, w * 3 // 8), max(1, h * 3 // 8))
        cases.write(json.dumps(dict(kind='downscale', width=w, height=h, input=f'in/{n[0] - 1}.bin',
                                    to=[max(1, w * 3 // 8), max(1, h * 3 // 8)], out=sha(d.tobytes()))) + '\n')

    # astc-encoder at every preset and tier, several footprints
    configs = [(q, {}) for q in ('fastest', 'fast', 'medium', 'thorough')]
    configs += [(q, T.Astc.TIERS[t]) for q in ('fast', 'medium') for t in ('line', 'near')]
    configs += [GT[i] for i in range(len(GT))]
    for k in range(12):
        w, h = rng.randrange(8, 70), rng.randrange(8, 70)
        data = structured(rng, w, h, alpha=k % 2 == 0)
        p = put(data)
        for block in ((4, 4), (5, 5), (6, 6), (8, 8), (10, 10), (12, 12), (6, 5)):
            for q, limits in (configs if block == (4, 4) else configs[:4]):
                cfg = A.ASTCConfig(A.ASTCProfile.LDR, block[0], block[1], block_z=1,
                                   quality=getattr(A.ASTCQualityPreset, q.upper()),
                                   flags=A.ASTCConfigFlags.USE_DECODE_UNORM8)
                for kk, v in limits.items():
                    setattr(cfg, kk, min(v, getattr(cfg, kk)))
                ctx = A.ASTCContext(cfg, threads=3)
                b = ctx.compress(A.ASTCImage(A.ASTCType.U8, w, h, 1, data), A.ASTCSwizzle.from_str('RGBA'))
                cases.write(json.dumps(dict(kind='astc', width=w, height=h, input=p, block=list(block), quality=q,
                                            limits=limits, out=sha(b))) + '\n')

    # textures.Astc, guided, with and without DXT seeds
    for k in range(16):
        w, h = rng.randrange(4, 90), rng.randrange(4, 90)
        data = structured(rng, w, h, alpha=k % 3 != 0)
        for q in ('fast', 'medium'):
            enc = T.Astc(q, 3, 'guided', 64, 128, 1024)
            seed = None
            if k % 2 and q == 'fast':
                dxt5 = k % 4 == 1
                nb = -(-w // 4) * -(-h // 4)
                # a DXT encoding of the image's blocks would be ideal; random endpoints exercise the path
                seed = (bytes(rng.randrange(256) for _ in range(nb * (16 if dxt5 else 8))), dxt5)
            img = Image.frombytes('RGBA', (w, h), data)
            b = enc.encode_rows(img, (4, 4), seed)
            cases.write(json.dumps(dict(kind='guided', width=w, height=h, input=put(data), quality=q,
                                        seed=put(seed[0]) if seed else None, dxt5=bool(seed and seed[1]),
                                        counts=enc.counts, out=sha(b))) + '\n')
    cases.close()
    print(f'{n[0]} inputs', file=sys.stderr)


if __name__ == '__main__':
    main(sys.argv[1:])
