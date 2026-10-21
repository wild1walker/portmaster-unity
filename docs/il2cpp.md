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
the build uses about 1 GB of disk and 100-500 MB of memory per compiler process), in Docker.
Everything it writes is in `--home` and `<out>`: the tools are plain folders bind-mounted into a
stock `ubuntu:24.04` container (no image is built), so Docker's own disk only holds that image
and a slow external drive works for all of it.

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
| `<home>/root/usr`, `root/etc` (tools/il2cpp/root.sh): .NET 8, Mono 6.8, clang/lld 14, unpacked with `dpkg-deb -x` over a copy of the stock image's own `/usr` and `/etc`, then mounted over them | Ubuntu 24.04's archive | 180 MB | 0.85 GB |

NDK per Unity line (what that line's il2cpp checks for): r16b for 2017.4 - 2019.2, r19 for 2019.3 -
2020.3, r21d for 2021.x - 2022.1, r23b for 2022.2+. The NDK's compilers and binutils are x86-64
programs, so only its target files are fetched; `tools/il2cpp/ndk-clang.sh` is put where
il2cpp's Android toolchain runs them (`toolchains/llvm/prebuilt/linux-x86_64/bin/clang++`,
`<triple>-objcopy`, ...) and runs Ubuntu's clang 14 and lld with the NDK's sysroot, libc++
and libgcc (the NDK's clang finds those next to itself). The tools thus run natively on arm64
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

Every `il2cpp_*` function the players' `libunity.so` look up by name was exported (194 for
2018.4, 222 for 2019.4, 225 for 2020.3); the metadata versions match those of real IL2CPP builds
of these lines (a 2020.3 Android build's is 27). The layout of the APK matches Unity's IL2CPP
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

## Not tested yet

- Unity 2021.x / 2022.x (NDK r21d / r23b: the NDK selection and `ndk_wanted` know them, r23b's
  compiler-rt builtins and libunwind in `lib64/clang/<ver>/lib/linux` are fetched but the link
  flags for them are not), 2017.4 (r16b, as 2018.4); .NET 3.5 games of 2017.4 / 2018.4 (their
  class libraries are not the unityaot profile).
- Games with native plugins, Burst (`lib_burst_generated` is not rebuilt: Burst jobs run as their
  managed code), the Input System package, link.xml-dependent reflection.
- IL2CPP data over 2 GiB (APK + OBB, see above); 16 KiB-page kernels.
- (Since run on a handheld: the 2018.4, 2019.4 and 2020.3 games reached their titles at 59.4 - 59.6
  fps with the arm64 `ua_host` on an RG34XX-SP's 64-bit userland; compat/README.md, "On a handheld".)
- `--strip none` end to end (it built 1 GB of C++ for the 2019.4 game; stopped there).
- armv7 for 2018.4 and 2020.3.

## Notes per version

- 2018.4: `il2cpp/build/il2cpp.exe` (no `deploy/`), NDK r16b (GNU binutils layout and GNU
  libstdc++ (`gnustl_static`): il2cpp passes `--sysroot`, `-gcc-toolchain` and a target without
  the API level, which ndk-clang.sh adds from `-D__ANDROID_API__`); armv7 there is API 16.
- 2019.4: `il2cpp/build/deploy/net471/` (il2cpp.exe, UnityLinker.exe), NDK r19; armv7 at API 19
  needs the NDK's `sysroot/usr/local/include` (libandroid_support's headers).
- 2020.3: `il2cpp/build/deploy/netcoreapp3.1/` (self-contained Windows .NET Core builds: only the
  application files are fetched, the .NET runtime it carries is left out by its `deps.json`),
  `baselib.a` from the player's `StaticLibs/<abi>`; il2cpp takes `--jobs`.

## Debugging a build

`<out>/.work` keeps `unitylinker.log`, `il2cpp.log` and `cc.log` (every compiler and linker
command as ndk-clang.sh ran it); `--keep-cpp` keeps the generated C++ and the object cache, so
a rebuild only compiles what changed. `PMUNITY_IL2CPP_EXTRA_ARGS` appends il2cpp options, e.g.
`--additional-cpp=<file> --linker-flags=-Wl,--wrap=__cxa_throw` to log every throw (the file must
be under `<out>` or `--home`, which the container mounts). `libil2cpp.sym.so` (symbols only) is
in `.work`.
