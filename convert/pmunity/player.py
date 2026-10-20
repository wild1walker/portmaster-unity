"""`python -m pmunity player`: Unity's Android player of an exact Unity version, fetched from Unity.

    python -m pmunity player url <version>
    python -m pmunity player fetch <version> <dir> [--backend mono|il2cpp] [--abi armeabi-v7a|arm64-v8a]
                                                   [--installer FILE|URL] [--symbols | --symbols-only]

A game runs on the Android player of the Unity version it was built with (`python -m pmunity version
<data>` reads it). Unity's release API (services.api.unity.com) names that version's Android Build
Support installer (TargetSupportInstaller/UnitySetup-Android-Support-for-Editor-<version>.exe). It is
a non-solid NSIS installer, so only the files a port needs are read, one HTTP range request each
(nsis.py): about 10 MB of a 200-700 MB download. --installer reads a local copy (or another URL).

<dir> gets the player package layout `android-data --pkg` reads:
  Data/unity default resources
  Variations/mono/Managed/*.dll                  the Android UnityEngine assemblies (mono)
  Variations/<backend>/Release/Libs/<abi>/*.so   libmain.so, libunity.so
  Variations/mono/Release/MonoLibs/<abi>/*.so    libmono.so (Mono 2.x), libmonobdwgc-2.0.so (4.x), ...
  player.json                                    version, changeset, backend, abi, installer
  Variations/<backend>/Release/Symbols/<abi>/*.sym.so   with --symbols: libunity's and libmain's symbol
                                                 files, for crash reports and samplers (tools/analysis); a
                                                 game's own Android build (pmunity apk) carries the libraries
                                                 but not these: --symbols-only fetches just them
Unity's files stay Unity's: whoever installs a port fetches them; ports never ship them.
"""
import argparse
import fnmatch
import json
import os
import sys
import urllib.parse
import urllib.request

API = 'https://services.api.unity.com/unity/editor/release/v1/releases'
CDN = 'https://download.unity3d.com/download_unity/{rev}/TargetSupportInstaller/UnitySetup-Android-Support-for-Editor-{ver}.exe'


def release(version):
    """Unity's release record of `version` (e.g. '2019.4.40f1'), or None if Unity does not list it."""
    q = urllib.parse.urlencode({'version': version, 'limit': 1})
    with urllib.request.urlopen(f'{API}?{q}', timeout=60) as r:
        d = json.load(r)
    for rec in d.get('results', []):
        if rec.get('version') == version:
            return rec
    return None


def installer_url(rec):
    """The Windows Android Build Support installer of a release record: the API's module URL, else
    the CDN path built from the changeset (the API lists the module for every version so far)."""
    for dl in rec.get('downloads', []):
        if dl.get('platform') != 'WINDOWS':
            continue
        for m in dl.get('modules', []):
            if m.get('id') == 'android' and str(m.get('url', '')).endswith('.exe'):
                return m['url']
    rev = rec.get('shortRevision')
    if not rev:
        raise SystemExit(f"Unity's release record of {rec.get('version')} has no Android installer or changeset")
    return CDN.format(rev=rev, ver=rec['version'])


def package_path(nsis_path):
    """The installer's path of a player file relative to the AndroidPlayer folder: the first
    component is the install folder variable ('$_OUTDIR', or '$INSTDIR$_59_' in older installers)."""
    parts = nsis_path.split('/')
    return '/'.join(parts[1:]) if parts and parts[0].startswith('$') else None


def wanted(backend, abi, symbols=False, symbols_only=False):
    sym = [f'Variations/{backend}/Release/Symbols/{abi}/*.sym.so']
    if symbols_only:
        return sym
    g = ['Data/unity default resources', f'Variations/{backend}/Release/Libs/{abi}/*.so']
    if backend == 'mono':
        g += ['Variations/mono/Managed/*.dll', f'Variations/mono/Release/MonoLibs/{abi}/*.so']
    return g + (sym if symbols else [])


def fetch(version, out, backend='mono', abi='armeabi-v7a', installer=None, log=print, symbols=False, symbols_only=False):
    from . import nsis
    rec = None
    if not installer:
        rec = release(version)
        if rec is None:
            raise SystemExit(f'Unity does not list version {version} (check `python -m pmunity version <data>`)')
        installer = installer_url(rec)
    log(f'player {version} ({backend}, {abi}) from {installer}')
    ns = nsis.Nsis(installer)
    globs = wanted(backend, abi, symbols, symbols_only)
    got = []
    for path, off, _ in sorted(ns.files()):
        rel = package_path(path)
        if rel is None or not any(fnmatch.fnmatchcase(rel, g) for g in globs):
            continue
        if rel.endswith(('.sym.so', '.dbg.so')) and '/Symbols/' not in rel:
            continue
        data = ns.extract(off)
        dst = os.path.join(out, *rel.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst + '.part', 'wb') as f:
            f.write(data)
        os.replace(dst + '.part', dst)
        got.append(rel)
    libs = f'Variations/{backend}/Release/Libs/{abi}/'
    missing = [n for n in ('libmain.so', 'libunity.so') if libs + n not in got]
    if 'Data/unity default resources' not in got:
        missing.append('Data/unity default resources')
    if symbols_only:
        missing = [] if any(r.endswith('/libunity.sym.so') for r in got) else ['libunity.sym.so']
    if missing:
        raise SystemExit(f'the installer has no {", ".join(missing)} for {backend}/{abi} '
                         f'(Unity ships arm64-v8a players only with IL2CPP, from 2017.4.16 / 2018.2)')
    info = {'version': version, 'changeset': (rec or {}).get('shortRevision'), 'backend': backend, 'abi': abi,
            'installer': installer, 'files': len(got)}
    if symbols or symbols_only:
        info['symbols'] = sorted(r for r in got if '/Symbols/' in r)
    if not symbols_only:
        with open(os.path.join(out, 'player.json'), 'w') as f:
            json.dump(info, f, indent=1)
    log(f'{len(got)} files, {ns.s.fetched / 1e6:.1f} MB read -> {out}')
    return info


def main(argv):
    ap = argparse.ArgumentParser(prog='pmunity player', description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    u = sub.add_parser('url', help="the version's Android Build Support installer")
    u.add_argument('version')
    f = sub.add_parser('fetch', help="the player files a port needs, into DIR")
    f.add_argument('version'); f.add_argument('dir')
    f.add_argument('--backend', default='mono', choices=['mono', 'il2cpp'])
    f.add_argument('--abi', default='armeabi-v7a', choices=['armeabi-v7a', 'arm64-v8a'])
    f.add_argument('--installer', help='a local copy (or another URL) of the installer')
    f.add_argument('--symbols', action='store_true', help="also libunity's and libmain's symbol files (*.sym.so)")
    f.add_argument('--symbols-only', action='store_true', help='only the symbol files (for a game\'s own Android build)')
    a = ap.parse_args(argv)
    if a.cmd == 'url':
        rec = release(a.version)
        if rec is None:
            raise SystemExit(f'Unity does not list version {a.version}')
        print(installer_url(rec))
    else:
        fetch(a.version, a.dir, a.backend, a.abi, a.installer, log=lambda m: print(m, file=sys.stderr),
              symbols=a.symbols, symbols_only=a.symbols_only)
