"""flatshaders.py on synthetic shaders: python3 -m unittest discover -s tools/device/test"""
import os, sys, tempfile, unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import flatshaders  # noqa: E402

FS3 = """#version 320 es
precision highp float;
uniform sampler2D _MainTex;
in vec2 uv;
layout(location = 0) out vec4 SV_Target0;
layout(location = 1) out mediump vec4 SV_Target1;
void main()
{
    SV_Target0 = texture(_MainTex, uv);
    SV_Target1 = vec4(1.0);
}
"""
FS2 = """#version 100
precision mediump float;
varying vec2 uv;
uniform sampler2D t;
void main() { gl_FragColor = texture2D(t, uv); }
"""
VS = """#version 300 es
in vec4 pos;
void main() { gl_Position = pos; }
"""


class FlatShadersTest(unittest.TestCase):
    def test_flatten(self):
        out = flatshaders.flatten(FS3)
        self.assertIn('layout(location = 1) out mediump vec4 SV_Target1;', out)       # declarations kept
        self.assertIn('SV_Target0 = vec4(0.5, 0.5, 0.5, 1.0); SV_Target1 = vec4(0.5, 0.5, 0.5, 1.0);', out)
        self.assertNotIn('texture(', out)
        self.assertIn('gl_FragColor = vec4(0.5', flatshaders.flatten(FS2))
        self.assertIsNone(flatshaders.flatten(VS))                                 # vertex shaders stay

    def test_notex(self):
        fs = """#version 300 es
precision highp float;
uniform highp sampler2D _MainTex;
uniform highp sampler2DShadow _Shadow;
in vec2 uv;
layout(location = 0) out vec4 SV_Target0;
void main()
{
    vec4 c = texture(_MainTex, uv * 2.0 + vec2(0.5), 0.0);
    float s = texture(_Shadow, vec3(uv, c.x));
    SV_Target0 = c * s + textureLod(_MainTex, (uv).yx, 1.0);
}
"""
        out = flatshaders.notex(fs)
        body = out[out.index('void main()'):]
        self.assertNotIn('texture', body)                                         # no lookup left in main()
        self.assertIn('vec4((float((uv * 2.0 + vec2(0.5)).x) + float(0.0) * 0.0) * 0.5 + 0.25)', body)   # from the coordinate
        self.assertIn('float s = ((float((vec3(uv, c.x)).x)) * 0.5 + 0.25);', body)                    # a shadow lookup: a float
        self.assertIn('uniform highp sampler2D _MainTex;', out)                   # declarations kept
        self.assertIsNone(flatshaders.notex(VS))

    def test_dirs(self):
        src, dst = tempfile.mkdtemp(), tempfile.mkdtemp()
        for name, text in (('a.glsl', FS3), ('b.glsl', VS), ('c.txt', FS2)):
            with open(os.path.join(src, name), 'w') as f:
                f.write(text)
        self.assertEqual(flatshaders.main(['flatshaders.py', src, dst]), 0)
        self.assertEqual(sorted(os.listdir(dst)), ['a.glsl'])


if __name__ == '__main__':
    unittest.main()
