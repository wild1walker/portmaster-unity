// node e2e.js <port.zip> <out.zip> <answers json> <game files...>: the page's pipeline (core.js stage, ports.js,
// textures.js with worker_threads, writeZip) with a disk store, as prepatch.html runs it in a browser
const fs = require('fs'), path = require('path'), os = require('os');
const { Worker } = require('worker_threads');
const S = path.join(__dirname, '..', 'src');
globalThis.UNITY = require(`${S}/unity.js`);
const CORE = require(`${S}/core.js`), T = require(`${S}/textures.js`), ST = require(`${S}/stage.js`), PORTS = require(`${S}/ports.js`), STEPS = require(`${S}/steps.js`);
UNITY.setTypeTrees(JSON.parse(fs.readFileSync(`${S}/typetrees.json`, 'utf8')));
const WASM = fs.readFileSync(path.join(__dirname, '..', 'wasm', 'pmucore.wasm'));
const [portPath, outPath, answersJson, ...games] = process.argv.slice(2);

async function fileOf(p) { const b = await fs.openAsBlob(p); return Object.assign(b, { name: path.basename(p) }); }
function diskStore(dir) {
  fs.mkdirSync(dir, { recursive: true });
  return {
    async write(name, chunks) { const p = path.join(dir, name); const fd = fs.openSync(p, 'w'); for await (const c of chunks) fs.writeSync(fd, c); fs.closeSync(fd); return p; },
    file: async p => fileOf(p),
    async clear() { fs.rmSync(dir, { recursive: true, force: true }); },
  };
}
function fileSink(p) { const fd = fs.openSync(p, 'w'); let pos = 0;
  return { async write(b) { fs.writeSync(fd, b, 0, b.length, pos); pos += b.length; }, async rewrite(at, b) { fs.writeSync(fd, b, 0, b.length, at); },
    async close() { fs.closeSync(fd); }, async abort() { fs.closeSync(fd); } }; }
const workerPool = (n, guide = null) => require(`${S}/pool.js`).node(n, WASM, guide, { split: process.env.SPLIT !== '0' });
(async () => {
  const t0 = Date.now(), log = s => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] ${s}`);
  const portFile = await fileOf(portPath);
  const port = await CORE.inspectPort(portFile); port.file = portFile;
  const sources = [];
  for (const g of games) { const f = await fileOf(g), l = f.name.toLowerCase(); sources.push({ name: f.name, kind: l.endsWith('.pkg') ? 'pkg' : 'zip', file: f, size: f.size }); }
  const stage = new ST.Stage(diskStore(path.join(os.tmpdir(), `pmustage-${Date.now()}`)));
  await CORE.stage({ port, sources, stage, log, tick: () => {} });
  log(`staged ${stage.entries.size} files`);
  const prof = PORTS.profile(port), data = prof.dataDir(stage);
  log(`profile ${prof.name}, data ${data}, questions ${prof.questions.map(q => q.id)}`);
  if (prof.check) log(`version check: ${await prof.check(stage, data)}`);
  const answers = JSON.parse(answersJson);
  const cfg = await prof.config(answers), c = T.merged(cfg);
  const gb = await PORTS.guideBytes(port, c), guide = gb ? T.parseGuide(gb, c, log) : null;
  log(`guide: ${guide ? (guide.ok ? 'ok' : 'not for these settings') : 'none'}`);
  const pool = workerPool(Math.max(1, os.cpus().length - 1), guide);
  const conv = await T.run({ fs: stage.view(data), cfg, pool, log,
    onFile: async (rel, outputs) => { if (outputs) for (const [r, v] of outputs) await stage.putParts(`${data}/${r}`, v.parts); } });
  pool.close();
  const state = { version: 1, done: { textures: conv.done }, scaled: conv.scaled };
  const left = await STEPS.runAll({ fs: stage.view(data), cfg, log, state, plugins: PORTS.plugins(port),
    put: (rel, parts) => stage.putParts(`${data}/${rel}`, parts) });
  // FULL=1: the shaders step, then the packing and install (prof.complete) as prepatch.html does them
  const PIXELS = require(`${S}/pixels.js`), BUNDLE = require(`${S}/bundle.js`);
  const px = new PIXELS.Pixels(await PIXELS.instantiate(await WebAssembly.compile(WASM)), null);
  BUNDLE.setUnlz4((s, n) => px.unlz4(s, n));
  const lz = { unlz4: (s, n) => px.unlz4(s, n), lz4: (s, l) => px.lz4(s, l) };
  if (process.env.FULL && left.includes('shaders')) {
    await require(`${S}/shaders.js`).run({ fs: stage.view(data), cfg, log, journal: STEPS.journal(state), lz, bundle: BUNDLE, lz4hc: c => px.lz4hc(c),
      put: (rel, parts) => stage.putParts(`${data}/${rel}`, parts) });
    left.splice(left.indexOf('shaders'), 1);
  }
  log(`steps done: ${Object.keys(state.done)}; left to the handheld: ${left}`);
  await stage.putParts(`${data}/.pmunity.json`, [new TextEncoder().encode(STEPS.pyJson(state))]);
  await prof.finish(stage, answers);
  if (process.env.FULL && !left.length && !conv.skipped.length && prof.complete) {
    const FW = path.join(__dirname, '..', 'codepatch');
    const codepatch = { crcs: JSON.parse(fs.readFileSync(`${FW}/known.json`, 'utf8')).crcs, start: async () => {
      const { dotnet } = await import(require('url').pathToFileURL(`${FW}/_framework/dotnet.js`).href);
      const rt = await dotnet.create(), ex = await rt.getAssemblyExports(rt.getConfig().mainAssemblyName);
      return { run(managed, args) {
        const r = ex.Entry.Run(managed.map(m => m.name), new Uint8Array(Buffer.concat(managed.map(m => m.bytes))), managed.map(m => m.bytes.length), args.join(' '));
        const v = new DataView(r.buffer, r.byteOffset, r.byteLength), rc = v.getInt32(0, true), ln = v.getInt32(4, true);
        return { rc, log: new TextDecoder().decode(r.subarray(8, 8 + ln)), dll: r.slice(8 + ln) }; } }; } };
    let k = 0;
    const putStore = async parts => fileOf(await stage.store.write(`t${k++}`, (async function* () { for (const p of parts) yield p; })()));
    const r = await prof.complete(stage, { lz, log, codepatch, putStore });
    log(`packed: ${Object.keys(r.pack.archives).join(', ')}; build-id ${r.pack.res.build_id}`);
  }
  const r = await CORE.writeZip({ port, stage, sink: fileSink(outPath), compress: true, log, tick: () => {} });
  await stage.clear();
  log(`zip ${r.entries} entries ${(r.size / 2 ** 20).toFixed(0)} MB; found ${JSON.stringify(r.found)}`);
})().catch(e => { console.error('FAILED', e); process.exit(1); });
