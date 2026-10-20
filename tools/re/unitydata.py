#!/usr/bin/env python3
"""What a Unity player's built-in data says about its formats: the SerializedFile header of
"unity default resources" (format version, Unity version string, platform), its object types,
and the compiled shader programs of its Shaders (blob layout, serializer version per program
platform, whether convert/pmunity/shaders/blob.py reads and writes each entry back unchanged).

usage: python3 -I unitydata.py "unity default resources" [REPO_DIR]  -> JSON on stdout
Needs UnityPy and lz4 (the converter's environment).
"""
import sys, os, json, struct, collections


def header(path):
    with open(path, 'rb') as f:
        h = f.read(256)
    msize, fsize, ver, doff = struct.unpack_from('>IIII', h, 0)
    r = {'format': ver}
    p = 16
    if ver >= 9:
        r['big_endian'] = bool(h[16]); p = 20
    if ver >= 22:                         # 64-bit sizes: metadata size, file size, data offset
        msize, fsize, doff = struct.unpack_from('>IQQ', h, 20)
        p = 48
    r['header_bytes'] = p
    end = h.index(b'\0', p)
    r['unity_version'] = h[p:end].decode('ascii', 'replace')
    bo = '>' if r.get('big_endian') else '<'
    r['platform'] = struct.unpack_from(bo + 'I', h, end + 1)[0]
    r['platform_offset'] = end + 1
    if ver >= 13:
        r['type_tree'] = bool(h[end + 5])
    return r


def main():
    path = sys.argv[1]
    repo = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', '..')
    sys.path.insert(0, os.path.join(repo, 'convert'))
    out = {'header': header(path)}
    import UnityPy
    import lz4.block
    try:
        from pmunity.shaders import blob as pblob
    except Exception as ex:                # the converter's parser is optional here
        pblob = None
        out['blob_import_error'] = str(ex)
    env = UnityPy.load(path)
    types = collections.Counter()
    shaders = []
    for obj in env.objects:
        types[obj.type.name] += 1
        if obj.type.name != 'Shader':
            continue
        try:
            t = obj.read_typetree()
        except Exception as ex:
            shaders.append({'error': 'typetree: %s' % ex})
            continue
        name = t.get('m_ParsedForm', {}).get('m_Name') or t.get('m_Name')
        s = {'name': name, 'keys': sorted(k for k in t.keys() if k not in ('m_ParsedForm', 'compressedBlob'))}
        plats = t.get('platforms', [])
        offs, clens, dlens = t.get('offsets', []), t.get('compressedLengths', []), t.get('decompressedLengths', [])
        raw = bytes(t.get('compressedBlob', b''))
        s['platforms'] = plats
        s['nested_offsets'] = bool(offs) and isinstance(offs[0], list)
        per = {}
        for i, plat in enumerate(plats):
            segs = list(zip(offs[i], clens[i], dlens[i])) if s['nested_offsets'] else [(offs[i], clens[i], dlens[i])]
            data = [lz4.block.decompress(raw[o:o + c], uncompressed_size=d) if c else b'' for o, c, d in segs]
            st = collections.Counter()
            if not data or not data[0]:
                per[str(plat)] = {}
                continue
            # the entry table is in the first segment: count, then (offset, length) per entry;
            # 2019.3+ (nested offsets): (offset, length, segment)
            n, = struct.unpack_from('<i', data[0])
            esz = 12 if s['nested_offsets'] else 8
            for k in range(n):
                eo, el = struct.unpack_from('<ii', data[0], 4 + esz * k)
                sg = struct.unpack_from('<i', data[0], 4 + esz * k + 8)[0] if esz == 12 else 0
                if not el:
                    continue
                e = data[sg][eo:eo + el]
                v, g = struct.unpack_from('<ii', e)
                res = 'not-parsed'
                if pblob is not None:
                    try:
                        ent = pblob.Entry.parse(e)
                        res = 'guessed-%d' % len(ent.stats) if ent.guessed else 'ok'
                        if not ent.code:            # 2021.2+: parameter entries share the blob
                            res = 'no-code(%s)' % res
                    except Exception:
                        res = 'FAIL'
                st['v%d gpu%d %s' % (v, g, res)] += 1
            per[str(plat)] = dict(st)
            if s['nested_offsets']:
                per[str(plat)]['segments'] = len(segs)
        s['programs'] = per
        pf = t.get('m_ParsedForm', {})
        sub = None
        for ss in pf.get('m_SubShaders', []):
            for ps in ss.get('m_Passes', []):
                for prog in ('progVertex', 'progFragment'):
                    pr = ps.get(prog, {})
                    sps = pr.get('m_SubPrograms', []) or pr.get('m_PlayerSubPrograms', [])
                    while sps and isinstance(sps[0], list):     # per-tier lists (2021.2+)
                        sps = [x for x in sps if x][0] if any(sps) else []
                    if sps:
                        sub = sorted(sps[0].keys())
                        s['program_fields'] = sorted(pr.keys())
                        break
                if sub:
                    break
            if sub:
                break
        s['subprogram_fields'] = sub
        shaders.append(s)
    out['types'] = dict(types)
    out['shaders'] = shaders
    print(json.dumps(out, indent=1, default=str))


main()
