"""Opt-in check of the Metal path (metal.py) on games' data, read from local paths (nothing of it is in
the repository; run Python with -I on data you downloaded):

    PMUNITY_METAL_CORPUS=<Game>.app/Contents/Resources/Data[:...] python -m unittest tests.test_metal_corpus -v

Per data folder, in every serialized file (and asset bundle under StreamingAssets): every Shader
with Metal programs has entries that read and write back byte for byte and converts in memory with
add_gles3_metal (nothing written). Every GLES subprogram names a translated entry of its own type,
every program is GLSL ES 3.00 or 3.10. Where the same pass has a GLCore program of the variant (a
macOS build of 2017 - 2020.1 has both), the translated program's interface is compared with it, as
in tests/test_vulkan_corpus.py; PMUNITY_METAL_REFERENCE=<data> compares with another build's GLES3
programs instead (the same game's Android build: shader, pass and keywords), block members counted
where ours declares them. PMUNITY_METAL_EXPORT=<dir>: the programs of each game go to <dir>/<n>/
for runtime/host/shadercheck. PMUNITY_METAL_VERBOSE=1: each difference and left-out variant.
"""
import collections
import copy
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import UnityPy
from pmunity import unity_version as UV
from pmunity.job import Job
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import export as X
from pmunity.shaders import glsl as G
from pmunity.shaders import metal as M
from pmunity.shaders.step import shader_files
from test_vulkan_corpus import interface, compare, _reason

GAMES = [p for p in os.environ.get('PMUNITY_METAL_CORPUS', '').split(os.pathsep) if p]
REFERENCE = os.environ.get('PMUNITY_METAL_REFERENCE')
EXPORT = os.environ.get('PMUNITY_METAL_EXPORT')
VERBOSE = os.environ.get('PMUNITY_METAL_VERBOSE') == '1'


def _flatten(i, ours=None):
    """Uniform blocks as their members (those ours declares, for a reference), outputs by location name."""
    for st, d in i.items():
        for _, mem in d['blocks'].items():
            for m in mem:
                if ours is None or m in ours.get(st, {}).get('uniform', ()):
                    d['uniform'].add(m)
        d['blocks'] = {}
        d['out'] = {(t, re.sub(r'(?i)^SV_Target', 'SV_Target', n), a) for t, n, a in d['out']}
    return i


def reference_programs(data):
    """{(shader, pass index, keyword set): GLES3 program text} of a build's GLES3 programs."""
    from pmunity.unity_io import Loaded
    job = Job.reader(data, {'version': 1}, log=lambda m: None)
    out = {}
    for rel in shader_files(job):
        try:
            lf = Loaded(job.path_in(rel))
        except Exception:
            continue
        for _, o in lf.objects('Shader'):
            t = o.read_typetree()
            if B.PLATFORM_GLES3PLUS not in t['platforms']:
                continue
            es = C.platform_entries(t, B.PLATFORM_GLES3PLUS)
            for ss in t['m_ParsedForm']['m_SubShaders']:
                for pi, p in enumerate(ss['m_Passes']):
                    for lst, _ in C._tier_lists(p.get('progVertex')):
                        for sp in lst:
                            e = es[sp['m_BlobIndex']] if sp['m_GpuProgramType'] in (B.GPU_GLES3, B.GPU_GLES31) else None
                            if isinstance(e, B.Entry) and e.code:
                                key = (t['m_ParsedForm']['m_Name'], pi, frozenset(e.keywords + (e.local_keywords or [])))
                                out.setdefault(key, e.code.decode('utf-8', 'replace'))
        lf.close()
    return out


@unittest.skipUnless(GAMES, 'set PMUNITY_METAL_CORPUS to game data folders')
class MetalCorpusTest(unittest.TestCase):
    def test_games(self):
        from pmunity.unity_io import Loaded
        summary = []
        ref = reference_programs(REFERENCE) if REFERENCE else None
        for gi, data in enumerate(GAMES):
            v = UV.check(UV.of_data(data))
            job = Job.reader(data, {'version': 1}, log=lambda m: None)
            UnityPy.config.FALLBACK_UNITY_VERSION = str(v)    # (version-stripped bundles)
            c = collections.Counter()
            seen, rows = set(), []
            for rel in shader_files(job):
                try:
                    lf = Loaded(job.path_in(rel))
                except Exception as ex:
                    c['files not read'] += 1
                    continue
                for _, o in lf.objects('Shader'):
                    t0 = o.read_typetree()
                    if B.PLATFORM_METAL not in t0['platforms'] or B.PLATFORM_GLES3PLUS in t0['platforms']:
                        continue
                    name = t0['m_ParsedForm']['m_Name']
                    sb = B.ShaderBlobs(t0)
                    lists = list(C.subprogram_lists(t0))
                    progs = {sp['m_BlobIndex'] for _, _, lst, _ in lists for sp in lst if sp['m_GpuProgramType'] in (23, 24)}
                    params = {pbi[i] for _, _, lst, pbi in lists if pbi is not None for i, sp in enumerate(lst)
                              if sp['m_GpuProgramType'] in (23, 24)}
                    es = sb.entries(B.PLATFORM_METAL, params, progs)
                    raw = sb.platforms[B.PLATFORM_METAL]
                    segs = raw if sb.nested else [raw]
                    for e, (off, n, seg) in zip(es, B._table(segs[0], sb.nested)):
                        self.assertEqual(e.serialize() if e is not None else b'', bytes(segs[seg][off:off + n]), f'{data} {rel} {name}')
                    # the variants' keywords as names (for the reference), before the twins are added
                    t = copy.deepcopy(t0)
                    r = M.add_gles3_metal(t, unity=getattr(lf.sf, 'unity_version', None))
                    c[r.status] += 1
                    c['VR-only variants left out'] += r.dropped
                    c['instancing variants left out'] += r.dropped_instancing
                    c['empty GLES3 blobs (stripped shaders)'] += r.empty
                    if r.status == 'skipped':
                        c['left out: ' + _reason(r.why)] += 1
                        if VERBOSE:
                            print(f'{v} {rel}: {name}: left out: {r.why}', file=sys.stderr)
                        continue
                    for si, why in r.failed:
                        c['SubShaders left out: ' + _reason(why)] += 1
                        if VERBOSE:
                            print(f'{v} {rel}: {name}: SubShader {si} left out: {why}', file=sys.stderr)
                    if r.status != 'converted':
                        continue
                    c['programs'] += r.programs
                    c['GLES 3.1 programs'] += r.es31
                    ours = C.platform_entries(t, B.PLATFORM_GLES3PLUS)
                    gl = C.platform_entries(t, B.PLATFORM_GLCORE) if B.PLATFORM_GLCORE in t['platforms'] else None
                    for ss in t['m_ParsedForm']['m_SubShaders']:
                        for pi, p in enumerate(ss['m_Passes']):
                            for lst, _ in C._tier_lists(p.get('progVertex')):
                                glsp = {C._keyset(sp): sp for sp in lst if sp['m_GpuProgramType'] == B.GPU_GLCORE32}
                                for sp in lst:
                                    if sp['m_GpuProgramType'] not in (B.GPU_GLES3, B.GPU_GLES31):
                                        continue
                                    e = ours[sp['m_BlobIndex']]
                                    self.assertEqual(e.gpu_type, sp['m_GpuProgramType'], f'{v} {name}')
                                    if not e.code:
                                        continue
                                    code = e.code.decode()
                                    self.assertRegex(code, r'^#ifdef VERTEX\n#version 3[01]0 es\n', f'{v} {name}')
                                    other = None
                                    if ref is not None:
                                        other = ref.get((name, pi, frozenset(e.keywords + (e.local_keywords or []))))
                                    elif gl is not None and C._keyset(sp) in glsp:
                                        g = gl[glsp[C._keyset(sp)]['m_BlobIndex']]
                                        other = g.code.decode() if isinstance(g, B.Entry) and g.code else None
                                    if other is not None:
                                        a = _flatten(interface(code))
                                        d = [x for x in compare(a, _flatten(interface(other), a)) if x[1] != 'blocks']
                                        c['compared'] += 1
                                        c['same interface'] += not d
                                        for x in d:
                                            c['interface: ' + ' '.join(x[:2])] += 1
                                        if d and VERBOSE:
                                            print(f'{v} {rel}: {name} entry {sp["m_BlobIndex"]}: {d}', file=sys.stderr)
                                    if EXPORT and code not in seen:
                                        dd = os.path.join(EXPORT, str(gi))
                                        os.makedirs(dd, exist_ok=True)
                                        for st, src in G.stage_sources(code).items():
                                            with open(os.path.join(dd, f'{len(seen):04d}.{"vert" if st == "VERTEX" else "frag"}'), 'w') as f:
                                                f.write(X.as_driver_sees(src))
                                        rows.append(f'{len(seen):04d}\t{name}\t{rel}\t{sp["m_BlobIndex"]}\t{" ".join(e.keywords)}')
                                        seen.add(code)
                lf.close()
            if EXPORT and rows:
                with open(os.path.join(EXPORT, str(gi), 'index.tsv'), 'w') as f:
                    f.write('\n'.join(rows) + '\n')
            if EXPORT:
                c['distinct programs'] = len(seen)
            self.assertGreater(c['converted'], 0, data)
            summary.append(f'{v}: ' + ', '.join(f'{k} {n}' for k, n in sorted(c.items()) if n))
        print('\n' + '\n'.join(summary), file=sys.stderr)


if __name__ == '__main__':
    unittest.main()
