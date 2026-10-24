"""stepstat.py <checkout> <label> -- <pmunity arguments>: one pmunity command run as a handheld's
setup runs it, with what it cost: wall and CPU time, peak memory (VmHWM, and the peak of RssAnon
sampled every 20 ms) and what it read and wrote (/proc/self/io's rchar and wchar: what reaches a
card through FUSE). Linux only; for an arm64 container with the setup's own Python, e.g.

    docker run --rm --platform linux/arm64 --memory 1500m --cpus 4 -v <the checkout, the data> \\
      ua-port-rehearse <python>/bin/python3.12 tools/port/stepstat.py <checkout> textures -- \\
      convert <data> --config <the port's convert.json> --steps textures

STEPSTAT_MEM_MB (default 973) is the RAM the textures step is told the system has (its 'auto'
pipeline converts big textures alone under 2 GB, as on a 1 GB handheld). STEPSTAT_EACH=1 also
prints the anonymous RSS after each file the converter closes, and its peak since the one before.
Used for docs/setup-speed.md.
"""
import os
import resource
import sys
import threading
import time


def _status():
    d = {}
    with open('/proc/self/status') as f:
        for line in f:
            k, _, v = line.partition(':')
            if k in ('VmHWM', 'RssAnon'):
                d[k] = int(v.split()[0]) >> 10
    return d


def main():
    co, label = sys.argv[1], sys.argv[2]
    args = sys.argv[sys.argv.index('--') + 1:]
    sys.path.insert(0, os.path.join(co, 'convert'))
    peak = {'all': 0, 'since': 0}

    def sample():
        while True:
            try:
                a = _status()['RssAnon']
                peak['all'], peak['since'] = max(peak['all'], a), max(peak['since'], a)
            except (OSError, KeyError):
                pass
            time.sleep(0.02)
    threading.Thread(target=sample, daemon=True).start()
    import pmunity.textures as T
    T.mem_total_mb = lambda: int(os.environ.get('STEPSTAT_MEM_MB', '973'))
    if os.environ.get('STEPSTAT_EACH'):
        import pmunity.unity_io as U
        close = U.Loaded.close

        def each(self, close=close):
            close(self)
            print(f'rss {_status()["RssAnon"]} MB, peak {peak["since"]} MB: {os.path.basename(self.path)}',
                  file=sys.stderr, flush=True)
            peak['since'] = 0
        U.Loaded.close = each
    from pmunity.__main__ import main as pmunity
    t0, rc = time.time(), 0
    try:
        pmunity(args)
    except SystemExit as ex:
        rc = ex.code
    finally:
        with open('/proc/self/io') as f:
            io = dict(line.split(': ') for line in f.read().split('\n') if line)
        ru = resource.getrusage(resource.RUSAGE_SELF)
        print(f'## {label}: wall {time.time() - t0:.1f} s, cpu {ru.ru_utime + ru.ru_stime:.1f} s, '
              f'VmHWM {_status()["VmHWM"]} MB, peak anon {peak["all"]} MB, read {int(io["rchar"]) >> 20} MB, '
              f'written {int(io["wchar"]) >> 20} MB', file=sys.stderr, flush=True)
    sys.exit(rc)


if __name__ == '__main__':
    main()
