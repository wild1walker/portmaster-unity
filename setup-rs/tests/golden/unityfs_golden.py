"""The Python side of unityfs's golden test: what pmunity (UnityPy 1.25.4) reads and writes for each
Unity file of a data folder, one JSON line a file. The Rust side (crates/unityfs/examples/golden.rs)
writes the same lines; unityfs.sh runs both and compares them.

    python3 unityfs_golden.py <data dir> <scratch dir> [--only TEXT] [--no-write] [--no-patch] > py.jsonl

Per file:
  objects    [[path id, class id, class name, read, rewrite]]: read is the SHA-256 (16 hex) of the
             object's value tree in a canonical form (unityfs_canon: numbers by their bits, strings
             and bytes as bytes) or "ERR <message>"; rewrite the SHA-256 of the bytes UnityPy's
             write_typetree gives for it, "=" when they are the object's own bytes
  classes    {class name: objects}
  save       SHA-256 of the file written back unchanged (unity_io.Loaded.write_to; a plain file:
             save_serialized_to, the bytes sf.save() gives)
  edit       the same after one edit: the first object (table order) that reads and has a string
             m_Name gets "~" added to it (save_typetree)
  inline     a bundle written with inline=True (android_data's texture inlining)
  blocks     a bundle's blocks: SHA-256 of every block decompressed, of every block compressed again
             with UnityPy's compress_lz4 (LZ4 HC 9) and with lz4's fast mode
  patch      a bundle copy after android_data.patch_bundle_platform(copy, 13): its SHA-256 and the
             methods used; a plain file: patch_sf_platform
  rss_mb     the process's peak RSS so far
"""
import argparse
import hashlib
import json
import os
import re
import resource
import shutil
import struct
import sys
import traceback

NUM = {'SInt8': 'b', 'UInt8': 'B', 'char': 'B', 'short': 'h', 'SInt16': 'h', 'unsigned short': 'H', 'UInt16': 'H',
       'int': 'i', 'SInt32': 'i', 'unsigned int': 'I', 'UInt32': 'I', 'Type*': 'I', 'long long': 'q', 'SInt64': 'q',
       'unsigned long long': 'Q', 'UInt64': 'Q', 'FileSize': 'Q', 'float': 'f', 'double': 'd', 'bool': '?'}


def canon(node, v, h):
    """Feeds the canonical form of a value read with node to the hash h (the same bytes as
    golden.rs's canon)."""
    t = node.m_Type
    code = NUM.get(t)
    if code:
        if code == 'f':
            h.update(b'f' + struct.pack('<f', v))
        elif code == 'd':
            h.update(b'd' + struct.pack('<d', v))
        elif code == '?':
            h.update(b'b1' if v else b'b0')
        else:
            h.update(b'i%d;' % v)
    elif t == 'string':
        b = v.encode('utf8', 'surrogateescape')
        h.update(b's%d:' % len(b) + b)
    elif t == 'TypelessData':
        h.update(b'y%d:' % len(v) + bytes(v))
    elif t == 'pair':
        h.update(b'(')
        canon(node.m_Children[0], v[0], h)
        canon(node.m_Children[1], v[1], h)
        h.update(b')')
    elif node.m_Children and node.m_Children[0].m_Type == 'Array':
        sub = node.m_Children[0].m_Children[1]
        if NUM.get(sub.m_Type) == 'B':
            h.update(b'y%d:' % len(v) + bytes(v))
        else:
            h.update(b'[%d:' % len(v))
            for x in v:
                canon(sub, x, h)
            h.update(b']')
    else:
        kids = {}
        for c in node.m_Children:
            kids[c.m_Name] = c
        h.update(b'{')
        for k, x in v.items():
            h.update(k.encode('utf8', 'surrogateescape') + b'=')
            c = kids[k]
            if c.m_Type == 'ReferencedObjectData':
                raise ValueError('ReferencedObject: not in the canonical form')
            canon(c, x, h)
            h.update(b',')
        h.update(b'}')


def sha(b):
    return hashlib.sha256(b).hexdigest()


def sha_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


class Job:
    """What Loaded.write_to needs of a convert job: the output written to one path."""

    def __init__(self, out):
        self.out = out

    def write(self, rel, data):
        with open(self.out, 'wb') as f:
            f.write(data)

    def write_file(self, rel, path):
        os.replace(path, self.out)


def is_unityfs(p):
    with open(p, 'rb') as f:
        return f.read(8) == b'UnityFS\0'


def objects_of(path, spill):
    """[[path id, class id, class name, read, rewrite]] and {class: n} of a file's serialized file."""
    from pmunity.unity_io import Loaded
    from UnityPy.helpers import TypeTreeHelper as TT
    from UnityPy.streams import EndianBinaryWriter
    lf = Loaded(path, spill_dir=spill, spill_mb=64)
    out, classes = [], {}
    try:
        for pid, o in lf.objects():
            name = o.type.name
            classes[name] = classes.get(name, 0) + 1
            rec = [pid, o.class_id, name, None, None]
            try:
                t = o.read_typetree()
                node = o._get_typetree_node()
                h = hashlib.sha256()
                canon(node, t, h)
                rec[3] = h.hexdigest()[:16]
                w = EndianBinaryWriter(endian=o.reader.endian)
                TT.write_typetree(t, node, w, o.assets_file)
                b = w.bytes
                rec[4] = '=' if b == bytes(o.get_raw_data()) else sha(b)[:16]
            except Exception as ex:  # noqa: BLE001 - recorded: the Rust side must fail on it too
                # (a type tree that reads too few bytes: the counts, which the Rust side gives too)
                rec[3] = 'ERR ' + (str(ex) if str(ex).startswith('Expected to read') else type(ex).__name__)
            out.append(rec)
    finally:
        lf.close()
    return out, classes


def edit_one(lf):
    """The test edit: the first object that reads and has a string m_Name gets '~' added."""
    for pid, o in lf.objects():
        try:
            t = o.read_typetree()
        except Exception:  # noqa: BLE001
            continue
        if isinstance(t.get('m_Name'), str):
            t['m_Name'] += '~'
            o.save_typetree(t)
            return pid
    return None


def write(path, out, spill, edit=False, inline=False):
    from pmunity.unity_io import Loaded, save_serialized_to
    lf = Loaded(path, spill_dir=spill, spill_mb=64)
    try:
        if edit:
            edit_one(lf)
        if lf.is_bundle:
            lf.write_to(Job(out), 'x', inline=inline)
        elif not save_serialized_to(lf.sf, out):
            with open(out, 'wb') as f:
                f.write(lf.save())
    finally:
        lf.close()
    h = sha_file(out)
    os.remove(out)
    return h


def blocks(path):
    """SHA-256 of a bundle's blocks: decompressed, then each compressed with LZ4 HC 9 and with
    LZ4's fast mode."""
    import lz4.block
    from pmunity import bigbundle
    hd = bigbundle.header(path)
    if hd is None:
        return None
    start = hd[7]
    d, z, fz = hashlib.sha256(), hashlib.sha256(), hashlib.sha256()
    kinds = {}
    with open(path, 'rb') as f:
        f.seek(start)
        for usize, csize, flags in hd[5]:
            kinds[flags & 0x3F] = kinds.get(flags & 0x3F, 0) + 1
            plain = b''.join(bigbundle._pieces(f, csize, usize, flags))
            d.update(plain)
            z.update(lz4.block.compress(plain, mode='high_compression', compression=9, store_size=False))
            fz.update(lz4.block.compress(plain, store_size=False))
    return {'plain': d.hexdigest(), 'hc9': z.hexdigest(), 'fast': fz.hexdigest(),
            'kinds': {str(k): v for k, v in sorted(kinds.items())}}


def patch(path, scratch):
    from pmunity import android_data as AD
    cp = os.path.join(scratch, 'patch.copy')
    with open(path, 'rb') as fi, open(cp, 'wb') as fo:     # (no sendfile: it crawls on a shared folder)
        while True:
            b = fi.read(1 << 20)
            if not b:
                break
            fo.write(b)
    try:
        if is_unityfs(path):
            res = [list(r) for r in AD.patch_bundle_platform(cp, AD.ANDROID)]
        else:
            res = [AD.patch_sf_platform(cp, AD.ANDROID)]
        return {'sha': sha_file(cp), 'res': res}
    finally:
        os.remove(cp)


def unity_files(data):
    """The Unity files of a data folder: UnityFS bundles, and the files UnityPy opens as serialized
    files (check_file_type, not a stream by name), sorted."""
    from UnityPy.helpers.ImportHelper import check_file_type
    from UnityPy.enums import FileType
    from UnityPy.streams import EndianBinaryReader
    out = []
    for r, ds, fs in os.walk(data):
        ds.sort()
        for n in sorted(fs):
            p = os.path.join(r, n)
            if os.path.islink(p) or n.endswith(('.resS', '.resource', '.config', '.xml', '.dat')):
                continue
            with open(p, 'rb') as f:
                if f.read(8) == b'UnityFS\0':
                    out.append(p)
                    continue
                f.seek(0)
                if check_file_type(EndianBinaryReader(f))[0] == FileType.AssetsFile:
                    out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data')
    ap.add_argument('scratch')
    ap.add_argument('--only')
    ap.add_argument('--no-write', action='store_true')
    ap.add_argument('--no-patch', action='store_true')
    ap.add_argument('--no-objects', action='store_true')
    a = ap.parse_args()
    from pmunity.unity_io import map_big_blocks
    map_big_blocks()
    os.makedirs(a.scratch, exist_ok=True)
    spill = os.path.join(a.scratch, 'spill')
    for p in unity_files(a.data):
        rel = os.path.relpath(p, a.data)
        if a.only and a.only not in rel:
            continue
        rec = {'file': rel, 'kind': 'bundle' if is_unityfs(p) else 'serialized'}
        try:
            if not a.no_objects:
                rec['objects'], rec['classes'] = objects_of(p, spill)
            if not a.no_write:
                out = os.path.join(a.scratch, 'out.bin')
                rec['save'] = write(p, out, spill)
                rec['edit'] = write(p, out, spill, edit=True)
                if rec['kind'] == 'bundle':
                    rec['inline'] = write(p, out, spill, inline=True)
            if rec['kind'] == 'bundle':
                rec['blocks'] = blocks(p)
            if not a.no_patch:
                rec['patch'] = patch(p, a.scratch)
        except Exception as ex:  # noqa: BLE001
            rec['error'] = f'{type(ex).__name__}: {ex}'
            traceback.print_exc(file=sys.stderr)
        shutil.rmtree(spill, ignore_errors=True)
        rec['rss_mb'] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss >> 10
        print(json.dumps(rec), flush=True)


if __name__ == '__main__':
    main()
