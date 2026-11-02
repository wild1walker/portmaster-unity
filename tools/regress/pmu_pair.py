#!/usr/bin/env python3
"""tools/regress/pmu_pair.py: one game's setup run twice, by the Python (`python -m pmunity`, the
reference) and by pmu (setup-rs), on two copies of the same data, and everything they leave
compared. regress.py's `pmu` command runs it in setup-rs's Rust image (ua-rust: the reference
Python at /opt/py with the setup's packages, the musl pmu), once a game:

    pmu_pair.py --data <data dir> --work <dir> --pmu <pmu> --checkout <checkout> --result <json>
                [--config <convert.json>] [--set k=v]... [--plugin P]...
                [--android-data <json list of extra args>] [--pkg <player>] [--logs <dir>] [--keep]

The stages are the android route's setup (port_template/android/patchscript) as it runs them:
`unpack <data> --remove` for a compressed build, `convert <data> --config ...` in place, then
`android-data --src <data> ... --consume-src --verify-bundles 2 --pack all`. After each stage:
the exit statuses, and every file of the data folder (and of android-data's output) by size and
SHA-256, an archive's entries by name and CRC where the archives differ. The journal
(.pmunity.json) is compared as JSON with audio.switch sorted (the Python writes that list in its
set's order: docs/rust-setup.md); log lines are compared with their timings taken out, and a log
difference is a note, not a failure. A stage runs only when the stage before it gave the same
result on both sides.

The result (JSON): {"stages": [{"stage", "py": status, "rs": status, "same": bool, "diffs": [...],
"notes": [...], "seconds": {"py", "rs"}}], "same": bool}. Run with the reference Python (-I is
not needed: nothing is imported from the data)."""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

JOURNAL = '.pmunity.json'
TIMING = re.compile(r'^\[\s*\d+(?:\.\d+)?s\] ')                    # job.log's prefix
SECONDS = re.compile(r'\b\d+(?:\.\d+)? ?(?:s|ms|min|seconds|MB/s)\b')   # durations, rates in a line


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def tree(root, skip_dirs=('.pmunity-work',)):
    """{relative path: (size, sha256 or symlink target)} of every file under root."""
    out = {}
    for d, dirs, files in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x not in skip_dirs)
        for n in sorted(files):
            p = os.path.join(d, n)
            rel = os.path.relpath(p, root)
            if os.path.islink(p):
                out[rel] = ('link', os.readlink(p))
            else:
                out[rel] = (os.path.getsize(p), sha256(p))
    for d, dirs, _ in os.walk(root):             # empty folders count too
        for x in dirs:
            p = os.path.join(d, x)
            if not os.listdir(p):
                out[os.path.relpath(p, root) + '/'] = ('dir', '')
    return out


def journal(path):
    try:
        with open(path) as f:
            j = json.load(f)
    except (OSError, ValueError) as ex:
        return f'unreadable: {ex}'
    a = j.get('audio')
    if isinstance(a, dict) and isinstance(a.get('switch'), list):
        a['switch'] = sorted(a['switch'], key=json.dumps)
    return j


def mtimes_out(v):
    """android-data's state with the source files' modification times taken out: [size, mtime_ns,
    action] lists (the two copies' converted files were written at different times)."""
    if isinstance(v, dict):
        return {k: mtimes_out(x) for k, x in v.items()}
    if isinstance(v, list):
        if len(v) == 3 and isinstance(v[0], int) and isinstance(v[1], int) and isinstance(v[2], str):
            return [v[0], '<mtime>', v[2]]
        return [mtimes_out(x) for x in v]
    return v


def same_but_times(a, b, name):
    """Whether two files that differ are the same but for times: android-data's state (source
    modification times), a log (timings)."""
    if name == '.android_data_state.json':
        try:
            return mtimes_out(json.load(open(a))) == mtimes_out(json.load(open(b)))
        except (OSError, ValueError):
            return False
    if name.endswith('.log'):
        return log_lines(a) == log_lines(b)
    return False


def zip_entries(path):
    try:
        with zipfile.ZipFile(path) as z:
            return {i.filename: (i.CRC, i.file_size, i.compress_type, i.header_offset) for i in z.infolist()}
    except (OSError, zipfile.BadZipFile) as ex:
        return {'(not a zip)': str(ex)}


def compare_trees(a_root, b_root, what, journal_name=JOURNAL):
    """Differences between two folders, as lines."""
    a, b = tree(a_root), tree(b_root)
    diffs = []
    for rel in sorted(set(a) | set(b)):
        if rel not in b:
            diffs.append(f'{what}: {rel}: only the Python wrote it')
            continue
        if rel not in a:
            diffs.append(f'{what}: {rel}: only pmu wrote it')
            continue
        if a[rel] == b[rel]:
            continue
        if os.path.basename(rel) == journal_name:
            ja, jb = journal(os.path.join(a_root, rel)), journal(os.path.join(b_root, rel))
            if ja == jb:
                continue
            keys = sorted(k for k in set(ja) | set(jb) if ja.get(k) != jb.get(k)) \
                if isinstance(ja, dict) and isinstance(jb, dict) else ['(whole)']
            diffs.append(f'{what}: {rel}: the journals differ in {", ".join(keys)}')
            continue
        if same_but_times(os.path.join(a_root, rel), os.path.join(b_root, rel), os.path.basename(rel)):
            continue
        if rel.endswith('.log'):
            diffs.append(f'(log) {what}: {rel}: the log files differ beyond their timings')
            continue
        line = f'{what}: {rel}: {a[rel][0]} bytes against {b[rel][0]}'
        if rel.endswith(('.apk', '.obb', '.zip')):
            za, zb = zip_entries(os.path.join(a_root, rel)), zip_entries(os.path.join(b_root, rel))
            ent = [n for n in sorted(set(za) | set(zb)) if za.get(n) != zb.get(n)]
            line += f'; {len(ent)} of {len(set(za) | set(zb))} entries differ: {", ".join(ent[:8])}'
        diffs.append(line)
    return diffs


def log_lines(path):
    out = []
    for line in open(path, errors='replace').read().splitlines():
        line = TIMING.sub('', line)
        if line.startswith(('Traceback', '  File ')):
            continue
        out.append(SECONDS.sub('<t>', line))
    return out


class Pair:
    def __init__(self, a):
        self.a = a
        self.work = os.path.abspath(a.work)
        self.stages = []
        self.cur = os.path.join(self.work, 'run')
        self.env_py = dict(os.environ, PYTHONHOME='/opt/py', LD_LIBRARY_PATH='/opt/py/libs',
                           PYTHONPATH=os.path.join(a.checkout, 'convert') + ':/opt/pylibs', PYTHONDONTWRITEBYTECODE='1')
        self.env_rs = dict(os.environ)

    def side(self, s):
        return os.path.join(self.work, s)

    def data(self, s):
        return os.path.join(self.side(s), 'data')

    def run(self, s, args, log):
        cmd = (['/opt/py/bin/python3', '-m', 'pmunity'] if s == 'py' else [self.a.pmu]) + args
        t = time.time()
        with open(log, 'w') as f:
            f.write('$ ' + ' '.join(cmd) + '\n')
            f.flush()
            p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=self.env_py if s == 'py' else self.env_rs,
                               cwd=os.path.join(self.a.checkout, 'convert') if s == 'py' else self.work)
        return p.returncode, round(time.time() - t, 1)

    def stage(self, name, args, trees):
        """Runs a stage on both sides, each in turn at the same path (work/run: what a tool writes
        of its paths into reports and logs is then the same); trees: the folders compared (relative
        to a side's folder)."""
        st = {'stage': name, 'diffs': [], 'notes': [], 'seconds': {}}
        logs = {}
        for s in ('py', 'rs'):
            logs[s] = os.path.join(self.work, f'{name}.{s}.log')
            os.rename(self.side(s), self.cur)
            try:
                st[s], st['seconds'][s] = self.run(s, args, logs[s])
            finally:
                os.rename(self.cur, self.side(s))
        if st['py'] != st['rs']:
            st['diffs'].append(f"exit status: the Python {st['py']}, pmu {st['rs']}")
        for label in trees:
            for d in compare_trees(os.path.join(self.side('py'), label), os.path.join(self.side('rs'), label), label):
                (st['notes'] if d.startswith('(log) ') else st['diffs']).append(d)
        la, lb = log_lines(logs['py'])[1:], log_lines(logs['rs'])[1:]
        if la != lb:
            n = sum(1 for x, y in zip(la, lb) if x != y) + abs(len(la) - len(lb))
            first = next((f'{x!r} / {y!r}' for x, y in zip(la, lb) if x != y), f'{len(la)} lines against {len(lb)}')
            st['notes'].append(f'log: {n} lines differ (first: {first[:300]})')
        st['tail'] = {s: open(logs[s], errors='replace').read().splitlines()[-3:] for s in ('py', 'rs')}
        st['same'] = not st['diffs']
        self.stages.append(st)
        print(f"  {name}: Python {st['py']} ({st['seconds']['py']} s), pmu {st['rs']} ({st['seconds']['rs']} s): "
              f"{'same' if st['same'] else str(len(st['diffs'])) + ' differences'}", flush=True)
        for d in st['diffs'][:12] + st['notes']:
            print('    ' + d, flush=True)
        return st['same'] and st['py'] == 0

    def main(self):
        a = self.a
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.side('py'))
        os.makedirs(self.side('rs'))
        t = time.time()
        if a.builtin:
            # the desktop player's built-in resources as the data's one assets file, with the data's
            # globalgamemanagers: their Shaders and ComputeShaders carry GLCore and GLES3 programs
            # and kernels (Unity's own), which `shaders.replace` rebuilds from the GLCore ones
            # (a macOS app has it beside its Data folder, in Contents/Resources: --builtin-res)
            res = a.builtin_res or next((r for r in (os.path.join(a.data, 'Resources', 'unity default resources'),
                                                     os.path.join(a.data, '..', 'unity default resources'))
                                         if os.path.isfile(r)), os.path.join(a.data, 'Resources', 'unity default resources'))
            if not os.path.isfile(res):
                print(f'  no {res}', flush=True)
                with open(a.result, 'w') as f:
                    json.dump({'stages': [], 'same': False, 'skip': 'no unity default resources'}, f)
                return 3
            os.makedirs(self.data('py'))
            shutil.copy2(os.path.join(a.data, 'globalgamemanagers'), self.data('py'))
            shutil.copy2(res, os.path.join(self.data('py'), 'builtin.assets'))
        elif a.files:
            # only these files of the data (a big game's shaders and settings: its serialized files)
            for rel in a.files.split(','):
                dst = os.path.join(self.data('py'), rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(os.path.join(a.data, rel), dst)
        else:
            shutil.copytree(a.data, self.data('py'), symlinks=True)
        shutil.copytree(self.data('py'), self.data('rs'), symlinks=True)
        print(f'  copied the data twice ({round(time.time() - t)} s)', flush=True)
        cur = os.path.join(self.cur, 'data')
        ok = True
        if not os.path.isfile(os.path.join(self.data('py'), 'globalgamemanagers')) and \
                os.path.isfile(os.path.join(self.data('py'), 'data.unity3d')):
            ok = self.stage('unpack', ['unpack', cur, '--remove'], ['data'])
        if ok and a.config:
            args = ['convert', cur, '--config', a.config]
            for p in a.plugin:
                args += ['--plugin', p]
            for kv in a.set:
                args += ['--set', kv]
            if a.steps:
                args += ['--steps', a.steps]
            ok = self.stage('convert', args, ['data'])
        if ok and a.pkg:
            args = ['android-data', '--src', cur, '--pkg', a.pkg, '--out', os.path.join(self.cur, 'android'),
                    '--name', 'Game', '--package', 'com.regress.pmu', '--swappy', 'off'] + \
                ([] if a.no_consume else ['--consume-src']) + \
                ['--verify-bundles', '2', '--pack', 'all'] + json.loads(a.android_data or '[]')
            self.stage('android-data', args, ['android', 'data'])
        if a.logs:
            for n in sorted(os.listdir(self.work)):
                if n.endswith('.log'):
                    shutil.copyfile(os.path.join(self.work, n), os.path.join(a.logs, n))
        res = {'stages': self.stages, 'same': all(s['same'] for s in self.stages) and bool(self.stages)}
        with open(a.result, 'w') as f:
            json.dump(res, f, indent=1)
        if not a.keep:
            shutil.rmtree(self.work, ignore_errors=True)
        return 0 if res['same'] else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--pmu', required=True)
    ap.add_argument('--checkout', required=True)
    ap.add_argument('--result', required=True)
    ap.add_argument('--config')
    ap.add_argument('--set', action='append', default=[])
    ap.add_argument('--steps')
    ap.add_argument('--plugin', action='append', default=[])
    ap.add_argument('--android-data', help='more android-data arguments (a JSON list)')
    ap.add_argument('--pkg', help="Unity's Android player (android-data runs only with one)")
    ap.add_argument('--no-consume', action='store_true', help='android-data without --consume-src')
    ap.add_argument('--logs', help='a folder the stages\' logs are copied to')
    ap.add_argument('--files', help='only these files of the data (relative paths, comma-separated)')
    ap.add_argument('--builtin', action='store_true',
                    help="the data: its globalgamemanagers and the player's Resources/unity default resources as builtin.assets")
    ap.add_argument('--builtin-res', help="with --builtin: the player's unity default resources (default: the data's "
                    "Resources/, else the folder above the data: a macOS app's Contents/Resources)")
    ap.add_argument('--keep', action='store_true')
    return Pair(ap.parse_args(argv)).main()


if __name__ == '__main__':
    sys.exit(main())
