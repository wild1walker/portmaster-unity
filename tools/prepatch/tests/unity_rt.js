// node unity_rt.js <data dir> <out.json>: unity.js on every serialized file of a data folder: objects of the
// classes the textures step reads, read and written back (must give the same bytes), and their key fields
const fs = require('fs'), path = require('path');
const U = require(path.join(__dirname, '..', 'src', 'unity.js'));
U.setTypeTrees(JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'src', 'typetrees.json'), 'utf8')));
const [dir, outPath] = process.argv.slice(2);
const CLASSES = { 28: 'Texture2D', 213: 'Sprite', 687078895: 'SpriteAtlas', 43: 'Mesh' };
const out = {}; let ok = 0, bad = 0;
for (const n of fs.readdirSync(dir).sort()) {
  if (!(n.endsWith('.assets') || n === 'globalgamemanagers' || /^level\d+$/.test(n))) continue;
  const sf = U.parseSerialized(new Uint8Array(fs.readFileSync(path.join(dir, n))), n);
  const fields = {};
  for (const o of sf.objects) {
    if (!CLASSES[o.classId]) continue;
    let v;
    try { v = U.readObject(sf, o); } catch (e) { bad++; console.log(n, o.pathId, CLASSES[o.classId], 'READ', e.message); continue; }
    const back = U.writeObject(sf, o, v), orig = U.objectBytes(sf, o);
    if (back.length === orig.length && back.every((x, i) => x === orig[i])) ok++;
    else { bad++; if (bad < 10) console.log(n, o.pathId, CLASSES[o.classId], 'ROUNDTRIP', back.length, orig.length); }
    if (o.classId === 28) fields[String(o.pathId)] = [v.m_Name, v.m_Width, v.m_Height, v.m_TextureFormat, v.m_MipCount ?? null,
      v.m_StreamData ? [Number(v.m_StreamData.offset), v.m_StreamData.size, v.m_StreamData.path] : null, (v['image data'] || []).length];
    if (o.classId === 213) fields[String(o.pathId)] = [v.m_Name, v.m_Rect.width, v.m_PixelsToUnits, String(v.m_RD.texture.m_PathID)];
  }
  out[n] = { unity: sf.unity, version: sf.version, typeTree: sf.typeTree, objects: sf.objects.length, fields };
}
fs.writeFileSync(outPath, JSON.stringify(out));
console.log('roundtrip same', ok, 'different/failed', bad);
