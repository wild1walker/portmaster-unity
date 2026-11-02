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

Where pmu differs from the Python on purpose (2026-11-02, [below](#on-the-handheld-time-memory-bad-conditions-2026-11-02)):
the journal is written fewer times (a file a step leaves as it is is marked with the next write;
the files and the final journal are the same, a killed run looks at a few files again); a second
`convert` (or `android-data`) on a folder another one is working on stops at once, status 1; a card
that fills up while a file is written ends with status 3 (`android-data`: 4) and says so, where the
Python's write raises (status 1); I/O and decoding errors name the file and the texture.
`PMU_PROF=1` prints each step's wall time by stage on stderr; `PMU_TEXTURE_POOL_MB` sets (0: no)
the memory the textures step's workers may hold.

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
