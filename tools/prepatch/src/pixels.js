// One texture's pixel work, as pmunity's textures step does it (textures.py _prepare, Astc.encode /
// _guided_rows, astc_guide), on pmucore.wasm: decode (UnityPy's decoders: Pillow's BcnDecode,
// texture2ddecoder's crunch), scale (Pillow's Lanczos on premultiplied alpha), mip levels, guided ASTC
// (guided_astc + astc-encoder, with the port's ASTC guide). Runs in a Web Worker (or in Node for tests).
'use strict';
const PIXELS = (() => {
const PRESET = { fastest: 0, fast: 10, medium: 60, thorough: 98 };
// astc_guide.TIERS (cheapest first) and textures.py Astc.TIERS
const GUIDE_TIERS = [['fastest', { p: 1 }], ['fast', { p: 1 }], ['fastest', {}], ['fast', { p: 2, i: 8 }], ['fast', { p: 2 }],
  ['medium', { p: 1 }], ['fast', {}], ['medium', { p: 2, i: 16 }], ['medium', { p: 2 }], ['medium', {}], ['thorough', {}]];
const CHUNK_ROWS = 32;                    // rows of 256 blocks a chunk, when a big texture's ASTC work is shared
const ROUTE_TIERS = { line: { p: 1 }, near: { p: 2 }, other: {} };
const TF = { RGB24: 3, RGBA32: 4, ARGB32: 5, DXT1: 10, DXT5: 12, BGRA32: 14, BC7: 25, BC4: 26, BC5: 27, DXT1Crunched: 28, DXT5Crunched: 29 };

const CRC = (() => { const t = new Uint32Array(256); for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ c >>> 1 : c >>> 1; t[i] = c; } return t; })();
const crc32 = b => { let c = ~0; for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]) & 255] ^ c >>> 8; return ~c >>> 0; };

async function instantiate(module) {          // pmucore.wasm (a compiled WebAssembly.Module) -> its exports
  let mem;
  const wasi = { fd_close: () => 0, fd_fdstat_get: () => 0, fd_seek: () => 0,
    fd_write: (fd, iov, n, nw) => { const v = new DataView(mem.buffer); let t = 0; for (let i = 0; i < n; i++) t += v.getUint32(iov + 8 * i + 4, true); v.setUint32(nw, t, true); return 0; } };
  const inst = await WebAssembly.instantiate(module, { wasi_snapshot_preview1: wasi });
  const X = inst.exports; mem = X.memory; X._initialize();
  return X;
}

class Pixels {
  constructor(X, guide) {
    this.X = X; this.ctx = new Map(); this.guide = guide;   // guide: {ok, index, base, data} or null
    this.counts = [0, 0, 0, 0, 0, 0]; this.used = 0; this.missed = 0;
    this.split = null;                      // jobs -> Promise of their blocks (other workers help), or null
    this.times = { decode: 0, resize: 0, guided: 0, astc: 0 };   // ms per phase (profiling)
  }
  put(a) { const p = this.X.malloc(a.length || 1); new Uint8Array(this.X.memory.buffer, p, a.length).set(a); return p; }
  take(p, n) { const a = new Uint8Array(this.X.memory.buffer, p, n).slice(); this.X.free(p); return a; }
  free(...ps) { for (const p of ps) this.X.free(p); }

  // UnityPy's parse_image_data(flip=False) + texture_image: RGBA rows in the data's order (bottom first)
  decode(fmt, raw, w, h, vint) {
    const X = this.X;
    let src = raw;
    if (fmt === TF.DXT1Crunched || fmt === TF.DXT5Crunched) {
      const unity = vint >= (2017n << 48n | 3n << 32n);
      const s = this.put(raw), lenp = X.malloc(4);
      const out = X.pm_crunch(unity ? 1 : 0, s, raw.length, lenp);
      const n = new DataView(X.memory.buffer).getUint32(lenp, true);
      this.free(s, lenp);
      if (!out) throw new Error('crunch: cannot unpack');
      src = this.take(out, n);
    }
    const s = this.put(src), d = X.malloc(w * h * 4);
    const r = X.pm_decode(fmt, s, src.length, w, h, d);
    this.free(s);
    if (r) { X.free(d); throw new Error(`decode: format ${fmt} (${r})`); }
    return this.take(d, w * h * 4);
  }
  resize(img, sw, sh, dw, dh) {             // pmunity downscale(): unchanged when the size is the same
    if (sw === dw && sh === dh) return img;
    const s = this.put(img), d = this.X.malloc(dw * dh * 4);
    const r = this.X.pm_resize(s, sw, sh, d, dw, dh);
    this.free(s);
    if (r) { this.X.free(d); throw new Error('resize failed'); }
    return this.take(d, dw * dh * 4);
  }
  static flip(img, w, h) {
    const o = new Uint8Array(img.length), row = w * 4;
    for (let y = 0; y < h; y++) o.set(img.subarray(y * row, (y + 1) * row), (h - 1 - y) * row);
    return o;
  }
  context(bw, bh, preset, lim) {
    const key = `${bw}x${bh}|${preset}|${lim.p ?? -1}|${lim.i ?? -1}`;
    if (!this.ctx.has(key)) {
      const c = this.X.pm_astc_ctx(bw, bh, PRESET[preset], lim.p ?? -1, lim.i ?? -1);
      if (!c) throw new Error('astc-encoder: bad settings');
      this.ctx.set(key, c);
    }
    return this.ctx.get(key);
  }
  async guideCodes(key, hard) {             // astc_guide.Reader.codes
    const g = this.guide;
    if (!g || !g.ok) return null;
    const e = g.index[key];
    const n = hard.length / 4;
    if (!e || e[2] !== n || e[3] !== crc32(hard)) { this.missed++; return null; }
    const z = g.data.subarray(e[0], e[0] + e[1]);
    const codes = new Uint8Array(await new Response(new Blob([z]).stream().pipeThrough(new DecompressionStream('deflate'))).arrayBuffer());
    if (codes.length !== n || codes.some(c => c >= GUIDE_TIERS.length)) { this.missed++; return null; }
    this.used++;
    return codes;
  }
  // Astc._guided_rows: rows already in Unity's order; seed: {dxt: bytes, dxt5} or null; key: guide key or null
  async encodeRows(img, w, h, block, c, seed, key) {
    const X = this.X, [bw, bh] = block;
    const bx = Math.ceil(w / bw), by = Math.ceil(h / bh), nb = bx * by;
    const s = this.put(img), bl = X.malloc(nb * 16), cl = X.malloc(nb);
    let dp = 0, dxt5 = 0;
    if (seed && bw === 4 && bh === 4) { dp = this.put(seed.dxt); dxt5 = seed.dxt5 ? 1 : 0; }
    let t0 = performance.now();
    X.pm_guided(s, w, h, bw, bh, c.guided_max_err, c.guided_line, c.guided_near, dp, dxt5, bl, cl);
    this.times.guided += performance.now() - t0;
    if (dp) X.free(dp);
    const cls = new Uint8Array(X.memory.buffer, cl, nb).slice();
    X.free(cl);
    const SLOT = [5, 0, 1, 2, 3, 4];
    for (let b = 0; b < nb; b++) this.counts[SLOT[cls[b]]]++;
    const lists = { line: [], near: [], other: [] };
    for (let b = 0; b < nb; b++) { const k = cls[b]; if (k === 4) lists.line.push(b); else if (k === 5) lists.near.push(b); else if (k === 0) lists.other.push(b); }
    let groups = [['line', lists.line], ['near', lists.near], ['other', lists.other]].map(([t, l]) => [ROUTE_TIERS[t], c.quality, l]);
    const nhard = lists.line.length + lists.near.length + lists.other.length;
    if (key !== null && this.guide && nhard) {
      const hard = new Uint32Array(nhard);
      hard.set(lists.line, 0); hard.set(lists.near, lists.line.length); hard.set(lists.other, lists.line.length + lists.near.length);
      const hb = new Uint8Array(hard.buffer);
      const codes = await this.guideCodes(key, hb);
      if (codes) groups = GUIDE_TIERS.map(([preset, lim], t) => [lim, preset, [...hard].filter((_, i) => codes[i] === t)]);
    }
    const far = [];                         // a big group's rows, in chunks other workers can take (this.split)
    for (const [lim, preset, rest] of groups) {
      const n = rest.length;
      if (!n) continue;
      const cols = Math.min(n, 256), rows = Math.ceil(n / cols);
      if (this.split && rows > 2 * CHUNK_ROWS) {
        for (let r0 = 0; r0 < rows; r0 += CHUNK_ROWS) {
          const part = rest.slice(r0 * cols, (r0 + CHUNK_ROWS) * cols), pr = Math.ceil(part.length / cols);
          far.push([part, { bw, bh, preset, lim, w: cols * bw, h: pr * bh, sub: this.gather(s, w, h, bw, bh, part, cols, pr) }]);
        }
        continue;
      }
      const sub = this.gather(s, w, h, bw, bh, rest, cols, rows);
      this.scatter(bl, rest, this.astcJob({ bw, bh, preset, lim, w: cols * bw, h: rows * bh, sub }));
    }
    if (far.length) {                       // each block is encoded on its own: the same bytes wherever a chunk runs
      const outs = await this.split(far.map(f => f[1]));
      far.forEach(([part], i) => this.scatter(bl, part, outs[i]));
    }
    X.free(s);
    return this.take(bl, nb * 16);
  }
  gather(s, w, h, bw, bh, list, cols, rows) {     // the listed blocks side by side, cols a row (pm_gather)
    const X = this.X, rp = this.put(new Uint8Array(new Uint32Array(list).buffer)), n = cols * bw * rows * bh * 4, sub = X.malloc(n);
    X.pm_gather(s, w, h, bw, bh, rp, list.length, cols, sub);
    X.free(rp);
    return this.take(sub, n);
  }
  astcJob(j) {                              // {bw, bh, preset, lim, w, h, sub} -> one 16-byte block per footprint
    const X = this.X, nb = Math.ceil(j.w / j.bw) * Math.ceil(j.h / j.bh), s = this.put(j.sub), out = X.malloc(nb * 16);
    const t0 = performance.now();
    const r = X.pm_astc_compress(this.context(j.bw, j.bh, j.preset, j.lim), s, j.w, j.h, j.bw, j.bh, out);
    this.times.astc += performance.now() - t0;
    X.free(s);
    if (r) { X.free(out); throw new Error(`astc-encoder: error ${-r}`); }
    return this.take(out, nb * 16);
  }
  scatter(bl, list, blocks) {               // the encoded blocks back to their places in the texture
    const m = new Uint8Array(this.X.memory.buffer, bl);
    for (let i = 0; i < list.length; i++) m.set(blocks.subarray(16 * i, 16 * i + 16), 16 * list[i]);
  }
  unlz4(src, usize) {                       // an LZ4 block of a bundle, decompressed by lz4's own decoder
    const X = this.X, s = this.put(src), d = X.malloc(usize || 1);
    const n = X.pm_unlz4(s, src.length, d, usize);
    X.free(s);
    if (n !== usize) { X.free(d); throw new Error('LZ4: bad block'); }
    return this.take(d, usize);
  }
  lz4hc(chunks) {                           // UnityPy's compress_lz4: LZ4HC level 9, each chunk on its own
    const X = this.X, out = [];
    for (const ch of chunks) {
      const cap = X.pm_lz4_bound(ch.length), s = this.put(ch), d = X.malloc(cap);
      const n = X.pm_lz4hc(s, ch.length, d, cap, 9);
      X.free(s);
      if (n <= 0) { X.free(d); throw new Error('LZ4HC failed'); }
      out.push(this.take(d, n));
    }
    return out;
  }
  lz4(src, level) {                         // lz4.block.compress(store_size=False): level > 0 high_compression at it, else default
    const X = this.X, cap = X.pm_lz4_bound(src.length), s = this.put(src), d = X.malloc(cap);
    const n = X.pm_lz4hc(s, src.length, d, cap, level);
    X.free(s);
    if (n <= 0) { X.free(d); throw new Error('LZ4 failed'); }
    return this.take(d, n);
  }
  // textures.py _prepare + finish (or strip_blocks): the texture's new data, all its levels
  async convert(t) {        // t: {fmt, raw, w, h, vint, width, height, mips, block, c, seed, key, strip}
    let t0 = performance.now();
    let img = this.decode(t.fmt, t.raw, t.w, t.h, t.vint);
    this.times.decode += performance.now() - t0;
    if (t.strip) return { blob: await this.encodeRows(img, t.w, t.h, t.block, t.c, t.seed, null), levels: 1 };
    let top = Pixels.flip(img, t.w, t.h);                   // UnityPy's tex.image (top row first)
    img = null;
    t0 = performance.now();
    let cur = this.resize(top, t.w, t.h, t.width, t.height), cw = t.width, ch = t.height;
    this.times.resize += performance.now() - t0;
    top = null;
    const parts = [];
    for (let i = 0; i < t.mips; i++) {                      // mip_chain: halved down to 1x1 at most
      if (i > 0) {
        if (cw === 1 && ch === 1) break;
        const nw = Math.max(1, cw >> 1), nh = Math.max(1, ch >> 1);
        t0 = performance.now();
        cur = this.resize(cur, cw, ch, nw, nh); cw = nw; ch = nh;
        this.times.resize += performance.now() - t0;
      }
      parts.push(await this.encodeRows(Pixels.flip(cur, cw, ch), cw, ch, t.block, t.c, i === 0 ? t.seed : null,
        t.key === null ? null : `${t.key}|${i}`));
    }
    const blob = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
    parts.reduce((o, p) => (blob.set(p, o), o + p.length), 0);
    return { blob, levels: parts.length };
  }
}
// A worker's side of the pool (src/pool.js): post(message, transfers) sends to the page. Tasks: {task} a texture,
// {lz4} chunks, {astc} a chunk of another worker's texture. A big texture's chunks go to the pool ({help}); this
// worker takes them back one by one ({claim} -> {mine}) while idle workers take the others ({helped}).
function serve(post) {
  let px, tags = 0;
  const own = new Map();
  const done = (t, k, out, err) => { const st = own.get(t);
    if (!st) return;
    if (err) { own.delete(t); st.no(new Error(err)); return; }
    st.outs[k] = out;
    if (!--st.left) { own.delete(t); st.ok(st.outs); } };
  return async m => {
    if (m.init) {
      px = new Pixels(await instantiate(m.module || await WebAssembly.compile(m.wasm)), m.guide);
      if (m.split !== false) px.split = jobs => new Promise((ok, no) => { const t = tags++;
        own.set(t, { outs: new Array(jobs.length), left: jobs.length, ok, no });
        post({ help: t, jobs }, jobs.map(j => j.sub.buffer));
        post({ claim: t }); });
      post({ ready: 1 });
      return;
    }
    if (m.mine !== undefined) {           // a chunk of this worker's own texture, or none left
      if (m.k < 0) return;
      let out = null, err = null;
      try { out = px.astcJob(m.job); } catch (e) { err = String(e && e.message || e); }
      done(m.mine, m.k, out, err);
      if (own.has(m.mine)) post({ claim: m.mine });
      return;
    }
    if (m.helped !== undefined) { done(m.helped, m.k, m.out, m.err); return; }
    try {
      if (m.lz4) { const out = px.lz4hc(m.lz4); post({ id: m.id, r: out }, out.map(x => x.buffer)); return; }
      if (m.astc) { const out = px.astcJob(m.astc); post({ id: m.id, r: out }, [out.buffer]); return; }
      const r = await px.convert(m.task); post({ id: m.id, r }, [r.blob.buffer]);
    } catch (err) { post({ id: m.id, err: String(err && err.message || err) }); }
  };
}
return { instantiate, Pixels, serve, GUIDE_TIERS, TF, crc32 };
})();
if (typeof module !== 'undefined') module.exports = PIXELS;
