// pmunity's shaders step for OpenGL Core data (shaders/glsl.py, blob.py, convert.py add_gles3, step.py): every
// Shader with GLCore programs gets a GLES3Plus blob of GLSL ES 3.00 (or 3.10) programs and GLES3 twins of its
// GLCore subprograms, as the handheld's setup writes them. Direct3D 11, Vulkan and Metal sources, compute kernels,
// grab_fetch and fused_post are pmunity's native or other paths: such data is left to the handheld (Unsupported).
'use strict';
const SHADERS = (() => {
// ---------------------------------------------------------------- glsl.py
class Unsupported extends Error {}
class NeedsES31 extends Unsupported {}
const VERSIONS = ['150', '330'];
const DROP_EXTENSIONS = new Set(['GL_ARB_explicit_attrib_location', 'GL_ARB_shader_bit_encoding']);
const SAMPLER_TYPES = ['sampler2D', 'sampler3D', 'samplerCube', 'sampler2DShadow', 'samplerCubeShadow', 'sampler2DArray',
  'sampler2DArrayShadow', 'isampler2D', 'isampler3D', 'isamplerCube', 'isampler2DArray', 'usampler2D', 'usampler3D', 'usamplerCube',
  'usampler2DArray', 'sampler2DMS', 'isampler2DMS', 'usampler2DMS'];
const R = (s, f = '') => new RegExp(s, f);
const UNSUPPORTED = [
  [R(String.raw`\bgl_(Vertex|Normal|Color|SecondaryColor|MultiTexCoord\d|ModelView\w*|Projection\w*|NormalMatrix|TextureMatrix\w*|FragColor|FragData)\b`), 'fixed-function built-in'],
  [R(String.raw`\b(sampler1D\w*|sampler2DRect\w*|samplerBuffer|[iu]?sampler2DMSArray|image\w+D\w*)\b`), 'sampler type'],
  [R(String.raw`\b(texture1D\w*|texture2D\w*|texture3D\w*|textureCube\w*|shadow2D\w*)\s*\(`), 'GLSL 1.10 texture function'],
  [R(String.raw`\b(textureQueryLod|textureQueryLevels|fma|imageLoad|imageStore|atomic\w+|memoryBarrier\w*|barrier|interpolateAt\w+)\s*\(`), 'ES 3.2+ / desktop function'],
  [R(String.raw`\b(noperspective|sample|patch|subroutine|precise)\b(?=\s+(in|out|uniform|centroid|flat|smooth|vec|float|int|uint|mat|bool))`), 'qualifier'],
  [R(String.raw`layout\s*\([^)]*\b(binding|offset|std430|index)\b`), 'layout qualifier'],
  [R(String.raw`\bbuffer\s+\w+\s*\{`), 'storage buffer'],
  [R(String.raw`\b(gl_ClipDistance|gl_PrimitiveID|gl_Layer|gl_ViewportIndex|gl_SampleID|gl_SamplePosition|gl_SampleMask\w*)\b`), 'built-in variable'],
  [R(String.raw`\b(double|dvec\d|dmat\d)\b`), 'double precision'],
];
const ES31 = [
  [R(String.raw`\b[iu]?sampler2DMS\b`), 'sampler type'],
  [R(String.raw`\b(textureGather\w*|bitfield\w+|bitCount|findLSB|findMSB|packUnorm4x8|packSnorm4x8|unpackUnorm4x8|unpackSnorm4x8|uaddCarry|usubBorrow|umulExtended|imulExtended|frexp|ldexp)\s*\(`), 'ES 3.1 function'],
];
const VERSIONS_4X = ['400', '410', '420', '430', '440', '450'];
const DROP_EXTENSIONS_4X = new Set([...DROP_EXTENSIONS, 'GL_ARB_shading_language_420pack', 'GL_ARB_shader_storage_buffer_object', 'GL_ARB_shader_image_load_store']);
const IMAGE_TYPES = ['image2D', 'image3D', 'imageCube', 'image2DArray', 'iimage2D', 'iimage3D', 'iimageCube', 'iimage2DArray',
  'uimage2D', 'uimage3D', 'uimageCube', 'uimage2DArray'];
const IMAGE_FORMATS = ['rgba32f', 'rgba16f', 'r32f', 'rgba8', 'rgba8_snorm', 'rgba32i', 'rgba16i', 'rgba8i', 'r32i', 'rgba32ui', 'rgba16ui', 'rgba8ui', 'r32ui'];
const IMAGE_DECL = () => R(String.raw`^(.*)\b(${IMAGE_TYPES.join('|')})\s+(\w+)\s*[\[;]`, 'gm');
const PRECISE_DUMMY = () => /^precise ([biu]?vec4) (u_xlat_precise_\1);[ \t]*\n/gm;
const VARYING_LOCATION = {
  VERTEX: () => R(String.raw`^([ \t]*)layout\s*\(\s*location\s*=\s*\d+\s*\)[ \t]*((?:(?:invariant|flat|smooth|centroid)\s+)*out\s)`, 'gm'),
  FRAGMENT: () => R(String.raw`^([ \t]*)layout\s*\(\s*location\s*=\s*\d+\s*\)[ \t]*((?:(?:invariant|flat|smooth|centroid)\s+)*in\s)`, 'gm'),
};
const UNSUPPORTED_4X = [
  UNSUPPORTED[0],
  [R(String.raw`\b([iu]?sampler1D\w*|[iu]?sampler2DRect\w*|[iu]?samplerBuffer|[iu]?sampler2DMSArray|[iu]?samplerCubeArray\w*|[iu]?image(?:1D\w*|2DRect|Buffer|CubeArray|2DMS\w*))\b`), 'sampler type'],
  UNSUPPORTED[2],
  [R(String.raw`\b(textureQueryLod|textureQueryLevels|textureSamples|imageSamples|fma|imageAtomic\w+|atomicCounter\w*|barrier|groupMemoryBarrier|memoryBarrierShared|interpolateAt\w+)\s*\(`), 'ES 3.2+ / desktop function'],
  [R(String.raw`\b(noperspective|sample|patch|subroutine|precise)\b(?=\s+(in|out|uniform|centroid|flat|smooth|[biud]?vec|float|int|uint|mat|bool|highp|mediump|lowp))`), 'qualifier'],
  [R(String.raw`layout\s*\([^)]*\b(index|component|xfb_\w+|origin_upper_left|pixel_center_integer|depth_\w+|offset|align|stream)\b`), 'layout qualifier'],
  [R(String.raw`^[ \t]*(?:layout\s*\([^)]*\)\s*)?(?:(?:flat|smooth|centroid)\s+)*(?:in|out)\s+\w+\s*\{`, 'm'), 'interface block'],
  [R(String.raw`\b(gl_ClipDistance|gl_CullDistance|gl_PrimitiveID|gl_Layer|gl_ViewportIndex|gl_SampleID|gl_SamplePosition|gl_SampleMask\w*|gl_PerVertex)\b`), 'built-in variable'],
  UNSUPPORTED[UNSUPPORTED.length - 1],
];
const ES31_4X = [...ES31,
  [R(String.raw`layout\s*\([^)]*\b(binding|std430|early_fragment_tests)\b`), 'layout qualifier'],
  [R(String.raw`layout\s*\([^)]*\blocation\b[^)]*\)\s*(?:(?:highp|mediump|lowp)\s+)?uniform\b`), 'uniform location'],
  [R(String.raw`\bbuffer\s+\w+\s*\{`), 'storage buffer'],
  [R(String.raw`\b(${IMAGE_TYPES.join('|')})\b`), 'image'],
  [R(String.raw`\b(imageLoad|imageStore|imageSize|atomic(?:Add|Min|Max|And|Or|Xor|Exchange|CompSwap)|memoryBarrier(?:Buffer|Image|AtomicCounter)?)\s*\(`), 'ES 3.1 function'],
];
const escRe = s => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
const pyRepr = s => s.includes("'") && !s.includes('"') ? `"${s}"` : `'${s.replace(/\\/g, '\\\\').replace(/'/g, "\\'")}'`;
const pyStrip = s => s.replace(/^[\s\x1c-\x1f\x85]+|[\s\x1c-\x1f\x85]+$/g, '');
function splitlines(s) {                     // str.splitlines(keepends=True)
  const out = [], rx = /\r\n|[\n\r\v\f\x1c\x1d\x1e\x85\u2028\u2029]/g;
  let last = 0, m;
  while ((m = rx.exec(s))) { out.push(s.slice(last, m.index + m[0].length)); last = m.index + m[0].length; }
  if (last < s.length) out.push(s.slice(last));
  return out;
}
const replaceFirst = (s, a, b) => { const i = s.indexOf(a); return i < 0 ? s : s.slice(0, i) + b + s.slice(i + a.length); };

function dropPreciseDummies(src) {
  for (const m of [...src.matchAll(PRECISE_DUMMY())]) {
    const rest = replaceFirst(src, m[0], '');
    if (!R(String.raw`\b${m[2]}\b`).test(rest)) src = rest;
  }
  return src;
}
function checkImages(code, stage) {
  for (const m of code.matchAll(IMAGE_DECL())) {
    const lay = [...m[1].matchAll(/layout\s*\(([^)]*)\)/g)].map(x => x[1]);
    if (!lay.some(q => R(String.raw`\b(${IMAGE_FORMATS.join('|')})\b`).test(q)))
      throw new Unsupported(`${stage}: image without a format qualifier ${pyRepr(m[3])} (GLSL ES needs the format, the program does not give it)`);
  }
}
function stageBlocks(code) {                 // [[stage, start, end]]
  const out = [], lines = splitlines(code);
  let pos = 0, i = 0;
  while (i < lines.length) {
    const m = /^\s*#\s*ifdef\s+(VERTEX|FRAGMENT|GEOMETRY|HULL|DOMAIN)\b/.exec(lines[i]);
    if (!m) { pos += lines[i].length; i++; continue; }
    const stage = m[1], start = pos + lines[i].length;
    let depth = 1;
    pos = start; i++;
    while (i < lines.length && depth) {
      if (/^\s*#\s*(if|ifdef|ifndef)\b/.test(lines[i])) depth++;
      else if (/^\s*#\s*endif\b/.test(lines[i])) { depth--; if (depth === 0) out.push([stage, start, pos]); }
      pos += lines[i].length; i++;
    }
  }
  return out;
}
const stripComments = s => s.replace(/\/\/[^\n]*|\/\*[\s\S]*?\*\//g, ' ');
const UNROLL_MAX = 16, UNROLL_STATEMENTS = 120, OPTIMIZE = ['const-arrays', 'unroll'];
const NUM = String.raw`[-+]?(?:\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?)[uU]?`;
const TYPES = String.raw`(?:float|int|uint|bool|[biu]?vec[234]|mat[234](?:x[234])?)`;

function constArrays(src) {
  let done = 0;
  for (const m of [...src.matchAll(R(String.raw`^(${TYPES}) (ImmCB_\w+)\[(\d+)\];\n`, 'gm'))]) {
    const typ = m[1], name = m[2], n = parseInt(m[3], 10);
    const rx = () => R(String.raw`^[ \t]*${name}\[(\d+)\] = ((?:${TYPES})\((?:\s*${NUM}\s*,)*\s*${NUM}\s*\)|${NUM});\n`, 'gm');
    const assigns = [...src.matchAll(rx())];
    const vals = new Map();
    for (const a of assigns) { const k = parseInt(a[1], 10); if (!vals.has(k)) vals.set(k, []); vals.get(k).push(a[2]); }
    const keys = [...vals.keys()].sort((x, y) => x - y);
    if (keys.length !== n || keys.some((k, i) => k !== i) || [...vals.values()].some(v => v.length !== 1)) continue;
    const rest = src.replace(rx(), '');
    if (R(String.raw`\b${name}\s*\[[^\]]*\](\.\w+)?\s*([-+*/%&|^]?=(?!=)|\+\+|--)`).test(rest) ||
        R(String.raw`\b${name}\b(?!\s*\[)`).test(rest.split(m[0]).join(''))) continue;
    const mm = /void main\(\)\s*\{\n/.exec(src);
    if (!mm || assigns.some(a => a.index < mm.index + mm[0].length)) continue;
    const head = src.slice(mm.index + mm[0].length, Math.max(...assigns.map(a => a.index + a[0].length)));
    if (pyStrip(head.replace(rx(), ''))) continue;
    const ctor = `${typ}[${n}](${keys.map(i => { const v = vals.get(i)[0]; return v.includes('(') || ['float', 'int', 'uint'].includes(typ) ? v : `${typ}(${v})`; }).join(', ')})`;
    src = rest.split(m[0]).join(`const ${typ} ${name}[${n}] = ${ctor};\n`);
    done++;
  }
  return [src, done];
}
function unroll(src, limit = UNROLL_MAX, statements = UNROLL_STATEMENTS) {
  let done = 0, pos = 0;
  const rx = /^([ \t]*)for\(int (\w+) = (?:int\()?(-?\d+)\)? ?; ?\2 ?(<|<=) ?(-?\d+) ?; ?\2\+\+\)\n\1\{\n/gm;
  for (;;) {
    rx.lastIndex = pos;
    const m = rx.exec(src);
    if (!m) break;
    const mEnd = m.index + m[0].length;
    const ind = m[1], v = m[2], a = parseInt(m[3], 10), op = m[4], b = parseInt(m[5], 10);
    const er = R(String.raw`^${escRe(ind)}\}\n`, 'gm'); er.lastIndex = mEnd;
    const end = er.exec(src);
    if (!end) { pos = mEnd; continue; }
    const body = src.slice(mEnd, end.index);
    const trips = []; for (let k = a; k < (op === '<=' ? b + 1 : b); k++) trips.push(k);
    const semis = (body.match(/;/g) || []).length;
    const bad = !(trips.length > 0 && trips.length <= limit) || trips.length * semis > statements ||
      body.includes('{') || body.includes('}') ||
      /\b(break|continue|return|discard|for|while|do)\b/.test(body) ||
      R(String.raw`^\s*(const\s+)?(highp |mediump |lowp )?${TYPES}\s+\w+`, 'm').test(body) ||
      R(String.raw`\b${v}\s*([-+*/%&|^]?=(?!=)|\+\+|--)`).test(body) ||
      R(String.raw`(\+\+|--)\s*${v}\b`).test(body);
    if (bad) { pos = mEnd; continue; }
    const out = trips.map(k => body.replace(R(String.raw`\b${v}\b`, 'g'), String(k))).join('');
    src = src.slice(0, m.index) + out + src.slice(end.index + end[0].length);
    pos = m.index + out.length;
    done++;
  }
  return [src, done];
}
const codeLines = body => body.replace(/^[ \t]*#[^\n]*/gm, '');

function stageToEs(src, stage, precision = 'highp', optimize = OPTIMIZE, notes = null, es = '300') {
  const head = /^\s*#\s*version\s+(\d+)/.exec(src), gl4 = !!head && VERSIONS_4X.includes(head[1]);
  if (gl4) src = dropPreciseDummies(src);
  const body = stripComments(src), code = codeLines(body);
  for (const [rx, what] of gl4 ? UNSUPPORTED_4X : UNSUPPORTED) { const m = rx.exec(code); if (m) throw new Unsupported(`${stage}: ${what} ${pyRepr(pyStrip(m[0]))}`); }
  if (gl4) checkImages(code, stage);
  if (es === '300') for (const [rx, what] of gl4 ? ES31_4X : ES31) { const m = rx.exec(code); if (m) throw new NeedsES31(`${stage}: ${what} ${pyRepr(pyStrip(m[0]))} (GLSL ES 3.10)`); }
  const lines = splitlines(src), out = [];
  let i = 0;
  while (i < lines.length && !pyStrip(lines[i])) i++;
  const m = i < lines.length ? /^\s*#\s*version\s+(\d+)\s*(\w*)/.exec(lines[i]) : null;
  if (!m) throw new Unsupported(`${stage}: no #version line`);
  if (!((VERSIONS.includes(m[1]) && !m[2]) || (gl4 && ['', 'core'].includes(m[2])))) throw new Unsupported(`${stage}: #version ${m[1]} ${m[2]}`.trimEnd());
  const drop = gl4 ? DROP_EXTENSIONS_4X : DROP_EXTENSIONS;
  out.push(`#version ${es} es\n`);
  i++;
  while (i < lines.length) {
    const s = pyStrip(lines[i]);
    const e = /^#\s*extension\s+(\w+)\s*:\s*(\w+)/.exec(s), w = /^#\s*ifdef\s+(\w+)$/.exec(s);
    if (e) { if (!drop.has(e[1])) out.push(lines[i]); i++; }
    else if (w && i + 2 < lines.length && R(String.raw`^#\s*extension\s+${w[1]}\s*:`).test(pyStrip(lines[i + 1])) && /^#\s*endif\b/.test(pyStrip(lines[i + 2]))) {
      if (!drop.has(w[1])) out.push(...lines.slice(i, i + 3));
      i += 3;
    } else if (!s) i++;
    else break;
  }
  out.push(precisionBlock(body, precision, gl4));
  let rest = lines.slice(i).join('');
  if (gl4 && es === '300' && VARYING_LOCATION[stage]) rest = rest.replace(VARYING_LOCATION[stage](), '$1$2');
  if (optimize.includes('const-arrays')) { let n; [rest, n] = constArrays(rest); if (n && notes) notes.push(`${stage} const-arrays ${n}`); }
  if (optimize.includes('unroll')) { let n; [rest, n] = unroll(rest); if (n && notes) notes.push(`${stage} unroll ${n}`); }
  out.push(rest);
  return out.join('');
}
function precisionBlock(body, precision = 'highp', images = false) {
  const p = ['precision highp float;\n', 'precision highp int;\n'], words = new Set(body.match(/\w+/g) || []);
  for (const t of [...SAMPLER_TYPES, ...(images ? IMAGE_TYPES : [])]) {
    if (!words.has(t)) continue;
    if (precision === 'highp' || (t !== 'sampler2D' && t !== 'samplerCube')) p.push(`precision highp ${t};\n`);
  }
  return p.join('');
}
function programToEs(code, precision = 'highp', optimize = OPTIMIZE, notes = null, es = '300') {
  const blocks = stageBlocks(code);
  if (!blocks.length) throw new Unsupported('no stage blocks');
  const out = [];
  let last = 0;
  for (const [stage, a, b] of blocks) {
    if (stage !== 'VERTEX' && stage !== 'FRAGMENT') throw new Unsupported(`${stage} stage`);
    out.push(code.slice(last, a), stageToEs(code.slice(a, b), stage, precision, optimize, notes, es));
    last = b;
  }
  out.push(code.slice(last));
  return out.join('');
}

// ---------------------------------------------------------------- blob.py
const PLATFORM_D3D11 = 4, PLATFORM_GLES20 = 5, PLATFORM_GLES3PLUS = 9, PLATFORM_METAL = 14, PLATFORM_GLCORE = 15, PLATFORM_VULKAN = 18;
const GPU_GLES31AEP = 2, GPU_GLES31 = 3, GPU_GLES3 = 4, GPU_GLES = 5, GPU_GLCORE32 = 6, GPU_GLCORE41 = 7, GPU_GLCORE43 = 8;
const GPU_SPIRV = 25, GPU_METAL_VS = 23, GPU_METAL_FS = 24;
const SERIALIZERS = { 201609010: [4, 1, false], 201708220: [4, 1, false], 201802150: [4, 1, false], 201806140: [4, 2, false],
  202012090: [4, 1, true], 202605290: [4, 1, true] };
const CANDIDATES = [4, 3, 5, 2].flatMap(n => [[n, 1], [n, 2]]);
const pad4 = n => (n + 3) & ~3;
const utf8f = new TextDecoder('utf-8', { fatal: true }), ascii = new TextDecoder('latin1');
const dv = b => new DataView(b.buffer, b.byteOffset, b.byteLength);
const eqb = (a, b) => { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; };
const cat = parts => { const o = new Uint8Array(parts.reduce((n, x) => n + x.length, 0)); let p = 0; for (const x of parts) { o.set(x, p); p += x.length; } return o; };
const i32 = (...v) => { const b = new Uint8Array(4 * v.length), d = dv(b); v.forEach((x, i) => d.setInt32(4 * i, x, true)); return b; };
const findBytes = (hay, needle, from = 0, to = hay.length) => {
  outer: for (let i = from; i + needle.length <= to; i++) { for (let k = 0; k < needle.length; k++) if (hay[i + k] !== needle[k]) continue outer; return i; }
  return -1;
};
const DXBC = new TextEncoder().encode('DXBC');
const isD3d11 = code => { const i = findBytes(code, DXBC, 0, Math.min(code.length, 64)); return i >= 6 && code.length >= i + 32; };
function isVulkan(code) {
  if (code.length < 20) return false;
  let first = code.length, p = 4, found = false;
  const d = dv(code);
  while (p + 8 <= first && p < 4 + 8 * 9) {
    const off = d.getInt32(p, true), size = d.getInt32(p + 4, true);
    if (size) {
      const magic = ascii.decode(code.subarray(off, off + 4));
      if (size < 0 || off < p + 8 || off + size > code.length || (magic !== 'LOMS' && magic !== '\x03\x02\x23\x07')) return false;
      first = Math.min(first, off); found = true;
    }
    p += 8;
  }
  return found;
}
function readStrings(data, p) {
  const d = dv(data);
  if (p + 4 > data.length) throw new RangeError('end');
  const n = d.getInt32(p, true); p += 4;
  if (!(n >= 0 && n < 4096)) return [null, p];
  const out = [];
  for (let i = 0; i < n; i++) {
    if (p + 4 > data.length) throw new RangeError('end');
    const k = d.getInt32(p, true);
    if (!(k > 0 && k < 1024) || p + 4 + k > data.length) return [null, p];
    const s = data.subarray(p + 4, p + 4 + k);
    if (s.some(c => c > 127)) throw new RangeError('not ascii');
    out.push(ascii.decode(s));
    p = pad4(p + 4 + k);
  }
  return [out, p];
}
function writeStrings(parts, strs) {
  parts.push(i32(strs.length));
  for (const k of strs) { const b = new TextEncoder().encode(k); parts.push(i32(b.length), b, new Uint8Array(pad4(b.length) - b.length)); }
}
class Entry {
  constructor(version, gpuType, stats, keywords, code, rest, local = null) {
    Object.assign(this, { version, gpuType, stats: [...stats], keywords: [...keywords], code, rest, local: local === null ? null : [...local], guessed: false });
  }
  static parse(data) {
    const d = dv(data), version = d.getInt32(0, true), gpu = d.getInt32(4, true), lay = SERIALIZERS[version];
    if (lay) {
      const e = Entry.tryParse(data, version, gpu, lay[0], lay[1]);
      if (!e) throw new Error(`shader program entry (serializer ${version}, gpu type ${gpu}) does not match its layout`);
      return e;
    }
    for (const [n, k] of CANDIDATES) { const e = Entry.tryParse(data, version, gpu, n, k); if (e) { e.guessed = true; return e; } }
    throw new Error(`shader program entry (serializer ${version}, gpu type ${gpu}): unknown layout`);
  }
  static tryParse(data, version, gpu, nstats, nlists) {
    let e;
    try {
      const d = dv(data);
      let p = 8;
      if (p + 4 * nstats > data.length) return null;
      const stats = []; for (let i = 0; i < nstats; i++) stats.push(d.getInt32(p + 4 * i, true));
      p += 4 * nstats;
      let kws; [kws, p] = readStrings(data, p);
      if (kws === null) return null;
      let local = null;
      if (nlists === 2) { [local, p] = readStrings(data, p); if (local === null) return null; }
      if (p + 4 > data.length) return null;
      const n = d.getInt32(p, true);
      if (!(n >= 0 && n <= data.length - p - 4)) return null;
      const code = data.slice(p + 4, p + 4 + n), rest = data.slice(pad4(p + 4 + n));
      if (isD3d11(code)) { /* D3D11 */ }
      else if (gpu === GPU_SPIRV && isVulkan(code)) { /* Vulkan */ }
      else if ((gpu === GPU_METAL_VS || gpu === GPU_METAL_FS) && code[0] === 0xfe && code[1] === 0xca && code[2] === 0x0d && code[3] === 0xf0) { /* Metal */ }
      else if (code.length && !['#', '/'].includes(ascii.decode(code).replace(/^[\s\x1c-\x1f]+/, '').slice(0, 1))) return null;
      else utf8f.decode(code);
      const r = ascii.decode(rest);
      if (r.includes('#version') || r.includes('DXBC')) return null;
      e = new Entry(version, gpu, stats, kws, code, rest, local);
    } catch (ex) { if (ex instanceof RangeError || ex instanceof TypeError) return null; throw ex; }
    return eqb(e.serialize(), data) ? e : null;
  }
  serialize() {
    const parts = [i32(this.version, this.gpuType), i32(...this.stats)];
    writeStrings(parts, this.keywords);
    if (this.local !== null) writeStrings(parts, this.local);
    parts.push(i32(this.code.length), this.code, new Uint8Array(pad4(this.code.length) - this.code.length), this.rest);
    return cat(parts);
  }
  copy() { const e = new Entry(this.version, this.gpuType, this.stats, this.keywords, this.code, this.rest, this.local); e.guessed = this.guessed; return e; }
}
class ParamEntry {
  constructor(version, data) { this.version = version; this.data = data; this.gpuType = null; this.code = new Uint8Array(0); this.guessed = false; }
  static parse(b) { return new ParamEntry(dv(b).getInt32(0, true), b.slice(4)); }
  serialize() { return cat([i32(this.version), this.data]); }
  copy() { return new ParamEntry(this.version, this.data); }
}
class RawEntry {
  constructor(data) { this.data = data; this.gpuType = null; this.code = new Uint8Array(0); this.guessed = false; this.keywords = []; this.local = null; }
  serialize() { return this.data; }
  copy() { return new RawEntry(this.data); }
}
function table(seg0, nested) {
  const d = dv(seg0), count = d.getInt32(0, true), out = [];
  for (let i = 0; i < count; i++) out.push(nested ? [d.getInt32(4 + 12 * i, true), d.getInt32(8 + 12 * i, true), d.getInt32(12 + 12 * i, true)]
    : [d.getInt32(4 + 8 * i, true), d.getInt32(8 + 8 * i, true), 0]);
  return out;
}
function parseBlob(data, params, nested, programs) {
  const segs = Array.isArray(data) ? data : [data], out = [];
  table(segs[0], nested).forEach(([off, len, seg], i) => {
    if (!len) { out.push(null); return; }
    const b = segs[seg].slice(off, off + len);
    if (params.has(i)) { out.push(ParamEntry.parse(b)); return; }
    try { out.push(Entry.parse(b)); }
    catch (ex) { if (programs === null || programs.has(i)) throw ex; out.push(new RawEntry(b)); }
  });
  return out;
}
function buildBlob(entries) {
  const head = 4 + 8 * entries.length, body = [], tab = [];
  let blen = 0;
  for (const e of entries) {
    const b = e ? e.serialize() : new Uint8Array(0);
    tab.push([head + blen, b.length]); body.push(b); blen += b.length;
    const pad = pad4(blen) - blen; if (pad) { body.push(new Uint8Array(pad)); blen += pad; }
  }
  return cat([i32(entries.length), ...tab.map(([o, n]) => i32(o, n)), ...body]);
}
function buildSegments(entries, segments) {
  segments = segments ? [...segments] : entries.map(() => 0);
  const nseg = Math.max(0, ...segments) + 1, head = 4 + 12 * entries.length, bodies = Array.from({ length: nseg }, () => ({ parts: [], len: 0 })), tab = [];
  entries.forEach((e, i) => {
    const s = segments[i], b = e ? e.serialize() : new Uint8Array(0), B = bodies[s];
    tab.push([(s === 0 ? head : 0) + B.len, b.length, s]); B.parts.push(b); B.len += b.length;
    const pad = pad4(B.len) - B.len; if (pad) { B.parts.push(new Uint8Array(pad)); B.len += pad; }
  });
  return [cat([i32(entries.length), ...tab.map(t => i32(...t)), ...bodies[0].parts]), ...bodies.slice(1).map(B => cat(B.parts))];
}
class ShaderBlobs {
  constructor(tree, lz) {
    this.tree = tree; this.lz = lz;
    const raw = tree.compressedBlob, offs = tree.offsets;
    this.nested = offs.length > 0 && Array.isArray(offs[0]);
    this.platforms = new Map();
    Array.from(tree.platforms).forEach((plat, i) => {
      const o = offs[i], c = tree.compressedLengths[i], d = tree.decompressedLengths[i];
      this.platforms.set(plat, this.nested ? o.map((a, k) => lz.unlz4(raw.subarray(a, a + c[k]), d[k])) : lz.unlz4(raw.subarray(o, o + c), d));
    });
    this.stageCounts = 'stageCounts' in tree ? new Map(Array.from(tree.platforms).map((p, i) => [p, tree.stageCounts[i]])) : null;
  }
  entries(platform, params, programs) { return parseBlob(this.platforms.get(platform), params, this.nested, programs); }
  segments(platform) { const d = this.platforms.get(platform); return this.nested ? table(d[0], true).map(t => t[2]) : table(d, false).map(() => 0); }
  setEntries(platform, entries, segments, stageCount) {
    this.platforms.set(platform, this.nested ? buildSegments(entries, segments) : buildBlob(entries));
    if (this.stageCounts && (stageCount != null || !this.stageCounts.has(platform))) this.stageCounts.set(platform, stageCount == null ? 1 : stageCount);
  }
  remove(platform) { this.platforms.delete(platform); if (this.stageCounts) this.stageCounts.delete(platform); }
  store() {                                   // the blobs back into the tree, platforms ascending, LZ4HC 9
    const blob = [], plats = [], offs = [], clens = [], dlens = [];
    let blen = 0;
    for (const plat of [...this.platforms.keys()].sort((a, b) => a - b)) {
      const data = this.platforms.get(plat);
      plats.push(plat);
      if (this.nested) {
        const o = [], c = [], d = [];
        for (const seg of data) { const z = this.lz.lz4(seg, 9); o.push(blen); c.push(z.length); d.push(seg.length); blob.push(z); blen += z.length; }
        offs.push(o); clens.push(c); dlens.push(d);
      } else { const z = this.lz.lz4(data, 9); offs.push(blen); clens.push(z.length); dlens.push(data.length); blob.push(z); blen += z.length; }
    }
    const t = this.tree;
    t.platforms = plats; t.offsets = offs; t.compressedLengths = clens; t.decompressedLengths = dlens; t.compressedBlob = cat(blob);
    if (this.stageCounts) t.stageCounts = plats.map(p => this.stageCounts.get(p));
  }
}

// ---------------------------------------------------------------- convert.py (add_gles3)
const STAGES = ['progVertex', 'progFragment', 'progGeometry', 'progHull', 'progDomain'];
const GLES_TYPES = [GPU_GLES31AEP, GPU_GLES31, GPU_GLES3, GPU_GLES];
const GLCORE4X = [GPU_GLCORE41, GPU_GLCORE43];
const PLATFORM_TYPES = { [PLATFORM_GLCORE]: [GPU_GLCORE32, GPU_GLCORE41, GPU_GLCORE43], [PLATFORM_GLES3PLUS]: [GPU_GLES31AEP, GPU_GLES31, GPU_GLES3],
  [PLATFORM_GLES20]: [GPU_GLES], [PLATFORM_D3D11]: [15, 16, 17, 18, 19, 20, 21, 22] };
const VR_KEYWORDS = ['STEREO_INSTANCING_ON', 'STEREO_MULTIVIEW_ON', 'UNITY_SINGLE_PASS_STEREO', 'STEREO_CUBEMAP_RENDER_ON', 'STEREO_INSTANCING_ENABLED', 'STEREO_DOUBLEWIDE_TARGET'];
function* subprogramLists(tree) {            // [pass, stage, list, parameter indices | null]
  for (const ss of tree.m_ParsedForm.m_SubShaders) for (const p of ss.m_Passes) for (const st of STAGES) {
    const prog = p[st];
    if (!prog) continue;
    if (prog.m_SubPrograms != null) yield [p, st, prog.m_SubPrograms, null];
    const players = prog.m_PlayerSubPrograms;
    if (players && players.length) { const pbi = prog.m_ParameterBlobIndices; for (let tier = 0; tier < players.length; tier++) yield [p, st, players[tier], pbi[tier]]; }
  }
}
function* subprograms(tree) { for (const [p, st, lst] of subprogramLists(tree)) yield [p, st, lst]; }
function paramIndices(tree, platform) {
  const types = PLATFORM_TYPES[platform] || [], out = new Set();
  for (const [, , lst, pi] of subprogramLists(tree)) if (pi !== null) lst.forEach((sp, i) => { if (types.includes(sp.m_GpuProgramType)) out.add(pi[i]); });
  return out;
}
function programIndices(tree, platform) {
  const types = PLATFORM_TYPES[platform] || [], out = new Set();
  for (const [, , lst] of subprograms(tree)) for (const sp of lst) if (types.includes(sp.m_GpuProgramType)) out.add(sp.m_BlobIndex);
  return out;
}
function addPlatform(p, platform) { const pl = p.m_Platforms; if (Array.isArray(pl) && !pl.includes(platform)) { pl.push(platform); pl.sort((a, b) => a - b); } }
const variantKey = sp => JSON.stringify([['m_KeywordIndices', 'm_GlobalKeywordIndices', 'm_LocalKeywordIndices'].filter(k => k in sp)
  .map(k => Array.from(sp[k]).sort((a, b) => a - b)), sp.m_ShaderHardwareTier ?? null]);
const ES_CACHE = new Map();
function programToEsOnce(code, precision, optimize, notes, es = '300') {
  const text = utf8f.decode(code), key = `${precision}\u0000${optimize.join(',')}\u0000${es}\u0000${text}`;
  let hit = ES_CACHE.get(key);
  if (!hit) {
    const added = [];
    let res;
    try { res = new TextEncoder().encode(programToEs(text, precision, optimize, added, es)); } catch (ex) { if (!(ex instanceof Unsupported)) throw ex; res = ex; }
    hit = [res, added]; ES_CACHE.set(key, hit);
  }
  notes.push(...hit[1]);
  if (hit[0] instanceof Error) throw hit[0];
  return hit[0];
}
function addGles3(tree, lz, { precision = 'highp', replace = false, optimize = OPTIMIZE, glcore4x = true } = {}) {
  const name = tree.m_ParsedForm.m_Name, r = { name, status: 'unchanged', changed: false, why: '', programs: 0, twins: 0, guessed: false,
    notes: [], dropped: 0, es31: 0, programs4x: 0, empty: false, failed: [] };
  const plats = Array.from(tree.platforms);
  if (!plats.includes(PLATFORM_GLCORE)) { r.why = 'no OpenGL Core programs'; return r; }
  if (plats.includes(PLATFORM_GLES3PLUS)) {
    if (!replace) { r.why = 'already has GLES3 programs'; return r; }
    throw new Unsupported(`${name}: replace (rebuilding GLES programs)`);
  }
  const blobs = new ShaderBlobs(tree, lz);
  const entries = blobs.entries(PLATFORM_GLCORE, paramIndices(tree, PLATFORM_GLCORE), programIndices(tree, PLATFORM_GLCORE));
  const used = [...new Set([...subprograms(tree)].flatMap(([, , lst]) => lst.filter(sp => sp.m_GpuProgramType === GPU_GLCORE32).map(sp => sp.m_BlobIndex)))].sort((a, b) => a - b);
  const subOf = new Map(), used4 = new Map();
  tree.m_ParsedForm.m_SubShaders.forEach((ss, si) => { for (const p of ss.m_Passes) subOf.set(p, si); });
  if (glcore4x) for (const [p, , lst] of subprograms(tree)) for (const sp of lst) if (GLCORE4X.includes(sp.m_GpuProgramType)) {
    const si = subOf.get(p); if (!used4.has(si)) used4.set(si, new Set()); used4.get(si).add(sp.m_BlobIndex);
  }
  if (!used.length && !used4.size) {
    if (glcore4x) {
      const stages = blobs.stageCounts ? blobs.stageCounts.get(PLATFORM_GLCORE) : null;
      blobs.setEntries(PLATFORM_GLES3PLUS, entries.map(e => e && e.copy()), blobs.segments(PLATFORM_GLCORE), stages);
      blobs.store();
      r.status = 'converted'; r.empty = true; r.why = 'no programs (every variant stripped): an empty GLES3 blob';
      return r;
    }
    r.why = 'no GLCore32 programs'; return r;
  }
  const all4 = new Set([...used4.values()].flatMap(s => [...s]));
  const both = used.filter(i => all4.has(i));
  if (both.length) throw new Error(`${name}: blob entry ${Math.min(...both)} is both a GLCore32 and a GLCore41/43 program`);
  const tr = code => programToEsOnce(code, precision, optimize, r.notes), tr31 = code => programToEsOnce(code, precision, optimize, r.notes, '310');
  const out = entries.map(e => e && e.copy()), dropped = new Set(), es31 = new Set();
  const translateEntry = i => {
    const e = out[i];
    if (e === null) return null;
    if (!(e instanceof Entry)) throw new Error(`${name}: blob entry ${i} is both a program and parameters`);
    r.guessed = r.guessed || e.guessed;
    if (e.code.length) {
      try {
        try { e.code = tr(e.code); }
        catch (ex) { if (!(ex instanceof NeedsES31)) throw ex; e.code = tr31(e.code); es31.add(i); }
      } catch (ex) {
        if (!(ex instanceof Unsupported)) throw ex;
        out[i] = entries[i].copy();
        if ([...e.keywords, ...(e.local || [])].some(k => VR_KEYWORDS.includes(k))) { dropped.add(i); r.dropped++; return 'vr'; }
        return ex;
      }
      r.programs++;
    }
    e.gpuType = GPU_GLES3;
    return null;
  };
  for (const i of used) {
    const ex = translateEntry(i);
    if (ex instanceof Error) { r.status = 'skipped'; r.why = `program ${i}: ${ex.message}`; return r; }
  }
  const done4 = new Map(), counted = new Set(), failed4 = new Set();
  for (const si of [...used4.keys()].sort((a, b) => a - b)) {
    for (const i of [...used4.get(si)].sort((a, b) => a - b)) {
      if (!done4.has(i)) { const n = r.programs; done4.set(i, translateEntry(i)); if (r.programs > n) counted.add(i); }
      if (done4.get(i) instanceof Error) { failed4.add(si); r.failed.push([si, `GLCore41/43 program ${i}: ${done4.get(i).message}`]); break; }
    }
  }
  const keep = new Set([...used4].filter(([si]) => !failed4.has(si)).flatMap(([, s]) => [...s]));
  for (const i of done4.keys()) if (!keep.has(i)) { out[i] = entries[i] ? entries[i].copy() : null; es31.delete(i); if (counted.has(i)) r.programs--; }
  r.programs4x = [...counted].filter(i => keep.has(i)).length;
  const twinned = (p, sp) => { const t = sp.m_GpuProgramType; return t === GPU_GLCORE32 || (GLCORE4X.includes(t) && glcore4x && !failed4.has(subOf.get(p))); };
  const ids = new Map(); let nid = 0; const idOf = p => { if (!ids.has(p)) ids.set(p, nid++); return ids.get(p); };
  const k3 = (p, sp) => `${idOf(p)}|${variantKey(sp)}|${sp.m_GpuProgramType}`;
  const skip = new Set(), raised = new Set();
  for (const [p, , lst] of subprograms(tree)) for (const sp of lst) if (twinned(p, sp)) {
    if (dropped.has(sp.m_BlobIndex)) skip.add(k3(p, sp));
    if (es31.has(sp.m_BlobIndex)) raised.add(k3(p, sp));
  }
  for (const [p, , lst] of subprograms(tree)) for (const sp of lst) if (twinned(p, sp) && raised.has(k3(p, sp)) && out[sp.m_BlobIndex] !== null) out[sp.m_BlobIndex].gpuType = GPU_GLES31;
  r.es31 = es31.size;
  const stages = blobs.stageCounts ? blobs.stageCounts.get(PLATFORM_GLCORE) : null, twinsOf = [];
  for (const [p, , lst, pbi] of subprogramLists(tree)) {
    const twins = [];
    lst.forEach((sp, i) => {
      const k = k3(p, sp);
      if (twinned(p, sp) && !skip.has(k)) { const t = structuredClone(sp); t.m_GpuProgramType = raised.has(k) ? GPU_GLES31 : GPU_GLES3; twins.push([t, pbi === null ? null : pbi[i]]); }
    });
    twinsOf.push([p, lst, pbi, twins]);
  }
  if (failed4.size && !twinsOf.some(x => x[3].length)) { r.status = 'skipped'; r.why = r.failed.map(([si, w]) => `SubShader ${si}: ${w}`).join('; '); return r; }
  blobs.setEntries(PLATFORM_GLES3PLUS, out, blobs.segments(PLATFORM_GLCORE), stages);
  blobs.store();
  for (const [p, lst, pbi, twins] of twinsOf) {
    for (const [t, pi] of twins) { lst.push(t); if (pbi !== null) pbi.push(pi); }
    if (twins.length) addPlatform(p, PLATFORM_GLES3PLUS);
    r.twins += twins.length;
  }
  r.status = 'converted';
  if (r.failed.length) r.why = (r.why ? r.why + ' ' : '') + r.failed.map(([si, w]) => `SubShader ${si} left out: ${w}`).join('; ');
  return r;
}

// ---------------------------------------------------------------- step.py (OpenGL Core sources)
// run({fs, cfg, put, journal, log, lz, bundle}): fs as the textures step's (list, read, slice); put(rel, parts);
// bundle: {open(file) -> b|null, serializedNodes, write(b, nodes, lz4hc)} (bundle.js) and lz4hc(chunks)
const SHADER = 48, COMPUTE = 72;
async function run({ fs, cfg, put, journal, log, lz, bundle, lz4hc, unity }) {
  const U = typeof UNITY !== 'undefined' ? UNITY : require('./unity.js');
  const c = cfg.shaders || {}, precision = c.precision || 'highp', bundles = 'bundles' in c ? c.bundles : '.';
  const optimize = c.optimize ? [...c.optimize] : OPTIMIZE, glcore4x = c.glcore4x === undefined || c.glcore4x === null ? true : !!c.glcore4x;
  const compute = c.compute === undefined ? true : c.compute;
  if (c.replace) throw new Unsupported('shaders.replace');
  if (c.grab_fetch && !['off', 'false'].includes(c.grab_fetch)) throw new Unsupported('shaders.grab_fetch');
  if (c.fused_post) throw new Unsupported('shaders.fused_post');
  if ((c.from || ['glcore']).some(s => s !== 'glcore')) throw new Unsupported(`shaders from ${(c.from || []).join(', ')}`);
  const cmp = (a, b) => a < b ? -1 : a > b ? 1 : 0;
  const top = (await fs.list('')).filter(e => !e.dir).map(e => e.name);
  const files = top.filter(n => n.endsWith('.assets') || n === 'globalgamemanagers' || /^level\d+$/.test(n)).sort(cmp);
  files.push(...(await fs.list('Resources').catch(() => [])).filter(e => !e.dir && e.name === 'unity_builtin_extra').map(e => `Resources/${e.name}`));
  if (bundles) {
    const rx = new RegExp(bundles), walk = async d => {
      let ents; try { ents = await fs.list(d); } catch { return; }
      const names = ents.filter(e => !e.dir).map(e => e.name);
      const here = [];
      for (const n of names) if (rx.test(n) && !n.endsWith('.manifest') && !n.endsWith('.meta')) {
        const h = await fs.slice(`${d}/${n}`, 0, 8);
        if (ascii.decode(h) === 'UnityFS\0') here.push(`${d}/${n}`);
      }
      files.push(...here.sort(cmp));
      for (const e of ents.filter(x => x.dir).map(x => x.name).sort(cmp)) await walk(`${d}/${e}`);   // (os.walk order)
    };
    await walk('StreamingAssets');
  }
  const results = [];
  let changedFiles = 0;
  for (const rel of files) {
    if (journal.isDone('shaders', rel)) continue;
    const raw = await fs.read(rel);
    let sf, b = null, sfNode = null;
    if (ascii.decode(raw.subarray(0, 8)) === 'UnityFS\0') {
      b = await bundle.open(new Blob([raw]));
      if (!b) throw new Unsupported(`${rel}: an LZMA or encrypted bundle`);
      const sfs = bundle.serializedNodes(b);
      if (sfs.length !== 1) { log(`shaders: ${rel}: left as it is (not one serialized file)`); journal.markDone('shaders', rel); continue; }
      sfNode = sfs[0];
      sf = U.parseSerialized(await b.node(sfNode.name), sfNode.name);
    } else sf = U.parseSerialized(raw, rel);
    const changed = new Map();
    for (const o of sf.objects) {
      if (o.classId === COMPUTE && compute) throw new Unsupported(`${rel}: compute shaders (compute.py)`);
      if (o.classId !== SHADER) continue;
      const t = U.readObject(sf, o);
      const r = addGles3(t, lz, { precision, optimize, glcore4x });
      r.file = rel; results.push(r);
      if (r.status === 'converted' || r.changed) changed.set(String(o.pathId), U.writeObject(sf, o, t));
      if (r.status === 'skipped') log(`shaders: ${rel}: ${r.name}: ${r.status} ${r.why}`.trimEnd());
      if (r.guessed) log(`shaders: ${rel}: ${r.name}: program layout of an unverified Unity version (guessed, checked by round trip)`);
    }
    if (changed.size) {
      const saved = U.save(sf, changed, { redump: true });       // (UnityPy's save: Loaded.save / write_to)
      if (b) {
        const nodes = [];
        for (const n of b.nodes) nodes.push({ name: n.name, flags: n.flags, parts: n === sfNode ? saved.parts : [await b.node(n.name)] });
        await put(rel, (await bundle.write(b, nodes, lz4hc)).parts);
      } else await put(rel, saved.parts);
      changedFiles++;
    }
    journal.markDone('shaders', rel);
  }
  const conv = results.filter(r => r.status === 'converted');
  const kinds = [...new Set(conv.flatMap(r => r.notes.map(n => n.split(' ').slice(1, -1).join(' '))))].sort(cmp);
  log(`shaders: ${conv.length} shaders given GLES3 programs (${conv.reduce((a, r) => a + r.programs, 0)} programs, ${conv.reduce((a, r) => a + r.twins, 0)} subprograms) in ${changedFiles} files; ` +
    `${results.filter(r => r.status === 'skipped').length} not translatable; precision ${precision}; rewrites: ${conv.reduce((a, r) => a + r.notes.length, 0)} stages (${kinds.join(', ') || 'none'})`);
  if (!glcore4x) log('shaders: GLCore41/43 programs not translated (glcore4x off)');
  void unity;
  return results;
}
return { run, addGles3, programToEs, stageToEs, Unsupported, NeedsES31, ShaderBlobs, Entry };
})();
if (typeof module !== 'undefined') module.exports = SHADERS;
