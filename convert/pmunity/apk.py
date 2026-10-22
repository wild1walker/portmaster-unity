"""`python -m pmunity apk`: a game the player owns as an Android build (APK, XAPK, APKS, AAB, OBBs),
made ready for ua_host. Nothing is converted: the build already holds Unity's Android player and the
data it reads (GLES shaders, ASTC/ETC textures); the tool picks the ABI, extracts the native
libraries, keeps the APK as the package code path, names the OBBs as the player looks for them and
writes android_package.json.

    python -m pmunity apk <file>... <out> [--target auto|arm64|armhf] [--abi arm64-v8a|armeabi-v7a]
                          [--any-unity-version] [--force] [--dry-run]

<file>: an .apk (with OBBs beside it or given too), split APKs (base + config.<abi> + asset packs),
an .xapk / .apks / .apkm-style zip of those (and of OBBs under Android/obb/), or an .aab (Android
App Bundle: rebuilt into one APK, as bundletool would for a device).

<out> gets what ua_host runs:
  base.apk                       the game's APK, unchanged (a clone or link when possible; split
                                 installs with Unity data in asset packs get one merged APK)
  main.<code>.<package>.obb      OBBs under the names the player derives from the manifest
  patch.<code>.<package>.obb
  lib/*.so                       lib/<abi>/ of the APK (and of its ABI split): libmain, libunity,
                                 libil2cpp or libmono*, plugins
  assets/bin/Data/Managed/etc/   Mono's config (Mono builds), read relative to the working dir
  android_package.json           package, versionCode/Name, min/target SDK, manifest meta-data
                                 (unity.build-id ties the OBB to the APK), ABI, backend, Unity version

ABI: arm64-v8a for a 64-bit userland, armeabi-v7a for a 32-bit one (--target; auto = the machine
running the tool when it is an ARM Linux, else arm64-v8a when the build has it). Unity ships
arm64-v8a players only with IL2CPP; Mono builds are armeabi-v7a. --abi picks one directly.

Refused, with the reason: builds without Unity's player (libmain/libunity) for an ARM ABI, x86-only
builds, an ABI the target cannot run, Vulkan-only builds (the runtime draws with GLES), IL2CPP
builds without their metadata, Unity data that is not there (an OBB or asset pack not given), Unity
versions outside the runtime's range (--any-unity-version), VR (headset) builds and OBBs of another
build (--force).
"""
import argparse
import io
import json
import os
import platform
import re
import shutil
import struct
import sys
import tempfile
import zipfile

from . import unity_version as UV

ABIS = ('arm64-v8a', 'armeabi-v7a')
TARGETS = {'arm64': 'arm64-v8a', 'armhf': 'armeabi-v7a'}    # --target userland -> ABI
# headset runtimes (Oculus / Meta, OpenXR, Pico) and VR launcher categories: such builds render for
# a headset's two eyes and need its tracking
VR_LIBS = {'libvrapi.so', 'libOVRPlugin.so', 'libopenxr_loader.so', 'libpxr_api.so', 'libPvr_UnitySDK.so',
           'libovrplatformloader.so', 'libUnityOpenXR.so', 'libgvr.so'}
VR_CATEGORIES = ('com.oculus.intent.category.VR', 'org.khronos.openxr.intent.category.IMMERSIVE_HMD',
                 'com.google.intent.category.CARDBOARD', 'com.google.intent.category.DAYDREAM')
OTHER_ABIS = ('x86', 'x86_64', 'armeabi', 'mips', 'mips64', 'riscv64')
IL2CPP_MAGIC = 0xFAB11BAF
GFX = {8: 'GLES2', 11: 'GLES3', 21: 'Vulkan', 17: 'Metal', 16: 'GLCore', 2: 'D3D11'}
# PlayerSettings.defaultScreenOrientation
ORIENTATIONS = {0: 'portrait', 1: 'portrait upside down', 2: 'landscape right', 3: 'landscape left', 4: 'auto rotation'}


class Refused(Exception):
    """A build ua_host cannot run, with the reason."""


# --------------------------------------------------------------------------------------------
# Android binary XML (AXML: AndroidManifest.xml in an APK)
# --------------------------------------------------------------------------------------------

# android: attribute resource ids, for manifests whose attribute names are stripped or obfuscated
ATTR_IDS = {
    0x01010003: 'name', 0x01010024: 'value', 0x01010025: 'resource', 0x0101021b: 'versionCode',
    0x0101021c: 'versionName', 0x0101020c: 'minSdkVersion', 0x01010270: 'targetSdkVersion',
    0x01010271: 'maxSdkVersion', 0x01010281: 'glEsVersion', 0x0101028e: 'required',
    0x010104ea: 'extractNativeLibs', 0x0101000c: 'hasCode', 0x01010001: 'label',
    0x01010572: 'compileSdkVersion', 0x01010591: 'isSplitRequired', 0x0101055e: 'isFeatureSplit',
    0x01010018: 'screenOrientation', 0x0101001f: 'configChanges', 0x01010000: 'theme',
}


class Element:
    def __init__(self, tag, attrs):
        self.tag, self.attrs, self.children = tag, attrs, []

    def iter(self, tag=None):
        if tag is None or self.tag == tag:
            yield self
        for c in self.children:
            yield from c.iter(tag)

    def get(self, name, default=None):
        return self.attrs.get(name, default)

    def __repr__(self):
        return f'<{self.tag} {self.attrs}>'


def _string_pool(b, off):
    _, hsize, size = struct.unpack_from('<HHI', b, off)
    count, _styles, flags, start, _sstart = struct.unpack_from('<IIIII', b, off + 8)
    utf8 = flags & 0x100
    offs = struct.unpack_from(f'<{count}I', b, off + hsize)
    out = []
    for o in offs:
        p = off + start + o
        if utf8:
            n = b[p]; p += 1
            if n & 0x80:
                p += 1
            n = b[p]; p += 1
            if n & 0x80:
                n = ((n & 0x7f) << 8) | b[p]; p += 1
            out.append(b[p:p + n].decode('utf-8', 'replace'))
        else:
            n, = struct.unpack_from('<H', b, p); p += 2
            if n & 0x8000:
                n = ((n & 0x7fff) << 16) | struct.unpack_from('<H', b, p)[0]; p += 2
            out.append(b[p:p + 2 * n].decode('utf-16-le', 'replace'))
    return out


def _value(dtype, data, raw, strings):
    if dtype == 0x03:                                       # TYPE_STRING
        return strings[data] if data < len(strings) else ''
    if dtype == 0x12:                                       # TYPE_INT_BOOLEAN
        return 'true' if data else 'false'
    if dtype == 0x10:                                       # TYPE_INT_DEC
        return str(struct.unpack('<i', struct.pack('<I', data))[0])
    if dtype == 0x11:                                       # TYPE_INT_HEX
        return '0x%x' % data
    if dtype == 0x04:                                       # TYPE_FLOAT
        return repr(struct.unpack('<f', struct.pack('<I', data))[0])
    if dtype in (0x01, 0x07):                               # (dynamic) reference: a resource
        return '@0x%08x' % data
    if dtype == 0x00:
        return None
    if raw is not None and raw < len(strings):
        return strings[raw]
    return '0x%x' % data


def parse_axml(b):
    """The element tree of a binary AndroidManifest.xml (attribute names without namespace)."""
    if len(b) < 8 or struct.unpack_from('<H', b, 0)[0] != 0x0003:
        raise ValueError('not an Android binary XML file')
    strings, resmap = [], []
    root, stack = None, []
    _, hsize, total = struct.unpack_from('<HHI', b, 0)
    off = hsize
    end = min(total, len(b))
    while off + 8 <= end:
        ctype, chsize, csize = struct.unpack_from('<HHI', b, off)
        if csize < 8 or off + csize > end:
            break
        if ctype == 0x0001:
            strings = _string_pool(b, off)
        elif ctype == 0x0180:
            resmap = list(struct.unpack_from(f'<{(csize - chsize) // 4}I', b, off + chsize))
        elif ctype == 0x0102:
            ext = off + chsize
            _ns, name, astart, asize, acount = struct.unpack_from('<IIHHH', b, ext)
            attrs = {}
            for i in range(acount):
                a = ext + astart + i * asize
                _ans, aname, raw, _vsize, _r0, dtype, data = struct.unpack_from('<IIIHBBI', b, a)
                key = ATTR_IDS.get(resmap[aname]) if aname < len(resmap) else None
                if key is None:
                    key = strings[aname] if aname < len(strings) else f'attr{aname}'
                attrs[key] = _value(dtype, data, None if raw == 0xffffffff else raw, strings)
            el = Element(strings[name] if name < len(strings) else '?', attrs)
            if stack:
                stack[-1].children.append(el)
            elif root is None:
                root = el
            stack.append(el)
        elif ctype == 0x0103:
            if stack:
                stack.pop()
        off += csize
    if root is None:
        raise ValueError('binary XML without elements')
    return root


# --------------------------------------------------------------------------------------------
# protobuf XML (an App Bundle's manifest: aapt2's Resources.proto XmlNode)
# --------------------------------------------------------------------------------------------

def _pb_fields(b):
    """[(field number, wire type, value)] of one protobuf message (values: int or bytes)."""
    out, p = [], 0

    def varint():
        nonlocal p
        v = s = 0
        while True:
            c = b[p]; p += 1
            v |= (c & 0x7f) << s
            s += 7
            if not c & 0x80:
                return v
    while p < len(b):
        key = varint()
        f, wt = key >> 3, key & 7
        if wt == 0:
            out.append((f, wt, varint()))
        elif wt == 1:
            out.append((f, wt, struct.unpack_from('<Q', b, p)[0])); p += 8
        elif wt == 2:
            n = varint()
            out.append((f, wt, bytes(b[p:p + n]))); p += n
        elif wt == 5:
            out.append((f, wt, struct.unpack_from('<I', b, p)[0])); p += 4
        else:
            raise ValueError(f'protobuf wire type {wt}')
    return out


def _pb_prim(b):
    for f, wt, v in _pb_fields(b):
        if f == 8:                                          # boolean_value
            return 'true' if v else 'false'
        if f == 6:                                          # int_decimal_value (int32)
            return str(v - (1 << 64) if v >= 1 << 63 else v)
        if f == 7:                                          # int_hexadecimal_value
            return '0x%x' % v
        if f == 3:                                          # float_value
            return repr(struct.unpack('<f', struct.pack('<I', v))[0])
    return None


def _pb_item(b):
    for f, wt, v in _pb_fields(b):
        if f == 7:                                          # prim
            return _pb_prim(v)
        if f == 1:                                          # ref: Reference.id (field 1)
            rid = next((x for g, _, x in _pb_fields(v) if g == 1), 0)
            return '@0x%08x' % rid
        if f in (2, 3):                                     # str / raw_str: String.value (field 1)
            return next((x.decode('utf-8', 'replace') for g, _, x in _pb_fields(v) if g == 1), '')
    return None


def parse_proto_xml(b):
    """The element tree of an App Bundle's AndroidManifest.xml (protobuf XmlNode)."""
    def element(eb):
        name, attrs, kids = '?', {}, []
        for f, wt, v in _pb_fields(eb):
            if f == 3:
                name = v.decode('utf-8', 'replace')
            elif f == 4:
                aname, aval, rid, item = '', None, 0, None
                for g, _, x in _pb_fields(v):
                    if g == 2:
                        aname = x.decode('utf-8', 'replace')
                    elif g == 3:
                        aval = x.decode('utf-8', 'replace')
                    elif g == 5:
                        rid = x
                    elif g == 6:
                        item = _pb_item(x)
                key = ATTR_IDS.get(rid) or aname
                attrs[key] = item if item is not None else aval
            elif f == 5:
                for g, _, x in _pb_fields(v):
                    if g == 1:
                        kids.append(element(x))
        el = Element(name, attrs)
        el.children = kids
        return el
    for f, wt, v in _pb_fields(b):
        if f == 1:
            return element(v)
    raise ValueError('protobuf XML without a root element')


def parse_manifest(b):
    return parse_axml(b) if b[:2] == b'\x03\x00' else parse_proto_xml(b)


class Manifest:
    """What the runtime needs from AndroidManifest.xml."""

    def __init__(self, root):
        self.root = root
        if root.tag != 'manifest':
            raise ValueError(f'manifest root is <{root.tag}>')
        self.package = root.get('package') or ''
        self.version_code = _int(root.get('versionCode')) or 0
        self.version_name = root.get('versionName') or ''
        self.split = root.get('split')                      # a split APK's name (config.arm64_v8a, ...)
        us = next(root.iter('uses-sdk'), None)
        self.min_sdk = _int(us.get('minSdkVersion')) if us is not None else None
        self.target_sdk = _int(us.get('targetSdkVersion')) if us is not None else None
        self.meta = {}
        app = next(root.iter('application'), None)
        if app is not None:
            for m in app.children:
                if m.tag == 'meta-data' and m.get('name'):
                    v = m.get('value')
                    self.meta[m.get('name')] = v if v is not None else m.get('resource')
        self.activities = [a.get('name') for a in root.iter('activity')]
        self.categories = [c.get('name') for c in root.iter('category') if c.get('name')]
        self.gles = None
        self.features = []
        for f in root.iter('uses-feature'):
            if f.get('glEsVersion'):
                self.gles = _int(f.get('glEsVersion'))
            if f.get('name'):
                self.features.append((f.get('name'), f.get('required') != 'false'))


def _int(v):
    try:
        return int(v, 0) if isinstance(v, str) else int(v)
    except (TypeError, ValueError):
        return None


# --------------------------------------------------------------------------------------------
# inputs: APKs, splits, OBBs, containers
# --------------------------------------------------------------------------------------------

class Apk:
    def __init__(self, path, label=None):
        self.path, self.label = path, label or os.path.basename(path)
        self.z = zipfile.ZipFile(path)
        self.names = self.z.namelist()
        mb = self.z.read('AndroidManifest.xml') if 'AndroidManifest.xml' in self.names else None
        if mb is None:
            raise Refused(f'{self.label}: no AndroidManifest.xml (not an APK)')
        self.manifest = Manifest(parse_manifest(mb))
        self.abis = sorted({n.split('/')[1] for n in self.names if n.startswith('lib/') and n.count('/') == 2 and n.endswith('.so')})

    def libs(self, abi):
        return [n for n in self.names if n.startswith(f'lib/{abi}/') and n.endswith('.so') and n.count('/') == 2]

    def has_unity_data(self):
        return any(re.match(r'assets/bin/Data/(globalgamemanagers|mainData|data\.unity3d)$', n) for n in self.names)


def _is_obb_name(n):
    return n.lower().endswith('.obb')


def unpack_inputs(paths, tmp, log):
    """Splits the command line's files into APKs and OBBs, opening containers (.xapk, .apks, .apkm,
    a zip of APKs) into tmp. An .aab is returned as ('aab', path)."""
    apks, obbs, aabs = [], [], []
    for p in paths:
        if not os.path.isfile(p):
            raise Refused(f'{p}: no such file')
        low = p.lower()
        if _is_obb_name(low):
            obbs.append(p)
            continue
        if not zipfile.is_zipfile(p):
            raise Refused(f'{p}: not a zip archive (APK, XAPK, APKS and AAB files are zips; an encrypted .apkm is not supported)')
        with zipfile.ZipFile(p) as z:
            names = z.namelist()
            if 'AndroidManifest.xml' in names:
                apks.append(Apk(p))
            elif 'base/manifest/AndroidManifest.xml' in names or 'BundleConfig.pb' in names:
                aabs.append(p)
            else:
                inner = [n for n in names if n.lower().endswith('.apk') and not n.endswith('/')]
                inner_obb = [n for n in names if _is_obb_name(n) and not n.endswith('/')]
                if any(n.startswith('splits/') for n in inner):
                    # bundletool's .apks: splits/ (base-master + config splits), asset-slices/ (asset
                    # packs); standalones/ repeat the whole app per ABI for pre-5.0 devices
                    inner = [n for n in inner if not n.startswith('standalones/')]
                if not inner:
                    raise Refused(f'{p}: a zip without an APK, AAB or AndroidManifest.xml')
                d = tempfile.mkdtemp(prefix='c', dir=tmp)
                log(f'{os.path.basename(p)}: {len(inner)} APK(s), {len(inner_obb)} OBB(s) inside')
                for n in inner + inner_obb:
                    safe = os.path.normpath(n)
                    if safe.startswith(('..', '/')) or os.path.isabs(safe):
                        raise Refused(f'{p}: unsafe path in archive: {n}')
                    dst = os.path.join(d, safe.replace(os.sep, '__'))
                    with z.open(n) as fi, open(dst, 'wb') as fo:
                        shutil.copyfileobj(fi, fo, 1 << 20)
                    if _is_obb_name(n):
                        obbs.append(dst)
                    else:
                        apks.append(Apk(dst, f'{os.path.basename(p)}:{n}'))
    return apks, obbs, aabs


def _split_kind(apk):
    s = apk.manifest.split
    if not s:
        return 'base'
    if s.startswith('config.'):
        return 'config'
    return 'pack'                                           # an asset pack or a feature module


def pick_base(apks):
    base = [a for a in apks if _split_kind(a) == 'base']
    if not base:
        raise Refused('no base APK among the inputs (only splits: ' + ', '.join(a.manifest.split or a.label for a in apks) + ')')
    if len(base) > 1:
        pk = {a.manifest.package for a in base}
        raise Refused(f'more than one base APK given ({", ".join(a.label for a in base)}; packages {", ".join(sorted(pk))})')
    other = [a for a in apks if a is not base[0]]
    for a in other:
        if a.manifest.package and a.manifest.package != base[0].manifest.package:
            raise Refused(f'{a.label} belongs to {a.manifest.package}, not {base[0].manifest.package}')
    return base[0], other


# --------------------------------------------------------------------------------------------
# App Bundle -> one APK
# --------------------------------------------------------------------------------------------

def aab_to_apk(path, out_apk, log):
    """An .aab rebuilt as one APK for this runtime: the base module's assets/, root/ and lib/ and
    the install-time asset packs' assets/ (a phone installs them as splits; Unity's data asset pack
    UnityDataAssetPack among them), the manifest (protobuf, read by this tool only). Entries keep
    their compression; stored ones are 4-byte aligned as zipalign does."""
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        modules = sorted({n.split('/')[0] for n in names if n.endswith('/manifest/AndroidManifest.xml')})
        if 'base' not in modules:
            raise Refused(f'{path}: an App Bundle without a base module')
        entries, skipped = [], []
        for mod in modules:
            mf = Manifest(parse_proto_xml(z.read(f'{mod}/manifest/AndroidManifest.xml')))
            delivery = 'install-time'
            for el in mf.root.iter():
                if el.tag.endswith('delivery'):
                    kids = [c.tag.split(':')[-1] for c in el.children]
                    delivery = next((k for k in ('install-time', 'fast-follow', 'on-demand') if k in kids), delivery)
            if mod != 'base' and delivery != 'install-time':
                skipped.append(f'{mod} ({delivery})')
            for n in names:
                if not n.startswith(mod + '/') or n.endswith('/'):
                    continue
                rel = n[len(mod) + 1:]
                if rel.startswith('assets/') or (mod == 'base' and rel.startswith(('lib/', 'root/'))):
                    arc = rel[len('root/'):] if rel.startswith('root/') else rel
                    entries.append((arc, n))
                elif mod == 'base' and rel == 'manifest/AndroidManifest.xml':
                    entries.append(('AndroidManifest.xml', n))
        if skipped:
            log(f'aab: asset packs delivered later by the store, included as well: {", ".join(skipped)}')
        copy_entries(z, entries, out_apk)
    log(f'aab: {len(entries)} entries from {len(modules)} module(s) -> {out_apk}')


def copy_entries(z, entries, out_path):
    """Writes [(new name, name in z)] to a new zip, keeping each entry's compressed bytes."""
    tmp = out_path + '.tmp'
    with zipfile.ZipFile(tmp, 'w', allowZip64=True) as zo:
        for arc, n in entries:
            zi = z.getinfo(n)
            ni = zipfile.ZipInfo(arc, date_time=zi.date_time)
            ni.compress_type = zi.compress_type
            ni.external_attr = 0o644 << 16
            if zi.compress_type == zipfile.ZIP_STORED:
                off = zo.fp.tell() + 30 + len(arc.encode())
                pad = (-(off + 4)) % 4
                ni.extra = struct.pack('<HH', 0xD935, pad) + b'\0' * pad
            with z.open(n) as fi, zo.open(ni, 'w') as fo:
                shutil.copyfileobj(fi, fo, 1 << 20)
    os.replace(tmp, out_path)


def merge_apks(base, packs, out_apk, log):
    """base.apk plus the assets/ of asset-pack splits, as one APK (Unity reads its data through the
    package code path)."""
    seen = set(base.names)
    srcs = [(base.z, [(n, n) for n in base.names])]
    for p in packs:
        add = [(n, n) for n in p.names if n.startswith('assets/') and not n.endswith('/') and n not in seen]
        seen.update(n for n, _ in add)
        srcs.append((p.z, add))
        log(f'merge: {len(add)} assets of split {p.manifest.split}')
    tmp = out_apk + '.parts'
    os.makedirs(tmp, exist_ok=True)
    try:
        parts = []
        for i, (z, ents) in enumerate(srcs):
            pp = os.path.join(tmp, f'{i}.zip')
            copy_entries(z, ents, pp)
            parts.append(pp)
        with zipfile.ZipFile(out_apk + '.tmp', 'w', allowZip64=True) as zo:
            for pp in parts:
                with zipfile.ZipFile(pp) as zi:
                    for info in zi.infolist():
                        ni = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                        ni.compress_type = info.compress_type
                        ni.external_attr = info.external_attr
                        if info.compress_type == zipfile.ZIP_STORED:
                            off = zo.fp.tell() + 30 + len(info.filename.encode())
                            pad = (-(off + 4)) % 4
                            ni.extra = struct.pack('<HH', 0xD935, pad) + b'\0' * pad
                        with zi.open(info) as fi, zo.open(ni, 'w') as fo:
                            shutil.copyfileobj(fi, fo, 1 << 20)
        os.replace(out_apk + '.tmp', out_apk)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# --------------------------------------------------------------------------------------------
# what the build is
# --------------------------------------------------------------------------------------------

def version_of_head(head):
    """The Unity version in the first bytes of a SerializedFile or UnityFS bundle."""
    if head.startswith(b'UnityFS\0') or head.startswith((b'UnityWeb\0', b'UnityRaw\0')):
        parts = head[8 + 4:].split(b'\0')
        return UV.Version(parts[1].decode('ascii'))
    return UV.Version(UV.sf_header(head)[1])


def unity_data(zips):
    """(zipfile, entry) of the first Unity data file found in the archives (APK first, then OBBs)."""
    for z in zips:
        for n in ('assets/bin/Data/globalgamemanagers', 'assets/bin/Data/data.unity3d', 'assets/bin/Data/mainData'):
            if n in z.namelist():
                return z, n
    return None, None


def player_settings(z, n, log):
    """PlayerSettings / BuildSettings fields the runtime cares about (UnityPy; {} without it)."""
    try:
        import UnityPy
    except ImportError:
        log('note: UnityPy is not installed: graphics APIs and frame pacing not checked')
        return {}
    out = {}
    try:
        env = UnityPy.load(z.read(n))
        for o in env.objects:
            if o.type.name not in ('PlayerSettings', 'BuildSettings'):
                continue
            t = o.read_typetree()
            if o.type.name == 'BuildSettings':
                out['graphics_apis'] = list(t.get('m_GraphicsAPIs') or [])
                out['build_version'] = t.get('m_Version')
            else:
                for k in ('productName', 'companyName', 'androidUseSwappy', 'm_MTRendering', 'defaultScreenOrientation'):
                    if k in t:
                        out[k] = t[k]
    except Exception as ex:                                 # noqa: BLE001 - a report line, not fatal
        log(f'note: could not read PlayerSettings ({type(ex).__name__}: {ex})')
    return out


def il2cpp_metadata(z):
    """(present, version or None, note) of assets/bin/Data/Managed/Metadata/global-metadata.dat."""
    n = 'assets/bin/Data/Managed/Metadata/global-metadata.dat'
    if n not in z.namelist():
        return False, None, None
    with z.open(n) as f:
        head = f.read(8)
    if len(head) == 8:
        magic, ver = struct.unpack('<II', head)
        if magic == IL2CPP_MAGIC:
            return True, ver, None
    return True, None, 'not in the usual format (encrypted or obfuscated: the game\'s own libil2cpp reads it)'


def choose_abi(have, target, abi, log):
    """(ABI, why) for the build's ARM ABIs `have`: --abi, else the first userland of `target` the
    build has a player for (target: auto, arm64, armhf, or a preference list "armhf,arm64")."""
    arm = [a for a in ABIS if a in have]
    if abi:
        if abi not in have:
            raise Refused(f'--abi {abi}: the build has {", ".join(have) or "no native libraries"}')
        return abi, f'--abi {abi}'
    if not arm:
        if any(a in OTHER_ABIS for a in have):
            raise Refused(f'the build has native libraries only for {", ".join(have)}: the runtime runs arm64-v8a or armeabi-v7a')
        raise Refused('the build has no native libraries (lib/<abi>/): not a Unity Android build, or its libraries '
                      'are in a split APK (config.arm64_v8a / config.armeabi_v7a) that was not given')
    targets = [t.strip() for t in (target or 'auto').split(',') if t.strip()] or ['auto']
    bad = [t for t in targets if t not in TARGETS and t != 'auto']
    if bad or ('auto' in targets and len(targets) > 1):
        raise Refused(f'--target {target}: auto, arm64, armhf, or a list of arm64 and armhf (the first the build has)')
    if targets == ['auto']:
        m = platform.machine().lower()
        if sys.platform.startswith('linux') and m in ('aarch64', 'arm64', 'armv8l', 'armv7l', 'armhf', 'armv7', 'arm'):
            targets = ['arm64' if m in ('aarch64', 'arm64') and struct.calcsize('P') == 8 else 'armhf']
            why = f'this machine ({m}, {8 * struct.calcsize("P")}-bit userland)'
        else:
            pick = arm[0]
            return pick, (f'{pick} (the build has {", ".join(arm)}; --target armhf or --abi picks the 32-bit one)'
                          if len(arm) > 1 else f'{pick}, the only ARM ABI of the build')
    else:
        why = f'--target {",".join(targets)}'
    for t in targets:
        want = TARGETS[t]
        if want in have:
            return want, f'{want} for {why}' + (f' (the build has {", ".join(arm)})' if len(targets) > 1 else '')
    if len(targets) > 1:
        raise Refused(f'the build has {", ".join(arm)}, a player for none of {why}')
    if targets == ['arm64']:
        raise Refused(f'the build has only armeabi-v7a (Unity: Mono builds, and IL2CPP builds without ARM64), which needs a '
                      f'32-bit (armhf) userland; {why} is 64-bit. On a firmware with a 32-bit userland use --target armhf')
    raise Refused(f'the build has only arm64-v8a, which needs a 64-bit userland; {why} is 32-bit (--target arm64 for a '
                  '64-bit firmware)')


def classify_obb(path, package, code):
    """('main'|'patch', name the player looks for) of an OBB file name: Google Play's
    main.<code>.<package>.obb / patch...., Unity's split build output <product>.main.obb."""
    b = os.path.basename(path)
    m = re.match(r'^(main|patch)\.(\d+)\.(.+)\.obb$', b)
    if m:
        kind = m.group(1)
        if m.group(3) != package:
            raise Refused(f'{b}: an OBB of {m.group(3)}, not of {package}')
        return kind, b, int(m.group(2))
    kind = 'patch' if re.search(r'[._-]patch\.obb$', b, re.I) else 'main'
    return kind, f'{kind}.{code}.{package}.obb', None


def obb_build_id(path):
    """The unity.build-id an OBB carries (an empty entry assets/<guid>), and its unity_obb_guid."""
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            ids = [n[len('assets/'):] for n in names
                   if re.match(r'^assets/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$', n)]
            guid = z.read('unity_obb_guid').decode('ascii', 'replace').strip() if 'unity_obb_guid' in names else None
            return ids, guid, z
    except zipfile.BadZipFile:
        raise Refused(f'{os.path.basename(path)}: not a zip (Unity OBBs are zips)')


def place(src, dst, log, move=False):
    """dst = src: moved when src is this tool's own (unpacked from a container, rebuilt), else a clone,
    a hard link or a copy (the input stays as it was)."""
    how = None
    if move:
        try:
            os.replace(src, dst)
            how = 'moved'
        except OSError:
            pass
    if how is None:
        from .android_data import clone
        how = clone(src, dst, allow_hardlink=True)
    log(f'  {os.path.basename(dst)} ({how}, {os.path.getsize(dst) / 2**20:.1f} MiB)')


def _inside(path, d):
    return os.path.commonpath([os.path.abspath(path), os.path.abspath(d)]) == os.path.abspath(d)


# --------------------------------------------------------------------------------------------

def run(inputs, out, target='auto', abi=None, any_version=False, force=False, dry_run=False, log=print,
        managed_to=None):
    """Imports the build into out (or with dry_run only reads it) and returns what it is: the
    android_package.json fields, and for a dry run the report (`abis`, `graphics_apis`, `obbs`, ...).
    managed_to: a folder that gets a Mono build's assemblies (assets/bin/Data/Managed/*.dll), for
    `pmunity port check`'s code survey."""
    if not dry_run:
        os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='.apk-', dir=None if dry_run else out)
    try:
        return _run(inputs, out, tmp, target, abi, any_version, force, dry_run, log, managed_to)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _run(inputs, out, tmp, target, abi, any_version, force, dry_run, log, managed_to=None):
    apks, obbs, aabs = unpack_inputs(inputs, tmp, log)
    if aabs:
        if len(aabs) > 1 or apks:
            raise Refused('give one App Bundle (.aab), without APKs')
        built = os.path.join(tmp, 'from-aab.apk')
        aab_to_apk(aabs[0], built, log)
        apks = [Apk(built, os.path.basename(aabs[0]))]
    if not apks:
        raise Refused('no APK among the inputs')
    base, others = pick_base(apks)
    mf = base.manifest
    configs = [a for a in others if _split_kind(a) == 'config']
    packs = [a for a in others if _split_kind(a) == 'pack']
    # OBBs beside a single APK: Unity's "Split Application Binary" writes <name>.main.obb next to it
    if not obbs and len(inputs) == 1 and inputs[0].lower().endswith('.apk'):
        d, stem = os.path.dirname(os.path.abspath(inputs[0])), os.path.splitext(os.path.basename(inputs[0]))[0]
        for n in sorted(os.listdir(d)):
            if _is_obb_name(n) and (n.startswith(stem + '.') or n.endswith('.' + mf.package + '.obb')):
                obbs.append(os.path.join(d, n))
    code = mf.version_code
    log(f'package {mf.package}, versionCode {code}, versionName {mf.version_name!r}, '
        f'minSdk {mf.min_sdk}, targetSdk {mf.target_sdk}')
    if not mf.package:
        raise Refused('the manifest names no package')

    # native libraries: the base APK and its ABI splits
    have = sorted(set(base.abis).union(*[a.abis for a in configs]) if configs else base.abis)
    pick, why = choose_abi(have, target, abi, log)
    lib_src = [(a, n) for a in [base] + configs for n in a.libs(pick)]
    libnames = sorted({os.path.basename(n) for _, n in lib_src})
    missing = [n for n in ('libmain.so', 'libunity.so') if n not in libnames]
    if missing:
        raise Refused(f'lib/{pick} has no {" or ".join(missing)}: not a Unity Android build '
                      f'(libraries: {", ".join(libnames) or "none"})')
    if 'libil2cpp.so' in libnames:
        backend = 'il2cpp'
    elif any(n in libnames for n in ('libmonobdwgc-2.0.so', 'libmono.so', 'libmonosgen-2.0.so')):
        backend = 'mono'
    else:
        raise Refused(f'lib/{pick} has neither libil2cpp.so nor a Mono runtime (libmono*.so)')
    log(f'ABI {why}; scripting backend {backend}')

    # OBBs
    obb_plan = {}
    for p in obbs:
        kind, name, ocode = classify_obb(p, mf.package, code)
        if ocode is not None and ocode != code:
            log(f'note: {os.path.basename(p)} names versionCode {ocode}; the APK has {code}: placed as {kind}.{code}.{mf.package}.obb')
            name = f'{kind}.{code}.{mf.package}.obb'
        if kind in obb_plan:
            raise Refused(f'two {kind} OBBs: {os.path.basename(obb_plan[kind][0])} and {os.path.basename(p)}')
        obb_plan[kind] = (p, name)
    bid = mf.meta.get('unity.build-id')
    obb_zips = []
    for kind, (p, name) in sorted(obb_plan.items()):
        ids, guid, _ = obb_build_id(p)
        obb_zips.append(zipfile.ZipFile(p))
        if bid and ids and bid not in ids:
            msg = f'{os.path.basename(p)} belongs to another build (its build id {ids[0]}, the APK\'s {bid}): the player would not mount it'
            if not force:
                raise Refused(msg + ' (--force places it anyway)')
            log('warning: ' + msg)
        log(f'OBB {kind}: {os.path.basename(p)} -> {name}' + (f' (build id {ids[0]})' if ids else ''))

    # Unity data and version
    data_zips = [base.z] + [a.z for a in packs] + obb_zips
    z, n = unity_data(data_zips)
    if z is None:
        if 'com.unity3d.player.UnityPlayerActivity' not in mf.activities and not any('unity3d' in (a or '') for a in mf.activities):
            raise Refused('no Unity data (assets/bin/Data) and no Unity activity: not a Unity game')
        raise Refused('the APK has no Unity data (assets/bin/Data/globalgamemanagers or data.unity3d): it is in an OBB or a '
                      'Play asset pack that was not given (store downloads); give the .obb (or the .xapk / .apks / .aab) too')
    with z.open(n) as f:
        head = f.read(512)
    try:
        ver = version_of_head(head)
    except ValueError as ex:
        raise Refused(f'{n}: cannot read the Unity version ({ex})')
    log(f'Unity {ver} ({n.rsplit("/", 1)[1]}{" in an OBB" if z in obb_zips else ""})')
    try:
        UV.check(ver, any_version)
    except UV.Unsupported:
        lo, hi = '%d.%d' % UV.FIRST, '%d.%d' % UV.LAST
        raise Refused(f'Unity {ver}: the runtime runs players of Unity {lo} - {hi} (docs/compat/README.md); '
                      '--any-unity-version tries anyway')
    if backend == 'mono' and pick == 'arm64-v8a':
        raise Refused('a Mono build with arm64-v8a libraries: Unity never shipped one; the APK looks modified')
    ps = player_settings(z, n, log)
    apis = ps.get('graphics_apis') or []
    if apis and not any(a in (8, 11) for a in apis):
        raise Refused(f'graphics APIs {", ".join(GFX.get(a, str(a)) for a in apis)}: the build has no OpenGL ES; '
                      'the runtime draws with GLES 2/3 only')
    notes = []
    if apis:
        names = [GFX.get(a, str(a)) for a in apis]
        log(f'graphics APIs {", ".join(names)}' + (' (Vulkan is not offered: the player takes GLES)' if 21 in apis else ''))
    if ps.get('androidUseSwappy') and ver.line >= (2019, 3):
        notes.append('Optimized Frame Pacing (Swappy) is on in this build')
    meta_ver = None
    if backend == 'il2cpp':
        mz = next((x for x in data_zips if 'assets/bin/Data/Managed/Metadata/global-metadata.dat' in x.namelist()), None)
        if mz is None:
            raise Refused('IL2CPP build without assets/bin/Data/Managed/Metadata/global-metadata.dat (the metadata '
                          'libil2cpp loads at startup): moved or stripped by a protector, not runnable as is')
        _, meta_ver, note = il2cpp_metadata(mz)
        log(f'IL2CPP metadata version {meta_ver}' if meta_ver else f'IL2CPP metadata {note}')
    plugins = [x for x in libnames if x not in ('libmain.so', 'libunity.so', 'libil2cpp.so', 'libmonobdwgc-2.0.so',
                                                'libmono.so', 'libMonoPosixHelper.so', 'libmono-native.so', 'libmonosgen-2.0.so')]
    if plugins:
        log(f'native plugins: {", ".join(plugins)}')
    vr = sorted(set(plugins) & VR_LIBS) + [c for c in mf.categories if c in VR_CATEGORIES] + \
        [f for f, req in mf.features if req and f.startswith('android.hardware.vr.')]
    if vr:
        msg = f'a VR build (headset only: {", ".join(vr)})'
        if not force:
            raise Refused(msg + '; --force imports it anyway')
        notes.append(msg)
    if mf.gles and mf.gles > 0x30002:
        notes.append(f'the manifest asks for OpenGL ES {mf.gles >> 16}.{mf.gles & 0xffff}')
    for nm in notes:
        log('note: ' + nm)
    if managed_to and backend == 'mono':
        os.makedirs(managed_to, exist_ok=True)
        for x in data_zips:
            for nm in x.namelist():
                if re.match(r'^assets/bin/Data/Managed/[^/]+\.dll$', nm):
                    with x.open(nm) as fi, open(os.path.join(managed_to, nm.rsplit('/', 1)[1]), 'wb') as fo:
                        shutil.copyfileobj(fi, fo, 1 << 20)
    if dry_run:
        return {'package': mf.package, 'version_code': code, 'version_name': mf.version_name, 'min_sdk': mf.min_sdk,
                'target_sdk': mf.target_sdk, 'abi': pick, 'abi_why': why, 'abis': [x for x in ABIS if x in have],
                'all_abis': have, 'backend': backend, 'unity_version': str(ver),
                'data_in': 'obb' if z in obb_zips else 'asset pack' if z is not base.z else 'apk',
                'obbs': {kind: name for kind, (_, name) in sorted(obb_plan.items())},
                'graphics_apis': [GFX.get(x, str(x)) for x in apis], 'swappy': bool(ps.get('androidUseSwappy')),
                'product_name': ps.get('productName'), 'company_name': ps.get('companyName'),
                'orientation': ORIENTATIONS.get(ps.get('defaultScreenOrientation')),
                'il2cpp_metadata': meta_ver, 'plugins': plugins, 'notes': notes, 'meta_data': mf.meta,
                'size_mb': int(sum(os.path.getsize(p) for p in set(inputs) | {p for p, _ in obb_plan.values()
                                                                             if not _inside(p, tmp)}) / 2**20 + 0.5)}

    # write the layout
    log(f'-> {out}')
    apk_out = os.path.join(out, 'base.apk')
    data_packs = [a for a in packs if any(x.startswith('assets/') for x in a.names)]
    if data_packs:
        merge_apks(base, data_packs, apk_out, log)
        log(f'  base.apk (merged with {len(data_packs)} asset pack split(s), {os.path.getsize(apk_out) / 2**20:.1f} MiB)')
    else:
        place(base.path, apk_out, log, move=_inside(base.path, tmp))
    libdir = os.path.join(out, 'lib')
    if os.path.isdir(libdir):
        for x in os.listdir(libdir):
            if x.endswith('.so'):
                os.unlink(os.path.join(libdir, x))
    os.makedirs(libdir, exist_ok=True)
    for a, nm in lib_src:
        with a.z.open(nm) as fi, open(os.path.join(libdir, os.path.basename(nm)), 'wb') as fo:
            shutil.copyfileobj(fi, fo, 1 << 20)
    log(f'  lib/: {len(libnames)} libraries from lib/{pick}')
    for x in os.listdir(out):
        if re.match(r'^(main|patch)\.\d+\..+\.obb$', x):
            os.unlink(os.path.join(out, x))
    res = {'package': mf.package, 'version_code': code, 'version_name': mf.version_name, 'apk': 'base.apk'}
    for kind, (p, name) in sorted(obb_plan.items()):
        place(p, os.path.join(out, name), log, move=_inside(p, tmp))
        res[f'{kind}_obb'] = name
    if backend == 'mono':
        etc = [nm for nm in base.names if nm.startswith('assets/bin/Data/Managed/etc/') and not nm.endswith('/')]
        for nm in etc:
            dst = os.path.join(out, *nm.split('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with base.z.open(nm) as fi, open(dst, 'wb') as fo:
                shutil.copyfileobj(fi, fo)
        if etc:
            log(f'  assets/bin/Data/Managed/etc/: {len(etc)} files (Mono config)')
    if bid:
        res['build_id'] = bid
    res.update({'meta_data': mf.meta, 'min_sdk': mf.min_sdk, 'target_sdk': mf.target_sdk, 'abi': pick,
                'backend': backend, 'unity_version': str(ver), 'source': 'apk'})
    if meta_ver:
        res['il2cpp_metadata'] = meta_ver
    if plugins:
        res['plugins'] = plugins
    if ps.get('productName'):
        res['product_name'] = ps['productName']
    if notes:
        res['notes'] = notes
    with open(os.path.join(out, 'android_package.json'), 'w') as f:
        json.dump(res, f, indent=1)
    log(f'  android_package.json; run: ua_host ({"aarch64" if pick == "arm64-v8a" else "armhf"} build) {out}')
    return res


def port_new(ctx):
    """`pmunity port new` hook: port.json for the apk route (no download, no conversion; the 32-bit
    userland only when the port runs nothing but armeabi-v7a)."""
    p = ctx.cfg['port']
    if p['route'] != 'apk':
        return
    pj_path = os.path.join(ctx.out, p['name'], 'port.json')
    if not os.path.isfile(pj_path):
        return
    with open(pj_path) as f:
        pj = json.load(f)
    a = pj['attr']
    abis = ctx.cfg['player'].get('abis') or list(ABIS)
    a['reqs'] = ['armhf'] if abis == ['armeabi-v7a'] else [r for r in a.get('reqs') or [] if r != 'armhf']
    if not p.get('desc'):
        a['desc'] = f"{p['title']}: the game's own Android build, run on the handheld by ua_host."
    if not p.get('inst'):
        a['inst'] = (f"Requires {ctx.cfg['game']['what']}: put it in ports/{p['name']}/gamedata. The first launch "
                     f"installs it ({p['setup_time']}; nothing is downloaded).")
    ctx.write(f"{p['name']}/port.json", json.dumps(pj, indent=4) + '\n')


def main(argv):
    ap = argparse.ArgumentParser(prog='pmunity apk', description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__.split('\n\n', 1)[1])
    ap.add_argument('files', nargs='+', help='APK / split APKs / XAPK / APKS / AAB / OBB files, then the output dir')
    ap.add_argument('--target', default='auto',
                    help='the handheld userland: arm64 (64-bit) -> arm64-v8a, armhf (32-bit) -> armeabi-v7a, auto; '
                         'or a list in order of preference, the first the build has a player for (armhf,arm64)')
    ap.add_argument('--abi', choices=list(ABIS), help='take this ABI\'s libraries')
    ap.add_argument('--any-unity-version', action='store_true', help='also Unity versions outside the runtime\'s range')
    ap.add_argument('--force', action='store_true', help='place OBBs of another build too')
    ap.add_argument('--dry-run', action='store_true', help='report only')
    ap.add_argument('--json', action='store_true', help='what the build is, as JSON on stdout (the log goes to stderr)')
    a = ap.parse_args(argv)
    if len(a.files) < 2:
        ap.error('give the input file(s) and the output directory')
    *inputs, out = a.files
    log = (lambda m: print(m, file=sys.stderr, flush=True)) if a.json else (lambda m: print(m, flush=True))
    try:
        res = run(inputs, out, a.target, a.abi, a.any_unity_version, a.force, a.dry_run, log=log)
    except Refused as ex:
        sys.exit(f'pmunity apk: refused: {ex}')
    if a.json:
        print(json.dumps(res, indent=1, default=str))
