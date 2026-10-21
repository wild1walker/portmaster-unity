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


if __name__ == '__main__':
    unittest.main()
