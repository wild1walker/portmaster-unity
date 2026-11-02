"""Converter plugins without code (edits.py): the plugin file's checks, field paths and kinds, the
data files a glob sees. (The whole step on made-up and real data, against pmu's:
setup-rs/tests/golden/convert_madeup.sh, the edits cases.)"""
import json
import os
import tempfile
import unittest

from pmunity import edits as E


class EditsTest(unittest.TestCase):
    def load(self, cfg):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'p.json')
            with open(p, 'w') as f:
                f.write(cfg if isinstance(cfg, str) else json.dumps(cfg))
            try:
                return E.load(p)
            except SystemExit as ex:
                return str(ex).split(': ', 1)[1]

    def test_checks(self):
        self.assertEqual(self.load([]), 'not a JSON object')
        self.assertEqual(self.load({'x': 1}), "unknown key 'x' (about, steps)")
        self.assertEqual(self.load({'steps': {}}), '"steps" must be an object of named steps')
        self.assertEqual(self.load({'steps': {'a': [{'file': 'g', 'class': 'MonoBehaviour', 'set': {'x': 1}}]}}),
                         "step a, edit 0: MonoBehaviour fields need the game's assemblies (a Python plugin)")
        self.assertEqual(self.load({'steps': {'a': [{'file': 'g', 'class': 'C', 'set': {'x': [1]}}]}}),
                         'step a, edit 0: x: a value must be a bool, a number or a string')
        self.assertEqual(self.load('{"steps": {"x": ['), 'Expecting value: line 1 column 18 (char 17)')
        steps = self.load({'about': 'x', 'steps': {'a': [{'file': '*', 'class': 'Light', 'set': {'m_Intensity': 1}}]}})
        self.assertEqual(list(steps), ['a'])

    def test_set(self):
        t = {'b': False, 'i': 3, 'f': 1.0, 's': 'x', 'l': [('k', 0.5)], 'st': {'a': 1}}
        t = E._set(t, 'b', True)
        t = E._set(t, 'f', 2)
        t = E._set(t, 'l.0.1', 0.25)
        self.assertEqual((t['b'], t['f'], t['l']), (True, 2.0, [('k', 0.25)]))
        for path, v, why in (('i', 1.5, 'an integer, not a number'), ('s', True, 'a string, not a bool'),
                             ('st', 1, 'not a single value (a struct, list or byte array)'),
                             ('l.1', 1, '1: not an index of 1 items'), ('nope', 1, 'no field nope'),
                             ('i.x', 1, 'x: i is not a struct or a list')):
            with self.assertRaises(E._Bad) as cm:
                E._set(t, path, v)
            self.assertEqual(str(cm.exception), why)

    def test_data_files(self):
        with tempfile.TemporaryDirectory() as t:
            for rel in ('globalgamemanagers', 'a/b.bundle', '.pmunity.json', '.pmunity-work/x', 'level0'):
                os.makedirs(os.path.dirname(os.path.join(t, rel)) or t, exist_ok=True)
                open(os.path.join(t, rel), 'w').close()
            self.assertEqual(E.data_files(t), ['a/b.bundle', 'globalgamemanagers', 'level0'])


if __name__ == '__main__':
    unittest.main()
