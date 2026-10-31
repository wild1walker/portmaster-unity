// node cmp_bundle.js <mine dir> <ref dir> <rel...>: content comparison (objects by path id, other nodes by bytes)
const fs = require('fs'), path = require('path');
const S = path.join(__dirname, '..', 'src');
const U = require(`${S}/unity.js`), B = require(`${S}/bundle.js`);
U.setTypeTrees(JSON.parse(fs.readFileSync(`${S}/typetrees.json`, 'utf8')));
const [mine, ref, ...rels] = process.argv.slice(2);
const fileOf = async p => Object.assign(await fs.openAsBlob(p), { name: path.basename(p) });
function cmpSF(a, b, name) {
  const sa = U.parseSerialized(a, name), sb = U.parseSerialized(b, name);
  const mb = new Map(sb.objects.map(o => [String(o.pathId), o]));
  let same = 0, diff = 0;
  for (const o of sa.objects) { const x = Buffer.from(U.objectBytes(sa, o)), y = Buffer.from(U.objectBytes(sb, mb.get(String(o.pathId))));
    if (x.equals(y)) same++; else { diff++; if (diff < 3) console.log('   object', o.pathId, 'class', o.classId, x.length, y.length); } }
  const order = (s) => s.objects.map(o => o.pathId);
  const sorted = a => a.every((v, i) => !i || a[i - 1] < v);
  console.log(`  ${name}: objects same ${same}, different ${diff}; table sorted by path id: mine ${sorted(order(sa))} ref ${sorted(order(sb))}; data order by path id: ref ${sorted([...sb.objects].sort((x, y) => x.byteStart - y.byteStart).map(o => o.pathId))}`);
}
(async () => {
  for (const rel of rels) {
    const pa = path.join(mine, rel), pb = path.join(ref, rel);
    const head = fs.readFileSync(pa).subarray(0, 7).toString();
    console.log(rel);
    if (head !== 'UnityFS') { cmpSF(new Uint8Array(fs.readFileSync(pa)), new Uint8Array(fs.readFileSync(pb)), rel); continue; }
    const a = await B.open(await fileOf(pa)), b = await B.open(await fileOf(pb));
    console.log('  nodes mine', a.nodes.map(n => `${n.name}:${n.size}:${n.flags}`).join(' '), '| ref', b.nodes.map(n => `${n.name}:${n.size}:${n.flags}`).join(' '));
    for (const n of a.nodes) {
      const x = await a.node(n.name), y = await b.node(n.name);
      if (!y) { console.log('  ', n.name, 'not in ref'); continue; }
      if (n.flags & 4) cmpSF(x, y, n.name); else console.log('  ', n.name, Buffer.from(x).equals(Buffer.from(y)) ? 'same' : 'DIFFERENT');
    }
  }
})();
