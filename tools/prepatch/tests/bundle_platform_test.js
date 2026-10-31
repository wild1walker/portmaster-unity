// node bundle_platform_test.js <input bundles dir> <pmunity's output bundles dir>: android.js's
// patchBundlePlatform on every bundle, compared with android-data's (tests/refcup_android.sh) byte for byte
const fs = require('fs'), path = require('path');
const S = path.join(__dirname, '..');
const PIXELS = require(`${S}/src/pixels.js`), A = require(`${S}/src/android.js`);
const [inDir, refDir] = process.argv.slice(2);
(async () => {
  const px = new PIXELS.Pixels(await PIXELS.instantiate(await WebAssembly.compile(fs.readFileSync(`${S}/wasm/pmucore.wasm`))), null);
  const lz = { unlz4: (s, n) => px.unlz4(s, n), lz4: (s, l) => px.lz4(s, l) };
  const methods = {}, bad = [];
  const t0 = Date.now();
  for (const n of fs.readdirSync(inDir).sort()) {
    const r = A.patchBundlePlatform(new Uint8Array(fs.readFileSync(path.join(inDir, n))), lz);
    for (const [, , m] of r.done) methods[m] = (methods[m] || 0) + 1;
    const ref = fs.readFileSync(path.join(refDir, n));
    if (!Buffer.from(r.buf.buffer, r.buf.byteOffset, r.buf.length).equals(ref)) bad.push(n);
  }
  console.log(`methods ${JSON.stringify(methods)}; different from pmunity: ${bad.length}`, bad.slice(0, 10), `${((Date.now() - t0) / 1000).toFixed(1)} s`);
})().catch(e => { console.error(e); process.exit(1); });
