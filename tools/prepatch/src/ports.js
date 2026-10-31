// What each kind of port's setup expects in gamedata/, and how it is told that work is done:
//   standard (pmunity port new): the data folder anywhere under gamedata/ (find_any), its config
//     setup/convert.json with the answers' --set values; answers that change textures saved in
//     gamedata/.setup/answers/<ID>, which the setup takes before asking
//   cuphead (portmaster-cuphead's own setup): step 1 done here: gamedata/Cuphead_Data, mono-etc,
//     GOG's .info files, .setup/.step_game and .texture_quality (the answer the conversion used)
// The conversion's journal (<data>/.pmunity.json) is the handheld converter's own: it skips those files.
'use strict';
const PORTS = (() => {
const PLAYER_ = typeof PLAYER !== 'undefined' ? PLAYER : require('./player.js');
const QUESTIONS_ = typeof QUESTIONS !== 'undefined' ? QUESTIONS : require('./questions.js');
const unq = s => s.replace(/\\(.)/g, (_, c) => c === 'n' ? '\n' : c);

function luaQuestions(lua) {                  // questions.lua: [{id, question, options: [[value, label]]}]
  const out = [];
  for (const chunk of lua.split(/\bid\s*=/).slice(1)) {
    const id = chunk.match(/^\s*"((?:[^"\\]|\\.)*)"/);
    if (!id) continue;
    const q = chunk.match(/question\s*=\s*"((?:[^"\\]|\\.)*)"/);
    const opts = [...chunk.matchAll(/\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}/g)].map(m => [unq(m[1]), unq(m[2])]);
    out.push({ id: unq(id[1]), question: q ? unq(q[1]) : unq(id[1]), options: opts });
  }
  return out;
}
const setValue = v => { try { return JSON.parse(v); } catch { return v; } };     // convert --set: JSON, else a string
function applySet(cfg, key, v) { const [sec, k] = [key.slice(0, key.indexOf('.')), key.slice(key.indexOf('.') + 1)]; (cfg[sec] = cfg[sec] || {})[k] = setValue(v); }
const hasFiles = (stage, dir) => ['globalgamemanagers', 'data.unity3d', 'mainData'].some(n => stage.has(`${dir}/${n}`));

function standard(port) {
  const env = port.env, labels = Object.fromEntries(luaQuestions(port.lua).map(q => [q.id, q]));
  const questions = (env.QUESTIONS || '').split(/\s+/).filter(Boolean).map(id => {
    const values = (env[`Q_${id}_OPTIONS`] || '').split(/\s+/).filter(Boolean);
    const lab = Object.fromEntries((labels[id] || { options: [] }).options);
    return { id, question: (labels[id] || {}).question || id, options: values.map(v => [v, lab[v] || v]),
      default: env[`Q_${id}_DEFAULT`] || values[0], set: env[`Q_${id}_SET`] || '' };
  }).filter(q => q.set.startsWith('textures.'));     // (only what changes the conversion done here)
  return {
    name: 'standard', questions,
    dataDir(stage) {                                  // the patchscript's find_any: GAME_DATA, else *_Data, else Data
      const dirs = [...stage.dirs()].filter(d => d.split('/').length <= 6 && hasFiles(stage, d));
      const base = d => d.split('/').pop();
      const glob = g => new RegExp('^' + g.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.') + '$');
      for (const g of [env.GAME_DATA, '*_Data', 'Data'].filter(Boolean)) { const d = dirs.find(x => glob(g).test(base(x))); if (d) return d; }
      return null;
    },
    async config(answers) {
      const text = await port.read('setup/convert.json');
      if (!text) return null;
      const cfg = JSON.parse(text.split('{setup}').join('zip:setup'));
      for (const q of questions) applySet(cfg, q.set, answers[q.id] ?? q.default);
      return cfg;
    },
    async finish(stage, answers) {
      for (const q of questions) await stage.putParts(`.setup/answers/${q.id}`, [new TextEncoder().encode(`${answers[q.id] ?? q.default}\n`)]);
    },
  };
}

// The patcher's questions as the port's own file asks them (QUESTIONS.run: the Lua file run for the handheld
// the player describes), with where the setup puts each answer (the patchscript's echo lines). sets: an
// answer that the conversion done here takes ({ID: 'textures.quality'})
function asked(port, sets = {}) {
  const files = QUESTIONS_.answerFiles(port.script);
  return hh => QUESTIONS_.run(port.lua, QUESTIONS_.handheld({ ...hh, portdir: port.portdir }), port.qfile).map(q => ({
    ...q, default: q.options[0][0], file: files[q.id] || null, set: sets[q.id] || null,
    hint: sets[q.id] ? "The game is converted with this answer here, and the handheld's setup takes the same one."
      : files[q.id] ? `Asked here instead of on the handheld (saved as ports/${port.portdir}/${files[q.id]}).` : '' }));
}

function cuphead(port) {
  const questionsFor = port.lua ? asked(port, { TEXTURE_QUALITY: 'textures.quality' }) : null;
  const files = QUESTIONS_.answerFiles(port.script);
  let app = null, unity = null;
  return {
    name: 'cuphead', questions: [], questionsFor,
    dataDir(stage) {
      app = [...stage.dirs()].find(d => /(^|^[^/]+\/)Cuphead\.app$/.test(d)) || null;
      return app && hasFiles(stage, `${app}/Contents/Resources/Data`) ? `${app}/Contents/Resources/Data` : null;
    },
    async check(stage, data) {                          // the setup's own check: the macOS build 1.3.4 (Unity 2017.4.9f1)
      const head = new Uint8Array(await (await stage.file(`${data}/globalgamemanagers`)).slice(0, 4096).arrayBuffer());
      return new TextDecoder('latin1').decode(head).includes('2017.4.9f1');
    },
    async config(answers) {
      const cfg = JSON.parse(await port.read('setup/cuphead.json'));
      const q = answers.TEXTURE_QUALITY || 'fast';
      cfg.textures = Object.assign(cfg.textures || {}, { quality: q, guide: `zip:setup/astc_guide_${q}.bin` });
      return cfg;
    },
    async finish(stage, answers) {                      // patchscript step 1, done here
      if (!app) return;
      const D = `${app}/Contents/Resources/Data/`, R = `${app}/Contents/Resources/`, M = `${app}/Contents/Mono/etc/`;
      const dlc = stage.has(`${D}StreamingAssets/AssetBundles/atlas_map_common_dlc`);
      stage.rename(rel => rel.startsWith(D) ? `Cuphead_Data/${rel.slice(D.length)}` :
        rel.startsWith(M) ? `mono-etc/${rel.slice(M.length)}` :
        rel.startsWith(R) && /^goggame-[^/]*\.info$/.test(rel.slice(R.length)) ? rel.slice(R.length) :
        rel.startsWith(`${app}/`) ? null : rel);
      const enc = new TextEncoder();
      await stage.putParts('.setup/.step_game', [new Uint8Array(0)]);
      await stage.putParts('.setup/.texture_quality', [enc.encode(`${answers.TEXTURE_QUALITY || 'fast'}\n`)]);
      if (dlc) await stage.putParts('.setup/.dlc_files', [new Uint8Array(0)]);
      // the answers, as the patchscript writes them (echo) into the port folder: ../ = ports/<portdir>
      for (const [id, f] of Object.entries(files)) if (answers[id]) await stage.putParts(`../${f}`, [enc.encode(`${answers[id]}\n`)]);
      // step 2: Unity's player from the port's donor APK (as `pm player donor`)
      const m = port.script.match(/pm player donor (\S+) "\$P" --apk "\$GAMEDIR\/([^"]+)"/);
      const apk = m && await port.readBytes(m[2]);
      if (!apk) return;
      await PLAYER_.donor({ version: m[1], apks: [{ name: m[2].split('/').pop(), blob: new Blob([apk]) }],
        put: (rel, b) => stage.putParts(`.setup/player/${rel}`, [b]) });
      await stage.putParts('.setup/.step_player', [new Uint8Array(0)]);
      unity = m[1];
    },
    // steps 3-5, once every conversion step is done here: the packing (the patchscript's own android-data
    // line, PACK), then what step 5 installs. Throws Unsupported (nothing changed) where the page cannot
    async complete(stage, ctx) {
      if (!unity) throw new (ANDROID_().Unsupported)("no Unity player (the port's donor)");
      const r = await packPort(port, stage, { ...ctx, unity });
      install(port, stage, r);
      return r;
    },
  };
}

// ---- the packing and install of a port whose patchscript runs `pm android-data` (gamedata/ paths: staged files;
// setup/ ones: the port's own). ctx: {unity, lz, codepatch: {crcs, start()}, log, putStore(parts) -> Blob}
const ANDROID_ = () => (typeof ANDROID !== 'undefined' ? ANDROID : require('./android.js'));
const PACK_ = () => (typeof PACK !== 'undefined' ? PACK : require('./pack.js'));
async function packPort(port, stage, ctx) {
  const A = ANDROID_(), P = PACK_(), U = m => new A.Unsupported(m);
  const gd = p => { if (!p.startsWith('/gamedata/')) throw U(`android-data reads ${p} (not under gamedata/)`); return p.slice(10).replace(/\/+$/, ''); };
  const c = P.command(port.script, { GAMEDIR: '' }, f => f.startsWith('/gamedata/') && stage.has(f.slice(10)));
  if (!c) throw U('no `pm android-data` line in the patchscript');
  const o = P.options(c.words), M = o.multi;
  const src = gd(o['--src']), pkgDir = gd(o['--pkg']), parent = src.includes('/') ? src.slice(0, src.lastIndexOf('/')) : '';
  if (o['--game-managed'] && gd(o['--game-managed']) !== `${src}/Managed`) throw U("android-data --game-managed other than the data's own Managed");
  const data = stage.view(src), pkg = stage.view(pkgDir), gdv = stage.view(parent);
  const metc = stage.view(o['--mono-etc'] ? gd(o['--mono-etc']) : `${src}/.none`);
  const setupFile = async p => { const rel = p.replace(/^\/(?=setup\/)/, ''), b = rel.startsWith('setup/') && await port.readBytes(rel);
    if (!b) throw U(`android-data's ${p} (not a file of the port's setup)`); return b; };
  const managedFiles = [];
  for (const f of M['--managed-file'] || []) managedFiles.push({ name: f.split('/').pop(), bytes: await setupFile(f) });
  // --assembly-patch: the port's codepatch, as the page's WebAssembly build of it
  const assemblyPatch = {};
  let cp = null;
  for (const [name, cmd] of o.patch) {
    if (name !== 'Assembly-CSharp.dll') throw U(`an assembly patch of ${name}`);
    const args = P.codepatchArgs(cmd, port, ctx.codepatch ? ctx.codepatch.crcs : []);
    assemblyPatch[name] = async () => {
      cp = cp || await ctx.codepatch.start();
      const names = (await data.list('Managed')).filter(e => !e.dir && e.name.endsWith('.dll')).map(e => e.name).sort();
      const managed = [];
      for (const n of names) managed.push({ name: n, bytes: await data.read(`Managed/${n}`) });
      const r = cp.run(managed, args);
      ctx.log(`codepatch ${args.join(' ')}: exit ${r.rc}, ${r.dll.length} bytes`);
      if (r.rc !== 0 || !r.dll.length) throw new Error(`codepatch failed (exit ${r.rc}): ${r.log}`);
      return r.dll;
    };
  }
  const tree = new Map();
  const out = {
    put: async (dst, parts) => tree.set(dst, { size: parts.reduce((a, x) => a + x.length, 0), file: await ctx.putStore(parts) }),
    link: async (dst, v, rel) => { const f = await v.file(rel); tree.set(dst, { size: f.size, file: f }); },
  };
  const opt = { data, pkg, metc, gd: gdv, unity: ctx.unity, name: o['--name'], out, lz: ctx.lz, log: ctx.log,
    rootFiles: M['--root-file'] || [], managedFiles, assemblyPatch, dropMissing: M['--drop-missing-assembly'] || [],
    dllmap: (M['--dllmap'] || []).map(x => [x.slice(0, x.indexOf('=')), x.slice(x.indexOf('=') + 1)]),
    videoShader: !o.flags.has('--no-video-shader') };
  if (!opt.name) throw U('android-data without --name');
  if (o['--graphics-apis']) opt.graphicsApis = o['--graphics-apis'].split(',').map(Number);
  if (o['--blit']) opt.blit = o['--blit'];
  if (o['--mt-rendering']) opt.mtRendering = o['--mt-rendering'];
  if (o['--async-upload-buffer']) opt.asyncUploadBuffer = +o['--async-upload-buffer'];
  if (o['--rewired-joysticks']) opt.rewiredJoysticks = +o['--rewired-joysticks'];
  if (M['--beside']) opt.beside = M['--beside'];
  const r = await A.build(opt);
  const sizes = new Map([...tree].map(([k, v]) => [k, v.size]));
  const pk = A.pack({ tree: sizes, name: opt.name, pkgName: o['--package'] || `com.pmunity.${opt.name.toLowerCase()}`, versionCode: +(o['--version-code'] || 1),
    unity: ctx.unity, bid: A.buildId(r.ggm), loose: M['--loose-assets'] || (r.plugins.fmod ? ['*.bank'] : []), pluginMap: r.pluginMap });
  return { build: r, pack: pk, tree, vars: c.vars, line: c.line };
}

// step 5 as the patchscript does it after the packing: `cp "$P/<dir>/$f" "$GAMEDIR/<dir>/"` for each f of a
// `for f in ...` list, the packing's zip folder into the port folder, `touch "$GAMEDIR/<file>"` (with an
// optional `[ -f "<file>" ] &&`); then gamedata/ is emptied (the player's copy of the game is removed)
function install(port, stage, r) {
  const A = ANDROID_(), P = PACK_(), CORE_ = typeof CORE !== 'undefined' ? CORE : require('./core.js');
  const lines = port.script.split('\n').slice(r.line + 1), U = m => new A.Unsupported(m), vars = r.vars;
  const put = new Map();                      // ../<path> -> {size, file} | {size, chunks} | {stageRel}
  for (const [file, list] of Object.entries(r.pack.archives)) {
    const entries = list.map(([a, s]) => s instanceof Uint8Array ? { name: a, size: s.length, chunks: async function* () { if (s.length) yield s; } }
      : { name: a, size: r.tree.get(s).size, chunks: () => blobChunks(r.tree.get(s).file) });
    put.set(`../${file}`, { size: A.zipSize(entries), chunks: () => A.zipChunks(entries, CORE_.crc32) });
  }
  for (const [rel, s] of r.pack.zipFiles) put.set(`../${rel}`, s instanceof Uint8Array ? { size: s.length, file: new Blob([s]) } : r.tree.get(s));
  const has = f => f.startsWith('/gamedata/') ? stage.has(f.slice(10)) : put.has(`..${f}`);
  let zipped = false;
  for (let i = 0; i < lines.length; i++) {
    const l = lines[i].trim();
    if (/^for f in "\$W"\/android\/zip\/\*; do/.test(l)) { zipped = true; continue; }
    let m = /^for f in ([^;]+); do$/.exec(l);
    if (m) {
      const cp = /^cp "\$(\w+)\/([^"]*)\$f" "\$GAMEDIR\/([^"]*)"/.exec((lines[i + 1] || '').trim());
      if (!cp) throw U(`step 5's loop ${l}`);
      const from = P.words(`"$${cp[1]}"`, vars)[0];
      if (!from.startsWith('/gamedata/')) throw U(`step 5 copies from ${from}`);
      for (const f of P.words(m[1], vars)) {
        const rel = `${from.slice(10)}/${cp[2]}${f}`;
        if (!stage.has(rel)) throw U(`step 5: ${rel} is not there`);
        put.set(`../${cp[3]}${f.split('/').pop()}`, { stageRel: rel });
      }
      i++; continue;
    }
    m = /^(?:\[ -f ("[^"]+") \] && )?touch "\$GAMEDIR\/([^"$]+)"$/.exec(l);
    if (m && (!m[1] || has(P.words(m[1], vars)[0]))) put.set(`../${m[2]}`, { size: 0, file: new Blob([]) });
  }
  if (!zipped) throw U("step 5 does not move the packing's zip folder into the port folder");
  const kept = new Map();
  for (const [rel, e] of put) if (e.stageRel) kept.set(rel, stage.entries.get(e.stageRel));
  stage.rename(rel => rel.startsWith('../') && !put.has(rel) ? rel : null);       // gamedata/ emptied; earlier ../ files kept
  for (const [rel, e] of put) {
    if (e.stageRel) stage.entries.set(rel, kept.get(rel));
    else if (e.chunks) stage.putLazy(rel, e.size, e.chunks);
    else stage.putFile(rel, e.file, 0o644);
  }
}
async function* blobChunks(blob) {
  const rd = blob.stream().getReader();
  for (;;) { const { value, done } = await rd.read(); if (done) return; yield value; }
}

function profile(port) {
  if (port.standard) return standard(port);
  if (/Cuphead_Data/.test(port.script) && /texture_quality/.test(port.script)) return cuphead(port);
  return null;                                          // another setup: unpacking only
}
// the conversion plugins the port's setup passes (--plugin "$S/<file>": its setup folder), with the CRC-32 of
// each file as the port zip stores it (the page runs a plugin's step only if it knows that file: STEPS)
function plugins(port) {
  return [...new Set([...(port.script || '').matchAll(/--plugin\s+"?\$\{?S\}?\/([^"\s]+)/g)].map(m => `setup/${m[1]}`))]
    .map(file => { const e = port.entries.find(x => x.name === `${port.portdir}/${file}`); return { file, crc: e ? e.crc >>> 0 : null }; });
}
async function guideBytes(port, c) {                  // textures.guide: a file of the port's setup (zip:setup/...)
  if (!c.guide) return null;
  if (!c.guide.startsWith('zip:')) return null;
  return port.readBytes(c.guide.slice(4));
}
return { profile, guideBytes, luaQuestions, plugins };
})();
if (typeof module !== 'undefined') module.exports = PORTS;
