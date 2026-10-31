"""Compares unityfs's golden lines: the Python's (unityfs_golden.py) and the Rust's (golden.rs).

    python3 unityfs_compare.py py.jsonl rs.jsonl

Prints the totals (files, objects, classes, bundles, what is identical) and every difference;
exits 1 when there is one.
"""
import json
import sys


def load(p):
    out = {}
    with open(p) as f:
        for line in f:
            if line.strip():
                r = json.loads(line)
                out[r['file']] = r
    return out


def main():
    py, rs = load(sys.argv[1]), load(sys.argv[2])
    diffs = []
    tot = dict(files=0, bundles=0, serialized=0, objects=0, read_same=0, read_err_both=0, rewrite_same=0,
               rewrite_orig=0, saves=0, saves_same=0, blocks=0, blocks_same=0, patches=0, patches_same=0)
    classes = {}
    for f in sorted(set(py) | set(rs)):
        a, b = py.get(f), rs.get(f)
        if a is None or b is None:
            diffs.append(f'{f}: only in {"rust" if a is None else "python"}')
            continue
        tot['files'] += 1
        tot['bundles' if a['kind'] == 'bundle' else 'serialized'] += 1
        if a.get('error') or b.get('error'):
            if a.get('error') != b.get('error'):
                diffs.append(f'{f}: error python={a.get("error")!r} rust={b.get("error")!r}')
        if 'objects' in a or 'objects' in b:
            oa, ob = a.get('objects', []), b.get('objects', [])
            if len(oa) != len(ob):
                diffs.append(f'{f}: {len(oa)} objects (python), {len(ob)} (rust)')
            for x, y in zip(oa, ob):
                tot['objects'] += 1
                if x[:3] != y[:3]:
                    diffs.append(f'{f}: object {x[:3]} (python) vs {y[:3]} (rust)')
                    continue
                if x[3].startswith('ERR') or y[3].startswith('ERR'):
                    if x[3].startswith('ERR') and y[3].startswith('ERR'):
                        tot['read_err_both'] += 1
                    else:
                        diffs.append(f'{f}: object {x[0]} ({x[2]}): read python={x[3]} rust={y[3]}')
                    continue
                if x[3] != y[3]:
                    diffs.append(f'{f}: object {x[0]} ({x[2]}): value tree differs')
                    continue
                tot['read_same'] += 1
                if x[4] != y[4]:
                    diffs.append(f'{f}: object {x[0]} ({x[2]}): rewrite python={x[4]} rust={y[4]}')
                else:
                    tot['rewrite_same'] += 1
                    tot['rewrite_orig'] += x[4] == '='
            if a.get('classes') != b.get('classes'):
                diffs.append(f'{f}: classes differ: {a.get("classes")} vs {b.get("classes")}')
            for k, v in (a.get('classes') or {}).items():
                classes[k] = classes.get(k, 0) + v
        for k in ('save', 'edit', 'inline'):
            if k in a or k in b:
                tot['saves'] += 1
                if a.get(k) == b.get(k):
                    tot['saves_same'] += 1
                else:
                    diffs.append(f'{f}: {k}: python {a.get(k)} rust {b.get(k)}')
        if 'blocks' in a or 'blocks' in b:
            tot['blocks'] += 1
            if a.get('blocks') == b.get('blocks'):
                tot['blocks_same'] += 1
            else:
                diffs.append(f'{f}: blocks: python {a.get("blocks")} rust {b.get("blocks")}')
        if 'patch' in a or 'patch' in b:
            tot['patches'] += 1
            if a.get('patch') == b.get('patch'):
                tot['patches_same'] += 1
            else:
                diffs.append(f'{f}: patch: python {a.get("patch")} rust {b.get("patch")}')
    rss = lambda d: max((r.get('rss_mb', 0) for r in d.values()), default=0)  # noqa: E731
    print(json.dumps(tot))
    print(f'classes: {len(classes)} ({sum(classes.values())} objects)')
    print(f'peak rss: python {rss(py)} MB, rust {rss(rs)} MB')
    for d in diffs[:200]:
        print('DIFF', d)
    if len(diffs) > 200:
        print(f'... {len(diffs) - 200} more')
    print(f'{len(diffs)} differences')
    sys.exit(1 if diffs else 0)


if __name__ == '__main__':
    main()
