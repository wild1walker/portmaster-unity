// node tex_test.js <data dir> <reference out dir> <out dir> <convert.json> [workers]: textures.js on a data folder
// (the port's convert config, scale 0.5), then each output compared with pmunity's (objects by path id, streams byte for byte)
const fs = require('fs'), path = require('path');
const { Worker } = require('worker_threads');
const S = path.join(__dirname, '..', 'src');
const U = require(`${S}/unity.js`), T = require(`${S}/textures.js`), P = require(`${S}/pixels.js`);
U.setTypeTrees(JSON.parse(fs.readFileSync(`${S}/typetrees.json`, 'utf8')));
const [data, ref, out, cfgPath, nw] = process.argv.slice(2);
const WASM = fs.readFileSync(path.join(__dirname, '..', 'wasm', 'pmucore.wasm'));

const nfs = {
  list: async d => fs.readdirSync(path.join(data, d), { withFileTypes: true }).map(e => ({ name: e.name, dir: e.isDirectory() })),
  read: async r => new Uint8Array(fs.readFileSync(path.join(data, r))),
  slice: async (r, off, n) => { const fd = fs.openSync(path.join(data, r), 'r'); const b = Buffer.alloc(n); const k = fs.readSync(fd, b, 0, n, off); fs.closeSync(fd); return new Uint8Array(b.buffer, b.byteOffset, k); },
};
const workerPool = (n, guide = null) => require(`${S}/pool.js`).node(n, WASM, guide, { split: process.env.SPLIT !== '0' });
(async () => {
  const cfg = JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
  cfg.textures = Object.assign(cfg.textures || {}, { scale: 0.5 });
  const pool = workerPool(+(nw || 8));
  const t0 = Date.now();
  const r = await T.run({ fs: nfs, cfg, pool, log: s => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] ${s}`) });
  pool.close();
  fs.mkdirSync(out, { recursive: true });
  for (const [rel, v] of r.outputs) fs.writeFileSync(path.join(out, rel), Buffer.concat(v.parts.map(p => Buffer.from(p.buffer, p.byteOffset, p.length))));
  fs.writeFileSync(path.join(out, '.pmunity.json'), T.journal(r.done, r.scaled));
  console.log('time', (Date.now() - t0) / 1000, 's');
  // compare with pmunity's output
  const refState = JSON.parse(fs.readFileSync(path.join(ref, '.pmunity.json')));
  console.log('journal done same:', JSON.stringify(refState.done.textures) === JSON.stringify(r.done), 'scaled same:', JSON.stringify(refState.scaled) === JSON.stringify(r.scaled));
  for (const n of fs.readdirSync(ref)) {
    if (n.startsWith('.')) continue;
    const a = fs.readFileSync(path.join(out, n)), b = fs.readFileSync(path.join(ref, n));
    if (n.endsWith('.resS')) { console.log(n, a.equals(b) ? 'SAME' : `DIFFERENT (${a.length} vs ${b.length})`); continue; }
    const sa = U.parseSerialized(new Uint8Array(a), n), sb = U.parseSerialized(new Uint8Array(b), n);
    const mb = new Map(sb.objects.map(o => [String(o.pathId), o]));
    let same = 0, diff = 0;
    for (const o of sa.objects) {
      const x = U.objectBytes(sa, o), y = U.objectBytes(sb, mb.get(String(o.pathId)));
      if (Buffer.from(x).equals(Buffer.from(y))) same++; else { diff++; if (diff < 4) console.log('  object', o.pathId, 'class', o.classId, 'differs', x.length, y.length); }
    }
    console.log(n, `objects same ${same}, different ${diff}`);
  }
})().catch(e => { console.error('FAILED', e); process.exit(1); });
