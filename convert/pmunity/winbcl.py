"""`python -m pmunity.winbcl` (or `pmunity winbcl`): Unity's own class libraries a desktop build
needs for the Android player, from Unity's Windows editor installer.

    python -m pmunity.winbcl <Game>_Data <out dir> [--version V] [--installer FILE|URL]
                             [--player PKG] [--no-facades] [--json FILE]

Two cases, both read from the same installer:

1. A Windows build of Unity 2021.2+. From 2021.2 a Mono build ships the class libraries
   (mscorlib.dll, System*.dll, ...) of its platform's profile: a Windows build has
   `unityjit-win32`'s, whose file system, console and threading code P/Invokes kernel32
   (Interop+Kernel32); on the Android player that fails at once ("DllNotFoundException: __Internal
   ... Interop+Kernel32.SetThreadErrorMode", then System.Console and file access, and Mono asserts
   on the Input System's start). A Linux or Android build has `unityjit-linux`'s (Interop+Sys over
   libmono-native.so, which the Android player has). Each class library of the game's Managed
   folder that both profiles have (or their Facades/) is written to <out dir> as the
   `unityjit-linux` one (the build's own are stripped by Unity's linker; the editor's whole files
   are a superset with the same assembly identity). Up to 2021.1 the class libraries were the same
   for every desktop platform; nothing is needed then.
2. With --player (a `pmunity player fetch` folder): assemblies the player's engine modules
   reference that the build does not ship (android-data's missing_references), e.g. netstandard.dll,
   which Unity 6's Android modules reference and a desktop build made without it lacks. Those
   that are Unity's own class libraries or facades (the game's profile: `unityjit-linux` from
   2021.2, `unityjit` from 2018.1; not for .NET 3.5 games) are written to <out dir> too; others
   (a package's or the developer's own assemblies, missing from the build) are only named.

The Windows editor installer (Windows64EditorInstaller/UnitySetup64-<version>.exe, Unity's release
API names it) is an NSIS installer; only the header and the files needed are read, with HTTP range
requests (nsis.py): about 6 MB for case 1 (22 class libraries and the header), about 1 MB for case
2 (the header and netstandard.dll). A solid Unity 6 installer (6000.0.85, 6000.3.26, 6000.4+) is read by
walking its 2 MiB chunks to the files: a few thousand small requests more. Nothing is read when
neither case applies. `android-data --managed-file <out dir>/<name>` ships the files instead (the
command prints the options; a port's setup does all of it: docs/porting.md).
"""
import argparse
import json
import os
import sys
import urllib.parse
import urllib.request

API = 'https://services.api.unity.com/unity/editor/release/v1/releases'
WIN, LINUX = 'MonoBleedingEdge/lib/mono/unityjit-win32/', 'MonoBleedingEdge/lib/mono/unityjit-linux/'
UNITYJIT = 'MonoBleedingEdge/lib/mono/unityjit/'   # (2018.1 - 2021.1: one profile for every platform)
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


def assembly_version(path):
    """(major, minor, build, revision) of a .NET assembly, or None."""
    from . import clrmeta as CM
    try:
        r = CM.Assembly(path).rows(0x20)
        return tuple(r[0][1:5]) if r else None
    except Exception:  # noqa: BLE001 - not an assembly we can read
        return None


def facade_profile(version, managed=None):
    """The editor installer's class library folder that matches a Mono build of this Unity version
    on the Android player (`unityjit-linux` from 2021.2, `unityjit` 2018.1 - 2021.1), or None (older,
    or a .NET 3.5 build: mscorlib 2.0)."""
    from . import unity_version as UV
    v = version if isinstance(version, UV.Version) else UV.Version(str(version))
    if v.line >= (2021, 2):
        return LINUX
    if v.line < (2018, 1):
        return None
    if managed and (assembly_version(os.path.join(managed, 'mscorlib.dll')) or (4,))[0] < 4:
        return None
    return UNITYJIT


def player_managed(player):
    """The engine modules of a `pmunity player fetch` folder (the Mono player's Managed/)."""
    return os.path.join(player, 'Variations', 'mono', 'Managed')


def missing(managed, player):
    """{assembly: [engine modules]}: what the player's engine modules reference that neither they
    nor the game's Managed folder have (android_data.missing_references)."""
    from .android_data import missing_references
    pm = player_managed(player)
    names = set()
    for d in (managed, pm):
        if os.path.isdir(d):
            names |= {n for n in os.listdir(d) if n.lower().endswith('.dll')}
    return missing_references(pm, names)


def _index(ns):
    """{installer path relative to the editor folder: block offset} of an nsis.Nsis."""
    return {_rel(path): off for path, off, _ in ns.files()}


def _nsis(installer):
    from . import nsis
    return installer if isinstance(installer, nsis.Nsis) else nsis.Nsis(installer)


def replacements(managed, installer, out, log=print, files=None):
    """Writes the unityjit-linux twins of the game's unityjit-win32 class libraries to out: every
    DLL of the Managed folder that both profiles have (Unity's own names; the build's are stripped
    by Unity's linker, the editor's are whole, a superset with the same identity). A DLL of the
    same name but another assembly version is the game's own (a NuGet package's
    System.ComponentModel.Annotations 5.0.0.0 next to Unity's 4.x facade) and is kept. installer:
    a path, a URL or an nsis.Nsis. Returns [(name, path)]."""
    own = not hasattr(installer, 'extract')
    ns = _nsis(installer)
    files = files if files is not None else _index(ns)
    names = sorted(n for n in os.listdir(managed) if n.lower().endswith('.dll'))
    out_list = []
    os.makedirs(out, exist_ok=True)
    for n in names:
        sub = next((s for s in (n, 'Facades/' + n) if WIN + s in files and LINUX + s in files), None)
        if sub is None:
            continue
        dst = os.path.join(out, n)
        with open(dst + '.part', 'wb') as f:
            f.write(ns.extract(files[LINUX + sub]))
        theirs, ours = assembly_version(os.path.join(managed, n)), assembly_version(dst + '.part')
        if theirs is not None and ours is not None and theirs != ours:
            os.remove(dst + '.part')
            log(f"winbcl: {n}: kept the game's own (version {'.'.join(map(str, theirs))}; Unity's is "
                f"{'.'.join(map(str, ours))})")
            continue
        os.replace(dst + '.part', dst)
        out_list.append((n, dst))
    log(f'winbcl: {len(out_list)} class libraries as unityjit-linux ({ns.s.fetched / 1e6:.1f} MB read): '
        + ', '.join(n for n, _ in out_list))
    if own:
        ns.close()
    return out_list


def facades(refs, profile, installer, out, log=print, files=None):
    """Writes Unity's own copies (profile, or its Facades/) of the assemblies named in refs to out.
    Returns ([(name, path)], [names not in the profile])."""
    own = not hasattr(installer, 'extract')
    ns = _nsis(installer)
    files = files if files is not None else _index(ns)
    low = {k.lower(): k for k in files if k.startswith(profile)}
    got, other = [], []
    os.makedirs(out, exist_ok=True)
    for ref in sorted(refs):
        n = ref + '.dll'
        key = next((low[k] for k in ((profile + n).lower(), (profile + 'Facades/' + n).lower()) if k in low), None)
        if key is None:
            other.append(ref)
            continue
        dst = os.path.join(out, n)
        with open(dst + '.part', 'wb') as f:
            f.write(ns.extract(files[key]))
        os.replace(dst + '.part', dst)
        v = assembly_version(dst)
        log(f"winbcl: {n}: Unity's {key[len(profile):]}" + (f" (version {'.'.join(map(str, v))})" if v else ''))
        got.append((n, dst))
    if own:
        ns.close()
    return got, other


def supply(data, out, version=None, installer=None, player=None, with_facades=True, log=print):
    """What the build in `data` needs of Unity's class libraries for the Android player (the
    module's two cases), written to out. Returns a report: {'class_libraries', 'facades': names
    written, 'not_unity': {assembly: [engine modules]} (only the developer has them), 'read_mb',
    'installer'}."""
    from . import unity_version as UV
    managed = os.path.join(data, 'Managed')
    v = UV.Version(version) if version else UV.of_data(data)
    rep = {'version': v.text, 'class_libraries': [], 'facades': [], 'not_unity': {}, 'read_mb': 0.0, 'installer': None}
    win = v.line >= (2021, 2) and is_win32_profile(managed)
    want = missing(managed, player) if with_facades and player else {}
    profile = facade_profile(v, managed) if want else None
    if not want:
        pass
    elif profile is None:
        rep['not_unity'] = want
        log(f"winbcl: the engine modules reference {', '.join(sorted(want))}, not in the build; Unity {v} "
            f"({'.NET 3.5' if v.line >= (2018, 1) else 'before 2018.1'}) has no class library profile to take them from")
        want = {}
    if not win and not want:
        if not rep['not_unity']:
            log('winbcl: nothing to do (the class libraries are not the unityjit-win32 ones'
                + (', and the build has every assembly the engine modules reference)' if player else ')'))
        return rep
    try:
        src = installer or editor_installer(v.text)
        rep['installer'] = src
        log(f"winbcl: Unity {v}'s Windows editor installer: {src}")
        ns = _nsis(src)
    except (OSError, SystemExit) as ex:
        if win:
            raise
        # (only facades wanted: the game may still run without them; android-data warns)
        rep['error'] = str(ex)
        log(f"winbcl: could not read the installer ({ex}): {', '.join(sorted(want))} left out")
        return rep
    try:
        files = _index(ns)
        if win:
            rep['class_libraries'] = [n for n, _ in replacements(managed, ns, out, log, files)]
        if want:
            got, other = facades(want, profile, ns, out, log, files)
            rep['facades'] = [n for n, _ in got]
            rep['not_unity'] = {r: want[r] for r in other}
            if other:
                log(f"winbcl: not Unity's class libraries (a package's or the developer's assemblies the build "
                    f"lacks; their types fail to load): " +
                    ', '.join(f"{r} (referenced by {', '.join(want[r][:3])})" for r in other))
        rep['read_mb'] = round(ns.s.fetched / 1e6, 2)
    finally:
        ns.close()
    log(f'winbcl: {rep["read_mb"]:.1f} MB read from the installer')
    return rep


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m pmunity.winbcl', description=__doc__.split('\n\n')[0])
    ap.add_argument('data', help="the build's <Game>_Data folder")
    ap.add_argument('out', help='where the class libraries go')
    ap.add_argument('--version', help="the data's Unity version (default: read from the data)")
    ap.add_argument('--installer', help='a local copy (or another URL) of the Windows editor installer')
    ap.add_argument('--player', help="Unity's Android player (`pmunity player fetch` folder): also the assemblies its "
                                     "engine modules reference that the build lacks (facades such as netstandard.dll)")
    ap.add_argument('--no-facades', action='store_true', help='only the Windows class libraries (case 1)')
    ap.add_argument('--json', help='write the report (what was written, what is missing) here')
    a = ap.parse_args(argv)
    rep = supply(a.data, a.out, a.version, a.installer, a.player, not a.no_facades,
                 log=lambda m: print(m, file=sys.stderr))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(rep, f, indent=1)
    print(' '.join(f'--managed-file {os.path.join(a.out, n)}' for n in rep['class_libraries'] + rep['facades']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
