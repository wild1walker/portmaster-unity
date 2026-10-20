"""Unit tests of the shader converter on small hand-written programs (no game data):
python -m unittest discover -s tests   (from convert/)"""
import copy
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import glsl as G

PROGRAM = '''#ifdef VERTEX
#version 150
#extension GL_ARB_explicit_attrib_location : require
#extension GL_ARB_shader_bit_encoding : enable

uniform 	vec4 hlslcc_mtx4x4unity_MatrixVP[4];
in  vec4 in_POSITION0;
in  vec2 in_TEXCOORD0;
out vec2 vs_TEXCOORD0;
void main()
{
    gl_Position = hlslcc_mtx4x4unity_MatrixVP[0] * in_POSITION0.xxxx;
    vs_TEXCOORD0.xy = in_TEXCOORD0.xy;
    return;
}

#endif
#ifdef FRAGMENT
#version 150
#extension GL_ARB_explicit_attrib_location : require
#extension GL_ARB_shader_bit_encoding : enable

vec4 ImmCB_0_0_0[3];
uniform  sampler2D _MainTex;
uniform  sampler3D _Lut;
in  vec2 vs_TEXCOORD0;
layout(location = 0) out vec4 SV_Target0;
vec4 u_xlat0;
vec2 u_xlat1;
void main()
{
	ImmCB_0_0_0[0] = vec4(0.25, 0.0, 0.0, 0.0);
	ImmCB_0_0_0[1] = vec4(0.5, 0.0, 0.0, 1.0);
	ImmCB_0_0_0[2] = vec4(0.25, 0.0, 0.0, 0.0);
    u_xlat0 = vec4(0.0, 0.0, 0.0, 0.0);
    u_xlat1.xy = vs_TEXCOORD0.xy;
    for(int u_xlati_loop_1 = 0 ; u_xlati_loop_1<3 ; u_xlati_loop_1++)
    {
        u_xlat0 = texture(_MainTex, u_xlat1.xy) * ImmCB_0_0_0[u_xlati_loop_1].xxxw + u_xlat0;
        u_xlat1.x = u_xlat1.x + 0.01;
    }
    SV_Target0 = u_xlat0 + texture(_Lut, u_xlat0.xyz);
    return;
}

#endif
'''


class GlslTest(unittest.TestCase):
    def test_translation(self):
        out = G.program_to_es(PROGRAM, optimize=())
        st = G.stage_sources(out)
        self.assertEqual(set(st), {'VERTEX', 'FRAGMENT'})
        for src in st.values():
            self.assertTrue(src.startswith('#version 300 es\n'))
            self.assertNotIn('GL_ARB_', src)
            self.assertIn('precision highp float;', src)
        self.assertIn('precision highp sampler2D;', st['FRAGMENT'])
        self.assertIn('precision highp sampler3D;', st['FRAGMENT'])
        self.assertNotIn('sampler', st['VERTEX'])
        self.assertIn('for(int u_xlati_loop_1', st['FRAGMENT'])

    def test_default_samplers(self):
        f = G.stage_sources(G.program_to_es(PROGRAM, precision='default-samplers', optimize=()))['FRAGMENT']
        self.assertNotIn('precision highp sampler2D;', f)
        self.assertIn('precision highp sampler3D;', f)        # no ES default: must be declared

    def test_rewrites(self):
        notes = []
        f = G.stage_sources(G.program_to_es(PROGRAM, notes=notes))['FRAGMENT']
        self.assertIn('const vec4 ImmCB_0_0_0[3] = vec4[3](vec4(0.25, 0.0, 0.0, 0.0), vec4(0.5, 0.0, 0.0, 1.0), '
                      'vec4(0.25, 0.0, 0.0, 0.0));', f)
        self.assertNotIn('ImmCB_0_0_0[0] = ', f)
        self.assertNotIn('for(', f)
        for k in range(3):
            self.assertIn(f'ImmCB_0_0_0[{k}].xxxw', f)
        self.assertEqual(sorted(notes), ['FRAGMENT const-arrays 1', 'FRAGMENT unroll 1'])

    def test_no_unroll_when_counter_written_or_break(self):
        for body in ('        u_xlati_loop_1 = 2;\n', '        break;\n', '        float x = 1.0;\n'):
            p = PROGRAM.replace('        u_xlat1.x = u_xlat1.x + 0.01;\n', '        u_xlat1.x = u_xlat1.x + 0.01;\n' + body)
            f = G.stage_sources(G.program_to_es(p))['FRAGMENT']
            self.assertIn('for(int u_xlati_loop_1', f, body)

    def test_no_const_array_when_written_later(self):
        p = PROGRAM.replace('    SV_Target0 = u_xlat0', '    ImmCB_0_0_0[1].x = 2.0;\n    SV_Target0 = u_xlat0')
        f = G.stage_sources(G.program_to_es(p))['FRAGMENT']
        self.assertNotIn('const vec4 ImmCB', f)

    def test_unsupported(self):
        for bad in ('gl_FragColor = vec4(1.0);', 'u_xlat0 = textureGather(_MainTex, vs_TEXCOORD0);',
                    'noperspective out vec4 x;'):
            p = PROGRAM.replace('    return;\n}\n\n#endif\n#ifdef FRAGMENT', '    ' + bad + '\n    return;\n}\n\n#endif\n#ifdef FRAGMENT')
            with self.assertRaises(G.Unsupported):
                G.program_to_es(p)


def _entry(gpu_type, code, keywords=()):
    return B.Entry(201708220, gpu_type, [0, 0, 0, 0], list(keywords), code, b'\x01\x00\x00\x00' * 3)


class BlobTest(unittest.TestCase):
    def test_round_trip(self):
        es = [_entry(6, PROGRAM.encode(), ['A_KEYWORD']), _entry(6, b''), None]
        data = B.build_blob(es)
        back = B.parse_blob(data)
        self.assertEqual(back[0].code, PROGRAM.encode())
        self.assertEqual(back[0].keywords, ['A_KEYWORD'])
        self.assertEqual(back[1].code, b'')
        self.assertIsNone(back[2])
        self.assertEqual(B.build_blob(back), data)

    def test_add_gles3(self):
        es = [_entry(6, PROGRAM.encode()), _entry(6, b'')]
        raw = B.build_blob(es)
        import lz4.block
        comp = lz4.block.compress(raw, store_size=False)
        sub = lambda i: {'m_BlobIndex': i, 'm_GpuProgramType': 6, 'm_KeywordIndices': []}
        tree = {'m_ParsedForm': {'m_Name': 'Test/Shader', 'm_SubShaders': [{'m_Passes': [
                    {'progVertex': {'m_SubPrograms': [sub(0)]}, 'progFragment': {'m_SubPrograms': [sub(1)]}}]}]},
                'platforms': [15], 'offsets': [0], 'compressedLengths': [len(comp)], 'decompressedLengths': [len(raw)],
                'compressedBlob': list(comp)}
        r = C.add_gles3(tree)
        self.assertEqual(r.status, 'converted')
        self.assertEqual(tree['platforms'], [9, 15])
        self.assertEqual(r.twins, 2)
        p = tree['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]
        self.assertEqual([s['m_GpuProgramType'] for s in p['progVertex']['m_SubPrograms']], [6, 4])
        progs = C.gles3_programs(tree)
        self.assertEqual([i for i, _ in progs], [0])
        self.assertTrue(progs[0][1].startswith('#ifdef VERTEX\n#version 300 es\n'))
        self.assertEqual(C.add_gles3(tree).status, 'unchanged')           # already has GLES3
        self.assertEqual(C.add_gles3(tree, replace=True).status, 'converted')
        self.assertEqual(len(p['progVertex']['m_SubPrograms']), 2)
        # replacing in a shader whose programs no longer translate: the old GLES programs go
        bad = PROGRAM.replace('    return;\n}\n\n#endif\n#ifdef FRAGMENT', '    gl_FragColor = vec4(1.0);\n    return;\n}\n\n#endif\n#ifdef FRAGMENT', 1)
        sb = B.ShaderBlobs(tree)
        sb.set_entries(15, [_entry(6, bad.encode()), _entry(6, b'')])
        sb.store()
        r = C.add_gles3(tree, replace=True)
        self.assertEqual((r.status, r.changed), ('skipped', True))
        self.assertEqual(tree['platforms'], [15])
        self.assertEqual([s['m_GpuProgramType'] for s in p['progVertex']['m_SubPrograms']], [6])


# Unity 2019+ programs: the optional extension wrapped in #ifdef, the macro blocks of the
# uniform-buffer and uniform-location switches, and a texture declared through UNITY_LOCATION
PROGRAM_2019 = """#ifdef VERTEX
#version 150
#extension GL_ARB_explicit_attrib_location : require
#ifdef GL_ARB_shader_bit_encoding
#extension GL_ARB_shader_bit_encoding : enable
#endif

#define HLSLCC_ENABLE_UNIFORM_BUFFERS 1
#if HLSLCC_ENABLE_UNIFORM_BUFFERS
#define UNITY_UNIFORM
#else
#define UNITY_UNIFORM uniform
#endif
#define UNITY_SUPPORTS_UNIFORM_LOCATION 1
#if UNITY_SUPPORTS_UNIFORM_LOCATION
#define UNITY_LOCATION(x) layout(location = x)
#define UNITY_BINDING(x) layout(binding = x, std140)
#else
#define UNITY_LOCATION(x)
#define UNITY_BINDING(x) layout(std140)
#endif
uniform 	vec4 hlslcc_mtx4x4unity_MatrixVP[4];
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
#ifdef GL_ARB_shader_bit_encoding
#extension GL_ARB_shader_bit_encoding : enable
#endif

#define UNITY_SUPPORTS_UNIFORM_LOCATION 1
#if UNITY_SUPPORTS_UNIFORM_LOCATION
#define UNITY_LOCATION(x) layout(location = x)
#define UNITY_BINDING(x) layout(binding = x, std140)
#else
#define UNITY_LOCATION(x)
#define UNITY_BINDING(x) layout(std140)
#endif
UNITY_LOCATION(0) uniform  sampler2D _MainTex;
layout(location = 0) out vec4 SV_Target0;
void main()
{
    SV_Target0 = texture(_MainTex, vec2(0.5));
    return;
}

#endif
"""


class Glsl2019Test(unittest.TestCase):
    def test_wrapped_extensions_and_macros(self):
        st = G.stage_sources(G.program_to_es(PROGRAM_2019))
        for src in st.values():
            self.assertTrue(src.startswith('#version 300 es\nprecision highp float;\n'), src[:80])
            self.assertNotIn('GL_ARB_', src)
            self.assertNotIn('#ifdef GL_', src)
            self.assertIn('#define UNITY_SUPPORTS_UNIFORM_LOCATION 1', src)   # switched by the player
        self.assertIn('UNITY_LOCATION(0) uniform  sampler2D _MainTex;', st['FRAGMENT'])

    def test_other_wrapped_extension_kept_first(self):
        p = PROGRAM_2019.replace('#ifdef GL_ARB_shader_bit_encoding\n#extension GL_ARB_shader_bit_encoding : enable\n',
                                 '#ifdef GL_EXT_shader_framebuffer_fetch\n#extension GL_EXT_shader_framebuffer_fetch : enable\n', 1)
        v = G.stage_sources(G.program_to_es(p))['VERTEX']
        self.assertTrue(v.startswith('#version 300 es\n#ifdef GL_EXT_shader_framebuffer_fetch\n'
                                     '#extension GL_EXT_shader_framebuffer_fetch : enable\n#endif\nprecision highp float;'))

    def test_driver_view(self):
        from pmunity.shaders import export as X
        f = X.as_driver_sees(G.stage_sources(G.program_to_es(PROGRAM_2019))['FRAGMENT'])
        self.assertIn('#define UNITY_SUPPORTS_UNIFORM_LOCATION 0', f)


def _entry19(gpu_type, code, keywords=(), local=()):
    return B.Entry(201806140, gpu_type, [0, 0, 0, 0], list(keywords), code, struct.pack('<2i', 9, 0), list(local))


def _entry21(gpu_type, code, keywords=()):
    return B.Entry(202012090, gpu_type, [0, 0, 0, 0], list(keywords), code, struct.pack('<2i', 9, 0))


def _params21(name='$Globals'):
    """A parameter entry: one constant buffer (the default one), then `name` with one float4."""
    b = name.encode()
    cb = struct.pack('<i', len(b)) + b + b'\0' * (-len(b) % 4) + struct.pack('<ii', 16, 1)
    cb += struct.pack('<i', 6) + b'_Color\0\0' + struct.pack('<6i', 0, 1, 4, 0, 0, 0) + struct.pack('<i', 0)
    return B.ParamEntry(202012090, struct.pack('<i', 2) + struct.pack('<4i', 0, 0, 0, 0) + cb + struct.pack('<i', 0))


def _tree(platform_blobs, parsed, nested, stage_counts=None):
    import lz4.block
    blob, offs, cl, dl = bytearray(), [], [], []
    for data in platform_blobs.values():
        segs = data if nested else [data]
        o, c, d = [], [], []
        for seg in segs:
            z = lz4.block.compress(seg, store_size=False)
            o.append(len(blob)); c.append(len(z)); d.append(len(seg))
            blob += z
        offs.append(o if nested else o[0]); cl.append(c if nested else c[0]); dl.append(d if nested else d[0])
    t = {'m_ParsedForm': parsed, 'platforms': list(platform_blobs), 'offsets': offs,
         'compressedLengths': cl, 'decompressedLengths': dl, 'compressedBlob': bytes(blob)}
    if stage_counts is not None:
        t['stageCounts'] = stage_counts
    return t


class NewerLayoutsTest(unittest.TestCase):
    def test_two_keyword_lists(self):
        e = _entry19(6, PROGRAM_2019.encode(), ['A'], ['B_LOCAL'])
        back = B.Entry.parse(e.serialize())
        self.assertEqual((back.keywords, back.local_keywords, back.code), (['A'], ['B_LOCAL'], PROGRAM_2019.encode()))
        self.assertFalse(back.guessed)

    def test_unknown_version_with_two_lists_guessed(self):
        e = _entry19(6, PROGRAM_2019.encode(), [], [])
        e.version = 201911110                          # not in SERIALIZERS
        back = B.Entry.parse(e.serialize())
        self.assertTrue(back.guessed)
        self.assertEqual(back.code, PROGRAM_2019.encode())   # not the empty local list taken as code
        self.assertEqual(back.local_keywords, [])

    def test_segments_round_trip(self):
        es = [_entry19(6, PROGRAM_2019.encode(), ['A']), None, _entry19(6, b''), _entry19(6, PROGRAM_2019.encode())]
        segs = B.build_segments(es, [0, 0, 1, 1])
        self.assertEqual(len(segs), 2)
        self.assertEqual(B.blob_segments(segs), [0, 0, 1, 1])
        back = B.parse_blob(segs)
        self.assertEqual([e and e.serialize() for e in back], [e and e.serialize() for e in es])
        self.assertEqual(B.build_segments(back, B.blob_segments(segs)), segs)

    def test_add_gles3_2020(self):
        """2019.3 - 2021.1: segmented blob, global/local keyword indices, m_Platforms."""
        sub = lambda i, kw: {'m_BlobIndex': i, 'm_GpuProgramType': 6, 'm_GlobalKeywordIndices': kw,
                             'm_LocalKeywordIndices': [], 'm_ShaderHardwareTier': 3}
        p = {'m_Platforms': [14, 15], 'progVertex': {'m_SubPrograms': [sub(0, []), sub(1, [0])]},
             'progFragment': {'m_SubPrograms': []}}
        es = [_entry19(6, PROGRAM_2019.encode()), _entry19(6, PROGRAM_2019.encode(), ['UNITY_SINGLE_PASS_STEREO'])]
        tree = _tree({15: B.build_segments(es, [0, 1])}, {'m_Name': 'T', 'm_SubShaders': [{'m_Passes': [p]}]}, True)
        r = C.add_gles3(tree)
        self.assertEqual((r.status, r.programs, r.twins), ('converted', 2, 2))
        self.assertEqual(tree['platforms'], [9, 15])
        self.assertEqual(p['m_Platforms'], [9, 14, 15])
        sb = B.ShaderBlobs(tree)
        self.assertEqual(sb.segments(9), [0, 1])
        ours = sb.entries(9)
        self.assertEqual([e.gpu_type for e in ours], [4, 4])
        self.assertEqual(ours[1].keywords, ['UNITY_SINGLE_PASS_STEREO'])
        self.assertTrue(ours[0].code.startswith(b'#ifdef VERTEX\n#version 300 es\n'))
        self.assertEqual([(s['m_GpuProgramType'], s['m_BlobIndex']) for s in p['progVertex']['m_SubPrograms']],
                         [(6, 0), (6, 1), (4, 0), (4, 1)])

    def test_add_gles3_2021(self):
        """2021.2+: parameter entries, per-tier m_PlayerSubPrograms and m_ParameterBlobIndices,
        stageCounts; a VR-only variant that does not translate is left out."""
        sub = lambda i, kw: {'m_BlobIndex': i, 'm_KeywordIndices': kw, 'm_ShaderRequirements': 1, 'm_GpuProgramType': 6}
        stereo = PROGRAM_2019.replace('    return;\n}\n\n#endif\n#ifdef FRAGMENT',
                                      '    gl_Layer = 1;\n    return;\n}\n\n#endif\n#ifdef FRAGMENT', 1)
        vert = {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], [sub(2, []), sub(3, [0])]],
                'm_ParameterBlobIndices': [[], [], [], [0, 1]], 'm_CommonParameters': {}}
        frag = {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], []], 'm_ParameterBlobIndices': [[], [], [], []]}
        p = {'m_Platforms': [4, 15], 'progVertex': vert, 'progFragment': frag}
        es = [_params21(), _params21('UnityStereoGlobals'), _entry21(6, PROGRAM_2019.encode()),
              _entry21(6, stereo.encode(), ['STEREO_INSTANCING_ON'])]
        tree = _tree({4: B.build_segments([]), 15: B.build_segments(es)},
                     {'m_Name': 'T', 'm_SubShaders': [{'m_Passes': [p]}]}, True, stage_counts=[2, 1])
        r = C.add_gles3(tree)
        self.assertEqual((r.status, r.programs, r.twins, r.dropped), ('converted', 1, 1, 1))
        self.assertEqual(tree['platforms'], [4, 9, 15])
        self.assertEqual(tree['stageCounts'], [2, 1, 1])
        self.assertEqual(p['m_Platforms'], [4, 9, 15])
        self.assertEqual(vert['m_PlayerSubPrograms'][3][-1], dict(sub(2, []), m_GpuProgramType=4))
        self.assertEqual(vert['m_ParameterBlobIndices'][3], [0, 1, 0])
        ours = C.platform_entries(tree, 9)
        # (the parameters only the left-out variant used: no GLES3 subprogram names them, kept as bytes)
        self.assertEqual([type(e).__name__ for e in ours], ['ParamEntry', 'RawEntry', 'Entry', 'Entry'])
        self.assertEqual(ours[0].serialize(), es[0].serialize())
        self.assertEqual((ours[2].gpu_type, ours[2].rest), (4, es[2].rest))
        self.assertEqual(ours[3].gpu_type, 6)                 # left out: no GLES3 subprogram uses it
        self.assertEqual([i for i, _ in C.gles3_programs(tree)], [2])
        # rebuilt from scratch: the same result
        before = copy.deepcopy(tree)
        self.assertEqual(C.add_gles3(tree, replace=True).status, 'converted')
        self.assertEqual(tree['m_ParsedForm'], before['m_ParsedForm'])
        self.assertEqual(B.ShaderBlobs(tree).platforms, B.ShaderBlobs(before).platforms)


if __name__ == '__main__':
    unittest.main()
