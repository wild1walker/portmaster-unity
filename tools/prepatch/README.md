# prepatch: run a port's setup in the browser

One HTML file, `prepatch.html`, for ports made with portmaster-unity. Open it in Chrome or Edge, give it the
port's release zip and your copy of the game, and it writes a new zip with the game unpacked and the
setup's work already done, with the same rules the handheld's converter uses, on every core of the PC.
Nothing is uploaded: everything runs in the page. The zip holds your copy of the game: keep it for your own
devices.

How much it does depends on the port:

- **Every portmaster-unity port:** the game is unpacked where the setup looks for it (GOG's macOS `.pkg`,
  zips, GOG's Linux `.sh`, folders), its textures are converted (the longest part of the setup on a
  handheld), and the settings, audio and known port plugins steps are run. On the first launch the
  handheld does the rest, and its converter skips the work the journal (`<data>/.pmunity.json`) lists.
- **Ready to run (the Cuphead port, [portmaster-cuphead](https://github.com/wild1walker/portmaster-cuphead)
  2026.10.29):** the page does the whole setup. That covers Unity's player from the donor APK, the shaders,
  the port's code patch, the packing into APK + OBB and the patchscript's install step. The handheld starts
  the game on the first launch, without Python and without a network.

## Use

1. Open `prepatch.html` from a web server (`python3 -m http.server`, then
   `http://127.0.0.1:8000/prepatch.html`). Chrome and Edge write the zip straight to disk; other browsers
   build it in memory.
2. Drop the port's release zip, then the game's installers, archives or folder.
3. Answer the questions shown. Where the port's launcher names a questions file (`PATCHER_QUESTIONS`, e.g.
   `setup/questions.lua`), the page runs that file as PortMaster's patcher would, for the handheld you pick
   (screen, memory): the same questions and options it would get, from the port itself (`src/questions.js`,
   Lua run in fengari). Answers go where the port's patchscript puts them (its
   `echo "$ID" > "$GAMEDIR/<file>"` lines).
4. **Build zip…**, choose where to save it. Install it on the handheld like any port.

When the browser has less room than the work needs (about 2.5 times the size of the game files given), the
page asks for a work folder on a disk.

## How it matches the handheld

Each step is the converter's own, ported to JavaScript and WebAssembly, and checked byte for byte against
`pmunity` on the same input (`tests/`):

| part | here | from |
|---|---|---|
| decoding (DXT1/5, BC4/5/7, raw formats, crunch) | `pmucore.wasm` | Pillow 12.3.0 `BcnDecode.c` (as UnityPy uses it), texture2ddecoder 1.0.6 crunch |
| scaling | `pmucore.wasm` | Pillow 12.3.0 `Resample.c` (Lanczos on premultiplied alpha): its coefficients and plan, the two 8-bit passes in WebAssembly SIMD (the same integer sums) |
| ASTC | `pmucore.wasm` | `convert/native/guided_astc` + ARM astc-encoder (astc_encoder_py 0.1.12; its own SSE 4.1 vector layer on WebAssembly SIMD through `wasm/shim/sse/immintrin.h`, invariance mode on), the port's ASTC guide |
| Unity files | `src/unity.js`, `src/bundle.js` | UnityPy 1.25.4's rules; type trees exported from UnityPy's database |
| bundles | `src/bundle.js` | UnityPy `save(packer='lz4')`: 128 KiB LZ4HC-9 chunks (lz4 4.4.5), `inline_streams` as pmunity |
| textures | `src/textures.js` | `choose`, the new `.resS`, sprites/atlases, guide keys, strip rule, journal |
| settings, audio, plugins | `src/steps.js` | the converter's steps; port plugins only where the page knows the plugin file (by its CRC) |
| shaders | `src/shaders.js` | the shaders step (glcore path) |
| Unity's player | `src/player.js` | `pmunity player donor` |
| packing | `src/android.js`, `clrmeta.js`, `plugins.js`, `pack.js` | `pmunity android-data`, with its options read from the patchscript's `pm android-data` line |
| code patch | `src/codepatch.js`, `codepatch/` | the port's codepatch (C#, Mono.Cecil) built for WebAssembly; only for the release whose `setup/codepatch` CRC is in `codepatch/known.json` |

One known difference: inside a bundle, the serialized file's metadata keeps Unity's own bytes where UnityPy
writes its own equivalent re-serialization. Both load the same way.

Left to the handheld: LZMA-compressed or encrypted bundles, a `pm android-data` option or code patch the page
does not know, ports whose setup the page does not know (it then only unpacks the game), IL2CPP.

## Building

```sh
wasm/build.sh                                         # pmucore.wasm (wasi-sdk at ~/wasi-sdk; PMUNITY=<this checkout>)
python3 tools/export_typetrees.py src/typetrees.json  # with UnityPy 1.25.4 installed
codepatch/build.sh 2026.10.29                         # codepatch/_framework (.NET 8 SDK + wasm-tools workload)
node tools/build.js                                   # -> prepatch.html (one file, everything embedded)
```

None of the built files are committed (`.gitignore`).

Speed: up to 8 files are converted at once (within 768 MB held in memory), every core runs a worker
(`src/pool.js`), a big texture's ASTC work is cut into chunks of 32 × 256 blocks that idle workers take over
(astc-encoder encodes every block on its own, so the bytes do not depend on where a chunk runs), bundles'
LZ4 blocks are decompressed by lz4's own decoder, BC7 by a faster copy of Pillow's decoder (`pm_decode_ref`
keeps Pillow's; both give the same pixels on millions of random blocks).

Results on a 12-core PC: a 2018.4 game's zip in 27 s in the browser, after which its setup on an RG Cube XX
(Knulli) takes 2 min 28 s instead of about 7 min. The Cuphead port with its add-on: textures in 7.4 min, the
whole ready-to-run zip (2.4 GB) in about 9 min (the Node run of the same code, `tests/e2e.js`). Its port folder is file for file the release setup's own
output, and it starts on a handheld with no setup.

## Third-party code

The built page carries:

- fengari-web 0.1.4 (MIT, `src/vendor/`, license embedded with it);
- in `pmucore.wasm`: Pillow (MIT-CMU), ARM astc-encoder (Apache-2.0), texture2ddecoder (MIT) with crunch
  (zlib), lz4 (BSD-2), compiled from their release sources (`wasm/build.sh` fetches them);
- the Cuphead port's codepatch (MIT, wild1walker), Mono.Cecil (MIT) and the .NET 8 runtime (MIT), with their
  license texts and the runtime's third-party notices in `codepatch/` (all embedded in the page).
