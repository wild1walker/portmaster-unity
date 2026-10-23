"""Unit tests of the Vulkan path (spirv.py, vulkan.py) on hand-assembled SPIR-V (no Unity or game
files): python -m unittest discover -s tests   (from convert/)

The translation tests run pmspirvcross (tools/spirvcross/build.sh, or $PMSPIRVCROSS) and are
skipped without it."""
import os
import struct
import sys
import unittest

import lz4.block

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import dxbc as D
from pmunity.shaders import spirv as S
from pmunity.shaders import vulkan as V


# ---------------------------------------------------------------- a tiny SPIR-V assembler

def string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


def module(insts, bound=100):
    words = [S.MAGIC, 0x10000, 0, bound, 0]
    for op, *a in insts:
        flat = []
        for x in a:
            flat += x if isinstance(x, list) else [x]
        words += [(len(flat) + 1) << 16 | op] + flat
    return struct.pack('<%dI' % len(words), *words)


# ids
VOID, FN, FLOAT, V4, V2, UINT, C4, ARR4, UBO, PUBO, UVAR = range(1, 12)
PIN4, PIN2, POUT4, POUT2, IN0, IN1, OUT0, GLPOS, INT, I0, I1, PU4, POF, MAIN = range(12, 26)
L, A, M0P, M0, P, T, STP, ST, STXY, UV, YP, Y, NY = range(30, 43)

VERTEX = module([
    (17, 1),                                            # OpCapability Shader
    (14, 0, 1),                                         # OpMemoryModel Logical GLSL450
    (15, 0, MAIN, string('main'), IN0, IN1, OUT0, GLPOS),
    (3, 2, 450),                                        # OpSource GLSL 450
    (5, OUT0, string('vs_TEXCOORD0')),                  # (the one name Unity keeps)
    (71, IN0, 30, 0), (71, IN1, 30, 1), (71, OUT0, 30, 0), (71, GLPOS, 11, 0),
    (71, ARR4, 6, 16), (72, UBO, 0, 35, 0), (72, UBO, 1, 35, 64), (71, UBO, 2),
    (71, UVAR, 34, 1), (71, UVAR, 33, 1),
    (19, VOID), (33, FN, VOID), (22, FLOAT, 32), (23, V4, FLOAT, 4), (23, V2, FLOAT, 2), (21, UINT, 32, 0),
    (43, UINT, C4, 4), (28, ARR4, V4, C4), (30, UBO, ARR4, V4), (32, PUBO, 2, UBO), (59, PUBO, UVAR, 2),
    (32, PIN4, 1, V4), (32, PIN2, 1, V2), (32, POUT4, 3, V4), (32, POUT2, 3, V2),
    (59, PIN4, IN0, 1), (59, PIN2, IN1, 1), (59, POUT2, OUT0, 3), (59, POUT4, GLPOS, 3),
    (21, INT, 32, 1), (43, INT, I0, 0), (43, INT, I1, 1), (32, PU4, 2, V4), (32, POF, 3, FLOAT),
    (54, VOID, MAIN, 0, FN), (248, L),
    (61, V4, A, IN0), (65, PU4, M0P, UVAR, I0, I0), (61, V4, M0, M0P), (133, V4, P, A, M0), (62, GLPOS, P),
    (61, V2, T, IN1), (65, PU4, STP, UVAR, I1), (61, V4, ST, STP), (79, V2, STXY, ST, ST, 0, 1),
    (133, V2, UV, T, STXY), (62, OUT0, UV),
    (65, POF, YP, GLPOS, I1), (61, FLOAT, Y, YP), (127, FLOAT, NY, Y), (62, YP, NY),    # Unity's y flip
    (253,), (56,),
])

IMG, PIMG, TEX, SAMP, PSAMP, SMP, SIMG, VIN, FOUT, TI, SI, CI, FUV, COL = range(50, 64)
FRAGMENT = module([
    (17, 1), (14, 0, 1),
    (15, 4, MAIN, string('main'), VIN, FOUT), (16, MAIN, 7),           # OriginUpperLeft
    (3, 2, 450),
    (71, VIN, 30, 0), (71, FOUT, 30, 0), (71, TEX, 34, 0), (71, TEX, 33, 1), (71, SMP, 34, 0), (71, SMP, 33, 0),
    (19, VOID), (33, FN, VOID), (22, FLOAT, 32), (23, V4, FLOAT, 4), (23, V2, FLOAT, 2),
    (25, IMG, FLOAT, 1, 0, 0, 0, 1, 0), (32, PIMG, 0, IMG), (59, PIMG, TEX, 0),
    (26, SAMP), (32, PSAMP, 0, SAMP), (59, PSAMP, SMP, 0), (27, SIMG, IMG),
    (32, PIN2, 1, V2), (59, PIN2, VIN, 1), (32, POUT4, 3, V4), (59, POUT4, FOUT, 3),
    (54, VOID, MAIN, 0, FN), (248, L),
    (61, IMG, TI, TEX), (61, SAMP, SI, SMP), (86, SIMG, CI, TI, SI), (61, V2, FUV, VIN),
    (87, V4, COL, CI, FUV), (62, FOUT, COL),
    (253,), (56,),
])


def vulkan_code(stages, slots=9):
    """Unity's Vulkan code: flags, a (offset, size) per stage slot, the modules."""
    head = 4 + 8 * slots
    table, body = [], b''
    for m in stages:
        table.append((head + len(body), len(m)) if m else (0, 0))
        body += m or b''
    table += [(0, 0)] * (slots - len(table))
    return struct.pack('<i', 32) + b''.join(struct.pack('<ii', *t) for t in table) + body


BINDINGS = D.Bindings(25, [(0, 13), (4, 14)], [
    D.ConstantBuffer('', 0, []),
    D.ConstantBuffer('VGlobals1234', 80, [D.Param('unity_ObjectToWorld', 0, 4, 4, 1, 0, 0),
                                          D.Param('_MainTex_ST', 0, 1, 4, 0, 0, 64)])],
    [D.Binding('_MainTex', 0, 0x08000001, [0x08000000, 4], True), D.Binding('VGlobals1234', 1, 0x04010001, [0], True)])

TRANSLATOR = V.find_spirv_cross()


class SpirvTest(unittest.TestCase):
    def test_reads_and_names(self):
        m = S.Module(VERTEX)
        self.assertEqual(m.stage, 'VERTEX')
        self.assertEqual(m.source_language, 2)
        self.assertEqual(m.interface(S.SC_INPUT), [IN0, IN1])
        self.assertEqual(m.interface(S.SC_OUTPUT), [OUT0])           # (gl_Position is a built-in)
        self.assertEqual(m.decoration(IN1, S.DEC_LOCATION), 1)
        self.assertEqual(m.member_decoration(UBO, 1, S.DEC_OFFSET), 64)
        self.assertEqual(m.describe(ARR4), 'vec4[4]')
        self.assertEqual(m.names[OUT0], 'vs_TEXCOORD0')
        m.set_name(OUT0, 'vs_LOCATION0')
        m.set_name(IN0, 'in_POSITION0')
        m.set_member_name(UBO, 1, '_MainTex_ST')
        back = S.Module(m.serialize())
        self.assertEqual((back.names[OUT0], back.names[IN0], back.member_names[(UBO, 1)]),
                         ('vs_LOCATION0', 'in_POSITION0', '_MainTex_ST'))
        self.assertEqual(sum(1 for op, a in back.insts if op == S.OP_NAME and a[0] == OUT0), 1)
        # the names go before the decorations (SPIR-V's logical layout)
        ops = [op for op, _ in back.insts]
        self.assertLess(max(i for i, op in enumerate(ops) if op in (S.OP_NAME, S.OP_MEMBER_NAME)), ops.index(S.OP_DECORATE))
        self.assertEqual(S.Module(VERTEX).serialize(), VERTEX)

    def test_refused(self):
        with self.assertRaises(S.SpirvError):
            S.Module(b'\0' * 20)
        with self.assertRaises(S.SpirvError):
            S.Module(VERTEX[:-4] + b'\x05\x00\x09\x00')


class CodeTest(unittest.TestCase):
    def test_split(self):
        code = vulkan_code([VERTEX, FRAGMENT])
        flags, mods = V.split_code(code)
        self.assertEqual((flags, [s for s, _ in mods]), (32, [0, 1]))
        self.assertEqual(mods[1][1], FRAGMENT)
        self.assertTrue(B._is_vulkan(code))
        # 2017.x: 5 slots; 2019.x: 6
        self.assertEqual([s for s, _ in V.split_code(vulkan_code([VERTEX, FRAGMENT], 5))[1]], [0, 1])
        self.assertFalse(B._is_vulkan(b'#ifdef VERTEX\n#version 150\n'))
        e = B.Entry(201806140, 25, [0, 0, 0, 0], [], code, BINDINGS.serialize(), [])
        self.assertEqual(B.Entry.parse(e.serialize()).code, code)

    def test_plan(self):
        p = V.Plan(BINDINGS, 202012090)
        self.assertEqual(p.loc_channel, {0: 'in_POSITION0', 1: 'in_TEXCOORD0'})
        self.assertEqual(p.cb_at, {0x10001: 'VGlobals1234'})
        self.assertEqual(p.sampler_at, {0: 'sampler_MainTex'})
        # Unity 2017.x: the older channel numbering and targets from 12
        old = V.Plan(D.Bindings(0, [(0, 12), (2, 13), (3, 14)], [], []), 201708220)
        self.assertEqual(old.loc_channel, {0: 'in_POSITION0', 1: 'in_COLOR0', 2: 'in_TEXCOORD0'})
        self.assertTrue(V.is_globals('VGlobals2505651496') and V.is_globals('PGlobals1') and not V.is_globals('UnityPerDraw'))


class UvOriginTest(unittest.TestCase):
    """The SRP core library's full-screen coordinates (UNITY_UV_STARTS_AT_TOP's flips) in the forms
    SPIRV-Cross and metal.py write them."""

    def test_triangle(self):
        vs = ('void main()\n{\n    _9.x = uint(bitfieldInsert(0, gl_VertexID, 1, 1));\n    _9.w = uint(gl_VertexID) & 2u;\n'
              '    vec2 _35 = vec2(_9.xw);\n    _30 = vec3(_35.x, _35.y, _30.z);\n    _30.z = (-_30.y) + 1.0;\n'
              '    vs_LOCATION0 = (_30.xz * _BlitScaleBias.xy) + _BlitScaleBias.zw;\n}\n')
        out, n = V.uv_origin(vs)
        self.assertEqual(n, 1)
        self.assertIn('    _30.z = _30.y;\n', out)
        self.assertEqual(out.replace('_30.z = _30.y;', '_30.z = (-_30.y) + 1.0;'), vs)

    def test_quad(self):
        vs = ('    _8 = int(uint(gl_VertexID) & 1u);\n    _18 = uint(gl_VertexID) >> 1u;\n    _8 += int(_18);\n'
              '    _36.x = float(_18);\n    _42 = uint(_8) & 1u;\n    _46 = float(_42);\n    _36.z = (-_46) + 1.0;\n'
              '    vs_LOCATION0.x = (-_36.x) + 1.0;\n')
        out, n = V.uv_origin(vs)
        self.assertEqual(n, 1)
        self.assertIn('    _36.z = _46;\n', out)
        self.assertIn('vs_LOCATION0.x = (-_36.x) + 1.0;', out)     # (x is not the flipped coordinate)

    def test_metal_form_and_flip_y(self):
        vs = ('uniform float _FlipY;\n    u_xlatu0.w = uint(gl_VertexID) & 0x2u;\n    u_xlat0.xy = vec2(u_xlatu0.xw);\n'
              '    u_xlat0.z = -u_xlat0.y + 1.0;\n    if(_FlipY < 0.5){\n')
        out, n = V.uv_origin(vs)
        self.assertEqual(n, 2)
        self.assertIn('u_xlat0.z = u_xlat0.y;', out)
        self.assertIn('if(false){', out)
        self.assertEqual(V.uv_origin('    u_xlat0.z = -u_xlat0.y + 1.0;\n'), ('    u_xlat0.z = -u_xlat0.y + 1.0;\n', 0))


@unittest.skipUnless(TRANSLATOR, 'pmspirvcross not built (tools/spirvcross/build.sh)')
class TranslateTest(unittest.TestCase):
    def setUp(self):
        self.tr = V.Translator(TRANSLATOR)

    def tearDown(self):
        self.tr.close()

    def test_program(self):
        text, typ = V.translate_program(self.tr, vulkan_code([VERTEX, FRAGMENT]), BINDINGS, 202012090, (2021, 3))
        self.assertEqual(typ, B.GPU_GLES3)
        st = C.glsl.stage_sources(text)
        vs, fs = st['VERTEX'], st['FRAGMENT']
        self.assertTrue(vs.startswith('#version 300 es\n'))
        for decl in ('uniform vec4 hlslcc_mtx4x4unity_ObjectToWorld[4];', 'uniform vec4 _MainTex_ST;',
                     'in vec4 in_POSITION0;', 'in vec2 in_TEXCOORD0;', 'out vec2 vs_LOCATION0;'):
            self.assertIn(decl, vs)
        self.assertNotIn('gl_Position.y = -gl_Position.y', vs)
        self.assertNotIn('layout(location', vs.split('#endif', 1)[1])      # no locations on inputs or varyings
        self.assertIn('UNITY_LOCATION(0) uniform highp sampler2D _MainTex;', fs)
        self.assertIn('layout(location = 0) out vec4 SV_Target0;', fs)
        self.assertIn('in vec2 vs_LOCATION0;', fs)
        self.assertIn('texture(_MainTex, vs_LOCATION0)', fs)
        self.assertIn('#define UNITY_SUPPORTS_UNIFORM_LOCATION 1', fs)
        # Unity's GL binding info of it
        info = D.gl_bindings(text, (2021, 3), BINDINGS.source_map)
        self.assertEqual([(p.name, p.offset) for cb in info.cbuffers for p in cb.params],
                         [('_MainTex_ST', 64), ('unity_ObjectToWorld', 0)])   # (declaration order; vectors listed first)
        self.assertEqual([(b.name, b.index) for b in info.bindings], [('_MainTex', 0)])
        # up to 2018: plain uniforms, no macros
        old, _ = V.translate_program(self.tr, vulkan_code([VERTEX, FRAGMENT]), BINDINGS, 201802150, (2018, 4))
        self.assertNotIn('UNITY_LOCATION', old)
        self.assertIn('uniform highp sampler2D _MainTex;', old)

    def test_refused(self):
        with self.assertRaises(V.Unsupported):          # a texture the binding info does not name
            V.translate_program(self.tr, vulkan_code([VERTEX, FRAGMENT]),
                                D.Bindings(25, BINDINGS.channels, BINDINGS.cbuffers, BINDINGS.bindings[1:]), 202012090)
        with self.assertRaises(V.Unsupported):          # no fragment stage
            V.translate_program(self.tr, vulkan_code([VERTEX]), BINDINGS, 202012090)
        with self.assertRaises(V.Unsupported):          # the matrix is not the vec4 array Unity says
            cbs = [BINDINGS.cbuffers[0], D.ConstantBuffer('VGlobals1234', 80, [D.Param('unity_ObjectToWorld', 0, 4, 4, 1, 2, 0)])]
            V.translate_program(self.tr, vulkan_code([VERTEX, FRAGMENT]), D.Bindings(25, BINDINGS.channels, cbs,
                                                                                    BINDINGS.bindings), 202012090)

    def test_shader(self):
        """add_gles3_vulkan on a 2021.2+ Shader: one subprogram per variant, a parameter entry each."""
        code = B.Entry(202012090, 25, [0, 0, 0, 0], [], vulkan_code([VERTEX, FRAGMENT]), BINDINGS.head())
        params = B.ParamEntry(202012090, BINDINGS.params_bytes())
        raw = B.build_segments([code, params])
        comp = lz4.block.compress(raw[0], store_size=False)
        sp = {'m_BlobIndex': 0, 'm_Channels': {'m_Channels': [{'source': 0, 'target': 13}], 'm_SourceMap': 25},
              'm_GlobalKeywordIndices': [], 'm_LocalKeywordIndices': [], 'm_ShaderHardwareTier': -1,
              'm_GpuProgramType': 25, 'm_Parameters': {k: [] for k in ('m_VectorParams', 'm_MatrixParams',
                                                                        'm_TextureParams', 'm_BufferParams',
                                                                        'm_ConstantBuffers', 'm_ConstantBufferBindings',
                                                                        'm_UAVParams', 'm_Samplers')},
              'm_ShaderRequirements': 33}
        prog = {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], [sp]], 'm_ParameterBlobIndices': [[], [], [], [1]],
                'm_CommonParameters': {}}
        tree = {'m_ParsedForm': {'m_Name': 'Test/Vulkan', 'm_SubShaders': [{'m_Passes': [
                    {'m_NameIndices': [], 'm_Platforms': [18], 'progVertex': prog, 'progFragment': {}}]}]},
                'platforms': [18], 'offsets': [[0]], 'compressedLengths': [[len(comp)]], 'decompressedLengths': [[len(raw[0])]],
                'compressedBlob': list(comp), 'stageCounts': [1]}
        r = V.add_gles3_vulkan(tree, self.tr, unity='2021.3.16f1')
        self.assertEqual((r.status, r.programs, r.twins), ('converted', 1, 1), r)
        self.assertEqual(tree['platforms'], [9, 18])
        self.assertEqual(tree['stageCounts'], [1, 1])
        p = tree['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]
        self.assertEqual(p['m_Platforms'], [9, 18])
        tw = p['progVertex']['m_PlayerSubPrograms'][3][1]
        self.assertEqual((tw['m_GpuProgramType'], tw['m_BlobIndex'], tw['m_ShaderRequirements']), (4, 0, 33))
        self.assertEqual(tw['m_Channels']['m_Channels'], [])
        self.assertEqual(p['progVertex']['m_ParameterBlobIndices'][3], [1, 1])
        es = C.platform_entries(tree, B.PLATFORM_GLES3PLUS)
        self.assertTrue(es[0].code.startswith(b'#ifdef VERTEX\n#version 300 es\n'))
        info = D.Bindings.parse(es[0].rest + es[1].data, 202012090)
        self.assertEqual([b.name for b in info.bindings], ['_MainTex', '$Globals'][:len(info.bindings)])
        self.assertEqual(V.add_gles3_vulkan(tree, self.tr).status, 'unchanged')

    def test_shader_6000_6(self):
        """Unity 6000.6 (serializer 202605290): the parameter entries in 6000.6's layout, read and written
        so; the shared parameters (6000.6's names) put back into the Vulkan parameter entry."""
        ser = D.TYPED_FROM
        own = D.Bindings(25, [(0, 13), (4, 14)], [
            D.ConstantBuffer('', 0, []), D.ConstantBuffer('VGlobals1234', 80, [D.Param('_MainTex_ST', 0, 1, 4, 0, 0, 64)])],
            [D.Binding('_MainTex', 0, 0x08000001, [0x08000000, 4], True, (0, 0)),
             D.Binding('VGlobals1234', 1, 0x04010001, [], True, (0,))], typed=True)
        code = B.Entry(ser, 25, [0, 0, 0, 0], [], vulkan_code([VERTEX, FRAGMENT]), own.head())
        params = B.ParamEntry(ser, own.params_bytes())
        self.assertEqual(D.entry_bindings(code, params, typed=True).serialize(), own.serialize())
        raw = B.build_segments([code, params])
        comp = lz4.block.compress(raw[0], store_size=False)
        empty = ('m_VectorParams', 'm_MatrixParams', 'm_TextureParams', 'm_BufferParams', 'm_ConstantBuffers',
                 'm_ConstantBufferBindings', 'm_UAVParams', 'm_Samplers')
        common = {k: [] for k in empty}
        common['m_ConstantBuffers'] = [{'m_NameIndex': 0, 'm_VectorParams': [], 'm_StructParams': [], 'm_Size': 80,
                                        'm_IsPartialCB': True, 'm_MatrixParams': [
                                            {'m_NameIndex': 1, 'm_OffsetInConstantBuffer': 0, 'm_ArraySize': 0, 'm_Type': 0,
                                             'm_RowCount': 4}]}]
        sp = {'m_BlobIndex': 0, 'm_Channels': {'m_Channels': [{'source': 0, 'target': 13}], 'm_SourceMap': 25},
              'm_GlobalKeywordIndices': [], 'm_LocalKeywordIndices': [], 'm_ShaderHardwareTier': -1,
              'm_GpuProgramType': 25, 'm_ShaderRequirements': 33}
        prog = {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], [sp]], 'm_ParameterBlobIndices': [[], [], [], [1]],
                'm_CommonParameters': common, 'm_SerializedKeywordStateMask': []}
        tree = {'m_ParsedForm': {'m_Name': 'Test/Vulkan66', 'm_SubShaders': [{'m_Passes': [
                    {'m_NameIndices': [['VGlobals1234', 0], ['unity_ObjectToWorld', 1]], 'm_Platforms': [18],
                     'm_SerializedKeywordStateMask': [], 'progVertex': prog, 'progFragment': {}}]}]},
                'platforms': [18], 'offsets': [[0]], 'compressedLengths': [[len(comp)]], 'decompressedLengths': [[len(raw[0])]],
                'compressedBlob': list(comp), 'stageCounts': [1]}
        r = V.add_gles3_vulkan(tree, self.tr, unity='6000.6.2f1')
        self.assertEqual((r.status, r.programs, r.twins), ('converted', 1, 1), r.why)
        p = tree['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]
        self.assertEqual(p['progVertex']['m_ParameterBlobIndices'][3], [1, 1])
        es = C.platform_entries(tree, B.PLATFORM_GLES3PLUS)
        self.assertEqual([type(e).__name__ for e in es], ['Entry', 'ParamEntry'])
        self.assertEqual((es[0].version, es[1].version), (ser, ser))
        self.assertTrue(es[0].code.startswith(b'#ifdef VERTEX\n#version 300 es\n'))
        self.assertIn(b'uniform vec4 hlslcc_mtx4x4unity_ObjectToWorld[4];', es[0].code)
        gl = D.entry_bindings(es[0], es[1], typed=True)
        self.assertTrue(gl.typed)
        self.assertEqual([(x.name, x.kind, x.index) for x in gl.bindings], [('_MainTex', 0, 0), ('$Globals', 1, -1)])
        self.assertEqual([(p.name, p.offset) for cb in gl.cbuffers for p in cb.params],
                         [('_MainTex_ST', 64), ('unity_ObjectToWorld', 0)])
        with self.assertRaises(D.Unsupported):          # (not the layout of the lines before)
            D.entry_bindings(es[0], es[1])
        # the shared parameters are in the Vulkan parameter entry now, in its layout
        blobs = B.ShaderBlobs(tree)
        ves = blobs.entries(B.PLATFORM_VULKAN, {1}, {0})
        vk = D.entry_bindings(ves[0], ves[1], typed=True)
        self.assertEqual([p.name for p in vk.cbuffers[1].params], ['_MainTex_ST', 'unity_ObjectToWorld'])   # (vectors, matrices)
        self.assertEqual(p['progVertex']['m_CommonParameters']['m_ConstantBuffers'], [])


if __name__ == '__main__':
    unittest.main()
