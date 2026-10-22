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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_port import write_assembly


class NdkTest(unittest.TestCase):
    def test_ndk_per_line(self):
        self.assertEqual(I.ndk_for(UV.Version('2017.4.40f1'))[:2], ('r13b', '13.1.3345770'))
        self.assertEqual(I.ndk_for(UV.Version('2018.2.21f1'))[0], 'r13b')
        self.assertEqual(I.ndk_for(UV.Version('2018.3.0f2'))[0], 'r16b')
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
        self.assertEqual((I.api_level('r13b', 'armeabi-v7a'), I.api_level('r13b', 'arm64-v8a')), (16, 21))

    def test_ndk_members_r13b(self):
        """r13b (2017.x il2cpp): the platform's own headers and libraries, GNU libstdc++, libgcc"""
        w = lambda n, abi='arm64-v8a': I.ndk_wanted('android-ndk-r13b/' + n, 'r13b', abi)
        self.assertTrue(w('platforms/android-21/arch-arm64/usr/include/stdio.h'))
        self.assertTrue(w('platforms/android-21/arch-arm64/usr/lib/libc.so'))
        self.assertIsNone(w('platforms/android-23/arch-arm64/usr/include/stdio.h'))
        self.assertTrue(w('platforms/android-16/arch-arm/usr/include/stdio.h', 'armeabi-v7a'))
        self.assertTrue(w('sources/cxx-stl/gnu-libstdc++/4.9/libs/arm64-v8a/include/bits/c++config.h'))
        self.assertTrue(w('sources/cxx-stl/gnu-libstdc++/4.9/libs/arm64-v8a/libgnustl_static.a'))
        self.assertIsNone(w('sources/cxx-stl/gnu-libstdc++/4.9/libs/arm64-v8a/libgnustl_shared.so'))
        self.assertTrue(w('toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64/lib/gcc/aarch64-linux-android/4.9.x/libgcc.a'))
        self.assertIsNone(w('toolchains/llvm/prebuilt/linux-x86_64/bin/clang'))


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

    def test_class_library_folders(self):
        """a class library folder: its assemblies and its Mono's etc (il2cpp copies that into the data)"""
        u = 'Mono/lib/mono/unity'
        self.assertTrue(I.editor_wanted('Mono/lib/mono/unity/mscorlib.dll', u))
        self.assertTrue(I.editor_wanted('Mono/etc/mono/2.0/machine.config', u))
        self.assertTrue(I.editor_wanted('MonoBleedingEdge/etc/mono/config', u))
        self.assertTrue(I.editor_wanted('il2cpp/libil2cpp/il2cpp-api.h', u))
        self.assertFalse(I.editor_wanted('Mono/lib/mono/unity/mscorlib.dll.mdb', u))
        self.assertFalse(I.editor_wanted('Mono/lib/mono/unity/smcs.exe', u))
        self.assertFalse(I.editor_wanted('Mono/lib/mono/2.0/mscorlib.dll', u))
        self.assertFalse(I.editor_wanted('MonoBleedingEdge/lib/mono/unityaot/mscorlib.dll', u))
        f = 'MonoBleedingEdge/lib/mono/4.5'
        self.assertTrue(I.editor_wanted('MonoBleedingEdge/lib/mono/4.5/Facades/System.Runtime.dll', f))
        self.assertFalse(I.editor_wanted('MonoBleedingEdge/lib/mono/4.5/mscorlib.dll.dylib', f))
        self.assertFalse(I.editor_wanted('Mono/etc/mono/config', f))

    def test_class_library_complete(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertFalse(I.bcl_complete(t, I.BCL_DIR))
            os.makedirs(os.path.join(t, I.BCL_DIR))
            open(os.path.join(t, I.BCL_DIR, 'mscorlib.dll'), 'w').close()
            self.assertFalse(I.bcl_complete(t, I.BCL_DIR))
            open(os.path.join(t, '.complete'), 'w').close()
            self.assertTrue(I.bcl_complete(t, I.BCL_DIR))       # (a home from before the per-folder marks)
            u = 'Mono/lib/mono/unity'
            os.makedirs(os.path.join(t, u))
            open(os.path.join(t, u, 'mscorlib.dll'), 'w').close()
            self.assertFalse(I.bcl_complete(t, u))
            open(os.path.join(t, u, '.complete'), 'w').close()
            self.assertTrue(I.bcl_complete(t, u))

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


class ProfileTest(unittest.TestCase):
    def test_profiles(self):
        v = UV.Version
        self.assertEqual(I.bcl_profile(v('2017.4.40f1'), 2, True), ('legacyunity', 'Mono/lib/mono/unity'))
        self.assertEqual(I.bcl_profile(v('2017.4.40f1'), 2), ('net20', 'Mono/lib/mono/2.0'))
        self.assertEqual(I.bcl_profile(v('2018.4.36f1'), 2, True), ('legacyunity', 'Mono/lib/mono/unity'))
        self.assertEqual(I.bcl_profile(v('2017.4.40f1'), 4), ('net45', 'MonoBleedingEdge/lib/mono/4.5'))
        self.assertEqual(I.bcl_profile(v('2018.1.0f1'), 4), ('unityaot', 'MonoBleedingEdge/lib/mono/unityaot'))
        self.assertEqual(I.bcl_profile(v('2020.3.49f1'), 4), ('unityaot', I.BCL_DIR))
        with self.assertRaises(UV.Unsupported):
            I.bcl_profile(v('2019.4.8f1'), 2)

    def test_dotnet_level(self):
        with tempfile.TemporaryDirectory() as t:
            m = os.path.join(t, 'Managed')
            os.makedirs(m)
            with self.assertRaises(UV.Unsupported):
                I.dotnet_level(t)
            self.assertEqual(I.dotnet_level(t, UV.Version('2019.4.8f1')), 4)
            # the old Mono's "unity" profile (".NET 2.0 Subset"), its "2.0" one, .NET 4.x
            write_assembly(os.path.join(m, 'mscorlib.dll'), 'mscorlib', [('System', 'Object'), ('System', 'MonoTouchAOTHelper')],
                           version=2)
            self.assertEqual((I.dotnet_level(t), I.subset_profile(t)), (2, True))
            write_assembly(os.path.join(m, 'mscorlib.dll'), 'mscorlib', [('System', 'Object'), ('System', 'TermInfoDriver')],
                           version=2)
            self.assertEqual((I.dotnet_level(t), I.subset_profile(t)), (2, False))
            write_assembly(os.path.join(m, 'mscorlib.dll'), 'mscorlib', [('System', 'Object')], version=4)
            self.assertEqual((I.dotnet_level(t), I.subset_profile(t)), (4, False))
            os.unlink(os.path.join(m, 'mscorlib.dll'))      # (none shipped: what Assembly-CSharp references)
            write_assembly(os.path.join(m, 'Assembly-CSharp.dll'), 'Assembly-CSharp', [('', 'Menu')])
            with self.assertRaises(UV.Unsupported):
                I.dotnet_level(t)

    def test_tool_version(self):
        v = UV.Version
        self.addCleanup(setattr, I, 'log', I.log)
        I.log = lambda m: None
        self.assertEqual(str(I.tool_version(v('2017.4.9f1'), 'arm64-v8a')), '2017.4.40f1')     # no arm64 player before .16
        self.assertEqual(str(I.tool_version(v('2017.4.9f1'), 'armeabi-v7a')), '2017.4.9f1')
        self.assertEqual(str(I.tool_version(v('2017.4.16f1'), 'arm64-v8a')), '2017.4.16f1')
        self.assertEqual(str(I.tool_version(v('2017.4.9f1'), 'arm64-v8a', '2017.4.30f1')), '2017.4.30f1')
        self.assertEqual(str(I.tool_version(v('2019.4.8f1'), 'arm64-v8a')), '2019.4.8f1')
        with self.assertRaises(UV.Unsupported):
            I.tool_version(v('2017.4.9f1'), 'arm64-v8a', '2018.4.36f1')

    def test_link_args_2017(self):
        a = I.link_args_2017(2, '/s', '/l', ['B.dll', 'A.dll'], '/d', subset=True)
        self.assertEqual(a[:6], ['--api=NET_2_0_Subset', '--out=/l', '--i18n=none', '--core-action=link',
                                 '--descriptor-directory=/d', '--search-directory=/s'])
        self.assertEqual(a[6:], ['--include-assembly=/s/A.dll', '--include-assembly=/s/B.dll'])
        self.assertEqual(I.link_args_2017(2, '/s', '/l', [], '/d')[0], '--api=NET_2_0')
        self.assertEqual(I.link_args_2017(4, '/s', '/l', [], '/d')[0], '--api=NET_4_6')
        self.assertIn('--include-link-xml=/k.xml', I.link_args_2017(2, '/s', '/l', [], '/d', keep_xml='/k.xml'))

    def test_engine_keep_xml(self):
        """the engine assemblies: kept whole, linked (so the linker's stubs are written into them)"""
        import xml.etree.ElementTree as ET
        with tempfile.TemporaryDirectory() as t:
            p = I.engine_keep_xml(['UnityEngine.dll', 'UnityEngine.CoreModule.dll'], os.path.join(t, 'k.xml'))
            root = ET.parse(p).getroot()
            self.assertEqual(root.tag, 'linker')
            self.assertEqual([(a.get('fullname'), a.get('preserve')) for a in root],
                             [('UnityEngine.CoreModule', 'all'), ('UnityEngine', 'all')])

    def test_linker_descriptors(self):
        with tempfile.TemporaryDirectory() as t:
            d = os.path.join(t, 'editor', 'il2cpp', 'LinkerDescriptors')
            os.makedirs(d)
            for n in ('mscorlib.xml', 'mscorlib45.xml', 'mscorlib_mono.xml', 'System.xml', 'System45.xml', 'Mono.Security.xml'):
                with open(os.path.join(d, n), 'w') as f:
                    f.write(n)
            ed, work = os.path.join(t, 'editor'), os.path.join(t, 'work')
            self.assertEqual(I.linker_descriptors(ed, 'legacyunity', work), d)
            got = I.linker_descriptors(ed, 'net45', work)
            self.assertEqual(sorted(os.listdir(got)), ['Mono.Security.xml', 'System.xml', 'mscorlib.xml'])
            for n, src in (('mscorlib.xml', 'mscorlib45.xml'), ('Mono.Security.xml', 'Mono.Security.xml')):
                with open(os.path.join(got, n)) as f:
                    self.assertEqual(f.read(), src)

    def test_il2cpp_profile_arg(self):
        a = I.il2cpp_args(UV.Version('2017.4.40f1'), 'arm64-v8a', '/m', '/c', '/k', '/o.so', '/ndk', {}, '/p', 'legacyunity')
        self.assertIn('--dotnetprofile=legacyunity', a)
        self.assertNotIn('--dotnetprofile=unityaot', a)
        self.assertIn('--map-file-parser=/bin/true', a)
        a = I.il2cpp_args(UV.Version('2018.4.36f1'), 'arm64-v8a', '/m', '/c', '/k', '/o.so', '/ndk', {}, '/p')
        self.assertFalse(any(x.startswith('--map-file-parser') for x in a))


def write_elf(path, syms, is64=True):
    """An ELF file with only a symbol table (SHT_SYMTAB) and its strings: syms = [(name, value,
    size, type)] (type 2 = function), defined in section 1."""
    strtab = b'\0' + b''.join(n.encode() + b'\0' for n, _, _, _ in syms)
    ent = 24 if is64 else 16
    tab, pos = bytes(ent), 1
    for n, v, size, typ in syms:
        info = (1 << 4) | typ                                   # STB_GLOBAL
        tab += struct.pack('<IBBHQQ', pos, info, 0, 1, v, size) if is64 else struct.pack('<IIIBBH', pos, v, size, info, 0, 1)
        pos += len(n) + 1
    hdr = 64 if is64 else 52
    shoff = hdr + len(tab) + len(strtab)
    if is64:
        sh = lambda typ, off, size, link, entsize: struct.pack('<IIQQQQIIQQ', 0, typ, 0, 0, off, size, link, 0, 1, entsize)
        head = bytearray(b'\x7fELF\x02\x01\x01' + bytes(hdr - 7))
        struct.pack_into('<Q', head, 0x28, shoff); struct.pack_into('<HH', head, 0x3a, 64, 3)
    else:
        sh = lambda typ, off, size, link, entsize: struct.pack('<IIIIIIIIII', 0, typ, 0, 0, off, size, link, 0, 1, entsize)
        head = bytearray(b'\x7fELF\x01\x01\x01' + bytes(hdr - 7))
        struct.pack_into('<I', head, 0x20, shoff); struct.pack_into('<HH', head, 0x2e, 40, 3)
    with open(path, 'wb') as f:
        f.write(bytes(head) + tab + strtab + sh(0, 0, 0, 0, 0) + sh(2, hdr, len(tab), 2, ent) +
                sh(3, hdr + len(tab), len(strtab), 0, 0))


class SourceFixTest(unittest.TestCase):
    def test_arm_clobbers(self):
        src = ('    __asm__ __volatile__\n    (\n        "ldrexd\\t%1, %2, [%3]\\n\\t"\n'
               '        : "=&r" (success), "=&r" (lo), "=&r" (hi)\n        : "r" (p)\n        : "cc", "r2", "r3"\n    );\n'
               '        : "cc", "memory", "r0", "r1", "r3"\n        : "cc", "memory"\n')
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'il2cpp', 'libil2cpp', 'os', 'Unity', 'ExtendedAtomicOps-arm.h')
            os.makedirs(os.path.dirname(p))
            with open(p, 'w') as f:
                f.write(src)
            self.assertEqual(I.fix_editor_sources(t), ['il2cpp/libil2cpp/os/Unity/ExtendedAtomicOps-arm.h'])
            with open(p) as f:
                got = f.read()
            self.assertNotIn('"r2"', got)
            self.assertNotIn('"r3"', got)
            self.assertIn('        : "cc"\n    );', got)
            self.assertEqual(got.count('        : "cc", "memory"\n'), 2)
            self.assertIn(': "=&r" (success), "=&r" (lo), "=&r" (hi)', got)       # (operands untouched)
            self.assertEqual(I.fix_editor_sources(t), [])                           # idempotent


class SymbolMapTest(unittest.TestCase):
    def read(self, path):
        with open(path, 'rb') as f:
            d = f.read()
        n, = struct.unpack_from('<i', d)
        self.assertEqual(len(d), 4 + 12 * n)
        return [struct.unpack_from('<QI', d, 4 + 12 * i) for i in range(n)]

    def test_arm64(self):
        with tempfile.TemporaryDirectory() as t:
            elf, out = os.path.join(t, 'libil2cpp.sym'), os.path.join(t, 'SymbolMap-ARM64')
            write_elf(elf, [('b', 0x2000, 16, 2), ('a', 0x1000, 32, 2), ('data', 0x3000, 8, 1), ('zero', 0x4000, 0, 2)])
            self.assertEqual(I.write_symbol_map(elf, out), 2)          # functions with a size only, sorted
            self.assertEqual(self.read(out), [(0x1000, 32), (0x2000, 16)])

    def test_armv7_thumb(self):
        with tempfile.TemporaryDirectory() as t:
            elf, out = os.path.join(t, 'libil2cpp.sym'), os.path.join(t, 'SymbolMap-ARMv7')
            write_elf(elf, [('t', 0x1001, 10, 2), ('a', 0x2000, 4, 2)], is64=False)
            I.write_symbol_map(elf, out, thumb=True)
            self.assertEqual(self.read(out), [(0x1000, 10), (0x2000, 4)])


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
        self.assertIn('--dotnetprofile=unityaot', a)
        self.assertIn('--dotnetprofile=legacyunity',
                      I.link_args(UV.Version('2018.4.36f1'), 'arm64-v8a', '/s', '/l', [], 'legacyunity'))
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

    def test_pic_wins(self):
        """2017.4's il2cpp: -fPIC then -fPIE for a shared library's objects: PIC stays"""
        out = self.run_tool('toolchains/llvm/prebuilt/linux-x86_64/bin/clang++', '-c', '-fPIC', '-Os', '-fPIE', 'a.cpp')
        self.assertIn('-fPIC', out)
        self.assertNotIn('-fPIE', out)
        out = self.run_tool('toolchains/llvm/prebuilt/linux-x86_64/bin/clang', '-c', '-fPIE', 'a.c')
        self.assertIn('-fPIE', out)

    def test_binutils(self):
        out = self.run_tool('toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64/bin/aarch64-linux-android-objcopy',
                            '--strip-all', 'a', 'b')
        self.assertEqual(out, ['llvm-objcopy', '--strip-all', 'a', 'b'])
        out = self.run_tool('toolchains/llvm/prebuilt/linux-x86_64/aarch64-linux-android/bin/objcopy', '--strip-all', 'a', 'b')
        self.assertEqual(out, ['llvm-objcopy', '--strip-all', 'a', 'b'])     # (r21d's, Unity 2021.x's Bee build)

    def test_runtime_r23b(self):
        """r23b has no libgcc: links take compiler-rt and libunwind from its clang's resource folder"""
        tool = 'toolchains/llvm/prebuilt/linux-x86_64/bin/clang++'
        tc = os.path.join(self.ndk, 'toolchains/llvm/prebuilt/linux-x86_64')
        os.makedirs(os.path.join(tc, 'lib/gcc/aarch64-linux-android/4.9.x'))      # (r23b: crtbegin.o, no libgcc.a)
        open(os.path.join(tc, 'lib/gcc/aarch64-linux-android/4.9.x/crtbegin.o'), 'w').close()
        os.makedirs(os.path.join(tc, 'lib64/clang/12.0.8/lib/linux/aarch64'))
        out = self.run_tool(tool, '@objs.rsp', '-shared', '-o', 'x.so')
        self.assertIn('-rtlib=compiler-rt', out)
        self.assertIn('-unwindlib=libunwind', out)
        self.assertIn('-resource-dir=' + os.path.join(tc, 'lib64/clang/12.0.8'), out)
        self.assertNotIn('-rtlib=libgcc', out)
        out = self.run_tool(tool, '-c', 'a.cpp')                        # (compiles: clang's own headers)
        self.assertFalse(any(a.startswith(('-resource-dir', '-rtlib')) for a in out))
        open(os.path.join(tc, 'lib/gcc/aarch64-linux-android/4.9.x/libgcc.a'), 'w').close()   # (r19 - r21d)
        self.assertIn('-rtlib=libgcc', self.run_tool(tool, '@objs.rsp', '-shared', '-o', 'x.so'))

    def test_libcxx_default(self):
        """the unified sysroot's libc++ for C++ without -stdlib (Unity 2021.x / 2022.x's Bee build), as the
        NDK's clang++ does; nothing added for C, an explicit -stdlib, or an NDK without libc++ headers"""
        tool = 'toolchains/llvm/prebuilt/linux-x86_64/bin/clang++'
        self.assertNotIn('-stdlib=libc++', self.run_tool(tool, '-c', 'a.cpp'))
        os.makedirs(os.path.join(self.ndk, 'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/c++/v1'))
        self.assertIn('-stdlib=libc++', self.run_tool(tool, '-c', 'a.cpp'))
        self.assertIn('-stdlib=libc++', self.run_tool(tool, '@objs.rsp', '-shared', '-static-libstdc++', '-o', 'x.so'))
        self.assertEqual(self.run_tool(tool, '-c', '-stdlib=libstdc++', 'a.cpp').count('-stdlib=libc++'), 0)
        self.assertNotIn('-stdlib=libc++', self.run_tool('toolchains/llvm/prebuilt/linux-x86_64/bin/clang', '-c', 'a.c'))
        self.assertNotIn('-stdlib=libc++', self.run_tool(tool, '-c', '-x', 'c', 'gc.c'))     # (Bee: C through clang++)


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

    def test_player_match(self):
        """libil2cpp.so goes with the data it was built from and the player of its IL2CPP's release"""
        v = UV.Version
        info = {'unity': '2017.4.40f1', 'data_unity': '2017.4.9f1'}
        self.assertIsNone(AD.il2cpp_mismatch(info, v('2017.4.9f1'), v('2017.4.40f1')))
        self.assertIsNone(AD.il2cpp_mismatch(info, v('2017.4.9f1'), None))
        self.assertIn('2017.4.9f1 data', AD.il2cpp_mismatch(info, v('2017.4.40f1'), v('2017.4.40f1')))
        self.assertIn('player package is 2017.4.9f1', AD.il2cpp_mismatch(info, v('2017.4.9f1'), v('2017.4.9f1')))
        self.assertIsNone(AD.il2cpp_mismatch({'unity': '2019.4.8f1'}, v('2019.4.8f1'), v('2019.4.8f1')))  # (older il2cpp.json)

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
            self.assertNotIn('il2cppguid', [a for d, s, a in got])
            # 2021.x+: boot.config as it is; the stamp is unity_app_guid, a GUID from the metadata
            args.unity = UV.Version('2021.3.16f1')
            got = AD.il2cpp_items(args, [(os.path.join(data, 'boot.config'), '/src/boot.config', 'link')], data)
            self.assertEqual(got[0][2], 'link')
            self.assertEqual(got[-1], (os.path.join(data, 'unity_app_guid'),
                                       os.path.join(md, 'global-metadata.dat'), 'il2cppguid'))
            g = AD.il2cpp_app_guid(os.path.join(md, 'global-metadata.dat'))
            self.assertRegex(g, r'^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$')
            self.assertEqual(g, AD.il2cpp_app_guid(os.path.join(md, 'global-metadata.dat')))


if __name__ == '__main__':
    unittest.main()
