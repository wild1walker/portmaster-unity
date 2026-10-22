"""Function names for module+offset addresses, from ELF files with symbols: Unity's symbol files
(Variations/<backend>/Release/Symbols/<abi>/libunity.sym.so, libmonobdwgc-2.0.sym.so, libmain.sym.so,
as `pmunity player fetch --symbols` downloads them), an unstripped ua_host, or any library copied
from a handheld (its exported names only, when it is stripped).

    names = ElfNames(dirs=['.../Symbols/armeabi-v7a'], files=['runtime/host/out/ua_host'])
    names('libunity.so', 0x4a1c2)  ->  'PlayerLoop'  (None when unknown)

A folder's <name>.sym.so stands for module <name>.so, any other ELF file for its own name. Tables
are cached next to these scripts as syms_<module>_<hash>.txt (gitignored: derived from Unity's or
the device's binaries). libunity.so also comes from unitysyms.py's sources ($UA_SYMS,
syms_libunity.txt, $UA_LIBUNITY_SYM) when no folder or file names it."""
import bisect, hashlib, os, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def _demangle(names):
    for cmd in (['c++filt', '-n'], ['c++filt']):          # macOS c++filt needs -n
        try:
            out = subprocess.run(cmd, input='\n'.join(names), capture_output=True, text=True).stdout.split('\n')
        except OSError:
            continue
        if len(out) >= len(names) and (not any(n.startswith('_Z') for n in names) or
                                        any(not o.startswith(('_Z', '__Z')) for o, n in zip(out, names) if n.startswith('_Z'))):
            return out[:len(names)]
    return names


def _build(path, out):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection
    rows = {}
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        for sec in elf.iter_sections():
            if not isinstance(sec, SymbolTableSection):
                continue
            for s in sec.iter_symbols():
                if s['st_info']['type'] in ('STT_FUNC', 'STT_GNU_IFUNC') and s['st_value'] and s['st_shndx'] != 'SHN_UNDEF':
                    a = s['st_value'] & ~1                     # Thumb functions have bit 0 set
                    if a not in rows or (sec.name == '.symtab' and rows[a][1] == 0):
                        rows[a] = (s.name, s['st_size'])
    addrs = sorted(rows)
    names = _demangle([rows[a][0] for a in addrs])
    with open(out + '.tmp', 'w') as f:
        for a, n in zip(addrs, names):
            f.write(f'{a:x} {rows[a][1]:x} {n}\n')
    os.replace(out + '.tmp', out)


def load(path):
    """[(address, size, name)] of an ELF file's functions, sorted (cached)"""
    st = os.stat(path)
    key = hashlib.sha1(f'{os.path.abspath(path)}:{st.st_size}:{int(st.st_mtime)}'.encode()).hexdigest()[:10]
    cache = os.path.join(HERE, f'syms_{os.path.basename(path)}_{key}.txt')
    if not os.path.exists(cache):
        _build(path, cache)
    rows = []
    for line in open(cache, errors='replace'):
        p = line.rstrip('\n').split(' ', 2)
        if len(p) == 3:
            rows.append((int(p[0], 16), int(p[1], 16), p[2]))
    return rows


def module_name(path):
    b = os.path.basename(path)
    return b[:-len('.sym.so')] + '.so' if b.endswith('.sym.so') else b


class ElfNames:
    def __init__(self, dirs=(), files=(), short=True):
        self.short = short
        self.sources = {}
        for d in dirs:
            for f in sorted(os.listdir(d)):
                p = os.path.join(d, f)
                if os.path.isfile(p) and (f.endswith('.so') or '.so.' in f):
                    self.sources.setdefault(module_name(p), p)
        for p in files:
            self.sources[module_name(p)] = p
        self.tables = {}

    def _table(self, mod):
        if mod not in self.tables:
            rows = []
            if mod in self.sources:
                try:
                    rows = load(self.sources[mod])
                except Exception as e:                       # not an ELF file, no pyelftools, ...
                    print(f'(symbols of {mod}: {e})')
            elif mod == 'libunity.so':
                try:
                    import unitysyms
                    syms, _ = unitysyms.load()
                    rows = [(a, 0, n) for a, n in syms]
                except Exception:
                    rows = []
            self.tables[mod] = (rows, [r[0] for r in rows])
        return self.tables[mod]

    def has(self, mod):
        return bool(self._table(mod)[0])

    def __call__(self, mod, off):
        rows, addrs = self._table(mod)
        i = bisect.bisect_right(addrs, off & ~1) - 1
        if i < 0:
            return None
        a, size, n = rows[i]
        if size and off - a >= size + 4:                    # past its end: not this function
            return None
        if self.short:
            n = re.sub(r'\(.*', '', n)[:100]
        return n
