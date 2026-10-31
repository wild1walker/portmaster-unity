// pmunity's conversion steps `settings` and `audio`, and port plugins the page knows, as pmunity runs them on
// the handheld (settings.py, audio.py, a port's plugin file): the same objects changed the same way, the files
// saved as UnityPy saves them (unity.js), each finished file in the journal under its step so the handheld's
// setup skips it. fs: the data folder (list, read: a later step reads what an earlier one wrote);
// put(rel, parts): a converted file.
'use strict';
const STEPS = (() => {
const U = typeof UNITY !== 'undefined' ? UNITY : require('./unity.js');
const pyRound = typeof TEXTURES !== 'undefined' ? TEXTURES.pyRound : require('./textures.js').pyRound;
const C = { InputManager: 13, AudioManager: 11, AudioClip: 83, AnimationClip: 74, BuildSettings: 141, PreloadData: 150 };

async function files(fs, test) {            // job.list('', predicate): the folder's own files, sorted (Python order)
  return (await fs.list('')).filter(e => !e.dir && test(e.name)).map(e => e.name).sort(cmpStr);
}
const cmpStr = (a, b) => a < b ? -1 : a > b ? 1 : 0;
const load = async (fs, rel) => U.parseSerialized(await fs.read(rel), rel);
const objects = (sf, cls) => sf.objects.filter(o => cls === undefined || o.classId === cls);

// ---- settings (settings.py)
const SETTINGS = { graphics_apis: null, mouse_events: true, trim_joystick_axes: false, audio_sample_rate: 0, real_voices: 0 };
async function settings({ fs, cfg, put, journal, log }) {
  const rel = 'globalgamemanagers';
  if (journal.isDone('settings', rel)) return;
  const c = { ...SETTINGS, ...(cfg.settings || {}) };
  const sf = await load(fs, rel), changed = new Map(), done = [];
  for (const o of sf.objects) {
    if (o.classId === C.BuildSettings) {
      const t = U.readObject(sf, o);
      if (c.graphics_apis !== null) { t.m_GraphicsAPIs = [...c.graphics_apis]; done.push(`graphics APIs [${c.graphics_apis.join(', ')}]`); }
      if (!c.mouse_events && t.usesOnMouseEvents) { t.usesOnMouseEvents = false; done.push('mouse events off'); }
      changed.set(String(o.pathId), U.writeObject(sf, o, t));
    } else if (o.classId === C.InputManager && c.trim_joystick_axes) {
      const t = U.readObject(sf, o);
      const kept = t.m_Axes.filter(a => !/^Joy\d+Axis\d+$/.test(a.m_Name));
      if (kept.length !== t.m_Axes.length) {
        done.push(`input axes ${t.m_Axes.length} -> ${kept.length}`);
        t.m_Axes = kept;
        changed.set(String(o.pathId), U.writeObject(sf, o, t));
      }
    } else if (o.classId === C.AudioManager && c.audio_sample_rate) {
      const t = U.readObject(sf, o);
      t.m_SampleRate = c.audio_sample_rate;
      if (c.real_voices) t.m_RealVoiceCount = c.real_voices;
      changed.set(String(o.pathId), U.writeObject(sf, o, t));
      done.push(`audio ${c.audio_sample_rate} Hz`);
    }
  }
  await put(rel, U.save(sf, changed).parts);
  journal.markDone('settings', rel);
  log('settings: ' + (done.join(', ') || 'nothing to change'));
}

// ---- audio (audio.py)
const AUDIO = { pcm_budget_mb: 16, compressed_min_s: 2.0, resident_scenes: 2 };
const DECOMPRESS_ON_LOAD = 0, COMPRESSED_IN_MEMORY = 1, VORBIS = 1;
const sceneOf = rel => { const m = /^sharedassets(\d+)\.assets$/.exec(rel); return m ? parseInt(m[1], 10) : null; };
const key = (rel, pid) => `${rel}\u0000${pid}`;
const unkey = k => { const i = k.indexOf('\u0000'); return [k.slice(0, i), BigInt(k.slice(i + 1))]; };
const cmpKey = (a, b) => { const [ra, pa] = unkey(a), [rb, pb] = unkey(b); return cmpStr(ra, rb) || (pa < pb ? -1 : pa > pb ? 1 : 0); };

// plan(clips: Map key -> clip, scenes: Map scene -> Set of keys, budget bytes | null, minS, resident) -> {sw, left}
function plan(clips, scenes, budget, minS, resident = 2) {
  const pcm = k => clips.get(k).pcm;
  const switchable = k => { const c = clips.get(k); return c.load === DECOMPRESS_ON_LOAD && c.fmt === VORBIS && c.length >= minS; };
  const used = new Set(); for (const s of scenes.values()) for (const k of s) used.add(k);
  const sw = new Set([...clips.keys()].filter(k => switchable(k) && (budget === null || !used.has(k))));
  const left = new Map();
  if (budget === null) return { sw, left };
  const base = new Set(); for (const [i, s] of scenes) if (i < resident) for (const k of s) base.add(k);
  for (const i of [...scenes.keys()].sort((a, b) => a - b)) {
    const held = [...new Set([...scenes.get(i), ...base])].filter(k => clips.get(k).load === DECOMPRESS_ON_LOAD && !sw.has(k));
    let total = held.reduce((a, k) => a + pcm(k), 0);
    const late = k => i >= resident && base.has(k) ? 1 : 0;
    const order = held.filter(switchable).sort((a, b) => late(a) - late(b) || pcm(b) - pcm(a) || cmpKey(a, b));
    for (const k of order) { if (total <= budget) break; sw.add(k); total -= pcm(k); }
    left.set(i, total);
  }
  return { sw, left };
}
async function survey(fs, list) {
  const clips = new Map(), scenes = new Map(), refs = new Map();
  for (const rel of list) {
    const sf = await load(fs, rel), mine = new Set(), r = [];
    const ext = (sf.externals || []).map(p => p.slice(p.lastIndexOf('/') + 1));
    for (const o of sf.objects) {
      if (o.classId === C.AudioClip) {
        const t = U.readObject(sf, o);
        const c = { load: t.m_LoadType, fmt: t.m_CompressionFormat, length: t.m_Length, rate: t.m_Frequency, channels: t.m_Channels };
        c.pcm = pyRound(c.length * c.rate) * c.channels * 2;
        const k = key(rel, o.pathId);
        clips.set(k, c); mine.add(k);
      } else if (o.classId === C.PreloadData) {
        for (const p of U.readObject(sf, o).m_Assets || []) {
          const fid = p.m_FileID;
          if (fid >= 0 && fid <= ext.length) r.push(key(fid === 0 ? rel : ext[fid - 1], p.m_PathID));
        }
      }
    }
    const n = sceneOf(rel);
    if (n !== null) { scenes.set(n, mine); refs.set(n, r); }
  }
  for (const [n, r] of refs) for (const k of r) if (clips.has(k)) scenes.get(n).add(k);
  return { clips, scenes };
}
async function audio({ fs, cfg, put, journal, log }) {
  const c = { ...AUDIO, ...(cfg.audio || {}) };
  const ccfg = { pcm_budget_mb: c.pcm_budget_mb, compressed_min_s: c.compressed_min_s, resident_scenes: c.resident_scenes };
  const list = await files(fs, n => n.endsWith('.assets'));
  let st = journal.state.audio;
  if (!st) {
    const { clips, scenes } = await survey(fs, list);
    const budget = c.pcm_budget_mb === null ? null : Math.trunc(c.pcm_budget_mb * 2 ** 20);
    const { sw, left } = plan(clips, scenes, budget, c.compressed_min_s, c.resident_scenes);
    const kept = [...clips].filter(([k, x]) => x.load === DECOMPRESS_ON_LOAD && x.fmt === VORBIS && !sw.has(k)).map(([k]) => k);
    const byFile = {};
    for (const k of sw) { const [rel, pid] = unkey(k); (byFile[rel] = byFile[rel] || []).push(pid); }
    for (const rel in byFile) byFile[rel].sort((a, b) => a < b ? -1 : a > b ? 1 : 0);
    let top = null; for (const [i, v] of left) if (!top || v > top[1]) top = [i, v];
    const mib = ks => ([...ks].reduce((a, k) => a + clips.get(k).pcm, 0) / 2 ** 20).toFixed(1);
    st = { cfg: ccfg, switch: byFile,
      summary: `${sw.size} clips kept compressed in memory (${mib(sw)} MiB of PCM), ${kept.length} decompressed on load as the game has them (${mib(kept)} MiB); ` +
        (top ? `budget ${c.pcm_budget_mb} MiB a scene, the most a scene holds ${(top[1] / 2 ** 20).toFixed(1)} MiB (scene ${top[0]})`
          : `every clip of ${c.compressed_min_s} s or longer`) };
    journal.state.audio = st;
  }
  for (const rel of list) {
    const pids = new Set((st.switch[rel] || []).map(String));
    if (!pids.size || journal.isDone('audio', rel)) continue;
    const sf = await load(fs, rel), changed = new Map();
    for (const o of objects(sf, C.AudioClip)) {
      if (!pids.has(String(o.pathId))) continue;
      const t = U.readObject(sf, o);
      if (t.m_LoadType === DECOMPRESS_ON_LOAD) { t.m_LoadType = COMPRESSED_IN_MEMORY; changed.set(String(o.pathId), U.writeObject(sf, o, t)); }
    }
    await put(rel, U.save(sf, changed).parts);
    journal.markDone('audio', rel);
  }
  log('audio: ' + st.summary);
}

// ---- port plugins (a port's own Python step, --plugin): done here only for a file the page knows, by its
// CRC-32 (a changed plugin is left to the handheld, which runs the port's file)
// Cuphead's setup/animclips.py: animclips.txt, what each animation clip animates
const RANK = { S: 0, H: 1, '-': 2 };
async function animclips({ fs, put, journal, log }) {
  const OUT = 'animclips.txt';
  if (journal.isDone('animclips', OUT)) return;
  const clips = new Map();
  let n = 0;
  for (const rel of await files(fs, name => /^(sharedassets\d+|resources)\.assets$/.test(name))) {
    const sf = await load(fs, rel);
    for (const o of objects(sf, C.AnimationClip)) {
      const t = U.readObject(sf, o);
      const binds = ((t.m_ClipBindingConstant || {}).genericBindings) || [];
      const types = new Set(binds.map(b => b.typeID));
      const events = (t.m_Events || []).length;
      const only = allowed => [...types].every(x => allowed.includes(x));
      const flag = events === 0 && only([212]) ? 'S' : only([212, 4]) ? 'H' : '-';
      const name = t.m_Name || '';
      if (!clips.has(name)) clips.set(name, [flag, new Set()]);
      const c = clips.get(name);
      if (RANK[flag] > RANK[c[0]]) c[0] = flag;
      for (const b of binds) if (b.typeID === 4) c[1].add(Number(b.path || 0));
      n++;
    }
  }
  const names = [...clips.keys()].filter(Boolean).sort(cmpStr);
  const lines = names.map(k => { const [f, ps] = clips.get(k); return `${k}\t${f}\t${[...ps].sort((a, b) => a - b).join(',')}`; });
  await put(OUT, [new TextEncoder().encode(lines.join('\n') + '\n')]);
  journal.markDone('animclips', OUT);
  const count = f => [...clips.values()].filter(c => c[0] === f).length;
  log(`animclips: ${n} clips, ${lines.length} names (S ${count('S')}, H ${count('H')}, other ${count('-')})`);
}
const BUILTIN = { settings, audio };
// the journal (.pmunity.json): {version, done: {step: [rel]}, scaled, <step state>}, as job.py keeps it
function journal(state) {
  state.done = state.done || {}; state.scaled = state.scaled || {};
  return { state, isDone: (s, rel) => (state.done[s] || []).includes(rel), markDone: (s, rel) => (state.done[s] = state.done[s] || []).push(rel) };
}
function pyJson(x) {                        // json.dump's default separators; 64-bit ids exact
  if (typeof x === 'bigint') return x.toString();
  if (Array.isArray(x)) return `[${x.map(pyJson).join(', ')}]`;
  if (x && typeof x === 'object') return `{${Object.keys(x).map(k => `${JSON.stringify(k)}: ${pyJson(x[k])}`).join(', ')}}`;
  return JSON.stringify(x);
}
// port plugins by the CRC-32 of their file (the port zip's own entry CRC)
const KNOWN_PLUGINS = { 0x969870be: { step: 'animclips', run: animclips, what: "Cuphead's setup/animclips.py (release 2026.10.29)" } };

// the config's steps after `textures` (done before this) that the page can do, in the config's order:
// settings and audio, and the steps of plugins it knows; the rest (shaders, unknown plugins) is left to the
// handheld, whose setup skips what the journal lists. plugins: [{file, crc}] the port's setup passes.
// Returns the steps left to the handheld.
async function runAll({ fs, cfg, put, log, state, plugins = [] }) {
  const j = journal(state), byStep = {}, left = [];
  for (const p of plugins) { const k = KNOWN_PLUGINS[p.crc]; if (k) byStep[k.step] = k; else log(`plugin ${p.file}: not one the page knows (left to the handheld)`); }
  for (const s of cfg.steps || ['shaders', 'textures', 'settings', 'audio']) {
    if (s === 'textures') continue;
    const run = BUILTIN[s] || (byStep[s] && byStep[s].run);
    if (!run) { left.push(s); continue; }
    await run({ fs, cfg, put, journal: j, log });
  }
  return left;
}
return { settings, audio, animclips, plan, BUILTIN, KNOWN_PLUGINS, journal, pyJson, runAll };
})();
if (typeof module !== 'undefined') module.exports = STEPS;
