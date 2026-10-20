"""macpkg on installers built like GOG's macOS ones (macOS only: needs pkgbuild / productbuild)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import macpkg

POSTINSTALL = '''#!/bin/bash
gog_bundle_name="Game.app"
gog_installer_type="{kind}"
gog_full_path="/Applications/${{gog_bundle_name}}"
mkdir -p "${{gog_full_path}}"
{copy} payload/* "${{gog_full_path}}"
'''


@unittest.skipUnless(shutil.which('pkgbuild') and shutil.which('productbuild'), 'needs macOS pkgbuild')
class GogLayout(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def make(self, name, kind, files):
        """A GOG-style installer: no payload; the files in Scripts/payload; a postinstall script."""
        sd = os.path.join(self.t, name + '-scripts')
        for rel, data in files.items():
            p = os.path.join(sd, 'payload', rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(data)
        with open(os.path.join(sd, 'postinstall'), 'w') as f:
            f.write(POSTINSTALL.format(kind=kind, copy='cp -rf' if kind == 'dlc' else 'mv'))
        os.chmod(os.path.join(sd, 'postinstall'), 0o755)
        comp = os.path.join(self.t, name + '-comp.pkg')
        out = os.path.join(self.t, name + '.pkg')
        subprocess.run(['pkgbuild', '--nopayload', '--scripts', sd, '--identifier', 'com.gog.pkg-installer',
                        '--install-location', '/Applications', comp], check=True, capture_output=True)
        subprocess.run(['productbuild', '--package', comp, out], check=True, capture_output=True)
        return out

    def test_game_and_dlc(self):
        big = os.urandom(3 << 20)
        game = self.make('game', 'game', {'Contents/Info.plist': b'plist', 'Contents/Resources/Data/level0': big,
                                          'Contents/Resources/goggame-1.info': b'base'})
        dlc = self.make('dlc', 'dlc', {'Contents/Resources/goggame-2.info': b'dlc',
                                       'Contents/Resources/Data/StreamingAssets/dlc_bundle': b'bundle'})
        root = os.path.join(self.t, 'root')
        for p in (game, dlc):
            macpkg.extract(p, root, log=lambda m: None)
        app = os.path.join(root, 'Applications', 'Game.app', 'Contents')
        self.assertEqual(open(os.path.join(app, 'Resources/Data/level0'), 'rb').read(), big)
        self.assertEqual(open(os.path.join(app, 'Resources/goggame-2.info'), 'rb').read(), b'dlc')
        self.assertTrue(os.path.exists(os.path.join(app, 'Resources/Data/StreamingAssets/dlc_bundle')))
        self.assertEqual(sorted(os.listdir(root)), ['Applications'])            # no staging left behind
        self.assertEqual(os.listdir(os.path.join(root, 'Applications')), ['Game.app'])


if __name__ == '__main__':
    unittest.main()
