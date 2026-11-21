# 64-bit ARM (arm64-v8a) in the loader

## Real players

Unity's arm64-v8a players exist only with IL2CPP, and a game's own Android build carries them
(`libmain.so`, `libunity.so`, `libil2cpp.so` in `lib/arm64-v8a/`): `pmunity apk` makes such a
build ready for the aarch64 `ua_host` (docs/apk.md). Free IL2CPP Android builds of Unity 2020.3.3,
2021.3.4 (with Burst) and 2022.3.14 (Burst, FMOD, a 3D scene) reach their title menus in the
emulated rig (`EMU_ARCH=arm64 tools/emu/emu.sh`: a native aarch64 container on an Apple-silicon
Mac, Mesa llvmpipe, the `ua-build-arm64` image), with no unresolved imports. What the bring-up
needed was outside the loader:
- **frame pacing**: their builds have Optimized Frame Pacing on; with Swappy kept off (the
  converter's choice for desktop data, where the setting is turned off) 2021.3 stops after 3
  frames. Imported builds run Swappy; its Java `ChoreographerCallback` ticks on a looper thread of
  its own (runtime/jni), not on UnityMain, which waits inside `nativeRender` for those ticks (on
  UnityMain the frame time grew without bound: 130 ms to 480 ms per frame within 90 s).
- **API level**: the 64-bit host reports API 22 for converted data (64-bit Android starts at 21), an
  imported build its `targetSdkVersion` (at most 33, at least its `minSdkVersion`; docs/apk.md). From
  API 24 Swappy ticks on the NDK's `AChoreographer` (runtime/android/choreographer.c); before that
  existed here, 2022.3 told API 26 waited for vsyncs forever, so builds were held at 23.
- **IL2CPP startup**: `libil2cpp.so` comes up with libunity through libmain's `NativeLoader.load`
  (its `JNI_OnLoad` runs right after libmain's), libunity extracts `Managed/Metadata` and
  `Managed/Resources` to `<external files>/il2cpp/` (`data/external/il2cpp` under the root) and
  binds the `il2cpp_*` API; Burst's `lib_burst_generated.so` arrives through `System.load`: all as
  on a phone.
- **symbols**: IL2CPP builds with engine code stripping (the default) link their own
  `libunity.so` from Unity's static libraries, so its build id differs from the player package's
  and `pmunity player fetch --symbols-only` symbols do not fit it (only the exported names do);
  Mono builds and IL2CPP builds without stripping use the prebuilt library the symbols belong to.
- 2021.3+ builds list Vulkan first; `libvulkan.so` is mapped to nothing, so the player takes
  GLES3, as on a phone without Vulkan.

The loader itself needed nothing for these libraries: their relocations, the guard slot, emulated
TLS and the LP64 bionic types held (whether a C++ exception crossed the unwinder in these runs is
not known). Not run yet: 2017.4 and 2018.4 IL2CPP arm64 builds (none in the corpus: the one 2018.4
build found was a VR title), 2019.4 IL2CPP arm64, 16 KiB-page kernels. The 2020.3, 2021.3 and 2022.3
builds have since reached their titles on a handheld's Mali driver with the 64-bit host
(compat/README.md, "On a handheld").

`runtime/loader` loads the Android libraries of its own build's architecture: armeabi-v7a on
armhf (`runtime/loader/build.sh`, output `out/`) and arm64-v8a on aarch64 glibc
(`ARCH=arm64 runtime/loader/build.sh`, output `out/arm64/`, built natively in the
`ua-build-arm64` image). Symbol resolution, shim tables and dlopen routing are the same on both;
this page is about what differs.

## ELF and relocations
- ELF64 / `EM_AARCH64`; the GNU hash Bloom filter has 64-bit words there.
- RELA tables (`DT_RELA`, and `DT_JMPREL` with `DT_PLTREL = DT_RELA`): `R_AARCH64_ABS64`,
  `GLOB_DAT`, `JUMP_SLOT` (addend included, as bionic does), `RELATIVE`, `IRELATIVE`.
- Both architectures also take the formats newer NDKs write: Android's packed relocations
  (`DT_ANDROID_REL` / `DT_ANDROID_RELA`, "APS2" + SLEB128 groups) and RELR (`DT_RELR`, and the
  older `DT_ANDROID_RELR` tags), applied in the Android linker's order: packed, RELR, REL/RELA,
  PLT. A table that runs out early or points outside the image fails the load.
- `IRELATIVE` relocations and references to a library's own IFUNC symbols are resolved after
  the library's segments get their final protections (the resolver is its code), with bionic's
  arguments (`AT_HWCAP | _IFUNC_ARG_HWCAP`, `__ifunc_arg_t`).
- ELF TLS (`PT_TLS`, the TLS relocations) is refused: NDK code for API levels below 29 uses
  emulated TLS (`__emutls_get_address`, inside each library, on pthread keys).
- 4 KiB pages only (a 16 or 64 KiB page kernel fails the load with a message).

## The stack guard slot (TLS)
bionic keeps per-thread slots at `tpidr_el0`; NDK code compiled with the stack protector loads
its canary from slot 5, `tpidr_el0 + 0x28`, on entry to and exit from every protected function
(clang has done so for Android/AArch64 for years; GCC needs `-mstack-protector-guard=sysreg
-mstack-protector-guard-reg=tpidr_el0 -mstack-protector-guard-offset=40`). Older GCC-built
libraries read the global `__stack_chk_guard` instead, which stays a shim, as on 32-bit ARM.

On glibc, `tpidr_el0` points at the 16-byte TCB (`dtv`, an unused word) and then the
executable's static TLS block, then the libraries' blocks (libc's errno among them). If
`+0x28` were some library's variable, the canary would change under running functions and they
would abort in `__stack_chk_fail`. So the loader owns that word: `so_loader.c` defines a 48-byte
`__thread` array standing for bionic's slots 2-7 (`tp + 16 .. + 63`), with slot 5 holding a
fixed canary. It is the executable's only initialized thread-local data, so the linker puts it
first (`.tdata` before `.tbss`), and glibc places the executable's block right after the TCB:
the array lands exactly on the slots, in every thread, and each new thread copies the canary
from the TLS image. The value is fixed rather than random because a frame only compares the
canary it stored with the one it reads on return (per thread, during the thread's life), and
the image is read-only after relocation; its low byte is zero, as with glibc's and bionic's.

Rules for the program that links the loader (so_load() checks them and fails with an
explanation):
- `libsoloader.a` is linked into the executable, not into a shared library;
- no other thread-local variable of the executable has an initial value or an alignment above
  16 (`runtime/host`, `runtime/jni` and `runtime/android` only have zero-initialized ones).

The other slots: 0-1 are glibc's TCB (only bionic's libc reads them); 3-4 (OpenGL) belong to
Android's GLES wrappers, 6 to HWASan, 7 to ART; none of these run here, the words are zero.
Android code never uses x18 (reserved there), so glibc code clobbering it is harmless.

## Unwinding
AArch64 has no EHABI: C++ exceptions unwind with `.eh_frame`. arm64 Android libraries carry their
own unwinder (libgcc's or LLVM libunwind, linked in; bionic never exported one on LP64), which
finds frames through `dl_iterate_phdr`. `bionic_compat_table()` routes that (and `dladdr`,
`android_dlopen_ext`) to the loader, which reports its modules first, with `dlpi_adds` counting
its loads so libgcc's FDE cache notices them, then the host's objects: an exception or a
backtrace passes Android and host frames alike. The host's own libgcc_s is not hooked (no
Android code reaches it on arm64); `__gnu_Unwind_Find_exidx` and its link flag are 32-bit only.

## bionic types on LP64 (hybris_compat.c)
| type | bionic arm64 | handling |
|---|---|---|
| `jmp_buf` / `sigjmp_buf` | 32 / 33 longs | `bionic_setjmp.S`: x19-x30, sp, d8-d15, the mask (24 words) |
| `sigset_t` | 8 bytes, signals 1-64 | translated, as are `struct sigaction` (`{flags, handler, mask, restorer}`), the set functions (`sigemptyset`...: glibc's would clear 128 bytes), `sigpending`, `sigwait*`, `sigtimedwait`, `signalfd` |
| `pthread_mutex_t` / `cond` / `rwlock` | 40 / 48 / 56 bytes, 4-byte aligned | a pointer to a glibc object in the first 8-byte aligned word (offset 0 or 4), installed by compare-and-swap; rwlocks and `pthread_mutex_init` moved from libhybris for that |
| `sem_t` | 16 bytes, the first word is the semaphore | the same futex code as 32-bit |
| `struct stat`, `statfs` | the kernel's generic layouts = glibc's | passed through (`stat64` names too) |
| `FILE` / `__sF` | 152-byte `__sFILE` | libhybris (LP64 layout) |
| `mbstate_t` | 8 bytes | per-thread state for `mbrtowc`/`wcrtomb`, as on 32-bit; the string conversions (`mbsrtowcs`, ...) pass it to glibc (same size) |
| `struct passwd`, `struct statvfs` | glibc's layouts (`pw_gecos` present; 11 longs + 24 reserved bytes) | passed through (32-bit ARM translates both) |
| `nftw`/`ftw`, `fnmatch`, `pathconf` | BSD flag values (`FTW_DEPTH` 4 / `FTW_CHDIR` 8, `FTW_DP` 3 / `NS` 4 / `SL` 5; `FNM_NOESCAPE` 1 / `FNM_PATHNAME` 2; `_PC_*` numbered from `_PC_FILESIZEBITS` 0) | translated on both architectures |
| fortify | `__FD_SET_chk`, `__strlen_chk`, ... (bionic-only) | ours, with bionic's checks and messages; the `__*_chk` glibc also has go to glibc |
| `__fpclassify*` | bionic's `FP_*` values | translated (no softfp wrappers on AArch64) |
| dlfcn | `RTLD_GLOBAL` 0x100, `RTLD_DEFAULT` 0, `RTLD_NEXT` -1 | handled |

AAPCS64 is the same on Android and Linux: `so_softfp_wrap()` returns the function unchanged,
`SO_ABI` is empty. Properties default to API 22 and `arm64-v8a`.

## Tests
`ARCH=arm64 runtime/loader/build.sh test` (and `runtime/loader/build.sh test` for armhf) builds
`test_sem`, `test_sync` and `test_syn`. `test_syn` loads synthetic "Android" libraries
(`test/syn_lib.c`, `test/syn_dep.c`: freestanding, `-nostdlib`, stack-protected the Android way,
`DT_NEEDED` libc.so) linked five times and rewritten by `test/pack_relocs.py` into RELR, APS2,
both, and Android's RELR tags; each must relocate to the same image as the plain one. It also
checks imports from shims, host and dependency, init order, dlopen/dlsym, IFUNC, setjmp,
signals, pthread objects at 4 mod 8, `stat`, the guard slot (and that overrunning a buffer
aborts), a corrupt packed table, and (arm64) an unwinder inside a library walking through
Android and host frames. GNU ld 2.34 cannot write packed relocations, so the encoder and the
loader's decoder are both ours: a library from the NDK's lld is still the real proof.

## Shipping aarch64 only
The goal: Unity Mono ports with no armhf binaries, the 32-bit route dropped once the 64-bit one
matches it on the handhelds (docs/mono-arm64.md, "32-bit or 64-bit only"). This section maps every
place that picks, builds, packs or ships an armhf or aarch64 piece, and the switch that makes a
release aarch64 only.

### The switch: `port.aarch64_only`
`port.toml`'s `[port] aarch64_only = true` (`port.build.json`'s `config.port.aarch64_only`; default
false, `AARCH64_ONLY_DEFAULT` in `convert/pmunity/port.py`). Dropping armhf from every port is that
one line set to `True` (then `port new` and `port package` again for each port).
- **`port new`** (`aarch64_only_route()`): a port on the android route (Unity's armv7 Mono player,
  run by the 32-bit `ua_host`) moves to the mono-arm64 route: its templates (launcher, setup), Unity's
  arm64 IL2CPP player from the IL2CPP donor, the game's own assemblies on Unity's Mono for aarch64
  (`setup/mono64/<line>`), the 64-bit `ua_host`. The game data, conversion, settings, questions and
  `[runtime]` stay the same. port.toml's `player.backend`/`abi` and `sha256` (the armv7 player's)
  give way to the route's `il2cpp`/`arm64-v8a`; port.json asks for no `armhf`. A game the mono-arm64
  route cannot take stops `port new` with the check's reason (e.g. a .NET 3.5 game: below). An apk
  route port keeps only its `arm64-v8a` player (or needs `player.il2cpp`); il2cpp-arm64 and box64 are
  aarch64 already.
- **`port package`**: refuses an aarch64-only port whose folder was made for the android route (run
  `port new` again); ships the Mono donor without its `lib/` (`slim_donor`: every entry but
  `lib/<abi>/` of another ABI than arm64-v8a, the same bytes and compression; the setup reads only its
  `assets/bin/Data/` engine assemblies, class libraries and default resources); leaves out the porter's
  `build.outputs` that are EM_ARM files (a file, or files inside an output folder: e.g. Hollow Knight's
  `pmhwdec/` armhf decoder), with a log line; never packs the armhf `ua_host` (`wants()`); takes
  `uaflipbook.dll` from `runtime/host/out/` for the 64-bit `ua_host` (managed code, one build); and
  stops if any EM_ARM file is left in the release, the donors' entries included (`armhf_files`).
- **`pmu arch-check <release zip | folder>`** (setup-rs `crates/pkgzip/src/arch.rs`, docs/rust-setup.md)
  is the full check: no EM_ARM ELF file anywhere (the donor APKs' and OBBs' entries too), and the 64-bit
  route's pieces there, each an ELF64 `EM_AARCH64` file: setup.env's `PORT_ROUTE` mono-arm64 (or
  il2cpp-arm64) with `PLAYER_BACKEND=il2cpp`, `PLAYER_ABI=arm64-v8a`; `setup/patchscript`; the launcher;
  `ua_host`; `setup/pmu` (or the setup's Python); for each Unity version the IL2CPP arm64 donor with
  `lib/arm64-v8a/libmain.so`, `libunity.so`, `libil2cpp.so`, the Mono donor with
  `assets/bin/Data/Managed/UnityEngine.dll`, and `setup/mono64/<line>/libil2cpp.so` (the bridge) and
  `libmonobdwgc-2.0.so`. Its tests (`crates/pkgzip/tests/arch_only.rs`) build such a release from
  synthetic ELF headers and APKs: it passes; the full Mono donor, a stray armhf library, an armhf
  `ua_host`, a missing Mono runtime, donor or engine assembly, and the android route's setup each fail
  it by name, as files and as a zip. `tools/port/rehearse.sh` with `REHEARSE_AARCH64_ONLY=1` runs the
  release's own pmu on it before the setup, and removes the container's 32-bit loader
  (`REHEARSE_64ONLY=1`).
- Dry run (2026-11-20, no handheld): Hollow Knight's port.toml with `aarch64_only = true` (its
  `build.commands` replaced by the released pmhwdec files, both builds): `port new` took the
  mono-arm64 route, `port package` dropped 7 `lib/` files from the Mono donor and the armhf
  `pmhwdec/libpmhwdec.so.1` and `librockchip_mpp.so`; the zip (41 MB) passed `pmu arch-check` (15 ELF
  files, all aarch64); `REHEARSE_AARCH64_ONLY=1 REHEARSE_NET=none tools/port/rehearse.sh` set the game
  up from GOG's Linux files in the container without a 32-bit loader in 53 min (1,860 MB installed, the
  IL2CPP arm64 player and Unity's Mono 6000.0 in `lib/`), and every ELF file of the installed port, the
  APK's entries included, is aarch64 (10). The game itself did not run there (no emulated run).
- With the switch off nothing changes: the android route, `ua_host` for armhf, the full donors
  (`convert/tests/test_port.py` `test_aarch64_only` checks both).

### Where armhf and aarch64 are decided
| place | what it decides | aarch64 only |
|---|---|---|
| `convert/pmunity/port.py` `decide_routes`, `DEFAULTS['port']['route']` | the route `port check` / `port new` recommends: android first for a Mono desktop build, then il2cpp-arm64, mono-arm64 (.NET 4.x only), box64 | `aarch64_only_route()` moves android to mono-arm64 |
| `port_template/<route>/route.json` `package` | the release's parts: android `ua_host` (armhf); mono-arm64 and il2cpp-arm64 `ua_host_arm64` (the 64-bit one named `ua_host`) and `mono64`; apk `ua_host?`, `ua_host64?` by `player.abis` | android's list is never used: the route changes |
| `route.json` `donors`, `port_donors()`, `DONOR_RX` | android: `donor-<v>-mono-armv7.apk`; mono-arm64: the `il2cpp-arm64` one and the `mono-armv7` one (its engine assemblies: an IL2CPP build has none) | the Mono donor ships without `lib/` |
| `port.py` `COMPONENTS` / `wants()` | `_component_ua_host` (runtime/host/out/ua_host, armhf), `_component_ua_host_arm64` (out/arm64/ua_host as `ua_host`), `_component_ua_host64` (as `ua_host64`, the apk route), `_component_mono64` (bridge + Unity's Mono per line), pmu/asmpatch/pmhlslcc/pmspirvcross (always aarch64) | `wants('ua_host')` false |
| `port.py` `port_json()`, route hooks (`mono64.py`, `apk.py` `port_new`) | port.json `reqs` `armhf` (android always; apk with armeabi-v7a only) | no `armhf` |
| `port.py` `package()` `build.outputs` | the porter's own builds, any architecture (pmhwdec's `armhf/`, `arm64/`, `aarch64/`) | EM_ARM outputs left out |
| `port_template/android/launcher.sh` | stops without `/lib/ld-linux-armhf.so.3`; runs `ua_host` (armhf) | not used |
| `port_template/mono-arm64/launcher.sh`, `il2cpp-arm64/launcher.sh` | stop unless `uname -m` is aarch64; run `ua_host` (aarch64) | used |
| `port_template/apk/launcher.sh`, `apk/patchscript` | `t32` where `/lib/ld-linux-armhf.so.3` and `ua_host` exist, else `t64` with `ua_host64` | only `ua_host64` shipped |
| `port_template/*/patchscript` `donor()` | `donor-$V-<backend>-<armv7\|arm64>.apk`; mono-arm64 reads `donor mono armeabi-v7a` for engine assemblies and `pm winbcl --donor` | the slim Mono donor keeps its name |
| `pmu player donor --backend --abi [--managed]` (`crates/pkgzip/src/player.rs`) | the player package from donors: `Libs/<abi>/` only from a donor of the backend and ABI; `--managed` the engine assemblies from any donor | reads no `lib/` of the Mono donor for `arm64-v8a` |
| `pmu apk --target auto\|arm64\|armhf` (`crates/apk/src/lib.rs`) | the apk route's ABI: auto = armeabi-v7a where the system has a 32-bit userland, else arm64-v8a; `--il2cpp` compiles a Mono armv7-only build for arm64 | `player.abis = ["arm64-v8a"]` |
| `pmu android-data --mono64` / `--il2cpp` (`crates/androiddata`) | the APK and OBB for the IL2CPP player with the Mono bridge / IL2CPP metadata | the 64-bit route's |
| `pmu il2cpp --on-device --abi` (`crates/il2cpp`) | il2cpp-arm64: Unity's IL2CPP and player fetched on the handheld | aarch64 already |
| `runtime/build.sh` (`ARCH=armhf` default \| `arm64`), each part's `build.sh` | `host/out/ua_host` (armhf, cross) or `host/out/arm64/ua_host` (native in `ua-build-arm64`); loader, jni, android shims, media per ARCH | `ARCH=arm64` only |
| `runtime/mono64/build.sh`, `tools/mono64/build-mono.sh <line>`, `build-classlibs.sh` | the bridge `libil2cpp_mono.so` and Unity's Mono (and 4.x class libraries) for aarch64 | needed |
| `runtime/host/managed/build.sh` | `uaflipbook.dll` (managed, one for both) | taken from `host/out/` |
| `tools/donor/build.sh` | `MonoArmv7`, `Il2cppArm64` donors (Unity builds no arm64 Mono player) | both still built: the Mono one for its assemblies |
| `tools/pmhwdec/build.sh` | `armhf/`, `arm64/` libpmhwdec, `aarch64/pmhwdec-cedard`; Rockchip MPP armhf only unless `MPP_ARM64=1` | arm64 outputs; `MPP_ARM64=1` for Rockchip |
| `setup-rs/tools/build_pmu.sh`, `tools/asmpatch`, `tools/hlslcc`, `tools/spirvcross` | the setup's tools: aarch64 only already | unchanged |
| `tools/port/rehearse.sh` (`REHEARSE_64ONLY`, `REHEARSE_AARCH64_ONLY`), `tools/emu/emu.sh` (`EMU_ARCH`) | the rehearsal's 32-bit loader, the emulated rig's architecture | `REHEARSE_AARCH64_ONLY=1` |

### The shipped ports
None is made by `port new` + `port package` alone today; each needs its own step before armhf goes.
- **Cuphead** (portmaster-cuphead; own `build.sh`, `build64.sh`, `package.sh`): one zip with both
  builds: `ua_host` (armhf) and `ua_host64`, `setup/` (32-bit setup) and `setup64/` (64-bit:
  `mono64/` with Unity's Mono 2017.4 and Mono's 4.x class libraries, `classlibs/`), the donors
  `donor-2017.4.9f1-mono-armv7.apk` (the game's own version's player, 32-bit route),
  `donor-2017.4.40f1-il2cpp-arm64.apk` and `donor-2017.4.40f1-mono-armv7.apk`; the launcher picks
  `CUPHEAD_ROUTE` (32 where `/lib/ld-linux-armhf.so.3` exists) and sources `launch64.txt`. Left:
  package.sh without `ua_host`, `setup/`, the 2017.4.9f1 donor and with the 2017.4.40f1 Mono donor
  slimmed; the launcher always 64-bit; `pmu arch-check` on the zip.
- **Hollow Knight** (portmaster-hollowknight; android route from `port new`, the 64-bit setup
  `setup/patchscript64` and `build64.sh` hand-merged from the mono-arm64 route): ships `ua_host` and
  `ua_host64`, `pmhwdec/` (armhf libpmhwdec, Rockchip MPP) and `pmhwdec64/`, `setup/mono64/6000.0/`
  through `build.outputs`, both donors; `HK_ROUTE` picks. Left: with `aarch64_only` `port new` would
  write the mono-arm64 template's launcher and setup over the hand-made ones (`.port_files`), so its
  launcher/patchscript64 have to become the port's own files (or its 64-bit parts the template's),
  `pmhwdec/` armhf outputs dropped (package does that), `ua_host64` renamed or the launcher's 64-bit
  path kept; Rockchip hardware decode needs `MPP_ARM64=1`.
- **Blasphemous** (android route, Unity 2017.4.40f1, .NET 3.5): armhf only today (`ua_host`,
  `donor-2017.4.40f1-mono-armv7.apk`, `pmhwdec/` armhf + cedard). Left: the mono-arm64 route refuses
  .NET 3.5 games; Cuphead runs a .NET 3.5 game of 2017.4 on Unity's Mono 2017.4 with Mono's 4.x class
  libraries (docs/mono-arm64.md, "A .NET 3.5 game on the 4.x class libraries"), which the route's
  template and `_component_mono64` do not do yet (build-classlibs.sh, the class library remapping);
  then the IL2CPP arm64 donor of 2017.4.40f1, `aarch64_only = true`, and its FMOD player and
  asmpatch patches checked on the 64-bit player.
- **Enter the Gungeon** (android route, Unity 2017.4.27f1, .NET 3.5): armhf only (`ua_host`,
  `donor-2017.4.27f1-mono-armv7.apk`, `gungeonpatch` is linux-arm64 already). Left: the same .NET 3.5
  step as Blasphemous, an IL2CPP arm64 donor of 2017.4.27f1, and its `UA_MONO_INVOKE`/`UA_MONO_TLS`
  speedups, which are gated to the 2017.4 armv7 libmono, measured on the 64-bit Mono.
