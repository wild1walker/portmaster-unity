// pmunity's textures step (convert/pmunity/textures.py) for one game data folder, in the browser: which
// files and textures, what each becomes (choose), the new .resS streams, the sprites and atlases that
// follow scaled textures, and the journal entries the handheld's own converter then skips. The pixel
// work goes to a pool of workers (pixels.js). UnityFS bundles are left to the handheld (not journaled).
'use strict';
const TEXTURES = (() => {
const U = typeof UNITY !== 'undefined' ? UNITY : require('./unity.js');
const BUN = typeof BUNDLE !== 'undefined' ? BUNDLE : require('./bundle.js');

const DEFAULTS = { scale: 1.0, block: [4, 4], quality: 'medium', mode: 'guided', guided_max_err: 64, guided_line: 128,
  guided_near: 1024, sharp_names: '', scale_sharp: 1.0, sharp_block: [4, 4], keep_max: 64, min_side: 16, min_convert: 4,
  mips: 'keep', inline: true, files: '.', threads: 0, pipeline: 'auto', prepare_split: true, solo_mb: 128, strips: true,
  guide: '', inline_streams: false, spill_mb: 64 };
const CONVERTIBLE = { 3: false, 4: true, 5: true, 10: false, 12: true, 14: true, 25: true, 26: false, 27: false, 28: false, 29: true };
const SEED = { 10: false, 12: true };                    // DXT1, DXT5: their blocks seed guided encoding
const BLOCK_BYTES = { 10: 8, 12: 16, 25: 16, 26: 8, 27: 16 };
const PIXEL_BYTES = { 3: 3, 4: 4, 5: 4, 14: 4 };
const OTHER_STREAMED = new Set([43, 89, 188, 187, 117]);   // Mesh, Cubemap, CubemapArray, Texture2DArray, Texture3D
const STRIP_MIN_PX = 2048 * 2048;
const ASTC_BLOCKS = ['4x4', '5x5', '6x6', '8x8', '10x10', '12x12'];
// the settings a guide's blocks depend on (astc_guide.KEYS)
const GUIDE_KEYS = ['scale', 'scale_sharp', 'sharp_names', 'block', 'sharp_block', 'keep_max', 'min_side', 'min_convert',
  'mips', 'mode', 'guided_max_err', 'guided_line', 'guided_near'];

const PROF = {};                                         // main-thread time per part (ms), for profiling
async function timed(name, fn) { const t0 = performance.now(); try { return await fn(); } finally { PROF[name] = (PROF[name] || 0) + performance.now() - t0; } }
function pyRound(x) {                                    // Python's round(): halves to even
  const f = Math.floor(x), d = x - f;
  return d > 0.5 ? f + 1 : d < 0.5 ? f : (f % 2 === 0 ? f : f + 1);
}
const bitLength = n => n > 0 ? 32 - Math.clz32(n) : 0;

function choose(t, c, sharpRx) {                         // textures.py choose
  const w = t.m_Width, h = t.m_Height, fmt = t.m_TextureFormat;
  if (!(fmt in CONVERTIBLE)) return null;
  if (t.m_IsReadable) return null;
  if ((t.m_TextureDimension ?? 2) !== 2 || (t.m_ImageCount ?? 1) !== 1) return null;
  if (Math.min(w, h) < c.min_convert) return null;
  const sharp = !!(sharpRx && sharpRx.test(t.m_Name));
  let s = sharp ? c.scale_sharp : c.scale;
  if (Math.max(w, h) <= c.keep_max || Math.min(w, h) * s < c.min_side) s = 1.0;
  const nw = Math.max(1, pyRound(w * s)), nh = Math.max(1, pyRound(h * s));
  let mips = c.mips === 'keep' ? (t.m_MipCount ?? 1) : 1;
  if (mips > 1) mips = Math.min(mips, bitLength(Math.max(nw, nh)));
  return { block: sharp ? c.sharp_block : c.block, width: nw, height: nh, alpha: CONVERTIBLE[fmt], mips };
}
function astcFormatId(block, alpha, unity) {             // textures.py astc_format_id
  if (unity && parseInt(unity.split('.')[0], 10) >= 2019) alpha = false;
  const i = ASTC_BLOCKS.indexOf(`${block[0]}x${block[1]}`);
  if (i < 0) throw new Error(`ASTC block ${block} is not a Unity texture format`);
  return (alpha ? 54 : 48) + i;
}
function firstLevelBytes(fmt, w, h) {
  if (BLOCK_BYTES[fmt]) return Math.ceil(w / 4) * Math.ceil(h / 4) * BLOCK_BYTES[fmt];
  return PIXEL_BYTES[fmt] ? w * h * PIXEL_BYTES[fmt] : 0;
}
function stripable(fmt, w, h, raw) {                     // strip_blocks would cut it in rows (else None)
  if (fmt === 28 || fmt === 29) return false;            // crunched
  const bh = BLOCK_BYTES[fmt] ? 4 : 1;
  const units = (BLOCK_BYTES[fmt] ? Math.ceil(h / 4) * 4 : h) / bh;
  return units > 0 && raw.length % units === 0;
}
const isUnityFS = head => head.length >= 8 && String.fromCharCode(...head.subarray(0, 8)) === 'UnityFS\0';

class StreamFile {                                       // textures.py StreamFile: 16-byte aligned blobs
  constructor() { this.parts = []; this.size = 0; }
  add(blob) {
    const pad = (16 - this.size % 16) % 16;
    if (pad) this.parts.push(new Uint8Array(pad));
    const off = this.size + pad;
    this.parts.push(blob);
    this.size = off + blob.length;
    return off;
  }
}

function merged(cfg) { return Object.assign({}, DEFAULTS, cfg.textures || {}); }
function canon(x) {                                     // JSON with sorted keys at every level (dict equality)
  if (Array.isArray(x)) return `[${x.map(canon).join(',')}]`;
  if (x && typeof x === 'object') return `{${Object.keys(x).sort().map(k => `${JSON.stringify(k)}:${canon(x[k])}`).join(',')}}`;
  return JSON.stringify(x === undefined ? null : x);
}
const GUIDE_TIERS_PY = [['fastest', { tune_partition_count_limit: 1 }], ['fast', { tune_partition_count_limit: 1 }], ['fastest', {}],
  ['fast', { tune_partition_count_limit: 2, tune_2partition_index_limit: 8 }], ['fast', { tune_partition_count_limit: 2 }],
  ['medium', { tune_partition_count_limit: 1 }], ['fast', {}], ['medium', { tune_partition_count_limit: 2, tune_2partition_index_limit: 16 }],
  ['medium', { tune_partition_count_limit: 2 }], ['medium', {}], ['thorough', {}]];
function guideOk(head, c) {                              // astc_guide.Reader: made for these settings and tiers?
  if (!head || head.version !== 1) return false;
  const mine = {}; for (const k of GUIDE_KEYS) mine[k] = c[k] === undefined ? null : c[k];
  return canon(head.settings) === canon(mine) && canon(head.tiers) === canon(GUIDE_TIERS_PY);
}
function parseGuide(bytes, c, log) {                     // the guide file -> {ok, index, data} for the workers
  const magic = 'PMUNITY-ASTC-GUIDE\n';
  if (String.fromCharCode(...bytes.subarray(0, magic.length)) !== magic) { log('textures: the ASTC guide is not one: not used'); return null; }
  const n = new DataView(bytes.buffer, bytes.byteOffset).getUint32(magic.length, true);
  const head = JSON.parse(new TextDecoder().decode(bytes.subarray(magic.length + 4, magic.length + 4 + n)));
  const ok = guideOk(head, c);
  if (!ok) log('textures: the ASTC guide was made for other texture settings: not used');
  return { ok, index: head.index, data: bytes.subarray(magic.length + 4 + n) };
}

// run(): fs {list(dir) -> [{name, dir}], size(rel), read(rel) -> bytes, slice(rel, off, n) -> bytes}
async function run({ fs, cfg, pool, log, onFile, only, parallel, ...opts }) {
  const c = merged(cfg);
  const sharpRx = c.sharp_names ? new RegExp(c.sharp_names, 'i') : null;
  const filesRx = new RegExp(c.files);
  if (c.mode !== 'guided') throw new Error(`textures.mode ${c.mode}: only guided is done here`);
  const stats = { textures: 0, sprites: 0, atlas: 0, in: 0, out: 0 };
  const done = [], scaled = {}, outputs = new Map(), skipped = [];
  // texture_files
  let files = (await fs.list('')).filter(e => !e.dir && (e.name.endsWith('.assets') || e.name === 'globalgamemanagers' || /^level\d+$/.test(e.name)))
    .map(e => e.name).sort();
  const walk = async dir => {
    const entries = await fs.list(dir);
    for (const e of entries.filter(e => !e.dir).sort((a, b) => a.name < b.name ? -1 : 1)) {
      const rel = `${dir}/${e.name}`;
      if (isUnityFS(await fs.slice(rel, 0, 8))) files.push(rel);
    }
    for (const e of entries.filter(e => e.dir)) await walk(`${dir}/${e.name}`);
  };
  if ((await fs.list('')).some(e => e.dir && e.name === 'StreamingAssets')) await walk('StreamingAssets');
  files = files.filter(r => filesRx.test(r));
  if (only) files = files.filter(r => new RegExp(only).test(r));   // (convert --only)
  // several files at once (each on its own: the same bytes as one after the other), finished in their order:
  // the workers stay busy while a file waits for its biggest texture
  const one = async rel => {
    const st = { textures: 0, sprites: 0, atlas: 0, in: 0, out: 0 };
    if (isUnityFS(await fs.slice(rel, 0, 8))) {
      const b = await convertBundle(rel, await fs.file(rel), { c, sharpRx, pool, stats: st, log });
      return { st, r: b.outputs ? b : null, left: b.left, asIs: b.asIs };
    }
    return { st, r: await convertFile(rel, await fs.read(rel), { fs, c, sharpRx, pool, stats: st, log }) };
  };
  const inFlight = [];
  const finish = async () => {
    const { rel, i, job } = inFlight.shift();
    const { st, r, left, asIs } = await job;
    if (left) { skipped.push(rel); log(`textures: ${rel}: ${left}`); if (onFile) await onFile(rel, null, files.length); return; }   // (not journaled: the handheld does it)
    if (asIs) log(`textures: ${rel}: left as it is (${asIs})`);
    for (const k in stats) stats[k] += st[k];
    if (r) {
      if (!onFile) for (const [k, v] of r.outputs) outputs.set(k, v);
      if (Object.keys(r.sizes).length) scaled[rel] = r.sizes;
      log(`textures: ${rel}: ${Math.floor(st.in / 1048576)} -> ${Math.floor(st.out / 1048576)} MB (${i + 1}/${files.length})`);
    }
    done.push(rel);
    if (onFile) await onFile(rel, r ? r.outputs : null, files.length);
  };
  // (at most `parallel` files, and while their sizes stay within `budget` bytes: each is held in memory)
  const maxFiles = parallel || 8, budget = (opts && opts.budget) || 768 << 20;
  const held = () => inFlight.reduce((a, f) => a + f.size, 0);
  for (let i = 0; i < files.length; i++) {
    const size = fs.size ? await fs.size(files[i]) : 0;
    while (inFlight.length && (inFlight.length >= maxFiles || held() + size > budget)) await finish();
    inFlight.push({ rel: files[i], i, size, job: one(files[i]) });
    inFlight[inFlight.length - 1].job.catch(() => {});      // (rejections are handled in finish)
  }
  while (inFlight.length) await finish();
  const MB = n => Math.floor(n / 1048576);
  log(`textures: ${stats.textures} converted (${MB(stats.in)} -> ${MB(stats.out)} MB), ${stats.atlas} atlas entries and ` +
      `${stats.sprites} sprites follow their scaled textures` + (skipped.length ? `; ${skipped.length} bundles left to the handheld` : ''));
  return { done, scaled, outputs, skipped, stats };
}

async function convertFile(rel, bytes, { fs, c, sharpRx, pool, stats, log }) {
  const sf = U.parseSerialized(bytes, rel);
  const dir = rel.includes('/') ? rel.slice(0, rel.lastIndexOf('/')) : '';
  const join = n => dir ? `${dir}/${n}` : n;
  const readStream = (path, off, size) => fs.slice(join(path.split('/').pop()), off, size);
  const r = await convertObjects(sf, rel, readStream, { c, sharpRx, pool, stats });
  if (!r) return null;
  const outputs = new Map();
  outputs.set(rel, U.save(sf, r.changed));
  if (r.streamName) outputs.set(join(r.streamName.split('/').pop()), { parts: r.stream.parts, size: r.stream.size });
  return { outputs, sizes: r.sizes };
}

// a bundle (Loaded on a UnityFS file, then write_to): one serialized file, its streams read from the
// bundle's nodes; with inline_streams the textures' pixels go into the objects and unread .resS nodes go
async function convertBundle(rel, file, { c, sharpRx, pool, stats, log }) {
  const b = await timed('bundle open', () => BUN.open(file));
  if (!b) return { left: 'LZMA or encrypted: left to the handheld' };
  const sfs = BUN.serializedNodes(b);
  if (sfs.length !== 1) return { asIs: `expected one serialized file in the bundle, found ${sfs.length}` };
  const sf = await timed('bundle node', async () => U.parseSerialized(await b.node(sfs[0].name), sfs[0].name));
  const readStream = async (path, off, size) => { const n = await timed('bundle node', () => b.node(path.split('/').pop())); return n.subarray(off, off + size); };
  const r = await convertObjects(sf, rel, readStream, { c, sharpRx, pool, stats });
  if (!r) return { unchanged: true };
  const res = new Map();                              // changed nodes: the new stream
  if (r.streamName) res.set(r.streamName.split('/').pop(), concat(r.stream.parts, r.stream.size));
  const node = async name => res.get(name) || b.node(name);
  let dropped = new Set();
  if (c.inline_streams) {                             // android_data.inline_texture_streams, on the current objects
    const still = new Set();
    const current = o => r.changed.has(String(o.pathId)) ? U.readObjectBytes(sf, o, r.changed.get(String(o.pathId))) : U.readObject(sf, o);
    for (const o of sf.objects) {
      if (o.classId === 43) { const sd = current(o).m_StreamData; if (sd && sd.path && sd.size) still.add(sd.path.split('/').pop()); continue; }
      if (![28, 89, 187, 117].includes(o.classId)) continue;
      const t = current(o), sd = t.m_StreamData;
      if (!sd || !sd.path) continue;
      if (!('image data' in t)) { still.add(sd.path.split('/').pop()); continue; }
      const n = await node(sd.path.split('/').pop());
      const data = n.slice(Number(sd.offset), Number(sd.offset) + sd.size);
      if (data.length !== sd.size) throw new Error(`${t.m_Name}: its stream gave ${data.length} of ${sd.size} bytes`);
      t['image data'] = data;
      t.m_StreamData = { offset: 0, size: 0, path: '' };
      r.changed.set(String(o.pathId), U.writeObject(sf, o, t));
    }
    dropped = new Set(b.nodes.filter(n => n.name.endsWith('.resS') && !still.has(n.name)).map(n => n.name));
  }
  const saved = await timed('save', () => U.save(sf, r.changed));
  const nodes = [];
  for (const n of b.nodes) {
    if (dropped.has(n.name)) continue;
    nodes.push({ name: n.name, flags: n.flags, parts: n === sfs[0] ? saved.parts : [await node(n.name)] });
  }
  const out = await timed('bundle write (lz4hc)', () => BUN.write(b, nodes, chunks => pool.lz4hc(chunks)));
  return { outputs: new Map([[rel, out]]), sizes: r.sizes };
}
const concat = (parts, size) => { const o = new Uint8Array(size); let p = 0; for (const x of parts) { o.set(x, p); p += x.length; } return o; };

async function convertObjects(sf, rel, readStream, { c, sharpRx, pool, stats }) {
  const items = await timed('read textures', () => sf.objects.filter(o => o.classId === 28).map(o => [o, U.readObject(sf, o)]));
  const changed = new Map(), sizes = {};
  let streamName = null, converted = 0;
  const jobs = [];                                        // [o, t, sd, streamed, ch, promise | null(copy)]
  for (const [o, t] of items) {
    const sd = t.m_StreamData || {};
    const streamed = !!sd.size;
    if (streamed) {
      if (streamName !== null && streamName !== sd.path) throw new Error(`${rel}: textures stream from more than one file`);
      streamName = sd.path;
    } else if (!c.inline || !(t['image data'] && t['image data'].length)) continue;
    const ch = choose(t, c, sharpRx);
    if (!ch) { if (streamed) jobs.push([o, t, sd, true, null, null]); continue; }
    const fmt = t.m_TextureFormat, w = t.m_Width, h = t.m_Height;
    await pool.ready();                                   // (a few textures in flight, not the whole file)
    const raw = streamed ? await readStream(sd.path, Number(sd.offset), sd.size) : t['image data'];
    const same = ch.width === w && ch.height === h;
    const strip = c.strips && ch.mips === 1 && (t.m_MipCount ?? 1) === 1 && same && ch.width * ch.height >= STRIP_MIN_PX && stripable(fmt, w, h, raw);
    let seed = null;
    if (fmt in SEED && same) {                            // dxt_seed (the first level's blocks)
      const n = firstLevelBytes(fmt, w, h);
      if (raw.length >= n) seed = { dxt: raw.slice(0, n), dxt5: SEED[fmt] };
    }
    const task = { fmt, raw, w, h, vint: sf.vint, width: ch.width, height: ch.height, mips: ch.mips, block: ch.block,
      c: { quality: c.quality, guided_max_err: c.guided_max_err, guided_line: c.guided_line, guided_near: c.guided_near },
      seed, key: strip ? null : `${rel}|${o.pathId}`, strip, px: w * h };
    jobs.push([o, t, sd, streamed, ch, pool.convert(task)]);
  }
  const stream = new StreamFile();
  for (const [o, t, sd, streamed, ch, p] of jobs) {      // in the textures' order (as the stream was written)
    if (!ch) {                                            // streamed, unchanged: copied into the new stream
      t.m_StreamData = Object.assign({}, sd, { offset: stream.add(await readStream(sd.path, Number(sd.offset), sd.size)) });
      changed.set(String(o.pathId), U.writeObject(sf, o, t));
      continue;
    }
    const { blob, levels } = await timed('wait for workers', () => p);
    if (ch.width !== t.m_Width || ch.height !== t.m_Height) sizes[String(o.pathId)] = [t.m_Width, t.m_Height, ch.width, ch.height];
    stats.in += sd.size || (t['image data'] || []).length;
    stats.out += blob.length;
    t.m_TextureFormat = astcFormatId(ch.block, ch.alpha, sf.unity);
    t.m_Width = ch.width; t.m_Height = ch.height;
    if ('m_MipCount' in t) t.m_MipCount = levels;
    t.m_CompleteImageSize = blob.length;
    if (streamed) { t.m_StreamData = Object.assign({}, sd, { offset: stream.add(blob), size: blob.length }); t['image data'] = new Uint8Array(0); }
    else t['image data'] = blob;
    await timed('write textures', () => changed.set(String(o.pathId), U.writeObject(sf, o, t)));
    converted++;
  }
  if (!converted) return null;
  if (streamName) {                                       // other objects streaming from the same file go along
    for (const o of sf.objects) {
      if (!OTHER_STREAMED.has(o.classId)) continue;
      const t = U.readObject(sf, o), sd = t.m_StreamData || {};
      if (sd.size && sd.path === streamName) {
        t.m_StreamData = Object.assign({}, sd, { offset: stream.add(await readStream(sd.path, Number(sd.offset), sd.size)) });
        changed.set(String(o.pathId), U.writeObject(sf, o, t));
      }
    }
  }
  stats.textures += converted;
  await timed('sprites+atlases', () => fixReferences(sf, sizes, stats, changed));
  return { changed, stream, streamName, sizes };
}

function fixReferences(sf, sizes, stats, changed) {     // textures.py fix_references
  const mul = (d, keys, f) => { for (const k of keys) if (k in d) d[k] *= f; };
  const scaleRD = (rd, sx, sy) => {
    mul(rd.textureRect, ['x', 'width'], sx); mul(rd.textureRect, ['y', 'height'], sy);
    rd.textureRectOffset.x *= sx; rd.textureRectOffset.y *= sy;
    const aro = rd.atlasRectOffset;
    if (aro && !(aro.x === -1 && aro.y === -1)) { aro.x *= sx; aro.y *= sy; }
    if (rd.uvTransform) { mul(rd.uvTransform, ['x', 'y'], sx); mul(rd.uvTransform, ['z', 'w'], sy); }
  };
  for (const o of sf.objects.filter(o => o.classId === 687078895)) {
    const t = U.readObject(sf, o);
    let n = 0;
    for (const [, rd] of t.m_RenderDataMap) {
      const ref = rd.texture;
      const sz = ref.m_FileID === 0 && sizes[String(ref.m_PathID)];
      if (sz) { if ('downscaleMultiplier' in rd) rd.downscaleMultiplier = rd.downscaleMultiplier * sz[2] / sz[0]; n++; }
    }
    if (n) { changed.set(String(o.pathId), U.writeObject(sf, o, t)); stats.atlas += n; }
  }
  for (const o of sf.objects.filter(o => o.classId === 213)) {
    const t = U.readObject(sf, o), ref = t.m_RD.texture;
    const sz = ref.m_FileID === 0 && sizes[String(ref.m_PathID)];
    if (!sz) continue;
    const [ow, oh, nw, nh] = sz, sx = nw / ow, sy = nh / oh;
    mul(t.m_Rect, ['x', 'width'], sx); mul(t.m_Rect, ['y', 'height'], sy);
    t.m_Offset.x *= sx; t.m_Offset.y *= sy;
    mul(t.m_Border, ['x', 'z'], sx); mul(t.m_Border, ['y', 'w'], sy);
    t.m_PixelsToUnits *= sx;
    scaleRD(t.m_RD, sx, sy);
    changed.set(String(o.pathId), U.writeObject(sf, o, t));
    stats.sprites++;
  }
}

// the journal as pmunity's Job.save_state writes it (json.dump: ", " and ": " separators)
function journal(done, scaled) {
  const py = x => Array.isArray(x) ? `[${x.map(py).join(', ')}]` :
    x && typeof x === 'object' ? `{${Object.keys(x).map(k => `${JSON.stringify(k)}: ${py(x[k])}`).join(', ')}}` : JSON.stringify(x);
  return py({ version: 1, done: { textures: done }, scaled });
}
return { run, merged, parseGuide, choose, pyRound, journal, DEFAULTS, PROF };
})();
if (typeof module !== 'undefined') module.exports = TEXTURES;
