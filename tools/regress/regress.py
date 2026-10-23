#!/usr/bin/env python3
"""tools/regress: the regression run. Takes the games of games.toml through the tools as a porter
would and compares what comes out with the list's expectations and with the recorded baseline.
See README.md.

    regress.py fast [--only REGEX] [--update] [--keep] [--checkout DIR]   convert + pack + checks (no Docker)
    regress.py rig  [--only REGEX] [--update] [--keep] [--checkout DIR]   the games in the emulated rig
    regress.py list [--only REGEX]                                        the list: games x levels x checks
    regress.py bisect GOOD BAD --level fast|rig --only REGEX [--build CMD] the first commit at which they fail

Exit status: 0 when nothing failed, 1 when a check failed, 2 for a usage or setup error.
"""
import argparse
import datetime
import glob
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import checks as K  # noqa: E402

LEVELS = ('fast', 'rig')
ROUTES = ('android', 'apk', 'il2cpp', 'box64', 'shaders', 'prebuilt')
PACK = ('none', 'tree', 'all')
GENERIC = {'steps': ['shaders', 'settings'], 'settings': {'graphics_apis': [11]}}
LIBS = 'Variations/{backend}/Release/Libs/{abi}'
APK_FACTS = ('abi', 'abis', 'backend', 'min_sdk', 'target_sdk', 'unity_version', 'source', 'obbs', 'il2cpp', 'swappy',
             'graphics_apis', 'target')        # what `pmunity apk --json` found, never the game's names
MONOLIBS = 'Variations/mono/Release/MonoLibs/armeabi-v7a'


DRY_RUN_LINE = re.compile(r'(data|assemblies|asmpatch|UnityLinker|il2cpp|mix\w*|bee\w*|Bee\w*): |\S+\.(h|c|cpp): ')


class SetupError(Exception):
    pass


class Skip(Exception):
    pass


class Fail(Exception):
    pass


# --------------------------------------------------------------------------- configuration

def deep_merge(a, b):
    out = dict(a)
    for k, v in b.items():
        out[k] = deep_merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def load_local(path):
    """The machine's own settings (local.toml, gitignored; REGRESS_CONFIG names another file);
    environment variables REGRESS_<KEY> override single keys."""
    path = path or os.environ.get('REGRESS_CONFIG') or os.path.join(HERE, 'local.toml')
    cfg = {}
    if os.path.isfile(path):
        with open(path, 'rb') as f:
            cfg = tomllib.load(f)
    for k in ('corpus', 'scratch', 'python', 'il2cpp_home', 'hostbin', 'hostbin64', 'asmpatch', 'pmhlslcc',
              'pmspirvcross', 'dotnet', 'box64_bin'):
        v = os.environ.get('REGRESS_' + k.upper())
        if v:
            cfg[k] = v
    if os.environ.get('REGRESS_PLAYERS'):
        cfg['players'] = os.environ['REGRESS_PLAYERS'].split(os.pathsep)
    cfg['_path'] = path
    return cfg


def load_list(path):
    with open(path, 'rb') as f:
        doc = tomllib.load(f)
    games = doc.get('game') or []
    seen = set()
    for g in games:
        for k in ('slug', 'route'):
            if k not in g:
                raise SetupError(f'{path}: a [[game]] without {k}: {g}')
        g.setdefault('id', g['slug'])
        if g['id'] in seen:
            raise SetupError(f"{path}: {g['id']} twice (give one an id)")
        seen.add(g['id'])
        if g['route'] not in ROUTES:
            raise SetupError(f"{path}: {g['slug']}: route {g['route']!r} (one of {', '.join(ROUTES)})")
        g.setdefault('levels', ['fast'])
        g.setdefault('covers', [])
        if g.get('pack', 'none') not in PACK:
            raise SetupError(f"{path}: {g['slug']}: pack {g.get('pack')!r} (one of {', '.join(PACK)})")
    return doc.get('defaults') or {}, games


class Ctx:
    def __init__(self, a, local):
        self.a = a
        self.local = local
        self.checkout = os.path.abspath(a.checkout or REPO)
        self.convert_dir = os.path.join(self.checkout, 'convert')
        if not os.path.isdir(os.path.join(self.convert_dir, 'pmunity')):
            raise SetupError(f'{self.checkout}: no convert/pmunity (not a portmaster-unity checkout)')
        self.py = local.get('python') or sys.executable
        self.scratch = local.get('scratch')
        if not self.scratch:
            raise SetupError(f"no scratch folder: set scratch in {local['_path']} or REGRESS_SCRATCH")
        self.scratch = os.path.abspath(self.scratch)
        self.corpus = local.get('corpus')
        self.players = [os.path.abspath(p) for p in local.get('players') or ()]
        self.fetch_players = os.path.join(self.scratch, 'players')
        self.il2cpp_home = local.get('il2cpp_home')
        main_out = os.path.join(self.checkout, 'runtime', 'host', 'out')
        self.hostbin = os.path.abspath(local.get('hostbin') or main_out)
        self.hostbin64 = os.path.abspath(local.get('hostbin64') or os.path.join(self.hostbin, 'arm64'))
        self.asmpatch = local.get('asmpatch')
        self.dotnet = local.get('dotnet') or 'dotnet'
        if not self.asmpatch:
            dll = os.path.join(self.checkout, 'tools', 'asmpatch', 'out', 'any', 'asmpatch.dll')
            self.asmpatch = f'{self.dotnet} {dll}' if os.path.isfile(dll) else None
        self.games = local.get('games') or {}
        self.prebuilt = local.get('prebuilt') or {}
        self.env = dict(os.environ)
        for k, envk in (('pmhlslcc', 'PMHLSLCC'), ('pmspirvcross', 'PMSPIRVCROSS')):
            if local.get(k):
                self.env[envk] = local[k]
        if self.asmpatch:
            self.env['PMUNITY_ASMPATCH'] = self.asmpatch
        if self.il2cpp_home:
            self.env['PMUNITY_IL2CPP_HOME'] = self.il2cpp_home
        self.run_id = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        self.results = os.path.join(self.scratch, 'results', f'{a.level}-{self.run_id}')

    def paths_to_hide(self):
        """Machine paths replaced in what goes into a baseline (commands of a dry run)."""
        out = [(self.scratch, '<scratch>'), (self.checkout, '<checkout>')]
        if self.il2cpp_home:
            out.append((os.path.abspath(self.il2cpp_home), '<home>'))
        if self.corpus:
            out.append((os.path.abspath(self.corpus), '<corpus>'))
        out.append((os.path.expanduser('~'), '<home-dir>'))
        return sorted(out, key=lambda p: -len(p[0]))


# --------------------------------------------------------------------------- helpers

def run(cmd, log, cwd=None, env=None, timeout=None):
    """Runs cmd with its output appended to the file log; returns the exit status."""
    with open(log, 'a') as f:
        f.write(f'$ {" ".join(shlex.quote(c) for c in cmd)}\n')
        f.flush()
        try:
            p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=cwd, env=env, timeout=timeout)
        except subprocess.TimeoutExpired:
            f.write(f'(timed out after {timeout} s)\n')
            return 124
    return p.returncode


def run_json(cmd, log, cwd=None, env=None, timeout=None):
    """Runs a command that prints JSON on stdout (its log on stderr, into the file log): (status, object)."""
    with open(log, 'a') as f:
        f.write(f'$ {" ".join(shlex.quote(c) for c in cmd)}\n')
        f.flush()
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=f, cwd=cwd, env=env, timeout=timeout, text=True,
                               errors='replace')
        except subprocess.TimeoutExpired:
            f.write(f'(timed out after {timeout} s)\n')
            return 124, None
    try:
        return p.returncode, json.loads(p.stdout)
    except ValueError:
        with open(log, 'a') as f:
            f.write(p.stdout[-2000:])
        return p.returncode or 1, None


def tail(path, n=4):
    try:
        with open(path, errors='replace') as f:
            return ' | '.join(l.strip() for l in f.readlines()[-n:] if l.strip())[-400:]
    except OSError:
        return ''


def read_lines(path):
    try:
        with open(path, errors='replace') as f:
            return f.read().splitlines()
    except OSError:
        return []


def pmunity(ctx, args, log, timeout=None, env=None):
    e = dict(ctx.env, **(env or {}))
    return run([ctx.py, '-m', 'pmunity'] + args, log, cwd=ctx.convert_dir, env=e, timeout=timeout)


def digest(ctx, args, log):
    """digest.py in its own process with -I: it reads game data and the converter's output."""
    return run([ctx.py, '-I', os.path.join(HERE, 'digest.py')] + args, log, env=ctx.env)


def children_cpu():
    import resource
    r = resource.getrusage(resource.RUSAGE_CHILDREN)
    return r.ru_utime + r.ru_stime


def rmtree(p):
    if os.path.lexists(p):
        if os.path.islink(p) or not os.path.isdir(p):
            os.unlink(p)
        else:
            shutil.rmtree(p, ignore_errors=True)


def du_mb(p):
    n = 0
    for d, _, fs in os.walk(p):
        for f in fs:
            try:
                n += os.lstat(os.path.join(d, f)).st_size
            except OSError:
                pass
    return n >> 20


def game_root(ctx, g):
    """The game's folder: the corpus' games/<slug>/game (games/<slug>/source.json beside it), or
    a path the local config gives under [games]."""
    slug = g['slug']
    if slug in ctx.games:
        p = ctx.games[slug]
        if not os.path.exists(p):
            raise Skip(f'{p} (local config [games].{slug}) not found')
        return os.path.abspath(p)
    if not ctx.corpus:
        raise Skip('no corpus (corpus in the local config, or REGRESS_CORPUS)')
    p = os.path.join(ctx.corpus, 'games', slug, 'game')
    if not os.path.isdir(p):
        raise Skip(f'not in the corpus (games/{slug}/game)')
    return p


def find_data(root, g):
    """The Unity data folder in a game folder, as the corpus scripts find it."""
    if g.get('data'):
        p = os.path.join(root, g['data'])
        if not os.path.isdir(p):
            raise Skip(f"data folder {g['data']} not in the game folder")
        return p
    if os.path.isfile(os.path.join(root, 'globalgamemanagers')) or os.path.isfile(os.path.join(root, 'data.unity3d')):
        return root
    found = []
    for d, dirs, _ in os.walk(root, followlinks=True):
        if '__MACOSX' in d.split(os.sep):
            dirs[:] = []
            continue
        if d[len(root):].count(os.sep) > 5:
            dirs[:] = []
            continue
        if d.endswith('_Data') or d.endswith(os.path.join('.app', 'Contents', 'Resources', 'Data')):
            found.append(d)
            dirs[:] = []
    if not found:
        raise Skip('no Unity data folder in the game folder')
    return sorted(found, key=lambda p: (p.count(os.sep), p))[0]


def mono_etc(data):
    for d in (data, os.path.dirname(data), os.path.dirname(os.path.dirname(data))):
        for m in ('MonoBleedingEdge', 'Mono'):
            if os.path.isdir(os.path.join(d, m, 'etc')):
                return os.path.join(d, m, 'etc')
    return None


def player_pkg(ctx, version, log):
    """Unity's Android player of that version: a `pmunity player fetch` folder in one of the
    local config's player folders (<dir>/<version>/pkg or <dir>/<version>), else fetched (about
    10 MB of range reads) into <scratch>/players."""
    for d in ctx.players + [ctx.fetch_players]:
        for p in (os.path.join(d, version, 'pkg'), os.path.join(d, version)):
            if os.path.isfile(os.path.join(p, 'player.json')):
                return p
    if ctx.a.offline:
        raise Skip(f'no Android player {version} here (--offline: not fetched)')
    p = os.path.join(ctx.fetch_players, version, 'pkg')
    rc = pmunity(ctx, ['player', 'fetch', version, p], log, timeout=1800)
    if rc or not os.path.isfile(os.path.join(p, 'player.json')):
        raise Fail(f'player fetch {version} failed: {tail(log, 2)}')
    return p


def source_version(ctx, g):
    try:
        with open(os.path.join(ctx.corpus or '', 'games', g['slug'], 'source.json')) as f:
            return json.load(f).get('version')
    except (OSError, ValueError):
        return g.get('unity')


def view_dir(src, overlays, dst, skip=('data.unity3d',)):
    """dst = a folder of symlinks to every file of src (real folders), each overlay folder's files
    replacing src's (an unpacked data.unity3d, a convert overlay); nothing is copied. The converter
    and android-data read through the links (android-data then clones or copies what it places)."""
    rmtree(dst)
    os.makedirs(dst)
    layers = [src] + [o for o in overlays if o and os.path.isdir(o)]
    for base in layers:
        for d, dirs, files in os.walk(base, followlinks=True):
            dirs[:] = [x for x in dirs if not x.startswith('.pmunity')]
            rel = os.path.relpath(d, base)
            od = os.path.normpath(os.path.join(dst, rel))
            os.makedirs(od, exist_ok=True)
            for n in files:
                if n.startswith('.pmunity') or (base is src and n in skip):
                    continue
                t = os.path.join(od, n)
                if os.path.lexists(t):
                    os.unlink(t)
                os.symlink(os.path.realpath(os.path.join(d, n)), t)
    return dst


def docker_names():
    try:
        return subprocess.run(['docker', 'ps', '--format', '{{.Names}}'], capture_output=True, text=True,
                              timeout=60).stdout.split()
    except (OSError, subprocess.TimeoutExpired):
        return []


def docker_gate(ctx, log):
    """At most one container of this run at a time (names regress*), and none started while two or
    more others run (the Mac's Docker VM is shared)."""
    waited = 0
    while True:
        try:
            names = subprocess.run(['docker', 'ps', '--format', '{{.Names}}'], capture_output=True, text=True,
                                   timeout=60).stdout.split()
        except (OSError, subprocess.TimeoutExpired):
            raise Skip('docker not answering')
        mine = [n for n in names if n.startswith('regress')]
        others = [n for n in names if not n.startswith('regress')]
        if not mine and len(others) < 2:
            if waited:
                with open(log, 'a') as f:
                    f.write(f'(waited {waited} s for Docker: {len(others)} other containers)\n')
            return
        if waited % 300 == 0:
            print(f'    waiting for Docker: ours {mine}, {len(others)} others', flush=True)
        time.sleep(20)
        waited += 20


# --------------------------------------------------------------------------- steps

class Game:
    """One game's run of one level: its work folder, log, values (what goes into the baseline),
    checks and notes."""

    def __init__(self, ctx, g, level):
        self.ctx, self.g, self.level = ctx, g, level
        self.slug, self.id = g['slug'], g['id']
        self.work = os.path.join(ctx.scratch, 'work', f'{level}-{self.id}')
        self.out = os.path.join(ctx.results, self.id)
        self.log = os.path.join(self.out, 'run.txt')
        self.values = {}
        self.checks = []
        self.notes = []
        self.timing = {}
        self.cpu = {}

    def step(self, name):
        """A timed step: wall seconds (timing) and the CPU seconds of the commands it ran (cpu: what
        the step costs on a machine whose disk keeps up)."""
        g = self

        class T:
            def __enter__(self):
                self.t = time.time()
                self.c = children_cpu()
                print(f'    {name}', flush=True)
                with open(g.log, 'a') as f:
                    f.write(f'\n== {name}\n')

            def __exit__(self, *a):
                g.timing[name] = round(time.time() - self.t, 1)
                g.cpu[name] = round(children_cpu() - self.c, 1)
        return T()

    # ---- the game's data
    def locate(self):
        if self.g['route'] == 'prebuilt':
            return
        self.root = game_root(self.ctx, self.g)
        if self.g['route'] == 'apk':
            return
        self.data = find_data(self.root, self.g)
        self.version = self.g.get('unity') or source_version(self.ctx, self.g)

    def check(self):
        """port check --json (android, il2cpp, apk) or route --json (box64): the verdict."""
        ctx = self.ctx
        j = os.path.join(self.out, 'check.log')
        if self.g['route'] == 'box64':
            with self.step('route'):
                rc, rep = run_json([ctx.py, '-m', 'pmunity', 'route', self.root, '--json'], j, cwd=ctx.convert_dir, env=ctx.env)
                if rc or rep is None:
                    raise Fail(f'pmunity route failed: {tail(j)}')
                self.values['route'] = K.route_verdict(rep)
            return None
        with self.step('port check'):
            args = ['port', 'check', '--json', self.root]
            if ctx.asmpatch:
                args[3:3] = ['--asmpatch', ctx.asmpatch]
            rc, rep = run_json([ctx.py, '-m', 'pmunity'] + args, j, cwd=ctx.convert_dir, env=ctx.env, timeout=1800)
            if rep is None or rc not in (0, 1):         # (1: its report says no route fits)
                raise Fail(f'port check failed (exit {rc}): {tail(j)}')
            self.values['check'] = K.check_verdict(rep)
            self.values['check']['exit'] = rc
            want = self.g.get('unity')
            if want and rep.get('unity_version') and rep['unity_version'] != want:
                self.checks.append(('unity', False, f"data is Unity {rep['unity_version']}, the list says {want}"))
            if rep.get('unity_version'):
                self.version = rep['unity_version']
            return rep

    def source(self):
        """The data to convert: the game's own folder, or for a compressed build (data.unity3d) a
        view of it with the archive unpacked into the work folder."""
        if os.path.isfile(os.path.join(self.data, 'globalgamemanagers')):
            return self.data
        if not os.path.isfile(os.path.join(self.data, 'data.unity3d')):
            raise Fail('no globalgamemanagers and no data.unity3d in the data folder')
        with self.step('unpack'):
            up = os.path.join(self.work, 'unpacked')
            rc = pmunity(self.ctx, ['unpack', self.data, '--out', up], self.log)
            if rc:
                raise Fail(f'unpack failed: {tail(self.log)}')
            self.notes.append('compressed build: unpacked')
            return view_dir(self.data, [up], os.path.join(self.work, 'src'))

    def anyver(self):
        return ['--any-unity-version'] if self.g.get('any_unity_version') else []

    def convert_config(self, rep):
        cfg = json.loads(json.dumps(GENERIC))
        build = (rep or {}).get('build') or self.g.get('build')
        if build == 'windows':
            cfg['shaders'] = {'from': ['d3d11']}
        # of what the check suggests only the shaders' source (metal, vulkan): its other steps
        # (textures, audio) change a port's picture or memory, which a port opts into; a game of the
        # list gets them through its own convert table
        sug = ((rep or {}).get('suggest') or {}).get('convert') or {}
        cfg = deep_merge(cfg, {k: v for k, v in sug.items() if k == 'shaders'})
        if self.g['route'] == 'box64':
            with open(os.path.join(self.ctx.checkout, 'tools', 'box64', 'gles.json')) as f:
                b = json.load(f)
            b.pop('comment', None)
            cfg = deep_merge(cfg, b)
        cfg = deep_merge(cfg, self.g.get('convert') or {})
        cfg.setdefault('version', 1)
        return cfg

    def shaders(self, src, cfg, real):
        """The shaders step: real = `pmunity convert --steps shaders` into the overlay (android-data
        reads it next), else in memory (digest.py shaders: the same step, nothing written)."""
        ctx = self.ctx
        cfgp = os.path.join(self.work, 'convert.json')
        with open(cfgp, 'w') as f:
            json.dump(cfg, f, indent=1)
        dj = os.path.join(self.work, 'shaders.json')
        conv = os.path.join(self.work, 'conv')
        with self.step('shaders' + ('' if real else ' (in memory)')):
            if real:
                clog = os.path.join(self.work, 'convert.log')
                rc = pmunity(ctx, ['convert', src, conv, '--config', cfgp, '--steps', 'shaders'] + self.anyver(), clog,
                             timeout=7200)
                shutil.copyfile(clog, os.path.join(self.out, 'convert-shaders.log'))
                if rc:
                    raise Fail(f'convert (shaders) failed: {tail(clog)}')
                lines = read_lines(clog)
                rc = digest(ctx, ['files', ctx.checkout, conv, dj], self.log)
            else:
                rc = digest(ctx, ['shaders', ctx.checkout, src, cfgp, os.path.join(self.work, 'mem'), dj] + self.anyver(),
                            self.log)
                lines = None
            if rc:
                raise Fail(f'shader digest failed: {tail(self.log)}')
            d = json.load(open(dj))
            if lines is None:
                lines = d.get('log') or []
            files = d['files']
            self.values['shaders'] = K.shader_counts(lines)
            self.values['shaders_digest'] = {'total': d['total'], 'files': len(files),
                                             'shaders': sum(f.get('shaders', 0) for f in files.values()),
                                             'compute': sum(f.get('compute', 0) for f in files.values()),
                                             'per_file': {hide_name(k): f['file'][:16] for k, f in files.items()}}
            self._names = {hide_name(k): k for k in files}
        return conv

    def settings(self, src, conv, cfg):
        """The other convert steps (settings, ...) into the overlay; their written files' digests."""
        steps = [s for s in cfg.get('steps', ()) if s != 'shaders']
        if not steps:
            return
        ctx = self.ctx
        before = {}
        dj = os.path.join(self.work, 'before.json')
        if os.path.isdir(conv) and digest(ctx, ['tree', conv, dj], self.log) == 0:
            before = json.load(open(dj))['files']
        with self.step('convert ' + ','.join(steps)):
            clog = os.path.join(self.work, 'convert2.log')
            rc = pmunity(ctx, ['convert', src, conv, '--config', os.path.join(self.work, 'convert.json'),
                               '--steps', ','.join(steps)] + self.anyver(), clog, timeout=7200)
            shutil.copyfile(clog, os.path.join(self.out, 'convert-steps.log'))
            if rc:
                raise Fail(f"convert ({','.join(steps)}) failed: {tail(clog)}")
            if digest(ctx, ['tree', conv, dj], self.log):
                raise Fail('digest of the converted files failed')
            after = json.load(open(dj))['files']
            written = {k: v for k, v in after.items() if before.get(k) != v}
            self.values['steps'] = {'steps': steps, 'files': len(written),
                                    'per_file': {hide_name(k): v[1][:16] for k, v in sorted(written.items())}}

    def android_data(self, src, conv, rep, pack, il2cpp=None, pkg=None):
        """android-data on the converted data (a view: the data's files + the overlay's)."""
        ctx = self.ctx
        if pkg is None:
            pkg = player_pkg(ctx, self.version, self.log)
        view = view_dir(src, [conv], os.path.join(self.work, 'view'))
        out = os.path.join(self.work, 'android')
        args = ['android-data', '--src', view, '--pkg', pkg, '--out', out, '--name', 'Game',
                '--package', 'com.regress.' + re.sub(r'[^a-z0-9]', '', self.slug),
                '--pack', 'none' if pack == 'tree' else pack, '--force']
        sug = ((rep or {}).get('suggest') or {}).get('android_data') or {}
        sam = self.g.get('streaming_assets', sug.get('streaming_assets'))
        if sam:
            args += ['--streaming-assets', sam]
        asm = self.g.get('asmpatch', sug.get('asmpatch', 'off'))
        if il2cpp is None:
            if asm not in (None, 'off', False):
                if not ctx.asmpatch:
                    raise Skip('the check suggests asmpatch: no asmpatch (tools/asmpatch build, asmpatch in the local config)')
                args += ['--asmpatch', ctx.asmpatch if asm in ('auto', True) else f"{ctx.asmpatch} --only={','.join(asm)}"]
            elif sam and sam != 'obb' and ctx.asmpatch:
                args += ['--asmpatch', f'{ctx.asmpatch} --only=']
        else:
            args += ['--il2cpp', il2cpp]
        etc = mono_etc(self.data)
        if etc and not os.path.isdir(os.path.join(self.data, 'MonoBleedingEdge', 'etc')) and \
                not os.path.isdir(os.path.join(self.data, 'Mono', 'etc')):
            args += ['--mono-etc', etc]
        if (rep or {}).get('build') == 'windows' and il2cpp is None:
            args += self.winbcl(view, pkg)
        args += [str(x) for x in self.g.get('android_data', ())]
        with self.step(f'android-data --pack {pack}'):
            alog = os.path.join(self.work, 'pack.log')
            rc = pmunity(ctx, args, alog, timeout=7200)
            shutil.copyfile(alog, os.path.join(self.out, 'android-data.log'))
            if rc:
                raise Fail(f'android-data failed (exit {rc}): {tail(alog)}')
            try:
                report = json.load(open(os.path.join(out, 'android_data_report.json')))
            except (OSError, ValueError):
                raise Fail('android-data wrote no report')
            p = K.pack_summary(report, read_lines(alog))
            dj = os.path.join(self.work, 'tree.json')
            if digest(ctx, ['tree', out, dj, '--state', os.path.join(out, '.android_data_state.json'),
                            'android_data_report.json', 'make_android_data.log', '*.log'], self.log):
                raise Fail('digest of the android-data output failed')
            t = json.load(open(dj))
            p['tree'] = {'total': t['total'], 'files': t['count'], 'mb': t['bytes'] >> 20,
                         'zips': {k: {'members': z['members'][:16], 'entries': z['entries']} for k, z in sorted(t['zips'].items())}}
            self.values['pack'] = p
        return out

    def winbcl(self, data, pkg):
        """A Windows build's Unix class libraries (pmunity winbcl: range reads from Unity's
        installer), cached in <scratch>/cache/winbcl/<slug> once read."""
        ctx = self.ctx
        cache = os.path.join(ctx.scratch, 'cache', 'winbcl', self.slug)
        opts = os.path.join(cache, 'options.txt')
        if not os.path.isfile(opts):
            if ctx.a.offline:
                raise Skip('Windows build: winbcl not cached (--offline)')
            with self.step('winbcl'):
                os.makedirs(cache, exist_ok=True)
                o = subprocess.run([ctx.py, '-m', 'pmunity.winbcl', data, os.path.join(cache, 'bcl'), '--player', pkg],
                                   cwd=ctx.convert_dir, env=ctx.env, capture_output=True, text=True, timeout=1800)
                with open(self.log, 'a') as f:
                    f.write(o.stderr[-4000:])
                if o.returncode:
                    raise Fail(f'winbcl failed: {o.stderr.strip()[-300:]}')
                with open(opts, 'w') as f:
                    f.write(o.stdout.strip())
        return open(opts).read().split()

    # ---- rig
    def emu_root(self, pkg, backend='mono', abi='armeabi-v7a', runtime=None, il2cpp_lib=None):
        """The emulated device root: lib/ with the player (and only the Mono runtime the data
        needs, as an Android build ships one), data/ for saves."""
        root = os.path.join(self.work, 'emu')
        rmtree(root)
        lib = os.path.join(root, 'lib')
        os.makedirs(lib)
        for f in glob.glob(os.path.join(pkg, LIBS.format(backend=backend, abi=abi), '*.so')):
            shutil.copy2(f, lib)
        if backend == 'mono':
            other = 'libmono.so' if 'bdwgc' in (runtime or '') else 'libmonobdwgc-2.0.so'
            for f in glob.glob(os.path.join(pkg, MONOLIBS, '*.so')):
                if os.path.basename(f) != other:
                    shutil.copy2(f, lib)
        if il2cpp_lib:
            shutil.copy2(il2cpp_lib, lib)
        return root

    def rig_run(self, root, zipdir, arch, env_extra):
        """tools/emu/emu.sh with screenshots at the list's times, counted from ua_host's first log
        line (a cold start from a slow drive can take minutes before it)."""
        ctx, r = self.ctx, self.g.get('rig') or {}
        secs = int(r.get('seconds', 300))
        shots = sorted(int(t) for t in r.get('shots', [secs // 4, secs // 2, secs * 9 // 10]))
        hostbin = ctx.hostbin64 if arch == 'arm64' else ctx.hostbin
        if not os.path.isfile(os.path.join(hostbin, 'ua_host')):
            raise Skip(f'no ua_host in {hostbin} (build the runtime, or hostbin in the local config)')
        env = dict(ctx.env)
        env.update({'EMU_ROOT': root, 'EMU_ZIP': zipdir, 'EMU_ARCH': arch, 'EMU_HOSTBIN': hostbin,
                    'EMU_NAME': f'regress-{self.id}'})
        if self.g.get('input'):
            env['UA_INPUT_SCRIPT'] = self.g['input']
        env.update({k: str(v) for k, v in env_extra.items()})
        prefixes = sorted({k.split('_', 1)[0] + '_' for k in env_extra if not k.startswith(('UA_', 'GC_', 'ANDROID_'))})
        if prefixes:
            env['EMU_ENV'] = ' '.join(prefixes)
        if os.path.realpath(root) == os.path.realpath(zipdir):
            raise SetupError('EMU_ROOT and EMU_ZIP are the same folder')
        runlog = os.path.join(root, 'run.log')
        docker_gate(ctx, self.log)
        others = [n for n in docker_names() if not n.startswith('regress')]
        if others:      # (frames and timing depend on what else runs: noted with the result)
            self.notes.append(f'{len(others)} other container{"s" if len(others) > 1 else ""} running at the start')
        with self.step(f'rig {arch} {secs} s'):
            with open(self.log, 'a') as f:
                f.write('env: ' + ' '.join(f'{k}={env[k]}' for k in sorted(env) if k.startswith(('UA_', 'EMU_')) or k in env_extra) + '\n')
            p = subprocess.Popen(['bash', os.path.join(ctx.checkout, 'tools', 'emu', 'emu.sh'), 'run.log', str(secs), 'release'],
                                 cwd=ctx.checkout, env=env, stdout=open(os.path.join(self.work, 'emu.out'), 'w'),
                                 stderr=subprocess.STDOUT)
            t0 = None
            deadline = time.time() + secs + 1800
            while p.poll() is None and time.time() < deadline:
                if os.path.isfile(runlog) and os.path.getsize(runlog):
                    t0 = time.time()
                    break
                time.sleep(2)
            taken = []
            req = os.path.join(root, 'shot.req')
            for t in shots:
                while p.poll() is None and t0 and time.time() - t0 < t:
                    time.sleep(1)
                if p.poll() is not None:
                    break
                open(req, 'w').close()
                taken.append(t)
                # ua_host takes it with the next frame (and removes the request): a slow frame
                # delays the shot, never merges it with the next one
                while p.poll() is None and os.path.exists(req) and time.time() - t0 < secs:
                    time.sleep(1)
            try:
                p.wait(timeout=max(60, secs + 600 - (time.time() - (t0 or time.time()))))
            except subprocess.TimeoutExpired:
                subprocess.run(['docker', 'kill', f'regress-{self.id}'], capture_output=True)
                p.wait()
                self.notes.append('the rig did not stop by itself: killed')
        lines = read_lines(runlog)
        shutil.copyfile(runlog, os.path.join(self.out, 'emu-run.log')) if os.path.isfile(runlog) else None
        runv = K.run_log(lines)
        files = sorted(glob.glob(os.path.join(root, 'shot-*.ppm')), key=lambda f: int(re.findall(r'(\d+)\.ppm$', f)[0]))
        stats = []
        for t, f in zip(taken, files):
            try:
                w, h, rgb = K.read_ppm(f)
                s = K.picture_stats(w, h, rgb)
            except (OSError, ValueError, IndexError):
                s = {'content': False, 'dark': 1.0, 'colours': 0, 'top': 1.0}
            s['t'] = t
            stats.append(s)
        for t in taken[len(files):]:
            stats.append({'t': t, 'content': False, 'dark': 1.0, 'colours': 0, 'top': 1.0, 'missing': True})
        self.keep_shots(files)
        self.values['run'] = {k: runv[k] for k in ('frames', 'crash', 'exit', 'fatal', 'exceptions', 'fps_lines')}
        self.values['shots'] = stats
        exp = dict(r)
        self.checks += K.expectations_rig(exp, runv, stats, '\n'.join(lines))
        return runv, stats

    def keep_shots(self, files):
        """The shots as small PNGs in the results folder (for a look; never in the repository)."""
        if not files:
            return
        code = ('import sys\nfrom PIL import Image\nfor src, dst in zip(sys.argv[1::2], sys.argv[2::2]):\n'
                '    im = Image.open(src); im.thumbnail((360, 360)); im.save(dst)\n')
        args = []
        for f in files:
            args += [f, os.path.join(self.out, os.path.basename(f).replace('.ppm', '.png'))]
        subprocess.run([self.ctx.py, '-I', '-c', code] + args, capture_output=True)


def hide_name(rel):
    """A data file's name as the baseline keeps it: the converter's generic Unity names as they
    are (level3, sharedassets0.assets, Resources/unity_builtin_extra), anything else (a game's own
    bundle names) as a short hash."""
    import hashlib
    if re.fullmatch(r'(Resources/)?(level\d+|sharedassets\d+\.assets|resources\.assets|globalgamemanagers(\.assets)?|'
                    r'unity_builtin_extra|unity default resources)', rel):
        return rel
    return 'f-' + hashlib.sha256(rel.encode()).hexdigest()[:12]


# --------------------------------------------------------------------------- per route

def fast_android(G, pack_override):
    rep = G.check()
    src = G.source()
    cfg = G.convert_config(rep)
    pack = pack_override or G.g.get('pack', 'none')
    conv = G.shaders(src, cfg, real=pack != 'none')
    G.settings(src, conv, cfg)
    if pack != 'none':
        G.android_data(src, conv, rep, pack)


def fast_box64(G, pack_override):
    G.check()
    src = G.source()
    cfg = G.convert_config(None)
    conv = G.shaders(src, cfg, real=False)
    G.settings(src, conv, cfg)


def fast_shaders(G, pack_override):
    src = G.source()
    cfg = G.convert_config(None)
    conv = G.shaders(src, cfg, real=False)
    G.settings(src, conv, cfg)


def apk_inputs(G):
    ins = sorted(f for f in glob.glob(os.path.join(G.root, '*'))
                 if f.endswith(('.apk', '.obb', '.xapk', '.apks', '.aab')))
    if not ins:
        raise Skip('no APK in the game folder')
    return ins


def fast_apk(G, pack_override):
    ctx = G.ctx
    ins = apk_inputs(G)
    a = G.g.get('apk') or {}
    out = os.path.join(G.work, 'zip')
    with G.step('pmunity apk'):
        j = os.path.join(G.out, 'apk.log')
        rc, rep = run_json([ctx.py, '-m', 'pmunity', 'apk'] + ins + [out, '--json', '--target', a.get('target', 'auto')]
                           + [str(x) for x in a.get('args', ())], j, cwd=ctx.convert_dir, env=ctx.env, timeout=3600)
        if rc:
            raise Fail(f'pmunity apk failed (exit {rc}): {tail(j)}')
        rep = rep or {}
        dj = os.path.join(G.work, 'tree.json')
        if digest(ctx, ['tree', out, dj, '*.log'], G.log):
            raise Fail('digest of the apk output failed')
        t = json.load(open(dj))
        G.values['apk'] = {
            'facts': {k: rep[k] for k in APK_FACTS if k in rep},
            'tree': {'total': t['total'], 'files': t['count'], 'mb': t['bytes'] >> 20,
                     'zips': {hide_name(k): {'members': z['members'][:16], 'entries': z['entries']} for k, z in sorted(t['zips'].items())}}}
    return out, rep


def il2cpp_args(G, rep):
    i = G.g.get('il2cpp') or {}
    args = ['--abi', i.get('abi', 'arm64-v8a')]
    sug = ((rep or {}).get('suggest') or {}).get('android_data') or {}
    asm = G.g.get('asmpatch', sug.get('asmpatch', 'off'))
    sam = G.g.get('streaming_assets', sug.get('streaming_assets'))
    if asm not in (None, 'off', False) and G.ctx.asmpatch:
        args += ['--asmpatch', G.ctx.asmpatch if asm in ('auto', True) else f"{G.ctx.asmpatch} --only={','.join(asm)}"]
    elif sam and sam != 'obb' and G.ctx.asmpatch:
        args += ['--asmpatch', f'{G.ctx.asmpatch} --only=']     # (the streaming-assets patch alone, as android-data)
    if sam and sam != 'obb':
        if not G.ctx.asmpatch:
            raise Skip(f'streaming_assets {sam} on the IL2CPP route needs asmpatch (none here)')
        args += ['--streaming-assets', sam]
    return args + [str(x) for x in i.get('args', ())]


def fast_il2cpp(G, pack_override):
    """pmunity il2cpp --dry-run: the tools fetched, the assemblies staged, the commands it would run
    (machine paths replaced) - what changes when the IL2CPP route's staging or options change."""
    ctx = G.ctx
    if not ctx.il2cpp_home:
        raise Skip('no il2cpp_home (PMUNITY_IL2CPP_HOME) in the local config')
    rep = G.check()
    src = G.source()
    out = os.path.join(G.work, 'il2cpp')
    with G.step('il2cpp --dry-run'):
        dlog = os.path.join(G.work, 'dry.log')
        rc = pmunity(ctx, ['il2cpp', src, out, '--dry-run'] + il2cpp_args(G, rep), dlog, timeout=3600)
        shutil.copyfile(dlog, os.path.join(G.out, 'il2cpp-dry-run.log'))
        if rc:
            raise Fail(f'il2cpp --dry-run failed (exit {rc}): {tail(dlog)}')
        text = open(dlog, errors='replace').read()
        text = text.replace(G.work, '<work>')
        for p, name in ctx.paths_to_hide():
            text = text.replace(p, name)
        # what it decided and would run; not what it fetched first (a first run fetches the tools)
        cmds = [l for l in text.splitlines()[1:] if DRY_RUN_LINE.match(l)]
        open(os.path.join(G.out, 'il2cpp-dry-run.log'), 'w').write(text)
        import hashlib
        staged = os.path.join(out, '.work', 'Managed-in')
        dj = os.path.join(G.work, 'staged.json')
        st = {'total': None, 'count': 0}
        if os.path.isdir(staged) and digest(ctx, ['tree', staged, dj], G.log) == 0:
            st = json.load(open(dj))
        head = cmds[0] if cmds else ''
        G.values['il2cpp_dry_run'] = {
            'summary': re.sub(r'^data: ', '', head)[:200],
            'assemblies': next((l for l in cmds if l.startswith('assemblies:')), None),
            'commands': hashlib.sha256('\n'.join(cmds).encode()).hexdigest()[:16],
            'lines': len(cmds),
            'staged': {'total': (st.get('total') or '')[:16], 'files': st.get('count', 0)}}


# --------------------------------------------------------------------------- rig per route

def rig_android(G):
    rep = G.check()
    src = G.source()
    cfg = G.convert_config(rep)
    conv = G.shaders(src, cfg, real=True)
    G.settings(src, conv, cfg)
    pkg = player_pkg(G.ctx, G.version, G.log)
    out = G.android_data(src, conv, rep, 'all', pkg=pkg)
    rmtree(os.path.join(out, 'Game'))          # the zip folder has it all now
    rmtree(os.path.join(G.work, 'view'))
    runtime = (G.values.get('pack') or {}).get('scripting_runtime') or ''
    root = G.emu_root(pkg, runtime=runtime)
    env = dict(((rep or {}).get('suggest') or {}).get('runtime') or {}) if G.g.get('suggested_env', True) else {}
    env.update(G.g.get('env') or {})
    G.rig_run(root, os.path.join(out, 'zip'), 'armhf', env)


def rig_apk(G):
    a = G.g.get('apk') or {}
    arch = (G.g.get('rig') or {}).get('arch', 'arm64' if a.get('target', 'arm64') != 'armhf' else 'armhf')
    zipdir, rep = fast_apk(G, None)
    root = os.path.join(G.work, 'emu')
    rmtree(root)
    os.makedirs(root)
    G.rig_run(root, zipdir, arch, dict(G.g.get('env') or {}))


def rig_il2cpp(G):
    """The whole IL2CPP route: pmunity il2cpp (Docker; minutes to an hour), convert, android-data
    --il2cpp, the arm64 rig."""
    ctx = G.ctx
    if not ctx.il2cpp_home:
        raise Skip('no il2cpp_home (PMUNITY_IL2CPP_HOME) in the local config')
    rep = G.check()
    src = G.source()
    abi = (G.g.get('il2cpp') or {}).get('abi', 'arm64-v8a')
    out = os.path.join(G.work, 'il2cpp')
    docker_gate(ctx, G.log)
    with G.step('il2cpp build'):
        blog = os.path.join(G.work, 'il2cpp.log')
        rc = pmunity(ctx, ['il2cpp', src, out] + il2cpp_args(G, rep), blog, timeout=3 * 3600,
                     env={'PMUNITY_IL2CPP_DOCKER_NAME': f'regress-il2cpp-{G.id}'})
        shutil.copyfile(blog, os.path.join(G.out, 'il2cpp.log'))
        if rc:
            raise Fail(f'pmunity il2cpp failed (exit {rc}): {tail(blog)}')
        lib = os.path.join(out, 'lib', abi, 'libil2cpp.so')
        if not os.path.isfile(lib):
            raise Fail('pmunity il2cpp wrote no libil2cpp.so')
        G.values['il2cpp'] = {'libil2cpp_mb': round(os.path.getsize(lib) / (1 << 20), 1),
                              'metadata': next((m[1] for l in read_lines(blog) for m in [re.search(r'metadata version (\d+)', l)] if m), None)}
    cfg = G.convert_config(rep)
    conv = G.shaders(src, cfg, real=True)
    G.settings(src, conv, cfg)
    ver = G.version
    pkg = os.path.join(ctx.il2cpp_home, ver, f'player-{abi}')
    if not os.path.isfile(os.path.join(pkg, 'player.json')):
        raise Skip(f'no IL2CPP player in {pkg}')
    adir = G.android_data(src, conv, rep, 'all', il2cpp=out, pkg=pkg)
    rmtree(os.path.join(adir, 'Game'))
    rmtree(os.path.join(G.work, 'view'))
    root = G.emu_root(pkg, backend='il2cpp', abi=abi, il2cpp_lib=lib)
    env = dict(((rep or {}).get('suggest') or {}).get('runtime') or {}) if G.g.get('suggested_env', True) else {}
    env.update(G.g.get('env') or {})
    G.rig_run(root, os.path.join(adir, 'zip'), 'arm64' if abi == 'arm64-v8a' else 'armhf', env)


def rig_prebuilt(G):
    """A game packaged before (its zip folder and lib/ from the local config's [prebuilt.<slug>]):
    the runtime under test on the same data."""
    pb = G.ctx.prebuilt.get(G.slug)
    if not pb:
        raise Skip(f'no [prebuilt.{G.slug}] in the local config')
    zipdir, lib = pb.get('zip'), pb.get('lib')
    if not (zipdir and os.path.isdir(zipdir) and lib and os.path.isdir(lib)):
        raise Skip(f'[prebuilt.{G.slug}]: zip or lib folder missing')
    root = os.path.join(G.work, 'emu')
    rmtree(root)
    shutil.copytree(lib, os.path.join(root, 'lib'))
    for f in pb.get('root_files', ()):
        shutil.copy2(f, root)
    G.rig_run(root, zipdir, pb.get('arch', 'armhf'), dict(G.g.get('env') or {}))


def rig_box64(G):
    b = G.ctx.local.get('box64_bin')
    if not b or not os.path.isfile(os.path.join(b, 'box64', 'box64')):
        raise Skip('no box64 build (tools/box64/build.sh; box64_bin in the local config)')
    raise Skip('box64 rig runs are not wired into tools/regress yet (tools/box64/rig.sh)')


FAST = {'android': fast_android, 'box64': fast_box64, 'shaders': fast_shaders, 'apk': fast_apk,
        'il2cpp': fast_il2cpp}
RIG = {'android': rig_android, 'apk': rig_apk, 'il2cpp': rig_il2cpp, 'prebuilt': rig_prebuilt, 'box64': rig_box64}


# --------------------------------------------------------------------------- the run

def baseline_path(level):
    return os.path.join(HERE, 'baseline', f'{level}.json')


def load_baseline(level):
    try:
        with open(baseline_path(level)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_baseline(level, data):
    os.makedirs(os.path.dirname(baseline_path(level)), exist_ok=True)
    with open(baseline_path(level), 'w') as f:
        json.dump(dict(sorted(data.items())), f, indent=1, sort_keys=True)
        f.write('\n')


def compare(G, golden):
    """The run's values against the baseline: the fast level exactly (counts and digests of the
    tools' output), the rig with tolerance (checks.compare_rig)."""
    if not golden:
        G.notes.append('no baseline yet')
        return
    if G.level == 'fast':
        diffs = K.compare_exact(golden, G.values)
        if diffs:
            shown = []
            for k, a, b in diffs[:6]:
                if k.endswith('per_file'):
                    c, add, rem, names = K.changed_files(a, b)
                    real = [getattr(G, '_names', {}).get(n, n) for n in names]
                    shown.append(f'{k}: {c} changed, {add} new, {rem} gone ({", ".join(real[:4])})')
                else:
                    shown.append(f'{k}: {short(a)} -> {short(b)}')
            G.checks.append(('baseline', False, f'{len(diffs)} values differ from the baseline: ' + '; '.join(shown)))
        else:
            G.checks.append(('baseline', True, 'same as the baseline'))
    else:
        G.checks += K.compare_rig(golden, G.values.get('run') or {}, G.values.get('shots') or [])
        for part in ('pack', 'shaders_digest', 'apk'):
            if part in golden and part in G.values:
                d = K.compare_exact(golden[part], G.values[part])
                G.checks.append((f'baseline:{part}', not d, 'same as the baseline' if not d else
                                 f'{len(d)} values differ: ' + '; '.join(f'{k}: {short(a)} -> {short(b)}' for k, a, b in d[:4])))


def short(v):
    s = json.dumps(v) if not isinstance(v, str) else v
    return s if len(s) <= 40 else s[:37] + '...'


def run_game(ctx, g, level, golden):
    G = Game(ctx, g, level)
    rmtree(G.work)
    os.makedirs(G.work)
    os.makedirs(G.out, exist_ok=True)
    t0 = time.time()
    status, reason = 'pass', ''
    try:
        if level not in g['levels']:
            raise Skip(f'not in the {level} level')
        fn = (FAST if level == 'fast' else RIG).get(g['route'])
        if fn is None:
            raise Skip(f"route {g['route']} has no {level} level")
        G.locate()
        if level == 'fast':
            fn(G, ctx.a.pack)
            G.checks += K.expectations_fast(g.get('expect') or {}, G.values)
        else:
            fn(G)
        compare(G, golden)
        if ctx.a.update:            # the baseline is being recorded: its differences are news, not faults
            for c in [c for c in G.checks if c[0].startswith('baseline') and not c[1]]:
                G.checks.remove(c)
                G.notes.append('baseline updated: ' + c[2])
        bad = [c for c in G.checks if not c[1]]
        if bad:
            status, reason = 'fail', '; '.join(c[2] for c in bad)
    except Skip as ex:
        status, reason = 'skip', str(ex)
    except Fail as ex:
        status, reason = 'fail', str(ex)
    except Exception as ex:                 # a fault of this tool: the run goes on with the next game
        import traceback
        with open(G.log, 'a') as f:
            traceback.print_exc(file=f)
        status, reason = 'fail', f'regress.py: {type(ex).__name__}: {ex}'
    finally:
        if not ctx.a.keep:
            rmtree(G.work)
    secs = round(time.time() - t0)
    res = {'slug': g['id'], 'route': g['route'], 'level': level, 'covers': g.get('covers', []), 'status': status,
           'reason': reason, 'notes': G.notes, 'seconds': secs, 'timing': G.timing, 'cpu': G.cpu, 'values': G.values,
           'checks': [list(c) for c in G.checks]}
    with open(os.path.join(G.out, 'result.json'), 'w') as f:
        json.dump(res, f, indent=1)
    return res


def table(results):
    rows = ['| game | route | covers | status | time | reason, notes |', '|---|---|---|---|---|---|']
    for r in results:
        why = r['reason'] or ', '.join(r['notes']) or ''
        if r['reason'] and r['notes']:
            why += ' (' + ', '.join(r['notes']) + ')'
        rows.append(f"| {r['slug']} | {r['route']} | {', '.join(r['covers'])} | {r['status']} | "
                    f"{fmt_secs(r['seconds'])} | {why.replace('|', '/')} |")
    n = {s: sum(r['status'] == s for r in results) for s in ('pass', 'fail', 'skip')}
    total = sum(r['seconds'] for r in results)
    cpu = round(sum(sum((r.get('cpu') or {}).values()) for r in results))
    rows.append('')
    rows.append(f"{n['pass']} pass, {n['fail']} fail, {n['skip']} skip; {fmt_secs(total)} in all "
                f"(the tools' CPU time {fmt_secs(cpu)})")
    return '\n'.join(rows)


def fmt_secs(s):
    return f'{s // 60}:{s % 60:02d}' if s >= 60 else f'{s} s'


def selected(games, only):
    if not only:
        return games
    rx = re.compile(only)
    return [g for g in games if rx.search(g['id']) or rx.search(g['slug']) or any(rx.search(c) for c in g.get('covers', ()))
            or rx.search(g['route'])]


def cmd_list(a, games):
    print('| game | route | Unity | covers | levels | fast checks | rig checks |')
    print('|---|---|---|---|---|---|---|')
    for g in selected(games, a.only):
        fast = {'android': 'check, shaders, settings' + (f", android-data ({g.get('pack')})" if g.get('pack', 'none') != 'none' else ''),
                'box64': 'route, shaders (GLES mode), settings', 'shaders': 'shaders, settings',
                'apk': 'pmunity apk', 'il2cpp': 'check, il2cpp --dry-run', 'prebuilt': '-'}[g['route']]
        e = g.get('expect') or {}
        if e:
            fast += '; ' + ', '.join(f'{k} {v}' for k, v in e.items())
        rig = '-'
        if 'rig' in g['levels']:
            r = g.get('rig') or {}
            parts = [f"{r.get('seconds', 300)} s", 'shots ' + '/'.join(map(str, r.get('shots', [])))]
            if r.get('content'):
                parts.append('picture at ' + '/'.join(map(str, r['content'])))
            if r.get('frames_min'):
                parts.append(f"frames >= {r['frames_min']}")
            parts.append('no crash')
            parts += [f'log /{x}/' for x in r.get('log', ())]
            if g.get('input'):
                parts.append('input script')
            rig = ', '.join(parts)
        print(f"| {g['id']} | {g['route']} | {g.get('unity', '')} | {', '.join(g.get('covers', []))} | "
              f"{', '.join(g['levels'])} | {fast} | {rig} |")


def cmd_bisect(a, local):
    """git bisect over a worktree of this repository (in the scratch folder): at each commit this
    regress.py, its list and baseline judge the worktree's tools."""
    if len(a.rest) != 2 or not a.only:
        raise SetupError('bisect GOOD BAD --level fast|rig --only REGEX [--build CMD]')
    good, bad = a.rest
    scratch = local.get('scratch')
    if not scratch:
        raise SetupError('no scratch folder in the local config')
    repo = subprocess.run(['git', '-C', HERE, 'rev-parse', '--show-toplevel'], capture_output=True, text=True).stdout.strip()
    wt = os.path.join(os.path.abspath(scratch), 'bisect-worktree')
    if os.path.exists(wt):
        subprocess.run(['git', '-C', repo, 'worktree', 'remove', '--force', wt], capture_output=True)
        rmtree(wt)
    subprocess.run(['git', '-C', repo, 'worktree', 'add', '--detach', wt, bad], check=True)
    try:
        subprocess.run(['git', '-C', wt, 'bisect', 'start', bad, good], check=True)
        cmd = [sys.executable, os.path.abspath(__file__), a.bisect_level, '--only', a.only, '--checkout', wt,
               '--bisect-run', '--list', a.list] + (['--config', a.config] if a.config else []) + \
            (['--offline'] if a.offline else []) + (['--pack', a.pack] if a.pack else [])
        if a.build:
            cmd = ['sh', '-c', f'cd {shlex.quote(wt)} && ({a.build}) >/dev/null 2>&1 || exit 125; exec '
                   + ' '.join(shlex.quote(c) for c in cmd)]
        subprocess.run(['git', '-C', wt, 'bisect', 'run'] + cmd)
        log = subprocess.run(['git', '-C', wt, 'bisect', 'log'], capture_output=True, text=True).stdout
        print(log)
        first = re.findall(r'^# first bad commit: \[(\w+)\] (.*)$', log, re.M)
        if first:
            print(f'first bad commit: {first[0][0][:12]} {first[0][1]}')
    finally:
        subprocess.run(['git', '-C', wt, 'bisect', 'reset'], capture_output=True)
        subprocess.run(['git', '-C', repo, 'worktree', 'remove', '--force', wt], capture_output=True)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog='regress.py', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('level', choices=LEVELS + ('list', 'bisect'))
    ap.add_argument('rest', nargs='*', help='bisect: GOOD BAD (commits)')
    ap.add_argument('--level', dest='bisect_level', choices=LEVELS, default='fast', help='bisect: the level to run')
    ap.add_argument('--build', help='bisect: a command run in the worktree before each test (e.g. runtime/build.sh)')
    ap.add_argument('--bisect-run', action='store_true', help=argparse.SUPPRESS)
    ap.add_argument('--only', help='regex over slugs, covers tags and routes')
    ap.add_argument('--list', default=os.path.join(HERE, 'games.toml'), help='the list file (default games.toml)')
    ap.add_argument('--config', help='the local config (default tools/regress/local.toml, $REGRESS_CONFIG)')
    ap.add_argument('--checkout', help='the portmaster-unity checkout under test (default: this one)')
    ap.add_argument('--update', action='store_true', help="record this run's values as the baseline of the games it ran")
    ap.add_argument('--keep', action='store_true', help="keep each game's work folder (converted data, APK) in the scratch folder")
    ap.add_argument('--pack', choices=PACK, help='fast level: android-data for every android game this way (default: the list\'s pack)')
    ap.add_argument('--offline', action='store_true', help='fetch nothing (players, winbcl): skip what is not here')
    a = ap.parse_args(argv)
    try:
        defaults, games = load_list(a.list)
        if a.level == 'list':
            cmd_list(a, games)
            return 0
        local = load_local(a.config)
        if a.level == 'bisect':
            return cmd_bisect(a, local)
        ctx = Ctx(a, local)
    except SetupError as ex:
        print(f'regress.py: {ex}', file=sys.stderr)
        return 125 if a.bisect_run else 2
    games = [g for g in selected(games, a.only) if a.level in g['levels']]
    if not games:
        print('regress.py: no game of the list for this level and --only', file=sys.stderr)
        return 125 if a.bisect_run else 2
    os.makedirs(ctx.results, exist_ok=True)
    base = load_baseline(a.level)
    head = subprocess.run(['git', '-C', ctx.checkout, 'log', '-1', '--format=%h %s'], capture_output=True, text=True).stdout.strip()
    print(f'regress {a.level}: {len(games)} games, checkout {ctx.checkout} ({head}); results {ctx.results}', flush=True)
    results = []
    t0 = time.time()
    for i, g in enumerate(games, 1):
        print(f"[{i}/{len(games)}] {g['id']} ({g['route']})", flush=True)
        r = run_game(ctx, g, a.level, base.get(g['id']))
        print(f"    -> {r['status']} {fmt_secs(r['seconds'])} {r['reason'][:300]}", flush=True)
        results.append(r)
        if a.update and r['status'] != 'skip' and r['values']:
            base[g['id']] = r['values']
            save_baseline(a.level, base)
    out = table(results)
    summary = {'level': a.level, 'checkout': head, 'run': ctx.run_id, 'seconds': round(time.time() - t0),
               'results': results}
    with open(os.path.join(ctx.results, 'results.json'), 'w') as f:
        json.dump(summary, f, indent=1)
    with open(os.path.join(ctx.results, 'results.md'), 'w') as f:
        f.write(f'# regress {a.level}, {head}, {ctx.run_id}\n\n{out}\n')
    print()
    print(out)
    if a.update:
        print(f'baseline: {baseline_path(a.level)} updated for {sum(1 for r in results if r["status"] != "skip" and r["values"])} games')
    if a.bisect_run and all(r['status'] == 'skip' for r in results):
        return 125                      # (git bisect: this commit cannot be judged)
    return 1 if any(r['status'] == 'fail' for r in results) else 0


if __name__ == '__main__':
    sys.exit(main())
