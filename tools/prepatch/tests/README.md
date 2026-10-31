# Parity tests (prepatch vs pmunity)

Scripts that check the page's code against `pmunity` (Python) on the same input, byte for byte. They need
real game files, so nothing here runs on its own: each script takes your own copies as arguments. The
reference scripts (`ref*.sh`) run in Linux and need `PMU_PY`, a Python with the port setup's package
versions (`setup/pylibs`, for the PC), and write to `REF` (default: a `ref-*` folder in the current
directory); `refcup_full.sh` and `refcup_android.sh` also run the port's aarch64 codepatch under qemu
(`QEMU_LD_PREFIX`: an arm64 root file system). Build the page's parts first (`../README.md`, Building).

| script | checks |
|---|---|
| `wasm_side.js` + `ref_side.py` | pmucore.wasm vs Pillow / guided_astc / astc_encoder_py on generated inputs |
| `unity_rt.js` + `unity_ref.py` | unity.js reads/writes every object of a data folder back to the same bytes; fields vs UnityPy |
| `tex_test.js` | textures.js output vs pmunity's textures step on a data folder (scale 0.5), file by file |
| `cmp_bundle.js` | bundles: nodes and objects compared by content |
| `refplayer.sh` + `player_test.js` | `src/player.js` vs `pmunity player donor` on the donor APK (md5 per file) |
| `refcup_steps.sh` + `steps_test.js` | `src/steps.js` (settings, audio, the Cuphead port's animclips plugin) vs pmunity's on the same data |
| `shaders_test.js` | `src/shaders.js` vs pmunity's shaders step (`refcup_steps.sh <zip> shaders`), file by file |
| `refcup_android.sh` + `android_test.mjs` | `src/android.js` (packing: tree, APK, OBB) vs pmunity's android-data on the same input; `bundle_platform_test.js`, `sf_android_test.js` check its parts |
| `e2e.js` | the whole page pipeline in Node (stage, convert, port layout, zip); `FULL=1` adds the shaders, codepatch, packing and install (the ready-to-run zip) |
| `refcup_full.sh` + `cmp_full.py` | the Cuphead release's whole patchscript (steps 2-5) on a page-built zip, then a ready-to-run zip's `cuphead/` against that tree, file by file (the APK and OBB entry by entry where they differ) |
| `verify_zip.py` | every serialized file and bundle of a built zip loads in UnityPy, as the handheld's packing checks |

The Node tests use the page's own pool (`src/pool.js`); `SPLIT=0` turns off sharing a big texture's chunks.
