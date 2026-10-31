// node shaders_test.js <data dir> <out dir> <cfg.json> <pmunity's out dir>: src/shaders.js's step on a data folder in
// overlay, the written files and the journal compared with pmunity's shaders step (tests/refcup_steps.sh ... shaders)
const fs = require('fs'), path = require('path');
const S = path.join(__dirname, '..', 'src');
globalThis.UNITY = require(`${S}/unity.js`);
const SH = require(`${S}/shaders.js`), STEPS = require(`${S}/steps.js`), BUNDLE = require(`${S}/bundle.js`), PIXELS = require(`${S}/pixels.js`);
UNITY.setTypeTrees(JSON.parse(fs.readFileSync(`${S}/typetrees.json`, 'utf8')));
const [data, out, cfgPath, ref] = process.argv.slice(2);
const cfg = JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
const nfs = {
  list: async d => fs.readdirSync(path.join(data, d), { withFileTypes: true }).map(e => ({ name: e.name, dir: e.isDirectory() })),
  read: async r => new Uint8Array(fs.readFileSync(path.join(data, r))),
  slice: async (r, off, n) => { const fd = fs.openSync(path.join(data, r), 'r'), b = Buffer.alloc(n); const k = fs.readSync(fd, b, 0, n, off); fs.closeSync(fd); return new Uint8Array(b.buffer, b.byteOffset, k); },
};
(async () => {
  const px = new PIXELS.Pixels(await PIXELS.instantiate(await WebAssembly.compile(fs.readFileSync(`${S}/../wasm/pmucore.wasm`))), null);
  BUNDLE.setUnlz4((s, n) => px.unlz4(s, n));
  const lz = { unlz4: (s, n) => px.unlz4(s, n), lz4: (s, l) => px.lz4(s, l) };
  fs.rmSync(out, { recursive: true, force: true }); fs.mkdirSync(out, { recursive: true });
  const put = async (rel, parts) => { const p = path.join(out, rel); fs.mkdirSync(path.dirname(p), { recursive: true }); fs.writeFileSync(p, Buffer.concat(parts.map(x => Buffer.from(x.buffer, x.byteOffset, x.length)))); };
  const j = STEPS.journal({ version: 1, done: {}, scaled: {} });
  const t0 = Date.now(), log = s => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] ${s}`);
  await SH.run({ fs: nfs, cfg, put, journal: j, log, lz, bundle: BUNDLE, lz4hc: chunks => px.lz4hc(chunks) });
  fs.writeFileSync(path.join(out, '.pmunity.json'), STEPS.pyJson(j.state));
  const files = (root, pre = '') => fs.readdirSync(root, { withFileTypes: true }).flatMap(e => e.isDirectory() ? files(path.join(root, e.name), pre + e.name + '/') : [pre + e.name]);
  const a = files(ref).filter(f => f !== '.pmunity.json').sort(), b = files(out).filter(f => f !== '.pmunity.json').sort();
  const bad = a.filter(f => !b.includes(f) || !fs.readFileSync(path.join(ref, f)).equals(fs.readFileSync(path.join(out, f))));
  const ja = JSON.parse(fs.readFileSync(path.join(ref, '.pmunity.json'))), jb = JSON.parse(fs.readFileSync(path.join(out, '.pmunity.json')));
  console.log(`files: ${b.length} (pmunity ${a.length}); missing or different: ${bad.length}`, bad.slice(0, 10), 'extra:', b.filter(f => !a.includes(f)));
  console.log('journal same:', JSON.stringify(ja.done) === JSON.stringify(jb.done));
})().catch(e => { console.error(e); process.exit(1); });
