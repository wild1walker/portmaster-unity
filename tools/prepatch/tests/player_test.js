// node tests/player_test.js <donor apk> <version> <out dir>: src/player.js's donor() into <out dir>, to
// compare with pmunity's `player donor` (tests/refplayer.sh) by md5
const fs = require('fs'), path = require('path');
const PLAYER = require('../src/player.js');
const [apk, version, out] = process.argv.slice(2);
(async () => {
  const blob = new Blob([fs.readFileSync(apk)]);
  const info = await PLAYER.donor({ version, apks: [{ name: path.basename(apk), blob }],
    put: async (rel, b) => { const p = path.join(out, ...rel.split('/')); fs.mkdirSync(path.dirname(p), { recursive: true }); fs.writeFileSync(p, b); } });
  console.log(JSON.stringify(info));
})().catch(e => { console.error(e); process.exit(1); });
