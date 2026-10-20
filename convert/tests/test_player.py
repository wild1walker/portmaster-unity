"""pmunity player: installer lookup and the package layout of the installer's paths (offline)."""
import unittest

from pmunity import player as P


class PlayerTest(unittest.TestCase):
    def test_package_path(self):
        self.assertEqual(P.package_path('$_OUTDIR/Variations/mono/Release/Libs/armeabi-v7a/libunity.so'),
                         'Variations/mono/Release/Libs/armeabi-v7a/libunity.so')
        self.assertEqual(P.package_path('$INSTDIR$_59_/Data/unity default resources'), 'Data/unity default resources')
        self.assertIsNone(P.package_path('Data/x'))

    def test_installer_url(self):
        rec = {'version': '2019.4.11f1', 'shortRevision': '2d9804dddde7', 'downloads': [
            {'platform': 'MAC_OS', 'modules': [{'id': 'android', 'url': 'https://x/MacEditorTargetInstaller/a.pkg'}]},
            {'platform': 'WINDOWS', 'modules': [{'id': 'ios', 'url': 'https://x/i.exe'},
                                                {'id': 'android', 'url': 'https://x/TargetSupportInstaller/a.exe'}]}]}
        self.assertEqual(P.installer_url(rec), 'https://x/TargetSupportInstaller/a.exe')
        rec['downloads'] = []
        self.assertEqual(P.installer_url(rec), 'https://download.unity3d.com/download_unity/2d9804dddde7/'
                         'TargetSupportInstaller/UnitySetup-Android-Support-for-Editor-2019.4.11f1.exe')

    def test_wanted(self):
        g = P.wanted('mono', 'armeabi-v7a')
        self.assertIn('Variations/mono/Release/MonoLibs/armeabi-v7a/*.so', g)
        self.assertNotIn('Variations/mono/Managed/*.dll', P.wanted('il2cpp', 'arm64-v8a'))


if __name__ == '__main__':
    unittest.main()
