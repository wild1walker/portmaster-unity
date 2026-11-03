"""scene_bundles.py: pmu's shaders step on bundles with several serialized files (scene bundles:
`BuildPlayer-<scene>` and its `.sharedAssets`) against a reference the Python makes. The Python's
converter leaves such a bundle as it is (its Loaded opens one serialized file a bundle), so each
serialized file is taken out of the bundle as a plain file and converted by the Python's shaders
step, as any plain .assets file is; pmu's bundle must then hold those bytes.

    python scene_bundles.py <source data> <pmu's converted data> <work> --config <json>
                            [--set k=v]... [--steps shaders] [--keep] [--rest <pmu data>]

For each UnityFS bundle under <source data>/StreamingAssets with more than one serialized file
(UnityPy's reading): every serialized node written to <work>/Data/scene<i>_<k>.assets (with the
data's globalgamemanagers beside, for the version), `python -m pmunity convert <work>/Data
--config <json> --steps shaders [--set ...]` run on it, then pmu's bundle compared: the same nodes
in the same order with the same flags, each serialized node the Python's file byte for byte (one
the Python did not change: as it was), every other node as it was. Prints a line a bundle and
the Shader objects of a node that differs; exit status 1 when one does. `--rest <folder>`: the
same data converted by pmu without the scene bundles (the Python and pmu agree on it: the golden
cases): every other file of <pmu's converted data> the same bytes, the journal the same but for
the scene bundles' entries. Run in the build image (the reference Python at /opt/py); no game
file is written outside <work>.
"""
import json
import argparse
import os
import shutil
import subprocess
import sys

import UnityPy
from UnityPy.files import SerializedFile


def nodes(path):
    """(name, flags, bytes, serialized) of each node of a bundle, in order."""
    env = UnityPy.load(path)
    bf = env.file
    out = []
    for name, f in bf.files.items():
        if isinstance(f, SerializedFile):
            out.append((name, f.flags, f.reader.bytes, True))
        else:
            out.append((name, getattr(f, 'flags', 0), f.bytes, False))
    return out


def shaders(raw):
    """Shader name -> object bytes of a serialized file's bytes."""
    env = UnityPy.load(raw)
    out = {}
    for o in env.objects:
        if o.type.name == 'Shader':
            t = o.read_typetree()
            out[(t['m_ParsedForm']['m_Name'], o.path_id)] = o.get_raw_data()
    return out


def scene_bundles(data):
    sa = os.path.join(data, 'StreamingAssets')
    found = []
    for d, _, names in os.walk(sa):
        for n in sorted(names):
            p = os.path.join(d, n)
            with open(p, 'rb') as f:
                if f.read(8) != b'UnityFS\0':
                    continue
            try:
                ns = nodes(p)
            except Exception as ex:
                print(f'{os.path.relpath(p, data)}: not read ({type(ex).__name__}: {ex})')
                continue
            if sum(1 for x in ns if x[3]) > 1:
                found.append((os.path.relpath(p, data), ns))
    return sorted(found)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('pmu')
    ap.add_argument('work')
    ap.add_argument('--config', required=True)
    ap.add_argument('--set', action='append', default=[])
    ap.add_argument('--steps', default='shaders')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--rest')
    a = ap.parse_args()
    found = scene_bundles(a.src)
    if not found:
        print('no bundle with several serialized files')
        return 0
    data = os.path.join(a.work, 'Data')
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(data)
    shutil.copy(os.path.join(a.src, 'globalgamemanagers'), data)
    plain = {}
    for i, (rel, ns) in enumerate(found):
        for k, (name, _, raw, ser) in enumerate(ns):
            if ser:
                fn = f'scene{i}_{k}.assets'
                plain[(rel, name)] = fn
                with open(os.path.join(data, fn), 'wb') as f:
                    f.write(raw)
    cmd = [sys.executable, '-m', 'pmunity', 'convert', data, '--config', a.config, '--steps', a.steps]
    for s in a.set:
        cmd += ['--set', s]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:], r.stderr[-3000:])
        print(f'the Python stopped: exit {r.returncode}')
        return 1
    bad = 0
    total = {'files': 0, 'changed': 0, 'shaders': 0}
    for rel, ns in found:
        got = nodes(os.path.join(a.pmu, rel))
        why = []
        if [(n, fl) for n, fl, _, _ in got] != [(n, fl) for n, fl, _, _ in ns]:
            why.append(f'nodes {[(n, fl) for n, fl, _, _ in got]}, the source has {[(n, fl) for n, fl, _, _ in ns]}')
        else:
            for (name, _, raw, ser), (_, _, mine, _) in zip(ns, got):
                if not ser:
                    if mine != raw:
                        why.append(f'{name}: changed')
                    continue
                with open(os.path.join(data, plain[(rel, name)]), 'rb') as f:
                    ref = f.read()
                total['files'] += 1
                total['changed'] += ref != raw
                total['shaders'] += len(shaders(ref)) if ref != raw else 0
                if mine != ref:
                    a_sh, b_sh = shaders(ref), shaders(mine)
                    diff = [k for k in sorted(set(a_sh) | set(b_sh)) if a_sh.get(k) != b_sh.get(k)]
                    why.append(f'{name}: {len(mine)} bytes, the Python {len(ref)}; shaders that differ: {diff[:10]}')
        if why:
            bad += 1
            print(f'FAIL {rel}: ' + '; '.join(why))
        else:
            print(f'ok   {rel}: {sum(1 for x in ns if x[3])} serialized files the Python\'s, '
                  f'{sum(1 for x in ns if not x[3])} other nodes as they were')
    print(f'{len(found)} bundles, {total["files"]} serialized files ({total["changed"]} converted, '
          f'{total["shaders"]} shaders in them), {bad} differ')
    if a.rest:
        bad += rest_same(a.rest, a.pmu, {rel for rel, _ in found})
    if not a.keep:
        shutil.rmtree(a.work, ignore_errors=True)
    return 1 if bad else 0


def drop_from_journal(j, rels):
    """The journal with the files of `rels` taken out of its lists and maps."""
    for v in j.get('done', {}).values():
        v[:] = [x for x in v if x not in rels]
    for k in list(j.get('scaled', {})):
        if k in rels or k.split(':')[0] in rels:
            del j['scaled'][k]
    return j


def rest_same(a_top, b_top, rels):
    """Every file but `rels` the same in two converted data folders (1 when not)."""
    def files(top):
        out = {}
        for d, _, names in os.walk(top):
            for n in names:
                p = os.path.join(d, n)
                out[os.path.relpath(p, top)] = p
        return out
    fa, fb = files(a_top), files(b_top)
    bad = []
    for rel in sorted((set(fa) | set(fb)) - rels):
        if rel not in fa or rel not in fb:
            bad.append(f'{rel}: only in {"the second" if rel in fb else "the first"}')
            continue
        with open(fa[rel], 'rb') as x, open(fb[rel], 'rb') as y:
            p, q = x.read(), y.read()
        if os.path.basename(rel) == '.pmunity.json':
            ja, jb = (drop_from_journal(json.loads(t), rels) for t in (p, q))
            for j in (ja, jb):
                if 'audio' in j:
                    j['audio']['switch'] = dict(sorted(j['audio']['switch'].items()))
            if ja != jb:
                bad.append(f'{rel}: differs beyond the scene bundles\' entries')
        elif p != q:
            bad.append(f'{rel}: differs')
    for b in bad:
        print(f'FAIL rest: {b}')
    print(f'rest: {len(set(fa) - rels)} other files, {len(bad)} differ from the run without the scene bundles')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
