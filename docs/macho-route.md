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

### Its plugins (2026-11-10)

`Contents/PlugIns` of the build, each file universal; what the arm64 slices need (`nm -um`,
the loader's probe and its instruction scan, `test/shims.sh cover` for the names):

| plugin | links | for the route |
|---|---|---|
| `lib_burst_generated.bundle` (1.7 MB) | nothing: no library, no undefined symbol; 1,463 exports | nothing to resolve; 620 instructions beyond ARMv8.0 (LSE atomics), all of kinds the rewrite knows. The engine opens this one, not the game's code (see "Burst's library" below). In the Linux container its arm64 part loads through the loader with every rewrite forced and `burst.initialize` runs to its end: 1,178 names asked of the engine (internal calls and `burst_abort`, no `#dllimport:`), the static set-up done (104 calls of the engine's memory functions, 109 others), 360 job functions found by their names. Not yet run under the player with the game |
| `lib_coacd.dylib` (6.6 MB, collision decomposition) | libc++ (206 names, every one in the route's libc++) and libSystem (103 names) | every name is answered (`test/shims.sh cover`: 103 names, 0 without an answer); the 22 that had no shim are written, see "What a plugin's own libraries asked for" below; 4,073 instructions beyond ARMv8.0 (LSE, `ldapr`); thread-local variables. **Runs through the loader** in the arm64 Linux container with every rewrite forced: opened by the name the game's code has (`dlopen("lib_coacd")`), `CoACD_run` cuts an L-shaped test solid into 2 or 3 convex hulls whose volumes add up to the solid's (four runs: its preprocessing on, off and left to it, two seeds), `CoACD_freeMeshArray` frees them |
| `MicrophoneUtility_macos.dylib` | Foundation, AVFoundation, the Objective-C runtime | not loadable (no Objective-C here), and not needed: the game's code names none of its functions. The one file of the five built for macOS 15 only, so with chained fixups in place of the bind streams: the loader reads those too (in the container it maps the file and finds 5 rebases, 4 binds and 2 exports, the counts `xcrun dyld_info` gives), so what keeps it out is the Objective-C names alone |
| `libEOSSDK-Mac-Shipping.dylib` (47 MB) | Cocoa, Carbon, IOKit, Security, CoreAudio, CFNetwork, ... | not loadable, and not needed: the game's code names no `EOS_` function |
| `steam_api.bundle` | | the game's code names 939 Steamworks functions (Steamworks.NET's flat API); the runtime's own answer stands in: Steam is not running |

What the setup makes of the five (`pmu android-data --macho`, "The data" below; the same
survey without writing anything: `cargo run -p androiddata --example macho_plugins --
<Game>.app --names`): `lib_coacd.dylib` and `lib_burst_generated.bundle` are copied as arm64
code to the lib folder, `steam_api.bundle` and `libEOSSDK-Mac-Shipping.dylib` are answered by
the runtime's own stubs through the plugin map (Steam: not running; EOS: no platform), and
`MicrophoneUtility_macos.dylib` is left out with the reason in the log.

What the game's code calls is in the code itself: IL2CPP compiles each `DllImport` to a lookup
with the library's and the function's name as C strings, so `strings` over the arm64 image
lists them (`lib_coacd` with `CoACD_run`, `CoACD_freeMeshArray`, `CoACD_setLogLevel`;
`steam_api`). A plugin in the folder that no string names is not loaded by the game's code.

The declaration a C# wrapper has is not always the one the plugin was built with, and on arm64
macOS that matters past the eighth argument: stack arguments are packed to their own size, not
to 8 bytes. `CoACD_run` as shipped reads two `bool`s one byte apart and an `unsigned` after
them (`[x29,#0x10]`, `#0x11`, `#0x14`); the open source header of a later version has more
arguments. A test caller has to be written against the disassembly of the shipped file, and
the game's own call is right by construction (IL2CPP compiled it for this convention, and both
ends are Mach-O code).

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
`-Wl,-no_fixup_chains` (rebases and binds as `LC_DYLD_INFO` streams; the loader reads chained
fixups as well, see "Chained fixups" below, and the streams stay the form these two are built in).
Managed exceptions are C++ throws, so the unwinder reads the image's compact unwind info
(`__unwind_info`) and its LSDAs (`__gcc_except_tab`, the same format as on Linux) as well as the
engine's ELF tables, and catches through engine frames work (icalls raise managed exceptions).

The other native libraries: lib_burst_generated through the same loader, offered to the engine
("Burst's library" below; without it the jobs run as the IL2CPP code), steam_api answered by the runtime's stub, EOS and the microphone
utility by stubs that fail as without the service, lib_coacd through the loader when the game
calls it (see "Plugins the game opens" below).

## The shim layer (`runtime/macho/sh_*.c`, `dyld.c`, `tlv.c`)

506 libSystem, CoreFoundation and dyld names are answered. The rules every shim keeps:

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
  `EINTR`, and a call with a time limit keeps its deadline: the time a thread stood stopped is
  time it waited or slept (condition waits, `__ulock_wait`, semaphores, `select`, `poll`,
  `nanosleep`, `usleep`, `sleep`, and socket calls under `SO_RCVTIMEO`/`SO_SNDTIMEO`, which the
  shim finishes with a `poll` for what is left of the timeout). On macOS a suspended thread's
  wait simply goes on in the kernel; here the stop is a signal, and every such call has to put
  that back;
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

Not answered yet, each refused or reported by name: arm64e images (signed pointers), resolver
exports, the `SIOCGIFCONF` and `SIOCGIFFLAGS` ioctls.

### Chained fixups (`macho.c`, 2026-11-10)

A Mach-O file says in one of two forms which of its words the loader must set. Up to a
deployment target of macOS 11 the linker writes opcode streams (`LC_DYLD_INFO_ONLY`: rebase,
bind, weak bind, lazy bind, and an export trie). From macOS 12 on it writes chained fixups
(`LC_DYLD_CHAINED_FIXUPS`, with the export trie in `LC_DYLD_EXPORTS_TRIE` and the
initializers as 32-bit offsets in a `__init_offsets` section, `S_INIT_FUNC_OFFSETS`): each
word to set holds its own fixup and the distance to the next one, page by page. A game takes
whichever its developer's minimum macOS version gives, plugin by plugin: of Dave's files only
the microphone library has the new form; a newer game may have it throughout. The loader
reads both, and refuses a file with both or neither.

What it reads of the new form:

- the import table in its three layouts (1: 32 bits, library ordinal 8, weak 1, name offset
  23; 2: the same and a 32-bit addend, which the linker uses when an addend is negative or
  above 255; 3: 64 bits with a 64-bit addend), names uncompressed only;
- library ordinals as dyld does: 0 the image itself, 1..n its libraries, and only the top
  values special (8-bit: above 0xf0; 16-bit: above 0xfff0): -1 the main executable, -2 a flat
  search of every image, -3 a coalesced (weak) name. A plain sign extension of the byte would
  turn the 128th to 240th library into an error;
- the two plain 64-bit pointer layouts, stride 4 bytes: `DYLD_CHAINED_PTR_64` (2: a rebase
  holds the linked address) and `DYLD_CHAINED_PTR_64_OFFSET` (6: it holds the offset from the
  image's start); a rebase is target 36 bits, top byte 8, next 12, a bind is ordinal 24,
  addend 8, next 12, the top bit telling the two apart. Any other layout (arm64e's signed
  pointers, 32-bit, kernel caches) is refused by its number;
- each import is resolved once, then the chains are walked twice: the first pass checks every
  link stays inside its segment's file data and counts, the second writes. An image with a
  name that has no answer is therefore refused before one of its words is changed, the same
  as with the streams;
- a use of a coalesced name is noted the way the weak bind stream's entries are, so C++
  inline variables and `operator new` still end up as one across the images.

What this Mac's linker (ld-1167.5) does, which decides how the tests are built: a target of
12.0 gives chained fixups with pointer layout 6; a target of 11.0 with `-Wl,-fixup_chains`
gives them with layout 2; `-Wl,-no_fixup_chains` gives the streams at any target;
`-undefined dynamic_lookup` silently falls back to the streams whatever the target, while
`-Wl,-U,_name` (one name allowed to be missing) keeps the chains and binds that name flat.

Not exercised by any compiler output here: import layout 3 and a non-zero top byte (both
written from the format's definition, with a unit test on the setup's side for layout 3).

### What a plugin's own libraries asked for (2026-11-10)

A plugin brings its own libraries with it (a task scheduler, a logger, a volume library), and
they reach deeper into libSystem than IL2CPP does. The 22 names the first such plugin lacked,
each measured on macOS first (the test prints what macOS answers; the shim has to print the
same):

- **`getcontext`, `setcontext`, `makecontext`, `swapcontext`** (`sh_context.c`, the register
  part in `sh_asm.S`): macOS's 880-byte `ucontext_t` with its mcontext inside, filled the way
  its libc fills one, oddities included, since code that reads a context reads these:
  `uc_stack` is the sp at the call and `RLIMIT_STACK`'s soft limit, not the thread's stack;
  `uc_link` after `getcontext` is the number of the signal whose handler is running (0 outside
  one; macOS keeps one cell for the whole process, cleared when a handler returns);
  `swapcontext` sets the top bit of `uc_onstack` in the context it saves and keeps that
  context's `uc_link`; d8 to d15 go to the upper halves of their v slots; `makecontext`'s
  arguments are ints, 4 bytes each, at the new stack's top; a context whose function has
  returned has `uc_mcsize` 0 and `setcontext` refuses it with `EINVAL`; a function that returns
  with no `uc_link` ends the process with status 0. The offsets come from the generated header
  and are pinned against the assembly's numbers by `_Static_assert`.
  Open: the slot arm64e keeps its flags in is not written; `backtrace` on a `makecontext`
  stack gives 0 frames.
- **Mach semaphores** (`semaphore_create`, `_wait`, `_signal`, `_destroy`, in `sh_thread.c`): a
  futex word per semaphore and port names of macOS's shape, never handed out twice in a row (a
  destroyed slot waits in a queue behind every fresh one, and carries a generation). The error
  numbers are macOS's and differ by what the wrong name is: a destroyed or never made name is
  `KERN_INVALID_NAME` to `semaphore_wait` and `MACH_SEND_INVALID_RIGHT` to
  `semaphore_destroy`, the task's own port or a thread's is `KERN_INVALID_CAPABILITY`, a wait
  on a semaphore destroyed under it ends with `KERN_TERMINATED`. A wait a signal handler
  interrupts returns `KERN_ABORTED` on macOS whatever `SA_RESTART` says; Linux restarts a futex
  wait that has no timeout by itself, so the wait is given a timeout of an hour and looped,
  and tells a handler of the image's from the runtime's own stop signal by a per-thread count
  of handlers run. Open: with `SYNC_POLICY_LIFO` the newest waiter is not woken first.
- **signal masks** (`sigprocmask`, `pthread_sigmask`, `pthread_kill`): macOS has signals Linux
  has not (`SIGEMT`, `SIGINFO`) and a 32-bit mask whose 32nd bit is no signal at all, yet a
  mask that is set and read back keeps all three bits (and never `SIGKILL` or `SIGSTOP`).
  Those bits are kept beside the host's mask, per thread, handed to a new thread at its
  start and saved and put back around a handler like the rest of the mask. The host's
  real-time signals (the stop signal among them) are never touched by an image's mask.
  `pthread_sigmask` returns the error number, `sigprocmask` sets `errno`; `how` is only
  checked when a set is given. `pthread_kill` of `SIGEMT` or `SIGINFO` is said by name and
  not sent.
- **`pthread_attr_setstacksize`**: macOS wants a multiple of 4096 of at least 16 K (`EINVAL`
  otherwise) and rounds up to its 16 K page. The host's thread gets that size plus a 64 K
  margin, and never less than 512 K: the shims and glibc run on the same stack, and a stack
  that is enough for macOS's libc is not sure to be enough for them.
  `pthread_attr_getstacksize` has no shim yet (it would have to give back the size asked for).
- **`pthread_threadid_np`**: the kernel's thread id, for the caller or for another thread the
  shims started; `EINVAL` without a place to put it.
- **libm keeps `errno`**: macOS's math functions never store `EDOM` or `ERANGE`; glibc's do,
  into the cell the images read. Every libm name an image can import goes through a wrapper
  that puts `errno` back (33 of them, `ldexp` and `log2` new).
- **`fegetenv`, `fesetenv`, `_FE_DFL_ENV`**: macOS's `fenv_t` is the status register, then the
  control register, 64 bits each; glibc's is the other order in 32-bit halves.
- **`rand`, `srand`, `rand_r`**: macOS's own row of numbers (x = 16807 x mod 2^31 - 1, a seed
  of 0 standing for 123459876), not glibc's: code that seeds the generator counts on the row.
- **`__assert_rtn`** (macOS's three message forms, then `abort`), **`__udivti3`**, and the
  plain C ones glibc answers the same way (`atof`, `strncat`, `strspn`, `strtok`, `shmdt`).

One more thing only a build with other feature macros shows: with `_XOPEN_SOURCE` and
`_DARWIN_C_SOURCE` both defined, the SDK's headers make the compiler import `fopen`, `fdopen`,
`select`, `realpath` and `syslog` under a second name (`_fopen$DARWIN_EXTSN`, ...). On arm64
it is the same function; the tables carry both names. The SDK has five more such names
(`getgroups`, `popen`, `pselect`, `reallocarray`, `reallocarrayf`) with no shim under either
name yet.

### Plugins the game opens (`dyld.c`, 2026-11-10)

IL2CPP resolves a `DllImport` at the first call: `dlopen` with the library's name, then `dlsym`.
In a macOS build that `dlopen` is the image's import, so it arrives at the shim, and the shim
loads a plugin that is a Mach-O file the way dyld would, at that moment:

- **which file**: the path as given if it is a Mach-O file; else, in the plugin folder (the
  port's lib folder under the bridge), the base name as given, then with `.dylib` and
  `.bundle` in place of its extension (`.dylib`, `.bundle`, `.so`), with `lib` put in front,
  and with a leading `lib` taken off. A name macOS code would use (`lib_x`, `x.bundle`,
  `@rpath/x.dylib`) and the name the Android player would use (`libx.so`, a path into an APK's
  lib folder) both find `x.dylib`. An image that is already loaded answers first, by its path,
  its install name or its file name, so two spellings give one handle, as on macOS;
- **its own libraries** (`LC_LOAD_DYLIB` and its kin): `/usr/lib/` and `/System/` names are the
  system's, answered by the shims (or by an image of that name: the route's own libc++); the
  rest are looked for among the loaded images, then beside the file that needs them, then in
  the plugin folder, and loaded first, to a depth of 8. One that is not found is named in the
  error only if a symbol then stays unbound; one that is found and does not load fails the
  whole `dlopen`;
- **all or nothing**: the files are mapped and bound, the instruction rewrite, the thread-local
  set-up and the page protections done, and only then do the images become visible to other
  threads (image count, `dladdr`, unwinding, symbol lookup), followed by the add-image
  callbacks and the initializers, dependencies first. If any step fails, everything this
  `dlopen` mapped is unmapped again and no initializer has run. macOS does the same: its test
  plugin with a missing symbol takes the dylib it brought along away with it;
- **strict binding**: an import with no answer fails the load and is printed by name. macOS
  with `RTLD_LAZY` would load such a plugin and crash at the call; a refusal with the name is
  the better report, and a failed file is remembered, so the message comes once and a game
  that retries every frame does not map the file every frame;
- **modes**: `RTLD_NOLOAD` only looks; `RTLD_LOCAL` keeps the image's exports out of flat
  lookups (`dlsym(RTLD_DEFAULT)`, other images' flat binds) until some later `dlopen` of it
  without `RTLD_LOCAL`, which macOS also treats as a promotion; `dlclose` returns 0 and unloads
  nothing (a plugin's thread-locals and C++ statics outlive most reasons to unload);
- **threads**: one recursive lock around loading (an initializer may call `dlopen` itself, and
  a second thread asking for the same plugin waits and gets the same handle after the
  initializers have run once); readers (`_dyld_image_count`, `dladdr`, the unwinder) take no
  lock and see only finished images;
- **what is not Mach-O** goes to the host's loader as before (an Android library a port adds,
  the runtime's Steam answer); a file that is Mach-O and fails is the answer, with its
  message in `dlerror`, never a fall through to another library of the same name.

`UA_MACHO_LOG=1` prints each `dlopen` with what answered it.

### Burst's library (2026-11-10)

Burst's library (`lib_burst_generated.bundle`, directly under `Contents/PlugIns`) is the
exception among a game's plugins: the engine opens it, not the game's code, and the engine is
the Android player, whose `dlopen` is the host loader's. What the player does, read from
6000.0.52f1's arm64 library:

- at its first Burst job it asks for the library `_burst_generated` (Android's name, without
  `lib`), then `lib_burst_0_0`, `lib_burst_0_1`, ... until one is absent;
- each name goes through `FindPluginExecutable`: Java's `ClassLoader.findLibrary(name)`, and
  when that answers a path, `System.load(path)`; when it answers null, `UnityPlayer.loadLibrary(name)`,
  and the result stays the bare name. **A result that is empty or equal to the name counts as
  "no such library"**, whatever `loadLibrary` did: the library can be loaded and still go
  unused, with no message. The jobs then run as the game's IL2CPP code, correct and slower;
- with a path, the player `dlopen`s it, looks up `burst.initialize` and calls it with one
  argument, a function that answers a name with a pointer: every internal call the engine has
  registered (it keeps a map of them for Burst, filled from the same registration il2cpp gets),
  `burst_abort`, `burst_get_dispatch_info`, and `#dllimport:<library>|<function>` for a
  `DllImport` of Burst code. The library asks for all of its names there and then, runs its
  static set-up, and returns; job functions are looked up later with `dlsym`, by hash names.

Three pieces make that work for a Mach-O library:

- **the host loader takes foreign libraries** (`so_set_foreign_libs`, `so_loader.h`): three
  functions of the host's, `has(path)`, `open(path)`, `sym(lib, name)`. A `dlopen` that finds
  no ELF file asks `open`; the handle is the loader's own (one per file name, so a path and a
  bare name give the same one), `dlsym` on it asks `sym`, and it stays loaded. The bridge
  gives the three (`ua_macho_host.foreign_libs`), answered by the shim layer's plugin search
  and loader, the same as for the game's own `dlopen`: `/x/lib_burst_generated.so` finds
  `lib_burst_generated.bundle` in the plugin folder;
- **`findLibrary` answers for such a library**: the Java layer's `has_library` callback
  (`unityplayer.h`) is asked when no file `lib<name>.so` is there, and the host passes the
  question to `has`. Without it the player took the first branch above to its dead end: 21 of
  the test game's 24 checks passed and the three Burst ones reported that their code had run
  as ordinary code;
- **`burst.initialize` is wrapped**: `dlsym` of that name gives the player a stub of the
  bridge's, which calls the library's own function with the bridge's resolver in place of
  the player's. The resolver asks the player's, and when the pointer that comes back is an
  internal call the bridge has a stub for (same target, same name), it answers with that stub:
  Burst's code was compiled for macOS, so it calls an internal call the way the game's code
  does, and gets the same treatment (arguments on the stack laid out again, narrow results
  extended; a call the classification cannot place is logged and passed as the player
  answered). Names with no record (`burst_abort`, a `#dllimport:`) pass as the player answered.

`UA_MACHO_LOG=1` prints one line per Burst library when it has started: how many of its names
got stubs and how many passed through. The test game's (three jobs): 3 and 6.

The limits, as they stand. The engine calls a job's entry function with the Linux convention
and the function expects the macOS one; for pointers and integers in registers, which is what
job entries take, the two are the same. A `#dllimport:` from Burst code into a library that
is not Mach-O is called as is, right only up to eight arguments of full width. And a native
plugin that the engine calls into for rendering or audio (`UnityPluginLoad` and its
interfaces) is a different thing, not covered: its interfaces are C++ tables of the macOS
player.

### The string family, and how narrow the cover still is

The shims were written for what IL2CPP's image and the first plugin import, and a first own
test plugin showed how little that is: it needed `__strlcat_chk`, and of 50 everyday string
names 44 had no answer. Added with tests: `memccpy`, `memmem`, `memset_pattern4/8/16`, the
fortified forms a build at `-O2` imports instead of the plain ones (`__memcpy_chk`,
`__strcpy_chk`, `__stpcpy_chk`, `__strncpy_chk`, `__stpncpy_chk`, `__strcat_chk`,
`__strncat_chk`, `__strlcpy_chk`, `__strlcat_chk`, `__memccpy_chk`; an overflow ends the
process as on macOS), `strlcat`, `stpcpy`, `stpncpy`, `strnlen`, `strndup`, `strpbrk`,
`strcspn`, `strsep`, `strtok_r`, `strcasecmp`, `strncasecmp`, `strcasestr`, `strnstr`,
`index`, `rindex`, and the wide ones (`wcscmp`, `wcsncmp`, `wcscpy`, `wcsncpy`, `wcscat`,
`wcschr`, `wcsrchr`, `wcsstr`, `wmemcpy`, `wmemmove`, `wmemset`).

- **`wcscmp`, `wcsncmp`** return the difference of the first two characters that differ
  (`wcsncmp(L"wide two", L"wide one", 7)` is 5), computed without sign: a character above
  `0x7fffffff` compares as greater. glibc returns -1, 0 or 1, and its `wchar_t` is unsigned on
  arm64 where macOS's is signed.
- Still without an answer, known: `strcoll` and `strxfrm` (they belong to the locale shims),
  `strsignal` (the signal numbers differ). A game with plugins of its own will ask for more
  of libSystem than this; `test/shims.sh cover` over a plugin's import list says what, before
  anything is run.

## How it is tested

`sh runtime/macho/test/shims-test.sh`, on an Apple-silicon Mac with Xcode's tools and Docker:

1. `test/build-mac.sh` builds `darwin_c.c` with `darwin_ctx.c`, `darwin_cxx.cpp` and
   `darwin_dl.c` with its plugins as macOS arm64 dylibs (the flags a game's image would have,
   plus `-mcpu=generic`) and as native programs, and runs the native programs: their output is
   the expectation. It builds everything three times, once per form of fixups a game can
   have: opcode streams (`test/out`), chained fixups as a target of macOS 12 makes them
   (`test/out/chained`), and chained fixups as a target of macOS 11 that asks for them makes
   them (`test/out/chained11`, the other pointer layout); it checks with `otool -l` that each
   file came out in the form meant, because the linker changes form on its own for some flags;
2. the same dylibs run through the loader (`test/machorun.c`) in an arm64 Linux container, with
   every rewrite forced (`UA_A64FIX=all`);
3. the two outputs must be the same, byte for byte, in each of the three forms.

The C test (339 lines of output) covers formatting, files, directories, time, sysctl, threads
(mutexes, conditions, keys, 200 stop-the-world rounds over running and sleeping threads, a
sleep and a socket read with a timeout that are stopped half way and still end on time),
locale, sockets, signal masks, Mach semaphores (the error numbers, a wait a handler
interrupts, a wait a destroy ends), contexts (a switch there and back, eleven arguments, a
chain of links, the fields macOS fills), the floating point environment, `rand`'s row,
libm's `errno` and the string family. The context tests are a file of their own (`darwin_ctx.c`) because
`<ucontext.h>` wants `_XOPEN_SOURCE`, and that macro changes the names the rest of the test
would import (see the `$DARWIN_EXTSN` names above): one file is built as a game's code is,
the other with the macro, and both sets of names are exercised. The C++ test (55 lines) covers
exceptions through 12 frames, `dynamic_cast`, streams, locales, containers, `std::thread`,
`thread_local`, futures, and a C++ plugin opened late: exceptions thrown in it and caught
outside, thrown outside and passing through its frames, its static objects and its
`thread_local`. Both match; the C test matched in 20 of 20 repeated runs, the C++ test in 100
of 100 (and in 15 of 15 with the plugin part).

The `dlopen` test (`darwin_dl.c`, plugins from `dl_plug.c`, 41 lines) runs natively against
dyld and then through the loader: a plugin with a dylib of its own and an initializer that
opens a third file, the order of initializers, `RTLD_NOLOAD` before and after, the image count
and the add-image callbacks, a second spelling of the path, `dladdr` into the plugin, its
thread-locals on two threads, a `.bundle`, `RTLD_LOCAL` and its promotion, a missing file
(`dlerror` once, then NULL), a plugin with a missing symbol opened twice, the same with a
dylib of its own (which must be gone afterwards, its initializer never run), two threads
opening one plugin whose initializer sleeps, `dlopen(NULL)`, `dlclose`, and a plugin
(`dl_fix`) made of pointers the loader has to set: 4,096 to its own data spread over pages
with 50,000 bytes holding none between them and the next, 1,024 into another image's table,
one before that table's start (a negative addend), one far into it, one to a name that is
allowed to be missing (must be NULL), function pointers, strings, and a thread-local's first
value; it sums what they point to (8386564, 7857664, every single pointer right), on macOS
and through the loader, in all three forms. None of them is `static`: a pointer whose every
use the compiler sees is folded away and never reaches the loader. Before that last plugin
was added the test matched in 40 of 40 repeated runs. A second run of the same file (`-- probe`, expectation checked in as
`darwin_dl.probe`) asks for the plugins by the names a game's code may have, which macOS
itself would only find through the bundle's own search paths.

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
the bridge before the player, hands it the loader's `dlopen` (for what a `dlopen` of the game's
code names that is not a Mach-O file; a Mach-O plugin in the lib folder is loaded by the shim,
see "Plugins the game opens"), and maps it as the player's `libil2cpp.so`. arm64 only.

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
(`[RequiredByNativeCode]` members of the Android modules, those not marked optional) against
what the game's own `global-metadata.dat` holds, not a diff of the two platforms' modules:
stripping differs per game. `pmu android-data --macho` makes that comparison
(`setup-rs/crates/androiddata/src/required.rs`), for the engine modules the build has an image
of, and puts it in the log and in the report (`player_required`: the modules compared, the
members required, those the bridge stands in for, those absent with no stand-in). A unit test
holds the tool's list of stand-ins to the bridge's.

The absent list is **what to look at when the player stops, not a verdict**. The test game
(707 required members in 36 modules): 6 are not in its build; 2 are the `Mobile` pair above;
the other 4 are `m_Ptr` and `m_IsWrapper` of `UnityEngine.UIElements.VisualManager` and
`VisualNodeClassNameStore`. The macOS UIElements module marks those four fields exactly as the
Android one does: the build's own stripping took them, because the game uses nothing of that
module and the macOS player then never asks. The Android player does not ask either, and the
game runs. A build does not say whether a member went with the platform or with stripping, so
the tool lists both and the player's own message ("Unable to find ...", then a stop with the
name in the bridge) decides.

Also open: 13 internal calls the image may ask for that the Android player does not register
(`UnityEngine.Screen::GetMainWindowDisplayInfo_Injected` among them; none was called by the
test game), and 6 il2cpp functions the Mac image does not export (none was called).

### The data: `pmu android-data --macho <Game>.app`

A macOS build keeps `Contents/Resources/Data` (the Data folder), with
`il2cpp_data/{Metadata,Resources}`, and `Contents/Frameworks/GameAssembly.dylib`. The Android
IL2CPP player reads the metadata from `assets/bin/Data/Managed/{Metadata,Resources,etc}`. The
conversion is the Android route's with `"steps": ["shaders", "settings"]`,
`"shaders": {"from": ["metal"]}` (a Mac build made for Apple silicon carries Metal programs
only) and `"settings": {"graphics_apis": [11]}`, and then

    pmu android-data --src <converted Data> --pkg <AndroidPlayer> --macho <Game>.app --out <out> --pack all

What `--macho` (`macho.rs`) does that the Mono and `--il2cpp` modes do not:

- finds the build's code and metadata, and refuses with the reason where the route cannot run
  it: no arm64 code in the binary (an Intel-only build; arm64e), metadata that is not plain
  IL2CPP metadata (an encrypted or packed build, which this route leaves alone), data or a
  player package of another Unity version than the build (compiled code goes with one exact
  version). A build that is not Unity 6000.0 gets a warning: nothing else is tested;
- writes the binary's arm64 part to `lib/arm64-v8a/GameAssembly.dylib` (a universal binary's
  slice, or the file as it is; nothing in it is changed);
- lays `il2cpp_data`'s `Metadata`, `Resources` and `etc` out under `Managed/`, where the
  Android player reads them, and takes the game's name from the `.app`;
- reads the list of assemblies from the build's metadata (the images in it; the build's
  `ScriptingAssemblies.json` where the metadata's version is one the tool does not read)
  instead of `Managed/*.dll`. The build's own `ScriptingAssemblies.json` may name an assembly the metadata
  has no image of (an editor package the build left no code of; the test game has
  `Unity.Multiplayer.Center.Common.dll`): with `--macho` both are the build's own and it runs
  so on macOS, so that is logged ("as built"), not refused;
- compares what the player requires by name with the build (above);
- surveys the build's native plugins and copies the ones the loader runs (below);
- writes `"scripting": "macho"` into `android_package.json`.

**The build's plugins** (`macho_plugins.rs`). Unity puts a macOS build's native plugins under
`Contents/PlugIns`: one that is set for a single CPU in `PlugIns/ARM64/` or `PlugIns/x86_64/`,
one for any CPU (a universal file, as all of Dave's) in `PlugIns/` itself; a bundle is a folder
`X.bundle/Contents/{MacOS/<code>, Info.plist}` whose code file `CFBundleExecutable` names. The
survey walks those folders (not `.dSYM`, not through links), takes each plugin once (of two
copies the one with arm64 code), reads it the way the loader does (the arm64 part, the load
commands, both bind streams with every opcode or the chained fixups' import table), and gives
each one of three verdicts, one log line each:

- **copied**: `lib/arm64-v8a/<name>` gets the plugin's arm64 code under the plugin's own name
  (`lib_x.dylib`; for a bundle the folder's name, `x.bundle`, as a plain file), unchanged.
  That is where the game's `dlopen` finds it ("Plugins the game opens" above). The condition:
  a library or bundle with arm64 code, not encrypted, with its fixups in a form the loader
  reads (the streams, or chained fixups in a plain 64-bit pointer layout), and every name it
  asks a library for has an answer: among the runtime's shims, in the route's libc++, or in
  another plugin of the build that is itself copied (decided as a fixpoint, so a plugin that
  needs one that is left out is left out too);
- **answered by the runtime**: a plugin that is a known SDK with a built-in answer (Steamworks,
  EOS, ...; `plugins.rs`'s table) is not copied; its import name gets the entry in
  `plugins.map`, and the game's `dlopen` of it goes to the host's side as before. An SDK
  whose answer is a library the port brings (FMOD, Wwise) gets that entry only when the
  build's own file cannot be run; where it can, the build's own file is the better one;
- **left out**, with the reason: no arm64 code, an encrypted file (left alone), a framework,
  a file that is no library, fixups the loader refuses (arm64e's signed pointers, both forms
  at once, neither), a name of the route's own files, or names without an answer
  (how many, of which library, the first six). The game then gets `DllNotFoundException` at
  its first call into it, as it would for a plugin missing on macOS; that is logged as a
  warning at setup, and games often never call a plugin they ship (Dave's code names
  neither the microphone library nor EOS).

Which names the runtime answers is a list in the tool (`macho_shims.txt`, 506 names), and a
test of the crate reads the `darwin_syms_*` tables out of `runtime/macho/*.c` and fails when
the two differ: a shim added to the runtime without the list, or the other way round, does not
get past `cargo test`. The survey is also in the conversion's report (`macho_plugins`: file,
import name, libraries, verdict, reason).

Two findings on the way. The player's version is read from the player itself: the version
string followed by the build stamp. An editor's `AndroidPlayer` folder also holds the string
"2018.3.0a1" (a serialization floor in `modules.asset` and in the library), which is not it.
And `pmu convert` works on the Data folder in place, so a setup converts a copy of the
`.app`'s Data folder (or the `.app` itself, when the player's copy may be used up) and hands
`--macho` the `.app` for the code and metadata.

A third, from running the tool in a container on a Mac: the free space the pack step sees for
a folder mounted from an external drive is the Mac's **internal** disk's (Docker Desktop's
file sharing answers `statfs` so), and the step refuses with "keeping 1 GiB spare" while the
drive has hundreds free. What had filled the internal disk was the test game's build script:
a Unity project per build in the system's temporary folder, 0.8 GB each and 2.8 GB with the
packages, never removed. `test/game/build.sh` makes its project inside the output folder now
and removes it when it ends.

### The first run

The test game (`runtime/macho/test/game`: the project's Pong, built for macOS arm64 with IL2CPP
by `build.sh`, never run on the Mac) under Unity 6000.0.52f1's arm64 Android player in the
emulator rig (`EMU_ARCH=arm64 UA_SCRIPTING=macho tools/emu/emu.sh`): the picture is right
(paddles, ball, the score's text), the score counts, 3 556 frames at the rig's 60 fps in 36 s,
RSS 216 MB. 241 of the 247 il2cpp functions passed on, 6 the image does not export; 19
internal calls needed a thunk (bool results; `Texture2D::Internal_CreateImpl_Injected` and
`FontEngine::TryAddGlyphToTexture_Internal_Injected` with two stack arguments each).

### The checks under the player (2026-11-10)

The test build carries one more component, `test/game/MachoChecks.cs`, which runs after the
game's first frames and logs `[checks] ok <name>` or `[checks] FAIL <name>` for what has to be
right between the game's code and the Android player's, then `[checks] done: N passed, M
failed`:

- results narrower than 32 bits and arguments past the eighth (`Graphics.CopyTexture`'s region
  form has twelve), floats, doubles, structs, strings and arrays through the player's functions;
- an exception the player raises inside one of its functions and the game catches (200 times
  each: `GameObject.FindWithTag` with an unknown tag, `Input.GetKey` with an unknown key name,
  `Transform.GetChild` out of range): it unwinds through the player's ELF frames, the bridge's
  thunk where the call was laid out again, and into the image's compact unwind info;
- an exception of the game's that reaches the player (a `SendMessage` handler throws): logged
  by the player, which goes on;
- the class library on this system (formatting, culture, time, the environment), files in the
  game's folder, PlayerPrefs;
- threads of the game's, and collections while four of them allocate and verify what they hold
  (below);
- the player's scheduling of the game's code: coroutines, a task's continuation back on the
  main thread, a destroyed object comparing equal to null.

- the game's own native plugins (`test/game/checks_plugin.c`, built by `build.sh` as
  `lib_checks.dylib` and as a bundle, `checks_bundle.bundle`, and set up for the macOS player
  only, so the editor never loads them): P/Invoke with 23 arguments of which 7 go on the
  stack in sizes of 1, 2, 4 and 8 bytes, a `bool` each way, a struct returned in registers and
  one through memory, strings and arrays both ways, `errno` through `SetLastError`, the
  plugin's initializer run once and a thread-local of its own, a function of the game's called
  back by the plugin (on the calling thread, and from a thread the plugin started itself
  while collections run), a plugin in a bundle, and the two refusals a game may rely on:
  `DllNotFoundException` for a plugin that is not there and `EntryPointNotFoundException` for
  a function it does not have;
- Burst (the build has the Burst package, and `MacBuild.cs` fails the build if no
  `lib_burst_generated.bundle` comes out): a job, a parallel job over 200,000 items on the
  player's worker threads, and a compiled function pointer the game's code calls itself
  (int, long, float, double and a reference). Each sets a flag that a `[BurstDiscard]`
  function clears, a function Burst leaves out of its code, so a check fails with "it ran as
  the game's ordinary code" when the library was not used, not only when a result is wrong.

IL2CPP under the Android player asks `dlopen` for a `DllImport`'s name as written, then with
`.so`, `.dll`, `.dylib`, `.bundle` behind it, then the same four with `lib` in front; the
first is the image's own import and reaches the shim, which finds `lib_checks.dylib` for
`lib_checks` and `checks_bundle.bundle` for `checks_bundle` at the first try.

Result (2026-11-10): the 16 checks without the plugins pass in the emulator (10 of 10 runs) and
on the handheld below; the 21 without Burst pass in the emulator (3 of 3 runs with the plugins
put in the lib folder by hand, and 3 of 3 with a tree made by `pmu android-data --macho`
alone, whose two plugin files are byte for byte the hand-placed ones); all 24 pass in the
emulator with the Burst build (6 of 6 runs, a tree made by `pmu android-data --macho`, which
copied Burst's library and the two test plugins and left out Unity's purchasing plugin, which
needs StoreKit and Objective-C). That build has Unity's default packages in it (UI Toolkit
among them), which adds four members the player requires and the macOS build does not have
(`UnityEngine.UIElements.VisualManager` and `VisualNodeClassNameStore`: `m_IsWrapper`,
`m_Ptr`); the player did not ask for them in this game. The plugin and Burst checks have not
run on a handheld yet.
The handheld is the BattleXP
G350 (RK3326: four Cortex-A35, 1 GB; dArkOS, 64-bit userland). There the Mac build's game runs
under the Android player at a steady 60 fps (frames of 16.6 ms on average, 18 to 20 ms the 99th
percentile), the picture right, 160 MB resident at the start and 192 MB once settled (after
about 80 s), 640 MB of the device's memory still available. One or two frames in five seconds
come late there (30 to 50 ms); that is the device and the test set-up, not the route: Unity's
own Android IL2CPP build of the same game (the arm64 donor, ELF all the way) shows the same on
the same device, 0 to 2 late frames in five seconds with the worst at 31 to 50 ms. (Run the
donor with `UA_FRAME_RATE=desktop` for such a comparison: as an imported APK it keeps Android's
rule, 30 fps for a game that sets no rate.)

What a check may expect is what a **release player** does, and that is less than the editor:
`Input.GetButton` with a name that is not set up, and `CompareTag` with an undefined tag, raise
in the editor and in development players only. The release players (the macOS one and the
Android one alike) have no such message in them: `GetButton` does not raise, and `CompareTag`
logs an error. `Input.GetKey` with an unknown name is an `ArgumentException` in both. When a
check fails, look for the message's text in both players' binaries before looking for a fault
in the route.

And a check about time has to name its clock. `WaitForSeconds` counts `Time.time`, which the
Android player moves by the display's frame times (a whole number of refresh periods each
frame), not by the real clock: in the frames after a long one, 0.2000 s of game time passed in
0.1833 s of real time in the emulator, and 0.2500 s in 0.2173 s on the G350. The check that
held the wait to `realtimeSinceStartup` failed once in ten emulator runs; it holds the wait to
`Time.time` now and logs both clocks.

### Collections and stopped threads

Unity's collector (Boehm) takes one lock for every allocation and every collection: an unfair
spin lock that yields twelve times and then sleeps with a backoff of up to about 16 ms
(`GC_lock`). On macOS it stops the world thread by thread (`thread_suspend`, then
`thread_get_state`), which the shim layer answers with a signal per thread. Two things
followed, both found by a check that asked for collections in a loop while four threads
allocated flat out, and took anything from a fraction of a second to many seconds:

- the collector's own behaviour, the same code as on a Mac: under back-to-back collections the
  thread that just collected takes the lock again almost at once, and a thread in the backoff
  can wait a long time. A check must not time that: it now gives each thread a set amount of
  verified work, and passes when the work is intact and the collections happened;
- a defect of the shim that made it far worse: a sleep interrupted by a stop was started
  again for what was left of it, so with thousands of stops a second a thread in the backoff
  barely moved. A suspended thread's sleep runs on in macOS's kernel; `nanosleep`, `usleep` and
  `sleep` now sleep to a deadline, as every other timed wait of the layer does (the shim
  section above), and the native comparison covers it.

`UA_MACHO_STOPS=1` logs the stops every five seconds: how many, the time the world stood, and
the `thread_suspend` calls with their longest. Measured with the test game: while the
collections check runs (16 to 21 threads to stop, over 500 collections asked for in 2.5 s on
the handheld), a stop takes 0.4 to 0.7 ms in the emulator and 2.7 ms on the G350 (4.6 ms the
longest, 22 us per `thread_suspend`). In play the G350 has about two stops a second of 0.3 to
0.5 ms each (2 ms the longest), three to five threads to stop: nothing its frame times show.
The stop is sequential (one signal and one acknowledgement per thread, as the Mach calls ask),
where the collector's Linux build signals all threads and then waits for them all. If a game
with more managed threads shows the stops in its frame times, the first `thread_suspend` of an
episode can signal every thread ahead (not built: measure first).

Open, known and not yet met: a thread stopped while it holds one of the shim layer's own
locks (the wait buckets of `__ulock_wait`, the first-use lock of pthread objects, stdio's)
would hold up the collector if it needs the same one before resuming; `Application.platform`
answers Android to the game; `streamingAssetsPath` is the path into the APK, as on Android.

Next, Dave's own needs: its plugins (the table above: the Steam answer on the Mach-O side;
`lib_coacd` and Burst's library are done), answers for the internal calls the
Android player lacks, a control build of the checks for Android itself; then Dave in the
emulator and on the handheld, with the stops measured again (it has more managed threads).
