#!/usr/bin/env python3
"""Exact comparison of screenshots from two deterministic runs: imgdiff.py <dirA> <dirB> [pattern]
For each file in both: identical, or the number of differing pixels, the largest channel difference
and the bounding box of the differences (and <name>.diff.png beside B, differences in white)."""
import glob, os, sys
from PIL import Image, ImageChops
a, b = sys.argv[1], sys.argv[2]
pat = sys.argv[3] if len(sys.argv) > 3 else '*.png'
for fa in sorted(glob.glob(os.path.join(a, pat))):
    fb = os.path.join(b, os.path.basename(fa))
    if not os.path.exists(fb) or fa.endswith('.diff.png'):
        continue
    ia, ib = Image.open(fa).convert('RGB'), Image.open(fb).convert('RGB')
    if ia.size != ib.size:
        print(f'{os.path.basename(fa)}: sizes differ {ia.size} {ib.size}'); continue
    d = ImageChops.difference(ia, ib)
    box = d.getbbox()
    if not box:
        print(f'{os.path.basename(fa)}: identical'); continue
    px = d.load(); n = 0; mx = 0
    for y in range(box[1], box[3]):
        for x in range(box[0], box[2]):
            p = px[x, y]
            if p != (0, 0, 0): n += 1; mx = max(mx, *p)
    print(f'{os.path.basename(fa)}: {n} pixels differ (of {ia.size[0] * ia.size[1]}), max channel difference {mx}, in {box}')
    d.point(lambda v: 255 if v else 0).save(fb[:-4] + '.diff.png')
