"""`pmunity il2cpp --on-device`: the IL2CPP build (docs/il2cpp.md) run natively on an aarch64 Linux
handheld, inside a port's first-launch setup, instead of in Docker on the porter's machine. A
port cannot ship the result (it is made from the game and from Unity's IL2CPP), so on 64-bit-only
firmwares the player's own handheld builds it.

    python -m pmunity il2cpp <data> <out> --on-device [--toolchain DIR] [--jobs N] ...
    python -m pmunity il2cpp fetch <version> --on-device [--toolchain DIR]   (downloads only)

The tools are the PC route's, as Debian 12 ("bookworm") arm64 packages: clang/lld 14 (the NDK's
x86-64 compilers stand-in, as on the PC: tools/il2cpp/ndk-clang.sh), Mono 6.8 (the .NET Framework
builds of UnityLinker, Unity 2017.4 - 2019.4) and Microsoft's .NET 8 runtime (Unity 2020.1+'s
tools; il2cpp.exe of 2017.4 - 2019.4 too, unless PMUNITY_IL2CPP_NET471=mono: the same C++ but for
how floating-point literals are written, a sixth of Mono's CPU time). They are fetched at setup time from Debian's and Microsoft's servers, each file checked
against the sha256 in il2cpp_device_lock.py, and unpacked by this module (Python's tarfile + lzma:
Debian's packages are xz-compressed) into one folder:

  <toolchain>/lib/            every shared library under its soname, glibc's loader too
  <toolchain>/llvm/bin/       clang, lld, llvm-objcopy/-ar/-nm; ld.lld (a wrapper, below)
  <toolchain>/llvm/lib/clang/14.0.6/include/   clang's own headers
  <toolchain>/mono/           mono-sgen, the 4.5 class libraries il2cpp/UnityLinker load, etc/mono
  <toolchain>/dotnet/         .NET 8 (dotnet = the loader's copy, dotnet-muxer = .NET's dotnet)
  <toolchain>/bin/            the commands the build runs: clang, clang++, ld.lld, llvm-*, mono, dotnet

Every program runs through the folder's own glibc loader (`lib/ld-linux-aarch64.so.1
--library-path lib <program>`): the firmware's libraries (often an older glibc, no libstdc++ of
the right version) are never used, nothing is installed, no root is needed and the firmware's /usr
is not touched. The folder holds no symbolic links and needs no executable bits on the programs
(only the loader and the small scripts in bin/ are executed directly), so it works on exFAT / FAT32
cards as on ext4. Programs that find their files through /proc/self/exe (the loader, when run that
way) get what they need: clang `-no-canonical-prefixes` (its resource dir and ld.lld from its
argv[0]), Mono a copy of the loader named mono/bin/mono (Mono derives its prefix from that path),
.NET a copy named dotnet/dotnet (the muxer wants that name and finds host/fxr next to itself).

The build is the PC route's (il2cpp.py), with what a 1 GB, 4 x Cortex-A53 handheld needs:
  - memory: the compiles given their turns by a scheduler (Scheduler: up to one per CPU, the
    largest first, each only while the memory the running ones are predicted to reach leaves room
    for it; a compile the OOM killer stopped runs again alone), -mno-outline (identical objects, a
    third of the memory for the largest file), one CPU for UnityLinker and the conversion (the
    2017.4 - 2019.4 tools size their thread pools by the CPUs they may use);
    PMUNITY_IL2CPP_SCHED=0: the earlier job slots instead (jobs from MemAvailable at the start,
    files above PMUNITY_IL2CPP_BIG_KB alone, one job after an OOM kill);
  - progress lines (PortMaster's patcher shows them) and a log per step in <out>/.work;
  - resumable: UnityLinker, the C++ conversion and every compiled object are kept, so a killed
    build continues where it stopped (il2cpp's object cache; conversion and compilation run as two
    il2cpp invocations).
"""
import contextlib
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
import tempfile
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


def groups_for(tools, net471='dotnet'):
    """The package groups a build needs: tools = the il2cpp/UnityLinker programs (.exe: .NET
    Framework builds run on Mono; .dll: .NET Core / .NET builds run on .NET 8). For Mono, what the
    tools' folders reference beyond the base class libraries: the netstandard facade (Unity 2019.x:
    a netstandard Mono.Cecil), System.Web.Extensions (2019.x UnityLinker's JSON). net471='dotnet'
    (the handheld's default): il2cpp.exe runs on .NET 8 (UnityLinker.exe stays on Mono)."""
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
    if any(t.endswith('.dll') for t in tools) or (exe and net471 == 'dotnet'):
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


def _default_ca():
    """Whether OpenSSL's default locations hold certificates: the CA file, or hashed names
    (<8 hex digits>.<n>) in the CA folder (a folder holding only a bundle does not count)."""
    p = ssl.get_default_verify_paths()
    if p.cafile and os.path.isfile(p.cafile):
        return True
    try:
        return any(re.match(r'^[0-9a-f]{8}\.\d+$', n) for n in os.listdir(p.capath)) if p.capath else False
    except OSError:
        return False


def ca_file():
    """A CA bundle for HTTPS when OpenSSL's default locations hold none (the setup's Python looks
    in /etc/ssl; firmwares keep theirs in different places), else None."""
    if _default_ca():
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
    return _default_ca()


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
    # PMUNITY_DISK_FREE_MB: the free space to assume instead (tools/port/rehearse.sh: a container on
    # Docker Desktop's file sharing, whose statvfs reports another disk than the folder's)
    if os.environ.get('PMUNITY_DISK_FREE_MB', '').isdigit():
        return int(os.environ['PMUNITY_DISK_FREE_MB']) << 20
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
    if state.get('ready') == lock_id(groups) and all(g in state.get('groups', []) for g in groups):
        commands(tc, state['groups'])                # (cheap; they follow this pmunity's version and
        if state.get('path') != tc:                  # the folder's place: it may have been moved)
            state['path'] = tc
            _save_state(tc, state)
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
        cc = f'$PMUNITY_IL2CPP_CC_WRAPPER {ld} "{llvm}/clang"{mode} -no-canonical-prefixes $nool "$@"'
        w[name] = WRAP.format(what=name) + NO_OUTLINE_SH + SCHED_SH.replace('@CC@', cc) + KILLED_SH + 'exit $st\n'
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

# a compiler killed by a signal (the OOM killer's SIGKILL: 137) is noted in PMUNITY_IL2CPP_KILLED
KILLED_SH = r'''if [ "$st" -gt 128 ] && [ -n "$PMUNITY_IL2CPP_KILLED" ]; then
  echo "$st ${src:-?} $(date +%H:%M:%S 2>/dev/null)" >> "$PMUNITY_IL2CPP_KILLED"
fi
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

# The compiler wrapper (clang, clang++): with PMUNITY_SCHED (a folder the build's Scheduler
# watches) the wrapper asks the scheduler for its turn instead of taking a slot itself: it writes
# q/<its pid> ("<KiB of the source> <1: a compile, 0: a link> - <source>"), waits for go/<pid>, runs
# the compiler, and removes both. A compiler killed by a signal (the OOM killer) is run once more,
# after asking for a turn alone ("<KiB> <c> alone <source>"; noted in <sched>/retried): il2cpp's
# builder never sees it fail. The wrapper's oom_score_adj (and so its compiler's) is raised, which
# needs no root for one's own processes: when memory runs out the kernel stops a compiler, which is
# run again, rather than il2cpp or the setup's Python.
# Without PMUNITY_SCHED: the slots above (JOBS_SH).
SCHED_SH = r'''sd=$PMUNITY_SCHED; me=$$; st=0
if [ -n "$sd" ] && [ -d "$sd/q" ]; then
  src=; c=0
  for a do case $a in *.cpp|*.c|*.cc) src=$a ;; -c) c=1 ;; esac; done
  kb=0; [ -n "$src" ] && [ -f "$src" ] && kb=$(( $(wc -c < "$src") / 1024 ))
  release() { rm -f "$sd/q/$me" "$sd/go/$me"; }
  trap 'release; exit 130' INT TERM HUP
  { echo 500 > /proc/self/oom_score_adj; } 2>/dev/null
  for try in 1 2; do
    if [ $try = 1 ]; then how=-; else how=alone; fi
    echo "$kb $c $how $src" > "$sd/q/$me.t" && mv "$sd/q/$me.t" "$sd/q/$me"
    while [ ! -e "$sd/go/$me" ] && [ -d "$sd/q" ]; do sleep 0.2 2>/dev/null || sleep 1; done
    @CC@
    st=$?
    release
    { [ "$st" -gt 128 ] && [ "$st" -ne 130 ] && [ $try = 1 ] && [ -d "$sd/q" ]; } || break
    echo "$st ${src:-?}" >> "$sd/retried"
  done
else
''' + JOBS_SH.replace('\n', '\n  ').rstrip() + r'''
  @CC@
  st=$?
  release
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


def dotnet_heap_limit(root='/sys/fs/cgroup'):
    """Bytes for .NET's GC heap limit under a cgroup memory limit, else None. In a container with
    a memory limit .NET caps its heap at 75% of it (a 1 GB stand-in for a handheld: 750 MB, which
    Unity 2021.3's il2cpp exceeded converting a URP game: OutOfMemoryException); a handheld's setup
    has no such limit and its .NET may use all memory and swap. So: the limit plus the swap the
    cgroup may use, less 64 MB."""
    def num(name):
        try:
            with open(os.path.join(root, name)) as f:
                v = f.read().strip()
        except OSError:
            return None
        return None if v == 'max' else int(v) if v.isdigit() else None
    mem = num('memory.max')
    if mem is None:
        return None
    swap = num('memory.swap.max')
    if swap is None:                    # (no limit of its own: the system's free swap)
        swap = meminfo().get('SwapFree', 0) << 20
    return max(mem * 3 // 4, mem + swap - (64 << 20))


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


def sched_root():
    """Where the scheduler's request files go: a RAM file system when there is one (/dev/shm, /tmp;
    the wrappers poll them), else None (the work folder)."""
    for d in ('/dev/shm', '/tmp'):
        try:
            st = os.statvfs(d)
            if os.access(d, os.W_OK) and st.f_bavail * st.f_frsize > (4 << 20):
                with open('/proc/mounts') as f:
                    if any(line.split()[1] == d and line.split()[2] in ('tmpfs', 'ramfs') for line in f):
                        return d
        except (OSError, IndexError):
            pass
    return None


def children(pid):
    """A process's children (pids), from /proc/<pid>/task/*/children; None when the kernel has no
    such files."""
    out, seen = [], False
    try:
        tasks = os.listdir(f'/proc/{pid}/task')
    except OSError:
        return []
    for t in tasks:
        try:
            with open(f'/proc/{pid}/task/{t}/children') as f:
                out += [int(x) for x in f.read().split()]
            seen = True
        except (OSError, ValueError):
            pass
    return out if seen else None


def anon_kb(pid):
    try:
        with open(f'/proc/{pid}/status') as f:
            for line in f:
                if line.startswith('RssAnon:'):
                    return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    return 0


# The scheduler's memory model: a compile's peak anonymous memory (MB) from its source's size, an
# upper bound over Cuphead's 755 compiles (Unity 2017.4's il2cpp, clang 14, -Os, -mno-outline,
# measured one at a time in an arm64 container; docs/il2cpp.md, "Fitting a 1 GB, 4 x Cortex-A53
# handheld"): il2cpp's metadata tables (Il2Cpp*.cpp: TypeDefinitions 11 MiB 446 MB, Attributes
# 17 MiB 508 MB, ...) SCHED_META, everything else (Bulk files of 2-3 MiB: at most 77 MB, libil2cpp's
# own sources: 10-32 MB) SCHED_GEN, as (MB, MB per MiB of source); a link (no source) SCHED_LINK_MB.
# Each kind's prediction is raised to what this build's own compiles of that kind reached (+10 %):
# another game or compiler cannot make it run out.
SCHED_GEN = (40, 26)
SCHED_META = (40, 37)
SCHED_LINK_MB = 400
# kept free besides the compiles' predicted memory: the compiler's and LLVM's code (file pages the
# compilers share, which MemAvailable counts as available) and a margin
SCHED_RESERVE_MB = int(os.environ.get('PMUNITY_IL2CPP_SCHED_RESERVE_MB', '160'))


class Scheduler:
    """Gives the compiler wrappers (SCHED_SH) their turns: at most `jobs` compiles at once, the
    largest waiting request first, and a further one only while the memory the running ones are
    predicted to reach still leaves room for it: MemAvailable (the cgroup's limit too) less
    SCHED_RESERVE_MB must hold the new compile's predicted peak plus what the running ones may still
    grow (their prediction less their anonymous memory now). The first compile always runs (with
    nothing else running, there is nothing to wait for); a request that does not fit waits, and
    nothing smaller passes it (the big files never starve). A retried compile ("alone") runs with
    nothing else. Requests are files in a folder on a RAM file system (the wrappers poll for theirs);
    the scheduler is a thread of the setup's Python, checking every `tick` seconds."""

    def __init__(self, jobs, reserve_mb=None, tick=0.2, root=None, available=None):
        self.dir = tempfile.mkdtemp(prefix='pmunity-cc-', dir=root or sched_root())
        os.makedirs(os.path.join(self.dir, 'q'))
        os.makedirs(os.path.join(self.dir, 'go'))
        self.jobs = max(1, jobs)
        self.reserve = SCHED_RESERVE_MB if reserve_mb is None else reserve_mb
        self.tick = tick
        self.available = available or available_mb
        self.running = {}            # pid -> [predicted MB, kind, raw prediction, peak anon kB, started]
        self.scale = {'gen': 1.0, 'meta': 1.0, 'link': 1.0}
        self.stats = {'compiles': 0, 'most_at_once': 0, 'waited_for_memory': 0.0}
        self._stop = threading.Event()
        self._th = threading.Thread(target=self._loop, daemon=True)

    def __enter__(self):
        self._th.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._th.join()
        try:
            self.step()                                    # (the last compiles counted)
        except Exception:                                  # noqa: BLE001 (the folder may be gone)
            pass
        shutil.rmtree(self.dir, ignore_errors=True)       # (a wrapper still waiting then runs)
        return False

    def retried(self):
        try:
            with open(os.path.join(self.dir, 'retried')) as f:
                return [line.split() for line in f if line.strip()]
        except OSError:
            return []

    @staticmethod
    def predict(kb, compile_, src):
        if not compile_:
            return 'link', float(SCHED_LINK_MB)
        base, per = SCHED_META if os.path.basename(src).startswith('Il2Cpp') else SCHED_GEN
        return ('meta' if (base, per) == SCHED_META else 'gen'), base + per * kb / 1024

    def _requests(self):
        q = os.path.join(self.dir, 'q')
        out = {}
        for n in os.listdir(q):
            if not n.isdigit():
                continue
            try:
                with open(os.path.join(q, n)) as f:
                    kb, c, how, src = (f.read().split(None, 3) + ['', '', '', ''])[:4]
                out[int(n)] = (int(kb), c == '1', how == 'alone', src.strip())
            except (OSError, ValueError):
                continue
        return out

    def _alive(self, pid):
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True

    def _finish(self, pid):
        """A compile is over (its wrapper removed go/<pid>, or is gone): what it reached raises its
        kind's predictions; a gone wrapper's files are removed (a wrapper running again has queued
        anew)."""
        pred, kind, raw, peak, _ = self.running.pop(pid)
        if peak and raw:
            self.scale[kind] = max(self.scale[kind], peak / 1024 / raw * 1.1)
        self.stats['compiles'] += 1
        if not self._alive(pid):
            self._finish_stale(pid)

    def step(self):
        go = os.path.join(self.dir, 'go')
        for pid in list(self.running):
            if not os.path.exists(os.path.join(go, str(pid))) or not self._alive(pid):
                self._finish(pid)
                continue
            kids = children(pid)
            kb = sum(anon_kb(k) for k in (kids or ()))
            r = self.running[pid]
            r[3] = max(r[3], kb)
        reqs = {p: r for p, r in self._requests().items() if p not in self.running}
        for p in [p for p in reqs if not self._alive(p)]:          # (a wrapper killed while it waited)
            self._finish_stale(p)
            del reqs[p]
        if not reqs or len(self.running) >= self.jobs:
            return
        order = []
        for p, (kb, c, alone, src) in reqs.items():
            kind, raw = self.predict(kb, c, src)
            order.append((not alone, -raw * self.scale[kind], p, kind, raw, alone))
        order.sort()
        avail = self.available()
        growth = sum(max(0.0, r[0] - r[3] / 1024) for r in self.running.values())
        for _, neg, p, kind, raw, alone in order:
            pred = -neg
            if len(self.running) >= self.jobs or (alone and self.running):
                break
            if self.running and pred + growth + self.reserve > avail:
                self.stats['waited_for_memory'] += self.tick
                break
            try:
                with open(os.path.join(go, str(p)), 'w'):
                    pass
            except OSError:
                break
            self.running[p] = [pred, kind, raw, 0, time.time()]
            growth += pred
            self.stats['most_at_once'] = max(self.stats['most_at_once'], len(self.running))
            if alone:
                break

    def _finish_stale(self, pid):
        for d in ('q', 'go'):
            try:
                os.unlink(os.path.join(self.dir, d, str(pid)))
            except OSError:
                pass

    def _loop(self):
        while not self._stop.wait(self.tick):
            try:
                self.step()
            except Exception as ex:          # (never stop the build: the wrappers run once the folder goes)
                log(f'il2cpp: compile scheduler: {ex!r}; the compiles now run without it')
                shutil.rmtree(self.dir, ignore_errors=True)
                return


class Native:
    """Runs the build's commands on this machine with the device toolchain (the Container's
    counterpart): <tc>/bin first in PATH (mono, dotnet, the compilers), HOME and TMPDIR in the work
    folder, CPU affinity limited to the job count (UnityLinker, the conversion), the compiles given
    their turns by a Scheduler (or compiler job slots, JOBS_SH). Prints a progress line now and then
    and keeps the peak memory of each step."""

    def __init__(self, tc, jobs, work, progress=None):
        self.tc, self.work = tc, work
        cpus = cpu_count()
        # the compiles: with the scheduler (the default; PMUNITY_IL2CPP_SCHED=0: the slots, JOBS_SH)
        # up to one per CPU as the memory allows, else as many as auto_jobs() thinks fit at the start
        self.sched = os.environ.get('PMUNITY_IL2CPP_SCHED', '1') != '0'
        self.jobs = max(1, min(jobs, cpus)) if jobs else (cpus if self.sched else auto_jobs(cpus))
        # what il2cpp's builder (its compiler threads: .NET's ProcessorCount) and the Bee graph keep
        # in flight: with the scheduler more than run, so it always has the next files to choose from
        self.threads = max(8, 3 * cpus) if self.sched else self.jobs
        self.cpus = cpus
        self.sched_stats = {}
        self.peaks = {}                  # step log name -> peak MB (the process tree's resident memory)
        self.cpu = {}                    # step log name -> CPU seconds (the step and every process it waited for)
        self.progress = progress
        self.killed_log = os.path.join(work, 'killed.log')

    def killed(self):
        """Compilers killed by a signal so far (lines of killed.log)."""
        try:
            with open(self.killed_log) as f:
                return [line.split() for line in f if line.strip()]
        except OSError:
            return []

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
            'DOTNET_GCConserveMemory': '9',      # (.NET 8 converting a 2020.3 game: peak 766 -> 666 MB)
            'PMUNITY_LLVM': os.path.join(self.tc, 'bin'),
            'PMUNITY_IL2CPP_CCLOG': os.path.join(self.work, 'cc.log'),
            'PMUNITY_JOBS': str(self.jobs), 'PMUNITY_JOBS_DIR': os.path.join(self.work, 'jobs'),
            'PMUNITY_JOB_MEM_KB': str((JOB_MB + RESERVE_MB // 2) * 1024),
            'PMUNITY_IL2CPP_BIG_KB': str(BIG_KB),
            'PMUNITY_IL2CPP_KILLED': self.killed_log,
        })
        hard = dotnet_heap_limit()
        if hard and 'DOTNET_GCHeapHardLimit' not in os.environ:
            e['DOTNET_GCHeapHardLimit'] = hex(hard)
        return e

    @contextlib.contextmanager
    def scheduled(self, on):
        """The environment additions for compiles given their turns by a Scheduler (on and
        self.sched), else {}; the scheduler's figures go to self.sched_stats."""
        if not (on and self.sched):
            yield {}
            return
        with Scheduler(self.jobs) as s:
            yield {'PMUNITY_SCHED': s.dir, 'DOTNET_PROCESSOR_COUNT': str(self.threads)}
        st = self.sched_stats
        st['compiles'] = st.get('compiles', 0) + s.stats['compiles']
        st['most_at_once'] = max(st.get('most_at_once', 0), s.stats['most_at_once'])
        st['waited_for_memory_s'] = round(st.get('waited_for_memory_s', 0) + s.stats['waited_for_memory'])
        st['retried_alone'] = st.get('retried_alone', 0) + len(s.retried())
        st['memory_scale'] = {k: round(v, 2) for k, v in s.scale.items()}
        if s.retried():
            log(f'il2cpp: {len(s.retried())} compile(s) killed (out of memory, most likely) and run again alone')

    def run(self, cmd, logname, quiet=False, affinity=None, sched=False):
        with self.scheduled(sched) as extra:
            return self._run(cmd, logname, affinity, extra)

    def _run(self, cmd, logname, affinity, extra):
        jd = os.path.join(self.work, 'jobs')
        shutil.rmtree(jd, ignore_errors=True)            # (slots of a killed run)
        os.makedirs(jd, exist_ok=True)
        path = os.path.join(self.work, logname)
        n = affinity or (self.cpus if extra else self.jobs)
        cpus = sorted(os.sched_getaffinity(0))[:n] if hasattr(os, 'sched_getaffinity') else None

        def pre():
            if cpus:
                os.sched_setaffinity(0, cpus)       # (Mono's ProcessorCount: the tools' thread count)
        t0 = time.time()
        peak = [0]
        with open(path, 'w') as lf:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                                 env=dict(self.env(), **extra), cwd=self.work, preexec_fn=pre, start_new_session=True)
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
            k = self.killed()
            if k:
                hint += (f'; {len(k)} compiler(s) were killed by a signal ({k[-1][0]}, the last on {os.path.basename(k[-1][1])}): '
                         'out of memory, most likely')
            raise SystemExit(f'{prog} failed (exit {rcode}){hint}; full log: {path}')
        return secs


    @contextlib.contextmanager
    def shell(self):
        """Many commands (Unity 2021.2+'s Bee graph, il2cpp_bee): yields sh(command, cwd, env) ->
        (exit code, output), callable from several threads (self.threads at once: the compiles among
        them get their turns from a Scheduler); the peak memory of the commands running at once and
        their CPU time are kept as the 'build.log' step's."""
        with self.scheduled(True) as extra:
            with self._shell(extra) as sh:
                yield sh

    @contextlib.contextmanager
    def _shell(self, extra):
        jd = os.path.join(self.work, 'jobs')
        shutil.rmtree(jd, ignore_errors=True)            # (slots of a killed run)
        os.makedirs(jd, exist_ok=True)
        base = dict(self.env(), **extra)
        pids, lock, stop = set(), threading.Lock(), threading.Event()
        peak, cpu = [0], [0.0]

        def watch():
            while not stop.wait(2):
                with lock:
                    now = list(pids)
                kb = 0
                for pid in now:
                    try:
                        kb += tree_rss_kb(pid)
                    except OSError:
                        pass
                peak[0] = max(peak[0], kb)

        def sh(command, cwd, env):
            p = subprocess.Popen(['/bin/sh', '-c', command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, errors='replace', env=dict(base, **env), cwd=cwd, start_new_session=True)
            with lock:
                pids.add(p.pid)
            ru = None
            try:
                out = p.stdout.read()
                p.stdout.close()
                _, status, ru = os.wait4(p.pid, 0)
                p.returncode = os.waitstatus_to_exitcode(status)
            except BaseException:
                os.killpg(p.pid, 9)
                raise
            finally:
                with lock:
                    pids.discard(p.pid)
                    if ru is not None:
                        cpu[0] += ru.ru_utime + ru.ru_stime
            return p.returncode, out
        th = threading.Thread(target=watch, daemon=True)
        th.start()
        try:
            yield sh
        finally:
            stop.set()
            th.join()
            self.peaks['build.log'] = max(self.peaks.get('build.log', 0), peak[0] // 1024)
            self.cpu['build.log'] = self.cpu.get('build.log', 0) + round(cpu[0])


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


FILE_COST = 300 << 10      # (a compile's fixed part, the headers, in bytes of generated C++ it takes as long as)


def sources_estimate(cpp, editor):
    """What il2cpp's builder compiles: the generated C++ and libil2cpp's sources (with the Boehm
    GC it includes). Returns (files, cost): cost = bytes of source + FILE_COST per file, which the
    compile time follows better than the file count (the generated files are 1 KB - 15 MB)."""
    files = [os.path.join(r, x) for r, _, fs in os.walk(cpp) for x in fs if x.endswith(('.cpp', '.c'))]
    lib = os.path.join(editor, 'il2cpp', 'libil2cpp')
    files += [os.path.join(r, x) for r, _, fs in os.walk(lib) for x in fs if x.endswith(('.cpp', '.c'))]
    for gc in ('boehmgc', 'bdwgc'):
        extra = os.path.join(editor, 'il2cpp', 'external', gc, 'extra')
        files += [os.path.join(extra, x) for x in os.listdir(extra) if x.endswith('.c')] if os.path.isdir(extra) else []
    return len(files), sum(_size(f) + FILE_COST for f in files)


def _size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


class CompileLog:
    """The compiles started so far, from cc.log (ndk-clang.sh appends each command line): their
    number and cost (as sources_estimate)."""

    def __init__(self, path):
        self.path, self.pos, self.n, self.cost = path, 0, 0, 0

    def read(self):
        try:
            with open(self.path, 'rb') as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return self
        end = data.rfind(b'\n') + 1
        self.pos += end
        for line in data[:end].decode('utf-8', 'replace').splitlines():
            m = re.search(r' (\S+\.(?:cpp|c|cc)) -o ', line)
            if m:
                self.n += 1
                self.cost += _size(m.group(1)) + FILE_COST
        return self


def count_objects(cache):
    return sum(1 for r, _, fs in os.walk(cache) for x in fs if x.endswith('.o'))


def _mb(path):
    return du(path) / 2**20 if os.path.isdir(path) else 0


def on_mono(cmd):
    """An il2cpp.exe command run on .NET 8 (dotnet exec --runtimeconfig <rc> <tool> ...) -> the same
    on Mono, or None when it is not one."""
    if cmd[:3] == ['dotnet', 'exec', '--runtimeconfig'] and cmd[4].endswith('.exe'):
        return ['mono', '--gc=sgen'] + cmd[4:]
    return None


def _outputpath(cmd):
    return next((w.split('=', 1)[1] for w in cmd if w.startswith('--outputpath=')), None)


# what il2cpp 2017.4 / 2018.4 writes beside libil2cpp.so: the unstripped library and its symbols.
# A .debug left from an earlier link makes it skip stripping the next one ("Skipping Android
# library stripping": a resumed build would keep a library twice the size), so both are removed
# before the build, and after it the symbols go to .work (as il2cpp.finish() does for the newer
# names) and the unstripped copy is deleted.
SIDE_FILES = ('libil2cpp.so.debug', 'libil2cpp.sym')


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
    jobs = f'up to {box.jobs} compiles at a time as the memory allows' if box.sched else f'{box.jobs} compiler job(s)'
    log(f'IL2CPP build on this machine: {box.cpus} CPUs, {mem.get("MemTotal", 0)} MB RAM '
        f'({available_mb()} MB available, swap {mem.get("SwapFree", 0)} MB free), {jobs}; '
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
        log('il2cpp: converting the game\'s code to C++ (one CPU: 10 minutes to an hour on a handheld)')
        shutil.rmtree(cpp, ignore_errors=True)
        box.progress = lambda el: f'  converting: {el / 60:.0f} min, {_mb(cpp):.0f} MiB of C++ so far'
        try:
            t = box.run(conv, 'il2cpp-convert.log', affinity=1)
        except SystemExit:
            if not on_mono(conv) or box.peaks.get('il2cpp-convert.log', 0) > available_mb() * 0.9:
                raise
            log('il2cpp: il2cpp.exe failed on .NET 8: converting again on Mono (as Unity\'s Linux and macOS editors run it)')
            icmd = on_mono(icmd)
            conv = [w for w in icmd if w != '--compile-cpp']
            conv_id = hashlib.sha256(json.dumps([conv, link_id]).encode()).hexdigest()
            shutil.rmtree(cpp, ignore_errors=True)
            t = box.run(conv, 'il2cpp-convert.log', affinity=1)
        secs += t
        box.progress = None
        log(f'il2cpp: {_mb(cpp):.0f} MiB of C++ in {t / 60:.1f} min (CPU {box.cpu["il2cpp-convert.log"] / 60:.1f} min, '
            f'peak {box.peaks["il2cpp-convert.log"]} MB)')
        _mark(work, 'convert', conv_id)
    total, cost = sources_estimate(cpp, editor)
    before = count_objects(cache)
    comp = [w for w in icmd if w != '--convert-to-cpp']
    if box.sched:           # (2020.x+'s il2cpp: its compiler threads; the scheduler decides what runs)
        comp = [f'--jobs={box.threads}' if w.startswith('--jobs=') else w for w in comp]
    from . import il2cpp_bee
    at = f'up to {box.jobs}' if box.sched else box.jobs
    if il2cpp_bee.uses_bee(editor):         # (2021.2+: the build graph says what, il2cpp_bee counts it)
        log(f'il2cpp: compiling ({at} at a time), then linking (the long part: half an hour to a few hours on a '
            '4-core ARM handheld)')
    else:
        log(f'il2cpp: compiling about {total} files ({before} done before), {at} at a time, then linking '
            '(the long part: half an hour to a few hours on a 4-core ARM handheld)')
    cclog = CompileLog(os.path.join(work, 'cc.log'))
    left0 = cost * max(0, total - before) / max(1, total)     # (what a resumed build has left, roughly)

    def progress(el):
        n = count_objects(cache)
        started = cclog.read()
        eta = ''
        if started.n >= 5 and started.cost:
            eta = f', about {max(1, el / started.cost * (left0 - started.cost) / 60):.0f} min left'
        return f'  compiled {min(n, total)} of ~{total} files ({100 * min(n, total) // max(1, total)}%), ' \
               f'{el / 60:.0f} min{eta}'
    box.progress = progress
    so = _outputpath(comp)
    for n in SIDE_FILES:
        if so and os.path.exists(os.path.join(os.path.dirname(so), n)):
            os.unlink(os.path.join(os.path.dirname(so), n))
    killed = len(box.killed())
    from . import il2cpp as I

    def compile_():
        # (Unity 2021.2+: il2cpp writes the Bee build graph, whose commands then run here, il2cpp_bee)
        I.clear_bee_graph(editor, cache)
        return box.run(comp, 'il2cpp.log', sched=True) + I.run_bee_graph(box, editor, cache, work, progress=bee_progress)

    def bee_progress(n, total, el):
        eta = f', about {max(1, el / n * (total - n) / 60):.0f} min left' if n >= 5 else ''
        return f'  compiled {n} of {total} ({100 * n // max(1, total)}%), {el / 60:.0f} min{eta}'
    try:
        t = compile_()
    except SystemExit:
        if len(box.killed()) == killed or box.jobs == 1:
            raise
        # a compiler ran out of memory: once more, one at a time (the objects made so far are kept)
        log(f'il2cpp: a compiler was killed (out of memory, most likely): going on with one job at a time')
        box.jobs = 1
        t = compile_()
    secs += t
    box.progress = None
    log(f'il2cpp: compiled and linked in {t / 60:.1f} min (CPU {(box.cpu["il2cpp.log"] + box.cpu.get("build.log", 0)) / 60:.1f} min, '
        f'peak {max(box.peaks["il2cpp.log"], box.peaks.get("build.log", 0))} MB)')
    for n in SIDE_FILES:
        p = os.path.join(os.path.dirname(so), n) if so else None
        if p and os.path.exists(p):
            if n.endswith('.debug'):
                os.unlink(p)
            else:
                os.replace(p, os.path.join(work, n))
    return secs


def report(box):
    return {'jobs': box.jobs, 'cpus': box.cpus, 'scheduler': dict(box.sched_stats) if box.sched else None,
            'peak_mb': dict(box.peaks), 'cpu_seconds': dict(box.cpu),
            'mem_total_mb': meminfo().get('MemTotal')}
