"""Job: files put in place together, and a job killed between two renames finishing them when it
starts again (a serialized file and its .resS are never half old, half new)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from pmunity import job as J  # noqa: E402


def _write(p, data):
    with open(p, 'wb') as f:
        f.write(data)


def _read(p):
    with open(p, 'rb') as f:
        return f.read()


class CommitTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.data = os.path.join(self.d, 'Data')
        os.makedirs(self.data)
        for n in ('a.assets', 'a.assets.resS'):
            _write(os.path.join(self.data, n), b'old ' + n.encode())

    def tearDown(self):
        shutil.rmtree(self.d)

    def test_pair_in_place(self):
        j = J.Job(self.data, self.data, {'version': 1})
        w = j.work_dir('t')
        self.assertTrue(w.startswith(os.path.join(self.data, '.pmunity-work')))
        _write(os.path.join(w, 's'), b'new stream')
        _write(os.path.join(w, 'f'), b'new file')
        j.commit([('a.assets.resS', os.path.join(w, 's')), ('a.assets', os.path.join(w, 'f'))])
        self.assertEqual(_read(os.path.join(self.data, 'a.assets.resS')), b'new stream')
        self.assertEqual(_read(os.path.join(self.data, 'a.assets')), b'new file')
        with open(os.path.join(self.data, J.STATE)) as f:
            self.assertNotIn('pending', json.load(f))

    def test_killed_between_the_renames(self):
        j = J.Job(self.data, self.data, {'version': 1})
        w = j.work_dir('t')
        _write(os.path.join(w, 's'), b'new stream')
        _write(os.path.join(w, 'f'), b'new file')
        moves = [[os.path.join(w, 's'), os.path.join(self.data, 'a.assets.resS')],
                 [os.path.join(w, 'f'), os.path.join(self.data, 'a.assets')]]
        j.state['pending'] = moves                    # journal written, then killed after one rename
        j.save_state()
        os.replace(*moves[0])
        J.Job(self.data, self.data, {'version': 1})   # the next start finishes the pair
        self.assertEqual(_read(os.path.join(self.data, 'a.assets.resS')), b'new stream')
        self.assertEqual(_read(os.path.join(self.data, 'a.assets')), b'new file')
        with open(os.path.join(self.data, J.STATE)) as f:
            self.assertNotIn('pending', json.load(f))


class WriteOnceTest(unittest.TestCase):
    """write_once: a file changed and marked together; a job killed on the way ends with the file changed once and
    marked (its change is never made a second time)."""

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.data = os.path.join(self.d, 'Data')
        os.makedirs(self.data)
        self.f = os.path.join(self.data, 'a.assets')
        _write(self.f, b'old')

    def tearDown(self):
        shutil.rmtree(self.d)

    def _journal(self):
        with open(os.path.join(self.data, J.STATE)) as f:
            return json.load(f)

    def test_written_and_marked(self):
        j = J.Job(self.data, self.data, {'version': 1})
        j.write_once('refs', 'a.assets', b'new')
        self.assertEqual(_read(self.f), b'new')
        self.assertFalse(os.path.exists(self.f + '.tmp'))
        self.assertTrue(j.is_done('refs', 'a.assets'))
        st = self._journal()
        self.assertNotIn('once', st)
        self.assertEqual(st['done'], {'refs': ['a.assets']})

    def _killed(self, renamed):
        j = J.Job(self.data, self.data, {'version': 1})
        _write(self.f + '.tmp', b'new')
        j.state['once'] = [self.f + '.tmp', self.f, 'refs', 'a.assets']     # journal written, then killed
        j.save_state()
        if renamed:
            os.replace(self.f + '.tmp', self.f)
        j2 = J.Job(self.data, self.data, {'version': 1})                    # the next start finishes it
        self.assertEqual(_read(self.f), b'new')
        self.assertFalse(os.path.exists(self.f + '.tmp'))
        self.assertTrue(j2.is_done('refs', 'a.assets'))
        st = self._journal()
        self.assertNotIn('once', st)
        self.assertEqual(st['done'], {'refs': ['a.assets']})

    def test_killed_before_the_rename(self):
        self._killed(False)

    def test_killed_after_the_rename(self):
        self._killed(True)

    def test_killed_before_the_journal(self):
        j = J.Job(self.data, self.data, {'version': 1})
        j.save_state()
        _write(self.f + '.tmp', b'half')                                    # killed while writing the new file
        j2 = J.Job(self.data, self.data, {'version': 1})
        self.assertEqual(_read(self.f), b'old')                             # as it was, not marked: done again
        self.assertFalse(j2.is_done('refs', 'a.assets'))


if __name__ == '__main__':
    unittest.main()
