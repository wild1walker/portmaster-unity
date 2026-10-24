#!/usr/bin/env python3
"""What a frame recorder could take out of a frame, from a UA_GL_TRACE_DRAWS log:

    framegraph.py <run.log> [--gpu <run.log of a UA_GL_TRACE_GPU run of the same scene>] [--frame N] [--all]

A tiler (the Mali) pays for every pass: it loads each attachment the pass does not clear or
invalidate first, and stores each one at the pass's end. The GL driver sees the frame one call at
a time; a host that held a whole frame (up to the swap) before submitting it would know, for each
pass, what comes after it in the frame. This reads the traced frames' passes and draws (runtime/host
/gltrace.c: each pass's framebuffer, clears and invalidates; each draw's samplers and depth,
stencil and blend state; blits and copies) and lists, per frame, everything such a replay could
do without changing a pixel:

  dead     a pass all of whose results are overwritten (cleared or invalidated) later in the same
           frame before anything reads them (not the window);
  store    an attachment stored at a pass's end that nothing reads before a later clear or
           invalidate of it in the frame (or the frame's end, for the window's depth/stencil):
           an invalidate at the pass's end;
  load     a depth/stencil buffer loaded at a pass's start that the pass does not test against,
           and whose contents after the pass are dead as above: an invalidate at its start;
  merge    two passes on the same framebuffer with no chain of dependencies between them through
           the passes in between (textures sampled, blit and copy sources, attachments loaded and
           written): one could be submitted next to the other, and the second's loads and the
           first's stores of their shared attachments go.

A texture keeps its contents from one frame to the next: whatever a frame leaves in it may be
read by the next one, so only what is overwritten within the frame counts as dead (the window's
colour is presented; its depth/stencil is undefined after the swap). Colour loads are never
counted as avoidable: whether a pass's draws cover every pixel is not in the trace.

The estimate in ms uses a cost model (--pass-ms, --load-c, --load-ds, --store-c, --store-ds: per
pass, and ns per pixel), measured on the handheld with tools/device/passbench (docs/profiling.md).
With --gpu, each pass also gets its measured GPU time (UA_GL_TRACE_GPU=3) from a run without
UA_GL_TRACE_DRAWS (whose reads split the passes): the frames are matched by their pass sequence.
"""
import argparse, re, statistics, sys
from collections import defaultdict

SUMMARY = re.compile(r'^\[gl\] frame (\d+): (\d+) passes')
PASS = re.compile(r'^\[gl\]   fb(\d+) (\d+)x(\d+) clear=(\S+) draws (\d+) verts (\d+) inval=start:(\S+) end:(\S+)(.*)$')
FBDESC = re.compile(r'^\[gl\]   fb(\d+) = (?:the window (\d+)x(\d+)|(rb|tex)(\d+) (\d+)x(\d+) ([^,]*)(?:, depth (rb|tex)(\d+) D(\d+)S(\d+))?)')
EVENT = re.compile(r'^\[gl\]   #(\d+) (draw|clear|blit|copy) (.*)$')
GPU = re.compile(r'gpu ([\d.]+) ms')

ALWAYS_PASS = ('always',)
DST_FACTORS = ('dc', '1-dc', 'da', '1-da')


def bits(s):
    return set() if s == '-' else set(s)


class Pass:
    def __init__(self, fb, w, h, clear, draws, inval_start, inval_end, extra):
        self.fb, self.w, self.h = fb, w, h
        self.clear, self.inval_start, self.inval_end = bits(clear), bits(inval_start), bits(inval_end)
        self.ndraws = draws
        m = GPU.search(extra)
        self.gpu = float(m.group(1)) if m else None
        self.blits = int(re.search(r'blits (\d+)', extra).group(1)) if 'blits' in extra else 0
        self.copies = int(re.search(r'copies (\d+)', extra).group(1)) if 'copies' in extra else 0
        self.reads_px = int(re.search(r'reads (\d+)', extra).group(1)) if 'reads' in extra else 0
        self.events = []
        self.samples = set()       # textures read by draws
        self.srcs = set()          # resources read by blits / copies
        self.copy_dst = set()      # textures written by copies (glCopyTexSubImage)
        self.ds_read = False       # a draw tests depth or stencil values
        self.ds_write = False
        self.c_write = False


def parse_draw(rest):
    """the draw's samplers and fixed-function state"""
    d = {'tex': set(), 'depth': None, 'dwrite': False, 'stencil': None, 'swrite': False, 'sops': [], 'blend': None, 'mask': 'rgba'}
    for m in re.finditer(r' \S+=\d+:tex(\d+)', rest.split(' -> ')[0]):
        d['tex'].add('tex' + m.group(1))
    st = rest.split('| ', 1)[1] if '| ' in rest else ''
    m = re.search(r'depth (\S+)( write)?', st)
    if m:
        d['depth'] = None if m.group(1) == 'off' else m.group(1)
        d['dwrite'] = bool(m.group(2)) and d['depth'] is not None
    m = re.search(r'stencil (\S+)(?: ref -?\d+ (\S+)/(\S+)/(\S+))?( write)?', st)
    if m and m.group(1) != 'off':
        d['stencil'] = m.group(1)
        d['sops'] = [m.group(2), m.group(3), m.group(4)]
        d['swrite'] = bool(m.group(5)) and any(o not in (None, 'keep') for o in d['sops'])
    m = re.search(r'blend (\S+)', st)
    if m and m.group(1) != 'off':
        d['blend'] = m.group(1)
    m = re.search(r'mask (\S+)', st)
    if m:
        d['mask'] = m.group(1)
    return d


def parse_desc(m):
    if m.group(2):
        return ('win', 'winDS', (int(m.group(2)), int(m.group(3))), '', (24, 8))
    col = ('rb' if m.group(4) == 'rb' else 'tex') + m.group(5)
    if 'no colour buffer' in m.group(8):
        col = None
    ds = (('rb' if m.group(9) == 'rb' else 'tex') + m.group(10)) if m.group(9) else None
    return (col, ds, (int(m.group(6)), int(m.group(7))), m.group(8).strip(),
            (int(m.group(11)), int(m.group(12))) if m.group(9) else (0, 0))


def parse(path):
    """[(frame number, [Pass])] with each pass's events and its framebuffer's attachments (as the
    frame's trace described them: Unity's framebuffer objects change attachments between frames)"""
    fbs = {}            # fb -> the last description seen (for passes the frame does not describe)
    frames = []
    pending = []        # events since the last summary (the next frame's)
    cur = None
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        m = FBDESC.match(line)
        if m:
            d = parse_desc(m)
            fbs[int(m.group(1))] = d
            pending.append((-1, 'desc', 'fb%s' % m.group(1), d))
            continue
        m = EVENT.match(line)
        if m:
            pending.append((int(m.group(1)), m.group(2), m.group(3), None))
            continue
        m = SUMMARY.match(line)
        if m:
            cur = (int(m.group(1)), [], pending)
            frames.append(cur)
            pending = []
            continue
        m = PASS.match(line)
        if m and cur is not None:
            g = m.groups()
            cur[1].append(Pass(int(g[0]), int(g[1]), int(g[2]), g[3], int(g[4]), g[6], g[7], g[8]))
    out = []
    for num, passes, events in frames:
        attach_events(passes, events)
        out.append((num, passes))
    return out, fbs


def attach_events(passes, events):
    """give each pass its events: runs of events on one framebuffer, in order, by the passes' counts"""
    i = 0
    for p in passes:
        need_d, need_b, need_c = p.ndraws, p.blits, p.copies
        p.desc = None
        while i < len(events):
            n, kind, rest, d = events[i]
            if kind == 'desc':
                if int(rest[2:]) != p.fb or p.events:
                    break
                p.desc = d
                i += 1
                continue
            fb = int(re.match(r'fb(\d+)', rest).group(1)) if kind in ('draw', 'clear') else None
            if kind == 'blit':
                fb = int(re.search(r'-> fb(\d+)', rest).group(1))
            if kind == 'copy':
                fb = p.fb           # (a copy reads the read framebuffer: it belongs to the current pass)
            if fb != p.fb:
                break
            if kind == 'draw' and need_d == 0 and not (need_b or need_c):
                break
            p.events.append((n, kind, rest))
            if kind == 'draw':
                need_d -= 1
            elif kind == 'blit':
                need_b -= 1
            elif kind == 'copy':
                need_c -= 1
            i += 1
            if need_d == 0 and need_b == 0 and need_c == 0 and i < len(events) and events[i][1] == 'draw':
                break


def resources(fbs, fb, desc=None):
    if fb == 0:
        return 'win', 'winDS'
    f = desc or fbs.get(fb)
    if not f:
        return 'fb%d.c' % fb, 'fb%d.ds' % fb
    return f[0], f[1]


def analyse_pass(p, fbs):
    col, ds = resources(fbs, p.fb, p.desc)
    p.col, p.ds = col, ds
    for n, kind, rest in p.events:
        if kind == 'draw':
            d = parse_draw(rest)
            p.samples |= d['tex']
            if d['mask'] != '':
                p.c_write = True
            if d['depth'] not in (None,) + ALWAYS_PASS:
                p.ds_read = True
            if d['stencil'] is not None:
                ops_read = any(o in ('incr', 'decr', 'incrwrap', 'decrwrap', 'invert') for o in d['sops'] if o)
                if d['stencil'] not in ALWAYS_PASS or ops_read:
                    p.ds_read = True
            if d['dwrite'] or d['swrite']:
                p.ds_write = True
        elif kind == 'clear':
            m = re.match(r'fb\d+ (\S+)', rest)
            b = bits(m.group(1))
            if 'C' in b:
                p.c_write = True
            if b & {'D', 'S'}:
                p.ds_write = True
        elif kind == 'blit':
            m = re.match(r'fb(\d+) -> fb\d+ .* ([CDS-]+) ->', rest)
            if m:
                scol, sds = resources(fbs, int(m.group(1)))
                b = bits(m.group(2))
                if 'C' in b:
                    p.srcs.add(scol); p.c_write = True
                if b & {'D', 'S'}:
                    p.srcs.add(sds); p.ds_write = True
        elif kind == 'copy':
            m = re.match(r'fb(\d+) \(.*\) -> tex(\d+)', rest)
            if m:
                p.srcs.add(resources(fbs, int(m.group(1)))[0])
                p.copy_dst.add('tex' + m.group(2))
    if p.ndraws and not p.events:          # (draws not listed: assume they write colour)
        p.c_write = True
    p.load_c = p.col is not None and not ({'C'} & (p.clear | p.inval_start))
    has_ds = p.ds is not None
    p.has_ds = has_ds
    # the buffer's components (D24S0: depth only)
    dsbits = (p.desc[4] if p.desc else fbs.get(p.fb, (None, None, None, None, (24, 8)))[4]) if p.fb else (24, 8)
    comps = ({'D'} if dsbits[0] else set()) | ({'S'} if dsbits[1] else set()) or {'D', 'S'}
    p.load_ds = has_ds and not (comps <= (p.clear | p.inval_start))
    p.full_c = bool({'C'} & (p.clear | p.inval_start))
    p.full_ds = has_ds and comps <= (p.clear | p.inval_start)
    p.ds_comps = comps


def accesses(p):
    """what the pass reads and writes, by resource: (reads, writes, overwritten at its start)"""
    reads, writes, full = set(p.samples | p.srcs), set(p.copy_dst), set()
    if p.col:
        if p.full_c:
            full.add(p.col)
        elif p.load_c:
            reads.add(p.col)             # (its draws may not cover every pixel: the old colour shows)
        if p.c_write or p.ndraws:
            writes.add(p.col)
    if p.has_ds:
        if p.full_ds:
            full.add(p.ds)
        elif p.load_ds and p.ds_read:
            reads.add(p.ds)
        if p.ds_write:
            writes.add(p.ds)
        if p.load_ds and p.ds_write and not p.ds_read:
            reads.add(p.ds + '?')         # (kept where not written: dead or not, decided below)
    return reads, writes, full


def dead_after(passes, i, res, fbs):
    """nothing reads res after pass i before a later pass overwrites it whole (or the frame ends, for
    the window's depth/stencil); a pass that loads it without testing it passes it on"""
    for q in passes[i + 1:]:
        reads, writes, full = accesses(q)
        if res in reads:
            return False
        if res in full:
            return True
    return res == 'winDS'


def size_of(p, fbs):
    return p.w * p.h


def analyse(passes, fbs, cost):
    first_window = True
    for p in passes:
        analyse_pass(p, fbs)
        if p.fb == 0 and first_window:
            # the window after the swap: its contents are undefined (EGL_BUFFER_DESTROYED), nothing loaded
            first_window = False
            p.load_c = p.load_ds = False
            p.full_c, p.full_ds = True, p.has_ds
    items = []
    n = len(passes)
    # dead passes
    dead = set()
    for i, p in enumerate(passes):
        if p.fb == 0 or p.reads_px:
            continue
        reads, writes, full = accesses(p)
        outs = writes | full
        if outs and all(dead_after(passes, i, r, fbs) for r in outs):
            dead.add(i)
            ms = cost['pass'] + p.w * p.h * (cost['store_c'] + (cost['store_ds'] if p.has_ds else 0)) / 1e6
            if p.load_c:
                ms += p.w * p.h * cost['load_c'] / 1e6
            if p.load_ds:
                ms += p.w * p.h * cost['load_ds'] / 1e6
            items.append(('dead', i, 'fb%d %dx%d (%d draws): its results are overwritten before they are read' % (p.fb, p.w, p.h, p.ndraws), ms, p.gpu))
    # stores at a pass's end
    for i, p in enumerate(passes):
        if i in dead:
            continue
        for kind, res, attached in (('C', p.col, True), ('DS', p.ds, p.has_ds)):
            if not attached or res is None or res == 'win':
                continue
            if (kind == 'C' and 'C' in p.inval_end) or (kind == 'DS' and p.ds_comps <= p.inval_end):
                continue
            if dead_after(passes, i, res, fbs):
                ms = p.w * p.h * (cost['store_c'] if kind == 'C' else cost['store_ds']) / 1e6
                items.append(('store', i, 'fb%d %s (%s): not read again before it is overwritten' % (p.fb, kind, res), ms, None))
    # depth/stencil loads that nothing needs
    for i, p in enumerate(passes):
        if i in dead or not p.has_ds or not p.load_ds or p.ds_read:
            continue
        if dead_after(passes, i, p.ds, fbs):
            ms = p.w * p.h * cost['load_ds'] / 1e6
            items.append(('load', i, 'fb%d DS (%s): loaded, not tested, dead after the pass' % (p.fb, p.ds), ms, None))
    # merges: no chain p -> x -> q
    acc = [accesses(p) for p in passes]

    def dep(a, b):           # b (later) depends on a
        ra, wa, fa = acc[a]
        rb, wb, fb_ = acc[b]
        wa_all, wb_all = wa | fa, wb | fb_
        return bool((wa_all & rb) or (ra & wb_all) or (wa_all & wb_all))

    reach = [[False] * n for _ in range(n)]          # reach[a][b]: a chain of dependencies a -> ... -> b
    for b in range(n):
        for a in range(b - 1, -1, -1):
            if dep(a, b):
                reach[a][b] = True
            else:
                reach[a][b] = any(reach[a][x] and reach[x][b] for x in range(a + 1, b))
    merged = set()
    for i, p in enumerate(passes):
        if i in dead:
            continue
        for j in range(i + 1, n):
            q = passes[j]
            if q.fb != p.fb or j in dead:
                continue
            between = [x for x in range(i + 1, j) if x not in dead]
            if not between:
                break
            if any(reach[i][x] and reach[x][j] for x in between):
                break
            if q.full_c and (q.full_ds or not q.has_ds):
                break         # (q overwrites everything: nothing to save but the pass)
            ms = cost['pass']
            if not q.full_c and q.load_c:
                ms += q.w * q.h * (cost['load_c'] + cost['store_c']) / 1e6
            if q.has_ds and q.load_ds:
                ms += q.w * q.h * (cost['load_ds'] + cost['store_ds']) / 1e6
            # the passes in between that could go before or after
            items.append(('merge', i, 'fb%d: pass %d with pass %d (%d passes between, none depends on both)' % (p.fb, i, j, len(between)), ms, None))
            merged.add((i, j))
            break
    return items


def describe(passes):
    out = []
    for i, p in enumerate(passes):
        r, w, f = accesses(p)
        def fmt(s):
            return ','.join(sorted(s)) or '-'
        out.append('  %2d fb%-3d %4dx%-4d %-4s draws %3d load %s%s inval %s/%s reads %s writes %s%s' % (
            i, p.fb, p.w, p.h, ''.join(sorted(p.clear)) or '-', p.ndraws,
            'C' if p.load_c else '-', 'DS' if p.load_ds else '', ''.join(sorted(p.inval_start)) or '-',
            ''.join(sorted(p.inval_end)) or '-', fmt(r), fmt(w | f), (' gpu %.2f' % p.gpu) if p.gpu is not None else ''))
    return out


def gpu_times(path):
    """the GPU run's frames: [(fb sequence, [gpu ms])]"""
    frames, _ = parse(path)
    return [(tuple((p.fb, p.w, p.h) for p in ps), [p.gpu for p in ps]) for _, ps in frames if all(p.gpu is not None for p in ps)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--gpu', help='run.log of a UA_GL_TRACE_GPU run of the same scene (per-pass GPU times)')
    ap.add_argument('--frame', type=int, help='only this traced frame (its number)')
    ap.add_argument('--all', action='store_true', help='every traced frame, not only the first with draws')
    ap.add_argument('--passes', action='store_true', help='list each pass with its reads and writes')
    ap.add_argument('--pass-ms', type=float, default=0.10, help='GPU cost of a pass itself, ms (default 0.10)')
    ap.add_argument('--load-c', type=float, default=1.0, help='colour load, ns a pixel')
    ap.add_argument('--load-ds', type=float, default=1.9, help='depth/stencil load, ns a pixel')
    ap.add_argument('--store-c', type=float, default=0.5, help='colour store, ns a pixel')
    ap.add_argument('--store-ds', type=float, default=0.5, help='depth/stencil store, ns a pixel')
    a = ap.parse_args()
    cost = {'pass': a.pass_ms, 'load_c': a.load_c, 'load_ds': a.load_ds, 'store_c': a.store_c, 'store_ds': a.store_ds}
    frames, fbs = parse(a.log)
    frames = [(n, ps) for n, ps in frames if any(p.events for p in ps)] or frames
    if a.frame is not None:
        frames = [(n, ps) for n, ps in frames if n == a.frame]
    elif not a.all:
        frames = frames[1:2] if len(frames) > 1 else frames[:1]     # (the first traced frame is often a load's)
    gt = gpu_times(a.gpu) if a.gpu else []
    for num, passes in frames:
        seq = tuple((p.fb, p.w, p.h) for p in passes)
        times = [t for s, t in gt if s == seq]
        if times:
            for k, p in enumerate(passes):
                p.gpu = statistics.median(t[k] for t in times)
        items = analyse(passes, fbs, cost)
        tot = sum(p.gpu for p in passes if p.gpu is not None)
        print('frame %d: %d passes, %d draws%s' % (num, len(passes), sum(p.ndraws for p in passes),
                                                  (', GPU %.2f ms (median of %d frames, serialized)' % (tot, len(times))) if times else ''))
        if a.passes:
            print('\n'.join(describe(passes)))
        if not items:
            print('  nothing')
        total = 0
        for kind, i, text, ms, gpu in items:
            total += ms
            print('  %-5s pass %2d  %-90s ~%.2f ms%s' % (kind, i, text, ms, (' (pass %.2f ms measured)' % gpu) if gpu else ''))
        print('  estimate: %.2f ms a frame' % total)


if __name__ == '__main__':
    main()
