"""Unit tests of macpkg on small synthetic installer packages built here (no game data, no macOS
pkgbuild needed; contributed with PR #1): python -m unittest discover -s tests   (from convert/)"""
import gzip
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import macpkg


def cpio(entries):
    """odc cpio of (name, data or None for a directory)."""
    out = b''
    for name, data in entries + [('TRAILER!!!', b'')]:
        mode = 0o040755 if data is None else 0o100644
        data = data or b''
        n = name.encode() + b'\0'
        out += b'070707' + b'%06o%06o%06o%06o%06o%06o%06o%011o%06o%011o' % (
            0, 0, mode, 0, 0, 1, 0, 0, len(n), len(data)) + n + data
    return out


def xar(members):
    """xar archive of (path in the archive, data); data stored gzip-encoded (zlib), as pkgs do."""
    heap, files = b'', {}
    for path, data in members:
        z = zlib.compress(data)
        files[path] = (len(heap), len(z), len(data))
        heap += z
    def tree(prefix):
        names = sorted({p[len(prefix):].split('/')[0] for p in files if p.startswith(prefix)})
        xml = ''
        for nm in names:
            p = prefix + nm
            if p in files:
                off, ln, size = files[p]
                xml += (f'<file><name>{nm}</name><type>file</type><data><offset>{off}</offset>'
                        f'<length>{ln}</length><size>{size}</size>'
                        f'<encoding style="application/x-gzip"/></data></file>')
            else:
                xml += f'<file><name>{nm}</name><type>directory</type>{tree(p + "/")}</file>'
        return xml
    toc = f'<?xml version="1.0"?><xar><toc>{tree("")}</toc></xar>'.encode()
    tz = zlib.compress(toc)
    return b'xar!' + struct.pack('>HHQQI', 28, 1, len(tz), len(toc), 0) + tz + heap


INFO = b'<pkg-info install-location="/Applications" identifier="test"/>'


def gog_pkg(kind, files):
    post = f'#!/bin/bash\ngog_bundle_name="Game.app"\ngog_installer_type="{kind}"\n'.encode()
    scripts = gzip.compress(cpio([('.', None), ('./.DS_Store', b'x'), ('./payload', None)]
                                 + [('./payload/' + p, d) for p, d in files]
                                 + [('./postinstall', post)]))
    return xar([('Distribution', b'<installer-gui-script/>'), ('package.pkg/PackageInfo', INFO),
                ('package.pkg/Scripts', scripts)])


class MacpkgTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write(self, name, data):
        p = os.path.join(self.tmp, name)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def read(self, *rel):
        with open(os.path.join(self.tmp, 'root', *rel), 'rb') as f:
            return f.read()

    def test_payload(self):
        payload = gzip.compress(cpio([('.', None), ('./Game.app/Contents/Info.plist', b'plist'),
                                      ('./Game.app/._Info.plist', b'meta')]))
        pkg = self.write('a.pkg', xar([('PackageInfo', INFO), ('Payload', payload)]))
        n, total, _ = macpkg.extract(pkg, os.path.join(self.tmp, 'root'), log=lambda *a: None)
        self.assertEqual((n, total), (1, 5))
        self.assertEqual(self.read('Applications', 'Game.app', 'Contents', 'Info.plist'), b'plist')
        self.assertFalse(os.path.exists(os.path.join(self.tmp, 'root', 'Applications', 'Game.app', '._Info.plist')))

    def test_gog_scripts_payload_with_dlc(self):
        root = os.path.join(self.tmp, 'root')
        game = self.write('game.pkg', gog_pkg('game', [('Contents/Data/a', b'game a'), ('Contents/Data/b', b'game b')]))
        dlc = self.write('dlc.pkg', gog_pkg('dlc', [('Contents/Data/b', b'dlc b'), ('Contents/Data/c', b'dlc c')]))
        for pkg in (game, dlc):
            macpkg.extract(pkg, root, log=lambda *a: None)
        app = ('Applications', 'Game.app', 'Contents', 'Data')
        self.assertEqual(self.read(*app, 'a'), b'game a')
        self.assertEqual(self.read(*app, 'b'), b'dlc b')      # the DLC's copy wins, as cp -rf
        self.assertEqual(self.read(*app, 'c'), b'dlc c')
        self.assertEqual(sorted(os.listdir(root)), ['Applications'])   # no staging or .DS_Store left

    def test_scripts_payload_without_gog_postinstall(self):
        root = os.path.join(self.tmp, 'root')
        scripts = gzip.compress(cpio([('./payload', None), ('./payload/x', b'x'), ('./postinstall', b'#!/bin/sh\n')]))
        pkg = self.write('s.pkg', xar([('PackageInfo', INFO), ('Scripts', scripts)]))
        macpkg.extract(pkg, root, log=lambda *a: None)
        self.assertEqual(self.read('Applications', 'x'), b'x')       # no bundle name: the install location
        self.assertEqual(os.listdir(root), ['Applications'])

    def test_incomplete_installer(self):
        data = gog_pkg('dlc', [('Contents/Data/a', b'dlc a' * 1000)])
        pkg = self.write('dlc.pkg', data[:-200])                     # a download that stopped early
        with self.assertRaisesRegex(macpkg.Incomplete, 'dlc.pkg is incomplete'):
            macpkg.extract(pkg, os.path.join(self.tmp, 'root'), log=lambda *a: None)
        with self.assertRaises(SystemExit) as e:
            macpkg.main([os.path.join(self.tmp, 'root'), pkg])
        self.assertEqual(e.exception.code, 3)

if __name__ == '__main__':
    unittest.main()
