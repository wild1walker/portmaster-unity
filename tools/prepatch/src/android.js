// pmunity's android-data (the data tree Unity's Android player reads, packed as an APK and an OBB) for the Mono
// route, as the handheld's setup runs it: the same files with the same bytes. This part: the platform byte of a
// serialized file, Mono's config, and the stored zip32 archives as Python's zipfile writes them (_write_zip).
'use strict';
const ANDROID = (() => {
const PLATFORM = 13;                        // BuildTarget.Android

// sf_platform_offset: (offset of the uint32 target platform, little-endian?, current value) of a
// SerializedFile header (formats 9..21: 20-byte big-endian header; 22+: 48 bytes); then the metadata's
// byte order flag, the Unity version string and the platform
function sfPlatform(head) {
  const v = new DataView(head.buffer, head.byteOffset, head.byteLength);
  const fmt = v.getUint32(8, false);
  let p = fmt >= 22 ? 48 : 20;
  const big = head[16] !== 0;               // (the byte order flag, both header layouts)
  let e = p; while (head[e]) e++;
  const uv = new TextDecoder('latin1').decode(head.subarray(p, e));
  if (!/^\d+\.\d+\.\d+/.test(uv)) throw new Error(`bad unity version string ${JSON.stringify(uv)}`);
  const off = e + 1;
  return { off, little: !big, cur: v.getUint32(off, !big), uv };
}
function patchSfPlatform(head) {            // (the first bytes of the file, changed in place) -> old platform
  const s = sfPlatform(head);
  if (s.cur !== PLATFORM) new DataView(head.buffer, head.byteOffset, head.byteLength).setUint32(s.off, PLATFORM, s.little);
  return s.cur;
}

// mono_config_text: Mac library names -> Linux ones, plus the port's extra dllmaps
const MONO_CONFIG_EXTRA = [['libmono.so.0', 'libmono.so']];
const DYLIB_TO_SO = [[/libc\.dylib/, () => 'libc.so.6'], [/libintl\.dylib/, () => 'libc.so.6'],
  [/lib([\w.+-]+?)\.(\d+)\.dylib/, m => `lib${m[1]}.so.${m[2]}`], [/lib([\w.+-]+?)\.dylib/, m => `lib${m[1]}.so`]];
function monoConfigText(txt, extra) {
  const lines = txt.match(/[^\n]*\n|[^\n]+$/g) || [];
  for (let i = 0; i < lines.length; i++) {
    let l = lines[i];
    if (l.includes('os="osx"')) continue;
    for (const [rx, to] of DYLIB_TO_SO) {
      const full = new RegExp('target="' + rx.source + '"', 'g');
      l = l.replace(full, (...a) => `target="${to(a.slice(0, -2))}"`);
    }
    lines[i] = l;
  }
  txt = lines.join('');
  for (const [dll, target] of [...extra, ...MONO_CONFIG_EXTRA]) {
    const line = `\t<dllmap dll="${dll}" target="${target}" os="!windows"/>\n`;
    if (!txt.includes(`dll="${dll}"`)) txt = txt.split('</configuration>').join(line + '</configuration>');
  }
  return txt;
}

// _write_zip: STORE, no zip64, each file's data 4-byte aligned by a 0xD935 extra field, the dates of 2018-08-02,
// mode 0644, as CPython's zipfile writes it on a seekable file (made on Unix). entries: [{name, size, chunks()}]
// (chunks: an async iterable of the file's bytes, asked for twice: its CRC goes in the header before the data)
const ZIP_LIMIT = 2 ** 31 - 1;
const DOS_DATE = ((2018 - 1980) << 9) | (8 << 5) | 2, DOS_TIME = 0;
function zipLayout(entries, align = 4) {    // [{n, extra, off}] per entry and the archive's size
  let pos = 0;
  const out = entries.map(e => {
    const n = new TextEncoder().encode(e.name), pad = ((-(pos + 30 + n.length + 4)) % align + align) % align;
    const extra = new Uint8Array(4 + pad); extra[0] = 0x35; extra[1] = 0xD9; extra[2] = pad & 255; extra[3] = pad >> 8;
    const r = { n, extra, off: pos };
    pos += 30 + n.length + extra.length + e.size;
    return r;
  });
  const cd = out.reduce((a, r) => a + 46 + r.n.length + r.extra.length, 0);
  return { out, size: pos + cd + 22, cdStart: pos, cdSize: cd };
}
function zipSize(entries) { return zipLayout(entries).size; }
async function* zipChunks(entries, crc32) {
  const L = zipLayout(entries), crcs = [];
  if (L.size > ZIP_LIMIT) throw new Error(`the archive would be ${L.size} bytes, over the 2 GiB limit`);
  for (let i = 0; i < entries.length; i++) {
    const e = entries[i], r = L.out[i];
    let crc = 0, got = 0;
    for await (const c of e.chunks()) { crc = crc32(crc, c); got += c.length; }
    if (got !== e.size) throw new Error(`${e.name}: ${got} bytes, expected ${e.size}`);
    crcs.push(crc >>> 0);
    const flags = /[^\x00-\x7f]/.test(e.name) ? 0x800 : 0;
    const h = new Uint8Array(30 + r.n.length + r.extra.length), v = new DataView(h.buffer);
    v.setUint32(0, 0x04034b50, true); v.setUint16(4, 20, true); v.setUint16(6, flags, true); v.setUint16(8, 0, true);
    v.setUint16(10, DOS_TIME, true); v.setUint16(12, DOS_DATE, true); v.setUint32(14, crc >>> 0, true);
    v.setUint32(18, e.size, true); v.setUint32(22, e.size, true); v.setUint16(26, r.n.length, true); v.setUint16(28, r.extra.length, true);
    h.set(r.n, 30); h.set(r.extra, 30 + r.n.length);
    yield h;
    for await (const c of e.chunks()) yield c;
  }
  const cd = new Uint8Array(L.cdSize + 22), v = new DataView(cd.buffer);
  let p = 0;
  for (let i = 0; i < entries.length; i++) {
    const e = entries[i], r = L.out[i], flags = /[^\x00-\x7f]/.test(e.name) ? 0x800 : 0;
    v.setUint32(p, 0x02014b50, true); cd[p + 4] = 20; cd[p + 5] = 3; v.setUint16(p + 6, 20, true); v.setUint16(p + 8, flags, true);
    v.setUint16(p + 10, 0, true); v.setUint16(p + 12, DOS_TIME, true); v.setUint16(p + 14, DOS_DATE, true); v.setUint32(p + 16, crcs[i], true);
    v.setUint32(p + 20, e.size, true); v.setUint32(p + 24, e.size, true); v.setUint16(p + 28, r.n.length, true); v.setUint16(p + 30, r.extra.length, true);
    v.setUint32(p + 38, (0o644 << 16) >>> 0, true); v.setUint32(p + 42, r.off, true);
    cd.set(r.n, p + 46); cd.set(r.extra, p + 46 + r.n.length);
    p += 46 + r.n.length + r.extra.length;
  }
  v.setUint32(p, 0x06054b50, true); v.setUint16(p + 8, entries.length, true); v.setUint16(p + 10, entries.length, true);
  v.setUint32(p + 12, L.cdSize, true); v.setUint32(p + 16, L.cdStart, true);
  yield cd;
}

// ---- UnityFS bundles: the platform of every serialized file inside set to Android (patch_bundle_platform),
// in a bundle's bytes. lz: {unlz4(src, usize), lz4(src, level)} (pixels.js: lz4's own code, as python-lz4)
const cstrAt = (b, p) => { let e = p; while (b[e]) e++; return [b.subarray(p, e), e + 1]; };
const eq = (a, b) => { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; };
const cat = (...a) => { const o = new Uint8Array(a.reduce((n, x) => n + x.length, 0)); let p = 0; for (const x of a) { o.set(x, p); p += x.length; } return o; };

class Bundle {                               // android_data.Bundle: header, blocks, nodes, block positions
  constructor(buf, lz) {
    this.buf = buf; this.lz = lz;
    const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    let [sig, p] = cstrAt(buf, 0);
    if (new TextDecoder('latin1').decode(sig) !== 'UnityFS') throw new Error('not a UnityFS bundle');
    this.format = dv.getUint32(p, false); p += 4;
    [this.uv, p] = cstrAt(buf, p); [this.ur, p] = cstrAt(buf, p);
    this.size = Number(dv.getBigInt64(p, false)); this.ciSize = dv.getUint32(p + 8, false);
    this.uiSize = dv.getUint32(p + 12, false); this.flags = dv.getUint32(p + 16, false); p += 20;
    if (this.format >= 7) p = (p + 15) & ~15;
    this.headerEnd = p;
    let ci;
    if (this.flags & 0x80) { ci = buf.subarray(this.size - this.ciSize, this.size); this.dataStart = p; }
    else { ci = buf.subarray(p, p + this.ciSize); this.dataStart = p + this.ciSize; if (this.flags & 0x200) this.dataStart = (this.dataStart + 15) & ~15; }
    const comp = this.flags & 0x3f;
    let ub;
    if (comp === 0) ub = ci;
    else if (comp === 2 || comp === 3) ub = lz.unlz4(ci, this.uiSize);
    else throw new Error(`blocks info compression ${comp} (LZMA: left to the handheld)`);
    const u = new DataView(ub.buffer, ub.byteOffset, ub.byteLength);
    this.hash = ub.slice(0, 16);
    const n = u.getInt32(16, false);
    this.blocks = [];
    for (let i = 0; i < n; i++) this.blocks.push([u.getUint32(20 + 10 * i, false), u.getUint32(24 + 10 * i, false), u.getUint16(28 + 10 * i, false)]);
    p = 20 + 10 * n;
    const nn = u.getInt32(p, false); p += 4;
    this.nodes = [];
    for (let i = 0; i < nn; i++) {
      const off = Number(u.getBigInt64(p, false)), sz = Number(u.getBigInt64(p + 8, false)), fl = u.getUint32(p + 16, false); p += 20;
      let name; [name, p] = cstrAt(ub, p);
      this.nodes.push([off, sz, fl, new TextDecoder().decode(name)]);
    }
    this.blockPos = [];
    let fpos = this.dataStart, upos = 0;
    for (const [usz, csz] of this.blocks) { this.blockPos.push([fpos, upos]); fpos += csz; upos += usz; }
  }
  blockOf(uoff) {
    for (let i = 0; i < this.blocks.length; i++) { const upos = this.blockPos[i][1]; if (upos <= uoff && uoff < upos + this.blocks[i][0]) return i; }
    throw new Error('offset beyond data');
  }
  readBlock(i) {                             // [raw, plain] of block i as the bytes are now
    const [usz, csz, bfl] = this.blocks[i], raw = this.buf.subarray(this.blockPos[i][0], this.blockPos[i][0] + csz), c = bfl & 0x3f;
    if (c === 0) return [raw, raw];
    if (c === 2 || c === 3) return [raw, this.lz.unlz4(raw, usz)];
    throw new Error(`block compression ${c} (LZMA: left to the handheld)`);
  }
  readPlain(uoff, n) {
    const out = [];
    while (n > 0) {
      const i = this.blockOf(uoff), plain = this.readBlock(i)[1], s = uoff - this.blockPos[i][1];
      const take = plain.subarray(s, s + n); out.push(take); n -= take.length; uoff += take.length;
    }
    return cat(...out);
  }
}

function lz4LiteralPos(comp, target) {      // where in comp the literal for plaintext[target] is, or null (a match)
  let ip = 0, op = 0;
  const n = comp.length;
  while (ip < n) {
    const token = comp[ip++];
    let lit = token >> 4;
    if (lit === 15) { let b; do { b = comp[ip++]; lit += b; } while (b === 255); }
    if (op <= target && target < op + lit) return ip + (target - op);
    ip += lit; op += lit;
    if (ip >= n) break;
    ip += 2;
    let ml = token & 15;
    if (ml === 15) { let b; do { b = comp[ip++]; ml += b; } while (b === 255); }
    ml += 4;
    if (op <= target && target < op + ml) return null;
    op += ml;
  }
  return null;
}
function lz4Sequences(comp) {               // [[literals, offset|null, matchlen, match bytes]], plaintext
  const seqs = [], out = [];
  let ip = 0;
  const n = comp.length;
  while (ip < n) {
    const token = comp[ip++];
    let lit = token >> 4;
    if (lit === 15) { let b; do { b = comp[ip++]; lit += b; } while (b === 255); }
    const lits = comp.slice(ip, ip + lit); ip += lit;
    for (const x of lits) out.push(x);
    if (ip >= n) { seqs.push([lits, null, 0]); break; }
    const off = comp[ip] | comp[ip + 1] << 8; ip += 2;
    let ml = token & 15;
    if (ml === 15) { let b; do { b = comp[ip++]; ml += b; } while (b === 255); }
    ml += 4;
    const start = out.length - off;
    for (let j = 0; j < ml; j++) out.push(out[start + j]);
    seqs.push([lits, off, ml, Uint8Array.from(out.slice(out.length - ml))]);
  }
  return [seqs, Uint8Array.from(out)];
}
const ext = n => n < 15 ? 0 : 1 + Math.floor((n - 15) / 255);
const seqSize = q => 1 + ext(q[0].length) + q[0].length + (q[1] !== null ? 2 + ext(q[2] - 4) : 0);
function lz4Emit(seqs) {
  const o = [];
  for (const q of seqs) {
    const lits = q[0], off = q[1], ml = off !== null ? q[2] - 4 : 0, L = lits.length;
    o.push((Math.min(L, 15) << 4) | (off !== null ? Math.min(ml, 15) : 0));
    if (L >= 15) { let r = L - 15; while (r >= 255) { o.push(255); r -= 255; } o.push(r); }
    for (const x of lits) o.push(x);
    if (off !== null) {
      o.push(off & 255, off >> 8);
      if (ml >= 15) { let r = ml - 15; while (r >= 255) { o.push(255); r -= 255; } o.push(r); }
    }
  }
  return Uint8Array.from(o);
}
function lz4DecodeTrace(comp) {             // [plaintext, [[out start, matchlen, offset bytes' position]]] or [null]
  const out = [], matches = [];
  let ip = 0;
  const n = comp.length;
  while (ip < n) {
    const token = comp[ip++];
    let lit = token >> 4;
    if (lit === 15) { let b; do { b = comp[ip++]; lit += b; } while (b === 255); }
    for (let j = 0; j < lit; j++) out.push(comp[ip + j]);
    ip += lit;
    if (ip >= n) break;
    const opos = ip, off = comp[ip] | comp[ip + 1] << 8; ip += 2;
    let ml = token & 15;
    if (ml === 15) { let b; do { b = comp[ip++]; ml += b; } while (b === 255); }
    ml += 4;
    const start = out.length, s0 = out.length - off;
    if (s0 < 0) return [null, null];
    for (let j = 0; j < ml; j++) out.push(out[s0 + j]);
    matches.push([start, ml, opos]);
  }
  return [Uint8Array.from(out), matches];
}
function rfind(hay, sub, from, to) {         // bytes.rfind(sub, from, to)
  for (let q = to - sub.length; q >= from; q--) {
    let k = 0; while (k < sub.length && hay[q + k] === sub[k]) k++;
    if (k === sub.length) return q;
  }
  return -1;
}
function lz4Repoint(comp, expect, tries = 64) {
  comp = Uint8Array.from(comp);
  for (let t = 0; t < tries; t++) {
    const [out, matches] = lz4DecodeTrace(comp);
    if (out === null || out.length !== expect.length) return null;
    if (eq(out, expect)) return comp;
    let bad = 0; while (out[bad] === expect[bad]) bad++;
    const m = matches.find(x => x[0] <= bad && bad < x[0] + x[1]);
    if (!m) return null;
    const [start, ml, opos] = m;
    const q = rfind(expect, expect.subarray(start, start + ml), Math.max(0, start - 65535), start + ml - 1);
    if (q < 0 || q >= start || start - q > 65535) return null;
    comp[opos] = (start - q) & 255; comp[opos + 1] = (start - q) >> 8;
  }
  return null;
}
function lz4Exact(plain, size, lz) {        // a raw LZ4 block of plain exactly size bytes long, or null
  for (const level of [12, 9, 4, 0]) {
    const c = lz.lz4(plain, level);
    if (c.length > size) continue;
    let [seqs, check] = lz4Sequences(c);
    if (!eq(check, plain)) continue;
    let deficit = size - c.length;
    for (let pass = 0; pass < 8; pass++) {
      if (deficit === 0) break;
      const out = [];
      let cur = seqs[0];
      for (const nxt of seqs.slice(1)) {
        if (deficit && cur[1] !== null) {
          const merged = [cat(cur[0], cur[3], nxt[0]), ...nxt.slice(1)];
          const d = seqSize(merged) - seqSize(cur) - seqSize(nxt);
          if (d > 0 && d <= deficit) { cur = merged; deficit -= d; continue; }
        }
        out.push(cur); cur = nxt;
      }
      out.push(cur);
      seqs = out;
    }
    if (deficit) continue;
    const res = lz4Emit(seqs);
    if (res.length === size) { let ok = false; try { ok = eq(lz.unlz4(res, plain.length), plain); } catch { /* corrupt */ } if (ok) return res; }
  }
  return null;
}
function rewriteBundleBlock(b, i, rel, value) {   // block i re-encoded (LZ4HC 9) with plaintext[rel] = value: a new bundle
  const p = Uint8Array.from(b.readBlock(i)[1]); p[rel] = value;
  const newraw = b.lz.lz4(p, 9);
  const blocks = b.blocks.map(x => [...x]);
  blocks[i][1] = newraw.length; blocks[i][2] = (blocks[i][2] & ~0x3f) | 3;
  const enc = new TextEncoder(), parts = [b.hash];
  const be = (n, f) => { const x = new Uint8Array(n), v = new DataView(x.buffer); f(v); return x; };
  parts.push(be(4, v => v.setInt32(0, blocks.length, false)));
  for (const [usz, csz, bfl] of blocks) parts.push(be(10, v => { v.setUint32(0, usz, false); v.setUint32(4, csz, false); v.setUint16(8, bfl, false); }));
  parts.push(be(4, v => v.setInt32(0, b.nodes.length, false)));
  for (const [off, sz, fl, name] of b.nodes) parts.push(be(20, v => { v.setBigInt64(0, BigInt(off), false); v.setBigInt64(8, BigInt(sz), false); v.setUint32(16, fl, false); }), enc.encode(name), new Uint8Array(1));
  const bi = cat(...parts), ci = b.lz.lz4(bi, 9);
  const flags = (b.flags & ~0x3f) | 3;
  const out = [new Uint8Array(b.headerEnd)];
  let pos = b.headerEnd;
  if (!(flags & 0x80)) { out.push(ci); pos += ci.length; if (flags & 0x200) { const pad = (16 - pos % 16) % 16; out.push(new Uint8Array(pad)); pos += pad; } }
  for (let j = 0; j < b.blocks.length; j++) { const blk = j === i ? newraw : b.buf.subarray(b.blockPos[j][0], b.blockPos[j][0] + b.blocks[j][1]); out.push(blk); pos += blk.length; }
  if (flags & 0x80) { out.push(ci); pos += ci.length; }
  const res = cat(...out);
  res.set(b.buf.subarray(0, b.headerEnd), 0);
  const p0 = 8 + 4 + b.uv.length + 1 + b.ur.length + 1, v = new DataView(res.buffer);
  v.setBigInt64(p0, BigInt(pos), false); v.setUint32(p0 + 8, ci.length, false); v.setUint32(p0 + 12, bi.length, false); v.setUint32(p0 + 16, flags, false);
  return res;
}
// patchBundlePlatform(bytes, lz) -> {buf: the bundle's new bytes, done: [[node, old platform, method]]}
function patchBundlePlatform(buf, lz, platform = PLATFORM) {
  buf = Uint8Array.from(buf);
  const b = new Bundle(buf, lz), done = [];
  for (const [off, , fl, name] of b.nodes) {
    if (!(fl & 4)) continue;
    const head = b.readPlain(off, 128), s = sfPlatform(head);
    if (s.cur === platform) { done.push([name, s.cur, 'already']); continue; }
    const nw = new Uint8Array(4), old = new Uint8Array(4);
    new DataView(nw.buffer).setUint32(0, platform, s.little); new DataView(old.buffer).setUint32(0, s.cur, s.little);
    let method = 'literal';
    for (let k = 0; k < 4; k++) {
      if (old[k] === nw[k]) continue;
      const u = off + s.off + k, i = b.blockOf(u), [raw, plain] = b.readBlock(i), rel = u - b.blockPos[i][1], c = b.blocks[i][2] & 0x3f;
      let pos;
      const again = () => { done.push([name, s.cur, 'rewrite']); const r = patchBundlePlatform(rewriteBundleBlock(b, i, rel, nw[k]), lz, platform); return { buf: r.buf, done: done.concat(r.done) }; };
      if (c === 0) pos = rel;
      else if (c === 1) throw new Error('an LZMA block (left to the handheld)');
      else {
        pos = lz4LiteralPos(raw, rel);
        if (pos === null) return again();
        const patched = Uint8Array.from(raw); patched[pos] = nw[k];
        const expect = Uint8Array.from(plain); expect[rel] = nw[k];
        let same = false; try { same = eq(lz.unlz4(patched, b.blocks[i][0]), expect); } catch { /* corrupt */ }
        if (!same) {
          const enc = lz4Repoint(patched, expect) || lz4Exact(expect, raw.length, lz);
          if (enc === null) return again();
          buf.set(enc, b.blockPos[i][0]);
          method = 'reencode';
          continue;
        }
      }
      buf[b.blockPos[i][0] + pos] = nw[k];
    }
    done.push([name, s.cur, method]);
  }
  return { buf, done };
}

// ---- serialized files (unity.js: read and saved as UnityPy does)
const U = () => (typeof UNITY !== 'undefined' ? UNITY : require('./unity.js'));
const C = { Texture2D: 28, Cubemap: 89, Texture2DArray: 187, Texture3D: 117, Mesh: 43, BuildSettings: 141, GraphicsSettings: 30,
  MonoManager: 116, InputManager: 13, QualitySettings: 47, PlayerSettings: 129 };
const TEXTURES = new Set([C.Texture2D, C.Cubemap, C.Texture2DArray, C.Texture3D]);
const withPlatform = bytes => { const b = Uint8Array.from(bytes); patchSfPlatform(b.subarray(0, 128)); return b; };

// inline_serialized: textures' streamed pixels (m_StreamData in <file>.resS) into the textures, the platform
// Android. readStream(name, offset, size) -> bytes. Returns {parts, inlined, still: .resS names still read}
async function inlineSerialized(bytes, name, readStream) {
  const u = U(), sf = u.parseSerialized(withPlatform(bytes), name), changed = new Map(), still = new Set();
  let inlined = 0;
  for (const o of sf.objects) {
    if (o.classId === C.Mesh) {
      const sd = u.readObject(sf, o).m_StreamData;
      if (sd && sd.path && Number(sd.size)) still.add(sd.path.split('/').pop());
      continue;
    }
    if (!TEXTURES.has(o.classId)) continue;
    const t = u.readObject(sf, o), sd = t.m_StreamData;
    if (!sd || !sd.path) continue;
    if (!('image data' in t)) { still.add(sd.path.split('/').pop()); continue; }
    const data = await readStream(sd.path.split('/').pop(), Number(sd.offset), Number(sd.size));
    if (data.length !== Number(sd.size)) throw new Error(`${t.m_Name}: its stream gave ${data.length} bytes, not ${sd.size}`);
    t['image data'] = data;
    t.m_StreamData = { offset: typeof sd.offset === 'bigint' ? 0n : 0, size: typeof sd.size === 'bigint' ? 0n : 0, path: '' };
    changed.set(String(o.pathId), u.writeObject(sf, o, t));
    inlined++;
  }
  return { parts: u.save(sf, changed).parts, inlined, still };
}

// edit_globalgamemanagers: platform Android, GLES3 only, GLES3 shader defines from GLCore's, MonoManager's missing
// game assemblies dropped (--drop-missing-assembly), Rewired's joystick axes, PlayerSettings (blit never,
// mobileMTRenderingBaked). o: {graphicsApis, managedNames: Set, dropMissing: [], rewiredJoysticks, blit, mtRendering}
const SHADER_PLATFORM_GLCORE = 15, SHADER_PLATFORM_GLES3 = 9, SHADER_PLATFORM_D3D11 = 4;
const pyList = a => `[${a.map(x => typeof x === 'string' ? `'${x}'` : String(x)).join(', ')}]`;
function builtinAssemblySlots(uv) {          // MonoManager's engine slots (5.x - 2017.x)
  const [maj, min] = uv.split('.').map(Number);
  if (maj < 2017 || (maj === 2017 && min === 1)) return 2;
  if (maj === 2017 && min === 2) return 45;
  return 48;
}
const isEngineModule = n => n === 'UnityEngine.dll' || /^UnityEngine\.\w+Module\.dll$/.test(n);
function splitAssemblies(names, uv) {
  if (+uv.split('.')[0] <= 2017) { const n = builtinAssemblySlots(uv); return [names.filter((a, i) => i >= n && a), names.filter((a, i) => i < n && a)]; }
  return [names.filter(a => a && !isEngineModule(a)), names.filter(a => a && isEngineModule(a))];
}
function editGlobalGameManagers(bytes, o) {
  const u = U(), sf = u.parseSerialized(withPlatform(bytes), 'globalgamemanagers'), changed = new Map(), notes = [];
  const old = sfPlatform(bytes.subarray(0, 128)).cur;
  for (const ob of sf.objects) {
    const save = t => changed.set(String(ob.pathId), u.writeObject(sf, ob, t));
    if (ob.classId === C.BuildSettings) {
      const t = u.readObject(sf, ob), apis = o.graphicsApis;
      if (!eq(Int32Array.from(t.m_GraphicsAPIs), Int32Array.from(apis))) {
        notes.push(`BuildSettings.m_GraphicsAPIs ${pyList(t.m_GraphicsAPIs)} -> ${pyList(apis)}`);
        t.m_GraphicsAPIs = [...apis]; save(t);
      }
    } else if (ob.classId === C.GraphicsSettings) {
      const t = u.readObject(sf, ob);
      if (!('m_ShaderDefinesPerShaderCompiler' in t)) continue;
      const d = t.m_ShaderDefinesPerShaderCompiler, have = new Set(d.map(e => e.shaderPlatform));
      const src = [SHADER_PLATFORM_GLCORE, SHADER_PLATFORM_D3D11].find(p => have.has(p));
      if (!have.has(SHADER_PLATFORM_GLES3) && src !== undefined) {
        d.push({ ...d.find(e => e.shaderPlatform === src), shaderPlatform: SHADER_PLATFORM_GLES3 });
        notes.push(`GraphicsSettings: GLES3 shader defines = ${src === SHADER_PLATFORM_GLCORE ? 'GLCore' : 'D3D11'} defines`);
        save(t);
      }
    } else if (ob.classId === C.MonoManager) {
      const t = u.readObject(sf, ob);
      if (!('m_AssemblyNames' in t)) continue;
      const names = t.m_AssemblyNames, [game, engine] = splitAssemblies(names, sf.unity);
      let missing = game.filter(a => !o.managedNames.has(a));
      const slots = builtinAssemblySlots(sf.unity);
      const drop = names.map((a, i) => i).filter(i => missing.includes(names[i]) && o.dropMissing.includes(names[i]) && i >= slots);
      if (drop.length) {
        const types = t.m_AssemblyTypes;
        if (types !== undefined && types.length !== names.length) throw new Error('MonoManager: m_AssemblyTypes and m_AssemblyNames differ in length');
        notes.push(`MonoManager: removed ${pyList(drop.map(i => names[i]))} (not in Managed/)`);
        t.m_AssemblyNames = names.filter((a, i) => !drop.includes(i));
        if (types !== undefined) t.m_AssemblyTypes = Array.from(types).filter((a, i) => !drop.includes(i));
        save(t);
        missing = missing.filter(a => !o.dropMissing.includes(a));
      }
      if (missing.length) throw new Error(`MonoManager lists assemblies missing from Managed/: ${missing.join(', ')}`);
      const absent = engine.filter(a => !o.managedNames.has(a));
      if (absent.length) notes.push(`MonoManager: engine modules the player will skip (not in its Managed set): ${pyList(absent)}`);
    } else if (ob.classId === C.InputManager && o.managedNames.has('Rewired_Core.dll')) {
      const t = u.readObject(sf, ob), ax = t.m_Axes, have = new Set(ax.map(a => a.m_Name)), proto = ax[0];
      let added = 0;
      for (let j = 1; j <= o.rewiredJoysticks; j++) for (let a = 1; a <= 28; a++) {
        const name = `Joy${j}Axis${a}`;
        if (have.has(name)) continue;
        ax.push({ ...proto, m_Name: name, descriptiveName: '', descriptiveNegativeName: '', negativeButton: '', positiveButton: '',
          altNegativeButton: '', altPositiveButton: '', gravity: 0.0, dead: 0.0, sensitivity: 1.0, snap: false, invert: false,
          type: 2, axis: a - 1, joyNum: j });
        added++;
      }
      if (added) { notes.push(`InputManager: ${added} Rewired joystick axes (Joy1-${o.rewiredJoysticks} Axis1-28) added`); save(t); }
    } else if (ob.classId === C.QualitySettings && o.asyncUploadBuffer) {
      const t = u.readObject(sf, ob);
      for (const q of t.m_QualitySettings) q.asyncUploadBufferSize = o.asyncUploadBuffer;
      notes.push(`QualitySettings.asyncUploadBufferSize = ${o.asyncUploadBuffer} MB`); save(t);
    } else if (ob.classId === C.PlayerSettings) {
      const t = u.readObject(sf, ob);
      let ch = false;
      if (o.mtRendering !== 'keep') { t.m_MTRendering = o.mtRendering === 'on'; notes.push(`PlayerSettings.m_MTRendering = ${t.m_MTRendering ? 'True' : 'False'}`); ch = true; }
      if ('mobileMTRenderingBaked' in t && t.mobileMTRenderingBaked !== t.m_MTRendering) {
        t.mobileMTRenderingBaked = t.m_MTRendering; notes.push(`PlayerSettings.mobileMTRenderingBaked = m_MTRendering (${t.m_MTRendering ? 'True' : 'False'})`); ch = true;
      }
      if ('playerMinOpenGLESVersion' in t && t.playerMinOpenGLESVersion < 2) { t.playerMinOpenGLESVersion = 2; notes.push('PlayerSettings.playerMinOpenGLESVersion = 2 (OpenGL ES 3.0)'); ch = true; }
      if (o.blit !== 'keep') { t.androidBlitType = { always: 0, never: 1, auto: 2 }[o.blit]; notes.push(`PlayerSettings.androidBlitType = ${t.androidBlitType}`); ch = true; }
      if ('androidUseSwappy' in t) { t.androidUseSwappy = false; notes.push('PlayerSettings.androidUseSwappy = False'); ch = true; }
      if (ch) save(t);
    }
  }
  notes.push(`target platform ${old} -> ${PLATFORM}`);
  return { parts: u.save(sf, changed).parts, notes };
}

// ---- the tree (android_data.plan and the actions of main) and the archives (pack)
// A view of a folder: {list(dir) -> [{name, dir}], size(rel) (-1: none), read(rel) -> bytes, slice(rel, off, n)}
const cmpStr = (a, b) => a < b ? -1 : a > b ? 1 : 0;
async function walk(view, dir, sorted = true) {    // os.walk's files under dir (relative to the view), top-down
  const out = [];
  const go = async d => {
    let ents; try { ents = await view.list(d); } catch { return; }
    const fs = ents.filter(e => !e.dir).map(e => e.name), ds = ents.filter(e => e.dir).map(e => e.name);
    if (sorted) { fs.sort(cmpStr); ds.sort(cmpStr); }
    for (const f of fs) out.push(d ? `${d}/${f}` : f);
    for (const x of ds) await go(d ? `${d}/${x}` : x);
  };
  await go(dir);
  return out;
}
const fnmatch = (name, glob) => new RegExp('^' + glob.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.') + '$').test(name);
const DATA_JSON = ['ScriptingAssemblies.json', 'RuntimeInitializeOnLoads.json'];
const lineGe = (uv, l) => { const [a, b] = uv.split('.').map(Number); return a > l[0] || (a === l[0] && b >= l[1]); };
class Unsupported extends Error {}           // data this page leaves to the handheld's setup

// desktop_modules_needed: engine modules whose types the game's assemblies reference by that module but the
// Android player's copy lacks while the desktop copy has them: the desktop copy is shipped
function desktopModulesNeeded(gnames, anames, gopen, aopen) {
  const CM = typeof CLRMETA !== 'undefined' ? CLRMETA : require('./clrmeta.js');
  const types = (open, n) => { const b = open(n); if (!b) return null; try { return new Set(new CM.Assembly(b, n).types().map(t => t.full)); } catch { return null; } };
  const wanted = new Map();
  for (const n of [...gnames].sort(cmpStr)) {
    if (!n.endsWith('.dll') || isEngineModule(n)) continue;
    try {
      const a = new CM.Assembly(gopen(n), n);
      for (let r = 1; r <= a.count(CM.TYPEREF); r++) {
        const sc = a.typerefScope(r);
        if (sc && sc.startsWith('UnityEngine.') && sc.endsWith('Module')) {
          if (!wanted.has(sc + '.dll')) wanted.set(sc + '.dll', new Set());
          wanted.get(sc + '.dll').add(a.typerefName(r));
        }
      }
    } catch { continue; }
  }
  const out = {};
  for (const [mod, names] of wanted) {
    if (!anames.has(mod) || !gnames.has(mod)) continue;
    const have = types(aopen, mod), desk = types(gopen, mod);
    if (!have || !desk) continue;
    const missing = [...names].filter(t => !have.has(t) && desk.has(t)).sort(cmpStr);
    if (missing.length) out[mod] = missing;
  }
  return out;
}

// build(o): the tree android-data writes (its <name>/ and the files beside it), through o.out:
//   put(dst, parts) a written file, link(dst, view, rel) a file taken as it is
// o: {data, pkg, metc, gd: views; unity: the data's Unity version; name; rootFiles; managedFiles: [{name, bytes}];
//     assemblyPatch: {name: async () => bytes}; dropMissing; dllmap: [[dll, target]]; graphicsApis; rewiredJoysticks;
//     blit; mtRendering; asyncUploadBuffer; videoShader (false: --no-video-shader); beside: globs; pluginMap; lz; log}
async function build(o) {
  const { data, pkg, metc, gd, name } = o, log = o.log || (() => {}), D = `${name}/assets/bin/Data`, notes = [], warnings = [];
  const items = [], skipped = [], inlined = [];
  const top = (await data.list('')).filter(e => !e.dir && !e.name.startsWith('.')).map(e => e.name).sort(cmpStr);
  const has = n => top.includes(n);
  for (const n of top) {
    const src = ['data', n];
    if (n === 'globalgamemanagers') items.push([`${D}/${n}`, src, 'ggm']);
    else if (n === 'globalgamemanagers.assets' || n === 'resources.assets' || /^(level\d+|sharedassets\d+\.assets)$/.test(n))
      items.push([`${D}/${n}`, src, has(n + '.resS') ? 'sfinline' : 'sfpatch']);
    else if (n.endsWith('.resS') && has(n.slice(0, -5))) inlined.push(n);
    else if (n.endsWith('.resS') || n.endsWith('.resource') || n === 'boot.config' || DATA_JSON.includes(n)) items.push([`${D}/${n}`, src, 'link']);
    else if ((o.rootFiles || []).includes(n)) items.push([`${name}/${n}`, src, 'link']);
    else skipped.push(n);
  }
  let neon = 0;                              // realtime GI: each SSE file also under its NEON name
  const gi = await walk(data, 'GI');
  for (const rel of gi) {
    const f = rel.split('/').pop(), dir = rel.slice(0, rel.length - f.length);
    if (f.startsWith('.')) continue;
    items.push([`${D}/${rel}`, ['data', rel], 'link']);
    if (f.endsWith('.sse') && !gi.includes(dir + f.slice(0, -4) + '.neo')) { items.push([`${D}/${rel.slice(0, -4)}.neo`, ['data', rel], 'link']); neon++; }
  }
  if ((await pkg.list('Apk').catch(() => [])).some(e => e.dir && e.name === 'MemoryAdvice')) throw new Unsupported('a player with Memory Advice files (Unity 2023.1+)');
  const udrIn = lineGe(o.unity, [2023, 2]) ? 'Resources/unity default resources' : 'unity default resources';
  const udrPkg = await pkg.size('Data/unity default resources') >= 0 ? 'Data/unity default resources' : 'Data/Resources/unity default resources';
  items.push([`${D}/${udrIn}`, ['pkg', udrPkg], 'link']);
  if (o.videoShader !== false && new TextDecoder('latin1').decode(await data.read('Resources/unity_builtin_extra')).includes('Hidden/VideoDecode'))
    throw new Unsupported("the VideoPlayer's Android shader (made from Hidden/VideoDecode)");
  items.push([`${D}/Resources/unity_builtin_extra`, ['data', 'Resources/unity_builtin_extra'], 'sfpatch']);
  // Managed: the Android engine assemblies, the game's own and its extension / class-library assemblies
  const AM = 'Variations/mono/Managed';
  const anames = new Set((await pkg.list(AM)).filter(e => !e.dir).map(e => e.name));
  const gnames = new Set((await data.list('Managed')).filter(e => !e.dir).map(e => e.name));
  const bytesOf = new Map();
  for (const n of gnames) if (n.endsWith('.dll')) bytesOf.set(`g/${n}`, await data.read(`Managed/${n}`));
  for (const n of anames) if (n.endsWith('.dll')) bytesOf.set(`a/${n}`, await pkg.read(`${AM}/${n}`));
  const gopen = n => bytesOf.get(`g/${n}`) || null, aopen = n => bytesOf.get(`a/${n}`) || null;
  const override = new Map((o.managedFiles || []).map(f => [f.name, ['bytes', f.bytes]]));
  for (const [n, why] of Object.entries(desktopModulesNeeded(gnames, anames, gopen, aopen))) {
    if (override.has(n)) continue;
    override.set(n, ['data', `Managed/${n}`]);
    warnings.push(`${n}: the game's own (desktop) copy is shipped: its assemblies use types the Android one lacks (${why.slice(0, 4).join(', ')}${why.length > 4 ? '...' : ''})`);
  }
  const names = new Set(), dropped = [];
  for (const n of [...anames].sort(cmpStr)) {
    if (!n.endsWith('.dll')) continue;
    items.push([`${D}/Managed/${n}`, override.get(n) || ['pkg', `${AM}/${n}`], 'link']);
    override.delete(n); names.add(n);
  }
  const patched = o.assemblyPatch || {};
  for (const n of [...new Set([...gnames, ...override.keys()])].sort(cmpStr)) {
    if (!n.endsWith('.dll')) continue;
    if (isEngineModule(n)) { if (!names.has(n)) dropped.push(n); continue; }
    if (override.has(n)) items.push([`${D}/Managed/${n}`, override.get(n), 'link']);
    else if (patched[n]) items.push([`${D}/Managed/${n}`, ['patch', n], 'patch']);
    else items.push([`${D}/Managed/${n}`, ['data', `Managed/${n}`], 'link']);
    names.add(n);
  }
  for (const rel of await walk(metc, '', false)) items.push([`${D}/Managed/etc/${rel}`, ['metc', rel], rel === 'mono/config' ? 'monoconfig' : 'link']);
  // StreamingAssets -> assets/: every bundle gets the Android platform
  for (const rel of await walk(data, 'StreamingAssets')) {
    const r = rel.slice('StreamingAssets/'.length);
    if (r.split('/').pop().startsWith('.')) continue;
    if (/^aa[/\\]catalog[^/\\]*\.(json|bin)$/.test(r)) throw new Unsupported('an Addressables catalog');
    const head = await data.slice(rel, 0, 8);
    items.push([`${name}/assets/${r}`, ['data', rel], new TextDecoder('latin1').decode(head.subarray(0, 7)) === 'UnityFS' ? 'bundle' : 'link']);
  }
  for (const e of (await gd.list('')).filter(x => !x.dir).map(x => x.name).sort(cmpStr))
    if ((o.beside || ['goggame-*.info']).some(g => fnmatch(e, g))) items.push([e, ['gd', e], 'link']);
  // the actions
  const views = { data, pkg, metc, gd }, stats = {}, report = { bundles: {}, files: {} }, count = k => { stats[k] = (stats[k] || 0) + 1; };
  let ggm = null;
  for (const [dst, src, act] of items) {
    const rel = src[1];
    if (act === 'link') {
      if (src[0] === 'bytes') await o.out.put(dst, [src[1]]); else await o.out.link(dst, views[src[0]], rel);
      count('linked');
    } else if (act === 'sfpatch') {
      const b = Uint8Array.from(await data.read(rel)), old = patchSfPlatform(b.subarray(0, 128));
      await o.out.put(dst, [b]); report.files[dst] = `platform ${old} -> ${PLATFORM}`; count('platform');
    } else if (act === 'sfinline') {
      const r = await inlineSerialized(await data.read(rel), rel, (n, off, size) => data.slice(n, off, size));
      await o.out.put(dst, r.parts);
      report.files[dst] = `platform -> ${PLATFORM}, ${r.inlined} textures inlined`;
      for (const k of [...r.still].sort(cmpStr)) { await o.out.link(dst.replace(/[^/]+$/, k), data, k); report.files[dst] += `; ${k} kept`; }
      count('inlined');
    } else if (act === 'bundle') {
      const raw = await data.read(rel), b = new Bundle(raw, o.lz);
      if (b.nodes.some(nd => nd[3].endsWith('.resS'))) throw new Unsupported(`${rel}: a bundle with streamed textures`);
      const r = patchBundlePlatform(raw, o.lz);
      await o.out.put(dst, [r.buf]); report.bundles[dst.split('/').pop()] = r.done; count('bundle');
      if (r.done.some(x => x[2] === 'rewrite')) log(`  ${dst}: bundle rewritten (no same-size encoding of the patched block)`);
    } else if (act === 'ggm') {
      const r = editGlobalGameManagers(await data.read(rel), { graphicsApis: o.graphicsApis || [11], managedNames: names,
        dropMissing: o.dropMissing || [], rewiredJoysticks: o.rewiredJoysticks ?? 4, blit: o.blit || 'never', mtRendering: o.mtRendering || 'keep',
        asyncUploadBuffer: o.asyncUploadBuffer || 0 });
      ggm = cat(...r.parts); await o.out.put(dst, [ggm]); notes.push(...r.notes);
      log('globalgamemanagers: ' + r.notes.join('; '));
    } else if (act === 'patch') {
      await o.out.put(dst, [await patched[rel]()]); count('assembly patched');
    } else if (act === 'monoconfig') {
      await o.out.put(dst, [new TextEncoder().encode(monoConfigText(new TextDecoder().decode(await metc.read(rel)), o.dllmap || []))]);
      count('written');
    }
  }
  // native plugins (plugins.js) over the game's own Managed folder
  const PL = typeof PLUGINS !== 'undefined' ? PLUGINS : require('./plugins.js');
  const plugins = PL.survey(gnames, gopen);
  const pluginMap = PL.mapText(plugins, PL.mergeMap(plugins, o.pluginMap || {}));
  log(`placed: ${JSON.stringify(stats)}; skipped source files: ${JSON.stringify(skipped)}; engine modules not shipped: ${JSON.stringify(dropped)}; ` +
    `.resS moved into their serialized files: ${inlined.length}`);
  return { items, notes, warnings, skipped, dropped, ggm, plugins, pluginMap, report, neon };
}

// build_id: BuildSettings.buildGUID of the written globalgamemanagers, as a UUID
function buildId(ggm) {
  const u = U(), sf = u.parseSerialized(ggm, 'globalgamemanagers');
  for (const ob of sf.objects) if (ob.classId === C.BuildSettings) {
    const g = u.readObject(sf, ob).buildGUID;
    if (g && [0, 1, 2, 3].some(i => g[`data[${i}]`])) {
      const h = [0, 1, 2, 3].map(i => (g[`data[${i}]`] >>> 0).toString(16).padStart(8, '0')).join('');
      return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
    }
  }
  throw new Unsupported('no BuildSettings.buildGUID (Unity 2022+: PlayerSettings.productGUID)');
}
function pyJson1(v, ind = '') {             // json.dump(v, indent=1)
  const i2 = ind + ' ';
  if (Array.isArray(v)) return v.length ? `[\n${v.map(x => i2 + pyJson1(x, i2)).join(',\n')}\n${ind}]` : '[]';
  if (v && typeof v === 'object') { const k = Object.keys(v); return k.length ? `{\n${k.map(x => `${i2}${JSON.stringify(x)}: ${pyJson1(v[x], i2)}`).join(',\n')}\n${ind}}` : '{}'; }
  return JSON.stringify(v);
}
// pack: tree = Map(dst -> size) of the files under <name>/ and beside it. Returns {archives: {file name: [[arcname,
// dst | bytes]]}, zipFiles: [[rel in the zip folder, dst | bytes]], res}
function pack({ tree, name, pkgName, versionCode = 1, unity, bid, loose = [], minSdk = 0, pluginMap }) {
  const R = `${name}/`, files = [];
  const node = () => ({ f: [], d: new Map() }), root = node();
  for (const dst of tree.keys()) {
    if (!dst.startsWith(R)) continue;
    const parts = dst.slice(R.length).split('/'); let n = root;
    for (const p of parts.slice(0, -1)) { if (!n.d.has(p)) n.d.set(p, node()); n = n.d.get(p); }
    n.f.push(parts[parts.length - 1]);
  }
  // os.walk order: a folder's files (sorted), then its folders (sorted)
  const go = (n, pre) => { for (const f of [...n.f].sort(cmpStr)) files.push(pre + f); for (const d of [...n.d.keys()].sort(cmpStr)) go(n.d.get(d), pre + d + '/'); };
  go(root, '');
  const enc = new TextEncoder(), size = a => tree.get(R + a);
  let apk = files.filter(a => a.startsWith('assets/bin/'));
  let obb = files.filter(a => a.startsWith('assets/') && !a.startsWith('assets/bin/'));
  const looseE = obb.filter(a => loose.some(g => fnmatch(a.slice(7), g) || fnmatch(a.split('/').pop(), g)));
  obb = obb.filter(a => !looseE.includes(a));
  const beside = files.filter(a => !a.startsWith('assets/'));
  const res = { build_id: bid, package: pkgName, version_code: versionCode, unity_version: unity };
  const marker = [['assets/' + bid, new Uint8Array(0)]], guid = enc.encode(bid);
  if (lineGe(unity, [2021, 1])) { apk.push('assets/unity_obb_guid'); marker.push(['unity_obb_guid', guid]); res.unity_obb_guid = bid; }
  const esize = a => (a === 'assets/unity_obb_guid' ? guid.length : size(a)) + 30 + 46 + 2 * a.length + 8;
  // split_big_apk: later scenes and resources go to the OBBs until the APK fits
  const limit = ZIP_LIMIT - (64 << 20), pre = 'assets/bin/Data/';
  let total = apk.reduce((s, a) => s + esize(a), 0);
  if (total > limit) {
    const first = /^(Managed\/|Resources\/|boot\.config$|globalgamemanagers|unity default resources$|level0$|sharedassets0\.|ScriptingAssemblies\.json$|RuntimeInitializeOnLoads\.json$|unity_obb_guid$)/;
    const movable = apk.filter(a => a.startsWith(pre) && a !== 'assets/unity_obb_guid' && !first.test(a.slice(pre.length))).sort((x, y) => esize(y) - esize(x));
    const moved = [];
    for (const a of movable) { if (total <= limit) break; moved.push(a); total -= esize(a); }
    if (total > limit) throw new Unsupported('the APK would pass 2 GiB');
    apk = apk.filter(a => !moved.includes(a)); obb = [...moved.sort(cmpStr), ...obb];
  }
  const ent = a => a === 'assets/unity_obb_guid' ? [a, guid] : [a, R + a];
  const main = [...marker], rest = [];
  let osz = 0;
  for (const a of obb) {
    const sz = size(a) + 30 + 46 + 2 * a.length + 8;
    if (osz + sz < ZIP_LIMIT - (16 << 20) && !rest.length) { main.push([a, R + a]); osz += sz; } else rest.push([a, R + a]);
  }
  res.apk = `${name}.apk`; res.main_obb = `main.${versionCode}.${pkgName}.obb`;
  const archives = { [res.apk]: apk.map(ent), [res.main_obb]: main };
  if (rest.length) { res.patch_obb = `patch.${versionCode}.${pkgName}.obb`; archives[res.patch_obb] = [...marker, ...rest]; }
  const zipFiles = [];
  for (const a of files) if (a.startsWith('assets/bin/Data/Managed/etc/')) zipFiles.push([a, R + a]);
  for (const dst of [...tree.keys()].filter(d => !d.includes('/')).sort(cmpStr)) zipFiles.push([dst, dst]);    // (--beside files)
  for (const a of beside) zipFiles.push([a, R + a]);
  for (const a of looseE) zipFiles.push([a, R + a]);
  zipFiles.push(['plugins.map', enc.encode(pluginMap)]);
  if (looseE.length) res.loose_assets = looseE;
  if (minSdk) res.min_sdk = minSdk;
  zipFiles.push(['android_package.json', enc.encode(pyJson1({ ...res, meta_data: { 'unity.build-id': bid } }))]);
  return { archives, zipFiles, res };
}

return { PLATFORM, sfPlatform, patchSfPlatform, monoConfigText, zipChunks, zipSize, ZIP_LIMIT, Bundle, patchBundlePlatform,
  build, buildId, pack, walk, Unsupported,
  lz4Exact, lz4Repoint, inlineSerialized, editGlobalGameManagers, isEngineModule, splitAssemblies };
})();
if (typeof module !== 'undefined') module.exports = ANDROID;
