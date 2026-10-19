"""libunity.so symbols for the analysis tools, from the unstripped libunity.sym.so that comes with
Unity's Android player (Variations/mono/Release/Symbols/armeabi-v7a/libunity.sym.so).

Looked up in order: $UA_SYMS (a text file of 'address type name' lines, as `nm -C` prints them),
syms_libunity.txt next to these scripts, else built from $UA_LIBUNITY_SYM with pyelftools and
saved as syms_libunity.txt (not committed: it is derived from Unity's binary)."""
import bisect, os, re

HERE = os.path.dirname(os.path.abspath(__file__))


def _build(sym_so, out):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection
    rows = []
    with open(sym_so, 'rb') as f:
        elf = ELFFile(f)
        for sec in elf.iter_sections():
            if isinstance(sec, SymbolTableSection):
                for s in sec.iter_symbols():
                    if s['st_info']['type'] == 'STT_FUNC' and s['st_value']:
                        rows.append((s['st_value'] & ~1, s.name))
    import subprocess
    for cmd in (['c++filt', '-n'], ['c++filt']):          # demangled, like nm -C (macOS c++filt needs -n)
        try:
            names = subprocess.run(cmd, input='\n'.join(n for _, n in rows), capture_output=True, text=True).stdout.split('\n')
        except OSError:
            continue
        if len(names) >= len(rows) and any(n.startswith(('_Z', '__Z')) is False and '::' in n for n in names[:2000]):
            rows = [(a, names[i]) for i, (a, _) in enumerate(rows)]
            break
    with open(out, 'w') as f:
        for a, n in sorted(rows):
            f.write(f'{a:08x} T {n}\n')


def load():
    path = os.environ.get('UA_SYMS') or os.path.join(HERE, 'syms_libunity.txt')
    if not os.path.exists(path):
        so = os.environ.get('UA_LIBUNITY_SYM')
        if not so:
            return [], []
        _build(so, path)
    syms = []
    for line in open(path, errors='replace'):
        p = line.rstrip('\n').split(' ', 2)
        if len(p) == 3 and p[1] in 'tTwW':
            try:
                syms.append((int(p[0], 16), p[2]))
            except ValueError:
                pass
    syms.sort()
    return syms, [s[0] for s in syms]


def namer(short=True):
    syms, addrs = load()

    def name(tok):
        m = re.match(r'(\S+)\+0x([0-9a-f]+)$', tok)
        if not m:
            return tok
        mod, off = m.group(1), int(m.group(2), 16) & ~1
        if mod == 'libunity.so' and addrs:
            i = bisect.bisect_right(addrs, off) - 1
            if i >= 0:
                n = syms[i][1]
                return re.sub(r'\(.*', '', n)[:90] if short else f'{n[:150]} +{off - syms[i][0]:#x}'
        return f'{mod}+0x{off:x}'
    return name
