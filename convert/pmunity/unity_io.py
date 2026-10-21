"""Loading and saving Unity serialized files and UnityFS asset bundles with UnityPy."""
import gc
import os
import inspect
import textwrap

_patched = False


def _join_blocks(bundle, reader, blocks):
    """The bundle's decompressed blocks in one preallocated buffer (no copies)."""
    buf = bytearray(sum(b.uncompressedSize for b in blocks))
    pos = 0
    for i, b in enumerate(blocks):
        data = bundle.decompress_data(reader.read_bytes(b.compressedSize), b.uncompressedSize, b.flags, i)
        buf[pos:pos + len(data)] = data
        pos += len(data)
        del data
    return buf


def _node_view(reader, size):
    """A bundle node's bytes as a view into the decompressed buffer (no copy)."""
    view = getattr(reader, 'view', None)
    if view is None:
        return reader.read(size)
    pos = reader.Position
    reader.Position = pos + size
    return view[pos:pos + size]


def _recompile(module, cls, method, old, new, extra):
    """Recompiles cls.method with the source text old replaced by new (extra: names it needs)."""
    src = inspect.getsource(getattr(cls, method))
    i = src.find(old)
    if i < 0:
        return False                            # another UnityPy version: left as it is
    j = i + len(old)
    if old.endswith('('):                       # replace up to the matching closing parenthesis
        depth = 1
        while j < len(src) and depth:
            depth += {'(': 1, ')': -1}.get(src[j], 0)
            j += 1
    src = textwrap.dedent(src[:i] + new + src[j:])
    ns = dict(vars(module))
    ns.update(extra)
    exec(compile(src, module.__file__ + ' (pmunity)', 'exec'), ns)
    setattr(cls, method, ns[method])
    return True


def _patch_unitypy():
    """Opening a bundle, UnityPy (1.25) holds two copies of its decompressed contents: b"".join() of the
    decompressed blocks (every block plus the joined copy), then a copy of each file node read out of
    it. For the largest Cuphead bundle that is ~1.2 GB, more than a 1 GB handheld has. Recompiled
    here to decompress into one buffer and give the nodes views into it."""
    global _patched
    if _patched:
        return
    _patched = True
    import importlib
    BF = importlib.import_module('UnityPy.files.BundleFile')   # the modules (UnityPy.files re-exports
    FM = importlib.import_module('UnityPy.files.File')         # the classes under the same names)
    _recompile(BF, BF.BundleFile, 'read_fs', 'b"".join(', '_join_blocks(self, reader, m_BlocksInfo)', {'_join_blocks': _join_blocks})
    _recompile(FM, FM.File, 'read_files', 'reader.read(node.size)', '_node_view(reader, node.size)', {'_node_view': _node_view})


def _trim():
    """Hands freed heap memory back to the system (glibc keeps it otherwise, and a setup that
    converts hundreds of files one after the other grows until a 1 GB handheld runs out)."""
    import ctypes, sys
    if sys.platform.startswith('linux'):
        try:
            ctypes.CDLL(None).malloc_trim(0)
        except (OSError, AttributeError):
            pass


class NotOneSerializedFile(ValueError):
    """A bundle with no serialized file or several (scene bundles): not opened."""


class Loaded:
    """One data file opened with UnityPy: a plain serialized file (.assets, level, globalgamemanagers)
    or a UnityFS bundle holding one serialized file (CAB-...) and optionally its .resS."""

    def __init__(self, path, resource_dir=None, streams=None, spill_dir=None, spill_mb=64):
        """resource_dir: where the file's .resS / .resource streams are when they are not beside it
        (a conversion writing to an overlay folder rewrites the file, not its streams).
        streams: {name: file} for streams elsewhere under their own names; each is read from its
        file where needed, never loaded whole.
        spill_dir: an empty scratch folder on the output's file system. A bundle of more than
        spill_mb MB (uncompressed) is then unpacked there into its nodes (bigbundle.py) and opened
        from them: the same objects in little memory (UnityPy holds a whole bundle, twice while
        reading it). Write it with write_to()."""
        import os
        import UnityPy
        from . import bigbundle
        _patch_unitypy()
        self.path = path
        self.spilled = None                  # (bigbundle.Bundle, its serialized file's node)
        self._res, self._maps = {}, {}       # a spilled bundle's changed nodes; {file: (mmap, view)}
        load = path
        if spill_dir and spill_mb and bigbundle.size(path) > spill_mb << 20:
            b = bigbundle.unpack(path, spill_dir)
            if b:
                sfs = [n for n in b.nodes if n.flags & 4] or \
                    [n for n in b.nodes if not n.name.endswith(('.resS', '.resource'))]
                if len(sfs) != 1:
                    raise NotOneSerializedFile(f'{path}: expected one serialized file in the bundle, found {len(sfs)}')
                self.spilled = (b, sfs[0])
                streams = dict(streams or {}, **{n.name: n.path for n in b.nodes if n is not sfs[0]})
                load = sfs[0].path
        self.env = UnityPy.load(load)
        if streams:
            from UnityPy.streams import EndianBinaryReader
            for n, p in streams.items():
                self.env.register_cab(n, EndianBinaryReader(p))
        # a plain file's streams (beside it, else in resource_dir) are read from their files where
        # needed: UnityPy itself would read a whole .resS into memory at the first texture
        base = os.path.basename(path)
        stem = os.path.splitext(base)[0]
        for n in (base + '.resS', stem + '.resource', stem + '.resS'):
            if streams and n in streams:
                continue
            for d in (os.path.dirname(path), resource_dir):
                if d and os.path.isfile(os.path.join(d, n)):
                    from UnityPy.streams import EndianBinaryReader
                    self.env.register_cab(n, EndianBinaryReader(os.path.join(d, n)))
                    break
        f = self.env.file
        if self.spilled:
            if type(f).__name__ != 'SerializedFile':
                raise NotOneSerializedFile(f'{path}: its node {self.spilled[1].name} is not a serialized file')
            self.is_bundle, self.bundle = True, None
            self.cab_name, self.sf = self.spilled[1].name, f
            return
        self.is_bundle = type(f).__name__ == 'BundleFile'
        if self.is_bundle:
            self.bundle = f
            sfs = [(k, v) for k, v in f.files.items() if type(v).__name__ == 'SerializedFile']
            if len(sfs) != 1:
                raise NotOneSerializedFile(f'{path}: expected one serialized file in the bundle, found {len(sfs)}')
            self.cab_name, self.sf = sfs[0]
        else:
            self.bundle = None
            self.sf = f

    def objects(self, type_name=None):
        for pid, o in self.sf.objects.items():
            if type_name is None or o.type.name == type_name:
                yield pid, o

    def bundle_res(self, name):
        """A node (e.g. CAB-xxx.resS) inside the bundle as a memoryview (no copy), or None. A
        spilled bundle's node is its file, mapped (its pages stay the file's, not the process's)."""
        if self.spilled:
            import mmap
            n = next((n for n in self.spilled[0].nodes if n.name == name), None)
            if n is None:
                return None
            p = self._res.get(name, n.path)
            if p not in self._maps:
                with open(p, 'rb') as f:
                    if not os.fstat(f.fileno()).st_size:
                        return memoryview(b'')
                    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
                self._maps[p] = (m, memoryview(m))
            return self._maps[p][1]
        n = self.bundle.files.get(name)
        if n is None:
            return None
        if hasattr(n, 'view'):
            return n.view
        return memoryview(n.bytes if hasattr(n, 'bytes') else n)

    def set_bundle_res(self, name, data):
        """A node's new contents: bytes, or (a spilled bundle) the path of a file holding them."""
        if self.spilled:
            n = next(n for n in self.spilled[0].nodes if n.name == name)
            if isinstance(data, str):
                self._res[name] = data
            else:
                with open(n.path + '.new', 'wb') as f:
                    f.write(data)
                self._res[name] = n.path + '.new'
            return
        from UnityPy.streams import EndianBinaryReader
        old = self.bundle.files[name]
        r = EndianBinaryReader(bytes(data))
        r.flags = getattr(old, 'flags', 0)
        self.bundle.files[name] = r

    def save(self):
        """Serialized bytes of the whole file (bundles: chunked LZ4, which Unity reads in place)."""
        if self.spilled:
            raise RuntimeError(f'{self.path}: a spilled bundle is written with write_to()')
        if self.is_bundle:
            return self.bundle.save(packer='lz4')
        return self.sf.save()

    def write_to(self, job, rel):
        """The file, changed, into the job's output as <rel> (a spilled bundle: packed again from
        its node files, one chunk at a time)."""
        if not self.spilled:
            job.write(rel, self.save())
            return
        import os
        from . import bigbundle
        b, sf_node = self.spilled
        with open(sf_node.path + '.new', 'wb') as f:
            f.write(self.sf.save())
        nodes = [n._replace(path=sf_node.path + '.new') if n is sf_node else
                 n._replace(path=self._res.get(n.name, n.path)) for n in b.nodes]
        out = os.path.join(os.path.dirname(sf_node.path), 'bundle.new')
        bigbundle.pack(b, nodes, out)
        job.write_file(rel, out)

    def close(self):
        for m, v in self._maps.values():
            try:
                v.release()
                m.close()
            except BufferError:              # a slice of it still alive: unmapped with that
                pass
        self._maps = {}
        for r in list(getattr(self.env, 'cabs', {}).values()) if self.env else ():
            f = getattr(r, 'stream', None)          # streams registered from files
            if f is not None and hasattr(f, 'close'):
                f.close()
        self.env = self.sf = self.bundle = None
        gc.collect()
        _trim()
