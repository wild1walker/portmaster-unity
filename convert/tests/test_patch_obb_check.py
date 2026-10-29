"""patch-obb check: an add-on's bundles copied only in part (a download or copy that stopped early),
on made-up bundles (no Unity or game files): python -m unittest tests.test_patch_obb_check"""
import json
import os
import shutil
import struct
import tempfile
import unittest
import zipfile

from pmunity import patch_obb as PO


def bundle(size):
    head = b'UnityFS\0' + struct.pack('>I', 6) + b'5.x.x\0' + b'2017.4.9f1\0'
    head += struct.pack('>qIII', size, 0, 0, 0)
    return head + b'\0' * (size - len(head))


class CheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.names = ['dlc_a', 'dlc_b', 'dlc_c']

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def folder(self, name, files):
        d = os.path.join(self.tmp, name)
        os.makedirs(d)
        for n, data in files.items():
            with open(os.path.join(d, n), 'wb') as f:
                f.write(data)
        return d

    def test_complete(self):
        d = self.folder('b', {n: bundle(200) for n in self.names + ['game_x']})
        self.assertEqual(PO.incomplete(self.names, [d]), ([], []))

    def test_no_addon_there(self):                       # the game alone: nothing to say
        d = self.folder('b', {'game_x': bundle(200), 'notes.txt': b'x'})
        self.assertEqual(PO.incomplete(self.names, [d]), ([], []))

    def test_some_missing_and_cut_short(self):
        d = self.folder('b', {'dlc_a': bundle(200), 'dlc_b': bundle(300)[:150], '.DS_Store': b''})
        self.assertEqual(PO.incomplete(self.names, [d]), (['dlc_c'], [('dlc_b', 150, 300)]))

    def test_empty_and_header_only(self):
        d = self.folder('b', {'dlc_a': b'', 'dlc_b': bundle(300)[:20], 'dlc_c': bundle(200)})
        missing, cut = PO.incomplete(self.names, [d])
        self.assertEqual(missing, [])
        self.assertEqual([c[0] for c in cut], ['dlc_a', 'dlc_b'])

    def test_installed_game_counts(self):
        game = self.folder('game', {})
        with open(os.path.join(game, 'android_package.json'), 'w') as f:
            json.dump({'apk': 'g.apk', 'package': 'com.x', 'version_code': 1}, f)
        with zipfile.ZipFile(os.path.join(game, 'g.apk'), 'w') as z:
            z.writestr(PO.PREFIX + 'dlc_c', b'')
        d = self.folder('b', {'dlc_a': bundle(200), 'dlc_b': bundle(200)})
        self.assertEqual(PO.incomplete(self.names, [d], game), ([], []))

    def test_exit_code(self):
        d = self.folder('b', {'dlc_a': bundle(200)})
        names = os.path.join(self.tmp, 'names.txt')
        with open(names, 'w') as f:
            f.write('\n'.join(self.names) + '\n')
        with self.assertRaises(SystemExit) as e:
            PO.main(['check', names, d])
        self.assertEqual(e.exception.code, 3)
        PO.main(['check', names, self.folder('c', {n: bundle(200) for n in self.names})])


if __name__ == '__main__':
    unittest.main()
