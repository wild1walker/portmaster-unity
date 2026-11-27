# Parity tests (prepatch vs pmu)

Scripts that check the page's code against the setup's own tool, `pmu`, on the same input, byte for byte.
They need real game files, so nothing here runs on its own: each script takes your own copies as
arguments. The reference scripts (`ref*.sh`) run in Linux on aarch64 and need `PMU`, a pmu binary
(`setup-rs/tools/build_pmu.sh <dir>`), and write to `REF` (default: a `ref-*` folder in the current
directory); `refcup_android.sh` also runs the port's aarch64 codepatch (under qemu elsewhere:
`QEMU_LD_PREFIX`, an arm64 root file system). Build the page's parts first (`../README.md`, Building).
Until 2026-11-27 the references came from the Python converter, with Python-side comparers (ref_side.py,
unity_ref.py, cmp_full.py, verify_zip.py, refcup_full.sh: `git show 2147a04:tools/prepatch/tests/<name>`).

| script | checks |
|---|---|
| `tex_test.js` | textures.js output vs pmu's textures step on a data folder (scale 0.5), file by file |
| `cmp_bundle.js` | bundles: nodes and objects compared by content |
| `refplayer.sh` + `player_test.js` | `src/player.js` vs `pmu player donor` on the donor APK (md5 per file) |
| `refcup_steps.sh` + `steps_test.js` | `src/steps.js` (settings, audio, the Cuphead port's animclips plugin) vs pmu's on the same data |
| `shaders_test.js` | `src/shaders.js` vs pmu's shaders step (`refcup_steps.sh <zip> shaders`), file by file |
| `refcup_android.sh` + `android_test.mjs` | `src/android.js` (packing: tree, APK, OBB) vs pmu's android-data on the same input; `bundle_platform_test.js`, `sf_android_test.js` check its parts |
| `e2e.js` | the whole page pipeline in Node (stage, convert, port layout, zip); `FULL=1` adds the shaders, codepatch, packing and install (the ready-to-run zip) |

The Node tests use the page's own pool (`src/pool.js`); `SPLIT=0` turns off sharing a big texture's chunks.
