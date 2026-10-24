"""Tests of the handheld quality profile (pmunity/quality.py) and of MonoBehaviour type trees from
assemblies (pmunity/monotree.py).
    python -m unittest tests.test_quality   (from convert/)
Opt-in on games' data (local paths; nothing of it is in the repository):
    PMUNITY_GAMES=<Game>_Data[:...] python -m unittest tests.test_quality -v
builds a tree for every MonoBehaviour of each game and checks it reads its object exactly where it
reads it at all (and, where TypeTreeGeneratorAPI is installed, that both trees read the same
values: that package's trees misread the MonoBehaviour base and lists of strings, which are left out
of the comparison); then runs the step in memory on a copy of the decisions (nothing written)."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import android_data as AD
from pmunity import monotree as MT
from pmunity import quality as Q

GAMES = [p for p in os.environ.get('PMUNITY_GAMES', '').split(os.pathsep) if p]


def ssao_variants(occlusion, passes=('SSAO_Bilateral_HorizontalBlur', 'SSAO_Gaussian_HorizontalBlur', 'SSAO_Kawase')):
    """A Variants entry of URP's SSAO shader: the occlusion pass with these keyword sets, blur passes."""
    out = [(0, 0, '', 'SSAO_Occlusion', {frozenset(k.split()) for k in occlusion})]
    out += [(0, 0, '', p, {frozenset()}) for p in passes]
    return out


class Profile(unittest.TestCase):
    def test_auto_by_ram(self):
        self.assertEqual(Q.profile_of({'profile': 'auto'}, 973)[0], 'handheld')         # RG34XX-SP
        self.assertEqual(Q.profile_of({'profile': 'auto'}, 1536)[0], 'handheld')
        self.assertEqual(Q.profile_of({'profile': 'auto'}, 1900)[0], 'full')            # a 2 GB handheld
        self.assertEqual(Q.profile_of({'profile': 'auto', 'ram_mb': 4096}, 3800)[0], 'handheld')
        self.assertEqual(Q.profile_of({'profile': 'handheld'}, 8000)[0], 'handheld')
        self.assertEqual(Q.profile_of({'profile': 'full'}, 500)[0], 'full')
        with self.assertRaises(ValueError):
            Q.profile_of({'profile': 'low'}, 500)

    def test_host_caps(self):
        caps = Q.host_caps({'pixelLightCount': 1, 'antiAliasing': 0, 'softParticles': 0, 'anisotropicTextures': 1})
        self.assertEqual(caps, 'pixel_lights=1,msaa=0,anisotropic=1,soft_particles=0')
        self.assertEqual(Q.host_caps({}), '')
        # a run started again over data it capped already keeps the caps it found the first time
        self.assertEqual(Q.merge_caps(caps, 'msaa=0,anisotropic=1'), caps)
        self.assertEqual(Q.merge_caps('msaa=2', 'msaa=0,shadows=1'), 'shadows=1,msaa=0')
        self.assertEqual(Q.merge_caps(None, ''), '')
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(AD.quality_caps(d), '')
            with open(os.path.join(d, '.pmunity.json'), 'w') as f:
                json.dump({'version': 1, 'done': {}, 'quality_caps': caps}, f)
            self.assertEqual(AD.quality_caps(d), caps)
            with open(os.path.join(d, '.pmunity.json'), 'w') as f:
                json.dump({'quality_caps': 'msaa=0; rm -rf /'}, f)
            self.assertEqual(AD.quality_caps(d), '')


class Ssao(unittest.TestCase):
    def test_urp14_both_assets(self):
        # The Third Eye (URP 14): its High Fidelity feature (high samples, depth normals) and its
        # Balanced one (low samples, depth): both pairs are compiled, so High Fidelity may go low
        v = ssao_variants(['_INTERLEAVED_GRADIENT _SAMPLE_COUNT_HIGH _SOURCE_DEPTH_NORMALS',
                           '_INTERLEAVED_GRADIENT _ORTHOGRAPHIC _SAMPLE_COUNT_HIGH _SOURCE_DEPTH_NORMALS',
                           '_INTERLEAVED_GRADIENT _SAMPLE_COUNT_LOW _SOURCE_DEPTH_LOW',
                           '_INTERLEAVED_GRADIENT _ORTHOGRAPHIC _SAMPLE_COUNT_LOW _SOURCE_DEPTH_LOW'])
        st = {'AOMethod': 1, 'Downsample': 0, 'AfterOpaque': 0, 'Source': 1, 'NormalSamples': 1, 'Samples': 0,
              'BlurQuality': 0, 'SampleCount': -1}
        ch = Q.ssao_settings(st, v)
        self.assertEqual(st, {'AOMethod': 1, 'Downsample': 1, 'AfterOpaque': 0, 'Source': 0, 'NormalSamples': 0,
                              'Samples': 2, 'BlurQuality': 2, 'SampleCount': -1})
        self.assertEqual(len(ch), 3)
        self.assertEqual(Q.ssao_settings(st, v), [])                   # already the cheapest

    def test_urp17_stripped(self):
        # Phasing Gun (URP 17): only its own sample count is compiled; downsample and the blur
        # (no keywords) change, the samples stay
        v = ssao_variants(['_BLUE_NOISE _SAMPLE_COUNT_MEDIUM _SOURCE_DEPTH_NORMALS',
                           '_BLUE_NOISE _GTAO_MODE _SAMPLE_COUNT_MEDIUM _SOURCE_DEPTH_NORMALS'])
        st = {'AOMethod': 0, 'Downsample': False, 'Source': 1, 'NormalSamples': 1, 'Samples': 1, 'BlurQuality': 0}
        Q.ssao_settings(st, v)
        self.assertEqual(st, {'AOMethod': 0, 'Downsample': True, 'Source': 1, 'NormalSamples': 1, 'Samples': 1,
                              'BlurQuality': 2})

    def test_partial_cover_keeps_samples(self):
        # a cheaper pair compiled without the orthographic variant the current one has: kept
        v = ssao_variants(['_SAMPLE_COUNT_HIGH _SOURCE_DEPTH_NORMALS', '_ORTHOGRAPHIC _SAMPLE_COUNT_HIGH _SOURCE_DEPTH_NORMALS',
                           '_SAMPLE_COUNT_LOW _SOURCE_DEPTH_NORMALS'], passes=('SSAO_Bilateral_HorizontalBlur',))
        st = {'Downsample': 1, 'Source': 1, 'NormalSamples': 1, 'Samples': 0, 'BlurQuality': 0}
        self.assertEqual(Q.ssao_settings(st, v), [])                   # no kawase pass either
        self.assertEqual(st['Samples'], 0)

    def test_urp10_sample_count(self):
        v = ssao_variants(['_SOURCE_DEPTH_NORMALS'], passes=())
        st = {'Downsample': False, 'Source': 1, 'SampleCount': 12}
        Q.ssao_settings(st, v)
        self.assertEqual(st, {'Downsample': True, 'Source': 1, 'SampleCount': 4})
        self.assertEqual(Q.ssao_settings({'Downsample': 0}, None), [])  # no SSAO shader in the data


def standard(lods=(300, 150), passes150=('FORWARDBASE', 'FORWARDADD', 'SHADOWCASTER', 'META')):
    tree = {'m_ParsedForm': {'m_Name': 'Standard', 'm_SubShaders': [{'m_LOD': l, 'm_Passes': [], 'tag': l} for l in lods]}}
    info = [(0, lods[0], m, '', {frozenset({'DIRECTIONAL'})}) for m in ('FORWARDBASE', 'FORWARDADD', 'SHADOWCASTER', 'DEFERRED')]
    info += [(1, lods[1], m, '', {frozenset({'DIRECTIONAL'})}) for m in passes150]
    return tree, info


class Plausible(unittest.TestCase):
    def test_fields(self):
        asset = {'m_MSAA': 4, 'm_MainLightShadowmapResolution': 4096, 'm_AdditionalLightsShadowmapResolution': 2048,
                 'm_SoftShadowQuality': 3, 'm_RenderScale': 1.0}
        self.assertTrue(Q.plausible('UniversalRenderPipelineAsset', asset))
        self.assertFalse(Q.plausible('UniversalRenderPipelineAsset', dict(asset, m_MSAA=3)))
        self.assertFalse(Q.plausible('UniversalRenderPipelineAsset', dict(asset, m_RenderScale=7.5e-41)))
        self.assertIsNone(Q._urp_edit('UniversalRenderPipelineAsset', dict(asset, m_MainLightShadowmapResolution=1 << 30),
                                      Q.Scan(), dict(Q.DEFAULTS)))
        ssao = {'m_Settings': {'Downsample': 0, 'Source': 1, 'NormalSamples': 1, 'Samples': 0, 'BlurQuality': 0, 'SampleCount': -1}}
        self.assertTrue(Q.plausible('ScreenSpaceAmbientOcclusion', ssao))
        self.assertFalse(Q.plausible('ScreenSpaceAmbientOcclusion', {'m_Settings': dict(ssao['m_Settings'], Samples=9)}))
        self.assertFalse(Q.plausible('FilmGrain', {'active': 7}))
        self.assertTrue(Q.plausible('FilmGrain', {'active': True}))


class StandardShader(unittest.TestCase):
    def test_reorder(self):
        tree, info = standard()
        self.assertEqual(Q.standard_reorder(tree, info), (300, 150))
        self.assertEqual([s['tag'] for s in tree['m_ParsedForm']['m_SubShaders']], [150, 300])
        self.assertIsNone(Q.standard_reorder(tree, info))              # done: LOD 150 is first

    def test_left_alone(self):
        tree, info = standard(passes150=('FORWARDBASE', 'META'))        # no GLES3 forward-add programs
        self.assertIsNone(Q.standard_reorder(tree, info))
        tree, info = standard(lods=(300, 300))
        self.assertIsNone(Q.standard_reorder(tree, info))
        tree, info = standard()
        tree['m_ParsedForm']['m_Name'] = 'Custom/Standard'
        self.assertIsNone(Q.standard_reorder(tree, info))


def light(kind, mode=Q.AUTO, shadows=0, cookie=0, lm=4, res=-1):
    return {'m_Type': kind, 'm_RenderMode': mode, 'm_Lightmapping': lm, 'm_Cookie': {'m_FileID': 0, 'm_PathID': cookie},
            'm_Shadows': {'m_Type': shadows, 'm_Resolution': res}}


class Lights(unittest.TestCase):
    def test_per_pixel_kept(self):
        c = dict(Q.DEFAULTS)
        caps = {'pixelLightCount': 1}
        for l, important in ((light(Q.SPOT), True), (light(Q.POINT), False), (light(Q.POINT, shadows=1), True),
                             (light(Q.POINT, cookie=7), True), (light(Q.DIRECTIONAL, shadows=2), True),
                             (light(Q.SPOT, mode=2), False), (light(Q.SPOT, lm=Q.BAKED), False)):
            before = l['m_RenderMode']
            Q._light_edit(l, caps, c, apply=True)
            self.assertEqual(l['m_RenderMode'], Q.IMPORTANT if important else before, l)
        l = light(Q.SPOT)
        Q._light_edit(l, {}, c, apply=True)                            # no pixel light cap: left Auto
        self.assertEqual(l['m_RenderMode'], Q.AUTO)

    def test_shadow_resolution(self):
        c = dict(Q.DEFAULTS)
        l = light(Q.SPOT, shadows=1, res=3)
        self.assertTrue(Q._light_edit(l, {}, c, apply=True))
        self.assertEqual(l['m_Shadows']['m_Resolution'], 1)
        l = light(Q.SPOT, shadows=1, res=-1)                           # the quality level's
        self.assertFalse(Q._light_edit(l, {}, c))


class Decide(unittest.TestCase):
    def scan(self, vertexlight=True, soft_ok=True, srp=False):
        s = Q.Scan()
        s.srp = srp
        s.quality = {'m_QualitySettings': [
            {'name': 'Medium', 'pixelLightCount': 1, 'shadows': 1, 'shadowResolution': 0, 'shadowCascades': 1,
             'antiAliasing': 0, 'anisotropicTextures': 1, 'softParticles': False, 'lodBias': 0.7, 'shadowDistance': 20.0},
            {'name': 'Ultra', 'pixelLightCount': 4, 'shadows': 2, 'shadowResolution': 2, 'shadowCascades': 4,
             'antiAliasing': 2, 'anisotropicTextures': 2, 'softParticles': True, 'lodBias': 2.0, 'shadowDistance': 150.0}]}
        base = {frozenset({'DIRECTIONAL'})} | ({frozenset({'DIRECTIONAL', 'VERTEXLIGHT_ON'})} if vertexlight else set())
        add = {frozenset({'SPOT', 'SHADOWS_DEPTH', 'SHADOWS_SOFT'})} | ({frozenset({'SPOT', 'SHADOWS_DEPTH'})} if soft_ok else set())
        s.variants.shaders['Standard'] = [(0, 300, 'FORWARDBASE', 'FORWARD', base), (0, 300, 'FORWARDADD', 'FORWARD_DELTA', add)]
        s.lights = [('level1', 5, light(Q.SPOT, shadows=2)), ('level1', 6, light(Q.POINT))]
        return s

    def test_builtin_caps(self):
        s, plan = self.scan(), Q.Plan()
        edits = Q._decide(None, s, dict(Q.DEFAULTS), plan)
        self.assertEqual(s.qcaps, {'pixelLightCount': 1, 'shadows': 1, 'shadowResolution': 1, 'shadowCascades': 2,
                                   'antiAliasing': 0, 'anisotropicTextures': 1, 'softParticles': 0})
        self.assertEqual(edits, {'globalgamemanagers': {'settings'}, 'level1': {'lights'}})
        self.assertEqual(plan.kept, [])
        # never in the caps: LOD selection, shadow distance
        self.assertFalse({'lodBias', 'maximumLODLevel', 'shadowDistance'} & set(s.qcaps))

    def test_variants_missing(self):
        s, plan = self.scan(vertexlight=False, soft_ok=False), Q.Plan()
        Q._decide(None, s, dict(Q.DEFAULTS), plan)
        self.assertNotIn('pixelLightCount', s.qcaps)
        self.assertNotIn('shadows', s.qcaps)
        self.assertEqual(len(plan.kept), 2)

    def test_soft_particles_twin(self):
        s, plan = self.scan(), Q.Plan()
        s.variants.shaders['Particles/Additive'] = [(0, 0, '', '', {frozenset({'SOFTPARTICLES_ON'})})]
        Q._decide(None, s, dict(Q.DEFAULTS), plan)
        self.assertNotIn('softParticles', s.qcaps)
        s.variants.shaders['Particles/Additive'][0][4].add(frozenset())
        Q._decide(None, s, dict(Q.DEFAULTS), Q.Plan())
        self.assertEqual(s.qcaps['softParticles'], 0)

    def test_srp_leaves_forward_lights(self):
        s, plan = self.scan(srp=True), Q.Plan()
        Q._decide(None, s, dict(Q.DEFAULTS), plan)
        self.assertNotIn('pixelLightCount', s.qcaps)
        self.assertNotIn('shadows', s.qcaps)
        self.assertEqual(s.qcaps['antiAliasing'], 0)

    def test_cascades_floor(self):
        job = type('J', (), {'cfg': {'quality': {'profile': 'handheld', 'shadow_cascades': 1}}})()
        with self.assertRaises(ValueError):
            Q.run(job)

    def test_full_changes_nothing(self):
        logs = []
        job = type('J', (), {'cfg': {'quality': {'profile': 'full'}}, 'log': lambda self, m: logs.append(m)})()
        Q.run(job)
        self.assertIn('nothing changed', logs[0])


class MonoTreeNodes(unittest.TestCase):
    def test_builtin_sizes(self):
        sizes = {'float': 4, 'int': 4, 'unsigned int': 4, 'UInt8': 1}

        def size(fields):
            return sum(size(f[2]) if len(f) == 3 else sizes[f[0]] for f in fields)
        for name, (typ, fields) in MT.BUILTIN.items():
            self.assertEqual(size(fields) % 4, 0, name)
        self.assertEqual(size(MT.BUILTIN['UnityEngine.Matrix4x4'][1]), 64)
        self.assertEqual(size(MT.BUILTIN['UnityEngine.Bounds'][1]), 24)

    def test_shapes(self):
        s = MT._string(1, 'm_Name')
        self.assertEqual([(n['m_Level'], n['m_Type']) for n in s], [(1, 'string'), (2, 'Array'), (3, 'int'), (3, 'char')])
        self.assertTrue(s[0]['m_MetaFlag'] & MT.ALIGN and s[1]['m_MetaFlag'] & MT.ALIGN)
        v = MT._vector(1, 'items', MT._pptr(3, 'data', '$Texture'))
        self.assertEqual([n['m_Type'] for n in v], ['vector', 'Array', 'int', 'PPtr<$Texture>', 'int', 'SInt64'])
        self.assertTrue(v[1]['m_MetaFlag'] & MT.ALIGN)


@unittest.skipUnless(GAMES, 'PMUNITY_GAMES not set')
class GamesCorpus(unittest.TestCase):
    def test_trees(self):
        import UnityPy
        from pmunity import unity_version as UV
        try:
            from UnityPy.helpers.TypeTreeGenerator import TypeTreeGenerator
            TypeTreeGenerator('2022.3.0f1')
        except Exception:  # noqa: BLE001 - the oracle is optional
            TypeTreeGenerator = None
        for data in GAMES:
            gen = MT.Generator(os.path.join(data, 'Managed'), UV.of_data(data).line)
            read = total = same = 0
            for n in sorted(os.listdir(data)):
                if not (n.startswith(('level', 'sharedassets', 'resources', 'globalgamemanagers')) and
                        not n.endswith(('.resS', '.resource'))):
                    continue
                env = UnityPy.load(os.path.join(data, n))
                mine = {}
                for o in env.objects:
                    if o.type.name != 'MonoBehaviour':
                        continue
                    total += 1
                    t, node = MT.read(o, gen)
                    if t is not None:
                        read += 1
                        mine[o.path_id] = t
                if TypeTreeGenerator is None or not mine:
                    continue
                g = TypeTreeGenerator(str(env.file.unity_version))
                g.load_local_dll_folder(os.path.join(data, 'Managed'))
                env.typetree_generator = g
                for o in env.objects:
                    if o.path_id in mine:
                        try:
                            ot = o.read_typetree()
                        except Exception:  # noqa: BLE001
                            continue
                        a, b = _norm(mine[o.path_id]), _norm(ot)
                        same += a == b
            with self.subTest(data=data):
                self.assertGreater(read, 0, data)
                print(f'\n{data}: {read} of {total} MonoBehaviours read exactly; {same} the same as TypeTreeGeneratorAPI\'s')


def _norm(v):
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, dict):
        return {k: _norm(x) for k, x in v.items() if k != 'm_Script' and not (isinstance(x, (str, list)) and x in ('', []))}
    if isinstance(v, list):
        return [_norm(x) for x in v]
    return v


if __name__ == '__main__':
    unittest.main()
