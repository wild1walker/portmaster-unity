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
        for fmt, ver, off in ((17, '2017.4.9f1', 31), (21, '2019.4.38f1', 32), (22, '2020.3.48f1', 60),
                              (23, '6000.6.3f1', 59)):
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
        for ok in ('5.6.0f3', '5.6.7f1', '2017.1.5f1', '2017.2.0f3', '2017.3.0f3', '2017.4.40f1', '2020.3.49f1',
                   '2022.3.62f3', '2023.1.20f1', '2023.2.3f1', '6000.0.58f2', '6000.3.11f1', '6000.6.3f1'):
            UV.check(UV.Version(ok))
        for bad in ('5.5.4f1', '5.3.2p3', '6000.7.0b5'):
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


class OlderNewerLinesTest(unittest.TestCase):
    def test_assembly_slots(self):
        """MonoManager's engine slots: 5.x and 2017.1 2, 2017.2 45, 2017.3+ 48 (then by name)"""
        names = ['UnityEngine.dll', ''] + ['Assembly-CSharp.dll']
        self.assertEqual(A.split_assemblies(names, UV.Version('5.6.0f3'))[0], ['Assembly-CSharp.dll'])
        self.assertEqual(A.split_assemblies(names, UV.Version('2017.1.5f1'))[0], ['Assembly-CSharp.dll'])
        self.assertEqual(A.builtin_assembly_slots(UV.Version('2017.2.0f3')), 45)
        self.assertEqual(A.builtin_assembly_slots(UV.Version('2017.4.9f1')), 48)

    def test_default_resources(self):
        """2023.2+ players read Resources/unity default resources; packages keep it there too"""
        self.assertEqual(A.default_resources_path(UV.Version('2023.1.20f1')), ('unity default resources',))
        self.assertEqual(A.default_resources_path(UV.Version('6000.0.58f2')), ('Resources', 'unity default resources'))
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'Data', 'Resources'))
            p = os.path.join(d, 'Data', 'Resources', 'unity default resources')
            with open(p, 'wb') as f:
                f.write(sf_head(23, '6000.6.0b3', 13))
            self.assertEqual(A.pkg_default_resources(d), p)
            self.assertEqual(A.player_version(d).line, (6000, 6))

    def test_typetree_fix(self):
        """typetrees.py: a fixed tree only where UnityPy's does not read the object, and exactly"""
        from UnityPy.helpers.TypeTreeNode import TypeTreeNode
        from pmunity import typetrees as T
        node = TypeTreeNode.from_list([
            dict(m_Level=0, m_Type='PlayerSettings', m_Name='Base', m_ByteSize=-1, m_Version=1, m_Index=0, m_MetaFlag=0),
            dict(m_Level=1, m_Type='int', m_Name='insecureHttpOption', m_ByteSize=4, m_Version=1, m_Index=1, m_MetaFlag=0),
            dict(m_Level=1, m_Type='int', m_Name='last', m_ByteSize=4, m_Version=1, m_Index=2, m_MetaFlag=0)])
        fixed = T._with_fields(node, T.FIELDS['PlayerSettings'])
        self.assertEqual([c.m_Name for c in fixed.m_Children], ['insecureHttpOption', 'captureStartupLogsBaked', 'last'])
        self.assertEqual(fixed.m_Children[1].m_MetaFlag, T.ALIGN)

        class Obj:          # read_typetree with UnityPy's node fails, with the fixed one succeeds
            type = type('t', (), {'name': 'PlayerSettings'})
            def _get_typetree_node(self):
                return node
            def read_typetree(self, nodes=None):
                if nodes is None or len(nodes.m_Children) != 3:
                    raise ValueError('Expected to read 12 bytes, but only read 8 bytes')
                return {'ok': True}
        tree, used = T.read(Obj())
        self.assertEqual(tree, {'ok': True})
        self.assertEqual(len(used.m_Children), 3)

        class Tail(Obj):    # 2023.2.20f1: one byte more at the end, no FIELDS entry fits
            type = type('t', (), {'name': 'Other'})
            def read_typetree(self, nodes=None):
                if nodes is None or [c.m_Name for c in nodes.m_Children][-1] != '_unknown_0':
                    raise ValueError('Expected to read 9 bytes, but only read 8 bytes')
                return {'tail': True}
        tree, used = T.read(Tail(), edits=('insecureHttpOption',))
        self.assertEqual(tree, {'tail': True})
        self.assertEqual(used.m_Children[-1].m_Type, 'UInt8')

    def test_scripting_runtime(self):
        """2019.1 still had .NET 3.5 (a macOS build's Mono folder is outside its data): the shipped
        mscorlib says which runtime; 2019.2+ have only .NET 4.x"""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from test_port import write_assembly
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'Managed'))
            ms = os.path.join(d, 'Managed', 'mscorlib.dll')
            write_assembly(ms, 'mscorlib', version=2)
            self.assertEqual(A.scripting_runtime(d, UV.Version('2019.1.15f1')), '.NET 3.5 (libmono)')
            self.assertEqual(A.scripting_runtime(d, UV.Version('2018.4.36f1')), '.NET 3.5 (libmono)')
            self.assertEqual(A.scripting_runtime(d, UV.Version('2019.2.5f1')), '.NET 4.x (libmonobdwgc-2.0)')
            write_assembly(ms, 'mscorlib', version=4)
            self.assertEqual(A.scripting_runtime(d, UV.Version('2019.1.15f1')), '.NET 4.x (libmonobdwgc-2.0)')
            os.remove(ms)                   # no assemblies to read: the Mono folder decides
            os.makedirs(os.path.join(d, 'MonoBleedingEdge', 'etc'))
            self.assertEqual(A.scripting_runtime(d, UV.Version('2018.4.36f1')), '.NET 4.x (libmonobdwgc-2.0)')

    def test_missing_references(self):
        """engine modules that reference an assembly the data does not ship (a Unity 6 build without
        netstandard.dll) are named"""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from test_port import write_assembly
        from pmunity import clrmeta as CM
        with tempfile.TemporaryDirectory() as d:
            write_assembly(os.path.join(d, 'UnityEngine.CoreModule.dll'), 'UnityEngine.CoreModule',
                           assemblyrefs=['mscorlib', 'netstandard', 'UnityEngine.SharedInternalsModule'])
            write_assembly(os.path.join(d, 'UnityEngine.SharedInternalsModule.dll'), 'UnityEngine.SharedInternalsModule',
                           assemblyrefs=['mscorlib'])
            a = CM.Assembly(os.path.join(d, 'UnityEngine.CoreModule.dll'))
            self.assertEqual([a.string(r[6]) for r in a.rows(CM.ASSEMBLYREF)][1], 'netstandard')
            names = {'UnityEngine.CoreModule.dll', 'UnityEngine.SharedInternalsModule.dll', 'Assembly-CSharp.dll'}
            self.assertEqual(A.missing_references(d, names), {'netstandard': ['UnityEngine.CoreModule']})
            self.assertEqual(A.missing_references(d, names | {'netstandard.dll'}), {})

    def test_typetree_neighbours(self):
        """typetrees.read: an object a field shorter than UnityPy's tree for its version (6000.2.6f2's
        UnityConnectSettings lacks InsightsSettings, which UnityPy's trees have from 6000.2.0b7 on)
        is read with UnityPy's tree of a neighbouring version that reads every byte"""
        from UnityPy.helpers import Tpk, TypeTreeHelper
        from UnityPy.helpers.UnityVersion import UnityVersion
        from UnityPy.streams import EndianBinaryReader, EndianBinaryWriter
        from pmunity import typetrees as T
        info = [(v, c) for v, c in Tpk.get_typetree().ClassInformation[310] if c is not None]
        older = next(Tpk.generate_node(c) for v, c in reversed(info) if v < UnityVersion.from_str('6000.2.0b7'))
        self.assertNotIn('InsightsSettings', [c.m_Name for c in older.m_Children])
        # an object written with that tree (default values; nothing of a game)
        zero = TypeTreeHelper.read_typetree(older, EndianBinaryReader(bytes(4096), endian='<'), check_read=False)
        w = EndianBinaryWriter(endian='<')
        TypeTreeHelper.write_typetree(zero, older, w)
        data = w.bytes

        class Obj:
            class_id, serialized_type = 310, None
            type = type('t', (), {'name': 'UnityConnectSettings'})
            def __init__(self, version):
                self.version = UnityVersion.from_str(version)
            def _get_typetree_node(self, nodes=None):
                return nodes or Tpk.get_typetree_node(310, self.version)
            def read_typetree(self, nodes=None):
                return TypeTreeHelper.read_typetree(self._get_typetree_node(nodes), EndianBinaryReader(data, endian='<'),
                                                    byte_size=len(data), check_read=True)
        with self.assertRaises(ValueError):
            Obj('6000.2.6f2').read_typetree()                      # UnityPy's own tree
        tree, node = T.read(Obj('6000.2.6f2'))
        self.assertIsNotNone(node)
        self.assertNotIn('InsightsSettings', tree)
        self.assertEqual(T.read(Obj('6000.2.6f2'), edits=('m_Enabled',))[1].m_Children[0].m_Name, 'm_Enabled')
        self.assertIsNone(T.read(Obj('6000.2.0a8'))[1])            # its own tree reads it: no other tree
        self.assertEqual(T.neighbours(type('O', (), {'serialized_type': type('s', (), {'node': older})()})()), [])


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


class PlanTest(unittest.TestCase):
    """plan(): what of a desktop build's Data goes into the APK's assets/bin/Data."""

    def test_realtime_gi_data_is_shipped(self):
        td = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, td, True)
        src, pkg = os.path.join(td, 'Game_Data'), os.path.join(td, 'pkg')
        for d in (os.path.join(src, 'Managed'), os.path.join(src, 'GI', 'level1', 'f8'), os.path.join(src, 'Resources'),
                  os.path.join(pkg, 'Data'), os.path.join(pkg, 'Variations', 'mono', 'Managed')):
            os.makedirs(d)
        for f in (os.path.join(src, 'GI', 'level1', 'f8', 'abc.vis'), os.path.join(src, 'GI', 'level1', 'f8', 'abc.rgb'),
                  os.path.join(src, 'level1'), os.path.join(pkg, 'Data', 'unity default resources')):
            open(f, 'wb').close()
        args = type('Args', (), dict(src=src, pkg=pkg, name='Game', inline_textures=False, inlined_ress=[], root_file=[],
                                     skipped=[], video_shader=False, managed_file=[], assembly_patch=[], game_managed=None,
                                     dropped_modules=[], mono_etc=os.path.join(td, 'none'), beside=[]))()
        dst = {d: s for d, s, a in A.plan(args)}
        data = os.path.join('Game', 'assets', 'bin', 'Data')
        for f in ('abc.vis', 'abc.rgb'):
            self.assertEqual(dst.get(os.path.join(data, 'GI', 'level1', 'f8', f)), os.path.join(src, 'GI', 'level1', 'f8', f))
        self.assertNotIn('GI', args.skipped)
        # the SSE runtime files: as they are, and with --realtime-gi neon also under their NEON names
        gi = os.path.join(src, 'GI', 'level1', 'f8')
        for f in ('abc.iws.sse', 'abc.rsc.sse', 'def.rsc.sse', 'def.rsc.neo'):
            open(os.path.join(gi, f), 'wb').close()
        args.realtime_gi = 'as-is'
        dst = {d: s for d, s, a in A.plan(args)}
        gd = os.path.join(data, 'GI', 'level1', 'f8')
        self.assertEqual(dst.get(os.path.join(gd, 'abc.rsc.sse')), os.path.join(gi, 'abc.rsc.sse'))
        self.assertNotIn(os.path.join(gd, 'abc.rsc.neo'), dst)
        del args.realtime_gi                                         # the default: neon
        dst = {d: s for d, s, a in A.plan(args)}
        self.assertEqual(dst.get(os.path.join(gd, 'abc.iws.neo')), os.path.join(gi, 'abc.iws.sse'))
        self.assertEqual(dst.get(os.path.join(gd, 'abc.rsc.neo')), os.path.join(gi, 'abc.rsc.sse'))
        self.assertEqual(dst.get(os.path.join(gd, 'def.rsc.neo')), os.path.join(gi, 'def.rsc.neo'))   # its own kept
        self.assertEqual(args.gi_neon, 2)


class GlesLevelTest(unittest.TestCase):
    """PlayerSettings.playerMinOpenGLESVersion and android_package.json's min_sdk."""

    def test_min_gles(self):
        args = type('Args', (), {})()
        self.assertEqual(A.min_gles_level(args, 0), 2)              # a desktop build's 0: ES 3.0
        self.assertIsNone(A.min_gles_level(args, 2))
        self.assertIsNone(A.min_gles_level(args, None))             # before 2020.2: no such field
        args.min_gles = '3.1'
        self.assertEqual(A.min_gles_level(args, 0), 3)
        args.min_gles = 'keep'
        self.assertIsNone(A.min_gles_level(args, 0))

    def test_min_sdk(self):
        td = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, td, True)
        args = type('Args', (), dict(src=td))()
        self.assertEqual(A.min_sdk(args), 0)                        # no compute kernels
        args.compute_kernels = True
        self.assertEqual(A.min_sdk(args), A.GLES31_SDK)
        args.min_sdk = '0'
        self.assertEqual(A.min_sdk(args), 0)
        args.min_sdk = '24'
        self.assertEqual(A.min_sdk(args), 24)


class SplitBigApkTest(unittest.TestCase):
    """A game whose bin/Data passes the APK's 2 GiB: later scenes and resources move to the OBBs."""
    def test_split(self):
        import tempfile
        d = tempfile.mkdtemp()
        def f(n, sz):
            p = os.path.join(d, n.replace('/', '_'))
            with open(p, 'wb') as fh:
                fh.truncate(sz)
            return 'assets/bin/Data/' + n, p
        e = [f('globalgamemanagers', 10), f('level0', 10), f('sharedassets0.assets', 10), f('Managed/a.dll', 500),
             f('level5', 3000), f('sharedassets5.assets', 2000), f('resources.assets', 100)]
        apk, moved = A.split_big_apk(e, limit=3000)
        self.assertEqual(sorted(a for a, _ in moved), ['assets/bin/Data/level5', 'assets/bin/Data/sharedassets5.assets'])
        for first in ('globalgamemanagers', 'level0', 'sharedassets0.assets', 'Managed/a.dll'):
            self.assertIn('assets/bin/Data/' + first, [a for a, _ in apk])
        small, none = A.split_big_apk(e, limit=10 ** 9)
        self.assertEqual((small, none), (e, []))


if __name__ == '__main__':
    unittest.main()
