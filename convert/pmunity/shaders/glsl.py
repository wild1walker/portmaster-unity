"""GLSL 1.50 as Unity's HLSLcc writes it for OpenGL Core -> GLSL ES 3.00 for Unity's GLES3 renderer.

A GL program's code holds every stage, each in its own block, and Unity's GL backends compile each
block as one shader (the block's first line is its #version):

    #ifdef VERTEX
    #version 150
    #extension GL_ARB_explicit_attrib_location : require
    ...
    #endif
    #ifdef FRAGMENT
    ...
    #endif

HLSLcc already writes the subset of GLSL that both languages share (explicit conversions, `texture`
and `textureLod`, `layout(location)` outputs, no fixed-function state), so a program needs only:

  * `#version 300 es` instead of `#version 150` / `#version 330`;
  * no desktop extensions that ES 3.00 has built in (explicit attribute locations, the float/int bit
    casts); any other extension is kept, so a program that needs one fails to compile on a driver
    without it, as Unity's own GLES3 build would;
  * default precisions, because ES has no default float precision in fragment shaders and gives
    samplers a default of lowp. The default here is highp for floats, ints and every sampler type
    the stage declares: desktop GL computes and samples in 32-bit floats, so highp gives the same
    numbers the game shows on a PC or Mac. `precision='default-samplers'` leaves samplers at ES's
    defaults (sampler2D and samplerCube lowp; the result of a lowp texture() is then only 10 bits
    precise) - not the game's own picture, for A/B tests only.

Two rewrites that keep every operation and its operands (so the results are the same), on by
default (`optimize`):

  * const-arrays: HLSLcc writes a constant table (blur weights, sample offsets) as a global array
    that main() fills element by element on every invocation (`ImmCB_0_0_0[0] = vec4(...);`). It
    becomes a `const` array with the same values, which the driver folds into the code.
  * unroll: a loop with literal bounds (`for(int i = 0 ; i<7 ; i++)`, at most UNROLL_MAX trips)
    whose body only computes (no break / continue / return / discard / declarations / nested
    loop, the counter only read) is written out once per trip with the counter as a literal.

On the RGSP's Mali-G31 both together make Cuphead's 7-tap blur passes ~4% cheaper (see
tools/shadercheck -t); the rest of a pass is texture fetches.

Programs that use GLSL with no ES 3.00 equivalent (fixed-function built-ins such as gl_Vertex,
gl_FragColor in a 300 es shader, rectangle and 1D samplers, `noperspective`, ES 3.1+ functions) raise
Unsupported; the converter then gives that shader no GLES3 programs, and Unity treats it as not
supported on the device, just as a shader without GLES3 programs in an Android build.
"""
import re


class Unsupported(Exception):
    pass


VERSIONS = ('150', '330')                    # #version lines this translation handles
DROP_EXTENSIONS = {
    'GL_ARB_explicit_attrib_location',       # ES 3.00: layout(location) on vertex inputs and fragment outputs
    'GL_ARB_shader_bit_encoding',            # ES 3.00: floatBitsToInt & co.
}
SAMPLER_TYPES = ('sampler2D', 'sampler3D', 'samplerCube', 'sampler2DShadow', 'samplerCubeShadow',
                 'sampler2DArray', 'sampler2DArrayShadow', 'isampler2D', 'isampler3D', 'isamplerCube',
                 'isampler2DArray', 'usampler2D', 'usampler3D', 'usamplerCube', 'usampler2DArray')
_SAMPLER_RE = re.compile(r'\b(%s)\b' % '|'.join(SAMPLER_TYPES))

# no ES 3.00 equivalent
_UNSUPPORTED = [
    (re.compile(r'\bgl_(Vertex|Normal|Color|SecondaryColor|MultiTexCoord\d|ModelView\w*|Projection\w*|NormalMatrix|TextureMatrix\w*|FragColor|FragData)\b'),
     'fixed-function built-in'),
    (re.compile(r'\b(sampler1D\w*|sampler2DRect\w*|samplerBuffer|sampler2DMS\w*|image\w+D\w*)\b'), 'sampler type'),
    (re.compile(r'\b(texture1D\w*|texture2D\w*|texture3D\w*|textureCube\w*|shadow2D\w*)\s*\('), 'GLSL 1.10 texture function'),
    (re.compile(r'\b(textureGather\w*|textureQueryLod|textureQueryLevels|fma|bitfield\w+|bitCount|findLSB|findMSB|'
                r'imageLoad|imageStore|atomic\w+|memoryBarrier\w*|barrier|interpolateAt\w+|packUnorm4x8|packSnorm4x8|'
                r'unpackUnorm4x8|unpackSnorm4x8|uaddCarry|usubBorrow|umulExtended|imulExtended|frexp|ldexp)\s*\('),
     'ES 3.1+ function'),
    (re.compile(r'\b(noperspective|sample|patch|subroutine|precise)\b(?=\s+(in|out|uniform|centroid|flat|smooth|vec|float|int|uint|mat|bool))'),
     'qualifier'),
    (re.compile(r'layout\s*\([^)]*\b(binding|offset|std430|index)\b'), 'layout qualifier'),
    (re.compile(r'\bbuffer\s+\w+\s*\{'), 'storage buffer'),
    (re.compile(r'\b(gl_ClipDistance|gl_PrimitiveID|gl_Layer|gl_ViewportIndex|gl_SampleID|gl_SamplePosition|gl_SampleMask\w*)\b'),
     'built-in variable'),
    (re.compile(r'\b(double|dvec\d|dmat\d)\b'), 'double precision'),
]


def stage_blocks(code):
    """[(stage, start, end)]: the text between '#ifdef <STAGE>' and its matching #endif (code[start:end])."""
    out = []
    lines = code.splitlines(keepends=True)
    pos = 0
    i = 0
    while i < len(lines):
        m = re.match(r'\s*#\s*ifdef\s+(VERTEX|FRAGMENT|GEOMETRY|HULL|DOMAIN)\b', lines[i])
        if not m:
            pos += len(lines[i]); i += 1
            continue
        stage = m.group(1)
        start = pos + len(lines[i])
        depth = 1
        pos = start; i += 1
        while i < len(lines) and depth:
            if re.match(r'\s*#\s*(if|ifdef|ifndef)\b', lines[i]):
                depth += 1
            elif re.match(r'\s*#\s*endif\b', lines[i]):
                depth -= 1
                if depth == 0:
                    out.append((stage, start, pos))
            pos += len(lines[i]); i += 1
    return out


def _strip_comments(s):
    return re.sub(r'//[^\n]*|/\*.*?\*/', ' ', s, flags=re.S)


UNROLL_MAX = 16
OPTIMIZE = ('const-arrays', 'unroll')
_NUM = r'[-+]?(?:\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?)[uU]?'
_TYPES = r'(?:float|int|uint|bool|[biu]?vec[234]|mat[234](?:x[234])?)'


def const_arrays(src):
    """Global arrays that main() fills with literals before anything else -> const arrays."""
    done = 0
    for m in list(re.finditer(r'^(%s) (ImmCB_\w+)\[(\d+)\];\n' % _TYPES, src, re.M)):
        typ, name, n = m.group(1), m.group(2), int(m.group(3))
        # element assignments: '<name>[i] = <typ>(<literals>);' or '<name>[i] = <literal>;'
        rx = re.compile(r'^[ \t]*%s\[(\d+)\] = ((?:%s)\((?:\s*%s\s*,)*\s*%s\s*\)|%s);\n' % (name, _TYPES, _NUM, _NUM, _NUM), re.M)
        assigns = list(rx.finditer(src))
        vals = {}
        for a in assigns:
            vals.setdefault(int(a.group(1)), []).append(a.group(2))
        if sorted(vals) != list(range(n)) or any(len(v) != 1 for v in vals.values()):
            continue
        rest = rx.sub('', src)
        # any other write to the array (assignment, compound assignment, ++/--, out argument) -> leave it
        if re.search(r'\b%s\s*\[[^\]]*\](\.\w+)?\s*([-+*/%%&|^]?=(?!=)|\+\+|--)' % name, rest) or \
           re.search(r'\b%s\b(?!\s*\[)' % name, rest.replace(m.group(0), '')):
            continue
        # the assignments must come first in main(), before any statement that could read the array
        mm = re.search(r'void main\(\)\s*\{\n', src)
        if not mm or any(a.start() < mm.end() for a in assigns):
            continue
        head = src[mm.end():max(a.end() for a in assigns)]
        if rx.sub('', head).strip():
            continue
        ctor = '%s[%d](%s)' % (typ, n, ', '.join(vals[i][0] if '(' in vals[i][0] or typ in ('float', 'int', 'uint') else f'{typ}({vals[i][0]})' for i in range(n)))
        src = rest.replace(m.group(0), 'const %s %s[%d] = %s;\n' % (typ, name, n, ctor))
        done += 1
    return src, done


def unroll(src, limit=UNROLL_MAX):
    """Loops with literal bounds and a plain body -> one copy of the body per trip."""
    done = 0
    rx = re.compile(r'^([ \t]*)for\(int (\w+) = (?:int\()?(-?\d+)\)? ?; ?\2 ?(<|<=) ?(-?\d+) ?; ?\2\+\+\)\n\1\{\n', re.M)
    pos = 0
    while True:
        m = rx.search(src, pos)
        if not m:
            break
        ind, var, a, op, b = m.group(1), m.group(2), int(m.group(3)), m.group(4), int(m.group(5))
        end = re.compile(r'^%s\}\n' % re.escape(ind), re.M).search(src, m.end())
        if not end:
            pos = m.end(); continue
        body = src[m.end():end.start()]
        trips = list(range(a, b + 1 if op == '<=' else b))
        bad = (not 0 < len(trips) <= limit or '{' in body or '}' in body or
               re.search(r'\b(break|continue|return|discard|for|while|do)\b', body) or
               re.search(r'^\s*(const\s+)?(highp |mediump |lowp )?%s\s+\w+' % _TYPES, body, re.M) or
               re.search(r'\b%s\s*([-+*/%%&|^]?=(?!=)|\+\+|--)' % var, body) or
               re.search(r'(\+\+|--)\s*%s\b' % var, body))
        if bad:
            pos = m.end(); continue
        out = ''.join(re.sub(r'\b%s\b' % var, str(k), body) for k in trips)
        src = src[:m.start()] + out + src[end.end():]
        pos = m.start() + len(out)
        done += 1
    return src, done


def _code_lines(body):
    """A stage's source without comments and preprocessor lines (the macro blocks of Unity 2019+
    define layout qualifiers the player switches by device: not code the stage necessarily uses)."""
    return re.sub(r'^[ \t]*#[^\n]*', '', body, flags=re.M)


def stage_to_es(src, stage, precision='highp', optimize=OPTIMIZE, notes=None):
    """One stage's source (starting with its #version line) -> GLSL ES 3.00."""
    body = _strip_comments(src)
    code = _code_lines(body)
    for rx, what in _UNSUPPORTED:
        m = rx.search(code)
        if m:
            raise Unsupported(f'{stage}: {what} {m.group(0).strip()!r}')
    lines = src.splitlines(keepends=True)
    out = []
    i = 0
    while i < len(lines) and not lines[i].strip():
        i += 1
    m = re.match(r'\s*#\s*version\s+(\d+)\s*(\w*)', lines[i]) if i < len(lines) else None
    if not m:
        raise Unsupported(f'{stage}: no #version line')
    if m.group(1) not in VERSIONS or m.group(2):
        raise Unsupported(f'{stage}: #version {m.group(1)} {m.group(2)}'.rstrip())
    out.append('#version 300 es\n')
    i += 1
    # the extension directives right after #version (they must precede any declaration); Unity
    # 2019+ wraps the optional ones: #ifdef GL_ARB_shader_bit_encoding / #extension ... / #endif
    while i < len(lines):
        s = lines[i].strip()
        e = re.match(r'#\s*extension\s+(\w+)\s*:\s*(\w+)', s)
        w = re.match(r'#\s*ifdef\s+(\w+)$', s)
        if e:
            if e.group(1) not in DROP_EXTENSIONS:
                out.append(lines[i])
            i += 1
        elif w and i + 2 < len(lines) and re.match(r'#\s*extension\s+%s\s*:' % w.group(1), lines[i + 1].strip()) \
                and re.match(r'#\s*endif\b', lines[i + 2].strip()):
            if w.group(1) not in DROP_EXTENSIONS:
                out += lines[i:i + 3]
            i += 3
        elif not s:
            i += 1
        else:
            break
    out.append(precision_block(body, precision))
    rest = ''.join(lines[i:])
    if 'const-arrays' in optimize:
        rest, n = const_arrays(rest)
        if n and notes is not None: notes.append(f'{stage} const-arrays {n}')
    if 'unroll' in optimize:
        rest, n = unroll(rest)
        if n and notes is not None: notes.append(f'{stage} unroll {n}')
    out.append(rest)
    return ''.join(out)


def precision_block(body, precision='highp'):
    """Default precision statements for one stage (body: its source without comments)."""
    p = ['precision highp float;\n', 'precision highp int;\n']
    used = [t for t in SAMPLER_TYPES if re.search(r'\b%s\b' % t, body)]
    for t in used:
        if precision == 'highp' or t not in ('sampler2D', 'samplerCube'):
            p.append(f'precision highp {t};\n')      # (types other than these two have no default in ES)
    return ''.join(p)


def program_to_es(code, precision='highp', optimize=OPTIMIZE, notes=None):
    """A whole GL program (every stage block) -> its GLSL ES 3.00 twin. Text outside the stage
    blocks (none in HLSLcc output) is kept. notes: a list that gets the rewrites applied."""
    blocks = stage_blocks(code)
    if not blocks:
        raise Unsupported('no stage blocks')
    out = []
    last = 0
    for stage, a, b in blocks:
        if stage not in ('VERTEX', 'FRAGMENT'):
            raise Unsupported(f'{stage} stage')
        out.append(code[last:a])
        out.append(stage_to_es(code[a:b], stage, precision, optimize, notes))
        last = b
    out.append(code[last:])
    return ''.join(out)


def stage_sources(code):
    """{stage: source} as Unity hands each stage to the driver (for compile checks)."""
    return {stage: code[a:b] for stage, a, b in stage_blocks(code)}
