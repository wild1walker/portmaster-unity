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

The setup's `codepatch` (.NET) and `CupheadPort.dll` stay as they are.

## The setups that run pmu

Cuphead's (portmaster-cuphead) and those `pmunity port new` writes for the android route: its
patchscript runs `setup/pmu` when the release has it (`pm() { "$S/pmu" "$@"; }`; the install step
reads the report's scripting runtime with sed), else the setup's Python. `pmunity port package`
ships pmu (`setup-rs/tools/build_pmu.sh`: the binary and the licence texts of what is compiled into
it) when it runs every command of the port's setup (`port.py`'s `pmu_covers`: no shaders from
Direct3D 11, Vulkan or Metal, no `fused_post`, no quality step, no converter plugins of the
porter's own), else the converter and the Python as before. Hollow Knight's whole setup (player
fetch, winbcl, convert with grab_fetch, android-data with the video shader and asmpatch) runs on
pmu and gives the Python's APK and OBB, byte for byte.

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
| `shaderx` | shader blobs (shaders/blob.py) and the GLSL rewriting the convert step does (shaders/convert.py, glsl.py, step.py and what they call), as pure functions |
| `convert` | `pmu convert` (on unityfs, shaderx, texcodec): __main__.py's convert path, job.py (the in-place run and its journal), the steps shaders (step.py's files and summary), settings, audio, textures (textures.py's Unity-file layer) and Cuphead's plugin step animclips |
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
the `quality` step, plugins other than animclips, shaders from Direct3D 11, Vulkan or Metal,
compute shaders (`shaders.compute`, on by default: Cuphead's config turns it off),
`fused_post`. `grab_fetch` (and `grab_fetch_names`) is ported: shaderx's `grabfetch` checks and
rewrites a program as grabfetch.py does (its symbolic evaluation with exact rationals), the
convert crate edits the Shader object (Hollow Knight's port sets `"always"`). The textures step is textures.py's Unity-file layer in `convert` with texcodec as
its per-texture codec (`steps::textures::Codec`; the made-up tests also run it with a stand-in).

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

## Rules

- Byte-identical outputs (above). Exit statuses and the files a command leaves are part of it.
- No game files, no Unity files, and nothing derived from them in the repository.
- No local paths, user names or host names in the repository: test data comes from `$PMU_TEST_DATA`.
- Vendored code keeps its licence (`vendor/<name>/LICENSE*`), listed in THIRD_PARTY.md.
- Memory: the handhelds have 1 GB. Stream big files; never hold a whole bundle or OBB in memory
  when the Python does not.
- Dependencies: few and plain (crates with no build-time network or system libraries).
