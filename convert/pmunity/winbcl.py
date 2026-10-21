"""`python -m pmunity.winbcl`: the .NET class libraries a Windows build of Unity 2021.2+ needs for
the Android player.

    python -m pmunity.winbcl <Game>_Data <out dir> [--version V] [--installer FILE|URL]

From Unity 2021.2 a Mono build ships the class libraries (mscorlib.dll, System*.dll, ...) of its
platform's profile: a Windows build has `unityjit-win32`'s, whose file system, console and
threading code P/Invokes kernel32 (Interop+Kernel32); on the Android player that fails at once
("DllNotFoundException: __Internal ... Interop+Kernel32.SetThreadErrorMode", then System.Console
and file access, and Mono asserts on the Input System's start). A Linux or Android build has
`unityjit-linux`'s (Interop+Sys over libmono-native.so, which the Android player has). Unity
ships both profiles with the editor: the Windows editor installer
(Windows64EditorInstaller/UnitySetup64-<version>.exe, Unity's release API names it) is a
non-solid NSIS installer, so only the files needed are read, one HTTP range request each
(nsis.py): each class library of the game's Managed folder that both profiles have (or their
Facades/) is written to <out dir> as the `unityjit-linux` one (the build's own are stripped by
Unity's linker; the editor's whole files are a superset with the same assembly identity). `android-data --managed-file <out dir>/<name>` ships them instead (the
command prints the options). Up to 2021.1 the class libraries were the same for every desktop
platform; nothing is needed then.
"""
import argparse
import json
import os
import sys
import urllib.parse
import urllib.request

API = 'https://services.api.unity.com/unity/editor/release/v1/releases'
WIN, LINUX = 'MonoBleedingEdge/lib/mono/unityjit-win32/', 'MonoBleedingEdge/lib/mono/unityjit-linux/'
MARK = b'SetThreadErrorMode'                    # in the unityjit-win32 mscorlib (Interop+Kernel32)


def is_win32_profile(managed):
    """True if a Managed folder's mscorlib.dll is the unityjit-win32 one (Unity 2021.2+ Windows build)."""
    p = os.path.join(managed, 'mscorlib.dll')
    try:
        with open(p, 'rb') as f:
            return MARK in f.read()
    except OSError:
        return False


def editor_installer(version):
    q = urllib.parse.urlencode({'version': version, 'limit': 1})
    with urllib.request.urlopen(f'{API}?{q}', timeout=60) as r:
        d = json.load(r)
    for rec in d.get('results', []):
        if rec.get('version') != version:
            continue
        for dl in rec.get('downloads', []):
            if dl.get('platform') == 'WINDOWS' and str(dl.get('url', '')).endswith('.exe'):
                return dl['url']
    raise SystemExit(f"Unity's release API has no Windows editor installer for {version}")


def _rel(path):
    """An installer path relative to the editor folder ('$_OUTDIR/MonoBleedingEdge/...')."""
    parts = path.split('/')
    return '/'.join(parts[1:]) if parts and parts[0].startswith('$') else path


def replacements(managed, installer, out, log=print):
    """Writes the unityjit-linux twins of the game's unityjit-win32 class libraries to out: every
    DLL of the Managed folder that both profiles have (Unity's own names; the build's are stripped
    by Unity's linker, the editor's are whole, a superset with the same identity). Returns
    [(name, path)]."""
    from . import nsis
    ns = nsis.Nsis(installer)
    files = {}
    for path, off, _ in ns.files():
        rel = _rel(path)
        for prof in (WIN, LINUX):
            if rel.startswith(prof):
                files[(prof, rel[len(prof):])] = off
    names = sorted(n for n in os.listdir(managed) if n.lower().endswith('.dll'))
    out_list = []
    os.makedirs(out, exist_ok=True)
    for n in names:
        sub = next((s for s in (n, 'Facades/' + n) if (WIN, s) in files and (LINUX, s) in files), None)
        if sub is None:
            continue
        dst = os.path.join(out, n)
        with open(dst + '.part', 'wb') as f:
            f.write(ns.extract(files[(LINUX, sub)]))
        os.replace(dst + '.part', dst)
        out_list.append((n, dst))
    log(f'winbcl: {len(out_list)} class libraries as unityjit-linux ({ns.s.fetched / 1e6:.1f} MB read): '
        + ', '.join(n for n, _ in out_list))
    return out_list


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m pmunity.winbcl', description=__doc__.split('\n\n')[0])
    ap.add_argument('data', help="the Windows build's <Game>_Data folder")
    ap.add_argument('out', help='where the unityjit-linux class libraries go')
    ap.add_argument('--version', help="the data's Unity version (default: read from the data)")
    ap.add_argument('--installer', help='a local copy (or another URL) of the Windows editor installer')
    a = ap.parse_args(argv)
    managed = os.path.join(a.data, 'Managed')
    if not is_win32_profile(managed):
        print('winbcl: the class libraries are not the unityjit-win32 ones: nothing to do', file=sys.stderr)
        return 0
    version = a.version
    if not version:
        from . import unity_version as UV
        version = UV.of_data(a.data).text
    got = replacements(managed, a.installer or editor_installer(version), a.out, log=lambda m: print(m, file=sys.stderr))
    print(' '.join(f'--managed-file {p}' for _, p in got))
    return 0


if __name__ == '__main__':
    sys.exit(main())
