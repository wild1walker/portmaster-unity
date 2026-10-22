"""Unit tests of pmunity.winbcl (the class libraries of Unity 2021.2+ Windows builds) on hand-made
files: python -m unittest tests.test_winbcl   (from convert/)"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
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
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
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


if __name__ == '__main__':
    unittest.main()
