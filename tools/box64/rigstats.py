"""The test rig's numbers and pictures, on the host (tools/box64/rig.sh; Python standard library only).

    python3 -I rigstats.py <out dir> [--png]   screenshots (X window dumps, .xwd) -> .png, then
                                               frames per second and CPU per thread per frame -> rig.txt
                                               (printed); --png: the screenshots only

Reads only what rig_inside.sh wrote: shot_<s>.xwd, glxes_fps or hud/fps (frames per second, one
line a second), cpu0.txt / cpu1.txt (tid name ticks), t0.txt / t1.txt, gpid.txt, hz.txt.
"""
import glob
import os
import struct
import sys
import zlib


def xwd_to_png(src, dst):
    """A 24/32-bit TrueColor ZPixmap X window dump (xwd -root) as an RGB PNG."""
    with open(src, 'rb') as f:
        d = f.read()
    h = struct.unpack_from('>25I', d, 0)
    size, fmt, depth, w, ht, byte_order, bpp, bpl, ncolors = h[0], h[2], h[3], h[4], h[5], h[7], h[11], h[12], h[19]
    rmask, gmask, bmask = h[14], h[15], h[16]
    if fmt != 2 or bpp not in (24, 32):
        raise ValueError(f'{src}: unsupported dump (format {fmt}, {bpp} bits per pixel)')
    off = size + 12 * ncolors
    shifts = [(m & -m).bit_length() - 1 for m in (rmask, gmask, bmask)]
    step = bpp // 8
    rows = bytearray()
    for y in range(ht):
        rows.append(0)
        line = d[off + y * bpl: off + y * bpl + w * step]
        for x in range(0, w * step, step):
            px = int.from_bytes(line[x:x + step], 'little' if byte_order == 0 else 'big')
            rows += bytes(((px & m) >> s) & 255 for m, s in zip((rmask, gmask, bmask), shifts))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, ht, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b'')
    with open(dst, 'wb') as f:
        f.write(png)


def read(path, default=''):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


def stats(out):
    lines = []
    fps_file = os.path.join(out, 'glxes_fps')
    src = 'glxes'
    if not (os.path.exists(fps_file) and os.path.getsize(fps_file)):     # no glxes: Mesa's HUD
        fps_file, src = os.path.join(out, 'hud', 'fps'), 'HUD'
    vals = [float(l.split()[0]) for l in read(fps_file).splitlines() if l.strip()]
    t0, t1 = read(os.path.join(out, 't0.txt')).strip(), read(os.path.join(out, 't1.txt')).strip()
    m0, m1 = (int(t0), int(t1)) if t0.isdigit() and t1.isdigit() else (0, 0)
    win = vals[m0:m1] if m1 and len(vals) >= m1 else vals[-max(1, m1 - m0):]
    fps = sum(win) / len(win) if win else 0
    lines.append(f'fps {fps:.1f} over {len(win)} s ({src} seconds {len(vals)}; whole run min/max '
                 f'{min(vals or [0]):.0f}/{max(vals or [0]):.0f})')

    def ticks(name):
        return {l.split()[0]: (l.split()[1], int(l.split()[2])) for l in read(os.path.join(out, name)).splitlines()
                if len(l.split()) == 3}

    c0, c1 = ticks('cpu0.txt'), ticks('cpu1.txt')
    if not (c0 and c1 and m1 > m0):
        lines.append('no thread CPU counts (the game ended before the measured window)')
        return lines
    hz = int(read(os.path.join(out, 'hz.txt'), '100').strip() or 100)
    secs = m1 - m0
    pid = read(os.path.join(out, 'gpid.txt')).strip()
    rows = sorted(((c1[t][1] - c0.get(t, (0, 0))[1]) / hz, t, c1[t][0]) for t in c1)[::-1]
    lines.append(f'cpu total {100 * sum(r[0] for r in rows) / secs:.0f}% of a core over {secs} s')
    for cpu, tid, n in rows[:8]:
        if cpu > 0:
            per_frame = 1000 * cpu / (fps * secs) if fps else 0
            lines.append(f'  {"main" if tid == pid else "tid " + tid} {n}: {100 * cpu / secs:.0f}%, {per_frame:.1f} ms/frame')
    return lines


def main(out, png_only=False):
    for f in sorted(glob.glob(os.path.join(out, 'shot_*.xwd'))):
        try:
            xwd_to_png(f, f[:-4] + '.png')
            os.remove(f)
        except (ValueError, struct.error, OSError) as ex:
            print(f'{f}: {ex}', file=sys.stderr)
    if png_only:
        return
    text = read(os.path.join(out, 'rig.txt')) + '\n'.join(stats(out)) + '\n'
    with open(os.path.join(out, 'rig.txt'), 'w') as f:
        f.write(text)
    print(text, end='')


if __name__ == '__main__':
    main(sys.argv[1], '--png' in sys.argv[2:])
