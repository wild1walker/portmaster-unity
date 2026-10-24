#!/usr/bin/env python3
"""Fragment shaders that write a constant colour, for measuring a scene without the GPU's work:

    flatshaders.py [--notex] <dump dir> <override dir> [<hash> ...]

<dump dir> holds a run's shader sources (UA_SHADER_DUMP=<dir>: <fnv64>.glsl each); every fragment
shader among them (no gl_Position) is written to <override dir> under the same name with its main()
replaced by one that writes grey to each of its outputs. Run the game with UA_SHADER_OVERRIDE=
<override dir> (a folder outside the bench's run folders: profrun.sh empties <bench>/<run name>):
the draws, passes, blending and every CPU-side step stay, the fragment work is gone, and the frame
rate is what the CPU side allows. A measurement, never a setting: the picture is grey. The files
are made from the game's own shaders: keep them out of any repository.

--notex: instead, every texture lookup of each fragment shader is replaced by an expression of its
coordinate (the arithmetic before and after it stays, the fetch goes): with UA_GL_TRACE's per-pass
GPU times against the shaders as they are, whether a pass is arithmetic-bound (as long without its
fetches; the Mali's arithmetic and texture units run side by side) or fetch-bound. A shader whose
branches or loops depend on what it fetched takes other paths: read such a pass from the flat run.
Hashes after the folders: only those fragment shaders (default: all)."""
import os, re, sys

LOOKUP = re.compile(r'\b(texture|textureLod|textureProj|textureProjLod|textureGrad|textureOffset|textureLodOffset|'
                    r'texelFetch|texelFetchOffset|textureGather|textureGatherOffset|texture2D|texture2DLod|'
                    r'textureCube|shadow2D|shadow2DEXT)\s*\(')


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


def _args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        depth += ch in '([' and 1 or ch in ')]' and -1 or 0
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    return out + [cur.strip()]


def notex(src):
    """the shader with each texture lookup in main() replaced by a value made from its coordinate, or None"""
    if 'gl_Position' in src or 'void main()' not in src:
        return None
    types = {m[2]: m[1] for m in re.finditer(
        r'uniform\s+(?:(?:highp|mediump|lowp)\s+)?(\w*sampler\w+)\s+(\w+)\s*(?:\[[^\]]*\])?\s*;', src)}
    head, body = src.split('void main()', 1)
    out, i = '', 0
    while True:
        m = LOOKUP.search(body, i)
        if not m:
            return head + 'void main()' + out + body[i:]
        out += body[i:m.start()]
        j, depth = m.end(), 1
        while depth:
            depth += body[j] == '(' and 1 or body[j] == ')' and -1 or 0
            j += 1
        a = _args(body[m.end():j - 1])
        st = types.get(re.sub(r'\[.*', '', a[0]), 'sampler2D')
        coord = a[1] if len(a) > 1 else '0.0'
        rest = ''.join(f' + float({x}) * 0.0' for x in a[2:] if m[1] not in ('texelFetch', 'texelFetchOffset'))
        val = f'(float(({coord}).x){rest}) * 0.5 + 0.25'
        if 'Shadow' in st or m[1].startswith('shadow2D'):
            out += f'vec4({val})' if m[1].startswith('textureGather') else f'({val})'
        elif st.startswith('usampler'):
            out += f'uvec4(uint({val} * 8.0))'
        elif st.startswith('isampler'):
            out += f'ivec4(int({val} * 8.0))'
        else:
            out += f'vec4({val})'
        i = j


def main(argv):
    args = [a for a in argv[1:] if a != '--notex']
    if len(args) < 2:
        print(__doc__)
        return 2
    make = notex if '--notex' in argv else flatten
    src, dst = args[:2]
    only = set(args[2:])
    os.makedirs(dst, exist_ok=True)
    n = 0
    for f in sorted(os.listdir(src)):
        if not f.endswith('.glsl') or (only and f[:-5] not in only):
            continue
        with open(os.path.join(src, f), errors='replace') as fh:
            out = make(fh.read())
        if out is None:
            continue
        with open(os.path.join(dst, f), 'w') as fh:
            fh.write(out)
        n += 1
    print(f'{n} fragment shaders written to {dst}' + (' (texture lookups replaced)' if make is notex else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
