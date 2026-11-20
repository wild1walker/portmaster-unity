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
python -m pmunity player donor <ver> work/player64 --backend il2cpp --abi arm64-v8a --managed \
  --apk <donors>/donor-<ver>-il2cpp-arm64.apk --apk <donors>/donor-<ver>-mono-armv7.apk   # (porting.md, section 5)
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
`unity-<line>-mbe` branch of Unity-Technologies/mono for the runtime; checked with 2017.4, 2019.4,
2020.3, 2021.3 and 6000.0. A .NET 3.5 game's own runtime cannot run there: its desktop players run
Unity's older Mono fork (Mono 2.6.5, the `unity-<line>` branches), which has no 64-bit ARM code
generator (its `mono/mini` has `mini-arm.c`, 32-bit ARM, and no `mini-arm64.c`). Its assemblies
run on the line's MonoBleedingEdge instead, with Mono's 4.x class libraries built from the same
sources in place of the game's 2.0 ones ([below](#a-net-35-game-on-the-4x-class-libraries-cuphead):
Cuphead, 2017.4.9f1, on 2017.4.40f1's player and Mono 5.0.1).

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
parallel marking), with `-O2` and Boehm without its byte at the end of each object
(`DONT_ADD_BYTE_AT_END`, [below](#the-collectors-layout-and-what-else-was-tried-for-a-faster-64-bit-build-2026-11-19)): a glibc library, loaded by ua_host itself rather than through the
Android loader, so its threads, signals and `dlopen` are the host's. It fetches the branch's
tarball without the class libraries and tests (45 MB on disk), the pinned bdwgc and libatomic_ops,
and for Mono 6 corefx's native code (libmono-native); the build runs in an arm64 container
(`ua-mono64-build`: ua-build-arm64 plus autotools, glibc 2.31) in about 5 minutes on 8 cores.
Results: `libmonobdwgc-2.0.so` 3.6 MB (2017.4: Mono 5.0.1), 3.9 MB (2019.4, 2020.3), 4.2 MB (6000.0) stripped. The branch head
is used, not the exact commit of the game's release (Unity does not publish that mapping): the
game's `mscorlib.dll` must fit the runtime's internal calls, which change between lines, not
within one (each run checks that: Mono refuses a corlib of another version at start).

The game's class libraries are its own (`Managed/mscorlib.dll` ... of the desktop build; a .NET 3.5
game's are Mono's 4.x ones instead, [below](#a-net-35-game-on-the-4x-class-libraries-cuphead)), and so is
Mono's config (`MonoBleedingEdge/etc/mono/config`, whose Linux dllmaps fit a glibc runtime); the
class libraries' native helpers (`System.Native`, `MonoPosixHelper`) are mapped to the runtime's
folder. P/Invokes into Android libraries (plugins) go through the loader (Mono's `dlopen`
fallback); a game's Linux plugins are glibc libraries already.

## A .NET 3.5 game on the 4.x class libraries (Cuphead)

Unity 2017.4's MonoBleedingEdge is Mono 5.0.1 (`unity-2017.4-mbe`, its `mono/mini` has
`mini-arm64.c`), so a .NET 3.5 game of the line runs on it if it gets 4.x class libraries: its
desktop build ships Unity's 2.0 profile (`mscorlib` 2.0.0.0 ...), whose internal calls and corlib
version are Mono 2.6's, so Mono 5 refuses that `mscorlib`. The game's own assemblies need no change:
Mono 5 binds their references to the 2.0 framework assemblies (`mscorlib`, `System`, `System.Core`
3.5, `System.Xml`) to the 4.0 ones (its framework remapping, as .NET 4 does), and the 2.0 API is a
subset of 4.x's.

**The class libraries** come from the same tree as the runtime, so nothing of Unity's editor or
Mono installers is shipped: `tools/mono64/build-classlibs.sh <line> <work>` builds the branch's
`unityjit` profile (Unity's 4.x profile for JIT players) as Unity's `build.pl` does
(`configure --with-unityjit=yes`, then the class libraries with `mcs`), in the `ua-mono64-build`
image; Mono's own bootstrap compiler (`monolite`, which Mono's Makefile fetches for a source build)
compiles the first `mcs`. 2017.4: 65 assemblies of the profile and 121 facades, 27 MB, in about
20 minutes on 8 cores (the runtime the compiler runs on, the bootstrap profiles, the profile); some
of the profile's assemblies need submodules the tool leaves out (System.Web, WCF: not what games
use) and are not built. With its Linux config (`etc/mono/config`: `libc` -> `libc.so.6`, ...) and
`etc/mono/4.5/machine.config`, also from the tree.

What Cuphead takes from them (its assemblies' references and theirs, followed to the end): `mscorlib`,
`System`, `System.Core`, `System.Xml`, `Mono.Security`, `System.Configuration`, `System.Security`,
`Mono.Posix`: 12.0 MB, every reference resolved. The runtime is `build-mono.sh 2017.4`'s
(`libmonobdwgc-2.0.so` 3.6 MB stripped); libunity 2017.4.40f1 names 175 `il2cpp_*` functions, all
in the bridge, and 32 of the Mono functions the bridge binds are missing from Mono 5.0 (incremental
GC, `*_v2` GC handles, the memory profiler's walks, `mono_unity_gc_disable`/`enable`,
`mono_type_get_attrs`, ...): each use of one by the 2017.4 player's 175 functions is behind a
check with a fallback (`il2cpp_gc_disable` does nothing, GC handles stay 32-bit, ...). One bridge change for it: the domain gets an application
configuration file (`mono_domain_set_config(<Managed>, <Managed>/app.config)`, as Unity's
MonoBleedingEdge players set one): without it `XmlSerializer`'s constructor, through its
diagnostics switch, fails in `System.Configuration` ("The 'ExeConfigFilename' argument cannot be
null"), which stopped Cuphead's slot select (Dialoguer's global variables are XML in the save).

**Behaviour against the game's own class libraries** (Cuphead in the arm64 rig, the IL2CPP build's
APK and OBB, the Managed folder above; the port's test hooks; 2026-10-26):

| check | result |
|---|---|
| title, slot select, Veggies, Frogs, Forest Follies (1-1), the win screen | reached, no managed exception (none either in the IL2CPP build's run of title, slot select and Veggies, or in the 32-bit build's rig logs of them); Mono up 54 - 84 ms after `il2cpp_init`, 64 assemblies, 6,027 internal calls |
| saves (JSON from Unity's `JsonUtility`, with Dialoguer's global variables as `XmlSerializer` XML in a string) | seed slots migrated and rewritten at the slot screen: byte for byte the files the 32-bit build (Mono 2.6, the game's 2.0 libraries) wrote from the same seed. The 32-bit build's slot with progress loads; after a won fight its 1,344 other fields are as the 32-bit build and the IL2CPP build (the game's 2.0 libraries) write them, and the Dialoguer XML differs in one place: the root element declares `xmlns:xsd` before `xmlns:xsi` (the 2.0 libraries: `xsi` first). The reference source's `XmlSerializer` keeps the default namespaces in a `Hashtable`, whose order follows Mono's string hash. Both read either order: the IL2CPP build loaded the Mono route's save and, after the same fight, wrote the 32-bit build's file byte for byte |
| deterministic replay (`CUPHEAD_TEST_DETERMINISTIC`, Veggies, frames 300, 900, 1500 against the IL2CPP build in the same rig) | each build repeats its own frames exactly; between them frame 900 is identical and frames 300 and 1500 differ in 74 and 105 of 345,600 pixels by 1/255 (rounding in some float arithmetic of the game's code: JIT-compiled 64-bit Mono against IL2CPP's C++), the same scene otherwise |
| Dialoguer | its dialogue data is Unity-serialized (`DialogueEditorMasterObject`), not `XmlSerializer`'s; only its global variables go through `XmlSerializer` (the save check above) |
| `System.Random`, hash containers, sorts | the game's code (a Cecil scan of every call): `System.Random` only in a UI animation's shuffle (`UIImageAnimationLoop`, time-seeded); `Dictionary`/`HashSet` enumeration in 63 places, mostly the two players' dictionary: both libraries enumerate in insertion order with removed slots reused last-freed first (Mono 2.6's own `Dictionary` and the reference source's), so the same order; 6 unstable sorts in gameplay code (Merdusa's dots, Saltbaker's cutters, the spider's mines, Jack Frost's shards, coin order in run-and-gun levels): 2.6's quicksort and 4.x's introsort can order equal keys differently; the keys are positions and distances (`float`), equal ones not seen |
| culture | `float`/`int` parsing and formatting go through `CultureInfo.CurrentCulture`, which both runtimes take from the same environment (`LANG`); the game's own `Parser` uses the invariant culture |

## As a port (`--route mono-arm64`)

`pmunity port check` lists the route for a .NET 4.x Mono build of 2017.4 or later (not yet as the
recommended one); `pmunity port new <name> --data <game> --route mono-arm64` writes it. Its setup is
the android route's, with three changes: the player is the arm64 IL2CPP one with its engine modules
(`player donor --backend il2cpp --abi arm64-v8a --managed` from the release's IL2CPP and Mono donors:
nothing downloaded), the packing is
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
| Cuphead (GOG macOS, .NET 3.5) | 2017.4.9f1 data on the 2017.4.40f1 player; Mono 5.0.1 (`unity-2017.4-mbe`) with Mono's 4.x class libraries | title, slot select, Veggies, Frogs, Forest Follies, the win screen; 64 assemblies, 6,027 internal calls, Mono up 54 - 84 ms after `il2cpp_init` | not yet (the handheld was off) | the first .NET 3.5 game on the route ([above](#a-net-35-game-on-the-4x-class-libraries-cuphead), 2026-10-26) |
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

## 32-bit or 64-bit only: what the 64-bit build still costs (2026-11-19)

Ports made with this toolkit (Hollow Knight, Cuphead, other porters' Unity Mono ports) ship the 32-bit
`ua_host` next to `ua_host64`, and the launcher runs the 32-bit build wherever the system has a 32-bit
userland: on the handhelds tried it is the faster one and leaves more memory free. PortMaster's
reviewers ask why a port ships an armhf build ("every device runs the aarch64 build"). This is the
measurement of what an aarch64-only release would give up, and where the difference comes from.

**Setup.** Hollow Knight (6000.0.61f1), the same release's 32-bit and 64-bit installs side by side (one
setup's conversion, the 64-bit one packed `--mono64`; muOS: `hkmem-base` and `hk64r`'s `arm64/`), the
port's own runtime settings for a 1 GB handheld (port.env and the launcher's lines), the same save,
`tools/device/profbench.sh` with the release's binaries, King's Pass standing (`PLAN='log:pd.maxHealth;
sleep:8; press:0:a; sleep:6; press:0:a; sleep:6; press:0:a; hitch:2000; sleep:10; shot'`, 25 s measured),
each run from a cool handheld, 32 and 64 interleaved. The Knulli was not on adb that day: the RG34XX-SP
numbers are muOS's (the same H700; muOS runs its CPUs up to 1.8 GHz, Knulli to 1.5).

| handheld | build | fps | UnityMain, ms a frame | render thread | Mono heap | game resident | lowest available |
|---|---|---|---|---|---|---|---|
| RG34XX-SP (muOS) | 32-bit | 53.6, 53.0, 52.4 | 17.2, 16.6, 16.1 | 11.2, 10.9, 10.7 | 112 MB | 671 - 676 MB | 252 - 258 MB |
| RG34XX-SP (muOS) | 64-bit (Mono route) | 51.3, 50.8, 50.9, 51.0, 50.4 | 18.4, 18.4, 18.4, 18.5, 18.5 | 12.6, 12.7, 12.5, 12.6, 12.6 | 176 MB | 802 - 810 MB (747 - 760*) | 148 - 151 MB (198 - 209*) |
| BattleXP G350 (dArkOS) | 32-bit | 39.2 | 24.3 | GL thread 12.7 | | 589 MB | 261 MB |
| BattleXP G350 (dArkOS) | 64-bit (Mono route) | 37.0 | 25.5 | GL thread 16.0 | | 713 MB* | 173 MB* |

\* Runs in which the two CJK fonts of the port's `UA_UNLOAD_ASSETS` list (NotoSerifCJKsc-Regular,
NotoSerifTC-Medium) stayed given back. In every 32-bit run, and in about half of the 64-bit ones, the game
loaded them again after ua_host gave them back ("loaded again after it was given back: in use, kept"),
50 - 55 MB, so the like-for-like figures are the unstarred ones (the same fonts held by both builds). (The
release's launcher sets `UA_UNLOAD_ASSETS` only for a setup with its lazy step, which these older installs
lack; here both builds had it set. That the 32-bit build always takes the two fonts back is the port's to
look at.)

The 64-bit build runs the scene 4 - 6 % slower (2.2 fps on the RG34XX-SP, 2.2 on the G350) and, with the
same assets held, leaves about 100 MB less memory available (RG34XX-SP: 252 - 258 against 148 - 151 MB;
the G350's runs differ by the fonts, 261 against 173 MB with the 64-bit build holding 50 MB less of them).
The G350 then stopped answering during the 64-bit build's profiled run (`UA_PROF=500` on top of that: a
1 GB handheld without swap at its limit; it needs its power button).

**Where the time goes** (`UA_PROF=500`, the same scene, one hold each; ms a frame of UnityMain's samples
on a CPU, `profsum.py --syms` with each player's symbols and the runtime's own):

| UnityMain | 32-bit | 64-bit | difference |
|---|---|---|---|
| Unity's engine (libunity) | 11.26 | 12.45 | +1.19 |
| managed code (JIT) | 2.61 | 2.95 | +0.34 |
| Mono's runtime | 1.17 | 0.78 | -0.39 |
| libc | 1.27 | 0.78 | -0.49 |
| the rest (ua_host, the bridge, stubs) | 0.38 | 0.41 | +0.03 |

| render thread (UnityGfxDeviceW) | 32-bit | 64-bit | difference |
|---|---|---|---|
| the Mali driver | 4.67 | 5.52 | +0.85 |
| Unity's engine | 3.31 | 3.25 | -0.06 |
| libc, ua_host | 2.87 | 2.91 | +0.04 |

- **Unity's engine is most of it**: the arm64 player's libunity spends 1.2 ms a frame more than the
  armeabi-v7a one on the same work, spread over the whole frame (culling, renderer flattening, behaviour
  lists, playables, FMOD's update, the scripting layer's marshalling; the most at one place,
  `Scripting::GetCachedPtrFromScriptingWrapper`, 0.2 ms: a three-instruction read of a managed object's
  cached pointer, out of line in this player and waiting on the data cache). Twice the pointer size in the
  engine's own structures, on an A53/A35 with 32 KB of data cache, and its outline atomics
  (`__aarch64_ldadd*`, the NDK's for CPUs without LSE): Unity's code, the same for IL2CPP's 64-bit build.
- **The Mali driver**: the 64-bit libmali (r20p0, 44 MB) takes 18 % more CPU for the same draws than the
  32-bit one (14.5 MB); on the G350 (r13p0 GBM) the GL thread 26 % more.
- **Mono is not what is slower.** The 64-bit runtime (Unity's Mono built `-O2` by tools/mono64) takes
  0.39 ms a frame less than Unity's own armeabi-v7a libmono (built without optimisation); its JIT
  compiles a method in about 1 ms against 1.8 (`[jitwarm]`: 2,711 in 2.8 s against 4,081 in 7.5 s). The
  JIT's code is 13 % slower (+0.34 ms), the larger objects' share. The bridge itself: 0.05 ms.
- **Mono's own options do not move it**: `MONO_INLINELIMIT=40` 51.0 fps, UnityMain 18.5 ms;
  `UA_MONO64_OPTIONS=-O=abcrem` 51.0 fps, 18.3 ms (the same hold's plain run: 51.0, 18.5). In the arm64
  rig abcrem shrinks a method like `HeroController.CheckTouchingGround` by 12 %, but these methods are
  not where the frame goes.
- **Mono's ahead-of-time images: measured, slower** (2026-11-19). This runtime has Mono's AOT loader and
  compiler (`mono_main --aot`; configure's "Unity AOT: default"; LLVM back end: no). Compiled with the same
  `libmonobdwgc-2.0.so` in the arm64 build container (GNU as/ld; 5 s for Assembly-CSharp, 17,431 methods,
  13 MB; UnityEngine.CoreModule stops the compiler with a fatal error, left to the JIT) and put beside the
  assemblies (`UA_MONO64_MANAGED`), images of Assembly-CSharp, -firstpass, PlayMaker, mscorlib,
  TeamCherry.TK2D, Unity.TextMeshPro and UnityEngine.UI are taken (`UA_JIT_WARM`'s warm-up then finds 4,700
  of its methods compiled already instead of 3,353). King's Pass on muOS, JIT A B B A: 50.9, 50.9 fps,
  UnityMain 18.4, 18.4 ms; AOT 50.5, 50.8 fps, 18.7, 18.7 ms: the images' code is the same code generator's
  with GOT loads and PLT calls, 0.3 ms slower a frame (on the 32-bit route it cost the same, porting.md),
  and 6 MB more available (213, 216 against 206, 211 MB: less JIT-time memory). Not a setup step: no
  frame-rate gain, and the handhelds have no as/ld. Mono's LLVM back end (the one that could beat the
  JIT) is not built for this runtime, would compile on the handheld at setup, and LLVM may fuse a multiply
  and an add the JIT keeps apart (`fp-contract`): floating-point results the game's code would not get
  from either real player.

**Where the memory goes** (`/proc/<pid>/smaps` 85 s into a run, in the scene, both builds holding the
same fonts: 514 MB against 653 MB resident on the CPU side; the rest of each run's resident figure is the
GPU's mappings):

| | 32-bit | 64-bit | difference |
|---|---|---|---|
| anonymous memory (Mono's heap, every malloc) | 469 MB | 574 MB | +105 MB |
| the assemblies, file pages (the 32-bit player reads them into malloc, counted above) | - | 20.5 MB | +20.5 MB |
| libmali's code | 11.8 MB | 24.9 MB | +13 MB |
| libunity's code | 19.0 MB | 22.3 MB | +3 MB |
| Mono's code | 5.7 MB | 3.2 MB | -2.5 MB |

Mono's heap is the largest part: 112 MB on the 32-bit build, 176 MB on the 64-bit one in the same scene
(after the load's collections 56 MB live against 118 MB). An object's header and every reference take
twice the space, and Boehm's granule is 16 bytes instead of 8; Mono's own metadata (classes, methods,
JIT tables) grows the same way. glibc's malloc arenas are not it (`MALLOC_ARENA_MAX=2`: 594 against 599
MB, 202 against 199 MB available). The class libraries the bridge loads at start because IL2CPP lists every
assembly (System.Windows.Forms, System.Data, ...: 10.8 MB of the 20.5 MB of assembly pages) are the one
part that is the route's own (below).

**What the heap holds, object by object** (`UA_GC_CENSUS`, runtime.md: Boehm's reachable objects right after a
full collection, King's Pass standing after the load, the same save; RG34XX-SP under Knulli, 2026-11-19):

| | 32-bit | 64-bit (release bridge) | 64-bit, empty strings shared |
|---|---|---|---|
| live objects | 1,447,739 | 1,653,030 | 1,455,200 |
| live bytes | 68.1 MB | 110.4 MB | 104.3 MB |
| strings | 269,312, 12.5 MB | 466,911, 21.5 MB | 269,355, 15.5 MB |
| of them empty (`""`) | none (String.Empty) | 197,566, 6.0 MB | none |
| objects with references (Boehm's typed kind) | 797,372, 27.8 MB | 821,327, 53.5 MB | |
| pointer-free (strings, value arrays) | 477,303, 24.7 MB | 675,543, 37.8 MB | |
| Mono's heap at that point / the run's largest | 112 / 112 MB | 176 / 176 MB | 160 / 176 MB |

The one difference that is not the pointer size: 197,566 empty strings. libunity's IL2CPP scripting layer
makes a string through `il2cpp_string_new`/`_new_len`/`_new_utf16` for every serialized string field it
reads (PlayMaker's FSMs hold hundreds of thousands, most of them empty), and the bridge passed each to
Mono's `mono_string_new*`, which allocates a new empty string every time. Both real players answer
String.Empty there: IL2CPP's `String::NewSize(0)` returns it, and libunity's Mono backend makes its empty
strings with `mono_unity_string_empty_wrapper` (the 32-bit census: not one empty string besides it). The
bridge now does the same (`mono_string_empty`; `UA_MONO64_EMPTY_STRING=0` the old way, a measurement):
exact, 6 MB less live heap. Everything else scales with the pointer: the same 269,312 strings take 12.5
-> 15.5 MB (a 16-byte header and Boehm's 16-byte granule), the objects with references 35 -> 65 bytes
each (27.8 -> 53.5 MB), `string[]`, `int[]`, PlayMaker's arrays about twice; the 32-bit heap's classes
and counts are the 64-bit one's, class for class. Mono has no compressed references, so this part has no
switch: about 36 MB of live heap, and with the budget's 32 MB of garbage on top Mono's heap still reaches
176 MB later in the run (Boehm grows it 16 MB at a time). Mono's metadata grows the same way (its own
counters, `UA_GC_CENSUS`: MonoClass 3.2 -> 6.7 MB, inflated classes 1.8 -> 3.2, MonoMethod 0.75 -> 1.2,
inflated methods 1.1 -> 1.7; the JIT's code 2.8 MB on both).

**The three handhelds again, with the fix** (the same installs and settings, King's Pass,
`tools/device/profbench.sh`, each run from a cool handheld, 32-bit, the release bridge and the fixed bridge
interleaved A B C A C B in one hold; UnityMain and the render thread (G350: ua_host's GL thread) in ms a
frame of CPU; lowest MemAvailable; the G350's runs paired by whether the game took the CJK fonts back):

| handheld | build | fps | UnityMain | render | lowest available |
|---|---|---|---|---|---|
| RG34XX-SP (muOS) | 32-bit | 52.7, 52.3 | 16.7, 16.4 | 10.8, 10.7 | 251, 251 MB |
| | 64-bit, release bridge | 50.8, 50.9 | 18.5, 18.5 | 12.6, 12.6 | 202, 198 MB |
| | 64-bit, empty strings shared | 50.9, 50.8 | 18.6, 18.6 | 12.7, 12.7 | 204, 205 MB |
| RG34XX-SP (Knulli) | 32-bit | 53.8, 53.9 | 18.2, 18.1 | 11.6, 11.6 | 253, 255 MB |
| | 64-bit, release bridge | 50.7, 51.3 | 19.3, 19.1 | 13.2, 13.0 | 204, 202 MB |
| | 64-bit, empty strings shared | 50.5, 50.9 | 19.4, 19.2 | 13.1, 13.1 | 210, 209 MB |
| BattleXP G350 (dArkOS) | 32-bit | 40.2, 39.6 | 24.1, 23.9 | GL 12.0, 12.4 | 268, 267 MB (fonts kept) |
| | 64-bit, release bridge | 38.0, 37.8 | 24.8, 25.3 | GL 15.2, 15.3 | 186 (fonts given back), 149 MB (kept) |
| | 64-bit, empty strings shared | 37.8, 37.7 | 25.2, 25.1 | GL 15.7, 15.6 | 194 (given back), 158 MB (kept) |

The fix gives 6 - 9 MB of memory and nothing in frame rate. The 64-bit build stays 3.5 - 6 % slower
(muOS 1.7 fps, Knulli 3.0, G350 2.2) and leaves 45 MB less available on the RG34XX-SP, about 110 MB less on
the G350 with the same fonts held (its 64-bit libmali, r13p0 GBM, and GL thread: +3.3 ms, +27 %).
The G350 stopped answering (no network, a power button needed) during the next hold's first 64-bit run,
the fixed bridge with the same settings as the runs above, as it did once before under `UA_PROF`: a 1 GB
handheld without swap with 150 - 190 MB available at the scene and less during its load.

**Verdict.** On these handhelds an aarch64-only Hollow Knight gives up 3.5 - 6 % of its frame rate and 45
(RG34XX-SP) to 110 MB (G350) of available memory, and no part of that is the route's own any more: Unity's
arm64 engine (+1.1 - 1.2 ms of UnityMain), the 64-bit Mali driver (+1.5 - 1.9 ms of the render thread,
+3.3 ms of the G350's GL thread, +13 MB of code) and Mono's objects and metadata at 64-bit pointers (+36 MB
of live heap, about +7 MB of metadata: the census finds the same objects, each larger). The levers tried
(Mono's inline limit, abcrem, AOT images, the parameter-count cache, malloc arenas, the empty strings)
moved the frame rate by nothing measurable. A game near its frame-rate or memory limit keeps the armhf
build; one with headroom can ship aarch64 alone.

**What libunity asks of the runtime** (`UA_API_COUNT=10`, the same scene, a frame): the 32-bit Mono player
4,690 `mono_gc_wbarrier_set_field` and 717 `mono_runtime_invoke`; the 64-bit player on the Mono route
4,650 `il2cpp_gc_wbarrier_set_field` and 712 `il2cpp_runtime_invoke`, the same, plus 712
`il2cpp_method_get_param_count`: IL2CPP's scripting layer asks for the parameter count before every
invoke, which the bridge answered through `mono_method_signature` each time (0.07 ms a frame). The bridge
now remembers the count of every method of a loaded assembly's metadata (a method's signature is fixed
for its life; dynamic methods are asked each time): exact, and within the runs' spread on the frame (51.0
fps, UnityMain 18.5 ms, against 50.7 and 18.5 with the release's bridge in the same hold).

**What still keeps the 32-bit build ahead, and the next step for each:**

| what | cost on the 64-bit build | next step |
|---|---|---|
| Unity's arm64 engine (libunity) | +1.2 ms of UnityMain a frame (+11 %), all over the frame | none from the runtime's side: exact hooks of single engine functions measured about 1 % each (profiling.md). The NDK's outline atomics (`__aarch64_ldadd4_relax`, `_ldadd8_relax`, `_cas8_acq_rel` ...: 1.6 % of UnityMain's samples, about 0.3 ms) are mostly the LL/SC loop itself, which an A53/A35 without LSE runs either way; pointing them at versions without the `__aarch64_have_lse_atomics` test would save its load and branch, under 0.1 ms. `Scripting::GetCachedPtrFromScriptingWrapper` (0.2 ms) is out of line in this player and cannot be inlined into its callers from outside. A game whose main thread is the limit keeps the 32-bit build |
| the 64-bit Mali driver | +0.85 ms of the render thread (+18 %; G350's GL thread +3.3 ms, +27 %) and +13 MB of code | no choice of driver: muOS and Knulli each ship one libmali per architecture (`/usr/lib/libmali.so`, 44 MB with its symbols, and `/usr/lib32/libmali.so`, 14.5 MB, both `libmali.so.0.20`), no Panfrost; ua_host's GL layer does the same work on both builds (2.87 against 2.91 ms); fewer GL calls help both builds alike |
| Mono's heap and metadata at 64-bit pointers | +36 MB of live heap after the empty-string fix (68 -> 104 MB live; Mono's heap 112 -> 160 - 176 MB), Mono's metadata about +7 MB: most of the lost memory | Boehm's byte at the end of each object dropped (2026-11-19, [below](#the-collectors-layout-and-what-else-was-tried-for-a-faster-64-bit-build-2026-11-19): 104.3 -> 98.7 MB live, 6 - 9 MB more available); 8-byte granules stop the IL2CPP player. Otherwise none at run time: the census finds the same classes and counts on both builds, each object larger (Mono has no compressed references); `UA_GC_GROW` (a collection before the heap grows during a load when memory is low) is the lever for a 64-bit build at its memory limit |
| empty strings made new for libunity (the route's own) | 197,566 live, 6 MB of heap | **closed** (2026-11-19): the bridge answers String.Empty as both real players do; 6 - 9 MB more available, the frame rate the same |
| JIT code | +0.34 ms (+13 %) | not Mono's options (inline limit, abcrem: no change), not Mono's AOT images (0.3 ms slower, above); IL2CPP's arm64 build (compiled at setup, hours on the handheld) |
| the class libraries loaded at start | 10.8 MB of file pages (System.Windows.Forms, System.Data...) | `UA_MONO64_ASSEMBLIES=scripting` (runtime.md): only `ScriptingAssemblies.json`'s at start, as a Mono player, the class libraries when used: 88 + 7 assemblies instead of 116, 588 against 599 MB resident (assembly pages 20.5 -> 16.0 MB), 209 and 212 MB available against 198 - 204, the scene and its frame rate unchanged (50.8, 51.1 fps); opt-in until the route's other games (Cuphead's 2017.4 player, Tom City, Untitled Hook Game) have run with it, then the default |
| Mono's runtime invoke path | about 0.35 ms a frame (already less than the 32-bit libmono's) | not worth a cache: this Mono's `mono_jit_runtime_invoke` already finds a method's invoke info in a lock-free hash (`mono_conc_hashtable_lookup`, 0.09 ms a frame in the profile; the 32-bit libmono took the domain lock, which is what `UA_MONO_INVOKE` removes); what a cache could save is under the runs' spread |

## The collector's layout, and what else was tried for a faster 64-bit build (2026-11-19)

The aim: make the parts of the 64-bit build that are ours (Mono, its collector, the bridge, ua_host)
so much better than Unity's armeabi-v7a libmono that the 64-bit build overtakes the 32-bit one. King's
Pass as above (the 1 GB settings, `tools/device/profbench.sh`, cool between runs, 32 and 64 interleaved).

**Boehm's byte at the end of every object: dropped** (`tools/mono64/build-mono.sh`, every line). Mono's
bdwgc is configured with `ALL_INTERIOR_POINTERS`, which also adds one byte to every allocation (so that
a pointer just past an object keeps it alive). Mono's objects are multiples of 8 bytes, and aarch64's
granule is 16, so every object whose size is a multiple of 16 took a whole granule more (the 32-bit
build pays 8 bytes for every second object). `DONT_ADD_BYTE_AT_END` keeps interior pointers recognised
and drops the byte. The census (`UA_GC_CENSUS`, Knulli, the same save, right after the same full
collection in the scene):

| | 64-bit, release runtime | without the byte |
|---|---|---|
| reachable objects | 1,455,214 | 1,455,376 |
| live bytes | 104.30 MB | 98.69 MB |
| the load's second collection: live, Mono's heap | 69.0 MB, 96 MB | 65.4 MB, 80 MB |

| handheld | build | fps | UnityMain | render | lowest available |
|---|---|---|---|---|---|
| RG34XX-SP (muOS) | 32-bit | 53.0, 52.5 | 17.0, 16.7 | 11.0, 10.8 | 252, 252 MB |
| | 64-bit, release runtime | 50.8, 51.0 | 18.5, 18.5 | 12.6, 12.6 | 213, 207 MB |
| | 64-bit, without the byte | 51.1, 50.5, 48.6* | 18.5, 18.8, 18.6 | 12.7, 12.7, 12.6 | 216, 220, 219 MB |
| RG34XX-SP (Knulli) | 32-bit | 54.0, 53.8 | 18.1, 18.2 | 11.8, 11.7 | 250, 249 MB |
| | 64-bit, release runtime | 51.3, 50.7 | 19.1, 19.3 | 13.0, 13.1 | 209, 209 MB |
| | 64-bit, without the byte | 51.1, 51.1 | 19.0, 19.1 | 13.0, 13.0 | 218, 217 MB |

\* the GPU clock fell to 360 MHz for a second in that run; UnityMain is the others'.

6 - 9 MB more available, the frame rate the same. It is exact: what changes is where objects lie, and
managed code sees an address only through `RuntimeHelpers.GetHashCode` (Mono's Boehm build derives it
from the address, which differs from run to run anyway); the census counts the same objects. Mono's JIT
emits the same code for SGen, which never keeps an object alive through a pointer past its end.

**Tried, not taken:**

- *Interior pointers off* (`GC_all_interior_pointers` 0; Boehm still takes interior pointers from stacks
  and registers): the same live heap (98.71 MB), the census walk 250 against 261 ms, frame rate and
  available memory those of the byte-less build (muOS 50.9, 51.0 fps, 217, 216 MB; Knulli 50.9, 51.1 fps,
  220, 212 MB). Nothing gained for the risk of a heap interior pointer somewhere in Mono's native code.
- *8-byte granules* (`GC_GRANULE_BYTES=8`, SGen's alignment, the other half of the per-object waste):
  the game stops at its first scene on both handhelds (SIGSEGV, a call through a null function pointer
  in libunity during `TeamCherry.Localization.Language.LoadLanguage`), with and without interior
  pointers. Something in the IL2CPP player expects 16-byte aligned objects (IL2CPP's own Boehm on arm64
  has 16-byte granules); not followed further.
- *Transparent huge pages* (for libunity's code and the heap, against the A53's small TLB): neither
  RG34XX-SP kernel has them (muOS and Knulli, Linux 4.9.170: no `/sys/kernel/mm/transparent_hugepage`,
  no huge pages in `/proc/meminfo`); nothing a process can ask for.
- *Mono's remaining optimisation flags*: `sched` and `leaf` are not used by this Mono's JIT,
  `aggressive-inlining` is the inline limit (measured above), `ssa` is what `abcrem` builds on (measured
  above); `float32` is on already and must stay (it is the 32-bit libmono's arithmetic too).

**Not built, and why:**

- *SGen.* Its nursery moves objects, and the IL2CPP player was written for one that never does
  (IL2CPP's Boehm): SGen would be safe only if libunity keeps every object pointer where SGen sees it
  (its stacks, scanned conservatively and so pinned; GC handles; `il2cpp_gc_alloc_fixed` roots) and
  never one in its own heap memory, which no one has checked and which would show as rare corruption,
  not a clean failure; SGen has no mode without the copying nursery. Its memory advantage over Boehm,
  the 8-byte alignment, the IL2CPP player does not take even under Boehm (above). Not tried.
- *Mono's LLVM back end.* Unity's branch supports it only with dotnet/llvm-project (the fork as a
  submodule, built with the runtime: a full LLVM build), as an AOT compiler (`--aot=llvm`: `opt`, `llc`
  and a linker on the handheld at setup, where the game's assemblies are; 17,431 methods in
  Assembly-CSharp alone) or as a JIT (compiles far slower than the mini JIT the warm-up already waits
  for). Its code would also be AOT code, which on this runtime runs 0.3 ms a frame slower than the JIT's
  (GOT loads and PLT calls, above). Floating point could be kept exact (no `--ffast-math`, which is what
  turns on `-fp-contract=fast`; `-fp-contract=off` explicitly), but the arithmetic: managed code is
  2.95 ms of UnityMain's frame, so even 30 % faster code minus the AOT cost saves about 0.6 ms, against
  a 64-bit deficit of 1.8 - 2 ms that is Unity's engine and the Mali driver.

**Where that leaves the 64-bit build**: everything of ours is at or below the 32-bit build's cost
(Mono's runtime -0.39 ms, libc -0.49, the bridge 0.05, the JIT's code +0.34 from larger objects); the
gap is libunity arm64 (+1.2 ms) and the 64-bit Mali driver (+0.85 ms of the render thread) on the
frame, and pointer-sized objects on memory. A 64-bit build cannot overtake the 32-bit one from the
runtime's side on these handhelds.
