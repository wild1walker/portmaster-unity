// Unity serialized files for prepatch.html's texture conversion: the parts pmunity's textures step
// reads and changes through UnityPy 1.25.4, with UnityPy's own rules (SerializedFile, ObjectReader,
// TypeTreeNode, TypeTreeHelper). Metadata is never rebuilt: a changed object only moves, so saving
// lays the object data out again (in path id order, 8-byte aligned, as UnityPy does) and patches each object's offset
// and size in the object table and the header's file size; everything else stays byte for byte.
'use strict';
const UNITY = (() => {
const utf8 = new TextDecoder('utf-8');
const enc = new TextEncoder();

class R {                                   // a reader over bytes (positions from the start of bytes)
  constructor(b, little = true, pos = 0) { this.b = b; this.v = new DataView(b.buffer, b.byteOffset, b.byteLength); this.le = little; this.p = pos; }
  u8() { return this.b[this.p++]; }
  i8() { return this.v.getInt8(this.p++); }
  bool() { return this.b[this.p++] !== 0; }
  i16() { const x = this.v.getInt16(this.p, this.le); this.p += 2; return x; }
  u16() { const x = this.v.getUint16(this.p, this.le); this.p += 2; return x; }
  i32() { const x = this.v.getInt32(this.p, this.le); this.p += 4; return x; }
  u32() { const x = this.v.getUint32(this.p, this.le); this.p += 4; return x; }
  i64() { const x = this.v.getBigInt64(this.p, this.le); this.p += 8; return x; }
  u64() { const x = this.v.getBigUint64(this.p, this.le); this.p += 8; return x; }
  f32() { const x = this.v.getFloat32(this.p, this.le); this.p += 4; return x; }
  f64() { const x = this.v.getFloat64(this.p, this.le); this.p += 8; return x; }
  bytes(n) { if (n < 0 || this.p + n > this.b.length) throw new Error('read past the end'); const x = this.b.subarray(this.p, this.p + n); this.p += n; return x; }
  cstr() { let e = this.p; while (e < this.b.length && this.b[e]) e++; const s = utf8.decode(this.b.subarray(this.p, e)); this.p = e + 1; return s; }
  align(n = 4) { this.p += (n - this.p % n) % n; }
}

class W {                                   // a growing writer (positions from its start)
  constructor(little = true) { this.b = new Uint8Array(1024); this.v = new DataView(this.b.buffer); this.le = little; this.p = 0; }
  room(n) { if (this.p + n > this.b.length) { let m = this.b.length * 2; while (m < this.p + n) m *= 2;
    const nb = new Uint8Array(m); nb.set(this.b.subarray(0, this.p)); this.b = nb; this.v = new DataView(nb.buffer); } }
  u8(x) { this.room(1); this.b[this.p++] = x; }
  i8(x) { this.room(1); this.v.setInt8(this.p++, x); }
  bool(x) { this.u8(x ? 1 : 0); }
  i16(x) { this.room(2); this.v.setInt16(this.p, x, this.le); this.p += 2; }
  u16(x) { this.room(2); this.v.setUint16(this.p, x, this.le); this.p += 2; }
  i32(x) { this.room(4); this.v.setInt32(this.p, x, this.le); this.p += 4; }
  u32(x) { this.room(4); this.v.setUint32(this.p, x, this.le); this.p += 4; }
  i64(x) { this.room(8); this.v.setBigInt64(this.p, BigInt(x), this.le); this.p += 8; }
  u64(x) { this.room(8); this.v.setBigUint64(this.p, BigInt(x), this.le); this.p += 8; }
  f32(x) { this.room(4); this.v.setFloat32(this.p, x, this.le); this.p += 4; }
  f64(x) { this.room(8); this.v.setFloat64(this.p, x, this.le); this.p += 8; }
  bytes(a) { this.room(a.length); this.b.set(a, this.p); this.p += a.length; }
  align(n = 4) { const k = (n - this.p % n) % n; this.room(k); this.b.fill(0, this.p, this.p + k); this.p += k; }
  out() { return this.b.slice(0, this.p); }
}

// ---- Unity versions (UnityPy's UnityVersion: major<<48 | minor<<32 | build<<16 | type<<8 | number)
const VTYPE = { a: 0, b: 1, c: 2, f: 3, p: 4, x: 5 };
function versionInt(s) {
  const m = /^(\d+)\.(\d+)\.(\d+)(.+?)?(\d+)?(.*)$/s.exec(s || '');
  if (!m) throw new Error(`bad Unity version ${s}`);
  const t = m[4] === undefined ? 3 : (VTYPE[m[4]] ?? 255);
  return BigInt(m[1]) << 48n | BigInt(m[2]) << 32n | BigInt(m[3]) << 16n | BigInt(t) << 8n | BigInt(m[5] || 0);
}

// ---- type trees
let DB = null;                              // typetrees.json (set with setTypeTrees)
const nodeCache = new Map();
function setTypeTrees(db) { DB = db; nodeCache.clear(); }
function buildTree(flat) {                  // [[level, type, name, size, meta], ...] -> root node
  const root = { level: flat[0][0], type: flat[0][1], name: flat[0][2], size: flat[0][3], meta: flat[0][4], kids: [] };
  const stack = [root];
  for (let i = 1; i < flat.length; i++) {
    const [level, type, name, size, meta] = flat[i];
    const n = { level, type, name, size, meta, kids: [] };
    while (stack.length && stack[stack.length - 1].level >= level) stack.pop();
    stack[stack.length - 1].kids.push(n);
    stack.push(n);
  }
  return root;
}
function dbTree(classId, vint) {            // UnityPy's Tpk.get_typetree_node
  const key = `${classId}|${vint}`;
  if (nodeCache.has(key)) return nodeCache.get(key);
  const entries = DB && DB.classes[String(classId)];
  let li;
  if (entries) for (const [v, idx] of entries) { if (vint >= BigInt(v)) li = idx; else break; }
  const tree = li === undefined || li === null ? null : buildTree(DB.nodes[li]);
  nodeCache.set(key, tree);
  return tree;
}
function parseBlob(r, version) {            // TypeTreeNode.parse_blob
  if (version >= 23) { if (utf8.decode(r.bytes(4)) !== 'mhtt') throw new Error('bad type tree blob'); r.i32(); }
  const count = r.i32(), sbSize = r.i32(), ns = version >= 19 ? 32 : 24;
  const nodesStart = r.p;
  r.p += ns * count;
  const sb = r.bytes(sbSize);
  const str = v => {
    if ((v & 0x80000000) === 0) { let e = v; while (e < sb.length && sb[e]) e++; return utf8.decode(sb.subarray(v, e)); }
    const o = v & 0x7FFFFFFF; return (DB && DB.strings[String(o)]) ?? String(o);
  };
  const flat = [], names = [];
  const q = new R(r.b, r.le, nodesStart);
  for (let i = 0; i < count; i++) {
    q.p = nodesStart + i * ns;
    q.i16(); const level = q.u8(); q.u8(); const t = q.u32(), n = q.u32(), size = q.i32(); q.i32(); const meta = q.i32();
    flat.push([level, str(t), str(n), size, meta]);
    names.push([str(t), str(n)]);
  }
  const node = buildTree(flat);
  // where the blob is, for writing it again as UnityPy does (dumpBlob)
  node.blob = { start: nodesStart - 8, end: r.p, nodesStart, count, ns, names };
  return node;
}
// TypeTreeNode.dump_blob (formats 12 - 22): the same nodes, their strings laid out again: a string of UnityPy's
// common strings is referred to there (0x80000000 | offset; offset 0 counts as none, as UnityPy's `if common_offset`),
// any other in the blob's own buffer, each once, in the order the nodes name them (type, then name)
let commonOffset = null;
function dumpBlob(b, bytes, little) {
  if (!commonOffset) { commonOffset = new Map(); for (const [o, s] of Object.entries(DB.strings)) commonOffset.set(s, +o); }
  const enc = new TextEncoder(), sb = [], offs = new Map();
  let sbLen = 0;
  const str = s => {
    if (offs.has(s)) return offs.get(s);
    let o;
    const c = commonOffset.get(s);
    if (c) o = (c | 0x80000000) >>> 0;
    else { o = sbLen; const e = enc.encode(s); sb.push(e, new Uint8Array(1)); sbLen += e.length + 1; }
    offs.set(s, o);
    return o;
  };
  const nodes = bytes.slice(b.nodesStart, b.nodesStart + b.count * b.ns), v = new DataView(nodes.buffer);
  b.names.forEach(([t, n], i) => { v.setUint32(i * b.ns + 4, str(t), little); v.setUint32(i * b.ns + 8, str(n), little); });
  const head = new Uint8Array(8), hv = new DataView(head.buffer);
  hv.setInt32(0, b.count, little); hv.setInt32(4, sbLen, little);
  const out = new Uint8Array(8 + nodes.length + sbLen);
  out.set(head, 0); out.set(nodes, 8);
  let p = 8 + nodes.length; for (const x of sb) { out.set(x, p); p += x.length; }
  return out;
}
function parseOld(r, version) {             // TypeTreeNode.parse (formats before 10, and 11)
  const flat = [];
  const one = level => {
    const type = r.cstr(), name = r.cstr(), size = r.i32();
    if (version === 2) r.i32();
    if (version !== 3) r.i32();
    r.i32(); r.i32();
    const meta = version !== 3 ? r.i32() : 0;
    flat.push([level, type, name, size, meta]);
    const kids = r.i32();
    for (let i = 0; i < kids; i++) one(level + 1);
  };
  one(0);
  return buildTree(flat);
}

// ---- serialized files
function parseSerialized(b, name) {
  const h = new R(b, false);
  let metadataSize = h.u32(), fileSize = h.u32(); const version = h.u32(); let dataOffset = h.u32();
  if (version < 9) throw new Error(`${name}: serialized file format ${version} is not supported`);
  const little = !h.bool(); h.bytes(3);
  if (version >= 22) { metadataSize = h.u32(); fileSize = Number(h.i64()); dataOffset = Number(h.i64()); h.i64(); }
  const r = new R(b, little, h.p);
  const sf = { name, version, little, dataOffset, metadataSize, headerEnd: h.p, types: [], objects: [], typeTree: true, unity: '', platform: 0, blobs: [] };
  if (version >= 7) sf.unity = r.cstr();
  if (version >= 8) sf.platform = r.i32();
  if (version >= 13) sf.typeTree = r.bool();
  const readType = isRef => {
    const t = { classId: r.i32(), scriptIndex: -1, node: null };
    if (version >= 16) r.bool();
    if (version >= 17) t.scriptIndex = r.i16();
    if (version >= 13) {
      if ((isRef && t.scriptIndex >= 0) || (version < 16 && t.classId < 0) || (version >= 16 && t.classId === 114)) r.bytes(16);
      r.bytes(16);
    }
    if (sf.typeTree) {
      let size = null;
      if (version >= 23) { r.bytes(16); size = r.i32(); }
      if (version >= 12 || version === 10) { if (version < 23 || size) { t.node = parseBlob(r, version); sf.blobs.push(t.node.blob); } }
      else t.node = parseOld(r, version);
      if (version >= 21) { if (isRef) { r.cstr(); r.cstr(); r.cstr(); } else { const n = r.i32(); r.bytes(4 * n); } }
    }
    return t;
  };
  const nTypes = r.i32();
  for (let i = 0; i < nTypes; i++) sf.types.push(readType(false));
  let bigId = 0;
  if (version >= 7 && version < 14) bigId = r.i32();
  const nObj = r.i32();
  for (let i = 0; i < nObj; i++) {
    let pathId;
    if (bigId) pathId = r.i64();
    else if (version < 14) pathId = BigInt(r.i32());
    else { r.align(); pathId = r.i64(); }
    const startPos = r.p;
    const byteStart = (version >= 22 ? Number(r.i64()) : r.u32()) + dataOffset;
    const sizePos = r.p;
    const byteSize = r.u32();
    const typeId = r.i32();
    let classId, type;
    if (version < 16) { classId = r.u16(); type = sf.types.find(t => t.classId === typeId) || null; }
    else { type = sf.types[typeId]; classId = type.classId; }
    if (version < 11) r.u16();
    if (version >= 11 && version < 17) r.i16();
    if (version === 15 || version === 16) r.u8();
    sf.objects.push({ pathId, byteStart, byteSize, classId, type, startPos, sizePos });
  }
  // the files this one refers to (PPtr file ids 1..n): after the script types
  sf.externals = [];
  try {
    if (version >= 11) { const n = r.i32(); for (let i = 0; i < n; i++) { r.i32(); if (version < 14) r.i32(); else { r.align(); r.i64(); } } }
    const n = r.i32();
    for (let i = 0; i < n; i++) {
      if (version >= 6) r.cstr();
      if (version >= 5) { r.bytes(16); r.i32(); }
      sf.externals.push(r.cstr());
    }
  } catch { sf.externals = null; }         // (unread: only the steps that follow references need them)
  sf.vint = versionInt(sf.unity);
  sf.bytes = b;
  return sf;
}
function nodeOf(sf, o) {                    // ObjectReader._get_typetree_node
  if (o.type && o.type.node) return o.type.node;
  const t = dbTree(o.classId, sf.vint);
  if (!t) throw new Error(`${sf.name}: no type tree for class ${o.classId} (Unity ${sf.unity})`);
  return t;
}
const objectBytes = (sf, o) => sf.bytes.subarray(o.byteStart, o.byteStart + o.byteSize);

// ---- type tree values (TypeTreeHelper.read_value / write_value)
const NUM = { SInt8: 'i8', UInt8: 'u8', char: 'u8', short: 'i16', SInt16: 'i16', 'unsigned short': 'u16', UInt16: 'u16',
  int: 'i32', SInt32: 'i32', 'unsigned int': 'u32', UInt32: 'u32', 'Type*': 'u32', 'long long': 'i64', SInt64: 'i64',
  'unsigned long long': 'u64', UInt64: 'u64', FileSize: 'u64', float: 'f32', double: 'f64', bool: 'bool' };
const aligned = n => ((n.meta || 0) & 0x4000) !== 0;
const isVector = n => n.kids.length && n.kids[0].type === 'Array';

function readValue(n, r) {
  let al = aligned(n), v;
  const f = NUM[n.type];
  if (f) v = r[f]();
  else if (n.type === 'string') {
    const len = r.i32();
    if (len > 0 && len <= r.b.length - r.p) { v = utf8.decode(r.bytes(len)); r.align(); } else v = '';
  } else if (n.type === 'TypelessData') v = r.bytes(r.i32());
  else if (n.type === 'pair') v = [readValue(n.kids[0], r), readValue(n.kids[1], r)];
  else if (isVector(n)) {
    if (aligned(n.kids[0])) al = true;
    const size = r.i32();
    if (size < 0) throw new Error('negative length in a type tree');
    const sub = n.kids[0].kids[1];
    if (aligned(sub)) v = readArray(sub, r, size);
    else if ((sub.type === 'UInt8' || sub.type === 'char') && !sub.kids.length) v = r.bytes(size);
    else { v = new Array(size); for (let i = 0; i < size; i++) v[i] = readValue(sub, r); }
  } else {
    v = {};
    for (const k of n.kids) {
      if (k.type === 'ManagedReferencesRegistry' || k.type === 'ReferencedObject') throw new Error('managed references are not supported');
      v[k.name] = readValue(k, r);
    }
  }
  if (al) r.align();
  return v;
}
function readArray(n, r, size) {            // read_value_array: elements read as a block, aligned once after
  let v;
  const f = NUM[n.type];
  if (f) { if ((n.type === 'UInt8' || n.type === 'char') && !n.kids.length) v = r.bytes(size); else { v = new Array(size); for (let i = 0; i < size; i++) v[i] = r[f](); } }
  else if (n.type === 'string' || n.type === 'TypelessData' || n.type === 'pair') {
    v = new Array(size);
    for (let i = 0; i < size; i++) {
      if (n.type === 'pair') v[i] = [readValue(n.kids[0], r), readValue(n.kids[1], r)];
      else { const one = { ...n, meta: 0 }; v[i] = readValue(one, r); }
    }
  } else if (isVector(n)) {
    const sub = n.kids[0].kids[1];
    if (aligned(n.kids[0])) { /* align below, with the node's own */ }
    v = new Array(size);
    for (let i = 0; i < size; i++) {
      const k = r.i32();
      if (aligned(sub)) v[i] = readArray(sub, r, k);
      else { v[i] = new Array(k); for (let j = 0; j < k; j++) v[i][j] = readValue(sub, r); }
    }
    if (aligned(n.kids[0])) { r.align(); return v; }
  } else {
    v = new Array(size);
    for (let i = 0; i < size; i++) { const o = {}; for (const c of n.kids) o[c.name] = readValue(c, r); v[i] = o; }
  }
  if (aligned(n)) r.align();
  return v;
}
function writeValue(v, n, w) {
  let al = aligned(n);
  const f = NUM[n.type];
  if (f) w[f](f === 'f32' || f === 'f64' ? v : f === 'i64' || f === 'u64' ? BigInt(v) : v);
  else if (n.type === 'string') { const b = enc.encode(v); w.i32(b.length); w.bytes(b); if (b.length) w.align(); }
  else if (n.type === 'TypelessData') { w.i32(v.length); w.bytes(v); }
  else if (n.type === 'pair') { writeValue(v[0], n.kids[0], w); writeValue(v[1], n.kids[1], w); }
  else if (isVector(n)) {
    if (aligned(n.kids[0])) al = true;
    w.i32(v.length);
    const sub = n.kids[0].kids[1];
    if (v instanceof Uint8Array && !aligned(sub)) w.bytes(v);
    else for (const x of v) writeValue(x, sub, w);           // (UnityPy: each element on its own, aligned if flagged)
  } else for (const k of n.kids) writeValue(v[k.name], k, w);
  if (al) w.align();
}
function readObject(sf, o) {                // o.read_typetree() (with UnityPy's check that it read every byte)
  const b = objectBytes(sf, o), r = new R(b, sf.little);
  const v = readValue(nodeOf(sf, o), r);
  if (r.p !== b.length) throw new Error(`${sf.name}: object ${o.pathId}: read ${r.p} of ${b.length} bytes`);
  return v;
}
function readObjectBytes(sf, o, b) {        // current_typetree: an object's tree from its new bytes
  const r = new R(b, sf.little);
  return readValue(nodeOf(sf, o), r);
}
function writeObject(sf, o, v) {            // o.save_typetree(v) -> the object's new bytes
  const w = new W(sf.little);
  writeValue(v, nodeOf(sf, o), w);
  return w.out();
}
// where each named field of an object starts (for changes in place: same size, same bytes around)
function fieldOffsets(sf, o, wanted) {
  const b = objectBytes(sf, o), r = new R(b, sf.little), out = {};
  const walk = (n, path) => {
    const here = path ? `${path}.${n.name}` : n.name;
    if (wanted.has(here)) out[here] = { pos: r.p, type: n.type };
    if (!NUM[n.type] && n.type !== 'string' && n.type !== 'TypelessData' && n.type !== 'pair' && !isVector(n)) {
      let al = aligned(n);
      for (const k of n.kids) walk(k, here);
      if (al) r.align();
    } else readValue(n, r);
  };
  const root = nodeOf(sf, o);
  for (const k of root.kids) walk(k, '');
  return out;
}

// ---- saving: [header + metadata (patched), object data] as parts, and the new file size
function save(sf, changed, { redump = false } = {}) {   // changed: Map(String(pathId) -> new bytes)
  // UnityPy's save: header + metadata, padded to 16 (the data offset recomputed), then the object data in
  // path id order, each object followed by padding to 8; the metadata's bytes are the file's own
  // redump: the type trees (files that have them: bundles) written again as UnityPy's own save writes them
  // (dumpBlob; pmunity's shaders step). Without it they stay as they are, as pmunity's streamed save
  // (unity_io.save_serialized_to: the textures step, android-data) keeps them
  let head = sf.bytes.subarray(0, sf.headerEnd + sf.metadataSize), shift = () => 0;
  if (redump && sf.typeTree && sf.blobs.length && sf.version >= 12 && sf.version < 23) {
    const parts = [], moves = [];
    let at = 0, delta = 0;
    for (const b of sf.blobs) {
      const nb = dumpBlob(b, sf.bytes, sf.little);
      parts.push(sf.bytes.subarray(at, b.start), nb);
      delta += nb.length - (b.end - b.start); moves.push([b.end, delta]); at = b.end;
    }
    parts.push(sf.bytes.subarray(at, head.length));
    const nh = new Uint8Array(parts.reduce((a, x) => a + x.length, 0));
    let p = 0; for (const x of parts) { nh.set(x, p); p += x.length; }
    head = nh;
    shift = pos => { let d = 0; for (const [end, dd] of moves) if (pos >= end) d = dd; return d; };
    const hv = new DataView(head.buffer);
    if (sf.version >= 22) hv.setUint32(20, sf.metadataSize + delta, false); else hv.setUint32(0, sf.metadataSize + delta, false);
  }
  const headLen = head.length, off = headLen + (16 - headLen % 16) % 16;
  const meta = new Uint8Array(off);
  meta.set(head);
  const mv = new DataView(meta.buffer);
  const parts = [meta];
  let pos = 0;                              // in the data region
  const objs = [...sf.objects].sort((a, b) => a.pathId < b.pathId ? -1 : a.pathId > b.pathId ? 1 : 0);
  for (const o of objs) {
    const data = changed.get(String(o.pathId)) || objectBytes(sf, o);
    const sp = o.startPos + shift(o.startPos), zp = o.sizePos + shift(o.sizePos);
    if (sf.version >= 22) mv.setBigInt64(sp, BigInt(pos), sf.little); else mv.setUint32(sp, pos, sf.little);
    mv.setUint32(zp, data.length, sf.little);
    parts.push(data);
    pos += data.length;
    const pad = (8 - pos % 8) % 8;
    if (pad) { parts.push(new Uint8Array(pad)); pos += pad; }
  }
  const size = off + pos;
  if (sf.version >= 22) {                   // (the old header's sizes written as 0, as UnityPy does)
    mv.setUint32(0, 0, false); mv.setUint32(4, 0, false); mv.setUint32(12, 0, false);
    mv.setBigInt64(24, BigInt(size), false); mv.setBigInt64(32, BigInt(off), false);
  } else { mv.setUint32(4, size, false); mv.setUint32(12, off, false); }
  return { parts, size };
}

return { R, W, versionInt, setTypeTrees, parseSerialized, nodeOf, readObject, writeObject, fieldOffsets, objectBytes,
  save, readValue, writeValue, buildTree, readObjectBytes };
})();
if (typeof module !== 'undefined') module.exports = UNITY;
