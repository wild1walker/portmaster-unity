"""deadfrags.py on synthetic shaders and screenshots: python3 -m unittest discover -s tools/device/test"""
import os, sys, tempfile, unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import deadfrags  # noqa: E402

SPRITE = """#version 320 es
precision highp float;
UNITY_LOCATION(0) uniform  sampler2D _MainTex;
in  vec4 vs_COLOR0;
in  vec2 vs_TEXCOORD0;
layout(location = 0) out vec4 SV_Target0;
vec4 u_xlat0;
void main()
{
    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy);
    SV_Target0 = u_xlat0 * vs_COLOR0;
    return;
}
"""
POST = """#version 320 es
precision highp float;
uniform sampler2D _MainTex;
uniform sampler2D _Bloom;
in vec2 vs_TEXCOORD0;
layout(location = 0) out vec4 SV_Target0;
void main()
{
    SV_Target0 = texture(_MainTex, vs_TEXCOORD0) + texture(_Bloom, vs_TEXCOORD0);
}
"""
VS = """#version 320 es
in vec4 in_POSITION0;
void main() { gl_Position = in_POSITION0; }
"""
LOG = """[gl]   #5 draw fb5 prog25 vs 00000000000000aa fs 0000000000000001 18 _MainTex=0:tex1 -> (0,0,0,255) (0,0,0,255) | depth lequal stencil off blend 1,1-sa
[gl]   #6 draw fb5 prog26 vs 00000000000000aa fs 0000000000000002 6 _MainTex=0:tex2 -> (0,0,0,255) (0,0,0,255) | depth lequal stencil off blend sa,1-sa
[gl]   #7 draw fb5 prog27 vs 00000000000000aa fs 0000000000000003 6 _MainTex=0:tex2 -> (0,0,0,255) (0,0,0,255) | depth lequal stencil off blend sa,1 mask rgb
[gl]   #8 draw fb5 prog28 vs 00000000000000aa fs 0000000000000004 6 _MainTex=0:tex2 -> (0,0,0,255) (0,0,0,255) | depth always stencil off blend off
[gl]   #9 draw fb5 prog29 vs 00000000000000aa fs 0000000000000005 6 _MainTex=0:tex2 -> (0,0,0,255) (0,0,0,255) | depth always stencil off blend off
[gl]   #10 draw fb5 prog30 vs 00000000000000aa fs 0000000000000006 6 _MainTex=0:tex2 -> (0,0,0,255) (0,0,0,255) | depth always stencil off blend 1,1
[gl]   #11 draw fb5 prog31 vs 00000000000000aa fs 0000000000000007 6 _MainTex=0:tex2 -> (0,0,0,255) (0,0,0,255) | depth always stencil off blend 1,1-sa
[gl]   #12 draw fb5 prog32 vs 00000000000000aa fs 0000000000000007 6 _MainTex=0:tex2 -> (0,0,0,255) (0,0,0,255) | depth always stencil off blend sa,1-sa
"""


class DeadFragsTest(unittest.TestCase):
    def test_bodies(self):
        out, role = deadfrags.rewrite(SPRITE, 'count', '1,1-sa')
        self.assertEqual(role, 'count')
        self.assertIn('in  vec4 vs_COLOR0;', out)                                   # declarations kept
        self.assertIn('float ua_a = texture(_MainTex, vs_TEXCOORD0.xy).w;', out)
        self.assertIn('SV_Target0 = vec4(ua_d, 1.0 - ua_d, 0.0, 0.0) * (1.0 / 255.0);', out)
        self.assertNotIn('vs_COLOR0;\n    return', out)
        self.assertIn('vec4(ua_d, 1.0 - ua_d, 0.0, 1.0 / 255.0)', deadfrags.rewrite(SPRITE, 'count', 'sa,1-sa')[0])
        self.assertIn('* (1.0 / 255.0), 1.0)', deadfrags.rewrite(SPRITE, 'count', 'sa,1')[0])
        self.assertEqual(deadfrags.rewrite(SPRITE, 'count', '1,1'), (None, 'blend 1,1'))     # no exact count
        self.assertEqual(deadfrags.rewrite(POST, 'count', '1,1-sa'), (None, 'no _MainTex sample at a varying'))
        self.assertIn('texelFetch(_MainTex, ivec2(gl_FragCoord.xy), 0)', deadfrags.rewrite(POST, 'copy', 'off')[0])
        self.assertIn('vec4(0.0, 0.0, 1.0 / 255.0, 0.0)', deadfrags.rewrite(POST, 'mark', 'off')[0])
        self.assertEqual(deadfrags.rewrite(VS, 'count', '1,1-sa'), (None, 'vertex shader'))

    def test_blends(self):
        b = deadfrags.blends(LOG)
        self.assertEqual(b['0000000000000003'], {'sa,1'})
        self.assertEqual(b['0000000000000007'], {'1,1-sa', 'sa,1-sa'})

    def test_shaders_and_sum(self):
        dump, dst, run = tempfile.mkdtemp(), tempfile.mkdtemp(), tempfile.mkdtemp()
        for h, text in (('1', SPRITE), ('2', SPRITE), ('3', SPRITE), ('4', POST), ('5', POST), ('6', SPRITE),
                        ('7', SPRITE), ('aa', VS)):
            with open(os.path.join(dump, h.rjust(16, '0') + '.glsl'), 'w') as f:
                f.write(text)
        log = os.path.join(run, 'run.log')
        with open(log, 'w') as f:
            f.write(LOG)
        args = ['deadfrags.py', 'shaders', dump, log, dst, '--copy', '0000000000000004',
                '--mark', '0000000000000005', '--only', '0000000000000001,0000000000000003']
        self.assertEqual(deadfrags.main(args), 0)
        names = sorted(os.listdir(dst))
        # 1 and 3 count, 2 hides (--only), 4 copies, 5 marks; 6 (blend 1,1) and 7 (two blends) are left
        self.assertEqual(names, [h.rjust(16, '0') + '.glsl' for h in '12345'])
        with open(os.path.join(dst, '0000000000000002.glsl')) as f:
            self.assertIn('SV_Target0 = vec4(0.0);', f.read())

    def test_sum(self):
        try:
            import numpy as np
            from PIL import Image
        except ImportError:
            self.skipTest('sum needs numpy and Pillow')
        run = tempfile.mkdtemp()
        img = np.zeros((20, 10, 3), np.uint8)
        img[5:15, :, 0] = 1                                      # one dead layer over the picture
        img[5:15, :, 1] = 3                                      # three live ones
        img[0:2, 0:2, 1] = 200                                   # a HUD box left out
        Image.fromarray(img).save(os.path.join(run, 'shot-1.png'))
        dead, live, mark, px = deadfrags.counts(img.astype(int), rows=(5, 15), skip=(0, 0, 2, 2))
        self.assertEqual((dead, live, mark, px), (100, 300, 0, 100))
        self.assertEqual(deadfrags.main(['deadfrags.py', 'sum', run, '--rows', '5:15']), 0)


if __name__ == '__main__':
    unittest.main()
