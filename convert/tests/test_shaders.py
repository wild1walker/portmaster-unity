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


# GLSL 4.20, the form of Unity's GLCore41 / GLCore43 programs (`#pragma target 4.5` and up):
# 420pack, the unused `precise` declarations, varyings with explicit locations, binding macros
_MACROS = """#define UNITY_SUPPORTS_UNIFORM_LOCATION 1
#if UNITY_SUPPORTS_UNIFORM_LOCATION
#define UNITY_LOCATION(x) layout(location = x)
#define UNITY_BINDING(x) layout(binding = x, std140)
#else
#define UNITY_LOCATION(x)
#define UNITY_BINDING(x) layout(std140)
#endif
"""
PROGRAM_420 = """#ifdef VERTEX
#version 420
#extension GL_ARB_explicit_attrib_location : require
#extension GL_ARB_shading_language_420pack : require

""" + _MACROS + """precise vec4 u_xlat_precise_vec4;
precise ivec4 u_xlat_precise_ivec4;
precise bvec4 u_xlat_precise_bvec4;
precise uvec4 u_xlat_precise_uvec4;
uniform 	vec4 _Offset;
UNITY_BINDING(0) uniform PerDraw {
	vec4 hlslcc_mtx4x4ObjectToClip[4];
};
in  vec4 in_POSITION0;
in  vec2 in_TEXCOORD0;
layout(location = 0) out vec2 vs_TEXCOORD0;
layout(location = 1) flat out uint vs_INDEX0;
void main()
{
    gl_Position = hlslcc_mtx4x4ObjectToClip[0] * in_POSITION0.xxxx + _Offset;
    vs_TEXCOORD0.xy = in_TEXCOORD0.xy;
    vs_INDEX0 = uint(gl_VertexID);
    return;
}

#endif
#ifdef FRAGMENT
#version 420
#extension GL_ARB_explicit_attrib_location : require
#extension GL_ARB_shading_language_420pack : require

""" + _MACROS + """precise vec4 u_xlat_precise_vec4;
UNITY_LOCATION(0) uniform  sampler2D _MainTex;
layout(location = 0) in  vec2 vs_TEXCOORD0;
layout(location = 1) flat in  uint vs_INDEX0;
layout(location = 0) out vec4 SV_Target0;
void main()
{
    SV_Target0 = texture(_MainTex, vs_TEXCOORD0.xy) * float(vs_INDEX0);
    gl_FragDepth = 0.5;
    return;
}

#endif
"""
# a storage buffer read in the vertex stage (procedural instancing): GLSL ES 3.10
PROGRAM_420_SSBO = PROGRAM_420.replace(
    '#extension GL_ARB_shading_language_420pack : require\n\n#define', '#extension GL_ARB_shading_language_420pack : require\n'
    '#extension GL_ARB_shader_storage_buffer_object : enable\n\n#define', 1).replace(
    'uniform 	vec4 _Offset;\n', 'layout(std430, binding = 0) readonly buffer Instances {\n\tvec4 Instances_buf[];\n};\n', 1).replace(
    '+ _Offset;', '+ Instances_buf[gl_InstanceID];', 1)


def _frag_line(program, old, new):
    a = program.index('#ifdef FRAGMENT')
    return program[:a] + program[a:].replace(old, new, 1)


class Glsl4xTest(unittest.TestCase):
    def test_es300(self):
        st = G.stage_sources(G.program_to_es(PROGRAM_420))
        v, f = st['VERTEX'], st['FRAGMENT']
        for src in (v, f):
            self.assertTrue(src.startswith('#version 300 es\nprecision highp float;\nprecision highp int;\n'), src[:80])
            self.assertNotIn('GL_ARB_', src)
            self.assertNotIn('precise', src)
            self.assertIn('#define UNITY_SUPPORTS_UNIFORM_LOCATION 1', src)      # switched by the player
        self.assertIn('\nout vec2 vs_TEXCOORD0;\nflat out uint vs_INDEX0;\n', v)  # matched by name in ES 3.00
        self.assertIn('\nin  vec2 vs_TEXCOORD0;\nflat in  uint vs_INDEX0;\nlayout(location = 0) out vec4 SV_Target0;\n', f)
        self.assertIn('UNITY_BINDING(0) uniform PerDraw {', v)
        self.assertIn('UNITY_LOCATION(0) uniform  sampler2D _MainTex;', f)
        self.assertIn('precision highp sampler2D;', f)

    def test_used_precise_is_es32(self):
        p = PROGRAM_420.replace('vec4 u_xlat0;', '', 1).replace('void main()\n{\n    gl_Position', 'precise vec4 u_xlat0;\nvoid main()\n{\n    u_xlat0 = _Offset;\n    gl_Position', 1)
        with self.assertRaisesRegex(G.Unsupported, "qualifier 'precise'"):
            G.program_to_es(p)
        # a dummy that is used is not dropped (and so not translatable)
        p = PROGRAM_420.replace('+ _Offset;', '+ _Offset + u_xlat_precise_vec4;', 1)
        with self.assertRaisesRegex(G.Unsupported, 'precise'):
            G.program_to_es(p)

    def test_ssbo_es310(self):
        with self.assertRaisesRegex(G.NeedsES31, 'std430'):
            G.program_to_es(PROGRAM_420_SSBO)
        st = G.stage_sources(G.program_to_es(PROGRAM_420_SSBO, es='310'))
        v, f = st['VERTEX'], st['FRAGMENT']
        self.assertTrue(v.startswith('#version 310 es\nprecision highp float;'))
        self.assertNotIn('GL_ARB_', v)
        self.assertIn('layout(std430, binding = 0) readonly buffer Instances {', v)
        self.assertIn('layout(location = 0) out vec2 vs_TEXCOORD0;', v)          # ES 3.10 keeps them
        self.assertIn('layout(location = 1) flat in  uint vs_INDEX0;', f)

    def test_explicit_binding_es310(self):
        p = _frag_line(PROGRAM_420, 'UNITY_LOCATION(0) uniform  sampler2D', 'layout(binding = 0) uniform  sampler2D')
        with self.assertRaisesRegex(G.NeedsES31, 'binding'):
            G.program_to_es(p)
        self.assertIn('layout(binding = 0) uniform  sampler2D _MainTex;', G.program_to_es(p, es='310'))

    def test_images(self):
        decl = 'layout(location = 0) out vec4 SV_Target0;\n'
        store = '    gl_FragDepth = 0.5;\n'
        p = _frag_line(_frag_line(PROGRAM_420, decl, decl + 'writeonly layout(binding=1, rgba8) uniform image2D _Marks;\n'),
                       store, store + '    imageStore(_Marks, ivec2(gl_FragCoord.xy), vec4(1.0));\n')
        with self.assertRaises(G.NeedsES31):
            G.program_to_es(p)
        f = G.stage_sources(G.program_to_es(p, es='310'))['FRAGMENT']
        self.assertIn('precision highp image2D;', f)                             # no default precision in ES
        self.assertIn('writeonly layout(binding=1, rgba8) uniform image2D _Marks;', f)
        # without a format (desktop GL can store to it, ES cannot)
        with self.assertRaisesRegex(G.Unsupported, "image without a format qualifier '_Marks'"):
            G.program_to_es(p.replace('binding=1, rgba8', 'binding=1'), es='310')

    def test_desktop_only(self):
        for old, new in (('uniform  sampler2D _MainTex;', 'uniform  samplerCubeArray _MainTex;'),
                         ('gl_FragDepth = 0.5;', 'gl_FragDepth = float(textureQueryLevels(_MainTex));'),
                         ('layout(location = 1) flat in  uint vs_INDEX0;', 'layout(location = 1, component = 0) flat in  uint vs_INDEX0;'),
                         ('layout(location = 0) in  vec2 vs_TEXCOORD0;', 'in Block { vec2 vs_TEXCOORD0; };'),
                         ('layout(location = 0) in  vec2 vs_TEXCOORD0;', 'layout(location = 0) noperspective in  vec2 vs_TEXCOORD0;'),
                         ('gl_FragDepth = 0.5;', 'gl_FragDepth = interpolateAtCentroid(vs_TEXCOORD0).x;')):
            p = _frag_line(PROGRAM_420, old, new)
            self.assertNotEqual(p, PROGRAM_420, old)
            with self.assertRaises(G.Unsupported, msg=new) as cm:
                G.program_to_es(p, es='310')
            self.assertNotIsInstance(cm.exception, G.NeedsES31, new)

    def test_glsl150_rules_unchanged(self):
        """The ES 3.10 features of GLSL 4.x stay Unsupported in a GLSL 1.50 program."""
        p = PROGRAM_2019.replace('UNITY_LOCATION(0) uniform  sampler2D', 'layout(binding = 0) uniform  sampler2D', 1)
        with self.assertRaisesRegex(G.Unsupported, 'layout qualifier') as cm:
            G.program_to_es(p, es='310')
        self.assertNotIsInstance(cm.exception, G.NeedsES31)


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


class GLCore4xConvertTest(unittest.TestCase):
    """GLCore41/43 programs: twins of their own type, a SubShader at a time; stripped shaders."""

    def _two_subshaders(self, prog43=PROGRAM_420_SSBO):
        """2021.2+: SubShader 0 with a GLCore43 program (`#pragma target 4.5`), SubShader 1 its
        GLCore32 fallback, each a pass with one tier-independent vertex subprogram."""
        sub = lambda i, t: {'m_BlobIndex': i, 'm_KeywordIndices': [], 'm_ShaderRequirements': 1, 'm_GpuProgramType': t}
        def prog(i, t, pi):
            return {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], [sub(i, t)]],
                    'm_ParameterBlobIndices': [[], [], [], [pi]], 'm_CommonParameters': {}}
        empty = {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], []], 'm_ParameterBlobIndices': [[], [], [], []]}
        p0 = {'m_Platforms': [15], 'progVertex': prog(2, 8, 0), 'progFragment': copy.deepcopy(empty)}
        p1 = {'m_Platforms': [15], 'progVertex': prog(3, 6, 1), 'progFragment': copy.deepcopy(empty)}
        es = [_params21(), _params21('Other'), _entry21(8, prog43.encode()), _entry21(6, PROGRAM_2019.encode())]
        tree = _tree({15: B.build_segments(es)}, {'m_Name': 'T', 'm_SubShaders': [{'m_Passes': [p0]}, {'m_Passes': [p1]}]},
                     True, stage_counts=[1])
        return tree, p0, p1, es

    def test_subshader_per_target(self):
        tree, p0, p1, es = self._two_subshaders()
        before = copy.deepcopy(tree)
        r = C.add_gles3(tree)
        self.assertEqual((r.status, r.programs, r.programs4x, r.es31, r.twins, r.failed), ('converted', 2, 1, 1, 2, []))
        self.assertEqual(tree['platforms'], [9, 15])
        self.assertEqual(tree['stageCounts'], [1, 1])
        self.assertEqual((p0['m_Platforms'], p1['m_Platforms']), ([9, 15], [9, 15]))
        v0, v1 = p0['progVertex'], p1['progVertex']
        self.assertEqual([(s['m_GpuProgramType'], s['m_BlobIndex']) for s in v0['m_PlayerSubPrograms'][3]], [(8, 2), (3, 2)])
        self.assertEqual([(s['m_GpuProgramType'], s['m_BlobIndex']) for s in v1['m_PlayerSubPrograms'][3]], [(6, 3), (4, 3)])
        self.assertEqual((v0['m_ParameterBlobIndices'][3], v1['m_ParameterBlobIndices'][3]), ([0, 0], [1, 1]))
        ours = C.platform_entries(tree, 9)
        self.assertEqual([type(e).__name__ for e in ours], ['ParamEntry', 'ParamEntry', 'Entry', 'Entry'])
        self.assertEqual([e.serialize() for e in ours[:2]], [e.serialize() for e in es[:2]])
        self.assertEqual((ours[2].gpu_type, ours[3].gpu_type), (3, 4))
        self.assertEqual((ours[2].rest, ours[3].rest), (es[2].rest, es[3].rest))
        self.assertTrue(ours[2].code.startswith(b'#ifdef VERTEX\n#version 310 es\n'))
        self.assertEqual([i for i, _ in C.gles3_programs(tree)], [2, 3])
        # GLCore32 programs only (the converter before; glcore4x off): SubShader 0 gets nothing
        old = copy.deepcopy(before)
        r = C.add_gles3(old, glcore4x=False)
        self.assertEqual((r.status, r.programs, r.twins), ('converted', 1, 1))
        self.assertEqual(len(old['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]['progVertex']['m_PlayerSubPrograms'][3]), 1)
        self.assertEqual([e.gpu_type for e in C.platform_entries(old, 9)[2:]], [8, 4])
        # rebuilt: the same
        self.assertEqual(C.add_gles3(tree, replace=True).status, 'converted')
        again = copy.deepcopy(before)
        C.add_gles3(again)
        self.assertEqual(tree['m_ParsedForm'], again['m_ParsedForm'])
        self.assertEqual(B.ShaderBlobs(tree).platforms, B.ShaderBlobs(again).platforms)

    def test_subshader_left_out(self):
        """A GLCore43 program with no ES equivalent: its SubShader gets no twins, the fallback does."""
        bad = _frag_line(PROGRAM_420, 'UNITY_LOCATION(0) uniform  sampler2D _MainTex;',
                         'UNITY_LOCATION(0) uniform  samplerCubeArray _MainTex;')
        tree, p0, p1, es = self._two_subshaders(bad)
        r = C.add_gles3(tree)
        self.assertEqual((r.status, r.programs, r.programs4x, r.twins), ('converted', 1, 0, 1))
        self.assertEqual([si for si, _ in r.failed], [0])
        self.assertIn('SubShader 0 left out', r.why)
        self.assertIn('samplerCubeArray', r.why)
        self.assertEqual((p0['m_Platforms'], p1['m_Platforms']), ([15], [9, 15]))
        self.assertEqual(len(p0['progVertex']['m_PlayerSubPrograms'][3]), 1)
        ours = C.platform_entries(tree, 9)
        self.assertEqual(ours[2].serialize(), es[2].serialize())      # left as it was: no GLES subprogram names it
        # with no fallback SubShader: the shader is left out
        tree, p0, p1, es = self._two_subshaders(bad)
        del tree['m_ParsedForm']['m_SubShaders'][1]
        before = copy.deepcopy(tree)
        r = C.add_gles3(tree)
        self.assertEqual((r.status, r.changed), ('skipped', False))
        self.assertTrue(r.why.startswith('SubShader 0: GLCore41/43 program 2: FRAGMENT: sampler type'), r.why)
        self.assertEqual(tree, before)

    def test_variant_targets_in_one_pass(self):
        """2019.1 - 2021.1: a pass whose instanced variant is GLCore43 (keyword-dependent target),
        the others GLCore32; the GL fragment subprograms name entries without code."""
        sub = lambda i, t, kw: {'m_BlobIndex': i, 'm_GpuProgramType': t, 'm_GlobalKeywordIndices': kw,
                                'm_LocalKeywordIndices': [], 'm_ShaderHardwareTier': 3}
        p = {'m_Platforms': [15], 'progVertex': {'m_SubPrograms': [sub(0, 6, []), sub(1, 8, [3])]},
             'progFragment': {'m_SubPrograms': [sub(2, 6, []), sub(3, 8, [3])]}}
        es = [_entry19(6, PROGRAM_2019.encode()), _entry19(8, PROGRAM_420_SSBO.encode(), ['PROCEDURAL_INSTANCING_ON']),
              _entry19(6, b''), _entry19(8, b'', ['PROCEDURAL_INSTANCING_ON'])]
        tree = _tree({15: B.build_segments(es, [0, 0, 1, 1])}, {'m_Name': 'T', 'm_SubShaders': [{'m_Passes': [p]}]}, True)
        r = C.add_gles3(tree)
        self.assertEqual((r.status, r.programs, r.programs4x, r.es31, r.twins), ('converted', 2, 1, 1, 4))
        self.assertEqual([(s['m_GpuProgramType'], s['m_BlobIndex']) for s in p['progVertex']['m_SubPrograms']],
                         [(6, 0), (8, 1), (4, 0), (3, 1)])
        self.assertEqual([(s['m_GpuProgramType'], s['m_BlobIndex']) for s in p['progFragment']['m_SubPrograms']],
                         [(6, 2), (8, 3), (4, 2), (3, 3)])
        sb = B.ShaderBlobs(tree)
        self.assertEqual([e.gpu_type for e in sb.entries(9)], [4, 3, 4, 3])
        self.assertEqual(sb.segments(9), [0, 0, 1, 1])
        self.assertEqual(p['m_Platforms'], [9, 15])

    def test_stripped_shader(self):
        """No GLCore program at all (every variant stripped from the build): an empty GLES3 blob."""
        tree = _tree({15: B.build_segments([]), 18: [b'\0' * 4]}, {'m_Name': 'T', 'm_SubShaders': [{'m_Passes': []}]},
                     True, stage_counts=[1, 1])
        before = copy.deepcopy(tree)
        r = C.add_gles3(tree, glcore4x=False)
        self.assertEqual((r.status, r.why), ('unchanged', 'no GLCore32 programs'))
        self.assertEqual(tree, before)
        r = C.add_gles3(tree)
        self.assertEqual((r.status, r.empty, r.twins), ('converted', True, 0))
        self.assertEqual(tree['platforms'], [9, 15, 18])
        self.assertEqual(tree['stageCounts'], [1, 1, 1])
        sb = B.ShaderBlobs(tree)
        self.assertEqual(sb.platforms[9], sb.platforms[15])

    def test_nothing_to_translate(self):
        """step.nothing_to_translate: an error only when every shader lacks the source programs."""
        from pmunity.shaders import step

        def res(why, plats):
            r = C.Result('S')
            r.why, r.platforms = why, plats
            return r
        vulkan = [res('no OpenGL Core programs', (18,)), res('no OpenGL Core programs', (18, 14))]
        err = step.nothing_to_translate(vulkan, ('glcore',))
        self.assertIn('none of the 2 shaders has OpenGL Core programs', err)
        self.assertIn('Metal, Vulkan', err)
        self.assertIsNone(step.nothing_to_translate(vulkan + [res('', (15,))], ('glcore',)))     # one translatable
        self.assertIsNone(step.nothing_to_translate([res('already has GLES3 programs', (9, 15))], ('glcore',)))
        self.assertIsNone(step.nothing_to_translate([], ('glcore',)))
        self.assertIsNone(step.nothing_to_translate([res('no OpenGL Core programs', ())], ('glcore',)))
        err = step.nothing_to_translate([res('no Direct3D 11 programs', (18,))], ('d3d11',))
        self.assertIn('Direct3D 11 programs (their programs: Vulkan)', err)

    def test_glcore4x_default(self):
        """step.glcore4x_for: on unless a port's config turns it off, for data of every version."""
        from pmunity.shaders import step
        self.assertTrue(step.glcore4x_for('any'))
        self.assertTrue(step.glcore4x_for('any', True))
        self.assertFalse(step.glcore4x_for('any', False))

if __name__ == '__main__':
    unittest.main()
