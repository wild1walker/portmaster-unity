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
parallel marking), with `-O2`, Boehm without its byte at the end of each object
(`DONT_ADD_BYTE_AT_END`, [below](#the-collectors-layout-and-what-else-was-tried-for-a-faster-64-bit-build-2026-11-19))
and, for 6000.0, 8-byte granules ([below](#8-byte-granules-work-the-crash-was-a-broken-test-build)): a glibc library, loaded by ua_host itself rather than through the
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
| Mono's heap and metadata at 64-bit pointers | +36 MB of live heap after the empty-string fix (68 -> 104 MB live; Mono's heap 112 -> 160 - 176 MB), Mono's metadata about +7 MB | Boehm's byte at the end of each object dropped (104.3 -> 98.7 MB live), then 8-byte granules (98.7 -> 91.8 MB live, Mono's heap 160 -> 144 MB: [below](#8-byte-granules-work-the-crash-was-a-broken-test-build)); the rest is the pointer size in each object (Mono has no compressed references) |
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
  the test build stopped at the first scene (SIGSEGV, a call through a null function pointer in
  libunity). That build was broken, not the idea: it lacked Mono's collector functions, and the
  bridge called the missing `mono_gc_collect`. Built properly, 8-byte granules work and are 6000.0's
  default now ([below](#8-byte-granules-work-the-crash-was-a-broken-test-build)).
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
  the 8-byte alignment, Boehm now has too (8-byte granules, below). Not tried.
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

## A fourth look: 8-byte granules, the engine symbol by symbol, the GPU clock, thread placement (2026-11-19)

The same question once more, with what the first three passes left open: the crash with 8-byte
granules, where libunity's extra time is function by function, the Mali driver by GL call, whether
Unity ships any other arm64 player, and what the threads' placement does. King's Pass as before (the
1 GB settings, `tools/device/profbench.sh`, each run from a cool handheld, builds interleaved in one
hold), RG34XX-SP under muOS and Knulli; the G350 was off.

### 8-byte granules work: the crash was a broken test build

The runtime the third pass tried with `GC_GRANULE_BYTES=8` had been linked without Mono's collector
glue: it exported none of `mono_gc_collect`, `mono_gc_get_used_size`, `mono_gc_walk_heap` ... (19
functions of `boehm-gc.c` missing from its dynamic symbols; the collector itself, `GC_gcollect`, was
there). The bridge binds Mono's functions with `dlsym` and left `mono_gc_collect` NULL;
`il2cpp_gc_collect` ends in a tail call through that pointer, so the first collection libunity asks
for (`CleanupAfterLoad` -> `scripting_gc_collect_unsafe`, right after the first scene's load: the
"call through a null function pointer in libunity during `Language.LoadLanguage`") jumped to 0 with
libunity's return address still in the link register. Nothing in the IL2CPP player expects 16-byte
objects. Two guards against the same mistake: the bridge now refuses a runtime without
`mono_gc_collect`, `mono_gc_get_used_size` and `mono_gc_get_heap_size` when it loads it ("not a complete
build of the runtime"), and `build-mono.sh` checks the finished library's exports.

Built again from a clean tree (`-DGC_GRANULE_BYTES=8 -DGC_GRANULE_WORDS=1` on top of
`DONT_ADD_BYTE_AT_END`; bdwgc has the one-word granule for 64-bit targets in its size macros and its
mark loops), the game runs as before. Mono's objects are multiples of 8 bytes, the 32-bit collector's
granule is 8 bytes and SGen aligns to 8 on aarch64, so Mono's own code and its JIT expect nothing
more; the collector's 16 on 64-bit targets is bdwgc's default, which rounds every other object up by
8 bytes. The census (`UA_GC_CENSUS`, Knulli, the same save, right after the same full collection):

| | 16-byte granules (2026-11-19's build) | 8-byte granules |
|---|---|---|
| reachable objects | 1,455,484 | 1,457,342 |
| live bytes | 98.70 MB | 91.78 MB |
| Mono's heap at that point | 160 MB | 144 MB |
| the 32-bit build, for comparison | 1,447,725 objects, 68.14 MB, heap 112 MB | |

| handheld | runtime | fps | UnityMain | render | lowest available |
|---|---|---|---|---|---|
| RG34XX-SP (Knulli) | 16-byte granules | 51.0, 50.6 | 19.2, 19.2 | 13.1, 13.0 | 218, 216 MB |
| | 8-byte granules | 51.0, 51.0 | 19.2, 19.2 | 13.1, 13.1 | 220, 227 MB |
| RG34XX-SP (muOS) | 16-byte granules | 51.1, 50.9 | 18.4, 18.3 | 12.5, 12.5 | 220, 219 MB |
| | 8-byte granules | 51.0, 51.0 | 18.3, 18.4 | 12.5, 12.7 | 224, 229 MB |

The frame rate is the same; 6.9 MB less live heap, one 16 MB step less of Mono's heap, 4 - 9 MB more
available in these runs (14 - 35 MB in the census runs below, where both builds hold the same assets). Exact for the reason the dropped
byte is: only where objects lie changes, which managed code sees through nothing but
`RuntimeHelpers.GetHashCode` (address-derived in a Boehm build, different from run to run anyway; its
`>> 3` keeps every bit that differs between two objects at either granule).
`tools/mono64/build-mono.sh` builds 6000.0's runtime this way; the other lines keep 16 bytes until a
game of theirs has run with it (`MONO64_GRANULE=8|16` chooses for any line).

*Smaller heap steps, not taken.* With the game's collector off (Hollow Knight), Boehm grows the heap
by `MAXHINCR` blocks whenever an allocation finds no room: 16 MB at a time in Unity's configuration
(`LARGE_CONFIG`). Capped at 4 MB in `GC_collect_or_expand` (a test patch), the same run ends at 140 MB
instead of 144: the heap is the live data, the 32 MB budget (`UA_GC_BUDGET`) and about 20 MB of free
space inside partly used blocks, not the step. Not worth a patch to the collector.

### Memory with the same assets held: the gap is 55 - 70 MB, not 45

The frame-rate runs above (and every table of this page since "the three handhelds again") compare a
32-bit run that holds the two CJK fonts of `UA_UNLOAD_ASSETS` (NotoSerifCJKsc-Regular,
NotoSerifTC-Medium: about 50 MB; these test installs lack the setup's `lazy` step, and the 32-bit build
loads them again in every run) with 64-bit runs in which they stayed given back: 250 against 216 -
229 MB. Like for like the difference is larger. Census runs with the two fonts left out of the list, so
that both builds hold them (Knulli; lowest MemAvailable, the process's anonymous memory at the end):

| | lowest available | anonymous memory (RssAnon) |
|---|---|---|
| 32-bit | 249, 250 MB | 485, 486 MB |
| 64-bit, 16-byte granules | 180, 159 MB | 555, 575 MB |
| 64-bit, 8-byte granules | 194, 194, 187, 182 MB | 544 - 553 MB |

So the gap with the same assets held is 55 - 70 MB on the RG34XX-SP, not 45. (muOS, the same kind of run:
32-bit 245 MB, 64-bit with 8-byte granules 180 MB.)

| where the 64-bit build's anonymous memory is (Knulli, 8-byte granules) | 32-bit | 64-bit |
|---|---|---|
| Mono's heap | 112 MB | 144 MB |
| glibc's malloc, in use and mmapped (Mono's metadata, the Mali driver's userspace side, the host; the 32-bit player also keeps the assemblies there, about 20 MB) | 98 MB | 113 MB |
| Unity's allocators (libunity's anonymous mappings, resident; Unity's own count: 197 MB allocated of 345 MB reserved on 32-bit, 227 of 358 MB on 64-bit) | 293 MB | 305 - 310 MB |
| Mono's own counters: classes, inflated classes and methods, signatures | 8 MB | 14 MB |

The libraries' code and the assemblies the 64-bit build maps (Mapped 85 against 53 MB) are file
pages, which MemAvailable counts as reclaimable: the difference in available memory is the anonymous
memory. What remains is the pointer size in Mono's objects and metadata and in the engine's own
structures: no switch of the runtime's reaches it. (A game-side lead, for the port, both builds:
PlayMaker keeps every state's serialized `ActionData` after the actions are built, 12,528 objects with
25 lists each, about 17 MB of the 64-bit heap.)

### Unity's engine, symbol by symbol

Two recordings of each build (`UA_PROF=500`, 20 s and 45 s, UnityMain's samples on a CPU by the
function the program counter is in, each player's own symbol file; ms a frame, the 45 s pair):

| UnityMain | 32-bit | 64-bit | difference |
|---|---|---|---|
| Unity's engine (libunity), without the next line | 11.23 | 12.09 | +0.86 |
| the NDK's outline atomics (`__aarch64_ldadd8_relax`, `_ldadd4_relax`, `_cas8_acq` ...; inline `ldrex`/`strex` in the 32-bit player, counted in its callers) | - | 0.31 | +0.31 |
| managed code (JIT) | 2.51 | 2.95 | +0.44 |
| Mono's runtime | 1.20 | 0.77 | -0.44 |
| libc | 1.25 | 0.80 | -0.45 |
| ua_host, the bridge | 0.24 | 0.24 | 0 |
| unnamed code (trampolines) | 0.40 | 0.26 | -0.14 |

libunity's +0.86 ms over its 2,191 functions with a sample in either recording: the functions that
got slower sum to +3.15 ms, the ones that got faster to -2.29; six differ by more than 0.05 ms
(together +0.56), fourteen by more than 0.02 ms (+0.85). The largest, both recordings agreeing:

| function | 32-bit | 64-bit | what it is |
|---|---|---|---|
| `Scripting::GetCachedPtrFromScriptingWrapper` | 0 | 0.19 | the read of a managed object's native pointer: a function of its own in the IL2CPP player, inline in the Mono player, where the same cache miss shows in its callers (`MonoBehaviour::CallUpdateMethod` 0.35 -> 0.27, `SerializableManagedRef::GetScriptCache` 0.22 -> 0.18) |
| `FMOD::SystemI::UpdateAudibilities` | 0.27 | 0.40 | FMOD's per-frame walk of its channels |
| `BaseRenderer::FlattenBasicData` | 0.04 | 0.12 | renderer data copied for the render thread |
| `BaseBehaviourManager::CommonUpdate` | 0.11 | 0.16 | the walk of the behaviour lists |
| `Playable::Handle` | 0.10 | 0.15 | the playable graph's traversal |
| `InputAxis::Update` | 0.05 | 0.10 | |
| `GameObject::QueryComponentByType` | 0.15 | 0.19 | |
| `ProcessCameraIndexListIsNodeVisibleStep1` | 0.14 | 0.19 | culling |
| `MonoBehaviour::HandleNotifications`, `Behaviour::GetEnabled`, `b2GrowableStack::Push`, `PrepareSpriteRenderNodes` | 0.15 | 0.29 | |

None of them calls the runtime: the engine asks the bridge for the same things the 32-bit player asks
Mono for (`UA_API_COUNT`: 4,606 write barriers and 706 invokes a frame against 4,582 and 703, plus
the parameter count the bridge now remembers), and the bridge's own share is 0.05 ms. The functions
are pointer walks over the engine's lists, trees and component tables (and FMOD's channel list): the
same instructions' work on structures half again as large, on a core with 32 KB of data cache. Mono's
side shows the same thing in one number: `mono_runtime_class_init_full`, on this path a load of one
flag of the method's vtable, takes 120 ns a call (706 a frame): the time is the cache miss. Outside
the engine nothing is left to take: Mono's whole invoke path (bridge, `mono_runtime_invoke`, the
lookup, the wrappers) is 0.5 ms a frame on the 64-bit build against 1.2 ms of the 32-bit libmono.

### The render thread and the Mali driver

| UnityGfxDeviceW, ms a frame (the 45 s pair) | 32-bit | 64-bit | difference |
|---|---|---|---|
| the Mali driver | 4.54 | 5.47 | +0.92 |
| Unity's engine | 3.12 | 3.09 | -0.04 |
| libc | 2.03 | 1.96 | -0.07 |
| ua_host's GL layers | 1.06 | 1.05 | 0 |

The 64-bit driver's time by the GL call it was entered through (its stacks unwind; the 32-bit blob's
do not, so there is no pair for this): `glDrawElements` 2.5 ms a frame, `glBindFramebuffer` 2.1 (the
driver closes a pass's work there), `glClear` 1.4, the swap 0.3, `glBufferSubData` 0.2,
`glBindTexture` 0.2, `glUseProgram` and `glUniform4fv` 0.1 each (the driver's calls into libc
included). Draws, pass changes and clears: the calls the frame consists of, the same ones in the
same order on both builds (ua_host's layers cost the same, 1.06 and 1.05 ms).

It is not what holds the 64-bit frame: the render thread sleeps a third of its time waiting for
UnityMain's commands (33 % of its samples in `Semaphore::WaitForSignal`, 38 % on the 32-bit build),
and a core to itself (`UA_AFFINITY='UnityMain:3 UnityGfxDeviceW:2 *:0-1'`) changes nothing (muOS
51.1 against 50.9 fps, Knulli 51.6 against 51.3). UnityMain waits for it 0.4 - 0.8 ms a frame
(`GfxDeviceClient::WaitForPendingPresent`), on both builds.

### The GPU clock: on muOS the two builds meet once the handheld is warm

The RG34XX-SP's GPU clock follows the load (simple_ondemand) and the thermal cap (600 MHz from about
68 C). Each second's frame rate grouped by the second's GPU clock: on muOS the 32-bit build spends the
first seconds of a 25 s run at 648 - 816 MHz and the 64-bit build almost none, which is part of the
25 s runs' 2 fps. Over 150 s (muOS, both from a cool start, `MEASURE=150`) the two run 53.2 and 51.7
fps, and in the seconds both spend at 600 MHz 52.7 against 51.9 fps: a gap of 0.8 fps (1.5 %) once the
handheld is warm, which is how it plays. Knulli keeps 696 MHz in both builds' runs, and there the gap
stays 2 - 3 fps (55.0 - 55.5 against 52.2 - 53.2 fps in the seconds at 696 MHz).

### Thread placement

Thread placement through `UA_AFFINITY` (ua_host's own threads.c: the game's threads by name, nothing
of the handheld's settings), A B pairs in one hold:

| setting | Knulli 64-bit | Knulli 32-bit | muOS 64-bit |
|---|---|---|---|
| none | 51.1 - 51.3 fps (5 runs) | 53.8 | 50.9 - 51.2 |
| `UnityMain:3 Job.Worker*:2 *:0-1` (the main thread and the job worker a core each) | 52.3, 51.8 | 55.1 | 51.1 |
| `UnityMain:3 Job.Worker*:2 UnityGfxDeviceW:1 *:0` | 52.7, 52.1 | | 51.1 |
| `UnityMain:3 *:0-2` | 51.6 | | 51.1 |
| `FMOD*:0 *:1-3` | | | 50.8 |

On Knulli the main thread and the job worker on cores of their own give 1 - 2 % to both builds (the
worker's wait for a CPU 13 -> 5 %); on muOS nothing. It does not close the gap between the builds and
is not a default (one firmware, one game). Also measured, nothing: `UA_FUTEX_SPIN` 20, 200 and 500 us
(muOS 50.5 - 50.8 against 50.9 - 51.2 fps; Knulli 51.2), two job workers (muOS 51.1, UnityMain 17.8
against 18.4 ms but the same frame rate).

### Does Unity ship another arm64 player?

A Linux or Android arm64 player built for Mono would replace the IL2CPP player and the bridge. None
exists for the lines used here:

- Unity's manual, "Scripting back ends" (6000.0 and 6000.3): Mono is supported on Windows
  x86/x64/Arm64, macOS x64/Arm64, Linux x64 and Android Armv7; "any platform not mentioned in the
  previous list of Mono-supporting platforms is IL2CPP-only".
- 6000.0.61f1's module lists (`unity-6000.0.61f1-linux.ini`, `-osx.ini`, `-win.ini` on
  download.unity3d.com): Linux Build Support (IL2CPP), (Mono) and Dedicated Server; their archives hold
  the variations `linux64_player_*` and `linux64_server_*` only (x86-64, Mono and IL2CPP). No Embedded
  Linux module in the public lists; Unity's arm64 Linux sysroot package (`com.unity.sdk.linux-arm64`,
  for Linux and Embedded Linux) is for "IL2CPP-based Unity players".
- 2021.3: the Android player's `Variations/mono` has `armeabi-v7a` only (the module on disk),
  `Variations/il2cpp` has arm64-v8a; desktop Linux is x64 (system requirements); Embedded Linux x64
  and ARM64 on request. 2017.4: Android Mono for ARMv7 and x86, Linux x86 and x86-64.

The arm64 players Unity builds for Mono are macOS's and Windows's: a Mach-O or PE engine that draws
with Metal or Direct3D, not something the Android route's runtime can host in place of libunity.so
([macho-route.md](macho-route.md) uses a macOS build's game code, not its engine).

### Unity's allocator blocks: the largest memory lever (both builds)

Unity's native allocators take memory from the system in blocks (`memorysetup-*-allocator-block-size`,
16 MB by default for the main, thread and graphics allocators) and keep a block while any allocation in
it lives. Hollow Knight's player allocates 227 MB (64-bit) and holds 358 MB reserved; most of the
difference is freed memory in blocks that something small still pins. Smaller blocks through
`UA_BOOT_CONFIG` (boot.config lines Unity reads at launch; the APK unchanged), Knulli, census runs with
the same assets held:

| | Unity reserved | libunity resident | lowest available |
|---|---|---|---|
| 64-bit, 16 MB blocks (Unity's default) | 358 MB | 305 - 310 MB | 182 - 187 MB |
| 4 MB | 303 MB | 283 - 288 MB | 204 - 211 MB |
| 2 MB | 289 MB | 275 MB | 212 - 218 MB |
| 1 MB | 276 MB | 264 MB | 228 - 234 MB |
| 0.5 MB | 270 MB | 260 MB | 233 MB |
| 1 MB, also the cache and type-tree allocators at 1 MB | 269 MB | 262 MB | 228 MB |
| 32-bit, 16 MB | 345 MB | 289 - 293 MB | 249 MB |
| 32-bit, 1 MB | 240 MB | 228 MB | 305 MB |

Frame-rate runs with 1 MB blocks (`memorysetup-main-, -thread-, -gfx-main-, -gfx-thread-allocator-block-size=1048576`),
A B pairs in one hold, runs with the same fonts held compared:

| handheld | build | fps, 16 MB -> 1 MB | lowest available, 16 MB -> 1 MB |
|---|---|---|---|
| RG34XX-SP (Knulli) | 64-bit | 51.3, 51.1 -> 50.8, 51.2 | 228, 222 -> 268, 270 MB |
| | 32-bit | 54.1 -> 54.0 | 299 -> 311 MB (the 1 MB run held the fonts) |
| RG34XX-SP (muOS) | 64-bit | 50.9, 51.2 -> 51.0 | 226, 228 -> 271 MB |
| | 32-bit | 53.1 -> 53.2 | 305 -> 317 MB (the 1 MB run held the fonts) |
| BattleXP G350 | 64-bit | 39.4 -> 39.6, 38.9 | 207 -> 224, 224 MB |
| | 32-bit | 40.8 -> 40.9 | 269 -> 292 MB |

The frame rate is the same within the runs' spread (4 MB blocks: the same), frames over 50 ms the same.
It changes where Unity's allocations lie and how much freed memory its allocators keep, not what the
engine computes: the picture and play are the same. Not yet a default: measured on one game (6000.0),
and the launch-to-scene time of these runs varies by up to 8 s either way, which a load-heavy route
should settle first (the G350's loads especially). The setting for a port's `port.env`:
`UA_BOOT_CONFIG='memorysetup-main-allocator-block-size=1048576 memorysetup-thread-allocator-block-size=1048576 memorysetup-gfx-main-allocator-block-size=1048576 memorysetup-gfx-thread-allocator-block-size=1048576'`.

### Where that leaves the 64-bit build

Per lead:

- **8-byte granules**: the crash was ours (a broken build), not the player's; 6000.0's default now:
  6.9 MB less live heap, the frame rate the same.
- **libunity's +0.9 ms**: spread over the engine's pointer-walking functions (14 of 2,191 differ by more
  than 0.02 ms, together +0.85 ms), plus 0.3 ms of the NDK's outline atomics; nothing of it passes
  through the bridge, whose share is 0.05 ms. No call pattern of ours to change.
- **libmali +0.9 ms**: in the draw calls, pass changes and clears themselves, the same calls on both
  builds; the render thread is not the 64-bit frame's limit (a core of its own: no change).
- **Another arm64 player**: none with Mono for Linux or Android in 2017.4, 2021.3 or 6000.0.
- **Managed code** +0.44 ms: the same methods leading on both builds (PlayMaker's FSM updates); the
  runtime's invoke path is already 0.7 ms cheaper than the 32-bit libmono's.
- **Unity's allocator blocks** (the largest memory gain this pass, both builds: below), thread
  placement (both builds, Knulli only), the malloc settings of ua_host (PR #22: on these handhelds
  Hollow Knight's glibc memory is the same with and without them; the G350 12 - 20 MB more).

Hollow Knight at King's Pass, the 1 GB settings, 25 s runs, the same assets held by both builds:

| handheld | build | fps | lowest available |
|---|---|---|---|
| RG34XX-SP (muOS) | 32-bit today | 53.0 - 53.2 | 303 - 305 MB |
| | 64-bit, start of this pass | 50.9 - 51.2 | 219 - 229 MB |
| | 64-bit, 8-byte granules + 1 MB Unity blocks | 51.0 | 271 MB |
| RG34XX-SP (Knulli) | 32-bit today | 53.8 - 54.2 | 299 - 302 MB |
| | 64-bit, start of this pass | 50.6 - 51.3 | 216 - 228 MB |
| | 64-bit, 8-byte granules + 1 MB Unity blocks | 50.8, 51.2 | 268, 270 MB |
| BattleXP G350 (dArkOS) | 32-bit today (CJK fonts held) | 40.1 - 40.8 | 269 MB |
| | 64-bit, 8-byte granules (fonts given back) | 39.3, 39.4 | 202 - 211 MB |
| | 64-bit, 8-byte granules + 1 MB Unity blocks | 39.6, 38.9 | 224, 224 MB |

The 64-bit build's memory gap on the RG34XX-SP is down from 75 - 80 MB to about 30 MB, and on the G350
the 64-bit build now leaves 224 MB where it left 150 - 190 when it stopped the handheld. The frame rate
is where it was: 1.5 % behind on muOS once warm, 4 - 5 % on Knulli and the G350, and every part of that
measured above is Unity's arm64 engine, its NDK atomics, the JIT's code on larger objects and the
64-bit Mali driver: none of it reached through the runtime. The 1 MB blocks are Unity's, so the 32-bit
build gains from them too (+12 to +56 MB): what makes the aarch64 build better also makes the armhf one
better, and the order between them stays.

## Faster code for the game's scripts: four code generators measured on the handheld (2026-11-20)

The question: could another code generator make the game's managed code so much faster that the 64-bit
build overtakes the 32-bit one (UnityMain 18.4 - 18.6 against 16.4 - 17.0 ms a frame at King's Pass on
muOS; the JIT's code 2.95 ms of it)? Measured with a benchmark, not by porting the game to each.

**The benchmark** (kept outside the repository, with no game code in it): one IL assembly of 16
kernels written after the managed methods that lead UnityMain's samples (`p64-main`, `UA_PROF`):
PlayMaker's `Fsm.Update` / `UpdateDelayedEvents` / `UpdateState` / `UpdateStateChanges` /
`FsmState.OnUpdate` with their structure taken from PlayMaker.dll's IL (300 FSMs, 3 - 8 states, seven
action kinds, delayed events, the execution stack), `List<T>.get_Item` loops, UnityEngine.Object's `==`
/ `IsNativeObjectAlive` on live and destroyed objects, tk2d's `UpdateAnimation` and
`UpdateCameraMatrix` (Matrix4x4), Vector3 integration, InControl's axis update, `Dictionary<string,
_>` lookups, interface calls and `foreach`, delegates, `stelemref`, a pointer walk, strings, small
allocations, a P/Invoke (an internal call's transition) and a native-to-managed callback (an invoke).
They are stand-ins written from the game's patterns, not the game's code. The same IL ran under each
route on CPU 3 (`taskset`), the clock recorded (1.8 GHz on the RG34XX-SP, 1.512 GHz on the G350 in
every run; the governors left alone); per kernel the median of 7 (G350: 5) timed calls after 0.6 s of
warm-up, two passes in opposite orders on muOS (within 3 % of each other).

**Routes**: our Mono (`build-mono.sh` 6000.0, 8-byte granules, JIT, as the port ships it); upstream Mono
6.12.0.200 from mono-project's Debian arm64 packages as the LLVM proxy (Unity's branch needs the
dotnet/llvm-project fork built with it): its JIT, its LLVM JIT (`--llvm`) and LLVM AOT
(`--aot=llvm` of mscorlib, System.Core and the benchmark, `-fp-contract=off`; mono-llvm-tools' llc
needed two newer flags dropped and libmono-llvm three symbols the runtime does not export, all no-ops);
.NET 10.0.15 (CoreCLR, RyuJIT; the Terraria port's runtime): tiered (default), `TieredPGO=0`,
`TieredCompilation=0`, ReadyToRun (crossgen2); IL2CPP: Unity 6000.0.61f1's (2022.3.62f3 converter,
6000.0's libil2cpp and Boehm, Android's baselib.a) built for aarch64 Linux with clang 19 at `-Os` (what
Unity's Android build uses: every compile command of Hollow Knight's build has `-Os`) and `-O2`.

Time relative to our Mono's JIT (lower is faster), RG34XX-SP (muOS) / G350 where they differ:

| kernel | our Mono, ms | Mono 6.12 JIT | Mono LLVM JIT | Mono LLVM AOT | .NET 10 tiered | .NET 10 TC=0 | .NET 10 R2R | IL2CPP -Os | IL2CPP -O2 |
|---|---|---|---|---|---|---|---|---|---|
| fsm_frame (PlayMaker) | 55.3 | 1.11 | 0.79 | 0.84 | 1.97 / 1.49 | 0.77 | 1.20 | 0.66 / 0.82 | 0.59 |
| list_get_item | 54.9 | 1.12 | 0.56 | 0.56 | 0.22 | 0.33 | 0.89 | 0.44 | 0.37 |
| unity_null | 38.3 | 1.15 | 0.60 | 0.47 | 0.35 | 0.44 | 0.80 | 0.49 | 0.31 |
| dict_string | 36.1 | 1.10 | 0.73 | 0.80 | 0.54 | 0.54 | 0.59 | 0.64 | 0.63 |
| tk2d_anim | 22.1 | 1.00 | 0.55 | 0.56 | 0.54 | 0.60 | 0.61 | 0.81 | 0.81 |
| matrix4x4 | 283.9 | 0.96 | 0.13 | 0.21 | 0.39 | 0.20 | 0.22 | 0.05 | 0.05 |
| vector3 | 77.0 | 1.02 | 0.14 | 0.12 | 0.16 | 0.16 | 0.20 | 0.16 | 0.15 |
| incontrol | 94.3 | 0.99 | 0.19 | 0.20 | 0.20 | 0.27 | 0.27 | 0.28 | 0.22 |
| interface | 236.4 | 0.98 | 0.73 | 0.93 | 0.51 | 0.74 | 0.88 | 0.89 | 0.89 |
| delegate | 190.3 | 1.00 | 0.51 | 0.81 | 0.73 | 0.57 | 0.79 | 0.57 | 0.57 |
| stelemref | 80.1 | 1.06 | 0.49 | 0.88 | 1.25 | 1.37 | 1.46 | 1.37 | 1.49 |
| graph_walk | 16.4 | 0.58 | 0.47 | 0.48 | 1.13 | 1.49 | 0.99 | 2.66 | 2.30 |
| string | 215.4 | 0.51 | 0.24 | 0.26 | 0.18 | 0.22 | 0.42 | 0.69 | 0.67 |
| alloc | 946.8 | 0.28 | 0.26 | 0.28 | 0.19 | 0.19 | 0.20 | 1.12 | 1.10 |
| pinvoke (an internal call) | 38.9 | 2.87 | 2.86 | 2.88 | 0.36 | 0.37 | 1.90 | 0.40 | 0.23 |
| callback (an invoke) | 26.9 | 0.96 | 0.96 | 0.96 | 1.00 | 0.96 | 0.96 | 0.36 | 0.36 |
| **geomean, the 6 PlayMaker-like kernels** (fsm, list, null, stelemref, interface, delegate) | 1.00 | 1.07 / 1.05 | 0.60 / 0.61 | 0.72 / 0.71 | 0.64 / 0.59 | 0.64 / 0.67 | 0.97 / 1.01 | **0.68 / 0.72** | 0.61 / 0.60 |
| geomean, the 14 compute kernels | 1.00 | 0.86 / 0.81 | 0.39 | 0.44 / 0.42 | 0.44 / 0.39 | 0.44 / 0.43 | 0.56 / 0.57 | 0.55 / 0.52 | 0.51 / 0.47 |

The PlayMaker-like geomean is the one to scale the game by: for IL2CPP at `-Os` it gives 0.68, and in the
game the IL2CPP build's code and runtime took 3.4 ms a frame against the Mono route's 4.9 (0.69, above:
the 2026-10-24 profile). The struct-math kernels (Matrix4x4, Vector3, InControl) are 5 - 20x faster under
any optimizing compiler, but in this scene they are a small part of the 2.95 ms; the frame's managed time
is PlayMaker's pointer walking, interface/virtual calls and list indexing, where the gains are 30 - 40 %.
The same floating-point results everywhere (Matrix4x4's checksum identical on all routes).
Side results: our Mono allocates 3.5 - 4.5x slower than Mono 6.12 (Unity's Boehm glue has no managed
allocator: every `new` is a call into `GC_malloc`; IL2CPP's is the same, 1.1x; the Mono 6.12 runs are its
SGen collector, the `--gc=boehm` run too, as Debian's package has no `mono-boehm`; in the game it does not
matter: [below](#allocation-in-the-game-the-managed-allocator-lead-does-not-matter-2026-11-20)), a P/Invoke costs our Mono
40 ns and .NET 14 ns; Mono 6.12's armhf JIT on the same handheld takes 0.90 - 0.93 of its arm64 time on
the pointer-walking kernels (the 32-bit build's advantage in the game, 2.61 against 2.95 ms) and 1.2 - 2x
longer on everything else.

**Compile cost and memory** (muOS): compiling all of Hollow Knight's Assembly-CSharp (17,220 methods,
1.4 MB of IL) in the process: our Mono 5.6 s (0.33 ms a method), RyuJIT tier 0 5.7 s (0.33 ms),
RyuJIT fully optimized 18.8 s (1.09 ms); Mono's LLVM JIT compiled the 16 kernels in 2.0 s against our
JIT's 0.06 s (25 - 35x). Peak resident memory of the runtime with the benchmark's setup only: our Mono 16
MB, Mono 6.12 17 - 18, .NET 10 32 (R2R 28), IL2CPP 14; with all of Assembly-CSharp compiled: our Mono
66 MB, .NET 10 110 - 115 MB. Ahead-of-time cost on a Mac (M3, arm64 container): Mono 6.12's LLVM AOT of
Assembly-CSharp 34 s (25 s CPU, peak 586 MB; mscorlib 38 - 52 s), its plain AOT 3 s; IL2CPP of the whole
game (2026-11-04, `hk60/il2cpp`): 7.2 min converting (4.2 min CPU, peak 1.9 GB) and 20 min compiling with
6 jobs (35 min CPU, peak 869 MB). On a 1.5 - 1.8 GHz A53 (1/15 - 1/20 of an M3 core) that is about 10 - 13
h of CPU for IL2CPP (3 - 6 h with up to four compiles) with a conversion that needs more memory than a 1
GB handheld has, and 30 - 45 min for LLVM AOT of the game's and the class libraries' assemblies (llc
near 600 MB).

**What it would give Hollow Knight** (2.95 ms of JIT code a frame on muOS; the 32-bit build is 1.5 - 2
ms of UnityMain ahead):

| route | managed code | saved a frame | what it adds a frame | engineering | verdict |
|---|---|---|---|---|---|
| Mono LLVM AOT at setup | 0.72 | about 0.8 ms | nothing new (the same bridge) | Unity's Mono branch built with the dotnet/llvm-project fork; opt/llc and a linker on the handheld; 30 - 45 min of setup, ~600 MB peak | the realistic one, short of the gap |
| Mono LLVM JIT | 0.60 | about 1.2 ms | compiling 25 - 35x slower: the 2,711 methods `UA_JIT_WARM` compiles in 2.8 s would take over a minute, and every first call later stalls | the same LLVM build in the runtime (+21 MB of code) | no: hitches |
| .NET 10 (CoreCLR) | 0.64 (TC=0) | about 1.0 ms | invokes (712 a frame at ~90 ns, as Mono), cheaper internal calls (14 against 40 ns) | a new bridge; blocked by object layout (below) | not possible behind the IL2CPP player |
| IL2CPP | 0.68 | about 0.9 - 1.2 ms (measured in the game: 1.2) | none | exists (`pmu il2cpp`, `--on-device`) | 10+ h of handheld CPU and 1.9 GB to convert; +140 MB resident in the game |

None closes the gap alone: the best usable one (LLVM AOT) brings UnityMain from about 18.5 to 17.7 ms
against the 32-bit build's 16.7, because most of the gap is Unity's arm64 engine and the Mali driver
(above), not the scripts.

**CoreCLR in place of Mono, the facts**:
- *Hosting*: `libcoreclr.so`'s `coreclr_initialize` / `coreclr_create_delegate` / `coreclr_execute_assembly`
  (coreclrhost.h) or hostfxr, with a trusted-platform-assembly list; the Terraria port already runs .NET
  10.0.15 for linux-arm64 (glibc) from a squashfs on all three handhelds (muOS glibc 2.44, Knulli, dArkOS
  2.41).
- *Object layout, the blocker*: libunity's IL2CPP scripting layer is compiled against IL2CPP's object
  layout and reads objects in place (`GetCachedPtrFromScriptingWrapper` loads a field at a fixed offset;
  `kIl2CppSizeOfArray` and the string layout are inlined): an object header of two pointers (class,
  monitor) with fields from offset 16, arrays with `bounds` and `max_length` and elements from 32,
  strings' characters from 20. Mono's layout is the same, which is why the bridge works. CoreCLR's object
  has one pointer (the MethodTable; the sync block sits before it), fields from 8, arrays' elements from
  16, strings' characters from 12, fixed in the runtime. A CoreCLR behind the IL2CPP player would read
  every field of every UnityEngine object 8 bytes off.
- *Internal calls*: CoreCLR has no `mono_add_internal_call`; its InternalCall table is compiled in for
  CoreLib only. Unity's public fork (github.com/Unity-Technologies/runtime, MIT, default branch
  `unity-main`, its last commit 2025-04-04) had a Mono-API layer over CoreCLR (`src/coreclr/vm/mono/mono_coreclr.cpp`,
  `MonoFunctionsClr.h`) with registered "dynamic ecalls", and a 2021 branch `enable-conservative-gc`;
  PRs #238 - #268 (2024) removed most of that layer and the ecall registration (#239), as Unity's engine
  moved to generated P/Invoke-like bindings. What remains is profiler, class-info and invoke functions,
  not an embedding API for a Unity 6000.0 player.
- *GC*: CoreCLR's collector is precise and compacting; `gcConservative` scans stacks conservatively and
  pins what they reference, but heap objects still move, and there is no non-moving mode. The IL2CPP
  player keeps raw object pointers across calls (Boehm never moves anything), so even with the layout
  solved it would need every such pointer pinned or behind a handle.
- Unity's own CoreCLR player (announced for a later Unity 6 release) is not a player Hollow Knight's
  6000.0 data can use.

**Next step, if any**: the Mono LLVM AOT route is the only one that fits the bridge: build Unity's
6000.0 Mono branch with its LLVM (`--enable-llvm`, the dotnet/llvm-project fork) in `tools/mono64`, AOT
PlayMaker, Assembly-CSharp and mscorlib in the arm64 container, and measure King's Pass with those
images (`UA_MONO64_MANAGED`) before deciding whether the handheld could build them at setup. Expected
about -0.8 ms of UnityMain: a 64-bit build that is still behind the 32-bit one on frame rate.

### Allocation in the game: the managed-allocator lead does not matter (2026-11-20)

The benchmark's allocation kernel (a small object, a boxed int, a short array, a closure and its
delegate a step: 2 million objects) takes our Mono 0.47 us an object against 0.13 for Mono 6.12. Measured in
the games before building anything (`UA_ALLOC_LOG`, docs/runtime.md: Mono's allocation event, every object
the runtime makes; `UA_PROF` for the time):

| | objects a frame (UnityMain) | KB a frame | UnityMain in the allocation path (`UA_PROF` 500 Hz) |
|---|---|---|---|
| Hollow Knight, King's Pass, 32-bit (Unity's armv7 libmono), muOS | 35 - 37 | 2.2 | under 0.01 ms (3 of 20,436 samples, scanned) |
| Hollow Knight, King's Pass, 64-bit (our Mono, 8-byte granules), muOS | 36 - 37 | 2.7 - 2.8 | 0.04 - 0.05 ms (64 samples unwound through `ves_icall_object_new_specific`, 0.3 %; self in `GC_*` 0.2 %) |
| Cuphead, Veggies fight, 64-bit (Mono 5.0.1, `unity-2017.4-mbe`), Knulli | 19 - 21 | 1.1 | 0.05 ms (30 samples, 0.3 %; the game at 59.6 fps) |

What the frame allocates is the game's own garbage: at King's Pass 9 strings, 6 `GUILayoutGroup` + 6 `List`
(an `OnGUI`), 6 `Collision2D` + 6 `ContactPoint2D[]` (Physics2D's callbacks), 3 `WaitForFixedUpdate`;
Cuphead's 4 strings, 2 `Type[]`, 2 boxed floats and its collision delegates. The same count on both builds;
the 64-bit objects are larger (pointers: a `List` 40 bytes against 24). At ~1 us an object in the game (the
profile's 0.04 ms over 36) even a free allocation would give back 0.05 ms of an 18.5 ms frame (0.3 %), under
the run-to-run noise: not built. A scene load is where allocation counts (King's Pass's: 0.85 - 0.92
million objects, 29 frames on 32-bit, 128 on 64-bit), about 0.3 - 0.9 s of it at these costs; not measured
further.

What a managed allocator would have been: upstream Mono has none for Boehm on ARM. Mono 5.0's
`boehm-gc.c` (Unity's 2017.4 branch) has one only for x86/x86-64 with the old in-tree libgc
(`!HAVE_BDWGC_GC`), and its `mono_gc_get_managed_allocator` returns NULL first ("Tls implementation
changed"); Mono 6 removed it (Unity's 6000.0 branch keeps the stub and the note "the code was effectively
dead"). The fast path that is there is Boehm's own: our bdwgc is built with thread-local allocation
(`THREAD_LOCAL_ALLOC`, `USE_COMPILER_TLS`; the profile shows `GC_malloc_kind` and `GC_gcj_malloc` refilled by
`GC_generic_malloc_many`), and Hollow Knight's collector is the stop-the-world one (`[mono64] Mono up: ...
stop-the-world collector`). A game whose boot.config asks for the incremental one (`gc-max-time-slice`, which
the bridge passes on) allocates through Boehm's locked `GC_core_*` paths instead (bdwgc's thread-local
allocators step aside in incremental mode): not measured, no such game tested yet. The cost above it is the JIT's call through the `__icall_wrapper_ves_icall_object_new_specific`
wrapper (an LMF push and pop), `mono_object_new_specific`, `mono_gc_alloc_obj` and the TLS lookup.
Upstream's 3.5x is SGen's bump-pointer nursery and its cheaper collections, not a Boehm path to port.


6000.0 Mono branch with its LLVM, AOT the game's assemblies and measure King's Pass with those images
before deciding whether the handheld could build them at setup. Done the same day (below): in the game
it is no faster than the JIT.

## Mono's LLVM back end, measured in the game (2026-11-20)

**The build** (`MONO64_LLVM=1 tools/mono64/build-mono.sh 6000.0 <work>` -> `<work>/out/unity-6000.0-mbe-llvm/`):
Unity's branch names its LLVM as a submodule, `external/llvm-project` = dotnet/llvm-project
`7dfdea1` (2021-01-15, LLVM 6.0 with Mono's changes, `llvm-config --mono-api-version`). The script
fetches that commit's `llvm/` (without its tests), builds it as `mono/llvm/build.mk` does (release, no
assertions, `opt;llc;llvm-config;llvm-dis`) with only the AArch64 back end, then the runtime
`--enable-llvm --with-llvm=<that prefix>` with the same collector flags as the plain build, in a docker
volume of its own (`ua-mono64-llvm`, about 3 GB, a plain build's volume is not touched). About 70 minutes
on an M3's 8 cores in Docker (LLVM 60 of them). Two fixes: Unity's AOT compiler passes llc
`-disable-fault-maps`, an option of a later fork commit than the one it names, so the script adds it to
llc as an accepted no-op (LLVM 6 lowers fault maps only for x86: nothing is lost); and libmono names
libstdc++ (`LIBS=-lstdc++`; libtool's C link leaves it out, and `mono_main --aot=llvm` then stops on
`__cxa_pure_virtual`). Out: `llc` 24 MB, `opt` 28 MB (libstdc++ and libgcc linked in), the LLVM runtime
27 MB (stripped; the plain one 4.2 MB). Its version is the plain runtime's (`6.13.0 (tarball)`), so the
images it compiles load in the runtime the port ships: the LLVM runtime is only the compiler.

**Loading the images**: `UA_MONO64_AOT=<dir>` (the bridge, runtime/mono64): Mono probes
`<assembly>.dll.so` beside the assembly and then through its dl fallback, which the bridge now answers
from that folder with glibc's dlopen (and dlsym for those handles); without it nothing changes. So the
images can live outside the game's Managed folder, and a stale image is safe: Mono takes one only if
the assembly's GUID and the runtime version match, and JITs the assembly otherwise.

**Compiling** (`--aot=llvm,llvm-path=<dir>/,llvmllc=-fp-contract=off`; Mono runs `opt -O2` and llc,
then GNU as and ld for its own tables and trampolines): in the Mac's arm64 container, Hollow Knight's
assemblies:

| assembly | methods (LLVM) | time | peak memory | image |
|---|---|---|---|---|
| mscorlib | 24,635 | 46 s | 603 MB | 13.1 MB |
| Assembly-CSharp | 17,436 (99 % LLVM) | 31 s | 552 MB | 9.1 MB |
| System | 15,924 | 39 s | 417 MB | 9.9 MB |
| System.Core | 7,002 | 34 s | 215 MB | 5.3 MB |
| PlayMaker, -firstpass, TK2D, UnityEngine.UI, TextMeshPro | 6,395 | 14 s | 120 MB | 4.1 MB |
| UnityEngine.CoreModule | - | stops (internal calls not registered at compile time, as with plain AOT) | | |

Together 165 s and 40.5 MB of images (Assembly-CSharp's plain-AOT image is 13.7 MB). In Assembly-CSharp's
image: no direct calls (every call through the GOT/PLT, as plain AOT), and opt adds safepoint polls at
every loop back edge (`-place-safepoints -spp-all-backedges`, fixed in Mono's compiler).

**Exactness**: a floating-point checker (bit-exact FNV hashes of a*b+c chains a fused multiply-add would
round once, Vector3 integration with Lerp/Dot/Cross/sqrt, a 4x4 matrix chain, Math.Sin/Sqrt/Atan2 and
conversions, timers adding float deltas into doubles, denormals, infinities, NaN compares) under the
JIT and under the LLVM images, same runtime, on muOS: every floating-point hash equal. A control
compiled `-fp-contract=fast` changes three of them, and its image has 75 fmadd/fmsub instructions; the
`-fp-contract=off` images of all ten assemblies above (4.1 M instructions) have none. One difference,
not floating point: a float outside int's range converted to int (`(int)3e9f`): the 64-bit JIT
converts through 64 bits and keeps the low half (-1,294,967,296), LLVM saturates (2,147,483,647), as
the 32-bit Mono JIT (`vcvt`) and IL2CPP's arm64 code (`fcvtzs w`) do; C# leaves it unspecified. In the
game (the save loaded, the Knight standing in King's Pass; no movement scripted) the screenshots show
the same frame, the Knight at the same place, apart from drifting particles (5.8 - 6.5 % of pixels
differ, as between two JIT runs), and the logs the same lines.

**The kernels** (jitbench, our runtime, muOS, CPU 3, median of 5): LLVM images against the JIT:
fsm_frame 0.75, list_get_item 0.51, unity_null 0.41, dict_string 0.64, tk2d_anim 0.57, matrix4x4 0.21,
vector3 0.16, incontrol 0.20, interface 0.97, delegate 0.82, stelemref 0.80, string 0.72, alloc,
pinvoke, callback 1.0; the PlayMaker-like geomean **0.68** (Mono 6.12's LLVM AOT in the earlier table:
0.72).

**Hollow Knight, King's Pass** (UA_JIT_WARM on, the release's settings, JIT A B B A in one hold,
both with the new bridge):

| | fps | UnityMain ms | available MB | JIT warm-up |
|---|---|---|---|---|
| muOS, JIT | 51.4, 51.2 | 18.5, 18.3 | 228, 226 | 1.8, 1.9 s (3,435 methods already compiled) |
| muOS, LLVM AOT | 51.5, 51.1 | 18.3, 18.4 | 232, 231 | 0.7, 0.7 s (4,783 already compiled) |
| Knulli, JIT | 50.8, 51.1 | 19.3, 19.1 | 186, 186 | 2.6, 3.1 s |
| Knulli, LLVM AOT | 51.1, 51.3 | 19.1, 19.0 | 185, 185 | 1.7, 1.0 s |
| Knulli, 32-bit port | 53.6 | 18.1 | 211 | |

The frame rate is the same within run-to-run noise (+0.1 ms at most), memory +4 MB on muOS. A 500 Hz
profile of UnityMain on Knulli says why: managed code is 15.6 % of UnityMain's CPU time under the JIT
and 15.9 % with the images (10.7 % in the images, 5.2 % still JIT code: generic instances, wrappers and
UnityEngine.CoreModule); PlayMaker alone 4.4 % JIT against 5.2 % in its image. The kernels' gains do not
carry over: the frame's managed time is PlayMaker walking its object graph (cache misses that any code
generator waits for) and calls, which in an image all go through the GOT and PLT, against direct
branches in the JIT's code (the 0.3 ms plain AOT lost); LLVM's better code wins back about that much and
no more.

**Verdict: not a setup step.** It would cost the setup (below) and a 50 MB toolchain for no frame-rate
gain; the warm-up is 1 - 2 s shorter, which UA_JIT_WARM already hides behind the first frames. The build
option and `UA_MONO64_AOT` stay for measurements.

**What a setup step would have needed** (for the record): `opt`, `llc`, the LLVM runtime (one LLVM
inside each; as one `libLLVM.so` about 35 MB instead of 79), GNU as and ld (binutils, GPL-3: separate
programs, shippable as binaries with their source), monohost; about 82 MB, 35 MB compressed; the images
compiled from the player's own assemblies into a folder of the port's, `UA_MONO64_AOT` set by the
launcher, the JIT the fallback for anything missing or stale. On the handheld:
measured on the RG34XX-SP under Knulli (H700, 1 GB, zram on), PlayMaker 38 s and Assembly-CSharp
396 s (15x and 13x the Mac's), the compiler process alone 505 MB resident (Mono holds the whole
assembly's LLVM module before opt and llc run), MemAvailable down to 42 MB with 120 MB pushed to zram.
Scaled by the Mac's times, all ten assemblies would take about 35 - 40 minutes, and mscorlib's 603 MB
peak (System 417 MB) does not fit beside the frontend on a 1 GB handheld without swap (the G350, which
did not answer during this test): it would need assemblies split into parts (Mono's `--aot` has no such
option) or the class libraries left to the JIT. Not run in parallel with the textures step: either alone
takes most of the memory.
