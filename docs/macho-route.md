# The Mach-O route: a macOS IL2CPP build's arm64 code on ARM Linux

Status: in progress (first game: Dave the Diver, 2026-11-08). A macOS IL2CPP build of the
project's own test game runs under the Android player in the emulator rig (2026-11-09, below);
no handheld run and no commercial game yet. The setup side (`pmu android-data`) does not have
the route yet: the test's data was put together by hand.

## Why

A desktop IL2CPP build's game code is native (`GameAssembly`), so the Android route cannot
recompile it, and Windows and Linux builds carry it for x86-64 only (box64, docs/box64-route.md).
macOS builds made for Apple silicon are universal: their `GameAssembly.dylib` has an **arm64**
slice, AArch64 code the handhelds' Cortex-A53/A35 cores can run natively. The route keeps the
Android route's engine (Unity's arm64-v8a Android player of the same version, `libunity.so` on
our runtime, as the IL2CPP APK route runs it) and puts the macOS build's own GameAssembly where
the player expects `libil2cpp.so`, as the mono64 bridge does (runtime/host/mono64.c):

- a loader for Mach-O (segments, rebases and binds, lazy stubs, exports, initializers);
- the macOS calls it makes answered on Linux (libSystem, libc++, CoreFoundation);
- the few instructions an ARMv8.0 core lacks rewritten to ones with the same result;
- Apple's arm64 calling convention where it differs from AAPCS64 (below);
- the data converted as for any Metal-only macOS build (`shaders.from = ["metal"]`).

The game's code is not changed beyond those instruction substitutions; DRM and ownership checks
are never touched (the build's steam_api is answered by the runtime's stub: Steam not running).

## Dave the Diver (Steam, macOS, v1.0.6.768, Unity 6000.0.52f1)

| | |
|---|---|
| binaries | every one universal (x86_64 + arm64): the executable, UnityPlayer.dylib, GameAssembly.dylib (741 MB, the arm64 slice 351 MiB), lib_burst_generated.bundle, libsteam_api, libEOSSDK, MicrophoneUtility, lib_coacd |
| GameAssembly arm64 | `__TEXT` 143.5 MiB: `__text` 10.0 MiB (libil2cpp, Boehm GC, zlib, ...) and an `il2cpp` section of 116.9 MiB (the generated code), `__gcc_except_tab` 2.5 MiB, `__unwind_info` 1.7 MiB, `__eh_frame` 72 bytes; `__DATA_CONST` 7.3 MiB, `__DATA` 9.4 MiB with bss; `LC_DYLD_INFO_ONLY` (opcode rebases and binds, lazy stubs through `__stub_helper`); no thread-local variables; the symbol table kept (function names) |
| imports | 342: libSystem 236, libc++ 95, CoreFoundation 11 (`CFLocale*`, `CFString*`: the current locale only); Foundation and Security linked, nothing used |
| beyond ARMv8.0 | of 33.2 M instruction words: 489 LSE atomics (297 `ld<op>`/`swp`, 192 `cas`), 13 `ldapr` (RCPC), 13 `udot` (dot product), 6 `eor3` (SHA3, in `BigInteger.GetHashCode`'s vector code), 22 `crc32` (optional in ARMv8.0: zlib) |
| reach | the code ends 126.8 MiB from the image start: a block of up to 1.2 MiB placed right before the image is within a `B`'s ±128 MiB of every site |

Counted by the loader's own scan (`runtime/macho/a64fix.c`: the encodings of each ARMv8.1 - 8.5
extension and of ARMv8.0's optional ones) and checked against a second decoder: the whole image
disassembled as for a Cortex-A53 (`llvm-objdump -d --mcpu=cortex-a53`), every word it cannot
decode listed, and the two lists compared. They must be identical, and are: 521 addresses. The
first scan had missed the 13 `udot` (its mask covered `sdot` only): a hand-written list of
encodings is only trusted once a full decoder agrees with it over the whole image. The same
disassembly with every feature on leaves no undecodable word, so the code sections hold no data
(`LC_DATA_IN_CODE` is there with 0 entries); the 284 `udf` words are the lazy-binding immediates
of `__stub_helper`, never executed since lazy pointers are bound at load.

## What has to be answered

The rewrite (`a64fix.c`): `ldapr` becomes `ldar` in place (stronger ordering, same value). Each
LSE atomic, `eor3` and `sdot`/`udot` becomes a `B` to its own routine in the block before the
image and a `B` back: an `ldaxr`/`stlxr` loop for the atomics (`cas` and the min/max forms
included, sub-word sizes extended as the instruction does), two `eor`, or widening multiplies
and pairwise adds. A routine keeps every register it does not define and NZCV, and saves its
scratch registers 176 bytes below `sp`: past the 128-byte red zone. `crc32` stays if the handheld
has it (`crc32` in /proc/cpuinfo's Features). What the CPU lacks is read from `AT_HWCAP`; a kind
with no rewrite (`casp`, FP16, ...) refuses the image by name rather than run into a trap.

Verified by `runtime/macho/test/run.sh` on a CPU that has the instructions: each form runs once
as itself and once rewritten from the same registers, flags, vectors, stack and memory, and
everything is compared after (1.26 M cases over the register patterns that matter: the zero
register, `sp` as the base, operands that alias, the routine's own scratch registers), then four
threads count through rewritten `ldadd` and `cas`. The test is itself tested: 17 deliberately
wrong routines (a flag not restored, a frame inside the red zone, a missing sign extension, ...)
were each reported. On Dave's image: 521 sites rewritten, none refused, 24.0 KiB of routines.

Apple's arm64 convention against AAPCS64:

- variadic arguments all go on the stack (the printf family, `open`, `fcntl`, `ioctl`): shims
  read them from there; a `va_list` from macOS code is a pointer into 8-byte slots, which a Linux
  `va_list` with both register offsets at 0 reads the same way;
- callers extend arguments narrower than 32 bits, and macOS callees rely on it: the il2cpp_*
  exports the engine calls take them as 32-bit values;
- stack arguments past the eighth are packed at their natural alignment, not in 8-byte slots
  (calls with more than 8 integer or 8 floating-point arguments);
- leaf functions use a 128-byte red zone below `sp`: every signal the runtime handles on a thread
  that runs macOS code goes to an alternate stack (`SA_ONSTACK`);
- x18 is reserved (the macOS code never touches it).

libSystem against glibc: the layouts of `struct stat`, `dirent`, `addrinfo` (`ai_canonname` and
`ai_addr` swapped), `sockaddr` (`sa_len`), `utsname`, `sigaction`; the numbers of errno values,
`open`/`mmap`/`fcntl` flags, signals, `sysconf` and `sysctl` names, locale categories and masks,
`AF_INET6`, socket options; pthread's static initializers (a signature word that glibc would read
as a held lock) and 8-byte `pthread_key_t`; `strerror_r` (XSI); `__error` for errno; ctype tables
(`_DefaultRuneLocale`, `__maskrune`); `__sincos_stret`; `__chkstk_darwin`; libdispatch semaphores
(`dispatch_semaphore_*`, `dispatch_time`); Mach time (`mach_absolute_time`, `mach_timebase_info`).

Boehm GC as built for macOS stops the world with Mach calls (`mach_thread_self`,
`thread_suspend`, `thread_get_state`, `thread_resume`), answered with signals and the registers
saved at the stop, and finds its roots with dyld calls (`_dyld_image_count`,
`_dyld_get_image_header`, `getsectiondata`), answered with the image's own sections.

C++: the libc++ symbols (`std::__1`, iostreams, locale, strings, exceptions) from LLVM's libc++
and libc++abi built as macOS arm64 dylibs (`runtime/macho/cxx/build-cxx-macho.sh`) and mapped by
the same loader beside the game's image. The image's inline code was compiled against that
library's layouts (`mbstate_t`, ctype masks, `std::string`, pthread types), so nothing of it is
imitated on the Linux side, and its own libSystem imports go to the same shims. Two build flags
matter: `-mcpu=generic` (`-march=armv8-a` leaves the CPU at apple-m1, LSE included) and
`-Wl,-no_fixup_chains` (rebases and binds as `LC_DYLD_INFO` streams, the form the loader reads).
Managed exceptions are C++ throws, so the unwinder reads the image's compact unwind info
(`__unwind_info`) and its LSDAs (`__gcc_except_tab`, the same format as on Linux) as well as the
engine's ELF tables, and catches through engine frames work (icalls raise managed exceptions).

The other native libraries: lib_burst_generated through the same loader (Burst falls back to the
IL2CPP code without it), steam_api answered by the runtime's stub, EOS and the microphone
utility by stubs that fail as without the service, lib_coacd through the loader if the game calls
it.

## The shim layer (`runtime/macho/sh_*.c`, `dyld.c`, `tlv.c`)

432 libSystem, CoreFoundation and dyld names are answered. The rules every shim keeps:

- the image's side is Darwin's: errno numbers, flag bits and struct layouts are converted at the
  shim, from one generated header. `abi/gen.c` is compiled on a Mac against the SDK and prints
  `darwin_abi.h` (numbers, layouts, the error texts); nothing is typed in by hand;
- a shim returns `long` (a macOS caller reads the whole register), and takes a variadic's
  arguments from the stack (`darwin_va.h`);
- a call with no answer says so once, by name (`darwin_unanswered`), and fails the way macOS
  documents; it never pretends;
- thread-local variables (`__thread_vars`, the libc++ `thread_local`s): descriptors are bound
  to a thunk that keeps every register but x0, as the macOS convention promises the caller;
- stop-the-world (`thread_suspend`, `thread_get_state`, `thread_resume`): a real-time signal,
  a suspend count per thread, the registers saved at the stop, handlers on an alternate stack
  with every signal masked. A blocking call interrupted by a stop is retried, not failed with
  `EINTR`;
- pthread's static initializers are a signature word that glibc would read as a held lock: the
  first use builds the real object behind it, once, under a lock.

What differed from glibc in ways only the comparison below found:

- `strerror`'s texts ("Operation timed out", not "Connection timed out"): the generated table
  holds every text the Mac's libc knows (one more than the SDK's `ELAST`);
- `MB_CUR_MAX` is 4 for UTF-8 on macOS, 6 on glibc 2.31;
- `__maskrune`'s low byte carries a digit's value (`digittoint`);
- `printf("%p", NULL)` prints `0x0`; `long double` is `double`; `%D %O %U` are longs;
- `SO_NOSIGPIPE` has no Linux twin: the socket is marked and `send` adds `MSG_NOSIGNAL`
  (a plain `write` to such a socket still raises `SIGPIPE`: open);
- libc++abi asks `dlopen` for `/usr/lib/libstdc++.6.dylib` at the first throw and goes on
  without it.

Not answered yet, each refused or reported by name: `LC_DYLD_CHAINED_FIXUPS` and
`LC_DYLD_EXPORTS_TRIE` images, `S_INIT_FUNC_OFFSETS`, resolver exports, the `SIOCGIFCONF` and
`SIOCGIFFLAGS` ioctls.

## How it is tested

`sh runtime/macho/test/shims-test.sh`, on an Apple-silicon Mac with Xcode's tools and Docker:

1. `test/build-mac.sh` builds `darwin_c.c` and `darwin_cxx.cpp` as macOS arm64 dylibs (the
   flags a game's image would have, plus `-mcpu=generic`) and as native programs, and runs the
   native programs: their output is the expectation;
2. the same dylibs run through the loader (`test/machorun.c`) in an arm64 Linux container, with
   every rewrite forced (`UA_A64FIX=all`);
3. the two outputs must be the same, byte for byte.

The C test (180 lines of output) covers formatting, files, directories, time, sysctl, threads
(mutexes, conditions, keys, 200 stop-the-world rounds over running and sleeping threads),
locale and sockets. The C++ test (42 lines) covers exceptions through 12 frames, `dynamic_cast`,
streams, locales, containers, `std::thread`, `thread_local`, futures. Both match; the C++ test
matched in 100 of 100 repeated runs.

## il2cpp starts (2026-11-09)

`test/il2cpprun.c` loads libc++abi, libc++ and a game's GameAssembly, runs the initializers,
then calls the il2cpp API the way a player does. With Dave's image and metadata, every rewrite
forced, in the container on the Mac:

- loaded and linked in 2.2 s (a bind-mounted file; most of it is reading), `il2cpp_init`
  returns 1 in 2.0 s;
- `Int32.Parse`, a `Parse` that throws (a managed `FormatException`, caught through
  `il2cpp_runtime_invoke`), `String.Concat`, `Double.Parse`, `Environment.TickCount` and
  `ProcessorCount` all answer correctly;
- 400 000 string allocations with 20 forced collections leave the heap where it started
  (the Boehm stop-the-world over the Mach calls works).

GameAssembly needs only libSystem, libc++ and CoreFoundation, so il2cpp starts without the
engine: the managed side can be tested apart from the player.

## Under the Android player (2026-11-09)

The bridge (`runtime/macho/bridge`, built by its `build.sh` into `libil2cpp_macho.so`) is a host
(glibc) library that the port's lib folder holds as `libil2cpp.so`, beside the game's
`GameAssembly.dylib` and the two libc++ dylibs. `android_package.json` says
`"scripting": "macho"` (or `UA_SCRIPTING=macho`); the host (`runtime/host/macho64.c`) then loads
the bridge before the player, hands it the loader's `dlopen` (the game's plugins are Android
libraries), and maps it as the player's `libil2cpp.so`. arm64 only.

What the bridge does between the two sides:

- **The il2cpp API** (`bridge.S`, `il2cpp_api.inc`): one forwarder per `il2cpp_*` function the
  player may call, 247 names over Unity 2017.4 to 6000.0. `gen-api.py` derives the table from
  the `il2cpp-api-functions.h` of the editors at hand (the header itself is Unity's and is never
  checked in): a name, and which arguments are narrower than 32 bits (the forwarder extends
  them, as a macOS caller would) or are a class or a method (below). A name the game's image
  does not export stops with that name when called, not before.
- **Internal calls** (`bridge.c`): the player registers its engine functions by name; the image
  asks for one at its first use. Each is registered as a stub, and at its first call the
  method's signature is read from the image: if the result is narrower than 32 bits (the image
  expects it extended; AAPCS64 leaves the upper bits undefined) or arguments go on the stack
  (the image packs them at their natural alignment, the player reads 8-byte slots), the stub
  becomes a thunk that lays them out again; else the player's function is called directly.
  `UA_MACHO_LOG=1` logs each thunk made.
- **Threads and signals**: every thread gets an alternate signal stack before it runs the
  image's code (the loader's thread start hook, and the first il2cpp call of a thread made
  elsewhere), and every handler the runtime or the player installs gets `SA_ONSTACK`
  (`so_signal_handlers_onstack`): the image's leaf functions keep data below `sp`. The signal the
  collector stops threads with stays out of every mask Android code sets.
- **Unwinding**: the bridge's libunwind walks the Android libraries' ELF tables through the
  loader's `so_dl_iterate_phdr` and the image's compact unwind info.

### What only the Android engine modules have

Unity's engine modules (`UnityEngine.*.dll`) differ by platform, and the Android player looks up
and calls a few managed methods that only its own modules have. A macOS build's metadata does
not have them, and managed stripping may have removed the whole class. First case met, Unity
6000.0.52f1: the nested class `UnityEngine.AudioSettings/Mobile` with
`InvokeIsStopAudioOutputOnMuteEnabled` and `InvokeOnMuteStateChanged`. The player prints
"Unable to find method ..." for it at start, keeps a null method, and crashes three frames later
in `il2cpp_method_get_param_count(NULL)`.

How the player looks one up: `il2cpp_class_from_name(image, namespace, name)` (a nested class
misses there), then a walk of `il2cpp_class_get_nested_types(outer)` comparing names; the
method by a walk of `il2cpp_class_get_methods` (not `il2cpp_class_get_method_from_name`), up the
parents. Thousands of `il2cpp_class_from_name` misses at start are normal: the player asks for
every native type's class, in several namespaces.

The bridge answers such a class or method itself (`bridge.c`, "what only the Android engine
modules have"), **as the macOS module behaves**, so the game sees what it would on a Mac. For
the two above: the macOS module's `Mobile.stopAudioOutputOnMute` is the constant false, and
nothing in it ever raises `OnMuteStateChanged`; so the first answers false and the second does
nothing. The mechanism:

- the stand-in classes and methods are records in one page of the bridge
  (`ua_standin_space`); the player gets pointers into it, at the end of the image's own
  nested-type and method walks;
- every forwarder that takes a class or a method checks for a pointer into that page (three
  instructions) and then calls the bridge's own answer for that function instead of the image,
  which never sees such a pointer. The answers are the facts of the Android module's
  declaration (name, flags, parent, parameters, ...) and `il2cpp_runtime_invoke`;
- a function with no answer (fields, attributes, reflection objects, ...) stops with its name:
  nothing is guessed.

To read a module's declarations, the editor's own Mono has a disassembler:
`MonoBleedingEdge/bin/mono MonoBleedingEdge/lib/mono/4.5/ikdasm.exe <dll>`, on the Android
module (`PlaybackEngines/AndroidPlayer/Variations/il2cpp/Managed`) and the macOS one
(`PlaybackEngines/MacStandaloneSupport/Variations/il2cpp/Managed`). Its output is Unity's code:
it is read, not kept.

What decides the set for a game is what the Android player requires
(`[RequiredByNativeCode]` members of the Android modules) against what the game's own
`global-metadata.dat` holds, not a diff of the two platforms' modules: stripping differs per
game. The setup has to report every required member the game lacks and the bridge has no
record for (to do, with the `--macho` mode of `pmu android-data`).

Also open: 13 internal calls the image may ask for that the Android player does not register
(`UnityEngine.Screen::GetMainWindowDisplayInfo_Injected` among them; none was called by the
test game), and 6 il2cpp functions the Mac image does not export (none was called).

### The data, by hand for now

A macOS build keeps `Contents/Resources/Data` (the Data folder), with
`il2cpp_data/{Metadata,Resources}`, and `Contents/Frameworks/GameAssembly.dylib`. The Android
IL2CPP player reads the metadata from `assets/bin/Data/Managed/{Metadata,Resources,etc}`. The
conversion is the Android route's with `"steps": ["shaders", "settings"]`,
`"shaders": {"from": ["metal"]}` (a Mac build made for Apple silicon carries Metal programs
only) and `"settings": {"graphics_apis": [11]}`. Three things in `pmu android-data` stand in the
way of a Mac IL2CPP source today and were worked around for the test: it requires
`Managed/*.dll`; it reads the player's version from an editor `AndroidPlayer` folder's
`modules.asset` ("2018.3.0a1") instead of the player itself; and a source named `Data` gives an
empty game name.

### The first run

The test game (`runtime/macho/test/game`: the project's Pong, built for macOS arm64 with IL2CPP
by `build.sh`, never run on the Mac) under Unity 6000.0.52f1's arm64 Android player in the
emulator rig (`EMU_ARCH=arm64 UA_SCRIPTING=macho tools/emu/emu.sh`): the picture is right
(paddles, ball, the score's text), the score counts, 3 556 frames at the rig's 60 fps in 36 s,
RSS 216 MB. 241 of the 247 il2cpp functions passed on, 6 the image does not export; 19
internal calls needed a thunk (bool results; `Texture2D::Internal_CreateImpl_Injected` and
`FontEngine::TryAddGlyphToTexture_Internal_Injected` with two stack arguments each).

Next: natively raised managed exceptions and callbacks into the image under the player (a test
component), the `--macho` mode of `pmu android-data` with the required-members report, then a
handheld with a 64-bit userland.
