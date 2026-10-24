#!/usr/bin/env python3
"""Does a game show the same part of its scene at a smaller render size (UA_RENDER_SIZE)?

    framecmp.py <full shot> <small shot> [<out prefix>]

Both are screenshots of the same scene (a profbench plan's `shot` step: shot-<n>.ppm), the full one
at the panel's size, the small one at the render size. The full shot is scaled by (small / full) x k
for k = 0.70 .. 1.45, centred on the small one, and the two compared in the small picture's middle
70 %: the k with the least difference is the framing ratio. 1.00: the same part of the scene (only
sharpness differs); full / small (1.20 at 600x400, 1.33 at 540x360 on a 720x480 panel): a camera that
keeps its pixels (pixel-perfect, a zoom picked from Screen.height), which shows less of the scene at
the smaller size. A HUD at another place or size shows in the out prefix's pictures: <prefix>-pair.png
(the full shot scaled to the small size | the small shot) and <prefix>-diff.png (their difference x 4).
Animated parts differ anyway: compare two panel-size runs' shots for what that is. Needs Pillow and
NumPy (python3 -I with them installed)."""
import sys

import numpy as np
from PIL import Image


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    full, small = Image.open(sys.argv[1]).convert('RGB'), Image.open(sys.argv[2]).convert('RGB')
    out = sys.argv[3] if len(sys.argv) > 3 else None
    sw, sh = small.size
    fw, fh = full.size
    s0 = sw / fw
    S = np.asarray(small.convert('L'), dtype=np.float32)
    G = full.convert('L')
    diffs = {}
    for k100 in range(70, 146):
        k = k100 / 100
        w, h = max(8, round(fw * s0 * k)), max(8, round(fh * s0 * k))
        F = np.asarray(G.resize((w, h), Image.BILINEAR), dtype=np.float32)
        ox, oy = (w - sw) / 2, (h - sh) / 2           # a pixel of S at (x, y) is F's (x + ox, y + oy)
        x0, x1 = max(int(sw * 0.15), int(np.ceil(-ox))), min(int(sw * 0.85), int(np.floor(w - ox)))
        y0, y1 = max(int(sh * 0.15), int(np.ceil(-oy))), min(int(sh * 0.85), int(np.floor(h - oy)))
        if x1 - x0 < sw * 0.3 or y1 - y0 < sh * 0.3:
            continue
        fx, fy = int(round(x0 + ox)), int(round(y0 + oy))
        a, b = S[y0:y1, x0:x1], F[fy:fy + (y1 - y0), fx:fx + (x1 - x0)]
        if a.shape == b.shape:
            diffs[k100] = float(np.abs(a - b).mean())
    best = min(diffs, key=diffs.get)
    pp = round(100 * fw / sw)
    print(f'full {fw}x{fh}, small {sw}x{sh}: framing ratio {best / 100:.2f} (difference {diffs[best]:.2f}, 0-255); '
          f'at 1.00: {diffs.get(100, float("nan")):.2f}; at {pp / 100:.2f} (pixels kept): {diffs.get(pp, float("nan")):.2f}')
    if out:
        F1 = full.resize((sw, sh), Image.BILINEAR)
        d = np.abs(np.asarray(small, dtype=np.int16) - np.asarray(F1, dtype=np.int16))
        Image.fromarray(np.clip(d.max(axis=2) * 4, 0, 255).astype(np.uint8)).save(out + '-diff.png')
        pair = Image.new('RGB', (sw * 2, sh))
        pair.paste(F1, (0, 0))
        pair.paste(small, (sw, 0))
        pair.save(out + '-pair.png')
        print(f'  the whole picture at 1.00: {d.mean():.2f} mean difference; {out}-pair.png, {out}-diff.png')


main()
