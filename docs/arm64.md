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
  route cannot take stops `port new` with the check's reason (e.g. a .NET 3.5 game of a line other than
  2017.4; a 2017.4 one gets `player.mono64_classlibs`, mono-arm64.md). Files the porter made by hand stay
  as they are where port.toml lists them in `files.own` (a launcher, a setup, READMEs). An apk
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
| `convert/pmunity/port.py` `decide_routes`, `DEFAULTS['port']['route']` | the route `port check` / `port new` recommends: android first for a Mono desktop build, then il2cpp-arm64, mono-arm64 (.NET 4.x; .NET 3.5 on 2017.4 with Mono's 4.x class libraries, `player.mono64_classlibs`), box64 | `aarch64_only_route()` moves android to mono-arm64 |
| `setup-rs/templates/<route>/route.json` `package` | the release's parts: android `ua_host` (armhf); mono-arm64 and il2cpp-arm64 `ua_host_arm64` (the 64-bit one named `ua_host`) and `mono64`; apk `ua_host?`, `ua_host64?` by `player.abis` | android's list is never used: the route changes |
| `route.json` `donors`, `port_donors()`, `DONOR_RX` | android: `donor-<v>-mono-armv7.apk`; mono-arm64: the `il2cpp-arm64` one and the `mono-armv7` one (its engine assemblies: an IL2CPP build has none) | the Mono donor ships without `lib/` |
| `port.py` `COMPONENTS` / `wants()` | `_component_ua_host` (runtime/host/out/ua_host, armhf), `_component_ua_host_arm64` (out/arm64/ua_host as `ua_host`), `_component_ua_host64` (as `ua_host64`, the apk route), `_component_mono64` (bridge + Unity's Mono per line), pmu/asmpatch/pmhlslcc/pmspirvcross (always aarch64) | `wants('ua_host')` false |
| `port.py` `port_json()`, route hooks (`mono64.py`, `apk.py` `port_new`) | port.json `reqs` `armhf` (android always; apk with armeabi-v7a only) | no `armhf` |
| `port.py` `package()` `build.outputs` | the porter's own builds, any architecture (pmhwdec's `armhf/`, `arm64/`, `aarch64/`) | EM_ARM outputs left out |
| `setup-rs/templates/android/launcher.sh` | stops without `/lib/ld-linux-armhf.so.3`; runs `ua_host` (armhf) | not used |
| `setup-rs/templates/mono-arm64/launcher.sh`, `il2cpp-arm64/launcher.sh` | stop unless `uname -m` is aarch64; run `ua_host` (aarch64) | used |
| `setup-rs/templates/apk/launcher.sh`, `apk/patchscript` | `t32` where `/lib/ld-linux-armhf.so.3` and `ua_host` exist, else `t64` with `ua_host64` | only `ua_host64` shipped |
| `setup-rs/templates/*/patchscript` `donor()` | `donor-$V-<backend>-<armv7\|arm64>.apk`; mono-arm64 reads `donor mono armeabi-v7a` for engine assemblies and `pm winbcl --donor` | the slim Mono donor keeps its name |
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
Each has an aarch64-only build ready on its own branch (2026-11-20, not merged or released; nothing pushed):
the release passes `pmu arch-check`, and a launch without the game, an install over an earlier release's
32-bit build and the saves' carry-over were rehearsed in the container (`tools/port/rehearse.sh`,
`REHEARSE_64ONLY=1`). None of them has been set up and played from this build on a handheld yet.
- **Cuphead** (portmaster-cuphead, branch `aarch64-only`): `package.sh` with `AARCH64_ONLY=1` (default 0:
  both builds, as before) ships no `ua_host` for armhf, no 32-bit setup, no `donor-2017.4.9f1-mono-armv7.apk`
  and the 2017.4.40f1 Mono donor without its 32-bit player (`slim_donor`); the 64-bit ua_host as `ua_host`,
  the 64-bit setup as `setup/patchscript` with Unity's Mono 2017.4, the bridge and Mono's 4.x class libraries in
  `setup/mono64/2017.4/` and a `setup/setup.env` naming the route (this route's layout, so `pmu arch-check`
  checks it: `package.sh` runs it on the zip). The launcher's `AARCH64_ONLY=1` line (package.sh writes it)
  takes the 64-bit path always; the game stays in `arm64/` (`.cuphead_ready64`), so a 64-bit build an earlier
  release set up goes on as it is. Zip 58 MB, 9 ELF files, all aarch64.
- **Hollow Knight** (portmaster-hollowknight, branch `aarch64-only`): port.toml `route = "mono-arm64"`,
  `aarch64_only = true`, `ready_marker = ".setup_done64"`, the arm64 player's hashes; its own launcher
  (64-bit only: `ua_host`, no `HK_ROUTE`), setup (the former `setup/patchscript64` as `setup/patchscript`),
  questions and READMEs kept by `files.own` while `port new` writes the rest; the game stays in `arm64/`. The
  64-bit runtime is the route's (`ua_host_arm64`, `mono64`: `build64.sh` is gone); `pmhwdec/` is the arm64
  build with Rockchip MPP for aarch64 (`MPP_ARM64=1`; its source patches now run in ua-host-armhf, whose
  Python has the `Path.write_text(newline=)` they use: ua-build-arm64's 3.8 failed them). The code layout
  list is shipped beside `ua_host` (`relayout/`, `port package` for every 64-bit ua_host). Zip 41.5 MB, 16
  ELF files, all aarch64. On the G350 (dArkOS), the arm64 `pmhwdec-test` with the arm64 MPP finds H.264, HEVC
  and VP8 on the video engine (with the decoder's nodes open, as the launcher opens them).
- **Blasphemous** (portmaster-blasphemous, branch `aarch64-only`): port.toml `route = "mono-arm64"`,
  `aarch64_only = true`, `player.mono64_classlibs = true` (a .NET 3.5 game: the route's 2017.4 support below),
  `.setup_done64`, the 2017.4.40f1 arm64 player's hashes, `pmhwdec/` arm64 with MPP; its trimmed launcher kept
  by `files.own` (64-bit check instead of the armhf one, SDL picks the sound driver, `.setup_done64`). The setup
  is the route's template: the game is installed in the port folder over the 32-bit build's files (the saves in
  `data/` stay where they are). Its FMOD banks are played by ua_host's own FMOD player (fmplay, in both builds)
  and its asmpatch patches change the game's IL, which does not depend on the player; whether they run as
  before on Mono 5 with the 4.x class libraries is what its first 64-bit setup and play test has to show.
  Zip 33.7 MB, 13 ELF files, all aarch64 (its `setup/mono64/2017.4/classlibs/` the eight libraries above).
- **Enter the Gungeon** (Unity 2017.4.27f1): no branch yet. It needs an IL2CPP arm64 donor of 2017.4.27f1,
  which does not exist yet: the 2017.4.27f1 editor in the work folder has the Android player's
  `Variations/il2cpp/Release/Libs/arm64-v8a` (IL2CPP arm64 players start with 2017.4.16f1), so
  `tools/donor/build.sh <editor> Il2cppArm64 <out>` makes it with NDK r13b (not on this machine: Google's
  download) and the 2017.4 SDK and JDK of `donor/tools`; the editor kills an adb server that is not its SDK's,
  so it is built when no handheld is on adb, or with SDK set to the running adb's SDK (build.sh refuses
  otherwise). Then the same port.toml change as Blasphemous. Its `UA_MONO_INVOKE` and `UA_MONO_TLS` do not
  apply to the 64-bit build: both patch Unity's unoptimised 2017.4 armeabi-v7a libmono byte for byte (they
  turn themselves off for any other), and the 64-bit Mono is this repository's -O2 build, whose runtime
  invoke finds a method's info in a lock-free hash already (mono-arm64.md, "Mono's runtime invoke path":
  0.09 ms a frame in Hollow Knight) and reads its thread-local data with the compiler's TLS. What the
  64-bit build costs Gungeon is to be measured once it runs.
- **On the handhelds (2026-11-21):** each port's aarch64-only release (the current Mono builds, 7f504a8's
  runtime) set up from the player's files on the RG34XX-SP, then played silent through
  tools/device/profrun.sh. Setup times are the setup's own time on Knulli, summed over devlock holds:
  | port | setup | installed | gameplay reached | fps (30 s at the point reached), resident memory |
  |---|---|---|---|---|
  | Hollow Knight | 40 min (Knulli) | 1857 MB | a test save's bench | Knulli 59.4 avg / 57.5 min, 563 MB; muOS 60.0 / 59.0, 562 MB (relayout and prefetch on) |
  | Cuphead | 49 min, textures Low (Knulli) | 2.3 GB with the DLC | Botanic Panic (the helper's test level), shooting | Knulli 59.5 / 58.6, 490 MB; muOS 60.0 / 59.6, 498 MB |
  | Blasphemous | about 90 min of setup in one hold (Knulli; setup-speed.md, "A setup stopped often") | 3237 MB | New Game to the first level | Knulli 59.5 steady, one 0.8 s room-load hitch; 736 MB |
  | Enter the Gungeon | 17 min (muOS) | 711 MB | the Breach, from a test save | muOS 59.8 - 60.2, 618 MB |
  muOS played the game set up on Knulli (its card had no room for a second setup). Gungeon's setup first
  stopped in asmpatch's `inline` on the 4.x class libraries (fixed: tools/asmpatch/README.md). No managed
  exception in any run; Blasphemous's FMOD player and its asmpatch patches (catalogs, I2 sources) ran on Mono 5
  with the 4.x class libraries. The G350 was down for these runs.
  **The G350 (dArkOS, 2026-11-21): Hollow Knight's aarch64-only release** set up there from the installed game
  folder in eight holds of nine minutes, 67 minutes of the setup's own time, 1887 MB installed, nothing refused;
  played with the launcher's settings (EmulationStation left running, silent, `tools/device/profrun.sh`): King's
  Pass 48.0 fps over 94 s, 609 MB resident, 217 MB the lowest available; a second King's Pass run 32.5 fps and
  City of Tears 34.8 (601 - 611 MB resident, 212 - 217 MB available): the runs paced at half the refresh rate
  ([arm64-routes.md](arm64-routes.md), the G350's paragraph). The handheld then stopped answering 95 s into the next run (the boss save,
  loading its scene; arm64-routes.md): the other three ports are not tested there yet.
- An earlier release's 32-bit build on the card: the 64-bit setup needs the game's files again (the 32-bit
  setup used them up), and each launcher says so; the saves come over (Cuphead and Hollow Knight copy
  `data/files`, `shared_prefs`, `external` into `arm64/data/` once, where it has none; Blasphemous's stay in
  `data/`). The 32-bit build's own files (its `lib/`, APK, OBB) stay on the card until removed by hand
  (Blasphemous's are replaced by the 64-bit install).
