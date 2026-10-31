# python ref_side.py <dir>: the handheld's Python stack (Pillow 12.3.0, guided_astc, astc_encoder_py
# 0.1.12) on wasm_side.js's inputs; prints each case: same, or how many bytes differ
import json, sys
from PIL import Image
import guided_astc as G
import astc_encoder as A

d = sys.argv[1]
rd = lambda n: open(f'{d}/{n}', 'rb').read()
BCN = {10: ('RGBA', 1), 12: ('RGBA', 3), 25: ('RGBA', 7), 26: ('L', 4), 27: ('RGB', 5)}
RAW = {4: ('RGBA', 'RGBA'), 5: ('RGBA', 'ARGB'), 14: ('RGBA', 'BGRA'), 3: ('RGB', 'RGB')}
PRESET = {60: 'MEDIUM', 0: 'FASTEST', 10: 'FAST'}
fails = 0


def cmp(name, a, b):
    global fails
    if a == b:
        print(f'{name}: same ({len(a)} bytes)')
    else:
        n = sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
        fails += 1
        print(f'{name}: DIFFERENT, {n} of {len(b)} bytes')


for c in json.load(open(f'{d}/cases.json')):
    if c['kind'] == 'decode':
        f, w, h = c['fmt'], c['w'], c['h']
        src = rd(f'dec_{f}.in')
        if f in BCN:
            mode, n = BCN[f]
            pw, ph = -(-w // 4) * 4, -(-h // 4) * 4
            img = Image.frombytes(mode, (pw, ph), src, 'bcn', n).crop((0, 0, w, h))
        else:
            mode, raw = RAW[f]
            img = Image.frombytes(mode, (w, h), src, 'raw', raw)
        if f == 26:   # texture_image: BC4 -> (r, 0, 0, 1)
            r = img.convert('L'); z = Image.new('L', r.size, 0)
            img = Image.merge('RGBA', (r, z, z, Image.new('L', r.size, 255)))
        cmp(f'decode fmt {f} {w}x{h}', rd(f'dec_{f}.out'), img.convert('RGBA').tobytes())
    elif c['kind'] == 'resize':
        img = Image.frombytes('RGBA', (c['sw'], c['sh']), rd(f'rs_{c["tag"]}.in'))
        ref = img.convert('RGBa').resize((c['dw'], c['dh']), Image.Resampling.LANCZOS).convert('RGBA')
        cmp(f'resize {c["tag"]}', rd(f'rs_{c["tag"]}.out'), ref.tobytes())
    elif c['kind'] == 'guided':
        t = c['tag']
        blocks, rests, counts, _ = G.encode(rd(f'gd_{t}.in'), c['w'], c['h'], c['bw'], c['bh'], 64, 1, 128, 1024)
        cls = rd(f'gd_{t}.cls')
        SLOT = [5, 0, 1, 2, 3, 4]
        mine = [0] * 6
        for k in cls:
            mine[SLOT[k]] += 1
        cmp(f'guided {t} blocks', rd(f'gd_{t}.blocks'), bytes(blocks))
        print(f'  counts wasm {mine} python {list(counts)}')
        rest = b''.join(rests)
        import array
        mine_rest = array.array('I', [i for i, k in enumerate(cls) if k in (0, 4, 5)])
        ref_rest = array.array('I'); ref_rest.frombytes(rest)
        if sorted(mine_rest) == sorted(ref_rest) and len(rest):
            cmp(f'gather {t}', rd(f'gd_{t}.gather'), G.gather(rd(f'gd_{t}.in'), c['w'], c['h'], c['bw'], c['bh'], mine_rest.tobytes(), min(len(mine_rest), 256)))
    elif c['kind'] == 'astc':
        cfg = A.ASTCConfig(A.ASTCProfile.LDR, c['bw'], c['bh'], block_z=1, quality=getattr(A.ASTCQualityPreset, PRESET[c['q']]),
                           flags=A.ASTCConfigFlags.USE_DECODE_UNORM8)
        if c['p1'] >= 0: cfg.tune_partition_count_limit = min(c['p1'], cfg.tune_partition_count_limit)
        if c['p2'] >= 0: cfg.tune_2partition_index_limit = min(c['p2'], cfg.tune_2partition_index_limit)
        ctx = A.ASTCContext(cfg, threads=1)
        img = A.ASTCImage(A.ASTCType.U8, c['cols'] * c['bw'], c['rows'] * c['bh'], 1, rd(f'gd_{c["tag"]}.gather'))
        cmp(f'astc {c["tag"]} {c["name"]}', rd(f'gd_{c["tag"]}.astc_{c["name"]}'), ctx.compress(img, A.ASTCSwizzle.from_str('RGBA')))
print('FAILS', fails)
