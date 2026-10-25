"""Unit tests of shaders.fused_post (fusedpost.py) on small hand-written programs (no game data):
python -m unittest tests.test_fusedpost   (from convert/)"""
import copy
import os
import struct
import sys
import unittest
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import dxbc
from pmunity.shaders import fusedpost as FP
from pmunity.shaders import glsl as G

UNITY = (6000, 0)

# HLSLcc's GLES3 form of a blit's vertex stage and of three post-processing passes shaped like
# Hollow Knight's (a bloom composite, colour curves, brightness/contrast)
VERT = '''#version 300 es
precision highp float;
precision highp int;
uniform 	vec4 hlslcc_mtx4x4unity_ObjectToWorld[4];
uniform 	vec4 hlslcc_mtx4x4unity_MatrixVP[4];
in  vec4 in_POSITION0;
in  vec2 in_TEXCOORD0;
out vec2 vs_TEXCOORD0;
vec4 u_xlat0;
void main()
{
    u_xlat0 = in_POSITION0.yyyy * hlslcc_mtx4x4unity_ObjectToWorld[1];
    u_xlat0 = hlslcc_mtx4x4unity_ObjectToWorld[0] * in_POSITION0.xxxx + u_xlat0;
    gl_Position = hlslcc_mtx4x4unity_MatrixVP[3] * u_xlat0.wwww + u_xlat0;
    vs_TEXCOORD0.xy = in_TEXCOORD0.xy;
    return;
}
'''
HEAD = '''#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;
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
'''
BLOOM = HEAD + '''UNITY_LOCATION(0) uniform  sampler2D _MainTex;
UNITY_LOCATION(1) uniform  sampler2D _Bloom;
in  vec2 vs_TEXCOORD0;
layout(location = 0) out vec4 SV_Target0;
vec4 u_xlat0;
vec4 u_xlat1;
void main()
{
    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy);
    u_xlat1 = texture(_Bloom, vs_TEXCOORD0.xy);
    SV_Target0 = u_xlat0 + u_xlat1;
    return;
}
'''
CURVES = HEAD + '''uniform 	float _Saturation;
UNITY_LOCATION(0) uniform  sampler2D _MainTex;
UNITY_LOCATION(1) uniform  sampler2D _RgbTex;
in  vec2 vs_TEXCOORD0;
layout(location = 0) out vec4 SV_Target0;
vec4 u_xlat0;
float u_xlat9;
void main()
{
    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy);
    u_xlat0.x = texture(_RgbTex, vec2(u_xlat0.x, 0.125)).x;
    u_xlat9 = dot(u_xlat0.xyz, vec3(0.219999999, 0.707000017, 0.0710000023));
    SV_Target0.xyz = vec3(_Saturation) * (u_xlat0.xyz - vec3(u_xlat9)) + vec3(u_xlat9);
    SV_Target0.w = u_xlat0.w;
    return;
}
'''
BRIGHT = HEAD + '''uniform 	float _Brightness;
uniform 	float _Contrast;
UNITY_LOCATION(0) uniform  sampler2D _MainTex;
in  vec2 vs_TEXCOORD0;
layout(location = 0) out vec4 SV_Target0;
vec4 u_xlat0;
void main()
{
    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy);
    u_xlat0 = u_xlat0 * vec4(_Brightness) + vec4(-0.5, -0.5, -0.5, -0.5);
    SV_Target0 = u_xlat0 * vec4(_Contrast) + vec4(0.5, 0.5, 0.5, 0.5);
    return;
}
'''
NAMES = ['Hidden/Bloom', 'Hidden/Curves', 'Custom/Bright']
CHAIN = {'name': 'PMUNITY_FUSED_POST', 'chain': NAMES}


def program(frag, vert=VERT):
    return '#ifdef VERTEX\n' + vert + '#endif\n#ifdef FRAGMENT\n' + frag + '#endif\n'


def fragment(code):
    return next(code[a:b] for st, a, b in G.stage_blocks(code) if st == 'FRAGMENT')


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


def shader(name, code, passes=1):
    """A 2021.2+ Shader with GLES3 programs only: `passes` passes of one program each (entries: the
    parameters as Unity's GL compiler writes them, then the program)."""
    entries = []
    plist = []
    for k in range(passes):
        pi = len(entries)
        entries.append(B.ParamEntry(202012090, dxbc.gl_bindings(code, UNITY).params_bytes()))
        entries.append(B.Entry(202012090, B.GPU_GLES3, [0, 0, 0, 0], [], code.encode(), struct.pack('<2i', 0, 0)))
        sub = {'m_BlobIndex': pi + 1, 'm_KeywordIndices': [], 'm_ShaderRequirements': 33, 'm_GpuProgramType': B.GPU_GLES3}
        plist.append({'m_Type': 0, 'm_TextureName': '', 'm_Name': '', 'm_UseName': '', 'm_Platforms': [9],
                      'm_State': {'m_Name': ''}, 'm_Tags': {'tags': []},
                      'progVertex': {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], [sub]],
                                     'm_ParameterBlobIndices': [[], [], [], [pi]], 'm_CommonParameters': {}},
                      'progFragment': {'m_SubPrograms': [], 'm_PlayerSubPrograms': [[], [], [], []], 'm_ParameterBlobIndices': [[], [], [], []]}})
    parsed = {'m_Name': name, 'm_SubShaders': [{'m_Passes': plist, 'm_Tags': {'tags': []}, 'm_LOD': 0}]}
    return _tree({9: entries}, parsed, [1])


def trees():
    return {n: shader(n, program(f)) for n, f in zip(NAMES, (BLOOM, CURVES, BRIGHT))}


class FuseTest(unittest.TestCase):
    def test_fused_program(self):
        code = FP.fuse_program([program(BLOOM), program(CURVES), program(BRIGHT)], NAMES)
        vert = next(code[a:b] for st, a, b in G.stage_blocks(code) if st == 'VERTEX')
        self.assertEqual(vert, VERT)                                              # the last pass's vertex stage
        f = fragment(code)
        for decl in ('UNITY_LOCATION(0) uniform sampler2D _MainTex;', 'UNITY_LOCATION(1) uniform sampler2D _Bloom;',
                     'UNITY_LOCATION(2) uniform sampler2D _RgbTex;', '#define UNITY_LOCATION(x) layout(location = x)',
                     'uniform float _Saturation;', 'uniform float _Brightness;',
                     'uniform float _Contrast;', 'in vec2 vs_TEXCOORD0;', 'layout(location = 0) out vec4 SV_Target0;',
                     'precision highp sampler2D;', 'vec4 pmunity_pass0()', 'vec4 pmunity_pass1(vec4 pmunity_in)',
                     'vec4 pmunity_pass2(vec4 pmunity_in)', FP.MARK):
            self.assertEqual(f.count(decl), 1, decl)
        self.assertEqual(f.count('texture(_MainTex'), 1)                          # the source read once, by the first pass
        self.assertEqual(f.count('u_xlat0 = pmunity_in;'), 2)                     # the later passes read the pass before
        self.assertEqual(f.count('pmunity_c = pmunity_store(pmunity_c);'), 2)     # a store between each two passes
        self.assertNotIn('SV_Target0.', f)                                        # each pass writes its own local
        self.assertIn('    SV_Target0 = pmunity_c;', f)
        self.assertEqual(f.count('vec4 pmunity_out;'), 3)
        self.assertEqual(f.count('return pmunity_out;'), 3)
        none = fragment(FP.fuse_program([program(BLOOM), program(CURVES), program(BRIGHT)], NAMES, quantize='none'))
        self.assertNotIn(FP.STORE_FN, none)
        self.assertIn('pmunity_c = pmunity_pass1(pmunity_c);', none)
        # the binding info of the fused program names every uniform and texture once
        info = dxbc.gl_bindings(code, UNITY)
        self.assertLessEqual({'_Brightness', '_Contrast', '_Saturation'},
                             {p.name for cb in info.cbuffers for p in cb.params if cb.name == '$Globals'})
        self.assertEqual([b.name for b in info.bindings if b.kind == dxbc.Binding.KIND_TEXTURE], ['_MainTex', '_Bloom', '_RgbTex'])

    def test_two_passes(self):
        f = fragment(FP.fuse_program([program(CURVES), program(BRIGHT)], NAMES[1:]))
        self.assertEqual(f.count('pmunity_store'), 2)                             # the definition and one store
        self.assertIn('UNITY_LOCATION(1) uniform sampler2D _RgbTex;', f)

    def refused(self, codes, text, names=NAMES):
        with self.assertRaises(FP.NotFusable) as cm:
            FP.fuse_program(codes, names[:len(codes)])
        self.assertIn(text, str(cm.exception))

    def test_refused(self):
        bloom, curves, bright = program(BLOOM), program(CURVES), program(BRIGHT)
        self.refused([bloom, program(CURVES.replace('texture(_MainTex, vs_TEXCOORD0.xy)', 'texture(_MainTex, vs_TEXCOORD0.xy + vec2(0.001, 0.0))')), bright],
                     'at its own pixel')
        self.refused([bloom, program(CURVES.replace('u_xlat0.w;', 'u_xlat0.w + texture(_MainTex, vs_TEXCOORD0.yx).w;')), bright],
                     'other than at its own pixel')
        self.refused([bloom, program(CURVES.replace('texture(_MainTex, vs_TEXCOORD0.xy)', 'textureLod(_MainTex, vs_TEXCOORD0.xy, 0.0)')), bright],
                     'does not read _MainTex at its own pixel')
        self.refused([bloom, program(CURVES.replace('u_xlat0.w;', 'u_xlat0.w * gl_FragCoord.x;')), bright], 'uses gl_FragCoord')
        self.refused([bloom, curves, program(BRIGHT.replace('u_xlat0 = u_xlat0 *', 'u_xlat0 = dFdx(u_xlat0) *'))], 'uses dFdx')
        self.refused([bloom, curves, program(BRIGHT.replace('uniform \tfloat _Contrast;', 'uniform \tfloat _Contrast;\nuniform \tfloat _Saturation;'))],
                     "_Saturation is also")
        self.refused([bloom, curves, program(BRIGHT.replace('UNITY_LOCATION(0) uniform  sampler2D _MainTex;',
                                                            'uniform \tvec4 _MainTex_TexelSize;\nUNITY_LOCATION(0) uniform  sampler2D _MainTex;'))],
                     'reads _MainTex_TexelSize')
        self.refused([bloom, curves, program(BRIGHT, VERT.replace('vs_TEXCOORD0.xy = in_TEXCOORD0.xy;', 'vs_TEXCOORD0.xy = in_TEXCOORD0.yx;'))],
                     'vertex stage differs')
        self.refused([program(BLOOM, VERT.replace('vs_TEXCOORD0.xy = in_TEXCOORD0.xy;', 'vs_TEXCOORD0.xy = in_TEXCOORD0.yx;'))] * 3,
                     'does not pass the quad')
        self.refused([bloom, program(CURVES.replace('uniform \tfloat _Saturation;', 'UNITY_BINDING(0) uniform PerFrame {\n\tfloat _Saturation;\n};')), bright],
                     'uniform block')
        self.refused([program(BLOOM.replace('UNITY_LOCATION(0) uniform  sampler2D _MainTex;', '').replace('texture(_MainTex', 'texture(_Bloom')), curves, bright],
                     'no _MainTex sampler')
        self.refused([bloom, curves, program(BRIGHT.replace('layout(location = 0) out vec4 SV_Target0;',
                                                            'layout(location = 0) out vec4 SV_Target0;\nlayout(location = 1) out vec4 SV_Target1;'))],
                     'more than one output')
        self.refused([bloom, curves, program(BRIGHT.replace('precision highp sampler2D;', 'precision mediump sampler2D;'))], 'precision statements differ')
        self.refused([bloom, curves, '#ifdef FRAGMENT\n' + BRIGHT + '#endif\n'], 'not a vertex + fragment program')


class DataEditTest(unittest.TestCase):
    def test_apply(self):
        ts = trees()
        before = copy.deepcopy(ts)
        r = FP.apply(ts, CHAIN, UNITY)
        self.assertEqual(r.status, 'changed', r.why)
        self.assertEqual((r.shader, r.pass_index, r.programs, r.name), (NAMES[-1], 1, 1, 'PMUNITY_FUSED_POST'))
        for n in NAMES[:-1]:
            self.assertEqual(ts[n], before[n])                                    # the other shaders untouched
        t = ts[NAMES[-1]]
        passes = t['m_ParsedForm']['m_SubShaders'][0]['m_Passes']
        self.assertEqual(len(passes), 2)
        self.assertEqual(passes[0], before[NAMES[-1]]['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0])
        p = passes[1]
        self.assertEqual((p['m_State']['m_Name'], p['m_Platforms']), ('PMUNITY_FUSED_POST', [9]))
        self.assertEqual([(s['m_GpuProgramType'], s['m_BlobIndex']) for s in p['progVertex']['m_PlayerSubPrograms'][3]], [(B.GPU_GLES3, 2)])
        self.assertEqual(p['progVertex']['m_ParameterBlobIndices'][3], [3])
        ours = C.platform_entries(t, 9)
        old = C.platform_entries(before[NAMES[-1]], 9)
        self.assertEqual([e.serialize() for e in ours[:2]], [e.serialize() for e in old])
        self.assertEqual(ours[2].code.decode(), r.code)
        self.assertEqual(ours[2].rest, old[1].rest)
        self.assertIsInstance(ours[3], B.ParamEntry)
        self.assertEqual(ours[3].data, dxbc.gl_bindings(r.code, UNITY).params_bytes())
        self.assertEqual(ours[3].version, old[0].version)
        self.assertEqual(fragment(r.code), FP.fuse_fragment([FP.Fragment(fragment(program(f)), f'"{n}" pass 0') for n, f in zip(NAMES, (BLOOM, CURVES, BRIGHT))]))
        # done before: left alone
        r2 = FP.apply(ts, CHAIN, UNITY)
        self.assertEqual(r2.status, 'unchanged')
        self.assertIn('already', r2.why)
        self.assertEqual(len(passes), 2)

    def test_quantize_none_and_pass_choice(self):
        ts = trees()
        ts[NAMES[0]] = shader(NAMES[0], program(BLOOM), passes=3)
        chain = {'name': 'FUSED', 'chain': [NAMES[0] + ':2', NAMES[1], NAMES[2]], 'quantize': 'none'}
        r = FP.apply(ts, chain, UNITY)
        self.assertEqual(r.status, 'changed', r.why)
        self.assertNotIn(FP.STORE_FN, r.code)
        self.assertEqual(ts[NAMES[-1]]['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][1]['m_State']['m_Name'], 'FUSED')
        self.assertIn('pass 2', r.why)

    def test_skipped(self):
        ts = trees()
        del ts[NAMES[1]]
        r = FP.apply(ts, CHAIN, UNITY)
        self.assertEqual((r.status, r.why), ('skipped', 'not in this file: ' + NAMES[1]))
        ts = trees()
        r = FP.apply(ts, {'name': 'X', 'chain': [NAMES[0] + ':4', NAMES[2]]}, UNITY)
        self.assertEqual(r.status, 'skipped')
        self.assertIn('no such pass', r.why)
        ts = trees()
        ts[NAMES[1]] = shader(NAMES[1], program(CURVES.replace('texture(_MainTex, vs_TEXCOORD0.xy)', 'texture(_MainTex, vs_TEXCOORD0.yx)')))
        r = FP.apply(ts, CHAIN, UNITY)
        self.assertEqual(r.status, 'skipped')
        self.assertIn('does not read _MainTex at its own pixel', r.why)
        for n in NAMES:                                                           # nothing changed
            self.assertEqual(len(ts[n]['m_ParsedForm']['m_SubShaders'][0]['m_Passes']), 1)
        # a parameter entry this module does not reproduce: the layout is not understood, nothing changes
        ts = trees()
        t = ts[NAMES[-1]]
        blobs = B.ShaderBlobs(t)
        es = C.platform_entries(t, 9, blobs)
        es[0] = B.ParamEntry(es[0].version, es[0].data + b'\0\0\0\0')
        blobs.set_entries(9, es, blobs.segments(9))
        blobs.store()
        r = FP.apply(ts, CHAIN, UNITY)
        self.assertEqual(r.status, 'skipped')
        self.assertIn('layout not understood', r.why)

    def test_config(self):
        self.assertEqual(FP.chains_of({}), [])
        self.assertEqual(FP.chains_of({'shaders': {}}), [])
        chains = FP.chains_of({'shaders': {'fused_post': [CHAIN]}})
        self.assertEqual(FP.chain_items(chains[0]), [(n, 0) for n in NAMES])
        self.assertEqual(FP.chain_items({'chain': ['A:3', 'B']}), [('A', 3), ('B', 0)])
        self.assertEqual(FP.chain_names([CHAIN, {'chain': ['A:3', 'B']}]), set(NAMES) | {'A', 'B'})
        for bad in ({'shaders': {'fused_post': 'x'}}, {'shaders': {'fused_post': [{'chain': ['A']}]}},
                    {'shaders': {'fused_post': [{'chain': ['A', 'B'], 'quantize': 'rgba16'}]}},
                    {'shaders': {'fused_post': [{'chain': ['A', 'B'], 'name': 'has space'}]}}):
            with self.assertRaises(ValueError):
                FP.chains_of(bad)


if __name__ == '__main__':
    unittest.main()
