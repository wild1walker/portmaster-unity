# The setup's tools in Rust (setup-rs)

A port's first-launch setup runs `python -m pmunity <command>` on the handheld, so it needs
PortMaster's `python_3.11` runtime and the packages in the setup's `pylibs/` (UnityPy, Pillow, lz4,
texture2ddecoder, astc_encoder, guided_astc). `setup-rs/` replaces those commands with one static
binary, `pmu` (aarch64, musl), with the same arguments, outputs and exit statuses. The Python stays
until every command a setup runs is ported; a setup script then runs `pmu` instead of `pm`.

## The rule: byte-identical

For the same inputs and options, `pmu <command>` writes the same files, byte for byte, as
`python -m pmunity <command>` at the commit it was ported from. That is what every change is
tested against (below); a difference is a bug, unless it is in a log line. Where the Python output
comes from a native library (texture decoding, ASTC encoding, image scaling), port or vendor that
library's own code rather than writing a lookalike.

## The commands (what the setups run)

From Cuphead's setups (`pm/cuphead/cuphead/setup/patchscript`, `setup64/patchscript`,
`setup/dlcscript`, `setup/resume.txt`) and its launcher:

| command | Python | what it does |
|---|---|---|
| `macpkg <root> <pkg>...` | macpkg.py | unpacks macOS installers (xar; Payload or GOG's Scripts/payload; gzip, bzip2, pbzx/xz, cpio odc/newc); exit 3 when one is incomplete |
| `zip-extract <zip> <dir>` | (`python -m zipfile -e`) | the setup's own zips, unpacked by the launcher before the first setup |
| `player donor <unity> <dir> --apk <apk>... [--backend il2cpp --abi arm64-v8a --managed]` | player.py | Unity's Android player out of the donor APKs |
| `convert <data dir> --config <json> [--plugin <py>] [--steps a,b] [--set k=v]...` | job.py and the steps: shaders/, settings.py, textures.py (astc_guide.py), audio.py; the plugin animclips.py (portmaster-cuphead/convert) | in place, with the journal `.pmunity.json` (an interrupted run continues) |
| `android-data --src --pkg --out --name --package ... --pack all` | android_data.py (streaming.py, plugins.py, retarget.py, typetrees.py, video.py, unity_version.py) | the APK and OBB(s) the player reads; exit 4 (NO_SPACE) when the card is too full |
| `version <data>` | unity_version.py | the data's Unity version, "(supported)" or "(not supported)" |
| `unpack <data> [--out DIR] [--remove]` | unpack.py | a compressed build's data.unity3d unpacked into its files (unityfs::unpack) |
| `player fetch <version> <dir> [--backend --abi --installer FILE\|URL --symbols --symbols-only --managed]`, `player url <version>` | player.py, nsis.py | Unity's Android player from Unity's installer: its NSIS header and the files wanted read with HTTPS range requests (pkgzip's http.rs on rustls, the system's CA certificates: `$SSL_CERT_FILE`, `$SSL_CERT_DIR`, else the usual bundle files; `https_proxy`), or from a local copy; the installer's URL from Unity's release API |
| `winbcl <data> <dir> [--version --installer --player --no-facades --json]` | winbcl.py | Unity's class libraries a build lacks for the Android player (a Windows build's unityjit-linux twins, facades such as netstandard.dll), read from Unity's Windows editor installer the same way |
| `patch-obb new\|ggm\|pack\|check` | patch_obb.py | an add-on's bundles (the DLC added later) as a patch OBB; `check` exits 3 on an incomplete copy |
| `route <path> --env` | route.py | the box64 route's game.env: the first build under the path with a Linux x86-64 executable, its Unity version, backend and GL mode (the report, `--json` and `--launcher` stay with the Python: status 2) |

The setup's `codepatch` (.NET) and `CupheadPort.dll` stay as they are.

## The setups that run pmu

Cuphead's (portmaster-cuphead) and those `pmunity port new` writes for the android, mono-arm64 and
box64 routes: their patchscript runs `setup/pmu` when the release has it (`pm() { "$S/pmu" "$@"; }`;
the install step reads the report's scripting runtime with sed), else the setup's Python. `pmunity
port package` ships pmu (`setup-rs/tools/build_pmu.sh`: the binary and the licence texts of what is
compiled into it) when it runs every command of the port's setup (`port.py`'s `pmu_covers`: no
shaders from Metal, no converter plugins pmu has not built in, no `android-data --il2cpp`), else the
converter and the Python as before. The default convert config of `port new` (shaders, settings,
audio, quality; compute shaders on), a Windows build's (`from: ["d3d11"]`, the release's pmhlslcc
beside pmu), a Vulkan-only build's (`from: ["vulkan"]`, the release's pmspirvcross), `grab_fetch`
and `fused_post` run on pmu. Hollow Knight's whole setup (player fetch, winbcl, convert with
grab_fetch, android-data with the video shader and asmpatch) runs on pmu and gives the Python's
APK and OBB, byte for byte. pmu translates Metal programs too (below), but `pmu_covers` keeps a
Metal port on the Python until a macOS game's own shaders have been compared: so far only the
players' built-in shaders and hand-written MSL have been.

## What each setup runs on the handheld

The commands every setup template (`convert/pmunity/port_template/<route>/patchscript`) and
Cuphead's setups run on the handheld, and which run on pmu (2026-11-02). Besides these the setups
use the system's shell tools (sed, awk, find, sha256sum, df), PortMaster's 7-Zip for archives,
curl or wget for Unity's whole installer when range reads fail, and a port's own hooks
(`setup/hooks/<point>.sh`, which may call `pm` too).

| setup | command (Python module) | on pmu |
|---|---|---|
| android (macOS, Linux and Windows builds) | `macpkg` (macpkg.py), `unpack <data> --remove`, `version`, `player fetch` / `player url`, `winbcl` | yes |
| | `convert <data> --config convert.json [--plugin P] [--set k=v]`: shaders from OpenGL Core, Direct3D 11 (Windows builds, with `setup/pmhlslcc`) or Vulkan (with `setup/pmspirvcross`), compute kernels, grab_fetch, fused_post; settings, audio, quality, textures; the plugin animclips | yes |
| | the same with shaders from Metal (metal.py) | pmu translates them; the release ships the Python until a macOS game has been compared (above) |
| | the same with a porter's own plugin, or Vulkan programs of Unity 6000.6's typed layout | no: status 2, the release ships the Python |
| | `android-data ... --consume-src --verify-bundles 2 --pack all` (with asmpatch, StreamingAssets modes, `--managed-file`) | yes |
| mono-arm64 | the android route's commands, with `player fetch --managed` and `android-data --mono64` | yes (the template ran the Python before 2026-11-02; it now takes pmu when the release has it) |
| il2cpp-arm64 | `macpkg`, `unpack`, `version`, `convert` | yes (as the android route) |
| | `il2cpp <data> <out> --on-device --abi arm64-v8a ...` (il2cpp.py, il2cpp_device.py, il2cpp_bee.py, il2cpp_mix.py, monotree.py): Unity's IL2CPP and player by range reads, Debian's compilers and .NET from Microsoft (pinned by checksum), then UnityLinker, il2cpp and the C++ build | no |
| | `android-data --il2cpp <dir>` | no (status 2) |
| apk | `apk <inputs> <dir> --target T --json` (apk.py; with `player.il2cpp` also `--il2cpp --on-device`, the IL2CPP build above) | no |
| | `"$PY" -c '...'`: apk.json read into shell variables | no (the setup's own Python one-liner) |
| box64 | `route <gamedata> --env` (route.py: the Linux x86-64 build found, its Unity version, backend and GL mode) | yes (the template ran the Python before 2026-11-02) |
| | `convert <data> --config gles.json` (shaders, settings) | yes |
| Cuphead (portmaster-cuphead: `setup/patchscript`, `setup64/patchscript`, `setup/dlcscript`, `setup/resume.txt`, the launcher) | `zip-extract`, `macpkg`, `patch-obb check / new / ggm / pack`, `player donor` (mono armv7; il2cpp arm64 `--managed`), `convert --plugin animclips` (the DLC: `--steps shaders,textures,audio`), `journal-files`, `android-data` (64-bit: `--mono64`) | yes: the release ships no Python (its `codepatch` is .NET) |

So the android, mono-arm64 and box64 routes' releases ship pmu and no Python unless the port's
shaders are Metal ones or it brings converter plugins of its own; the il2cpp-arm64 and apk routes
still ship the Python for the commands marked no (what is left, and why: the end of this page).

## Layout

```
setup-rs/
  Cargo.toml          the workspace (members: pmu and each crate)
  pmu/                the binary: src/main.rs, src/cmd/<command>.rs (one module a command)
  crates/<name>/      libraries, one owner each while the port is under way
  vendor/<name>/      C/C++ taken from the libraries the Python uses, with their licence files
  tests/golden/       golden-test scripts (run Python and pmu on the same data, compare)
  tools/rs.sh         builds and tests in the arm64 Rust image
```

Crates while the port is under way (each is built and tested on its own branch, then merged):

| crate | owns |
|---|---|
| `pkgzip` | xar, cpio, pbzx; zip reading and extraction; the aligned STORE zip writer and its check (android_data `_write_zip`, `verify_zip`); `macpkg`, `zip-extract`, `player donor` |
| `unityfs` | UnityFS bundles (LZ4, LZ4HC, LZMA blocks), serialized files, type trees (the files' own, and the class database the Python takes from UnityPy/TPK), objects as typed value trees; reading and writing them as the Python does |
| `texcodec` | texture decoding, scaling, ASTC encoding with the guide; textures.py's work on one texture as a pure function |
| `shaderx` | shader blobs (shaders/blob.py) and the GLSL rewriting the convert step does (shaders/convert.py, glsl.py, step.py and what they call), as pure functions; Unity's binding info (dxbc.py's Bindings), Direct3D 11 (dxbc.py, with pmhlslcc), Vulkan (vulkan.py, spirv.py, with pmspirvcross), Metal (metal.py), grab_fetch and fused_post programs |
| `convert` | `pmu convert` (on unityfs, shaderx, texcodec): __main__.py's convert path, job.py (the in-place run and its journal), the steps shaders (step.py's files and summary; on the Shader objects: Direct3D 11, Vulkan and Metal sources, compute shaders, grab_fetch, fused_post), settings, audio, quality (with monotree.rs), textures (textures.py's Unity-file layer) and Cuphead's plugin step animclips |
| `androiddata` | `android-data` and `patch-obb pack` (android_data.py and what it uses on a setup's path: clrmeta.py, plugins.py, retarget.py, typetrees.py, streaming.py, unity_version.py), on `unityfs` and `pkgzip` |

Later (on top of `unityfs`): `android-data`, `patch-obb pack`.

### `pmu convert`

The same command line as `python -m pmunity convert` (argparse's rules), the same files, journal
(`.pmunity.json`, json.dump's text) and log lines (timings apart), the same exit statuses (0; 1
with sys.exit's message or the error's; 2 for bad arguments; 3 when the card is too full). Either
implementation continues the other's journal. Two differences, both outside the bytes compared:
the Python writes the journal's `audio.switch` in its set's hash order (it changes from run to
run; pmu writes the files in order), and where the Python prints a traceback pmu prints the
error's message. Anything not ported stops the run with status 2 and says so, after the steps
before it have journaled their files, so a setup can run the Python on the same folder to finish:
plugins other than animclips, Vulkan programs of 6000.6's typed layout. `grab_fetch`
(and `grab_fetch_names`) is ported: shaderx's `grabfetch` checks and rewrites a program as
grabfetch.py does (its symbolic evaluation with exact rationals), the convert crate edits the
Shader object (Hollow Knight's port sets `"always"`). `shaders.dead_varyings` (off by default) is
pmu's own, a rewrite the Python does not have (it ignores the key): vertex outputs no fragment
stage reads made plain globals ([shaders.md](shaders.md#vertex-outputs-no-fragment-stage-reads-dead_varyings-pmu-opt-in)).
The textures step is textures.py's
Unity-file layer in `convert` with texcodec as its per-texture codec (`steps::textures::Codec`;
the made-up tests also run it with a stand-in).

Compute shaders (`shaders.compute`, on by default) are ported: shaderx's `glsl::kernel_to_es`
(glsl.kernel_to_es: GLSL 4.20 / 4.30 compute kernels to GLSL ES 3.10, the format of a write-only
image taken from the Vulkan variant's SPIR-V) and the convert crate's `steps/compute.rs`
(compute.py's add_gles3 on the ComputeShader object: the GLES3 variant copied from the GLCore one,
its level, where Unity puts it). The `quality` step is ported (`steps/quality.rs`, quality.py: the
QualitySettings caps and the journal's `quality_caps`, lights, the Standard shaders' LOD-150
SubShader, URP's assets, SSAO and post components) with monotree.py as `monotree.rs` (MonoBehaviour
type trees from the game's assemblies, on androiddata's clrmeta) and typetrees.py's fixed trees
from androiddata. UnityPy reads a `vector<UInt8>` such as a pass's `m_Platforms` (2020.1+) as a
list where unityfs keeps bytes: the shaders step, grab_fetch and android-data's video shader take
both (before 2026-11-02 pmu stopped on 2020.3 - 2022.3 data with "m_Platforms: not a list of
integers", and its video shader kept the pass's old platforms there).

`fused_post` is ported: shaderx's `fusedpost` writes a chain's fused program (fusedpost.py's
Fragment, fuse_fragment, fuse_program) and the convert crate's `steps/fusedpost.rs` appends the
pass to the chain's last shader. Both it and Direct3D 11 need Unity's binding info: shaderx's
`bindings` reads and writes an entry's binding info (dxbc.py's Bindings: constant buffers,
members, textures, constant buffer bindings, inline samplers; 2021.2+ with the parameter entry)
and gives a GL program the binding info Unity's GL compiler would (dxbc.gl_bindings). Shaders from
Direct3D 11 (`from: ["d3d11"]`, Windows builds) are ported: shaderx's `dxbc` (the DXBC container,
its SHDR/SHEX declarations and constant buffer reads, the RDEF chunk rebuilt from the binding info,
pmhlslcc as a server process, the GLSL as Unity stores a GL program of the version) and the convert
crate's `steps/d3d11.rs` (convert.add_gles3_d3d11 and dxbc.with_common / check_parsed_form on the
Shader object). The setup gives pmu the release's `setup/pmhlslcc` through `$PMHLSLCC`, as it
gives the Python. Unity 6000.6's typed binding info (dxbc.py's `typed`) is not ported: the Vulkan
path is the only one that reads it.

Shaders from Vulkan (`from: ["vulkan"]`, Vulkan-only builds such as HDRP's Linux ones) are ported:
shaderx's `spirv` (SPIR-V modules as spirv.py reads and edits them: names, decorations, types,
the entry point) and `vulkan` (vulkan.py: the entry's stage modules decoded from SMOL-V and given
the names Unity's GL programs have, pmspirvcross as a server process, its GLSL made Unity's GL form: constant buffers flattened or kept as blocks, samplers and
their units, input and output locations, the uv origin of full-screen passes, the ES check), and
the convert crate's `steps/vulkan.rs` (add_gles3_vulkan: the programs stored by the data's Unity
version, the shared parameters put back into the Vulkan entries). The setup gives pmu
`setup/pmspirvcross` through `$PMSPIRVCROSS`. Vulkan programs of 6000.6's typed layout stop the
step with status 2.

Shaders from Metal (`from: ["metal"]`, macOS builds without OpenGL Core: Unity 2020.3 and later)
are ported: shaderx's `metal` reads the MSL Unity writes as metal.py does (tokens, the
`__METAL_VERSION__` blocks left out, expressions with their types, statements, the entry's
buffers, textures, stage_in structs and function constants) and writes HLSLcc's GL form of the
program; the convert crate's `steps/metal.rs` pairs the vertex and fragment subprograms as Direct3D
11's are paired and stores the programs with `steps/d3d11.rs`'s code (shared: `store`,
`restore_common` by platform). Unity 2017.4's Metal entries do not parse in either implementation
(both stop with status 1: "shader program entry ... does not match its layout", Cuphead's and the
2017.4 players' built-in resources). `cargo test -p shaderx --test metal` compares
the translator with the Python on 17 hand-written cases (`tests/metal_golden.json`, written by
`tests/golden/metal_golden.py` from convert/tests/test_metal.py's MSL and variants of it).

Player builds keep no type trees in their files: pmu reads them with UnityPy's class database
(lzma.tpk), embedded when the binary is built with `PMU_TPK=<lzma.tpk>`, else from `$PMU_TPK` at
run time (without either, the first such file stops the run, status 1, saying so).

`pmu journal-files <data>/.pmunity.json` prints the files the journal's steps have finished (0
when there is none), as resume.txt's `journal_files` does with Python, for the setup's loop that
reruns a conversion the kernel killed.

Plugins: pmu has the plugin steps built in, by the file's name: `--plugin <path>/animclips.py` (the
path need not exist) or `--plugin animclips` is portmaster-cuphead's convert/animclips.py.

Tests: `cargo test -p convert` (with `PMU_TPK` set to UnityPy's lzma.tpk) runs the job on made-up
data (crates/convert/tests/madeup: Unity 2017.4 files written from the class database's type
trees, bundles laid out as Unity writes them). `tests/golden/convert_madeup.sh <dir>` runs the
Python and pmu on copies of that data, case by case (Cuphead's arguments, reruns, a run continued
by the other implementation, the textures step unscaled (strips), in search mode, with an ASTC guide
the Python makes for the data (astc_guide.py: numpy, fetched with curl when the image has none),
options, errors), and compares statuses, output and trees (`PMU=<the musl build>` tests the binary
a setup ships);
`tests/golden/convert.sh <dir> [rs|rs-py|py-rs|both]` does it on Cuphead's data from
`$PMU_TEST_DATA` with the setup's arguments, against `converted-fast`.

## Building and testing

The image `ua-rust` (arm64 Debian bookworm) has Rust 1.88 with the `aarch64-unknown-linux-musl`
target, and the reference Python: PortMaster's `python_3.11` at `/opt/py` with the setup's
packages at `/opt/pylibs`. On Apple silicon it runs natively. Builds, the crate cache, the test data and the scratch folder
live in Docker volumes (bind mounts of the host's disks are slow; a build's output is copied out
with `docker cp` or a `cat` from the container when needed).

```bash
RS_ROOT=<the shared setup's folder> setup-rs/tools/rs.sh <worktree> cargo test
RS_ROOT=... setup-rs/tools/rs.sh <worktree> cargo build --release --target aarch64-unknown-linux-musl
```

The musl build links with the image's gcc and musl-tools (`aarch64-linux-musl-gcc` for the C
parts the crates build): no cross linker is needed on an arm64 host.

The reference Python, in the same image:

```bash
PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH=<worktree>/convert:/opt/pylibs /opt/py/bin/python3 -m pmunity ...
```

Golden tests read real game data from `$PMU_TEST_DATA` (never committed, never named in the
repository by its path): `Cuphead.app` (GOG, with the DLC), `pkg/` (GOG's installers),
`converted-fast/` (the Python's `convert` output for that game, fast quality), `donor/` (the donor
APKs), `guides/` (ASTC guides), `cuphead/` (`cuphead.json`, `animclips.py`, `dlc_bundles.txt`). They
are mounted read-only: a test that changes files works on its own copy in a scratch folder. Unit
tests use small made-up data built in the test (no Unity or game files in the repository).

`pmu android-data` makes the video shader as video.py does (Hidden/VideoDecodeAndroid from the
data's Hidden/VideoDecode, the game's clips sniffed for BT.601 or BT.709; the GLSL is
`crates/androiddata/src/video_decode_android.shader`, a copy of the Python's); `--il2cpp` and a
bundle with several serialized files and streamed textures (`--inline-textures`) stop it with
status 2. A serialized file too big for one archive with its textures inlined keeps its `.resS`,
split into parts that fit one (`crates/androiddata/src/ress_split.rs`; the Python stops at
packing there): [android-data.md](android-data.md), section 4. `cargo test -p androiddata` (with
`PMU_TPK`) runs that on made-up 2021.3 data (`tests/ress_split.rs`): the parts, the rewritten
streams, the archives within a lowered limit, every texture read back from them. The test-only
knobs `PMU_ZIP_LIMIT` and `PMU_RESS_SPLIT` (same page) make it happen on small data.

`androiddata`'s golden test, `tests/golden/android_data.sh <scratch> [mini32 mini64 obb video space 32 64]`,
runs the Python and pmu with the setups' exact invocations on copies of `converted-fast` (the whole
game, or a part of it for the mini cases) and compares status, the output folder, every archive's
entries and the data `--consume-src` leaves.

`shaderx`'s golden data comes from the reference Python: `tests/golden/shaderx_golden.py <data>
<config.json> <out> [--converted <converted data>]` runs the shaders step's own code on every Shader
and saves inputs, outputs and every program translation to `<out>`; then
`rs.sh <worktree> env SHADERX_GOLDEN=<out> cargo test --release -p shaderx` compares them.

texcodec's golden tests take their data from the Python, run in the same image:
`tests/golden/textures_golden.py dump <data> <config.json> <dir> [--set textures.k=v] [--range a:b]`
runs the textures step's own per-texture code and writes each texture's fields, raw pixels and
result to `<dir>`; `TEXCODEC_GOLDEN=<dir> cargo test -p texcodec --release --test golden` checks
texcodec against it (same decisions, same bytes). `tests/golden/texcodec_fuzz.py <dir>` and
`TEXCODEC_FUZZ=<dir> ... --test libs` do the same for the libraries on made-up images (every
format, resize boxes, every ASTC preset and tier, other block sizes). Run them for the musl target
too (`--target aarch64-unknown-linux-musl`): the shipped binary has musl's libm.

`unityfs`'s golden test runs the crate (its example `golden`) and the Python (pmunity with UnityPy)
over every Unity file of a data folder and compares what each reads and writes: every object's
value tree and its bytes written back, the file saved unchanged, after an edit, and (a bundle) with
its textures inlined, every block decompressed and compressed again, the platform patched in place:

```bash
RS_ROOT=... setup-rs/tools/rs.sh <worktree> sh tests/golden/unityfs.sh "$PMU_TEST_DATA/Cuphead.app/Contents/Resources/Data" <scratch>/out
```

A build folder in a Docker volume (`RS_MOUNTS="$RS_MOUNTS <volume>:/rt"`, `CARGO_TARGET_DIR=/rt`)
builds far faster than one on a shared host folder.

## Compared with the Python on games: `regress.py pmu`

`tools/regress/regress.py pmu` ([its README](../tools/regress/README.md), "pmu") runs each game of
the regression list through the android route's commands twice, by the Python and by pmu, in the
`ua-rust` image, on two copies of the data at the same path, and compares exit statuses, every file
(size and SHA-256), the archives' entries and the journal. `--builtin` does the same on the desktop
player's built-in resources (`unity default resources`: Unity's own shaders and compute shaders for
every platform), `--set`, `--steps` and `--convert-only` pick what convert does, and
`tools/regress/pmu_pair.py` runs one data folder or a subset of its files. Differences that are not
failures: the journal's `audio.switch` order, a traceback against pmu's message, android-data's
state file's source mtimes and log lines (timings taken out; `make_android_data.log` still differs
beyond its timings, as a note).

Results on 2026-11-02 (branch rust-cover). Most of the list's corpus games are no longer on this
machine; these are what it had: six corpus games with Linux builds (unsung-warriors 2017.4,
loco-motive 2018.4, buggish 2019.4, tom-city 2020.3, hook-game 2021.3, tiny-kingdom 2022.3),
Cuphead (macOS, 2017.4), Hollow Knight (Linux, 6000.0) and Unity 2017.4.9's Windows player. Every
run below gave the same files on both sides.

| what | data | result |
|---|---|---|
| a port's whole setup: convert with `port new`'s config and the check's suggestions, then `android-data --consume-src --verify-bundles 2 --pack all` with the player pmu fetched | the six corpus games | the same APK and OBBs for five; unsung-warriors: the Python stopped in android-data (a realtime GI file read twice, its NEON twin, with `--consume-src`) where pmu, fixed for it, finishes; without `--consume-src` the same |
| the quality step (`quality.profile=handheld`) | the six | the same (tiny-kingdom: URP's assets edited through monotree.rs; the others: QualitySettings caps) |
| compute shaders and every GLES3 program rebuilt from OpenGL Core (`replace`) | the built-in resources of the six, Cuphead and Hollow Knight | the same (5 - 6 shaders and 1 - 7 compute shaders each; Hollow Knight: 3 of 7 compute shaders, 50 kernels) |
| Direct3D 11 (`from: ["d3d11"]`) | the built-in resources of the six, Hollow Knight and the Windows player | the same (5 - 6 shaders, 15 - 19 programs each) |
| Vulkan (`from: ["vulkan"]`) | the built-in resources of the six and Hollow Knight; the six games' own shaders; Hollow Knight's own (resources.assets, sharedassets0, the built-in extra) | the same (the games: 36 - 87 shaders, 156 - 2556 programs each; Hollow Knight 160 shaders, 759 programs) |
| `fused_post` (with `grab_fetch: "always"` and the quality step) | Hollow Knight's resources.assets | the same: a three-shader chain fused (rgba8), a two-shader chain, chains left as they are |
| Metal (`from: ["metal"]`) | the built-in resources of the five games of 2018.4 - 2022.3 and Hollow Knight | the same (19 - 23 programs each, every layout of the GL programs: 2018, 2019, 2020.1+, 2021.2+ with parameter entries); unsung-warriors' and Cuphead's (2017.4): both stop the same way |
| `route --env` | 16 paths (each Linux build, its data folder and executable, the folders above up to the whole corpus, Hollow Knight, Cuphead's app, missing folders) and 16 made-up builds | the same output and exit status in all 64 runs (with `--env` and `--env --json`) |

## Porter plugins without Python

A port's converter plugin is Python (a file with `STEPS = {name: function(job)}`, run in the
converter's process with the job's API), so pmu cannot run it. pmu has plugins built in instead,
chosen by the file's name (`--plugin <dir>/animclips.py` or `--plugin animclips`; `plugin()` in
`crates/convert/src/steps/mod.rs`, `PMU_PLUGINS` in port.py): Cuphead's animclips. The way for a
porter's plugin to run without Python is the same: the plugin's step ported into
`crates/convert/src/steps/<name>.rs` (as animclips.rs was), checked against the plugin on the game's
data (`pmu_pair.py --plugin <file>` runs both converts with it), and its name added to `plugin()` and
`PMU_PLUGINS`; the port's release then ships pmu and no Python. Most plugins edit fields of objects
the steps already read, so a declarative step (a list of edits in the convert config: file, class,
object name, field path, value) that both implementations run would cover them without code; it is
not written. Until a plugin is built in, `pmu_covers` keeps its port on the Python. Built-ins are
matched by the file's name only: a porter's own plugin called `animclips.py` would run Cuphead's
step, so a new plugin needs a name of its own (comparing the file's hash with the ported one's in
`pmu_covers` would catch it).

## What is left, and why

- `il2cpp --on-device` (the il2cpp-arm64 route, and the apk route with `player.il2cpp`): il2cpp.py,
  il2cpp_device.py, il2cpp_bee.py and il2cpp_mix.py (about 3800 lines) fetch Unity's IL2CPP and
  player by range reads, Debian's compilers and .NET (pinned by checksum), then run UnityLinker,
  il2cpp and the C++ build. It is orchestration of other programs more than conversion, but large,
  and its outputs (a built libil2cpp.so) need their own comparison; not started.
- `android-data --il2cpp` (il2cpp-arm64): small (the IL2CPP output's files instead of Managed/, the
  metadata's app guid, the version checks), but its route needs the Python for `il2cpp` anyway;
  status 2.
- The apk route: `apk` (apk.py, about 1100 lines: APK, split APKs, XAPK and APKS zips, an AAB
  rebuilt into one APK, the OBBs' names, android_package.json) and the setup's `"$PY" -c` line that
  reads apk.json into shell variables (a `pmu` subcommand or sed would do); not started.
- Metal releases: pmu translates them as the Python does on everything compared so far, but no
  macOS game's own shaders were on this machine; one corpus run (`regress.py pmu --set
  'shaders.from=["metal"]' --convert-only` on macOS builds of 2020.3+) and `metal` in port.py's
  `PMU_SHADER_SOURCES` would let Metal ports ship pmu alone.
- Vulkan programs of Unity 6000.6's typed layout (dxbc.py's `typed` binding info): status 2.
- Porter plugins other than animclips (above).
- The comparison itself: the corpus games of the regression list that are gone from this machine
  (Windows builds through Direct3D 11, macOS and Unity 6 games, compressed builds, Vulkan-only
  ones) are where a divergence would show next.

## Rules

- Byte-identical outputs (above). Exit statuses and the files a command leaves are part of it.
- No game files, no Unity files, and nothing derived from them in the repository.
- No local paths, user names or host names in the repository: test data comes from `$PMU_TEST_DATA`.
- Vendored code keeps its licence (`vendor/<name>/LICENSE*`), listed in THIRD_PARTY.md.
- Memory: the handhelds have 1 GB. Stream big files; never hold a whole bundle or OBB in memory
  when the Python does not.
- Dependencies: few and plain (crates with no build-time network or system libraries).
