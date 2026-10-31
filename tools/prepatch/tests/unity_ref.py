# python unity_ref.py <data dir> <js out.json>: UnityPy's reading of the same objects, compared with unity.js's
import json, os, sys
import UnityPy
d, js = sys.argv[1], json.load(open(sys.argv[2]))
diff = same = 0
for n, info in sorted(js.items()):
    env = UnityPy.load(os.path.join(d, n))
    sf = env.file
    assert len(sf.objects) == info['objects'], (n, len(sf.objects), info['objects'])
    for pid, o in sf.objects.items():
        if o.type.name not in ('Texture2D', 'Sprite'):
            continue
        t = o.read_typetree()
        if o.type.name == 'Texture2D':
            sd = t.get('m_StreamData')
            mine = [t['m_Name'], t['m_Width'], t['m_Height'], t['m_TextureFormat'], t.get('m_MipCount'),
                    [sd['offset'], sd['size'], sd['path']] if sd else None, len(t.get('image data') or b'')]
        else:
            mine = [t['m_Name'], t['m_Rect']['width'], t['m_PixelsToUnits'], str(t['m_RD']['texture']['m_PathID'])]
        theirs = info['fields'].get(str(pid))
        if theirs != mine:
            diff += 1
            if diff < 10:
                print(n, pid, 'UnityPy', mine, 'js', theirs)
        else:
            same += 1
    print(n, info['unity'], 'format', info['version'], 'type trees', info['typeTree'], info['objects'], 'objects')
print('fields same', same, 'different', diff)
