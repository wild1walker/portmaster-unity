"""Opt-in check of the Vulkan path (vulkan.py) on games' data, read from local paths (nothing of it is
in the repository; run Python with -I on data you downloaded):

    PMUNITY_VULKAN_CORPUS=<Game>_Data[:...] python -m unittest tests.test_vulkan_corpus -v   (from convert/)

Needs pmspirvcross (tools/spirvcross/build.sh, or $PMSPIRVCROSS). Per data folder, in every
serialized file (and asset bundle under StreamingAssets): every Shader with Vulkan programs has a
Vulkan blob whose entries read and write back byte for byte and converts in memory
with add_gles3_vulkan (nothing written; GLCore programs, where the shader has them too, are left
as they are). Every GLES subprogram names a translated entry of its own type, every program is
GLSL ES 3.00 or 3.10 with a vertex and a fragment stage. Where the same pass also has a GLCore
program of the variant (a Linux build with both APIs), the translated program's interface is
compared with it: the uniforms, uniform blocks, samplers, vertex inputs and fragment outputs it
declares (names and types; varyings by type, their names differ) should be the GL program's, or
a subset where Unity's Vulkan compile dropped what a stage does not read; the numbers are
reported (PMUNITY_VULKAN_VERBOSE=1: each difference). PMUNITY_VULKAN_EXPORT=<dir>: the programs of
each game go to <dir>/<n>/ (index.tsv) for runtime/host/shadercheck.
"""
import collections
import copy
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import unity_version as UV
from pmunity.job import Job
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import export as X
from pmunity.shaders import glsl as G
from pmunity.shaders import vulkan as V
from pmunity.shaders.step import shader_files

GAMES = [p for p in os.environ.get('PMUNITY_VULKAN_CORPUS', '').split(os.pathsep) if p]
EXPORT = os.environ.get('PMUNITY_VULKAN_EXPORT')
VERBOSE = os.environ.get('PMUNITY_VULKAN_VERBOSE') == '1'
_QUALS = r'(?:(?:flat|smooth|centroid|invariant)\s+)*'
_DECL = re.compile(r'^\s*(%s)(uniform|in|out)\s+(\w+)\s+(\w+)\s*(\[\d*\])?\s*;' % _QUALS, re.M)
_BLOCK = re.compile(r'^\s*uniform\s+(\w+)\s*\{(.*?)\}\s*\w*\s*;', re.M | re.S)


def interface(code):
    """{stage: {'uniform': {(type, name, array)}, 'blocks': {name: [(type, name, array)]}, 'in': ..., 'out': ...}}
    of a GL program, whatever its layout qualifiers, macros and precision."""
    out = {}
    for st, src in G.stage_sources(code).items():
        src = G._code_lines(G._strip_comments(src))
        src = re.sub(r'\blayout\s*\([^)]*\)|\bUNITY_(?:LOCATION|BINDING)\(\d+\)|\b(?:highp|mediump|lowp)\b|\bUNITY_UNIFORM\b',
                     ' ', src)
        d = {'uniform': set(), 'blocks': {}, 'in': set(), 'out': set()}
        for m in _BLOCK.finditer(src):
            d['blocks'][m.group(1)] = [(t, n, a or '') for t, n, a in re.findall(r'(\w+)\s+(\w+)\s*(\[\d+\])?\s*;', m.group(2))
                                       if not n.startswith('Xhlslcc_UnusedX')]
        src = _BLOCK.sub('', src)
        for m in _DECL.finditer(src):
            typ, name = m.group(3), m.group(4)
            if m.group(2) == 'uniform' and name.startswith('hlslcc_zcmp'):
                name = name[len('hlslcc_zcmp'):]
            d[m.group(2)].add((typ, name, m.group(5) or ''))
        out[st] = d
    return out


def compare(ours, gl):
    """Differences between the interface of a translated program and the GL one ([] if the same,
    ('subset', ...) items where ours lacks what GL declares)."""
    diffs = []
    for st in ('VERTEX', 'FRAGMENT'):
        a, b = ours.get(st), gl.get(st)
        if a is None or b is None:
            diffs.append((st, 'stage'))
            continue
        for k in ('uniform',):
            if a[k] - b[k]:
                diffs.append((st, k + ' extra', sorted(a[k] - b[k])))
            if b[k] - a[k]:
                diffs.append((st, k + ' missing', sorted(b[k] - a[k])))
        if set(a['blocks']) != set(b['blocks']):
            diffs.append((st, 'blocks', sorted(a['blocks']), sorted(b['blocks'])))
        for n in set(a['blocks']) & set(b['blocks']):
            if [x for x in a['blocks'][n]] != [x for x in b['blocks'][n]] and \
                    not set(a['blocks'][n]) <= set(b['blocks'][n]):
                diffs.append((st, 'block members ' + n))
        io_in = 'in' if st == 'VERTEX' else None
        if io_in and a['in'] != b['in']:
            diffs.append((st, 'vertex inputs', sorted(a['in'] ^ b['in'])))
        if st == 'FRAGMENT' and a['out'] != b['out']:
            diffs.append((st, 'outputs', sorted(a['out'] ^ b['out'])))
        va = sorted(t for t, _, _ in (a['out'] if st == 'VERTEX' else a['in']))
        vb = sorted(t for t, _, _ in (b['out'] if st == 'VERTEX' else b['in']))
        if va != vb:
            diffs.append((st, 'varyings', va, vb))
    return diffs


@unittest.skipUnless(GAMES, 'set PMUNITY_VULKAN_CORPUS to game data folders')
class VulkanCorpusTest(unittest.TestCase):
    def test_games(self):
        from pmunity.unity_io import Loaded
        summary = []
        tr = V.Translator()
        try:
            for gi, data in enumerate(GAMES):
                v = UV.check(UV.of_data(data))
                job = Job.reader(data, {'version': 1}, log=lambda m: None)
                c = collections.Counter()
                seen, rows = set(), []
                for rel in shader_files(job):
                    lf = Loaded(job.path_in(rel))
                    for _, o in lf.objects('Shader'):
                        t0 = o.read_typetree()
                        if B.PLATFORM_VULKAN not in t0['platforms'] or B.PLATFORM_GLES3PLUS in t0['platforms']:
                            continue
                        name = t0['m_ParsedForm']['m_Name']
                        sb = B.ShaderBlobs(t0)
                        progs, params = V.vulkan_indices(t0)
                        es = sb.entries(B.PLATFORM_VULKAN, params, progs)
                        raw = sb.platforms[B.PLATFORM_VULKAN]
                        segs = raw if sb.nested else [raw]
                        for e, (off, n, seg) in zip(es, B._table(segs[0], sb.nested)):
                            self.assertEqual(e.serialize() if e is not None else b'', bytes(segs[seg][off:off + n]),
                                             f'{data} {rel} {name}')
                        t = copy.deepcopy(t0)
                        r = V.add_gles3_vulkan(t, tr, unity=getattr(lf.sf, 'unity_version', None))
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
                        for p, _, lst in C.subprograms(t):
                            glsp = {C._variant_key(sp): sp for sp in lst if sp['m_GpuProgramType'] == B.GPU_GLCORE32}
                            for sp in lst:
                                if sp['m_GpuProgramType'] not in (B.GPU_GLES3, B.GPU_GLES31):
                                    continue
                                e = ours[sp['m_BlobIndex']]
                                self.assertEqual(e.gpu_type, sp['m_GpuProgramType'], f'{v} {name}')
                                if not e.code:
                                    continue
                                code = e.code.decode()
                                self.assertRegex(code, r'^#ifdef VERTEX\n#version 3[01]0 es\n', f'{v} {name}')
                                g = glsp.get(C._variant_key(sp))
                                if gl is not None and g is not None and gl[g['m_BlobIndex']] is not None and gl[g['m_BlobIndex']].code:
                                    d = compare(interface(code), interface(gl[g['m_BlobIndex']].code.decode()))
                                    c['compared with the GL program'] += 1
                                    if not d:
                                        c['same interface as the GL program'] += 1
                                    else:
                                        for x in d:
                                            c['interface: ' + ' '.join(x[:2])] += 1
                                        if VERBOSE:
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
                c['distinct programs'] = len(seen) if EXPORT else c['distinct programs']
                summary.append(f'{v}: ' + ', '.join(f'{k} {n}' for k, n in sorted(c.items()) if n))
        finally:
            tr.close()
        print('\n' + '\n'.join(summary), file=sys.stderr)


def _reason(why):
    why = re.sub(r'^pass \d+ variant .*?: ', '', why)
    why = re.sub(r'\b(at location|offset|set|binding|program|entry) \d+', r'\1 N', why)
    return re.sub(r"'[^']*'", "'...'", why)[:140]


if __name__ == '__main__':
    unittest.main()
