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
- **API level**: the 64-bit host reports API 22 (64-bit Android starts at 21), an imported build at
  least its `minSdkVersion`, but at most 23: from 24 Swappy uses the NDK's `AChoreographer`, which
  the libandroid shims lack (2022.3 then waits for vsyncs forever).
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
build found was a VR title), 2019.4 IL2CPP arm64, 16 KiB-page kernels, a real handheld's Mali/GPU
driver with the 64-bit host.

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
