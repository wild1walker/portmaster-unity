# prep.py <orig data dir> <new data dir> <linux data dir> <plan journal> <out dir>: the inputs of the render
# comparison: each PALETTE_ON GLES3 fragment program of the palette shaders as converted before (orig) and
# after exactindex (new), the R8 textures with their RGBA32 originals, the RGBA32 palette textures the
# programs still sample (kept), and every PAL_* palette (RGB, 256x1). Writes cases.txt.
import UnityPy, sys, os, re, json
orig, new, linux, journal, out = sys.argv[1:6]
os.makedirs(out, exist_ok=True)

def frags(path, pid):
    s = list(UnityPy.load(path).files.values())[0].objects[pid].read().export()
    res = []
    for m in re.finditer(r'SubProgram "gles3 " \{\n(Keywords \{([^}]*)\}\n)?"(.*?)"\n\}', s, re.S):
        kw = (m.group(2) or '').replace('"', '').split()
        code = m.group(3)
        if 'PALETTE_ON' not in kw or '#ifdef FRAGMENT' not in code:
            continue
        f = code[code.index('#ifdef FRAGMENT') + len('#ifdef FRAGMENT\n'):]
        f = f[:f.rindex('#endif')]
        res.append(('_'.join(kw), f))
    return res

cases = []
for pid in (1115, 1148, 1155):
    a = frags(os.path.join(orig, 'resources.assets'), pid)
    b = frags(os.path.join(new, 'resources.assets'), pid)
    assert len(a) == len(b) and len(a) > 0, (pid, len(a), len(b))
    for i, ((ka, fa), (kb, fb)) in enumerate(zip(a, b)):
        assert ka == kb
        assert fa != fb and 'u_xlat0.z == 0.0' in fb, (pid, ka)
        open(f'{out}/orig_{pid}_{i}.glsl', 'w').write(fa)
        open(f'{out}/new_{pid}_{i}.glsl', 'w').write(fb)
        cases.append(f'prog {pid}_{i} {ka}')
plan = json.load(open(journal))['exactindex']
F = {}
def obj(d, fn, pid):
    k = (d, fn)
    if k not in F:
        F[k] = list(UnityPy.load(os.path.join(d, fn)).files.values())[0]
    return F[k].objects[pid].read_typetree()
for fn, pid, form in plan['textures']:
    o = obj(linux, fn, pid); n = obj(new, fn, pid)
    assert o['m_TextureFormat'] == 4 and n['m_TextureFormat'] == 63
    w, h = o['m_Width'], o['m_Height']
    open(f'{out}/t_{fn}_{pid}.rgba', 'wb').write(bytes(o['image data']))
    open(f'{out}/t_{fn}_{pid}.r8', 'wb').write(bytes(n['image data']))
    cases.append(f'tex {fn}_{pid} {w} {h} {o["m_Name"]}')
lf = list(UnityPy.load(os.path.join(linux, 'resources.assets')).files.values())[0]
for o in lf.objects.values():
    if o.type.name == 'Texture2D':
        t = o.read_typetree()
        if t['m_Name'].startswith('PAL_') and t['m_Width'] == 256 and t['m_TextureFormat'] == 3:
            open(f'{out}/pal_{t["m_Name"]}.rgb', 'wb').write(bytes(t['image data']))
            cases.append(f'pal {t["m_Name"]}')
# a few RGBA32 textures the programs still sample (the Penitent's own, left as they are): the new
# program must draw them as the old one
for name in sys.argv[6:]:
    fn, pid = name.split(':'); o = obj(new, fn, int(pid))
    assert o['m_TextureFormat'] == 4
    open(f'{out}/k_{fn}_{pid}.rgba', 'wb').write(bytes(o['image data']))
    cases.append(f'kept {fn}_{pid} {o["m_Width"]} {o["m_Height"]} {o["m_Name"]}')
open(f'{out}/cases.txt', 'w').write('\n'.join(cases) + '\n')
print(len(cases), 'cases')
