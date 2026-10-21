"""`python -m pmunity il2cpp`: a Mono desktop game's managed code compiled ahead of time with
Unity's IL2CPP for the IL2CPP Android player of the same Unity version. Unity's 64-bit ARM
Android players (arm64-v8a) are IL2CPP players only; this is how a Mono game reaches them.

    python -m pmunity il2cpp <data> <out> [--abi arm64-v8a|armeabi-v7a] [--pkg DIR] [--home DIR]
                             [--jobs N] [--strip low|none] [--keep-cpp] [--dry-run]
    python -m pmunity il2cpp fetch <version> [--abi ...] [--pkg DIR] [--home DIR]   (toolchain only)

<data>: the game's desktop data folder (<Game>_Data, or <Game>.app/Contents/Resources/Data; read
only). <out> gets
  lib/<abi>/libil2cpp.so                        the game's code and the IL2CPP runtime (stripped)
  Data/Managed/Metadata/global-metadata.dat     -> assets/bin/Data/Managed/... in the APK
  Data/Managed/Resources/*-resources.dat        (`android-data --il2cpp <out>` puts them there)
  il2cpp.json                                   versions, assemblies, metadata version, options
Then `pmunity android-data --il2cpp <out> --pkg <the IL2CPP player package> ...` packs the data.

What it uses, all fetched once into --home (default $PMUNITY_IL2CPP_HOME, else
~/.cache/pmunity/il2cpp) with HTTP range requests, never whole installers:
  <ver>/editor/il2cpp/...                       Editor/Data/il2cpp of the Windows editor installer
                                                (UnitySetup64-<ver>.exe: non-solid NSIS, nsis.py):
                                                il2cpp + UnityLinker (.NET), libil2cpp's sources
  <ver>/editor/MonoBleedingEdge/lib/mono/unityaot/   the class libraries IL2CPP builds use
  ndk-<rev>/...                                 the Android NDK's target files only (sysroot,
                                                libc++, libgcc) from Google's zip; its x86-64
                                                compilers are replaced by tools/il2cpp/ndk-clang.sh
  --pkg (default <home>/<ver>/player-<abi>)     the IL2CPP Android player (`pmunity player fetch
                                                --backend il2cpp`) + Variations/il2cpp/Managed
                                                (the IL2CPP UnityEngine assemblies) and, from
                                                Unity 2020.1, StaticLibs/<abi>/baselib.a
  root/usr, root/etc                            .NET 8, Mono, clang/lld 14 from Ubuntu 24.04's archive
                                                (tools/il2cpp/root.sh), bind-mounted over a stock
                                                ubuntu:24.04 container's own: no image is built
il2cpp and the C++ build run in Docker that way, with every file in --home and <out>. See
docs/il2cpp.md: heavy (minutes of CPU, ~1 GB of build files), for a PC or Mac, and its result is
the user's own build of their game, never something to publish.

Unity's files stay where the porter downloads them; nothing of them is in this repository.
"""
import argparse
import concurrent.futures
import fnmatch
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import zlib

from . import unity_version as UV

ABIS = {'arm64-v8a': ('ARM64', 'aarch64-linux-android', 21),
        'armeabi-v7a': ('ARMv7', 'arm-linux-androideabi', 19)}


def api_level(rev, abi):
    """The Android API level il2cpp's Android toolchain builds for: 21 on arm64; on armv7 16 with
    NDK r16b (Unity 2017.4 - 2019.2), 19 from r19 (2019.3+)."""
    return 16 if abi == 'armeabi-v7a' and rev == 'r16b' else ABIS[abi][2]


IMAGE = 'ubuntu:24.04'                   # stock; the tools are in <home>/root (tools/il2cpp/root.sh)
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, '..', '..', 'tools', 'il2cpp'))
EDITOR_API = 'https://services.api.unity.com/unity/editor/release/v1/releases'
EDITOR_CDN = 'https://download.unity3d.com/download_unity/{rev}/Windows64EditorInstaller/UnitySetup64-{ver}.exe'
NDK_BASE = 'https://dl.google.com/android/repository/'
# the NDK each Unity line's il2cpp checks for (AndroidNDKUtilities: it wants that revision or newer;
# the Android Build Support of the line installs exactly this one)
NDKS = [  # (first Unity line, name, Pkg.Revision, zip)
    ((2017, 1), 'r16b', '16.1.4479499', 'android-ndk-r16b-linux-x86_64.zip'),
    ((2019, 3), 'r19', '19.0.5232133', 'android-ndk-r19-linux-x86_64.zip'),
    ((2021, 1), 'r21d', '21.3.6528147', 'android-ndk-r21d-linux-x86_64.zip'),
    ((2022, 2), 'r23b', '23.1.7779620', 'android-ndk-r23b-linux.zip'),
]
METADATA_MAGIC = 0xFAB11BAF


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def ndk_for(version):
    best = None
    for first, name, rev, z in NDKS:
        if version.line >= first:
            best = (name, rev, z)
    if best is None:
        raise UV.Unsupported(f'Unity {version}: IL2CPP Android builds of this tool start at 2017.4')
    return best


# --------------------------------------------------------------------------- range reads

def _get(url, off, n, tries=6):
    if n <= 0:
        return b''
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={'Range': 'bytes=%d-%d' % (off, off + n - 1)})
            with urllib.request.urlopen(req, timeout=180) as r:
                if r.status != 206:
                    raise IOError('server ignored the Range request (status %d)' % r.status)
                data = r.read()
            if len(data) != n:
                raise IOError('short read %d of %d' % (len(data), n))
            return data
        except OSError as ex:
            if attempt == tries - 1:
                raise
            log(f'  retry {attempt + 1} at {off}: {ex}')
            time.sleep(3 * (attempt + 1))


def _spans(items, gap=1 << 16, limit=24 << 20):
    """[(off, size, key)] sorted -> [(start, end, [(off, size, key)])] with nearby items merged into
    one read (gaps up to `gap` bytes are read and dropped)."""
    out = []
    for off, size, key in sorted(items):
        if out and off - out[-1][1] <= gap and off + size - out[-1][0] <= limit:
            s, e, lst = out[-1]
            out[-1] = (s, max(e, off + size), lst + [(off, size, key)])
        else:
            out.append((off, off + size, [(off, size, key)]))
    return out


def _parallel(fn, jobs, workers=6):
    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
        for r in ex.map(fn, jobs):
            yield r


def _write(path, data, mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + '.part', 'wb') as f:
        f.write(data)
    os.replace(path + '.part', path)
    if mode:
        os.chmod(path, mode)


def nsis_fetch(src, select, dest, log=log):
    """Files of a non-solid NSIS installer (local or URL, or an nsis.Nsis) chosen by select(nsis path) -> relative
    output path or None, written under dest. Neighbouring blocks are read together and several
    reads run at once. Returns the relative paths written."""
    from . import nsis
    ns = src if isinstance(src, nsis.Nsis) else nsis.Nsis(src)
    src = ns.s.src
    want = {}
    for path, off, size in ns.files():
        rel = select(path)
        if rel:
            want.setdefault(off, (size, []))[1].append(rel)
    if not want:
        return []
    items = [(ns.data_base + off, size + 4 + 64, off) for off, (size, _) in want.items()]
    got, total = [], 0

    def read_span(span):
        start, end, lst = span
        buf = _get(src, start, end - start) if src.startswith(('http://', 'https://')) else _local(src, start, end - start)
        return start, buf, lst

    def block(buf, p, off):
        w, = struct.unpack_from('<I', buf, p)
        n = w & 0x7fffffff
        blk = buf[p + 4:p + 4 + n]
        if len(blk) < n:                    # the size estimate was short (last block): read it alone
            blk = _get(src, ns.data_base + off + 4, n) if src.startswith('http') else _local(src, ns.data_base + off + 4, n)
        return nsis.decompress(blk) if w & 0x80000000 else blk

    for start, buf, lst in _parallel(read_span, _spans(items)):
        total += len(buf)
        for absoff, _, off in lst:
            data = block(buf, absoff - start, off)
            for rel in want[off][1]:
                _write(os.path.join(dest, rel), data)
                got.append(rel)
    log(f'  {len(got)} files, {total / 1e6:.1f} MB read')
    return got


def _local(path, off, n):
    with open(path, 'rb') as f:
        f.seek(off)
        return f.read(n)


class RemoteZip:
    """The central directory of a zip archive at a URL (or a local path), read with range requests;
    `extract` fetches only the chosen members (stored or deflated; zip64 sizes and offsets)."""

    def __init__(self, src):
        self.src = src
        if src.startswith(('http://', 'https://')):
            req = urllib.request.Request(src, method='HEAD')
            with urllib.request.urlopen(req, timeout=60) as r:
                self.size = int(r.headers['Content-Length'])
        else:
            self.size = os.path.getsize(src)
        tail = self.read(max(0, self.size - 65558), min(self.size, 65558))
        i = tail.rfind(b'PK\x05\x06')
        if i < 0:
            raise ValueError(f'{src}: no zip end record')
        n, cdsize, cdoff = struct.unpack_from('<HII', tail, i + 10)
        if 0xffffffff in (cdsize, cdoff) or n == 0xffff:
            j = tail.rfind(b'PK\x06\x07', 0, i)
            z64, = struct.unpack_from('<Q', tail, j + 8)
            rec = self.read(z64, 56)
            n, cdsize, cdoff = struct.unpack_from('<QQQ', rec, 32)
        cd = self.read(cdoff, cdsize)
        self.entries = []
        p = 0
        while p + 46 <= len(cd) and cd[p:p + 4] == b'PK\x01\x02':
            (meth, crc, cs, us, nl, el, cl), ea, lho = struct.unpack_from('<H4xIIIHHH', cd, p + 10), \
                struct.unpack_from('<I', cd, p + 38)[0], struct.unpack_from('<I', cd, p + 42)[0]
            name = cd[p + 46:p + 46 + nl].decode('utf-8', 'replace')
            extra = cd[p + 46 + nl:p + 46 + nl + el]
            vals = [v for v, m in ((us, 0xffffffff), (cs, 0xffffffff), (lho, 0xffffffff)) if v == m]
            if vals:                         # zip64 extra field: the 0xffffffff fields, in order
                q = 0
                while q + 4 <= len(extra):
                    hid, hl = struct.unpack_from('<HH', extra, q)
                    if hid == 1:
                        f64 = list(struct.unpack_from('<%dQ' % (hl // 8), extra, q + 4))
                        if us == 0xffffffff: us = f64.pop(0)
                        if cs == 0xffffffff: cs = f64.pop(0)
                        if lho == 0xffffffff: lho = f64.pop(0)
                        break
                    q += 4 + hl
            self.entries.append((name, meth, cs, us, lho, ea >> 16, crc))
            p += 46 + nl + el + cl

    def read(self, off, n):
        return _get(self.src, off, n) if self.src.startswith(('http://', 'https://')) else _local(self.src, off, n)

    def extract(self, select, dest, log=log):
        """Members chosen by select(name) -> relative output path or None; symbolic links are made
        as links. Returns the relative paths written."""
        want = []
        for e in self.entries:
            if e[0].endswith('/'):
                continue
            rel = select(e[0])
            if rel:
                want.append((e[4], e[2] + 30 + 1024, (e, rel)))     # local header + name + extra slack
        got, total = [], 0

        def read_span(span):
            start, end, lst = span
            return start, self.read(start, min(end, self.size) - start), lst

        for start, buf, lst in _parallel(read_span, _spans(want)):
            total += len(buf)
            for off, _, ((name, meth, cs, us, lho, mode, crc), rel) in lst:
                p = off - start
                nl, el = struct.unpack_from('<HH', buf, p + 26)
                d0 = p + 30 + nl + el
                blk = buf[d0:d0 + cs]
                if len(blk) < cs:
                    blk = self.read(off + 30 + nl + el, cs)
                data = blk if meth == 0 else zlib.decompressobj(-15).decompress(blk)
                if len(data) != us or (zlib.crc32(data) & 0xffffffff) != crc:
                    raise IOError(f'{name}: bad size or CRC')
                dst = os.path.join(dest, rel)
                if mode & 0o170000 == 0o120000:              # a symbolic link: data = its target
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    if os.path.lexists(dst):
                        os.unlink(dst)
                    os.symlink(data.decode(), dst)
                else:
                    _write(dst, data, (mode & 0o777) or None)
                got.append(rel)
        log(f'  {len(got)} files, {total / 1e6:.1f} MB read')
        return got


# --------------------------------------------------------------------------- toolchain

def editor_installer(version):
    """URL of the Windows editor installer of a Unity version (Unity's release API)."""
    from urllib.parse import urlencode
    with urllib.request.urlopen(f'{EDITOR_API}?{urlencode({"version": version, "limit": 1})}', timeout=60) as r:
        d = json.load(r)
    for rec in d.get('results', []):
        if rec.get('version') != version:
            continue
        for dl in rec.get('downloads', []):
            if dl.get('platform') == 'WINDOWS' and str(dl.get('url', '')).endswith('.exe'):
                return dl['url']
        if rec.get('shortRevision'):
            return EDITOR_CDN.format(rev=rec['shortRevision'], ver=version)
    raise SystemExit(f'Unity does not list version {version} (check `python -m pmunity version <data>`)')


def editor_rel(nsis_path):
    """Editor installer path -> path under Editor/Data ('$_OUTDIR/il2cpp/x' or
    '$INSTDIR/Editor/Data/il2cpp/x' -> 'il2cpp/x'), else None."""
    p = nsis_path
    if p.startswith('$_OUTDIR/'):
        return p[len('$_OUTDIR/'):]
    i = p.find('/Editor/Data/')
    return p[i + len('/Editor/Data/'):] if i >= 0 else None


EDITOR_PARTS = ('il2cpp/il2cpp_root', 'MonoBleedingEdge/etc/mono/config')   # (fetched with every part)


def editor_wanted(rel):
    if rel.startswith('MonoBleedingEdge/etc/'):          # il2cpp copies it into the data (Managed/etc)
        return True
    if rel.startswith('MonoBleedingEdge/lib/mono/unityaot/'):
        return not rel.endswith(('.pdb', '.mdb'))
    if not rel.startswith('il2cpp/'):
        return False
    return not (rel.startswith(('il2cpp/MapFileParser/', 'il2cpp/external/mono/mono/tests/')) or rel.endswith(('.pdb', '.mdb')))


def runtime_pack_files(deps):
    """File names a self-contained .NET build's deps.json lists from the .NET runtime pack (the
    framework a Windows build of il2cpp carries; the image's .NET runs the tools instead)."""
    out = set()
    for name, lib in deps.get('libraries', {}).items():
        if lib.get('type') != 'runtimepack':
            continue
        for tgt in deps.get('targets', {}).values():
            for asset in ('runtime', 'native'):
                for f in (tgt.get(name) or {}).get(asset, {}):
                    out.add(f.split('/')[-1].lower())
    return out


def fetch_editor(version, dest, installer=None):
    """Editor/Data/il2cpp and MonoBleedingEdge/lib/mono/unityaot of the Windows editor installer.
    A self-contained .NET build of il2cpp (2020.1+) leaves out the .NET runtime it carries."""
    from . import nsis
    if os.path.isfile(os.path.join(dest, '.complete')) and all(os.path.exists(os.path.join(dest, p)) for p in EDITOR_PARTS):
        return dest
    installer = installer or editor_installer(str(version))
    log(f'editor {version}: il2cpp and the unityaot class libraries from {installer}')
    ns = nsis.Nsis(installer)
    deps = {}
    for rel in nsis_fetch(ns, lambda p: (lambda r: r if r and r.startswith('il2cpp/build/') and r.endswith('.deps.json') else None)(
            editor_rel(p)), dest):
        with open(os.path.join(dest, rel), encoding='utf-8-sig') as f:
            deps.setdefault(os.path.dirname(rel), set()).update(runtime_pack_files(json.load(f)))

    def select(p):
        r = editor_rel(p)
        if not r or not editor_wanted(r) or r.endswith('.deps.json'):
            return None
        if r.split('/')[-1].lower() in deps.get(os.path.dirname(r), ()):
            return None
        return r
    got = nsis_fetch(ns, select, dest)
    if not any(g.startswith('il2cpp/libil2cpp/') for g in got):
        raise SystemExit(f'{installer}: no Editor/Data/il2cpp in this installer')
    # framework-dependent from here on: the deps files would name the runtime pack's files
    for d in deps:
        for n in os.listdir(os.path.join(dest, d)):
            if n.endswith('.deps.json'):
                os.replace(os.path.join(dest, d, n), os.path.join(dest, d, n + '.orig'))
    open(os.path.join(dest, 'il2cpp', 'il2cpp_root'), 'a').close()
    with open(os.path.join(dest, '.complete'), 'w') as f:
        json.dump({'version': str(version), 'installer': installer, 'files': len(got),
                   'runtime_pack_files_left_out': sorted({n for v in deps.values() for n in v})}, f, indent=0)
    return dest


def ndk_wanted(name, rev, abi):
    """NDK zip member -> relative path (without the top folder) when il2cpp's Android build needs it."""
    _, triple, _ = ABIS[abi]
    api = api_level(rev, abi)
    parts = name.split('/', 1)
    if len(parts) < 2:
        return None
    rel = parts[1]
    if rel == 'source.properties':
        return rel
    llvm = 'toolchains/llvm/prebuilt/linux-x86_64/'
    if rev == 'r16b':                    # GNU binutils layout: platforms/ + sysroot/ + sources/cxx-stl, GNU libstdc++
        arch = 'arm64' if abi == 'arm64-v8a' else 'arm'
        gcc = f'toolchains/{triple}-4.9/prebuilt/linux-x86_64/'
        stl = 'sources/cxx-stl/gnu-libstdc++/4.9/'
        if rel.startswith('sysroot/usr/include/') or rel.startswith(f'platforms/android-{api}/arch-{arch}/'):
            return rel
        if rel.startswith(stl + 'include/') or rel.startswith(stl + f'libs/{abi}/'):
            return rel if not rel.endswith('.so') else None
        if rel.startswith(gcc + f'lib/gcc/{triple}/') or rel.startswith(gcc + f'{triple}/lib'):
            return rel if not rel.endswith('/') else None
        return None
    if not rel.startswith(llvm):
        return None
    r = rel[len(llvm):]
    if r.startswith(('sysroot/usr/include/', 'sysroot/usr/local/include/')):
        return rel                        # (usr/local/include: libandroid_support's headers, API < 21)
    if r.startswith(f'sysroot/usr/lib/{triple}/'):
        rest = r[len(f'sysroot/usr/lib/{triple}/'):]
        return rel if '/' not in rest or rest.startswith(f'{api}/') else None
    if r.startswith(f'lib/gcc/{triple}/') or r.startswith(f'{triple}/lib'):
        return rel if 'ldscripts' not in r else None
    if re.match(r'^lib64/clang/[^/]+/lib/linux/', r) and ('android' in r or f'/{triple.split("-")[0]}/' in r or '/arm/' in r):
        return rel
    return None


def fetch_ndk(rev_name, zipname, abi, dest):
    mark = os.path.join(dest, f'.complete-{abi}')
    if os.path.isfile(mark):
        return dest
    url = NDK_BASE + zipname
    log(f'NDK {rev_name} ({abi}): sysroot, C++ runtime and libgcc from {url}')
    z = RemoteZip(url)
    got = z.extract(lambda n: ndk_wanted(n, rev_name, abi), dest)
    if 'source.properties' not in got:
        raise SystemExit(f'{url}: not an NDK archive (no source.properties)')
    install_ndk_shims(dest, abi)
    with open(mark, 'w') as f:
        json.dump({'ndk': rev_name, 'zip': url, 'files': len(got)}, f)
    return dest


def install_ndk_shims(ndk, abi):
    """tools/il2cpp/ndk-clang.sh where il2cpp's Android toolchain runs the NDK's x86-64 programs."""
    with open(os.path.join(TOOLS, 'ndk-clang.sh'), 'rb') as f:
        script = f.read()
    _, triple, _ = ABIS[abi]
    paths = ['toolchains/llvm/prebuilt/linux-x86_64/bin/clang', 'toolchains/llvm/prebuilt/linux-x86_64/bin/clang++']
    for tool in ('objcopy', 'strip', 'ar', 'nm'):
        paths.append(f'toolchains/{triple}-4.9/prebuilt/linux-x86_64/bin/{triple}-{tool}')
        paths.append(f'toolchains/llvm/prebuilt/linux-x86_64/bin/{triple}-{tool}')
    for p in paths:
        _write(os.path.join(ndk, p), script, 0o755)


def player_wanted(abi):
    return [f'Variations/il2cpp/Managed/*.dll', f'Variations/il2cpp/Release/StaticLibs/{abi}/baselib.a']


def fetch_player(version, pkg, abi):
    """The IL2CPP player package: `pmunity player fetch --backend il2cpp` (libmain, libunity, the
    default resources) plus what the IL2CPP build reads (the IL2CPP UnityEngine assemblies, baselib)."""
    from . import player
    pj = os.path.join(pkg, 'player.json')
    info = json.load(open(pj)) if os.path.isfile(pj) else None
    if info and (info.get('backend') != 'il2cpp' or info.get('abi') != abi):
        raise SystemExit(f'{pkg} holds the {info.get("backend")} / {info.get("abi")} player; use another --pkg '
                         f'for the IL2CPP {abi} one')
    if info is None:
        info = player.fetch(str(version), pkg, 'il2cpp', abi, log=log)
    man = os.path.join(pkg, 'Variations', 'il2cpp', 'Managed')
    if not (os.path.isdir(man) and any(n.endswith('.dll') for n in os.listdir(man))):
        log(f'player {version}: the IL2CPP UnityEngine assemblies and static libraries')
        globs = player_wanted(abi)
        nsis_fetch(info['installer'], lambda p: (lambda r: r if r and any(fnmatch.fnmatchcase(r, g) for g in globs) else None)(
            player.package_path(p)), pkg)
    return info


def toolchain(version, abi, home, pkg=None):
    """Fetches what is missing; returns the paths."""
    vdir = os.path.join(home, str(version))
    editor = fetch_editor(version, os.path.join(vdir, 'editor'))
    name, rev, zipname = ndk_for(version)
    ndk = fetch_ndk(name, zipname, abi, os.path.join(home, 'ndk-' + name))
    install_ndk_shims(ndk, abi)                  # (again: the script may be newer than the fetch)
    pkg = pkg or os.path.join(vdir, 'player-' + abi)
    info = fetch_player(version, pkg, abi)
    return {'editor': editor, 'ndk': ndk, 'ndk_name': name, 'ndk_revision': rev, 'pkg': pkg, 'player': info}


# --------------------------------------------------------------------------- the build

def deploy_tool(editor, name):
    """A .NET program of Editor/Data/il2cpp/build: net471 <name>.exe (Unity 2018.4 - 2019.4) or the
    .NET Core / .NET <name>.dll (2020.1+)."""
    b = os.path.join(editor, 'il2cpp', 'build')
    for rel in (f'deploy/net471/{name}.exe', f'{name}.exe'):
        if os.path.isfile(os.path.join(b, rel)):
            return os.path.join(b, rel)
    d = os.path.join(b, 'deploy')
    for sub in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        p = os.path.join(d, sub, f'{name}.dll')
        if os.path.isfile(p):
            return p
    p = os.path.join(d, f'{name}.dll')
    if os.path.isfile(p):
        return p
    raise SystemExit(f'{editor}: no {name} program under il2cpp/build')


RUNTIMECONFIG = {'runtimeOptions': {
    'tfm': 'net8.0', 'rollForward': 'Major',
    'framework': {'name': 'Microsoft.NETCore.App', 'version': '6.0.0'},
    'configProperties': {
        'System.Globalization.Invariant': True,
        'System.Runtime.Serialization.EnableUnsafeBinaryFormatterSerialization': True,
        'System.GC.Concurrent': False}}}


BCL_DIR = os.path.join('MonoBleedingEdge', 'lib', 'mono', 'unityaot')


def is_engine(name):
    return name == 'UnityEngine.dll' or bool(re.match(r'^UnityEngine\.\w+Module\.dll$', name))


def assembly_refs(path):
    """Names of the assemblies a .NET assembly references (its AssemblyRef table)."""
    return [r + '.dll' for r in _AsmRefs(path).refs]


class _AsmRefs:
    """Minimal ECMA-335 reader: the AssemblyRef table's names (enough to follow references)."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            d = f.read()
        self.refs = []
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError(f'{path}: not a PE file')
        nsec, = struct.unpack_from('<H', d, pe + 6)
        optsz, = struct.unpack_from('<H', d, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from('<H', d, opt)
        dd = opt + (96 if magic == 0x10b else 112)
        cli_rva, _ = struct.unpack_from('<II', d, dd + 14 * 8)
        secs = [struct.unpack_from('<8sIIII', d, opt + optsz + 40 * i) for i in range(nsec)]

        def off(rva):
            for _, vsz, va, rsz, ro in secs:
                if va <= rva < va + max(vsz, rsz):
                    return rva - va + ro
            raise ValueError('rva outside sections')
        if not cli_rva:
            raise ValueError(f'{path}: not a .NET assembly')
        c = off(cli_rva)
        md_rva, _ = struct.unpack_from('<II', d, c + 8)
        m = off(md_rva)
        vlen, = struct.unpack_from('<I', d, m + 12)
        p = m + 16 + vlen
        nstreams, = struct.unpack_from('<H', d, p + 2)
        p += 4
        streams = {}
        for _ in range(nstreams):
            so, ss = struct.unpack_from('<II', d, p)
            e = d.index(b'\0', p + 8)
            nm = d[p + 8:e].decode()
            streams[nm] = (m + so, ss)
            p = (e + 4) & ~3
        t, _ = streams.get('#~') or streams['#-']
        strings = streams['#Strings'][0]
        hs = d[t + 6]
        valid, = struct.unpack_from('<Q', d, t + 8)
        rows = []
        q = t + 24
        for i in range(64):
            if valid >> i & 1:
                rows.append(struct.unpack_from('<I', d, q)[0]); q += 4
            else:
                rows.append(0)
        if not valid >> 0x23 & 1:
            return                          # no AssemblyRef table
        si = 4 if hs & 1 else 2
        gi = 4 if hs & 2 else 2
        bi = 4 if hs & 4 else 2
        idx = lambda *tabs: 4 if max(rows[x] for x in tabs) >= 1 << 16 else 2
        def coded(bits, *tabs):
            return 4 if max(rows[x] for x in tabs) >= 1 << (16 - bits) else 2
        tdor = coded(2, 0x02, 0x01, 0x1b)
        rs = coded(2, 0x00, 0x1a, 0x23, 0x01)
        mrp = coded(3, 0x02, 0x01, 0x1a, 0x06, 0x1b)
        hc = coded(2, 0x04, 0x08, 0x17)
        hfm = coded(1, 0x04, 0x08)
        hs_ = coded(1, 0x14, 0x17)
        mdor = coded(1, 0x06, 0x0a)
        cat = coded(3, 0x06, 0x0a)
        hca = coded(5, 0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x00, 0x0e, 0x17, 0x14, 0x11, 0x1a, 0x1b,
                    0x20, 0x23, 0x26, 0x27, 0x28, 0x2a, 0x2b, 0x2c)
        hds = coded(2, 0x02, 0x06, 0x20)
        impl = coded(2, 0x26, 0x23, 0x27)
        mf = coded(1, 0x04, 0x06)
        tom = coded(1, 0x02, 0x06)
        sizes = {
            0x00: 2 + si + gi * 3, 0x01: rs + si * 2, 0x02: 4 + si * 2 + tdor + idx(0x04) + idx(0x06),
            0x03: idx(0x04), 0x04: 2 + si + bi, 0x05: idx(0x06), 0x06: 4 + 2 + 2 + si + bi + idx(0x08),
            0x07: idx(0x08), 0x08: 2 + 2 + si, 0x09: idx(0x02) + tdor, 0x0a: mrp + si + bi,
            0x0b: 2 + hc + bi, 0x0c: hca + cat + bi, 0x0d: hfm + bi, 0x0e: 2 + hds + bi,
            0x0f: 2 + 4 + idx(0x02), 0x10: 4 + idx(0x04), 0x11: bi, 0x12: idx(0x02) + idx(0x14),
            0x13: idx(0x14), 0x14: 2 + si + tdor, 0x15: idx(0x02) + idx(0x17), 0x16: idx(0x17),
            0x17: 2 + si + bi, 0x18: 2 + idx(0x06) + hs_, 0x19: idx(0x02) + mdor * 2, 0x1a: si,
            0x1b: bi, 0x1c: 2 + mf + si + idx(0x1a), 0x1d: 4 + idx(0x04), 0x1e: 4 + 4, 0x1f: 4,
            0x20: 4 + 2 * 4 + 4 + bi + si * 2, 0x21: 4, 0x22: 4 * 3,
        }
        for i in range(0x23):
            q += sizes[i] * rows[i]
        n = rows[0x23]
        rsz = 2 * 4 + 4 + bi + si + si + bi
        for k in range(n):
            r = q + k * rsz
            nm_off = r + 12 + bi
            name_idx = struct.unpack_from('<I' if si == 4 else '<H', d, nm_off)[0]
            e = d.index(b'\0', strings + name_idx)
            self.refs.append(d[strings + name_idx:e].decode('utf-8', 'replace'))


def stage_assemblies(data, tc, out_dir, log=log):
    """The assembly set il2cpp compiles, in out_dir: the game's own assemblies (and Unity's package
    assemblies the build shipped) from <data>/Managed, the IL2CPP player's UnityEngine assemblies
    instead of the desktop ones, and the IL2CPP class libraries (unityaot) instead of the desktop
    Mono's, following references from the game's assemblies. Returns {name: source}."""
    gm = os.path.join(data, 'Managed')
    eng = os.path.join(tc['pkg'], 'Variations', 'il2cpp', 'Managed')
    bcl = os.path.join(tc['editor'], BCL_DIR)
    bcl_files = {}
    for r, ds, fs in os.walk(bcl):
        for n in fs:
            if n.endswith('.dll'):
                bcl_files.setdefault(n, os.path.join(r, n))         # unityaot/ before unityaot/Facades/
    engine = {n: os.path.join(eng, n) for n in os.listdir(eng) if n.endswith('.dll')}
    game = {}
    for n in sorted(os.listdir(gm)):
        if not n.endswith('.dll') or is_engine(n) or n in bcl_files:
            continue
        game[n] = os.path.join(gm, n)
    chosen = dict(engine)
    chosen.update(game)
    todo = list(chosen)
    missing = []
    while todo:
        n = todo.pop()
        for ref in assembly_refs(chosen[n]):
            if ref in chosen:
                continue
            src = bcl_files.get(ref) or engine.get(ref)
            if src is None and os.path.isfile(os.path.join(gm, ref)):
                src = os.path.join(gm, ref)
                log(f'  {ref}: not in the IL2CPP class libraries; the game\'s copy is used')
            if src is None:
                missing.append((ref, n))
                continue
            chosen[ref] = src
            todo.append(ref)
    for ref, by in sorted(set(missing)):
        log(f'  WARNING: {by} references {ref}, which is nowhere (il2cpp may refuse or leave calls to it failing)')
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    for n, src in sorted(chosen.items()):
        shutil.copyfile(src, os.path.join(out_dir, n))
    log(f'assemblies: {len(game)} of the game, {len(engine)} UnityEngine (IL2CPP player), '
        f'{len(chosen) - len(game) - len(engine)} class libraries (unityaot)')
    return chosen


def player_settings(data):
    """What of the game's settings the IL2CPP build depends on: incremental GC. The editor writes
    gc-max-time-slice=<ms> into boot.config when it is on (Unity 2019.1+); an IL2CPP build then
    compiles the code with GC write barriers (--incremental-g-c-time-slice), and the player turns
    the incremental collector on from boot.config: the two must agree."""
    out = {'gcIncremental': False}
    try:
        with open(os.path.join(data, 'boot.config'), encoding='utf-8', errors='replace') as f:
            for line in f:
                k, _, v = line.strip().partition('=')
                if k == 'gc-max-time-slice' and v.strip().isdigit() and int(v) > 0:
                    out = {'gcIncremental': True, 'gcTimeSlice': int(v)}
    except OSError:
        pass
    return out


def elf_exports(path):
    """Names of the defined dynamic symbols of an ELF shared library (32- or 64-bit, little endian)."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'\x7fELF':
        raise ValueError(f'{path}: not an ELF file')
    is64 = d[4] == 2
    if is64:
        shoff, = struct.unpack_from('<Q', d, 0x28); shentsize, shnum = struct.unpack_from('<HH', d, 0x3a)
    else:
        shoff, = struct.unpack_from('<I', d, 0x20); shentsize, shnum = struct.unpack_from('<HH', d, 0x2e)
    secs = []
    for i in range(shnum):
        o = shoff + i * shentsize
        if is64:
            name, typ, flags, addr, off, size, link, info, align, ent = struct.unpack_from('<IIQQQQIIQQ', d, o)
        else:
            name, typ, flags, addr, off, size, link, info, align, ent = struct.unpack_from('<IIIIIIIIII', d, o)
        secs.append((typ, off, size, link, ent))
    out = set()
    for typ, off, size, link, ent in secs:
        if typ != 11:                               # SHT_DYNSYM
            continue
        stro = secs[link][1]
        for k in range(1, size // ent):
            o = off + k * ent
            if is64:
                nm, info, other, shndx = struct.unpack_from('<IBBH', d, o)
            else:
                nm, _, _, info, other, shndx = struct.unpack_from('<IIIBBH', d, o)
            if shndx == 0 or (info >> 4) == 0:      # undefined or local
                continue
            e = d.index(b'\0', stro + nm)
            out.add(d[stro + nm:e].decode('ascii', 'replace'))
    return out


def il2cpp_api_check(libunity, libil2cpp):
    """The il2cpp_* functions the player's libunity.so looks up by name (dlsym of the strings it
    holds) that the built libil2cpp.so does not export: [] when the two are of the same IL2CPP."""
    with open(libunity, 'rb') as f:
        wanted = {m.decode() for m in re.findall(rb'\0(il2cpp_[a-z0-9_]+)\0', f.read())}
    have = elf_exports(libil2cpp)
    return sorted(w for w in wanted if w not in have), len(wanted)


def metadata_version(path):
    with open(path, 'rb') as f:
        magic, ver = struct.unpack('<Ii', f.read(8))
    if magic != METADATA_MAGIC:
        raise ValueError(f'{path}: not IL2CPP metadata (magic {magic:#x})')
    return ver


def ensure_root(root, image):
    """<home>/root: the tool folders bind-mounted over the stock image's /usr and /etc."""
    if os.path.isfile(os.path.join(root, '.complete')):
        return root
    log(f'making the il2cpp tool folders in {root} (tools/il2cpp/root.sh: Ubuntu packages, once, ~0.85 GB)')
    subprocess.run(['sh', os.path.join(TOOLS, 'root.sh'), root], check=True,
                   env=dict(os.environ, PMUNITY_IL2CPP_BASE=image))
    return root


def il2cpp_args(version, abi, managed, cpp, cache, so, ndk, settings, pkg):
    arch = ABIS[abi][0]
    a = ['--convert-to-cpp', '--compile-cpp', '--libil2cpp-static',
         '--emit-null-checks', '--enable-array-bounds-check',
         '--dotnetprofile=unityaot',
         '--platform=Android', f'--architecture={arch}', '--configuration=Release',
         f'--outputpath={so}', f'--cachedirectory={cache}', f'--generatedcppdir={cpp}',
         f'--directory={managed}', f'--tool-chain-path={ndk}', '--verbose']
    if settings.get('gcIncremental') and version.line >= (2019, 1):
        a.append(f'--incremental-g-c-time-slice={settings.get("gcTimeSlice", 3)}')
    baselib = os.path.join(pkg, 'Variations', 'il2cpp', 'Release', 'StaticLibs', abi)
    if os.path.isfile(os.path.join(baselib, 'baselib.a')):
        a.append(f'--baselib-directory={baselib}')
    return a


class Container:
    """Runs commands in the stock image with the tool folders (<home>/root) over its /usr and /etc,
    the toolchain and output folders mounted at the same paths and /tmp in the work folder: the
    container's own file system gets nothing. As the calling user, with --jobs CPUs."""

    def __init__(self, image, root, mounts, jobs, work):
        self.image, self.root, self.work = image, root, work
        try:                                     # the CPUs Docker's machine has (a VM on macOS / Windows)
            n = int(subprocess.run(['docker', 'info', '--format', '{{.NCPU}}'], capture_output=True, text=True).stdout)
        except ValueError:
            n = jobs
        self.jobs = max(1, min(jobs, n))
        ms = sorted({os.path.realpath(p) for p in mounts})
        self.mounts = [m for m in ms if not any(m != o and m.startswith(o + os.sep) for o in ms)]

    def run(self, cmd, logname, quiet=False):
        # --cpuset-cpus as well as --cpus: il2cpp starts one compiler per CPU it sees (Mono counts the
        # CPUs it may run on, not the quota)
        tmp = os.path.join(self.work, 'tmp')
        os.makedirs(tmp, exist_ok=True)
        d = ['docker', 'run', '--rm', f'--cpus={self.jobs}', f'--cpuset-cpus=0-{self.jobs - 1}',
             '-v', f'{os.path.join(self.root, "usr")}:/usr:ro', '-v', f'{os.path.join(self.root, "etc")}:/etc:ro',
             '-v', f'{tmp}:/tmp', '-e', 'HOME=/tmp', '-e', 'DOTNET_CLI_HOME=/tmp', '-e', 'TMPDIR=/tmp',
             '-e', 'DOTNET_CLI_TELEMETRY_OPTOUT=1', '-e', 'DOTNET_NOLOGO=1', '-e', 'DOTNET_ROLL_FORWARD=Major',
             '-e', 'PMUNITY_LLVM=/usr/lib/llvm-14/bin',
             '-e', f'PMUNITY_IL2CPP_CCLOG={os.path.join(self.work, "cc.log")}']
        if hasattr(os, 'getuid'):
            d += ['--user', f'{os.getuid()}:{os.getgid()}']
        if os.environ.get('PMUNITY_IL2CPP_DEBUG_INFO'):
            d += ['-e', 'PMUNITY_IL2CPP_DEBUG_INFO=1']
        for m in self.mounts:
            d += ['-v', f'{m}:{m}']
        d += ['-w', self.work, self.image] + cmd
        path = os.path.join(self.work, logname)
        t0 = time.time()
        with open(path, 'w') as lf:
            p = subprocess.Popen(d, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
            tail = []
            for line in p.stdout:
                lf.write(line)
                lf.flush()
                tail = (tail + [line])[-40:]
            rcode = p.wait()
        if rcode != 0:
            sys.stderr.write(''.join(tail))
            prog = next((os.path.basename(w) for w in cmd if w.endswith(('.exe', '.dll'))), cmd[0])
            raise SystemExit(f'{prog} failed (exit {rcode}); full log: {path}')
        return time.time() - t0


def has_option(tool, field):
    """Whether a Unity .NET tool has an option field of that name (Unity.Options: one public
    static field per option, its name in the metadata strings)."""
    with open(tool, 'rb') as f:
        return b'\0' + field.encode() + b'\0' in f.read()


def link_args(version, abi, staged, linked, roots):
    """UnityLinker as an Android IL2CPP build with Managed Stripping Level "Low" runs it: the
    "Conservative" rules (the class libraries are linked with Unity's own descriptors; every root
    assembly is kept whole, MonoBehaviours, [Preserve] and [RequiredByNativeCode] members too).
    Here every assembly but the class libraries is a root (the game's, Unity's packages' and the
    UnityEngine modules): a desktop build carries no record of the types its scenes use (the
    editor's TypesInScenes / editor data), so nothing the data might name is stripped."""
    a = [f'--out={linked}', f'--search-directory={staged}', f'--include-directory={staged}',
         '--dotnetruntime=il2cpp', '--dotnetprofile=unityaot', '--use-editor-options',
         '--rule-set=Conservative', '--platform=Android', f'--architecture={ABIS[abi][0]}']
    a += [f'--include-unity-root-assembly={os.path.join(staged, n)}' for n in sorted(roots)]
    return a


def build(data, out, abi='arm64-v8a', home=None, pkg=None, jobs=None, image=IMAGE, keep_cpp=False, dry=False,
          strip='low'):
    version = UV.of_data(data)
    UV.check(version)
    home = os.path.abspath(home or default_home())
    out = os.path.abspath(out)
    data = os.path.abspath(data)
    log(f'data: Unity {version}; IL2CPP for {abi}')
    tc = toolchain(version, abi, home, pkg and os.path.abspath(pkg))
    work = os.path.join(out, '.work')
    staged, linked = os.path.join(work, 'Managed-in'), os.path.join(work, 'Managed')
    chosen = stage_assemblies(data, tc, staged)
    settings = player_settings(data)
    cpp, cache = os.path.join(work, 'cpp'), os.path.join(work, 'cache')
    so = os.path.join(out, 'lib', abi, 'libil2cpp.so')
    os.makedirs(os.path.dirname(so), exist_ok=True)
    rc = os.path.join(work, 'tools.runtimeconfig.json')
    with open(rc, 'w') as f:
        json.dump(RUNTIMECONFIG, f)

    def runner(tool, net471='mono'):
        # .NET Framework builds (2018.4 - 2019.4) run on Mono, as in Unity's Linux and macOS editors
        # (UnityLinker uses .NET Framework-only types); .NET Core / .NET builds on .NET 8.
        # PMUNITY_IL2CPP_NET471=dotnet runs a .NET Framework il2cpp.exe on .NET 8 instead (about
        # twice as fast at converting; not what Unity itself does)
        if tool.endswith('.exe') and net471 == 'mono':
            return ['mono', '--gc=sgen', tool]
        return ['dotnet', 'exec', '--runtimeconfig', rc, tool]
    root = os.path.join(home, 'root')
    box = Container(image, root, (home, out, tc['pkg']), jobs or min(4, os.cpu_count() or 2), work)
    if os.path.exists(os.path.join(work, 'cc.log')):
        os.unlink(os.path.join(work, 'cc.log'))
    roots = [n for n, src in chosen.items() if not src.startswith(os.path.join(tc['editor'], BCL_DIR) + os.sep)]
    lcmd = runner(deploy_tool(tc['editor'], 'UnityLinker')) + link_args(version, abi, staged, linked, roots)
    tool = deploy_tool(tc['editor'], 'il2cpp')
    icmd = runner(tool, os.environ.get('PMUNITY_IL2CPP_NET471', 'mono')) + \
        il2cpp_args(version, abi, linked, cpp, cache, so, tc['ndk'], settings, tc['pkg'])
    if has_option(tool, 'Jobs'):                # (2020.x+; older ones run a compiler per CPU they see)
        icmd.append(f'--jobs={box.jobs}')
    icmd += os.environ.get('PMUNITY_IL2CPP_EXTRA_ARGS', '').split()     # e.g. --additional-cpp=... (debugging)
    if strip == 'none':
        icmd = [w.replace(f'--directory={linked}', f'--directory={staged}') for w in icmd]
    log('UnityLinker: ' + ' '.join(lcmd) if strip != 'none' else 'UnityLinker: not run (--strip none)')
    log('il2cpp: ' + ' '.join(icmd))
    if dry:
        return None
    for where, need in ((out, 2 << 30), (home, 1 << 30)):
        st = os.statvfs(where)
        if st.f_bavail * st.f_frsize < need:
            raise SystemExit(f'{where}: {st.f_bavail * st.f_frsize / 2**30:.1f} GiB free; the build needs about '
                             f'{need >> 30} GiB there (generated C++, objects; the tool folders)')
    ensure_root(root, image)
    secs = 0
    if strip != 'none':
        shutil.rmtree(linked, ignore_errors=True)
        secs += box.run(lcmd, 'unitylinker.log')
        kept = sorted(n for n in os.listdir(linked) if n.endswith('.dll'))
        log(f'UnityLinker: {len(kept)} assemblies kept, {sum(os.path.getsize(os.path.join(linked, n)) for n in kept) / 2**20:.1f} MiB '
            f'(from {sum(os.path.getsize(os.path.join(staged, n)) for n in chosen) / 2**20:.1f} MiB) in {secs:.0f} s')
    t = box.run(icmd, 'il2cpp.log')
    secs += t
    log(f'il2cpp: done in {t:.0f} s')
    return finish(out, work, cpp, so, version, abi, tc, chosen, settings, keep_cpp, secs, strip)


def finish(out, work, cpp, so, version, abi, tc, chosen, settings, keep_cpp, secs, strip):
    """The data folder il2cpp wrote (Metadata, Resources) -> <out>/Data/Managed; il2cpp.json."""
    src = None
    for cand in (os.path.join(cpp, 'Data'), os.path.join(os.path.dirname(so), 'Data'), os.path.join(work, 'Data')):
        if os.path.isfile(os.path.join(cand, 'Metadata', 'global-metadata.dat')):
            src = cand
            break
    if src is None:
        raise SystemExit(f'il2cpp wrote no Metadata/global-metadata.dat (looked in {cpp}/Data)')
    dst = os.path.join(out, 'Data', 'Managed')
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if src == os.path.join(os.path.dirname(so), 'Data'):
        os.replace(src, dst)                     # (il2cpp writes it beside the library)
    else:
        shutil.copytree(src, dst)
    shutil.rmtree(os.path.join(os.path.dirname(so), 'Data'), ignore_errors=True)   # (2019.4: also there)
    mv = metadata_version(os.path.join(dst, 'Metadata', 'global-metadata.dat'))
    libunity = os.path.join(tc['pkg'], 'Variations', 'il2cpp', 'Release', 'Libs', abi, 'libunity.so')
    missing, nwanted = il2cpp_api_check(libunity, so) if os.path.isfile(libunity) else ([], 0)
    if missing:
        log(f'WARNING: the player\'s libunity.so looks up {len(missing)} il2cpp_* functions libil2cpp.so lacks: {missing[:8]}')
    for extra in ('libil2cpp.dbg.so', 'libil2cpp.sym.so'):     # the unstripped library, its symbols
        p = os.path.join(os.path.dirname(so), extra)
        if os.path.exists(p):
            if extra.endswith('.dbg.so'):
                os.unlink(p)
            else:
                os.replace(p, os.path.join(work, extra))
    info = {
        'unity': str(version), 'abi': abi, 'metadata_version': mv,
        'libil2cpp_bytes': os.path.getsize(so),
        'player_api': {'looked_up': nwanted, 'missing': missing},
        'ndk': tc['ndk_name'], 'player': tc['player'].get('version'),
        'assemblies': sorted(chosen), 'gc_incremental': settings.get('gcIncremental', False), 'strip': strip,
        'build_seconds': round(secs),
        'files': sorted(os.path.relpath(os.path.join(r, n), out) for r, _, fs in os.walk(os.path.join(out, 'Data')) for n in fs),
    }
    with open(os.path.join(out, 'il2cpp.json'), 'w') as f:
        json.dump(info, f, indent=1)
    if not keep_cpp:
        shutil.rmtree(cpp, ignore_errors=True)
        shutil.rmtree(os.path.join(work, 'cache'), ignore_errors=True)
    shutil.rmtree(os.path.join(work, 'tmp'), ignore_errors=True)
    log(f'il2cpp: {so} ({os.path.getsize(so) / 2**20:.1f} MiB), metadata version {mv}, '
        f'{len(info["files"])} data files -> {dst}')
    return info


def default_home():
    return os.environ.get('PMUNITY_IL2CPP_HOME') or os.path.join(os.path.expanduser('~'), '.cache', 'pmunity', 'il2cpp')


def main(argv):
    if argv[:1] == ['fetch']:
        ap = argparse.ArgumentParser(prog='pmunity il2cpp fetch')
        ap.add_argument('version')
        ap.add_argument('--abi', default='arm64-v8a', choices=sorted(ABIS))
        ap.add_argument('--home'); ap.add_argument('--pkg')
        a = ap.parse_args(argv[1:])
        tc = toolchain(UV.Version(a.version), a.abi, os.path.abspath(a.home or default_home()), a.pkg)
        print(json.dumps({k: v for k, v in tc.items() if k != 'player'}, indent=1))
        return
    ap = argparse.ArgumentParser(prog='pmunity il2cpp', description=__doc__.split('\n\n')[0],
                                 epilog='See docs/il2cpp.md.')
    ap.add_argument('data', help="the game's desktop data folder (read only)")
    ap.add_argument('out')
    ap.add_argument('--abi', default='arm64-v8a', choices=sorted(ABIS))
    ap.add_argument('--home', help='toolchain folder (default $PMUNITY_IL2CPP_HOME, else ~/.cache/pmunity/il2cpp)')
    ap.add_argument('--pkg', help='the IL2CPP Android player package (default <home>/<version>/player-<abi>)')
    ap.add_argument('--jobs', type=int, help='parallel compiles (CPUs given to the container; default min(4, CPUs))')
    ap.add_argument('--image', default=IMAGE, help='the stock image the tool folders are mounted into (default ubuntu:24.04)')
    ap.add_argument('--keep-cpp', action='store_true', help='keep the generated C++ and the object cache (<out>/.work)')
    ap.add_argument('--strip', default='low', choices=['low', 'none'],
                    help="managed code stripping: low (UnityLinker's conservative rules, as Unity's default for IL2CPP; "
                         "the game's and the engine's assemblies are kept whole) or none (every class library compiled "
                         "whole: about 3x the C++ and the build time)")
    ap.add_argument('--dry-run', action='store_true', help='fetch and stage, print the commands, do not run them')
    a = ap.parse_args(argv)
    try:
        build(a.data, a.out, a.abi, a.home, a.pkg, a.jobs, a.image, a.keep_cpp, a.dry_run, a.strip)
    except UV.Unsupported as ex:
        sys.exit(str(ex))
