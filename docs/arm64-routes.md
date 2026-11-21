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

| UnityMain, a frame | muOS 32-bit (2 runs) | muOS 64-bit (3 runs) | Knulli 32-bit (2 runs) | Knulli 64-bit (3 runs) |
|---|---|---|---|---|
| fps | 52.9, 52.5 | 51.2, 51.2, 51.1 | 53.6, 53.6 | 51.1, 51.4, 51.1 |
| cycles (user) | 28.4, 28.1 M | 31.5, 31.6, 31.8 M | 25.8, 25.8 M | 27.5, 27.2, 27.2 M |
| instructions | 5.15, 5.14 M | 4.50, 4.53, 4.48 M | 5.11, 5.11 M | 4.46, 4.48, 4.44 M |
| IPC | 0.181 | 0.143 | 0.198 | 0.164 |
| L1I refills | 125, 130 K | 120, 117, 123 K | 124, 125 K | 121, 119, 115 K |
| L1D refills | 93, 94 K | 100, 101, 101 K | 94, 93 K | 100, 100, 99 K |
| L1I / L1D micro-TLB refills | 9.1 / 13.8 K | 8.7 / 14.3 K | 8.5 / 12.9 K | 8.6 / 14.2 K |
| L2 accesses | 348 K | 365, 361 K | 346 K | 363, 362 K |
| **L2 refills (from DRAM)** | **72.6, 72.1 K** | **87.6, 87.4, 87.3 K** | **73.0, 72.7 K** | **86.1, 86.4, 85.2 K** |
| bus accesses | 314 K | 379, 374 K | 318 K | 373, 371 K |
| branch mispredicts | 97.6, 97.2 K | 44.6, 44.7, 44.4 K | 100.5, 100.1 K | 44.4, 44.0, 44.0 K |
| loads / stores retired | 1.26 / 0.60 M | 1.04 / 0.52 M | 1.25 / 0.60 M | 1.04 / 0.52 M |
| **cycles stalled on a load miss** (0xE7) | **15.2, 14.9 M** | **18.5, 18.5, 18.7 M** | **13.1, 13.2 M** | **15.2, 15.4, 15.1 M** |
| **cycles stalled on an i-cache miss** (0xE1) | **5.8, 5.7 M** | **6.9, 6.9, 7.0 M** | **5.2, 5.2 M** | **6.2, 5.8, 6.0 M** |
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

## The graphics side, counted: render thread, Mali driver and GPU (2026-11-20)

The same Hollow Knight installs and settings as above (King's Pass standing, the release's 1 GB settings,
25 s from a cool handheld, 32-bit and 64-bit alternated A B A B in one hold; every run checked at the scene:
the plan's hitch marker, 85 draws a frame), with four measurements at once: `UA_GL_REC=time` (the render
thread's time in each GL entry point, read around the call at the top of ua_host's GL layers, so it is the
driver's time plus the layers' below the recorder), `GPUCNT=1` (the Mali's own counters: GPU-active cycles,
`gpucnt`), `HWCOUNT=1 HWCOUNT_ARGS='-t UnityGfxDeviceW'` (the A53's counters on the render thread) and
`freq.log` (the GPU clock and the thermal cooling devices' states each second). On the G350, whose GL runs on
ua_host's GL thread (`UA_GL_REC=auto`), `UA_GL_REC_TIME=1` (new, [runtime.md](runtime.md)) times each call
where the GL thread submits it, and hwcount counts `ua_glthread` too; gpucnt is never run there.

| a frame | Knulli 32-bit | Knulli 64-bit | muOS 32-bit | muOS 64-bit | G350 32-bit | G350 64-bit |
|---|---|---|---|---|---|---|
| fps (two runs) | 53.5, 52.9 | 51.1, 51.0 (later pair at 696 MHz: 51.5, 51.2) | 52.1, 52.0 | 50.7, 50.6 | 40.0, 39.8 (with the counters and timer: 37.5, 37.2) | 38.5, 37.9 (with them: 37.4, 37.4) |
| GPU clock (average; thermal state of the GPU) | 684, 677 MHz (0.1) | 600, 600 MHz (1.0) | 602, 631 MHz (3.0) | 594, 596 MHz (3.1) | 786 MHz | 786 MHz |
| GPU busy / its cycles a frame | 94.3, 94.1 % / 12.1 M | 99.5 % / 11.7 M | 99.3, 96.4 % / 11.5, 11.7 M | 98.8, 98.1 % / 11.6 M | (not read) | (not read) |
| UnityMain on a CPU | 18.1, 18.2 ms | 19.0, 19.1 ms | 16.9, 17.1 ms | 18.4, 18.8 ms | 23.9, 23.9 ms | 24.8, 24.9 ms |
| render thread on a CPU (G350: + GL thread) | 12.1 ms | 13.5, 13.4 ms | 11.3 ms | 13.2 ms | 8.0 + GL thread 12.2, 12.1 ms (with the timer 18.8) | 8.2 + GL thread 15.1, 15.5 ms (with the timer 17.9) |
| its cycles a frame (user) | 16.3, 16.2 M | 18.4, 18.2 M | 17.7 M | 21.1, 21.2 M | GL thread 16.7, 17.0 M | GL thread 18.4, 18.3 M |
| of them waiting for an instruction fetch (`ic_dep_stall`) | 3.9, 4.0 M | 5.9, 5.8 M | 4.8 M | 7.1, 7.1 M | 7.0 M | 9.5, 9.2 M |
| waiting for a load (`ld_dep_stall`) | 7.0 M | 7.8, 7.7 M | 7.9 M | 9.1, 9.2 M | 5.0, 4.9 M | 5.3, 5.1 M |
| L2 refills | 34.7, 34.3 K | 45.7, 45.3 K | 36.5 K | 47.6 K | 84, 85 K | 99, 96 K |
| instructions | 3.78 M | 3.55 M | 3.63 M | 3.51 M | 2.9 M | 2.3 M |

Time in the GL calls, per call (us; the G350's per call on the GL thread; its 32-bit runs could not read the
generic timer from AArch32 and took `clock_gettime`, about 0.8 us a call more, taken off here):

| call (a frame) | Knulli 32 | Knulli 64 | muOS 32 | muOS 64 | G350 32 | G350 64 |
|---|---|---|---|---|---|---|
| `glDrawElements` (85; G350 87) | 34.0 | 39.6 | 33.5 | 40.1 | 75 | 86 |
| `glBindFramebuffer` (16; 15) | 126 | 152 | 125 | 160 | 326 | 342 |
| `glClear` (5; 3) | 50 | 58 | 48 | 59 | 79 | 91 |
| `glBufferSubData` (18; 20) | 10.2 | 14.6 | 11.4 | 16.0 | 23 | 26 |
| `glTexSubImage2D` (2) | 43 | 54 | 47 | 52 | 70 | 80 |
| `glUseProgram` (57; 60) | 3.0 | 3.4 | 3.0 | 3.6 | 6.7 | 7.1 |
| `glBindTexture`, `glUniform4fv`, `glVertexAttribPointer`, `glActiveTexture` | 1.2, 1.5, 0.5, 0.3 | 1.4, 1.5, 0.5, 0.2 | 1.2, 1.5, 0.5, 0.2 | 1.4, 1.5, 0.5, 0.2 | 2.4, 2.5, 1.8, 0.9 | 2.7, 2.4, 1.8, 1.0 |
| `glInvalidateFramebuffer` (12) | 33 - 51 | 150 - 185 | 274 - 336 | 164 - 210 | 42 | 53 |
| all, ms a frame | 7.3, 7.5 | 10.3, 9.7 | 10.7, 10.1 | 10.7, 10.2 | (17.9, 18.0 with the timer's cost) | 18.1, 17.9 |

What it says:

- **The GPU does the same work on both builds.** 11.5 - 11.7 M GPU-active cycles a frame at 600 MHz on both
  (the counters count the GPU, not a process; at 696 MHz the same frame counts 12.1 M: memory latency in
  cycles); the frame is the same 16 passes and 85 draws, the same 13,000 indices. Nothing in our GL layers
  differs between the builds (the same `[unitypool]`, `[glarena]`, `[glspec]`, `[glopt]` counts).
- **Entering the driver costs the same.** The cheap calls (`glBindTexture`, `glUniform4fv`,
  `glVertexAttribPointer`, `glActiveTexture`: 0.2 - 1.5 us) take the same time on both builds: neither our
  shims, nor libmali's TLS (`R_AARCH64_TLS_TPREL64`, initial-exec: one `mrs tpidr_el0` and a load) nor its
  dispatch cost more on 64-bit. What costs more is the work inside the heavy calls: a draw +17 - 20 %, a
  framebuffer change (the driver closes and submits the pass there) +20 - 28 %, a clear +16 - 23 %, an upload
  +15 - 45 % on the RG34XX-SP; +6 - 15 % on the G350.
- **The render thread's extra time is instruction fetch.** Its cycles grow 2.0 - 2.2 M a frame (Knulli) and
  3.4 M (muOS) while it retires 6 % fewer instructions; the instruction-fetch stall cycles alone grow 1.9 -
  2.0 M (Knulli) and 2.3 M (muOS), L2 refills +11 K. libmali's code a frame: 1,849 sampled 64-byte lines
  (116 KB) on 64-bit against 1,450 (91 KB) on 32-bit (the mu-8 45 s recordings), spread over 201 against 165
  pages; the 64-bit blob has 33 MB of `.text` against 10.9 MB. On the G350 the GL thread waits for instruction
  fetch half its cycles on both builds (7.0 M of 16.7 - 17.0 M; 9.2 - 9.5 M of 18.3 M on 64-bit): the RK3326's
  L2 is 256 KB.
- **`glInvalidateFramebuffer` is where the render thread waits for the GPU** when the GPU is the limit (150 -
  340 us against 33 - 51 us), on both builds (the call the driver blocks in when it is a frame ahead): time
  asleep, not CPU.

**What sets the frame rate, per handheld:**

- *muOS*: the GPU, on both builds (busy 96 - 99 %, its clock held at 552 - 648 MHz by the thermal governor,
  GPU-only cooling: the CPU's state stays 0 - 0.2). The 64-bit build's 1.4 fps are its clock: it reaches the
  scene with the GPU already at state 3 (600 MHz) where the 32-bit build still runs 648 - 816 MHz for the
  first seconds (the same temperatures at the start), and 1 - 2 % of GPU time left idle (the render thread
  later with its commands). Warm, at 600 MHz both, the gap was 0.8 fps (150 s runs, mono-arm64.md).
- *Knulli*: the power allocator (one budget, `sustainable_power` 1000 mW, control at 70 C, both actors weight
  1024) gave the 64-bit runs' budget to the CPU (cpufreq state 0.7 - 0.8) and capped the GPU at 600 MHz
  (state 1.0) in the first pair; the 32-bit runs the other way round (CPU state 1.3 - 1.6, GPU 677 - 684 MHz).
  At 600 MHz the frame's 11.7 M GPU cycles take 19.5 ms (51.3 fps): those 64-bit runs are GPU-bound, and
  UnityMain's 19.0 ms holds the same 51 at 696 MHz: a later 64-bit pair that kept 692 MHz (GPU busy 90 - 91 %)
  ran 51.2 - 51.5 fps with UnityMain at 18.9 - 19.0 ms. On Knulli the 64-bit build has two limits at the same
  51 fps; only both moving gains anything. Over the
  earlier 65 64-bit Knulli runs of the arm64par benches, 23 sat at 600 MHz, against 2 of 19 32-bit runs.
- *G350*: UnityMain (24 - 25 ms of a 25 - 27 ms frame), with three of the four A35s busy (UnityMain, the GL
  thread 12 - 18 ms, FMOD's mixer 11 - 12 ms, the job worker 9 ms, the render thread 9 ms, the Mali back end
  6 ms a frame) and the CPU clock held at 1.22 - 1.43 GHz on average by its power allocator (the GPU stays at
  786 MHz). The clean pair (no counters, no timer; CPU 1.30 - 1.33 GHz on average in both): UnityMain +0.9 ms,
  the GL thread +3.2 ms (12.1 - 12.2 against 15.1 - 15.5 ms): CPU the main thread's cores share, and heat the
  clock pays for. (The counted pairs slow the GL thread, the 32-bit one more, its timer a system call: they
  give the per-call costs, not the gap.)

So on the RG34XX-SP the graphics side loses the 64-bit build its frame rate through heat, not through the
render thread's time: its extra CPU work (UnityMain +1.0 - 1.5 ms, the render thread +1.4 - 1.9 ms) and the
64-bit code's extra DRAM traffic warm the SoC and the thermal governors take it from the GPU's clock. Every
CPU cycle the 64-bit build saves counts twice there.

**The window's load is not the bars pass's cost.** The frame's first pass on the window (two letterbox bars:
Unity's scissored clear quads, `ClearByDrawingQuad`, because the 720x450 viewport is not the 720x480 window)
starts with neither a clear nor an invalidate, so the driver loads last frame's window; it reads 1.25 ms
serialized (`UA_GL_TRACE`, about 0.3 of it any pass's start-up). Invalidating the window's colour, depth and
stencil at its first bind (a measurement, `UA_GL_WININVAL`, not kept: exact only where the frame covers the
window) changed the GPU's cycles a frame by nothing (64-bit, A B A B; Knulli 12.16, 12.22 against 12.19, 12.20 M,
51.5, 51.2 against 51.1, 51.5 fps; muOS 11.63, 11.55 against 11.56, 11.53 M at ~600 MHz, 51.2, 51.4 against
50.8, 51.1 fps): the bars pass costs its two quads' fixed
work and the pass itself, not the load.

**What can be cut from outside the driver.** Nothing on the way into libmali (above). The driver's own
work by call is fixed per draw and per pass (drawbench, passbench above): fewer draws and passes would help
both builds alike (`UA_GL_MERGE` measured slower). The 64-bit-only part is the code's size: libmali's
hot code, relaid out the way route 2 relays libunity, would take most of the render thread's 2 - 2.3 M
extra fetch-stall cycles. libmali is stripped and has FDEs for only 1,949 of its functions, so its function
boundaries would come from its 42,188 BL targets and exported symbols (and a verifier that every pc-relative
field of a moved function stays inside it or reaches a known start); by those, 90 % of the render thread's
libmali samples fall in 281 functions (277 KB, spread over 5.5 MB), 99 % in 515 (398 KB).

**Tried and stopped (2026-11-21): libmali's hot code relaid out.** Built as an experiment (a branch of its
own, not on main): route 2's engine extended to functions without unwind info, their extents from the
driver's call targets and a verifier (no unwind info inside, an end that does not fall through, no ADR into
itself, no literal load from inside), the copies made at start-up before any EGL call. The render thread's
i-fetch stalls are the driver's (an `ic_dep_stall` profile, Knulli 64-bit: 63 % of them in libmali, 24 % in
libunity, 10 % in ua_host). 587 whole functions (401 KB, from the muOS 45 s recording) moved, A B A B, 64-bit,
King's Pass:

| | base | relaid out |
|---|---|---|
| Knulli: fps | 51.1, 50.9 | 51.1, 51.3 |
| render thread cycles / i-fetch stall cycles a frame (same GPU clock, 600 MHz) | 18.32 / 5.95 M | 18.10 / 6.04 M |
| muOS: fps | 50.9, 50.9 | 50.8, 50.7 |
| render thread cycles / i-fetch stall cycles | 20.30, 20.39 / 7.03, 6.77 M | 21.03, 21.09 / 7.16, 7.21 M |
| pixels (static in both shots of both runs) differing | 0.20 - 0.68 % between base runs | 0.05 - 0.44 %: within that |

Whole functions packed together fetch the same lines (as with libunity, where only hot/cold splitting paid),
and every call from code that stays still passes through the original entry. Not taken further, for a second
reason: the Arm Mali user-space driver's licence (the EULA Rockchip distributes with its builds, LES-PRE-20769;
the handhelds' blobs come under Arm's terms too) allows redistribution with the licence but says the software
may not be reverse engineered, decompiled or disassembled except where the law allows. Relaying it out means
decoding its instructions and shipping offsets into it. Routes that touch only our own code, or an open
driver (Mesa's Panfrost), avoid that.

**Another 64-bit blob instead?** Knifethrower's h700-mali-wayland runs Rockchip's g29p1 Wayland blob on the
H700. It needs kbase r54p2 (ROCKNIX's 7.2 kernel), two kbase patches, Panfrost unbound and his AFBC shim (the
H700's G31 has no AFBC). Knulli and muOS run kernel 4.9.170 with mali_kbase r20p0-01rel0 (UK 11.17), so it is
no drop-in: it needs another kernel driver. He measured glmark2 and frame rates against Panfrost, not the
render thread. The one other 64-bit build at hand, the G350's Rockchip `rxp0` GBM blob (40.8 MB), costs as much
more per call against its 32-bit build (draws +15 %), so nothing points to a blob with smaller hot code.

**A flight recorder for the G350's 64-bit hangs.** The G350 has stopped answering (no network, a power button
needed) during 64-bit King's Pass runs more than once (mono-arm64.md; again 2026-11-20); nothing reaches its card after the hang.
`UA_GL_REC_BLACKBOX=<file>` ([runtime.md](runtime.md)) rewrites a 4 KB record in place and syncs it before
each frame's calls go to the driver, before the swap and after it (level 2: before each pass too): the frame, the
stage, the call and the 64 calls before it. Run through the game as always (on the G350 its GL thread is the one
that submits); after a power cycle the file says whether the GL thread was in a pass, in the swap or between
frames (then the hang is elsewhere: the render thread's own calls, a load).

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

#### Route 1, built and measured (2026-11-20)

`UA_FMOD_ASYNC=1` (runtime/host/fmodasync.c, [runtime.md](runtime.md)); off by default: it is exact, but it gives
UnityMain back 0.3 - 0.6 ms of CPU a frame and no frame rate.

**Safety, read from both 6000.0.61f1 players' code and counted on the handhelds (`UA_FMOD_COUNT`):**

- Unity's audio is FMOD Ex 4.44.56 (`SystemI::getVersion` answers 0x44456; Unity's own build, linked into
  libunity.so; 2017.4's players the same version). Its API takes no lock: each of the 135 public functions
  (`System::`, `Channel::`, `ChannelGroup::`, `Sound::`, `DSP::`, `DSPConnection::`, `Reverb::`) calls its handle's
  validate (7 functions) and then the implementation. `FMOD_INIT_THREAD_UNSAFE` is FMOD Studio's (1.x and later)
  and does not exist here; Unity calls `System::init` with flags 0 (`FMOD_INIT_NORMAL`: no
  `STREAM_FROM_UPDATE`, the mixer and stream threads FMOD's own). So the only safety is the order of calls, and
  the hook keeps it: every call into FMOD's code from outside it in both libunity.so files was listed from their
  symbol files (calls and tail calls): the 135 public functions, FMOD's start-up, and the Java AudioTrack output's
  mix entry (FMOD's own mixing work, on its own thread); AudioManager::Update holds no lock around the update and
  drops its result.
- What the update reads that the frame sets: the listener and the channels' 3D attributes, volumes and pitches
  that AudioManager::Update and the scripts set before it (unchanged: the handover comes at the same point), and
  the mixer's DSP clock (FMOD's real-time thread: its position at the update already varies from run to run).
  Its callbacks into Unity: `SoundChannelInstance::FMODChannelCallback` (a finished channel's Unity object
  released; the only lock it can reach is the log's, on an FMOD error), `AudioSource::rolloffCallback` (returns
  1.0), `AudioManager::systemCallback` (device-list changes: `FMOD_OS_CheckDriverList` is a stub on Android).
- Between the update's return and the frame's end, Hollow Knight at King's Pass (muOS and Knulli, 32 and 64-bit,
  and the G350): UnityMain enters FMOD in no gameplay frame (only while loading: `Sound::getFormat`, `lock`,
  `getLength`, ... of new clips), Unity's loading threads (`Loading.Preload`, `Loading.AsyncRead`) call
  `System::createSound` while scenes load, FMOD's mixer calls `DSP::getUserData` about 20 times a frame (Unity's
  DSP callbacks; it never waits: it may hold FMOD's locks), the update's own callbacks call FMOD 0.1 times a
  frame (channel ends), and UnityMain enters managed code 12 - 51 times a frame. The first managed call comes
  0.5 - 0.8 ms after the update (Unity's `RenderPipelineManager.HandleRenderPipelineChange`, every frame), then
  `Camera.FireOnPreCull`, or `ScriptableRuntimeReflectionSystemWrapper.TickRealtimeProbes` on some frames; the
  first game script (`DarknessCameraEffect.OnPreRender`) about 1.0 ms after it on the 64-bit build. The three
  Unity methods are let through only when their IL is the code read and the statics they test say they change
  nothing.
- The update itself: 0.18 - 0.42 ms inline on Knulli's 32-bit build, 0.49 - 0.52 ms on muOS's, 0.67 - 0.72 ms on
  the 64-bit build (both firmwares), 0.50 - 0.65 ms on the G350. On the worker it takes longer (0.6 ms on the
  32-bit build, 0.7 - 1.0 ms on the 64-bit one: its data was last written on UnityMain's core) and starts 0.12 -
  0.25 ms after the handover (a futex wake; no spinning). So UnityMain still waits at the first script on most
  frames (0.3 - 0.5 ms).

**A/B, King's Pass standing, 25 s, cool starts, alternated (o = off, a = `UA_FMOD_ASYNC=1`); UnityMain ms is its
CPU time a frame (/proc), the counters `hwcount` on UnityMain relative to the off runs:**

| | fps off | fps async | UnityMain ms off | async | cycles | L2 refills | load-miss stall cycles |
|---|---|---|---|---|---|---|---|
| Knulli 32-bit | 53.5, 53.2 | 52.4, 53.7 | 16.16, 15.98 | 15.40, 15.89 | -2 to -6 % | -3 to -5 % | -4 to -10 % |
| Knulli 64-bit | 51.0 | 51.2, 51.3 | 16.53 | 16.19, 16.19 | -2.5 % | -3 to -4 % | -5 % |
| muOS 32-bit | 53.2, 52.0 | 52.1, 52.1 | 16.08, 15.78 | 15.75, 14.82 | -0 to -4 % | -0 to -4 % | -2 to -7 % |
| G350 32-bit | 39.1, 39.5 | 39.4, 39.6 | (on a CPU) 24.42, 24.29 | 24.03, 24.16 | -1.5 % | -2.5 % | -2.5 % |

The frame rate follows the GPU clock here (muOS: the 53.2 fps run had 635 MHz on average, the three others 610;
King's Pass is held by the GPU, mono-arm64.md), so what moved is UnityMain's own time: 0.3 - 0.6 ms of CPU a
frame and 2 - 5 % of its L2 refills and load-miss stalls. The worker spends 0.6 - 0.7 ms for it: more CPU in all
than the update took inline, on a handheld whose four cores are 65 - 70 % busy. (The first 32-bit Knulli async
run let no managed call through; the others let `HandleRenderPipelineChange`, and the muOS ones the camera's
too, through.)

**Exactness.** Every FMOD call, managed call and the frame's end on UnityMain, and every FMOD call from Unity's
other threads, comes after the update has ended, as without it; the update runs between the same calls. The
sound cannot be compared sample for sample between two runs (the game's timing differs from run to run, with or
without it): captured output (`ANDROID_SLES_CAPTURE`) of the scripted walk, jump and attacks in King's Pass on
Knulli's 32-bit build: two runs without it correlate 0.65 - 0.76 (10 ms envelopes, aligned), with it 0.53 - 0.81
against them, levels within 0.05 dB: no difference beyond run-to-run.

**Cuphead (Unity 2017.4).** The rows are there (2017.4.9f1 armeabi-v7a, the 32-bit port's player; 2017.4.40f1
armeabi-v7a and arm64-v8a), but 2017.4's AudioManager::Update calls managed code right after the update
(`AudioSettings.InvokeOnAudioManagerUpdate`, every frame): the join comes at once, so nothing is gained there
(exact, not run on the handheld).

**What would make it pay:** the update would need to run where nothing waits for it soon: the game's first camera
script comes about 1 ms after it in Hollow Knight, as long as the update on another core takes with its wake-up.
Handing it over earlier is not exact (the frame's audio settings come right before it). The route's expected
-0.5 to -0.7 ms of UnityMain is reached in CPU time but not in frame time; the 64-bit build's gap stays for
routes 2 and 3.

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

### Route 2 built: load-time relayout (`UA_RELAYOUT`)

**The tool** (2026-11-20; runtime.md `UA_RELAYOUT`, porting.md for making a list). Built as planned
above, with what the offline pass found deciding the details:

- **What can move.** `relayprof check` on 6000.0.61f1's arm64 libunity: 76,082 functions with FDEs,
  all 18.96 MB of them movable mechanically: no literal loads at all, no data among the code, jump
  tables are byte/halfword offsets from an ADR inside the function (so they move with it), every
  `.eh_frame` pointer pc-relative. The loader still checks each function it is given (rl_check) and
  leaves one it cannot prove (a literal pool inside, a SIMD literal from outside, a function a hook
  changed: compared with the file's bytes).
- **The list.** UnityMain and the render thread from three recordings on the muOS RG34XX-SP (time
  samples at King's Pass and Greenpath, `ic_dep_stall` samples at King's Pass), the functions samples
  fell in plus those on the sampled stacks: 2,804 functions, 830 KB, in 1,266 call-chain clusters
  (C3, up to a page each), densest first. The loader builds 836 KB of copies (islands: 627 words used
  of 2.5 KB reserved), 112 KB of `.eh_frame` and 22 KB of `.eh_frame_hdr`; 4 stay (changed by
  `unityhooks`: `UA_GLES_POOL`, `UA_GFX_JOBS`). Load cost: a few ms.
- **Calls.** Inside the copies BL/B go copy to copy. Each original entry is a `b` to its copy. 295 -
  375 of the hot functions (22 % of the profile's weight) are reached through vtables and function
  tables (R_AARCH64_RELATIVE); only 4 have their address formed in code and none is exported, so
  `UA_RELAYOUT_DATA=1` points those data words (5,551 of them) at the copies and leaves the 178
  functions whose address code forms or exports alone.
- **Unwinding.** The copies' FDEs (with their CIEs, personality and LSDA pointers re-encoded) and a
  search table are reported through the loader's `dl_iterate_phdr` as an object of their own: the
  unwinder linked into libunity (and any other) finds them as it finds libunity's own, so exceptions
  and backtraces pass through copies; nothing had to be excluded for throwing.
- **Exactness.** Only branch and address fields change; a copied function computes what the original
  did (same instructions, same order, same addresses formed). `test_relayout` (runtime/loader/test)
  runs a synthetic library before and after: B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ to copied and unmoved
  functions out of reach (islands), ADR and literal loads (W, X, SW, unaligned X, PRFM) out of reach,
  ADRP + ADD/LDR, a jump table, a loop through the function's own entry, a C forced unwind through
  copied frames with cleanups (personality, LSDA, landing pads, `_Unwind_Resume`) and a backtrace, a
  vtable redirected and one kept, a hooked function and a data-in-code function left in place, the
  outline-atomic helpers patched in the copies: every result the same. On the handhelds Hollow Knight
  runs its scenes with it (King's Pass, Greenpath, City of Tears) and the profiler names the copies as
  the originals.

**Measured** (64-bit Hollow Knight, the arm64par copies, `HWCOUNT=1`, interleaved runs from a cool
handheld; UnityMain's counters a frame; `r` = `UA_RELAYOUT`, `d` = with `UA_RELAYOUT_DATA=1`):

Knulli RG34XX-SP, City of Tears (a scene the list was not made from; there the render thread and
the GPU set the frame rate, 45 fps on every run, so UnityMain's time is what to read):

| City of Tears, Knulli (2 runs each) | base | r | d |
|---|---|---|---|
| UnityMain ms a frame | 14.95, 14.69 | 15.10, 14.86 | 14.23, 14.87 |
| render thread ms a frame | 15.42, 14.94 | 15.46, 15.17 | 14.81, 15.52 |
| instructions | 3.68 M | 3.78 M (+2.6 %) | 3.72 M (+0.9 %) |
| i-cache miss stall cycles (0xE1) | 5.36 M | 5.50 M (+2.6 %) | 5.17 M (-3.5 %) |
| L1I refills | 112 K | 117 K (+4 %) | 111 K (-0.5 %) |
| iTLB miss stall cycles (0xE2) | 0.62 M | 0.56 M (-9 %) | 0.50 M (-19 %) |
| L1I micro-TLB refills | 8.3 K | 7.2 K (-13 %) | 6.6 K (-21 %) |
| L2 refills | 80.9 K | 79.5 K (-1.8 %) | 78.5 K (-3.0 %) |
| load-miss stall cycles (0xE7) | 13.05 M | 13.34 M | 13.11 M |

King's Pass (the profiled scene; UnityMain sets the frame rate there):

| King's Pass, muOS (2 runs each) | base | r | d |
|---|---|---|---|
| fps | 50.6, 50.6 | 50.7, 50.5 | 51.3, 51.2 |
| UnityMain ms a frame | 17.00, 17.20 | 17.13, 16.86 | 16.65, 16.36 (-0.60) |
| render thread ms a frame | 11.86, 11.83 | 11.83, 11.70 | 11.73, 11.30 |
| cycles (user) | 31.31 M | 31.90 M | 30.80 M (-1.6 %) |
| instructions | 4.55 M | 4.64 M | 4.50 M |
| i-cache miss stall cycles | 6.81 M | 7.07 M (+3.7 %) | 6.60 M (-3.2 %) |
| L1I refills | 118.8 K | 123.9 K | 115.4 K (-2.8 %) |
| iTLB miss stall cycles | 0.78 M | 0.66 M | 0.60 M (-23 %) |
| L2 refills | 88.1 K | 87.2 K | 85.9 K (-2.5 %) |

| King's Pass, Knulli (2 runs each) | base | d | `UA_ATOMICS=llsc` alone |
|---|---|---|---|
| fps | 50.7, 50.7 | 51.4, 51.3 | 51.1, 51.2 |
| UnityMain ms a frame | 16.89, 16.92 | 16.67, 16.73 (-0.21) | 16.51, 16.63 (-0.34) |
| cycles (user) | 27.32 M | 26.96 M (-1.3 %) | 27.33 M |
| instructions | 4.48 M | 4.48 M | 4.49 M |
| i-cache miss stall cycles | 5.96 M | 5.65 M (-5.2 %) | 5.79 M (-3.0 %) |
| L1I refills | 119.5 K | 118.6 K | 120.0 K |
| iTLB miss stall cycles | 0.68 M | 0.53 M (-21 %) | 0.68 M |
| L2 refills | 86.5 K | 84.3 K (-2.5 %) | 86.7 K |
| load-miss stall cycles | 15.51 M | 15.31 M | 15.42 M |

Then the hot/cold split (`s`: the list made with `--split`, 584 of its 2,804 functions in pieces, 557 KB
of hot code first and 273 KB of cold after it; with `UA_RELAYOUT_DATA=1` and `UA_ATOMICS=llsc`), against
whole functions (`x`: the same plus `UA_ATOMICS=llsc`) and the atomics alone (`a`):

| King's Pass (2 runs each) | muOS base | muOS a | muOS s | Knulli base | Knulli x | Knulli s |
|---|---|---|---|---|---|---|
| fps | 50.3, 50.6 | 50.9, 50.6 | 50.9, 50.9 | 51.1, 50.7 | 51.6, 51.3 | 51.6, 51.1 |
| cycles (user) a frame | 31.79 M | 31.40 M (-1.2 %) | 30.77 M (-3.2 %) | 27.50 M | 26.99 M (-1.9 %) | 26.89 M (-2.2 %) |
| i-cache miss stall cycles | 6.99 M | 6.88 M | 6.58 M (-5.9 %) | 5.99 M | 5.78 M (-3.5 %) | 5.76 M (-3.8 %) |
| L1I refills | 122.0 K | 117.9 K | 114.1 K (-6.5 %) | 120.9 K | 118.3 K | 115.0 K (-4.9 %) |
| L2 refills | 88.7 K | 87.5 K | 86.3 K (-2.8 %) | 86.7 K | 84.5 K (-2.6 %) | 84.7 K (-2.3 %) |
| iTLB miss stall cycles | 0.78 M | 0.80 M | 0.61 M (-22 %) | 0.70 M | 0.54 M (-23 %) | 0.55 M (-21 %) |
| load-miss stall cycles | 18.88 M | 18.51 M | 18.37 M | 15.45 M | 15.31 M | 15.28 M |

City of Tears again (Knulli, not in the recordings; render-bound at 45 fps, so the counters say it),
with only the entries unmoved code calls patched (1,578 of 2,800):

| City of Tears, Knulli (2 runs each) | base | x (whole + vtables + atomics) | s (split + vtables + atomics) |
|---|---|---|---|
| cycles (user) a frame | 23.89 M | 23.26 M (-2.7 %) | 23.43 M (-1.9 %) |
| i-cache miss stall cycles | 5.40 M | 5.14 M (-4.9 %) | 5.17 M (-4.2 %) |
| L1I refills | 111.6 K | 107.2 K (-3.9 %) | 110.4 K (-1.1 %) |
| L2 refills | 80.3 K | 78.5 K (-2.3 %) | 79.1 K (-1.5 %) |
| iTLB miss stall cycles | 0.63 M | 0.50 M (-21 %) | 0.50 M (-20 %) |

The whole-function layout carries to a scene the list was not made from; the split's hot ranges,
sampled at King's Pass and Greenpath, carry less (code a scene runs that the recordings did not sample
lies in the cold part): a game's list wants recordings of more of its scenes before `--split` pays
everywhere.

(UnityMain's CPU time a frame from the thread's schedstat moves within the runs' spread, +-0.4 ms; the
cycle counters, which count only UnityMain's user time, are the steadier measure. The muOS base runs of
this hold, 31.8 M cycles, match the earlier hold's 31.3 M within the spread.)

What it says:

- **Whole functions packed, original entries in the way: nothing.** Every virtual call into a moved
  function still went through the original entry's line and a taken branch (`r`: +2.6 - 3.7 % i-cache
  stall cycles); the packing gained only pages (iTLB stalls -9 to -13 %).
- **With the vtables pointed at the copies (`d`): -1.3 to -1.6 % of UnityMain's cycles**, i-cache stalls
  -3 to -5 %, L2 refills -2.5 %, iTLB stall cycles -21 to -23 %; muOS -0.60 ms of UnityMain's CPU time a
  frame, +0.6 fps on both handhelds at King's Pass.
- **The atomics alone (`a`): -0.6 to -1.2 % of the cycles**, +0.3 - 0.5 fps: each of the frame's
  atomic operations skips a load, a branch and the call's indirect cost; the helpers' LL/SC loops are what
  ran before.
- **Hot/cold split + vtables + atomics (`s`): -2.2 to -3.2 % of UnityMain's cycles** (muOS -1.0 M a
  frame, 0.57 ms at 1.8 GHz), L1I refills -5 to -6.5 %: the first route-2 change that fetches fewer
  lines. The 32 -> 64-bit gap at King's Pass is +3.1 M cycles (muOS) / +1.5 M (Knulli): this closes about
  a third of it on muOS and half on Knulli.
- **What is left on the table.** The hot part is still 557 KB, larger than the L2: the code a frame
  touches is spread over 1,800 functions and the cold half of each hot line stays. The data side (load
  stalls, three quarters of the gap) is barely touched (-1 to -3 %): it needs route 3's prefetches.

Exactness on the handhelds: every run reached and measured its scene (the game logs and the frame
counts as without), with no crash in the 21 laid-out runs (and 4 with the atomics alone) across both firmwares (the profiler's naming
of copies is covered by test_relayout, not yet by a device recording). G350: its runs (base and laid out
alike) stopped in the measurement on the runner's memory floor (106 - 109 MB available, `UA_MEM_MIN=110`),
and the handheld was unreachable over SSH later; not measured.

Memory: the region (836 KB whole, 866 KB split) plus its unwind data (224 - 237 KB), and each library
page that gets an entry branch made private (at most 864 pages, 3.4 MB, when every entry is patched;
`UA_RELAYOUT_DATA` patches only entries that code left in place calls).

**Against the 32-bit build** (King's Pass, interleaved in the same holds, 2 runs each; `s` = split list,
vtables, atomics; the 64-bit base with `UA_RELAYOUT=0 UA_ATOMICS=0`):

| King's Pass | muOS 32-bit | muOS 64 base | muOS 64 s | Knulli 32-bit | Knulli 64 base | Knulli 64 s |
|---|---|---|---|---|---|---|
| fps | 52.1, 52.6 | 50.2, 50.2 | 51.0, 51.0 | 53.0, 53.0 | 50.8, 51.2 | 50.5, 51.5 |
| UnityMain cycles a frame | 28.02 M | 31.80 M | 30.91 M (-2.8 %) | 26.01 M | 27.35 M | 27.36 M (0) |
| i-cache stall cycles | 5.62 M | 6.94 M | 6.65 M | 5.22 M | 5.69 M | 6.08 M |
| L1I refills | 125.5 K | 120.0 K | 113.6 K | 125.0 K | 120.2 K | 117.2 K |
| L2 refills | 72.1 K | 88.2 K | 85.8 K | 73.5 K | 86.7 K | 85.6 K |
| load-miss stall cycles | 15.12 M | 18.56 M | 17.92 M | 13.25 M | 15.57 M | 15.46 M |

On muOS the 64-bit build's deficit at King's Pass goes from 3.8 M to 2.9 M cycles a frame and 2.2 to
1.4 fps. On Knulli this hold showed no change (one of its two `s` runs took more i-cache stall cycles
than any base run; the three earlier Knulli holds gave -1.3 to -2.2 %): the effect is 0 - 3 % of
UnityMain, about the size of run-to-run spread, and the data side (L2 refills +12 - 16 K, load stalls
+2.2 - 2.8 M over the 32-bit build) is untouched. The G350 (Cortex-A35, dArkOS): laid-out runs reach
King's Pass and play as without (723 GL calls a frame, as the base runs), but its frame rate there moved
30.9 / 43.4 fps between two base runs of the same build (the GPU clock throttles, 528 - 786 MHz):
no relayout effect can be read from it yet, and its 32-bit runs need a check that they measure the
same spot.

**Defaults** (2026-11-20): `UA_ATOMICS=llsc` on for the 64-bit player (exact, no cost, a CPU with LSE is
left alone); `UA_RELAYOUT` on where a list for the library's build id is in `relayout/` beside ua_host
(runtime/relayout/: Hollow Knight's 6000.0.61f1 IL2CPP arm64 player, whole functions: they carry to scenes the recordings missed), with `UA_RELAYOUT_DATA` on;
`UA_RELAYOUT=0` / `UA_ATOMICS=0` turn them off. A port ships the lists by copying `relayout/` next to its
ua_host (runtime/host/build.sh puts it beside the build's ua_host).

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

#### Route 3, built: prefetch stubs at the loops' call sites (`UA_PREFETCH`, 2026-11-20)

`runtime/host/unityprefetch.c` (opt-in, `UA_PREFETCH=1` or a comma list of `beh,sprite,cull,play`; 64-bit
player only). No function is rewritten: in each hot loop, the one `bl` made once per element is pointed at a
small stub in a page within branch range of libunity. The stub reads where the loop is from the loop's own
registers at that call, issues `PRFM` for what the next elements will read, and jumps on to the original callee
with x0 - x7, x19 - x28, the stack and the float registers as they were (scratch: x16/x17, which any call may
lose to a veneer, and x9 - x12, which each callee's code - tied to its bytes by its row - writes before it reads;
the flags). Its loads are only of memory Unity's loop reads next anyway (the list nodes still to run, the index
list below its end, fields of a MonoBehaviour once its vtable says it is one), each asked for by a PRFM a round
earlier; everything else is PRFM, which changes no register, flag or memory and never faults. Unity's own
loads, stores and calls are the same and in the same order: the picture and the play cannot change.
`test_unityprefetch` runs every stub over models of Unity's structures placed against guard pages (a load past
what Unity reads faults) and checks that the callee gets every register back and that nothing is written.

| site | loop (6000.0.61f1 arm64-v8a) | fetched ahead |
|---|---|---|
| `beh` | `CommonUpdate<Behaviour / LateBehaviour / FixedBehaviourManager>` at its call of `SafeIterator::Next` | 4 behaviours deep: the managed object and its cached-pointer line (through the GC handle's slot), the handle slot, GameObject's active byte, script cache and its control block's weak count (for writing), the MonoBehaviour's lines `CallUpdateMethod` reads, the next node |
| `sprite` | `PrepareSpriteRenderNodes<false / true>` at its call of `FlattenBasicData` | the next renderer's 0x1c0 bytes, the next RenderNode (for writing), the entry after |
| `cull` | `ProcessCameraIndexListIsNodeVisibleStep1` at its call of `IsNodeVisibleFast` | SceneNodes 8 - 10 ahead |
| `play` | `PreOrderPlayableTraverser::Traverse`'s input loop at its call of `RecursiveVisit` | the next input's Playable |

Hollow Knight, King's Pass (the same installs and settings as above, `lib-gG8`), A B A B in one hold from a
cool handheld, `hwcount` on UnityMain, a frame's worth (UnityMain ms: its CPU time a frame):

| | fps | UnityMain ms | cycles | load-miss stall cycles | L2 refills | i-cache stall cycles |
|---|---|---|---|---|---|---|
| Knulli, off (2 runs) | 50.9, 50.9 | 16.46, 16.63 | 27.34, 27.33 M | 15.48, 15.55 M | 87.3, 87.6 K | 5.77, 5.79 M |
| Knulli, `UA_PREFETCH=1` | 51.7, 52.9 | 16.05, 16.02 | 25.65, 26.33 M | 13.47, 14.11 M | 85.8, 86.7 K | 5.96, 6.24 M |
| difference (means) | +1.4 (+2.8 %) | -0.51 | -1.35 M (-4.9 %) | -1.73 M (-11 %) | -1.2 K (-1.3 %) | +0.32 M |
| muOS, off (2 runs) | 51.0, 50.8 | 16.58, 16.71 | 31.49, 31.76 M | 18.75, 18.37 M | 87.4, 88.4 K | 6.86, 6.95 M |
| muOS, `UA_PREFETCH=1` | 51.7, 51.4 | 16.15, 16.20 | 29.33, 29.56 M | 16.08, 16.50 M | 87.9, 87.3 K | 6.88, 6.99 M |
| difference (means) | +0.6 (+1.2 %) | -0.47 | -2.18 M (-6.9 %) | -2.27 M (-12 %) | -0.3 K | +0.03 M |
| G350 (Cortex-A35; relayout off, UA_ATOMICS on; the playerprefs restored before each run), off | 37.9, 37.7 | | 29.80, 29.09 M | | 146.2, 145.6 K | |
| G350, `UA_PREFETCH=1` | 38.5, 38.5 | | 28.41, 27.10 M | | 148.8, 147.8 K | |
| difference (means) | +0.7 (its GPU clock throttles 528 - 786 MHz: the cycles count, not the fps) | | -1.69 M (-5.8 %) | | +2.4 K | |

With the code layout on (`UA_RELAYOUT`, HK's whole-function list, `UA_RELAYOUT_DATA`, `UA_ATOMICS=llsc`, the
aarch64 defaults now), Knulli, A B A B: off 51.5, 51.2 fps, UnityMain 16.53, 16.44 ms, cycles
27.07, 27.26 M, load-miss stalls 15.33, 15.50 M, L2 refills 84.6, 84.5 K; `UA_PREFETCH=1` 52.1, 52.8 fps, 15.36,
15.91 ms, 25.06, 25.68 M, 13.22, 13.99 M, 82.0, 84.5 K: +1.1 fps (+2.1 %), UnityMain -0.86 ms, cycles -1.79 M
(-6.6 %), load-miss stalls -1.82 M (-12 %). muOS, the same: off 50.9, 51.0 fps, 16.93, 16.79 ms, 31.04, 30.94 M,
18.49, 18.19 M, 84.8, 86.4 K; on 48.4 (a 0.7 s hitch in the measurement, lowest second 11.8 fps), 52.0 fps,
15.83, 15.83 ms, 28.55, 28.91 M, 16.24, 16.21 M, 83.9, 83.8 K: UnityMain -1.03 ms, cycles -2.26 M (-7.3 %),
load-miss stalls -2.12 M (-11.5 %), L2 refills -1.7 K. The relayout then leaves 10 functions in place instead of 4 (the six
whose call sites changed); the two gains add.

One site at a time (Knulli, one run each, the same hold): off 27.44 M cycles / 15.48 M load-miss stalls; `beh`
alone 26.68 / 14.50; `sprite,cull,play` 27.39 / 15.12; all 25.68 / 13.59. The behaviour loops carry most of it.

(A first build missed `CommonUpdate<BehaviourManager>`'s call by one word - the Update loop, the largest -
and still took -0.78 M (Knulli) / -1.27 M (muOS) load-stall cycles a frame, +0.8 / +0.6 fps.)

What it shows: the prefetches hide misses rather than remove them (L2 refills fall only 1 %: the lines still
come from DRAM, ahead of their use), and they pay where the loop has an element of work to overlap with. Load-
miss stalls fall by 11 - 13 % of UnityMain's, about as much as the whole 64-bit penalty of these loops; the
i-cache stalls grow a little (the stubs' code, the functions they leave in place under UA_RELAYOUT). The rest of
the gap is the misses spread over the engine with no loop to put a prefetch in.

Exactness: by construction (PRFM is a hint; the stubs' loads read what Unity reads; registers, stack, flags as
the ABI lets a call find them), the unit test, and the runs above reaching King's Pass with the same play.
With `UA_RELAYOUT`: a function whose call site is changed differs from the
library file, so the relayout leaves it in place and its moved callers reach it there; the two work together
and the patched functions lose only their own move. Should become the default for 6000.0.61f1's 64-bit player
(port.env) once a busier scene (City of Tears, a boss) and a soak have been run with it; other Unity versions
need their own rows (tools/re/unityhooks.py prints them; the loops' registers read by hand).

### 4. Less L2 traffic from the other threads

What: the render thread's own L2 refills grew 35 -> 47 K a frame and its i-cache stalls 4.7 -> 7.0 M cycles
(the 64-bit libmali's 25 MB of code against 12 MB), in the same L2 UnityMain misses in. ua_host's GL layers
(1.05 ms of the render thread on both builds) are ours: fewer distinct code paths and smaller tables there,
fewer GL calls (each into libmali's code), and the job worker's and FMOD mixer's buffers sized to the game.

Measured since ([the graphics side, counted](#the-graphics-side-counted-render-thread-mali-driver-and-gpu-2026-11-20)):
the render thread's extra 2 - 3.4 M cycles a frame are libmali's instruction fetch; on the RG34XX-SP they matter
through heat (the GPU clock), not through the render thread's own time.

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
| 1. FMOD update on a worker | built (`UA_FMOD_ASYNC=1`, off): UnityMain CPU -0.3 to -0.6 ms, frame rate the same | done | yes | - |
| 2. libunity hot-code relayout | built (`UA_RELAYOUT`): -2.2 to -3.2 % of UnityMain's cycles split + vtables + atomics, +0.4 - 0.6 fps | done | yes (tested) | larger lists per game; route 3 for the data side |
| 3. rewritten hot functions (prefetch, inline atomics) | -0.2 to -0.4 ms; built as `UA_PREFETCH` (call-site stubs): UnityMain -0.47 / -0.51 ms, cycles -6.9 / -4.9 % (muOS / Knulli) | medium, per Unity version | yes | more loops; default after a busier scene and a soak |
| 4. less render-thread L2 traffic | 0.1 - 0.3 ms (to be measured) | small to measure | yes | `hwcount` on UnityMain with render-thread work removed |
| 5. Unity bucket allocator settings | 0 - 0.2 ms | small | yes | three boot.config variants, `hwcount` |
| 6. Mono LLVM AOT (elsewhere) | about -0.8 ms | in progress | as measured there | - |
| 7. 32-bit game + 64-bit GPU process | 32-bit speed, still armhf | large | yes | only if a firmware lacks a 32-bit userland |
| 8. armv7 -> A64 translator | worse than today | very large | hard | none |

Routes 1 + 2 + 6 together reach the 1.5 - 1.9 ms the 64-bit build needs; 3 - 5 are margin. Route 1 is the
first prototype: one hook, measurable in a day, and it pays on both builds and every Unity game that uses
Unity's built-in audio.
