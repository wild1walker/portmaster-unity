// node wasm_side.js <pmucore.wasm> <outdir>: test inputs + pmucore.wasm's outputs, for ref_side.py to compare
const fs = require('fs');
const [wasmPath, out] = process.argv.slice(2);
fs.mkdirSync(out, { recursive: true });
let seed = 12345;
const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) >>> 8 & 255;
const bytes = n => { const b = new Uint8Array(n); for (let i = 0; i < n; i++) b[i] = rnd(); return b; };
function smooth(w, h, alphaMode) {      // gradients, flat areas and edges (so every guided class shows up)
  const b = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = 4 * (y * w + x), flat = ((x >> 3) + (y >> 3)) % 3 === 0;
    b[i] = flat ? 200 : (x * 255 / w) | 0; b[i + 1] = flat ? 30 : (y * 255 / h) | 0;
    b[i + 2] = flat ? 90 : ((x ^ y) & 255); b[i + 3] = alphaMode === 'opaque' ? 255 : ((x * 7 + y * 3) % 5 === 0 ? 0 : 255 - ((x + y) & 127));
  }
  return b;
}
(async () => {
  let mem;
  const wasi = { fd_close: () => 0, fd_fdstat_get: () => 0, fd_seek: () => 0,
    fd_write: (fd, iov, n, nw) => { const v = new DataView(mem.buffer); let t = 0; for (let i = 0; i < n; i++) t += v.getUint32(iov + 8 * i + 4, true); v.setUint32(nw, t, true); return 0; } };
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), { wasi_snapshot_preview1: wasi });
  const X = instance.exports; mem = X.memory; X._initialize();
  const put = a => { const p = X.malloc(a.length || 1); new Uint8Array(mem.buffer, p, a.length).set(a); return p; };
  const get = (p, n) => new Uint8Array(mem.buffer, p, n).slice();
  const save = (n, a) => fs.writeFileSync(`${out}/${n}`, a);
  const cases = [];
  // decode
  for (const [fmt, bs, w, h] of [[10, 8, 37, 21], [12, 16, 64, 40], [25, 16, 33, 17], [26, 8, 20, 12], [27, 16, 19, 23], [4, 0, 9, 7], [5, 0, 9, 7], [14, 0, 9, 7], [3, 0, 9, 7]]) {
    const n = bs ? Math.ceil(w / 4) * Math.ceil(h / 4) * bs : w * h * (fmt === 3 ? 3 : 4);
    const src = bytes(n), d = X.malloc(w * h * 4), s = put(src);
    const r = X.pm_decode(fmt, s, n, w, h, d);
    save(`dec_${fmt}.in`, src); save(`dec_${fmt}.out`, get(d, w * h * 4));
    cases.push({ kind: 'decode', fmt, w, h, r });
  }
  // resize
  for (const [sw, sh, dw, dh, a] of [[301, 187, 113, 70, 'alpha'], [256, 256, 96, 96, 'alpha'], [64, 30, 24, 11, 'opaque'], [5, 3, 2, 1, 'alpha']]) {
    const src = smooth(sw, sh, a); for (let i = 0; i < src.length; i += 97) src[i] = rnd();
    const s = put(src), d = X.malloc(dw * dh * 4);
    const r = X.pm_resize(s, sw, sh, d, dw, dh);
    const tag = `${sw}x${sh}_${dw}x${dh}`;
    save(`rs_${tag}.in`, src); save(`rs_${tag}.out`, get(d, dw * dh * 4));
    cases.push({ kind: 'resize', sw, sh, dw, dh, r, tag });
  }
  // guided (+ gather and the encoder on the rest, at medium)
  for (const [w, h, bw, bh, a] of [[96, 64, 4, 4, 'alpha'], [80, 48, 4, 4, 'opaque'], [60, 36, 6, 6, 'alpha']]) {
    const img = smooth(w, h, a);
    const s = put(img), bx = Math.ceil(w / bw), by = Math.ceil(h / bh), nb = bx * by;
    const bl = X.malloc(nb * 16), cl = X.malloc(nb);
    X.pm_guided(s, w, h, bw, bh, 64, 128, 1024, 0, 0, bl, cl);
    const tag = `${w}x${h}_${bw}`;
    save(`gd_${tag}.in`, img); save(`gd_${tag}.blocks`, get(bl, nb * 16)); save(`gd_${tag}.cls`, get(cl, nb));
    // the "other" blocks through the encoder, as textures.py does at medium
    const cls = get(cl, nb), rest = []; cls.forEach((c, i) => { if (c === 0 || c === 4 || c === 5) rest.push(i); });
    if (rest.length) {
      const cols = Math.min(rest.length, 256), rows = Math.ceil(rest.length / cols);
      const r32 = put(new Uint8Array(new Uint32Array(rest).buffer)), sub = X.malloc(cols * bw * rows * bh * 4);
      X.pm_gather(s, w, h, bw, bh, r32, rest.length, cols, sub);
      save(`gd_${tag}.gather`, get(sub, cols * bw * rows * bh * 4));
      for (const [q, p1, p2, name] of [[60, -1, -1, 'medium'], [0, 1, -1, 'fastest_p1'], [10, 2, 8, 'fast_p2i8']]) {
        const ctx = X.pm_astc_ctx(bw, bh, q, p1, p2), o = X.malloc(cols * rows * 16);
        const r = X.pm_astc_compress(ctx, sub, cols * bw, rows * bh, bw, bh, o);
        save(`gd_${tag}.astc_${name}`, get(o, cols * rows * 16));
        cases.push({ kind: 'astc', tag, name, q, p1, p2, cols, rows, bw, bh, r });
      }
    }
    cases.push({ kind: 'guided', w, h, bw, bh, tag, rest: rest.length });
  }
  fs.writeFileSync(`${out}/cases.json`, JSON.stringify(cases, null, 1));
  console.log(cases.length, 'cases written');
})();
