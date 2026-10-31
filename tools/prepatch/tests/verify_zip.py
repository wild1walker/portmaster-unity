# python verify_zip.py <zip> <data prefix in zip>: every serialized file and bundle of the zip's data folder loaded
# with UnityPy (as the handheld's android-data --verify-bundles does) and every texture read back
import io, sys, zipfile, collections
import UnityPy
z = zipfile.ZipFile(sys.argv[1]); base = sys.argv[2]
counts = collections.Counter(); bad = 0
for n in z.namelist():
    if not n.startswith(base) or n.endswith('/'):
        continue
    rel = n[len(base):]
    if not (rel.startswith('StreamingAssets/AssetBundles/') or rel.endswith('.assets')):
        continue
    data = z.read(n)
    if rel.endswith('.resS'):
        continue
    try:
        env = UnityPy.load(data)
        for f in env.files.values():
            objs = getattr(f, 'objects', None) or {}
            if not objs and hasattr(f, 'files'):
                for g in f.files.values():
                    objs = getattr(g, 'objects', None) or objs
            for o in objs.values():
                if o.type.name == 'Texture2D':
                    t = o.read_typetree()
                    sd = t.get('m_StreamData') or {}
                    if sd.get('size') and rel.startswith('StreamingAssets/'):
                        raise ValueError(f'{t["m_Name"]}: still streamed (inline_streams)')
                    if t['m_TextureFormat'] in range(48, 60) and not sd.get('size'):
                        bw = [4, 5, 6, 8, 10, 12][(t['m_TextureFormat'] - 48) % 6]
                        w, h, want = t['m_Width'], t['m_Height'], 0
                        for _ in range(t.get('m_MipCount', 1)):
                            want += -(-w // bw) * -(-h // bw) * 16; w, h = max(1, w // 2), max(1, h // 2)
                        if len(t['image data']) != want:
                            raise ValueError(f'{t["m_Name"]}: {len(t["image data"])} bytes, {want} expected')
                    counts['textures'] += 1
        counts['files'] += 1
    except Exception as ex:
        bad += 1
        print('FAIL', rel, ex)
print(dict(counts), 'failures', bad)
