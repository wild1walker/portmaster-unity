"""Digests of what the tools wrote for a game: run by regress.py as its own process, with Python's
-I (game data is read here; nothing is imported from the data's folders), the checkout under test
first on sys.path.

    python -I digest.py shaders <checkout> <data> <cfg.json> <work> <out.json> [--any-unity-version]
        the converter's shaders step on <data> with the config's "shaders" section, in this process:
        the real step (pmunity.shaders.step through pmunity's own step table), but each file it
        would write is digested instead (Loaded.write_to): nothing big is written (a spilled
        bundle's packed file is hashed and removed). The step's log lines are kept.
    python -I digest.py files <checkout> <dir> <out.json> [<rel>...]
        the same digests of files a real `pmunity convert` wrote into an overlay folder.
    python -I digest.py tree <dir> <out.json> [<skip glob>...]
        every file under <dir> but dot files and the globs (matched against each relative path):
        size and SHA-256 (android-data's tree), and an APK's or OBB's zip members.

Digests (all of the tools' own output; nothing of a game's content is kept, only hashes):
  file      SHA-256 of the whole written file
  gles3     per file, SHA-256 over its Shaders' GLES3 platform blobs (decompressed; the segments
            of 2019.3+ blobs with their lengths) and its ComputeShaders' GLES3 variants (canonical
            JSON), in path-id order: what the shaders step adds
  total     SHA-256 over the files' (path, digest) pairs in path order
"""
import hashlib
import json
import os
import sys
import time
import zipfile

GLES3 = 9            # ShaderBlobs platform: GLES3+ (shaders/blob.py PLATFORM_GLES3PLUS)
RENDERER_GLES3 = 11  # ComputeShader variant targetRenderer (shaders/compute.py)


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def sha256_file(path, bufsize=1 << 20):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _jsonable(o):
    if isinstance(o, (bytes, bytearray, memoryview)):
        return bytes(o).hex()
    raise TypeError(type(o).__name__)


def _objects(src):
    """(path id, object) of every object in a data file given as bytes or a path: a serialized
    file, or each serialized file of a UnityFS bundle. The file is read back as it was written,
    not from the converter's objects in memory."""
    import UnityPy
    env = UnityPy.load(src)
    f = env.file
    sfs = [v for v in f.files.values() if type(v).__name__ == 'SerializedFile'] \
        if type(f).__name__ == 'BundleFile' else [f]
    for sf in sfs:
        for pid, o in sorted(sf.objects.items()):
            yield pid, o


def gles3_digest(src):
    """(digest, number of Shaders with GLES3 programs, number of ComputeShaders with a GLES3
    variant) of one data file (bytes or a path)."""
    from pmunity.shaders import blob as B
    h = hashlib.sha256()
    ns = nc = 0
    for pid, o in _objects(src):
        kind = o.type.name
        if kind == 'Shader':
            t = o.read_typetree()
            if GLES3 not in t.get('platforms', ()):
                continue
            data = B.ShaderBlobs(t).platforms[GLES3]
            if isinstance(data, list):
                blob = b''.join(len(s).to_bytes(4, 'little') + bytes(s) for s in data)
            else:
                blob = bytes(data)
            d = sha256_bytes(blob)
            ns += 1
        elif kind == 'ComputeShader':
            t = o.read_typetree()
            vs = [v for v in t.get('variants', ()) if v.get('targetRenderer') == RENDERER_GLES3]
            if not vs:
                continue
            d = sha256_bytes(json.dumps(vs, sort_keys=True, default=_jsonable).encode())
            nc += 1
        else:
            continue
        h.update(f'{pid} {kind} {d}\n'.encode())
    return h.hexdigest(), ns, nc


def total(files):
    h = hashlib.sha256()
    for rel in sorted(files):
        f = files[rel]
        h.update(f"{rel}\0{f['file']}\0{f['gles3']}\n".encode())
    return h.hexdigest()


def _setup(checkout):
    sys.path.insert(0, os.path.join(os.path.abspath(checkout), 'convert'))


def cmd_shaders(checkout, data, cfg_path, work, out, *flags):
    _setup(checkout)
    from pmunity import unity_io
    from pmunity.job import Job
    from pmunity import __main__ as M
    cfg = json.load(open(cfg_path))
    cfg.setdefault('version', 1)
    files, log = {}, []
    orig = unity_io.Loaded.write_to

    class Sink:
        """The job write_to() writes into: the bytes or the packed file are hashed, never kept."""
        def __init__(self, rec):
            self.rec = rec

        def write(self, rel, data):
            data = bytes(data)
            self.rec['file'], self.rec['size'] = sha256_bytes(data), len(data)
            self.rec['gles3'], self.rec['shaders'], self.rec['compute'] = gles3_digest(data)

        def write_file(self, rel, path):
            self.rec['file'], self.rec['size'] = sha256_file(path), os.path.getsize(path)
            self.rec['gles3'], self.rec['shaders'], self.rec['compute'] = gles3_digest(path)
            os.unlink(path)

    def write_to(self, job, rel):
        orig(self, Sink(files.setdefault(rel, {})), rel)

    unity_io.Loaded.write_to = write_to
    M._check_data(data, '--any-unity-version' in flags)      # (as `pmunity convert` does first)
    os.makedirs(work, exist_ok=True)
    job = Job(data, work, cfg, log=lambda m: (log.append(m), print(m, flush=True)))
    t0 = time.time()

    class A:          # the parsed command line the step table expects (only textures reads it)
        only = None
    M._builtin('shaders')(job, A)
    json.dump({'files': files, 'total': total(files), 'log': log, 'seconds': round(time.time() - t0, 1)},
              open(out, 'w'), indent=1, sort_keys=True)


def cmd_files(checkout, root, out, rels):
    _setup(checkout)
    from pmunity import unity_io
    unity_io._patch_unitypy()
    if not rels:
        rels = sorted(os.path.relpath(os.path.join(d, n), root) for d, _, ns in os.walk(root) for n in ns
                      if not n.startswith('.'))
    files = {}
    for rel in rels:
        p = os.path.join(root, rel)
        rec = {'file': sha256_file(p), 'size': os.path.getsize(p)}
        try:
            rec['gles3'], rec['shaders'], rec['compute'] = gles3_digest(p)
        except Exception as ex:          # not a serialized file or bundle (or one UnityPy cannot open)
            rec['gles3'], rec['shaders'], rec['compute'] = '-', 0, 0
            rec['note'] = type(ex).__name__
        files[rel] = rec
    json.dump({'files': files, 'total': total(files)}, open(out, 'w'), indent=1, sort_keys=True)


def zip_members(path):
    """(digest, entries, bytes) of a zip's central directory: names, CRC-32s, sizes, compression,
    and each member's data offset (alignment is part of what android-data's packer promises)."""
    h = hashlib.sha256()
    n = size = 0
    with zipfile.ZipFile(path) as z:
        for i in sorted(z.infolist(), key=lambda i: i.filename):
            h.update(f'{i.filename}\0{i.CRC:08x}\0{i.file_size}\0{i.compress_type}\0{i.header_offset}\n'.encode())
            n += 1
            size += i.file_size
    return h.hexdigest(), n, size


def cmd_tree(root, out, skip=()):
    import fnmatch
    files = {}
    zips = {}
    h = hashlib.sha256()
    nbytes = 0
    for d, dirs, names in os.walk(root):
        dirs[:] = sorted(x for x in dirs if not x.startswith('.'))
        for n in sorted(names):
            p = os.path.join(d, n)
            rel = os.path.relpath(p, root)
            if n.startswith('.') or any(fnmatch.fnmatch(rel, g) for g in skip) or \
                    (os.path.islink(p) and not os.path.exists(p)):
                continue
            s = os.path.getsize(p)
            if n.endswith(('.apk', '.obb')):
                try:
                    zd, ze, zb = zip_members(p)
                    zips[rel] = {'members': zd, 'entries': ze, 'bytes': zb}
                except zipfile.BadZipFile:
                    zips[rel] = {'members': 'bad zip', 'entries': 0, 'bytes': 0}
                d_ = zips[rel]['members']        # (the members' CRCs stand for the zip's contents)
            else:
                d_ = sha256_file(p)
            files[rel] = [s, d_]
            nbytes += s
            h.update(f'{rel}\0{s}\0{d_}\n'.encode())
    json.dump({'files': files, 'zips': zips, 'total': h.hexdigest(), 'count': len(files), 'bytes': nbytes},
              open(out, 'w'), indent=1, sort_keys=True)


def main(argv):
    cmd = argv[0] if argv else ''
    if cmd == 'shaders' and len(argv) >= 6:
        return cmd_shaders(*argv[1:])
    if cmd == 'files' and len(argv) >= 4:
        return cmd_files(argv[1], argv[2], argv[3], argv[4:])
    if cmd == 'tree' and len(argv) >= 3:
        return cmd_tree(argv[1], argv[2], argv[3:])
    sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
