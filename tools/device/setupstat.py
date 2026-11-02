#!/usr/bin/env python3
"""Device-side: a sampler for a port's setup (docs/rust-setup.md, "On the handheld"). Every
<interval> s one line of JSON: the cores' times (/proc/stat), memory (/proc/meminfo), the card's
sectors read and written and its busy time (/proc/diskstats, <partition>), the exFAT FUSE daemon's
CPU, the clock and the temperature, and for each process whose name is in WATCH (and the names
given) its CPU, threads' CPU, memory (VmRSS, VmHWM, RssAnon) and I/O counters (/proc/<pid>/io).
setupstat_report.py sums it up per interval.
Usage: python3 setupstat.py <out.jsonl> [interval] [partition] [process name...]   (until killed)"""
import json, os, sys, time

out = open(sys.argv[1], 'a', buffering=1)
iv = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
PART = sys.argv[3] if len(sys.argv) > 3 else 'mmcblk0p4'
WATCH = {'pmu', '7zzs.aarch64', 'asmpatch', 'sha256sum', 'cp', 'mv', 'rm', 'sync', 'unzip'} | set(sys.argv[4:])


def rd(p):
    try:
        with open(p) as f:
            return f.read()
    except OSError:
        return ''


def fuse_pid():
    for p in os.listdir('/proc'):
        if p.isdigit() and any(n in rd(f'/proc/{p}/cmdline') for n in ('mount.exfat', 'exfat-fuse')):
            return p
    return None


fp = fuse_pid()
while True:
    s = {'t': round(time.time(), 2)}
    cpus = {}
    for l in rd('/proc/stat').splitlines():
        if l.startswith('cpu'):
            f = l.split()
            cpus[f[0]] = [int(x) for x in f[1:8]]
    s['cpu'] = cpus
    mi = {}
    for l in rd('/proc/meminfo').splitlines():
        k, v = l.split(':', 1)
        if k in ('MemAvailable', 'MemFree', 'Cached', 'Dirty', 'Writeback', 'SwapFree', 'Buffers', 'AnonPages', 'Mapped'):
            mi[k] = int(v.split()[0])
    s['mem'] = mi
    for l in rd('/proc/diskstats').splitlines():
        f = l.split()
        if len(f) > 13 and f[2] == PART:
            s['disk'] = [int(f[5]), int(f[9]), int(f[12])]   # sectors read, written, ms busy
    if fp:
        st = rd(f'/proc/{fp}/stat').rsplit(')', 1)[-1].split()
        if st:
            s['fuse'] = int(st[11]) + int(st[12])
    fr = rd('/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq').strip()
    s['mhz'] = int(fr) // 1000 if fr else 0
    tz = rd('/sys/class/thermal/thermal_zone0/temp').strip()
    s['temp'] = int(tz) // 1000 if tz else 0
    procs = []
    for p in os.listdir('/proc'):
        if not p.isdigit():
            continue
        comm = rd(f'/proc/{p}/comm').strip()
        if comm not in WATCH:
            continue
        st = rd(f'/proc/{p}/stat').rsplit(')', 1)[-1].split()
        if not st:
            continue
        d = {'pid': int(p), 'comm': comm, 'cpu': int(st[11]) + int(st[12]), 'thr': int(st[17])}
        for l in rd(f'/proc/{p}/status').splitlines():
            k, _, v = l.partition(':')
            if k in ('VmRSS', 'VmHWM', 'RssAnon'):
                d[k] = int(v.split()[0])
        for l in rd(f'/proc/{p}/io').splitlines():
            k, _, v = l.partition(':')
            if k in ('rchar', 'wchar', 'read_bytes', 'write_bytes', 'syscr', 'syscw'):
                d[k] = int(v)
        a = rd(f'/proc/{p}/cmdline').split('\0')
        d['arg'] = ' '.join(a[1:3])[:60]
        # per-thread CPU (names), for the encoder's threads
        thr = {}
        try:
            for t in os.listdir(f'/proc/{p}/task'):
                ts = rd(f'/proc/{p}/task/{t}/stat').rsplit(')', 1)[-1].split()
                if ts:
                    thr[t] = int(ts[11]) + int(ts[12])
        except OSError:
            pass
        d['tcpu'] = thr
        procs.append(d)
    s['p'] = procs
    out.write(json.dumps(s, separators=(',', ':')) + '\n')
    time.sleep(iv)
