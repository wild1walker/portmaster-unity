"""Golden data for shaderx's Metal translator (crates/shaderx/tests/metal.rs): metal.translate_program
on the hand-written MSL of convert/tests/test_metal.py and on variants of it written here (no Unity
or game files), each case's GL program text and type or the Unsupported it raises.

    python metal_golden.py <checkout> <out.json>

Run it with the reference Python (setup-rs's image: /opt/py with PYTHONPATH=<checkout>/convert:/opt/pylibs)
and commit the JSON as crates/shaderx/tests/metal_golden.json.
"""
import json
import os
import sys

checkout, out = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(checkout, 'convert'))
sys.path.insert(0, os.path.join(checkout, 'convert', 'tests'))
from pmunity.shaders import glsl as G          # noqa: E402
from pmunity.shaders import metal as M         # noqa: E402
from pmunity.shaders import vulkan as VK       # noqa: E402
from test_metal import FS, VS                  # noqa: E402

# a vertex stage with a matrix, the instance id and a point size
VS2 = VS.replace('    uint TEXCOORD1 [[ user(TEXCOORD1) ]];\n',
                 '    uint TEXCOORD1 [[ user(TEXCOORD1) ]];\n    float mtl_PointSize [[ point_size ]];\n').replace(
    '    uint mtl_VertexID [[ vertex_id ]],\n',
    '    uint mtl_VertexID [[ vertex_id ]],\n    uint mtl_InstanceID [[ instance_id ]],\n').replace(
    '    float4 u_xlat1;\n',
    '    float4 u_xlat1;\n    float4x4 u_xlat_m;\n    float3x3 u_xlat_n;\n').replace(
    '    return output;\n',
    '    u_xlat_m = float4x4(VGlobals.hlslcc_mtx4x4unity_ObjectToWorld[0], VGlobals.hlslcc_mtx4x4unity_ObjectToWorld[1], '
    'VGlobals.hlslcc_mtx4x4unity_ObjectToWorld[2], VGlobals.hlslcc_mtx4x4unity_ObjectToWorld[3]);\n'
    '    u_xlat0 = u_xlat_m * input.POSITION0;\n'
    '    u_xlat_n = float3x3(u_xlat0.xyz, u_xlat1.xyz, input.COLOR0.xyz);\n'
    '    u_xlat1.xyz = input.COLOR0.xyz * u_xlat_n;\n'
    '    output.COLOR0.w = float(mtl_InstanceID) + u_xlat1.x;\n'
    '    output.mtl_PointSize = 1.0;\n'
    '    return output;\n')

# a fragment stage with gather, read, sizes, array, 3D and cube textures, loops, switch, the
# bit-field helper, ternaries, shifts, the depth output and the remapped colour output
FS2 = '''#include <metal_stdlib>
#include <metal_texture>
using namespace metal;
struct FGlobals_Type
{
    float4 _Params;
    int _Count;
    float4 _Arr[3];
};

struct Mtl_FragmentIn
{
    float4 COLOR0 [[ user(COLOR0) ]] ;
    float2 TEXCOORD0 [[ user(TEXCOORD0) ]] ;
    uint TEXCOORD1 [[ user(TEXCOORD1) ]] [[ flat ]];
};

struct Mtl_FragmentOut
{
    float4 SV_Target0 [[ color(xlt_remap_o[0]) ]];
    float mtl_Depth [[ depth(any) ]];
};

template <typename UVecType> UVecType bitFieldInsert(const UVecType width, const UVecType offset, const UVecType src2, const UVecType src3)
{
    UVecType bitmask = (((UVecType(1) << width)-1) << offset) & 0xffffffff;
    return ((src2 << offset) & bitmask) | (src3 & ~bitmask);
};
fragment Mtl_FragmentOut xlatMtlMain(
    constant FGlobals_Type& FGlobals [[ buffer(0) ]],
    sampler sampler_MainTex [[ sampler (0) ]],
    texture2d<float, access::sample > _MainTex [[ texture(0) ]] ,
    texture2d_array<float, access::sample > _ArrTex [[ texture(1) ]] ,
    texture3d<float, access::sample > _Vol [[ texture(2) ]] ,
    texturecube<float, access::sample > _Cube [[ texture(3) ]] ,
    depthcube<float, access::sample > _CubeShadow [[ texture(4) ]] ,
    bool mtl_FrontFace [[ front_facing ]],
    float4 mtl_FragCoord [[ position ]],
    Mtl_FragmentIn input [[ stage_in ]])
{
    Mtl_FragmentOut output;
    constexpr sampler _mtl_xl_shadow_sampler(address::clamp_to_edge, filter::linear, compare_func::greater_equal);
    float4 u_xlat0;
    float4 u_xlat1;
    int u_xlati2;
    uint u_xlatu3;
    bool u_xlatb4;
    float3 u_xlat5;
    bool4 u_xlatb6;
    u_xlat0 = _MainTex.gather(sampler_MainTex, input.TEXCOORD0.xy, int2(0, 0), component::y);
    u_xlat1 = _MainTex.read(uint2(mtl_FragCoord.xy), 0);
    u_xlat1.x = float(_MainTex.get_width(0)) + float(_MainTex.get_height(0));
    u_xlat1 = _ArrTex.sample(sampler_MainTex, input.TEXCOORD0.xy, round(FGlobals._Params.x), level(1.0));
    u_xlat5.xyz = _Vol.sample(sampler_MainTex, u_xlat1.xyz, gradient3d(u_xlat1.xyz, u_xlat1.zyx)).xyz;
    u_xlat1 = _Cube.sample(sampler_MainTex, u_xlat5.xyz, bias(0.5));
    u_xlat1.w = _CubeShadow.sample_compare(_mtl_xl_shadow_sampler, u_xlat5.xyz, u_xlat1.x);
    u_xlati2 = 0;
    for(int u_xlati_loop_1 = 0 ; u_xlati_loop_1<FGlobals._Count ; u_xlati_loop_1++)
    {
        u_xlati2 = u_xlati2 + u_xlati_loop_1;
        if(u_xlati2 > 4) { break; }
    }
    while(u_xlati2 < 2) u_xlati2++;
    switch(int(input.TEXCOORD1)){
        case 0x0:
            u_xlat0.x = FGlobals._Arr[0].x;
            break;
        default:
            u_xlat0.x = FGlobals._Arr[u_xlati2 % 3].y;
            break;
    }
    u_xlatu3 = bitFieldInsert(0x3u, 0x2u, input.TEXCOORD1, 0x0u);
    u_xlatb4 = mtl_FrontFace && u_xlat0.x>=0.5;
    u_xlat0.y = u_xlatb4 ? 1.0 : 0.0;
    u_xlat0.z = (u_xlatb4) ? float(u_xlatu3) : rsqrt(u_xlat1.w);
    u_xlat0.w = u_xlat1.x % 2.0;
    u_xlatb6 = (u_xlat0>=float4(0.5)) || (u_xlat1<u_xlat0);
    u_xlat1.xy = float2(int2(u_xlat0.xy) >> int2(1, 1));
    u_xlat1.zw = min(u_xlat0.zw, 0.5);
    u_xlat1.xyz = select(u_xlat1.xyz, -u_xlat1.xyz, u_xlatb6.xyz);
    u_xlat1.x = dot(u_xlat1.xyz, u_xlat5) + length(u_xlat1) + as_type<float>(0x3f800000);
    u_xlat1.y = (!u_xlatb4) ? clamp(u_xlat1.y, 0.0f, 1.0h) : abs(u_xlat1.y) * -2.5e-1;
    if(!any(u_xlatb6)) { u_xlat1.z = 0.0; } else if(all(u_xlatb6.xy)) { u_xlat1.z = 1.0; } else { u_xlat1.z = .5; }
    output.SV_Target0 = u_xlat0 * u_xlat1 + input.COLOR0;
    output.mtl_Depth = mtl_FragCoord.z;
    return output;
}
'''

CASES = [
    ('test_metal', VS, FS, None),
    ('test_metal 2018.4', VS, FS, (2018, 4)),
    ('test_metal 2021.3', VS, FS, (2021, 3)),
    ('matrix and point size', VS2, FS, (2020, 3)),
    ('ES 3.10 fragment', VS, FS2, None),
    ('ES 3.10 fragment 2022.3', VS2, FS2, (2022, 3)),
    ('a kernel', VS.replace('vertex Mtl_VertexOut', 'kernel void'), FS, None),
    ('a device buffer', VS.replace('    Mtl_VertexIn input', '    const device float4* _Buf [[ buffer(1) ]],\n'
                                                         '    Mtl_VertexIn input'), FS, None),
    ('a varying not written', VS.replace('    uint TEXCOORD1 [[ user(TEXCOORD1) ]];\n', '').replace(
        '    output.TEXCOORD1 = mtl_VertexID & 0x1u;\n', ''), FS, None),
    ('a varying of another type', VS.replace('    uint TEXCOORD1 [[ user(TEXCOORD1) ]];', '    int TEXCOORD1 [[ user(TEXCOORD1) ]];'), FS, None),
    ('a writable texture', VS, FS.replace('texture2d<float, access::sample > _MainTex', 'texture2d<float, access::read_write > _MainTex'), None),
    ('an unknown function', VS, FS.replace('saturate(u_xlat0.w)', 'popcount(u_xlat0.w)'), None),
    ('an unknown name', VS, FS.replace('u_xlat0.z = u_xlati1;', 'u_xlat0.z = u_xlati9;'), None),
    ('a vector ?:', VS, FS.replace('u_xlat0.w = as_type', 'u_xlat0.y = (u_xlatb0.xy) ? 1.0 : 0.0;\n    u_xlat0.w = as_type'), None),
    ('a matrix +', VS2.replace('u_xlat0 = u_xlat_m * input.POSITION0;', 'u_xlat_m = u_xlat_m + u_xlat_m;'), FS, None),
    ('sample of a depth texture', VS, FS.replace('_ShadowMap.sample_compare(_mtl_xl_shadow_sampler, input.TEXCOORD0.xy, saturate(u_xlat0.w), level(0.0))',
                                                 '_ShadowMap.sample(_mtl_xl_shadow_sampler, input.TEXCOORD0.xy).x'), None),
    ('cannot read', VS.replace('u_xlat1 = u_xlat0.yyyy', 'u_xlat1 = u_xlat0.yyyy @'), FS, None),
]

rows = []
for name, vs, fs, unity in CASES:
    notes = []
    row = {'name': name, 'vs': vs, 'fs': fs, 'unity': list(unity) if unity else None}
    try:
        text, typ = M.translate_program(vs, fs, unity=unity, notes=notes)
        row.update(text=text, type=typ)
    except (M.Unsupported, G.Unsupported, VK.Unsupported) as ex:
        row.update(error=str(ex))
    row['notes'] = notes
    rows.append(row)
with open(out, 'w') as f:
    json.dump(rows, f, indent=1)
print(f'{out}: {len(rows)} cases, {sum("text" in r for r in rows)} translated')
