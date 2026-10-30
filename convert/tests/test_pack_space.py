"""The pack step's free-space check (no Unity or game files): python -m unittest tests.test_pack_space"""
import os
import shutil
import tempfile
import unittest
from unittest import mock

from pmunity import android_data as AD


class SpaceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.src = os.path.join(self.tmp, 'a')
        with open(self.src, 'wb') as f:
            f.write(b'x' * 1000)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def free(self, n):
        return mock.patch.object(AD.os, 'statvfs', return_value=mock.Mock(f_bavail=n, f_frsize=1))

    def test_enough(self):
        with self.free((1 << 30) + 2000):
            AD.check_space(self.tmp, [('a', self.src), ('b', b'y' * 1000)], [], print)

    def test_too_full(self):
        said = []
        with self.free(1 << 30), self.assertRaises(SystemExit) as e:
            AD.check_space(self.tmp, [('a', self.src)], [], said.append)
        self.assertEqual(e.exception.code, AD.NO_SPACE)
        self.assertIn('free 0.0 GiB more', said[0])

    def test_archive_written_again_counts_as_free(self):
        old = os.path.join(self.tmp, 'Game.apk')
        with open(old, 'wb') as f:
            f.write(b'z' * 1000)
        with self.free(1 << 30):
            AD.check_space(self.tmp, [('a', self.src)], [old, os.path.join(self.tmp, 'none.obb')], print)


if __name__ == '__main__':
    unittest.main()
