"""apk_golden.py <work> [--pmu PMU] [--real DIR] [--only REGEX] [--keep]: `pmunity apk` (the
reference Python, convert/pmunity/apk.py) and `pmu apk` on the same inputs, case by case: exit
status, stdout, stderr (the log and the messages) and every file of the output folder (sha256) must
be the same. Runs in setup-rs's image (ua-rust: the Python at /opt/py with the setup's packages).

Cases: made-up builds (convert/tests/test_apk.py's builders: binary and protobuf manifests, ABIs,
splits, XAPK and APKS containers, App Bundles, OBBs, refusals, stored and deflated entries, names
past ASCII), and with --real each APK in DIR (a free game's Android build: never committed) as it is
and laid out again from its own files as split APKs, an XAPK, an APKS, an App Bundle and an APK
with its data in an OBB, so the zips pmu writes (merged splits, a rebuilt bundle) come from real
data. Both commands run with the output at the same path (the log names it); the Python's tree is
moved aside before pmu's run. What may differ (docs/rust-setup.md): a Python traceback against pmu's
message (both status 1), the random temporary folder in the App Bundle's log line, the text of the
note when UnityPy and unityfs both cannot read PlayerSettings, zipfile's duplicate-name warning's
source line. Exit status: 1 when a case differs.

    PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH=<worktree>/convert:/opt/pylibs \\
      /opt/py/bin/python3 tests/golden/apk_golden.py /tmp/apk-golden --pmu <pmu> [--real <dir of APKs>]
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CONVERT = os.path.join(HERE, '..', '..', '..', 'convert')
sys.path.insert(0, CONVERT)
from tests import test_apk as T          # noqa: E402  (the made-up builds' writers)
from pmunity import apk as A             # noqa: E402

BID = T.BID


def zwrite(path, entries, method=zipfile.ZIP_STORED, level=None):
    """A zip of [(name, bytes)] (or (name, bytes, method))."""
    with zipfile.ZipFile(path, 'w') as z:
        for e in entries:
            n, b = e[0], e[1]
            m = e[2] if len(e) > 2 else method
            zi = zipfile.ZipInfo(n, date_time=(2021, 5, 6, 7, 8, 10))
            zi.compress_type = m
            z.writestr(zi, b, compresslevel=level)
    return path


def entries_of(apk, keep=lambda n: True):
    """[(name, bytes, method)] of a zip's entries (the real APK's), in order."""
    out = []
    with zipfile.ZipFile(apk) as z:
        for i in z.infolist():
            if keep(i.filename):
                out.append((i.filename, z.read(i), i.compress_type))
    return out


# ------------------------------------------------------------------ made-up cases

def madeup(d):
    """[(case, [inputs], [options])] of made-up builds written into d."""
    os.makedirs(os.path.join(d, 'obbs'), exist_ok=True)
    j = lambda *p: os.path.join(d, *p)                  # noqa: E731
    mf, apk = T.manifest, T.make_apk
    cases = []
    p = apk(j('g.apk'), mf(meta=(('unity.splash-mode', 0), ('unity.build-id', BID))),
            extra={'lib/arm64-v8a/libplugin.so': b'x'})
    for t in ('arm64', 'armhf', 'armhf,arm64', 'arm64,armhf', 'auto', 'x86', 'auto,arm64', ' arm64 , '):
        cases.append((f'il2cpp-{t.strip()}', [p], ['--target', t, '--json']))
    cases.append(('il2cpp-abi-v7', [p], ['--target', 'arm64', '--abi', 'armeabi-v7a']))
    cases.append(('il2cpp-text-log', [p], ['--target', 'arm64']))
    cases.append(('il2cpp-dry', [p], ['--target', 'arm64', '--dry-run', '--json']))
    cases.append(('il2cpp-il2cpp-flag', [p], ['--target', 'arm64', '--il2cpp', '--json']))
    m = apk(j('mono.apk'), mf(), abis=('armeabi-v7a', 'x86'), backend='mono', version='2019.4.18f1')
    cases += [('mono-arm64', [m], ['--target', 'arm64', '--json']), ('mono-armhf', [m], ['--target', 'armhf', '--json']),
              ('mono-il2cpp-armhf-first', [m], ['--target', 'armhf,arm64', '--il2cpp', '--json']),
              ('mono-dry', [m], ['--target', 'armhf', '--dry-run', '--json'])]
    for name, kw in (('x86', dict(abis=('x86',))), ('nolib', dict(abis=())), ('noscripting', dict(backend=None)),
                     ('old', dict(version='5.5.4f1')), ('nodata', dict(data=False)), ('nometa', dict(metadata=False)),
                     ('vr', dict(extra={'lib/arm64-v8a/libvrapi.so': b'x'})), ('new', dict(version='6000.7.1f1')),
                     ('utf8', dict(axml_kw={'utf8': True})), ('stripped', dict(axml_kw={'strip_names': True})),
                     ('v8only', dict(abis=('arm64-v8a',)))):
        q = apk(j(f'{name}.apk'), mf(), **kw)
        cases.append((name, [q], ['--target', 'arm64', '--json']))
        cases.append((name + '-armhf', [q], ['--target', 'armhf']))
    cases.append(('old-any', [j('old.apk')], ['--target', 'arm64', '--any-unity-version', '--json']))
    cases.append(('vr-force', [j('vr.apk')], ['--target', 'arm64', '--force', '--json']))
    nounity = zwrite(j('nounity.apk'), [('AndroidManifest.xml', T.axml(mf())), ('lib/arm64-v8a/libgame.so', b'x')])
    cases.append(('nounity', [nounity], ['--target', 'arm64']))
    gl = apk(j('gles.apk'), mf(gles=0x30002 + 1, meta=(('a', 'x'), ('b', True))), extra={
        'assets/bin/Data/Managed/etc/mono/config': b'c'})
    cases.append(('gles-note', [gl], ['--target', 'arm64', '--json']))
    # OBBs: beside, of another build, of another package, patch and versionCode
    b = j('build')
    os.makedirs(b, exist_ok=True)
    g = apk(os.path.join(b, 'Game.apk'), mf(meta=(('unity.build-id', BID),)), data=False,
            extra={'assets/bin/Data/Managed/Metadata/global-metadata.dat': struct.pack('<II', 0xFAB11BAF, 27)})
    T.make_obb(os.path.join(b, 'Game.main.obb'), BID)
    with zipfile.ZipFile(os.path.join(b, 'Game.main.obb'), 'a') as z:
        z.writestr('assets/bin/Data/globalgamemanagers', T.sf_head('2020.3.3f1'))
        z.writestr('unity_obb_guid', b'guid\n')
    cases.append(('obb-beside', [g], ['--target', 'arm64', '--json']))
    cases.append(('obb-beside-dry', [g], ['--target', 'arm64', '--dry-run', '--json']))
    other = T.make_obb(j('obbs', 'main.7.com.test.game.obb'), BID.replace('0123', '9999'))
    g2 = apk(j('G2.apk'), mf(meta=(('unity.build-id', BID),)))
    cases += [('obb-other', [g2, other], ['--target', 'arm64']), ('obb-other-force', [g2, other], ['--target', 'arm64', '--force', '--json'])]
    wrong = T.make_obb(j('obbs', 'main.7.com.other.obb'), BID)
    cases.append(('obb-package', [g2, wrong], ['--target', 'arm64']))
    g12 = apk(j('g12.apk'), mf(code=12))
    mo = T.make_obb(j('obbs', 'main.11.com.test.game.obb'), BID)
    q = T.make_obb(j('obbs', 'x.patch.obb'), BID)
    q2 = T.make_obb(j('obbs', 'y-PATCH.obb'), BID)
    cases += [('obb-patch-code', [g12, mo, q], ['--target', 'arm64', '--json']),
              ('obb-two-patches', [g12, q, q2], ['--target', 'arm64'])]
    notzip = j('obbs', 'bad.obb')
    with open(notzip, 'wb') as f:
        f.write(b'not a zip')
    cases.append(('obb-not-zip', [g12, notzip], ['--target', 'arm64']))
    # splits, containers
    base = apk(j('base.apk'), mf(meta=(('unity.build-id', BID),)), abis=())
    cfg = zwrite(j('config.arm64_v8a.apk'), [('AndroidManifest.xml', T.axml(mf(split='config.arm64_v8a')))] +
                 [(f'lib/arm64-v8a/{n}', b'split') for n in ('libmain.so', 'libunity.so', 'libil2cpp.so')])
    obb = T.make_obb(j('obbs', 'o.obb'), BID)
    x = zwrite(j('game.xapk'), [('com.test.game.apk', T.rd(base)), ('config.arm64_v8a.apk', T.rd(cfg)),
                               ('Android/obb/com.test.game/main.7.com.test.game.obb', T.rd(obb)),
                               ('manifest.json', b'{}'), ('icon.png', b'')], zipfile.ZIP_DEFLATED)
    cases += [('xapk', [x], ['--target', 'arm64', '--json']), ('xapk-dry', [x], ['--target', 'arm64', '--dry-run', '--json']),
              ('base-alone', [base], ['--target', 'arm64']), ('splits', [base, cfg], ['--target', 'arm64', '--json']),
              ('splits-only', [cfg], ['--target', 'arm64'])]
    standalone = apk(j('standalone.apk'), mf())
    apks = zwrite(j('game.apks'), [('toc.pb', b''), ('splits/base-master.apk', T.rd(base)),
                                   ('splits/base-arm64_v8a.apk', T.rd(cfg)),
                                   ('standalones/standalone-arm64_v8a.apk', T.rd(standalone))])
    cases.append(('apks', [apks], ['--target', 'arm64', '--json']))
    two = zwrite(j('two.xapk'), [('a.apk', T.rd(standalone)), ('b.apk', T.rd(g12))])
    cases.append(('two-bases', [two], ['--target', 'arm64']))
    otherpkg = apk(j('otherpkg.apk'), mf(pkg='com.other', split='config.arm64_v8a'), abis=())
    cases.append(('split-other-package', [standalone, otherpkg], ['--target', 'arm64']))
    # an asset pack split merged into the APK: stored and deflated entries, a name past ASCII,
    # directory entries, a name twice
    base2 = T.make_apk(j('base2.apk'), mf(), data=False, extra={'assets/bin/Data/x': b'base' * 300})
    with zipfile.ZipFile(base2, 'a') as z:
        z.writestr(zipfile.ZipInfo('assets/'), b'')
        z.writestr('dup.bin', b'one')
        z.writestr('dup.bin', b'two')
        zi = zipfile.ZipInfo('res/raw/café.txt', date_time=(2019, 1, 2, 3, 4, 5))
        zi.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(zi, 'café ' * 500)
    pack = zwrite(j('UnityDataAssetPack.apk'), [
        ('AndroidManifest.xml', T.axml(mf(split='UnityDataAssetPack')), zipfile.ZIP_DEFLATED),
        ('assets/bin/Data/globalgamemanagers', T.sf_head('2022.3.10f1')),
        ('assets/bin/Data/Managed/Metadata/global-metadata.dat', struct.pack('<II', 0xFAB11BAF, 31)),
        ('assets/bin/Data/data.unity3d', os.urandom(3000) + b'D' * 300000, zipfile.ZIP_DEFLATED),
        ('assets/bin/Data/x', b'pack'), ('assets/empty', b''), ('assets/empty2', b'', zipfile.ZIP_DEFLATED),
        ('lib/arm64-v8a/libskip.so', b'not merged')], level=9)
    cases.append(('asset-pack', [base2, pack], ['--target', 'arm64', '--json']))
    cases.append(('asset-pack-dry', [base2, pack], ['--target', 'arm64', '--dry-run', '--json']))
    # App Bundles
    aab = zwrite(j('game.aab'), [
        ('BundleConfig.pb', b''),
        ('base/manifest/AndroidManifest.xml', T.proto_xml(mf(meta=(('unity.build-id', BID),))), zipfile.ZIP_DEFLATED),
        *[(f'base/lib/arm64-v8a/{n}', b'aab' * 100, zipfile.ZIP_DEFLATED) for n in ('libmain.so', 'libunity.so', 'libil2cpp.so')],
        ('base/dex/classes.dex', b'dex'), ('base/root/unity_extra', b'r'), ('base/assets/', b''),
        ('base/assets/bin/Data/Managed/Metadata/global-metadata.dat', struct.pack('<II', 0xFAB11BAF, 29)),
        ('UnityDataAssetPack/manifest/AndroidManifest.xml', T.proto_xml(
            ('manifest', [('package', 'com.test.game'), ('split', 'UnityDataAssetPack')],
             [('dist:module', [], [('dist:delivery', [], [('dist:install-time', [], [])])])]))),
        ('UnityDataAssetPack/assets/bin/Data/globalgamemanagers', T.sf_head('2021.3.20f1')),
        ('Later/manifest/AndroidManifest.xml', T.proto_xml(
            ('manifest', [('package', 'com.test.game'), ('split', 'Later')],
             [('dist:module', [], [('dist:delivery', [], [('dist:on-demand', [], [])])])]))),
        ('Later/assets/later.bin', b'L' * 5000, zipfile.ZIP_DEFLATED)], level=1)
    cases += [('aab', [aab], ['--target', 'arm64', '--json']), ('aab-dry', [aab], ['--target', 'arm64', '--dry-run', '--json']),
              ('aab-with-apk', [aab, standalone], ['--target', 'arm64']), ('two-aabs', [aab, aab], ['--target', 'arm64'])]
    # bad inputs
    junk = j('junk.apk')
    with open(junk, 'wb') as f:
        f.write(b'not a zip')
    other_zip = zwrite(j('other.zip'), [('readme.txt', b'x')])
    evil = zwrite(j('evil.xapk'), [('../../g.apk', T.rd(p))])
    cases += [('junk', [junk], []), ('other-zip', [other_zip], []), ('evil', [evil], []),
              ('missing', [j('nope.apk')], []), ('obb-only', [obb], ['--target', 'arm64']),
              ('noargs', [], []), ('bad-abi-opt', [p], ['--abi', 'mips'])]
    return cases


# ------------------------------------------------------------------ real APKs, laid out again

def real(apk, d):
    """[(case, [inputs], [options])] of a real APK as it is and laid out as splits, containers,
    a bundle and an OBB (its own files)."""
    os.makedirs(d, exist_ok=True)
    stem = os.path.splitext(os.path.basename(apk))[0]
    j = lambda *p: os.path.join(d, *p)                  # noqa: E731
    with zipfile.ZipFile(apk) as z:
        mfb = z.read('AndroidManifest.xml')
    mf = A.Manifest(A.parse_manifest(mfb))
    pkg, code = mf.package, mf.version_code
    abis = sorted({n.split('/')[1] for n in zipfile.ZipFile(apk).namelist() if n.startswith('lib/') and n.count('/') == 2})
    cases = [(f'{stem}', [apk], ['--target', 'arm64,armhf', '--json']),
             (f'{stem}-armhf', [apk], ['--target', 'armhf']),
             (f'{stem}-dry', [apk], ['--dry-run', '--json', '--target', 'armhf,arm64'])]
    # base without its libraries + config splits with them
    abi = abis[0] if abis else 'armeabi-v7a'
    sm = lambda split: T.axml(T.manifest(pkg=pkg, code=code, split=split))   # noqa: E731
    base = zwrite(j(f'{stem}-base.apk'), entries_of(apk, lambda n: not n.startswith('lib/')))
    cfgs = []
    for a in abis:
        cfgs.append(zwrite(j(f'{stem}-config.{a}.apk'), [('AndroidManifest.xml', sm('config.' + a.replace('-', '_')),
                                                           zipfile.ZIP_DEFLATED)] +
                           entries_of(apk, lambda n, a=a: n.startswith(f'lib/{a}/'))))
    cases.append((f'{stem}-splits', [base] + cfgs, ['--target', 'arm64,armhf', '--json']))
    # the data in an asset pack split (merged into one APK: the zip pmu writes)
    nodata = zwrite(j(f'{stem}-nodata.apk'), entries_of(apk, lambda n: not n.startswith(('lib/', 'assets/bin/'))))
    pack = zwrite(j(f'{stem}-UnityDataAssetPack.apk'), [('AndroidManifest.xml', sm('UnityDataAssetPack'))] +
                  entries_of(apk, lambda n: n.startswith('assets/bin/')))
    cases.append((f'{stem}-assetpack', [nodata, pack] + cfgs, ['--target', 'arm64,armhf', '--json']))
    # an XAPK of those, and an APKS
    xapk = zwrite(j(f'{stem}.xapk'), [(f'{pkg}.apk', T.rd(nodata))] +
                  [(os.path.basename(c).replace(f'{stem}-', ''), T.rd(c)) for c in cfgs] +
                  [('UnityDataAssetPack.apk', T.rd(pack)), ('manifest.json', json.dumps({'package_name': pkg}).encode())])
    cases.append((f'{stem}-xapk', [xapk], ['--target', 'armhf,arm64', '--json']))
    apks = zwrite(j(f'{stem}.apks'), [('toc.pb', b''), ('splits/base-master.apk', T.rd(base))] +
                  [(f'splits/base-{a.replace("-", "_")}.apk', T.rd(c)) for a, c in zip(abis, cfgs)] +
                  [('standalones/standalone.apk', T.rd(apk))])
    cases.append((f'{stem}-apks', [apks], ['--target', 'arm64,armhf', '--json']))
    # an App Bundle of its files (a protobuf manifest with the APK's package and versionCode)
    ents = [('BundleConfig.pb', b''), ('base/manifest/AndroidManifest.xml',
                                       T.proto_xml(T.manifest(pkg=pkg, code=code, meta=tuple(
                                           (k, v) for k, v in mf.meta.items() if isinstance(v, str)))))]
    for n, b, m in entries_of(apk):
        if n.startswith('lib/') or n.startswith('assets/') and not n.startswith('assets/bin/'):
            ents.append(('base/' + n, b, m))
        elif n.startswith('assets/bin/'):
            ents.append(('UnityDataAssetPack/' + n, b, m))
        elif n.endswith('.dex'):
            ents.append(('base/dex/' + n, b, m))
        elif n != 'AndroidManifest.xml' and not n.startswith('META-INF/'):
            ents.append(('base/root/' + n, b, m))
    ents.append(('UnityDataAssetPack/manifest/AndroidManifest.xml', T.proto_xml(
        ('manifest', [('package', pkg), ('split', 'UnityDataAssetPack')],
         [('dist:module', [], [('dist:delivery', [], [('dist:install-time', [], [])])])]))))
    aab = zwrite(j(f'{stem}.aab'), ents)
    cases.append((f'{stem}-aab', [aab], ['--target', 'arm64,armhf', '--json']))
    # its data in an OBB (Unity's split application binary), beside the APK
    ob = os.path.join(d, 'obb')
    os.makedirs(ob, exist_ok=True)
    bid = mf.meta.get('unity.build-id')
    lean = zwrite(os.path.join(ob, f'{stem}.apk'), entries_of(apk, lambda n: not n.startswith('assets/bin/Data/') or
                                                                 n.startswith('assets/bin/Data/Managed/')))
    zwrite(os.path.join(ob, f'{stem}.main.obb'), ([(f'assets/{bid}', b'')] if bid else []) +
           [e for e in entries_of(apk, lambda n: n.startswith('assets/bin/Data/') and
                                  not n.startswith('assets/bin/Data/Managed/'))])
    cases.append((f'{stem}-obb', [lean], ['--target', 'arm64,armhf', '--json']))
    return cases


# ------------------------------------------------------------------ running and comparing

def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True)
    return p.returncode, p.stdout.decode('utf-8', 'replace'), p.stderr.decode('utf-8', 'replace')


def tree(d):
    out = {}
    if not os.path.isdir(d):
        return None
    for r, ds, fs in os.walk(d):
        ds.sort()
        rel = os.path.relpath(r, d)
        out[rel + '/'] = 'dir'
        for f in sorted(fs):
            p = os.path.join(r, f)
            with open(p, 'rb') as fh:
                out[os.path.normpath(os.path.join(rel, f))] = hashlib.sha256(fh.read()).hexdigest()
    return out


NOTE = re.compile(r'^note: could not read PlayerSettings \(.*\)$', re.M)
AAB_TMP = re.compile(r'/\.apk-[a-z0-9_]{6,8}/')
DUP = re.compile(r'^.*UserWarning: (Duplicate name: .*)$\n(  return self\._open_to_write.*\n)?', re.M)


def norm(text):
    text = NOTE.sub('note: could not read PlayerSettings (...)', text)
    text = AAB_TMP.sub('/.apk-*/', text)
    return DUP.sub(r'UserWarning: \1\n', text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('work')
    ap.add_argument('--pmu', default=os.environ.get('PMU', 'pmu'))
    ap.add_argument('--real', help='a folder of APKs (free games\' Android builds)')
    ap.add_argument('--only', default='.')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    w = os.path.abspath(a.work)
    shutil.rmtree(w, ignore_errors=True)
    cases = madeup(os.path.join(w, 'in'))
    if a.real:
        for f in sorted(os.listdir(a.real)):
            if f.lower().endswith('.apk'):
                cases += real(os.path.join(a.real, f), os.path.join(w, 'real', os.path.splitext(f)[0]))
    py = [sys.executable, '-m', 'pmunity', 'apk']
    bad = 0
    rows = []
    for name, inputs, opts in cases:
        if not re.search(a.only, name):
            continue
        out = os.path.join(w, 'run', name, 'out')
        res = {}
        for side, cmd in (('py', py), ('rs', [a.pmu, 'apk'])):
            shutil.rmtree(os.path.join(w, 'run', name), ignore_errors=True)
            os.makedirs(os.path.join(w, 'run', name))
            st, so, se = run(cmd + inputs + [out] + opts, CONVERT)
            res[side] = (st, so, se, tree(out))
            shutil.rmtree(os.path.join(w, side, name), ignore_errors=True)
            os.makedirs(os.path.join(w, side), exist_ok=True)
            os.rename(os.path.join(w, 'run', name), os.path.join(w, side, name))
        (ps, po, pe, pt), (rs, ro, re_, rt) = res['py'], res['rs']
        why = []
        if ps != rs:
            why.append(f'status {ps} != {rs}')
        if norm(po) != norm(ro):
            why.append('stdout')
        trace = ps == rs == 1 and 'Traceback (most recent call last)' in pe
        if not trace and norm(pe) != norm(re_):
            why.append('stderr')
        if pt != rt:
            diff = sorted(k for k in set(pt or {}) | set(rt or {}) if (pt or {}).get(k) != (rt or {}).get(k))
            why.append('files: ' + ', '.join(diff[:6]))
        n_files = len([k for k in (pt or {}) if not k.endswith('/')])
        rows.append((name, ps, n_files, 'same' + (' (traceback / message)' if trace else '') if not why else 'DIFFERS: ' + '; '.join(why)))
        if why:
            bad += 1
            for lab, (st, so, se, _) in res.items():
                print(f'--- {name} {lab}: status {st}\n{so}{se}', file=sys.stderr)
    print('| case | status | files | result |\n|---|---|---|---|')
    for r in rows:
        print(f'| {r[0]} | {r[1]} | {r[2]} | {r[3]} |')
    print(f'{len(rows) - bad} of {len(rows)} the same')
    if not a.keep:
        shutil.rmtree(w, ignore_errors=True)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
