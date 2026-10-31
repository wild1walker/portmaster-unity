// node sf_android_test.js <packing input data dir> <pmunity's tree (android/<Name>)>: android.js's serialized-file
// actions (platform byte, textures inlined from .resS, globalgamemanagers) vs android-data's, byte for byte
const fs = require('fs'), path = require('path');
const S = path.join(__dirname, '..', 'src');
globalThis.UNITY = require(`${S}/unity.js`);
const A = require(`${S}/android.js`);
UNITY.setTypeTrees(JSON.parse(fs.readFileSync(`${S}/typetrees.json`, 'utf8')));
const [data, tree] = process.argv.slice(2);
const D = path.join(tree, 'assets/bin/Data');
const same = (parts, ref) => Buffer.concat(parts.map(p => Buffer.from(p.buffer, p.byteOffset, p.length))).equals(fs.readFileSync(ref));
(async () => {
  const r = { patch: [0, 0], inline: [0, 0], ggm: null }, bad = [];
  const names = fs.readdirSync(data).filter(n => fs.statSync(path.join(data, n)).isFile()).sort();
  for (const n of names) {
    if (!(n === 'globalgamemanagers.assets' || n === 'resources.assets' || /^(level\d+|sharedassets\d+\.assets)$/.test(n))) continue;
    const bytes = new Uint8Array(fs.readFileSync(path.join(data, n)));
    if (fs.existsSync(path.join(data, n + '.resS'))) {
      const res = await A.inlineSerialized(bytes, n, async (name, off, size) => {
        const fd = fs.openSync(path.join(data, name), 'r'), b = Buffer.alloc(size); fs.readSync(fd, b, 0, size, off); fs.closeSync(fd); return new Uint8Array(b); });
      r.inline[0]++; if (same(res.parts, path.join(D, n))) r.inline[1]++; else bad.push(n);
    } else {
      const b = Uint8Array.from(bytes); A.patchSfPlatform(b.subarray(0, 128));
      r.patch[0]++; if (same([b], path.join(D, n))) r.patch[1]++; else bad.push(n);
    }
  }
  const bex = Uint8Array.from(fs.readFileSync(path.join(data, 'Resources/unity_builtin_extra'))); A.patchSfPlatform(bex.subarray(0, 128));
  r.patch[0]++; if (same([bex], path.join(D, 'Resources/unity_builtin_extra'))) r.patch[1]++; else bad.push('Resources/unity_builtin_extra');
  const managed = new Set(fs.readdirSync(path.join(D, 'Managed')).filter(n => n.endsWith('.dll')));
  const g = A.editGlobalGameManagers(new Uint8Array(fs.readFileSync(path.join(data, 'globalgamemanagers'))),
    { graphicsApis: [11], managedNames: managed, dropMissing: ['GalaxyCSharp.dll'], rewiredJoysticks: 4, blit: 'never', mtRendering: 'keep', asyncUploadBuffer: 0 });
  r.ggm = same(g.parts, path.join(D, 'globalgamemanagers'));
  console.log(`platform byte: ${r.patch[1]}/${r.patch[0]} same; inlined: ${r.inline[1]}/${r.inline[0]} same; globalgamemanagers same: ${r.ggm}`);
  console.log('notes:', g.notes.join('; '));
  if (bad.length) console.log('different:', bad.slice(0, 10));
})().catch(e => { console.error(e); process.exit(1); });
