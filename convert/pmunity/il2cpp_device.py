"""`pmunity il2cpp --on-device`: the IL2CPP build (docs/il2cpp.md) run natively on an aarch64 Linux
handheld, inside a port's first-launch setup, instead of in Docker on the porter's machine. A
port cannot ship the result (it is made from the game and from Unity's IL2CPP), so on 64-bit-only
firmwares the player's own handheld builds it.

    python -m pmunity il2cpp <data> <out> --on-device [--toolchain DIR] [--jobs N] ...
    python -m pmunity il2cpp fetch <version> --on-device [--toolchain DIR]   (downloads only)

The tools are the PC route's, as Debian 12 ("bookworm") arm64 packages: clang/lld 14 (the NDK's
x86-64 compilers stand-in, as on the PC: tools/il2cpp/ndk-clang.sh), Mono 6.8 (the .NET Framework
builds of il2cpp and UnityLinker, Unity 2017.4 - 2019.4) and, for Unity 2020.1+, Microsoft's .NET 8
runtime. They are fetched at setup time from Debian's and Microsoft's servers, each file checked
against the sha256 in il2cpp_device_lock.py, and unpacked by this module (Python's tarfile + lzma:
Debian's packages are xz-compressed) into one folder:

  <toolchain>/lib/            every shared library under its soname, glibc's loader too
  <toolchain>/llvm/bin/       clang, lld, llvm-objcopy/-ar/-nm; ld.lld (a wrapper, below)
  <toolchain>/llvm/lib/clang/14.0.6/include/   clang's own headers
  <toolchain>/mono/           mono-sgen, the 4.5 class libraries il2cpp/UnityLinker load, etc/mono
  <toolchain>/dotnet/         .NET 8 (Unity 2020.1+ only)
  <toolchain>/bin/            the commands the build runs: clang, clang++, ld.lld, llvm-*, mono, dotnet

Every program runs through the folder's own glibc loader (`lib/ld-linux-aarch64.so.1
--library-path lib <program>`): the firmware's libraries (often an older glibc, no libstdc++ of
the right version) are never used, nothing is installed, no root is needed and the firmware's /usr
is not touched. The folder holds no symbolic links and needs no executable bits on the programs
(only the loader and the small scripts in bin/ are executed directly), so it works on exFAT / FAT32
cards as on ext4. Programs that find their files through /proc/self/exe (the loader, when run that
way) get what they need: clang `-no-canonical-prefixes` (its resource dir and ld.lld from its
argv[0]), Mono a copy of the loader named mono/bin/mono (Mono derives its prefix from that path),
.NET a copy inside dotnet/ (the muxer finds host/fxr next to itself).

The build is the PC route's (il2cpp.py), with what a 1 GB, 4 x Cortex-A53 handheld needs:
  - memory: compiler jobs from MemAvailable (one at a time on a 1 GB handheld), files above
    PMUNITY_IL2CPP_BIG_KB compiled alone, il2cpp's CPU affinity limited to the job count (the
    2017.4 - 2019.4 tools start one compiler per CPU they see);
  - progress lines (PortMaster's patcher shows them) and a log per step in <out>/.work;
  - resumable: UnityLinker, the C++ conversion and every compiled object are kept, so a killed
    build continues where it stopped (il2cpp's object cache; conversion and compilation run as two
    il2cpp invocations).
"""
import hashlib
import json
import os
import re
import shutil
import ssl
import struct
import subprocess
import sys
import tarfile
import threading
import time
import urllib.error
import urllib.request

from . import il2cpp_device_lock as LOCK

LOADER = 'ld-linux-aarch64.so.1'
MUXER = 'dotnet-muxer'       # .NET's `dotnet` program, renamed (its name must be dotnet: the loader's copy has it)
LAYOUT = 2                   # (the folder's layout: a change unpacks everything again)
CLANG_VERSION = '14.0.6'
DEBIAN = 'https://deb.debian.org/debian/'
SNAPSHOT = 'https://snapshot.debian.org/archive/debian/{ts}/'


def log(msg):
    print(msg, file=sys.stderr, flush=True)


# --------------------------------------------------------------------------- what is unpacked

# (regex on the member path, without './', destination). Symbolic links in the packages are not
# needed: libraries are stored under their sonames (finish()); Mono finds its class libraries in
# the GAC (4.5/ only links to them; strong-named references are looked up in the GAC).
PLACE = [
    (r'^(?:usr/)?lib/aarch64-linux-gnu/([^/]+\.so(?:\.[0-9]+)*)$', r'lib/\1'),
    (r'^usr/lib/llvm-14/lib/(libclang-cpp\.so\.14)$', r'lib/\1'),
    (r'^usr/lib/llvm-14/bin/(clang|lld|llvm-objcopy|llvm-ar|llvm-nm)$', r'llvm/bin/\1'),
    (r'^usr/lib/llvm-14/lib/clang/([^/]+/include/.+)$', r'llvm/lib/clang/\1'),
    (r'^usr/bin/(mono-sgen)$', r'mono/bin/\1'),
    (r'^usr/lib/(libmono-native)\.so\.0\.0\.0$', r'mono/lib/\1.so'),       # (etc/mono/config: $mono_libdir/...)
    (r'^usr/lib/(libMonoPosixHelper\.so)$', r'mono/lib/\1'),
    (r'^etc/mono/(config|4\.5/machine\.config|4\.5/settings\.map)$', r'mono/etc/mono/\1'),
    (r'^usr/lib/mono/4\.5/([^/]+\.dll)$', r'mono/lib/mono/4.5/\1'),       # (mscorlib; the rest are links)
    (r'^usr/lib/mono/gac/([^/]+/[^/]+/[^/]+\.dll)$', r'mono/lib/mono/gac/\1'),
    (r'^usr/lib/mono/4\.5/Facades/([^/]+\.dll)$', r'mono/lib/mono/4.5/Facades/\1'),
]
# never needed (other architectures' clang headers, glibc's debugging and legacy libraries, ICU's
# tools and the parts of it libxml2 does not load)
DROP = [
    r'^llvm/lib/clang/[^/]+/include/(riscv_vector\.h|hexagon_.*|hvx_hexagon_.*|altivec\.h|vecintrin\.h|'
    r'htm.*intrin\.h|msa\.h|__clang_cuda_.*|__clang_hip_.*|opencl-c.*\.h|ppc_wrappers/.*|openmp_wrappers/.*|cuda_wrappers/.*)$',
    r'^lib/lib(BrokenLocale|memusage|pcprofile|c_malloc_debug|thread_db|nsl|anl|nss_hesiod|nss_compat|tic|'
    r'icutest|icutu|icuio|icui18n)\.so.*$',
]
_PLACE = [(re.compile(a), b) for a, b in PLACE]
_DROP = [re.compile(d) for d in DROP]

# what the build runs, per group; the libraries they load (DT_NEEDED, and what they dlopen) must
# all be in lib/ (checked after unpacking)
ROOTS = {
    'llvm': ['llvm/bin/clang', 'llvm/bin/lld', 'llvm/bin/llvm-objcopy', 'llvm/bin/llvm-ar', 'llvm/bin/llvm-nm'],
    'mono': ['mono/bin/mono-sgen', 'mono/lib/libmono-native.so', 'mono/lib/libMonoPosixHelper.so'],
    'dotnet': [],                     # (dotnet/'s own libraries, found when the group is installed)
}
DLOPEN = {'base': ['libnss_files.so.2', 'libnss_dns.so.2'], 'dotnet': ['libssl.so.3', 'libcrypto.so.3']}


def place(member):
    """Path inside a package -> path in the toolchain folder, or None."""
    name = member[2:] if member.startswith('./') else member
    for rx, dst in _PLACE:
        m = rx.match(name)
        if m:
            out = m.expand(dst)
            return None if any(d.match(out) for d in _DROP) else out
    return None


# packages of which only part is needed
ONLY = {'mono-devel': re.compile(r'^(?:\./)?usr/lib/mono/4\.5/Facades/')}


def groups_for(tools):
    """The package groups a build needs: tools = the il2cpp/UnityLinker programs (.exe: .NET
    Framework builds run on Mono; .dll: .NET Core / .NET builds run on .NET 8). For Mono, what the
    tools' folders reference beyond the base class libraries: the netstandard facade (Unity 2019.x:
    a netstandard Mono.Cecil), System.Web.Extensions (2019.x UnityLinker's JSON)."""
    g = ['base', 'llvm']
    exe = [t for t in tools if t.endswith('.exe')]
    if exe:
        g.append('mono')
        from .il2cpp import assembly_refs
        refs = set()
        for d in sorted({os.path.dirname(t) for t in exe}):
            for n in os.listdir(d):
                if n.endswith(('.dll', '.exe')):
                    try:
                        refs.update(assembly_refs(os.path.join(d, n)))
                    except (ValueError, OSError, struct.error):
                        pass
        if 'netstandard.dll' in refs:
            g.append('mono-facades')
        if 'System.Web.Extensions.dll' in refs:
            g.append('mono-web')
    if any(t.endswith('.dll') for t in tools):
        g.append('dotnet')
    return g


# --------------------------------------------------------------------------- ELF

def elf_dynamic(path):
    """(soname, [needed]) of a 64-bit little-endian ELF file, from its PT_DYNAMIC; (None, []) when it
    has none; None when it is not ELF."""
    with open(path, 'rb') as f:
        head = f.read(64)
        if head[:4] != b'\x7fELF' or head[4] != 2:
            return None
        phoff, = struct.unpack_from('<Q', head, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', head, 0x36)
        f.seek(phoff)
        ph = f.read(phentsize * phnum)
        loads, dyn = [], None
        for i in range(phnum):
            typ, flags, off, vaddr, paddr, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', ph, i * phentsize)
            if typ == 1:
                loads.append((vaddr, off, filesz))
            elif typ == 2:
                dyn = (off, filesz)
        if dyn is None:
            return None, []
        f.seek(dyn[0])
        d = f.read(dyn[1])
        tags = [struct.unpack_from('<qQ', d, i) for i in range(0, len(d) - 15, 16)]
        strtab = next((v for t, v in tags if t == 5), None)
        strsz = next((v for t, v in tags if t == 10), 0)

        def off_of(va):
            for v, o, n in loads:
                if v <= va < v + n:
                    return va - v + o
            raise ValueError(f'{path}: address {va:#x} outside the file')
        f.seek(off_of(strtab))
        strs = f.read(strsz)

        def s(i):
            return strs[i:strs.index(b'\0', i)].decode()
        soname = next((s(v) for t, v in tags if t == 14), None)
        return soname, [s(v) for t, v in tags if t == 1]


# --------------------------------------------------------------------------- downloads

CA_FILES = ('/etc/ssl/certs/ca-certificates.crt', '/etc/pki/tls/certs/ca-bundle.crt', '/etc/ssl/cert.pem',
            '/etc/ssl/ca-bundle.pem', '/etc/pki/tls/cacert.pem', '/etc/ssl/certs/ca-bundle.crt')


def ca_file():
    """A CA bundle for HTTPS when Python's own default locations hold none (the setup's Python
    looks in /etc/ssl; firmwares keep theirs in different places), else None."""
    p = ssl.get_default_verify_paths()
    if (p.cafile and os.path.isfile(p.cafile)) or (p.capath and os.path.isdir(p.capath) and os.listdir(p.capath)):
        return None
    return next((f for f in CA_FILES if os.path.isfile(f)), None)


def install_ca():
    """Makes urllib (here and in il2cpp.py's range reads) use a CA bundle the firmware has where
    Python does not look. Returns False when the system has none at all."""
    f = ca_file()
    if f:
        urllib.request.install_opener(urllib.request.build_opener(
            urllib.request.HTTPSHandler(context=ssl.create_default_context(cafile=f))))
        return True
    p = ssl.get_default_verify_paths()
    return bool((p.cafile and os.path.isfile(p.cafile)) or (p.capath and os.path.isdir(p.capath) and os.listdir(p.capath)))


_unverified = []


def no_ca():
    log('  (no CA certificates on this system: downloading without checking the server; every file is '
        'checked against its sha256 instead)')
    _unverified.append(1)


def _urlopen(url, timeout=60):
    req = urllib.request.Request(url, headers={'User-Agent': 'pmunity-il2cpp-device'})
    if _unverified:                          # (no CA certificates on this system: the sha256 pins check every file)
        return urllib.request.urlopen(req, timeout=timeout, context=ssl._create_unverified_context())
    return urllib.request.urlopen(req, timeout=timeout)


def download(urls, dest, sha256, size=None, tries=4):
    """The first of urls that serves the file with that sha256 -> dest (via dest.part). A 404 moves
    on to the next URL (Debian's pool keeps only current versions; the snapshot archive keeps all)."""
    if os.path.isfile(dest) and sha_file(dest) == sha256:
        return dest
    errors = []
    for url in urls:
        for attempt in range(tries):
            try:
                h = hashlib.sha256()
                with _urlopen(url, 120) as r, open(dest + '.part', 'wb') as f:
                    while True:
                        b = r.read(1 << 20)
                        if not b:
                            break
                        h.update(b)
                        f.write(b)
                if h.hexdigest() != sha256:
                    raise IOError(f'checksum mismatch ({h.hexdigest()})')
                os.replace(dest + '.part', dest)
                return dest
            except urllib.error.HTTPError as ex:
                errors.append(f'{url}: {ex}')
                if ex.code in (403, 404, 410):
                    break
            except (OSError, IOError) as ex:
                errors.append(f'{url}: {ex}')
                if 'CERTIFICATE_VERIFY_FAILED' in str(ex) and not _unverified:
                    no_ca()
                    continue
            time.sleep(2 * (attempt + 1))
    try:
        os.unlink(dest + '.part')
    except OSError:
        pass
    raise SystemExit('download failed (is Wi-Fi on?):\n  ' + '\n  '.join(errors[-4:]))


def sha_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def deb_data(f):
    """The data.tar member of a .deb (an ar archive) as a streaming tarfile."""
    if f.read(8) != b'!<arch>\n':
        raise ValueError('not a Debian package')
    while True:
        h = f.read(60)
        if len(h) < 60:
            raise ValueError('no data.tar in the package')
        name = h[:16].decode('ascii', 'replace').strip().rstrip('/')
        size = int(h[48:58])
        start = f.tell()
        if name.startswith('data.tar'):
            mode = {'xz': 'r|xz', 'gz': 'r|gz', 'tar': 'r|'}.get(name.rsplit('.', 1)[-1])
            if mode is None:
                raise ValueError(f'{name}: compression not supported (Debian packages are xz)')
            return tarfile.open(fileobj=f, mode=mode)
        f.seek(start + size + (size & 1))


def _chmod(path, mode):
    try:
        os.chmod(path, mode)
    except OSError:                         # (exFAT / FAT: no modes; the mount gives exec to all)
        pass


def _write(path, src, mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + '.part', 'wb') as f:
        shutil.copyfileobj(src, f, 1 << 20)
    os.replace(path + '.part', path)
    if mode is not None:
        _chmod(path, mode)


def unpack_deb(deb, tc, select=place):
    """The members of a .deb that select() places -> tc. Returns the relative paths written."""
    got = []
    with open(deb, 'rb') as f:
        t = deb_data(f)
        for m in t:
            if not m.isfile():
                continue
            rel = select(m.name)
            if not rel:
                continue
            _write(os.path.join(tc, rel), t.extractfile(m), 0o755 if m.mode & 0o111 else None)
            got.append(rel)
    return got


def unpack_tgz(path, tc, prefix):
    """Microsoft's .NET runtime tarball -> tc/prefix (files only; it holds no needed links)."""
    got = []
    with tarfile.open(path, 'r:gz') as t:
        for m in t:
            if not m.isfile():
                continue
            name = m.name[2:] if m.name.startswith('./') else m.name
            if name.startswith('/') or '..' in name.split('/'):
                continue
            if name == 'dotnet':                 # (the muxer: dotnet/dotnet becomes the loader, finish())
                name = MUXER
            rel = os.path.join(prefix, name)
            _write(os.path.join(tc, rel), t.extractfile(m), 0o755 if m.mode & 0o111 else None)
            got.append(rel)
    return got


# --------------------------------------------------------------------------- the toolchain folder

def default_toolchain(home):
    return os.path.join(home, 'device-arm64')


def lock_packages(groups):
    return [p for p in LOCK.DEBIAN['packages'] if p['group'] in groups]


def lock_id(groups):
    h = hashlib.sha256(json.dumps([[p['name'], p['version'], p['sha256']] for p in lock_packages(groups)]
                                  + ([LOCK.DOTNET] if 'dotnet' in groups else []), sort_keys=True).encode())
    return h.hexdigest()[:16]


def disk_free(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize


def fetch(tc, groups, log=log):
    """Downloads and unpacks what `groups` need into tc (resumable: a package once unpacked is not
    fetched again); writes the wrappers; checks every library is there. Returns tc."""
    tc = os.path.abspath(tc)
    os.makedirs(tc, exist_ok=True)
    state = _state(tc)
    if state and state.get('layout') != LAYOUT:          # (made by an older pmunity: start again)
        for n in os.listdir(tc):
            if n != '.download':
                p = os.path.join(tc, n)
                shutil.rmtree(p) if os.path.isdir(p) else os.unlink(p)
        state = {}
    state['layout'] = LAYOUT
    if state.get('ready') == lock_id(groups) and state.get('path') == tc and all(g in state.get('groups', []) for g in groups):
        commands(tc, state['groups'])                # (cheap; they follow this pmunity's version)
        return tc
    if not install_ca():
        no_ca()
    pkgs = lock_packages(groups)
    done = set(state.get('unpacked', []))
    todo = [p for p in pkgs if _pkey(p) not in done]
    dl = os.path.join(tc, '.download')
    if todo or ('dotnet' in groups and 'dotnet' not in done):
        mb = sum(p['size'] for p in todo) / 1e6 + (LOCK.DOTNET['size'] / 1e6 if 'dotnet' in groups and 'dotnet' not in done else 0)
        need = 400 << 20
        if disk_free(tc) < need:
            raise SystemExit(f'{tc}: {disk_free(tc) >> 20} MB free; the compiler tools need about {need >> 20} MB')
        log(f'IL2CPP tools for this handheld: {len(todo)} packages, {mb:.0f} MB to download (Debian 12, Microsoft .NET)')
        os.makedirs(dl, exist_ok=True)
    for i, p in enumerate(todo, 1):
        log(f'  [{i}/{len(todo)}] {p["name"]} {p["version"]} ({p["size"] / 1e6:.1f} MB)')
        deb = os.path.join(dl, os.path.basename(p['filename']))
        download([DEBIAN + p['filename'], SNAPSHOT.format(ts=LOCK.DEBIAN['snapshot']) + p['filename']],
                 deb, p['sha256'], p['size'])
        only = ONLY.get(p['name'])
        unpack_deb(deb, tc, (lambda m: place(m) if only.match(m) else None) if only else place)
        os.unlink(deb)
        done.add(_pkey(p))
        state['unpacked'] = sorted(done)
        _save_state(tc, state)
    if 'dotnet' in groups and 'dotnet' not in done:
        d = LOCK.DOTNET
        log(f'  .NET runtime {d["version"]} ({d["size"] / 1e6:.1f} MB, Microsoft)')
        tgz = os.path.join(dl, os.path.basename(d['url']))
        download([d['url']], tgz, d['sha256'], d['size'])
        shutil.rmtree(os.path.join(tc, 'dotnet'), ignore_errors=True)
        unpack_tgz(tgz, tc, 'dotnet')
        os.unlink(tgz)
        done.add('dotnet')
        state['unpacked'] = sorted(done)
        _save_state(tc, state)
    shutil.rmtree(dl, ignore_errors=True)
    finish(tc, groups)
    state.update(ready=lock_id(groups), path=tc, groups=sorted(set(groups) | set(state.get('groups', []))))
    _save_state(tc, state)
    log(f'IL2CPP tools ready: {tc} ({du(tc) / 1e6:.0f} MB)')
    return tc


def _pkey(p):
    return f'{p["name"]}_{p["version"]}'


def _state(tc):
    try:
        with open(os.path.join(tc, '.state.json')) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _save_state(tc, state):
    p = os.path.join(tc, '.state.json')
    with open(p + '.part', 'w') as f:
        json.dump(state, f, indent=1)
    os.replace(p + '.part', p)


def du(path):
    n = 0
    for r, _, fs in os.walk(path):
        for x in fs:
            try:
                n += os.lstat(os.path.join(r, x)).st_size
            except OSError:
                pass
    return n


def finish(tc, groups):
    """Libraries under their sonames, the loader copies, the wrappers; then every root's libraries
    are checked to be in lib/ (a missing one is named, instead of a loader error later)."""
    lib = os.path.join(tc, 'lib')
    for n in sorted(os.listdir(lib)):
        p = os.path.join(lib, n)
        if n.endswith('.part') or not os.path.isfile(p):
            continue
        dyn = elf_dynamic(p)
        so = dyn[0] if dyn else None
        if so and so != n:
            if os.path.exists(os.path.join(lib, so)):
                os.unlink(p)
            else:
                os.replace(p, os.path.join(lib, so))
    commands(tc, groups)
    check(tc, groups)


def commands(tc, groups):
    """The loader's copies programs take their prefix from, and the wrappers in bin/."""
    loader = os.path.join(tc, 'lib', LOADER)
    if not os.path.isfile(loader):
        raise SystemExit(f'{tc}: glibc\'s loader is missing (the libc6 package did not unpack)')
    _chmod(loader, 0o755)
    copies = []
    if 'mono' in groups:
        copies.append(os.path.join(tc, 'mono', 'bin', 'mono'))     # Mono's prefix: <dir of this>/..
    if 'dotnet' in groups:
        # the muxer takes its name (it must be dotnet) and its root (host/fxr, shared/) from
        # /proc/self/exe: the loader's copy, named dotnet, in that folder
        copies.append(os.path.join(tc, 'dotnet', 'dotnet'))
    for c in copies:
        if not (os.path.isfile(c) and os.path.getsize(c) == os.path.getsize(loader)):
            with open(loader, 'rb') as src:
                _write(c, src, 0o755)
    write_wrappers(tc, groups)


# .NET's own optional libraries (LTTng tracing, the debugger's): never loaded by a build
DOTNET_SKIP = ('libcoreclrtraceptprovider.so', 'libmscordbi.so', 'libmscordaccore.so')


def check(tc, groups):
    """Every library the programs need (DT_NEEDED) is in lib/ (or, for .NET's, next to the one that
    needs it: they find each other through $ORIGIN); otherwise SystemExit naming each missing one."""
    have = set(os.listdir(os.path.join(tc, 'lib')))
    roots = [os.path.join(tc, r) for g in groups for r in ROOTS.get(g, [])]
    if 'dotnet' in groups:
        d = os.path.join(tc, 'dotnet')
        roots.append(os.path.join(d, MUXER))
        for r, _, fs in os.walk(d):
            roots += [os.path.join(r, n) for n in fs if n.endswith('.so') and n not in DOTNET_SKIP]
    missing = {}
    seen = set()
    todo = list(roots) + [os.path.join(tc, 'lib', n) for g in groups for n in DLOPEN.get(g, [])]
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.add(p)
        if not os.path.isfile(p):
            missing.setdefault(os.path.basename(p), 'the toolchain')
            continue
        dyn = elf_dynamic(p)
        for n in (dyn[1] if dyn else []):
            if n in have:
                todo.append(os.path.join(tc, 'lib', n))
            elif os.path.isfile(os.path.join(os.path.dirname(p), n)):
                todo.append(os.path.join(os.path.dirname(p), n))
            else:
                missing.setdefault(n, os.path.relpath(p, tc))
    if missing:
        raise SystemExit(f'{tc}: libraries missing: ' + ', '.join(f'{n} (needed by {by})' for n, by in sorted(missing.items())))


WRAP = '#!/bin/sh\n# pmunity il2cpp --on-device ({what}): run through the toolchain\'s own glibc loader\n'


def write_wrappers(tc, groups):
    """The commands in <tc>/bin (absolute paths: rewritten when the folder moves)."""
    ld = f'"{tc}/lib/{LOADER}" --library-path "{tc}/lib"'
    llvm = f'{tc}/llvm/bin'
    w = {}
    # clang: its install dir (resource headers, ld.lld) from argv[0] instead of /proc/self/exe
    for name, mode in (('clang', ''), ('clang++', ' --driver-mode=g++')):
        w[name] = WRAP.format(what=name) + JOBS_SH + NO_OUTLINE_SH + \
            f'$PMUNITY_IL2CPP_CC_WRAPPER {ld} "{llvm}/clang"{mode} -no-canonical-prefixes $nool "$@"\nst=$?\nrelease\nexit $st\n'
    w['ld.lld'] = WRAP.format(what='lld') + f'exec {ld} "{llvm}/lld" -flavor gnu $PMUNITY_LLD_FLAGS "$@"\n'
    for name, real in (('llvm-objcopy', 'llvm-objcopy'), ('llvm-strip', 'llvm-objcopy'), ('llvm-ar', 'llvm-ar'),
                       ('llvm-ranlib', 'llvm-ar'), ('llvm-nm', 'llvm-nm')):
        w[name] = WRAP.format(what=name) + f'exec {ld} --argv0 {name} "{llvm}/{real}" "$@"\n'
    if 'mono' in groups:
        # mono/bin/mono is the loader: Mono finds lib/mono/4.5 and etc/mono from that path
        w['mono'] = WRAP.format(what='Mono') + \
            f'exec "{tc}/mono/bin/mono" --library-path "{tc}/lib" "{tc}/mono/bin/mono-sgen" "$@"\n'
    if 'dotnet' in groups:
        w['dotnet'] = WRAP.format(what='.NET') + \
            f'exec "{tc}/dotnet/dotnet" --library-path "{tc}/lib" --argv0 "{tc}/dotnet/dotnet" "{tc}/dotnet/{MUXER}" "$@"\n'
    w = {os.path.join('bin', n): t for n, t in w.items()}
    w[os.path.join('llvm', 'bin', 'ld.lld')] = w[os.path.join('bin', 'ld.lld')]   # (clang looks next to itself)
    for rel, text in w.items():
        p = os.path.join(tc, rel)
        try:
            with open(p) as f:
                if f.read() == text:
                    continue
        except OSError:
            pass
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p + '.part', 'w') as f:            # (a new file: a shell running the old one reads on)
            f.write(text)
        _chmod(p + '.part', 0o755)
        os.replace(p + '.part', p)


# AArch64 compiles get -mno-outline: LLVM's machine outliner (on by default for AArch64) keeps the
# machine code of every function of the file until the end, although at -Os it outlines nothing
# (only `minsize` functions): the object files are byte-identical without it, and the compiler's
# peak on IL2CPP's largest generated file (Il2CppAttributes.cpp, 14 MB) falls from 725 to 270 MB.
NO_OUTLINE_SH = r'''nool=; c=; a64=
for a do case $a in -c) c=1 ;; aarch64*|--target=aarch64*) a64=1 ;; esac; done
[ -n "$c" ] && [ -n "$a64" ] && [ -z "$PMUNITY_IL2CPP_OUTLINER" ] && nool=-mno-outline
'''

# job slots for the compiler (POSIX sh; busybox ash and dash): PMUNITY_JOBS slots in
# PMUNITY_JOBS_DIR, taken with mkdir (atomic), freed on exit or found stale (owner gone). A source
# file larger than PMUNITY_IL2CPP_BIG_KB takes every slot (it compiles alone: the compiler's memory
# grows with the file). Slot 0 always runs; a further slot only while MemAvailable stays above
# PMUNITY_JOB_MEM_KB.
JOBS_SH = r'''slots=${PMUNITY_JOBS:-0}; jd=$PMUNITY_JOBS_DIR; held=
release() { for s in $held; do rm -rf "$jd/$s"; done; held=; }
memok() {
  [ -z "$PMUNITY_JOB_MEM_KB" ] && return 0
  while read -r k v _; do [ "$k" = MemAvailable: ] && { [ "$v" -ge "$PMUNITY_JOB_MEM_KB" ]; return; }; done < /proc/meminfo
  return 0
}
take() {   # take <slot>: 0 when this process now holds it
  if mkdir "$jd/$1" 2>/dev/null; then echo $$ > "$jd/$1/pid"; held="$held $1"; return 0; fi
  o=$(cat "$jd/$1/pid" 2>/dev/null)
  if [ -n "$o" ] && ! kill -0 "$o" 2>/dev/null; then rm -rf "$jd/$1"; fi
  return 1
}
if [ -n "$jd" ] && [ "$slots" -gt 0 ]; then
  big=; src=
  for a do case $a in *.cpp|*.c|*.cc) src=$a ;; esac; done
  if [ -n "$src" ] && [ -n "$PMUNITY_IL2CPP_BIG_KB" ] && [ -f "$src" ]; then
    kb=$(( $(wc -c < "$src") / 1024 )); [ "$kb" -ge "$PMUNITY_IL2CPP_BIG_KB" ] && big=1
  fi
  trap 'release; exit 130' INT TERM HUP
  if [ -n "$big" ]; then          # one big file at a time; it keeps the slots it got until it has all
    until take big; do sleep 1; done
    i=0; while [ $i -lt "$slots" ]; do if take $i; then i=$((i+1)); else sleep 1; fi; done
  else
    while :; do
      i=0; while [ $i -lt "$slots" ]; do
        if take $i; then
          if [ $i -eq 0 ] || memok; then break 2; fi
          release
        fi
        i=$((i+1))
      done
      sleep 1
    done
  fi
fi
'''


# --------------------------------------------------------------------------- memory and jobs

# measured in an arm64 container with the device's limits (docs/il2cpp.md, "On the handheld"):
# the compiler's peak per generated file, what il2cpp's builder keeps meanwhile, a margin
JOB_MB = int(os.environ.get('PMUNITY_IL2CPP_JOB_MB', '200'))
RESERVE_MB = int(os.environ.get('PMUNITY_IL2CPP_RESERVE_MB', '250'))
BIG_KB = int(os.environ.get('PMUNITY_IL2CPP_BIG_KB', '4096'))


def meminfo():
    out = {}
    try:
        with open('/proc/meminfo') as f:
            for line in f:
                k, v = line.split(':', 1)
                out[k] = int(v.split()[0]) // 1024
    except OSError:
        pass
    return out


def cgroup_limits():
    """(CPUs, MB of memory left) a cgroup v2 limit allows this process, or None each (a container;
    a handheld's setup normally has neither)."""
    cpus = mem = None
    try:
        with open('/sys/fs/cgroup/cpu.max') as f:
            q, per = f.read().split()
        if q != 'max':
            cpus = max(1, int(q) // int(per))
    except (OSError, ValueError):
        pass
    try:
        with open('/sys/fs/cgroup/memory.max') as f:
            lim = f.read().strip()
        if lim != 'max':
            with open('/sys/fs/cgroup/memory.current') as f:
                cur = int(f.read())
            reclaim = 0
            with open('/sys/fs/cgroup/memory.stat') as f:
                for line in f:
                    k, v = line.split()
                    if k in ('inactive_file', 'active_file'):
                        reclaim += int(v)
            mem = max(0, int(lim) - cur + reclaim) >> 20
    except (OSError, ValueError):
        pass
    return cpus, mem


def cpu_count():
    n = len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else (os.cpu_count() or 1)
    q = cgroup_limits()[0]
    return min(n, q) if q else n


def available_mb(mem=None):
    mem = meminfo() if mem is None else mem
    avail = mem.get('MemAvailable', mem.get('MemFree', 0))
    lim = cgroup_limits()[1]
    return min(avail, lim) if lim is not None else avail


def auto_jobs(cpus=None, mem=None):
    """Compiler jobs that fit: one per JOB_MB of MemAvailable beyond RESERVE_MB, at most one per
    CPU, at least one (swap is not counted: compiling from swap on an SD card is slower than
    compiling one file at a time)."""
    cpus = cpus or cpu_count()
    avail = available_mb(mem) if mem is None else mem.get('MemAvailable', mem.get('MemFree', 0))
    return max(1, min(cpus, (avail - RESERVE_MB) // JOB_MB if avail else 1))


def tree_rss_kb(root):
    """Memory of a process and its descendants (kB), from /proc: their anonymous resident memory
    plus the largest file-backed share of one of them (the compilers map the same libraries, so
    adding those up would count libLLVM once per compiler)."""
    kids, mem = {}, {}
    for d in os.listdir('/proc'):
        if not d.isdigit():
            continue
        try:
            with open(f'/proc/{d}/stat') as f:
                st = f.read()
            ppid = int(st[st.rindex(')') + 2:].split()[1])
            anon = filed = 0
            with open(f'/proc/{d}/status') as f:
                for line in f:
                    if line.startswith('RssAnon:'):
                        anon = int(line.split()[1])
                    elif line.startswith(('RssFile:', 'RssShmem:')):
                        filed += int(line.split()[1])
            mem[int(d)] = (anon, filed)
            kids.setdefault(ppid, []).append(int(d))
        except (OSError, ValueError, IndexError):
            continue
    anon, filed, todo = 0, 0, [root]
    while todo:
        p = todo.pop()
        a, f = mem.get(p, (0, 0))
        anon += a
        filed = max(filed, f)
        todo += kids.get(p, [])
    return anon + filed


class Native:
    """Runs the build's commands on this machine with the device toolchain (the Container's
    counterpart): <tc>/bin first in PATH (mono, dotnet, the compilers), HOME and TMPDIR in the work
    folder, CPU affinity limited to the job count, compiler job slots (JOBS_SH). Prints a progress
    line now and then and keeps the peak memory of each step."""

    def __init__(self, tc, jobs, work, progress=None):
        self.tc, self.work = tc, work
        cpus = cpu_count()
        self.jobs = max(1, min(jobs, cpus)) if jobs else auto_jobs(cpus)
        self.cpus = cpus
        self.peaks = {}                  # step log name -> peak MB (the process tree's resident memory)
        self.cpu = {}                    # step log name -> CPU seconds (the step and every process it waited for)
        self.progress = progress

    def env(self):
        tmp = os.path.join(self.work, 'tmp')
        os.makedirs(tmp, exist_ok=True)
        e = dict(os.environ)
        for k in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'PYTHONHOME', 'MONO_PATH', 'MONO_CFG_DIR', 'DOTNET_ROOT'):
            e.pop(k, None)
        e.update({
            'PATH': os.path.join(self.tc, 'bin') + os.pathsep + e.get('PATH', '/usr/bin:/bin'),
            'HOME': tmp, 'TMPDIR': tmp, 'DOTNET_CLI_HOME': tmp,
            'DOTNET_CLI_TELEMETRY_OPTOUT': '1', 'DOTNET_NOLOGO': '1', 'DOTNET_ROLL_FORWARD': 'Major',
            'DOTNET_SYSTEM_GLOBALIZATION_INVARIANT': '1', 'DOTNET_gcServer': '0', 'DOTNET_GCgen0size': '0x2000000',
            'PMUNITY_LLVM': os.path.join(self.tc, 'bin'),
            'PMUNITY_IL2CPP_CCLOG': os.path.join(self.work, 'cc.log'),
            'PMUNITY_JOBS': str(self.jobs), 'PMUNITY_JOBS_DIR': os.path.join(self.work, 'jobs'),
            'PMUNITY_JOB_MEM_KB': str((JOB_MB + RESERVE_MB // 2) * 1024),
            'PMUNITY_IL2CPP_BIG_KB': str(BIG_KB),
        })
        return e

    def run(self, cmd, logname, quiet=False, affinity=None):
        jd = os.path.join(self.work, 'jobs')
        shutil.rmtree(jd, ignore_errors=True)            # (slots of a killed run)
        os.makedirs(jd, exist_ok=True)
        path = os.path.join(self.work, logname)
        n = affinity or self.jobs
        cpus = sorted(os.sched_getaffinity(0))[:n] if hasattr(os, 'sched_getaffinity') else None

        def pre():
            if cpus:
                os.sched_setaffinity(0, cpus)       # (Mono's ProcessorCount: the tools' thread count)
        t0 = time.time()
        peak = [0]
        with open(path, 'w') as lf:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                                 env=self.env(), cwd=self.work, preexec_fn=pre, start_new_session=True)
            stop = threading.Event()

            def watch():
                last = time.time()
                while not stop.wait(2):
                    try:
                        peak[0] = max(peak[0], tree_rss_kb(p.pid))
                    except OSError:
                        pass
                    if self.progress and time.time() - last >= 30:
                        last = time.time()
                        msg = self.progress(time.time() - t0)
                        if msg:
                            log(msg)
            th = threading.Thread(target=watch, daemon=True)
            th.start()
            tail = []
            try:
                for line in p.stdout:
                    lf.write(line)
                    lf.flush()
                    tail = (tail + [line])[-40:]
                _, status, ru = os.wait4(p.pid, 0)
                p.returncode = rcode = os.waitstatus_to_exitcode(status)
            except BaseException:
                os.killpg(p.pid, 9)
                raise
            finally:
                stop.set()
                th.join()
        secs = time.time() - t0
        self.peaks[logname] = peak[0] // 1024
        self.cpu[logname] = round(ru.ru_utime + ru.ru_stime)
        if rcode != 0:
            sys.stderr.write(''.join(tail))
            prog = next((os.path.basename(w) for w in cmd if w.endswith(('.exe', '.dll'))), cmd[0])
            hint = ' (killed: out of memory? see docs/il2cpp.md, "On the handheld")' if rcode in (-9, 137) else ''
            raise SystemExit(f'{prog} failed (exit {rcode}){hint}; full log: {path}')
        return secs


# --------------------------------------------------------------------------- the steps, resumable

def _digest(cmd, *dirs):
    h = hashlib.sha256(json.dumps(cmd).encode())
    for d in dirs:
        for r, ds, fs in os.walk(d):
            ds.sort()
            for n in sorted(fs):
                h.update(os.path.relpath(os.path.join(r, n), d).encode())
                with open(os.path.join(r, n), 'rb') as f:
                    for b in iter(lambda: f.read(1 << 20), b''):
                        h.update(b)
    return h.hexdigest()


def _done(work, step, digest):
    try:
        with open(os.path.join(work, f'.done-{step}')) as f:
            return f.read().strip() == digest
    except OSError:
        return False


def _mark(work, step, digest):
    with open(os.path.join(work, f'.done-{step}'), 'w') as f:
        f.write(digest)


def sources_estimate(cpp, editor):
    """How many files il2cpp's builder compiles: the generated C++ and libil2cpp's sources (with
    the Boehm GC it includes)."""
    n = sum(1 for r, _, fs in os.walk(cpp) for x in fs if x.endswith(('.cpp', '.c')))
    lib = os.path.join(editor, 'il2cpp', 'libil2cpp')
    n += sum(1 for r, _, fs in os.walk(lib) for x in fs if x.endswith(('.cpp', '.c')))
    for gc in ('boehmgc', 'bdwgc'):
        extra = os.path.join(editor, 'il2cpp', 'external', gc, 'extra')
        n += sum(1 for x in os.listdir(extra) if x.endswith('.c')) if os.path.isdir(extra) else 0
    return n


def count_objects(cache):
    return sum(1 for r, _, fs in os.walk(cache) for x in fs if x.endswith('.o'))


def _mb(path):
    return du(path) / 2**20 if os.path.isdir(path) else 0


def run_steps(box, lcmd, icmd, work, staged, linked, cpp, cache, editor, strip):
    """UnityLinker, il2cpp's conversion to C++, then its build of libil2cpp.so, each skipped when a
    run before finished it with the same inputs; the build reuses every object il2cpp's cache
    holds. Returns the seconds spent."""
    secs = 0
    free = disk_free(work)
    need = 1536 << 20
    if free < need and not os.path.isdir(cpp):
        raise SystemExit(f'{work}: {free >> 20} MB free; the build needs about {need >> 20} MB '
                         '(generated C++ and objects, removed when it is done)')
    mem = meminfo()
    log(f'IL2CPP build on this machine: {box.cpus} CPUs, {mem.get("MemTotal", 0)} MB RAM '
        f'({available_mb()} MB available, swap {mem.get("SwapFree", 0)} MB free), {box.jobs} compiler job(s); '
        'it continues where it stopped if interrupted')
    link_id = 'none'
    if strip != 'none':
        link_id = _digest(lcmd, staged)
        if _done(work, 'unitylinker', link_id) and os.path.isdir(linked):
            log('UnityLinker: done before')
        else:
            log('UnityLinker: removing the unused parts of the class libraries (a few minutes)')
            shutil.rmtree(linked, ignore_errors=True)
            t = box.run(lcmd, 'unitylinker.log', affinity=1)
            secs += t
            kept = [n for n in os.listdir(linked) if n.endswith('.dll')]
            log(f'UnityLinker: {len(kept)} assemblies, {_mb(linked):.1f} MiB, in {t / 60:.1f} min '
                f'(CPU {box.cpu["unitylinker.log"] / 60:.1f} min, peak {box.peaks["unitylinker.log"]} MB)')
            _mark(work, 'unitylinker', link_id)
    conv = [w for w in icmd if w != '--compile-cpp']
    conv_id = hashlib.sha256(json.dumps([conv, link_id]).encode()).hexdigest()
    if _done(work, 'convert', conv_id) and os.path.isdir(cpp):
        log('il2cpp: C++ written before')
    else:
        log('il2cpp: converting the game\'s code to C++ (the slow single-threaded part: up to an hour)')
        shutil.rmtree(cpp, ignore_errors=True)
        box.progress = lambda el: f'  converting: {el / 60:.0f} min, {_mb(cpp):.0f} MiB of C++ so far'
        t = box.run(conv, 'il2cpp-convert.log', affinity=1)
        secs += t
        box.progress = None
        log(f'il2cpp: {_mb(cpp):.0f} MiB of C++ in {t / 60:.1f} min (CPU {box.cpu["il2cpp-convert.log"] / 60:.1f} min, '
            f'peak {box.peaks["il2cpp-convert.log"]} MB)')
        _mark(work, 'convert', conv_id)
    total = sources_estimate(cpp, editor)
    before = count_objects(cache)
    comp = [w for w in icmd if w != '--convert-to-cpp']
    log(f'il2cpp: compiling about {total} files ({before} done before), {box.jobs} at a time, then linking '
        '(the long part: one to a few hours on a handheld)')

    def progress(el):
        n = count_objects(cache)
        done = n - before
        left = max(0, total - n)
        eta = f', about {el / done * left / 60:.0f} min left' if done >= 5 else ''
        return f'  compiled {min(n, total)} of ~{total} files ({100 * min(n, total) // max(1, total)}%), ' \
               f'{el / 60:.0f} min{eta}'
    box.progress = progress
    t = box.run(comp, 'il2cpp.log')
    secs += t
    box.progress = None
    log(f'il2cpp: compiled and linked in {t / 60:.1f} min (CPU {box.cpu["il2cpp.log"] / 60:.1f} min, '
        f'peak {box.peaks["il2cpp.log"]} MB)')
    return secs


def report(box):
    return {'jobs': box.jobs, 'cpus': box.cpus, 'peak_mb': dict(box.peaks), 'cpu_seconds': dict(box.cpu),
            'mem_total_mb': meminfo().get('MemTotal')}
