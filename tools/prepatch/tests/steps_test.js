// node steps_test.js <data dir> <out dir> <cfg.json> [steps]: src/steps.js's settings / audio / animclips on a
// data folder in overlay (later steps read what earlier ones wrote), the converted files and the journal into
// <out dir>, to compare with pmunity's (tests/refcup_steps.sh) byte for byte
const fs = require('fs'), path = require('path');
const S = path.join(__dirname, '..', 'src');
globalThis.UNITY = require(`${S}/unity.js`);
const STEPS = require(`${S}/steps.js`);
UNITY.setTypeTrees(JSON.parse(fs.readFileSync(`${S}/typetrees.json`, 'utf8')));
const [data, out, cfgPath, which = 'settings,audio,animclips'] = process.argv.slice(2);
const cfg = JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
const newest = r => fs.existsSync(path.join(out, r)) ? path.join(out, r) : path.join(data, r);
const nfs = {
  list: async d => fs.readdirSync(path.join(data, d), { withFileTypes: true }).map(e => ({ name: e.name, dir: e.isDirectory() })),
  read: async r => new Uint8Array(fs.readFileSync(newest(r))),
};
const put = async (rel, parts) => { fs.mkdirSync(path.dirname(path.join(out, rel)), { recursive: true }); fs.writeFileSync(path.join(out, rel), Buffer.concat(parts)); };
(async () => {
  fs.rmSync(out, { recursive: true, force: true }); fs.mkdirSync(out, { recursive: true });
  const j = STEPS.journal({ version: 1, done: {}, scaled: {} });
  const t0 = Date.now(), log = s => console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] ${s}`);
  for (const s of which.split(',')) {
    const run = STEPS.BUILTIN[s] || Object.values(STEPS.KNOWN_PLUGINS).find(k => k.step === s).run;
    await run({ fs: nfs, cfg, put, journal: j, log });
  }
  fs.writeFileSync(path.join(out, '.pmunity.json'), STEPS.pyJson(j.state));
  log('done');
})().catch(e => { console.error(e); process.exit(1); });
