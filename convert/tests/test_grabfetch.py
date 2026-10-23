"""Unit tests of shaders.grab_fetch (grabfetch.py) on small hand-written programs (no game data):
python -m unittest tests.test_grabfetch   (from convert/)"""
import copy
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import glsl as G
from pmunity.shaders import grabfetch as GF
from pmunity.shaders import step

# HLSLcc's form of a sprite that blends against the picture under it: the clip position from two
# matrices, a ComputeGrabScreenPos-style varying, the fragment stage dividing it by w
VERT = '''#version 150
#extension GL_ARB_explicit_attrib_location : require

uniform 	vec4 hlslcc_mtx4x4unity_ObjectToWorld[4];
uniform 	vec4 hlslcc_mtx4x4unity_MatrixVP[4];
in  vec4 in_POSITION0;
in  vec2 in_TEXCOORD0;
out vec2 vs_TEXCOORD0;
out vec4 vs_TEXCOORD1;
vec4 u_xlat0;
vec4 u_xlat1;
void main()
{
    u_xlat0 = in_POSITION0.yyyy * hlslcc_mtx4x4unity_ObjectToWorld[1];
    u_xlat0 = hlslcc_mtx4x4unity_ObjectToWorld[0] * in_POSITION0.xxxx + u_xlat0;
    u_xlat0 = hlslcc_mtx4x4unity_ObjectToWorld[2] * in_POSITION0.zzzz + u_xlat0;
    u_xlat0 = u_xlat0 + hlslcc_mtx4x4unity_ObjectToWorld[3];
    u_xlat1 = u_xlat0.yyyy * hlslcc_mtx4x4unity_MatrixVP[1];
    u_xlat1 = hlslcc_mtx4x4unity_MatrixVP[0] * u_xlat0.xxxx + u_xlat1;
    u_xlat1 = hlslcc_mtx4x4unity_MatrixVP[2] * u_xlat0.zzzz + u_xlat1;
    u_xlat0 = hlslcc_mtx4x4unity_MatrixVP[3] * u_xlat0.wwww + u_xlat1;
    gl_Position = u_xlat0;
@VARYING@
    vs_TEXCOORD0.xy = in_TEXCOORD0.xy;
    return;
}
'''
GRAB_SCREEN_POS = '''    u_xlat1.xy = u_xlat0.ww + u_xlat0.xy;
    vs_TEXCOORD1.zw = u_xlat0.zw;
    vs_TEXCOORD1.xy = u_xlat1.xy * vec2(0.5, 0.5);'''
CLIP_POS = '''    vs_TEXCOORD1 = u_xlat0;'''

FRAG = '''#version 150
#extension GL_ARB_explicit_attrib_location : require

uniform 	vec4 _GrabTexture_TexelSize;
uniform  sampler2D _MainTex;
uniform  sampler2D _GrabTexture;
in  vec2 vs_TEXCOORD0;
in  vec4 vs_TEXCOORD1;
layout(location = 0) out vec4 SV_Target0;
vec4 u_xlat0;
vec4 u_xlat1;
bool u_xlatb2;
void main()
{
    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy);
    u_xlatb2 = u_xlat0.w<0.0099999998;
    if(u_xlatb2){discard;}
@UV@
    u_xlat1 = texture(_GrabTexture, u_xlat1.xy);
    SV_Target0.w = u_xlat0.w;
    SV_Target0.xyz = u_xlat0.xyz * u_xlat1.xyz + u_xlat1.xyz;
    return;
}
'''
UV_DIVIDED = '''    u_xlat1.xy = vs_TEXCOORD1.xy / vs_TEXCOORD1.ww;'''
UV_NDC = '''    u_xlat1.xy = vs_TEXCOORD1.xy / vs_TEXCOORD1.ww;
    u_xlat1.xy = u_xlat1.xy + vec2(1.0, 1.0);
    u_xlat1.xy = u_xlat1.xy * vec2(0.5, 0.5);'''


def program(varying=GRAB_SCREEN_POS, uv=UV_DIVIDED, frag=FRAG, vert=VERT):
    gl = '#ifdef VERTEX\n' + vert.replace('@VARYING@', varying) + '\n#endif\n#ifdef FRAGMENT\n' + \
        frag.replace('@UV@', uv) + '\n#endif\n'
    return G.program_to_es(gl)


def stages(code):
    return G.stage_sources(code)


class ScreenPositionTest(unittest.TestCase):
    def ok(self, code):
        out = GF.rewrite_program(code)
        f = stages(out)['FRAGMENT']
        self.assertNotIn('texture(_GrabTexture', f)
        self.assertIn('u_xlat1 = ua_FramebufferColor;', f)
        self.assertEqual(stages(out)['VERTEX'], stages(code)['VERTEX'])
        return f

    def refused(self, code, why):
        with self.assertRaises(GF.NotScreenPosition) as cm:
            GF.rewrite_program(code)
        self.assertIn(why, str(cm.exception))

    def test_grab_screen_pos(self):
        f = self.ok(program())
        self.assertTrue(f.startswith('#version 300 es\n' + GF.MARK + '\n#ifdef GL_EXT_shader_framebuffer_fetch\n'))
        self.assertIn('#ifdef GL_EXT_shader_framebuffer_fetch\nlayout(location = 0) inout vec4 SV_Target0;\n#else\n'
                      'layout(location = 0) out vec4 SV_Target0;\n#endif', f)
        main = f[f.index('void main()'):]
        self.assertTrue(main.startswith('void main()\n{\n#ifdef GL_EXT_shader_framebuffer_fetch\n'
                                        '    vec4 ua_FramebufferColor = SV_Target0;\n#else\n'
                                        '    vec4 ua_FramebufferColor = gl_LastFragColorARM;\n#endif\n'), main)
        self.assertIn('uniform  sampler2D _GrabTexture;', f)        # (declared, unused: Unity's bindings unchanged)

    def test_clip_position_divided_in_the_fragment_stage(self):
        self.ok(program(CLIP_POS, UV_NDC))

    def test_texture_proj(self):
        code = program().replace('    u_xlat1.xy = vs_TEXCOORD1.xy / vs_TEXCOORD1.ww;\n'
                                 '    u_xlat1 = texture(_GrabTexture, u_xlat1.xy);',
                                 '    u_xlat1 = textureProj(_GrabTexture, vs_TEXCOORD1);')
        self.assertIn('textureProj(_GrabTexture, vs_TEXCOORD1)', code)
        self.assertIn('u_xlat1 = ua_FramebufferColor;', stages(GF.rewrite_program(code))['FRAGMENT'])

    def test_offset_refused(self):
        self.refused(program(uv=UV_DIVIDED + '\n    u_xlat1.xy = u_xlat1.xy + _GrabTexture_TexelSize.xy;'),
                     'not the fragment\'s screen position')
        self.refused(program(uv=UV_DIVIDED + '\n    u_xlat1.x = u_xlat1.x + 0.00999999978;'), 'screen position')

    def test_flipped_refused(self):
        self.refused(program(uv=UV_DIVIDED + '\n    u_xlat1.y = (-u_xlat1.y) + 1.0;'), 'its v is not the fragment')

    def test_scaled_refused(self):
        self.refused(program(uv=UV_DIVIDED + '\n    u_xlat1.xy = u_xlat1.xy * vec2(0.5, 0.5);'), 'its u is not the fragment')

    def test_object_position_refused(self):
        self.refused(program('    vs_TEXCOORD1 = in_POSITION0;'), 'not a linear function of gl_Position')

    def test_constant_term_refused(self):
        v = '''    vs_TEXCOORD1.xy = u_xlat0.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5);
    vs_TEXCOORD1.zw = u_xlat0.zw;'''
        self.refused(program(v), 'constant term')

    def test_ndc_divided_in_the_vertex_stage_refused(self):
        v = '''    vs_TEXCOORD1.xy = u_xlat0.xy / u_xlat0.ww;
    vs_TEXCOORD1.zw = vec2(1.0, 1.0);'''
        self.refused(program(v, UV_NDC), 'varying vs_TEXCOORD1')

    def test_conditional_uv_refused(self):
        uv = UV_DIVIDED + '\n    if(u_xlatb2){u_xlat1.x = 0.5;}'
        self.refused(program(uv=uv), 'screen position')

    def test_branch_that_keeps_the_value(self):
        uv = '    if(u_xlatb2){u_xlat0.x = 0.5;} else {u_xlat0.x = 0.25;}\n' + UV_DIVIDED
        self.ok(program(uv=uv))

    def test_loop_assigned_uv_refused(self):
        # (a loop with literal bounds is written out by the converter's unroll: a bound it does not know)
        uv = UV_DIVIDED + ('\n    for(int i = 0 ; i<int(_GrabTexture_TexelSize.z) ; i++)\n    {\n'
                           '        u_xlat1.x = u_xlat1.x + 0.0;\n    }')
        self.refused(program(uv=uv), 'screen position')

    def test_other_use_refused(self):
        uv = UV_DIVIDED + '\n    u_xlat0.xy = vec2(textureSize(_GrabTexture, 0));'
        self.refused(program(uv=uv), 'not a plain sample')

    def test_two_outputs_refused(self):
        code = program().replace('layout(location = 0) out vec4 SV_Target0;',
                                 'layout(location = 0) out vec4 SV_Target0;\nlayout(location = 1) out vec4 SV_Target1;')
        self.refused(code, 'exactly one vec4 output')

    def test_adreno_block_in_main(self):
        """Unity's own GLES3 programs: #ifdef UNITY_ADRENO_ES3 blocks between statements."""
        uv = UV_DIVIDED + '\n#ifdef UNITY_ADRENO_ES3\n    u_xlat0.w = min(max(u_xlat0.w, 0.0), 1.0);\n#else\n' \
                          '    u_xlat0.w = clamp(u_xlat0.w, 0.0, 1.0);\n#endif'
        self.ok(program(uv=uv))
        uv = UV_DIVIDED + '\n#ifdef UNITY_ADRENO_ES3\n    u_xlat1.x = min(max(u_xlat1.x, 0.0), 1.0);\n#endif'
        self.refused(program(uv=uv), 'screen position')


def _entry21(gpu_type, code, keywords=()):
    return B.Entry(202012090, gpu_type, [0, 0, 0, 0], list(keywords), code, struct.pack('<2i', 9, 0))


def _params21():
    return B.ParamEntry(202012090, struct.pack('<i', 0) * 6)


def _tree(entries_by_platform, parsed, stage_counts):
    import lz4.block
    blob, offs, cl, dl = bytearray(), [], [], []
    for entries in entries_by_platform.values():
        o, c, d = [], [], []
        for seg in B.build_segments(entries):
            z = lz4.block.compress(seg, store_size=False)
            o.append(len(blob)); c.append(len(z)); d.append(len(seg))
            blob += z
        offs.append(o); cl.append(c); dl.append(d)
    return {'m_ParsedForm': parsed, 'platforms': list(entries_by_platform), 'offsets': offs, 'compressedLengths': cl,
            'decompressedLengths': dl, 'compressedBlob': bytes(blob), 'stageCounts': stage_counts}


def _pass(kind, subs=(), pbi=(), platforms=(15,), texture=''):
    vert = {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], list(subs)],
            'm_ParameterBlobIndices': [[], [], [], list(pbi)], 'm_CommonParameters': {}}
    frag = {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], []], 'm_ParameterBlobIndices': [[], [], [], []]}
    return {'m_Type': kind, 'm_TextureName': texture, 'm_Name': '', 'm_UseName': '', 'm_Platforms': list(platforms),
            'progVertex': vert if subs else {}, 'progFragment': frag if subs else {}}


def _sub(i, kw=(), t=6):
    return {'m_BlobIndex': i, 'm_KeywordIndices': list(kw), 'm_ShaderRequirements': 33, 'm_GpuProgramType': t}


def blend_shader(gl_code, texture=''):
    """A 2021.2+ Shader: SubShader 0 = [GrabPass, a pass sampling _GrabTexture], two variants,
    GLCore programs only (then add_gles3 gives it GLES3 twins)."""
    gl = gl_code.encode()
    es = [_params21(), _entry21(6, gl), _entry21(6, gl.replace(b'SV_Target0.w = u_xlat0.w;', b'SV_Target0.w = 1.0;'),
                                                 ['UNITY_UI_CLIP_RECT'])]
    p = _pass(0, [_sub(1), _sub(2, [0])], [0, 0], platforms=[15])
    parsed = {'m_Name': 'Test/Screen', 'm_SubShaders': [{'m_Passes': [_pass(GF.GRAB_PASS, platforms=[], texture=texture), p],
                                                          'm_Tags': {'tags': []}, 'm_LOD': 0}]}
    t = _tree({15: es}, parsed, [1])
    r = C.add_gles3(t)
    assert r.status == 'converted', r
    return t


def gl_program(varying=GRAB_SCREEN_POS, uv=UV_DIVIDED):
    return '#ifdef VERTEX\n' + VERT.replace('@VARYING@', varying) + '\n#endif\n#ifdef FRAGMENT\n' + \
        FRAG.replace('@UV@', uv) + '\n#endif\n'


class DataEditTest(unittest.TestCase):
    def test_copy_in_front(self):
        t = blend_shader(gl_program())
        before = copy.deepcopy(t)
        r = GF.apply(t, True)
        self.assertEqual((r.status, r.subshaders, r.grabs, r.programs), ('changed', 1, 1, 2))
        subs = t['m_ParsedForm']['m_SubShaders']
        self.assertEqual(len(subs), 2)
        self.assertEqual(subs[1], before['m_ParsedForm']['m_SubShaders'][0])     # the original, as it was
        cp = subs[0]
        self.assertEqual([p['m_Type'] for p in cp['m_Passes']], [0])             # no GrabPass
        p = cp['m_Passes'][0]
        self.assertEqual(p['m_Platforms'], [9])
        tier = p['progVertex']['m_PlayerSubPrograms'][3]
        self.assertEqual([(s['m_GpuProgramType'], s['m_BlobIndex'], s['m_ShaderRequirements']) for s in tier],
                         [(4, 3, 33 | GF.FBFETCH), (4, 4, 33 | GF.FBFETCH)])
        self.assertEqual(p['progVertex']['m_ParameterBlobIndices'][3], [0, 0])
        self.assertEqual(t['stageCounts'], before['stageCounts'])
        ours = C.platform_entries(t, 9)
        old = C.platform_entries(before, 9)
        self.assertEqual(len(ours), len(old) + 2)
        self.assertEqual([e.serialize() for e in ours[:3]], [e.serialize() for e in old])     # entries kept
        self.assertEqual(ours[4].keywords, ['UNITY_UI_CLIP_RECT'])
        self.assertEqual(ours[3].code, GF.rewrite_program(old[1].code.decode()).encode())
        self.assertEqual(B.ShaderBlobs(t).platforms[15], B.ShaderBlobs(before).platforms[15])
        self.assertEqual(GF.apply(t, True).status, 'unchanged')                 # done before: left alone

    def test_always(self):
        t = blend_shader(gl_program())
        r = GF.apply(t, 'always')
        self.assertEqual(r.status, 'changed')
        tier = t['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]['progVertex']['m_PlayerSubPrograms'][3]
        self.assertEqual([s['m_ShaderRequirements'] for s in tier], [33, 33])

    def test_named_grab_pass_left(self):
        t = blend_shader(gl_program(), texture='_SharedGrab')
        before = copy.deepcopy(t)
        r = GF.apply(t, True)
        self.assertEqual(r.status, 'skipped')
        self.assertIn('named GrabPass', r.why)
        self.assertEqual(t, before)

    def test_offset_left(self):
        t = blend_shader(gl_program(uv=UV_DIVIDED + '\n    u_xlat1.x = u_xlat1.x + _GrabTexture_TexelSize.x;'))
        before = copy.deepcopy(t)
        r = GF.apply(t, True)
        self.assertEqual(r.status, 'skipped')
        self.assertIn('program 1', r.why)
        self.assertEqual(t, before)

    def test_no_grab_pass_or_no_gles3(self):
        t = blend_shader(gl_program())
        t['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]['m_Type'] = 0
        self.assertEqual((GF.apply(t, True).status, GF.apply(t, True).why), ('unchanged', 'no GrabPass'))
        u = blend_shader(gl_program())
        u['platforms'] = [15]
        self.assertEqual(GF.apply(u, True).status, 'skipped')

    def test_unread_grab_pass(self):
        """A GrabPass whose copy no pass samples: left out of the copy, nothing to rewrite."""
        gl = gl_program().replace('    u_xlat1 = texture(_GrabTexture, u_xlat1.xy);', '    u_xlat1 = vec4(0.5);') \
                         .replace('uniform  sampler2D _GrabTexture;\n', '')
        t = blend_shader(gl)
        r = GF.apply(t, True)
        self.assertEqual((r.status, r.grabs, r.programs), ('changed', 1, 0))
        tier = t['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]['progVertex']['m_PlayerSubPrograms'][3]
        self.assertEqual([(s['m_BlobIndex'], s['m_ShaderRequirements']) for s in tier], [(1, 33), (2, 33)])

    def test_reader_not_after_its_grab_pass(self):
        t = blend_shader(gl_program())
        ss = t['m_ParsedForm']['m_SubShaders'][0]
        ss['m_Passes'].insert(1, copy.deepcopy(ss['m_Passes'][1]))
        ss['m_Passes'][1]['m_Type'] = 0
        r = GF.apply(t, True)
        self.assertEqual(r.status, 'skipped')
        self.assertIn('pass 2 samples _GrabTexture without a GrabPass right before it', r.why)

    def test_names(self):
        t = blend_shader(gl_program())
        self.assertEqual(GF.apply(t, True, names=r'^Other/').status, 'unchanged')
        self.assertEqual(GF.apply(t, True, names=r'^Test/').status, 'changed')

    def test_modes(self):
        self.assertEqual([step.grab_fetch_mode(v) for v in (None, False, 'off', True, 'on', 'always')],
                         [False, False, False, True, True, 'always'])
        with self.assertRaises(ValueError):
            step.grab_fetch_mode('yes')


class OlderLayoutTest(unittest.TestCase):
    def test_2019_vertex_and_fragment_subprograms(self):
        """Up to 2019.x: a vertex and a fragment subprogram per variant, the program in the vertex
        one's entry, the fragment one's entry without code (it keeps pointing there)."""
        gl = gl_program().encode()
        e = lambda code: B.Entry(201806140, 6, [0, 0, 0, 0], [], code, struct.pack('<2i', 9, 0), [])
        sub = lambda i: {'m_BlobIndex': i, 'm_GpuProgramType': 6, 'm_GlobalKeywordIndices': [], 'm_LocalKeywordIndices': [],
                         'm_ShaderHardwareTier': 3, 'm_ShaderRequirements': 33}
        p = {'m_Type': 0, 'm_TextureName': '', 'm_Platforms': [15], 'progVertex': {'m_SubPrograms': [sub(0)]},
             'progFragment': {'m_SubPrograms': [sub(1)]}}
        g = {'m_Type': GF.GRAB_PASS, 'm_TextureName': '', 'm_Platforms': [], 'progVertex': {'m_SubPrograms': []},
             'progFragment': {'m_SubPrograms': []}}
        import lz4.block
        segs = B.build_segments([e(gl), e(b'')])
        z = [lz4.block.compress(s, store_size=False) for s in segs]
        t = {'m_ParsedForm': {'m_Name': 'T', 'm_SubShaders': [{'m_Passes': [g, p]}]}, 'platforms': [15],
             'offsets': [[0]], 'compressedLengths': [[len(z[0])]], 'decompressedLengths': [[len(segs[0])]],
             'compressedBlob': z[0]}
        self.assertEqual(C.add_gles3(t).status, 'converted')
        r = GF.apply(t, True)
        self.assertEqual((r.status, r.programs), ('changed', 1))
        cp = t['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]
        self.assertEqual([(s['m_GpuProgramType'], s['m_BlobIndex'], s['m_ShaderRequirements'])
                          for s in cp['progVertex']['m_SubPrograms']], [(4, 2, 33 | GF.FBFETCH)])
        self.assertEqual([(s['m_GpuProgramType'], s['m_BlobIndex']) for s in cp['progFragment']['m_SubPrograms']], [(4, 1)])
        ours = C.platform_entries(t, 9)
        self.assertEqual((len(ours), ours[1].code, ours[2].local_keywords), (3, b'', []))


if __name__ == '__main__':
    unittest.main()
