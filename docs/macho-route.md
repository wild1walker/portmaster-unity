# The Mach-O route: a macOS IL2CPP build's arm64 code on ARM Linux

Status: in progress (first game: Dave the Diver, 2026-11-08). Nothing of it runs yet; this page
records what the build holds and what the route has to answer.

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

Next: the bridge library that stands in for `libil2cpp.so` under Unity's arm64 Android player
(the il2cpp_* forwarders, extending narrow arguments), then a handheld with a 64-bit userland.
