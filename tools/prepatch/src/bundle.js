// UnityFS asset bundles for the textures step, as UnityPy 1.25.4 reads them (BundleFile.read_fs) and as
// pmunity writes them back (UnityPy save(packer='lz4') / bigbundle.pack: blocks info at the end, 128 KiB
// chunks LZ4HC level 9, a chunk stored raw when that does not make it smaller). LZMA bundles and
// encrypted ones are left to the handheld (open() returns null).
'use strict';
const BUNDLE = (() => {
const utf8 = new TextDecoder('utf-8');
const CHUNK = 0x20000, DATA_FLAG = 194, BLOCK_FLAG = 2;

function unlz4(src, out) {                    // an LZ4 block -> out (its size known); returns the bytes written
  let s = 0, d = 0;
  const end = src.length;
  while (s < end) {
    const tok = src[s++];
    let lit = tok >> 4;
    if (lit === 15) { let b; do { b = src[s++]; lit += b; } while (b === 255); }
    out.set(src.subarray(s, s + lit), d); s += lit; d += lit;
    if (s >= end) break;
    const off = src[s] | src[s + 1] << 8; s += 2;
    let len = tok & 15;
    if (len === 15) { let b; do { b = src[s++]; len += b; } while (b === 255); }
    len += 4;
    let from = d - off;
    if (off >= len) { out.copyWithin(d, from, from + len); d += len; }
    else for (let i = 0; i < len; i++) out[d++] = out[from++];
  }
  return d;
}
let fastUnlz4 = null;                          // setUnlz4(fn(src, usize) -> bytes): the compiled decoder, when there is one
function setUnlz4(fn) { fastUnlz4 = fn; }
function decompress(data, usize, flags) {
  const k = flags & 0x3F;
  if (k === 0) return data;
  if ((k === 2 || k === 3) && fastUnlz4) return fastUnlz4(data, usize);
  if (k === 2 || k === 3) { const out = new Uint8Array(usize); if (unlz4(data, out) !== usize) throw new Error('LZ4: bad block'); return out; }
  return null;                                 // LZMA (1), LZHAM (4): not here
}
const vtuple = s => (s.match(/^(\d+)\.(\d+)\.(\d+)/) || [0, 0, 0, 0]).slice(1).map(Number);
const vlt = (a, b) => { for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] < b[i]; return false; };

// open(file) -> {version, player, engine, aligned, nodes: [{name, flags, offset, size}], node(name) -> bytes}, or null
async function open(file) {
  const head = new Uint8Array(await file.slice(0, Math.min(file.size, 4096)).arrayBuffer());
  const dv = new DataView(head.buffer);
  let p = 0;
  const cstr = () => { let e = p; while (head[e]) e++; const s = utf8.decode(head.subarray(p, e)); p = e + 1; return s; };
  if (cstr() !== 'UnityFS') return null;
  const version = dv.getUint32(p, false); p += 4;
  const player = cstr(), engine = cstr();
  p += 8;                                                       // size
  const csize = dv.getUint32(p, false), usize = dv.getUint32(p + 4, false), flags = dv.getUint32(p + 8, false); p += 12;
  const ev = vtuple(engine);
  const old = vlt(ev, [2020, 0, 0]) || (ev[0] === 2020 && vlt(ev, [2020, 3, 34])) || (ev[0] === 2021 && vlt(ev, [2021, 3, 2])) ||
    (ev[0] === 2022 && vlt(ev, [2022, 1, 1]));
  if (old ? flags & 0x200 : flags & 0x400) return null;         // encrypted
  const aligned = version >= 7 || (ev[0] === 2019 && !vlt(ev, [2019, 4, 15]));
  if (aligned) p += (16 - p % 16) % 16;
  let info, dataStart;
  if (flags & 0x80) { info = new Uint8Array(await file.slice(file.size - csize, file.size).arrayBuffer()); dataStart = p; }
  else { info = new Uint8Array(await file.slice(p, p + csize).arrayBuffer()); dataStart = p + csize; }
  info = decompress(info, usize, flags);
  if (!info) return null;
  if (!old && flags & 0x200) dataStart += (16 - dataStart % 16) % 16;
  const iv = new DataView(info.buffer, info.byteOffset, info.byteLength);
  let q = 16;
  const nb = iv.getInt32(q, false); q += 4;
  const blocks = [];
  let coff = dataStart, uoff = 0;
  for (let i = 0; i < nb; i++) {
    const u = iv.getUint32(q, false), c = iv.getUint32(q + 4, false), f = iv.getUint16(q + 8, false); q += 10;
    if ((f & 0x3F) !== 0 && (f & 0x3F) !== 2 && (f & 0x3F) !== 3) return null;
    blocks.push({ u, c, f, coff, uoff }); coff += c; uoff += u;
  }
  const nn = iv.getInt32(q, false); q += 4;
  const nodes = [];
  for (let i = 0; i < nn; i++) {
    const offset = Number(iv.getBigInt64(q, false)), size = Number(iv.getBigInt64(q + 8, false)), f = iv.getUint32(q + 16, false); q += 20;
    let e = q; while (info[e]) e++;
    nodes.push({ offset, size, flags: f, name: utf8.decode(info.subarray(q, e)) }); q = e + 1;
  }
  const cache = new Map();
  async function node(name) {                    // a node's bytes (decompressing the blocks it spans)
    if (cache.has(name)) return cache.get(name);
    const n = nodes.find(x => x.name === name);
    if (!n) return null;
    const out = new Uint8Array(n.size);
    for (const b of blocks) {
      if (b.uoff + b.u <= n.offset || b.uoff >= n.offset + n.size) continue;
      const raw = new Uint8Array(await file.slice(b.coff, b.coff + b.c).arrayBuffer());
      const d = decompress(raw, b.u, b.f);
      const a = Math.max(n.offset, b.uoff), z = Math.min(n.offset + n.size, b.uoff + b.u);
      out.set(d.subarray(a - b.uoff, z - b.uoff), a - n.offset);
    }
    cache.set(name, out);
    return out;
  }
  return { version, player, engine, aligned, nodes, node };
}
// pmunity's serialized_nodes: the nodes flagged as serialized files (4), else every node that is not a stream
const serializedNodes = b => { const f = b.nodes.filter(n => n.flags & 4); return f.length ? f : b.nodes.filter(n => !/\.(resS|resource)$/.test(n.name)); };

// write(b, nodes, lz4hc) -> {parts, size}: nodes [{name, flags, parts}] in order; lz4hc(chunks) -> compressed chunks
async function write(b, nodes, lz4hc) {
  const enc = new TextEncoder();
  const total = nodes.reduce((a, n) => a + n.parts.reduce((x, p) => x + p.length, 0), 0);
  // the nodes' bytes cut in 128 KiB chunks (the last one shorter)
  const chunks = [];
  let cur = new Uint8Array(Math.min(CHUNK, total)), fill = 0, left = total;
  for (const n of nodes) for (let part of n.parts) {
    while (part.length) {
      const k = Math.min(part.length, cur.length - fill);
      cur.set(part.subarray(0, k), fill); fill += k; part = part.subarray(k);
      if (fill === cur.length) { chunks.push(cur); left -= fill; cur = new Uint8Array(Math.min(CHUNK, left)); fill = 0; }
    }
  }
  const packed = await lz4hc(chunks);
  const out = [], blocks = [];
  for (let i = 0; i < chunks.length; i++) {
    const ch = chunks[i], c = packed[i];
    const raw = ch.length === CHUNK ? c.length >= CHUNK : c.length > ch.length;
    if (raw) { out.push(ch); blocks.push([ch.length, ch.length, BLOCK_FLAG ^ (BLOCK_FLAG & 0x3F)]); }
    else { out.push(c); blocks.push([ch.length, c.length, BLOCK_FLAG]); }
  }
  // blocks info: hash (16 zero bytes), blocks, directory; LZ4HC too (DATA_FLAG & 0x3F)
  const names = nodes.map(n => enc.encode(n.name));
  const isz = 16 + 4 + 10 * blocks.length + 4 + names.reduce((a, s) => a + 20 + s.length + 1, 0);
  const info = new Uint8Array(isz), iv = new DataView(info.buffer);
  let q = 16;
  iv.setInt32(q, blocks.length, false); q += 4;
  for (const [u, c, f] of blocks) { iv.setUint32(q, u, false); iv.setUint32(q + 4, c, false); iv.setUint16(q + 8, f, false); q += 10; }
  iv.setInt32(q, nodes.length, false); q += 4;
  let off = 0;
  nodes.forEach((n, i) => {
    const sz = n.parts.reduce((x, p) => x + p.length, 0);
    iv.setBigInt64(q, BigInt(off), false); iv.setBigInt64(q + 8, BigInt(sz), false); iv.setUint32(q + 16, n.flags, false); q += 20;
    info.set(names[i], q); q += names[i].length + 1; off += sz;
  });
  const [cinfo] = await lz4hc([info]);
  // header
  const hp = [enc.encode('UnityFS\0'), new Uint8Array(4), enc.encode(b.player + '\0'), enc.encode(b.engine + '\0')];
  new DataView(hp[1].buffer).setUint32(0, b.version, false);
  let hlen = hp.reduce((a, x) => a + x.length, 0);
  const sizes = new Uint8Array(20), sv = new DataView(sizes.buffer);
  hlen += 20;
  const pad = b.aligned ? (16 - hlen % 16) % 16 : 0;
  const size = hlen + pad + out.reduce((a, x) => a + x.length, 0) + cinfo.length;
  sv.setBigInt64(0, BigInt(size), false); sv.setUint32(8, cinfo.length, false); sv.setUint32(12, info.length, false); sv.setUint32(16, DATA_FLAG, false);
  return { parts: [...hp, sizes, new Uint8Array(pad), ...out, cinfo], size };
}

return { open, serializedNodes, write, unlz4, setUnlz4 };
})();
if (typeof module !== 'undefined') module.exports = BUNDLE;
