"""Unit tests of the compute kernel translation (ComputeShader assets) on small hand-written
kernels and typetrees (no game data): python -m unittest discover -s tests   (from convert/)"""
import copy
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity.shaders import compute as CS
from pmunity.shaders import glsl as G

# the form of HLSLcc's OpenGL Core kernels (GLSL 4.30): 420pack, Unity's macro block, the unused
# `precise` declarations, a uniform block, a storage buffer, a write-only image without a format,
# group-shared memory and barriers
KERNEL = """#version 430
#extension GL_ARB_shading_language_420pack : require

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
precise vec4 u_xlat_precise_vec4;
precise ivec4 u_xlat_precise_ivec4;
precise bvec4 u_xlat_precise_bvec4;
precise uvec4 u_xlat_precise_uvec4;
UNITY_BINDING(0) uniform CGlobals {
	uint Count;
	float Speed;
};
 struct Items_type {
	uint[4] value;
};

layout(std430, binding = 0) buffer Items {
	Items_type Items_buf[];
};
UNITY_LOCATION(0) uniform  sampler2D _Noise;
writeonly layout(binding=0) uniform image2D Result;
vec4 u_xlat0;
shared struct {
	uint value[1];
} TGSM0[64];
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
void main()
{
    TGSM0[gl_LocalInvocationIndex].value[(0 >> 2)] = gl_LocalInvocationIndex;
    memoryBarrierShared();
    barrier();
    u_xlat0.x = uintBitsToFloat(Items_buf[gl_GlobalInvocationID.x].value[(0 >> 2)]) * Speed;
    u_xlat0 = u_xlat0.xxxx * textureLod(_Noise, vec2(0.5, 0.5), 0.0);
    Items_buf[gl_GlobalInvocationID.x].value[(0 >> 2)] = floatBitsToUint(u_xlat0.x);
    imageStore(Result, ivec2(gl_GlobalInvocationID.xy), u_xlat0);
    return;
}
"""


class KernelTest(unittest.TestCase):
    def test_translation(self):
        self.assertEqual(G.image_names(KERNEL), ['Result'])
        es = G.kernel_to_es(KERNEL, image_formats={'Result': 'rgba32f'})
        self.assertTrue(es.startswith('#version 310 es\nprecision highp float;\nprecision highp int;\n'), es[:80])
        self.assertNotIn('GL_ARB_', es)
        self.assertNotIn('precise', es)
        self.assertIn('precision highp sampler2D;\n', es)
        self.assertIn('precision highp image2D;\n', es)                         # no default precision in ES
        self.assertIn('writeonly layout(binding=0, rgba32f) uniform image2D Result;', es)
        self.assertIn('layout(std430, binding = 0) buffer Items {', es)
        self.assertIn('UNITY_BINDING(0) uniform CGlobals {', es)
        self.assertIn('#define UNITY_SUPPORTS_UNIFORM_LOCATION 1', es)          # switched by the player
        for kept in ('memoryBarrierShared();', 'barrier();', 'shared struct {',
                     'layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;'):
            self.assertIn(kept, es)
        # the body is the GLCore one
        self.assertEqual(es[es.index('void main()'):], KERNEL[KERNEL.index('void main()'):])
        # default-samplers: sampler2D keeps ES's default, images still get one
        es = G.kernel_to_es(KERNEL, 'default-samplers', {'Result': 'rgba32f'})
        self.assertNotIn('precision highp sampler2D;', es)
        self.assertIn('precision highp image2D;', es)

    def test_image_formats(self):
        with self.assertRaisesRegex(G.Unsupported, "image without a format qualifier 'Result'"):
            G.kernel_to_es(KERNEL)
        with self.assertRaisesRegex(G.Unsupported, 'rg32f'):
            G.kernel_to_es(KERNEL, image_formats={'Result': 'rg32f'})
        # no layout at all
        k = KERNEL.replace('writeonly layout(binding=0) uniform image2D Result;', 'writeonly uniform image2D Result;')
        self.assertIn('writeonly layout(r32f) uniform image2D Result;', G.kernel_to_es(k, image_formats={'Result': 'r32f'}))
        # one with a format is left alone
        k = KERNEL.replace('layout(binding=0) uniform image2D', 'layout(binding=0, rgba8) uniform image2D')
        self.assertEqual(G.image_names(k), [])
        self.assertIn('writeonly layout(binding=0, rgba8) uniform image2D Result;', G.kernel_to_es(k))

    def test_read_write_images(self):
        k = KERNEL.replace('writeonly layout(binding=0) uniform image2D', 'layout(binding=0, rgba8) uniform image2D')
        with self.assertRaisesRegex(G.Unsupported, "image 'Result' read and written"):
            G.kernel_to_es(k)
        k = KERNEL.replace('writeonly layout(binding=0) uniform image2D', 'layout(binding=0, r32f) highp uniform image2D')
        self.assertIn('layout(binding=0, r32f) highp uniform image2D Result;', G.kernel_to_es(k))

    def test_image_atomics(self):
        k = KERNEL.replace('writeonly layout(binding=0) uniform image2D Result;', 'layout(binding=0, r32ui) highp uniform uimage2D Hits;') \
                  .replace('    imageStore(Result, ivec2(gl_GlobalInvocationID.xy), u_xlat0);\n',
                           '    imageAtomicAdd(Hits, ivec2(gl_GlobalInvocationID.xy), 1u);\n')
        es = G.kernel_to_es(k)
        self.assertIn('#version 310 es\n#extension GL_OES_shader_image_atomic : require\nprecision highp float;', es)

    def test_extensions_of_glsl_420(self):
        k = KERNEL.replace('#version 430\n#extension GL_ARB_shading_language_420pack : require\n',
                           '#version 420\n#extension GL_ARB_compute_shader : enable\n#extension GL_ARB_shader_atomic_counters : enable\n'
                           '#extension GL_ARB_shading_language_420pack : require\n#extension GL_ARB_shader_storage_buffer_object : enable\n')
        es = G.kernel_to_es(k, image_formats={'Result': 'rgba32f'})
        self.assertTrue(es.startswith('#version 310 es\nprecision highp float;'))
        self.assertNotIn('#extension', es)

    def test_unsupported(self):
        for old, new, what in (
                ('#version 430', '#version 150', '#version 150'),
                ('u_xlat0.x = uint', 'u_xlat0.x = fma(Speed, Speed, Speed) + uint', 'fma'),
                ('UNITY_LOCATION(0) uniform  sampler2D _Noise;', 'uniform samplerBuffer _Noise;', 'sampler type'),
                ('u_xlat0 = u_xlat0.xxxx', 'u_xlat0.x = float(textureQueryLevels(_Noise));\n    u_xlat0 = u_xlat0.xxxx', 'textureQueryLevels'),
                ('vec4 u_xlat0;', 'double u_xlat9;\nvec4 u_xlat0;', 'double')):
            k = KERNEL.replace(old, new, 1)
            self.assertNotEqual(k, KERNEL, old)
            with self.assertRaisesRegex(G.Unsupported, what):
                G.kernel_to_es(k, image_formats={'Result': 'rgba32f'})
        with self.assertRaisesRegex(G.Unsupported, 'not a compute shader'):
            G.kernel_to_es(KERNEL.replace('layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;\n', ''))

    def test_barriers_stay_unsupported_in_draw_programs(self):
        from tests.test_shaders import PROGRAM_420, _frag_line
        p = _frag_line(PROGRAM_420, 'gl_FragDepth = 0.5;', 'barrier();\n    gl_FragDepth = 0.5;')
        with self.assertRaisesRegex(G.Unsupported, 'barrier'):
            G.program_to_es(p, es='310')


# ---- typetrees
def _spirv(images):
    """A minimal SPIR-V module declaring image variables: [(name, format id)]."""
    words = [0x07230203, 0x00010000, 0, 100, 0]

    def op(code, *args):
        words.extend([(len(args) + 1) << 16 | code] + list(args))

    def string(s):
        b = s.encode() + b'\0'
        b += b'\0' * (-len(b) % 4)
        return list(struct.unpack('<%dI' % (len(b) // 4), b))

    for i, (name, fmt) in enumerate(images):
        v, img, ptr = 10 + 3 * i, 11 + 3 * i, 12 + 3 * i
        op(5, v, *string(name))                                  # OpName
        op(25, img, 1, 1, 0, 0, 0, 2, fmt)                      # OpTypeImage 2D, storage, format
        op(32, ptr, 0, img)                                      # OpTypePointer UniformConstant
        op(59, ptr, v, 0)                                        # OpVariable
    return struct.pack('<%dI' % len(words), *words)


def _binding(name, bp):
    return {'name': name, 'generatedName': '', 'bindPoint': bp, 'samplerBindPoint': -1, 'texDimension': -1}


def _kv(code):
    return {'cbVariantIndices': [0], 'cbs': [_binding('$Globals', 0)], 'textures': [_binding('Result', 0)],
            'builtinSamplers': [], 'inBuffers': [], 'outBuffers': [_binding('Items', 0)],
            'code': list(code), 'threadGroupSize': [64, 1, 1], 'requirements': 0}


def _variant(renderer, level, kernels):
    return {'targetRenderer': renderer, 'targetLevel': level, 'kernels': kernels,
            'constantBuffers': [{'name': '$Globals', 'byteSize': 16, 'params': []}], 'resourcesResolved': True}


def _tree(layout, names=('KMain',), gl_code=KERNEL.encode() + b'\0', renderers=(17, 21)):
    """A ComputeShader typetree of a layout: '2019' (code in the kernel), '2020' (variantMap),
    '2021.2' (uniqueVariants)."""
    def kernel(name, code):
        if layout == '2019':
            k = _kv(code)
            del k['cbVariantIndices'], k['requirements']
            return dict(name=name, **k)
        if layout == '2020':
            return {'name': name, 'variantMap': [('', _kv(code))], 'globalKeywords': [], 'localKeywords': []}
        return {'name': name, 'uniqueVariants': [_kv(code)], 'variantIndices': [('', 0)], 'globalKeywords': [],
                'localKeywords': [], 'dynamicKeywords': []}
    vs = []
    for r in renderers:
        code = gl_code if r == 17 else _spirv([('Result_origX0X', 1)])
        vs.append(_variant(r, 11 if r == 17 else 0, [kernel(n, code) for n in names]))
    return {'m_Name': 'Swarm', 'variants': vs}


class ComputeShaderTest(unittest.TestCase):
    def test_spirv_formats(self):
        self.assertEqual(CS.spirv_image_formats(_spirv([('A_origX0X', 1), ('B', 3), ('C', 6)])),
                         {'A_origX0X': 'rgba32f', 'B': 'r32f', 'C': 'rg32f'})
        self.assertEqual(CS.spirv_image_formats(b'#version 430\n'), {})

    def test_layouts(self):
        for layout, unity in (('2019', '2019.4.40f1'), ('2020', '2021.1.28f1'), ('2021.2', '2021.3.45f2')):
            t = _tree(layout)
            gl = copy.deepcopy(t['variants'][0])
            r = CS.add_gles3(t, unity)
            self.assertEqual(r.status, 'converted', layout)
            self.assertEqual((r.kernels, r.variants, r.failed), (1, 1, []))
            self.assertEqual([v['targetRenderer'] for v in t['variants']], [17, 11, 21], layout)
            es = t['variants'][1]
            self.assertEqual(es['targetLevel'], 3)
            # everything but the renderer, level and code is the GLCore variant's
            a, b = copy.deepcopy(es), copy.deepcopy(gl)
            for v in (a, b):
                v['targetRenderer'] = v['targetLevel'] = None
                for k in v['kernels']:
                    for kv in CS.kernel_variants(k):
                        kv['code'] = None
            self.assertEqual(a, b, layout)
            code = bytes(CS.kernel_variants(es['kernels'][0])[0]['code'])
            self.assertTrue(code.startswith(b'#version 310 es\n'), layout)
            self.assertTrue(code.endswith(b'}\n\0'), layout)                      # the terminating zero kept
            self.assertIn(b'writeonly layout(binding=0, rgba32f) uniform image2D Result;', code)   # from SPIR-V
            self.assertEqual(t['variants'][0], gl)                                  # GLCore untouched

    def test_formats_by_keywords(self):
        # the Vulkan kernel's variants in another order: each GLCore variant takes the formats of
        # the Vulkan variant with its keywords
        t = _tree('2020', renderers=(17, 21))
        gl, vk = t['variants'][0]['kernels'][0], t['variants'][1]['kernels'][0]
        gl['variantMap'] = [('A', _kv(KERNEL.encode())), ('B', _kv(KERNEL.encode()))]
        vk['variantMap'] = [('B', _kv(_spirv([('Result_origX0X', 3)]))), ('A', _kv(_spirv([('Result_origX0X', 1)])))]
        CS.add_gles3(t, '2021.1.28f1')
        es = t['variants'][1]['kernels'][0]['variantMap']
        self.assertIn(b'layout(binding=0, rgba32f) uniform image2D Result;', bytes(es[0][1]['code']))
        self.assertIn(b'layout(binding=0, r32f) uniform image2D Result;', bytes(es[1][1]['code']))
        # 2021.2+: through variantIndices
        t = _tree('2021.2', renderers=(17, 21))
        gl, vk = t['variants'][0]['kernels'][0], t['variants'][1]['kernels'][0]
        gl['uniqueVariants'] = [_kv(KERNEL.encode()), _kv(KERNEL.encode())]
        gl['variantIndices'] = [('A', 0), ('B', 1)]
        vk['uniqueVariants'] = [_kv(_spirv([('Result_origX0X', 3)])), _kv(_spirv([('Result_origX0X', 1)]))]
        vk['variantIndices'] = [('B', 0), ('A', 1)]
        CS.add_gles3(t, '2021.3.45f2')
        es = t['variants'][1]['kernels'][0]['uniqueVariants']
        self.assertIn(b'rgba32f) uniform image2D Result;', bytes(es[0]['code']))
        self.assertIn(b'r32f) uniform image2D Result;', bytes(es[1]['code']))

    def test_order_and_level(self):
        t = _tree('2021.2', renderers=(2, 17, 18, 21))
        CS.add_gles3(t, '2022.3.62f1')
        self.assertEqual([v['targetRenderer'] for v in t['variants']], [2, 11, 17, 18, 21])
        t = _tree('2021.2', renderers=(2, 18, 21, 17))
        CS.add_gles3(t, '2021.1.28f1')
        self.assertEqual([v['targetRenderer'] for v in t['variants']], [2, 18, 21, 17, 11])
        t = _tree('2021.2', renderers=(2, 17, 18, 21))
        CS.add_gles3(t, '6000.0.58f2')
        self.assertEqual(next(v['targetLevel'] for v in t['variants'] if v['targetRenderer'] == 11), 2)

    def test_existing_and_replace(self):
        t = _tree('2020', renderers=(17, 11, 21))
        before = copy.deepcopy(t)
        r = CS.add_gles3(t, '2021.1.28f1')
        self.assertEqual((r.status, r.why), ('unchanged', 'already has GLES3 kernels'))
        self.assertEqual(t, before)
        r = CS.add_gles3(t, '2021.1.28f1', replace=True)
        self.assertEqual(r.status, 'converted')
        self.assertEqual([v['targetRenderer'] for v in t['variants']], [17, 11, 21])
        self.assertTrue(bytes(t['variants'][1]['kernels'][0]['variantMap'][0][1]['code']).startswith(b'#version 310 es'))

    def test_kernels_left_out(self):
        # no Vulkan variant: the image's format is unknown; a kernel without images still translates
        plain = KERNEL.replace('writeonly layout(binding=0) uniform image2D Result;\n', '') \
                      .replace('    imageStore(Result, ivec2(gl_GlobalInvocationID.xy), u_xlat0);\n', '')
        t = _tree('2020', names=('KImage', 'KPlain'), renderers=(17,))
        t['variants'][0]['kernels'][1]['variantMap'][0][1]['code'] = list(plain.encode() + b'\0')
        r = CS.add_gles3(t, '2020.3.48f1')
        self.assertEqual(r.status, 'converted')
        self.assertEqual([k['name'] for k in t['variants'][1]['kernels']], ['KPlain'])
        self.assertEqual(r.failed[0][0], 'KImage')
        self.assertIn('1 of 2 kernels left out', r.why)
        # none translates: the shader is left as it is
        t = _tree('2020', renderers=(17,))
        before = copy.deepcopy(t)
        r = CS.add_gles3(t, '2020.3.48f1')
        self.assertEqual(r.status, 'skipped')
        self.assertEqual(t, before)

    def test_several_glcore_levels(self):
        # 2018.x: a GLCore variant of level 10 with the kernels and an empty one of level 8
        t = _tree('2019')
        t['variants'].append(_variant(17, 8, []))
        r = CS.add_gles3(t, '2018.2.7f1')
        self.assertEqual((r.status, r.kernels), ('converted', 1))
        self.assertEqual([(v['targetRenderer'], v['targetLevel'], len(v['kernels'])) for v in t['variants']],
                         [(17, 11, 1), (11, 3, 1), (21, 0, 1), (17, 8, 0)])
        # only empty GLCore variants: nothing to do
        t = _tree('2019', renderers=(21,))
        t['variants'].append(_variant(17, 8, []))
        self.assertEqual(CS.add_gles3(t, '2018.2.7f1').status, 'unchanged')

    def test_no_glcore(self):
        t = _tree('2020', renderers=(2, 21))
        r = CS.add_gles3(t, '2020.3.48f1')
        self.assertEqual((r.status, r.why), ('unchanged', 'no OpenGL Core kernels'))


if __name__ == '__main__':
    unittest.main()
