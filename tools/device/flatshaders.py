#!/usr/bin/env python3
"""Fragment shaders that write a constant colour, for measuring a scene without the GPU's work:

    flatshaders.py <dump dir> <override dir>

<dump dir> holds a run's shader sources (UA_SHADER_DUMP=<dir>: <fnv64>.glsl each); every fragment
shader among them (no gl_Position) is written to <override dir> under the same name with its main()
replaced by one that writes grey to each of its outputs. Run the game with UA_SHADER_OVERRIDE=
<override dir> (a folder outside the bench's run folders: profrun.sh empties <bench>/<run name>):
the draws, passes, blending and every CPU-side step stay, the fragment work is gone, and the frame
rate is what the CPU side allows. A measurement, never a setting: the picture is grey. The files
are made from the game's own shaders: keep them out of any repository."""
import os, re, sys


def flatten(src):
    """the shader with a constant main(), or None (not a fragment shader, no main)"""
    if 'gl_Position' in src:
        return None
    i = src.find('void main()')
    if i < 0:
        return None
    head = src[:i]
    outs = re.findall(r'layout\s*\(\s*location\s*=\s*\d+\s*\)\s*out\s+(?:\w+\s+)?vec4\s+(\w+)\s*;', head)
    grey = 'vec4(0.5, 0.5, 0.5, 1.0)'
    if outs:
        body = ' '.join(f'{o} = {grey};' for o in outs)
    elif 'FRAG_COLOR' in head:
        body = f'FRAG_COLOR = {grey};'
    elif 'gl_FragData' in src:
        body = f'gl_FragData[0] = {grey};'
    else:
        body = f'gl_FragColor = {grey};'
    return head + 'void main()\n{\n    ' + body + '\n}\n'


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    src, dst = argv[1:3]
    os.makedirs(dst, exist_ok=True)
    n = 0
    for f in sorted(os.listdir(src)):
        if not f.endswith('.glsl'):
            continue
        with open(os.path.join(src, f), errors='replace') as fh:
            out = flatten(fh.read())
        if out is None:
            continue
        with open(os.path.join(dst, f), 'w') as fh:
            fh.write(out)
        n += 1
    print(f'{n} fragment shaders written to {dst}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
