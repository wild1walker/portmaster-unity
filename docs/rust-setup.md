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
| `patch-obb new\|ggm\|pack\|check` | patch_obb.py | an add-on's bundles (the DLC added later) as a patch OBB; `check` exits 3 on an incomplete copy |

The setup's `codepatch` (.NET) and `CupheadPort.dll` stay as they are.

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

Later (on top of `unityfs`): the convert job and its steps, `android-data`, `patch-obb`.

## Building and testing

The image `ua-rust` (arm64 Debian bookworm) has Rust 1.88 with the `aarch64-unknown-linux-musl`
target, and the reference Python: PortMaster's `python_3.11` at `/opt/py` with the setup's
packages at `/opt/pylibs`. On Apple silicon it runs natively.

```bash
RS_ROOT=<a folder for builds> setup-rs/tools/rs.sh <worktree> cargo test
RS_ROOT=... setup-rs/tools/rs.sh <worktree> cargo build --release --target aarch64-unknown-linux-musl
```

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

## Rules

- Byte-identical outputs (above). Exit statuses and the files a command leaves are part of it.
- No game files, no Unity files, and nothing derived from them in the repository.
- No local paths, user names or host names in the repository: test data comes from `$PMU_TEST_DATA`.
- Vendored code keeps its licence (`vendor/<name>/LICENSE*`), listed in THIRD_PARTY.md.
- Memory: the handhelds have 1 GB. Stream big files; never hold a whole bundle or OBB in memory
  when the Python does not.
- Dependencies: few and plain (crates with no build-time network or system libraries).
