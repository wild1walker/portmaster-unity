"""Unit tests of pmunity.winbcl (the class libraries of Unity 2021.2+ Windows builds) on hand-made
files: python -m unittest tests.test_winbcl   (from convert/)"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pmunity import winbcl as W


class WinBclTest(unittest.TestCase):
    def test_profile(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(W.is_win32_profile(d))                 # no mscorlib.dll
            with open(os.path.join(d, 'mscorlib.dll'), 'wb') as f:
                f.write(b'MZ...Interop+Sys...')                     # a Unix profile's
            self.assertFalse(W.is_win32_profile(d))
            with open(os.path.join(d, 'mscorlib.dll'), 'wb') as f:
                f.write(b'MZ...Interop+Kernel32...SetThreadErrorMode...')
            self.assertTrue(W.is_win32_profile(d))

    def test_installer_paths(self):
        self.assertEqual(W._rel('$_OUTDIR/MonoBleedingEdge/lib/mono/unityjit-linux/System.dll'),
                         'MonoBleedingEdge/lib/mono/unityjit-linux/System.dll')
        self.assertTrue(W._rel('$_OUTDIR/' + W.LINUX + 'Facades/netstandard.dll').startswith(W.LINUX))

    def test_game_assemblies_kept(self):
        # a DLL named like a class library but of another version (a NuGet package's) stays the game's
        from test_nsis import installer
        from test_port import write_assembly
        with tempfile.TemporaryDirectory() as d:
            def asm(name, version):
                p = os.path.join(d, 'tmp.dll')
                write_assembly(p, name, version=version)
                with open(p, 'rb') as f:
                    return f.read()
            files = [(f'{prof}/{n}.dll', asm(n, 4)) for prof in ('win', 'linux') for n in ('Lib', 'Annotations')]
            exe = os.path.join(d, 'setup.exe')
            with open(exe, 'wb') as f:
                f.write(installer('classic', files))
            managed = os.path.join(d, 'Managed')
            os.makedirs(managed)
            write_assembly(os.path.join(managed, 'Lib.dll'), 'Lib', version=4)                 # Unity's own
            write_assembly(os.path.join(managed, 'Annotations.dll'), 'Annotations', version=5)  # the game's
            keep = W.WIN, W.LINUX
            W.WIN, W.LINUX = 'sub/win/', 'sub/linux/'
            try:
                got = W.replacements(managed, exe, os.path.join(d, 'out'), log=lambda m: None)
            finally:
                W.WIN, W.LINUX = keep
            self.assertEqual([n for n, _ in got], ['Lib.dll'])
            self.assertEqual(W.assembly_version(got[0][1]), (4, 0, 0, 0))
            self.assertFalse(os.path.exists(os.path.join(d, 'out', 'Annotations.dll')))

    def test_facade_profile(self):
        from test_port import write_assembly
        self.assertEqual(W.facade_profile('6000.2.6f2'), W.LINUX)
        self.assertEqual(W.facade_profile('2021.2.0f1'), W.LINUX)
        self.assertEqual(W.facade_profile('2020.3.48f1'), W.UNITYJIT)
        self.assertIsNone(W.facade_profile('2017.4.40f1'))
        with tempfile.TemporaryDirectory() as d:
            write_assembly(os.path.join(d, 'mscorlib.dll'), 'mscorlib', version=2)     # a .NET 3.5 build
            self.assertIsNone(W.facade_profile('2018.4.36f1', d))
            write_assembly(os.path.join(d, 'mscorlib.dll'), 'mscorlib', version=4)
            self.assertEqual(W.facade_profile('2018.4.36f1', d), W.UNITYJIT)

    def test_facades(self):
        """A build without netstandard.dll that the player's engine modules reference: Unity's facade
        of the game's profile from the installer; a missing package assembly is only named; nothing
        is read when nothing is missing."""
        from test_nsis import installer
        from test_port import write_assembly, write_sf

        def asm(d, name, version=2):
            p = os.path.join(d, 'tmp.dll')
            write_assembly(p, name, version=version)
            with open(p, 'rb') as f:
                return f.read()
        with tempfile.TemporaryDirectory() as d:
            files = [('linux/Facades/netstandard.dll', asm(d, 'netstandard')), ('linux/System.dll', asm(d, 'System', 4)),
                     ('win/System.dll', asm(d, 'System', 4)), ('jit/Facades/netstandard.dll', asm(d, 'netstandard', 1))]
            exe = os.path.join(d, 'setup.exe')
            with open(exe, 'wb') as f:
                f.write(installer('classic', files))
            data = os.path.join(d, 'Game_Data')
            os.makedirs(os.path.join(data, 'Managed'))
            write_sf(os.path.join(data, 'globalgamemanagers'), '6000.2.6f2')
            write_assembly(os.path.join(data, 'Managed', 'mscorlib.dll'), 'mscorlib', version=4)
            write_assembly(os.path.join(data, 'Managed', 'System.dll'), 'System', version=4)
            pm = W.player_managed(os.path.join(d, 'player'))
            os.makedirs(pm)
            write_assembly(os.path.join(pm, 'UnityEngine.CoreModule.dll'), 'UnityEngine.CoreModule',
                           assemblyrefs=['mscorlib', 'netstandard', 'System'])
            write_assembly(os.path.join(pm, 'UnityEngine.AIModule.dll'), 'UnityEngine.AIModule',
                           assemblyrefs=['UnityEngine.CoreModule', 'Unity.Some.Package'])
            self.assertEqual(W.missing(os.path.join(data, 'Managed'), os.path.join(d, 'player')),
                             {'netstandard': ['UnityEngine.CoreModule'], 'Unity.Some.Package': ['UnityEngine.AIModule']})
            keep = W.WIN, W.LINUX, W.UNITYJIT
            W.WIN, W.LINUX, W.UNITYJIT = 'sub/win/', 'sub/linux/', 'sub/jit/'
            msgs = []
            try:
                out = os.path.join(d, 'out')
                rep = W.supply(data, out, player=os.path.join(d, 'player'), installer=exe, log=msgs.append)
                self.assertEqual(rep['class_libraries'], [])                   # (a Linux build's own are kept)
                self.assertEqual(rep['facades'], ['netstandard.dll'])
                self.assertEqual(W.assembly_version(os.path.join(out, 'netstandard.dll')), (2, 0, 0, 0))
                self.assertEqual(rep['not_unity'], {'Unity.Some.Package': ['UnityEngine.AIModule']})
                self.assertEqual(sorted(os.listdir(out)), ['netstandard.dll'])
                # 2020.3: the one profile of that time
                write_sf(os.path.join(data, 'globalgamemanagers'), '2020.3.48f1')
                rep = W.supply(data, os.path.join(d, 'out2'), player=os.path.join(d, 'player'), installer=exe,
                               log=msgs.append)
                self.assertEqual(W.assembly_version(os.path.join(d, 'out2', 'netstandard.dll')), (1, 0, 0, 0))
                # every reference there: no installer is opened (none given, none looked up)
                write_assembly(os.path.join(data, 'Managed', 'netstandard.dll'), 'netstandard', version=2)
                write_assembly(os.path.join(data, 'Managed', 'Unity.Some.Package.dll'), 'Unity.Some.Package')
                rep = W.supply(data, os.path.join(d, 'out3'), player=os.path.join(d, 'player'), log=msgs.append)
                self.assertEqual((rep['facades'], rep['installer']), ([], None))
                self.assertFalse(os.path.exists(os.path.join(d, 'out3')))
                # an installer that cannot be read: only facades wanted, so the setup goes on without them
                os.remove(os.path.join(data, 'Managed', 'netstandard.dll'))
                rep = W.supply(data, os.path.join(d, 'out4'), player=os.path.join(d, 'player'),
                               installer=os.path.join(d, 'nothing.exe'), log=msgs.append)
                self.assertIn('error', rep)
                # a Windows build of 2021.2+: its class libraries are replaced, the facade added
                with open(os.path.join(data, 'Managed', 'mscorlib.dll'), 'ab') as f:
                    f.write(b'Interop+Kernel32 SetThreadErrorMode')
                write_sf(os.path.join(data, 'globalgamemanagers'), '2022.2.17f1')
                rep = W.supply(data, os.path.join(d, 'out5'), player=os.path.join(d, 'player'), installer=exe,
                               log=msgs.append)
                self.assertEqual((rep['class_libraries'], rep['facades']), (['System.dll'], ['netstandard.dll']))
                with self.assertRaises(OSError):            # (and that needs the installer)
                    W.supply(data, os.path.join(d, 'out6'), installer=os.path.join(d, 'nothing.exe'), log=msgs.append)
            finally:
                W.WIN, W.LINUX, W.UNITYJIT = keep


if __name__ == '__main__':
    unittest.main()
