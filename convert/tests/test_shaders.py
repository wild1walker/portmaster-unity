"""Unit tests of the shader converter on small hand-written programs (no game data):
python -m unittest discover -s tests   (from convert/)"""
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


if __name__ == '__main__':
    unittest.main()
