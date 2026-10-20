"""Unit tests of the version-dependent parts of the data conversion on hand-made data:
SerializedFile headers (formats 17, 22), Unity versions, assembly lists, compressed builds
(data.unity3d), GLES 3.1 programs.   python -m unittest discover -s tests   (from convert/)"""
import lzma
import os
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import android_data as A
from pmunity import unity_version as UV
from pmunity import unpack as U
from pmunity.shaders import glsl as G


def sf_head(fmt, version, platform):
    """The first bytes of a SerializedFile of format 17 (20-byte header) or 22 (48 bytes)."""
    meta = version.encode() + b'\0' + struct.pack('<I', platform) + b'\0' * 32
    if fmt >= 22:
        return struct.pack('>IIII', 0, 0, fmt, 0) + b'\0' * 4 + struct.pack('>IQQQ', 100, 1000, 4096, 0) + meta
    return struct.pack('>IIII', 100, 1000, fmt, 4096) + b'\0' * 4 + meta


def archive(files, unity='2021.3.16f1', block=40, comp=(3, 0, 1)):
    """A UnityFS archive (format 6) holding files [(name, bytes)], cut into blocks of `block` bytes
    compressed in turn with each of `comp` (3 LZ4HC, 0 stored, 1 LZMA)."""
    import lz4.block
    data = b''.join(d for _, d in files)
    blocks, raw = [], b''
    for k, i in enumerate(range(0, len(data), block)):
        plain, c = data[i:i + block], comp[k % len(comp)]
        if c == 3:
            z = lz4.block.compress(plain, mode='high_compression', store_size=False)
        elif c == 1:
            lc, lp, pb, dict_size = 3, 0, 2, 1 << 16
            z = bytes([(pb * 5 + lp) * 9 + lc]) + struct.pack('<I', dict_size) + lzma.compress(
                plain, lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': dict_size, 'lc': lc, 'lp': lp, 'pb': pb}])
        else:
            z = plain
        blocks.append((len(plain), len(z), c))
        raw += z
    info = b'\0' * 16 + struct.pack('>i', len(blocks))
    for u, c, f in blocks:
        info += struct.pack('>IIH', u, c, f)
    info += struct.pack('>i', len(files))
    off = 0
    for name, d in files:
        info += struct.pack('>qqI', off, len(d), 4) + name.encode() + b'\0'
        off += len(d)
    ci = lz4.block.compress(info, store_size=False)
    head = b'UnityFS\0' + struct.pack('>I', 6) + b'5.x.x\0' + unity.encode() + b'\0'
    size = len(head) + 20 + len(ci) + len(raw)
    return head + struct.pack('>qIII', size, len(ci), len(info), 0x42) + ci + raw


class HeaderTest(unittest.TestCase):
    def test_platform_offset(self):
        for fmt, ver, off in ((17, '2017.4.9f1', 31), (21, '2019.4.38f1', 32), (22, '2020.3.48f1', 60)):
            head = sf_head(fmt, ver, 24)
            self.assertEqual(A.sf_platform_offset(head), (off, '<', 24, ver))
            self.assertEqual(UV.sf_header(head)[:2], (fmt, ver))

    def test_patch_format_22(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'level0')
            with open(p, 'wb') as f:
                f.write(sf_head(22, '2022.3.36f1', 24) + b'\0' * 64)
            self.assertEqual(A.patch_sf_platform(p), 24)
            with open(p, 'rb') as f:
                self.assertEqual(A.sf_platform_offset(f.read(128))[2], A.ANDROID)
            self.assertEqual(str(UV.of_file(p)), '2022.3.36f1')


class VersionTest(unittest.TestCase):
    def test_parse_and_check(self):
        v = UV.Version('2021.3.45f2')
        self.assertEqual((v.line, v.patch, v.kind, v.build), ((2021, 3), 45, 'f', 2))
        for ok in ('2017.3.0f3', '2017.4.40f1', '2020.3.49f1', '2022.3.62f3'):
            UV.check(UV.Version(ok))
        for bad in ('5.6.7f1', '2017.2.5f1', '2023.1.0f1', '6000.0.23f1'):
            with self.assertRaises(UV.Unsupported):
                UV.check(UV.Version(bad))
            UV.check(UV.Version(bad), force=True)

    def test_of_data(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'data.unity3d'), 'wb') as f:
                f.write(archive([('globalgamemanagers', b'x' * 10)], unity='2020.3.49f1'))
            self.assertEqual(str(UV.of_data(d)), '2020.3.49f1')
            with open(os.path.join(d, 'globalgamemanagers'), 'wb') as f:
                f.write(sf_head(22, '2020.3.48f1', 24))
            self.assertEqual(str(UV.of_data(d)), '2020.3.48f1')      # the plain file first

    def test_assembly_lists(self):
        engine = ['UnityEngine.dll'] + [f'UnityEngine.M{i}Module.dll' for i in range(46)] + ['']
        names = engine + ['Assembly-CSharp.dll', 'UnityEngine.UI.dll']
        game, eng = A.split_assemblies(names, UV.Version('2017.4.40f1'))
        self.assertEqual(game, ['Assembly-CSharp.dll', 'UnityEngine.UI.dll'])
        self.assertEqual(len(eng), 47)
        # 2018+: by name; no reserved slots
        game, eng = A.split_assemblies(['Assembly-CSharp.dll', 'UnityEngine.CoreModule.dll', 'UnityEngine.UI.dll'],
                                       UV.Version('2019.4.8f1'))
        self.assertEqual((game, eng), (['Assembly-CSharp.dll', 'UnityEngine.UI.dll'], ['UnityEngine.CoreModule.dll']))


class SwappyTest(unittest.TestCase):
    def test_options_key(self):
        """--swappy (default off) is part of the conversion key only where androidUseSwappy exists
        (2019.3+), so a 2017.4 or 2018.4 conversion is not redone for it"""
        import types
        base = dict(video_shader=True, platform_patch=True, graphics_apis='11', mt_rendering='keep', blit='never', rewired_joysticks=None,
                    name='Game', async_upload_buffer=None, inline_textures=True, swappy='off')
        old = A.opts_key(types.SimpleNamespace(unity=UV.Version('2017.4.9f1'), **base))
        new = A.opts_key(types.SimpleNamespace(unity=UV.Version('2021.3.16f1'), **base))
        self.assertNotIn('swappy=off', old)
        self.assertIn('swappy=off', new)
        self.assertEqual(new[:-1], old)
        self.assertEqual((A.OBB_GUID_FILE, A.OBB_GUID_FILE_IN_OBB), ('assets/unity_obb_guid', 'unity_obb_guid'))


class UnpackTest(unittest.TestCase):
    def test_unpack(self):
        files = [('globalgamemanagers', bytes(range(256)) * 3), ('level0', b'level data ' * 20),
                 ('sharedassets0.assets.resS', b''), ('Resources/unity_builtin_extra', b'builtin' * 9)]
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'data.unity3d'), 'wb') as f:
                f.write(archive(files))
            names = U.unpack(d, remove=True, log=lambda m: None)
            self.assertEqual(names, [n for n, _ in files])
            for n, data in files:
                with open(os.path.join(d, n), 'rb') as f:
                    self.assertEqual(f.read(), data, n)
            self.assertFalse(os.path.exists(os.path.join(d, 'data.unity3d')))

    def test_names_stay_inside(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, 'data.unity3d'), 'wb') as f:
                f.write(archive([('../evil', b'x')]))
            with self.assertRaises(ValueError):
                U.unpack(d, log=lambda m: None)


class ES31Test(unittest.TestCase):
    PROGRAM = '''#ifdef VERTEX
#version 150
in  vec4 in_POSITION0;
void main()
{
    gl_Position = in_POSITION0;
    return;
}

#endif
#ifdef FRAGMENT
#version 150
uniform  sampler2DMS _CameraDepthAttachment;
layout(location = 0) out vec4 SV_Target0;
void main()
{
    SV_Target0 = texelFetch(_CameraDepthAttachment, ivec2(gl_FragCoord.xy), 1);
    return;
}

#endif
'''

    def test_needs_es31(self):
        with self.assertRaises(G.NeedsES31):
            G.program_to_es(self.PROGRAM)
        f = G.stage_sources(G.program_to_es(self.PROGRAM, es='310'))['FRAGMENT']
        self.assertTrue(f.startswith('#version 310 es\n'))
        self.assertIn('precision highp sampler2DMS;', f)

    def test_still_unsupported(self):
        with self.assertRaises(G.Unsupported) as cm:
            G.program_to_es(self.PROGRAM.replace('sampler2DMS', 'sampler2DMSArray'), es='310')
        self.assertNotIsInstance(cm.exception, G.NeedsES31)


class AsmpatchTest(unittest.TestCase):
    """--asmpatch: the command gets <Managed> <work dir>; what it writes is shipped like
    --managed-file; its failure stops the run; an assembly a port also provides is an error."""

    FAKE = ('import os, sys\n'
            'managed, out = sys.argv[-2], sys.argv[-1]\n'
            'assert os.path.isfile(os.path.join(managed, "Assembly-CSharp.dll"))\n'
            'os.makedirs(out, exist_ok=True)\n'
            'mode = sys.argv[1]\n'
            'if mode == "fail": print("FAIL rewired-android: no constant"); sys.exit(1)\n'
            'if mode == "patch":\n'
            '    open(os.path.join(out, "Assembly-CSharp.dll"), "wb").write(b"patched")\n'
            '    print("ok   rewired-android: Assembly-CSharp.dll: platform Linux -> Android (7)")\n'
            '    print("wrote Assembly-CSharp.dll")\n'
            'else: print("n/a  rewired-android: no Rewired")\n')

    def run_it(self, mode, **kw):
        td = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, td, True)
        os.makedirs(os.path.join(td, 'src', 'Managed'))
        with open(os.path.join(td, 'src', 'Managed', 'Assembly-CSharp.dll'), 'wb') as f:
            f.write(b'orig')
        script = os.path.join(td, 'fake_asmpatch.py')
        with open(script, 'w') as f:
            f.write(self.FAKE)
        args = type('Args', (), dict(src=os.path.join(td, 'src'), out=os.path.join(td, 'out'), game_managed=None,
                                     asmpatch=f'"{sys.executable}" -I "{script}" {mode}', assembly_patch=[], managed_file=[]))()
        for k, v in kw.items():
            setattr(args, k, v)
        os.makedirs(args.out)
        logged = []
        return args, A.run_asmpatch(args, logged.append), logged

    def test_patched_assembly_is_shipped(self):
        args, notes, logged = self.run_it('patch')
        self.assertEqual([os.path.basename(f) for f in args.managed_file], ['Assembly-CSharp.dll'])
        with open(args.managed_file[0], 'rb') as f:
            self.assertEqual(f.read(), b'patched')
        self.assertTrue(notes[0].startswith('ok   rewired-android'))
        self.assertIn('asmpatch: wrote Assembly-CSharp.dll', logged)

    def test_nothing_applies(self):
        args, notes, _ = self.run_it('none')
        self.assertEqual(args.managed_file, [])
        self.assertEqual(notes, [])

    def test_failure_stops(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_it('fail')
        self.assertIn('no constant', str(cm.exception))

    def test_clash_with_port_patch(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_it('patch', assembly_patch=[('Assembly-CSharp.dll', 'codepatch {managed} {out}')])
        self.assertIn('also provide', str(cm.exception))


if __name__ == '__main__':
    unittest.main()
