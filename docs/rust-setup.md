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
| `apk <inputs> <dir> --target T --json` | apk.py | the apk route: a game's own Android build (an APK with the OBBs beside it, split APKs, XAPK and APKS zips, an App Bundle rebuilt into one APK) laid out for ua_host: the ABI's libraries, the APK (split asset packs merged into it), the OBBs under the player's names, android_package.json; the refusals with the Python's messages (`pmunity apk: refused: ...`). `--il2cpp --on-device`: a Mono build (armeabi-v7a only) compiled for the arm64 IL2CPP player on the handheld (apk.py's il2cpp_stub, il2cpp_build on crates/il2cpp, il2cpp_apk: the APK with IL2CPP's metadata); `--il2cpp` without `--on-device` (Docker, a PC): status 2 |
| `wwise <banks> <out> [--check] [--wwise-lib PATH]` | (none: Rust only, 2026-11-02) | a Wwise game's soundbanks (bank version 128) into the data of ua_host's Wwise player ([wwise-player.md](wwise-player.md)): per bank `<Bank>.uaww.json` and `<Bank>.uaww.pak`, the media read and written one at a time (DATA is never held in memory); status 3 for another bank version |
| `il2cpp <data> <out> --on-device ...`, `il2cpp fetch <version> [--on-device]` | il2cpp.py, il2cpp_device.py, il2cpp_bee.py, il2cpp_mix.py | the il2cpp-arm64 route's build on the handheld: Unity's IL2CPP and player by range reads, Debian's compilers and Microsoft's .NET (pinned by sha256), UnityLinker, il2cpp, the C++ build (Bee's graph from 2021.2), libil2cpp.so and Data/Managed (below, "`pmu il2cpp`"); the Docker route of a porter's PC (no `--on-device`) stays with the Python: status 2 |
| `video <game dir> [--config <convert.json>] [--set KEY=VALUE]...` | (pmu's own, no Python) | convert's `video` step on a game set up before it existed (the android template's `setup/update` runs it), in place: the resource files of its APK and OBBs that hold WebM clips, with their asset files, taken out, the step run on them, and each changed file written back over its entry (stored, the same length: nothing else moves; every file checked before any is written, a stopped run writes them again); below, "`video`" |

The setup's `codepatch` (.NET) and `CupheadPort.dll` stay as they are.

## The setups that run pmu

Cuphead's (portmaster-cuphead) and those `pmunity port new` writes for the android, mono-arm64,
il2cpp-arm64, box64 and apk routes: their patchscript runs `setup/pmu` when
the release has it (`pm() { "$S/pmu" "$@"; }`; the install step reads the report's scripting
runtime with sed, the apk route's setup apk.json's fields with sed), else the setup's Python.
`pmunity port package` ships pmu (`setup-rs/tools/build_pmu.sh`: the binary and the licence texts
of what is compiled into it) when it runs every command of the port's setup (`port.py`'s
`pmu_covers`: no Python converter plugin of the porter's), else the
converter and the Python as before. The default convert config of `port new` (shaders, settings,
audio, quality; compute shaders on), a Windows build's (`from: ["d3d11"]`, the release's pmhlslcc
beside pmu), a Vulkan-only build's (`from: ["vulkan"]`, the release's pmspirvcross), `grab_fetch`
a Metal-only macOS build's (`from: ["metal"]`), `grab_fetch`, `fused_post` and JSON converter
plugins ([plugins.md](plugins.md#converter-plugins-a-ports-own-conversion-steps)) run on pmu.
Hollow Knight's whole setup (player fetch, winbcl, convert with grab_fetch, android-data with the
video shader and asmpatch) runs on pmu and gives the Python's APK and OBB, byte for byte. Metal
was compared on four free macOS games of 2020.3 - 6000.3 (below) before `metal` went into
`PMU_SHADER_SOURCES` (2026-11-02).

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
| | the same with shaders from Metal (metal.py), Vulkan programs of Unity 6000.6's typed layout, or a JSON converter plugin (edits.py) | yes (2026-11-02) |
| | the same with a porter's Python converter plugin | no: status 2, the release ships the Python |
| | `android-data ... --consume-src --verify-bundles 2 --pack all` (with asmpatch, StreamingAssets modes, `--managed-file`) | yes |
| mono-arm64 | the android route's commands, with `player fetch --managed` and `android-data --mono64` | yes (the template ran the Python before 2026-11-02; it now takes pmu when the release has it) |
| android, mono-arm64, il2cpp-arm64 (a Wwise game, `wwise.banks`) | `wwise <data>/<banks> <port>/wwise --wwise-lib <data>/Plugins` (crates/wwise): the soundbanks for ua_host's Wwise player, the Vorbis codebooks read from the game's Wwise library | yes (Rust only; without pmu in the release the step is skipped: the game stays silent) |
| il2cpp-arm64 | `macpkg`, `unpack`, `version`, `convert` | yes (as the android route) |
| | `il2cpp <data> <out> --on-device --abi arm64-v8a ...` (il2cpp.py, il2cpp_device.py, il2cpp_bee.py, il2cpp_mix.py): Unity's IL2CPP and player by range reads, Debian's compilers and .NET from Microsoft (pinned by checksum), then UnityLinker, il2cpp and the C++ build | yes (crates/il2cpp, 2026-11-02; the template ran the Python before) |
| | `android-data --il2cpp <dir> ... --consume-src --verify-bundles 2 --pack all` | yes |
| apk | `apk <inputs> <dir> --target T --json` (apk.py) | yes (2026-11-02) |
| | with `player.il2cpp`: `apk ... --il2cpp --on-device` (the IL2CPP build above, on a system without a 32-bit userland) | yes (crates/apk's il2cpp.rs on crates/il2cpp, 2026-11-02; the release shipped the Python before) |
| | apk.json's fields read into shell variables | sed (before 2026-11-02 a `"$PY" -c` one-liner) |
| box64 | `route <gamedata> --env` (route.py: the Linux x86-64 build found, its Unity version, backend and GL mode) | yes (the template ran the Python before 2026-11-02) |
| | `convert <data> --config gles.json` (shaders, settings) | yes |
| Cuphead (portmaster-cuphead: `setup/patchscript`, `setup64/patchscript`, `setup/dlcscript`, `setup/resume.txt`, the launcher) | `zip-extract`, `macpkg`, `patch-obb check / new / ggm / pack`, `player donor` (mono armv7; il2cpp arm64 `--managed`), `convert --plugin animclips` (the DLC: `--steps shaders,textures,audio`), `journal-files`, `android-data` (64-bit: `--mono64`) | yes: the release ships no Python (its `codepatch` is .NET) |

So the android, mono-arm64, il2cpp-arm64, box64 and apk routes' releases ship pmu and no Python
unless the port brings a Python converter plugin of its own (a JSON one runs on pmu): what is left,
and why, is at the end of this page.

## Layout

```
setup-rs/
  Cargo.toml          the workspace (members: pmu and each crate)
  pmu/                the binary: src/main.rs, src/cmd/<command>.rs (one module a command)
  crates/<name>/      libraries, one owner each while the port is under way
  vendor/<name>/      C/C++ taken from the libraries the Python uses, with their licence files
  tests/golden/       golden-test scripts (run Python and pmu on the same data, compare)
  fuzz/               fuzz targets (cargo-fuzz, a workspace of its own) and their made-up seeds
  tools/rs.sh         builds and tests in the arm64 Rust image
```

Crates while the port is under way (each is built and tested on its own branch, then merged):

| crate | owns |
|---|---|
| `pkgzip` | xar, cpio, pbzx; zip reading and extraction; the aligned STORE zip writer and its check (android_data `_write_zip`, `verify_zip`); `macpkg`, `zip-extract`, `player donor` |
| `unityfs` | UnityFS bundles (LZ4, LZ4HC, LZMA blocks), serialized files, type trees (the files' own, and the class database the Python takes from UnityPy/TPK), objects as typed value trees; reading and writing them as the Python does |
| `texcodec` | texture decoding, scaling, ASTC encoding with the guide; textures.py's work on one texture as a pure function |
| `shaderx` | shader blobs (shaders/blob.py) and the GLSL rewriting the convert step does (shaders/convert.py, glsl.py, step.py and what they call), as pure functions; Unity's binding info (dxbc.py's Bindings), Direct3D 11 (dxbc.py, with pmhlslcc), Vulkan (vulkan.py, spirv.py, with pmspirvcross), Metal (metal.py), grab_fetch and fused_post programs |
| `convert` | `pmu convert` (on unityfs, shaderx, texcodec): __main__.py's convert path, job.py (the in-place run and its journal), the steps shaders (step.py's files and summary; on the Shader objects: Direct3D 11, Vulkan and Metal sources, compute shaders, grab_fetch, fused_post), settings, audio, quality (with monotree.rs), textures (textures.py's Unity-file layer), Cuphead's plugin step animclips and JSON plugins' field edits (edits.py) |
| `androiddata` | `android-data` and `patch-obb pack` (android_data.py and what it uses on a setup's path: clrmeta.py, plugins.py, retarget.py, typetrees.py, streaming.py, unity_version.py), on `unityfs` and `pkgzip` |
| `apk` | `apk` (apk.py): manifests (binary AXML, an App Bundle's protobuf XML), the ABI choice, OBBs, PlayerSettings on `unityfs`, zips read with `pkgzip` and written as zipfile writes them (deflate: zlib's own, `libz-sys`'s bundled copy) |
| `wwise` | `pmu wwise` (no Python twin): soundbanks of version 128 (BKHD, DIDX, DATA located, HIRC read object by object, each to its exact size, STMG), Wwise Vorbis rebuilt as Ogg Vorbis with ww2ogg-rs (`vendor/ww2ogg`, BSD-3-Clause, without its built-in codebooks) and the codebooks found in the game's own Wwise library (`codebooks.rs`), its pages packed to 4 KB (`ogg.rs`), IMA ADPCM and PCM copied, the json of docs/wwise-player.md; `--check` decodes every Vorbis packet with lewton and resolves every event |
| `il2cpp` | `il2cpp --on-device` and `il2cpp fetch` (il2cpp.py's toolchain and build, il2cpp_device.py, il2cpp_bee.py, il2cpp_mix.py, il2cpp_device_lock.py), on `pkgzip` (range reads, NSIS), `androiddata` (.NET metadata, versions, JSON) and `unityfs` (MonoManager's assembly list) |
| `video` | the `video` step's work on one clip (pmu's own: there is no Python step): WebM read and written (EBML; SeekHead, Cues and Void left out), VP8 decoded and encoded by libvpx (vendor/libvpx: Debian 12's libvpx 1.12.0 source, the VP8 encoder and decoder configured for arm64 with NEON, compiled in by build.rs through csrc/vpx_capi.c; other architectures build without it), a 2x2 box downscale |

Later (on top of `unityfs`): `android-data`, `patch-obb pack`.

`crates/wwise`'s tests (`cargo test -p wwise`) run `pmu wwise`'s code on made-up banks written in
the test (`tests/madeup.rs`: every object type, ADPCM, PCM and streamed media, an object that does
not read, another bank version); on a game's banks, `pmu wwise <banks> <out> --check` decodes every
Vorbis stream (lewton) and resolves every event (docs/wwise-player.md has Enter the Gungeon's
numbers).

### `pmu convert`

The same command line as `python -m pmunity convert` (argparse's rules), the same files, journal
(`.pmunity.json`, json.dump's text) and log lines (timings apart), the same exit statuses (0; 1
with sys.exit's message or the error's; 2 for bad arguments; 3 when the card is too full). Either
implementation continues the other's journal. Two differences, both outside the bytes compared:
the Python writes the journal's `audio.switch` in its set's hash order (it changes from run to
run; pmu writes the files in order), and where the Python prints a traceback pmu prints the
error's message. Anything not ported stops the run with status 2 and says so, after the steps
before it have journaled their files, so a setup can run the Python on the same folder to finish:
Python plugins other than animclips. `grab_fetch`
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
gives the Python. Unity 6000.6's typed binding info (dxbc.py's `typed`: each binding an (encoded
data, slot) pair, lists by kind) is read and written by `bindings::Bindings::parse_typed`,
`entry_bindings_typed` and `gl_bindings_as` (the Vulkan path is the only one that asks for it).

Shaders from Vulkan (`from: ["vulkan"]`, Vulkan-only builds such as HDRP's Linux ones) are ported:
shaderx's `spirv` (SPIR-V modules as spirv.py reads and edits them: names, decorations, types,
the entry point) and `vulkan` (vulkan.py: the entry's stage modules decoded from SMOL-V and given
the names Unity's GL programs have, pmspirvcross as a server process, its GLSL made Unity's GL form: constant buffers flattened or kept as blocks, samplers and
their units, input and output locations, the uv origin of full-screen passes, the ES check), and
the convert crate's `steps/vulkan.rs` (add_gles3_vulkan: the programs stored by the data's Unity
version, the shared parameters put back into the Vulkan entries; 6000.6's in its typed layout, by
its names). The setup gives pmu `setup/pmspirvcross` through `$PMSPIRVCROSS`.

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
`tests/golden/metal_golden.py` from convert/tests/test_metal.py's MSL and variants of it); on
games, `regress.py pmu` (below: four macOS builds of 2020.3 - 6000.3, the same files).

Player builds keep no type trees in their files: pmu reads them with UnityPy's class database
(lzma.tpk), embedded when the binary is built with `PMU_TPK=<lzma.tpk>`, else from `$PMU_TPK` at
run time (without either, the first such file stops the run, status 1, saying so).

Where pmu differs from the Python on purpose (2026-11-02, [below](#on-the-handheld-time-memory-bad-conditions-2026-11-02)):
the journal is written fewer times (a file a step leaves as it is is marked with the next write;
the files and the final journal are the same, a killed run looks at a few files again); a second
`convert` (or `android-data`) on a folder another one is working on stops at once, status 1; a card
that fills up while a file is written ends with status 3 (`android-data`: 4) and says so, where the
Python's write raises (status 1); I/O and decoding errors name the file and the texture.
`PMU_PROF=1` prints each step's wall time by stage on stderr; `PMU_TEXTURE_POOL_MB` sets (0: no)
the memory the textures step's workers may hold.

**Bundles with several serialized files** (scene bundles: `BuildPlayer-<scene>` and its
`.sharedAssets`) are pmu's own (2026-11-02): the Python's Loaded opens one serialized file a
bundle, so its shaders, textures and edits steps leave such a bundle as it is ("expected one
serialized file in the bundle, found 2"), and Enter the Gungeon's title and Breach drew magenta.
unityfs's `DataFile` opens every serialized file of a bundle when a step asks
(`Options::all_serialized`): `sf` is one of them, `select(k)` makes the k-th `sf`, `file_of(id)`
follows a PPtr's file id to another serialized file of the bundle (the external path's last part,
without regard to case: Unity writes it in lower case), and writing saves the files that changed
(the others and the other nodes keep their bytes; packed as a one-file bundle is). The shaders step
converts each file's shaders and compute shaders (a fused_post chain may span them); the textures
step each file's textures into a stream of its own, then the sprites and atlases of all of them,
a PPtr into another file followed, and journals the scaled sizes under `<file>:<serialized file>`;
a bundle whose files share a stream one of them would replace is left as it is. JSON plugins'
edits reach every file. A bundle a step leaves (no serialized file, a node that is not one,
files sharing a stream) and a file under StreamingAssets that looks like a Unity bundle of another
kind are listed as `<step>: WARNING: ...` at the end of the step's summary. Without a Python
reference for such a bundle, it is checked against the Python's conversion of its serialized files
taken out of it as plain files (`tests/golden/scene_bundles.py`), and everything else stays the
Python's byte for byte: `tests/golden/convert_madeup.sh` runs its cases without the made-up data's
scene bundle and then `scenes` (pmu's bundle against that reference, every other file against the
`cuphead` case's); on Enter the Gungeon (`tools/regress/pmu_pair.py`, the port's config) the 3
scene bundles with shaders are the only files that differ from the Python's, and their 8
serialized files are the reference's; `regress.py pmu` on the six corpus games on this machine
(none has a scene bundle): every converted file the Python's, as before. `cargo test -p convert
--test scenes` covers the steps on
made-up scene bundles (the same bytes as the same shaders and textures elsewhere, a sprite in the
scene following its texture in the shared assets, edits, the bundles left) and `cargo test -p
unityfs` the opening, selecting, saving and inlining.

`pmu journal-files <data>/.pmunity.json` prints the files the journal's steps have finished (0
when there is none), as resume.txt's `journal_files` does with Python, for the setup's loop that
reruns a conversion the kernel killed.

Plugins: a JSON plugin (`--plugin <file>.json`: field edits, `steps/edits.rs`, the Python's
edits.py) runs on pmu; pmu has Cuphead's plugin step built in, by the file's name: `--plugin
<path>/animclips.py` (the path need not exist) or `--plugin animclips` is portmaster-cuphead's
convert/animclips.py. Any other plugin stops with status 2.

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

### `video`

pmu's own step (convert's `video`, crates/video; no Python counterpart): a game's VP8 clips
(WebM, a Linux build's VideoClips) taller than `max_height` are halved until they are not (2x2
means) and encoded again with libvpx: constant quality `cq_level` (10) under `max_kbps`, speed
`cpu_used` (4), key frames where the clip had them, no look-ahead, four token partitions. Each new
clip is smaller and goes where the old one was in its resource file, the rest of that place
zeroed, so no other object of the file moves; the VideoClip gets its new `m_ExternalResources.m_Size`,
`Width` and `Height`, spliced into its asset file in place (its numbers keep their width). `mode`:
`auto` (default: where MemTotal is at most `ram_mb`, 1024), `on`, `off`. A resource file and its
asset files are committed together and journalled together. Clips of other codecs and clips in
bundles stay as they are.

Why: Hollow Knight's cinematics are 1920x1080 VP8 at 25 - 30 fps. On an H700 (Cortex-A53) the
media core's libvpx decodes such a frame in 31 ms alone and ~40 ms in the game, so with the player
waiting for every picture they played in slow motion against their music. Halved (960x540) they
are a quarter of the decoding and look the same on a 720x480 panel: the prologue's luma 56.6 dB
on average against the original halved the same way (45.2 dB at the worst frame); with ua_host's
display rate while a video plays (`UA_VIDEO_RATE`, [runtime.md](runtime.md)) the cinematic shows
its 30 pictures a second apart from the game's own loading frames (muOS, 2026-11-02).

On the handheld (muOS, H700, 2026-11-02): Hollow Knight's 14 clips over 720 lines (9,116 frames,
84.2 MB) in 578 s, about 63 ms a frame, 43.9 MB after; its two 720p clips are left as they are.

`pmu video <game dir> [--config <convert.json>] [--set KEY=VALUE]...` runs the step on a game set
up before it existed (with the port's convert config: only when its steps name `video`, with its
`video` settings; the mode is checked before anything is read): the
resource files of its APK and OBBs (assets/bin/Data/*.resource) that hold WebM clips are taken out
with their asset files and globalgamemanagers into `<game dir>/.pmunity-video`, the step runs on
them, and each file it changed is written back over its APK or OBB entry (stored and of the same
length: `pkgzip::zip::Archive::overwrite_stored`, its CRC-32 in the local header and the central
directory). Every file is checked against its entry before any is written; the changed files wait
marked ready until all are written back, so a run that is stopped writes them again. On the muOS
test copy of Hollow Knight: 6 files written back after the step, the APK and OBB whole (`unzip -t`),
a second run finds nothing to do (48 s of reading the resource files).

### `pmu il2cpp`

`pmu il2cpp <data> <out> --on-device [--abi --home --pkg --jobs --keep-cpp --strip --unity
--managed-file --asmpatch --assembly-patch NAME COMMAND --streaming-assets --data-path --link-xml
--dry-run --toolchain]` and `pmu il2cpp fetch <version> [--on-device ...]` are crates/il2cpp:
il2cpp.py's toolchain and build (`tools.rs`, `build.rs`), il2cpp_device.py (`device.rs`: the tool
folder from Debian's packages (ar, data.tar.xz, `tar.rs`) and Microsoft's .NET tarball, pinned
by `lock.rs`, a copy of il2cpp_device_lock.py that a test checks; `native.rs`: the steps, the
compile scheduler, the wrappers' scripts), il2cpp_bee.py (`bee.rs`), il2cpp_mix.py (`mix.rs`),
the range reads of `fetch.rs` (pkgzip's HTTP client and NSIS reader; NDK zip members) and
tools/il2cpp/ndk-clang.sh built in (`src/ndk-clang.sh`, checked against the repository's by a
test). The build runs the same programs (UnityLinker, il2cpp, clang/lld 14 through the wrappers)
with the same command lines, environment and folders as the Python; what pmu writes itself (the
fetched parts, the tool folder and its wrappers, the staged assemblies, link.xml files, Bee's
written and copied files, the step records `.done-*` and `pmunity-done.jsonl`, il2cpp.json) is
written as the Python writes it, so either one continues the other's interrupted
build. The Docker route (no `--on-device`) stays with the Python (status 2).

`android-data --il2cpp <out>` is androiddata's: IL2CPP's Managed/ (Metadata, Resources, etc)
instead of the assemblies, the assembly list checked against il2cpp.json's, the build's versions
and StreamingAssets / data-path patches checked (il2cpp_mismatch, il2cpp_folder_patches), Unity
2020.x's `buildDate` in boot.config and 2021.x+'s `unity_app_guid` from the metadata's sha256,
`abi` in android_package.json, one APK when the data fits.

Compared with the Python (2026-11-02), in the `ua-rust` image (arm64, natively, as on the
handheld; 8 CPUs, 4 GB): `tests/golden/il2cpp_device.sh <data> <scratch> py1|rs1|py2 [options]`
runs one build at the same paths (`--keep-cpp`, so the generated C++ and the objects are kept)
and records every file left (sha256, mode) and the text the build writes;
`tests/golden/il2cpp_compare.py <a> <b> --noise <c>` compares two runs, where <c> is a second run
of the Python: what differs between two Python runs differs from build to build anyway (il2cpp's
cached objects are named by hashes of what they are built from, so they are compared by their
source, from cc.log). `KILL_AT=<n>` kills a build, every process of it, after n compiles, and
`RESUME=1` runs the next one on what it left.
`tests/golden/il2cpp_android_data.sh` runs android-data --il2cpp by both on copies of the data.
The reference Python needs OpenSSL 1.1 (handhelds have it, the image has not: `PY_SSL_LIB`).

Results (2026-11-02; every run with the setup's options, `--streaming-assets obb`):

| data | what it takes through | result |
|---|---|---|
| a 2018.4.36f1 Linux game (.NET 4.x), `--dry-run` | Unity's editor parts (il2cpp, the unityaot class libraries), NDK r16b and the IL2CPP player by range reads; UnityLinker's and il2cpp's command lines | the home's 4345 files the same, the messages and command lines the same |
| the same, the whole build | UnityLinker.exe on Mono, il2cpp.exe on .NET 8, il2cpp's own builder given its turns by the scheduler (677 compiles); the tool folder (37 Debian packages, .NET) | 5757 of 5773 files the same: the tool folder, Unity's parts, the staged assemblies, the 286 MiB of C++, global-metadata.dat, 676 of the 677 objects; libil2cpp.so of the same size. The 16 others differ between two Python runs too: UnityLinker's output (each assembly's MVID), the object of libil2cpp's Runtime.cpp (`__DATE__`, `__TIME__`) and what is linked from it, il2cpp.log's timings, cc.log's response-file name, il2cpp.json's times and memory |
| the same, killed (every process, as a power cut) after 300 compiles by one implementation and run again by the other, both ways | `.done-*`, il2cpp's object cache | "UnityLinker: done before", "il2cpp: C++ written before", "(279 / 289 done before)"; every file as a fresh build's (but for the build-to-build ones) |
| the same, `PMUNITY_IL2CPP_SCHED=0` | the job slots instead of the scheduler | the same |
| Cuphead (macOS, 2017.4.9f1, built with 2017.4.40f1's IL2CPP; .NET 3.5, the "unity" profile) | 2017.x's UnityLinker with its link.xml, Mono's class libraries, NDK r13b, MonoManager's assembly list (unityfs), 752 compiles | 4561 of 4618 files the same (451 MiB of C++, the metadata); the 57 others differ between two Python runs too (the 50 linked assemblies' MVIDs, Runtime.cpp's object, ...) |
| a 2020.3.49f1 Linux game (incremental GC) | a self-contained .NET Core il2cpp (deps.json), baselib.a, `--jobs`, the time slice, NDK r19 | 5403 of 5428 files the same; the 25 others differ between two Python runs too: 2020.3's il2cpp writes seven of its metadata tables, and so global-metadata.dat, in another order each run, and their objects with them |
| a 2022.3.36f1 Linux game (URP) | Bee's graph (780 commands; WriteFile payloads, copies), `--allowed-assembly`, NDK r23b | 6176 of 6183 files the same (libil2cpp.so too); the 7 others differ between two Python runs too (Runtime.cpp's object, Bee's own trace, the logs, the done records of what links it) |
| the same, killed after 250 compiles by pmu and finished by the Python | Bee's pmunity-done.jsonl | "553 commands (227 done before)": every file as a fresh build's (and the compiles cut short's `.o.tmp`) |
| `il2cpp fetch 6000.0.58f2 --on-device` | a mixed build's toolchain: Unity 6's editor parts, the converter (2022.3.62f3) and its compat check, the mixed folder, NDK r23b and its API 23 libraries, the tool folder | the 6856 files and the printed JSON the same |
| `android-data --il2cpp` on the builds of 2018.4, 2020.3 (boot.config's buildDate), 2022.3 (unity_app_guid) | the setup's arguments, `--consume-src --pack all` | status, every file (189, 183, 203: the APK, android_package.json, the report, what is left of the data) and the state the same |
| `apk <apk> --target arm64 --il2cpp --on-device` (the apk route's `player.il2cpp`) on two free Mono APKs (armeabi-v7a only): a 2022.3.23f1 card game (Bee, unity_app_guid) and a 2019.4.18f1 action game | apk.py's il2cpp_stub (the assemblies, boot.config, ScriptingAssemblies.json out of the APK), the build with the data's version and MonoManager's list, il2cpp_apk (base.apk without lib/, the assemblies and the signature, with IL2CPP's Managed/ stored and aligned), lib/ from the IL2CPP player | 5786 of 5794 and 5021 of 5032 files the same, base.apk among them (and libil2cpp.so for the 2022.3 game); the others differ between two Python runs too (Runtime.cpp's object and what is linked from it, the logs, android_package.json's `build_seconds`) |
| the 2018.4 game through the route on pmu alone | `il2cpp --on-device`, `convert` (shaders, settings), `android-data --il2cpp`, then the arm64 rig (tools/emu) | its title menu ("Scripting Backend 'il2cpp', CPU 'arm64-v8a'"), no exception |

pmu is not faster at this: the time is UnityLinker's, il2cpp's and the compilers' (the same programs);
what pmu saves is the setup's Python (its unpacking and its memory beside the compilers). Not run on
a handheld yet (the builds are an hour or more on a Cortex-A53).

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
`crates/androiddata/src/video_decode_android.shader`, a copy of the Python's); a bundle with
several serialized files and streamed textures (`--inline-textures`: the Python's in-memory path)
is inlined as the Python inlines it, one texture at a time (2026-11-02; before, status 2:
`tests/golden/inline_scenes.sh` compares the two on made-up scene bundles). A serialized file too
big for one archive with its textures inlined keeps its `.resS`, split into parts that fit one (`crates/androiddata/src/ress_split.rs`; the Python stops at
packing there): [android-data.md](android-data.md), section 4. `cargo test -p androiddata` (with
`PMU_TPK`) runs that on made-up 2021.3 data (`tests/ress_split.rs`): the parts, the rewritten
streams, the archives within a lowered limit, every texture read back from them. The test-only
knobs `PMU_ZIP_LIMIT` and `PMU_RESS_SPLIT` (same page) make it happen on small data.

`androiddata`'s golden test, `tests/golden/android_data.sh <scratch> [mini32 mini64 obb video space 32 64]`,
runs the Python and pmu with the setups' exact invocations on copies of `converted-fast` (the whole
game, or a part of it for the mini cases) and compares status, the output folder, every archive's
entries and the data `--consume-src` leaves.

`pmu apk` (crates/apk) is apk.py's import: the zips it writes (split asset packs merged into one
APK, an App Bundle rebuilt) are zipfile's, entry by entry (the local header written again with the
CRC and sizes, stored data aligned by the zipalign-style extra field), deflated with zlib's own
deflate at its default level (`libz-sys` built with its bundled zlib: Python's zipfile compresses
with the system's zlib, and zlib-rs, which pkgzip reads with, gives other bytes); PlayerSettings,
BuildSettings and MonoManager are read with unityfs and UnityPy's class database (a stored entry in
place, a compressed one in memory, as the Python holds it). `cargo test -p apk` (with `PMU_TPK`)
runs it on made-up builds (binary and protobuf manifests written in the test); the golden test runs
both implementations case by case and compares status, stdout, stderr and every output file:

```bash
RS_ROOT=... setup-rs/tools/rs.sh <worktree> bash -c 'PMU_TPK=/opt/pylibs/UnityPy/resources/lzma.tpk \
  PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH=<worktree>/convert:/opt/pylibs \
  /opt/py/bin/python3 tests/golden/apk_golden.py <scratch> --pmu <pmu> [--real <a folder of APKs>]'
```

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

Then on 2026-11-02 (branch rust-apk), with free games downloaded for it (itch.io; deleted after):

| what | data | result |
|---|---|---|
| Metal, the games' own shaders (`--set 'shaders.from=["metal"]' --convert-only`: port new's steps and the check's suggestions) | four Metal-only macOS builds: 2020.3.49, 2021.3.16 (URP), 2022.3.4 (URP), 6000.3.11 | the same files (184, 318, 220 and 1342 programs; the SubShaders left out the same: MSL with `base_vertex`, writable textures, device buffers, multisampled arrays) |
| | a 6000.6.5 macOS build | both stop the same way in the shaders step (writing a Shader object: the Python's struct.error, pmu's "data: -1 out of range"): 6000.6's Metal entries are a gap of the Python's too |
| Vulkan of 6000.6's typed layout (`--set 'shaders.from=["vulkan"]' --convert-only`, and `--builtin`) | two 6000.6 Linux builds (6000.6.2, 6000.6.5) | the same files: the games' own shaders (97 shaders, 526 programs; 85, 701) and their players' built-in resources (5 shaders, 14 programs each); pmu stopped with status 2 before |
| a JSON converter plugin (`pmu_pair.py --plugin`: QualitySettings, AudioManager, Light, Camera and Material fields) | three corpus games (2018.4, 2020.3, 2022.3) | the same files (26 objects edited in the 2020.3 game); the 2022.3 game: both stop with the same message (a field its Materials do not have) |
| `apk` (`tests/golden/apk_golden.py`, the musl build) | 71 made-up builds (79 since `--il2cpp --on-device` is ported: its dry run, its refusals before the build: 2017.3, 2017.4 before .16, 6000.3, an IL2CPP build with only armeabi-v7a, no assemblies; 79 of 79 the same); four free games' APKs (2019.4 and 2022.3 Mono armv7, 2021.3 and 2022.3 IL2CPP), each as it is and laid out again from its own files as split APKs, an XAPK, an APKS, an App Bundle and an APK with its data in an OBB | 107 of 107 the same: exit status, stdout, stderr, every output file (base.apk merged or rebuilt, lib/, OBBs, android_package.json) |

## Porter plugins without Python

A port's converter plugin in Python (a file with `STEPS = {name: function(job)}`, run in the
converter's process with the job's API) cannot run on pmu. Two ways keep such a port's release free
of the Python ([plugins.md](plugins.md#converter-plugins-a-ports-own-conversion-steps)):

- A JSON plugin (2026-11-02): named steps of field edits (a glob over the data files, the class,
  a glob over m_Name, field paths and values), which pmu (`crates/convert/src/steps/edits.rs`) and
  the Python (`convert/pmunity/edits.py`, plugged into `--plugin` by the file's `.json`) run the
  same way, with the same messages; `pmu_covers` counts it as pmu's. Nothing of the porter's runs,
  so nothing needs a sandbox or a protocol. Checked with `tests/golden/convert_madeup.sh` (the
  `edits` cases) and `pmu_pair.py --plugin` (above). MonoBehaviours are refused (their fields
  need the game's assemblies' type trees, which neither implementation gives a plugin).
- A step built into pmu, chosen by the file's name (`--plugin <dir>/animclips.py` or `--plugin
  animclips`; `plugin()` in `crates/convert/src/steps/mod.rs`, `PMU_PLUGINS` in port.py):
  Cuphead's animclips. The plugin's step is ported into `crates/convert/src/steps/<name>.rs`,
  checked against the plugin on the game's data (`pmu_pair.py --plugin <file>`), its name added
  to `plugin()` and `PMU_PLUGINS`. Built-ins are matched by the file's name only: a porter's own
  plugin called `animclips.py` would run Cuphead's step, so a new plugin needs a name of its own.

A porter's program speaking a JSON protocol with pmu was the other design looked at: it would
need the job's API (files, objects, type trees, the journal) as a protocol, a program the
handheld can run, and the porter's code run by the setup; field edits cover what the ports seen
change, with none of that. This repository has no converter plugins of its own (animclips lives
in portmaster-cuphead and is built in).

## What is left, and why

- `il2cpp` and `apk --il2cpp` without `--on-device` (the Docker route of a porter's PC:
  tools/il2cpp/root.sh's folders in a stock container): status 2. pmu is for the handheld; a PC
  runs the Python.
- Unity 2023.1 - 6000.2's mixed builds (crates/il2cpp's mix.rs) are compared on `il2cpp fetch` (the
  converter, the mixed folder, NDK r23b's API 23 libraries) but not on a whole build: no Mono game
  of those versions was on this machine.
- A porter's Python converter plugin (above): the release ships the Python; a JSON plugin or a
  built-in step avoids it. Such a release's converter also leaves scene bundles as they are (the
  Python opens one serialized file a bundle): `port check` says so for a game that has them.
- The quality step still looks at no StreamingAssets bundle (scenes, lights and URP data there
  keep the game's quality; it says so in its summary), scene bundles included.
- `tests/golden/scene_bundles.py`'s reference needs serialized files that carry their Unity
  version (taken out of the bundle, a file without one does not open as a plain file: the 6000.x
  Addressables bundles of shaders.md); Gungeon's (2017.4) and the made-up ones do.
- Metal entries of Unity 6000.6: the Python stops on them too (a 6000.6.5 macOS build: both
  stop in the shaders step, status 1), so there is nothing to compare yet.
- The comparison itself: the corpus games of the regression list that are gone from this machine
  (Windows builds through Direct3D 11, more macOS and Unity 6 games, compressed builds, Vulkan-only
  ones) are where a divergence would show next.

## Fuzzing

What pmu reads comes from a download or an SD card: a damaged or hostile file must end in an
error and the command's exit status, never a panic, a hang, the end of the stack or an
allocation the file asks for. `setup-rs/fuzz/` (cargo-fuzz, libFuzzer; a workspace of its own,
outside the setup's builds and tests) has a target for each parser of untrusted bytes:

| target | reads |
|---|---|
| `zip` | the end record, central directory, local headers and data (stored, deflate, bzip2, LZMA); `zip-extract` |
| `nsis` | an NSIS installer: first header, script header (LZMA, deflate, Unity 6's chunks), entries, strings, files |
| `macpkg` | a .pkg: the xar header and TOC, each file's data, `macpkg` unpacking it |
| `cpio` | odc and newc archives |
| `pbzx` | pbzx, gzip, xar's zlib and bzip2, xz and .lzma chunks (the first byte picks one) |
| `http` | an HTTP response (status, 1xx, headers, chunked or sized body) and a redirect's URL |
| `bundle` | a UnityFS bundle: header, blocks info, blocks, nodes, unpacking, the bundle opened, its objects read and written back, inlined |
| `bundle_tools` | `unpack`'s data.unity3d reader and android-data's platform patching in place |
| `serialized` | a serialized file: header, metadata, type trees, every object read (the file's trees, else the class database's) and saved |
| `typetree` | a type tree (blob or old format) and an object read and written with it, managed references included |
| `texture`, `texture_encode` | a texture's fields and data decoded, scaled, prepared; small ones encoded to ASTC (search and guided) |
| `glsl`, `grabfetch` | a program through the GLSL ES rewriting; grab_fetch's check and rewrite |
| `shader_blob` | a Shader's blob fields: LZ4 blocks, entry tables, entries, written back |
| `clrmeta` | a game's managed DLL as android-data reads its .NET metadata |
| `journal` | `.pmunity.json` and the JSON pmu reads (convert's and android-data's), `journal-files`, a job resumed on it |
| `args` | command lines: pmu's argparse rules (cmd/argp.rs), `convert`'s and `android-data`'s |

The seeds are made up: `cargo run --example seeds <corpus>` (with `PMU_TPK`) writes them,
archives, installers and streams built there and Unity files from the convert tests' made-up
data. Corpora and crash files stay out of the repository (`fuzz/.gitignore`). In the image
(Rust 1.88, stable: `RUSTC_BOOTSTRAP=1` for the sanitizer; cargo-fuzz 0.12.0, the last that
builds with 1.88, `cargo install cargo-fuzz@0.12.0 --locked`), from `setup-rs/fuzz`:

```bash
export RUSTC_BOOTSTRAP=1 RUSTFLAGS=-Cdebuginfo=line-tables-only
cargo fuzz build -a --strip-dead-code
cargo fuzz run -a --strip-dead-code nsis <corpus>/nsis -- -max_total_time=600 -rss_limit_mb=1024 \
  -malloc_limit_mb=512 -max_len=32768
```

`-a` keeps the overflow checks; `-malloc_limit_mb=512` makes any allocation a small input
asks for past half the handhelds' memory a finding. The image has no llvm-symbolizer:
`ASAN_OPTIONS=symbolize=0` and `addr2line -f -C -i -e <target> <offsets>` name the frames.

What the targets found (each fixed with a regression test of made-up bytes):

- Sizes and counts allocated before they were checked: unityfs's `read_vec` (a blocks info or
  an object of up to 4 GB), the blocks and nodes counts, LZ4 sizes past what a block can make
  (255 bytes a byte), LZMA blocks' room, unpack's and the platform patch's blocks; shaderx's
  LZ4 size; texcodec's pixels for a texture whose fields say 2^20 x 2^20 (the data is checked
  to hold the image first, as the whole decode checks it); `Bundle::unpack`'s 1 MB buffer a
  node (4 MB in all now); the .NET reader's row counts and android-data's stream reads.
- Integer overflow on a file's numbers: zip64 header offsets, NSIS's entry size, string table
  and data length (Python's integers: negative where the Python's are), xar's data ends,
  negative bundle node offsets, a node offset wrapping around, unpack's file ranges, a bundle's
  size field, texture sizes, Shader blob offsets, slices of i64::MIN, a .NET section offset
  near 4 GB and row counts of 2^32 - 1, a BC7 endpoint of 8 bits (`u8 >> 8`; the bytes are
  Pillow's).
- Panics on short or odd data: an NSIS solid header chunk cut short, a Shader platform with no
  segments, an LZ4 match of offset 0 in the platform patch.
- Recursion: type trees nested deeper than 256 (the old format's levels, a managed reference of
  its own class) and JSON nested deeper than 500 (a damaged journal counts as damaged) are
  errors; an object is read into at most a million values and 16 for each of its bytes (two
  billion empty structs, a million elements of structs nested around nothing).
- Decompression bombs and dictionaries: an LZMA dictionary a file asks for is allocated up to
  256 MB (exact for any smaller output); an NSIS file or script header up to 1 GB (Unity 6's
  chunks 2 MB each); a pbzx chunk up to 64 MB (Apple's are 16); a xar TOC up to 64 MB; a cpio
  name up to 256 KB.
- HTTP (by review, then its target): bodies past the request's range (and 64 KB) or 64 MB, lines
  past 64 KB and more than 100 headers (http.client's limits) are errors, not allocations.

None of this changes what a well-formed file gives (the bounds are far above what real data
needs): a limit reached is an error with its message, the command's status 1 (pkgzip's, unityfs's,
texcodec's and shaderx's errors as before; a damaged journal is status 5).

## On the handheld: time, memory, bad conditions (2026-11-02)

Hollow Knight's whole setup (the release's patchscript; GOG's Linux installer in `gamedata/`, Unity's
player placed beforehand: the handheld had no network) on the RG34XX-SP (4x Cortex-A53 at 1.5 GHz,
1 GB, Knulli; the card's `/userdata` is exFAT through FUSE, 87-91 % full), measured with
`tools/device/setupstat.py` beside it (every 2 s: the cores, memory, the card's I/O, the FUSE
daemon, each tool's CPU, memory and I/O; `setupstat_report.py` sums it up) and `PMU_PROF=1` (pmu's
wall time per stage on stderr, after each convert step). Main's pmu (8195e6c), then this branch's
twice (before and after its last two changes) on fresh copies of the game, the same card, each phase
started cool (CPU and GPU <= 50 C); the stock clocks and governors (the step's heat lowers the CPU's
clock: up to 75 C):

| step | main | branch, first | branch | what limits it (main) |
|---|---|---|---|---|
| unpacking (PortMaster's 7-Zip, not pmu) | 481 s | 483 s | 483 s | the card: 4.9 GB written at 10.7 MB/s, 0.24 cores busy |
| `player fetch` (the arm64 container, the network) | 13.6 s, 0.6 s CPU, 34 MB | | | 151 range requests for 13.3 MB |
| `winbcl` | 0.8 s | | | (nothing to do for this game) |
| convert: shaders | 138 s | 35.6 s | 36.4 s | pmu 0.08 cores busy: for each of the 1005 files a journal write with its two syncs (78 ms) and a scratch folder made and removed (14 ms) |
| convert: settings, audio | 11.9 s | 11.8 s | 11.0 s | |
| convert: textures | 1575 s | 1439 s | **1359 s** (pmu 2.5 cores busy) | 2.1 of 4 cores busy (1.9 with the steps before): the encoder's slowest thread on each small texture, reads waited for (~10-16 MB/s), the syncs of each converted file |
| convert | **1725 s** | 1486 s | **1407 s** (-18 %) | |
| convert, peak RSS; MemAvailable's low | 169 MB; 582 MB | 252 MB; 503 MB | 222 MB; 529 MB | |
| android-data | 777 s, 265 MB (asmpatch, a child: 256 MB) | 779 s | (as first) | the card: placing reads 3.0 GB and writes 2.5 GB (the inlined files), packing reads and writes 4.0 GB at 9.8 MB/s each, the card 90 % busy; pmu 0.16 cores |
| installing | 8 s | 7 s | | |
| **the setup** | **49.9 min** | **46.0 min** | **44.6 min** (-11 %; packing and installing as first) | |

(The 47-minute run of 2026-11-01 was main's pmu on the same card, less full: 86 %.) "Branch,
first": all but the last two changes below (the strips on the workers, the read-ahead's
order): it read 2.4 GB more than main (6.4 GB), the next textures' streams again after one left out.
android-data is the same code in both. The APK and OBB of main and branch: the same bytes (sha256
18182a99... and 88323ed0..., also those of the 47-minute run of 2026-11-01); every file of the data
folder after converting (1747 files: serialized files, streams, the journal) the same, with both
branch builds.

**The card** (`tools/device/cardbench.py`, through the FUSE mount):

| what | cost |
|---|---|
| a random read of 4 / 16 / 64 / 256 / 1024 KiB | 0.9 / 1.5 / 5.4 / 28.7 / 64.9 ms |
| a sequential read in 1 MiB reads | 20.7 MiB/s |
| a journal write as convert's (80 KiB, fsync, rename, the folder synced) | 78 ms (1.1 ms without the syncs) |
| two folders made and removed | 13.6 ms |
| 100 MiB written (32 KiB to 4 MiB writes) | 54-145 MiB/s into the page cache, 17-18 MiB/s with the fsync |

So the setup's time on the card goes to its syncs (each one waits for all the card's dirty data
and a flush) and to moving bytes at ~10-20 MB/s; the CPU waits whenever a step reads, syncs or
writes on its own thread.

### What changed (byte-identical)

| change | where | what it saves |
|---|---|---|
| the marks of files a step leaves as they were go to the journal with the next changed file's mark, at the step's end, or 2 s after the last write (`mark_unchanged`, `flush`); a run killed before such a write looks at those files again and leaves them again | convert `job.rs`, shaders and textures steps | ~2 x 1000 journal writes (78 ms each) |
| one scratch folder a step, made when something spills into it, emptied after each file (`work_dir_lazy`, `drop_spill`); the textures step's stream file made with its first blob | convert | 2 x 1005 folders made and removed, ~600 empty files |
| commit's last journal write (the end of `pending`) merged with the file's mark (`commit_then_mark`) | textures step | one journal write (2 syncs) a converted file |
| the next 8 textures' objects read early, their streams read in order by a reader thread, up to 32 MB ahead (in the textures' order: one read out of turn made the step read the next ones again, 2.4 GB more on Hollow Knight in the first try) | textures step `Streams` | the main thread's waits for the card (~10-16 MB/s for a texture's read) |
| a file's textures converted on workers, one per core, each with its own encoder on one core: those under 1 Mpix (new size) whole, a big single-level texture kept at its size a strip (512K pixels) each; their turns finished in the file's order (the stream, the objects, the totals as one texture at a time makes them); other big textures (mip levels, scaled) alone on every core, as before; what the workers' turns hold together is kept under a quarter of MemAvailable (at most 160 MB; a cgroup's limit counts) | textures step `Turns`, `Pool`; texcodec `strips`, `strip` | the encoder's barriers on each texture (its slowest thread), its threads started for each, the main thread's own work between textures |
| converted files written by a thread of their own (`unityfs::bgwrite::BgWriter`), the new stream synced every 16 MB as it is written | unityfs, textures step, android-data's inlining | the waits of write() for the card; the sync before the rename waits for the last part only |
| an archive's entries read three 1 MiB chunks ahead while written | pkgzip `write_zip` | read and write in turn |
| android-data's state written at most every 2 s while files are consumed (a consumed file is told by its source gone and its copy there, as before) | android-data | ~1240 writes of a growing JSON |

On a part of the game on the handheld (globalgamemanagers, resources and sharedassets 0-19, 771 MB,
`convert` as the setup runs it, `PMU_PROF=1`), every converted file and the journal the same:

| | main | lazy journal + read-ahead + pool | + strips on the pool, read-ahead in order, synced on the way |
|---|---|---|---|
| wall | 307.7 s | 281.5 s | **253.2 s** (-18 %) |
| CPU (user + system) | 705 s (2.3 cores) | 733 s | 720 s (2.8 cores) |
| the main thread waiting for texture reads | 47.8 s | 21.6 s | 27.0 s |
| ... for its syncs before the renames | 16.7 s | 20.1 s | 11.1 s |
| ... for journal writes | 4.7 s | 4.2 s | 2.8 s |
| peak RSS | 75 MB | 159 MB | 158 MB |

(The journal, scratch folders and a texture read ahead alone, without workers: 304.7 s.) The rest of the time
is the encoder: astc-encoder's search on the blocks the guided encoder leaves to it (12 % of
Hollow Knight's blocks) and the guided pass itself, at ~2.8 of the 4 cores; what keeps the fourth
idle is the main thread's work at each file's start (its tables, the first textures' reads) and the
card's writes when a file is committed.

What did not pay or was not done: the encoder itself (astc-encoder's NEON build) is the CPU, and
its blocks decide the bytes: a faster build (`-mtune=cortex-a53`, clang) would have to be checked
block by block against GCC 10's; a deeper pipeline across files (the next file's reads while this
one's last textures are encoded) would hide the few seconds at each file's start.

### Same output

- Hollow Knight on the handheld, the whole setup with main and with the branch: the APK and OBB
  sha256 the same; after converting (before packing), every file of the data folder (1747 files:
  serialized files, streams, the journal) the same, with both branch builds.
- The part of the game above on the handheld, converted by main, by the branch, and by the branch
  without workers (`PMU_TEXTURE_POOL_MB=0`): every file and the journal the same.
- In the arm64 container (Docker, 4 CPUs, 1 GB): the same part of Hollow Knight with its Managed,
  Plugins and the rest of the data folder through convert and android-data as the setup runs them
  (`--consume-src --pack all`, asmpatch): every converted file, the APK, OBB, tree and report the
  same (the state file holds the run's mtimes: it differs between any two runs, main's too); a
  Cuphead sample (ten atlas bundles, `resources.assets`, `sharedassets1`, Cuphead's config, guided
  ASTC): every file and the guided blocks' counts the same.
- `cargo test --workspace` (the textures step with workers on made-up data, the folder lock,
  BgWriter's writes, seeks and errors); `tests/golden/convert_madeup.sh` (pmu against the Python,
  every case); `tests/golden/android_data.sh` mini32 and space (pmu against the Python).

### Bad conditions

What each does with this branch's pmu, tested on the handheld (part of the game) and in the arm64
container (Docker, `--memory` without swap, a tmpfs as the card), next to main's:

| condition | what happens |
|---|---|
| convert killed (-9) at any point, then run again | goes on from the journal: the files done are skipped, a file cut short is done again from its source (its new files are in `.pmunity-work` until renamed together through `pending`). Container: killed at 7 points in a row (1-23 s), then to the end: every converted file and the journal as an uninterrupted run's. Handheld, the part of the game: killed 40 s into a run (31 files journaled) and 130 s into the next (33: inside resources.assets), then run to the end (a second launch refused meanwhile): every file and the journal as an uninterrupted run's. A run killed again and again inside one big file (resources.assets: 20 s in the container, ~3 min on the handheld) never gets past it: the journal moves a file at a time |
| android-data killed (-9) at any point, then run again | placing goes on from its state and `--consume-src`'s files already moved; an archive cut short (`<name>.tmp`) is written again. Container, killed at 0.5-5 s: the APK, OBB and tree the same; the report lists only what the last run placed |
| a card too full | convert: status 3 before a file is begun (`need_space`, "ERROR: not enough free space to convert <file>: N MB free, M MB needed"); android-data: status 4 before the archives (`pack: the APK and OBB need ... free N GiB more on the card`). New: a card that fills up while a file is written (another program writing; an estimate short; android-data's placing, which has no check of its own) gives the same statuses, with the file and "the card is full: free some space on it, then launch again" (`.../.pmunity-work/textures/stream.new: No space left on device`, `.../resources.assets.tmp`); main: status 1 and `No space left on device (os error 28)` without the file (the Python: a traceback). After space is freed the run goes on and the files come out the same (container: convert's every file and journal) |
| low memory | the arm64 container at 600 and 650 MB without swap: convert and android-data of the part of the game pass, the same files. At 450 MB android-data is killed while asmpatch (a .NET process, 236 MB anonymous) runs beside pmu (138 MB). On the handheld: convert's peak RSS 222 MB (main 169 MB: the workers' encoders, a context a tier, ~3.5 MB apiece, and their textures), MemAvailable at least 529 MB while converting (main 582 MB); android-data 265 MB with asmpatch's 256 MB as both, MemAvailable at least 489 MB |
| a second launch while one runs | convert and android-data lock the data folder (`flock` on the folder itself: no file is made, and the lock goes with the process when it is killed): the second stops at once, status 1, "<folder>: another pmu convert is converting this folder (a setup still running?): wait for it to end, then launch again". Main ran both, each undoing the other's files. On the handheld (exFAT through FUSE: the kernel's own locks, which the FUSE daemon need not know of) the second stopped at once, the first ran on |
| a damaged journal (empty after a restart) | status 5 and the message, as before |
| a read-only data folder | status 1, "<folder>/.pmunity.json.tmp: Permission denied (os error 13)" (main named no file) |
| a missing data folder | status 1, "<folder>: no globalgamemanagers, mainData or data.unity3d (not a Unity data folder?)" |
| a missing or cut stream file | status 1 with the file ("<...>/sharedassets10.assets.resS: No such file or directory") or the texture ("resources.assets: texture ui_0003_ps4_controller_image_osx (path id 305): not enough image data"); main named neither |
| a cut serialized file | status 1, "<file>: not a serialized file" |
| an installer cut short | Hollow Knight's Linux installer is unpacked by PortMaster's 7-Zip (the patchscript: "Could not unpack ..."); `macpkg` on Cuphead's add-on cut at 150 MB: status 3, "dlc_cut.pkg is incomplete: 150000000 of 1650520319 bytes (a download or copy that stopped early); download or copy it again", as documented; `zip-extract` on a zip cut in half or garbage: status 1, "zipfile.BadZipFile: File is not a zip file" |

patchlog.txt gets these lines as pmu prints them (the patchscript tees both streams).

### What is left

- **android-data writes every inlined file twice** (into the tree, then into the archive): 2.5 GB
  written and read again on the card, ~4 of its 13 minutes. Streaming them into the archives
  (their sizes from a dry run, the tree left out) changes what the command leaves (the tree), so it
  would be an option the setup asks for, and its resume would keep the sources until the archive
  is written.
- **Unpacking** (8 min) is the card's write speed for 4.9 GB (7-Zip, not pmu); reading the game's
  files from the installer's zip in place would save it.
- **The textures step** is the encoder's CPU now (astc-encoder at medium on the blocks guided
  encoding leaves to it, and the guided pass), at ~2.8 of the 4 cores; the rest goes at each file's
  start (its tables, its first textures' reads: the next file is not read ahead) and to the card
  when a file is committed. A faster build of the encoder (`-mtune=cortex-a53`, clang) needs every
  block checked against GCC 10's.
- **Memory**: the workers' encoders keep their contexts (~3.5 MB a tier and core) for the step;
  the peak rose from 169 to 222 MB on Hollow Knight (MemAvailable at least 529 MB on the 1 GB handheld).
- A kill during one big file loses that file's work (~2-3 min for `resources.assets` on the
  handheld): the journal moves a file at a time.

## Rules

- Byte-identical outputs (above). Exit statuses and the files a command leaves are part of it.
- No game files, no Unity files, and nothing derived from them in the repository.
- No local paths, user names or host names in the repository: test data comes from `$PMU_TEST_DATA`.
- Vendored code keeps its licence (`vendor/<name>/LICENSE*`), listed in THIRD_PARTY.md.
- Memory: the handhelds have 1 GB. Stream big files; never hold a whole bundle or OBB in memory
  when the Python does not.
- Dependencies: few and plain (crates with no build-time network or system libraries).
