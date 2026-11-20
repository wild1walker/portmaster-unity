# Retiring the armhf build: what the 64-bit build loses, counted in hardware, and the routes to win it back (2026-11-20)

The goal: an aarch64-only release whose Unity games run at least as fast as the armhf build does today,
with the same picture and play. [mono-arm64.md](mono-arm64.md) measured the gap with a sampling profiler
(Hollow Knight, 6000.0.61f1, King's Pass, 1 GB settings: UnityMain 16.7 - 17.0 ms a frame on the 32-bit
build, 18.3 - 18.6 ms on the 64-bit Mono route; libunity +0.9 ms over 14 pointer-walking functions, the
NDK's outline atomics +0.3 ms, the JIT's code +0.4 ms, the render thread not the limit) and closed the
levers on the runtime's side. A profiler says where the time is; it cannot say why the same engine code
takes longer. This page counts the why with the CPU's own counters, looks at how others closed 32 -> 64-bit
gaps on in-order ARM cores, and ranks what is left to try.

**The short answer** (details below): on the Cortex-A53 the 64-bit code does the same frame in 12 % fewer
instructions, with less than half the branch mispredicts and fewer pipeline interlocks, and would be about
1 ms a frame *faster* than the 32-bit build if it missed the caches as often. It does not: it takes 20 %
more L2 misses (to DRAM, about 220 cycles each), and waiting for those is the whole gap and more (muOS:
+3.3 M cycles a frame of load-miss stalls, +1.1 M of instruction-fetch stalls, against +3.1 M cycles in
all). The loss is the memory system's, about three quarters data and one quarter code, spread thinly over
the engine; no single function holds it. The routes that pay are the ones that take memory traffic off
UnityMain (FMOD's update to an idle core, code layout of libunity) or hide it (prefetch in rewritten hot
functions), not the ones that make instructions cheaper.

## The counters: `hwcount` (runtime/host/hwcount.c)

A static aarch64 program that opens the kernel's PMU counters (`perf_event_open`, the `armv8_pmuv3`
PMU both RG34XX-SP kernels expose: 27 common events, plus the Cortex-A53's implementation-defined ones by
raw code) on chosen threads of a running game, as root (the games run as root; `perf_event_paranoid` is 3
and is left alone; no `perf` binary on the handhelds). The 64-bit kernel counts a 32-bit process's threads
the same way, so one build serves both players. Groups of cycles + instructions + four events are counted
one at a time in 100 ms slices round-robin (no multiplexing inside a group; every group carries cycles and
instructions, so its events are read against them); user mode only (the code the builds differ in). The
A53's stall events are the useful ones: `ld_dep_stall` (0xE7, cycles the write-back stage waits for a load
that missed), `ic_dep_stall` (0xE1, cycles the issue queue is empty while an instruction-cache miss is
served), `iutlb_dep_stall` (0xE2), `agu_dep_stall` (0xE5), `st_dep_stall` (0xE8) and the others of the TRM's
table. `profrun.sh` runs it during the measurement with `HWCOUNT=1` (`HWCOUNT_ARGS` its options; profbench.sh
sends the binary, `runtime/host/out/arm64/hwcount`); `hwcount -L` is a pointer-chase latency sweep. Per-function
attribution uses the profiler's existing `UA_PROF_EVENT` (a sample every N occurrences of an event).

Built with `gcc -O2 -static` in the ua-build-arm64 image (runtime/host/build.sh, ARCH=arm64).

## UnityMain, 32-bit against 64-bit, counted

Hollow Knight, the same installs and settings as [mono-arm64.md](mono-arm64.md) (the 64-bit one with 8-byte
granules, `lib-gG8`), King's Pass standing, `tools/device/profbench.sh` 25 s runs from a cool handheld,
counters over 24 s of it; a frame's worth = count / (the group's wall time x the run's frame rate; the first
runs, before the tool printed each group's wall time, take 24 s / 7 groups: within 3 % of it). muOS runs
the A53s at 1.8 GHz, Knulli at 1.512 GHz (both read, not set).

| UnityMain, a frame | muOS 32-bit | muOS 64-bit (2 runs) | Knulli 32-bit (2 runs) | Knulli 64-bit (3 runs) |
|---|---|---|---|---|
| fps | 52.9 | 51.2, 51.2 | 53.6, 53.6 | 51.1, 51.4, 51.1 |
| cycles (user) | 28.4 M | 31.5, 31.6 M | 25.8, 25.8 M | 27.5, 27.2, 27.2 M |
| instructions | 5.15 M | 4.50, 4.53 M | 5.11, 5.11 M | 4.46, 4.48, 4.44 M |
| IPC | 0.181 | 0.143 | 0.198 | 0.164 |
| L1I refills | 125 K | 120, 117 K | 124, 125 K | 121, 119, 115 K |
| L1D refills | 93 K | 100, 101 K | 94, 93 K | 100, 100, 99 K |
| L1I / L1D micro-TLB refills | 9.1 / 13.8 K | 8.7 / 14.3 K | 8.5 / 12.9 K | 8.6 / 14.2 K |
| L2 accesses | 348 K | 365, 361 K | 346 K | 363, 362 K |
| **L2 refills (from DRAM)** | **72.6 K** | **87.6, 87.4 K** | **73.0, 72.7 K** | **86.1, 86.4, 85.2 K** |
| bus accesses | 314 K | 379, 374 K | 318 K | 373, 371 K |
| branch mispredicts | 97.6 K | 44.6, 44.7 K | 100.5, 100.1 K | 44.4, 44.0, 44.0 K |
| loads / stores retired | 1.26 / 0.60 M | 1.04 / 0.52 M | 1.25 / 0.60 M | 1.04 / 0.52 M |
| **cycles stalled on a load miss** (0xE7) | **15.2 M** | **18.5, 18.5 M** | **13.1, 13.2 M** | **15.2, 15.4, 15.1 M** |
| **cycles stalled on an i-cache miss** (0xE1) | **5.8 M** | **6.9, 6.9 M** | **5.2, 5.2 M** | **6.2, 5.8, 6.0 M** |
| iTLB miss stalls (0xE2) | 0.75 M | 0.77, 0.79 M | 0.68 M | 0.71, 0.68 M |
| store stalls + store buffer full | 0.81 M | 1.04, 1.04 M | 0.71 M | 0.93, 0.87 M |
| address-generation interlocks (0xE5) | 0.81 M | 0.44, 0.44 M | 0.80 M | 0.44, 0.44 M |
| other interlocks, SIMD, IQ empty, decode | 1.35 M | 0.95 M | 1.34 M | 0.94 M |

What it says, muOS (Knulli the same within 10 %):

- **The frame is memory-bound on both builds.** IPC 0.14 - 0.20 on a dual-issue core; 53 - 59 % of
  UnityMain's cycles wait for a load that missed and 20 - 22 % for instruction fetch. Each L2 refill costs
  about 220 cycles (the extra load-stall cycles over the extra refills: 3.3 M / 15 K).
- **The 64-bit code is the better code.** 12 % fewer instructions for the same frame (A64's addressing
  modes, register count, no IT blocks), 54 % fewer branch mispredicts (53 K x ~8 cycles), half the
  address-generation interlocks, no pre-decode stalls (Thumb-2's mixed widths): about -1.8 M cycles.
- **The 64-bit data misses L2 more.** L1D refills +8 %, L2 refills +20 % (+15 K a frame), bus accesses
  +20 %: the same accesses (L2 accesses +4 %), a working set that no longer fits the L2. +3.3 M load-stall
  cycles (1.8 ms at 1.8 GHz) and +0.2 M store stalls.
- **The 64-bit code misses L2 more too**: fewer L1I refills (-4 %) but +1.1 M i-cache stall cycles: the
  refills come from DRAM more often (the L2 is shared and fuller, and A64's hot code is 14 % larger).
- **TLBs are not it**: micro-TLB refills within 5 %, iTLB stall cycles the same. (No THP on these 4.9 kernels:
  [mono-arm64.md](mono-arm64.md); the A53's 512-entry main TLB covers 2 MB of 4 KB pages, and the counters show
  no growth there.)
- Net +3.1 M cycles (muOS) / +1.5 M (Knulli), which is the profiler's +1.5 - 1.9 ms.

So the gap is cache capacity, not instruction count, pointer width in registers or decode: if the 64-bit
build's L2 behaved like the 32-bit one's, its UnityMain would be about 1.8 M cycles (1 ms) a frame *shorter*
than the 32-bit build's.

The render thread (UnityGfxDeviceW) shows the same shape and shares the L2: muOS 17.4 -> 20.4 M cycles a
frame, L2 refills 35.2 -> 46.7 K (+33 %), i-cache stall cycles 4.7 -> 7.0 M (the 64-bit libmali's code,
+13 MB), L1I refills +15 %. Its misses compete with UnityMain's for the same L2.

**The memory hierarchy** (`hwcount -L` on the Knulli RG34XX-SP, CPU 3 at 1.512 GHz, a random pointer chase
one 64-byte line a step; Allwinner does not publish the H700's L2, the related H616's is 256 KB or 1 MB by die
revision, [lkml](https://lkml.iu.edu/hypermail/linux/kernel/2405.3/04482.html)):

| working set | 8 - 32 KB | 64 - 512 KB | 1 MB | 2 - 16 MB |
|---|---|---|---|---|
| ns a load | 2.3 (L1D, 3.5 cycles) | 9.3 - 11.4 (L2, 14 - 17 cycles) | 61 (partly L2) | 185 - 199 (DRAM, ~290 cycles) |

A 1 MB L2 shared by the four cores. UnityMain alone runs about 0.5 MB of engine code a frame, the render
thread the Mali driver's, and the frame's data on top: the L2 cannot hold a frame's working set on either
build, and the 64-bit one's is larger.

### Where the extra misses are (event-sampled profiles)

`UA_PROF_EVENT` on UnityMain, 25 s each, the same scene (Knulli: `l2d_cache_refill` every 5,000 and
`ld_dep_stall` every 1,000,000 cycles; muOS: `ic_dep_stall` every 400,000), each player's symbols; a frame's
worth (an `l1i_cache_refill` profile on muOS adds: libunity's own L1I refills +14 %, Mono's and libc's
fewer):

| L2 refills a frame (Knulli) | 32-bit | 64-bit | | load-miss stall cycles a frame (Knulli) | 32-bit | 64-bit |
|---|---|---|---|---|---|---|
| all of UnityMain | 74 K | 87 K | | all of UnityMain | 12.65 M | 14.75 M |
| libunity | 54.6 K | 66.1 K | | libunity | 9.36 M | 11.36 M |
| managed (JIT) | 11.3 K | 14.1 K | | managed (JIT) | 1.90 M | 2.37 M |
| Mono, libc, ua_host, other | 8.3 K | 6.1 K | | Mono, libc, ua_host, other | 1.39 M | 1.02 M |

The libunity increase is spread: the 20 functions that grew most add +7.3 K of the +11.5 K L2 refills and
+1.5 M of the +2.0 M stall cycles, none more than 0.22 M. The largest (load-miss stall cycles a frame,
32-bit -> 64-bit): `FMOD::SystemI::UpdateAudibilities` 0.36 -> 0.57 M (its channel walk; the most of any
function on either build), `Scripting::GetCachedPtrFromScriptingWrapper` 0 -> 0.21 M (inline in the 32-bit
player, where the same miss sits in `MonoBehaviour::CallUpdateMethod` 0.39 -> 0.31 and `GetScriptCache` 0.29
-> 0.27), `BaseRenderer::FlattenBasicData` 0.05 -> 0.20 M, the NDK's `__aarch64_ldadd4/8_relax` 0 -> 0.22 M
(the atomic's target line: reference counts; inline in the 32-bit player), `Playable::Handle` 0.11 -> 0.20,
`Behaviour::GetEnabled` 0.01 -> 0.09, `Transform::GetTransformAccess` 0.07 -> 0.14,
`GameObject::QueryComponentByType` 0.20 -> 0.26, `PrepareSpriteRenderNodes` 0.06 -> 0.11, culling
(`ProcessCameraIndexListIsNodeVisibleStep1`) 0.12 -> 0.15; PlayMaker's managed `UpdateStateChanges`,
`FsmState.OnUpdate`, `set_Fsm` +0.24 M together.

The instruction-fetch stalls are spread even more thinly: 6.1 -> 7.2 M cycles a frame (muOS), libunity 4.0
-> 5.2 M, and no libunity function above 0.12 M on either build: the misses are in code that runs once or a
few times a frame (camera setup, render-target setup, culling setup, the scriptable render loop's
bookkeeping, hash-set lookups), each fetched from L2 or DRAM every frame because 500 - 560 KB of engine
code runs between two visits.

### The engine's code footprint

From the time profiles of [mono-arm64.md](mono-arm64.md)'s 45 s recordings (UnityMain's on-CPU samples by
the function they fell in; muOS):

| libunity code UnityMain runs | 32-bit (Thumb-2) | 64-bit (A64) |
|---|---|---|
| functions with a sample | 1,750 | 1,794 |
| 50 % of the samples | 77 functions, 43 KB, 77 pages | 75 functions, 44 KB, 74 pages |
| 90 % | 809 functions, 329 KB, 443 pages | 824 functions, 354 KB, 450 pages |
| all sampled functions | 494 KB over 687 4 KB pages | 564 KB over 744 pages |

The whole engine's code is 16.3 MB (Thumb-2) against 19.5 MB (A64), 1.21x; the code a frame runs, 1.14x.
Both builds run about half a megabyte of engine code a frame scattered over 2.7 - 3 MB of address space (the
hot functions lie between 0x75xxxx and 0x185xxxx of the arm64 library: the linker's order, not the frame's):
16 - 18 times the 32 KB L1I, and as large as or larger than the whole shared L2.

Also counted (objdump of the arm64 `libunity.so`): 4.89 M instructions; 3,340 call sites of 32 outline-atomic
helpers (`__aarch64_ldadd4_rel` 598, `_ldadd4_relax` 428, `_cas4_acq` 332, `_ldadd4_acq_rel` 324, ...),
each helper `bti c; adrp; ldrb; cbz` (the LSE test) before its LL/SC loop; 684 `bti` and 676 `paciasp`
(hint space: no-ops on the A53, one issue slot each); no literal pools; `-Os` code (every compile command
of Unity's Android build).

## What others did (research)

**Unity's own 32 -> 64-bit move on Android (2019's Google Play requirement).** The reported arm64 regressions
were mostly not code generation: an IL2CPP arm64 build's stack-trace resolution for `Debug.Log` was ~28x
slower than ARMv7's (Unity issue tracker, [arm64 takes longer to execute stacktrace api](https://issuetracker.unity3d.com/issues/android-arm64-architecture-takes-longer-to-execute-stacktrace-api-than-armv7);
the forum thread [60 fps ARMv7 to 25/30 fps ARM64](https://discussions.unity.com/t/huge-fps-drops-from-60fps-armv7-to-25-30-fps-arm64-unity-2017-4-20/744170)
fixed it by turning stack-trace logging off). Our runs log through ua_host with Unity's stack traces off
already; not this. Unity's engineers measured outline atomics on a Cortex-A53 (Honor 9) at 40 - 170 % slower
per operation than inline LL/SC (`fetch_add(relaxed)` 8.8 -> 13.5 ns, a failing `cmp_xchg_weak` 4.7 -> 12.8 ns;
Arm attributed it to the in-order core's branch prediction) and decided against outline atomics then
([Arm outline atomics](https://over17.github.io/2023/03/12/arm-outline-atomics.html)) - yet 6000.0's arm64
libunity has them (the NDK's default). Unity 6.5 turns on ThinLTO for libunity and reports -3.7 % main-thread
CPU on arm64 ([Boosting Android performance with LTO](https://discussions.unity.com/t/boosting-android-performance-with-lto-optimizations-in-unity-6-5/1720008)):
the engine's own code layout and inlining are worth several per cent, which is the part we can redo after
the fact. IL2CPP's `Il2CppSetOption` (null and bounds checks) and managed stripping concern IL2CPP-compiled
scripts ([IL2CPP runtime checks](https://docs.unity3d.com/Manual/il2cpp-runtime-checks.html)); on the Mono
route the scripts are JIT code (0.44 ms of the gap, being worked on through Mono LLVM AOT elsewhere). No
Unity source found a Cortex-A53-specific arm64 slowdown in the engine itself or a remedy for it.

**Pointer compression (keep 32-bit references in a 64-bit process).** V8: 32-bit offsets from a 4 GB-aligned
base, up to 43 % smaller V8 heaps; on low-end arm64 the decompression cost barely registered
([v8.dev](https://v8.dev/blog/pointer-compression)). The JVM's compressed oops: IBM's compressed references
brought 64-bit WebSphere to within 5 % of 32-bit speed and 1 % of its footprint
([Ramarao](https://www.eecg.utoronto.ca/~steffan/workshops/08/cdp/ramarao.html)). Android's ART stores heap
references as 32 bits on arm64 (`HeapReference`, [object_reference.h](https://android.googlesource.com/platform/art/+/d160641/runtime/mirror/object_reference.h)):
Android's own runtime avoided exactly this cost. ILP32 ABIs: x32 5 - 8 % faster than x86-64 on SPECint, 40 %
on pointer-heavy mcf ([X32 ABI](https://en.wikipedia.org/wiki/X32_ABI)); AArch64 ILP32 +5 - 14 % on SPECint
2006 on ThunderX2 ([Kuvyrkov](https://lore.kernel.org/all/266952F2-53F5-4D5E-83F0-6C8203092F67@linaro.org/)),
never merged into Linux or glibc. **Not open to us**: libunity is compiled against 64-bit IL2CPP object and
engine layouts (mono-arm64.md, CoreCLR), and its own structures are fixed in its binary; Mono has no
compressed references. These results do confirm the mechanism the counters show: the cost of 64-bit is the
cache footprint of pointers, 5 - 15 % on pointer-heavy code.

**Running 32-bit code on 64-bit-only hardware.** MAMBO-X64 (dynamic translation of AArch32 to A64, keeping
the guest's 32-bit data layout): 7.5 - 8.3 % geomean overhead against native AArch32 on SPEC CPU2006 on a
Cortex-A53, ~0 % on out-of-order X-Gene ([PLDI 2017](https://pldi17.sigplan.org/details/pldi-2017-papers/28/Low-Overhead-Dynamic-Binary-Translation-on-ARM),
[Beehive report](https://arxiv.org/pdf/1509.04085)); OEM phones on Snapdragon 8 Gen 3 (no AArch32) ship
"Tango" for the same job ([Android Authority](https://www.androidauthority.com/oneplus-12-32-bit-app-support-3394525/)).
box64's box32 runs 32-bit x86 programs in a 64-bit process with wrapped native libraries (experimental,
[box86.org](https://box86.org/2024/12/new-version-of-box64-v0-3-2-and-box86-v0-3-8/)); FEX's thunks forward
GL from a guest to the host's 64-bit driver, handling the 32/64-bit layout differences per call
([FEX thunks](https://deepwiki.com/FEX-Emu/FEX/3.2-library-thunking-system)). On an A53 a translator of
Unity's armv7 player would start ~8 % behind the native 32-bit player, worse than today's 64-bit build.

**Post-link code layout.** BOLT (Facebook) and Propeller (Google) reorder functions and basic blocks from a
profile and split hot from cold; Propeller reports 1 - 8 % on top of PGO+ThinLTO on warehouse binaries, 7 %
on Clang ([Propeller, ASPLOS 2023](https://research.google/pubs/propeller-a-profile-guided-relinking-optimizer-for-warehouse-scale-applications/)).
BOLT supports AArch64 in relocation mode only in practice: it wants `--emit-relocs` (`-Wl,-q`) binaries
([BOLT AArch64 status](https://llvm.googlesource.com/llvm-project.git/+/refs/heads/main/bolt/docs/BOLTAArch64OptimizationStatus.md));
in non-relocation mode it keeps every function at its address, warns "non-relocation mode for AArch64 is not
fully supported", and can only rewrite inside a function and split its cold part out (BOLT's
RewriteInstance.cpp). Unity ships libunity without static relocations, so BOLT cannot move its functions.
Android's NDK and Chrome use linker order files for the same locality (`--symbol-ordering-file`,
[NDK order files](https://developer.android.com/ndk/guides/orderfile),
[Chrome orderfile](https://chromium.googlesource.com/chromium/src/+/main/docs/orderfile.md)); that needs the
link, which we do not have. The function-moving half has to be our own (below): A64 makes it tractable
(fixed 4-byte instructions; every pc-relative form is one of B/BL/B.cond/CBZ/TBZ/ADR/ADRP/LDR literal;
function pointers in data are R_AARCH64_RELATIVE relocations our loader applies itself; FDEs in
`.eh_frame` give every function's extent).

**Cortex-A53/A35 specifics.** In-order, dual-issue; a load miss stalls the pipeline at its first use (the
`ld_dep_stall` event); 32 KB L1I/L1D, 10-entry micro-TLBs, a 512-entry main TLB, a shared L2 (1 MB on the
H700 by the latency sweep above). Arm's
and Unity's measurements agree that out-of-line calls and their branches cost more on it than on
out-of-order cores (above). The hardware data prefetcher follows strides, not pointer chains (the 32-bit and
64-bit builds both get ~20 K prefetch line fills a frame against 73 - 87 K demand misses); the A53's
prefetch controls are EL1 registers, not ours to touch. PRFM (software prefetch) is architecturally a hint:
it changes no result.

## The routes, ranked

Expected gains are ms of UnityMain a frame on muOS (1.8 GHz; 1 M cycles = 0.56 ms); the 64-bit build needs
about -1.5 ms to match today's 32-bit one. Several stack.

### 1. FMOD's update on an idle core (main-thread work moved, exactly)

What: `AudioManager::Update` calls `FMOD::System::update` once a frame on UnityMain
(PostLateUpdate.UpdateAudio, before the frame's rendering work); every sample of `UpdateAudibilities` sits
under it (`updateChannels < SystemI::update < System::update < AudioManager::Update`). Hook
`FMOD::System::update` (libunity's internal symbol, hooked at load as runtime/host/unityhooks.c does) to hand
the call to a ua_host worker thread and return; wait for it before the next FMOD call from UnityMain (or at a
fixed later point of the frame, verified to have no FMOD call in between).

Evidence: FMOD's code is 4.8 % of the 64-bit UnityMain's on-CPU samples (4.2 % on 32-bit), `System::update`
3.5 % inclusive (~0.65 ms), and `UpdateAudibilities` is the function with the most load-miss stall cycles on
either build (0.57 M a frame on 64-bit: a pointer walk that also evicts UnityMain's other data from L1). The
cores have room: UnityMain 18.5 ms, render thread 12.8, the job worker 6.0, FMOD's mixer 4.9, Mali 3.0 ms of
a 19.6 ms frame across four cores.

Expected: -0.5 to -0.7 ms (both builds). Effort: small (one hook, a worker, a join; a counter that FMOD is
not entered from two threads at once). Exactness: FMOD's low-level API is thread-safe (Unity does not init
it `THREAD_UNSAFE`: to be checked in the init flags); the update reads the same state and runs within the
same frame; scripts see channel state (`isPlaying`, `time`) as of the last completed update either way if the
join comes before the next frame's scripts. Risk: a game that calls FMOD between UpdateAudio and the join
(the hook can count that and fall back to the inline call). First step: `UA_API_COUNT`-style counting of
FMOD entry points between `System::update` and the next frame's first script callback, and the update's time
measured on its own thread.

### 2. Profile-guided relayout of libunity's hot code (our own BOLT for a binary without relocations)

What: at load, copy the functions a frame runs (from `UA_PROF` profiles: 824 functions / 354 KB cover 90 %
of UnityMain's libunity samples, 1,794 / 564 KB all of them) into one contiguous executable region in the
frame's call order, hot blocks first, cold blocks split out; fix every pc-relative instruction in the copies;
point the original entry at the copy (`b`), BL/B between moved functions straight at the copies, and the
R_AARCH64_RELATIVE relocations that hold their addresses (vtables, function tables) at the copies (so_loader.c
applies them already). Jump tables survive a whole-function move (clang's A64 tables are offsets from an ADR
inside the function). The instruction words do not change, only branch and address-forming fields: the
engine computes exactly what it did.

Evidence: 20 - 22 % of UnityMain's cycles are instruction-fetch stalls (6.9 M a frame on 64-bit, +1.1 M
against 32-bit), spread over code that runs a few times a frame; the 564 KB of code a frame runs lies over
744 pages of 4 KB, much of each line cold; the L2 is shared with the data, so code lines that go also leave
room for the data misses that are three quarters of the gap. BOLT/Propeller-class layout gives 1 - 8 % on
large binaries; Unity's ThinLTO alone 3.7 % of main-thread CPU. Unity's `-Os` code runs straight through
cold paths; hot/cold splitting packs the lines.

Expected: -0.5 to -1.0 ms (cuts in i-cache stalls of 15 - 30 % plus some data relief); the 32-bit build gets
nothing from it unless done for Thumb-2 too (harder: mixed widths, literal pools). Effort: large (an A64
rewriter for a subset of functions, a profile-to-layout step at setup or a shipped order list per Unity
version, `.eh_frame` entries for the copies registered so that C++ exceptions and Unity's crash handler still
unwind, the profiler's naming of the copies). Risks: a missed pc-relative form or a function whose address is
compared (both detectable in a verifier pass); patching Unity's code in memory at load is the existing pattern
(unityhooks.c, ASMPATCH_*), nothing of Unity's ships. First step: an offline pass over 6000.0.61f1's arm64
libunity that classifies the 1,794 sampled functions (pc-relative forms, jump tables, address-taken through
RELATIVE relocations, FDE present) to show what fraction can move mechanically; then the 75 functions of the
first half of the samples moved in a test build, `hwcount` A/B on `ic_dep_stall` and L2 refills.

### 3. Rewritten hot functions with prefetch, inline atomics and the cached-pointer read folded in

What: on top of route 2's copies (or as single hooks), exact replacements for the functions whose load-miss
stalls grew most, written to hide the miss: `UpdateAudibilities` (if route 1 does not take it off the thread),
`FlattenBasicData`, `PrepareSpriteRenderNodes`, the culling index-list walk, `Playable::Handle` /
`PreOrderPlayableTraverser::Traverse`, `BaseBehaviourManager::CommonUpdate`, `QueryComponentByType`,
`CallUpdateMethod` with `GetCachedPtrFromScriptingWrapper` inline: `PRFM` of the next element an iteration
ahead (array and index walks), the NDK atomics' LL/SC loops inline with the same memory order (exact: the
helper's non-LSE path, minus the call and the LSE test), the same loads and stores in the same order otherwise.

Evidence: these functions add +1.2 M of the +2.1 M load-stall cycles (Knulli) and the atomics +0.22 M more;
Unity's A53 numbers put outline atomics at +5 - 10 ns an operation; the A53 overlaps a prefetch with work
already issued.

Expected: -0.2 to -0.4 ms (prefetch where the walk is an array; pointer chains gain little). Effort: medium per
function (each one disassembled and rewritten against its 6000.0.61f1 bytes, matched by `unityhooks_matches`).
Exactness: a prefetch has no architectural effect; the atomics keep their ordering. Risk: per-version work
(each Unity line has its own bytes); the earlier exact hooks of single functions measured ~1 % each
(profiling.md), which is in range. First step: the outline-atomic helpers patched to branch past the LSE test
(32 helpers, `adrp; ldrb; cbz` -> `b` to the LL/SC loop: exact on a CPU without LSE, and the A53/A35 have none),
`hwcount` A/B; then `FlattenBasicData` with a prefetch.

### 4. Less L2 traffic from the other threads

What: the render thread's own L2 refills grew 35 -> 47 K a frame and its i-cache stalls 4.7 -> 7.0 M cycles
(the 64-bit libmali's 25 MB of code against 12 MB), in the same L2 UnityMain misses in. ua_host's GL layers
(1.05 ms of the render thread on both builds) are ours: fewer distinct code paths and smaller tables there,
fewer GL calls (each into libmali's code), and the job worker's and FMOD mixer's buffers sized to the game.

Evidence: indirect - the L2 is shared and the counters cannot split whose lines evict whose. Expected: unknown,
probably 0.1 - 0.3 ms; measurable cheaply: an A/B of `UA_GL_REC`/`UA_GL_MEMO`-style settings that remove render-
thread work and UnityMain's L2 refills counted with `hwcount`. Effort: small to measure. Exactness: GL layers
already keep the picture identical.

### 5. Unity's allocator settings for locality (data side, both builds)

What: boot.config `memorysetup-bucket-allocator-granularity` / `-bucket-count` / `-block-size` (libunity reads
them; `UA_BOOT_CONFIG` passes them) so that small engine objects that grew past the bucket allocator's largest
class on 64-bit go back into its dense blocks. Evidence: none yet beyond the mechanism (64-bit structures are
up to 2x, the bucket classes are byte sizes); the 1 MB block setting changed memory, not frame rate. Expected:
0 - 0.2 ms. Effort: a few runs with `hwcount`. Exactness: where allocations lie, not what is computed.

### 6. Managed code: the JIT's objects (being done elsewhere)

Mono LLVM AOT of the scripts at setup (about -0.8 ms expected, another helper's work; not duplicated here).
The counters add one thing: managed code's load-miss stalls grew 1.90 -> 2.37 M cycles a frame and its L2
refills 11 -> 14 K, so the gain of better code generation is capped by the same misses: an LLVM build that
also emits prefetches for PlayMaker's list walks would add to it.

### 7. A 32-bit game process with a 64-bit GPU process (records only)

What: Unity's armv7 player in a 32-bit process (static ua_host, its own loader and bionic shims, no 32-bit
system libraries), its GL forwarded through shared memory to a 64-bit process that owns the 64-bit libmali,
the way FEX/box32 thunk GL (ua_host's GL recorder, `UA_GL_REC`, already serializes the frame's GL). It keeps
the 32-bit data layout and moves the driver to another process. It still ships armhf binaries (Unity's armv7
player and our 32-bit host): it removes the need for a 32-bit userland on the handheld, not the armhf build.
Expected: the 32-bit build's UnityMain, plus the cost of forwarding (the render thread already runs apart, so
UnityMain would only pay the copy). Effort: large. Use: only if a firmware without a 32-bit userland must be
served before routes 1 - 3 close the gap.

### 8. Translating the armv7 player to A64 with 32-bit data (moonshot, not recommended)

A static or dynamic AArch32 -> A64 translator for libunity/libmono (MAMBO-X64's design), keeping 32-bit
pointers in a low-4 GB address space, calling the 64-bit libmali through thunks. It would keep the 32-bit
build's cache footprint with A64 code, but MAMBO-X64's 8 % overhead on the A53 is larger than today's gap,
and translating a 16 MB Thumb-2 engine exactly (flags, IT blocks, exclusive monitors, self-referencing literal
pools) is years of work. Recorded for completeness.

### Not routes (closed by the counters)

- Huge pages / TLB work: TLB refill counts and iTLB stall cycles are the same on both builds.
- Branch layout for the predictor: the 64-bit build mispredicts half as often already.
- Decode width / code density as such: the 64-bit build retires fewer instructions; only the cache footprint
  of its code matters (route 2).
- Mono's own code: Mono, libc and ua_host have fewer misses on the 64-bit build than on the 32-bit one.

## Summary table

| route | expected UnityMain a frame | effort | exact? | first step |
|---|---|---|---|---|
| 1. FMOD update on a worker | -0.5 to -0.7 ms (both builds) | small | yes, with the join before the next FMOD call | count FMOD calls between `System::update` and the next frame's scripts; time the update alone |
| 2. libunity hot-code relayout | -0.5 to -1.0 ms (64-bit) | large | yes (instructions unchanged but branch fields) | classify the 1,794 sampled functions for a mechanical move; move the top 75; A/B `ic_dep_stall` |
| 3. rewritten hot functions (prefetch, inline atomics) | -0.2 to -0.4 ms | medium, per Unity version | yes | outline-atomic helpers branched past the LSE test; `FlattenBasicData` with a prefetch |
| 4. less render-thread L2 traffic | 0.1 - 0.3 ms (to be measured) | small to measure | yes | `hwcount` on UnityMain with render-thread work removed |
| 5. Unity bucket allocator settings | 0 - 0.2 ms | small | yes | three boot.config variants, `hwcount` |
| 6. Mono LLVM AOT (elsewhere) | about -0.8 ms | in progress | as measured there | - |
| 7. 32-bit game + 64-bit GPU process | 32-bit speed, still armhf | large | yes | only if a firmware lacks a 32-bit userland |
| 8. armv7 -> A64 translator | worse than today | very large | hard | none |

Routes 1 + 2 + 6 together reach the 1.5 - 1.9 ms the 64-bit build needs; 3 - 5 are margin. Route 1 is the
first prototype: one hook, measurable in a day, and it pays on both builds and every Unity game that uses
Unity's built-in audio.
