// node android_test.mjs <port dir (setup/, gamedata/<Data>, gamedata/mono-etc)> <player dir> <pmunity's out dir> <work dir>
//   [codepatch _framework dir]: android.js's whole packing step (build, pack, archives) on the same input as
// tests/refcup_android.sh, every tree file and the zip folder compared with pmunity's by MD5
import fs from 'fs';
import path from 'path';
import crypto from 'crypto';
import { createRequire } from 'module';
import { pathToFileURL, fileURLToPath } from 'url';
const require = createRequire(import.meta.url);
const S = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'src');
globalThis.UNITY = require(`${S}/unity.js`);
const A = require(`${S}/android.js`), PIXELS = require(`${S}/pixels.js`), CORE = require(`${S}/core.js`);
UNITY.setTypeTrees(JSON.parse(fs.readFileSync(`${S}/typetrees.json`, 'utf8')));
const [G, P, REF, W, FW = path.join(S, '..', 'codepatch', '_framework')] = process.argv.slice(2);
const view = root => ({
  list: async d => fs.readdirSync(path.join(root, d), { withFileTypes: true }).map(e => ({ name: e.name, dir: e.isDirectory() })),
  size: async r => { try { return fs.statSync(path.join(root, r)).size; } catch { return -1; } },
  read: async r => new Uint8Array(fs.readFileSync(path.join(root, r))),
  slice: async (r, off, n) => { const fd = fs.openSync(path.join(root, r), 'r'), b = Buffer.alloc(n); const k = fs.readSync(fd, b, 0, n, off); fs.closeSync(fd); return new Uint8Array(b.buffer, b.byteOffset, k); },
  path: r => path.join(root, r),
});
const t0 = Date.now(), log = s => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] ${s}`);
const GD = path.join(G, 'gamedata'), D = path.join(GD, fs.readdirSync(GD).find(n => n.endsWith('_Data')));
const px = new PIXELS.Pixels(await PIXELS.instantiate(await WebAssembly.compile(fs.readFileSync(`${S}/../wasm/pmucore.wasm`))), null);
const lz = { unlz4: (s, n) => px.unlz4(s, n), lz4: (s, l) => px.lz4(s, l) };
// codepatch (WebAssembly) on the game's Managed folder
const { dotnet } = await import(pathToFileURL(path.join(FW, 'dotnet.js')).href);
const rt = await dotnet.create(), ex = await rt.getAssemblyExports(rt.getConfig().mainAssemblyName);
const codepatch = async () => {
  const dir = path.join(D, 'Managed'), names = fs.readdirSync(dir).filter(n => n.endsWith('.dll')), bufs = names.map(n => fs.readFileSync(path.join(dir, n)));
  const r = ex.Entry.Run(names, new Uint8Array(Buffer.concat(bufs)), bufs.map(b => b.length), '--target=android --steam-dlc');
  const v = new DataView(r.buffer, r.byteOffset, r.byteLength), rc = v.getInt32(0, true), ln = v.getInt32(4, true);
  if (rc !== 0) throw new Error('codepatch failed: ' + new TextDecoder().decode(r.subarray(8, 8 + ln)));
  return r.slice(8 + ln);
};
fs.rmSync(W, { recursive: true, force: true }); fs.mkdirSync(W, { recursive: true });
const tree = new Map(), where = new Map();
const out = {
  put: async (dst, parts) => { const p = path.join(W, 'tree', dst); fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, Buffer.concat(parts.map(x => Buffer.from(x.buffer, x.byteOffset, x.length)))); tree.set(dst, fs.statSync(p).size); where.set(dst, p); },
  link: async (dst, v, rel) => { const p = v.path(rel); tree.set(dst, fs.statSync(p).size); where.set(dst, p); },
};
const r = await A.build({ data: view(D), pkg: view(P), metc: view(path.join(GD, 'mono-etc')), gd: view(GD), unity: '2017.4.9f1', name: 'Cuphead',
  rootFiles: ['animclips.txt'], managedFiles: [{ name: 'CupheadPort.dll', bytes: new Uint8Array(fs.readFileSync(path.join(G, 'setup/CupheadPort.dll'))) }],
  assemblyPatch: { 'Assembly-CSharp.dll': codepatch }, dropMissing: ['GalaxyCSharp.dll'], dllmap: [['SDL2', 'libSDL2-2.0.so.0']], videoShader: false, lz, log, out });
const bid = A.buildId(r.ggm);
const pk = A.pack({ tree, name: 'Cuphead', pkgName: 'com.cupheadport.game', unity: '2017.4.9f1', bid, loose: r.plugins.fmod ? ['*.bank'] : [], pluginMap: r.pluginMap });
log(`build-id ${bid}; archives ${Object.keys(pk.archives).join(', ')}`);
const chunksOf = src => async function* () {
  if (src instanceof Uint8Array) { if (src.length) yield src; return; }
  const fd = fs.openSync(where.get(src), 'r'), b = Buffer.alloc(8 << 20);
  try { for (;;) { const k = fs.readSync(fd, b, 0, b.length, null); if (!k) break; yield new Uint8Array(b.subarray(0, k)); } } finally { fs.closeSync(fd); }
};
const Z = path.join(W, 'zip'); fs.mkdirSync(Z, { recursive: true });
for (const [file, list] of Object.entries(pk.archives)) {
  const entries = list.map(([a, src]) => ({ name: a, size: src instanceof Uint8Array ? src.length : tree.get(src), chunks: chunksOf(src) }));
  const fd = fs.openSync(path.join(Z, file), 'w');
  for await (const c of A.zipChunks(entries, CORE.crc32)) fs.writeSync(fd, c);
  fs.closeSync(fd);
  log(`pack: ${file}: ${entries.length} entries, ${(fs.statSync(path.join(Z, file)).size / 2 ** 20).toFixed(1)} MiB`);
}
for (const [rel, src] of pk.zipFiles) { const p = path.join(Z, rel); fs.mkdirSync(path.dirname(p), { recursive: true }); fs.writeFileSync(p, src instanceof Uint8Array ? src : fs.readFileSync(where.get(src))); }
// compare
const md5 = p => crypto.createHash('md5').update(fs.readFileSync(p)).digest('hex');
const files = (root, pre = '') => fs.readdirSync(root, { withFileTypes: true }).flatMap(e => e.isDirectory() ? files(path.join(root, e.name), pre + e.name + '/') : [pre + e.name]);
const refTree = files(path.join(REF, 'Cuphead')).map(f => 'Cuphead/' + f).sort();
const mine = [...tree.keys()].filter(d => d.startsWith('Cuphead/')).sort();
const badTree = refTree.filter(f => !tree.has(f) || md5(where.get(f)) !== md5(path.join(REF, f)));
console.log(`tree: ${mine.length} files (pmunity ${refTree.length}); missing or different: ${badTree.length}`, badTree.slice(0, 10),
  'extra:', mine.filter(f => !refTree.includes(f)).slice(0, 5));
const refZip = files(path.join(REF, 'zip')).sort(), myZip = files(Z).sort();
const badZip = refZip.filter(f => !myZip.includes(f) || md5(path.join(Z, f)) !== md5(path.join(REF, 'zip', f)));
console.log(`zip folder: ${myZip.length} files (pmunity ${refZip.length}); missing or different: ${badZip.length}`, badZip.slice(0, 10),
  'extra:', myZip.filter(f => !refZip.includes(f)));
log('done');
process.exit(0);
