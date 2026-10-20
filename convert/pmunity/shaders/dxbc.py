"""Direct3D 11 programs (DXBC) of Unity Shader assets -> GL programs, with Unity's HLSLcc.

A Windows build's shaders carry only Direct3D 11 programs (platform blob 4). One D3D11 blob
entry (blob.Entry) holds one stage:

  code   a 6-byte Unity header, then the DXBC container (fxc's output). Header: 1, then counts of
         the slots Unity's D3D11 renderer binds (textures, constant buffers, samplers, 2 more);
         kept as it is, not needed for GL.
  rest   Unity's binding info. Unity strips the container's reflection chunk (RDEF: constant
         buffer layouts, resource names) in builds and keeps what it needs here instead:

           int32 source map                       vertex channels read (bit per ShaderChannel)
           int32 n; n x (int32 source, int32 target)                 vertex inputs
           int32 n; n x constant buffer           the first one (name '', size 0) holds the
                                                  globals outside any cbuffer (none in D3D11)
               string name; int32 size
               int32 n; n x (string name, int32 type (0 float, 1 int, 2 bool), int32 rows,
                             int32 columns, int32 is matrix, int32 array size, int32 offset)
               int32 n; n x struct member         (none in 2017.4 data seen: refused)
           int32 n; n x binding: string name, int32 kind, int32 index, then by kind:
               kind 0 texture:          int32 dimension (2 2D, 3 3D, 4 cube, 5 2D array,
                                        6 cube array), int32 (0 in all data seen)
               kind 1 constant buffer:  int32 (0 in all data seen)
               other kinds (buffers, UAVs) are refused
         (strings: int32 length, bytes, padded to 4). Checked by writing every entry back byte for
         byte (Bindings.serialize).

Only the names and constant buffer layouts are missing from the bytecode; registers, resource
types and sampler modes come from the declarations in the SHDR/SHEX chunk. with_reflection()
puts both back together as an RDEF chunk in the form HLSLcc reads (D3D11_SHADER_DESC tables:
resource bindings, constant buffers, variables, types, strings), so HLSLcc names every uniform,
texture and constant buffer as Unity's own GL programs do.

The stages are then translated by pmhlslcc (tools/hlslcc: HLSLcc with a small front end) the way
Unity's shader compiler does for OpenGL Core: GLSL 1.50, matrices as vec4 arrays
(hlslcc_mtx4x4<name>), vertex inputs in_<SEMANTIC><n>, varyings vs_<SEMANTIC><n>, outputs
SV_Target<n>, a GLSL sampler per texture named as the texture, every constant buffer member as a
plain uniform (Unity's GL backends bind uniforms by name). gl_program() joins the stages as
Unity stores a GL program (#ifdef VERTEX / #ifdef FRAGMENT blocks); glsl.py turns it into GLSL ES
3.00 as for a macOS/Linux build.
"""
import os
import re
import shutil
import struct
import subprocess


class Unsupported(Exception):
    """A program that this path cannot give a GL twin (the message says why)."""


# GPU program types (ShaderGpuProgramType, Unity 5.5 - 2018.x)
GPU_DX11_VS40, GPU_DX11_VS50, GPU_DX11_PS40, GPU_DX11_PS50 = 15, 16, 17, 18
GPU_DX11_GS40, GPU_DX11_GS50, GPU_DX11_HS50, GPU_DX11_DS50 = 19, 20, 21, 22
VERTEX_TYPES = (GPU_DX11_VS40, GPU_DX11_VS50)
PIXEL_TYPES = (GPU_DX11_PS40, GPU_DX11_PS50)
OTHER_STAGE_TYPES = (GPU_DX11_GS40, GPU_DX11_GS50, GPU_DX11_HS50, GPU_DX11_DS50)

HEADER_SIZE = 6

# HLSLcc flags (include/hlslcc.h) for Unity's OpenGL Core style: constant buffer members as plain
# uniforms (no uniform blocks, no struct around $Globals), matrices as vec4 arrays
HLSLCC_FLAG_DISABLE_GLOBALS_STRUCT = 0x800
HLSLCC_FLAG_TRANSLATE_MATRICES = 0x20000
FLAGS = HLSLCC_FLAG_TRANSLATE_MATRICES | HLSLCC_FLAG_DISABLE_GLOBALS_STRUCT
LANG = '150'


def _pad4(n):
    return (n + 3) & ~3


# ---------------------------------------------------------------- Unity's binding info

class _Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def i32(self):
        v, = struct.unpack_from('<i', self.d, self.p)
        self.p += 4
        return v

    def count(self, limit=4096):
        n = self.i32()
        if not 0 <= n <= limit:
            raise ValueError(f'count {n}')
        return n

    def string(self):
        n = self.count(1024)
        if self.p + n > len(self.d):
            raise ValueError('string past the end')
        s = bytes(self.d[self.p:self.p + n]).decode('ascii')
        self.p = _pad4(self.p + n)
        return s


def _wstr(s):
    b = s.encode('ascii')
    return struct.pack('<i', len(b)) + b + b'\0' * (_pad4(len(b)) - len(b))


class Param:
    __slots__ = ('name', 'type', 'rows', 'cols', 'is_matrix', 'array', 'offset')

    def __init__(self, name, type, rows, cols, is_matrix, array, offset):
        self.name, self.type, self.rows, self.cols = name, type, rows, cols
        self.is_matrix, self.array, self.offset = is_matrix, array, offset

    def __repr__(self):
        return f'{self.name}@{self.offset}'


class ConstantBuffer:
    __slots__ = ('name', 'size', 'params', 'structs')

    def __init__(self, name, size, params, structs=0):
        self.name, self.size, self.params, self.structs = name, size, params, structs


class Binding:
    KIND_TEXTURE, KIND_CBUFFER = 0, 1
    EXTRA = {KIND_TEXTURE: 2, KIND_CBUFFER: 1}          # ints after the index, by kind
    __slots__ = ('name', 'kind', 'index', 'extra')

    def __init__(self, name, kind, index, extra):
        self.name, self.kind, self.index, self.extra = name, kind, index, list(extra)

    def __repr__(self):
        return f'{self.name}:{self.kind}/{self.index}'


class Bindings:
    """Unity's binding info of one D3D11 entry (the bytes after its code)."""

    def __init__(self, source_map, channels, cbuffers, bindings):
        self.source_map, self.channels = source_map, channels
        self.cbuffers, self.bindings = cbuffers, bindings

    @classmethod
    def parse(cls, data):
        r = _Reader(data)
        try:
            source_map = r.i32()
            channels = [(r.i32(), r.i32()) for _ in range(r.count(64))]
            cbs = []
            for _ in range(r.count(256)):
                name, size = r.string(), r.i32()
                params = [Param(r.string(), *(r.i32() for _ in range(6))) for _ in range(r.count())]
                structs = r.count()
                if structs:
                    raise Unsupported(f'constant buffer {name!r} has struct members (instancing data): layout not known')
                cbs.append(ConstantBuffer(name, size, params, structs))
            binds = []
            for _ in range(r.count(256)):
                name, kind, index = r.string(), r.i32(), r.i32()
                if kind not in Binding.EXTRA:
                    raise Unsupported(f'binding {name!r} of kind {kind} (buffer or UAV?): not supported')
                binds.append(Binding(name, kind, index, [r.i32() for _ in range(Binding.EXTRA[kind])]))
        except (struct.error, ValueError, UnicodeDecodeError) as ex:
            raise Unsupported(f'binding info does not match the known layout ({ex})')
        b = cls(source_map, channels, cbs, binds)
        if r.p != len(data) or b.serialize() != bytes(data):
            raise Unsupported('binding info does not match the known layout (round trip)')
        return b

    def serialize(self):
        out = bytearray(struct.pack('<ii', self.source_map, len(self.channels)))
        for s, t in self.channels:
            out += struct.pack('<ii', s, t)
        out += struct.pack('<i', len(self.cbuffers))
        for cb in self.cbuffers:
            out += _wstr(cb.name) + struct.pack('<ii', cb.size, len(cb.params))
            for p in cb.params:
                out += _wstr(p.name) + struct.pack('<6i', p.type, p.rows, p.cols, p.is_matrix, p.array, p.offset)
            out += struct.pack('<i', cb.structs)
        out += struct.pack('<i', len(self.bindings))
        for b in self.bindings:
            out += _wstr(b.name) + struct.pack('<ii', b.kind, b.index) + struct.pack('<%di' % len(b.extra), *b.extra)
        return bytes(out)


def check_parsed_form(bindings, sp, names):
    """Differences between an entry's binding info and the SerializedSubProgram that points to it
    (m_ParsedForm: constant buffers, their bindings, textures; names: {name index: name} of the
    pass). The two are written by Unity from the same compile, so any difference means the
    binding info was misread (another Unity version's layout): [] when they agree."""
    out = []
    cbs = {cb.name: cb for cb in bindings.cbuffers}
    have = {(b.name, b.index) for b in bindings.bindings if b.kind == Binding.KIND_CBUFFER}
    for b in sp.get('m_ConstantBufferBindings') or []:
        if (names.get(b['m_NameIndex']), b['m_Index']) not in have:
            out.append(f'constant buffer binding {names.get(b["m_NameIndex"])}/{b["m_Index"]}')
    for cb in sp.get('m_ConstantBuffers') or []:
        mine = cbs.get(names.get(cb['m_NameIndex']))
        if mine is None or mine.size != cb.get('m_Size', mine.size):
            out.append(f'constant buffer {names.get(cb["m_NameIndex"])}')
            continue
        offs = {p.name: p.offset for p in mine.params}
        for p in (cb.get('m_VectorParams') or []) + (cb.get('m_MatrixParams') or []):
            if offs.get(names.get(p['m_NameIndex'])) != p['m_Index']:
                out.append(f'constant {names.get(p["m_NameIndex"])} in {mine.name}')
    tex = {(b.name, b.index, b.extra[0]) for b in bindings.bindings if b.kind == Binding.KIND_TEXTURE}
    for t in sp.get('m_TextureParams') or []:
        if (names.get(t['m_NameIndex']), t['m_Index'], t['m_Dim']) not in tex:
            out.append(f'texture {names.get(t["m_NameIndex"])}/{t["m_Index"]}')
    return out


def gl_rest(source_map):
    """The binding info of a GL entry: Unity's GL backends find uniforms, textures and vertex
    inputs by name in the linked program, so it is only the source map (the same in every one of
    the 5782 GLCore entries of Cuphead and the GLES3 ones of Unity's own Android resources)."""
    return struct.pack('<8i', source_map, 0, 1, 0, 0, 0, 0, 0)


# ---------------------------------------------------------------- the DXBC container

def split_code(code):
    """A D3D11 entry's code -> (Unity's 6-byte header, the DXBC container)."""
    if len(code) < HEADER_SIZE + 32 or code[HEADER_SIZE:HEADER_SIZE + 4] != b'DXBC':
        raise Unsupported('not a D3D11 program (no DXBC container after the header)')
    size, = struct.unpack_from('<I', code, HEADER_SIZE + 24)
    if size != len(code) - HEADER_SIZE:
        raise Unsupported(f'DXBC container size {size} != {len(code) - HEADER_SIZE}')
    return bytes(code[:HEADER_SIZE]), bytes(code[HEADER_SIZE:])


def is_d3d11_code(code):
    return len(code) >= HEADER_SIZE + 32 and code[HEADER_SIZE:HEADER_SIZE + 4] == b'DXBC'


def chunks(dxbc):
    """[(fourcc, data)] of a DXBC container."""
    n, = struct.unpack_from('<I', dxbc, 28)
    out = []
    for i in range(n):
        off, = struct.unpack_from('<I', dxbc, 32 + 4 * i)
        fourcc = dxbc[off:off + 4].decode('ascii', 'replace')
        size, = struct.unpack_from('<I', dxbc, off + 4)
        out.append((fourcc, dxbc[off + 8:off + 8 + size]))
    return out


def build_container(chs):
    """[(fourcc, data)] -> DXBC container. The checksum is left zero (HLSLcc does not check it)."""
    head = 32 + 4 * len(chs)
    body = bytearray()
    offs = []
    for fourcc, data in chs:
        offs.append(head + len(body))
        body += fourcc.encode('ascii') + struct.pack('<I', len(data)) + data
        body += b'\0' * (_pad4(len(body)) - len(body))
    total = head + len(body)
    out = bytearray(b'DXBC' + b'\0' * 16 + struct.pack('<III', 1, total, len(chs)))
    for o in offs:
        out += struct.pack('<I', o)
    return bytes(out + body)


# ---------------------------------------------------------------- SHDR / SHEX declarations

STAGE_NAMES = {0: 'pixel', 1: 'vertex', 2: 'geometry', 3: 'hull', 4: 'domain', 5: 'compute'}
_OP_CUSTOMDATA, _OP_DCL_RESOURCE, _OP_DCL_CB, _OP_DCL_SAMPLER = 53, 88, 89, 90
_OP_BUFFERS = {156: 'UAV', 157: 'raw UAV', 158: 'structured UAV', 159: 'group shared memory',
               160: 'group shared memory', 161: 'raw buffer', 162: 'structured buffer'}
# RESOURCE_DIMENSION (bytecode) -> (D3D_SRV_DIMENSION for RDEF, Unity's TextureDimension or None)
_DIMS = {1: (1, None), 2: (2, None), 3: (4, 2), 4: (6, 2), 5: (8, 3), 6: (9, 4), 7: (3, None),
         8: (5, 5), 9: (7, 5), 10: (10, 6)}
_DIM_NAMES = {1: 'buffer', 2: '1D', 3: '2D', 4: '2D MS', 5: '3D', 6: 'cube', 7: '1D array', 8: '2D array',
              9: '2D MS array', 10: 'cube array'}


class Program:
    """What the bytecode declares: version, stage, registers of constant buffers (with their
    size in vec4s), textures (dimension, return type) and samplers (mode)."""

    def __init__(self, shdr):
        w = struct.unpack('<%dI' % (len(shdr) // 4), shdr[:len(shdr) // 4 * 4])
        if len(w) < 2:
            raise Unsupported('empty shader code chunk')
        self.minor, self.major, self.stage = w[0] & 0xf, (w[0] >> 4) & 0xf, w[0] >> 16
        self.cbuffers, self.textures, self.samplers = {}, {}, {}
        self.buffers = []               # other resource declarations (UAVs, buffers): their names
        end = min(w[1], len(w))
        i = 2
        while i < end:
            tok = w[i]
            op = tok & 0x7ff
            n = w[i + 1] if op == _OP_CUSTOMDATA else (tok >> 24) & 0x7f
            if n == 0 or i + n > end:
                raise Unsupported('shader code chunk: bad instruction length')
            if op == _OP_DCL_CB:
                idx, _ = self._operand(w, i + 1)
                self.cbuffers[idx[0]] = idx[1] if len(idx) > 1 else 0
            elif op == _OP_DCL_RESOURCE:
                idx, p = self._operand(w, i + 1)
                self.textures[idx[0]] = ((tok >> 11) & 0x1f, w[p] & 0xf)
            elif op == _OP_DCL_SAMPLER:
                idx, _ = self._operand(w, i + 1)
                self.samplers[idx[0]] = (tok >> 11) & 0xf          # 0 default, 1 comparison, 2 mono
            elif op in _OP_BUFFERS:
                self.buffers.append(_OP_BUFFERS[op])
            i += n

    def _operand(self, w, p):
        """Immediate register indices of a declaration's operand; position after it."""
        tok = w[p]
        p += 1
        if tok >> 31:                   # extended operand token
            p += 1
        idx = []
        for k in range((tok >> 20) & 3):
            rep = (tok >> (22 + 3 * k)) & 7
            if rep != 0:
                raise Unsupported('declaration with a relative register index')
            idx.append(w[p])
            p += 1
        if self.major == 5 and self.minor == 1:
            raise Unsupported('shader model 5.1 (register spaces)')
        return idx, p


_RDEF_PROGRAM_TYPES = {0: 0xffff, 1: 0xfffe, 2: 0x4753, 3: 0x4853, 4: 0x4453, 5: 0x4353}


def _var_layout(p):
    """(D3D_SHADER_VARIABLE_CLASS, D3D_SHADER_VARIABLE_TYPE, size in bytes) of a Unity param."""
    types = {0: 3, 1: 2, 2: 1}                       # float, int, bool
    if p.type not in types:
        raise Unsupported(f'constant {p.name!r} of Unity type {p.type}')
    if p.is_matrix:
        cls, regs, last = 3, p.cols, p.rows * 4      # column_major (HLSL's default): a register per column
    else:
        cls, regs, last = (0 if p.cols == 1 and p.rows == 1 else 1), p.rows, p.cols * 4
    one = (regs - 1) * 16 + last
    size = one if p.array <= 1 else (p.array - 1) * regs * 16 + one
    return cls, types[p.type], size


def _rdef(prog, cbs, textures, samplers, creator='pmunity'):
    """An RDEF chunk as HLSLcc reads it. cbs: [(register, ConstantBuffer)], textures:
    [(register, name)], samplers: [(register, name)]."""
    sm5 = prog.major >= 5
    var_words = 10 if sm5 else 6
    strings = bytearray()
    str_at = {}

    def s(text):
        if text not in str_at:
            str_at[text] = len(strings)
            strings.extend(text.encode('ascii') + b'\0')
        return str_at[text]

    binds = []          # (name, type, return type, dimension, samples, bind point, count, flags)
    for reg, name in samplers:
        binds.append((name, 3, 0, 0, 0, reg, 1, 2 if prog.samplers[reg] == 1 else 0))
    for reg, name in textures:
        dim, ret = prog.textures[reg]
        ms = dim in (4, 9)
        binds.append((name, 2, ret, _DIMS[dim][0], 0 if ms else 0xffffffff, reg, 1, 0xc))
    for reg, cb in cbs:
        binds.append((cb.name, 0, 0, 0, 0, reg, 1, 0))
    nvars = sum(len(cb.params) for _, cb in cbs)
    head = 7 * 4
    bind_off = head
    cb_off = bind_off + 32 * len(binds)
    var_off = cb_off + 24 * len(cbs)
    type_off = var_off + 4 * var_words * nvars
    str_base = type_off + 16 * nvars
    target = prog.minor | (prog.major << 8) | (_RDEF_PROGRAM_TYPES.get(prog.stage, 0) << 16)
    out = bytearray(struct.pack('<7I', len(cbs), cb_off, len(binds), bind_off, target, 0, str_base + s(creator)))
    for name, *rest in binds:
        out += struct.pack('<8I', str_base + s(name), *rest)
    v = 0
    for _, cb in cbs:
        out += struct.pack('<6I', str_base + s(cb.name), len(cb.params), var_off + 4 * var_words * v, cb.size, 0, 0)
        v += len(cb.params)
    types = bytearray()
    v = 0
    for _, cb in cbs:
        for p in cb.params:
            cls, typ, size = _var_layout(p)
            out += struct.pack('<6I', str_base + s(p.name), p.offset, size, 2, type_off + 16 * v, 0)
            if sm5:
                out += struct.pack('<4I', 0xffffffff, 0, 0xffffffff, 0)
            types += struct.pack('<6HI', cls, typ, p.rows, p.cols, p.array if p.array > 1 else 0, 0, 0)
            v += 1
    out += types
    assert len(out) == str_base
    return bytes(out + strings)


def with_reflection(dxbc, bindings):
    """The DXBC container with an RDEF chunk rebuilt from Unity's binding info (an existing one is
    replaced). Raises Unsupported when the bytecode declares something the binding info does not
    name, or what this path does not handle."""
    chs = [(f, d) for f, d in chunks(dxbc) if f != 'RDEF']
    code = [d for f, d in chs if f in ('SHDR', 'SHEX')]
    if not code:
        raise Unsupported('no shader code chunk')
    prog = Program(code[0])
    if prog.buffers:
        raise Unsupported(f'{STAGE_NAMES.get(prog.stage)} shader uses {", ".join(sorted(set(prog.buffers)))}')
    by_name = {cb.name: cb for cb in bindings.cbuffers}
    cb_regs = {b.index: b.name for b in bindings.bindings if b.kind == Binding.KIND_CBUFFER}
    tex_regs = {b.index: b for b in bindings.bindings if b.kind == Binding.KIND_TEXTURE}
    cbs = []
    for reg in sorted(prog.cbuffers):
        name = cb_regs.get(reg)
        if name is None or name not in by_name:
            raise Unsupported(f'constant buffer cb{reg} is not named in the binding info')
        cbs.append((reg, by_name[name]))
    textures = []
    for reg in sorted(prog.textures):
        b = tex_regs.get(reg)
        if b is None:
            raise Unsupported(f'texture t{reg} is not named in the binding info')
        dim, ret = prog.textures[reg]
        if dim not in _DIMS:
            raise Unsupported(f'texture {b.name!r}: resource dimension {dim}')
        if _DIMS[dim][1] is not None and b.extra[0] != _DIMS[dim][1]:
            raise Unsupported(f'texture {b.name!r}: declared {_DIM_NAMES[dim]} but Unity says dimension {b.extra[0]}')
        textures.append((reg, b.name))
    names = dict(textures)
    samplers = [(reg, 'sampler' + names[reg] if reg in names else f'sampler{reg}') for reg in sorted(prog.samplers)]
    rdef = _rdef(prog, cbs, textures, samplers)
    return build_container([('RDEF', rdef)] + chs), prog


# ---------------------------------------------------------------- HLSLcc (pmhlslcc)

def find_hlslcc():
    """$PMHLSLCC, else tools/hlslcc/out/pmhlslcc of this repository, else pmhlslcc on PATH."""
    p = os.environ.get('PMHLSLCC')
    if p:
        return p
    here = os.path.dirname(os.path.abspath(__file__))
    p = os.path.normpath(os.path.join(here, '..', '..', '..', 'tools', 'hlslcc', 'out', 'pmhlslcc'))
    if os.path.exists(p):
        return p
    return shutil.which('pmhlslcc')


class Translator:
    """A pmhlslcc server process (restarted if HLSLcc crashes on a program)."""

    def __init__(self, exe=None, lang=LANG, flags=FLAGS):
        self.exe = exe or find_hlslcc()
        if not self.exe or not os.path.exists(self.exe):
            raise FileNotFoundError('pmhlslcc not found: build it with tools/hlslcc/build.sh or set PMHLSLCC')
        self.lang, self.flags = lang, flags
        self.proc = None
        self.crashes = 0

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def close(self):
        if self.proc:
            try:
                self.proc.stdin.write(b'0 0 x\n')
                self.proc.stdin.close()
            except OSError:
                pass
            self.proc.wait()
            self.proc.stdout.close()
            self.proc = None

    def translate(self, stages):
        """[DXBC container] -> [GLSL text] (same order). Raises Unsupported with HLSLcc's log."""
        if self.proc is None:
            self.proc = subprocess.Popen([self.exe, '-s'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        head = f'{len(stages)} {self.flags:#x} {self.lang}' + ''.join(f' {len(s)}' for s in stages) + '\n'
        try:
            self.proc.stdin.write(head.encode() + b''.join(stages))
            self.proc.stdin.flush()
            line = self.proc.stdout.readline().split()
            if not line:
                raise BrokenPipeError
            status, n = int(line[0]), int(line[1])
            lens = [int(x) for x in line[2:]]
            texts = [self.proc.stdout.read(k).decode('utf-8') for k in lens[:n]]
            log = self.proc.stdout.read(lens[n]).decode('utf-8', 'replace')
        except (BrokenPipeError, ValueError, IndexError):
            self.proc.kill()
            self.proc.wait()
            for f in (self.proc.stdin, self.proc.stdout):
                try:
                    f.close()
                except OSError:
                    pass
            self.proc = None
            self.crashes += 1
            raise Unsupported('HLSLcc crashed on this program')
        if status:
            raise Unsupported('HLSLcc: ' + ('; '.join(l for l in log.splitlines() if 'error' in l) or log.strip() or 'failed'))
        return texts


_DEFINE_BLOCKS = (
    '#define HLSLCC_ENABLE_UNIFORM_BUFFERS 1\n#if HLSLCC_ENABLE_UNIFORM_BUFFERS\n#define UNITY_UNIFORM\n'
    '#else\n#define UNITY_UNIFORM uniform\n#endif\n',
    '#define UNITY_SUPPORTS_UNIFORM_LOCATION 1\n#if UNITY_SUPPORTS_UNIFORM_LOCATION\n'
    '#define UNITY_LOCATION(x) layout(location = x)\n#define UNITY_BINDING(x) layout(binding = x, std140)\n'
    '#else\n#define UNITY_LOCATION(x)\n#define UNITY_BINDING(x) layout(std140)\n#endif\n',
)


def tidy(glsl):
    """HLSLcc's GLSL as Unity 2017-2018 stored it. Newer HLSLcc wraps each optional extension in
    `#ifdef GL_X` (written plainly here, as before: `enable` only warns on a driver without it) and
    writes two macro blocks that later Unity players switch off at load time (uniform buffers,
    explicit uniform locations); they are removed and any use of their macros written out as
    switched off, as those players do on a driver without the feature."""
    glsl = re.sub(r'^#ifdef (\w+)\n(#extension \1 : enable\n)#endif\n', r'\2', glsl, flags=re.M)
    for b in _DEFINE_BLOCKS:
        glsl = glsl.replace(b, '')
    glsl = re.sub(r'\bUNITY_LOCATION\(\s*\d+\s*\)\s*', '', glsl)
    glsl = re.sub(r'\bUNITY_BINDING\(\s*\d+\s*\)', 'layout(std140)', glsl)
    return re.sub(r'\bUNITY_UNIFORM\b', 'uniform', glsl)


def gl_program(stages):
    """{'VERTEX': glsl, 'FRAGMENT': glsl, ...} -> one GL program as Unity stores it."""
    out = []
    for st in ('VERTEX', 'HULL', 'DOMAIN', 'GEOMETRY', 'FRAGMENT'):
        if st in stages:
            out.append(f'#ifdef {st}\n{tidy(stages[st])}\n#endif\n')
    return ''.join(out)


def translate_program(translator, vs, ps):
    """vs, ps: (code, rest) of the D3D11 vertex and pixel entries -> GL program text (GLSL 1.50,
    as Unity's GLCore programs). Raises Unsupported."""
    stages = []
    for code, rest in (vs, ps):
        _, dxbc = split_code(code)
        cont, prog = with_reflection(dxbc, Bindings.parse(rest))
        stages.append((prog, cont))
    if [p.stage for p, _ in stages] != [1, 0]:
        raise Unsupported('expected a vertex and a pixel shader, got ' + ', '.join(STAGE_NAMES.get(p.stage, '?') for p, _ in stages))
    vs_glsl, ps_glsl = translator.translate([c for _, c in stages])
    return gl_program({'VERTEX': vs_glsl, 'FRAGMENT': ps_glsl})
