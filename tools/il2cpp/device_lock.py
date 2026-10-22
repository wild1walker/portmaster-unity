#!/usr/bin/env python3
"""Writes convert/pmunity/il2cpp_device_lock.py: the Debian 12 arm64 packages and the .NET runtime
`pmunity il2cpp --on-device` fetches on a handheld, each with its sha256 (Debian's own index gives
the package hashes; the .NET tarball is hashed after downloading it once, and checked against
Microsoft's published sha512).

    tools/il2cpp/device_lock.py [--packages Packages.xz] [--dotnet 8.0.x]

Run it on the porter's machine when the pins should move (a Debian point release, a .NET 8
servicing release); the handheld never resolves anything. Old pins keep working: the setup falls
back to snapshot.debian.org (every version Debian ever published) when the pool has moved on.
"""
import argparse
import hashlib
import json
import lzma
import os
import re
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', '..', 'convert', 'pmunity', 'il2cpp_device_lock.py')
SUITE = 'bookworm'
INDEX = f'https://deb.debian.org/debian/dists/{SUITE}/main/binary-arm64/Packages.xz'
DOTNET_META = 'https://builds.dotnet.microsoft.com/dotnet/release-metadata/8.0/releases.json'

# what each group needs (no dependency resolution: Debian's dependencies pull in far more than
# these programs load; il2cpp_device.check() verifies the libraries on the unpacked files)
GROUPS = {
    'base': ['libc6', 'libgcc-s1', 'libstdc++6', 'zlib1g'],
    'llvm': ['libllvm14', 'libedit2', 'libbsd0', 'libmd0', 'libffi8', 'libtinfo6', 'libxml2', 'libicu72',
             'liblzma5', 'libz3-4', 'libclang-cpp14', 'clang-14', 'libclang-common-14-dev', 'lld-14', 'llvm-14'],
    # Mono and the class libraries il2cpp.exe / UnityLinker.exe (2017.4 - 2019.4) load
    'mono': ['mono-runtime-sgen', 'mono-runtime-common', 'libmono-corlib4.5-dll', 'libmono-system4.0-cil',
             'libmono-system-core4.0-cil', 'libmono-system-xml4.0-cil', 'libmono-system-configuration4.0-cil',
             'libmono-security4.0-cil', 'libmono-system-numerics4.0-cil', 'libmono-system-security4.0-cil',
             'libmono-posix4.0-cil',
             # libmono-native.so's GSSAPI (System.Net.Security), loaded with it
             'libgssapi-krb5-2', 'libkrb5-3', 'libk5crypto3', 'libkrb5support0', 'libcom-err2', 'libkeyutils1'],
    # Unity 2019.x's tools: Mono.Cecil built for netstandard (the facades, from mono-devel: only
    # usr/lib/mono/4.5/Facades is unpacked), UnityLinker's JSON (System.Web.Extensions)
    'mono-facades': ['mono-devel'],
    'mono-web': ['libmono-system-web-extensions4.0-cil', 'libmono-system-web4.0-cil',
                 'libmono-system-web-applicationservices4.0-cil'],
    'dotnet': ['libssl3'],               # (+ Microsoft's runtime: .NET hashes with OpenSSL on Linux)
}


def packages(path):
    out, cur, key = {}, {}, None
    with lzma.open(path, 'rt', encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line:
                if cur:
                    out.setdefault(cur['Package'], cur)
                cur = {}
                continue
            if line[0] in ' \t':
                cur[key] += '\n' + line
                continue
            key, _, v = line.partition(':')
            cur[key] = v.strip()
    if cur:
        out.setdefault(cur['Package'], cur)
    return out


def dotnet(version=None):
    with urllib.request.urlopen(DOTNET_META, timeout=60) as r:
        meta = json.load(r)
    rel = next(r for r in meta['releases'] if r.get('runtime') and (version is None or r['runtime']['version'] == version))
    f = next(f for f in rel['runtime']['files'] if f['rid'] == 'linux-arm64' and f['name'].endswith('.tar.gz'))
    h256, h512, n = hashlib.sha256(), hashlib.sha512(), 0
    with urllib.request.urlopen(f['url'], timeout=300) as r:
        for b in iter(lambda: r.read(1 << 20), b''):
            h256.update(b); h512.update(b); n += len(b)
    if h512.hexdigest() != f['hash'].lower():
        raise SystemExit(f'{f["url"]}: sha512 differs from Microsoft\'s release metadata')
    return {'version': rel['runtime']['version'], 'url': f['url'], 'size': n, 'sha256': h256.hexdigest()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--packages', help=f'a local copy of {INDEX}')
    ap.add_argument('--dotnet', help='the .NET 8 runtime version (default: the newest)')
    ap.add_argument('--keep-dotnet', action='store_true', help='keep the current .NET pin')
    a = ap.parse_args()
    path = a.packages
    if not path:
        path = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'pmunity-Packages.xz')
        urllib.request.urlretrieve(INDEX, path)
    P = packages(path)
    pk = []
    for group, names in GROUPS.items():
        for n in names:
            p = P[n]
            pk.append({'name': n, 'group': group, 'version': p['Version'], 'filename': p['Filename'],
                       'size': int(p['Size']), 'sha256': p['SHA256']})
    if a.keep_dotnet:
        sys.path.insert(0, os.path.dirname(OUT))
        import il2cpp_device_lock as old
        dn = old.DOTNET
    else:
        dn = dotnet(a.dotnet)
    snap = time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())
    text = ('"""Pins for `pmunity il2cpp --on-device` (il2cpp_device.py); written by tools/il2cpp/device_lock.py.\n'
            f'Debian {SUITE} arm64 packages (fetched from deb.debian.org, else snapshot.debian.org at\n'
            '`snapshot`) and Microsoft\'s .NET 8 runtime for linux-arm64 (Unity 2020.1+ only)."""\n'
            f'DEBIAN = {{\n    \'suite\': {SUITE!r},\n    \'snapshot\': {snap!r},\n    \'packages\': [\n')
    for p in pk:
        text += '        ' + repr(p) + ',\n'
    text += '    ],\n}\n' + f'DOTNET = {dn!r}\n'
    with open(OUT, 'w') as f:
        f.write(text)
    print(f'{os.path.normpath(OUT)}: {len(pk)} packages, {sum(p["size"] for p in pk) / 1e6:.1f} MB; .NET {dn["version"]}')


if __name__ == '__main__':
    main()
