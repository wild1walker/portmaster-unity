"""pmunity il2cpp: toolchain selection, range-read zip extraction, staging rules, the NDK stand-in
script and android-data's --il2cpp packing, on hand-made data. Opt-in:
PMUNITY_TEST_ASSEMBLY=<any .NET assembly> checks the AssemblyRef reader on a real file."""
import io
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import types
import unittest
import zipfile

from pmunity import il2cpp as I
from pmunity import unity_version as UV
from pmunity import android_data as AD


class NdkTest(unittest.TestCase):
    def test_ndk_per_line(self):
        self.assertEqual(I.ndk_for(UV.Version('2018.4.36f1'))[0], 'r16b')
        self.assertEqual(I.ndk_for(UV.Version('2019.2.21f1'))[0], 'r16b')
        self.assertEqual(I.ndk_for(UV.Version('2019.4.8f1'))[0], 'r19')
        self.assertEqual(I.ndk_for(UV.Version('2020.3.49f1'))[0], 'r19')
        self.assertEqual(I.ndk_for(UV.Version('2021.3.16f1'))[0], 'r21d')
        self.assertEqual(I.ndk_for(UV.Version('2022.1.24f1'))[0], 'r21d')
        self.assertEqual(I.ndk_for(UV.Version('2022.3.36f1'))[0], 'r23b')
        with self.assertRaises(UV.Unsupported):
            I.ndk_for(UV.Version('5.6.7f1'))

    def test_ndk_members(self):
        w = lambda n, abi='arm64-v8a', rev='r19': I.ndk_wanted('android-ndk-r19/' + n, rev, abi)
        llvm = 'toolchains/llvm/prebuilt/linux-x86_64/'
        self.assertEqual(w('source.properties'), 'source.properties')
        self.assertTrue(w(llvm + 'sysroot/usr/include/stdio.h'))
        self.assertTrue(w(llvm + 'sysroot/usr/local/include/stdlib.h', 'armeabi-v7a'))
        self.assertTrue(w(llvm + 'sysroot/usr/lib/aarch64-linux-android/libc++_static.a'))
        self.assertTrue(w(llvm + 'sysroot/usr/lib/aarch64-linux-android/21/libc.so'))
        self.assertIsNone(w(llvm + 'sysroot/usr/lib/aarch64-linux-android/28/libc.so'))
        self.assertIsNone(w(llvm + 'sysroot/usr/lib/arm-linux-androideabi/libc++_static.a'))
        self.assertTrue(w(llvm + 'sysroot/usr/lib/arm-linux-androideabi/19/libc.so', 'armeabi-v7a'))
        self.assertTrue(w(llvm + 'lib/gcc/aarch64-linux-android/4.9.x/libgcc.a'))
        self.assertIsNone(w(llvm + 'bin/clang'))                       # x86-64 programs: never
        self.assertIsNone(w(llvm + 'aarch64-linux-android/bin/ld'))
        self.assertIsNone(w('platforms/android-21/arch-arm64/usr/lib/libc.so'))   # (r19: the llvm sysroot)
        self.assertTrue(w('platforms/android-21/arch-arm64/usr/lib/libc.so', rev='r16b'))
        self.assertTrue(w('sources/cxx-stl/gnu-libstdc++/4.9/libs/arm64-v8a/libgnustl_static.a', rev='r16b'))
        self.assertTrue(w('sources/cxx-stl/gnu-libstdc++/4.9/include/vector', rev='r16b'))
        self.assertIsNone(w('sources/cxx-stl/gnu-libstdc++/4.9/libs/x86/libgnustl_static.a', rev='r16b'))
        self.assertTrue(w('platforms/android-16/arch-arm/usr/lib/libc.so', 'armeabi-v7a', rev='r16b'))
        self.assertIsNone(w('platforms/android-19/arch-arm/usr/lib/libc.so', 'armeabi-v7a', rev='r16b'))
        self.assertTrue(w('toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64/lib/gcc/aarch64-linux-android/4.9.x/libgcc.a', rev='r16b'))
        self.assertEqual((I.api_level('r16b', 'armeabi-v7a'), I.api_level('r19', 'armeabi-v7a'), I.api_level('r16b', 'arm64-v8a')), (16, 19, 21))


class EditorTest(unittest.TestCase):
    def test_paths(self):
        self.assertEqual(I.editor_rel('$_OUTDIR/il2cpp/libil2cpp/vm/Class.cpp'), 'il2cpp/libil2cpp/vm/Class.cpp')
        self.assertEqual(I.editor_rel('$INSTDIR/Editor/Data/il2cpp/il2cpp_root'), 'il2cpp/il2cpp_root')
        self.assertIsNone(I.editor_rel('$PLUGINSDIR/UAC.dll'))
        self.assertTrue(I.editor_wanted('il2cpp/build/deploy/net471/il2cpp.exe'))
        self.assertTrue(I.editor_wanted('MonoBleedingEdge/lib/mono/unityaot/mscorlib.dll'))
        self.assertTrue(I.editor_wanted('MonoBleedingEdge/etc/mono/4.5/machine.config'))
        self.assertFalse(I.editor_wanted('MonoBleedingEdge/lib/mono/unityjit/mscorlib.dll'))
        self.assertFalse(I.editor_wanted('il2cpp/MapFileParser/Driver.cpp'))
        self.assertFalse(I.editor_wanted('il2cpp/build/deploy/netcoreapp3.1/il2cpp.pdb'))
        self.assertFalse(I.editor_wanted('Resources/unity editor resources'))

    def test_runtime_pack(self):
        deps = {'targets': {'.NETCoreApp,Version=v3.1/win-x64': {
            'il2cpp/1.0.0': {'runtime': {'il2cpp.dll': {}}},
            'runtimepack.Microsoft.NETCore.App.Runtime.win-x64/3.1.6': {
                'runtime': {'System.Private.CoreLib.dll': {}, 'System.Runtime.WindowsRuntime.dll': {}},
                'native': {'coreclr.dll': {}, 'api-ms-win-core-file-l1-1-0.dll': {}}},
            'Newtonsoft.Json/12.0.3': {'runtime': {'lib/netstandard2.0/Newtonsoft.Json.dll': {}}}}},
            'libraries': {'il2cpp/1.0.0': {'type': 'project'},
                          'runtimepack.Microsoft.NETCore.App.Runtime.win-x64/3.1.6': {'type': 'runtimepack'},
                          'Newtonsoft.Json/12.0.3': {'type': 'package'}}}
        self.assertEqual(I.runtime_pack_files(deps), {'system.private.corelib.dll', 'system.runtime.windowsruntime.dll',
                                                      'coreclr.dll', 'api-ms-win-core-file-l1-1-0.dll'})

    def test_deploy_tool(self):
        with tempfile.TemporaryDirectory() as t:
            d = os.path.join(t, 'il2cpp', 'build', 'deploy', 'netcoreapp3.1')
            os.makedirs(d)
            open(os.path.join(d, 'il2cpp.dll'), 'w').close()
            self.assertEqual(I.deploy_tool(t, 'il2cpp'), os.path.join(d, 'il2cpp.dll'))
            n = os.path.join(t, 'il2cpp', 'build', 'deploy', 'net471')
            os.makedirs(n)
            open(os.path.join(n, 'UnityLinker.exe'), 'w').close()
            self.assertEqual(I.deploy_tool(t, 'UnityLinker'), os.path.join(n, 'UnityLinker.exe'))
            with self.assertRaises(SystemExit):
                I.deploy_tool(t, 'bee')


class SpansTest(unittest.TestCase):
    def test_merge(self):
        sp = I._spans([(100, 10, 'a'), (0, 50, 'b'), (120, 5, 'c'), (10 ** 7, 10, 'd')], gap=1000)
        self.assertEqual([(s, e, [k for _, _, k in l]) for s, e, l in sp], [(0, 125, ['b', 'a', 'c']), (10 ** 7, 10 ** 7 + 10, ['d'])])
        sp = I._spans([(0, 10, 'a'), (20, 10, 'b')], gap=5)
        self.assertEqual(len(sp), 2)
        sp = I._spans([(0, 10, 'a'), (11, 10, 'b')], gap=5, limit=15)
        self.assertEqual(len(sp), 2)


class RemoteZipTest(unittest.TestCase):
    def test_extract_local(self):
        with tempfile.TemporaryDirectory() as t:
            z = os.path.join(t, 'ndk.zip')
            with zipfile.ZipFile(z, 'w') as f:
                f.writestr('ndk/source.properties', 'Pkg.Desc = Android NDK\nPkg.Revision = 19.0.5232133\n')
                f.writestr(zipfile.ZipInfo('ndk/dir/'), '')
                f.writestr('ndk/a/big.bin', os.urandom(1000) * 50, compress_type=zipfile.ZIP_DEFLATED)
                f.writestr('ndk/a/skip.bin', b'x' * 100)
                zi = zipfile.ZipInfo('ndk/a/link')
                zi.external_attr = (stat.S_IFLNK | 0o777) << 16
                f.writestr(zi, 'big.bin')
                zi = zipfile.ZipInfo('ndk/bin/tool')
                zi.external_attr = (stat.S_IFREG | 0o755) << 16
                f.writestr(zi, '#!/bin/sh\n')
            rz = I.RemoteZip(z)
            self.assertEqual(len(rz.entries), 6)
            out = os.path.join(t, 'out')
            got = rz.extract(lambda n: None if 'skip' in n else n.split('/', 1)[1], out, log=lambda m: None)
            self.assertEqual(sorted(got), ['a/big.bin', 'a/link', 'bin/tool', 'source.properties'])
            with zipfile.ZipFile(z) as zf, open(os.path.join(out, 'a', 'big.bin'), 'rb') as f:
                self.assertEqual(f.read(), zf.read('ndk/a/big.bin'))
            self.assertEqual(os.readlink(os.path.join(out, 'a', 'link')), 'big.bin')
            self.assertTrue(os.stat(os.path.join(out, 'bin', 'tool')).st_mode & 0o100)
            self.assertFalse(os.path.exists(os.path.join(out, 'a', 'skip.bin')))


class StagingTest(unittest.TestCase):
    def test_metadata_version(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(struct.pack('<Ii', 0xFAB11BAF, 24) + bytes(64))
        try:
            self.assertEqual(I.metadata_version(f.name), 24)
            with open(f.name, 'wb') as g:
                g.write(bytes(16))
            with self.assertRaises(ValueError):
                I.metadata_version(f.name)
        finally:
            os.unlink(f.name)

    def test_incremental_gc(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(I.player_settings(t), {'gcIncremental': False})
            with open(os.path.join(t, 'boot.config'), 'w') as f:
                f.write('wait-for-native-debugger=0\ngc-max-time-slice=3\n')
            self.assertEqual(I.player_settings(t), {'gcIncremental': True, 'gcTimeSlice': 3})
            a = I.il2cpp_args(UV.Version('2020.3.49f1'), 'arm64-v8a', '/m', '/c', '/k', '/o.so', '/ndk',
                              I.player_settings(t), t)
            self.assertIn('--incremental-g-c-time-slice=3', a)
            self.assertIn('--architecture=ARM64', a)
            self.assertIn('--libil2cpp-static', a)
            self.assertFalse(any(x.startswith('--baselib-directory') for x in a))
            b = os.path.join(t, 'Variations', 'il2cpp', 'Release', 'StaticLibs', 'armeabi-v7a')
            os.makedirs(b)
            open(os.path.join(b, 'baselib.a'), 'w').close()
            a = I.il2cpp_args(UV.Version('2020.3.49f1'), 'armeabi-v7a', '/m', '/c', '/k', '/o.so', '/ndk', {}, t)
            self.assertIn(f'--baselib-directory={b}', a)
            self.assertIn('--architecture=ARMv7', a)
            self.assertNotIn('--incremental-g-c-time-slice=3', a)

    def test_link_args(self):
        a = I.link_args(UV.Version('2019.4.8f1'), 'arm64-v8a', '/s', '/l', ['B.dll', 'A.dll'])
        self.assertIn('--rule-set=Conservative', a)
        self.assertEqual([x for x in a if x.startswith('--include-unity-root-assembly')],
                         ['--include-unity-root-assembly=/s/A.dll', '--include-unity-root-assembly=/s/B.dll'])

    @unittest.skipUnless(os.environ.get('PMUNITY_TEST_ASSEMBLY'), 'PMUNITY_TEST_ASSEMBLY=<a .NET assembly> not set')
    def test_assembly_refs(self):
        refs = I.assembly_refs(os.environ['PMUNITY_TEST_ASSEMBLY'])
        self.assertTrue(all(r.endswith('.dll') for r in refs))
        print('refs:', refs)


class NdkClangTest(unittest.TestCase):
    """tools/il2cpp/ndk-clang.sh with a fake LLVM that prints its arguments."""

    def setUp(self):
        self.t = tempfile.mkdtemp()
        self.llvm = os.path.join(self.t, 'llvm')
        os.makedirs(self.llvm)
        for n in ('clang', 'clang++', 'llvm-objcopy'):
            p = os.path.join(self.llvm, n)
            with open(p, 'w') as f:
                f.write('#!/bin/sh\necho "%s" "$@"\n' % n)
            os.chmod(p, 0o755)
        self.ndk = os.path.join(self.t, 'ndk')
        os.makedirs(os.path.join(self.ndk, 'toolchains/llvm/prebuilt/linux-x86_64/sysroot'))
        os.makedirs(os.path.join(self.ndk, 'toolchains/llvm/prebuilt/linux-x86_64/lib/gcc'))
        I.install_ndk_shims(self.ndk, 'arm64-v8a')

    def tearDown(self):
        shutil.rmtree(self.t)

    def run_tool(self, rel, *args, **env):
        e = dict(os.environ, PMUNITY_LLVM=self.llvm)
        e.pop('PMUNITY_IL2CPP_DEBUG_INFO', None)
        e.update(env)
        r = subprocess.run([os.path.join(self.ndk, rel)] + list(args), capture_output=True, text=True, env=e, check=True)
        return r.stdout.split()

    def test_compile(self):
        tc = os.path.join(self.ndk, 'toolchains/llvm/prebuilt/linux-x86_64')
        out = self.run_tool('toolchains/llvm/prebuilt/linux-x86_64/bin/clang++', '-c', '-g', '-D__ANDROID_API__=21',
                            '-Os', '-target', 'aarch64-linux-android21', 'a.cpp', '-o', 'a.o')
        self.assertEqual(out[0], 'clang++')
        self.assertNotIn('-g', out)
        self.assertNotIn('-D__ANDROID_API__=21', out)
        self.assertIn('--sysroot=' + tc + '/sysroot', out)
        self.assertIn('--gcc-toolchain=' + tc, out)
        self.assertNotIn('-fuse-ld=lld', out)
        out = self.run_tool('toolchains/llvm/prebuilt/linux-x86_64/bin/clang', '-c', '-g', 'a.c', PMUNITY_IL2CPP_DEBUG_INFO='1')
        self.assertEqual(out[0], 'clang')
        self.assertIn('-g', out)

    def test_link(self):
        out = self.run_tool('toolchains/llvm/prebuilt/linux-x86_64/bin/clang++', '@objs', '-o', 'libx.so', '-shared',
                            '-fuse-ld=bfd', '-gcc-toolchain', '/g', '--sysroot', '/s', '-llog')
        self.assertNotIn('-fuse-ld=bfd', out)
        self.assertIn('-fuse-ld=lld', out)
        self.assertIn('--gcc-toolchain=/g', out)
        self.assertNotIn('-gcc-toolchain', out)
        self.assertEqual(sum(1 for x in out if x.startswith('--sysroot')), 1)        # theirs, not ours
        self.assertIn('-rtlib=libgcc', out)
        self.assertIn('-unwindlib=none', out)

    def test_api_level(self):
        out = self.run_tool('toolchains/llvm/prebuilt/linux-x86_64/bin/clang', '-c', '-target', 'aarch64-none-linux-android',
                            '-D__ANDROID_API__=21', 'a.c')
        self.assertIn('aarch64-none-linux-android21', out)
        self.assertNotIn('-D__ANDROID_API__=21', out)
        out = self.run_tool('toolchains/llvm/prebuilt/linux-x86_64/bin/clang', '-c', '-target', 'armv7-linux-androideabi19', 'a.c')
        self.assertIn('armv7-linux-androideabi19', out)

    def test_binutils(self):
        out = self.run_tool('toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64/bin/aarch64-linux-android-objcopy',
                            '--strip-all', 'a', 'b')
        self.assertEqual(out, ['llvm-objcopy', '--strip-all', 'a', 'b'])


class AndroidDataIl2cppTest(unittest.TestCase):
    def test_options_key(self):
        """a Mono conversion's key (and so its output) is the same as before --il2cpp existed"""
        base = dict(video_shader=True, platform_patch='data', graphics_apis='11', mt_rendering='keep', blit='never',
                    rewired_joysticks=4, name='Game', async_upload_buffer=0, inline_textures=True, swappy='off',
                    unity=UV.Version('2017.4.9f1'))
        mono = AD.opts_key(types.SimpleNamespace(**base))
        self.assertEqual(AD.opts_key(types.SimpleNamespace(il2cpp=None, **base)), mono)
        self.assertEqual(AD.opts_key(types.SimpleNamespace(il2cpp='/x', **base)), mono + ['il2cpp=/x'])

    def test_items(self):
        args = types.SimpleNamespace(il2cpp='/il2', unity=UV.Version('2019.4.8f1'), il2cpp_info={
            'files': ['Data/Managed/Metadata/global-metadata.dat', 'Data/Managed/Resources/mscorlib.dll-resources.dat',
                      'Data/Managed/etc/mono/config'],
            'assemblies': ['Assembly-CSharp.dll', 'mscorlib.dll']})
        data = os.path.join('Game', 'assets', 'bin', 'Data')
        items = [(os.path.join(data, 'globalgamemanagers'), '/src/globalgamemanagers', 'ggm'),
                 (os.path.join(data, 'Managed', 'Assembly-CSharp.dll'), '/src/Managed/Assembly-CSharp.dll', 'link'),
                 (os.path.join(data, 'Managed', 'etc', 'mono', 'config'), '/src/etc/mono/config', 'monoconfig'),
                 (os.path.join('Game', 'assets', 'Logo.png'), '/src/StreamingAssets/Logo.png', 'link')]
        got = AD.il2cpp_items(args, items, data)
        self.assertEqual([d for d, s, a in got], [
            os.path.join(data, 'globalgamemanagers'), os.path.join('Game', 'assets', 'Logo.png'),
            os.path.join(data, 'Managed', 'Metadata', 'global-metadata.dat'),
            os.path.join(data, 'Managed', 'Resources', 'mscorlib.dll-resources.dat'),
            os.path.join(data, 'Managed', 'etc', 'mono', 'config')])
        self.assertEqual(got[2][1], os.path.join('/il2', 'Data/Managed/Metadata/global-metadata.dat'))
        self.assertEqual(args.managed_names, {'Assembly-CSharp.dll', 'mscorlib.dll'})

    def test_build_date_2020(self):
        """2020.x: boot.config gets a buildDate (libunity's re-extraction stamp) from the metadata"""
        with tempfile.TemporaryDirectory() as t:
            md = os.path.join(t, 'Data', 'Managed', 'Metadata')
            os.makedirs(md)
            with open(os.path.join(md, 'global-metadata.dat'), 'wb') as f:
                f.write(struct.pack('<Ii', 0xFAB11BAF, 27))
            args = types.SimpleNamespace(il2cpp=t, unity=UV.Version('2020.3.49f1'), il2cpp_info={
                'files': ['Data/Managed/Metadata/global-metadata.dat'], 'assemblies': []})
            data = os.path.join('Game', 'assets', 'bin', 'Data')
            got = AD.il2cpp_items(args, [(os.path.join(data, 'boot.config'), '/src/boot.config', 'link')], data)
            self.assertEqual(got[0][2], 'il2cppboot')
            self.assertTrue(6 * 10 ** 17 <= args.il2cpp_build_date < 7 * 10 ** 17)
            args.unity = UV.Version('2021.3.16f1')
            got = AD.il2cpp_items(args, [(os.path.join(data, 'boot.config'), '/src/boot.config', 'link')], data)
            self.assertEqual(got[0][2], 'link')


if __name__ == '__main__':
    unittest.main()
