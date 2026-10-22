"""Unit tests of the Direct3D 11 path (dxbc.py, convert.add_gles3_d3d11) on hand-assembled
programs (no Unity or game files): python -m unittest discover -s tests   (from convert/)

The translation tests run pmhlslcc (tools/hlslcc/build.sh) and are skipped without it."""
import os
import struct
import sys
import unittest

import lz4.block

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import dxbc as D
from pmunity.shaders import glsl as G


# ---------------------------------------------------------------- a tiny SM4 assembler

def op(code, words=(), ctl=0):
    return [code | (ctl << 11) | ((1 + len(words)) << 24)] + list(words)


def dst(rtype, idx, mask=0xf):                          # register with a write mask, 1D index
    return [2 | (mask << 4) | (rtype << 12) | (1 << 20), idx]


def src(rtype, idx, swz='xyzw'):                       # register with a swizzle, 1D index
    s = sum('xyzw'.index(c) << (2 * i) for i, c in enumerate(swz))
    return [2 | (1 << 2) | (s << 4) | (rtype << 12) | (1 << 20), idx]


def cb(reg, off, swz='xyzw'):                          # cb<reg>[<off>]
    s = sum('xyzw'.index(c) << (2 * i) for i, c in enumerate(swz))
    return [2 | (1 << 2) | (s << 4) | (8 << 12) | (2 << 20), reg, off]


TEMP, INPUT, OUTPUT, SAMPLER, RESOURCE = 0, 1, 2, 6, 7
MAD, MOV, MUL, RET, SAMPLE = 50, 54, 56, 62, 69


def dcl_cb(reg, size):
    return op(89, [0x00208e46, reg, size])


def dcl_tex2d(reg):
    return op(88, [0x00107000, reg, 0x5555], ctl=3)     # texture2d, float return type


def dcl_sampler(reg):
    return op(90, [0x00106000, reg])


def shdr(stage, decls, code, version=0x40):
    body = [w for i in decls + code for w in i]
    return struct.pack('<%dI' % (2 + len(body)), (stage << 16) | version, 2 + len(body), *body)


def as_sm5(chs):
    """The same program as shader model 5.0 (SHEX; RDEF variables have 4 more words)."""
    out = []
    for f, d in chs:
        if f == 'SHDR':
            f, d = 'SHEX', struct.pack('<I', (struct.unpack_from('<I', d)[0] & ~0xff) | 0x50) + d[4:]
        out.append((f, d))
    return out


def signature(elems):
    """[(semantic, index, system value, register, mask, rw mask)] -> ISGN/OSGN chunk data."""
    head = 8 + 24 * len(elems)
    strings, offs = b'', []
    for e in elems:
        offs.append(head + len(strings))
        strings += e[0].encode() + b'\0'
    out = struct.pack('<II', len(elems), 8)
    for e, o in zip(elems, offs):
        out += struct.pack('<6I', o, e[1], e[2], 3, e[3], e[4] | (e[5] << 8))
    return out + strings + b'\0' * (-len(strings) % 4)


def unity_code(chs, counts=(0, 0, 0)):
    """Unity's D3D11 program code: its 6-byte header + the container (no RDEF, as in builds)."""
    return bytes([1, counts[0], counts[1], counts[2], 0, 0]) + D.build_container(chs)


VS_DXBC = [
    ('ISGN', signature([('POSITION', 0, 0, 0, 0xf, 0xf), ('TEXCOORD', 0, 0, 1, 0x3, 0x3)])),
    ('OSGN', signature([('SV_POSITION', 0, 1, 0, 0xf, 0), ('TEXCOORD', 0, 0, 1, 0x3, 0xc)])),
    ('SHDR', shdr(1, [op(106, ctl=1), dcl_cb(0, 5), op(95, [0x001010f2, 0]), op(95, [0x00101032, 1]),
                      op(103, [0x001020f2, 0, 1]), op(101, [0x00102032, 1]), op(104, [1])],
                  [op(MUL, dst(TEMP, 0) + src(INPUT, 0, 'yyyy') + cb(0, 1)),
                   op(MAD, dst(TEMP, 0) + cb(0, 0) + src(INPUT, 0, 'xxxx') + src(TEMP, 0)),
                   op(MAD, dst(TEMP, 0) + cb(0, 2) + src(INPUT, 0, 'zzzz') + src(TEMP, 0)),
                   op(MAD, dst(OUTPUT, 0) + cb(0, 3) + src(INPUT, 0, 'wwww') + src(TEMP, 0)),
                   op(MOV, dst(OUTPUT, 1, 0x3) + src(INPUT, 1, 'xyxx')),
                   op(RET)])),
]
PS_DXBC = [
    ('ISGN', signature([('SV_POSITION', 0, 1, 0, 0xf, 0), ('TEXCOORD', 0, 0, 1, 0x3, 0x3)])),
    ('OSGN', signature([('SV_Target', 0, 0, 0, 0xf, 0)])),
    ('SHDR', shdr(0, [op(106, ctl=1), dcl_cb(0, 5), dcl_sampler(0), dcl_sampler(1), dcl_tex2d(0), dcl_tex2d(1),
                      op(98, [0x00101032, 1], ctl=2), op(101, [0x001020f2, 0]), op(104, [2])],
                  [op(SAMPLE, dst(TEMP, 0) + src(INPUT, 1, 'xyxx') + src(RESOURCE, 0) + [0x00106000, 0]),
                   op(SAMPLE, dst(TEMP, 1) + src(INPUT, 1, 'xyxx') + src(RESOURCE, 1) + [0x00106000, 1]),
                   op(MUL, dst(TEMP, 0) + src(TEMP, 0) + src(TEMP, 1, 'wwww')),
                   op(MUL, dst(OUTPUT, 0) + src(TEMP, 0) + cb(0, 4)),
                   op(RET)])),
]
# a 3-tap blur as a game would write it: weights in an immediate constant buffer, a loop, sample_l,
# alpha test with discard
LOOP, ENDLOOP, BREAKC, IADD, IGE, DISCARD, SAMPLE_L, LT, ADD = 48, 22, 3, 30, 33, 13, 72, 49, 0
NONZERO = 1 << 7                                       # instruction test bit (opcode bit 18)


def imm(*v):                                           # l(...) float literal(s)
    return [(1 if len(v) == 1 else 2) | (4 << 12)] + [struct.unpack('<I', struct.pack('<f', x))[0] for x in v]


def immi(v):                                           # l(<int>)
    return [1 | (4 << 12), v]


def one(rtype, idx, c):                                # register.<c> (select 1)
    return [2 | (2 << 2) | ('xyzw'.index(c) << 4) | (rtype << 12) | (1 << 20), idx]


BLUR_PS_DXBC = [
    ('ISGN', signature([('SV_POSITION', 0, 1, 0, 0xf, 0), ('TEXCOORD', 0, 0, 1, 0x3, 0x3)])),
    ('OSGN', signature([('SV_Target', 0, 0, 0, 0xf, 0)])),
    ('SHDR', shdr(0, [op(106, ctl=1),
                      [53 | (3 << 11), 2 + 12] + imm(0.25, 0, 0, 0)[1:] + imm(0.5, 0, 0, 0)[1:] + imm(0.25, 0, 0, 0)[1:],
                      dcl_cb(0, 1), dcl_sampler(0), dcl_tex2d(0), op(98, [0x00101032, 1], ctl=2),
                      op(101, [0x001020f2, 0]), op(104, [4])],
                  [op(MOV, dst(TEMP, 0) + imm(0, 0, 0, 0)),
                   op(MOV, dst(TEMP, 1, 0x3) + src(INPUT, 1, 'xyxx')),
                   op(MOV, dst(TEMP, 1, 0x4) + immi(0)),
                   op(LOOP),
                   op(IGE, dst(TEMP, 1, 0x8) + one(TEMP, 1, 'z') + immi(3)),
                   op(BREAKC, one(TEMP, 1, 'w'), ctl=NONZERO),
                   op(SAMPLE_L, dst(TEMP, 2) + src(TEMP, 1, 'xyxx') + src(RESOURCE, 0) + [0x00106000, 0] + imm(0.0)),
                   op(MAD, dst(TEMP, 0) + src(TEMP, 2) + [2 | (1 << 2) | (9 << 12) | (1 << 20) | (3 << 22), 0] + one(TEMP, 1, 'z')
                      + src(TEMP, 0)),
                   op(ADD, dst(TEMP, 1, 0x1) + src(TEMP, 1, 'xxxx') + cb(0, 0, 'xxxx')),
                   op(IADD, dst(TEMP, 1, 0x4) + one(TEMP, 1, 'z') + immi(1)),
                   op(ENDLOOP),
                   op(LT, dst(TEMP, 3, 0x1) + one(TEMP, 0, 'w') + imm(0.5)),
                   op(DISCARD, one(TEMP, 3, 'x'), ctl=NONZERO),
                   op(MOV, dst(OUTPUT, 0) + src(TEMP, 0)),
                   op(RET)])),
]
BLUR_PS_BIND = D.Bindings(0, [], [D.ConstantBuffer('', 0, []), D.ConstantBuffer('$Globals', 16, [D.Param('_Step', 0, 1, 1, 0, 0, 0)])],
                          [D.Binding('_MainTex', 0, 0, [2, 0]), D.Binding('$Globals', 1, 0, [0])])

GLOBALS_VS = D.ConstantBuffer('$Globals', 80, [D.Param('unity_MatrixMVP', 0, 4, 4, 1, 0, 0),
                                               D.Param('_Tint', 0, 1, 4, 0, 0, 64)])
GLOBALS_PS = D.ConstantBuffer('$Globals', 80, [D.Param('_Tint', 0, 1, 4, 0, 0, 64)])
VS_BIND = D.Bindings(9, [(0, 0), (3, 1)], [D.ConstantBuffer('', 0, []), GLOBALS_VS], [D.Binding('$Globals', 1, 0, [0])])
PS_BIND = D.Bindings(0, [], [D.ConstantBuffer('', 0, []), GLOBALS_PS],
                     [D.Binding('_MainTex', 0, 0, [2, 0]), D.Binding('_Mask', 0, 1, [2, 0]), D.Binding('$Globals', 1, 0, [0])])
VS_CODE = unity_code(VS_DXBC, (0, 1, 0))
PS_CODE = unity_code(PS_DXBC, (2, 1, 2))


def read_rdef(data):
    """The parts of an RDEF chunk HLSLcc reads: ({name: (type, bind point)}, {cb: [(var, offset, size, class, rows, cols)]})."""
    s = lambda o: data[o:data.index(b'\0', o)].decode()
    ncb, cboff, nb, boff = struct.unpack_from('<4I', data)
    binds = {}
    for i in range(nb):
        w = struct.unpack_from('<8I', data, boff + 32 * i)
        binds[(s(w[0]), w[1])] = w[5]
    cbs = {}
    for i in range(ncb):
        name, nv, voff, size, _, _ = struct.unpack_from('<6I', data, cboff + 24 * i)
        vs = []
        for k in range(nv):
            vn, off, vsize, _, toff, _ = struct.unpack_from('<6I', data, voff + 24 * k)
            cls, typ, rows, cols, elems, members = struct.unpack_from('<6H', data, toff)
            vs.append((s(vn), off, vsize, cls, rows, cols))
        cbs[s(name)] = (size, vs)
    return binds, cbs


class BindingsTest(unittest.TestCase):
    def test_round_trip(self):
        for b in (VS_BIND, PS_BIND):
            data = b.serialize()
            back = D.Bindings.parse(data)
            self.assertEqual(back.serialize(), data)
        back = D.Bindings.parse(PS_BIND.serialize())
        self.assertEqual([(x.name, x.kind, x.index) for x in back.bindings],
                         [('_MainTex', 0, 0), ('_Mask', 0, 1), ('$Globals', 1, 0)])
        self.assertEqual(D.Bindings.parse(VS_BIND.serialize()).cbuffers[1].params[0].name, 'unity_MatrixMVP')

    def test_refused(self):
        bad = D.Bindings(0, [], [], [D.Binding('_Buf', 3, 0, [])])
        bad.bindings[0].extra = []
        with self.assertRaises(D.Unsupported):          # a kind whose layout is not known
            D.Bindings.parse(bad.serialize())
        with self.assertRaises(D.Unsupported):          # the struct list is not followed by the bindings
            D.Bindings.parse(D.Bindings(0, [], [D.ConstantBuffer('UnityInstancing_PerDraw0', 64, [])], []).serialize()[:-8]
                             + struct.pack('<ii', 1, 0))
        with self.assertRaises(D.Unsupported):          # trailing bytes
            D.Bindings.parse(VS_BIND.serialize() + b'\0\0\0\0')

    def test_structs_and_samplers(self):
        # GPU instancing data (an array of structs) and an inline sampler state, as Unity 2019+ writes them
        inst = D.ConstantBuffer('UnityInstancing_PerDraw0', 256, [], [D.StructParam('unity_Builtins0Array', 0, 2, 128, [
            D.Param('unity_ObjectToWorldArray', 0, 4, 4, 1, 0, 0), D.Param('unity_WorldToObjectArray', 0, 4, 4, 1, 0, 64)])])
        b = D.Bindings(0, [], [D.ConstantBuffer('', 0, []), inst],
                       [D.Binding('_MainTex', 0, 0, [-1, 4], True), D.Binding('', 4, 0, [84], True)])
        data = b.serialize()
        back = D.Bindings.parse(data, 201806140)
        self.assertEqual(back.serialize(), data)
        self.assertEqual(back.cbuffers[1].structs[0].params[1].offset, 64)
        self.assertEqual((back.bindings[0].sampler, back.bindings[0].dim), (-1, 2))
        self.assertEqual(D.Bindings.parse(b.head() + b.params_bytes(), 202012090).serialize(), data)
        vs = D.Bindings(9, [(0, 0)], [D.ConstantBuffer('', 0, []), inst], [D.Binding('UnityInstancing_PerDraw0', 1, 0, [0])])
        with self.assertRaisesRegex(D.Unsupported, 'instancing'):
            D.with_reflection(D.split_code(VS_CODE)[1], vs)

    def test_gl_rest(self):
        self.assertEqual(D.gl_rest(13), struct.pack('<8i', 13, 0, 1, 0, 0, 0, 0, 0))


class ContainerTest(unittest.TestCase):
    def test_split_and_chunks(self):
        head, dx = D.split_code(PS_CODE)
        self.assertEqual(head, bytes([1, 2, 1, 2, 0, 0]))
        self.assertEqual([f for f, _ in D.chunks(dx)], ['ISGN', 'OSGN', 'SHDR'])
        self.assertTrue(B._is_d3d11(PS_CODE) and D.is_d3d11_code(PS_CODE))
        with self.assertRaises(D.Unsupported):
            D.split_code(PS_CODE[:-4])                  # container size does not match

    def test_declarations(self):
        p = D.Program(dict(PS_DXBC)['SHDR'])
        self.assertEqual((p.major, p.minor, p.stage), (4, 0, 0))
        self.assertEqual(p.cbuffers, {0: 5})
        self.assertEqual(p.textures, {0: (3, 5), 1: (3, 5)})
        self.assertEqual(p.samplers, {0: 0, 1: 0})

    def test_reflection(self):
        cont, prog = D.with_reflection(D.split_code(PS_CODE)[1], PS_BIND)
        chs = dict(D.chunks(cont))
        self.assertEqual(list(chs)[0], 'RDEF')
        binds, cbs = read_rdef(chs['RDEF'])
        self.assertEqual(binds, {('$Globals', 0): 0, ('_MainTex', 2): 0, ('_Mask', 2): 1,
                                 ('sampler_MainTex', 3): 0, ('sampler_Mask', 3): 1})
        self.assertEqual(cbs, {'$Globals': (80, [('_Tint', 64, 16, 1, 1, 4)])})
        _, cbs = read_rdef(dict(D.chunks(D.with_reflection(D.split_code(VS_CODE)[1], VS_BIND)[0]))['RDEF'])
        self.assertEqual(cbs['$Globals'][1][0], ('unity_MatrixMVP', 0, 64, 3, 4, 4))   # column-major 4x4

    def test_constant_reads(self):
        self.assertEqual(D.Program(dict(VS_DXBC)['SHDR']).cb_reads, {(0, 0, 0), (0, 1, 0), (0, 2, 0), (0, 3, 0)})
        self.assertEqual(D.Program(dict(PS_DXBC)['SHDR']).cb_reads, {(0, 4, 0)})

    def test_reflection_refused(self):
        dx = D.split_code(PS_CODE)[1]
        other = D.Bindings(0, [], [D.ConstantBuffer('$Globals', 80, [D.Param('_Other', 0, 1, 4, 0, 0, 16)])], PS_BIND.bindings)
        with self.assertRaisesRegex(D.Unsupported, r'cb0\[4\]\.x is read'):   # _Tint is not described
            D.with_reflection(dx, other)
        nameless = D.Bindings(0, [], PS_BIND.cbuffers, PS_BIND.bindings[:1] + PS_BIND.bindings[2:])
        with self.assertRaises(D.Unsupported):          # t1 has no name
            D.with_reflection(dx, nameless)
        cube = D.Bindings(0, [], PS_BIND.cbuffers, [D.Binding('_MainTex', 0, 0, [4, 0])] + PS_BIND.bindings[1:])
        with self.assertRaises(D.Unsupported):          # declared 2D, Unity says cube
            D.with_reflection(dx, cube)

    def test_tidy(self):
        src = ('#version 150\n#ifdef GL_ARB_shader_bit_encoding\n#extension GL_ARB_shader_bit_encoding : enable\n#endif\n\n'
               + D._DEFINE_BLOCKS[0] + D._DEFINE_BLOCKS[1] + 'UNITY_LOCATION(0) uniform  sampler2D _MainTex;\n')
        self.assertEqual(D.tidy(src), '#version 150\n#extension GL_ARB_shader_bit_encoding : enable\n\n'
                                      'uniform  sampler2D _MainTex;\n')


GL_PROGRAM = '''#ifdef VERTEX
#version 150
#extension GL_ARB_explicit_attrib_location : require

uniform 	vec4 hlslcc_mtx4x4unity_MatrixMVP[4];
in  vec4 in_POSITION0;
void main()
{
    gl_Position = hlslcc_mtx4x4unity_MatrixVP[0] * in_POSITION0.xxxx;
    return;
}

#endif
#ifdef FRAGMENT
#version 150
#extension GL_ARB_explicit_attrib_location : require

layout(location = 0) out vec4 SV_Target0;
void main()
{
    SV_Target0 = vec4(1.0);
    return;
}

#endif
'''


def _entry(gpu_type, code, rest, keywords=()):
    return B.Entry(201708220, gpu_type, [0, 0, 0, 0], list(keywords), code, rest)


def _sp(i, gpu_type, kw, source_map=0, cbs=()):
    return {'m_BlobIndex': i, 'm_Channels': {'m_Channels': [{'source': 0, 'target': 0}] if source_map else [],
                                             'm_SourceMap': source_map},
            'm_KeywordIndices': list(kw), 'm_ShaderHardwareTier': 3, 'm_GpuProgramType': gpu_type,
            'm_VectorParams': [], 'm_MatrixParams': [], 'm_TextureParams': [], 'm_BufferParams': [],
            'm_ConstantBuffers': [], 'm_ConstantBufferBindings': [{'m_NameIndex': 0, 'm_Index': 0}] if cbs else [],
            'm_UAVParams': [], 'm_Samplers': [], 'm_ShaderRequirements': 1}


def d3d11_tree(entries, passes):
    """A Shader typetree with only a D3D11 blob. passes: [[(vertex sps), (fragment sps)]] per SubShader."""
    raw = B.build_blob(entries)
    comp = lz4.block.compress(raw, store_size=False)
    subs = [{'m_Passes': [{'m_NameIndices': [('$Globals', 0), ('FOO', 1), ('STEREO_INSTANCING_ON', 2)],
                           'progVertex': {'m_SubPrograms': list(v)}, 'progFragment': {'m_SubPrograms': list(f)}}
                          for v, f in ss]} for ss in passes]
    return {'m_ParsedForm': {'m_Name': 'Test/D3D11', 'm_SubShaders': subs},
            'platforms': [4], 'offsets': [0], 'compressedLengths': [len(comp)], 'decompressedLengths': [len(raw)],
            'compressedBlob': list(comp)}


class AddGles3D3D11Test(unittest.TestCase):
    def entries(self):
        vs, ps = VS_BIND.serialize(), PS_BIND.serialize()
        return [_entry(15, VS_CODE, vs), _entry(17, PS_CODE, ps), _entry(17, PS_CODE, ps, ['FOO']),
                _entry(15, VS_CODE + b'', vs, ['STEREO_INSTANCING_ON']), _entry(17, PS_CODE, ps, ['STEREO_INSTANCING_ON'])]

    def test_pairs_and_layout(self):
        # variant '' and 'FOO' share the vertex entry 0 (identical D3D11 code) but not the pixel one
        tree = d3d11_tree(self.entries(), [[([_sp(0, 15, [], 9, 1), _sp(0, 15, [1], 9, 1), _sp(3, 15, [2], 9, 1)],
                                             [_sp(1, 17, []), _sp(2, 17, [1]), _sp(4, 17, [2])])]])
        seen = []

        def translate(vs, ps):
            seen.append((vs.keywords, ps.keywords))
            if 'STEREO_INSTANCING_ON' in vs.keywords:
                raise D.Unsupported('gl_Layer')
            return GL_PROGRAM.replace('vec4(1.0)', 'vec4(%d.0)' % len(seen))

        r = C.add_gles3_d3d11(tree, translate=translate)
        self.assertEqual((r.status, r.programs, r.twins, r.dropped), ('converted', 2, 4, 1), r)
        self.assertEqual(tree['platforms'], [4, 9])
        p = tree['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]
        vl, fl = p['progVertex']['m_SubPrograms'], p['progFragment']['m_SubPrograms']
        tv = [sp for sp in vl if sp['m_GpuProgramType'] == 4]
        tf = [sp for sp in fl if sp['m_GpuProgramType'] == 4]
        self.assertEqual([(sp['m_BlobIndex'], sp['m_KeywordIndices']) for sp in tv], [(0, []), (5, [1])])
        self.assertEqual([(sp['m_BlobIndex'], sp['m_KeywordIndices']) for sp in tf], [(1, []), (2, [1])])
        self.assertEqual(tv[0]['m_Channels'], {'m_Channels': [], 'm_SourceMap': 9})
        self.assertEqual(tv[0]['m_ConstantBufferBindings'], [])
        es = B.ShaderBlobs(tree).entries(9)
        self.assertEqual(len(es), 6)
        self.assertTrue(all(e.gpu_type == 4 for e in es))
        self.assertIn('vec4(1.0)', es[0].code.decode())
        self.assertIn('vec4(2.0)', es[5].code.decode())
        self.assertTrue(es[0].code.startswith(b'#ifdef VERTEX\n#version 300 es\n'))
        self.assertEqual((es[1].code, es[1].rest), (b'', D.gl_rest(0)))
        self.assertEqual(es[0].rest, D.gl_rest(9))
        self.assertEqual(es[5].keywords, ['FOO'])          # the shared vertex entry's keywords and the pixel one's
        self.assertEqual(dict(C.gles3_programs(tree)).keys(), {0, 5})
        self.assertEqual(C.add_gles3_d3d11(tree, translate=translate).status, 'unchanged')

    def test_sprite_skinning_left_out(self):
        # URP 2D's SKINNED_SPRITE variants (bones in a StructuredBuffer) are left out, not the shader
        vs, ps = VS_BIND.serialize(), PS_BIND.serialize()
        e = [_entry(15, VS_CODE, vs), _entry(17, PS_CODE, ps), _entry(15, VS_CODE + b'', vs, ['SKINNED_SPRITE']),
             _entry(17, PS_CODE, ps, ['SKINNED_SPRITE'])]
        tree = d3d11_tree(e, [[([_sp(0, 15, [], 9, 1), _sp(2, 15, [2], 9, 1)], [_sp(1, 17, []), _sp(3, 17, [2])])]])

        def translate(vs, ps):
            if 'SKINNED_SPRITE' in vs.keywords:
                raise D.Unsupported("binding '_SpriteBoneTransforms' of kind 2 (buffer or UAV?): not supported")
            return GL_PROGRAM
        r = C.add_gles3_d3d11(tree, translate=translate)
        self.assertEqual((r.status, r.programs, r.dropped, r.dropped_skinning), ('converted', 1, 0, 1), r)
        self.assertEqual(r.failed, [])

    def test_subshader_left_out(self):
        e = self.entries()
        bad = [([_sp(0, 15, [], 9, 1)], [_sp(1, 17, [])])]
        good = [([_sp(0, 15, [1], 9, 1)], [_sp(2, 17, [1])])]
        tree = d3d11_tree(e, [bad, good])

        def translate(vs, ps):
            if not ps.keywords:
                raise D.Unsupported('uses something')
            return GL_PROGRAM

        r = C.add_gles3_d3d11(tree, translate=translate)
        self.assertEqual(r.status, 'converted')
        self.assertEqual([si for si, _ in r.failed], [0])
        subs = tree['m_ParsedForm']['m_SubShaders']
        self.assertEqual([sp['m_GpuProgramType'] for sp in subs[0]['m_Passes'][0]['progVertex']['m_SubPrograms']], [15])
        self.assertEqual([sp['m_GpuProgramType'] for sp in subs[1]['m_Passes'][0]['progVertex']['m_SubPrograms']], [15, 4])
        # nothing translatable: the shader is skipped and keeps no GLES programs
        tree = d3d11_tree(e, [bad])
        r = C.add_gles3_d3d11(tree, translate=translate)
        self.assertEqual(r.status, 'skipped')
        self.assertEqual(tree['platforms'], [4])

    def test_parsed_form_disagrees(self):
        tree = d3d11_tree(self.entries(), [[([_sp(0, 15, [], 9, 1)], [_sp(1, 17, [])])]])
        sp = tree['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]['progVertex']['m_SubPrograms'][0]
        sp['m_ConstantBufferBindings'] = [{'m_NameIndex': 0, 'm_Index': 3}]      # cb3: not what the entry says
        r = C.add_gles3_d3d11(tree, translate=lambda vs, ps: GL_PROGRAM)
        self.assertEqual(r.status, 'skipped')
        self.assertIn('disagrees', r.why)

    def test_geometry_stage(self):
        e = self.entries() + [_entry(19, VS_CODE, VS_BIND.serialize())]
        tree = d3d11_tree(e, [[([_sp(0, 15, [], 9, 1)], [_sp(1, 17, [])])]])
        tree['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]['progGeometry'] = {'m_SubPrograms': [_sp(5, 19, [])]}
        r = C.add_gles3_d3d11(tree, translate=lambda vs, ps: GL_PROGRAM)
        self.assertEqual(r.status, 'skipped')
        self.assertIn('geometry', r.why)


@unittest.skipUnless(D.find_hlslcc(), 'pmhlslcc not built (tools/hlslcc/build.sh)')
class HlslccTest(unittest.TestCase):
    def test_translate(self):
        with D.Translator() as tr:
            gl = D.translate_program(tr, (VS_CODE, VS_BIND.serialize()), (PS_CODE, PS_BIND.serialize()))
        st = G.stage_sources(gl)
        self.assertEqual(set(st), {'VERTEX', 'FRAGMENT'})
        vs, fs = st['VERTEX'], st['FRAGMENT']
        for s in (vs, fs):
            self.assertTrue(s.startswith('#version 150\n#extension GL_ARB_explicit_attrib_location : require\n'))
            self.assertNotIn('HLSLCC_ENABLE_UNIFORM_BUFFERS', s)
        self.assertIn('uniform \tvec4 hlslcc_mtx4x4unity_MatrixMVP[4];', vs)
        self.assertIn('in  vec4 in_POSITION0;', vs)
        self.assertIn('in  vec2 in_TEXCOORD0;', vs)
        self.assertIn('out vec2 vs_TEXCOORD0;', vs)
        self.assertIn('gl_Position = hlslcc_mtx4x4unity_MatrixMVP[3] * in_POSITION0.wwww + ', vs)
        self.assertIn('uniform \tvec4 _Tint;', fs)
        self.assertIn('uniform  sampler2D _MainTex;', fs)
        self.assertIn('uniform  sampler2D _Mask;', fs)
        self.assertIn('texture(_Mask, vs_TEXCOORD0.xy)', fs)
        self.assertIn('in  vec2 vs_TEXCOORD0;', fs)
        self.assertIn('layout(location = 0) out vec4 SV_Target0;', fs)
        es = G.program_to_es(gl)
        self.assertEqual(es.count('#version 300 es\n'), 2)

    def test_shader_model_5(self):
        vs, ps = unity_code(as_sm5(VS_DXBC)), unity_code(as_sm5(PS_DXBC))
        self.assertEqual(D.Program(dict(D.chunks(D.split_code(vs)[1]))['SHEX']).major, 5)
        with D.Translator() as tr:
            sm5 = D.translate_program(tr, (vs, VS_BIND.serialize()), (ps, PS_BIND.serialize()))
            sm4 = D.translate_program(tr, (VS_CODE, VS_BIND.serialize()), (PS_CODE, PS_BIND.serialize()))
        self.assertEqual(sm5, sm4)

    def test_blur(self):
        ps = unity_code(BLUR_PS_DXBC, (1, 1, 1))
        with D.Translator() as tr:
            gl = D.translate_program(tr, (VS_CODE, VS_BIND.serialize()), (ps, BLUR_PS_BIND.serialize()))
        f = G.stage_sources(gl)['FRAGMENT']
        self.assertIn('float ImmCB_0_0_0[3];', f)
        self.assertIn('for(int u_xlati_loop_1 = 0 ; u_xlati_loop_1<3 ; u_xlati_loop_1++)', f)
        self.assertIn('textureLod(_MainTex, u_xlat1.xy, 0.0)', f)
        self.assertIn('discard;', f)
        self.assertIn('uniform \tfloat _Step;', f)
        notes = []
        es = G.stage_sources(G.program_to_es(gl, notes=notes))['FRAGMENT']
        self.assertEqual(sorted(notes), ['FRAGMENT const-arrays 1', 'FRAGMENT unroll 1'])
        self.assertIn('const float ImmCB_0_0_0[3] = float[3](0.25, 0.5, 0.25);', es)
        self.assertIn('ImmCB_0_0_0[2] + u_xlat0;', es)

    def test_bad_program(self):
        with D.Translator() as tr:
            with self.assertRaises(D.Unsupported):
                tr.translate([b'DXBC' + b'\0' * 40])        # not a valid container: refused, server keeps running
            self.assertEqual(len(tr.translate([D.with_reflection(D.split_code(PS_CODE)[1], PS_BIND)[0]])), 1)
            self.assertEqual(tr.crashes, 0)


if __name__ == '__main__':
    unittest.main()
