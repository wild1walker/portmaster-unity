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

**Where it runs:** on the porter's PC or Mac, never on the handheld. il2cpp and the C++ build of
its output take minutes to an hour of CPU (a small game's generated C++ is a few hundred MB;
the build uses 1-2 GB of disk and about 300 MB of memory per compiler process), in Docker.

**What the result is:** `libil2cpp.so` holds the game's code (its assemblies, Unity's packages,
the UnityEngine bindings and the .NET class libraries, compiled) and Unity's IL2CPP runtime. It
is that game's build, made by the person who owns the game, for their own device; like the
converted data, it is not something to publish or share: it is made from the game and from
Unity's IL2CPP (libil2cpp's sources, the class libraries), which stay under their owners' terms.
Unity's il2cpp, libil2cpp and the Android NDK stay where the porter downloads them (`--home`) and
are never shipped by this project. A port ships the tools and a setup that runs them on the
user's machine, never the result.

## What it fetches (once, into `--home`)

`--home` defaults to `$PMUNITY_IL2CPP_HOME`, else `~/.cache/pmunity/il2cpp`. Everything comes
from Unity's and Google's servers with HTTP range requests; no installer or archive is
downloaded whole or stored. Unity's files stay there, on the porter's machine; nothing of them,
or of what is built from them, is in this repository.

| what | from | read | on disk |
|---|---|---|---|
| `Editor/Data/il2cpp` (il2cpp, UnityLinker, libil2cpp and Boehm GC sources) and `MonoBleedingEdge/lib/mono/unityaot` + `etc` (the class libraries IL2CPP builds use) | the Windows editor installer `UnitySetup64-<ver>.exe` (Unity's release API names it): a non-solid NSIS installer like the Android support ones, so `nsis.py` reads only these files' blocks (neighbouring blocks in one request) | 10-13 MB of 1.6-2.4 GB | 45-50 MB |
| the Android NDK's target files: unified sysroot (headers, per-API libraries), libc++, libgcc | Google's `android-ndk-<rev>-linux-x86_64.zip`: its central directory, then only those members | 34 MB (arm64-v8a), +24 MB (armeabi-v7a) of 820 MB | 145 MB / 200 MB both |
| the IL2CPP Android player: `libmain.so`, `libunity.so`, `unity default resources` (`pmunity player fetch --backend il2cpp`), the IL2CPP UnityEngine assemblies (`Variations/il2cpp/Managed`), `baselib.a` (2020.1+) | the Android Build Support installer | 9 MB | 30 MB per ABI |
| the `pmunity-il2cpp` Docker image (tools/il2cpp/Dockerfile): Ubuntu 24.04 with .NET 8, Mono 6.8, clang/lld 14 | Ubuntu's archive | | 1.2 GB |

NDK per Unity line (what that line's il2cpp checks for): r16b for 2017.4 - 2019.2, r19 for 2019.3 -
2020.3, r21d for 2021.x - 2022.1, r23b for 2022.2+. The NDK's compilers and binutils are x86-64
programs, so only its target files are fetched; `tools/il2cpp/ndk-clang.sh` is put where
il2cpp's Android toolchain runs them (`toolchains/llvm/prebuilt/linux-x86_64/bin/clang++`,
`<triple>-objcopy`, ...) and runs the image's clang 14 and lld with the NDK's sysroot, libc++
and libgcc (the NDK's clang finds those next to itself). The image thus runs natively on arm64
hosts (Apple Silicon) as on x86-64 ones. Differences from the NDK's own clang 8 (r19): lld instead
of GNU ld, no `-g` (debug info changes no code; `PMUNITY_IL2CPP_DEBUG_INFO=1` keeps it), and the
newer compiler's code generation.

## What it does

1. **Assemblies** (`stage_assemblies`): the game's own and Unity's package assemblies from
   `<Data>/Managed`; the IL2CPP player's UnityEngine assemblies (`Variations/il2cpp/Managed`)
   instead of the desktop ones, as `android-data` swaps in the Mono player's; the IL2CPP class
   libraries (`unityaot`) instead of the desktop Mono's (`unityjit`: same names and versions, the
   AOT profile), following the references from the game's assemblies.
2. **UnityLinker** (`--strip low`, the default) as an Android IL2CPP build with Managed Stripping
   Level "Low" runs it: `--rule-set=Conservative --use-editor-options --dotnetprofile=unityaot
   --dotnetruntime=il2cpp --platform=Android`. The class libraries are linked with Unity's own
   descriptors; every other assembly (the game's, the packages', the UnityEngine modules) is a
   root and kept whole: a desktop build carries no list of the types its scenes and assets use
   (the editor's TypesInScenes / editor data), so nothing the data might name is removed.
   `--strip none` compiles everything whole (3x the C++).
3. **il2cpp** with the options of an Android build of that version: `--convert-to-cpp
   --compile-cpp --libil2cpp-static --emit-null-checks --enable-array-bounds-check
   --dotnetprofile=unityaot --platform=Android --architecture=ARM64|ARMv7 --configuration=Release`,
   `--incremental-g-c-time-slice=<ms>` when the game's `boot.config` has `gc-max-time-slice`
   (the incremental GC that line turns on in the player needs the write barriers in the code), and
   `--baselib-directory` (2020.1+). il2cpp's own builder compiles the generated C++ and libil2cpp
   and links `libil2cpp.so` against the NDK (API 21 for arm64, 19 for armv7, libc++ static), then
   strips it. The .NET Framework builds of the tools (2018.4 - 2019.4) run on Mono, as in Unity's
   Linux and macOS editors (UnityLinker uses .NET Framework-only types); the .NET Core / .NET
   builds (2020.1+) on .NET 8 with roll-forward.
4. **Checks**: `global-metadata.dat`'s header (magic `0xFAB11BAF`, the metadata version), and that
   every `il2cpp_*` function the player's `libunity.so` looks up by name is exported by the new
   `libil2cpp.so` (`il2cpp.json`: `player_api`).

Output (`<out>`): `lib/<abi>/libil2cpp.so`; `Data/Managed/Metadata/global-metadata.dat`,
`Data/Managed/Resources/*.dat`, `Data/Managed/etc/` (Mono's config files, which IL2CPP builds
ship); `il2cpp.json`. The build files are in `<out>/.work` (`--keep-cpp` keeps the C++ and the
object cache; logs: `unitylinker.log`, `il2cpp.log`, `cc.log` with every compiler command).

`android-data --il2cpp <out>` then packs the data as for Mono, but with IL2CPP's `Managed/`
(Metadata, Resources, etc) instead of the assemblies and Mono's dllmap config, and checks the
assembly list against what IL2CPP compiled. The player package (`--pkg`) is the IL2CPP one
(`<home>/<ver>/player-<abi>`). `libil2cpp.so` goes into the runtime's `lib/` next to
`libmain.so` and `libunity.so` of the IL2CPP player; libunity loads it with `System.load`, then
copies `assets/bin/Data/Managed` out of the APK into `<files dir>/il2cpp` and runs from there.

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

| game (Unity) | backend | libil2cpp.so | metadata | result |
|---|---|---|---|---|

## Notes per version

- 2018.4: `il2cpp/build/il2cpp.exe` (no `deploy/`), NDK r16b (GNU binutils layout: il2cpp passes
  `--sysroot` and `-gcc-toolchain` itself; ndk-clang.sh maps them).
- 2019.4: `il2cpp/build/deploy/net471/` (il2cpp.exe, UnityLinker.exe), NDK r19.
- 2020.3: `il2cpp/build/deploy/netcoreapp3.1/` (self-contained Windows .NET Core builds: only the
  application files are fetched, the .NET runtime it carries is left out by its `deps.json`),
  `baselib.a` from the player's `StaticLibs/<abi>`.
