// pmunity's clrmeta.py: a small reader of .NET assembly metadata (ECMA-335 partition II): type and method names,
// P/Invoke declarations (ImplMap -> ModuleRef), method signatures, field constants, assembly and type references.
// Same tables, same answers (android-data's Managed choices and the native plugin survey use it).
'use strict';
const CLRMETA = (() => {
const [MODULE, TYPEREF, TYPEDEF, FIELDPTR, FIELD, METHODPTR, METHODDEF, PARAMPTR, PARAM] = [0, 1, 2, 3, 4, 5, 6, 7, 8];
const CONSTANT = 0x0B, NESTEDCLASS = 0x29, CLASSLAYOUT = 0x0F, MODULEREF = 0x1A, IMPLMAP = 0x1C, ASSEMBLYREF = 0x23;
void FIELDPTR; void METHODPTR; void PARAMPTR;
const CODED = {
  TypeDefOrRef: [2, [TYPEDEF, TYPEREF, 0x1B]],
  HasConstant: [2, [FIELD, PARAM, 0x17]],
  HasCustomAttribute: [5, [METHODDEF, FIELD, TYPEREF, TYPEDEF, PARAM, 0x09, 0x0A, MODULE, 0x0E, 0x17, 0x14, 0x11,
    MODULEREF, 0x1B, 0x20, ASSEMBLYREF, 0x26, 0x27, 0x28, 0x2A, 0x2C, 0x2B]],
  HasFieldMarshal: [1, [FIELD, PARAM]],
  HasDeclSecurity: [2, [TYPEDEF, METHODDEF, 0x20]],
  MemberRefParent: [3, [TYPEDEF, TYPEREF, MODULEREF, METHODDEF, 0x1B]],
  HasSemantics: [1, [0x14, 0x17]],
  MethodDefOrRef: [1, [METHODDEF, 0x0A]],
  MemberForwarded: [1, [FIELD, METHODDEF]],
  Implementation: [2, [0x26, ASSEMBLYREF, 0x27]],
  CustomAttributeType: [3, [null, null, METHODDEF, 0x0A, null]],
  ResolutionScope: [2, [MODULE, MODULEREF, ASSEMBLYREF, TYPEREF]],
  TypeOrMethodDef: [1, [TYPEDEF, METHODDEF]],
};
// columns: 1/2/4 fixed, 'S' string, 'G' guid, 'B' blob, a number = index into that table, a name = coded index
const SCHEMA = {
  0x00: [2, 'S', 'G', 'G', 'G'], 0x01: ['ResolutionScope', 'S', 'S'], 0x02: [4, 'S', 'S', 'TypeDefOrRef', FIELD, METHODDEF],
  0x03: [FIELD], 0x04: [2, 'S', 'B'], 0x05: [METHODDEF], 0x06: [4, 2, 2, 'S', 'B', PARAM], 0x07: [PARAM], 0x08: [2, 2, 'S'],
  0x09: [TYPEDEF, 'TypeDefOrRef'], 0x0A: ['MemberRefParent', 'S', 'B'], 0x0B: [1, 1, 'HasConstant', 'B'],
  0x0C: ['HasCustomAttribute', 'CustomAttributeType', 'B'], 0x0D: ['HasFieldMarshal', 'B'], 0x0E: [2, 'HasDeclSecurity', 'B'],
  0x0F: [2, 4, TYPEDEF], 0x10: [4, FIELD], 0x11: ['B'], 0x12: [TYPEDEF, 0x14], 0x13: [0x14], 0x14: [2, 'S', 'TypeDefOrRef'],
  0x15: [TYPEDEF, 0x17], 0x16: [0x17], 0x17: [2, 'S', 'B'], 0x18: [2, METHODDEF, 'HasSemantics'],
  0x19: [TYPEDEF, 'MethodDefOrRef', 'MethodDefOrRef'], 0x1A: ['S'], 0x1B: ['B'], 0x1C: [2, 'MemberForwarded', 'S', MODULEREF],
  0x1D: [4, FIELD], 0x1E: [4, 4], 0x1F: [4], 0x20: [4, 2, 2, 2, 2, 4, 'B', 'S', 'S'], 0x21: [4], 0x22: [4, 4, 4],
  0x23: [2, 2, 2, 2, 4, 'B', 'S', 'S', 'B'], 0x24: [4, ASSEMBLYREF], 0x25: [4, 4, 4, ASSEMBLYREF], 0x26: [4, 'S', 'B'],
  0x27: [4, 4, 'S', 'S', 'Implementation'], 0x28: [4, 4, 'S', 'Implementation'], 0x29: [TYPEDEF, TYPEDEF],
  0x2A: [2, 2, 'TypeOrMethodDef', 'S'], 0x2B: ['MethodDefOrRef', 'B'], 0x2C: [0x2A, 'TypeDefOrRef'],
};
// columns that are table indices although their table number is 1, 2 or 4
const INDEX_COLS = { '2,4': FIELD, '3,0': FIELD, '9,0': TYPEDEF, '15,2': TYPEDEF, '16,1': FIELD, '18,0': TYPEDEF, '21,0': TYPEDEF,
  '25,0': TYPEDEF, '29,1': FIELD, '41,0': TYPEDEF, '41,1': TYPEDEF };
const E = { VOID: 1, BOOL: 2, CHAR: 3, I1: 4, U1: 5, I2: 6, U2: 7, I4: 8, U4: 9, I8: 10, U8: 11, R4: 12, R8: 13, STRING: 14,
  PTR: 0x0F, BYREF: 0x10, VALUETYPE: 0x11, CLASS: 0x12, VAR: 0x13, ARRAY: 0x14, GENERICINST: 0x15, TYPEDBYREF: 0x16,
  I: 0x18, U: 0x19, FNPTR: 0x1B, OBJECT: 0x1C, SZARRAY: 0x1D, MVAR: 0x1E, CMOD_REQD: 0x1F, CMOD_OPT: 0x20, SENTINEL: 0x41, PINNED: 0x45 };
const PRIM = { [E.BOOL]: 'bool', [E.CHAR]: 'char', [E.I1]: 'sbyte', [E.U1]: 'byte', [E.I2]: 'short', [E.U2]: 'ushort', [E.I4]: 'int',
  [E.U4]: 'uint', [E.I8]: 'long', [E.U8]: 'ulong', [E.R4]: 'float', [E.R8]: 'double', [E.I]: 'IntPtr', [E.U]: 'UIntPtr',
  [E.STRING]: 'string', [E.OBJECT]: 'object', [E.VOID]: 'void', [E.TYPEDBYREF]: 'TypedReference' };
const PRIM_SIZE = { [E.BOOL]: 1, [E.CHAR]: 2, [E.I1]: 1, [E.U1]: 1, [E.I2]: 2, [E.U2]: 2, [E.I4]: 4, [E.U4]: 4, [E.I8]: 8, [E.U8]: 8,
  [E.R4]: 4, [E.R8]: 8, [E.I]: 4, [E.U]: 4 };
const utf8 = new TextDecoder('utf-8'), utf16 = new TextDecoder('utf-16le');

class NotAssembly extends Error {}
class Assembly {
  constructor(bytes, name = '') {             // the file's bytes
    this.path = name;
    const d = this.d = bytes, v = this.v = new DataView(d.buffer, d.byteOffset, d.byteLength);
    if (d.length < 0x40 || d[0] !== 0x4D || d[1] !== 0x5A) throw new NotAssembly(name);
    const pe = v.getUint32(0x3c, true);
    if (pe + 4 > d.length || d[pe] !== 0x50 || d[pe + 1] !== 0x45 || d[pe + 2] || d[pe + 3]) throw new NotAssembly(name);
    const nsec = v.getUint16(pe + 6, true), optsz = v.getUint16(pe + 20, true), opt = pe + 24;
    const dirs = opt + (v.getUint16(opt, true) === 0x10b ? 96 : 112);
    this.secs = [];
    for (let i = 0; i < nsec; i++) { const o = opt + optsz + 40 * i + 8; this.secs.push([v.getUint32(o, true), v.getUint32(o + 4, true), v.getUint32(o + 8, true), v.getUint32(o + 12, true)]); }
    const cliRva = v.getUint32(dirs + 14 * 8, true);
    if (!cliRva) throw new NotAssembly(name);
    const cli = this.off(cliRva), md = this.off(v.getUint32(cli + 8, true));
    if (d[md] !== 0x42 || d[md + 1] !== 0x53 || d[md + 2] !== 0x4A || d[md + 3] !== 0x42) throw new NotAssembly(name);
    let p = md + 16 + v.getUint32(md + 12, true);
    const n = v.getUint16(p + 2, true); p += 4;
    this.streams = {};
    for (let i = 0; i < n; i++) {
      const so = v.getUint32(p, true), ss = v.getUint32(p + 4, true);
      let e = p + 8; while (d[e]) e++;
      this.streams[utf8.decode(d.subarray(p + 8, e))] = [md + so, ss];
      p = (e + 4) & ~3;
    }
    this._rows = {};
    this.tablesInit();
  }
  off(rva) {
    for (const [vsize, va, rsize, raw] of this.secs) if (va <= rva && rva < va + Math.max(vsize, rsize)) return rva - va + raw;
    throw new NotAssembly(`${this.path}: rva ${rva.toString(16)} outside the sections`);
  }
  string(i) { const o = this.streams['#Strings'][0] + i; let e = o; while (this.d[e]) e++; return utf8.decode(this.d.subarray(o, e)); }
  blob(i) { const o = this.streams['#Blob'][0] + i, [n, p] = this.cuint(o); return this.d.subarray(p, p + n); }
  cuint(o, d = this.d) {
    const b = d[o];
    if (b < 0x80) return [b, o + 1];
    if (b < 0xC0) return [((b & 0x3f) << 8) | d[o + 1], o + 2];
    return [(((b & 0x1f) << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]) >>> 0, o + 4];
  }
  tablesInit() {
    const d = this.d, v = this.v, name = '#~' in this.streams ? '#~' : '#-';
    let o = this.streams[name][0];
    const heaps = d[o + 6], valid = v.getBigUint64(o + 8, true);
    o += 24;
    this.nrows = {};
    for (let t = 0; t < 64; t++) if ((valid >> BigInt(t)) & 1n) { this.nrows[t] = v.getUint32(o, true); o += 4; }
    if (heaps & 0x40) o += 4;
    const sizes = { S: heaps & 1 ? 4 : 2, G: heaps & 2 ? 4 : 2, B: heaps & 4 ? 4 : 2 };
    this.tables = {};
    for (const t of Object.keys(this.nrows).map(Number).sort((a, b) => a - b)) {
      if (!(t in SCHEMA)) throw new NotAssembly(`${this.path}: unknown metadata table ${t.toString(16)}`);
      const css = SCHEMA[t].map((c, k) => this.colsize(t, k, c, sizes)), rs = css.reduce((a, b) => a + b, 0), n = this.nrows[t];
      this.tables[t] = [o, rs, css, n];
      o += rs * n;
    }
  }
  colsize(t, k, c, sizes) {
    const ix = INDEX_COLS[`${t},${k}`];
    if (ix !== undefined) return (this.nrows[ix] || 0) < 65536 ? 2 : 4;
    if (typeof c === 'number' && (c === 1 || c === 2 || c === 4)) return c;
    if (c in sizes) return sizes[c];
    if (typeof c === 'string') {
      const [bits, tabs] = CODED[c];
      const big = Math.max(0, ...tabs.filter(x => x !== null).map(x => this.nrows[x] || 0));
      return big < (1 << (16 - bits)) ? 2 : 4;
    }
    return (this.nrows[c] || 0) < 65536 ? 2 : 4;
  }
  rows(t) {
    if (!(t in this._rows)) {
      const out = [];
      if (t in this.tables) {
        const [o, rs, css, n] = this.tables[t];
        for (let i = 0; i < n; i++) {
          let p = o + i * rs; const row = [];
          for (const s of css) { row.push(s === 1 ? this.d[p] : s === 2 ? this.v.getUint16(p, true) : this.v.getUint32(p, true)); p += s; }
          out.push(row);
        }
      }
      this._rows[t] = out;
    }
    return this._rows[t];
  }
  count(t) { return this.nrows[t] || 0; }
  static coded(kind, v) { const [bits, tabs] = CODED[kind]; return [tabs[v & ((1 << bits) - 1)], v >>> bits]; }
  types() {
    if (this._types) return this._types;
    const td = this.rows(TYPEDEF), nested = new Map(this.rows(NESTEDCLASS).map(r => [r[0], r[1]]));
    const nf = this.count(FIELD) + 1, nm = this.count(METHODDEF) + 1, out = [];
    td.forEach(([flags, name, ns, ext, fl, ml], i) => {
      const fe = i + 1 < td.length ? td[i + 1][4] : nf, me = i + 1 < td.length ? td[i + 1][5] : nm;
      out.push({ ns: this.string(ns), name: this.string(name), flags, extends: Assembly.coded('TypeDefOrRef', ext), fields: [fl, fe], methods: [ml, me] });
    });
    out.forEach((t, i) => {
      let full = t.name, k = i + 1;
      const seen = new Set();
      while (nested.has(k) && !seen.has(k)) { seen.add(k); k = nested.get(k); full = out[k - 1].name + '/' + full; t.ns = out[k - 1].ns; }
      t.full = (t.ns ? t.ns + '.' : '') + full;
    });
    return (this._types = out);
  }
  owner(key, row) {                           // the type whose member list holds row
    const ts = this.types();
    this._starts = this._starts || {};
    if (!this._starts[key]) this._starts[key] = ts.map(t => t[key][0]);
    const st = this._starts[key];
    let lo = 0, hi = st.length; while (lo < hi) { const m = (lo + hi) >> 1; if (st[m] <= row) lo = m + 1; else hi = m; }
    let i = lo - 1;
    while (i >= 0 && !(ts[i][key][0] <= row && row < ts[i][key][1])) i--;
    return i >= 0 ? ts[i] : null;
  }
  typeOfMethod(r) { return this.owner('methods', r); }
  typeOfField(r) { return this.owner('fields', r); }
  typerefName(row) { const [, name, ns] = this.rows(TYPEREF)[row - 1], n = this.string(name), s = this.string(ns); return (s ? s + '.' : '') + n; }
  typeName(tab, row) { return tab === TYPEDEF ? this.types()[row - 1].full : tab === TYPEREF ? this.typerefName(row) : '?'; }
  pinvokes() {
    const mods = this.rows(MODULEREF).map(r => this.string(r[0])), methods = this.rows(METHODDEF), out = [];
    for (const [, fwd, entry, scope] of this.rows(IMPLMAP)) {
      const [tab, row] = Assembly.coded('MemberForwarded', fwd);
      if (tab !== METHODDEF || !(row >= 1 && row <= methods.length) || !(scope >= 1 && scope <= mods.length)) continue;
      const m = methods[row - 1], t = this.typeOfMethod(row), name = this.string(m[3]);
      out.push({ module: mods[scope - 1], entry: this.string(entry) || name, method: name, type: t ? t.full : '?', sig: this.methodSig(this.blob(m[4])) });
    }
    return out;
  }
  fieldConstants() {
    const fields = this.rows(FIELD), out = {};
    for (const [typ, , parent, val] of this.rows(CONSTANT)) {
      const [tab, row] = Assembly.coded('HasConstant', parent);
      if (tab !== FIELD || !(row >= 1 && row <= fields.length)) continue;
      const t = this.typeOfField(row), b = this.blob(val), dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
      let v = null;
      if ((typ === E.I4 || typ === E.U4) && b.length === 4) v = typ === E.I4 ? dv.getInt32(0, true) : dv.getUint32(0, true);
      else if ((typ === E.I8 || typ === E.U8) && b.length === 8) v = typ === E.I8 ? dv.getBigInt64(0, true) : dv.getBigUint64(0, true);
      else if ((typ === E.I2 || typ === E.U2) && b.length === 2) v = typ === E.I2 ? dv.getInt16(0, true) : dv.getUint16(0, true);
      else if ((typ === E.I1 || typ === E.U1 || typ === E.BOOL) && b.length === 1) v = b[0];
      else if (typ === E.STRING) v = utf16.decode(b);
      if (t) out[t.full + '.' + this.string(fields[row - 1][1])] = v;
    }
    return out;
  }
  // signatures: ['prim', e] | ['ptr', t] | ['byref', t] | ['valuetype'|'class', name, [tab, row]] | ['array', t] | ['fnptr']
  typeAt(b, o) {
    let e = b[o++];
    while (e === E.CMOD_REQD || e === E.CMOD_OPT) { o = this.cuint(o, b)[1]; e = b[o++]; }
    if (e === E.PINNED) return this.typeAt(b, o);
    if (e in PRIM) return [['prim', e], o];
    if (e === E.PTR || e === E.BYREF) { const [t, p] = this.typeAt(b, o); return [[e === E.PTR ? 'ptr' : 'byref', t], p]; }
    if (e === E.VALUETYPE || e === E.CLASS) {
      const [v, p] = this.cuint(o, b), [tab, row] = Assembly.coded('TypeDefOrRef', v);
      return [[e === E.VALUETYPE ? 'valuetype' : 'class', this.typeName(tab, row), [tab, row]], p];
    }
    if (e === E.SZARRAY) { const [t, p] = this.typeAt(b, o); return [['array', t], p]; }
    if (e === E.ARRAY) {
      let [t, p] = this.typeAt(b, o); p = this.cuint(p, b)[1];
      let n; [n, p] = this.cuint(p, b); for (let i = 0; i < n; i++) p = this.cuint(p, b)[1];
      [n, p] = this.cuint(p, b); for (let i = 0; i < n; i++) p = this.cuint(p, b)[1];
      return [['array', t], p];
    }
    if (e === E.GENERICINST) {
      const k = b[o++]; let p = this.cuint(o, b)[1], n; [n, p] = this.cuint(p, b);
      for (let i = 0; i < n; i++) p = this.typeAt(b, p)[1];
      return [[k === E.VALUETYPE ? 'valuetype' : 'class', 'generic', null], p];
    }
    if (e === E.VAR || e === E.MVAR) return [['class', 'generic parameter'], this.cuint(o, b)[1]];
    if (e === E.FNPTR) return [['fnptr'], this.sigAt(b, o)[1]];
    throw new RangeError(`element type ${e}`);
  }
  sigAt(b, o) {
    const cc = b[o++];
    if (cc & 0x10) o = this.cuint(o, b)[1];
    let n; [n, o] = this.cuint(o, b);
    let ret; [ret, o] = this.typeAt(b, o);
    const ps = [];
    for (let i = 0; i < n; i++) { if (b[o] === E.SENTINEL) o++; let t; [t, o] = this.typeAt(b, o); ps.push(t); }
    return [[ret, ps], o];
  }
  methodSig(b) { try { return this.sigAt(b, 0)[0]; } catch (e) { if (e instanceof RangeError || e instanceof TypeError) return null; throw e; } }
  fieldSig(b) { try { return b.length && b[0] === 0x06 ? this.typeAt(b, 1)[0] : null; } catch (e) { if (e instanceof RangeError || e instanceof TypeError) return null; throw e; } }
  typedefLayout(row) {                        // [base type name, [[field name, type]], explicit size | null, flags]
    const t = this.types()[row - 1], [tab, r] = t.extends, base = r ? this.typeName(tab, r) : null, fields = this.rows(FIELD), out = [];
    for (let i = t.fields[0]; i < t.fields[1]; i++) { const [flags, name, sig] = fields[i - 1]; if (flags & 0x10) continue; out.push([this.string(name), this.fieldSig(this.blob(sig))]); }
    let size = null;
    for (const [, sz, parent] of this.rows(CLASSLAYOUT)) if (parent === row && sz) size = sz;
    return [base, out, size, t.flags];
  }
  assemblyRefs() { return this.rows(ASSEMBLYREF).map(r => this.string(r[6])); }
  typerefScope(row) {
    const [tab, r] = Assembly.coded('ResolutionScope', this.rows(TYPEREF)[row - 1][0]);
    return tab === ASSEMBLYREF ? this.string(this.rows(ASSEMBLYREF)[r - 1][6]) : null;
  }
}
return { Assembly, NotAssembly, E, PRIM, PRIM_SIZE, TYPEREF, TYPEDEF, FIELD, METHODDEF, ASSEMBLYREF, MODULEREF };
})();
if (typeof module !== 'undefined') module.exports = CLRMETA;
