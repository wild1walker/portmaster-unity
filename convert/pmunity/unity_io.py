"""Loading and saving Unity serialized files and UnityFS asset bundles with UnityPy."""
import gc
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


class Loaded:
    """One data file opened with UnityPy: a plain serialized file (.assets, level, globalgamemanagers)
    or a UnityFS bundle holding one serialized file (CAB-...) and optionally its .resS."""

    def __init__(self, path):
        import UnityPy
        _patch_unitypy()
        self.path = path
        self.env = UnityPy.load(path)
        f = self.env.file
        self.is_bundle = type(f).__name__ == 'BundleFile'
        if self.is_bundle:
            self.bundle = f
            sfs = [(k, v) for k, v in f.files.items() if type(v).__name__ == 'SerializedFile']
            if len(sfs) != 1:
                raise ValueError(f'{path}: expected one serialized file in the bundle, found {len(sfs)}')
            self.cab_name, self.sf = sfs[0]
        else:
            self.bundle = None
            self.sf = f

    def objects(self, type_name=None):
        for pid, o in self.sf.objects.items():
            if type_name is None or o.type.name == type_name:
                yield pid, o

    def bundle_res(self, name):
        """A node (e.g. CAB-xxx.resS) inside the bundle as a memoryview (no copy), or None."""
        n = self.bundle.files.get(name)
        if n is None:
            return None
        if hasattr(n, 'view'):
            return n.view
        return memoryview(n.bytes if hasattr(n, 'bytes') else n)

    def set_bundle_res(self, name, data):
        from UnityPy.streams import EndianBinaryReader
        old = self.bundle.files[name]
        r = EndianBinaryReader(bytes(data))
        r.flags = getattr(old, 'flags', 0)
        self.bundle.files[name] = r

    def save(self):
        """Serialized bytes of the whole file (bundles: chunked LZ4, which Unity reads in place)."""
        if self.is_bundle:
            return self.bundle.save(packer='lz4')
        return self.sf.save()

    def close(self):
        self.env = self.sf = self.bundle = None
        gc.collect()
