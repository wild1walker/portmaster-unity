"""Tests of tools/regress on synthetic input only (no game, no Unity file):

    python3 -m unittest discover -s tools/regress
"""
import json
import os
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import checks as K      # noqa: E402
import digest as D      # noqa: E402
import regress as R     # noqa: E402


def read(p):
    with open(p) as f:
        return f.read()


def tmpdir(test):
    import shutil
    d = tempfile.mkdtemp()
    test.addCleanup(shutil.rmtree, d, True)
    return d


def ppm(path, w, h, pixel):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(bytes(b for y in range(h) for x in range(w) for b in pixel(x, y)))


class ConverterLog(unittest.TestCase):
    LOG = [
        "[    1.3s] shaders: Resources/unity_builtin_extra: Hidden/X: skipped program 0: VERTEX: built-in variable 'gl_Layer'",
        '[    1.3s] shaders: 36 shaders given GLES3 programs (183 programs, 382 subprograms) in 2 files; 1 not translatable; '
        'precision highp; rewrites: 1 stages (const-arrays)',
        '[    1.3s] shaders: 14 programs need GLSL ES 3.10 (multisampled textures, ...): GLES 3.1 programs, used where ...',
        '[    1.3s] shaders: GLCore41/43 programs translated too: 16 programs; 0 SubShaders left out (no ES equivalent); '
        '2 shaders without programs (stripped) given an empty GLES3 blob',
        '[    1.3s] shaders: 3 VR-only OpenGL Core variants not translatable (left out, never used on a handheld)',
        '[    1.3s] shaders: 9 of 11 compute shaders given GLES3 kernels (40 kernels, 52 kernel variants, GLSL ES 3.10: used '
        'where the device has OpenGL ES 3.1+); 4 kernels left out; 1 without OpenGL Core kernels',
        '[    1.3s] shaders: from Direct3D 11: 49 of 51 shaders (300 programs); 2 SubShaders left out; 1 VR-only variants, '
        '3 GPU instancing and 4 GPU sprite skinning variants left out',
        '[    1.3s] shaders: from Metal: 92 of 93 shaders (1250 programs); 1 SubShaders left out; 0 VR-only and 5 GPU '
        'instancing variants left out',
    ]

    def test_counts(self):
        c = K.shader_counts(self.LOG)
        self.assertEqual((c['shaders'], c['programs'], c['subprograms'], c['files'], c['not_translatable']), (36, 183, 382, 2, 1))
        self.assertEqual(c['precision'], 'highp')
        self.assertEqual(c['es31_programs'], 14)
        self.assertEqual((c['gl4x_programs'], c['gl4x_subshaders_left_out'], c['gl4x_empty']), (16, 0, 2))
        self.assertEqual(c['vr_left_out'], 3)
        self.assertEqual((c['compute'], c['compute_total'], c['kernels'], c['kernels_left_out']), (9, 11, 40, 4))
        self.assertEqual(c['d3d11'], {'converted': 49, 'total': 51, 'programs': 300, 'subshaders_left_out': 2, 'vr': 1,
                                      'instancing': 3, 'skinning': 4})
        self.assertEqual(c['metal'], {'converted': 92, 'total': 93, 'programs': 1250, 'subshaders_left_out': 1, 'vr': 0,
                                      'instancing': 5})
        self.assertFalse(any('Hidden' in str(v) for v in c.values()))

    def test_glcore4x_off(self):
        self.assertEqual(K.shader_counts(['shaders: GLCore41/43 programs not translated (glcore4x off)']), {'gl4x': 'off'})


class Verdicts(unittest.TestCase):
    def test_check_keeps_no_names(self):
        rep = {'route': 'android', 'routes': [{'route': 'android', 'ok': True}, {'route': 'box64', 'ok': False}],
               'build': 'linux', 'product': 'A Game', 'data_survey': {'product': 'A Game', 'scripts_used': {'Foo': 1}},
               'suggest': {'runtime': {'UA_POINTER': '1'}}, 'warnings': ['w'], 'notes': ['a', 'b'],
               'path': '/home/someone/game'}
        v = K.check_verdict(rep)
        self.assertEqual(v['routes_ok'], ['android'])
        self.assertEqual(v['suggest']['runtime'], {'UA_POINTER': '1'})
        self.assertEqual((v['warnings'], v['notes']), (1, 2))
        self.assertNotIn('A Game', json.dumps(v))
        self.assertNotIn('/home', json.dumps(v))

    def test_route(self):
        v = K.route_verdict({'unity': '2020.3.1f1', 'android': {'fits': False, 'blockers': ['x']}, 'box64': {'fits': True},
                             'data': '/x/y', 'name': 'game'})
        self.assertEqual(v['android'], {'fits': False, 'blockers': 1, 'caveats': 0})
        self.assertTrue(v['box64']['fits'])
        self.assertNotIn('/x/y', json.dumps(v))


class RunLog(unittest.TestCase):
    def test_counts(self):
        lines = ['[fps] 3.0 fps, frame 334.9 avg / 3257.9 p99 / 3257.9 worst ms, late 12 hitch 5, gc 0 heap 64 MB '
                 '(15 frames, 16 total, 5 s) rss 383 MB',
                 '[frames] screenshot /emu/shot-1.ppm',
                 '[fps] 5.7 fps, frame 176.7 avg / 8821.4 p99 / 8821.4 worst ms, late 74 hitch 48, gc 0 heap 64 MB '
                 '(74 frames, 90 total, 18 s) rss 532 MB',
                 'NullReferenceException: Object reference not set',
                 '[fj 54] up: UnityMain thread exits after 98 frames', 'exit 124']
        r = K.run_log(lines)
        self.assertEqual((r['frames'], r['fps_lines'], r['shots_logged'], r['exceptions'], r['exit'], r['crash']),
                         (98, 2, 1, 1, 124, None))

    def test_crash(self):
        r = K.run_log(['[crash] signal 11 (code 1) at address 0x0, thread 12', 'exit 139'])
        self.assertEqual((r['crash'], r['exit']), (11, 139))
        c = dict((k, ok) for k, ok, _ in K.expectations_rig({}, r, [], ''))
        self.assertFalse(c['no_crash'])
        self.assertFalse(c['exit'])


class Pictures(unittest.TestCase):
    def setUp(self):
        self.d = tmpdir(self)

    def stats(self, pixel):
        p = os.path.join(self.d, 'a.ppm')
        ppm(p, 64, 48, pixel)
        return K.picture_stats(*K.read_ppm(p))

    def test_black(self):
        s = self.stats(lambda x, y: (0, 0, 0))
        self.assertFalse(s['content'])
        self.assertEqual(s['dark'], 1.0)

    def test_one_colour(self):
        s = self.stats(lambda x, y: (200, 0, 200))
        self.assertFalse(s['content'])
        self.assertEqual(s['colours'], 1)

    def test_picture(self):
        s = self.stats(lambda x, y: (x * 4, y * 5, 128))
        self.assertTrue(s['content'])
        self.assertGreater(s['colours'], 20)

    def test_mostly_black_with_text(self):
        s = self.stats(lambda x, y: (255, 255, 255) if 20 <= x < 44 and 20 <= y < 28 else (0, 0, 0))
        self.assertTrue(s['content'])


class Compare(unittest.TestCase):
    def test_exact(self):
        g = {'shaders': {'shaders': 36, 'programs': 183}, 'shaders_digest': {'total': 'a', 'per_file': {'x': '1', 'y': '2'}}}
        v = {'shaders': {'shaders': 36, 'programs': 184}, 'shaders_digest': {'total': 'b', 'per_file': {'x': '1', 'y': '3'}}}
        d = {k: (a, b) for k, a, b in K.compare_exact(g, v)}
        self.assertEqual(set(d), {'shaders.programs', 'shaders_digest.total', 'shaders_digest.per_file'})
        self.assertEqual(K.changed_files(*d['shaders_digest.per_file'])[:3], (1, 0, 0))
        self.assertEqual(K.compare_exact(g, g), [])

    def test_rig(self):
        golden = {'run': {'frames': 1000, 'crash': None}, 'shots': [{'t': 60, 'content': True}, {'t': 120, 'content': False}]}
        ok = K.compare_rig(golden, {'frames': 900, 'crash': None}, [{'t': 60, 'content': True}, {'t': 120, 'content': False}])
        self.assertTrue(all(c[1] for c in ok))
        bad = K.compare_rig(golden, {'frames': 100, 'crash': 11}, [{'t': 60, 'content': False}])
        self.assertEqual(sorted(c[0] for c in bad if not c[1]), ['baseline:content@60', 'baseline:crash', 'baseline:frames'])

    def test_rig_expectations(self):
        shots = [{'t': 220, 'content': True, 'dark': 0.1, 'colours': 50}]
        c = {k: ok for k, ok, _ in K.expectations_rig({'content': [220, 300], 'frames_min': 10, 'log': ['scene_title']},
                                                      {'frames': 20, 'exit': 124}, shots, 'x scene_title y')}
        self.assertEqual(c, {'no_crash': True, 'exit': True, 'frames_min': True, 'content@220': True, 'content@300': False,
                             'log:scene_title': True})


class Names(unittest.TestCase):
    def test_hide(self):
        self.assertEqual(R.hide_name('level12'), 'level12')
        self.assertEqual(R.hide_name('Resources/unity_builtin_extra'), 'Resources/unity_builtin_extra')
        self.assertEqual(R.hide_name('sharedassets3.assets'), 'sharedassets3.assets')
        h = R.hide_name('StreamingAssets/aa/StandaloneLinux64/mygame_assets_all.bundle')
        self.assertTrue(h.startswith('f-'))
        self.assertNotIn('mygame', h)


class Files(unittest.TestCase):
    def test_view(self):
        d = tmpdir(self)
        src, ov, dst = (os.path.join(d, n) for n in ('src', 'ov', 'view'))
        for p, t in ((src, 'a'), (os.path.join(src, 'sub'), 'b'), (ov, 'a'), (os.path.join(ov, 'sub'), 'c')):
            os.makedirs(p, exist_ok=True)
            with open(os.path.join(p, t), 'w') as f:
                f.write(p)
        with open(os.path.join(src, 'data.unity3d'), 'w') as f:
            f.write('archive')
        R.view_dir(src, [ov], dst)
        self.assertEqual(read(os.path.join(dst, 'a')), ov)
        self.assertEqual(read(os.path.join(dst, 'sub', 'b')), os.path.join(src, 'sub'))
        self.assertEqual(read(os.path.join(dst, 'sub', 'c')), os.path.join(ov, 'sub'))
        self.assertFalse(os.path.exists(os.path.join(dst, 'data.unity3d')))
        self.assertTrue(os.path.islink(os.path.join(dst, 'a')) and os.path.isdir(os.path.join(dst, 'sub'))
                        and not os.path.islink(os.path.join(dst, 'sub')))

    def test_tree_and_zip(self):
        d = tmpdir(self)
        root = os.path.join(d, 'out')
        os.makedirs(os.path.join(root, 'zip'))
        with zipfile.ZipFile(os.path.join(root, 'zip', 'Game.apk'), 'w') as z:
            z.writestr('assets/bin/Data/level0', b'x' * 100)
        with open(os.path.join(root, 'zip', 'android_package.json'), 'w') as f:
            f.write('{}')
        with open(os.path.join(root, 'make_android_data.log'), 'w') as f:
            f.write('a path')
        j = os.path.join(d, 't.json')
        D.cmd_tree(root, j, ['*.log'])
        t = json.loads(read(j))
        self.assertEqual(sorted(t['files']), ['zip/Game.apk', 'zip/android_package.json'])
        self.assertEqual(t['zips']['zip/Game.apk']['entries'], 1)
        D.cmd_tree(root, j + '2', ['*.log'])
        self.assertEqual(json.loads(read(j + '2'))['total'], t['total'])


class List(unittest.TestCase):
    def test_games_toml(self):
        defaults, games = R.load_list(os.path.join(HERE, 'games.toml'))
        ids = [g['id'] for g in games]
        self.assertEqual(len(ids), len(set(ids)))
        for g in games:
            self.assertIn(g['route'], R.ROUTES)
            self.assertTrue(set(g['levels']) <= set(R.LEVELS), g['id'])
            if 'rig' in g['levels'] and g['route'] != 'box64':
                r = g.get('rig') or {}
                self.assertTrue(set(r.get('content', ())) <= set(r.get('shots', ())), g['id'])
            self.assertNotIn('/', g['slug'])
        self.assertGreaterEqual(sum('rig' in g['levels'] for g in games), 8)

    def test_baselines_hold_no_paths(self):
        for level in R.LEVELS:
            p = R.baseline_path(level)
            if os.path.isfile(p):
                text = read(p)
                self.assertNotRegex(text, r'/(Users|home|Volumes)/')


if __name__ == '__main__':
    unittest.main()
