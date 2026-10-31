// A port's codepatch (Mono.Cecil, C#) built for WebAssembly (.NET 8, browser-wasm: codepatch-wasm), carried in the
// page as one gzipped bundle (tools/build.js: codepatch/). Its runtime starts from that bundle: dotnet.js and its
// two modules as blob: URLs, every other file handed to the runtime's resource loader.
//   files(gz) -> Map(name -> bytes); start(files) -> run(managed [{name, bytes}], args [codepatch's options])
//   -> {rc, log, dll}: the patched Assembly-CSharp.dll codepatch writes, and what it printed
'use strict';
const CODEPATCH = (() => {

async function files(gz) {                  // [u16 name length][name][u32 size][bytes]...
  const raw = new Uint8Array(await new Response(new Blob([gz]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
  const v = new DataView(raw.buffer), out = new Map(), td = new TextDecoder();
  for (let p = 0; p < raw.length;) {
    const n = v.getUint16(p, true), name = td.decode(raw.subarray(p + 2, p + 2 + n)); p += 2 + n;
    const size = v.getUint32(p, true); p += 4;
    out.set(name, raw.subarray(p, p + size)); p += size;
  }
  return out;
}

const TYPES = { js: 'text/javascript', wasm: 'application/wasm', json: 'application/json' };
async function start(fs) {
  const F = n => { const b = fs.get(`_framework/${n}`); if (!b) throw new Error(`codepatch: ${n} missing from the page`); return b; };
  // (the scripts find their files from import.meta.url, which a blob: URL cannot be a base of: a stand-in address,
  // every request to which goes through the resource loader below)
  const base = 'https://codepatch.invalid/_framework/';
  const script = n => new TextDecoder().decode(F(n)).split('import.meta.url').join(JSON.stringify(base + n));
  const urls = new Map(), url = n => { if (!urls.has(n)) urls.set(n, URL.createObjectURL(new Blob([script(n)], { type: TYPES.js }))); return urls.get(n); };
  const { dotnet } = await import(url('dotnet.js'));
  const rt = await dotnet.withResourceLoader((type, name) => {
    const n = name.split('/').pop();
    if (type === 'dotnetjs') return url(n);
    return Promise.resolve(new Response(F(n), { headers: { 'content-type': TYPES[n.split('.').pop()] || 'application/octet-stream' } }));
  }).withConfig({ disableIntegrityCheck: true }).create();
  const ex = await rt.getAssemblyExports(rt.getConfig().mainAssemblyName);
  return {
    run(managed, args) {
      const sizes = managed.map(m => m.bytes.length), all = new Uint8Array(sizes.reduce((a, b) => a + b, 0));
      let p = 0; for (const m of managed) { all.set(m.bytes, p); p += m.bytes.length; }
      const r = ex.Entry.Run(managed.map(m => m.name), all, sizes, args.join(' '));
      const v = new DataView(r.buffer, r.byteOffset, r.byteLength), rc = v.getInt32(0, true), ln = v.getInt32(4, true);
      return { rc, log: new TextDecoder().decode(r.subarray(8, 8 + ln)).trimEnd(), dll: r.slice(8 + ln) };
    },
  };
}

return { files, start };
})();
if (typeof module !== 'undefined') module.exports = CODEPATCH;
