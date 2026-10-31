// Unity's Android player out of a donor APK (a Unity game's Android build of the same version), as
// pmunity's `player donor` lays it out: lib/<abi>/libmain.so, libunity.so and libmono*.so, the engine's
// UnityEngine*.dll and `unity default resources`, plus player.json. Same files, same bytes, same player.json.
'use strict';
const PLAYER = (() => {
const CORE_ = typeof CORE !== 'undefined' ? CORE : require('./core.js');
const MANAGED = /^assets\/bin\/Data\/Managed\/UnityEngine(\.\w+Module)?\.dll$/;

function has(hay, needle) {                     // bytes.includes for a byte string
  const n = needle.length, f = needle[0];
  for (let i = hay.indexOf(f); i >= 0 && i <= hay.length - n; i = hay.indexOf(f, i + 1)) {
    let k = 1; while (k < n && hay[i + k] === needle[k]) k++;
    if (k === n) return true;
  }
  return false;
}
const readAll = async (blob, e) => new Uint8Array(await new Response(await CORE_.zipStream(blob, e)).arrayBuffer());

// donor({version, apks: [{name, blob}], backend, abi, put(rel, bytes)}) -> player.json's object;
// put gets each file's path in the player folder, player.json last
async function donor({ version, apks, backend = 'mono', abi = 'armeabi-v7a', managed = false, put }) {
  const got = [], enc = new TextEncoder();
  for (const apk of apks) {
    const entries = await CORE_.zipOpen(apk.blob);
    const lib = entries.find(e => e.name === `lib/${abi}/libunity.so`);
    if (lib && !has(await readAll(apk.blob, lib), enc.encode(version))) throw new Error(`${apk.name}: its player is not Unity ${version}`);
    for (const e of entries) {
      const n = e.name, base = n.slice(n.lastIndexOf('/') + 1);
      let rel = null;
      if (lib && (n === `lib/${abi}/libmain.so` || n === `lib/${abi}/libunity.so`)) rel = `Variations/${backend}/Release/Libs/${abi}/${base}`;
      else if (lib && backend === 'mono' && n.startsWith(`lib/${abi}/libmono`)) rel = `Variations/mono/Release/MonoLibs/${abi}/${base}`;
      else if (MANAGED.test(n) && (backend === 'mono' || managed)) rel = `Variations/${backend}/Managed/${base}`;
      else if (n === 'assets/bin/Data/unity default resources') rel = 'Data/unity default resources';
      if (rel === null) continue;
      await put(rel, await readAll(apk.blob, e));
      got.push(rel);
    }
  }
  const libs = `Variations/${backend}/Release/Libs/${abi}/`;
  const need = [libs + 'libmain.so', libs + 'libunity.so', 'Data/unity default resources'];
  if (backend === 'mono' || managed) need.push(`Variations/${backend}/Managed/UnityEngine.dll`);
  if (backend === 'mono') need.push(`Variations/mono/Release/MonoLibs/${abi}/libmono.so`);
  const missing = need.filter(n => !got.some(g => g.startsWith(n)));
  if (missing.length) throw new Error(`the donors have no ${missing.join(', ')} for ${backend}/${abi}`);
  const info = { version, backend, abi, donors: apks.map(a => a.name), files: got.length };
  await put('player.json', enc.encode(pyJson(info)));
  return info;
}
function pyJson(v, ind = '') {                  // Python's json.dump(v, indent=1)
  const i2 = ind + ' ';
  if (Array.isArray(v)) return v.length ? `[\n${v.map(x => i2 + pyJson(x, i2)).join(',\n')}\n${ind}]` : '[]';
  if (v && typeof v === 'object') { const k = Object.keys(v); return k.length ? `{\n${k.map(x => `${i2}${JSON.stringify(x)}: ${pyJson(v[x], i2)}`).join(',\n')}\n${ind}}` : '{}'; }
  return JSON.stringify(v).replace(/[\u007f-￿]/g, c => '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0'));
}
return { donor, pyJson };
})();
if (typeof module !== 'undefined') module.exports = PLAYER;
