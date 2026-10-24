"""The audio step: which DecompressOnLoad clips are kept compressed in memory (plan), and the step
run over a data folder of stand-in files (the clips and PreloadData a real file would give)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from pmunity import audio as A  # noqa: E402
from pmunity.job import Job  # noqa: E402

M = 2 ** 20


def clip(seconds, load=A.DECOMPRESS_ON_LOAD, fmt=A.VORBIS, rate=44100, channels=2, name='c'):
    return A.Clip(name, load, fmt, seconds, rate, channels)


def secs(mib, rate=44100, channels=2):
    """The length of a clip whose PCM is <mib> MiB."""
    return mib * M / (rate * channels * 2)


class PcmTest(unittest.TestCase):
    def test_bytes(self):
        self.assertEqual(clip(1.0).pcm, 44100 * 4)
        self.assertEqual(clip(2.5, rate=48000, channels=1).pcm, 120000 * 2)

    def test_scene_of(self):
        self.assertEqual(A.scene_of('sharedassets12.assets'), 12)
        self.assertIsNone(A.scene_of('resources.assets'))
        self.assertIsNone(A.scene_of('sharedassets.assets'))


class PlanTest(unittest.TestCase):
    def test_length_rule(self):
        clips = {'long': clip(2.0), 'short': clip(1.9), 'pcm': clip(9, fmt=0),
                 'cim': clip(9, load=A.COMPRESSED_IN_MEMORY)}
        sw, left = A.plan(clips, {0: set(clips)}, None, 2.0)
        self.assertEqual(sw, {'long'})
        self.assertEqual(left, {})

    def test_within_budget_kept(self):
        clips = {'a': clip(secs(3)), 'b': clip(secs(4))}
        sw, left = A.plan(clips, {0: {'a'}, 1: {'b'}, 2: {'a', 'b'}}, 8 * M, 2.0)
        self.assertEqual(sw, set())                 # the game's own: decoded once at load
        self.assertAlmostEqual(left[2] / M, 7, places=3)

    def test_largest_first(self):
        clips = {'song': clip(secs(10)), 'loop': clip(secs(2)), 'loop2': clip(secs(3)), 'sfx': clip(1.0)}
        sw, left = A.plan(clips, {5: set(clips)}, 6 * M, 2.0, resident=0)
        self.assertEqual(sw, {'song'})
        sw, _ = A.plan(clips, {5: set(clips)}, 4 * M, 2.0, resident=0)
        self.assertEqual(sw, {'song', 'loop2'})

    def test_other_formats_count_but_stay(self):
        clips = {'pcm': clip(secs(5), fmt=0), 'adpcm': clip(secs(3), fmt=2), 'v': clip(secs(1))}
        sw, left = A.plan(clips, {3: set(clips)}, 6 * M, 2.0, resident=0)
        self.assertEqual(sw, {'v'})                 # only Vorbis is switched; the rest still holds 8 MiB
        self.assertAlmostEqual(left[3] / M, 8, places=3)

    def test_game_choices_untouched(self):
        clips = {'cim': clip(secs(9), load=A.COMPRESSED_IN_MEMORY), 'stream': clip(secs(9), load=2), 'v': clip(secs(1))}
        sw, left = A.plan(clips, {3: set(clips)}, 2 * M, 2.0, resident=0)
        self.assertEqual(sw, set())
        self.assertAlmostEqual(left[3] / M, 1, places=3)

    def test_resident_scenes_count_everywhere(self):
        clips = {'atmos': clip(secs(3)), 'menu': clip(secs(1)), 'own': clip(secs(2))}
        scenes = {0: set(), 1: {'atmos', 'menu'}, 7: {'own'}}
        sw, left = A.plan(clips, scenes, 5 * M, 2.0)
        self.assertEqual(sw, {'own'})               # scene 7 holds atmos + menu + own = 6 MiB: its own goes first
        self.assertAlmostEqual(left[7] / M, 4, places=3)
        sw, _ = A.plan(clips, scenes, 5 * M, 2.0, resident=0)
        self.assertEqual(sw, set())
        sw, left = A.plan(clips, scenes, 3 * M, 2.0)
        self.assertEqual(sw, {'atmos'})             # the title scene itself over 3: its largest; then 7 fits
        self.assertAlmostEqual(left[1] / M, 1, places=3)

    def test_resident_clips_after_the_scenes_own(self):
        clips = {'atmos': clip(secs(4)), 'boss_song': clip(secs(3)), 'boss_loop': clip(secs(2))}
        scenes = {1: {'atmos'}, 9: {'boss_song', 'boss_loop'}}
        sw, _ = A.plan(clips, scenes, 6 * M, 2.0)
        self.assertEqual(sw, {'boss_song'})         # (the largest of scene 9's set is atmos)
        sw, left = A.plan(clips, scenes, int(4.5 * M), 2.0)
        self.assertEqual(sw, {'boss_song', 'boss_loop'})
        self.assertAlmostEqual(left[9] / M, 4, places=3)
        sw, _ = A.plan(clips, scenes, int(3.5 * M), 2.0)
        self.assertEqual(sw, {'atmos', 'boss_song'})

    def test_short_clips_never_switched(self):
        clips = {'s%d' % i: clip(1.5) for i in range(20)}
        clips['long'] = clip(2.0)
        sw, left = A.plan(clips, {4: set(clips)}, 1 * M, 2.0, resident=0)
        self.assertEqual(sw, {'long'})              # the length rule's clips only: never more than it
        self.assertGreater(left[4], 1 * M)

    def test_unused_clips_keep_the_length_rule(self):
        clips = {'script': clip(5.0), 'blip': clip(1.0), 'scene': clip(5.0)}
        sw, _ = A.plan(clips, {2: {'scene'}}, 64 * M, 2.0)
        self.assertEqual(sw, {'script'})

    def test_every_scene_fits(self):
        import random
        r = random.Random(4)
        clips = {i: clip(r.uniform(0.1, 120), fmt=r.choice((A.VORBIS, A.VORBIS, A.VORBIS, 0))) for i in range(300)}
        scenes = {n: set(r.sample(range(300), 40)) for n in range(30)}
        budget = 12 * M
        sw, left = A.plan(clips, scenes, budget, 2.0)
        base = scenes[0] | scenes[1]
        for n, s in scenes.items():
            held = sum(clips[k].pcm for k in s | base if clips[k].load == A.DECOMPRESS_ON_LOAD and k not in sw)
            self.assertEqual(held, left[n])
            long_left = [k for k in s | base if k not in sw and clips[k].fmt == A.VORBIS and clips[k].length >= 2.0]
            self.assertTrue(held <= budget or not long_left)
        old, _ = A.plan(clips, scenes, None, 2.0)
        self.assertLessEqual(sw, old)


class _FakeObj:
    def __init__(self, pid, kind, tree):
        self.pid, self.kind, self.tree = pid, kind, tree
        self.type = type('T', (), {'name': kind})

    def read_typetree(self):
        return json.loads(json.dumps(self.tree))

    def save_typetree(self, t):
        self.tree = t


class _FakeLoaded:
    """A stand-in for unity_io.Loaded over a JSON file: {"externals": [...], "objects": {pid: [type, tree]}}."""
    def __init__(self, path):
        self.path = path
        with open(path) as f:
            d = json.load(f)
        self.sf = type('SF', (), {'externals': [type('E', (), {'path': p}) for p in d['externals']]})
        self.objs = {int(p): _FakeObj(int(p), k, t) for p, (k, t) in d['objects'].items()}
        self.ext = d['externals']

    def objects(self, type_name=None):
        for pid, o in self.objs.items():
            if type_name is None or o.kind == type_name:
                yield pid, o

    def save(self):
        return json.dumps({'externals': self.ext,
                           'objects': {str(p): [o.kind, o.tree] for p, o in self.objs.items()}}).encode()

    def close(self):
        pass


def _ac(name, seconds, load=A.DECOMPRESS_ON_LOAD, fmt=A.VORBIS):
    return ['AudioClip', {'m_Name': name, 'm_LoadType': load, 'm_CompressionFormat': fmt, 'm_Length': seconds,
                          'm_Frequency': 44100, 'm_Channels': 2}]


class StepTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.data = os.path.join(self.d, 'Data')
        os.makedirs(self.data)
        self.files = {
            # resources.assets: an atmosphere loop the title scene uses, a clip only scripts load
            'resources.assets': {'externals': [], 'objects': {
                '10': _ac('atmos', secs(6)), '11': _ac('scripted', 3.0), '12': _ac('own_cim', 30.0, load=1)}},
            'sharedassets0.assets': {'externals': [], 'objects': {'1': ['PreloadData', {'m_Assets': []}]}},
            'sharedassets1.assets': {'externals': ['resources.assets'], 'objects': {
                '1': ['PreloadData', {'m_Assets': [{'m_FileID': 1, 'm_PathID': 10}]}],
                '2': _ac('title_song', secs(10))}},
            'sharedassets2.assets': {'externals': ['library/unity default resources', 'resources.assets'], 'objects': {
                '1': ['PreloadData', {'m_Assets': [{'m_FileID': 2, 'm_PathID': 10}, {'m_FileID': 1, 'm_PathID': 5}]}],
                '2': _ac('boss_song', secs(12)), '3': _ac('loop', secs(2)), '4': _ac('hit', 0.3)}},
        }
        for n, d in self.files.items():
            with open(os.path.join(self.data, n), 'w') as f:
                json.dump(d, f)
        self._loaded, self._collect = A.Loaded, A.collect
        A.Loaded, A.collect = _FakeLoaded, lambda: None

    def tearDown(self):
        A.Loaded, A.collect = self._loaded, self._collect
        shutil.rmtree(self.d)

    def load_types(self, out, rel):
        p = os.path.join(out, rel)
        with open(p if os.path.exists(p) else os.path.join(self.data, rel)) as f:
            d = json.load(f)
        return {t['m_Name']: t['m_LoadType'] for k, t in d['objects'].values() if k == 'AudioClip'}

    def run_step(self, cfg, out=None):
        logs = []
        job = Job(self.data, out or self.data, {'version': 1, 'audio': cfg}, log=logs.append)
        A.run(job)
        return job, logs

    def test_budget(self):
        out = os.path.join(self.d, 'out')
        job, logs = self.run_step({'pcm_budget_mb': 20}, out)
        # scene 2 holds atmos 6 + title_song 10 (the resident title scene) + boss 12 + loop 2 + hit:
        # 30 MiB, over 20, so the largest goes: boss_song (18 MiB left)
        self.assertEqual(self.load_types(out, 'sharedassets2.assets'), {'boss_song': 1, 'loop': 0, 'hit': 0})
        self.assertEqual(self.load_types(out, 'sharedassets1.assets'), {'title_song': 0})
        self.assertFalse(os.path.exists(os.path.join(out, 'sharedassets1.assets')))      # nothing to change
        self.assertEqual(self.load_types(out, 'resources.assets'), {'atmos': 0, 'scripted': 1, 'own_cim': 1})
        self.assertIn('2 clips kept compressed in memory', logs[-1])
        self.assertIn('budget 20 MiB a scene, the most a scene holds 18.1 MiB (scene 2)', logs[-1])
        _, logs = self.run_step({'pcm_budget_mb': 12}, os.path.join(self.d, 'out2'))
        self.assertEqual(self.load_types(os.path.join(self.d, 'out2'), 'sharedassets1.assets'), {'title_song': 1})

    def test_length_rule_without_budget(self):
        out = os.path.join(self.d, 'out')
        self.run_step({'pcm_budget_mb': None}, out)
        self.assertEqual(self.load_types(out, 'sharedassets2.assets'), {'boss_song': 1, 'loop': 1, 'hit': 0})
        self.assertEqual(self.load_types(out, 'resources.assets'), {'atmos': 1, 'scripted': 1, 'own_cim': 1})

    def test_in_place_resume_keeps_the_plan(self):
        job, _ = self.run_step({'pcm_budget_mb': 20})
        with open(os.path.join(self.data, '.pmunity.json')) as f:
            st = json.load(f)
        self.assertEqual(st['audio']['switch'], {'resources.assets': [11], 'sharedassets2.assets': [2]})
        # a setup stopped after the plan and one file: the next run switches the same clips, and
        # does not plan again from files that are already half converted
        st['done']['audio'] = ['resources.assets']
        with open(os.path.join(self.data, '.pmunity.json'), 'w') as f:
            json.dump(st, f)
        with open(os.path.join(self.data, 'sharedassets2.assets'), 'w') as f:
            json.dump(self.files['sharedassets2.assets'], f)
        _, logs = self.run_step({'pcm_budget_mb': 1000})
        self.assertIn("chosen with {'pcm_budget_mb': 20, 'compressed_min_s': 2.0, 'resident_scenes': 2}", logs[0])
        self.assertEqual(self.load_types(self.data, 'sharedassets2.assets'), {'boss_song': 1, 'loop': 0, 'hit': 0})


if __name__ == '__main__':
    unittest.main()
