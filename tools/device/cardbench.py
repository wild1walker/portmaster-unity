#!/usr/bin/env python3
"""Device-side: cardbench.py <dir> <big file>: what the setup's I/O patterns cost on the card
(through its file system, on Knulli exFAT through FUSE): random reads of a few sizes from a big
file not read lately (use one of a few GB, so that the page cache does not hold it), a sequential
read, a journal write as convert's (80 KiB, fsync, rename, the folder synced) and without the syncs,
a folder made and removed, a file opened, 100 MiB written in a few write sizes with and without
fsync. It writes only inside <dir> (made, left with nothing in it); docs/rust-setup.md has the
RG34XX-SP's figures."""
import os, random, sys, time

d, big = sys.argv[1], sys.argv[2]
os.makedirs(d, exist_ok=True)
size = os.path.getsize(big)
rnd = random.Random(7)


def ms(t):
    return f'{t * 1000:.1f} ms'


def rand_reads(n, k):
    fd = os.open(big, os.O_RDONLY)
    ts = []
    for _ in range(n):
        off = rnd.randrange(0, size - k) // 4096 * 4096
        t = time.perf_counter()
        os.pread(fd, k, off)
        ts.append(time.perf_counter() - t)
    os.close(fd)
    ts.sort()
    return f'median {ms(ts[len(ts) // 2])}, mean {ms(sum(ts) / len(ts))}'


for k in (4096, 16384, 65536, 262144, 1 << 20):
    print(f'random read {k >> 10} KiB: {rand_reads(30, k)}', flush=True)

fd = os.open(big, os.O_RDONLY)
off = rnd.randrange(0, size - (200 << 20)) // 4096 * 4096
t = time.perf_counter()
n = 0
while n < 200 << 20:
    n += len(os.pread(fd, 1 << 20, off + n))
dt = time.perf_counter() - t
os.close(fd)
print(f'sequential read 200 MiB, 1 MiB reads: {200 / dt:.1f} MiB/s', flush=True)

blob = os.urandom(80 << 10)
ts = []
for i in range(20):
    t = time.perf_counter()
    p = os.path.join(d, 'j.json.tmp')
    with open(p, 'wb') as f:
        f.write(blob)
        f.flush()
        os.fsync(f.fileno())
    os.rename(p, os.path.join(d, 'j.json'))
    dfd = os.open(d, os.O_RDONLY)
    try:
        os.fsync(dfd)
    except OSError:
        pass
    os.close(dfd)
    ts.append(time.perf_counter() - t)
ts.sort()
print(f'journal save (80 KiB, fsync, rename, folder fsync): median {ms(ts[10])}, mean {ms(sum(ts) / 20)}', flush=True)

ts = []
for i in range(20):
    t = time.perf_counter()
    p = os.path.join(d, 'j2.json.tmp')
    with open(p, 'wb') as f:
        f.write(blob)
    os.rename(p, os.path.join(d, 'j2.json'))
    ts.append(time.perf_counter() - t)
ts.sort()
print(f'journal save without syncs: median {ms(ts[10])}, mean {ms(sum(ts) / 20)}', flush=True)

ts = []
for i in range(20):
    t = time.perf_counter()
    p = os.path.join(d, 'w', 'textures')
    os.makedirs(p)
    os.rmdir(p)
    os.rmdir(os.path.join(d, 'w'))
    ts.append(time.perf_counter() - t)
ts.sort()
print(f'2 folders made and removed: median {ms(ts[10])}, mean {ms(sum(ts) / 20)}', flush=True)

ts = []
for i in range(50):
    t = time.perf_counter()
    os.close(os.open(big, os.O_RDONLY))
    os.stat(big + '.nothere') if os.path.exists(big + '.nothere') else None
    ts.append(time.perf_counter() - t)
ts.sort()
print(f'open+close and a missing file looked up: median {ms(ts[25])}', flush=True)

for bs in (32 << 10, 128 << 10, 1 << 20, 4 << 20):
    p = os.path.join(d, 'w.bin')
    buf = os.urandom(bs)
    t = time.perf_counter()
    with open(p, 'wb', buffering=0) as f:
        n = 0
        while n < 100 << 20:
            n += f.write(buf)
        t1 = time.perf_counter()
        os.fsync(f.fileno())
    dt = time.perf_counter() - t
    print(f'write 100 MiB in {bs >> 10} KiB writes: {100 / (t1 - t):.1f} MiB/s into the cache, {100 / dt:.1f} MiB/s with fsync', flush=True)
    os.unlink(p)
    time.sleep(2)

for n in ('j.json', 'j2.json'):
    try:
        os.unlink(os.path.join(d, n))
    except OSError:
        pass
