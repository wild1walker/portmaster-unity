"""`python -m pmunity patch-obb`: asset bundles added to an installed game as its patch OBB.

    python -m pmunity patch-obb new <bundles dir> <game dir>     the bundles the game does not have yet
    python -m pmunity patch-obb ggm <game dir> <out file>        the game's globalgamemanagers (its
                                                                 Unity version, for `convert`)
    python -m pmunity patch-obb pack <bundles dir> <game dir>    patch.<version>.<package>.obb
    python -m pmunity patch-obb check <names file> <bundles dir>... [--game <game dir>]
                                                                 exits 3 when a copy stopped early: a
                                                                 bundle shorter than its header says,
                                                                 or some of the add-on's bundles (the
                                                                 names file) there and some not

An add-on that comes after the setup (Cuphead's DLC: asset bundles only) is converted on its own
(`convert` on a folder of the new bundles beside the game's globalgamemanagers) and packed beside the
installed game (<game dir>: its APK, main OBB and android_package.json, as android-data left them):
Unity's Android player mounts patch.<version>.<package>.obb next to the main one and finds a bundle
in either. Each bundle gets the Android platform as android-data gives the game's own (streamed
textures inlined, the CAB platform set), the OBB the build-id marker. The built game is not touched.
"""
import json
import os
import shutil
import struct
import sys
import zipfile

from . import android_data as AD
from . import streaming as ST

PREFIX = 'assets/AssetBundles/'


def package(game):
    with open(os.path.join(game, 'android_package.json')) as f:
        return json.load(f)


def archives(game):
    pk = package(game)
    names = [pk.get('apk', ''), pk.get('main_obb', ''), f"patch.{pk['version_code']}.{pk['package']}.obb"]
    return [os.path.join(game, n) for n in names if n and os.path.isfile(os.path.join(game, n))]


def have(game):
    """The bundle names the installed game's archives hold."""
    got = set()
    for a in archives(game):
        with zipfile.ZipFile(a) as z:
            got |= {n[len(PREFIX):] for n in z.namelist() if n.startswith(PREFIX)}
    return got


def new(bundles, game):
    got = have(game)
    return sorted(n for n in os.listdir(bundles) if not n.startswith('.') and n not in got
                  and os.path.isfile(os.path.join(bundles, n)))


def short(path):
    """(size, the size its header gives) of a UnityFS bundle cut short, else None."""
    have = os.path.getsize(path)
    with open(path, 'rb') as f:
        head = f.read(512)
    need = 1                                            # (an empty file)
    if head.startswith(b'UnityFS\0'):
        try:
            _, p = AD._cstr(head, 12)
            _, p = AD._cstr(head, p)
            need, = struct.unpack_from('>q', head, p)
        except (ValueError, struct.error):
            need = len(head) + 1
    return (have, need) if have < need else None


def incomplete(names, dirs, game=None):
    """What a download or copy that stopped early left out: (the add-on's bundles missing, though
    some of them are there; [(name, size, header size)] of the bundles cut short). The add-on's
    bundles count where the installed game already has them."""
    got = have(game) if game else set()
    cut = []
    for d in dirs:
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if n.startswith('.') or not os.path.isfile(p):
                continue
            got.add(n)
            s = short(p)
            if s:
                cut.append((n, *s))
    missing = sorted(set(names) - got)
    return (missing if len(missing) < len(set(names)) else []), cut


def ggm(game, out):
    pk = package(game)
    with zipfile.ZipFile(os.path.join(game, pk['apk'])) as z, open(out, 'wb') as f:
        f.write(z.read('assets/bin/Data/globalgamemanagers'))


def pack(bundles, game, log=print):
    pk = package(game)
    obb = os.path.join(game, f"patch.{pk['version_code']}.{pk['package']}.obb")
    if os.path.exists(obb):
        raise SystemExit(f'{obb}: there is a patch OBB already')
    stage = os.path.join(bundles, '.android')
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    entries = [('assets/' + pk['build_id'], b'')]               # the main OBB's marker
    for n in sorted(os.listdir(bundles)):
        src = os.path.join(bundles, n)
        if n.startswith('.') or not os.path.isfile(src) or not AD.is_unityfs(src):
            continue
        dst = os.path.join(stage, n)
        if any(nm.endswith('.resS') for _, _, _, nm in AD.Bundle(src).nodes):
            AD.inline_bundle(src, dst)                          # (as android-data: streamed textures inlined)
        else:
            shutil.copyfile(src, dst)
            try:
                AD.patch_bundle_platform(dst)
            except ValueError as ex:                            # (LZMA blocks: rewritten as LZ4)
                ST.repack_bundle(dst, AD.ANDROID, AD.write_bytes, why=str(ex))
        entries.append((PREFIX + n, dst))
    if len(entries) == 1:
        raise SystemExit(f'{bundles}: no asset bundles')
    AD._write_zip(obb, entries, log)
    AD.verify_zip(obb, entries, log)
    shutil.rmtree(stage, ignore_errors=True)
    return obb


def main(argv):
    if len(argv) == 3 and argv[0] == 'new':
        print('\n'.join(new(argv[1], argv[2])))
    elif len(argv) == 3 and argv[0] == 'ggm':
        ggm(argv[1], argv[2])
    elif len(argv) == 3 and argv[0] == 'pack':
        pack(argv[1], argv[2], log=lambda m: print(m, file=sys.stderr))
    elif len(argv) >= 3 and argv[0] == 'check':
        a = argv[1:]
        game = a[a.index('--game') + 1] if '--game' in a else None
        if game:
            del a[a.index('--game'):a.index('--game') + 2]
        with open(a[0]) as f:
            names = [n.strip() for n in f if n.strip()]
        missing, cut = incomplete(names, a[1:], game)
        if missing or cut:
            say = [f"{len(missing)} of the {len(names)} add-on bundles missing ({', '.join(missing[:5])}"
                   f"{', ...' if len(missing) > 5 else ''})"] if missing else []
            say += [f'{n} cut short: {h} of {s} bytes' for n, h, s in cut[:5]]
            print('incomplete (a download or copy that stopped early): ' + '; '.join(say), file=sys.stderr)
            sys.exit(3)
    else:
        sys.exit(__doc__.split('\n\n')[0] + '\n' + __doc__.split('\n\n')[1])
