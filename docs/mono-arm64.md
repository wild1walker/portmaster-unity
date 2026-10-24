# Mono on the 64-bit player: a Mono game's own assemblies on Unity's arm64 IL2CPP player

Unity's 64-bit ARM Android players are IL2CPP players only ([il2cpp.md](il2cpp.md)): their
`libunity.so` runs managed code through `libil2cpp.so`, which an IL2CPP build compiles from the
game's assemblies. This route gives that player a different `libil2cpp.so`: a bridge
(`runtime/mono64/il2cpp_mono.c`) that implements the `il2cpp_*` functions libunity calls on Unity's
own Mono runtime, built for aarch64 Linux from Unity's Mono sources. The game's assemblies (its
desktop build's `Managed` folder, with the IL2CPP player's engine modules) are then JIT-compiled at
run time, as on the desktop: no IL2CPP conversion, no C++ compile (15 - 48 minutes on a PC, an hour
on a handheld, and the setup's .NET and compiler downloads), and the game's code runs on the same
runtime and class libraries as on its desktop build, not on IL2CPP's ahead-of-time ones.

The idea comes from another porter, who built Unity's Mono fork for Android arm64 and bridged the
IL2CPP API to the Mono API. This is our own implementation of it.

```
tools/mono64/build-mono.sh <unity version> <work dir>          # Unity's Mono runtime for aarch64 Linux (once per line)
runtime/mono64/build.sh                                        # the bridge -> runtime/mono64/out/arm64/libil2cpp_mono.so
python -m pmunity player fetch <ver> work/player64 --backend il2cpp --abi arm64-v8a --managed
python -m pmunity convert <Game>_Data work/data --config game.json
python -m pmunity android-data --src <data view> --pkg work/player64 --mono64 --out work/android --pack all
```

The device folder is the 64-bit route's ([arm64.md](arm64.md)): `ua_host64`, the APK and
`android_package.json` (`"scripting": "mono"`, `"abi": "arm64-v8a"`), and in `lib/` the IL2CPP
player's `libmain.so` and `libunity.so`, the bridge **as `libil2cpp.so`**, and Mono's
`libmonobdwgc-2.0.so`, `libMonoPosixHelper.so` (and `libmono-native.so` from Unity 2021.2). ua_host
(`runtime/host/mono64.c`) loads the bridge as a host (glibc) library when the package says
`scripting: mono` (or `UA_SCRIPTING=mono`), hands it the loader's `dlopen`/`dlsym`, and maps
libunity's `dlopen("libil2cpp.so")` to it.

**Unity versions**: games on Unity's MonoBleedingEdge (the .NET 4.x scripting runtime, the default
from 2018.3) of 2017.4 and later (the arm64 players start with 2017.4.16), with the
`unity-<line>-mbe` branch of Unity-Technologies/mono for the runtime; checked with 2019.4, 2020.3,
2021.3 and 6000.0. .NET 3.5 games cannot take this route: their desktop players run Unity's older
Mono fork (Mono 2.6.5, the `unity-<line>` branches), which has no 64-bit ARM code generator
(its `mono/mini` has `mini-arm.c`, 32-bit ARM, and no `mini-arm64.c`). Running
their assemblies on MonoBleedingEdge would mean other class libraries than the game's (4.x instead
of the 2.0 "unity" profile), so not the game as it is: Cuphead (2017.4, .NET 3.5) stays on the
IL2CPP route or the 32-bit player.

## Why it works (the feasibility study, 2026-10-23)

What libunity needs from `libil2cpp.so`, from Unity's IL2CPP arm64 players of 2017.4.40f1,
2019.4.8f1, 2020.3.49f1, 2021.3.16f1, 2022.3.62f3 and 6000.0.61f1 with their symbol files
(`pmunity player fetch <v> --backend il2cpp --abi arm64-v8a --symbols`):

- **The API**: libunity names 175 (2017.4), 222 (2019.4), 225 (2020.3), 228 (2021.3) and 234
  (2022.3, 6000.0) `il2cpp_*` functions and looks each up with `dlsym` into a global function
  pointer of the same name (a missing one is logged and its use fails); the union is 240 names,
  all implemented by the bridge. libunity checks that `<lib>/libil2cpp.so` exists before it
  dlopens it (without the file: "Failed to load Il2CPP.").
- **Handles are opaque.** libunity's IL2CPP scripting layer (`scripting_class_get_name`,
  `scripting_method_invoke`, ...: 73 - 531 small functions per version) only passes
  `Il2CppClass*`, `MethodInfo*`, `FieldInfo*`, `Il2CppType*`, `Il2CppImage*`, `PropertyInfo*` back to
  `il2cpp_*` functions. A scan of every libunity (each load whose base register holds the result of
  an `il2cpp_*` call or of a wrapper that only forwards one, followed through moves until the next
  call; 3,200 - 23,500 direct and 3,200 - 12,000 wrapped call sites per version) found one field
  libunity reads itself: the class's user-data word at `il2cpp_class_get_userdata_offset()`
  (2019.4+, `GetComponent`'s fast path). Unity's Mono has the same word
  (`MonoClass.unity_user_data`, `mono_class_get_userdata_offset`), so the handles are simply
  `MonoClass*`, `MonoMethod*`, `MonoClassField*`, `MonoType*`, `MonoImage*`, `MonoProperty*`. The
  other hits of the scan were branch targets reached from elsewhere (the scan does not follow
  branches) or Unity's own structures. The type enumeration (`IL2CPP_TYPE_*`, `MONO_TYPE_*`) is
  ECMA-335's in both.
- **Objects are read in place, with the same layout**: a 16-byte header ({klass, monitor} and
  {vtable, synchronisation}), instance fields at `il2cpp_field_get_offset` (header included in
  both), arrays {header, bounds, max_length, data at 32}, strings {header, int32 length, UTF-16
  characters at 20}; libunity computes array element addresses itself (base + 32 + index * size)
  and writes boxed values at +16.
- **Write barriers**: libunity stores every managed reference it keeps, on its own stack too,
  through `il2cpp_gc_wbarrier_set_field(obj, &slot, value)` (19,000 call sites in 2019.4: every
  `ScriptingObjectPtr` assignment): a store, plus Mono's barrier for heap objects when the
  collector is incremental. Both runtimes use Boehm, and both are configured not to scan the
  libraries' static data (IL2CPP's and Unity's Mono builds of bdwgc: `--enable-ignore-dynamic-loading
  --enable-dont-register-main-static-data`): libunity keeps references in GC handles and on stacks.
- **Internal calls**: libunity registers them (6,100 - 9,600) before `il2cpp_init`, with the same
  managed names Unity's Mono players register (`UnityEngine.Transform::get_position_Injected`, no
  signatures): the bridge queues them and passes them to `mono_add_internal_call` once Mono is up.
  The functions are those of the IL2CPP player's engine modules (`Variations/il2cpp/Managed`, IL
  for any runtime), so those modules ship instead of the desktop's; their C signatures are the
  ones Mono's JIT calls (AAPCS64: Mono's arm64 managed convention passes value types as C does).
- **Managed code is entered only through `il2cpp_runtime_invoke`** (`mono_runtime_invoke`): no
  method pointers are called directly. libunity never catches IL2CPP's C++ exception
  (`Il2CppExceptionWrapper` appears nowhere in it), so `il2cpp_raise_exception` is
  `mono_raise_exception`, as in Unity's Mono players.
- **The incremental collector**: IL2CPP compiles it in (`--incremental-g-c-time-slice`), so its
  libunity never reads `boot.config`'s `gc-max-time-slice`; a desktop player starts Mono with it.
  `android-data --mono64` copies the value into `android_package.json` (`gc_time_slice_ms`) and the
  bridge starts Boehm's incremental mode with it (`MONO_GC_PARAMS=incremental=<ms>`); then every
  `il2cpp_gc_wbarrier_set_field` also marks its slot, whatever object it names, as IL2CPP's does.
- **Threads**: `il2cpp_thread_attach` is `mono_thread_attach`; a thread whose first call into the
  runtime allocates or runs managed code is attached then (IL2CPP is lenient there).
- **Unity's own Mono API covers the rest**: the liveness calculation of
  `Resources.UnloadUnusedAssets` (`mono_unity_liveness_*`, the same shape as IL2CPP's), GC
  enable/disable/mode, the memory profiler's heap and class walks and snapshot, the TLS interface,
  the plugin-finding callback, type names in chunks: IL2CPP's API was made after Unity's use of Mono.

Where IL2CPP's semantics differ from the obvious Mono call, the bridge follows IL2CPP's: method
lookup by name searches the parents, image names are file names ("UnityEngine.CoreModule.dll"),
`il2cpp_domain_get_assemblies` lists every assembly of the build (the bridge loads each one in the
Managed folder at `il2cpp_init`, as an IL2CPP runtime has them all), the stack frame struct has the
size of the player's line (one field to 2019.4, two in 2020.3, five in 2021.3 and later), GC handles
are 32-bit to 2022.3 and pointer-sized from 2023.2 (`Il2CppGCHandle`; Mono's `*_v2` handles then,
which Unity's Mono has from 2021.2 on: a pointer handle given to a 2021.3 or 2022.3 libunity would be
cut to 32 bits).

## The runtime: Unity's Mono for aarch64 Linux

`tools/mono64/build-mono.sh` builds the runtime of a Unity line from
github.com/Unity-Technologies/mono, branch `unity-<line>-mbe` (`unity-6000.0-mbe` for 6000.0.x),
as Unity's `build.pl` builds the Linux players' (no class libraries, no BTLS, Boehm without
parallel marking), with `-O2`: a glibc library, loaded by ua_host itself rather than through the
Android loader, so its threads, signals and `dlopen` are the host's. It fetches the branch's
tarball without the class libraries and tests (45 MB on disk), the pinned bdwgc and libatomic_ops,
and for Mono 6 corefx's native code (libmono-native); the build runs in an arm64 container
(`ua-mono64-build`: ua-build-arm64 plus autotools, glibc 2.31) in about 5 minutes on 8 cores.
Results: `libmonobdwgc-2.0.so` 3.9 MB (2019.4, 2020.3), 4.2 MB (6000.0) stripped. The branch head
is used, not the exact commit of the game's release (Unity does not publish that mapping): the
game's `mscorlib.dll` must fit the runtime's internal calls, which change between lines, not
within one (each run checks that: Mono refuses a corlib of another version at start).

The game's class libraries are its own (`Managed/mscorlib.dll` ... of the desktop build), and so is
Mono's config (`MonoBleedingEdge/etc/mono/config`, whose Linux dllmaps fit a glibc runtime); the
class libraries' native helpers (`System.Native`, `MonoPosixHelper`) are mapped to the runtime's
folder. P/Invokes into Android libraries (plugins) go through the loader (Mono's `dlopen`
fallback); a game's Linux plugins are glibc libraries already.

## As a port (`--route mono-arm64`)

`pmunity port check` lists the route for a .NET 4.x Mono build of 2017.4 or later (not yet as the
recommended one); `pmunity port new <name> --data <game> --route mono-arm64` writes it. Its setup is
the android route's, with three changes: the player is the arm64 IL2CPP one with its engine modules
(`player fetch --backend il2cpp --abi arm64-v8a --managed`, about 15 MB read), the packing is
`android-data --mono64`, and the install takes the IL2CPP player's `libmain.so`/`libunity.so` plus
the release's `setup/mono64/<line>/` (the bridge as `libil2cpp.so`, `libmonobdwgc-2.0.so`,
`libMonoPosixHelper.so`, from Unity 2021.2 `libmono-native.so`). Nothing is compiled on the handheld.
`pmunity port package` builds that folder for each line of `game.unity_versions` (the package part
`mono64`: `runtime/mono64/build.sh`, `tools/mono64/build-mono.sh <line> <work>` in Docker;
`--mono64-work DIR` or `$PMUNITY_MONO64_WORK` for an existing work folder) and adds Mono's, bdwgc's
and corefx's licences (THIRD_PARTY.md). Tom City (2020.3.49f1) as such a port, rehearsed on a
64-bit-only stand-in (`REHEARSE_64ONLY=1`, 1 GB): the setup in 13 minutes (the player 30 s, the
conversion with its texture step 3.4 min, the packing 7.3 min), 324 MB installed, then the title in
the arm64 rig, Mono up 310 ms after `il2cpp_init` (2026-10-23).

## Results

| game | Unity | emulated rig (native arm64) | handheld | notes |
|---|---|---|---|---|
| buggish | 2019.4.8f1 | title, 75 - 170 fps (Mesa llvmpipe), Mono up 31 - 104 ms after `il2cpp_init`, 92 assemblies, 7,626 internal calls | title 59.5 - 59.7 fps (as the 32-bit and the IL2CPP builds: 59.4 - 59.6), resident 255 MB | the first game on the route (2026-10-24) |
| Tom City | 2020.3.49f1 | title, 210 - 350 fps, 100 assemblies, 8,117 internal calls | | runtime built with `tools/mono64/build-mono.sh` (2026-10-23) |
| Untitled Hook Game | 2021.3.16f1, URP | the first-launch name prompt at about 28 fps, as the IL2CPP build in the same rig (about 30); 114 assemblies, 8,410 internal calls; 32-bit GC handles from Mono 6's runtime | | the first Mono 6 line of the route (2026-10-23) |
| Hollow Knight (GOG, Linux) | 6000.0.61f1 | first launch's language screen at 60 fps; 116 assemblies (the port's asmpatch patches applied to the desktop assemblies), 9,644 internal calls; resident 1,543 - 1,573 MB, as the IL2CPP build's 1,552 - 1,575 MB in the same rig | **King's Pass 34.0 fps**, as the IL2CPP build's 34.4 and the 32-bit port's 35.2 (GPU-bound: below); Mono up 1.6 s after `il2cpp_init`; resident 720 - 731 MB, 47 - 62 MB available (IL2CPP 723 - 825 MB, 52 - 54 MB) | the hk64 folder's APK, OBB and saves with lib/ swapped (2026-10-24) |

### Hollow Knight on the handheld: the Mono route against IL2CPP and the 32-bit port

King's Pass with the bench plan of profiling.md (`log:find GameCameras; ...; shot`, 20 s
measured), RG34XX-SP, every run from a cool handheld (`devlock.py cool`), 2026-10-24; the 64-bit
runs in one folder (hk64's APK, OBB and save; `lib/` the IL2CPP build's, `lib-mono/` the Mono
route's: `UA_SCRIPTING`, `UA_LIBDIR`; this branch's ua_host), the 32-bit ones in the 32-bit port's
folder (main's ua_host of the same day). Pairs measured in one hold: the Mono route and IL2CPP
with flat shaders; the 32-bit port and the Mono route with flat shaders (the second Mono run):

| run | fps | GPU clock (residency) | UnityMain on a CPU | resident, available |
|---|---|---|---|---|
| Mono route (64-bit) | 34.0 | 650 MHz (696: 54 %, 600: 46 %) | 90 % | 720 MB, 62 MB |
| IL2CPP (64-bit) | 34.4 | 645 MHz (696: 47 %, 600: 53 %) | 86 % | 825 MB, 52 MB |
| 32-bit Mono port | 35.2 | 662 MHz (696: 67 %, 600: 32 %) | 88 % | 802 MB, 80 MB |
| flat fragment shaders (the CPU side): Mono route | 46.1, 46.1 (two runs) | 240 MHz | 95 - 96 % | 725 - 731 MB, 47 - 55 MB |
| flat: IL2CPP | 48.8 | 240 MHz | 94 % | 723 MB, 54 MB |
| flat: 32-bit Mono port | 48.9 | 240 MHz | 96 % | 801 MB, 81 MB |

The scene is GPU-bound and all three run it at the frame rate their GPU clock gives. Without the
GPU's work the main thread of the Mono route takes 21.7 ms a frame, 1.2 ms (6 %) more than the
IL2CPP build's and the 32-bit port's 20.5 ms: on this game the route costs a little main-thread
time rather than saving any, and its memory is that of the IL2CPP build. What it saves is the
setup: no IL2CPP conversion and compile (48 minutes for this game in Docker on a Mac; on the
handheld not tried, an hour for Cuphead's smaller build), and none of their downloads.

Where the 1.2 ms go (`UA_PROF=500` in the flat-shader scene, one hold, `profsum.py --syms` with
the 6000.0.61f1 IL2CPP player's libunity symbols and the runtime's own; 44.2 and 47.1 fps with the
profiler): UnityMain's engine time is the same, 14.4 ms a frame (Mono route) and 14.2 ms (IL2CPP);
the game's code is not. The Mono route spends 4.9 ms a frame in JIT-compiled code (17.0 % of its
samples), Mono's runtime (3.8 %: `mono_jit_runtime_invoke` and the invoke wrappers, the JIT info
table) and trampolines (2.2 %), IL2CPP 3.4 ms in `libil2cpp.so` (17.5 %, its AOT code and runtime);
the bridge itself is 0.4 % (0.08 ms). PlayMaker's FSMs lead the managed methods
(`Fsm:UpdateStateChanges`, `UpdateDelayedEvents`, `FsmState:OnUpdate`). Unity's own Mono players
enter managed code the same way (`mono_runtime_invoke`: the 32-bit player's libunity imports no
other entry point), so what is slower is Mono's 64-bit code for these methods against IL2CPP's C++
of them; 32-bit Mono on the same scene keeps up with IL2CPP (48.9 fps).
