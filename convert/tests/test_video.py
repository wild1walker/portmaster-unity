"""Hidden/VideoDecodeAndroid (pmunity/video.py): the programs, the clip sniffing, and (opt-in, on
a desktop build's own unity_builtin_extra, nothing of which is in the repository) the Shader object
made from Hidden/VideoDecode:

    python -m unittest tests.test_video -v                                          (from convert/)
    PMUNITY_VIDEO_TEMPLATE=<Game>_Data/Resources/unity_builtin_extra python -m unittest tests.test_video -v

The programs are compiled and drawn with Mesa by runtime/media/test/test_surface_gl.c.
"""
import os
import re
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import video
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import glsl as G

TEMPLATE = os.environ.get('PMUNITY_VIDEO_TEMPLATE')


class Programs(unittest.TestCase):
    def test_passes(self):
        progs = video.programs()
        self.assertEqual(sorted(progs), sorted(p for p, _ in video.PASSES))
        for name, code in progs.items():
            st = G.stage_sources(code)
            self.assertEqual(sorted(st), ['FRAGMENT', 'VERTEX'], name)
            for s in st.values():
                self.assertTrue(s.lstrip().startswith('#version 300 es'), name)
                # no conditionals inside a stage: the stage is cut at its matching #endif
                self.assertNotRegex(s, r'^\s*#\s*(if|ifdef|ifndef|endif)\b')
            frag, vert = st['FRAGMENT'], st['VERTEX']
            self.assertIn('#extension GL_OES_EGL_image_external_essl3 : require', frag)
            self.assertIn('uniform highp samplerExternalOES _MainTex;', frag)
            self.assertNotIn('textureSize', frag)          # drivers differ for external samplers
            for u in ('hlslcc_mtx4x4unity_ObjectToWorld', 'hlslcc_mtx4x4unity_MatrixVP', '_MainTex_ST', 'in_POSITION0', 'in_TEXCOORD0'):
                self.assertIn(u, vert, name)
            self.assertIn('const bool UM_BT709 = false;', frag)
        self.assertIn('_AlphaParam', progs['RGBAExternal_To_RGBA'])

    def test_bt709(self):
        for code in video.programs(bt709=True).values():
            self.assertIn('const bool UM_BT709 = true;', code)
            self.assertNotIn('UM_BT709 = false', code)


class Sniff(unittest.TestCase):
    def clip(self, data):
        f = tempfile.NamedTemporaryFile(delete=False)
        f.write(b'\0' * 100 + data + b'\0' * 50)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name, 100, len(data)

    def test_webm(self):
        p, o, n = self.clip(b'\x1a\x45\xdf\xa3' + b'\x42\x82\x84webm' + b'\x86\x85V_VP8' + b'\0' * 20)
        self.assertEqual(video.sniff(p, o, n), 'vp8')
        p, o, n = self.clip(b'\x1a\x45\xdf\xa3' + b'\x86\x8fV_MPEG4/ISO/AVC')
        self.assertEqual(video.sniff(p, o, n), 'h264')

    def test_mp4(self):
        ftyp = struct.pack('>I', 16) + b'ftypisom' + b'\0\0\2\0'
        # the moov (with the sample entry) after a large mdat: found in the tail read
        mdat = struct.pack('>I', 8 + 200000) + b'mdat' + b'\0' * 200000
        moov = struct.pack('>I', 24) + b'moov' + struct.pack('>I', 16) + b'avc1' + b'\0' * 8
        p, o, n = self.clip(ftyp + mdat + moov)
        self.assertEqual(video.sniff(p, o, n), 'h264')
        p, o, n = self.clip(ftyp + struct.pack('>I', 16) + b'moov' + struct.pack('>I', 8) + b'hvc1')
        self.assertEqual(video.sniff(p, o, n), 'hevc')

    def test_other(self):
        p, o, n = self.clip(b'FSB5' + b'\0' * 60)
        self.assertIsNone(video.sniff(p, o, n))


@unittest.skipUnless(TEMPLATE, 'set PMUNITY_VIDEO_TEMPLATE to a desktop build\'s unity_builtin_extra')
class FromTemplate(unittest.TestCase):
    def test_shader_object(self):
        import UnityPy
        sf = UnityPy.load(TEMPLATE).file
        n_before = len(sf.objects)
        logs = []
        pid = video.add_to_builtin_extra(sf, logs.append, bt709=False)
        self.assertIsNotNone(pid)
        self.assertEqual(len(sf.objects), n_before + 1)
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, 'unity_builtin_extra')
            with open(out, 'wb') as f:
                f.write(sf.save())
            sf2 = UnityPy.load(out).file
            t = sf2.objects[pid].read_typetree()
            self.assertEqual(t['m_ParsedForm']['m_Name'], video.NAME)
            self.assertEqual(list(t['platforms']), [B.PLATFORM_GLES3PLUS])
            passes = t['m_ParsedForm']['m_SubShaders'][0]['m_Passes']
            self.assertEqual([p['m_State']['m_Name'] for p in passes], [p.upper() for p, _ in video.PASSES])
            progs = video.programs()
            got = dict(C.gles3_programs(t))
            self.assertEqual(sorted(got.values()), sorted(progs.values()))
            for p in passes:
                for st in ('progVertex', 'progFragment'):
                    for _, _, lst in C.subprograms({'m_ParsedForm': {'m_SubShaders': [{'m_Passes': [p]}]}}):
                        self.assertTrue(all(sp['m_GpuProgramType'] == B.GPU_GLES3 for sp in lst))
            # a second run adds nothing
            self.assertIsNone(video.add_to_builtin_extra(sf2, logs.append))
            self.assertEqual(video.path_id_for(out), None)


if __name__ == '__main__':
    unittest.main()
