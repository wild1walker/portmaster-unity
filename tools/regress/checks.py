"""The regression run's checks: what the tools' logs and outputs say, and the comparisons with the
list's expectations and the recorded baseline. Pure functions over text, JSON and pictures (no
game data is kept: counts, sizes and hashes only); tested by test_regress.py on synthetic input."""
import re

# --------------------------------------------------------------------------- converter log

_SHADERS = re.compile(r'shaders: (\d+) shaders given GLES3 programs \((\d+) programs, (\d+) subprograms\) in (\d+) '
                      r'files; (\d+) not translatable; precision (\S+); rewrites: (\d+) stages')
_ES31 = re.compile(r'shaders: (\d+) programs need GLSL ES 3\.10')
_GL4X = re.compile(r'shaders: GLCore41/43 programs translated too: (\d+) programs; (\d+) SubShaders left out '
                   r'\(no ES equivalent\); (\d+) shaders without programs')
_VR = re.compile(r'shaders: (\d+) VR-only OpenGL Core variants not translatable')
_COMPUTE = re.compile(r'shaders: (\d+) of (\d+) compute shaders given GLES3 kernels \((\d+) kernels, (\d+) kernel '
                      r'variants.*?\); (\d+) kernels left out; (\d+) without OpenGL Core kernels')
_FROM = re.compile(r'shaders: from (Metal|Vulkan|Direct3D 11): (\d+) of (\d+) shaders \((\d+) programs\); (\d+) '
                   r'SubShaders left out; (\d+) VR-only(?: variants)?(?: and|,) (\d+) GPU instancing(?: variants left out| and (\d+) '
                   r'GPU sprite skinning)?')
_FROM_KEY = {'Metal': 'metal', 'Vulkan': 'vulkan', 'Direct3D 11': 'd3d11'}


def shader_counts(lines):
    """The shaders step's summary (its log lines) as counts: {'shaders': .., 'programs': .., ...}.
    Per-shader lines (which name the game's shaders) are not looked at."""
    c = {}
    for line in lines:
        m = _SHADERS.search(line)
        if m:
            c.update(shaders=int(m[1]), programs=int(m[2]), subprograms=int(m[3]), files=int(m[4]),
                     not_translatable=int(m[5]), precision=m[6], rewrites=int(m[7]))
            continue
        m = _ES31.search(line)
        if m:
            c['es31_programs'] = int(m[1])
            continue
        m = _GL4X.search(line)
        if m:
            c.update(gl4x_programs=int(m[1]), gl4x_subshaders_left_out=int(m[2]), gl4x_empty=int(m[3]))
            continue
        if 'GLCore41/43 programs not translated (glcore4x off)' in line:
            c['gl4x'] = 'off'
            continue
        m = _VR.search(line)
        if m:
            c['vr_left_out'] = int(m[1])
            continue
        m = _COMPUTE.search(line)
        if m:
            c.update(compute=int(m[1]), compute_total=int(m[2]), kernels=int(m[3]), kernel_variants=int(m[4]),
                     kernels_left_out=int(m[5]), compute_without_glcore=int(m[6]))
            continue
        m = _FROM.search(line)
        if m:
            k = _FROM_KEY[m[1]]
            c[k] = {'converted': int(m[2]), 'total': int(m[3]), 'programs': int(m[4]),
                    'subshaders_left_out': int(m[5]), 'vr': int(m[6]), 'instancing': int(m[7])}
            if m[8] is not None:
                c[k]['skinning'] = int(m[8])
    return c


# --------------------------------------------------------------------------- port check / route

def check_verdict(rep):
    """What `pmunity port check --json` decided, without anything that names or describes the
    game: route, build kind, versions, suggestions, how many warnings and notes."""
    s = rep.get('suggest') or {}
    return {
        'route': rep.get('route'),
        'routes_ok': sorted(r.get('route') for r in rep.get('routes') or () if r.get('ok')),
        'build': rep.get('build'),
        'backend': rep.get('backend'),
        'compressed': rep.get('compressed'),
        'unity_version': rep.get('unity_version'),
        'unity_supported': rep.get('unity_supported'),
        'scripting_runtime': rep.get('scripting_runtime'),
        'shader_source': rep.get('shader_source'),
        'suggest': {'runtime': s.get('runtime') or {}, 'android_data': s.get('android_data') or {},
                    'convert': s.get('convert') or {}},
        'warnings': len(rep.get('warnings') or ()),
        'notes': len(rep.get('notes') or ()),
    }


def route_verdict(rep):
    """`pmunity route --json` reduced the same way."""
    out = {'unity': rep.get('unity'), 'platform': rep.get('platform'), 'backend': rep.get('backend'),
           'compressed': rep.get('compressed')}
    for k in ('android', 'box64'):
        r = rep.get(k) or {}
        out[k] = {'fits': r.get('fits'), 'blockers': len(r.get('blockers') or ()), 'caveats': len(r.get('caveats') or ())}
    for k in ('route', 'recommended', 'gl_mode', 'gl'):
        if k in rep and not isinstance(rep[k], (dict, list)):
            out[k] = rep[k]
    return out


# --------------------------------------------------------------------------- android-data

PLACEMENT = ('clone', 'copy', 'link', 'hardlink', 'symlink')


def placement_stats(stats):
    out = {k: v for k, v in stats.items() if k not in PLACEMENT}
    placed = sum(v for k, v in stats.items() if k in PLACEMENT and isinstance(v, int))
    if placed:
        out['placed'] = placed
    return dict(sorted(out.items()))


def pack_summary(report, log_lines):
    """android-data's report (android_data_report.json) and log: what it placed and checked."""
    v = report.get('verify') or {}
    u = report.get('unity') or {}
    s = {
        # how files were placed (a clone, a hard link or a copy) depends on the drives the game and
        # the scratch folder are on: only their total is compared, beside the other actions
        'stats': placement_stats(report.get('stats') or {}),
        'changed_files': len(report.get('files') or {}),
        'bundles': len(report.get('bundles') or {}),
        'verify_problems': len(v.get('problems') or ()),
        'warnings': len(u.get('warnings') or ()),
        'dropped_modules': len(report.get('dropped_modules') or ()),
        'player': u.get('player'),
        'scripting_runtime': u.get('scripting_runtime'),
        'plugins_map': len((report.get('plugins') or {}).get('map') or {}),
    }
    s['verify_ok'] = any(re.search(r'^verify: OK', l) for l in log_lines)
    s['pack_verified'] = sum(1 for l in log_lines if re.search(r'^pack: \S+ verified', l))
    return s


# --------------------------------------------------------------------------- the rig's run log

_FPS = re.compile(r'^\[fps\] ([\d.]+) fps, .*\((\d+) frames, (\d+) total, ([\d.]+) s\)')
_SHOT = re.compile(r'^\[frames\] screenshot \S*shot-(\d+)\.ppm')
_CRASH = re.compile(r'^\[crash\] signal (\d+)')
_FATAL = re.compile(r'FORTIFY|abort message|Fatal signal|Assertion .* failed|^\* Assertion|SIGSEGV')
_EXIT = re.compile(r'^exit (\d+)$')
_EXCEPTION = re.compile(r'\b\w*Exception\b(?::|$)')


def run_log(lines):
    """ua_host's log: frames, fps, crash, exit, shots taken, exceptions (counts only)."""
    r = {'frames': 0, 'fps_lines': 0, 'last_fps_at': None, 'fps_last': None, 'crash': None, 'fatal': 0,
         'exit': None, 'shots_logged': 0, 'exceptions': 0, 'unity_exit_frames': None, 'lines': len(lines),
         'stopped': False}
    for line in lines:
        if re.search(r'\] up: quit$', line):
            r['stopped'] = True         # the rig's time was up: ua_host was asked to stop (SIGINT)
        m = _FPS.search(line)
        if m:
            r['frames'] = max(r['frames'], int(m[3]))
            r['fps_lines'] += 1
            r['last_fps_at'] = float(m[4])
            r['fps_last'] = float(m[1])
            continue
        if _SHOT.search(line):
            r['shots_logged'] += 1
            continue
        m = _CRASH.search(line)
        if m and r['crash'] is None:
            r['crash'] = int(m[1])
            continue
        m = _EXIT.search(line.strip())
        if m:
            r['exit'] = int(m[1])
            continue
        m = re.search(r'UnityMain thread exits after (\d+) frames', line)
        if m:
            r['unity_exit_frames'] = int(m[1])
            r['frames'] = max(r['frames'], int(m[1]))
        if _FATAL.search(line):
            r['fatal'] += 1
        if _EXCEPTION.search(line):
            r['exceptions'] += 1
    return r


# --------------------------------------------------------------------------- pictures

def read_ppm(path):
    """(width, height, RGB bytes) of a binary PPM (P6, maxval 255) as ua_host writes them."""
    with open(path, 'rb') as f:
        data = f.read()
    tok, pos = [], 0
    while len(tok) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        tok.append(data[pos:end])
        pos = end
    if tok[0] != b'P6' or int(tok[3]) != 255:
        raise ValueError(f'{path}: not an 8-bit binary PPM')
    w, h = int(tok[1]), int(tok[2])
    pos += 1
    return w, h, data[pos:pos + w * h * 3]


def picture_stats(w, h, rgb, step=4):
    """Is there a picture: of every step-th pixel in both directions, how many are dark (all
    channels below 24), how many distinct colours (4 bits a channel), and the most common colour's
    share. content = neither (almost) all dark nor (almost) all one colour."""
    n = dark = 0
    counts = {}
    for y in range(0, h, step):
        row = y * w * 3
        for x in range(0, w, step):
            i = row + x * 3
            r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
            n += 1
            if r < 24 and g < 24 and b < 24:
                dark += 1
            k = (r >> 4, g >> 4, b >> 4)
            counts[k] = counts.get(k, 0) + 1
    if not n:
        return {'content': False, 'dark': 1.0, 'colours': 0, 'top': 1.0}
    top = max(counts.values()) / n
    dark_f = dark / n
    return {'content': dark_f < 0.995 and top < 0.995, 'dark': round(dark_f, 3), 'colours': len(counts),
            'top': round(top, 3)}


# --------------------------------------------------------------------------- comparisons

def flatten(d, prefix=''):
    out = {}
    for k, v in d.items():
        key = f'{prefix}{k}'
        if isinstance(v, dict) and v and k != 'per_file':
            out.update(flatten(v, key + '.'))
        else:
            out[key] = v
    return out


def compare_exact(golden, values, ignore=()):
    """Keys whose value differs from the baseline: [(key, baseline, now)]. Keys only in one of
    them are reported too (None for the side without it), except under ignore prefixes."""
    g, v = flatten(golden), flatten(values)
    diffs = []
    for k in sorted(set(g) | set(v)):
        if any(k == p or k.startswith(p + '.') for p in ignore):
            continue
        if g.get(k) != v.get(k):
            diffs.append((k, g.get(k), v.get(k)))
    return diffs


def changed_files(golden_files, files):
    """Which per-file digests differ: (changed, added, removed) counts and the changed names."""
    a, b = golden_files or {}, files or {}
    changed = sorted(k for k in set(a) & set(b) if a[k] != b[k])
    return len(changed), len(set(b) - set(a)), len(set(a) - set(b)), changed


def expectations_fast(exp, v):
    """The list's expectations for the fast level: [(check, ok, detail)]."""
    out = []
    sc = v.get('shaders') or {}
    if 'shaders_min' in exp:
        n = sc.get("shaders", 0)
        out.append(('shaders_min', n >= exp['shaders_min'], f"{n} shaders given GLES3 programs (at least {exp['shaders_min']})"))
    if 'not_translatable_max' in exp:
        n = sc.get('not_translatable', 0)
        out.append(('not_translatable_max', n <= exp['not_translatable_max'],
                    f"{n} not translatable (at most {exp['not_translatable_max']})"))
    if 'compute_min' in exp:
        n = sc.get('compute', 0)
        out.append(('compute_min', n >= exp['compute_min'], f"{n} compute shaders with GLES3 kernels (at least {exp['compute_min']})"))
    for src in ('d3d11', 'metal', 'vulkan'):
        k = f'{src}_min'
        if k in exp:
            n = (sc.get(src) or {}).get('converted', 0)
            out.append((k, n >= exp[k], f'{n} shaders from {src} (at least {exp[k]})'))
    if 'route' in exp:
        r = (v.get('check') or {}).get('route') or (v.get('route') or {}).get('route')
        out.append(('route', r == exp['route'], f"route {r} (expected {exp['route']})"))
    if 'android_fits' in exp:
        r = ((v.get('route') or {}).get('android') or {}).get('fits')
        out.append(('android_fits', r == exp['android_fits'], f'android route fits: {r}'))
    if 'box64_fits' in exp:
        r = ((v.get('route') or {}).get('box64') or {}).get('fits')
        out.append(('box64_fits', r == exp['box64_fits'], f'box64 route fits: {r}'))
    p = v.get('pack')
    if p is not None:
        mx = exp.get('verify_problems_max', 0)
        n = p.get('verify_problems') or 0
        ok = n <= mx and (p.get('verify_ok', False) or n > 0)
        out.append(('pack_verify', ok, f"android-data verify: {'OK' if p.get('verify_ok') else 'not OK'}, {n} problems"
                                       + (f' (at most {mx}: known)' if mx else '')))
    return out


def expectations_rig(exp, run, shots, log_text):
    """The list's expectations for the rig: [(check, ok, detail)]. shots: [{'t': s, 'content':..}]."""
    out = []
    if exp.get('no_crash', True):
        ok = run.get('crash') is None and not run.get('fatal')
        out.append(('no_crash', ok, 'no crash' if ok else f"crash (signal {run.get('crash')}), {run.get('fatal')} fatal lines"))
    allowed = exp.get('exit', [124])
    if run.get('exit') == 137 and run.get('stopped') and 124 in allowed:
        # asked to stop when the time was up, the engine did not pause in time and the rig's
        # timeout killed it: a slow shutdown on a busy machine, not a fault of the run
        out.append(('exit', True, 'exit 137: still running at the end, killed when its shutdown took over 20 s'))
    elif run.get('exit') is not None or allowed:
        out.append(('exit', run.get('exit') in allowed, f"exit {run.get('exit')} (expected {' or '.join(map(str, allowed))}: "
                                                          f"124 = still running at the end)"))
    if 'frames_min' in exp:
        out.append(('frames_min', run.get('frames', 0) >= exp['frames_min'],
                    f"{run.get('frames', 0)} frames (at least {exp['frames_min']})"))
    for t in exp.get('content', ()):
        s = next((s for s in shots if s['t'] == t), None)
        if s is None:
            out.append((f'content@{t}', False, f'no shot at {t} s (the run ended before it?)'))
        elif s.get('missing'):
            out.append((f'content@{t}', False, f'no shot at {t} s: no frame came after the request (the game drew nothing more)'))
        else:
            out.append((f'content@{t}', s['content'], f"shot at {t} s: {'a picture' if s['content'] else 'black or one colour'}"
                                                      f" (dark {s['dark']:.0%}, {s['colours']} colours)"))
    for rx in exp.get('log', ()):
        ok = re.search(rx, log_text, re.M) is not None
        out.append((f'log:{rx}', ok, ('found' if ok else 'not found') + f' in the run log: /{rx}/'))
    for rx in exp.get('log_absent', ()):
        ok = re.search(rx, log_text, re.M) is None
        out.append((f'no-log:{rx}', ok, ('absent' if ok else 'present') + f' in the run log: /{rx}/'))
    if 'exceptions_max' in exp:
        out.append(('exceptions_max', run.get('exceptions', 0) <= exp['exceptions_max'],
                    f"{run.get('exceptions', 0)} exception lines (at most {exp['exceptions_max']})"))
    return out


def compare_rig(golden, run, shots, frames_ratio=0.3):
    """The rig against its baseline: every shot time that had a picture has one now, the run is not
    far slower (frames at least frames_ratio of the baseline's: the rig's speed depends on the Mac's
    load), no crash where there was none."""
    out = []
    if not golden:
        return out
    gs = {s['t']: s for s in golden.get('shots') or ()}
    for s in shots:
        g = gs.get(s['t'])
        if g and g.get('content') and not s['content']:
            now = 'no frame came after the request' if s.get('missing') else 'black or one colour'
            out.append((f'baseline:content@{s["t"]}', False, f'shot at {s["t"]} s had a picture in the baseline, now {now}'))
    gf = (golden.get('run') or {}).get('frames') or 0
    if gf:
        ok = run.get('frames', 0) >= frames_ratio * gf
        out.append(('baseline:frames', ok, f"{run.get('frames', 0)} frames, baseline {gf}"
                                           + ('' if ok else f' (under {frames_ratio:.0%}: slower or stuck)')))
    if (golden.get('run') or {}).get('crash') is None and run.get('crash') is not None:
        out.append(('baseline:crash', False, f"crash (signal {run.get('crash')}); none in the baseline"))
    return out
