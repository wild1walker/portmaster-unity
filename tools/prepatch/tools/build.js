// node tools/build.js: prepatch.html, one file, from src/ (page.html + the scripts) with wasm/pmucore.wasm,
// src/typetrees.json and codepatch/ embedded (base64; the type trees and codepatch gzipped). Build the wasm first
// (wasm/build.sh); codepatch/_framework is the Cuphead port's codepatch for WebAssembly (codepatch/build.sh <tag>).
const fs = require('fs'), path = require('path'), zlib = require('zlib');
const ROOT = path.join(__dirname, '..'), S = path.join(ROOT, 'src'), CP = path.join(ROOT, 'codepatch');
const rd = n => fs.readFileSync(path.join(S, n), 'utf8');
const wasm = fs.readFileSync(path.join(ROOT, 'wasm', 'pmucore.wasm')).toString('base64');
const tt = zlib.gzipSync(fs.readFileSync(path.join(S, 'typetrees.json')), { level: 9 }).toString('base64');
// codepatch: [u16 name length][name][u32 size][bytes] for each file (src/codepatch.js), gzipped
const cpFiles = [...fs.readdirSync(path.join(CP, '_framework')).map(n => `_framework/${n}`),
  ...fs.readdirSync(CP).filter(n => /\.(txt|json)$/.test(n))];
const cpRaw = Buffer.concat(cpFiles.flatMap(n => { const name = Buffer.from(n), b = fs.readFileSync(path.join(CP, n)), h = Buffer.alloc(6 + name.length);
  h.writeUInt16LE(name.length, 0); name.copy(h, 2); h.writeUInt32LE(b.length, 2 + name.length); return [h, b]; }));
const cpGz = zlib.gzipSync(cpRaw, { level: 9 });
const known = JSON.parse(fs.readFileSync(path.join(CP, 'known.json'), 'utf8'));
const script = (n, src) => { if (src.includes('</script')) throw new Error(`${n} holds </script>`); return `<script>\n// ---- ${n}\n${src}</script>`; };
const data = (id, text) => `<script type="text/plain" id="${id}">${text}</script>`;
// fengari (Lua in JavaScript, for the ports' patcher questions) with its license, as shipped
const fengari = `/* fengari-web 0.1.4: ${rd('vendor/LICENSE.fengari.txt').trim()} */
${rd('vendor/fengari-web.js')}`;
const cpNote = `<!-- codepatch-gz: the codepatch of ${known.repo} (release ${known.tag}, MIT, (c) 2026 wild1walker) built for
WebAssembly with Mono.Cecil (MIT, (c) 2008-2015 Jb Evain, (c) 2008-2011 Novell, Inc.) and the .NET runtime (MIT, (c) .NET
Foundation and Contributors). Their full license texts and the runtime's third-party notices are in the bundle
(LICENSE.*.txt, THIRD-PARTY-NOTICES.dotnet.txt). -->`;
const scripts = [
  data('pmucore-wasm', wasm),
  data('typetrees-gz', tt),
  cpNote,
  data('codepatch-gz', cpGz.toString('base64')),
  `<script type="application/json" id="codepatch-known">${JSON.stringify(known)}</script>`,
  data('worker-src', rd('pixels.js').replace('</script', '<\\/script')),
  ...['core.js', 'pixels.js', 'unity.js', 'bundle.js', 'textures.js', 'steps.js', 'shaders.js', 'clrmeta.js', 'plugins.js', 'android.js',
    'pack.js', 'codepatch.js', 'pool.js', 'stage.js', 'player.js'].map(n => script(n, rd(n))),
  script('vendor/fengari-web.js', fengari),
  ...['questions.js', 'ports.js', 'app.js'].map(n => script(n, rd(n))),
].join('\n');
const html = rd('page.html').replace('<!--SCRIPTS-->', () => scripts);
fs.writeFileSync(path.join(ROOT, 'prepatch.html'), html);
console.log(`prepatch.html: ${(html.length / 1024).toFixed(0)} KB (pmucore.wasm ${(wasm.length * 3 / 4 / 1024).toFixed(0)} KB, ` +
  `codepatch ${(cpGz.length / 1024).toFixed(0)} KB gzipped, ${cpFiles.length} files; gzipped page ${(zlib.gzipSync(html, { level: 9 }).length / 1024).toFixed(0)} KB)`);
