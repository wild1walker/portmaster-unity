#!/usr/bin/env python3
"""Count a frame's 'dead' fragments: those whose texture sample has alpha exactly 0, which a sprite
drawn with alpha blending cannot change the picture with (a measurement, never a setting):

    deadfrags.py shaders <dump dir> <trace log> <override dir> [--copy H,..] [--mark H,..] [--hide H,..] [--only H,..]
    deadfrags.py sum <run dir>... [--rows A:B] [--skip X0,Y0,X1,Y1]

`shaders`: <dump dir> holds a run's shader sources (UA_SHADER_DUMP), <trace log> the run.log of a
UA_GL_TRACE_DRAWS run (each draw's fragment shader hash and blend state). Every fragment shader of
the traced frame that samples _MainTex at a plain varying (`texture(_MainTex, vs_X.xy)`) is written
to <override dir> with a main() that adds 1/255 to red when that sample's alpha is exactly 0 and to
green otherwise, in a way its blend state turns into an exact count:

  blend 1,1-sa      writes (d, l, 0, 0) / 255         dst + src
  blend sa,1-sa     writes (d, l, 0, 1/255)           dst * 254/255 + src / 255: +1 code where
                                                      src is 1, the others kept (counts under 127)
  blend sa,1        writes ((d, l, 0) / 255, 1)       dst + src

Other blend states are left out (listed). The roles of the frame's other fragment shaders are
given by hash: --copy (post effects between the scene and the window: an exact copy of _MainTex,
texelFetch at the fragment, so the counts reach the screenshot), --mark (adds 1/255 to blue: an
opaque background copy, a full-screen overlay; blend off or 1,1-sa), --hide (writes nothing: rgb
and alpha 0, e.g. the HUD's own shaders), --only (count only these; the other counting shaders hide).
Run the game with UA_SHADER_OVERRIDE=<override dir> and a plan's `shot` steps (a folder outside the
bench's run folders: profrun.sh empties <bench>/<run name>).

`sum`: adds the counters up over each run's shot-*.ppm: dead and live fragments, as full-screen
layers of the picture (--rows A:B: the picture's rows in the window, e.g. 15:465 for a 720x450
picture in a 720x480 window; --skip: a box left out, e.g. a HUD drawn with the same shaders).

The override files are made from the game's own shaders: keep them out of any repository."""
import glob
import os
import re
import sys

BLENDS = ('1,1-sa', 'sa,1-sa', 'sa,1')


def blends(log_text):
    """fragment shader hash -> set of blend states it was drawn with in the traced frame"""
    out = {}
    for m in re.finditer(r' fs ([0-9a-f]{16}) .*?\| .*?blend (\S+)', log_text):
        out.setdefault(m.group(1), set()).add(m.group(2))
    return out


def split_main(src):
    i = src.find('void main()')
    return (src[:i], src[i:]) if i >= 0 else (None, None)


def output_name(head):
    m = re.search(r'layout\s*\(\s*location\s*=\s*0\s*\)\s*out\s+(?:\w+\s+)?vec4\s+(\w+)\s*;', head)
    return m and m.group(1)


def main_uv(src):
    m = re.search(r'texture\(_MainTex,\s*(vs_\w+)\.xy\)', src)
    return m and m.group(1) + '.xy'


def body(role, blend, o, uv=None):
    """the statements of the new main(), or None when the role does not fit the blend state"""
    if role == 'copy':
        return [f'{o} = texelFetch(_MainTex, ivec2(gl_FragCoord.xy), 0);']
    if role == 'mark':
        return [f'{o} = vec4(0.0, 0.0, 1.0 / 255.0, 0.0);'] if blend in ('off', '1,1-sa') else None
    if role == 'hide':
        return [f'{o} = vec4(0.0);'] if blend in BLENDS else None
    if role == 'count':
        head = [f'float ua_a = texture(_MainTex, {uv}).w;', 'float ua_d = (ua_a == 0.0) ? 1.0 : 0.0;']
        if blend == '1,1-sa':
            return head + [f'{o} = vec4(ua_d, 1.0 - ua_d, 0.0, 0.0) * (1.0 / 255.0);']
        if blend == 'sa,1-sa':
            return head + [f'{o} = vec4(ua_d, 1.0 - ua_d, 0.0, 1.0 / 255.0);']
        if blend == 'sa,1':
            return head + [f'{o} = vec4(vec3(ua_d, 1.0 - ua_d, 0.0) * (1.0 / 255.0), 1.0);']
    return None


def rewrite(src, role, blend):
    """(new source, role) or (None, reason)"""
    if 'gl_Position' in src:
        return None, 'vertex shader'
    head, _ = split_main(src)
    if head is None:
        return None, 'no main()'
    o = output_name(head)
    if not o:
        return None, 'no layout(location = 0) vec4 output'
    uv = main_uv(src)
    if role == 'count' and not uv:
        return None, 'no _MainTex sample at a varying'
    b = body(role, blend, o, uv)
    if not b:
        return None, f'blend {blend}'
    return head + 'void main()\n{\n    ' + '\n    '.join(b) + '\n}\n', role


def cmd_shaders(args):
    def opt(name):
        if name in args:
            k = args.index(name)
            v = set(args[k + 1].split(','))
            del args[k:k + 2]
            return v
        return set()
    copy, mark, hide, only = opt('--copy'), opt('--mark'), opt('--hide'), opt('--only')
    if len(args) != 3:
        print(__doc__)
        return 2
    dump, log, dst = args
    os.makedirs(dst, exist_ok=True)
    with open(log, errors='replace') as f:
        bl = blends(f.read())
    n = 0
    for fs, bs in sorted(bl.items()):
        path = os.path.join(dump, fs + '.glsl')
        if not os.path.exists(path):
            print(f'{fs}: not in {dump}')
            continue
        if len(bs) != 1:
            print(f'{fs}: left (drawn with several blend states: {", ".join(sorted(bs))})')
            continue
        blend = next(iter(bs))
        role = 'copy' if fs in copy else 'mark' if fs in mark else 'hide' if fs in hide else 'count'
        if role == 'count' and only and fs not in only:
            role = 'hide'
        with open(path, errors='replace') as f:
            out, what = rewrite(f.read(), role, blend)
        if out is None:
            print(f'{fs}: left ({what})')
            continue
        with open(os.path.join(dst, fs + '.glsl'), 'w') as f:
            f.write(out)
        n += 1
        print(f'{fs}: {what} (blend {blend})')
    print(f'{n} fragment shaders written to {dst}')
    return 0


def counts(img, rows=None, skip=None):
    """(dead, live, marked, pixels) of one screenshot (numpy HxWx3 ints)"""
    import numpy as np
    keep = np.ones(img.shape[:2], bool)
    if skip:
        x0, y0, x1, y1 = skip
        keep[y0:y1, x0:x1] = False
    if rows:
        keep[:rows[0]] = False
        keep[rows[1]:] = False
    return (int(img[..., 0][keep].sum()), int(img[..., 1][keep].sum()), int(img[..., 2][keep].sum()),
            int(keep.sum()))


def cmd_sum(args):
    import numpy as np
    from PIL import Image
    rows = skip = None
    if '--rows' in args:
        k = args.index('--rows')
        rows = tuple(int(v) for v in args[k + 1].split(':'))
        del args[k:k + 2]
    if '--skip' in args:
        k = args.index('--skip')
        skip = tuple(int(v) for v in args[k + 1].split(','))
        del args[k:k + 2]
    for d in args:
        fs = sorted(glob.glob(os.path.join(d, 'shot-*.ppm')) + glob.glob(os.path.join(d, 'shot-*.png')))
        res = [counts(np.asarray(Image.open(f).convert('RGB')).astype(np.int64), rows, skip) for f in fs]
        if not res:
            print(f'{d}: no shots')
            continue
        dead, live, mark, px = (sum(r[i] for r in res) / len(res) for i in range(4))
        print(f'{os.path.basename(os.path.normpath(d))}: {len(res)} shots, {px:.0f} pixels; sprite fragments '
              f'{(dead + live) / px:.2f} layers, dead {dead / px:.2f} ({dead / max(1, dead + live) * 100:.1f} %), '
              f'marked {mark / px:.2f}')
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] not in ('shaders', 'sum'):
        print(__doc__)
        return 2
    return (cmd_shaders if argv[1] == 'shaders' else cmd_sum)(argv[2:])


if __name__ == '__main__':
    sys.exit(main(sys.argv))
