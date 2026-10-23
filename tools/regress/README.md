# tools/regress: the regression run

Takes a list of games through the tools the way a porter would, and says which game no longer
gets as far as it did: a changed shader translation, a step that fails, a game that crashes or
draws a black screen in the emulated rig. Two levels:

| level | what runs | Docker | checks |
|---|---|---|---|
| `fast` | per game: `port check` (or `route` for box64), the converter's shaders step, the other convert steps (`settings`), and where the list says so `android-data` (the tree, or the APK and OBB too); `pmunity apk` for Android builds; `pmunity il2cpp --dry-run` for the IL2CPP route | no | the steps succeed; android-data's own verification; the list's expectations (at least N shaders translated, at most M left out, ...); every count and digest the same as in the baseline |
| `rig` | per game: the whole route (convert, android-data `--pack all`, or `pmunity apk`, or `pmunity il2cpp` and its build), then the game in the emulated rig (`tools/emu`: QEMU armv7, or native arm64) with screenshots at fixed times | yes, one container at a time | no crash, still running when the time is up, frames advancing (at least N), a picture (not black, not one colour) in the shots the list names, log lines; against the baseline: shots that had a picture still have one, frames not far below it |

Run it **before merging a branch that touches `convert/` or `runtime/`**: the fast level for
every such branch (and whenever a game's output may change: shaders, android-data, the check's
suggestions, il2cpp's staging), the rig level for a branch that changes `runtime/`, packing or
the IL2CPP build, and before a release. If a change is meant to change a game's output, run the
fast level with `--update` and commit the baseline with that change, so the next run compares
against the new output.

## Setting up

1. A game corpus: `<corpus>/games/<slug>/game/` (the game as downloaded, unpacked) and
   `<corpus>/games/<slug>/source.json` (`{"version": "<Unity version>", ...}`), as the free
   itch.io games of [docs/compat/games.md](../../docs/compat/games.md) were collected. Games you
   own that are not in the corpus (a full port's game) go into `[games]` of the local config.
2. `cp tools/regress/local.example.toml tools/regress/local.toml` (git ignores it) and set the
   paths: `corpus`, `scratch` (where each game's work goes, deleted after its checks, and the
   results), `players` (folders of `pmunity player fetch` output; a missing version is fetched),
   for the rig the built `ua_host` (`hostbin`, `hostbin64`), for the IL2CPP route `il2cpp_home`.
   Or set them as `REGRESS_<KEY>` environment variables.
3. Built tools, as for porting: `tools/asmpatch` (games the check gives asmpatch), `tools/hlslcc`
   (Windows builds), `tools/spirvcross` (Vulkan-only builds), the runtime (rig), Docker (rig).

Python 3.11+ with the converter's requirements (`convert/requirements.txt`; the local config's
`python` runs the converter). regress.py reads the data only through the converter and
`digest.py`, which runs with Python's `-I`.

## Running

```sh
python3 tools/regress/regress.py fast                      # the whole list, fast level
python3 tools/regress/regress.py rig                       # the rig level (an evening)
python3 tools/regress/regress.py fast --only 'tom-city|d3d11'   # regex over ids, slugs, covers tags, routes
python3 tools/regress/regress.py list                      # the list: games x levels x checks
python3 tools/regress/regress.py fast --update             # record this run as the baseline
python3 tools/regress/regress.py fast --checkout ../other  # test another checkout with this list and baseline
python3 tools/regress/regress.py bisect <good> <bad> --level fast --only '^tom-city$'
python3 tools/regress/regress.py recheck <scratch>/results/rig-<time>   # a finished run, judged again
python3 -m unittest discover -s tools/regress              # the tool's own tests (synthetic data)
```

Options: `--keep` keeps each game's work folder (converted files, APK, the rig's device root);
`--pack none|tree|all` packs every android game of the fast level that way (by default the list
says which games android-data runs for); `--offline` fetches nothing (players, Windows class
libraries) and skips what is missing.

The result is a table, printed and written to `<scratch>/results/<level>-<time>/results.md`
(with `results.json`, and per game its logs, the rig's run log and small PNGs of its shots):

```
| game | route | covers | status | time | reason, notes |
|---|---|---|---|---|---|
| tom-city | android | 2020.3, crunched-textures | pass | 1:12 | |
| win-mixtape | android | 2021.3, windows, d3d11, ... | fail | 2:03 | 2 values differ from the baseline: shaders.programs: 301 -> 299; ... |
| cuphead | shaders | 2017.4, full-port, macos | skip | 0 s | /path (local config [games].cuphead) not found |
```

`pass`: every check held. `fail`: a step failed, an expectation of the list did not hold, or
(fast) a value differs from the baseline: the reason names it, and the game's `run.txt` and
step logs are in the results folder. `skip`: the game, a player or a tool is not on this machine,
or the game is not in that level. The exit status is 1 when something failed.

## The list: games.toml

One `[[game]]` per game and route, named by its corpus slug; the header of
[games.toml](games.toml) describes every key. It holds one or two games per Unity line (5.6 -
6000.6) on the 32-bit route, Windows (D3D11), macOS (OpenGL Core and Metal) and Vulkan-only
builds, compressed builds, the IL2CPP route (2017.4 - 6000.0, the mixed Unity 6 build included),
Android builds (APK route), box64, and the features that have broken before: compute shaders,
StreamingAssets read as files, Rewired, the Input System, InControl, FMOD (silent), video,
Addressables, Unity's Memory Advice files, winbcl. Cuphead and Hollow Knight are in it with their
ports' shaders and settings steps (`route = "shaders"`); `cuphead-title` runs the packaged Cuphead with
the runtime under test (`route = "prebuilt"`, paths in the local config).

A game's expectations come from what docs/compat/games.md records for it (e.g. "title": a
picture in the last shot, no crash, at least N frames). Shot times count from ua_host's first log
line, so a slow start from a slow drive does not move them. The rig is the emulated one (about
1 fps for the 32-bit route): frames vary with the Mac's load, so the frame checks are loose.

To add a game: give it a `[[game]]` (its slug, route, Unity version, what it covers, which
levels, its expectations), run `regress.py fast --only '^<id>$' --update` (and `rig`), look at the
result (the rig's shots are in the results folder), and commit games.toml with the baseline.

## The baseline: baseline/fast.json, baseline/rig.json

What a run measured, per game: the check's verdict (route, build kind, versions, suggested
options; never names), the shaders step's counts, a digest of the GLES3 programs it wrote (per
file and in all: the SHA-256 of each Shader's GLES3 blob and each ComputeShader's GLES3 variant)
and of the files it wrote, android-data's counts and the digest of its tree and of the APK's and
OBB's members, `pmunity apk`'s output, `il2cpp --dry-run`'s staging and commands (machine paths
replaced); for the rig, frames, exit, crash and per shot whether it showed a picture (dark share,
number of colours). Only counts, sizes and hashes of the tools' own output: no game file, no
picture, no shader or asset name (data files other than Unity's generic `level<n>`,
`sharedassets<n>.assets`, ... are keyed by a hash of their path). Keep it that way: the
repository rules ([CONTRIBUTING.md](../../CONTRIBUTING.md)) hold for it.

The fast level compares exactly: any difference fails, with the keys that differ (and for a
per-file digest, which files: the names are printed, not stored). `--update` records the values
of the games it ran (also when they fail an expectation: look at the reason first; not when a
step itself failed). `recheck <results folder>` judges a finished run again by the list and the
baseline as they are now (after tuning an expectation), without running the games.

## Finding the commit that broke a game

```sh
python3 tools/regress/regress.py bisect <good commit> <bad commit> --level fast --only '^<id>$'
```

runs `git bisect` over a temporary worktree (in the scratch folder) of this repository: at each
commit, this checkout's regress.py, list and baseline judge that worktree's converter (the
commit's `convert/`), so a commit is bad when the game fails there. For a change in compiled
parts give the build command: `--build 'runtime/build.sh'` (it runs in the worktree before each
test; a worktree has no `runtime/thirdparty/libhybris` submodule checkout: link it from your main
checkout first), and point `hostbin` at the worktree's `runtime/host/out`. A commit at which
nothing ran (all skipped) is skipped.

## How long it takes

Measured on the development Mac (Apple M-series, 8 cores, 8 GB) with the corpus on the internal
disk and two USB sticks, and the scratch folder on a USB stick that writes about 1 - 2 MB/s,
shared with another agent's conversions and IL2CPP builds (2026-10-23):

| level | games | wall time | the tools' CPU time | what the time goes into |
|---|---|---|---|---|
| fast | 54 | 73 min (60 min the second time, with the rig level running beside it) | 13 - 17 min | android-data writing the ten packed games' trees to the stick (0.5 - 7 min each); the rest is CPU: the shaders step in memory (5 s - 4 min a game: Hollow Knight and Cuphead the longest), port checks, `il2cpp --dry-run` (15 - 40 s), `pmunity apk` (about 1 min) |
| rig | 17 | about 3 hours (2 h 56 min for 16 of them) | 3 min (the rest is the emulator in Docker) | per game: packing (1 - 12 min on the stick), then the rig run itself (the list's 2 - 10 minutes); the two IL2CPP builds (2019.4: 16 min in all, the Unity 6 mixed build 24 min and 44 in all) |

The fast level's own work is its CPU time: with the scratch folder on an SSD, or on the corpus'
APFS volume (android-data then clones the game's files instead of copying them), it should take
little more than that, 15 - 20 minutes (an estimate: not measured on such a disk here). Each run
prints the CPU time under its table.
