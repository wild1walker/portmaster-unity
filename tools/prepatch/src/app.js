// ---- the page
const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const mb = n => n >= 1 << 30 ? (n / (1 << 30)).toFixed(2) + ' GB' : Math.max(1, Math.round(n / (1 << 20))) + ' MB';
let port = null, sources = [], stop = false, busy = false, workParent = null;

function log(s) { const l = $('log'); l.hidden = false; l.textContent += s + '\n'; l.scrollTop = l.scrollHeight; }
function msg(where, text, cls = 'bad') { $(where).insertAdjacentHTML('beforeend', `<div class="msg ${cls}">${esc(text)}</div>`); }

async function loadPort(file) {
  $('port-info').innerHTML = '';
  try { port = await CORE.inspectPort(file); port.file = file; }
  catch (ex) { port = null; msg('port-info', ex.message); render(); return; }
  const i = port;
  const store = i.store.map(s => `<a href="${esc(s.gameurl)}" target="_blank" rel="noopener">${esc(s.name)}</a>`).join(', ');
  $('port-info').innerHTML = `<dl class="kv">
    <dt>Game</dt><dd><b>${esc(i.title)}</b>${i.porter.length ? ' · port by ' + esc(i.porter.join(', ')) : ''}</dd>
    <dt>Zip</dt><dd>${esc(i.zip)} → <code>ports/${esc(i.portdir)}</code></dd>
    <dt>Setup</dt><dd>${esc(i.route || 'none')}</dd>
    ${i.unity.length ? `<dt>Unity</dt><dd>${esc(i.unity.join(', '))}</dd>` : ''}
    ${i.gameSizeMb ? `<dt>Game size</dt><dd>about ${mb(i.gameSizeMb << 20)}</dd>` : ''}
    ${store ? `<dt>Store</dt><dd>${store}</dd>` : ''}
  </dl>` + i.problems.map(p => `<div class="msg">${esc(p)}</div>`).join('');
  $('game-need').innerHTML = `<p>The setup needs <b>${esc(i.gameWhat || 'the game')}</b>.</p>`
    + (i.inst ? `<div class="need">${esc(i.inst)}</div>` : i.gamedataTxt ? `<div class="need">${esc(i.gamedataTxt)}</div>` : '')
    + (i.appName ? `<p class="hint">This port's own setup looks for <code>gamedata/${esc(i.appName)}</code>: installers are unpacked into that folder.</p>` : '');
  const prof = PORTS.profile(port);
  if (!prof) $('questions').innerHTML = `<div class="msg">This port's setup is its own: the page unpacks the game into the zip; the handheld does the conversion.</div>`;
  else if (prof.questionsFor) {             // the port's own questions file, run for the handheld described here
    const sel = (id, label, list) => `<div class="q"><label for="${id}">${label}</label><select id="${id}" data-hh>${list.map(([v, t]) => `<option value="${v}">${esc(t)}</option>`).join('')}</select></div>`;
    $('questions').innerHTML = `<p class="hint">The setup's questions come from the port (<code>${esc(port.qfile)}</code>) and can depend on the handheld: say which one it goes on.</p>`
      + `<div class="hh">${sel('hh-screen', 'Its screen', SCREENS.map(s => [s, s ? s.replace('x', ' × ') : 'Not sure']))}`
      + `${sel('hh-ram', 'Its memory', RAMS.map(m => [m, m ? (m >= 1024 ? `${m / 1024} GB` : `${m} MB`) : 'Not sure']))}</div><div id="q-list"></div>`;
    document.querySelectorAll('[data-hh]').forEach(s => s.onchange = () => askQuestions(prof));
    askQuestions(prof);
  } else $('questions').innerHTML = questionsHtml(prof.questions);
  render();
}
const SCREENS = ['', '480x320', '640x480', '720x720', '854x480', '960x544', '960x720', '1024x768', '1280x720', '1280x960', '1920x1080', '1920x1152'];
const RAMS = ['', 512, 1024, 2048, 3072, 4096, 8192];
function questionsHtml(qs, keep = {}) {
  return qs.map(q => { const pick = q.options.some(o => o[0] === keep[q.id]) ? keep[q.id] : q.default;
    return `<div class="q"><label for="q-${esc(q.id)}">${esc(q.question)}</label>
      <select id="q-${esc(q.id)}" data-q="${esc(q.id)}">${q.options.map(o => `<option value="${esc(o[0])}" ${o[0] === pick ? 'selected' : ''}>${esc(o[1])}</option>`).join('')}</select>
      ${q.hint ? `<div class="hint">${esc(q.hint)}</div>` : ''}</div>`; }).join('');
}
function askQuestions(prof) {               // the questions again for the handheld picked, answers kept where still offered
  const keep = {};
  document.querySelectorAll('[data-q]').forEach(s => keep[s.dataset.q] = s.value);
  const [w, h] = ($('hh-screen').value || 'x').split('x').map(Number);
  let qs;
  try { qs = prof.questionsFor({ width: w || 0, height: h || 0, ramMb: +$('hh-ram').value || 0 }); }
  catch (ex) { $('q-list').innerHTML = `<div class="msg">The port's questions need more about the handheld (pick its screen and memory): ${esc(ex.message)}</div>`; return; }
  $('q-list').innerHTML = qs.length ? questionsHtml(qs, keep) : '<p class="hint">The setup asks nothing on this handheld.</p>';
}

function addSource(s) { sources = sources.filter(x => x.name !== s.name); sources.push(s); render(); }
function addFiles(files) {
  const byTop = new Map();
  for (const f of files) {
    const rel = f.webkitRelativePath || f._rel || '';
    if (rel.includes('/')) {                      // a file from a folder: grouped by its top folder
      const top = rel.split('/')[0];
      if (!byTop.has(top)) byTop.set(top, []);
      byTop.get(top).push({ path: rel, file: f });
      continue;
    }
    const l = f.name.toLowerCase();
    const kind = l.endsWith('.pkg') ? 'pkg' : (l.endsWith('.zip') || l.endsWith('.sh')) ? 'zip' : 'file';
    addSource({ name: f.name, kind, file: f, size: f.size, label: CORE.kindOf(f.name, false) });
  }
  for (const [top, fs] of byTop) addSource({ name: top, kind: 'folder', files: fs, size: fs.reduce((a, x) => a + x.file.size, 0),
    label: CORE.kindOf(top, true) + `, ${fs.length} files` });
}
// the browser's own storage may hold less than the game needs while it is unpacked and converted (about 2.5 times
// what was given: the unpacked files, the converted ones, the packed game): then a work folder on a disk
const needed = () => 2.5 * sources.reduce((a, s) => a + s.size, 0);
async function checkSpace() {
  let room = Infinity;
  try { const e = await navigator.storage.estimate(); room = e.quota - e.usage; } catch { /* no estimate */ }
  const short = sources.length && room < needed();
  $('work').hidden = !short && !workParent;
  $('work-msg').textContent = short ? `The browser lets this page keep about ${mb(room)}; unpacking and converting this game needs about ${mb(needed())}. ` +
    `Choose a work folder on a disk with that much free (what the page puts there is removed when it finishes).` : '';
  $('work-st').textContent = workParent ? `Work folder: ${workParent.name}` : '';
}
async function entryFiles(entry, out) {          // a dropped folder, read through the entries API
  if (entry.isFile) { const f = await new Promise((ok, no) => entry.file(ok, no)); f._rel = entry.fullPath.replace(/^\//, ''); out.push(f); return; }
  const r = entry.createReader();
  for (;;) { const batch = await new Promise((ok, no) => r.readEntries(ok, no)); if (!batch.length) break;
    for (const e of batch) await entryFiles(e, out); }
}

function render() {
  checkSpace();
  const ok = !!port && port.hasSetup;
  $('c-game').classList.toggle('off', !ok || busy);
  $('c-build').classList.toggle('off', !ok || !sources.length);
  $('c-port').classList.toggle('off', busy);
  $('b-build').disabled = busy || !ok || !sources.length;
  $('b-stop').disabled = !busy;
  $('games').innerHTML = sources.map(s => `<li><span class="name">${esc(s.name)}${s.kind === 'folder' ? '/' : ''}</span>
    <span class="tag">${esc(s.label)}</span><span class="tag">${mb(s.size)}</span>
    <button class="x" title="Remove" data-rm="${esc(s.name)}" ${busy ? 'disabled' : ''}>✕</button></li>`).join('');
}

async function sinkFor(name) {
  if (window.showSaveFilePicker) {
    const h = await showSaveFilePicker({ suggestedName: name, types: [{ description: 'Zip', accept: { 'application/zip': ['.zip'] } }] });
    const w = await h.createWritable();
    let pending = [], pendingSize = 0, start = 0, end = 0;
    const flush = async () => { if (!pendingSize) return; await w.write(new Blob(pending)); start += pendingSize; pending = []; pendingSize = 0; };
    return {
      async write(b) { pending.push(b); pendingSize += b.length; end += b.length; if (pendingSize >= 16 << 20) await flush(); },
      async rewrite(pos, b) {                    // a header already written: in place
        if (pos >= start) return;                // (still waiting to be written: the change is in it)
        await flush(); await w.write({ type: 'write', position: pos, data: b }); await w.write({ type: 'seek', position: end });
      },
      async close() { await flush(); await w.close(); },
      async abort() { try { await w.abort(); } catch { /* gone */ } },
      where: h.name,
    };
  }
  // no File System Access (Firefox, Safari): the zip is kept in memory, then downloaded
  const parts = [];
  return {
    async write(b) { parts.push(b); }, async rewrite() {}, async abort() { parts.length = 0; },   // (the same arrays: headers are patched in place)
    async close() { const url = URL.createObjectURL(new Blob(parts, { type: 'application/zip' }));
      const a = document.createElement('a'); a.href = url; a.download = name; a.click(); parts.length = 0; },
    where: name + ' (download)',
  };
}

// ---- the embedded pixel core, type trees and codepatch, and the worker pool
let wasmModule = null, PX = null;
const b64 = id => Uint8Array.from(atob(document.getElementById(id).textContent.trim()), c => c.charCodeAt(0));
async function embedded() {
  if (wasmModule) return;
  wasmModule = await WebAssembly.compile(b64('pmucore-wasm'));
  // the page's own instance: bundles' LZ4 blocks by lz4's own code (not in JavaScript)
  const px = PX = new PIXELS.Pixels(await PIXELS.instantiate(wasmModule), null);
  BUNDLE.setUnlz4((src, usize) => px.unlz4(src, usize));
  const tt = await new Response(new Blob([b64('typetrees-gz')]).stream().pipeThrough(new DecompressionStream('gzip'))).json();
  UNITY.setTypeTrees(tt);
}
const LZ = { unlz4: (s, n) => PX.unlz4(s, n), lz4: (s, l) => PX.lz4(s, l) };
// the port's codepatch for WebAssembly (codepatch.js), started the first time a packing needs it
const codepatchCtx = () => ({ crcs: JSON.parse($('codepatch-known').textContent).crcs,
  start: async () => CODEPATCH.start(await CODEPATCH.files(b64('codepatch-gz'))) });
const unsupported = ex => ex instanceof ANDROID.Unsupported || ex instanceof SHADERS.Unsupported;
function workerPool(n, guide) {                 // Web Workers running pixels.js serve() (pool.js)
  const src = document.getElementById('worker-src').textContent +
    '\nconst serveTask = PIXELS.serve((m, t) => self.postMessage(m, t)); self.onmessage = e => serveTask(e.data);';
  const url = URL.createObjectURL(new Blob([src], { type: 'text/javascript' }));
  const pool = POOL.create(n, () => { const w = new Worker(url);
    return { post: (m, t) => w.postMessage(m, t), listen: (f, e) => { w.onmessage = x => f(x.data); w.onerror = x => e(x.message); }, end: () => w.terminate() }; },
  { init: 1, module: wasmModule, guide });
  const close = pool.close;
  pool.close = () => { close(); URL.revokeObjectURL(url); };
  return pool;
}

async function run() {
  const name = port.zip;
  let sink;
  try { sink = await sinkFor(name); } catch (ex) { if (ex.name !== 'AbortError') msg('result', ex.message); return; }
  busy = true; stop = false; render();
  $('result').innerHTML = ''; $('log').textContent = ''; $('bar').hidden = false;
  const t0 = performance.now();
  const secs = () => Math.round((performance.now() - t0) / 1000);
  const phase = (what, d, t) => { $('bar').firstElementChild.style.width = (t ? 100 * d / t : 0).toFixed(1) + '%';
    $('st').textContent = `${what}${t ? ` · ${typeof t === 'number' && t > 1e5 ? `${mb(d)} of ${mb(t)}` : `${d} of ${t}`}` : ''} · ${secs()} s`; };
  if (!window.showSaveFilePicker) log('This browser cannot write files directly: the zip is built in memory (needs RAM for the whole zip). Chrome or Edge write it straight to disk.');
  let stage = null, pool = null;
  const stopped = () => { if (stop) throw new Error('stopped'); };
  try {
    let store;
    if (workParent) { store = await STAGE.pickedFolder(workParent); log(`work folder: ${workParent.name}`); }
    else try { store = await STAGE.opfs(); }
    catch { log("The browser's own storage is not available here: choose a work folder for the unpacked game."); store = await STAGE.pickedFolder(); }
    stage = new STAGE.Stage(store);
    // 1. unpack
    const total = sources.reduce((a, s) => a + s.size, 0);
    let done = 0;
    await CORE.stage({ port, sources, stage, log, tick: n => { done += n; phase('Unpacking', done, total); stopped(); } });
    // 2. convert the textures, where this port's setup is one the page knows
    const prof = PORTS.profile(port);
    const data = prof && prof.dataDir(stage);
    let conv = null;
    if (!prof) log("This port's setup is its own: the game is unpacked into the zip, the handheld converts it.");
    else if (!data) log('No Unity data folder (with globalgamemanagers) was found in what was unpacked: the handheld converts the game.');
    else if (prof.check && !(await prof.check(stage, data))) log("This copy is not the version the port's setup accepts: left to the handheld (it says why).");
    else {
      const answers = {};
      document.querySelectorAll('[data-q]').forEach(s => answers[s.dataset.q] = s.value);
      const cfg = await prof.config(answers);
      const steps = cfg && (cfg.steps || ['shaders', 'textures', 'settings', 'audio']);
      if (!cfg || !steps.includes('textures')) log('The port does not convert textures: nothing to do here.');
      else {
        await embedded();
        const c = TEXTURES.merged(cfg);
        const gb = await PORTS.guideBytes(port, c);
        const guide = gb ? TEXTURES.parseGuide(gb, c, log) : null;
        const n = Math.max(1, Math.min(16, (navigator.hardwareConcurrency || 4) - 1));
        log(`converting the textures of gamedata/${data} with ${n} workers${guide && guide.ok ? ' and the port\'s ASTC guide' : ''}`);
        pool = workerPool(n, guide);
        let k = 0;
        conv = await TEXTURES.run({ fs: stage.view(data), cfg, pool, log,
          onFile: async (rel, outputs, total) => { phase('Converting textures', ++k, total); stopped();
            if (outputs) for (const [r, v] of outputs) await stage.putParts(`${data}/${r}`, v.parts); } });
        pool.close(); pool = null;
        // the config's other steps the page can do (settings, audio, known plugins), on the converted files
        const state = { version: 1, done: { textures: conv.done }, scaled: conv.scaled };
        const put = (rel, parts) => { stopped(); return stage.putParts(`${data}/${rel}`, parts); };
        conv.left = await STEPS.runAll({ fs: stage.view(data), cfg, log, state, plugins: PORTS.plugins(port), put });
        // the shaders last (as the handheld runs them after the page's steps: the same files)
        if (conv.left.includes('shaders')) {
          phase('Converting shaders', 0, 0);
          try { await SHADERS.run({ fs: stage.view(data), cfg, log, put, journal: STEPS.journal(state), lz: LZ, bundle: BUNDLE, lz4hc: c => PX.lz4hc(c) });
            conv.left = conv.left.filter(s => s !== 'shaders'); }
          catch (ex) { if (!unsupported(ex)) throw ex; log(`shaders: left to the handheld (${ex.message})`); }
        }
        conv.steps = Object.keys(state.done);
        await stage.putParts(`${data}/.pmunity.json`, [new TextEncoder().encode(STEPS.pyJson(state))]);
        await prof.finish(stage, answers);
        // every step done here: the packing and install too, a zip ready to run
        if (prof.complete && !conv.left.length && !conv.skipped.length) {
          phase('Packing the game', 0, 0);
          let k = 0;
          const putStore = async parts => { stopped(); const h = await stage.store.write(`p${k++}`, (async function* () { for (const p of parts) yield p; })()); return stage.store.file(h); };
          try { conv.packed = await prof.complete(stage, { lz: LZ, log, codepatch: codepatchCtx(), putStore }); }
          catch (ex) { if (!unsupported(ex)) throw ex; log(`packing: left to the handheld (${ex.message})`); }
        }
      }
    }
    // 3. the zip
    done = 0;
    const ztotal = port.file.size + [...stage.entries.values()].reduce((a, e) => a + e.size, 0);
    const r = await CORE.writeZip({ port, stage, sink, compress: $('o-compress').checked, log,
      tick: n => { done += n; phase('Writing the zip', done, ztotal); stopped(); } });
    await stage.clear(); stage = null;
    log(`done: ${r.entries} entries, ${mb(r.size)}, ${secs()} s`);
    phase('Finished', 1, 1);
    r.conv = conv;
    const want = port.unity;
    const foundList = r.found.map(f => {
      const okv = !want.length || want.includes(f.version);
      return `<li><code>gamedata/${esc(f.path)}</code>: Unity ${esc(f.version)} ${okv ? '✓' : `<b>(the port needs ${esc(want.join(' or '))})</b>`}</li>`; }).join('');
    const bad = r.found.some(f => want.length && !want.includes(f.version));
    if (conv && conv.packed) {
      const a = conv.packed.pack.archives;
      $('result').innerHTML = `<div class="msg ok"><b>Saved:</b> ${esc(sink.where)} (${mb(r.size)}). <b>Ready to run:</b> the whole setup was done here
        (textures, ${esc(conv.steps.filter(s => s !== 'textures').join(', '))}, the code patch and the packing into ${esc(Object.keys(a).join(' and '))}).
        <br>Install it as any port (unzip into <code>ports/</code>, or PortMaster's install from zip): the game starts at once, with nothing to set up on the handheld.
        <br>This zip holds your copy of the game: keep it for your own devices.</div>`;
      busy = false; render(); return;
    }
    $('result').innerHTML = `<div class="msg ${!r.found.length || bad ? 'bad' : 'ok'}"><b>Saved:</b> ${esc(sink.where)} (${mb(r.size)}).
      ${r.found.length ? `<br>Game data found:<ul class="plain">${foundList}</ul>` :
        '<br>No Unity data folder was found in what was unpacked: the setup will look for it itself (archives it can unpack are in gamedata/ as they are).'}
      ${r.conv ? `<br>Textures converted here: ${r.conv.stats.textures} (${mb(r.conv.stats.in)} → ${mb(r.conv.stats.out)}) in ${r.conv.done.length} files${r.conv.skipped.length ? `; ${r.conv.skipped.length} asset bundles are left to the handheld` : ''}.
        <br>Conversion steps done here: ${esc(r.conv.steps.join(', '))}${r.conv.left.length ? `; left to the handheld: ${esc(r.conv.left.join(', '))}` : ''}.` : ''}
      <br>Install it as any port (unzip into <code>ports/</code>, or PortMaster's install from zip). On first launch the handheld runs the rest of the setup
      (shaders, settings, packing, and anything the port downloads such as Unity's player)${r.conv ? ', skipping what was done here' : ''}.
      <br>This zip holds your copy of the game: keep it for your own devices.</div>`;
  } catch (ex) {
    if (pool) pool.close();
    await sink.abort();
    if (stage) await stage.clear();
    log(`stopped: ${ex.message}`);
    msg('result', ex.message === 'stopped' ? 'Stopped.' : `Failed: ${ex.message}`);
  }
  busy = false; render();
}

function dropzone(zone, onDrop) {
  zone.ondragover = e => { e.preventDefault(); zone.classList.add('hover'); };
  zone.ondragleave = () => zone.classList.remove('hover');
  zone.ondrop = async e => { e.preventDefault(); zone.classList.remove('hover'); await onDrop(e.dataTransfer); };
}
dropzone($('drop-port'), dt => dt.files[0] && loadPort(dt.files[0]));
dropzone($('drop-game'), async dt => {
  const out = [];
  for (const it of [...dt.items]) { const en = it.webkitGetAsEntry && it.webkitGetAsEntry(); if (en) await entryFiles(en, out); else if (it.getAsFile()) out.push(it.getAsFile()); }
  addFiles(out);
});
$('b-port').onclick = () => $('f-port').click();
$('f-port').onchange = e => { e.target.files[0] && loadPort(e.target.files[0]); e.target.value = ''; };
$('b-files').onclick = () => $('f-files').click();
$('b-folder').onclick = () => $('f-folder').click();
$('f-files').onchange = e => { addFiles([...e.target.files]); e.target.value = ''; };
$('f-folder').onchange = e => { addFiles([...e.target.files]); e.target.value = ''; };
$('games').onclick = e => { const n = e.target.dataset.rm; if (n) { sources = sources.filter(s => s.name !== n); render(); } };
$('b-build').onclick = run;
$('b-work').onclick = async () => { try { workParent = await showDirectoryPicker({ mode: 'readwrite', id: 'pmuprepatch-work' }); } catch { /* cancelled */ } render(); };
$('b-stop').onclick = () => { stop = true; };
render();
