"""Golden data for texcodec (setup-rs/crates/texcodec): runs the textures step's own per-texture
code (pmunity/textures.py: choose, strip_blocks, _prepare, Astc.encode, as convert_file calls them
with pipeline off) on a game's data and writes, for each Texture2D the step looks at, its fields,
its raw pixels (converted ones) and what the step made of it, for texcodec's golden test.

  textures_golden.py dump <data dir> <config.json> <out dir> [--set textures.k=v]... [--files rx]
                          [--range a:b] [--no-raw] [--levels]
      (--list: the decisions only, nothing converted)
  textures_golden.py one <data dir> <config.json> <rel> <path id> [--set ...] [--raw in.bin] [--blob out.bin]
      one texture alone: time and peak memory (VmHWM after clear_refs) of its conversion
  textures_golden.py peek <data dir> <config.json> <rel> <path id>...
      textures as a converted data folder has them: fields and SHA-256 of their image data

<out dir>/manifest.jsonl: a header line (settings, threads, the guide's path), then one line a
texture; raw pixels in <out dir>/raw/<n>.bin. Run with the reference Python (PYTHONPATH=<worktree>/
convert:/opt/pylibs). Nothing here names a machine's paths: everything comes from the arguments.
"""
import hashlib
import json
import os
import re
import sys
import time


def load_cfg(path, sets):
    cfg = json.load(open(path))
    cfg.setdefault('version', 1)
    for kv in sets:
        k, _, v = kv.partition('=')
        try:
            v = json.loads(v)
        except ValueError:
            pass
        cfg.setdefault(k.partition('.')[0], {})[k.partition('.')[2]] = v
    return cfg


def sha(b):
    return hashlib.sha256(b).hexdigest()


class Run:
    """The step's state for one run: settings, encoder (with the guide), sharp regex."""

    def __init__(self, data, cfg):
        from pmunity import textures as T
        from pmunity.job import Job
        self.T = T
        self.job = Job.reader(data, cfg, log=lambda m: print(m, file=sys.stderr))
        c = self.c = dict(T.DEFAULTS, **cfg.get('textures', {}))
        self.sharp_rx = re.compile('(?i)' + c['sharp_names']) if c['sharp_names'] else None
        self.enc = T.Astc(c['quality'], c['threads'], c['mode'], c['guided_max_err'], c['guided_line'],
                          c['guided_near'], self.job.log)
        if self.enc.mode == 'guided' and c['guide']:
            from pmunity import astc_guide
            self.enc.guide = astc_guide.Reader(c['guide'], c, self.job.log)

    def files(self):
        return self.T.texture_files(self.job, self.c['files'])

    def open(self, rel, work):
        from pmunity.unity_io import Loaded
        src = self.job.path_in(rel)
        res_dir = os.path.dirname(os.path.join(self.job.src, rel))
        lf = Loaded(src, res_dir, spill_dir=work, spill_mb=self.c['spill_mb'])
        handles = {}

        def read_stream(path, off, size):
            name = os.path.basename(path)
            if lf.is_bundle:
                node = lf.bundle_res(name)
                return bytes(node[off:off + size])
            p = self.job.path_in(os.path.join(os.path.dirname(rel), name))
            if not os.path.exists(p):
                p = os.path.join(res_dir, name)
            if p not in handles:
                handles[p] = open(p, 'rb')
            handles[p].seek(off)
            return handles[p].read(size)
        return lf, read_stream, handles

    def texture(self, rel, pid, o, t, read_stream, unity, levels_out=None):
        """convert_file's work on one texture (pipeline off). Returns (record, raw or None)."""
        T, c, enc = self.T, self.c, self.enc
        sd = t.get('m_StreamData') or {}
        streamed = bool(sd.get('size'))
        fmt_name = T._format_name(t)
        rec = dict(rel=rel, pid=pid, name=t['m_Name'], width=t['m_Width'], height=t['m_Height'],
                   format=t['m_TextureFormat'], mips=t.get('m_MipCount'), readable=bool(t.get('m_IsReadable')),
                   dimension=t.get('m_TextureDimension'), images=t.get('m_ImageCount'),
                   platform=int(getattr(o, 'platform', 3716)), blob=bytes(t.get('m_PlatformBlob') or b'').hex(),
                   version=list(getattr(o, 'version', (0, 0, 0, 0))), unity=unity,
                   streamed=streamed)
        if not streamed and (not c['inline'] or not t.get('image data')):
            rec['outcome'] = 'skip'
            return rec, None
        ch = T.choose(t, fmt_name, c, self.sharp_rx)
        if ch is None:
            rec['outcome'] = 'keep'
            return rec, None
        before = (list(enc.counts), enc.guide.used if enc.guide else 0, enc.guide.missed if enc.guide else 0)
        t0 = time.perf_counter()
        raw = read_stream(sd['path'], sd['offset'], sd['size']) if streamed else bytes(t['image data'])
        rec['choice'] = dict(block=list(ch.block), width=ch.width, height=ch.height, alpha=ch.alpha, mips=ch.mips)
        if getattr(self, 'list_only', False):
            rec['outcome'] = 'convert'
            return rec, None
        blob = None
        if c['strips'] and ch.mips == 1 and t.get('m_MipCount', 1) == 1 and \
                (ch.width, ch.height) == (t['m_Width'], t['m_Height']) and ch.width * ch.height >= T.STRIP_MIN_PX:
            blob = T.strip_blocks(o, t, raw, ch, enc, fmt_name)
            if blob is not None:
                rec['path'], nlevels = 'strips', 1
        if blob is None:
            levels, seed = T._prepare(o, ch, fmt_name, self.prep, enc.threads, True)
            if levels_out is not None:
                levels_out.extend(levels)
            rec['levels'] = [sha(lv.tobytes()) for lv in levels]
            blob = b''.join(enc.encode(level, ch.block, seed if i == 0 else None, f'{rel}|{pid}|{i}')
                            for i, level in enumerate(levels))
            rec['path'], nlevels = 'prepare', len(levels)
            rec['seed'] = seed is not None
        rec['seconds'] = round(time.perf_counter() - t0, 3)
        self.last_blob = blob
        rec['outcome'] = 'convert'
        rec['out'] = dict(format=T.astc_format_id(ch.block, ch.alpha, rec['unity']), width=ch.width,
                          height=ch.height, mips=nlevels, size=len(blob), sha256=sha(blob))
        rec['counts'] = [a - b for a, b in zip(enc.counts, before[0])]
        if enc.guide:
            rec['guide'] = [enc.guide.used - before[1], enc.guide.missed - before[2]]
        return rec, raw


def main(argv):
    import argparse
    import shutil
    import tempfile
    from concurrent.futures import ThreadPoolExecutor
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=('dump', 'one', 'peek'))
    ap.add_argument('data')
    ap.add_argument('config')
    ap.add_argument('rest', nargs='*')
    ap.add_argument('--set', action='append', default=[])
    ap.add_argument('--files', default=None)
    ap.add_argument('--range', default=None)
    ap.add_argument('--no-raw', action='store_true')
    ap.add_argument('--raw', default=None)
    ap.add_argument('--blob', default=None, help='one: write the converted data here')
    ap.add_argument('--list', action='store_true', help='decisions only, nothing converted')
    a = ap.parse_args(argv)
    cfg = load_cfg(a.config, a.set)
    run = Run(a.data, cfg)
    run.list_only = a.list
    run.prep = ThreadPoolExecutor(max_workers=run.enc.threads) if run.c.get('prepare_split', True) else None
    work = tempfile.mkdtemp(prefix='golden-')
    try:
        if a.cmd == 'peek':
            rel, pids = a.rest[0], {int(p) for p in a.rest[1:]}
            lf, read_stream, handles = run.open(rel, os.path.join(work, 'nodes'))
            for p, o in lf.objects('Texture2D'):
                if p in pids:
                    t = o.read_typetree()
                    sd = t.get('m_StreamData') or {}
                    data = read_stream(sd['path'], sd['offset'], sd['size']) if sd.get('size') else bytes(t['image data'])
                    print(json.dumps(dict(rel=rel, pid=p, name=t['m_Name'], format=t['m_TextureFormat'],
                                          width=t['m_Width'], height=t['m_Height'], mips=t.get('m_MipCount'),
                                          size=t.get('m_CompleteImageSize'), sha256=sha(data), len=len(data))))
            lf.close()
            return
        if a.cmd == 'one':
            rel, pid = a.rest[0], int(a.rest[1])
            lf, read_stream, handles = run.open(rel, os.path.join(work, 'nodes'))
            for p, o in lf.objects('Texture2D'):
                if p != pid:
                    continue
                t = o.read_typetree()
                import gc
                gc.collect()
                rss = int(re.search(r'VmRSS:\s+(\d+)', open('/proc/self/status').read()).group(1))
                with open('/proc/self/clear_refs', 'w') as f:
                    f.write('5')
                t0 = time.perf_counter()
                rec, raw = run.texture(rel, pid, o, t, read_stream, getattr(lf.sf, 'unity_version', None))
                dt = time.perf_counter() - t0
                hwm = int(re.search(r'VmHWM:\s+(\d+)', open('/proc/self/status').read()).group(1))
                rec['time'] = round(dt, 3)
                rec['rss_before_mb'] = round(rss / 1024, 1)
                rec['peak_extra_mb'] = round((hwm - rss) / 1024, 1)
                if a.raw and raw is not None:
                    open(a.raw, 'wb').write(raw)
                if a.blob and rec['outcome'] == 'convert':
                    open(a.blob, 'wb').write(run.last_blob)
                print(json.dumps(rec))
            return
        out = a.rest[0]
        os.makedirs(os.path.join(out, 'raw'), exist_ok=True)
        files = run.files()
        if a.files:
            files = [f for f in files if re.search(a.files, f)]
        if a.range:
            lo, hi = (int(x) if x else None for x in a.range.split(':'))
            files = files[lo:hi]
        man = open(os.path.join(out, 'manifest.jsonl'), 'w')
        c = run.c
        man.write(json.dumps(dict(header=True, textures=c, threads=run.enc.threads, mode=run.enc.mode,
                                  guide=c['guide'] if run.enc.guide else '', guide_ok=bool(run.enc.guide and run.enc.guide.ok),
                                  files=len(files))) + '\n')
        n = 0
        for fi, rel in enumerate(files):
            nodes = os.path.join(work, 'nodes')
            shutil.rmtree(nodes, ignore_errors=True)
            os.makedirs(nodes)
            from pmunity.unity_io import NotOneSerializedFile
            try:
                lf, read_stream, handles = run.open(rel, nodes)
            except NotOneSerializedFile as ex:
                print(f'{rel}: left as it is ({ex})', file=sys.stderr)
                continue
            items = [(pid, o, o.read_typetree()) for pid, o in lf.objects('Texture2D')]
            for pid, o, t in items:
                rec, raw = run.texture(rel, pid, o, t, read_stream, getattr(lf.sf, 'unity_version', None))
                rec['n'] = n
                if raw is not None and not a.no_raw:
                    open(os.path.join(out, 'raw', f'{n}.bin'), 'wb').write(raw)
                    rec['raw'] = f'raw/{n}.bin'
                man.write(json.dumps(rec) + '\n')
                man.flush()
                n += 1
            for h in handles.values():
                h.close()
            lf.close()
            print(f'[{fi + 1}/{len(files)}] {rel}: {len(items)} textures', file=sys.stderr, flush=True)
        man.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
        if run.prep:
            run.prep.shutdown()


if __name__ == '__main__':
    main(sys.argv[1:])
