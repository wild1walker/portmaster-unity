#!/usr/bin/env python3
"""Sum up tools/device/profbench.sh runs: benchsum.py [--syms DIR] [--top N] [--brief] <run folder>...

For each run folder (run.log, snap-0.txt / snap-1.txt, freq.log, prof-*.txt; tools/device/profrun.sh
writes them): the frame rate over the measurement, CPU and GPU clocks (residency from cpufreq's
time_in_state and devfreq's trans_stat, or the per-second samples), each core's use, the game's
threads by CPU time and their wait for a CPU (schedstat), memory and swap traffic, temperatures and
throttling, other processes that took CPU time, and the profile's summary (profsum.py). With more
than one run, a table compares them with the first (the baseline: run it first and last to see
the drift)."""
import collections, argparse, contextlib, glob, io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import profsum  # noqa: E402

FPS = re.compile(r'^\[fps\] ([\d.]+) fps, frame ([\d.]+) avg / ([\d.]+) p99 / ([\d.]+) worst ms, late (\d+) hitch (\d+), gc (-?\d+).*\((\d+) frames')


def read(path):
    with open(path, errors='replace') as f:
        return f.read()


def snapshot(path):
    """{section: [lines]} of a profrun.sh snapshot"""
    out, cur = {}, None
    if not os.path.exists(path):
        return out
    with open(path, errors='replace') as f:
        text = f.read()
    for line in text.splitlines():
        if line.startswith('=== '):
            cur = line[4:]
            out[cur] = []
        elif cur is not None:
            out[cur].append(line)
    return out


def kv(lines):
    d = {}
    for l in lines:
        p = l.replace(':', ' ').split()
        if len(p) >= 2:
            try:
                d[p[0]] = int(p[1])
            except ValueError:
                pass
    return d


def procstat(lines):
    d = {}
    for l in lines:
        p = l.split()
        if p and p[0].startswith('cpu'):
            d[p[0]] = [int(x) for x in p[1:8]]
    return d


def tasks(snap):
    """{tid: (name, run_ns, wait_ns, slices, utime, stime)}"""
    t = {}
    for k, lines in snap.items():
        if not k.startswith('task '):
            continue
        p = k.split(' ', 2)
        if len(p) < 2 or not p[1].isdigit():
            continue
        tid, name = int(p[1]), (p[2] if len(p) > 2 else '?').replace(' ', '_')
        run = wait = sl = ut = st = 0
        for l in lines:
            f = l.split()
            if len(f) == 3 and all(x.isdigit() for x in f):
                run, wait, sl = map(int, f)
            elif ')' in l:
                r = l[l.rindex(')') + 2:].split()
                if len(r) > 12:
                    ut, st = int(r[11]), int(r[12])
        t[tid] = (name, run, wait, sl, ut, st)
    return t


def procs(snap):
    """{pid: (name, utime + stime)}"""
    d = {}
    for l in snap.get('procs', []):
        if ')' not in l or '(' not in l:
            continue
        pid = int(l.split()[0])
        name = l[l.index('(') + 1:l.rindex(')')]
        r = l[l.rindex(')') + 2:].split()
        if len(r) > 12:
            d[pid] = (name, int(r[11]) + int(r[12]))
    return d


def residency(lines):
    """{freq: time} from cpufreq time_in_state ('freq ticks') or devfreq trans_stat (last column, ms)"""
    d = {}
    for l in lines:
        p = l.replace('*', ' ').split()
        if len(p) == 2 and p[0].isdigit() and p[1].isdigit():
            d[int(p[0])] = int(p[1])
        elif ':' in l and l.split(':')[0].replace('*', '').strip().isdigit():
            left = l.split(':')[0].replace('*', '').strip()
            nums = l.split(':')[1].split()
            if nums and nums[-1].isdigit():
                d[int(left)] = int(nums[-1])
    return d


def delta_res(a, b):
    return {f: b[f] - a.get(f, 0) for f in b if b[f] - a.get(f, 0) > 0}


def fmt_res(d, unit):
    tot = sum(d.values())
    if not tot:
        return '-'
    avg = sum(f * t for f, t in d.items()) / tot / unit
    top = sorted(d.items(), key=lambda x: -x[1])[:3]
    return f'{avg:.0f} MHz avg (' + ', '.join(f'{f / unit:.0f}: {100 * t / tot:.0f}%' for f, t in sorted(top, key=lambda x: -x[0])) + ')'


class Run:
    def __init__(self, d):
        self.dir, self.name = d, os.path.basename(os.path.normpath(d))
        rf = os.path.join(d, 'result')
        self.result = read(rf).strip() if os.path.exists(rf) else 'no result file'
        log = read(os.path.join(d, 'run.log')).splitlines() if os.path.exists(os.path.join(d, 'run.log')) else []
        self.log = log
        inside, self.fps = False, []
        self.notes = []
        for l in log:
            if l.startswith('[bench] measure start'):
                inside = True
            elif l.startswith('[bench] measure end'):
                inside = False
            m = FPS.match(l)
            if m and inside:
                self.fps.append(tuple(float(x) for x in m.groups()))
            if re.match(r'^\[bench\] env ', l) and not re.search(r'=(dummy|sdl)$|UA_FPS=|ANDROID_WINDOW|UA_EGL_PBUFFER|UATEST', l):
                self.notes.append(l[len('[bench] env '):])
            if any(s in l for s in ('JobSystem:', '[host] mono options', '[host] frame rate', 'gfx-threading', '[prof] prof-')):
                self.notes.append(l.strip()[:200])
        self.s0, self.s1 = snapshot(os.path.join(d, 'snap-0.txt')), snapshot(os.path.join(d, 'snap-1.txt'))
        self.secs = 0.0
        u0, u1 = self.s0.get('/proc/uptime'), self.s1.get('/proc/uptime')
        if u0 and u1:
            self.secs = float(u1[0].split()[0]) - float(u0[0].split()[0])
        self.profiles = sorted(glob.glob(os.path.join(d, 'prof-*.txt')))

    def fps_avg(self):
        if not self.fps:
            return None
        frames = sum(f[7] for f in self.fps)
        secs = sum(f[7] / f[0] for f in self.fps if f[0] > 0)
        return frames / secs if secs else None

    def main_cpu(self):
        t0, t1 = tasks(self.s0), tasks(self.s1)
        for tid, v in t1.items():
            if v[0] == 'UnityMain' and tid in t0 and self.secs:
                return 100 * (v[1] - t0[tid][1]) / 1e9 / self.secs, 100 * (v[2] - t0[tid][2]) / 1e9 / self.secs
        return None, None

    def clocks(self):
        cpu = gpu = '-'
        for k in self.s1:
            if k.endswith('time_in_state') and k in self.s0:
                cpu = fmt_res(delta_res(residency(self.s0[k]), residency(self.s1[k])), 1000)
                break
        g = [k for k in self.s1 if k.startswith('/sys/class/devfreq/') and k.endswith('trans_stat') and k in self.s0]
        out = []
        for k in g:
            dev = k.split('/')[4]
            out.append(f'{dev} {fmt_res(delta_res(residency(self.s0[k]), residency(self.s1[k])), 1e6)}')
        if out:
            gpu = '; '.join(out)
        return cpu, gpu

    def freq_samples(self):
        f = os.path.join(self.dir, 'freq.log')
        cpu, gpu = [], []
        if os.path.exists(f):
            for l in read(f).splitlines():
                m = re.search(r'cpu=([\d,]+) gpu=([\d,]+)', l)
                if m:
                    cpu += [int(x) for x in m.group(1).split(',')]
                    gpu += [int(x) for x in m.group(2).split(',')[:1]]
        return cpu, gpu

    def temp_samples(self):
        """each thermal zone's per-second temperatures (C) from freq.log (profrun.sh since 2026-10-23)"""
        f = os.path.join(self.dir, 'freq.log')
        zones = []
        if os.path.exists(f):
            for l in read(f).splitlines():
                m = re.search(r'temp=([\d,]+)', l)
                if m:
                    v = [int(x) for x in m.group(1).split(',')]
                    while len(zones) < len(v):
                        zones.append([])
                    for i, x in enumerate(v):
                        zones[i].append(x)
        return zones

    def report(self, syms, top, brief):
        print(f'\n######## {self.name}: {self.result}')
        for n in self.notes:
            print(f'  {n}')
        fa = self.fps_avg()
        if fa:
            print(f'  frames: {fa:.1f} fps over {len(self.fps)} s (lowest second {min(f[0] for f in self.fps):.1f}), '
                  f'p99 up to {max(f[2] for f in self.fps):.1f} ms, worst {max(f[3] for f in self.fps):.1f} ms, '
                  f'{sum(f[4] for f in self.fps):.0f} frames over 25 ms, {sum(f[5] for f in self.fps):.0f} over 50 ms, {sum(max(f[6], 0) for f in self.fps):.0f} GCs')
        else:
            print('  frames: no [fps] lines in the measurement')
        if not self.s1:
            return
        cpu, gpu = self.clocks()
        fc, fg = self.freq_samples()
        print(f'  CPU clock: {cpu}' + (f'; per-second samples {min(fc)}-{max(fc)} MHz, avg {sum(fc) / len(fc):.0f}' if fc else ''))
        print(f'  GPU/devfreq: {gpu}' + (f'; per-second samples {min(fg)}-{max(fg)} MHz' if fg else ''))
        gov = sorted({self.s1[k][0] for k in self.s1 if k.endswith('scaling_governor') and self.s1[k]})
        mx = sorted({int(self.s1[k][0]) // 1000 for k in self.s1 if k.endswith('scaling_max_freq') and self.s1[k]})
        dgov = sorted({k.split('/')[4] + '=' + self.s1[k][0] for k in self.s1 if k.startswith('/sys/class/devfreq/') and k.endswith('governor') and self.s1[k]})
        if gov or dgov:
            print(f'  governors: cpu {",".join(gov)} (max {",".join(map(str, mx))} MHz); {" ".join(dgov)}')
        p0, p1 = procstat(self.s0.get('/proc/stat', [])), procstat(self.s1.get('/proc/stat', []))
        cores = []
        for c in sorted(p1):
            if c == 'cpu' or c not in p0:
                continue
            d = [b - a for a, b in zip(p0[c], p1[c])]
            tot = sum(d)
            if tot:
                cores.append(f'{c} {100 * (tot - d[3] - d[4]) / tot:.0f}%')
        if cores:
            print('  cores busy: ' + ', '.join(cores))
        t0, t1 = tasks(self.s0), tasks(self.s1)
        rows = []
        for tid, v in t1.items():
            a = t0.get(tid)
            if a and self.secs:
                rows.append((100 * (v[1] - a[1]) / 1e9 / self.secs, 100 * (v[2] - a[2]) / 1e9 / self.secs, v[0], (v[4] - a[4]) * 10, (v[5] - a[5]) * 10))
        rows.sort(reverse=True)
        if rows:
            print('  threads (% of a core on a CPU / waiting for one; user, kernel ms): ' +
                  ', '.join(f'{n} {r:.0f}/{w:.0f}% ({u}, {k})' for r, w, n, u, k in rows[:8]))
        v0, v1 = kv(self.s0.get('/proc/vmstat', [])), kv(self.s1.get('/proc/vmstat', []))
        m1 = kv(self.s1.get('/proc/meminfo', []))
        rss = next((kv(v).get('VmRSS') for k, v in self.s1.items() if k.endswith('/status')), None)
        if v1:
            print(f'  memory: available {m1.get("MemAvailable", 0) >> 10} MB at the end, swap used {(m1.get("SwapTotal", 0) - m1.get("SwapFree", 0)) >> 10} MB, '
                  f'game rss {(rss or 0) >> 10} MB; during the measurement: swap-ins {v1.get("pswpin", 0) - v0.get("pswpin", 0)}, '
                  f'swap-outs {v1.get("pswpout", 0) - v0.get("pswpout", 0)}, major faults {v1.get("pgmajfault", 0) - v0.get("pgmajfault", 0)}')
        temps = []
        for k in sorted(self.s1):
            if k.endswith('/type') and 'thermal_zone' in k:
                tk = k[:-5] + '/temp'
                if tk in self.s1 and tk in self.s0 and self.s1[tk]:
                    temps.append(f'{self.s1[k][0]} {int(self.s0[tk][0]) // 1000}->{int(self.s1[tk][0]) // 1000} C')
        cool = [f'{self.s1[k][0]}={self.s1[k[:-5] + "/cur_state"][0]}' for k in sorted(self.s1)
                if 'cooling_device' in k and k.endswith('/type') and self.s1.get(k[:-5] + '/cur_state')]
        if temps or cool:
            print(f'  thermal: {", ".join(temps)}' + (f'; cooling {", ".join(cool)}' if cool else ''))
        zs = self.temp_samples()
        if zs and fg:
            zk = [k for k in self.s1 if k.startswith('/sys/class/thermal/thermal_zone') and k.endswith('/type') and self.s1[k]]
            names = [self.s1[k][0] for k in sorted(zk, key=lambda k: int(re.sub(r'\D', '', k.split('/')[4]) or 0))]
            per = ', '.join(f'{names[i] if i < len(names) else i} {min(z)}-{max(z)} (avg {sum(z) / len(z):.1f})' for i, z in enumerate(zs) if z)
            clk = ', '.join(f'{c} MHz {n} s' for c, n in sorted(collections.Counter(fg).items(), reverse=True))
            print(f'  per second: {per} C; GPU {clk}')
        q0, q1 = procs(self.s0), procs(self.s1)
        game = {k for k in self.s1 if k.startswith('/proc/') and k.endswith('/status')}
        gp = {int(k.split('/')[2]) for k in game}
        other = sorted(((q1[p][1] - q0[p][1], q1[p][0]) for p in q1 if p in q0 and p not in gp and q1[p][1] > q0[p][1]), reverse=True)[:5]
        if other and self.secs:
            print('  other processes: ' + ', '.join(f'{n} {t / self.secs:.0f}%' for t, n in other))
        gc = os.path.join(self.dir, 'gpucnt.txt')
        if os.path.exists(gc):
            # GPUCNT=1 runs: the GPU's counters over the measurement (runtime/host/gpucnt.c)
            lines = read(gc).splitlines()
            if '# derived' in ''.join(lines):
                derived = lines[[i for i, l in enumerate(lines) if l.startswith('# derived')][0] + 1:]
                print('  GPU counters: ' + '\n                '.join(derived))
            else:
                print('  GPU counters: ' + ' '.join(lines[-2:])[:200])
        if brief:
            return
        for p in self.profiles:
            print(f'  --- {os.path.basename(p)}')
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                profsum.main([p, '--top', str(top)] + sum((['--syms', s] for s in syms), []))
            print('\n'.join('  ' + l for l in out.getvalue().splitlines()))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--syms', action='append', default=[])
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--brief', action='store_true', help='no profile summaries')
    a = ap.parse_args(argv)
    runs = [Run(d) for d in a.runs]
    for r in runs:
        r.report(a.syms, a.top, a.brief)
    if len(runs) > 1:
        print('\n== compared with the first run')
        print(f'  {"run":<20} {"fps":>6} {"vs 1st":>7} {"main CPU":>8} {"waits":>6}  result')
        base = runs[0].fps_avg()
        for r in runs:
            f = r.fps_avg()
            mc, mw = r.main_cpu()
            d = f'{100 * (f - base) / base:+.1f}%' if f and base else '-'
            print(f'  {r.name:<20} {f or 0:6.1f} {d:>7} {mc or 0:7.0f}% {mw or 0:5.0f}%  {r.result}')


if __name__ == '__main__':
    main()
