#!/usr/bin/env python3
"""Device-side: per-thread CPU use of a running process, every <interval> s (100% = one core),
and a "[therm]" line with temperatures, CPU/GPU clocks and thermal throttle steps.
Usage: python3 profcpu.py <pid> [interval] [top]   (until the process exits)"""
import os, sys, time

pid = int(sys.argv[1]); iv = float(sys.argv[2]) if len(sys.argv) > 2 else 10; top = int(sys.argv[3]) if len(sys.argv) > 3 else 6
hz = os.sysconf('SC_CLK_TCK')

def rd(path):
    try: return open(path).read().strip()
    except OSError: return None

def thermal():
    """temperatures (C), CPU and GPU clocks (MHz), throttle steps: '[therm] cpu 61 gpu 63 | cpu 1512/1512 gpu 504 | cool 1/0'"""
    t = []
    for z in sorted(os.listdir('/sys/class/thermal')):
        if not z.startswith('thermal_zone'): continue
        typ, v = rd(f'/sys/class/thermal/{z}/type'), rd(f'/sys/class/thermal/{z}/temp')
        if typ and v and typ.endswith('_thermal_zone') and typ.split('_')[0] in ('cpu', 'gpu', 'ddr'):
            t.append(f'{typ.split("_")[0]} {int(v) // 1000}')
    f = [rd(f'/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_cur_freq') for c in range(4)]
    cpu = '/'.join(str(int(x) // 1000) for x in f if x)
    g = rd('/sys/class/devfreq/gpu/cur_freq')
    cool = '/'.join(rd(f'/sys/class/thermal/{c}/cur_state') or '?' for c in sorted(os.listdir('/sys/class/thermal')) if c.startswith('cooling_device'))
    return f'[therm] {" ".join(t)} | cpu {cpu} gpu {int(g) // 1000000 if g else "?"} | cool {cool}'

def sample():
    r = {}
    st = rd(f'/proc/{pid}/stat')
    if st is None or st[st.rindex(')') + 2] == 'Z':     # gone, or a zombie its parent has not reaped yet
        return None
    try:
        for t in os.listdir(f'/proc/{pid}/task'):
            try:
                st = open(f'/proc/{pid}/task/{t}/stat').read()
                name = st[st.index('(') + 1:st.rindex(')')]
                f = st[st.rindex(')') + 2:].split()
                r[t] = (name, int(f[11]) + int(f[12]))
            except OSError:
                pass
    except OSError:
        return None
    return r

prev, t0 = sample(), time.time()
while prev is not None:
    time.sleep(iv)
    cur, t1 = sample(), time.time()
    if cur is None:
        break
    use = {}
    for t, (name, ticks) in cur.items():
        d = ticks - prev.get(t, (name, ticks))[1]
        use[name] = use.get(name, 0) + d
    total = sum(use.values())
    rows = sorted(use.items(), key=lambda kv: -kv[1])[:top]
    print(f'[cpu] {time.strftime("%T")} total {100 * total / hz / (t1 - t0):.0f}%  ' +
          '  '.join(f'{n} {100 * d / hz / (t1 - t0):.0f}%' for n, d in rows), flush=True)
    print(thermal(), flush=True)
    prev, t0 = cur, t1
