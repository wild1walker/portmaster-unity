# python3 cmp_full.py <page-built zip> [reference tree]: the ready-to-run zip's ports/cuphead against the release's
# own setup run on the PC (tests/refcup_full.sh: $REF/cuphead), every file by MD5. The APK and OBB are
# compared as files and, where they differ, entry by entry. patchlog.txt (the setup's log) only exists in the reference.
import hashlib, os, sys, zipfile
zp = sys.argv[1]
ref = os.path.expanduser(sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.environ.get('REF', 'ref-cupfull'), 'cuphead'))
z = zipfile.ZipFile(zp)
mine = {i.filename[len('cuphead/'):]: i for i in z.infolist() if i.filename.startswith('cuphead/') and not i.is_dir()}
theirs = {}
for root, dirs, files in os.walk(ref):
    for f in files:
        p = os.path.join(root, f)
        theirs[os.path.relpath(p, ref).replace(os.sep, '/')] = p
skip = {'patchlog.txt'}
def md5z(i):
    h = hashlib.md5()
    with z.open(i) as f:
        for b in iter(lambda: f.read(1 << 22), b''): h.update(b)
    return h.hexdigest()
def md5f(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''): h.update(b)
    return h.hexdigest()
missing = sorted(set(theirs) - set(mine) - skip)
extra = sorted(set(mine) - set(theirs))
diff = []
for n in sorted(set(mine) & set(theirs)):
    if mine[n].file_size != os.path.getsize(theirs[n]) or md5z(mine[n]) != md5f(theirs[n]): diff.append(n)
print(f'files: {len(mine)} in the zip, {len(theirs)} in the reference; missing {len(missing)}, extra {len(extra)}, different {len(diff)}')
for k, v in (('missing', missing), ('extra', extra), ('different', diff)):
    if v: print(f'  {k}:', v[:20])
for n in diff:                                 # inner archives: which entries differ
    if n.endswith(('.apk', '.obb')):
        import io
        a = zipfile.ZipFile(z.open(mine[n])); b = zipfile.ZipFile(theirs[n])
        ea = {i.filename: i for i in a.infolist()}; eb = {i.filename: i for i in b.infolist()}
        bad = [x for x in eb if x not in ea or ea[x].CRC != eb[x].CRC or ea[x].file_size != eb[x].file_size]
        print(f'  {n}: entries {len(ea)} vs {len(eb)}; different or missing {len(bad)}', bad[:10], 'extra', [x for x in ea if x not in eb][:5],
              'order same' if [i.filename for i in a.infolist()] == [i.filename for i in b.infolist()] else 'order differs')
