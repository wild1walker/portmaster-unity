// node run_test.mjs <Managed dir> <out dll> [flags]: codepatch (_framework, WebAssembly) in Node on a
// Managed folder, as the page will run it; prints codepatch's log and writes the patched Assembly-CSharp.dll
import fs from 'fs';
import path from 'path';
import { pathToFileURL, fileURLToPath } from 'url';
const [dir, out, flags = '--target=android --steam-dlc'] = process.argv.slice(2);
const here = path.dirname(fileURLToPath(import.meta.url));
const { dotnet } = await import(pathToFileURL(path.join(here, '_framework/dotnet.js')).href);
const t0 = Date.now();
const rt = await dotnet.create();
const ex = await rt.getAssemblyExports(rt.getConfig().mainAssemblyName);
const t1 = Date.now();
const names = fs.readdirSync(dir).filter(n => n.endsWith('.dll'));
const bufs = names.map(n => fs.readFileSync(path.join(dir, n)));
const r = ex.Entry.Run(names, new Uint8Array(Buffer.concat(bufs)), bufs.map(b => b.length), flags);
const v = new DataView(r.buffer, r.byteOffset, r.byteLength), rc = v.getInt32(0, true), ln = v.getInt32(4, true);
console.log(new TextDecoder().decode(r.subarray(8, 8 + ln)).trimEnd());
fs.writeFileSync(out, r.subarray(8 + ln));
console.log(`exit ${rc}; runtime start ${t1 - t0} ms, patch ${Date.now() - t1} ms; ${r.length - 8 - ln} bytes -> ${out}`);
process.exit(rc);
