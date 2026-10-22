"""Unit tests of the Metal path (metal.py) on hand-written MSL in the form Unity's shader compiler
writes it (no Unity or game files): python -m unittest discover -s tests   (from convert/)"""
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
from pmunity.shaders import metal as M

VS = '''#include <metal_stdlib>
#include <metal_texture>
using namespace metal;
struct VGlobals_Type
{
    float4 hlslcc_mtx4x4unity_ObjectToWorld[4];
    float4 hlslcc_mtx4x4unity_MatrixVP[4];
    float4 _Color;
    float4 _Unused;
};

struct Mtl_VertexIn
{
    float4 POSITION0 [[ attribute(0) ]] ;
    float4 COLOR0 [[ attribute(1) ]] ;
    float2 TEXCOORD0 [[ attribute(2) ]] ;
};

struct Mtl_VertexOut
{
    float4 mtl_Position [[ position, invariant ]];
    float4 COLOR0 [[ user(COLOR0) ]];
    float2 TEXCOORD0 [[ user(TEXCOORD0) ]];
    uint TEXCOORD1 [[ user(TEXCOORD1) ]];
};

constant bool has_base_vertex_instance [[ function_constant(4) ]];
vertex Mtl_VertexOut xlatMtlMain(
    constant VGlobals_Type& VGlobals [[ buffer(0) ]],
    uint mtl_VertexID [[ vertex_id ]],
    uint mtl_BaseVertex [[ base_vertex, function_constant(has_base_vertex_instance) ]],
    Mtl_VertexIn input [[ stage_in ]])
{
    if(has_base_vertex_instance)
        mtl_VertexID = mtl_VertexID - mtl_BaseVertex;
    Mtl_VertexOut output;
    float4 u_xlat0;
    float4 u_xlat1;
    u_xlat0 = input.POSITION0.yyyy * VGlobals.hlslcc_mtx4x4unity_ObjectToWorld[1];
    u_xlat0 = fma(VGlobals.hlslcc_mtx4x4unity_ObjectToWorld[0], input.POSITION0.xxxx, u_xlat0);
    u_xlat1 = u_xlat0.yyyy * VGlobals.hlslcc_mtx4x4unity_MatrixVP[1];
    output.mtl_Position = fma(VGlobals.hlslcc_mtx4x4unity_MatrixVP[0], u_xlat0.xxxx, u_xlat1);
    output.COLOR0 = input.COLOR0 * VGlobals._Color;
    output.TEXCOORD0.xy = input.TEXCOORD0.xy;
    output.TEXCOORD1 = mtl_VertexID & 0x1u;
    return output;
}
'''

FS = '''#include <metal_stdlib>
#include <metal_texture>
using namespace metal;
constant uint32_t rp_output_remap_mask [[ function_constant(1) ]];
constant const uint rp_output_remap_0 = (rp_output_remap_mask >> 0) & 0xF;
struct FGlobals_Type
{
    float _Cutoff;
    float4 _MainTex_TexelSize;
};

struct Mtl_FragmentIn
{
    float4 COLOR0 [[ user(COLOR0) ]] ;
    float2 TEXCOORD0 [[ user(TEXCOORD0) ]] ;
    uint TEXCOORD1 [[ user(TEXCOORD1) ]] [[ flat ]];
};

struct Mtl_FragmentOut
{
    float4 SV_Target0 [[ color(rp_output_remap_0) ]];
};

constant float4 ImmCB_0[2] =
{
    float4(1.0, 0.0, 0.0, 0.0),
    float4(0.0, 1.0, 0.0, 0.0)
};
fragment Mtl_FragmentOut xlatMtlMain(
    constant FGlobals_Type& FGlobals [[ buffer(0) ]],
    sampler sampler_MainTex [[ sampler (0) ]],
    texture2d<float, access::sample > _MainTex [[ texture(0) ]] ,
    depth2d<float, access::sample > _ShadowMap [[ texture(1) ]] ,
    Mtl_FragmentIn input [[ stage_in ]])
{
    Mtl_FragmentOut output;
    constexpr sampler _mtl_xl_shadow_sampler(address::clamp_to_edge, filter::linear, compare_func::greater_equal);
    float4 u_xlat0;
    bool2 u_xlatb0;
    int u_xlati1;
    float u_xlat2;
    u_xlat0 = _MainTex.sample(sampler_MainTex, input.TEXCOORD0.xy, bias(-1.0));
    u_xlatb0.xy = (u_xlat0.xy<float2(FGlobals._Cutoff));
    u_xlat0.xy = select(float2(0.0, 0.0), u_xlat0.zw, bool2(u_xlatb0.xy));
    u_xlati1 = int(input.TEXCOORD1);
    u_xlat0.z = u_xlati1;
    u_xlat2 = _ShadowMap.sample_compare(_mtl_xl_shadow_sampler, input.TEXCOORD0.xy, saturate(u_xlat0.w), level(0.0));
    u_xlat0.w = as_type<float>(as_type<uint>(u_xlat2) & 0x7fffffffu);
#if __METAL_VERSION__ >= 230
    {
        rasterization_rate_map_decoder mtl_VrrMap(*(constant rasterization_rate_map_data*)(&mtl_VrrBuffer.vrrData));
    }
#endif
    if((u_xlat0.x<0.0)){discard_fragment();}
    output.SV_Target0 = fma(u_xlat0, input.COLOR0, ImmCB_0[u_xlati1 & 1]);
    output.SV_Target0.x = -abs(u_xlat2) + 1.0;
    return output;
}
'''


class TranslateTest(unittest.TestCase):
    def test_program(self):
        text, typ = M.translate_program(VS, FS)
        self.assertEqual(typ, B.GPU_GLES3)
        st = G.stage_sources(text)
        vs, fs = st['VERTEX'], st['FRAGMENT']
        self.assertTrue(vs.startswith('#version 300 es\n'))
        for decl in ('uniform vec4 hlslcc_mtx4x4unity_ObjectToWorld[4];', 'uniform vec4 _Color;', 'in vec4 in_POSITION0;',
                     'in vec2 in_TEXCOORD0;', 'out vec4 vs_COLOR0;', 'flat out uint vs_TEXCOORD1;'):
            self.assertIn(decl, vs)
        self.assertNotIn('_Unused', vs)                                         # (not read: not declared)
        self.assertNotIn('mtl_BaseVertex', vs)                                  # (function constant off)
        self.assertIn('gl_Position = hlslcc_mtx4x4unity_MatrixVP[0] * u_xlat0.xxxx + u_xlat1;', vs)
        self.assertIn('vs_TEXCOORD1 = uint(gl_VertexID) & 0x1u;', vs)
        for decl in ('uniform float _Cutoff;', 'uniform sampler2D _MainTex;', 'uniform sampler2DShadow hlslcc_zcmp_ShadowMap;',
                     'uniform sampler2D _ShadowMap;', 'precision highp sampler2DShadow;',
                     'flat in uint vs_TEXCOORD1;', 'layout(location = 0) out vec4 SV_Target0;',
                     'const vec4 ImmCB_0[2] = vec4[2](vec4(1.0, 0.0, 0.0, 0.0), vec4(0.0, 1.0, 0.0, 0.0));'):
            self.assertIn(decl, fs)
        for code in ('texture(_MainTex, vs_TEXCOORD0.xy, -1.0)',               # bias
                     'u_xlatb0.xy = lessThan(u_xlat0.xy, vec2(_Cutoff));',      # a vector comparison
                     'mix(vec2(0.0, 0.0), u_xlat0.zw, u_xlatb0.xy)',            # select
                     'u_xlat0.z = float(u_xlati1);',                            # MSL's implicit conversion
                     'textureLod(hlslcc_zcmp_ShadowMap, vec3(vs_TEXCOORD0.xy, clamp(u_xlat0.w, 0.0, 1.0)), 0.0)',
                     'uintBitsToFloat(floatBitsToUint(u_xlat2) & 0x7fffffffu)',
                     'discard;', 'ImmCB_0[u_xlati1 & 1]', '-abs(u_xlat2) + 1.0'):
            self.assertIn(code, fs)
        self.assertNotIn('rasterization_rate_map', fs)
        # 2019.1+: units in Unity's macro, the plain sampler before the comparison one
        text19, _ = M.translate_program(VS, FS, unity=(2021, 3))
        fs19 = G.stage_sources(text19)['FRAGMENT']
        self.assertIn('UNITY_LOCATION(0) uniform sampler2D _MainTex;', fs19)
        self.assertIn('UNITY_LOCATION(1) uniform sampler2D _ShadowMap;', fs19)
        self.assertIn('UNITY_LOCATION(2) uniform sampler2DShadow hlslcc_zcmp_ShadowMap;', fs19)
        info = D.gl_bindings(text19, (2021, 3), 25)
        self.assertEqual([(b.name, b.index) for b in info.bindings], [('_MainTex', 0), ('_ShadowMap', 1)])

    def test_refused(self):
        with self.assertRaises(M.Unsupported):
            M.translate_program(VS.replace('vertex Mtl_VertexOut', 'kernel void'), FS)
        with self.assertRaises(M.Unsupported):          # a device buffer (structured buffer)
            M.translate_program(VS.replace('    Mtl_VertexIn input', '    const device float4* _Buf [[ buffer(1) ]],\n'
                                                                    '    Mtl_VertexIn input'), FS)
        with self.assertRaises(M.Unsupported):          # the fragment reads what the vertex stage does not write
            M.translate_program(VS.replace('    uint TEXCOORD1 [[ user(TEXCOORD1) ]];\n', '').replace(
                '    output.TEXCOORD1 = mtl_VertexID & 0x1u;\n', ''), FS)


def _entry(gpu_type, code, rest, keywords=()):
    return B.Entry(201708220, gpu_type, [0, 0, 0, 0], list(keywords), code, rest)


def metal_code(msl):
    """Unity's header (0xf00dcafe, sizes, the entry point's name) and the MSL."""
    return B.METAL_MAGIC + struct.pack('<5i', 0x24, 2, 0x30, 0, 0) + b'\0' * 16 + b'xlatMtlMain\0' + msl.encode()


class AddGles3MetalTest(unittest.TestCase):
    def test_2017_layout(self):
        vb = D.Bindings(25, [(0, 12), (2, 13), (3, 14)], [D.ConstantBuffer('', 0, [])], [])
        entries = [_entry(23, metal_code(VS), vb.serialize()), _entry(24, metal_code(FS), D.Bindings(0, [], [], []).serialize())]
        raw = B.build_blob(entries)
        comp = lz4.block.compress(raw, store_size=False)

        def sp(i, t):
            return {'m_BlobIndex': i, 'm_Channels': {'m_Channels': [], 'm_SourceMap': 25 if t == 23 else 0},
                    'm_KeywordIndices': [], 'm_ShaderHardwareTier': 3, 'm_GpuProgramType': t, 'm_VectorParams': [],
                    'm_MatrixParams': [], 'm_TextureParams': [], 'm_BufferParams': [], 'm_ConstantBuffers': [],
                    'm_ConstantBufferBindings': [], 'm_UAVParams': [], 'm_Samplers': [], 'm_ShaderRequirements': 1}
        tree = {'m_ParsedForm': {'m_Name': 'Test/Metal', 'm_SubShaders': [{'m_Passes': [
                    {'m_NameIndices': [], 'progVertex': {'m_SubPrograms': [sp(0, 23)]},
                     'progFragment': {'m_SubPrograms': [sp(1, 24)]}}]}]},
                'platforms': [14], 'offsets': [0], 'compressedLengths': [len(comp)], 'decompressedLengths': [len(raw)],
                'compressedBlob': list(comp)}
        self.assertEqual(B.Entry.parse(entries[0].serialize()).code, entries[0].code)
        r = M.add_gles3_metal(tree, unity='2017.4.40f1')
        self.assertEqual((r.status, r.programs, r.twins), ('converted', 1, 2), r)
        self.assertEqual(tree['platforms'], [9, 14])
        p = tree['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]
        tv = [x for x in p['progVertex']['m_SubPrograms'] if x['m_GpuProgramType'] == 4]
        tf = [x for x in p['progFragment']['m_SubPrograms'] if x['m_GpuProgramType'] == 4]
        self.assertEqual(([x['m_BlobIndex'] for x in tv], [x['m_BlobIndex'] for x in tf]), ([0], [1]))
        es = B.ShaderBlobs(tree).entries(9)
        self.assertTrue(es[0].code.startswith(b'#ifdef VERTEX\n#version 300 es\n'))
        self.assertEqual(es[0].rest, D.gl_rest(25))
        self.assertEqual((es[1].code, es[1].rest), (b'', D.gl_rest(0)))
        self.assertEqual(M.add_gles3_metal(tree).status, 'unchanged')


if __name__ == '__main__':
    unittest.main()
