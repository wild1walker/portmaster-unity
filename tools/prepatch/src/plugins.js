// pmunity's plugins.py: which native libraries a game's managed code P/Invokes (clrmeta.js), what the Android-player
// runtime gives each (stub / lib / host / none), and plugins.map, the file ua_host reads (map and sig lines). The
// parts that decide the map and the FMOD check; the report's file descriptions are left out (nothing reads them).
'use strict';
const PLUGINS = (() => {
const CM = typeof CLRMETA !== 'undefined' ? CLRMETA : require('./clrmeta.js');
const E = CM.E;
// [id, DllImport name pattern (without lib prefix / extension), title, default use, stub id]
const SDKS = [
  ['fmod', /^fmod(studio)?l?(64)?$/i, 'FMOD Engine / FMOD Studio', 'lib', null],
  ['steamworks', /^steam_api(64)?$/i, 'Steamworks', 'stub', 'steam'],
  ['steam-encryptedappticket', /^sdkencryptedappticket(64)?$/i, 'Steamworks encrypted app tickets', 'none', null],
  ['csteamworks', /^csteamworks$/i, 'Steamworks.NET (CSteamworks glue, before 9.0)', 'stub', 'csteamworks'],
  ['discord-game-sdk', /^discord_game_sdk$/i, 'Discord Game SDK', 'stub', 'discord_game_sdk'],
  ['discord-rpc', /^discord-rpc$/i, 'Discord RPC', 'stub', 'discord-rpc'],
  ['discord-partner-sdk', /^discord_partner_sdk$/i, 'Discord Social SDK', 'none', null],
  ['galaxy', /^galaxy(csharpglue|peer)?(64)?$/i, 'GOG Galaxy SDK', 'none', null],
  ['eos', /^eossdk-.*$/i, 'Epic Online Services', 'stub', 'eos'],
  ['wwise', /^aksoundengine$/i, 'Audiokinetic Wwise', 'lib', null],
  ['sqlite', /^(e_)?sqlite3?$/i, 'SQLite', 'host', null],
  ['standalonefilebrowser', /^standalonefilebrowser$/i, 'StandaloneFileBrowser (native file dialogs)', 'none', null],
  ['xinput', /^xinputinterface(32|64)?$/i, 'XInputDotNet', 'none', null],
  ['rewired', /^rewired_.*$/i, 'Rewired native helper', 'none', null],
  ['steamaudio', /^phonon$/i, 'Steam Audio', 'lib', null],
  ['sdl2', /^sdl2(-2\.0)?$/i, 'SDL2', 'keep', null],
  ['openal', /^(soft_oal|openal(32)?)$/i, 'OpenAL', 'keep', null],
  ['udev', /^udev$/i, 'libudev (joystick enumeration, e.g. Rewired on Linux)', 'keep', null],
];
const HOST_DEFAULT = { sqlite: 'libsqlite3.so.0' };
const SYSTEM = /^(__internal|libc|c|libc\.so(\.6)?|libdl(\.so(\.2)?)?|dl|libm|m|libpthread|pthread|libz|z|zlib1?|monoposixhelper|system\.native|system\.net\.security\.native|system\.globalization\.native|system\.io\.compression\.native|libmono-btls-shared|mono-native)$/i;
const WINDOWS = /(^(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|ws2_32|winmm|comdlg32|crypt32|iphlpapi|winspool|ntdll|dwmapi|shcore|xinput[0-9_]*|dinput8|psapi|version|imm32|setupapi|hid|dbghelp|d3d\w*|dxgi|opengl32|msvcrt|ucrtbase)(\.dll|\.drv)?$)|\.dll$/i;
const MACOS = /^\/System\/|^\/usr\/lib\/|\.dylib$|\.framework|\.bundle$/i;
const FRAMEWORK = /^(mscorlib|netstandard|System|System\..*|Mono\.(Security|Posix|Cecil.*|CompilerServices.*|Simd|Tasklets|WebBrowser)|UnityEngine(\..*)?|Microsoft\..*|Boo\..*|UnityScript.*|mcs|nunit\.framework|ICSharpCode\..*)\.dll$/i;
const STUBS = ['steam', 'csteamworks', 'discord_game_sdk', 'discord-rpc', 'eos'];

function norm(name) {
  let n = name.replace(/\\/g, '/').split('/').pop();
  n = n.replace(/(\.so(\.\d+)*|\.dll|\.dylib|\.bundle)$/i, '');
  if (n.toLowerCase().startsWith('lib') && n.length > 3) n = n.slice(3);
  return n;
}
const sdkOf = name => { const n = norm(name).toLowerCase(); return SDKS.find(s => s[1].test(n)) || null; };

const KNOWN_VALUETYPES = {
  'System.IntPtr': 'i', 'System.UIntPtr': 'i', 'System.Boolean': 'i', 'System.Char': 'i', 'System.SByte': 'i', 'System.Byte': 'i',
  'System.Int16': 'i', 'System.UInt16': 'i', 'System.Int32': 'i', 'System.UInt32': 'i', 'System.Int64': 'l', 'System.UInt64': 'l',
  'System.Single': 'f', 'System.Double': 'd', 'System.Guid': 's4', 'UnityEngine.Vector2': 'F2', 'UnityEngine.Vector3': 'F3',
  'UnityEngine.Vector4': 'F4', 'UnityEngine.Quaternion': 'F4', 'UnityEngine.Color': 'F4', 'UnityEngine.Rect': 'F4',
  'UnityEngine.Color32': 's1', 'UnityEngine.Vector2Int': 's2', 'UnityEngine.Vector3Int': 's3',
};
class Resolver {                             // the Managed folder's assemblies, opened on demand (open(name) -> bytes|null)
  constructor(open) { this.open = open; this.cache = new Map(); }
  get(name) {
    if (!this.cache.has(name)) {
      let a = null;
      const b = this.open(name + '.dll');
      if (b) { try { a = new CM.Assembly(b, name + '.dll'); } catch { a = null; } }
      this.cache.set(name, a);
    }
    return this.cache.get(name);
  }
  find(asm, tab, row) {                      // [assembly, typedef row] | [null, full name] | [null, null]
    if (tab === CM.TYPEDEF) return [asm, row];
    if (tab !== CM.TYPEREF) return [null, null];
    const full = asm.typerefName(row), scope = asm.typerefScope(row), other = scope ? this.get(scope) : null;
    if (!other) return [null, full];
    const i = other.types().findIndex(t => t.full === full);
    return i >= 0 ? [other, i + 1] : [null, full];
  }
}
function leafClasses(res, asm, t, depth = 0) {
  if (depth > 8) return null;
  const k = t[0];
  if (k === 'prim') {
    const e = t[1];
    if (e === E.R4) return [['f', 4]];
    if (e === E.R8) return [['d', 8]];
    if (e === E.I8 || e === E.U8) return [['i', 8]];
    if ([E.STRING, E.OBJECT, E.I, E.U].includes(e)) return [['i', 4]];
    return [['i', CM.PRIM_SIZE[e] ?? 4]];
  }
  if (['ptr', 'byref', 'class', 'array', 'fnptr'].includes(k)) return [['i', 4]];
  if (k === 'valuetype') {
    const name = t[1], ref = t[2];
    if (ref === null) return null;
    const [a, row] = res.find(asm, ...ref);
    if (a === null) {
      const v = KNOWN_VALUETYPES[typeof row === 'string' ? row : name];
      if (v === undefined) return null;
      if (['i', 'l', 'f', 'd'].includes(v)) return [[v === 'l' ? 'i' : v, v === 'l' || v === 'd' ? 8 : 4]];
      const n = +v.slice(1);
      return Array.from({ length: n }, () => v[0] === 'F' ? ['f', 4] : ['i', 4]);
    }
    const [base, fields, size] = a.typedefLayout(row);
    if (base === 'System.Enum') {
      for (const [fname, ft] of fields) if (fname === 'value__' && ft) return leafClasses(res, a, ft, depth + 1);
      return [['i', 4]];
    }
    let out = [];
    for (const [, ft] of fields) {
      if (ft === null) return null;
      const sub = leafClasses(res, a, ft, depth + 1);
      if (sub === null) return null;
      out = out.concat(sub);
    }
    const sum = out.reduce((s, x) => s + x[1], 0);
    if (size && sum < size) out.push(['i', size - sum]);
    return out.length ? out : [['i', 4]];
  }
  return null;
}
function abiToken(res, asm, t) {
  if (t[0] === 'prim' && t[1] === E.VOID) return 'v';
  const leaves = leafClasses(res, asm, t);
  if (leaves === null) return '?';
  if (t[0] === 'prim' || (leaves.length === 1 && t[0] !== 'valuetype')) {
    const [kind, size] = leaves[0];
    return kind === 'f' ? 'f' : kind === 'd' ? 'd' : size === 8 ? 'l' : 'i';
  }
  const kinds = new Set(leaves.map(x => x[0])), only = c => kinds.size === 1 && kinds.has(c);
  if (t[0] === 'valuetype' && leaves.length === 1 && !only('f') && !only('d')) return leaves[0][1] === 8 ? 'l' : 'i';
  if (only('f') && leaves.length <= 4) return `F${leaves.length}`;
  if (only('d') && leaves.length <= 4) return `D${leaves.length}`;
  let size = 0, align = 4;
  for (const [, s] of leaves) { const a = s === 8 ? 8 : Math.min(4, s); align = Math.max(align, a); size = Math.floor((size + a - 1) / a) * a + s; }
  const words = Math.floor((size + 3) / 4);
  return align === 8 ? `S${Math.floor((words + 1) / 2) * 2}` : `s${words}`;
}
const needsThunk = ([ret, params]) => [ret, ...params].some(x => 'fdFD'.includes(x[0]));
const cmpStr = (a, b) => a < b ? -1 : a > b ? 1 : 0;

// survey(managedNames, open(name) -> bytes|null) -> {modules: {name: {use, stub?, lib?, entries}}, sigs, fmod: bool, map}
function survey(names, open) {
  const res = new Resolver(open), pins = [], consts = {};
  for (const n of [...names].filter(x => x.endsWith('.dll') && !FRAMEWORK.test(x)).sort(cmpStr)) {
    const a = res.get(n.slice(0, -4));
    if (!a) continue;
    try {
      for (const p of a.pinvokes()) {
        const sig = p.sig;
        p.abi = sig ? [abiToken(res, a, sig[0]), sig[1].map(x => abiToken(res, a, x))] : null;
        p.assembly = n; pins.push(p);
      }
      Object.assign(consts, a.fieldConstants());
    } catch (e) { if (!(e instanceof RangeError || e instanceof TypeError)) throw e; }
  }
  const mods = new Map();
  for (const p of pins) { if (!mods.has(p.module)) mods.set(p.module, new Map()); mods.get(p.module).set(p.entry, p.abi); }
  const out = {};
  for (const name of [...mods.keys()].sort((a, b) => cmpStr(a.toLowerCase(), b.toLowerCase()))) {
    const sdk = sdkOf(name), e = { entries: mods.get(name).size };
    if (sdk) { e.use = sdk[3]; if (sdk[4]) e.stub = sdk[4]; if (sdk[3] === 'host') e.lib = HOST_DEFAULT[sdk[0]] || null; }
    else if (SYSTEM.test(name) || WINDOWS.test(name) || MACOS.test(name)) e.use = 'system';
    else e.use = 'none';
    out[name] = e;
  }
  const sigs = [];
  for (const name of [...mods.keys()].sort(cmpStr))
    for (const entry of [...mods.get(name).keys()].sort(cmpStr)) {
      const abi = mods.get(name).get(entry);
      if (abi && needsThunk(abi) && ![abi[0], ...abi[1]].includes('?')) sigs.push([name, entry, abi[0], abi[1]]);
    }
  const fmod = Object.entries(consts).some(([k, v]) => k.endsWith('FMOD.VERSION.number') && typeof v === 'number') ||
    pins.some(p => { const s = sdkOf(p.module); return s && s[0] === 'fmod'; });
  const rep = { modules: out, sigs, fmod };
  rep.map = defaultMap(rep);
  return rep;
}
function defaultMap(rep) {
  const m = {};
  for (const [name, e] of Object.entries(rep.modules)) {
    if (e.use === 'system' || e.use === 'keep') continue;
    const ent = { use: e.use };
    if (e.use === 'stub') ent.stub = e.stub;
    else if (e.use === 'host') { if (!e.lib) continue; ent.lib = e.lib; }
    else if (e.use === 'lib') ent.file = 'lib' + norm(name) + '.so';
    m[name] = ent;
  }
  return m;
}
function mergeMap(rep, portMap) {
  const m = { ...rep.map }, pm = portMap && typeof portMap === 'object' ? (portMap.plugins || portMap) : {};
  for (let [name, ent] of Object.entries(pm)) {
    if (typeof ent === 'string') ent = { use: ent };
    if (!ent || !['lib', 'stub', 'host', 'none'].includes(ent.use)) throw new Error(`plugin map: ${name}: "use" must be lib, stub, host or none`);
    m[name] = ent;
  }
  return m;
}
function mapText(rep, pmap) {
  const lines = ['# plugins.map (pmunity android-data; docs/plugins.md): what the runtime gives each DllImport name',
    '#   map <name> lib <file in lib/> | stub <id> | host <system library> | none',
    '#   sig <name> <entry> <result> <args>: argument classes for the softfp thunks of glibc libraries'];
  for (const name of Object.keys(pmap).sort((a, b) => cmpStr(a.toLowerCase(), b.toLowerCase()))) {
    const e = pmap[name], u = e.use;
    if (/\s/.test(name)) continue;
    if (u === 'lib') lines.push(`map ${name} lib ${e.file || 'lib' + norm(name) + '.so'}`);
    else if (u === 'stub') {
      const st = e.stub || (sdkOf(name) || [])[4] || null;
      if (!STUBS.includes(st)) throw new Error(`plugin map: ${name}: no stub ${st}`);
      lines.push(`map ${name} stub ${st}`);
    } else if (u === 'host') lines.push(`map ${name} host ${e.lib}`);
    else lines.push(`map ${name} none`);
  }
  const libs = new Set(Object.entries(pmap).filter(([, e]) => e.use === 'lib' || e.use === 'host').map(([n]) => n));
  for (const [name, entry, ret, params] of rep.sigs) {
    if (/\s/.test(name + entry) || !libs.has(name)) continue;
    lines.push(`sig ${name} ${entry} ${ret} ${params.join(',') || '-'}`);
  }
  return lines.join('\n') + '\n';
}
return { survey, mergeMap, mapText, norm, sdkOf };
})();
if (typeof module !== 'undefined') module.exports = PLUGINS;
