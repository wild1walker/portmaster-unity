# IL2CPP: Mono desktop games on Unity's 64-bit ARM player

Unity's Android players with Mono are 32-bit ARM (armeabi-v7a) in every version; its 64-bit ARM
players (arm64-v8a) exist only for IL2CPP. A handheld without a 32-bit userland (ROCKNIX and
other 64-bit-only firmwares) can therefore run a Mono desktop game only after its managed code is
compiled ahead of time with Unity's IL2CPP, as a Unity IL2CPP Android build would have done:

```
python -m pmunity il2cpp <Game>_Data out/il2cpp --abi arm64-v8a       # the game's code -> libil2cpp.so
python -m pmunity convert <Game>_Data work/data --config game.json    # shaders, textures, settings (as for Mono)
python -m pmunity android-data --src work/data --pkg <IL2CPP player package> --il2cpp out/il2cpp \
    --out work/android --pack all                                     # APK + OBB with IL2CPP's metadata
```

`--abi armeabi-v7a` builds the 32-bit variant the same way (it runs on the 32-bit runtime and
the emulated armv7 rig; a Mono game does not need it there, but it is how the IL2CPP route is
tested before a 64-bit runtime and handheld are at hand).

Options for ports: `--managed-file DLL` compiles that assembly instead of the data's copy of the
same name (a port's patched `Assembly-CSharp.dll`, its helper assembly), `--assembly-patch NAME
COMMAND` runs a port's patch command (as `android-data --assembly-patch`: `{managed}` is the data's
`Managed` folder, `{out}` a folder where it writes NAME) and compiles what it writes, `--asmpatch
COMMAND` runs tools/asmpatch on the game's `Managed` and compiles what it changed (as `android-data
--asmpatch`: e.g. `--asmpatch "dotnet asmpatch.dll --only=rewired-android"`), and with it
`--streaming-assets both|loose` and `--data-path` add asmpatch's opt-in patches for StreamingAssets
and the data folder read with System.IO (below, "Files the game reads"). With IL2CPP the
assemblies are compiled into `libil2cpp.so`, so these go to `pmunity il2cpp`, not to android-data
(which refuses `--asmpatch`, `--managed-file` and `--assembly-patch` with `--il2cpp`).
**Unity versions**: 2017.4 - 2022.3. Their il2cpp is a .NET program the container's (or, with
`--on-device`, the handheld's) .NET runs; from 2021.2 the class libraries are per platform and the
build takes `unityaot-linux`, as Unity's Android builds do, and il2cpp builds the C++ with Bee,
whose native backend pmunity stands in for (below, "Unity 2021.2+: Bee"). From 2023.1 (Unity 6 included) il2cpp
is a natively compiled program for the editor's own system (the Windows installer's `il2cpp.exe`
is Windows x86-64 code, with no `il2cpp.dll`): not runnable here yet, so the route stops with a
message (a Linux x86-64 il2cpp under emulation would be the way).
`--link-xml FILE` gives UnityLinker a port's own link.xml (what the game reaches only by reflection,
e.g. the types XmlSerializer writes into saves); the 2017.x linker ignores the whole file when
anything, even a comment, comes before `<linker>`.
`--unity VERSION` builds with another release of the data's line (its IL2CPP and its player):
data of a 2017.4 release before 2017.4.16f1, which has no 64-bit ARM player, gets 2017.4.40f1 for
arm64-v8a by itself (below, Unity 2017.4).

**Where it runs:** on the porter's PC or Mac, in Docker; or, with `--on-device`, natively on the
player's aarch64 handheld inside a port's first-launch setup (below, "On the handheld"). il2cpp
and the C++ build of its output take minutes to an hour of CPU on a PC (a small game's generated
C++ is a few hundred MB; the build uses about 1 GB of disk and 100-800 MB of memory per compiler
process). On the PC, everything it writes is in `--home` and `<out>`: the tools are plain folders
bind-mounted into a stock `ubuntu:24.04` container (no image is built), so Docker's own disk only
holds that image and a slow external drive works for all of it.

**What the result is:** `libil2cpp.so` holds the game's code (its assemblies, Unity's packages,
the UnityEngine bindings and the .NET class libraries, compiled) and Unity's IL2CPP runtime. It
is that game's build, made by the person who owns the game, for their own device; like the
converted data, it is not something to publish or share: it is made from the game and from
Unity's IL2CPP (libil2cpp's sources, the class libraries), which stay under their owners' terms.
Unity's il2cpp, libil2cpp and the Android NDK stay where the porter downloads them (`--home`) and
are never shipped by this project. A port ships the tools and a setup that runs them on the
user's machine, never the result: for a 64-bit-only handheld, that machine is the handheld itself
(`--on-device`).

## What it fetches (once, into `--home`)

`--home` defaults to `$PMUNITY_IL2CPP_HOME`, else `~/.cache/pmunity/il2cpp`. Everything comes
from Unity's and Google's servers with HTTP range requests; no installer or archive is
downloaded whole or stored. Unity's files stay there, on the porter's machine; nothing of them,
or of what is built from them, is in this repository.

| what | from | read | on disk |
|---|---|---|---|
| `Editor/Data/il2cpp` (il2cpp, UnityLinker, libil2cpp and Boehm GC sources), `MonoBleedingEdge/etc` and the class libraries IL2CPP builds of the game's kind use (below: `MonoBleedingEdge/lib/mono/unityaot`; .NET 3.5 games `Mono/lib/mono/unity` or `2.0` and `Mono/etc`; .NET 4.x of 2017.x `MonoBleedingEdge/lib/mono/4.5`) | the Windows editor installer `UnitySetup64-<ver>.exe` (Unity's release API names it): a non-solid NSIS installer like the Android support ones, so `nsis.py` reads only these files' blocks (neighbouring blocks in one request) | 10-13 MB of 1.6-2.4 GB | 45-50 MB |
| the Android NDK's target files: unified sysroot (headers, per-API libraries), libc++, libgcc | Google's `android-ndk-<rev>-linux-x86_64.zip`: its central directory, then only those members | 34 MB (arm64-v8a), +24 MB (armeabi-v7a) of 820 MB | 145 MB / 200 MB both |
| the IL2CPP Android player: `libmain.so`, `libunity.so`, `unity default resources` (`pmunity player fetch --backend il2cpp`), the IL2CPP UnityEngine assemblies (`Variations/il2cpp/Managed`), `baselib.a` (2020.1+) | the Android Build Support installer | 9 MB | 30 MB per ABI |
| `<home>/root/usr`, `root/etc` (tools/il2cpp/root.sh): .NET 8, Mono 6.8, clang/lld 14, unpacked with `dpkg-deb -x` over a copy of the stock image's own `/usr` and `/etc`, then mounted over them | Ubuntu 24.04's archive | 180 MB | 0.85 GB |

NDK per Unity line (what that line's IL2CPP checks for): r13b for 2017.1 - 2018.2 (their Android
extension: "IL2CPP requires r13b"), r16b for 2018.3 - 2019.2, r19 for 2019.3 - 2020.3, r21d for
2021.x - 2022.1, r23b for 2022.2+ (with r21d and r23b the API 22 libraries too, which 2021.x and
2022.x link against). The NDK's compilers and binutils are x86-64
programs, so only its target files are fetched; `tools/il2cpp/ndk-clang.sh` is put where
il2cpp's Android toolchain runs them (`toolchains/llvm/prebuilt/linux-x86_64/bin/clang++`,
`<triple>-objcopy`, `<triple>/bin/objcopy`, `llvm-objcopy`, ...) and runs Ubuntu's clang 14 and
lld with the NDK's sysroot, libc++ and libgcc (the NDK's clang finds those next to itself, and its
clang++ takes libc++ without being told: the stand-in adds `-stdlib=libc++` to C++ when the
command has no `-stdlib`, as 2021.x's Bee build's commands have none; r23b has no libgcc, and its
links take compiler-rt's builtins and a static libunwind from the NDK clang's resource folder,
`lib64/clang/12.0.8`, as the NDK's clang does). The tools thus run natively on arm64
hosts (Apple Silicon) as on x86-64 ones. Differences from the NDK's own clang 8 (r19): lld instead
of GNU ld, no `-g` (debug info changes no code; `PMUNITY_IL2CPP_DEBUG_INFO=1` keeps it), and the
newer compiler's code generation.

## What it does

1. **Assemblies** (`stage_assemblies`): the game's own and Unity's package assemblies from
   `<Data>/Managed`; the IL2CPP player's UnityEngine assemblies (`Variations/il2cpp/Managed`)
   instead of the desktop ones, as `android-data` swaps in the Mono player's; the IL2CPP class
   libraries of the game's profile instead of the desktop Mono's, following the references from
   the game's assemblies. The profile is the one an IL2CPP Android build of that version picks
   (the editor's `ApiCompatibilityLevelToDotNetProfileArgument`; il2cpp's own profiles say where
   their assemblies are):

   | game | profile | class libraries (Editor/Data/...) |
   |---|---|---|
   | .NET 4.x, 2018.1+ | `unityaot` | `MonoBleedingEdge/lib/mono/unityaot` (the desktop's `unityjit`: same names and versions, the AOT profile) |
   | .NET 4.x, 2017.x | `net45` | `MonoBleedingEdge/lib/mono/4.5` |
   | .NET 3.5, API level ".NET 2.0 Subset" | `legacyunity` | `Mono/lib/mono/unity` (the old Mono 2.x fork's) |
   | .NET 3.5, API level ".NET 2.0" | `net20` | `Mono/lib/mono/2.0` |

   Which runtime: the version of the `mscorlib.dll` the game ships (2.0.0.0: .NET 3.5). Which API
   level: its types (the "unity" profile has Unity's `MonoTouchAOTHelper` and no console
   drivers); a desktop build ships the profile it ran on, so IL2CPP compiles that one.
   An assembly with a class library's name that the data lists among its script assemblies
   (`ScriptingAssemblies.json`, else MonoManager's `m_AssemblyNames`) is the game's own, e.g. a
   project's `Plugins/I18N.dll` and `I18N.CJK.dll` for code pages, which nothing references (they
   are loaded by name): it is staged from the game's copy and kept whole, as Unity's build treats a
   plugin assembly (without that, android-data stopped: "MonoManager lists assemblies missing").
2. **UnityLinker** (`--strip low`, the default) as an Android IL2CPP build with Managed Stripping
   Level "Low" runs it: `--rule-set=Conservative --use-editor-options --dotnetprofile=<profile>
   --dotnetruntime=il2cpp --platform=Android`. The class libraries are linked with Unity's own
   descriptors; every other assembly (the game's, the packages', the UnityEngine modules) is a
   root and kept whole: a desktop build carries no list of the types its scenes and assets use
   (the editor's TypesInScenes / editor data), so nothing the data might name is removed.
   2022.x's linker resolves, with `--use-editor-options`, only the assemblies it is given by
   `--allowed-assembly` (one per assembly of the build, as the editor passes them): every staged
   assembly is. `--strip none` compiles everything whole (3x the C++). 2017.x's UnityLinker is the older one
   (Mono's linker with Unity's steps and options), run as that editor runs it: `--api=NET_2_0[_Subset]|NET_4_6
   --core-action=link --i18n=none --descriptor-directory=<il2cpp/LinkerDescriptors>` (for `net45` a
   copy where each `*45.xml` replaces the file without 45), the game's and the packages'
   assemblies as `--include-assembly` roots (copied whole), the UnityEngine ones kept whole by a
   generated link.xml (`preserve="all"`) but linked: the linker then writes into them a stub for
   each member the game calls that the Android engine lacks (desktop-only API, e.g. Rewired's
   `Input.IsJoystickPreconfigured`; the stub throws, as Mono's call of a missing method does).
   Without the stubs 2017.4's il2cpp stops at such a call (`NullReferenceException` in
   `MethodBodyWriter.GetMethodCallExpression`).
3. **il2cpp** with the options of an Android build of that version: `--convert-to-cpp
   --compile-cpp --libil2cpp-static --emit-null-checks --enable-array-bounds-check
   --dotnetprofile=<profile> --platform=Android --architecture=ARM64|ARMv7 --configuration=Release`,
   `--incremental-g-c-time-slice=<ms>` when the game's `boot.config` has `gc-max-time-slice`
   (the incremental GC that line turns on in the player needs the write barriers in the code), and
   `--baselib-directory` (2020.1+). il2cpp's own builder compiles the generated C++ and libil2cpp
   and links `libil2cpp.so` against the NDK (API 21 for arm64; for armv7 19, 16 with NDK r13b and
   r16b; 22 for both from 2021.x; libc++ static, GNU libstdc++ with r13b and r16b), then strips it
   (from 2021.2 il2cpp only writes that build as a graph, which pmunity runs: below). The .NET Framework
   builds of the tools (2017.4 - 2019.4) run on Mono, as in Unity's
   Linux and macOS editors (UnityLinker uses .NET Framework-only types); the .NET Core / .NET
   builds (2020.1+) on .NET 8 with roll-forward.
4. **Checks**: `global-metadata.dat`'s header (magic `0xFAB11BAF`, the metadata version), and that
   every `il2cpp_*` function the player's `libunity.so` looks up by name is exported by the new
   `libil2cpp.so` (`il2cpp.json`: `player_api`).

Output (`<out>`): `lib/<abi>/libil2cpp.so`; `Data/Managed/Metadata/global-metadata.dat`,
`Data/Managed/Resources/*.dat`, `Data/Managed/etc/` (Mono's config files, which IL2CPP builds
ship); `il2cpp.json`. The build files are in `<out>/.work` (`--keep-cpp` keeps the C++ and the
object cache; logs: `unitylinker.log`, `il2cpp.log`, `cc.log` with every compiler command,
`build.log` with the Bee graph's commands that printed something, 2021.2+).

### Unity 2021.2+: Bee

From 2021.2 il2cpp's C++ build is Bee, Unity's build system: il2cpp's build program (inside
il2cpp.dll) writes the build as a graph, `<cache>/buildstate/bee.dag.json`, and `bee_backend`
runs it. That backend is a native program, and the editor installers carry it only for their own
system (the Windows one: `il2cpp/build/deploy/bee_backend/win-x64/bee_backend.exe`); il2cpp on
Linux runs `bee_backend/linux-x64/bee_backend`. As for the NDK's compilers, pmunity puts a
stand-in there (convert/pmunity/il2cpp_bee.py, a POSIX sh script, written into the fetched
`Editor/Data` like ndk-clang.sh into the NDK):

- without `--dagfilejson` (no graph yet) it exits 4, Bee's way of asking for the build program;
  Bee runs it, which writes the graph, and calls the backend again with `--dagfilejson`;
- then it notes its arguments (the target, `FinalProgram`) and exits 0: il2cpp ends with "Build
  succeeded with 0 successful nodes";
- pmunity runs the graph: the nodes the target needs (both of Bee's dependency kinds,
  `ToBuildDependencies` before a node and the `ToUseDependencies` of those), the commands
  (`RunShellCommand`: `/bin/sh -c` in the graph's folder with the node's environment) at most
  `--jobs` at a time, in one container kept running for them (`docker exec`), or natively with
  `--on-device`; the `WriteTextFile` nodes (response files, "lump" files that `#include` several
  libil2cpp sources; 2022.x: `WriteFile`, the bytes in the graph's `bee.dag.payloads`) and
  `CopyFiles` nodes directly; a node whose input another node writes waits for it. A command whose record in
  `<cache>/buildstate/pmunity-done.jsonl` (its command line, environment, and its inputs' contents:
  il2cpp writes all the C++ again when it converts again, mostly unchanged) still matches and whose outputs exist is not run again, so an interrupted build continues,
  as with il2cpp's own object cache in 2017.4 - 2020.3. The first failing command stops the build
  (no new commands start) with its output and command line.

il2cpp also hands the build program the way to start il2cpp itself: `deploy/il2cpp` (the editor's
apphost) when it exists, else the .NET in the editor's `il2cpp-deps`, which the installers do not
have ("Run build.pl to populate il2cpp-deps"): pmunity writes a `deploy/il2cpp` that runs
il2cpp.dll on the .NET in `PATH` (in this build mode nothing runs it).

What the graph does is what Unity's Android build does with that NDK: about 600 compiles (the
generated C++, libil2cpp in lumps of a few files, the Boehm GC), one link with a response file
(`-fuse-ld=bfd`, which the stand-in turns into lld, `-static-libstdc++`, `baselib.a`, `-llog`),
`objcopy --strip-all` / `--only-keep-debug` / `--add-gnu-debuglink`, and copies of
`libil2cpp.so`, `libil2cpp.dbg.so`, `libil2cpp.sym.so` and `Data/` into `lib/<abi>/`.

`android-data --il2cpp <out>` then packs the data as for Mono, but with IL2CPP's `Managed/`
(Metadata, Resources, etc) instead of the assemblies and Mono's dllmap config, and checks the
assembly list against what IL2CPP compiled. The player package (`--pkg`) is the IL2CPP one
(`<home>/<ver>/player-<abi>`). `libil2cpp.so` goes into the runtime's `lib/` next to
`libmain.so` and `libunity.so` of the IL2CPP player; libunity loads it, copies
`assets/bin/Data/Managed` out of the APK into `<external files>/il2cpp` (`data/external/il2cpp`
under the runtime's root) and runs IL2CPP from there. Two packing rules follow from that copy:
- **one APK, no OBB** (`--pack all` with `--il2cpp`, when the data stays under 2 GiB, as Unity's
  builds without "split application binary"): libunity 2020.3 lists the APK's
  `assets/bin/Data/Managed` to copy it, and with an OBB mounted that listing came back empty
  (nothing copied, then "Could not open .../global-metadata.dat"). Data over 2 GiB still gets an
  OBB, with that problem (untested);
- **`buildDate` in boot.config** (Unity 2020.x; a value from the metadata's hash): libunity keeps
  it in `il2cpp/unity.ver` and copies the data again only when it changes; without it the copy is
  redone at every start.

### Files the game reads (StreamingAssets, the data folder)

A desktop game that reads StreamingAssets with System.IO (localization XML, JSON, SQLite, level
files) needs them beside the APK and its reads pointed there (android-data.md, section 6): android-data
`--streaming-assets both|loose` puts the folder there, asmpatch's `streaming-assets` /
`streaming-assets-all` patch points the reads at it, and `--data-path` does the same for
`Application.dataPath`. Under IL2CPP the patch must be in the code IL2CPP compiles:

```
python -m pmunity il2cpp <Game>_Data out/il2cpp --asmpatch "dotnet asmpatch.dll --only=" --streaming-assets both
python -m pmunity android-data --src work/data --pkg <IL2CPP player> --il2cpp out/il2cpp --streaming-assets both \
    --out work/android --pack all
```

(`--only=` runs no patch but the opt-in ones the options name; without it asmpatch's default
patches run too.) `il2cpp.json` keeps `streaming_assets` and `data_path`, and android-data refuses
to pack a build whose compiled reads look for files its options leave out (built with `both` or
`loose`, packed with another mode; built with `--data-path`, packed without it). The il2cpp-arm64
port route does both from port.toml's `android_data.streaming_assets` and `data_path`.

## Android builds with Mono (`pmunity apk --il2cpp`)

A game owned as an Android build made with Mono has only Unity's armeabi-v7a player, which a
64-bit-only firmware cannot run. Its `assets/bin/Data/Managed/*.dll` are the assemblies IL2CPP
compiles (already the Android ones, with the game's Android code paths), and its data is already
Android data, so the same build applies without a conversion:

```
python -m pmunity apk <game>.apk out/ --target arm64 --il2cpp [--home DIR] [--jobs N]   # PC: Docker
python -m pmunity apk <game>.apk out/ --target arm64 --il2cpp --on-device --home DIR    # on the handheld
```

`--il2cpp` acts only when the target needs arm64-v8a and the build is Mono with only armeabi-v7a
(`--target armhf,arm64 --il2cpp` on a system with a 32-bit userland keeps the build's own player).
It runs `pmunity il2cpp` on the APK's assemblies and `boot.config` (the version from its data:
`globalgamemanagers` or `data.unity3d`) into `out/.il2cpp`, which a rerun continues, then writes the
arm64 layout:

- `lib/`: `libmain.so` and `libunity.so` of Unity's arm64 IL2CPP player of the build's version
  (`<home>/<version>/player-arm64-v8a`, fetched by the build), and the new `libil2cpp.so`. The
  build's own native plugins are 32-bit and left out (calls into them fail; the dry run's notes
  list them);
- `base.apk`: the APK without `lib/`, without `assets/bin/Data/Managed/` (the assemblies) and
  without its signature, with IL2CPP's `Managed/` (Metadata, Resources, etc up to 2020.x) stored
  and aligned, and libunity's re-extraction stamp from the metadata as android-data writes it
  (2020.x `buildDate` in boot.config, 2021.x+ `unity_app_guid`);
- `android_package.json`: `abi` arm64-v8a, `backend` il2cpp, `il2cpp_metadata`, and `il2cpp`
  (`from: mono`, the IL2CPP release, NDK, size, build time); `source` stays `apk` (the build's
  own frame pacing, Swappy as built).

Refused with the reason: versions outside 2017.4 - 2022.3, a 2017.4 release before 2017.4.16f1
(no arm64 player of it, and an Android build's data is not moved to another release here), an
IL2CPP build with only armeabi-v7a (no assemblies left to compile). The apk port route offers it
with `player.il2cpp = true` (porting.md, apk.md).

Run so far (PC route, then the native arm64 rig), two free itch.io Mono APKs whose data is a
`data.unity3d`:

| game | Unity | assemblies | build | `libil2cpp.so`, metadata | base.apk | result |
|---|---|---|---|---|---|---|
| card battler, GLES2, Unity Services config | 2022.3.23f1 | 109 (24 of the game) | 31 min (Bee graph, 607 commands; incremental GC) | 53.1 MiB, 29 | 18.7 MiB (from 28.9) | title at the build's 30 fps; pointer taps go into its first battle's tutorial |
| 2D, portrait, Vulkan + GLES3 | 2019.4.18f1 | 89 (8 of the game) | 15 min | 30.0 MiB, 24 | 18.3 MiB (from 28.4) | title (~140 fps in the rig); `UA_POINTER=touch` taps PLAY and a run starts |

Neither logged an exception. The card battler's apk port (`player.il2cpp = true`) ran its setup on a
64-bit-only stand-in (1 GB + 1 GB swap): `pmunity apk --il2cpp --on-device` in 38 minutes with
the downloads (UnityLinker 0.4 min, peak 549 MB; the conversion 8.6 min, 474 MiB of C++, peak 989
MB; 607 graph commands with 2 jobs in 11.6 min, peak 573 MB), and the install reached the same
battle. Not tried yet: a Mono APK whose data is in an OBB (with an OBB mounted
libunity 2020.3 listed no `Managed/` to copy out, above), split APKs and App Bundles with Mono,
2017.4 and 2018.x Mono APKs, a game with native plugins of its own (left out, as said).

## On the handheld (`--on-device`)

A port cannot ship `libil2cpp.so` (above), so on a 64-bit-only firmware the port's first-launch
setup builds it on the handheld itself, with the same tools as the PC route but without Docker:

```
python -m pmunity il2cpp fetch <version> --on-device --home <dir>        # downloads only (Wi-Fi)
python -m pmunity il2cpp <Game>_Data <out> --on-device --home <dir>      # the build (resumable)
```

`--on-device` runs everything natively on the aarch64 Linux machine it is started on (convert/
pmunity/il2cpp_device.py). It needs no root, installs nothing, never touches the firmware's
`/usr` and works with the setup's own Python (the port's `python.tar.gz`; urllib, tarfile and lzma
only).

### The tools

| what | from | download | on disk |
|---|---|---|---|
| glibc 2.36 (its loader runs every tool), libstdc++, zlib | Debian 12 arm64 packages | 3 MB | 6 MB |
| clang 14, lld 14, llvm-objcopy / -ar / -nm, clang's headers, and what libLLVM loads (libxml2, ICU, Z3, libedit, libffi, ...) | Debian 12 arm64 packages | 59 MB | 236 MB |
| Mono 6.8 and the class libraries UnityLinker.exe (and, if asked, il2cpp.exe) load: Unity 2017.4 - 2019.4 | Debian 12 packages | 7 MB (2019.x: +24 MB, the netstandard facades and System.Web.Extensions) | 20 MB |
| .NET 8 runtime (Unity 2020.1+: il2cpp / UnityLinker are .NET builds; 2017.4 - 2019.4: il2cpp.exe, below), libssl | Microsoft's `dotnet-runtime-8.0.x-linux-arm64.tar.gz`, Debian 12 | 32 MB | 76 MB |
| Unity's il2cpp parts, the NDK's target files, the IL2CPP player | as on the PC (above, range reads) | 55-60 MB | 150-370 MB |

Every Debian and .NET file is pinned by sha256 in `convert/pmunity/il2cpp_device_lock.py`
(`tools/il2cpp/device_lock.py` writes it from Debian's index and Microsoft's release metadata).
The packages come from `deb.debian.org`, else from `snapshot.debian.org` (which keeps every
version Debian has published, so old pins keep working after Debian replaces a package). Only the
groups a build needs are fetched: a 2018.4 game takes base + LLVM + Mono + .NET, 100 MB (68 MB
with `PMUNITY_IL2CPP_NET471=mono`), unpacked in about 2.5 minutes on a fast connection; a package
once unpacked is not fetched again after an interruption.
Firmwares keep their CA certificates in different places: the fetch looks in the usual ones; on a
system with none at all it downloads the pinned files without checking the server (the sha256
check still applies). Unity's and Google's range reads need the system's certificates.

The folder (`--toolchain`, default `<home>/device-arm64`) holds no symbolic links and needs no
executable bits on the programs, so it works on an exFAT or FAT32 card as on ext4: libraries
are stored under their sonames in `lib/`, Mono's class libraries in its GAC, and every program
runs through the folder's own loader (`lib/ld-linux-aarch64.so.1 --library-path lib <program>`),
so the firmware's libraries (often an older glibc) never matter. Only the loader and the small
scripts in `bin/` are executed directly (an exFAT card's mount gives every file the exec bit;
PortMaster needs that anyway). Programs that find their own files through `/proc/self/exe`
(which, run this way, names the loader) are given them: clang `-no-canonical-prefixes` (its
headers and `ld.lld` from argv[0]), Mono a copy of the loader named `mono/bin/mono` (Mono takes
its prefix from that path), .NET a copy named `dotnet/dotnet` (the muxer insists on its name and
finds `host/fxr` next to itself; the real one is `dotnet/dotnet-muxer`). `bin/` holds `clang`, `clang++`, `ld.lld`, `llvm-objcopy`, `llvm-strip`, `llvm-ar`,
`llvm-ranlib`, `llvm-nm`, `mono`, `dotnet`; tools/il2cpp/ndk-clang.sh runs them as on the PC
(`PMUNITY_LLVM=<toolchain>/bin`), so a port ships `setup/tools/il2cpp/ndk-clang.sh` next to
`setup/pmunity` (`pmunity port package` does).

### Fitting a 1 GB, 4 x Cortex-A53 handheld

- **Steps**: UnityLinker, il2cpp's conversion to C++ (`--convert-to-cpp` alone) and the C++ build
  (`--compile-cpp` alone, from the C++ on disk) run as three processes, each skipped when an
  earlier run finished it with the same inputs (`<out>/.work/.done-*`). A killed build continues:
  il2cpp's object cache (`.work/cache`) keeps every compiled file, so a resumed build compiles only
  what is missing. Nothing is lost but the file being compiled.
- **il2cpp.exe on .NET 8** (Unity 2017.4 - 2019.4; `PMUNITY_IL2CPP_NET471=mono` keeps Mono, as on
  the PC): the generated C++ is the same up to how 23 of 240 files write floating-point literals
  (`3.1415926535897931` vs `3.141592653589793`: the same values; the objects compiled from them
  are byte-identical), with a sixth of Mono's CPU time and less memory (2018.4 game: 0.4 vs 2.6
  min, 469 vs 535 MB). If it fails, the conversion runs again on Mono. UnityLinker.exe stays on
  Mono (it uses .NET Framework-only types).
- **Memory**: UnityLinker and the conversion run with their CPU affinity set to one CPU (the 2017.4 -
  2019.4 tools size their thread pools by the CPUs they may use); .NET with
  `DOTNET_GCConserveMemory=9` (2020.3 conversion: 766 -> 666 MB). Under a cgroup memory limit (a
  container standing in for a handheld) .NET caps its heap at 75% of the limit, 750 MB of 1 GB,
  which 2021.3's il2cpp exceeded converting a URP game (`OutOfMemoryException`); there the cap is
  the limit plus the cgroup's swap, less 64 MB (`DOTNET_GCHeapHardLimit`), as a handheld's setup,
  which has no such limit, may use all memory and swap. Compiler jobs: one per 200 MB of
  `MemAvailable` beyond 250 MB, at most one per CPU (`PMUNITY_IL2CPP_JOB_MB`,
  `PMUNITY_IL2CPP_RESERVE_MB`; `--jobs` sets it). The compiler wrappers hold job slots (POSIX sh,
  `mkdir` locks; a killed run's slots are taken over), so however many compilers il2cpp starts,
  only that many run; a further one starts only while `MemAvailable` stays above a job's size, and
  a source file over 4 MB (`PMUNITY_IL2CPP_BIG_KB`: IL2CPP's metadata tables) compiles alone.
  AArch64 compiles get `-mno-outline`: LLVM's machine outliner keeps the machine code of every
  function of a file until the end, although at `-Os` it outlines nothing; without it the objects
  are byte-identical and `Il2CppAttributes.cpp`'s compiler peak (14.7 MB, 15,000 functions)
  falls from 725 MB to 270-320 MB (at 450 MB it was killed with the outliner, and compiled
  without it). The largest compile is then `Il2CppTypeDefinitions.cpp`, 330-370 MB.
  A compiler killed by the OOM killer is noted in `.work/killed.log`; with more than one job the
  build then goes on once with one job at a time.
- **Progress**: a line every 30 seconds (PortMaster's patcher shows the setup's output): MB of C++
  written while converting, files compiled of the total with an estimate of the time left while
  compiling (from the size of the files started so far). `il2cpp.json` gets `on_device`: jobs,
  CPUs, the peak memory and CPU seconds of each step. `PMUNITY_IL2CPP_CC_WRAPPER=<command>` runs
  every compiler under that command (e.g. a timer).

### Measured (Unity 2018.4 game, the 2D pixel-art one of the table below)

In an arm64 Docker container limited like the handheld (`--cpus=4 --cpuset-cpus=0-3
--memory=1g --memory-swap=1g`, no swap) on an Apple M3, a Debian 11 userland (glibc 2.31) standing
in for the firmware, the setup's Python 3.12. The tools and build files were on Docker Desktop's
file sharing from a slow USB drive, which made every step wait on I/O (wall time 3-10x the CPU
time): the CPU time is the figure to scale. Nothing was killed by the OOM killer; killed by hand
while compiling, the build went on with the objects it had.

| step | wall (container) | CPU (M3) | peak memory | Cortex-A53 1.5 GHz, 1 GB (estimate) | Snapdragon 865, 8 GB (estimate) |
|---|---|---|---|---|---|
| fetch the tools (100 MB; 68 MB without .NET) | 2.5 min | - | 170 MB | 3-6 min (Wi-Fi, xz) | 1-2 min |
| UnityLinker (Mono) | 0.1-0.4 min | 0.1 min | 300 MB | 2-3 min | < 1 min |
| conversion to C++, il2cpp.exe on .NET 8 (the default) | 4.1-6.5 min | 0.4-0.5 min | 468-488 MB | 8-12 min | 1-2 min |
| (the same on Mono) | 9.5-13.6 min | 2.6 min | 535 MB | 40-55 min | 6-7 min |
| compile 677 files, link, strip (3 jobs) | 26.4 min | 7.0 min (6.0 in the compilers; 12.5 with il2cpp.exe on Mono on a busier machine) | 422 MB; largest compile 372 MB (`Il2CppTypeDefinitions.cpp`), lld 412 MB resident with its mapped inputs | 1-1.3 h with 2 jobs, 2-2.5 h with 1 | 3-6 min with 8 jobs |
| all | 32 min | 7.6 min | 821 MB in the container's cgroup, with its page cache; OOM events: none | **1.5 h (2 jobs) - 2.5 h (1 job)** | **5-10 min** |

The estimates scale the CPU times: a 1.5 GHz Cortex-A53 core does about 1/15 - 1/20 of an M3
core's work at compiling (single-core Geekbench 6: ~160-200 against ~3,000), a Snapdragon 865's
Cortex-A77 cores about 1/2.4 (its four A55 cores add little). The job count is what
`MemAvailable` allows: two from 650 MB available (a 1 GB handheld with little else running),
one below that, more with 2 GB and up. A source file over 4 MB compiles alone; the
largest ones take 3-11 s of CPU here, 1-4 minutes on an A53. The library is the PC route's in
size (30,633,664 bytes), exports (all 194 `il2cpp_*` the player looks up) and metadata (identical
`Data/Managed`); its bytes differ in place (same section sizes, as from objects linked in another
order: il2cpp names its cached objects by hashes of their compile commands, which hold the
output folder), while Debian's and Ubuntu's clang 14.0.6 make the same objects but for their name
in the producer string.

Disk: the tools 330 MB (255 MB without .NET), Unity's parts for 2018.4 arm64 about 150 MB, the
build files up to 650 MB (C++ 300 MB, objects 186 MB, the unstripped library (64 MB) and its
symbols (36 MB) until the end): about 1.1 GB at the peak, then `lib/arm64-v8a/libil2cpp.so` (30 MB) and
`Data/Managed` (5 MB) once `<home>` and `<out>/.work` are removed. The tools also run from an
exFAT or FAT32 card (tested with loop-mounted images: fetch, compile, lld, strip, Mono, .NET).

### When memory runs out

The kernel's OOM killer stops the process with the most memory:
- **a compiler**: the wrapper notes it in `.work/killed.log`; with more than one job the build goes
  on once with one job at a time (the objects made so far are kept). With one job, il2cpp fails
  with "N compiler(s) were killed by a signal (137) ...: out of memory, most likely";
- **the conversion or UnityLinker** (Mono or .NET, 470-590 MB; .NET 8 for a 2020.3 game 666 MB):
  "... failed (exit -9) (killed: out of memory?)". The conversion starts over when run again.

Running the setup again continues where it stopped, so a build that ran out of memory once
because something else was using it usually gets through the second time. It needs about 500 MB
available for the conversion (700 MB for a 2020.x game) and 380 MB for the largest compile: on a
1 GB handheld, quit what can be quit; zram swap (which some firmwares have) helps. Below that
the same step fails each time; the way out is swap on the card (`mkswap` / `swapon` need root,
which this tool never asks for) or the PC route.

### In a port's setup

```
pm il2cpp fetch "$VERSION" --on-device --home "$W/il2cpp-home"     # with Wi-Fi: about 155 MB
pm il2cpp "$D" "$W/il2cpp" --on-device --home "$W/il2cpp-home"     # 1.5 - 2.5 hours on an A53 handheld
pm android-data --src ... --pkg "$W/il2cpp-home/$VERSION/player-arm64-v8a" --il2cpp "$W/il2cpp" ...
rm -rf "$W/il2cpp-home" "$W/il2cpp/.work"                          # the tools and build files, ~1 GB
```

A firmware with a 32-bit userland keeps the Mono route (armeabi-v7a player); the IL2CPP route is
for the 64-bit-only ones (no `/lib/ld-linux-armhf.so.3`).

So far run in the arm64 containers only: the 2018.4 game end to end (the same UnityLinker output
as the PC route but for each assembly's MVID; 677 objects), the 2019.4 and 2020.3 games of the
table below through UnityLinker and the conversion into compiling, and the 2021.3 game end to end
in its port's setup on a 64-bit-only stand-in (1 GB + 1 GB swap): UnityLinker 0.4 min (peak 548 MB),
the conversion 10.6 min (CPU 2.4 min, peak 987 MB: above .NET's 750 MB container cap, see
"Memory"), the Bee graph's 605 commands in 9 min with 3 jobs (CPU 6.3 min, peak 599 MB); the
whole setup 32 min, and the installed game reaches its main menu. Not yet on a handheld.

## AOT limits

IL2CPP compiles everything ahead of time; what Mono does at run time instead fails:

- **Reflection.Emit, DynamicMethod, `System.Linq.Expressions` compiled lambdas**: no JIT. The
  unityaot profile keeps the types; Emit throws `PlatformNotSupportedException`, expression trees
  fall back to the interpreter (slow, but they work). Serializers and DI containers that emit IL
  need their reflection-only mode.
- **Generic code never seen at compile time**: a generic method or type instantiated only through
  reflection (`MakeGenericType`, `MakeGenericMethod`, `Activator.CreateInstance(typeof(List<>)...)`),
  or a **generic virtual method** called on value-type arguments that no compiled code uses, has
  no code: `ExecutionEngineException: Attempting to call method ... for which no ahead of time
  (AOT) code was generated`. Reference-type instantiations share one compiled body and work.
  Desktop Mono games never had to care; a game that does this needs a patch (tools/asmpatch) or a
  `--extra-types-file` entry.
- **Stripping**: `--strip low` keeps every non-class-library assembly whole, but class library
  members reached only by reflection from strings (`Type.GetType("System.Xml.XmlDocument")`, a
  serializer's type names) can be removed; a game's `link.xml` (in its project's Assets, not in
  the build) is not available. `--strip none` avoids that.
- **Native plugins**: P/Invokes into a desktop library (`Plugins/x86_64/*.so`) need an Android
  ARM build of it, as on the Mono route.
- **Behaviour not covered by the IL2CPP class libraries**: a handful of desktop-only APIs
  (`System.Diagnostics.Process`, parts of `System.Web`, ...) throw on IL2CPP.

## Tested

Free Linux (Mono, .NET 4.x) builds of the test corpus, one per line; `pmunity convert` (shaders,
settings) and `android-data --il2cpp --pack all`, then the emulated rigs (`tools/emu`:
`EMU_ARCH=arm64` runs the aarch64 `ua_host` natively on an Apple-silicon Mac, armhf is QEMU
armv7; Mesa llvmpipe). Build times on a busy 8-core Mac with 4 compiler jobs.

| game | Unity | ABI | libil2cpp.so | metadata | build | result |
|---|---|---|---|---|---|---|
| 2D, TextMeshPro, 2D Animation | 2019.4.8f1 | arm64-v8a | 31.9 MiB | 24 | 10 min | Unity splash, title screen, animated (1125 frames in 140 s) |
| same | 2019.4.8f1 | armeabi-v7a | 25.6 MiB | 24 | 8 min (cached) | splash, title screen (QEMU: ~1 fps) once the loader routes `__gnu_Unwind_Find_exidx` (below) |
| 2D city builder, incremental GC | 2020.3.49f1 | arm64-v8a | 33.6 MiB | 27 | 8 min | title menu (40-50 fps in the rig) once packed as one APK with `buildDate` (above) |
| 2D pixel art, TextMeshPro | 2018.4.36f1 | arm64-v8a | 29.2 MiB | 24 | 28 min (NDK r16b, GNU libstdc++) | title menu, animated (~90 fps in the rig) |
| 2D action, .NET 3.5 (".NET 2.0 Subset"), Rewired, VP8 intro videos | 2017.4.40f1 | arm64-v8a | 37.6 MiB | 24 | 45 min (NDK r13b; `legacyunity`) | intro videos play, then the title menu (~45 fps in the rig); with `--asmpatch` rewired-android the pad opens the main menu; sound since the loader raises small thread stacks (below) |
| same | 2017.4.40f1 | armeabi-v7a | 29.6 MiB | 24 | 16 min (cached) | title menu (QEMU: ~2 fps), with sound |
| 2D platformer, URP, TextMeshPro, Newtonsoft.Json | 2021.3.16f1 | arm64-v8a | 53.2 MiB | 29 | 32 min (NDK r21d; Bee graph: 605 commands, 29 min of them while other containers held Docker's file sharing; about 1 min for the first 110 without them) | the first-launch name entry, then (with a name in PlayerPrefs) the main menu (~30 fps in the rig) |
| 2D, menus from localization XML in StreamingAssets read with System.IO, Steamworks.NET, I18N plugins | 2018.3.2f1 | arm64-v8a | 34.5 MiB | 24 | 46 min (NDK r16b; UnityLinker 3 min, il2cpp 42, the machine's Docker file sharing busy) | title menu with its localized text: `--asmpatch ... --streaming-assets both`, the folder beside the APK (android-data `--streaming-assets both`), ~20 fps in the rig. Steamworks.NET's callbacks throw `NotSupportedException` ("IL2CPP does not support marshaling delegates that point to generic methods"); the Steam stub fails its init first anyway |
| 2D strategy, URP, FMOD Studio, Steamworks, Burst, Input System | 2022.3.36f1 | arm64-v8a | 82.0 MiB | 31 | 20 min (NDK r23b; Bee graph: 780 commands in 12.5 min) | title menu, 60 fps in the rig; silent without an ARM FMOD (`DllNotFoundException: fmodstudio`, as on the Mono route), the Steam stub answers, Burst jobs run their managed code |

Every `il2cpp_*` function the players' `libunity.so` look up by name was exported (175 for
2017.4, 194 for 2018.4, 222 for 2019.4, 225 for 2020.3, 228 for 2021.3, 162 for 2022.3); the metadata versions match those of real IL2CPP builds
of these lines (a 2020.3 Android build's is 27, 2021.3's 29, 2022.3's 31). 2021.x and 2022.x
builds have no `Managed/etc` (nor have Unity's: Cat Cove Inn's, The Ump Show's APKs), and their
APKs get `assets/bin/Data/unity_app_guid` (android-data, above). The layout of the APK matches Unity's IL2CPP
builds: `assets/bin/Data/Managed/Metadata/global-metadata.dat`,
`Managed/Resources/mscorlib.dll-resources.dat`, the 18 `Managed/etc/mono` files (2020.3), no
assemblies; `libil2cpp.so` needs the same four libraries (`liblog`, `libm`, `libdl`, `libc`).
The two linkers differ: lld writes four PT_LOAD segments with 4 KiB alignment, Unity's
bfd-linked libraries two with 64 KiB.

What the runs showed:
- **C++ exceptions on armv7**: IL2CPP throws managed exceptions as C++ exceptions. The EHABI
  unwinder linked into `libil2cpp.so` finds a frame's unwind table through
  `__gnu_Unwind_Find_exidx`, which the loader resolved to glibc's own (it only knows glibc's
  modules), so the first exception a game's startup catches (above: a `CultureNotFoundException`)
  terminated the player. runtime/loader now gives Android code its own lookup, as it did for
  `dl_unwind_find_exidx`. arm64 unwinds with `.eh_frame` through the loader's `dl_iterate_phdr`
  and was not affected.
- **Locale**: IL2CPP reads the process locale; with `LANG` unset it is `C`, and its startup
  throws (and catches) `CultureNotFoundException: Culture name c is not supported` before falling
  back to the invariant culture. Harmless; a runtime that sets `LANG` would avoid it.
- **Pointer-driven menus**: clicking a menu entry with the pointer mode did not start the
  2018.4 game in one scripted try (its Mono build does); not looked into.
- **2017.4 arm64 sound** (fixed): the 2017.4.40 arm64 player's FMOD created its OpenSL ES player,
  started and stopped it at once, reported "FMOD failed to initialize the output device", and its
  null output failed too ("An error occured that wasn't supposed to"). Its mixer thread was never
  created: it asks for a stack smaller than glibc's aarch64 minimum (128 KB, from the 64 KB page
  size; bionic accepts anything from 16 KB), and glibc refuses that with EINVAL. The loader now
  raises a smaller size to glibc's minimum (runtime/loader/hybris_compat.c); the OpenSL player
  then plays until the game quits. Not yet tried on a handheld.
- **Stack traces** (2017.4): with the SymbolMap written from the symbol table, Unity's logged
  stack traces name the same frames as the game's Mono build; without it they had extra frames
  named after neighbouring methods.
- **Rewired** (`--asmpatch "... --only=rewired-android"`): the patched Assembly-CSharp compiled in,
  Rewired takes the Android path (no libudev lookups) and the pad's A skips the intro videos and
  opens the main menu.

## Not tested yet

- `--streaming-assets loose` and `--data-path` with IL2CPP (the same patches as `both`, by the same
  path; only `both` has run);
- Unity 2021.1 - 2021.2 and 2022.1 (NDK r21d, like 2021.3; 2021.1 has no Bee yet if its editor
  installer has no `bee_backend` folder: then il2cpp's own builder runs, as for 2020.3), armv7
  with 2021.x / 2022.x; 2017.1 - 2017.3 and 2018.1 - 2018.2 (NDK r13b; no 64-bit ARM player; 2018.3
  has run, above);
  2017.4 .NET 4.x games (`net45`: none among the free 2017.4 builds surveyed, all were .NET 3.5),
  .NET 3.5 games of 2018.x (`legacyunity` / `net20` with the newer UnityLinker), the ".NET 2.0"
  (`net20`) profile.
- IL2CPP with data of a 2017.4 release before 2017.4.16f1 on the 2017.4.40f1 player (Cuphead's
  case: its data, retargeted, reaches its title on the 2017.4.40f1 Mono armv7 player).
- Games with native plugins, Burst (`lib_burst_generated` is not rebuilt: Burst jobs run as their
  managed code), the Input System package, link.xml-dependent reflection.
- IL2CPP data over 2 GiB (APK + OBB, see above); 16 KiB-page kernels.
- (Since run on a handheld: the 2018.4, 2019.4 and 2020.3 games reached their titles at 59.4 - 59.6
  fps with the arm64 `ua_host` on an RG34XX-SP's 64-bit userland; compat/README.md, "On a handheld".)
- `--strip none` end to end (it built 1 GB of C++ for the 2019.4 game; stopped there).
- armv7 for 2018.4 and 2020.3.

## Notes per version

- 2017.4: `il2cpp/build/il2cpp.exe` and the older `UnityLinker.exe` (above), run on Mono. NDK
  r13b, the last with per-platform headers: il2cpp passes `--sysroot
  <ndk>/platforms/android-21/arch-arm64` (armv7: `android-16/arch-arm`), `-gcc-toolchain` and a
  target without the API level, and no `-D__ANDROID_API__` (the platform's `api-level.h` defines
  it), GNU libstdc++; il2cpp takes no `--jobs`. Its compile flags end in `-fPIE` after `-fPIC`
  (as for an executable): the objects then reference GNU libstdc++'s template statics directly,
  which the NDK's GNU ld accepted in a shared library and lld does not, so ndk-clang.sh drops a
  `-fPIE` that comes with `-fPIC`. It links with a map file and then runs MapFileParser on it for
  `Data/SymbolMap-<arch>` (the editor passes its prebuilt one; without `--map-file-parser` il2cpp
  builds it from source with the host's GCC): `--map-file-parser=/bin/true`, and `pmunity il2cpp`
  writes `Data/Managed/SymbolMap-ARM64` / `-ARMv7` itself from the unstripped library's symbol
  table (the same records: each code symbol's offset and length). libil2cpp names the methods of
  managed stack frames with it; without it, it falls back to the method pointer table, which
  gave frames in native code a neighbouring method's name in Unity's logged stack traces.
  libil2cpp's armv7 64-bit atomics (`os/Unity/ExtendedAtomicOps-arm.h`, also 2018.4's) list the
  registers they bind operands to as clobbered, which clang 11+ refuses; the fetched copy gets
  the clobber lists of Unity's own 2019.4 file (`SOURCE_FIXES`: "cc", "memory"). For a .NET 3.5 game il2cpp copies the old Mono's
  `etc` (`Mono/etc/mono`: `config`, `2.0/machine.config`, ...) into the data, so that is fetched
  with its class libraries. A 64-bit ARM IL2CPP player exists from 2017.4.16f1; older data (Cuphead
  is 2017.4.9f1) is built with 2017.4.40f1's IL2CPP and run on its player (`--unity`; `il2cpp.json`
  keeps both versions): android-data retargets the data to the player's release
  ([android-data.md](android-data.md), "retargeted") and refuses a player other than the one
  `libil2cpp.so` was built for (`il2cpp_mismatch`).
- 2018.3: as 2018.4 (NDK r16b, `il2cpp.exe`), but like 2017.x it links with a map file and
  then builds MapFileParser from sources the installer does not carry (it stopped with
  `DirectoryNotFoundException: .../il2cpp/MapFileParser` after the whole compile): 2018.1 - 2018.3
  get `--map-file-parser=/bin/true` and the SymbolMap from the symbol table, as 2017.x.
- 2018.4: `il2cpp/build/il2cpp.exe` (no `deploy/`), NDK r16b (GNU binutils layout and GNU
  libstdc++ (`gnustl_static`): il2cpp passes `--sysroot`, `-gcc-toolchain` and a target without
  the API level, which ndk-clang.sh adds from `-D__ANDROID_API__`); armv7 there is API 16.
- 2019.4: `il2cpp/build/deploy/net471/` (il2cpp.exe, UnityLinker.exe), NDK r19; armv7 at API 19
  needs the NDK's `sysroot/usr/local/include` (libandroid_support's headers).
- 2020.3: `il2cpp/build/deploy/netcoreapp3.1/` (self-contained Windows .NET Core builds: only the
  application files are fetched, the .NET runtime it carries is left out by its `deps.json`),
  `baselib.a` from the player's `StaticLibs/<abi>`; il2cpp takes `--jobs`.
- 2021.3: `il2cpp/build/deploy/il2cpp.dll` (.NET 5/6 build), class libraries `unityaot-linux`,
  NDK r21d, API 22; the C++ build is Bee's graph (above, "Unity 2021.2+: Bee"): about 600 compiles
  (libil2cpp in lumps), `-fuse-ld=bfd` (lld here) with `-Wl,--wrap,sigaction`, objcopy for
  `libil2cpp.so` / `.dbg.so` / `.sym.so`; libunity re-extracts the IL2CPP data unless
  `assets/bin/Data/unity_app_guid` matches `il2cpp/unity.ver`.
- 2022.3: as 2021.3 with NDK r23b (no libgcc: compiler-rt and libunwind, above), `WriteFile`
  graph nodes with a payloads file, UnityLinker's `--allowed-assembly` list; libunity looks up
  fewer `il2cpp_*` functions by name (162).

## Debugging a build

`<out>/.work` keeps `unitylinker.log`, `il2cpp.log` and `cc.log` (every compiler and linker
command as ndk-clang.sh ran it); `--keep-cpp` keeps the generated C++ and the object cache, so
a rebuild only compiles what changed. `PMUNITY_IL2CPP_EXTRA_ARGS` appends il2cpp options, e.g.
`--additional-cpp=<file> --linker-flags=-Wl,--wrap=__cxa_throw` to log every throw (the file must
be under `<out>` or `--home`, which the container mounts). `libil2cpp.sym.so` (symbols only) is
in `.work`.
