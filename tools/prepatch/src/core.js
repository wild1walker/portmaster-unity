// ---- core: no page dependencies (runs in a browser or in Node 18+)
'use strict';
const CORE = (() => {
const le16 = (b, o) => b[o] | b[o + 1] << 8;
const le32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;
const le64 = (b, o) => le32(b, o) + le32(b, o + 4) * 2 ** 32;
const be16 = (b, o) => b[o] << 8 | b[o + 1];
const be64 = (b, o) => ((b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]) >>> 0) * 2 ** 32 +
  ((b[o + 4] << 24 | b[o + 5] << 16 | b[o + 6] << 8 | b[o + 7]) >>> 0);
const utf8 = new TextDecoder('utf-8');
const enc = new TextEncoder();
const bytes = async (blob, off, len) => new Uint8Array(await blob.slice(off, off + len).arrayBuffer());

// ---- CRC-32 (slicing by 4)
const T = (() => {
  const t = new Uint32Array(1024);
  for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ c >>> 1 : c >>> 1; t[i] = c; }
  for (let i = 0; i < 256; i++) for (let s = 1; s < 4; s++) t[s * 256 + i] = t[(s - 1) * 256 + i] >>> 8 ^ t[t[(s - 1) * 256 + i] & 255];
  return t;
})();
function crc32(crc, b) {
  let c = ~crc >>> 0, i = 0;
  const n = b.length - 3;
  for (; i < n; i += 4) {
    c ^= b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24;
    c = T[768 + (c & 255)] ^ T[512 + (c >>> 8 & 255)] ^ T[256 + (c >>> 16 & 255)] ^ T[c >>> 24];
  }
  for (; i < b.length; i++) c = T[(c ^ b[i]) & 255] ^ c >>> 8;
  return ~c >>> 0;
}

// ---- a stream read in exact pieces
class Reader {
  constructor(stream) { this.r = stream.getReader(); this.parts = []; this.off = 0; this.size = 0; this.done = false; }
  async fill(n) {
    while (this.size < n && !this.done) {
      const { value, done } = await this.r.read();
      if (done) { this.done = true; break; }
      if (value.length) { this.parts.push(value); this.size += value.length; }
    }
  }
  take(n) {
    const out = new Uint8Array(n); let k = 0;
    while (k < n) {
      const p = this.parts[0], m = Math.min(n - k, p.length - this.off);
      out.set(p.subarray(this.off, this.off + m), k); k += m; this.off += m; this.size -= m;
      if (this.off === p.length) { this.parts.shift(); this.off = 0; }
    }
    return out;
  }
  async exact(n) { await this.fill(n); if (this.size < n) throw new Error('the archive ends early'); return this.take(n); }
  async *chunks(n) {               // n bytes as they come, without copying
    while (n > 0) {
      if (!this.size) { await this.fill(1); if (!this.size) throw new Error('the archive ends early'); }
      const p = this.parts[0], m = Math.min(n, p.length - this.off);
      const c = p.subarray(this.off, this.off + m);
      this.off += m; this.size -= m; n -= m;
      if (this.off === p.length) { this.parts.shift(); this.off = 0; }
      yield c;
    }
  }
  async skip(n) { for await (const _ of this.chunks(n)) { /* skipped */ } }
  cancel() { return this.r.cancel().catch(() => {}); }
}
const counted = (stream, onBytes) => stream.pipeThrough(new TransformStream({
  transform(c, ctl) { onBytes(c.length); ctl.enqueue(c); } }));
async function peek(stream, n) {             // {head: at least the first n bytes, rest: the whole stream again}
  const r = stream.getReader(), got = [];
  let size = 0;
  while (size < n) { const { value, done } = await r.read(); if (done) break; got.push(value); size += value.length; }
  const head = new Uint8Array(size);
  got.reduce((o, c) => (head.set(c, o), o + c.length), 0);
  const rest = new ReadableStream({
    start(ctl) { for (const c of got) ctl.enqueue(c); },
    async pull(ctl) { const { value, done } = await r.read(); if (done) ctl.close(); else ctl.enqueue(value); },
    cancel(why) { return r.cancel(why); },
  });
  return { head, rest };
}

// ---- a small XML reader (xar's table of contents, PackageInfo, Distribution)
function xml(text) {
  const root = { tag: '#root', attrs: {}, kids: [], text: '' }, stack = [root];
  const rx = /<!--[\s\S]*?-->|<\?[\s\S]*?\?>|<!\[CDATA\[([\s\S]*?)\]\]>|<(\/?)([\w:.-]+)((?:\s+[\w:.-]+\s*=\s*(?:"[^"]*"|'[^']*'))*)\s*(\/?)>|([^<]+)/g;
  const unesc = s => s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&amp;/g, '&');
  let m;
  while ((m = rx.exec(text))) {
    const top = stack[stack.length - 1];
    if (m[1] !== undefined) top.text += m[1];
    else if (m[6] !== undefined) top.text += unesc(m[6]);
    else if (m[3]) {
      if (m[2]) { stack.pop(); continue; }
      const attrs = {};
      for (const a of m[4].matchAll(/([\w:.-]+)\s*=\s*(?:"([^"]*)"|'([^']*)')/g)) attrs[a[1]] = unesc(a[2] ?? a[3]);
      const el = { tag: m[3], attrs, kids: [], text: '' };
      top.kids.push(el);
      if (!m[5]) stack.push(el);
    }
  }
  return root;
}
const kid = (el, tag) => el && el.kids.find(k => k.tag === tag);
const kidText = (el, tag) => { const k = kid(el, tag); return k ? k.text.trim() : ''; };
const find = (el, tag) => { if (!el) return null; if (el.tag === tag) return el; for (const k of el.kids) { const f = find(k, tag); if (f) return f; } return null; };

// ---- zip reading (ZIP64; self-extracting archives such as GOG's Linux installers)
async function zipOpen(file) {
  const tail = Math.min(file.size, 65557 + 20);
  const b = await bytes(file, file.size - tail, tail);
  let i = b.length - 22;
  while (i >= 0 && le32(b, i) !== 0x06054b50) i--;
  if (i < 0) throw new Error(`${file.name}: not a zip file`);
  const eocd = file.size - tail + i;
  let count = le16(b, i + 10), cdSize = le32(b, i + 12), cdOff = le32(b, i + 16), delta;
  if (i >= 20 && le32(b, i - 20) === 0x07064b50) {
    const stated = le64(b, i - 20 + 8), actual = eocd - 20 - 56;
    const e64 = await bytes(file, actual, 56);
    if (le32(e64, 0) !== 0x06064b50) throw new Error(`${file.name}: broken ZIP64 record`);
    count = le64(e64, 32); cdSize = le64(e64, 40); cdOff = le64(e64, 48);
    delta = actual - stated;
  } else delta = eocd - cdSize - cdOff;
  const cd = await bytes(file, cdOff + delta, cdSize);
  const entries = [];
  for (let p = 0, k = 0; k < count; k++) {
    if (le32(cd, p) !== 0x02014b50) throw new Error(`${file.name}: broken central directory`);
    const nlen = le16(cd, p + 28), xlen = le16(cd, p + 30), clen = le16(cd, p + 32);
    const e = { madeBy: le16(cd, p + 4), flags: le16(cd, p + 8), method: le16(cd, p + 10), time: le16(cd, p + 12),
      date: le16(cd, p + 14), crc: le32(cd, p + 16), csize: le32(cd, p + 20), usize: le32(cd, p + 24),
      eattr: le32(cd, p + 38), lho: le32(cd, p + 42), name: utf8.decode(cd.subarray(p + 46, p + 46 + nlen)) };
    for (let x = p + 46 + nlen, end = x + xlen; x + 4 <= end;) {
      const id = le16(cd, x), sz = le16(cd, x + 2);
      if (id === 1) {
        let q = x + 4;
        if (e.usize === 0xFFFFFFFF) { e.usize = le64(cd, q); q += 8; }
        if (e.csize === 0xFFFFFFFF) { e.csize = le64(cd, q); q += 8; }
        if (e.lho === 0xFFFFFFFF) { e.lho = le64(cd, q); }
      }
      x += 4 + sz;
    }
    e.lho += delta;
    entries.push(e);
    p += 46 + nlen + xlen + clen;
  }
  return entries;
}
async function zipData(file, e) {               // the entry's stored bytes, as a Blob
  const h = await bytes(file, e.lho, 30);
  if (le32(h, 0) !== 0x04034b50) throw new Error(`${e.name}: broken local header`);
  const start = e.lho + 30 + le16(h, 26) + le16(h, 28);
  return file.slice(start, start + e.csize);
}
async function zipStream(file, e, onBytes) {    // the entry's contents, as a stream
  let s = (await zipData(file, e)).stream();
  if (onBytes) s = counted(s, onBytes);
  if (e.method === 0) return s;
  if (e.method === 8) return s.pipeThrough(new DecompressionStream('deflate-raw'));
  throw new Error(`${e.name}: compression method ${e.method} is not supported`);
}
async function zipText(file, entries, name) {
  const e = entries.find(x => x.name === name);
  if (!e) return '';
  return new Response(await zipStream(file, e)).text();
}

// ---- the port: what its setup needs
function shellEnv(text) {       // VAR=value lines as the setup's bash reads them (arrays skipped)
  const env = {};
  for (const line of text.split('\n')) {
    const m = line.match(/^\s*([A-Za-z_]\w*)=(.*)$/);
    if (!m || m[2].startsWith('(')) continue;
    let v = '', s = m[2], i = 0;
    while (i < s.length && !/\s/.test(s[i])) {
      if (s[i] === "'") { const j = s.indexOf("'", i + 1); v += s.slice(i + 1, j < 0 ? s.length : j); i = j < 0 ? s.length : j + 1; }
      else if (s[i] === '"') { i++; while (i < s.length && s[i] !== '"') { if (s[i] === '\\' && i + 1 < s.length) i++; v += s[i++]; } i++; }
      else if (s[i] === '\\') { v += s[i + 1] || ''; i += 2; }
      else v += s[i++];
    }
    env[m[1]] = v;
  }
  return env;
}

async function inspectPort(file) {
  const entries = await zipOpen(file);
  const names = new Set(entries.map(e => e.name));
  const ps = entries.find(e => /^[^/]+\/setup\/patchscript$/.test(e.name));
  const portdir = ps ? ps.name.split('/')[0] :
    (entries.find(e => /^[^/]+\/port\.json$/.test(e.name)) || { name: '' }).name.split('/')[0];
  const rd = n => zipText(file, entries, n);
  let pj = {};
  try { pj = JSON.parse(await rd(`${portdir}/port.json`) || '{}'); } catch { /* none */ }
  const attr = pj.attr || {};
  const env = shellEnv(await rd(`${portdir}/setup/setup.env`));
  const script = ps ? await rd(ps.name) : '';
  // the patcher's questions: the file the launcher (a .sh beside the port folder) names in PATCHER_QUESTIONS
  let qfile = 'setup/questions.lua';
  for (const e of entries.filter(x => /^[^/]+\.sh$/.test(x.name))) {
    const m = (await rd(e.name)).match(/PATCHER_QUESTIONS=["']?\$\{?GAMEDIR\}?\/([^"'\s]+)/);
    if (m) { qfile = m[1]; break; }
  }
  const info = {
    zip: file.name, portdir, entries, title: env.PORT_TITLE || attr.title || portdir,
    porter: attr.porter || [], store: attr.store || [], inst: attr.inst || '',
    route: env.PORT_ROUTE || (ps ? 'its own setup' : ''), standard: !!env.PORT_ROUTE,
    gameWhat: env.GAME_WHAT || '', gameBuild: env.GAME_BUILD || '', gameData: env.GAME_DATA || '',
    gameSizeMb: +(env.GAME_SIZE_MB || 0), unity: (env.UNITY_VERSIONS || '').split(/\s+/).filter(Boolean),
    gamedataTxt: await rd(`${portdir}/gamedata/PUT_GAME_FILES_HERE.txt`),
    ready: env.READY || '', hasSetup: !!ps, problems: [], appName: '',
    env, script, qfile, lua: await rd(`${portdir}/${qfile}`), read: n => zipText(file, entries, `${portdir}/${n}`),
    readBytes: async n => { const e = entries.find(x => x.name === `${portdir}/${n}`); return e ? new Uint8Array(await new Response(await zipStream(file, e)).arrayBuffer()) : null; },
  };
  if (!ps) info.problems.push('This zip has no setup/patchscript: the port has no first-launch setup, or it is already set up.');
  if (!info.standard && ps) {
    // a port's own setup: the names and the Unity version it checks for, read from its script
    const app = script.match(/"\$GD"\/(?:\*\/)?([^"\s/*$]+\.app)\b/);
    if (app) info.appName = app[1];
    const v = script.match(/"(\d{4}\.\d+\.\d+[abfpx]\d+|[45]\.\d+\.\d+[abfpx]\d+)"[^\n]*globalgamemanagers/);
    if (v) info.unity = [v[1]];
    if (!info.gameWhat) info.gameWhat = info.gamedataTxt.trim().split('\n').slice(0, 3).join(' ');
  }
  if (env.PORT_ROUTE === 'apk') info.problems.push("This port takes the game's own Android packages: they go into gamedata/ as they are (there is nothing to unpack).");
  if (names.has(`${portdir}/${info.ready}`) && info.ready) info.problems.push('This zip is already set up.');
  return info;
}

// ---- macOS installers (.pkg): xar, then cpio payloads (gzip; pbzx/xz cannot be read in a browser)
async function pkgOpen(file) {
  const h = await bytes(file, 0, 28);
  if (utf8.decode(h.subarray(0, 4)) !== 'xar!') throw new Error(`${file.name}: not a macOS installer (xar)`);
  const hsize = be16(h, 4), tocC = be64(h, 8);
  const toc = xml(await new Response(file.slice(hsize, hsize + tocC).stream().pipeThrough(new DecompressionStream('deflate'))).text());
  const heap = hsize + tocC, files = new Map();
  const walk = (el, pre) => { for (const fe of el.kids.filter(k => k.tag === 'file')) {
    const p = pre + kidText(fe, 'name'); files.set(p, fe); walk(fe, p + '/'); } };
  walk(find(toc, 'toc'), '');
  const member = (fe, onBytes) => {
    const d = kid(fe, 'data');
    const off = heap + +kidText(d, 'offset'), len = +kidText(d, 'length');
    const style = (kid(d, 'encoding') || { attrs: {} }).attrs.style || 'application/octet-stream';
    let s = file.slice(off, off + len).stream();
    if (onBytes) s = counted(s, onBytes);
    if (style === 'application/x-gzip') return s.pipeThrough(new DecompressionStream('deflate'));
    if (style === 'application/octet-stream') return s;
    throw new Error(`xar encoding ${style} cannot be read in a browser`);
  };
  const hasData = p => files.has(p) && !!kid(files.get(p), 'data');
  const text = async p => hasData(p) ? new Response(member(files.get(p))).text() : '';
  return { files, member, hasData, text };
}
async function payload(stream) {              // a cpio stream from a Payload or Scripts member
  const { head, rest } = await peek(stream, 4);
  if (head[0] === 0x1f && head[1] === 0x8b) return rest.pipeThrough(new DecompressionStream('gzip'));
  if (utf8.decode(head.subarray(0, 4)) === 'pbzx') { rest.cancel(); throw new Error("Apple's pbzx (xz) compression cannot be read in a browser"); }
  return rest;
}
async function* cpio(stream) {                // {name, mode, size, r} of each member; read r's data before the next
  const r = new Reader(stream);
  const hex = (h, o) => parseInt(utf8.decode(h.subarray(o, o + 8)), 16);
  const oct = (h, o, n) => parseInt(utf8.decode(h.subarray(o, o + n)), 8);
  for (;;) {
    const magic = utf8.decode(await r.exact(6));
    let mode, size, name, pad = 0;
    if (magic === '070707') {
      const h = await r.exact(70);
      mode = oct(h, 12, 6); const ns = oct(h, 53, 6); size = oct(h, 59, 11);
      name = utf8.decode((await r.exact(ns)).subarray(0, ns - 1));
    } else if (magic === '070701' || magic === '070702') {
      const h = await r.exact(104);
      mode = hex(h, 8); size = hex(h, 48); const ns = hex(h, 88);
      name = utf8.decode((await r.exact(ns)).subarray(0, ns - 1));
      await r.exact((4 - (110 + ns) % 4) % 4); pad = (4 - size % 4) % 4;
    } else throw new Error(`unknown cpio header ${JSON.stringify(magic)}`);
    if (name === 'TRAILER!!!') { await r.cancel(); return; }
    const m = { name, mode, size, r, used: false };
    yield m;
    if (!m.used) await r.skip(size);
    if (pad) await r.exact(pad);
  }
}
function cleanPath(p) {                       // a relative path, or null for what is not the game's
  const parts = [];
  for (const s of p.replace(/\\/g, '/').split('/')) {
    if (!s || s === '.') continue;
    if (s === '..') return null;
    parts.push(s);
  }
  const base = parts[parts.length - 1] || '';
  if (!parts.length || base.startsWith('._') || base === '.DS_Store' || parts.includes('__MACOSX')) return null;
  return parts.join('/');
}

// ---- zip writing: positioned header updates, ZIP64 where needed, file modes kept
class ZipWriter {
  constructor(sink) { this.sink = sink; this.pos = 0; this.cd = []; this.names = new Set(); this.now = dosTime(new Date()); }
  async put(b) { await this.sink.write(b); this.pos += b.length; }
  header(name, method, flags, time, date, z64) {
    const n = enc.encode(name), x = z64 ? 20 : 0, h = new Uint8Array(30 + n.length + x), v = new DataView(h.buffer);
    v.setUint32(0, 0x04034b50, true); v.setUint16(4, z64 ? 45 : 20, true); v.setUint16(6, flags | 0x0800, true);
    v.setUint16(8, method, true); v.setUint16(10, time, true); v.setUint16(12, date, true);
    v.setUint16(26, n.length, true); v.setUint16(28, x, true); h.set(n, 30);
    if (z64) { v.setUint16(30 + n.length, 1, true); v.setUint16(32 + n.length, 16, true); }
    return { h, v, n, z64 };
  }
  setSizes(hd, crc, csize, usize) {
    const { v, n, z64 } = hd;
    v.setUint32(14, crc, true);
    if (z64) {
      v.setUint32(18, 0xFFFFFFFF, true); v.setUint32(22, 0xFFFFFFFF, true);
      v.setBigUint64(34 + n.length, BigInt(usize), true); v.setBigUint64(42 + n.length, BigInt(csize), true);
    } else { v.setUint32(18, csize, true); v.setUint32(22, usize, true); }
  }
  // an entry of another zip, copied as it is stored
  async copy(file, e) {
    if (this.names.has(e.name)) return;
    this.names.add(e.name);
    const off = this.pos, z64 = e.usize >= 0xFFFFFFFF || e.csize >= 0xFFFFFFFF;
    const hd = this.header(e.name, e.method, e.flags & 0x0800, e.time, e.date, z64);
    this.setSizes(hd, e.crc, e.csize, e.usize);
    await this.put(hd.h);
    const r = (await zipData(file, e)).stream().getReader();
    for (;;) { const { value, done } = await r.read(); if (done) break; await this.put(value); }
    this.cd.push({ name: hd.n, madeBy: e.madeBy, method: e.method, time: e.time, date: e.date, crc: e.crc,
      csize: e.csize, usize: e.usize, eattr: e.eattr, off });
  }
  // a new entry from a stream of chunks of known size; deflated when compress
  async add(name, size, mode, chunks, compress, onData) {
    this.names.add(name);
    const off = this.pos, method = compress && size > 0 ? 8 : 0, z64 = size > 0xF0000000;
    const hd = this.header(name, method, 0, this.now.time, this.now.date, z64);
    await this.put(hd.h);
    let crc = 0, usize = 0, csize = 0;
    if (method === 0) {
      for await (const c of chunks) { crc = crc32(crc, c); usize += c.length; onData && onData(c, usize); await this.put(c); }
      csize = usize;
    } else {
      const cs = new CompressionStream('deflate-raw'), w = cs.writable.getWriter();
      const pump = (async () => {
        for await (const c of chunks) { crc = crc32(crc, c); usize += c.length; onData && onData(c, usize); await w.write(c); }
        await w.close();
      })();
      const drain = (async () => {
        const r = cs.readable.getReader();
        for (;;) { const { value, done } = await r.read(); if (done) break; csize += value.length; await this.put(value); }
      })();
      await Promise.all([pump, drain]);
    }
    if (usize !== size) throw new Error(`${name}: ${usize} bytes read, ${size} expected`);
    if (!z64 && csize >= 0xFFFFFFFF) throw new Error(`${name}: too large to compress in one piece; turn compression off`);
    this.setSizes(hd, crc, csize, usize);
    await this.sink.rewrite(off, hd.h);
    this.cd.push({ name: hd.n, madeBy: 3 << 8 | 45, method, time: this.now.time, date: this.now.date, crc, csize, usize,
      eattr: ((mode & 0o777 || 0o644) | 0o100000) << 16 >>> 0, off });
  }
  async finish() {
    const cdOff = this.pos;
    for (const e of this.cd) {
      const big = [e.usize >= 0xFFFFFFFF && e.usize, e.csize >= 0xFFFFFFFF && e.csize, e.off >= 0xFFFFFFFF && e.off].filter(x => x !== false);
      const x = big.length ? 4 + 8 * big.length : 0;
      const h = new Uint8Array(46 + e.name.length + x), v = new DataView(h.buffer);
      v.setUint32(0, 0x02014b50, true); v.setUint16(4, e.madeBy, true); v.setUint16(6, x ? 45 : 20, true);
      v.setUint16(8, 0x0800, true); v.setUint16(10, e.method, true); v.setUint16(12, e.time, true); v.setUint16(14, e.date, true);
      v.setUint32(16, e.crc, true);
      v.setUint32(20, e.csize >= 0xFFFFFFFF ? 0xFFFFFFFF : e.csize, true);
      v.setUint32(24, e.usize >= 0xFFFFFFFF ? 0xFFFFFFFF : e.usize, true);
      v.setUint16(28, e.name.length, true); v.setUint16(30, x, true);
      v.setUint32(38, e.eattr, true); v.setUint32(42, e.off >= 0xFFFFFFFF ? 0xFFFFFFFF : e.off, true);
      h.set(e.name, 46);
      if (x) {
        let q = 46 + e.name.length; v.setUint16(q, 1, true); v.setUint16(q + 2, 8 * big.length, true); q += 4;
        for (const b of big) { v.setBigUint64(q, BigInt(b), true); q += 8; }
      }
      await this.put(h);
    }
    const cdSize = this.pos - cdOff, n = this.cd.length;
    if (n >= 0xFFFF || cdOff >= 0xFFFFFFFF || cdSize >= 0xFFFFFFFF) {
      const e64 = new Uint8Array(56 + 20), v = new DataView(e64.buffer), at = this.pos;
      v.setUint32(0, 0x06064b50, true); v.setBigUint64(4, 44n, true); v.setUint16(12, 45, true); v.setUint16(14, 45, true);
      v.setBigUint64(24, BigInt(n), true); v.setBigUint64(32, BigInt(n), true);
      v.setBigUint64(40, BigInt(cdSize), true); v.setBigUint64(48, BigInt(cdOff), true);
      v.setUint32(56, 0x07064b50, true); v.setBigUint64(64, BigInt(at), true); v.setUint32(72, 1, true);
      await this.put(e64);
    }
    const e = new Uint8Array(22), v = new DataView(e.buffer);
    v.setUint32(0, 0x06054b50, true);
    v.setUint16(8, Math.min(n, 0xFFFF), true); v.setUint16(10, Math.min(n, 0xFFFF), true);
    v.setUint32(12, Math.min(cdSize, 0xFFFFFFFF), true); v.setUint32(16, Math.min(cdOff, 0xFFFFFFFF), true);
    await this.put(e);
    await this.sink.close();
    return this.pos;
  }
}
function dosTime(d) {
  return { time: d.getHours() << 11 | d.getMinutes() << 5 | d.getSeconds() >> 1,
           date: (d.getFullYear() - 1980) << 9 | (d.getMonth() + 1) << 5 | d.getDate() };
}

// ---- what a source becomes in gamedata/
function kindOf(name, isDir) {
  const l = name.toLowerCase();
  if (isDir) return l.endsWith('.app') ? 'macOS app (folder)' : 'folder';
  if (l.endsWith('.pkg')) return 'macOS installer';
  if (l.endsWith('.zip')) return 'zip archive';
  if (l.endsWith('.sh')) return "GOG's Linux installer";
  if (/^unitysetup.*\.exe$/.test(l)) return "Unity's installer";
  if (/\.(apk|xapk|apks|aab|obb)$/.test(l)) return 'Android package';
  if (/\.(7z|rar|tar|tgz|txz|tar\.(gz|xz|bz2))$/.test(l)) return 'archive (the handheld unpacks it)';
  return 'file';
}
const VERSION_RX = /(20\d\d\.\d+\.\d+[abfpx]\d+|[2-5]\.\d+\.\d+[abfpx]\d+|6000\.\d+\.\d+[abfpx]\d+)/;

// stage(): the game's files, unpacked, into the stage (stage.js) under their gamedata/ paths
async function stage({ port, sources, stage, log, tick }) {
  const addGame = (rel, size, mode, chunks) => stage.put(rel, size, mode, chunks);
  // as the setup does: installers smallest last on the device win, so here they go first and the
  // larger ones skip what is already in (the DLC's files over the game's)
  const ordered = [...sources].sort((a, b) => (a.kind === 'pkg' && b.kind === 'pkg') ? a.size - b.size : 0);
  for (const s of ordered) {
    if (s.kind === 'pkg') {
      try { await addPkg(s, port, addGame, tick, log); continue; }
      catch (ex) { if (!ex.unreadable) throw ex; log(`${s.name}: ${ex.message}: put in gamedata/ as it is (the handheld unpacks it)`); }
      stage.putFile(s.name, s.file, 0o644); tick(s.size); continue;
    }
    if (s.kind === 'zip') {
      let entries;
      try { entries = await zipOpen(s.file); } catch (ex) { entries = null; log(`${s.name}: ${ex.message}: put in as it is`); }
      if (entries) {
        let n = 0;
        for (const e of entries) {
          const rel = cleanPath(e.name);
          if (!rel || e.name.endsWith('/') || (e.eattr >>> 16 & 0o170000) === 0o120000) { tick(e.csize); continue; }
          const st = await zipStream(s.file, e, tick);
          if (await addGame(rel, e.usize, (e.madeBy >> 8) === 3 ? e.eattr >>> 16 : 0o644, readerChunks(st, e.usize))) n++;
        }
        log(`${s.name}: ${n} files unpacked`);
        continue;
      }
      stage.putFile(s.name, s.file, 0o644); tick(s.size); continue;
    }
    if (s.kind === 'folder') {                        // (read where it is: nothing to unpack)
      let n = 0;
      for (const f of s.files) {
        const rel = cleanPath(f.path);
        tick(f.file.size);
        if (rel && stage.putFile(rel, f.file, 0o644)) n++;
      }
      log(`${s.name}/: ${n} files`);
      continue;
    }
    stage.putFile(s.name, s.file, 0o644);
    tick(s.size);
    log(`${s.name}: put in as it is`);
  }
}

// writeZip(): the port zip's own entries, then the stage under <port>/gamedata/ (Unity versions noted)
async function writeZip({ port, stage, sink, compress, log, tick, drop }) {
  const zw = new ZipWriter(sink), GD = `${port.portdir}/gamedata/`;
  const found = [];
  // a staged ../<path> goes into the port's folder, in place of the port's own file of that name
  const where = rel => rel.startsWith('../') ? `${port.portdir}/${rel.slice(3)}` : GD + rel;
  const ours = new Set([...stage.entries.keys()].filter(r => r.startsWith('../')).map(where));
  log(`port zip: ${port.entries.length} entries`);
  for (const e of port.entries) { if (!(drop && drop.has(e.name)) && !ours.has(e.name)) await zw.copy(port.file, e); tick(e.csize); }
  for (const [rel, ent] of stage.entries) {
    if (ent.chunks) { await zw.add(where(rel), ent.size, ent.mode, tickChunks(ent.chunks(), tick), false); continue; }
    const file = await stage.file(rel);
    let head = null;
    if (/(^|\/)(globalgamemanagers|data\.unity3d|mainData)$/.test(rel)) head = new Uint8Array(await file.slice(0, 4096).arrayBuffer());
    if (head) { const m = utf8.decode(head).match(VERSION_RX); if (m) found.push({ path: rel.replace(/\/[^/]+$/, ''), version: m[1] }); }
    await zw.add(where(rel), file.size, ent.mode, blobChunks(file, tick), compress && !ent.stored);
  }
  const size = await zw.finish();
  return { size, found, entries: zw.cd.length };
}
async function* tickChunks(chunks, tick) { for await (const c of chunks) { tick && tick(c.length); yield c; } }
async function* blobChunks(blob, tick) {
  const r = blob.stream().getReader();
  for (;;) { const { value, done } = await r.read(); if (done) return; tick && tick(value.length); yield value; }
}
async function* readerChunks(stream, n) { const r = new Reader(stream); yield* r.chunks(n); await r.cancel(); }

async function addPkg(s, port, addGame, tick, log) {
  let x;
  try { x = await pkgOpen(s.file); } catch (ex) { ex.unreadable = true; throw ex; }
  const title = xml(await x.text('Distribution').catch(() => ''));
  const titleName = kidText(find(title, 'installer-gui-script'), 'title');
  const comps = [...new Set([...x.files.keys()].filter(p => /(^|\/)(Payload|Scripts|PackageInfo)$/.test(p))
    .map(p => p.includes('/') ? p.replace(/\/[^/]+$/, '') : ''))].sort();
  let files = 0;
  for (const c of comps) {
    const pre = c ? c + '/' : '';
    let loc = '/';
    if (x.hasData(pre + 'PackageInfo')) loc = (find(xml(await x.text(pre + 'PackageInfo')), 'pkg-info') || { attrs: {} }).attrs['install-location'] || '/';
    // the install location without the system's own folders: a game lands in gamedata/<Game>.app
    const place = p => cleanPath(p.replace(/^\/?(Applications|Library\/Application Support)(\/|$)/, ''));
    for (const which of ['Payload', 'Scripts']) {
      if (!x.hasData(pre + which)) continue;
      let st;
      try { st = await payload(x.member(x.files.get(pre + which), tick)); }
      catch (ex) { ex.unreadable = true; throw ex; }
      let bundle = null, n = 0, postinstall = '';
      if (which === 'Scripts') {
        // GOG: the game is in Scripts' payload/; its postinstall (at the end) names the .app it goes to.
        // Taken from the port's setup where it names one, else from the installer's title
        bundle = port.appName || (titleName ? `${titleName}.app` : `${s.name.replace(/\.pkg$/i, '')}.app`);
      }
      for await (const m of cpio(st)) {
        const kindBits = m.mode & 0o170000;
        let name = m.name.replace(/^\.\//, '');
        if (which === 'Scripts') {
          if (name === 'postinstall' && kindBits === 0o100000 && m.size < 1 << 20) {
            postinstall = utf8.decode(await m.r.exact(m.size)); m.used = true; continue;
          }
          if (!name.startsWith('payload/')) continue;
          name = name.slice(8);
        }
        if (kindBits !== 0o100000) continue;          // folders come with their files; symbolic links are skipped as on the device
        const dest = which === 'Scripts' ? place(`${loc}/${bundle}/${name}`) : place(`${loc}/${name}`);
        if (!dest) continue;
        m.used = true;
        if (await addGame(dest, m.size, m.mode, m.r.chunks(m.size))) n++;
      }
      if (which === 'Scripts' && n) {
        const g = postinstall.match(/gog_bundle_name="([^"/]+)"/);
        if (g && g[1] !== bundle) log(`${s.name}: note: the installer names it ${g[1]}; it went to ${bundle}${port.appName ? " (the name the port's setup looks for)" : ''}`);
      }
      if (n) log(`${s.name}: ${c || 'package'} ${which}${bundle ? ` -> gamedata/${place(loc + '/' + bundle)}` : ''}: ${n} files`);
      files += n;
    }
  }
  if (!files) log(`${s.name}: no files found in it`);
}

return { inspectPort, stage, writeZip, kindOf, zipText, zipStream, zipOpen, pkgOpen, crc32, shellEnv, cleanPath, VERSION_RX, ZipWriter };
})();
if (typeof module !== 'undefined') module.exports = CORE;
