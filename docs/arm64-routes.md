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

**Mesa's Panfrost on the handhelds' own kernel driver (2026-11-21, built, not yet on a GPU).** An open driver
can be measured and changed where the blob cannot. [tools/mesa-kbase](../tools/mesa-kbase/README.md): Gallium
Panfrost with a `pan_kmod` backend for kbase (JimVulkan's design on bmdhacks' kbase library, UK 11.7 and 11.17),
no AFBC, sync files optional, the window a CPU-cached pbuffer the host copies to the framebuffer
(`UA_GL_DRIVER=mesa`, off by default, [runtime.md](runtime.md)). Built for aarch64 and checked against a fake
`/dev/mali0`. First light on muOS the same day; then parked: about twice the blob's driver CPU on the render thread and 15 - 20 % more GPU work, with no fix within days (the numbers: the README).

## Replacing libmali

64-bit libmali (33 MB of code, against 10.9 MB on 32-bit) costs the GL thread 15-25 % more per heavy call,
almost all of it instruction-cache stalls: +3.2 ms a frame on the G350 (above). An open GLES driver that we
build ourselves could remove that. Assessed 2026-11-20; Step 0 (CPU cost, no GPU) measured 2026-11-21; Step 1
(the kbase backend, gl64) above.

**What the kernels speak.** All our handhelds run Arm's kbase, job-manager flavour, with no Panfrost DRM driver:

| Device | Kernel | kbase | UK | Display |
|---|---|---|---|---|
| RG34XX-SP muOS / Knulli (H700) | 4.9.170 | r20p0 | 11.17 | fbdev only (no /dev/dri) |
| G350 / R36S dArkOS (RK3326) | 4.4.189 | r12p0 | 11.7 | rockchip DRM, no sync_file |

Both GPUs report product 0x7093 (Mali-G31, Bifrost v7). To read the version: `cat /sys/module/mali_kbase/version`.

**Open drivers on kbase.**
- Mesa Panfrost supports G31 at GLES 3.1 (libmali: 3.2), but only on the DRM drivers. Upstream Mesa has no
  kbase backend.
- panfork (Icecream95) was Panfrost Gallium on kbase. It is now abandoned and needs UK >= 11.20.
- malisx2 (bmdhacks, MIT, Vulkan, ARMSX2) has a kbase layer for job-manager UK 11.7 to 11.36 (Knifethrower,
  PR #60) and an fbdev present path (PR #61). It has run on H700 and RK3326 handhelds.
- JimVulkan's mesa-unified-drivers (MIT) has Gallium Panfrost on job-manager kbase (UK 11.31), the design
  Step 1 takes.

**Chosen route: Panfrost Gallium (GLES) + a kbase `pan_kmod` backend over malisx2's `src/kbase`** (Step 1:
tools/mesa-kbase, above). Ruled out: Unity's Vulkan player on malisx2 (no Vulkan programs in our data; reversed Z
and other variants change the picture; no MSAA or secondary command buffers on v7); Zink (CPU cost; no transform
feedback).

### Step 0: Mesa's CPU cost per GL call against libmali's, no GPU (2026-11-21)

**The tools.** `UA_GL_REC_FILE=<file>` ([runtime.md](runtime.md)) writes every GL call the game makes to a trace;
`glreplay` (runtime/host/glreplay.c) replays it against any EGL/GLES driver and times each call. The trace: Hollow
Knight 64-bit (arm64par/hk64, the release's 1 GB settings, relayout on), launch to King's Pass standing, recorded on
each handheld (Knulli 3,457 frames, 3.0 M calls, 61 MB; muOS 3,735 frames); the last 300 frames (King's Pass, 85
draws and 16 passes a frame, as in the game) measured, `glFinish` untimed after each frame so no call waits for the
GPU. libmali-64 (/usr/lib/libmali.so) and libmali-32 (/usr/lib32, the armhf glreplay) replay it on the GPU with zero
GL errors; Mesa replays it on the CPU only, under Mesa's `libpanfrost_noop_drm_shim.so` (a fake
`/dev/dri/renderD128`: no job ever reaches the GPU, `/dev/mali0` never opened, a guard kills the run if it is).

**The Mesa build** (/Volumes/Unity3/work/mesa0/build-mesa.sh, not in the repo): bmdhacks/armsx2-turnip `malisx2`
@ b5f8787 (Mesa 27.0.0-devel, with Knifethrower's G31 fixes #2, #3, #5, #8, #9 already in) plus Knifethrower's
#6 pan-fb-pack-rounding, #7 bifrost-int64-atomics-cas and #4 nir-robust-image-noformat (each applied cleanly).
`-Dgallium-drivers=panfrost -Dvulkan-drivers= -Dllvm=disabled -Dtools=drm-shim -Dpanfrost-rust=false`, EGL
surfaceless, GLES only, `-O2` release. Panfrost's precompiled shaders need `mesa_clc` (LLVM 18, libclc,
SPIRV-LLVM-Translator) at build time, so the build runs in Ubuntu 24.04 (glibc 2.39: muOS has 2.44, Knulli 2.40);
the driver itself has no LLVM. Mesa refuses LTO (`src/meson.build`). Three drm-shim changes for the measurement:
exported sync files get a real file descriptor (else every Panfrost fence export fails), `PAN_SHADER_PRESENT`
sets the core count (1, as kbase reports), and its wrappers look up libc once (below). libgallium's `.text` is 9.7 MB (libmali-64: 33 MB, -32: 10.9 MB).

**GLES 3.2.** Unity compiles its shaders as `#version 320 es` against libmali (3.2). Panfrost stops at 3.1 and
refuses them; `MESA_GLES_VERSION_OVERRIDE=3.2 MESA_GLSL_VERSION_OVERRIDE=320` makes every shader of the trace
compile and link (23 GL errors left, all at load: one cube-map-array texture, a 3.2 feature). A shipped Mesa
needs either a real 3.2 or Unity steered to 3.1.

**The caches.** A replayer alone runs little code between GL calls, so the driver's code stays in the caches;
the game runs Unity's render-thread code between them. `--evict <KB>` runs that much code and reads that much data
between calls (untimed). At 16 KB libmali's calls cost what they cost in the game (draw 48 / 42 us on 64 / 32-bit
against 40 / 34 in the game, framebuffer bind 139 / 117 against 152 / 126) and 64-bit libmali costs +12 % over
32-bit (the game: +17 - 28 % on its heavy calls, so the game's caches are at least that cold); without eviction
64-bit libmali is within 2 % of 32-bit.

**A drm-shim trap.** drm-shim's `ioctl()` (and its other wrappers) looked up its ~23 real libc functions with
`dlsym()` on every call; Panfrost makes several ioctls per buffer (create, mmap, madvise) for its pools and for
each of Unity's 140 maps a frame, so the first Mesa runs measured the shim's lookups as Mesa's time (draw 121 us,
+75 % a frame). A gdb sampling of the replay found it; the shim now looks them up once (the change is in
/Volumes/Unity3/work/mesa0/patches/mesa0-drm-shim.patch). The numbers below are with that fix.

Time in the GL calls on the calling thread, ms a frame (each run after a cool; the pairs in one hold):

| driver | Knulli no eviction | muOS no eviction | Knulli `--evict 4` | muOS `--evict 4` | Knulli `--evict 16` | muOS `--evict 16` |
|---|---|---|---|---|---|---|
| libmali-64 | 11.03 | 11.24 | 11.25 | 9.14 | 10.43 (10.32 - 10.37 earlier) | 10.56 (10.38 - 10.60) |
| libmali-32 | (11.09, 11.37 earlier) | (10.41, 10.88) | | 8.89 | 9.34 (9.17 - 9.28) | 9.43 (9.44, 9.51) |
| Mesa-64 Panfrost, -O2 | 5.35 | 4.75 | 7.53 | 7.09 | 11.85, 11.88 | 12.05 |
| Mesa-64, PGO (profile: a Knulli replay) | | | | | 10.66 | 10.65 |
| Mesa-64, -O3 `-mcpu=cortex-a53` | | | | | 11.76 | |
| Mesa-64, `mesa_glthread=true` | | | | | 12.01 | 11.92 |

libmali also runs threads of its own: the process's CPU a frame exceeds the calling thread's by 5.3 - 5.5 ms
(no eviction) and 3.2 - 4.1 ms (`--evict 16`) with libmali-64, 3.8 ms with libmali-32, and by 0.00 ms with Mesa
under drm-shim (whose kernel side is missing, below).

Per call, Knulli `--evict 16` (us; calls a frame), and Mesa without eviction:

| call class | calls | libmali-64 | libmali-32 | Mesa -O2 | Mesa PGO | Mesa -O2, no eviction |
|---|---|---|---|---|---|---|
| draw | 87 | 48.4 | 41.6 | 86.6 | 78.1 | 35.0 |
| framebuffer bind | 16 | 138.7 | 116.5 | 8.8 | 7.9 | 3.3 |
| clear | 16 | 14.3 | 12.0 | 17.6 | 16.5 | 7.3 |
| invalidate | 12 | 8.6 | 6.9 | 2.5 | 2.4 | 0.7 |
| uploads (buffer data, maps, texture uploads) | 440 | 3.4 | 3.3 | 3.2 | 3.0 | 2.2 |
| uniforms | 165 | 1.7 | 1.8 | 2.6 | 2.2 | 1.2 |
| glUseProgram | 58 | 4.3 | 4.2 | 7.8 | 6.9 | 3.0 |
| state (binds, enables, blend, ...) | 1,068 | 1.45 | 1.40 | 1.28 | 1.12 | 0.67 |
| frame end (eglSwapBuffers + glFlush) | 1 | 98 | 87 | 205 | 192 | 78 |
| **all, ms a frame** | | **10.43** | **9.34** | **11.85** | **10.66** | **5.35** |
| process CPU beyond the calling thread, ms a frame | | 4.07 | 3.84 | 0.00 | 0.00 | 0.00 |

What it says:

- **On the calling thread, Mesa-64 is not clearly cheaper than libmali-64 where the game's caches are.** With
  warm caches it costs half (5.4 against 11.0 ms), at `--evict 4` two thirds, at `--evict 16` (where libmali's
  calls cost what they cost in the game) +14 % (+1.4 ms; PGO: +2 %, parity). Against libmali-32 it is +2.5 ms
  (PGO +1.3). Mesa's code is cheap when hot and suffers more from a cold cache than libmali's: its draw costs 35
  us warm and 87 us evicted (libmali-64: 52 and 48).
- **Mesa needs no helper threads.** libmali-64 spends another 4 - 5.5 ms a frame of CPU in threads of its own
  (on other cores: heat and the other threads' CPU, not the render thread's frame). Counting them, Mesa-64 under
  drm-shim uses less CPU in all than libmali-64 (11.9 against 14.5 ms at `--evict 16`) - before its kernel side.
- **Where Mesa spends it: the draw.** Panfrost builds each draw's descriptors and jobs at the draw; libmali
  defers a pass's work to the framebuffer bind that ends it (139 us against Mesa's 9). Mesa submits its batches
  at the frame's end (205 us). Its perf warnings (`PAN_MESA_DEBUG=perf`): three whole flushes a frame (the fence
  and the swap), a scissored clear made as a quad about once a frame. A gdb sampling shows no single hotspot left
  after the shim fix. `mesa_glthread` does nothing here (Unity maps, flushes and unmaps a buffer for every draw,
  which syncs the thread); -O3 `-mcpu=cortex-a53` 1 %; PGO 10 %.
- **drm-shim leaves out the kernel and the GPU, so Mesa's numbers are a lower bound.** Missing: each batch's
  submit (kbase: job-chain submission, external-resource mapping, event reads and the fence or event thread a
  kbase backend needs), BO creation and mmap in the kernel (Unity's 140 maps a frame each take a fresh BO in
  Mesa), page faults on fresh mappings, and any wait for the GPU inside a call. At ~16 submits and ~140 BO
  allocations a frame and a few us each in the kernel: +0.5 - 1.5 ms a frame on the calling thread, plus an event
  thread's CPU; and the GPU side (shader code, tiler) is not measured at all. gl64's Step 1 on real kbase (muOS)
  measures draws of 111 - 121 us against the blob's 54 and +10 - 12 ms a frame wall: the kernel, mapping and GPU
  costs drm-shim hides are large.

**Verdict: no - Mesa-64 is not clearly cheaper per call than libmali-64 where it matters.** On the render
thread, in a game-like cache state, it is at parity at best (PGO, +2 %) and +14 % as built, before its kernel
side; it wins only with warm caches the game does not give, and on total CPU through libmali's helper threads.
That is not the -3 ms a frame Step 0 was meant to find, and gl64's real-kbase numbers point the other way.
What could change it: Panfrost's per-draw work (descriptor reuse across draws with the same state, as libmali
evidently does), Unity's per-draw maps through a persistent arena (UA_GL_ARENA does this below the trace for
libmali; the replay gives Mesa Unity's raw pattern), GLES 3.2, and profile-guided layout (-10 % measured).
Step 1 stays useful as a measurement and picture-diff tool; as a frame-rate route it is not shown.

Credits: Mesa (Panfrost, drm-shim; MIT), bmdhacks and the ARMSX2 contributors (armsx2-turnip, malisx2),
Knifethrower (the G31/Bifrost v7 fixes, malisx2's UK 11.7/11.17 kbase layer), JimVulkan (Panfrost on kbase),
Icecream95 (panfork).

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
(runtime/relayout/: Hollow Knight's 6000.0.61f1 IL2CPP arm64 player; whole functions until 2026-11-21, now a `--split` list made from more scenes and the render thread's recordings, which carries to a scene the recordings missed: "The render thread's code" below), with `UA_RELAYOUT_DATA` on;
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
and the patched functions lose only their own move. Other Unity versions need their own rows
(pmu-dev unityhooks prints them; the loops' registers read by hand).

**City of Tears, a boss, the 32-bit build, a soak (2026-11-21), and the default.** All 64-bit runs with the
aarch64 defaults (relayout with HK's list, vtables, atomics), A B A B, then the 32-bit build (armeabi-v7a player,
its own ua_host from the same commit) twice at the same spot; UnityMain a frame, means of two runs:

| | fps | UnityMain ms | cycles | load-miss stalls | L2 refills | render thread ms |
|---|---|---|---|---|---|---|
| City, muOS: off / `UA_PREFETCH` / 32-bit | 44.7 / 44.8 / 45.5 | 15.10 / 14.40 / 14.53 | 27.70 / 26.58 / 26.37 M | 16.60 / 14.72 / 14.24 M | 80.0 / 81.0 / 71.3 K | 15.14 / 15.09 / 14.00 |
| City, Knulli | 45.1 / 45.2 / 46.1 | 14.81 / 14.31 / 14.09 | 23.23 / 22.17 / 22.48 M | 13.07 / 12.54 / 11.60 M | 79.2 / 78.3 / 69.8 K | 15.42 / 15.48 / 13.95 |
| boss, muOS | 40.2 / 41.0 / 41.4 | 15.06 / 15.54 / 15.06 | 27.32 / 26.55 / 26.65 M | 15.74 / 14.51 / 13.32 M | 78.3 / 81.0 / 68.8 K | 14.17 / 14.42 / 12.72 |
| boss, Knulli | 41.4 / 42.5 / 45.0 | 16.00 / 14.30 / 14.89 | 25.51 / 23.32 / 22.14 M | 14.24 / 12.43 / 10.91 M | 84.0 / 77.3 / 65.9 K | 13.83 / 13.77 / 12.05 |

The boss is the Hall of Gods' second lower-floor statue (`spots/gg`: continue from the Hall's bench, attack off
it, right 4.67 s, up, confirm twice; in the arena a fixed walk / attack / jump pattern). Each fight goes its own
way (the instructions retired a frame differ by up to 20 % between runs, three of the eight 64-bit runs include a
1.1 s death reload), so its rows say less than the standing scenes'. City is GPU-bound on both builds (44 - 46 fps):
there the prefetch takes 0.5 - 0.7 ms off UnityMain without moving the frame rate.

**Where 64-bit stands now**: UnityMain at City is at the 32-bit build's cycles (muOS 26.6 against 26.4 M, Knulli
22.2 against 22.5 M), at King's Pass below them (Knulli 25.4 against 25.8 M). The frame-rate gap left (0.7 - 0.9 fps
at City, 0.4 - 2.5 at the boss) is the render thread: UnityGfxDeviceW 15.1 - 15.5 ms against 13.9 - 14.0 ms, of it
+1.7 - 2.1 M i-cache stall cycles a frame (the 64-bit libmali's code), +1.3 M load-miss stalls, +10 K L2 refills.
The next lever is route 4 (and the 64-bit Mali driver), not libunity's loops.

Soak, Knulli, `UA_PREFETCH=1`, 25 min in the Crossroads with a fixed walk / attack / jump pattern (deaths and
respawns included): no crash, 55.4 fps throughout (per 3-minute segment 55.0 - 56.8), game RSS 736 - 739 MB from the
4th minute to the end, the only hitches 4 of ~310 ms at 6-minute intervals (`UA_GC_BUDGET`'s collections, as
without it), no swap-ins.

**Default**: on in the runtime for the 64-bit player (`UA_PREFETCH` unset = `beh,sprite,cull,play`; `0` off; `1`
adds `ps`), checked with the build that sets it: the unset runs below log the four sites and match `UA_PREFETCH=1`'s
earlier numbers. It only acts where a player's rows match (6000.0.61f1 arm64-v8a), silently elsewhere; nothing in a
port's port.env is needed.

**The next loops** (King's Pass, Knulli, load-miss stall samples `UA_PROF_EVENT=raw:0xe7` every 1 M cycles, 64-bit
with relayout and prefetch against 32-bit, share of UnityMain's stalls): `QueryComponentByType` 2.0 % against 1.5 %,
`Playable::Handle` 1.3 / 0.7, `FlattenBasicData` 1.0 / 0.4, the outline atomics 1.9 / 0 (inline in the 32-bit
player), `FMOD::SystemI::UpdateAudibilities` 3.7 / 3.0 (moved off the thread by `UA_FMOD_ASYNC`), `CallUpdateMethod`
now 0.5 against 3.2. `QueryComponentByType`'s samples come mostly from `ParticleSystem::UpdateManualTransformData`'s
loop over every particle system (the Transform lookup, then `GetTransformAccess`), then `AudioSource::CachePositional`
and the particle systems' job scheduling: site `ps` prefetches that loop three systems deep (the system, its
GameObject and the objects the loop reads from it, the GameObject's component array). Measured at King's Pass,
`UA_PREFETCH=0` / unset (the default) / `1` (with `ps`), A D P then P D A, UnityMain cycles a frame: muOS 31.72 /
28.78 / 28.35 M and 28.43 / 28.95 / 31.44 M (the default -8.6 %, `ps` a further -1.6 %, 51.2 / 51.9 / 52.2 fps);
Knulli 27.27 / 24.92 / 26.05 M and 28.63 / 28.35 / 30.72 M (the default -8.2 %, `ps` +2.6 %; the second hold's
King's Pass was heavier for all three, 4.9 M instructions a frame against 4.5). `ps` stays opt-in.

#### Route 3, the next step: our own behaviour loop (`UA_NATIVE_BEH`, 2026-11-21)

`runtime/host/nativebeh.c` (opt-in, `UA_NATIVE_BEH=1`; `=verify` the differential test; `UA_NATIVE_BEH_STATS=<s>`
the loops' time; [runtime.md](runtime.md)). The three `BaseBehaviourManager::CommonUpdate`s of 6000.0.61f1's arm64
player are replaced whole: one function does what Unity spreads over fourteen (CommonUpdate, SafeIterator::Next,
the vtable dispatch, MonoBehaviour::Update / CallUpdateMethod, IsStateMachineBehaviour with GetScriptCache, the
weak_ptr's lock and two releases with their four atomics, GetEnabled, IsActive's cached byte), the same loads and
stores in the same order (volatile accesses; the atomics as the NDK helpers' LL/SC loops, the same memory
orders), Unity's own functions called for everything else, and the `beh` prefetch stub's fetches in place (so
`UA_PREFETCH`'s `beh` sites, inside the replaced functions, stay off; `sprite`, `cull`, `play` work alongside).

**What the loop is, read from the player** (Hollow Knight, King's Pass standing): 612 - 657 behaviours a frame
(Update, LateUpdate, FixedUpdate together; PlayMaker's FSMs most of them), each one called (none of the early exits
taken there), 0 - 0.04 Starts a frame. Unity's lists are moved node by node through a SafeIterator; scripts that
add a behaviour put it on the queue's pending list (`AddBehaviour`, merged by `IntegrateLists` at the next loop's
start), so the next frame's order depends on the iterator's surgery: a list mirror that skipped it would change
that order. The surgery is kept as Unity does it.

**Exact, proven on the live game** (`UA_NATIVE_BEH=verify`: our loop drives, Unity's own `Next`, `IsStateMachineBehaviour`
and `CallUpdateMethod` do every step, ours is computed on the same state and compared: the iterator's step on a
write overlay against every word Unity's changed, the Start and update calls predicted and compared at Unity's
`InvokeMethodOrCoroutineChecked` / `ScriptingInvocation::Invoke` with object, method and instance id), the 64-bit
copies, 25 s at King's Pass with the title screen and the load before it:

| verify | iterator steps | IsStateMachineBehaviour | update calls | Starts | mismatches |
|---|---|---|---|---|---|
| muOS | 1,426,220 | 1,346,812 | 1,346,812 | 5 | 0 |
| Knulli | 1,251,588 | 1,179,336 | 1,179,336 | 5 | 0 |

The order of the scripts' calls (an FNV hash of instance id and method over a frame) is the same in Unity's loop,
ours and verify on both handhelds (the 625-call King's Pass frame: `52fd05d86a85d389` in all six). test_nativebeh
(runtime/host/test) runs the same three ways over a model with scripts that change the lists while they run and
every early exit of CallUpdateMethod, and checks that verify catches a wrong Next and a wrong CallUpdateMethod.

**Measured.** The loops' own work (`UA_NATIVE_BEH_STATS`: their time minus the scripts' calls; `UA_RELAYOUT=0`,
`UA_PREFETCH=1`, so Unity's loops run without the `beh` stubs, whose entries the timing hooks change):

| own work a frame | Unity's loops | ours |
|---|---|---|
| muOS | 1.37 - 1.45 ms | 0.57 - 0.62 ms |
| Knulli | 1.59 - 1.68 ms | 0.60 - 0.68 ms |

Against today's best (the aarch64 defaults: `UA_RELAYOUT` with HK's list, `UA_RELAYOUT_DATA`, `UA_ATOMICS=llsc`,
and `UA_PREFETCH=1` with its `beh` stubs), A B A B in one hold, `hwcount` on UnityMain, a frame's worth:

| King's Pass | fps | UnityMain ms | cycles | load-miss stall cycles | L2 refills | i-cache stall cycles |
|---|---|---|---|---|---|---|
| Knulli, base | 52.9, 52.7 | 15.97, 15.65 | 26.35, 26.47 M | 14.31, 14.32 M | 86.4, 86.5 K | 6.20, 6.17 M |
| Knulli, `UA_NATIVE_BEH=1` | 52.5, 52.8 | 16.04, 16.12 | 26.29, 26.35 M | 14.24, 14.12 M | 87.1, 85.9 K | 6.22, 6.23 M |
| muOS, base (hold 1) | 51.9, 51.4 | 15.93, 16.21 | 29.49, 29.51 M | 16.34, 16.68 M | 85.7, 86.9 K | 6.89, 6.89 M |
| muOS, `UA_NATIVE_BEH=1` (hold 1) | 46.7, 50.4 | 16.52, 16.01 | 31.89, 28.92 M | 18.30, 16.34 M | 90.8, 89.7 K | 7.56, 6.95 M |
| muOS, base (hold 2) | 51.8, 51.6 | 15.83, 16.11 | 29.20, 29.25 M | 16.17, 16.62 M | 86.1, 87.2 K | 6.85, 6.86 M |
| muOS, `UA_NATIVE_BEH=1` (hold 2) | 51.7, 51.5 | 15.43, 15.65 | 29.43, 29.71 M | 16.11, 16.58 M | 87.3, 86.5 K | 6.96, 6.95 M |

(muOS hold 1: both `UA_NATIVE_BEH` runs had 370 - 860 ms hitches after the scene's load, one inside the measurement,
and its first run retired 6 % more instructions (another state of the scene); hold 2, the same pairs again, had no
hitch after the load in any run: the hitches were not the loop's.)

What it says: the call overhead is real (Unity's loops spend 2.2 - 2.6 us of their own a script, ours 0.9 - 1.1 us)
but the relayout and the `beh` stubs had already taken most of it: against them our loop is within the runs'
spread (Knulli: cycles -0.3 %, load-miss stalls -0.9 %, UnityMain's CPU +0.27 ms; muOS hold 2: cycles +1.2 %,
UnityMain's CPU -0.43 ms, 51.6 against 51.7 fps). What is left is the lines each script's call must read - the
behaviour's four (vtable, handle, GameObject and enabled byte; the list node; the script cache and method table;
the started byte), its GameObject's, its managed object's - and the call into Mono (`ScriptingInvocation`, five
write barriers through the bridge, `mono_runtime_invoke`), which Unity's layout fixes. Taking lines away needs a
compact mirror of the per-behaviour state (handle, GameObject, method table, started, the script cache's flag)
kept in step with every place Unity writes them; that is not built (each writer must be found and hooked, and a
behaviour's memory is reused by later objects). Kept opt-in: exact, proven, no cost, no measured gain on top of
route 3's stubs.

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
| 3. rewritten hot functions (prefetch, inline atomics) | -0.2 to -0.4 ms; built as `UA_PREFETCH` (call-site stubs): UnityMain -0.47 / -0.51 ms, cycles -6.9 / -4.9 % (muOS / Knulli); the behaviour loop rewritten whole (`UA_NATIVE_BEH`, exact, verified on both handhelds): its own work 1.4 - 1.7 -> 0.6 ms, but no gain over the stubs + relayout | medium, per Unity version | yes | more loops; default after a busier scene and a soak; a compact per-behaviour mirror |
| 4. less render-thread L2 traffic | 0.1 - 0.3 ms (to be measured) | small to measure | yes | `hwcount` on UnityMain with render-thread work removed |
| 5. Unity bucket allocator settings | 0 - 0.2 ms | small | yes | three boot.config variants, `hwcount` |
| 6. Mono LLVM AOT (elsewhere) | about -0.8 ms | in progress | as measured there | - |
| 7. 32-bit game + 64-bit GPU process | 32-bit speed, still armhf | large | yes | only if a firmware lacks a 32-bit userland |
| 8. armv7 -> A64 translator | worse than today | very large | hard | none |

Routes 1 + 2 + 6 together reach the 1.5 - 1.9 ms the 64-bit build needs; 3 - 5 are margin. Route 1 is the
first prototype: one hook, measurable in a day, and it pays on both builds and every Unity game that uses
Unity's built-in audio.

## The render thread's code: libunity's hot/cold list with the render thread's recordings, and ua_host's own hot code grouped (2026-11-21)

Where Unity's render thread waits for instruction fetch (`UA_PROF_EVENT=raw:0xe1`, King's Pass and City of Tears,
64-bit, the aarch64 defaults; Knulli and muOS alike): 61 - 64 % in libmali, 23 - 24 % in libunity, 10 - 12 % in
ua_host, 3 % in libc. libmali's code is not ours to move (above). Of the libunity part, 98 - 99 % already falls in the
shipped list's functions (it was made from both threads' recordings): what is left there is the cold half of each hot
function. ua_host's part is our code: 120 - 149 functions with samples on the render thread, 17 - 27 KB of sampled
lines, spread over 1.2 MB of ua_host's text (the GL layers: the buffer pool's `h_acquire`, `glrec`'s hooks, the state
filter's `before_draw`, the `w_gl*` wrappers, the loader's hooks of bionic's `pthread_getspecific`, `memcpy`).

**What changed:**
- **libunity's list** (runtime/relayout/8936b5...txt) is now made with `--split` from the earlier three recordings plus six new
  ones (King's Pass and City of Tears, time and i-cache stall samples of UnityMain and the render thread, Knulli and muOS):
  3,166 functions, 593 of them in hot and cold pieces, 628 KB hot of 896 KB. The earlier split list carried less to scenes
  the recordings missed; this one, measured also in the Forgotten Crossroads (in no recording), carries.
- **ua_host's hot functions grouped at build time.** `runtime/host/hot-arm64.txt` names the 206 functions that cover
  99.5 % of ua_host's samples on UnityMain and the render thread (`tools/hotlist`, a small Rust crate, makes it from
  `UA_PROF` recordings and ua_host.debug's symbol table; porting.md); `runtime/host/build.sh` (ARCH=arm64) compiles ua_host's objects with
  `-ffunction-sections` (the loader, shims and fake JNI archives too) and renames those functions' sections
  `.text.hot.<name>` (objcopy, on the objects and on copies of the archives): GNU ld's default script puts `.text.hot.*`
  together before the rest. 95 KB of them, one block where they were spread over 1.2 MB. Nothing in the code changes.

**Measured** (64-bit, A = the defaults of origin/main, B = the new list + the grouped ua_host; `hwcount` on UnityMain and
the render thread, cycles and i-cache stall cycles a frame, the means of two runs each, A B A B in one hold from a cool
handheld):

| scene, handheld | fps A / B | UnityMain cycles | UnityMain i-fetch stalls | render thread cycles | render thread i-fetch stalls |
|---|---|---|---|---|---|
| City of Tears, Knulli | 45.5 / 45.5 | 22.19 -> 21.97 M (-1.0 %) | 5.14 -> 4.92 M (-4.1 %) | 23.85 -> 23.66 M (-0.8 %) | 6.61 -> 6.43 M (-2.7 %) |
| City of Tears, muOS | 46.9 / 46.1 | 27.87 -> 26.47 M | 6.79 -> 6.28 M | 27.33 -> 26.84 M (-1.8 %) | 7.88 -> 7.72 M (-2.0 %) |
| King's Pass, Knulli | 52.3 / 52.7 | 25.22 -> 24.96 M (-1.0 %) | 5.73 -> 5.52 M (-3.7 %) | 17.92 -> 17.89 M | 5.82 -> 5.83 M |
| King's Pass, muOS | 52.0 / 52.2 | 29.02 -> 28.62 M (-1.4 %) | 6.66 -> 6.41 M (-3.7 %) | 20.31 -> 20.31 M | 6.81 -> 6.70 M |
| Forgotten Crossroads (not recorded), muOS | 55.0 / 54.4 | 26.02 -> 24.55 M (-5.7 %) | 6.75 -> 6.15 M (-8.8 %) | 21.59 -> 20.99 M (-2.8 %) | 7.21 -> 6.86 M (-4.9 %) |
| Forgotten Crossroads, Knulli (B: one run; the other had a 1 s hitch in the measurement and 8 % more instructions: another state of the room) | 54.4 / 54.7 | 21.06 -> 21.12 M | 5.26 -> 5.11 M (-2.8 %) | 18.37 -> 18.42 M | 5.75 -> 5.77 M |

ua_host's grouping alone (the shipped whole-function list kept), City of Tears: render thread cycles -1.3 % (muOS,
27.31 -> 26.94 M) and -0.3 % (Knulli, 23.87 -> 23.79 M), its i-fetch stalls 0 / -1.7 %, its L1I micro-TLB refills
-12 / -15 % (one block of pages instead of 40); fps 45.0 / 46.5 (muOS), 45.4 / 45.3 (Knulli).

What it says: the frame rate is the GPU's in these scenes (City of Tears) or within the runs' spread; the counters move
the same way in every pair but one (the Crossroads on Knulli, where they stay within the spread): UnityMain -1 to -6 % cycles (the split list's hot ranges), the render thread 0 to -3 %
(mostly where ua_host's GL layers and libunity's render code are a larger share: City of Tears, the Crossroads). The
render thread's remaining i-fetch stalls are libmali's. Both are now the defaults (the list ships in runtime/relayout/,
the grouping is in the arm64 build).

## Remaining bottlenecks (2026-11-21)

A census with every 64-bit default on (relayout with the split list, atomics, prefetch, 1 MB allocator blocks;
`lib-gG8`), Hollow Knight, 32-bit against 64-bit in one hold a scene, order 32 64 64 32, 25 s each from a cool
handheld, `HWCOUNT=1` on UnityMain, the render thread, libmali's threads (`mali*`) and FMOD's, `GPUCNT=1`.
A frame's worth; two runs of each build.

| scene, handheld | fps 32 / 64 | GPU busy 32 / 64 | UnityMain cycles | render thread cycles | of them i-fetch stalls | load stalls | its L2 refills | libmali's thread |
|---|---|---|---|---|---|---|---|---|
| King's Pass, Knulli | 48.6, 52.6 / 52.9, 53.2 | 87, 92 / 93, 93 % | 26.0 / 26.2 M | 15.7 / 18.1 M | 4.1 / 5.7 M | 6.8 / 8.0 M | 36 / 44 K | 1.9 / 2.4 M |
| King's Pass, muOS | 52.6, 53.1 / 52.0, 52.6 | 84, 89 / 85, 90 % | 30.2 / 30.1 M | 17.7 / 20.7 M | 4.8 / 7.0 M | 7.9 / 9.3 M | 37 / 47 K | 2.2 / 2.7 M |
| City of Tears, Knulli | 49.6, 47.3 / 47.8, 47.2 | 99.4 - 99.8 % all | 24.2 / 24.1 M | 22.5 / 24.7 M | 5.0 / 7.0 M | 9.4 / 10.7 M | 47 / 56 K | 2.1 / 2.5 M |
| City of Tears, muOS | 46.4, 45.2 / 47.1, 45.1 | 98.5, 99.6 / 97.5, 99.5 % | 26.8 / 27.6 M | 24.6 / 27.5 M | 5.7 / 8.1 M | 10.9 / 12.1 M | 47 / 59 K | 2.4 / 2.8 M |
| Hall of Gods boss, Knulli (median second of the fight) | 42.4, 42.6 / 42.3, 42.4 | 99.1, 99.7 / 95.6, 94.8 % | 22.0 / 23.6 M (one run each) | 18.6 / 19.9 M | 4.3 / 5.8 M | 8.1 / 8.8 M | 40 / 48 K | 2.0 / 2.3 M |
| Hall of Gods boss, muOS (median second) | 42.3, 41.0 / 40.1, 40.2 | 99.0, 99.3 / 98.2, 99.0 % | 25.8 / 25.1 M | 20.6 / 24.6 M | 5.0 / 7.7 M | 9.2 / 10.9 M | 42 / 55 K | 2.2 / 3.0 M |

UnityMain's own split is unchanged in kind from the tables above: on 64-bit 12 % fewer instructions, load-miss
stalls +0.9 - 1.3 M, i-fetch stalls +0.4 - 0.6 M, L2 refills 75 -> 85 K, iTLB stall cycles *fewer* (0.50 against
0.72 M: the relayout's one block), micro-TLB refills the same or fewer. FMOD's threads count the same on both.

**The critical path, per scene.** King's Pass: UnityMain (93 - 97 % of a core, GPU 85 - 93 % busy), at the same
cycles on both builds: no gap (Knulli's first 32-bit run met a 660 MHz GPU). City of Tears and the boss: the GPU
(98 - 99.8 % busy), the same work on both builds; the frame rate follows the GPU's clock run by run (645 - 693 MHz
Knulli, 603 - 670 MHz muOS), not the build. The boss fight's "2.5 fps on Knulli" was the measurement: the 25 s
window takes in a different length of the 57 - 60 fps seconds after the fight ends each run (the fights diverge);
by the median second of the fight Knulli is equal and muOS 1 - 2 fps apart between runs whose fights differ.

**What the 64-bit build still pays, ranked** (none of it on a critical path on the RG34XX-SP):

| # | where | size a frame | whose | lever left |
|---|---|---|---|---|
| 1 | render thread, instruction fetch | +1.5 - 2.7 M cycles (0.9 - 1.5 ms) | libmali-64 (61 - 64 % of the thread's fetch stalls) | none outside the driver: see below |
| 2 | render thread, load misses | +0.7 - 1.7 M cycles | libmali's and libunity's data | - |
| 3 | UnityMain, load misses | +0.9 - 1.3 M, paid back by fewer instructions and mispredicts | libunity, managed | prefetch sites per game |
| 4 | libmali's own thread (`mali-cmar-backe`) | +0.4 - 0.8 M cycles | libmali-64 | - |
| 5 | memory | resident +16 - 50 MB (King's Pass 649 -> 691 - 701 MB Knulli, 675 -> 691 - 698 muOS; boss 680 -> 730, 696 -> 735) | pointers | `UA_MONO64_ASSEMBLIES=scripting` (opt-in, ~10 MB) |
| 6 | G350: hangs | see below | not found (not memory, not every 64-bit start) | a hang caught with the logger running |

**Tried against it, not kept:**

- `UA_GL_ARENA_SUBDATA=1` (constant-buffer uploads as copies into the arena), City of Tears, 64-bit, A B B A:
  every upload was copied (about 215 a second; none to the driver), render thread 24.10 -> 23.88 M cycles
  (Knulli), 27.49 -> 27.01 M (muOS); fps 48.2, 46.1 -> 46.9, 46.1 and 44.6, 44.7 -> 46.3, 45.1 with the GPU's clock.
  Too few uploads reach the driver to matter; stays opt-in.
- UnityMain on a core of its own (`UA_AFFINITY='UnityMain:3 *:0-2'`), King's Pass, 64-bit, A B B A: Knulli 54.2,
  53.6 -> 54.4, 53.9 fps, muOS 51.8, 51.8 -> 52.1, 51.8; UnityMain's cycles -1 %, its bus accesses -10 %. Within
  the spread; not set.
- Already there and counted again: `UA_GL_ARENA` serves every one of Unity's ~140 buffer maps a frame from its own
  persistent mapping (the driver sees no map, flush or unmap in the game; the 140 in the replay tables are the
  trace's), so there is no cheaper buffer path left to steer Unity onto; of the frame's 16 passes only one has no
  draw (the 720x450 darkness texture's clear: `UA_GL_SAMECLEAR`, exact, nothing gained), the other 15 binds each
  begin a pass with draws.

**libmali, the facts** (headers and strings only): Knulli and muOS ship the same 64-bit blob byte for byte
(r20p0-01rel0, 44.5 MB, `.text` 34.6 MB) and the same 32-bit build (r20p0-01rel0, `.text` 11.4 MB). The 64-bit
blob exports 114 OpenCL and Vulkan entry points (`cl*`, `vk*`) and the 32-bit one none: most of the 3x is whole
APIs the 32-bit build leaves out, not the GLES path, whose hot code is 116 KB against 91 KB (above).

**The G350** (2026-11-21, three sessions between power cycles; EmulationStation left running in all of them, as
a player's launch has it: profbench's `STOP_ES` knows only init scripts). King's Pass:

| | 32-bit | 64-bit |
|---|---|---|
| 20 s from a cool start (32 64 64 32) | 43.5, 43.6 | 44.2, 44.3 |
| 90 s (32 64 64 32, one hold) | 43.7, 43.5 | 43.6, 43.6 |
| 60 - 90 s, each right after the last (the 32-bit runs with 60 MB held in /dev/shm) | 43.7, 43.7, 43.8, 44.0 | 13 runs: 42.4, 43.7, 43.8, 43.8, 43.9, 43.9, 44.0 six times, 44.1 |
| lowest MemAvailable in a run / the game resident | 290 MB / 502 MB (231 - 235 with the 60 MB held) | 316 - 327 MB / 511 - 520 MB |

Equal frame rates (the GPU sits at 480 - 520 MHz and is the limit with EmulationStation running) and more memory
left on 64-bit. The first game run after a boot is slow on either build (the title screen at 30.2 fps, King's
Pass 31.1 fps on 32-bit and 31.6 on 64-bit in two such runs: paced at half the refresh rate); every later run is
as above. The counters there under-count with this many threads (the A35's fewer counters) and are not tabled; in
ratio the GL thread's cycles are +20 % on 64-bit, nearly all instruction fetch, and UnityMain's lower.

*Hangs.* The handheld stopped answering twice that day, both at the start of a 64-bit run: the fifth run after one
power cycle and the second after the next. After the third power cycle a logger ran beside every run (a line a
second appended and fsynced to the card: MemAvailable, the game's resident size, the Mali's pages from its
debugfs, temperatures, clocks; the kernel log followed into a second file): 22 runs, 16 of them 64-bit, in the
orders that had hung (64-bit right after 32-bit, 64-bit after 64-bit, the same 90 s list again), without a hang
and without a line from the Mali or the kernel in the log; the Mali's memory goes to 36,000 pages (140 MB) in a
run and back to its 3,400 - 5,300 after it, memory is never short. So the hang is neither memory pressure (it also
came once at 481 MB available, 2026-11-20) nor each 64-bit start; what sets it off is not found.
By build since 2026-11-19, from the helpers' result folders (a hung run leaves none, so the starts are
approximate): 64-bit about 64 starts, 6 hangs (11-19 twice, 11-20 twice, 11-21 twice); 32-bit about 36 runs, none
(two earlier: 11-08 after a long series, 11-09 in City of Tears with EmulationStation's memory held). A third hang the same afternoon was caught with the logger running (the release's own install, the boss save,
EmulationStation running): its last line is 95 s into the run, the game loading the boss scene (resident 533 ->
615 MB over the last 16 s), 230 MB available, the Mali at 21,400 pages (84 MB), 66 - 68 C, the CPU at 1.2 - 1.5
GHz; after it nothing, and nothing from the kernel or the Mali in the followed kernel log. A hard stop in the
middle of a 64-bit run during a scene load, with memory to spare and no kernel message: the same as the hangs
of 2026-11-20. Two things in that session's kernel log are not in the hang-free session's (one USB enumeration
at boot, a mode line for each of its game starts): the USB gadget was enumerated again by the host three times
while games ran, and the display's mode line (`vop_crtc_enable: Update mode to 640x480p60`) is there for the
session's first two game starts and missing for the last two, the run paced at half rate (City of Tears, 34.8
fps) and the one that hung. In these runs the game was started over SSH with EmulationStation in the foreground;
the system's own launch is `sudo perfmax %GOVERNOR% %ROM%; nice -n -19 %ROM%; sudo perfnorm` from
EmulationStation (es_systems.cfg), which sets the CPU's, GPU's and memory's governors to the system's launch
setting first (idle: ondemand, simple_ondemand, dmc_ondemand; the GPU's clocks are 400, 480 and 520 MHz), so
every frame rate of this page taken over SSH on the G350 is the idle governors', not necessarily a player's.
Until a
hang is caught with the logger running (`/roms/uatest/gap64hunt/glog.sh`; the last second is then on the card),
this, not the frame rate or memory, stands between the G350 and an aarch64-only port.

### Heat-soaked: eleven minutes a build (2026-11-21)

The 25 s runs start cool; a player's session does not. King's Pass standing, Knulli, 660 s measured, no counters,
each run its own hold after the lock's cooling wait (to 50 C at the sensor; the handheld's body is not that cool
after the first run), order 32 64 64 32. Frames a second by minute, the GPU's clock and the CPU zone's
temperature by minute:

| run | fps, minutes 1 - 11 | GPU MHz by minute | CPU zone C, first -> last minute | seconds at 696 MHz | minutes 6 - 11 |
|---|---|---|---|---|---|
| 32-bit, first (the handheld cold) | 55.2 55.8 55.6 54.8 54.1 53.4 53.3 53.3 52.9 53.3 52.8 | 696 x6, then 600 | 65 -> 72 | 353 | 53.2 |
| 64-bit | 53.4 53.2 53.2 53.2 53.2 53.4 53.2 52.9 53.2 53.3 52.7 | 616, then 600 | 69 -> 74 | 30 | 53.1 |
| 64-bit | 52.9 54.2 55.0 52.9 53.2 53.2 53.2 52.9 53.2 53.2 52.8 | 601 651 667, then 600 | 69 -> 74 | 77 | 53.1 |
| 32-bit, last | 54.4 53.5 53.9 52.7 53.4 53.3 53.2 52.4 52.9 53.5 52.9 | 649 603 635, then 600 | 69 -> 75 | 128 | 53.0 |

Once warm the two builds run the same 53 fps at the same 600 MHz and the same temperatures: at that clock the GPU
is the limit (UnityMain 86 - 88 % of a core on 64-bit, 94 % on 32-bit) and it does the same work for both. The
first run's four minutes at 696 MHz are the cold handheld, not the build: the second 32-bit run, started as warm
as the 64-bit ones (69 C in its first minute), is at 600 MHz from its second minute like them. What is left of a
difference is the time before the governor steps down from a warm start (128 s against 30 and 77 s: one run
against two, within what two runs of one build differ by).

**All of the game's CPU work, King's Pass** (Knulli, the census's second runs, % of one core on a CPU; every
thread above 1 %; the handheld's own processes add 8 - 10 % on either build):

| thread | 32-bit | 64-bit | |
|---|---|---|---|
| UnityMain | 97 | 97 | both |
| UnityGfxDeviceW (render thread) | 63 | 71 | +8: the 64-bit libmali's instruction fetch |
| Job.Worker_0 | 34 | 35 | both |
| FMOD mixer | 30 | 28 | both |
| mali-cmar-backe (the driver's thread) | 17 | 19 | +2 |
| ua_host (main), Background_Job, ua_flip (ours: 2) | 3, 3, 2 | 3, 3, 2 | both |
| sum | 249 | 258 | +3.6 % |
| GPU active | 91.7 % at 696 MHz | 93.0 % at 696 MHz | the same frame |

The 64-bit build's own share of the heat is that +9 % of a core, all of it inside the driver. Everything larger
is paid by both builds alike (UnityMain, the job worker's wake-ups, FMOD's mixer), and each of those was measured
before by frame rate and by clock residency ([profiling.md](profiling.md): the job workers, `UA_FUTEX_SPIN`, the
mixer's rate).

### The job worker and FMOD's mixer, taken apart (2026-11-21)

The two largest threads after UnityMain and the render thread, on both builds (King's Pass, Knulli, 64-bit;
`UA_PROF=500` on them with the player's symbols, `UA_FUTEX_STATS`; the profiled run itself is slower, 47 fps:
shares, not totals).

**The job worker** (one worker, 29 - 35 % of a core, 88 % of it user time). What it runs is work, most of it
ours to have put there: `ForwardRenderLoopJob` (`UA_GFX_JOBS=legacy`: the sprites' draw calls built off
UnityMain) is on a third of its samples, then playables, culling and the transform dispatch. Around the work:
1,376 futex waits a second, three in four shorter than 250 us (it sleeps and is woken about 26 times a frame),
and it waits for a core 16 - 19 % of the time. UnityMain makes about 3,000 `FUTEX_WAKE` calls a second of which
1,250 - 1,550 wake a thread (16 ms a second in them, 5 us each); the render thread 2,000, 550 - 660 of them
waking one. No spin or yield loop of Unity's shows in the samples.

| tried, 64-bit, A B B A (or A V V V A), fps | Knulli | muOS |
|---|---|---|
| wakes with no sleeper in the kernel not made, no watching (`UA_FUTEX_SPIN=1 UA_FUTEX_SPIN_THREADS=nobody`): 810 - 910 of UnityMain's wake calls a second left out, its time in wakes 16.4 -> 13.0 ms a second | 53.9, 54.5 -> 53.9, 53.5 | 54.0, 52.1 -> 53.4, 52.8 |
| a core each for UnityMain, the render thread and the worker, the rest on the fourth (`UA_AFFINITY='UnityMain:3 UnityGfxDeviceW:2 Job.Worker*:1 *:0'`; 42 threads placed): the worker's wait for a core 16 - 19 -> 6 - 12 % | 54.3, 53.9 -> 54.3 | 52.7, 52.5 -> 52.5 |
| the same with FMOD's threads on the worker's core: the worker waits 25 - 27 % | 54.0 | 52.0 |
| the same with libmali's thread on the render thread's core: the render thread waits 18 - 19 % | 53.5 | 51.8 |

None moves the frame rate: a no-sleeper wake costs the kernel a few microseconds (0.05 ms a frame in all), and
the worker's wait for a core is not what UnityMain waits for (it waits 15 - 37 ms a second in futexes in all).
Not set.

**FMOD's mixer** (28 % of a core on 64-bit, 30 % on 32-bit: nothing here is the 64-bit build's). With the sound
off it is clocked as with sound: the null sink takes 256 frames on the monotonic clock every 10.7 ms
(runtime/android/opensles.c), a sound card's pace. Its samples: Vorbis decoding 44.5 % (FMOD's Tremor: the clips
kept compressed in memory, decoded each time they play), `FMOD::DSPFilter::read` itself 15 % (the walk of the DSP
graph and its copies), the AudioMixer groups' faders 14 %, the SFX reverb 11 %, a flange 3.4 %, linear
resampling 3 %, the connections' mixing 2 %. The decoding is the audio step's memory budget at work
([profiling.md](profiling.md#fmods-mixer-and-the-audio-step-2026-10-24)): at the 1 GB budget of 16 MiB five of
the game's twelve always-playing loops stay compressed, and the budget of 36 MiB (all twelve decoded once) took
5 points of a core off the mixer there for about 20 MB; the rest of the decoding is clips the game itself ships
compressed. Which budget a 1 GB handheld gets was set for memory (2026-11-09); with 1 MB allocator blocks the
64-bit build has 60 MB more to spare at King's Pass than it had then.

**The sound budget on a 1 GB handheld (G350, 2026-11-21).** Hollow Knight's aarch64-only release set up twice on the
G350 (dArkOS, 895 MB, no swap), with `audio.lowmem_pcm_budget_mb` 16 and 36, against the 32-bit bench install (16 MiB),
every run launched by EmulationStation (its `perfmax performance`; one test entry first in Ports, EmulationStation
restarted and a single A for each launch), silent, EmulationStation resident. The lowest `MemAvailable` over the
scene (a logger's line every second) and FMOD's mixer thread in per cent of a core:

| scene | 32-bit, 16 MiB | 64-bit, 16 MiB | 64-bit, 36 MiB |
|---|---|---|---|
| King's Pass, standing 120 s | 206 MB, 48.9 % | 230 MB, 43.1 % | 208 MB, 39.7 % |
| City of Tears, standing 120 s | 223 and 209 MB, 46.2 and 45.9 % | 245 MB, 41.5 % | 218 and 221 MB, 38.4 and 38.2 % |
| Hall of Gods, walking and striking 120 s | 201 MB, 47.5 % | 208 MB, 34.1 % | 207 MB, 31.6 % |
| a Crossroads bench, 210 s | 221 MB, 61.8 % | 253 MB, 55.6 % | 233 MB, 49.4 % |
| 24 rooms, one every 8 s of play (a test DLL sends the Knight through the doors: 34 - 38 scene loads in 420 s) | 173 and 168 MB, 47.8 and 47.9 % | 189 MB, 45.6 % | 166 and 162 MB, 40.6 and 40.5 % |

With 36 MiB the 64-bit build has the 32-bit build's memory at 16 within a few MB (room by room in the tour between
8 MB more and 8 MB less; 6 MB less at the tour's lowest point, repeatably, with over 160 MB left), and the mixer
takes 3 - 6 points of a core less than at 16 MiB (and 7 - 16 less than the 32-bit build's); the frame rates do not
move (City of Tears 36.9 fps with both, the tour 44.1 and 44.4, the bench 45.6 and 45.2). The game's resident size
is about 20 MB larger (King's Pass 615 -> 626 MB, City 605 -> 625). Hollow Knight's 64-bit only release takes 36 MiB
on every handheld (portmaster-hollowknight port.toml). Not measured: a boss fight and a death with its respawn load.
The scripted presses that reach them on the RG34XX-SP did not on the G350 (the Knight stands at the statue, the
challenge does not start; it stays at the Crossroads bench), so the boss save's row is the Hall of Gods room and the
tour stands in for the loads.

**Looked at and left** (2026-11-21, numbers from `UA_PROF=500` on UnityMain and the mixer, Knulli, 64-bit):

- *Physics2D on the job workers.* `PhysicsManager2D::Simulate` is on 5.2 % of UnityMain's samples at King's Pass
  (0.95 ms a frame: contact updates 3.4 %, the step 1.6 %, the solver 0.8 %) and 3.7 % in the boss fight
  (0.6 ms). The ports run one job worker, 35 % busy and waiting 16 - 19 % of the time for a core, so the most a
  move could give is a few tenths of a millisecond; and Unity's multithreaded 2D solver orders contacts and
  callbacks by thread unless its consistency sorting is on, so "the same play" would have to be shown step by
  step in the emulator rig first. Not started.
- *A faster Vorbis decode.* The decode is 44.5 % of the mixer's time (FMOD's Tremor, integers throughout): the
  inverse MDCT and the overlap (`fmod_tremor_mdct_backward`, `_dsp_lapout`: 16 % of the mixer) could be done with
  NEON to the same bits, the codebook and bit-reader part (`decode_packed_entry_number`, `decode_map`,
  `book_decode*`: 18 %) is serial. About 2 - 3 points of one core on a thread that limits nothing, for hooks into
  FMOD's internals per Unity version and a bit-compare harness. Not started.
- *The mixer's effects on silence* (the groups' faders 14 %, the SFX reverb 11 %, a flange 3.4 %). The game's
  twelve resident loops play in every scene at whatever level the AudioMixer snapshots give them
  ([profiling.md](profiling.md#fmods-mixer-and-the-audio-step-2026-10-24)), so the groups' inputs are quiet signal,
  not zeros, and a reverb holds a tail: leaving a chain out would change the float mix even where the 16-bit
  output came out the same. `ANDROID_SLES_CAPTURE` can compare outputs, but no skip here is exact by
  construction. Dropped.

*As a player launches it (2026-11-21, evening).* dArkOS starts a port from EmulationStation with
`sudo perfmax %GOVERNOR% %ROM%; nice -n -19 %ROM%; sudo perfnorm`, and `%GOVERNOR%` is `performance` here: the
CPU's, GPU's and memory's governors are all `performance` while the game runs (the GPU fixed at 520 MHz, the CPU
at 1512 MHz until the thermal governor steps it down) and back to their on-demand ones after it; ES gives up the
display for the game. None of the runs above this paragraph had that: they were started over SSH under the idle
governors. Measured the system's own way instead: test launchers in `/roms/ports` (the release's own, and one
each for the 32-bit and 64-bit bench installs, the same game data), EmulationStation started on Ports and driven
by pad events injected into the handheld's own pad (`evemu-event`), what it started checked in the process list
before any further press, the save continued and the game left by PortMaster's Select + Start hotkey the same way;
silent (the installs' audio lines, and the mixer at 0 for the session), the logger running. 120 s a run, the
log's 10 s frame rates:

| ES-launched, fps | 32-bit | 64-bit |
|---|---|---|
| King's Pass (bench installs, 32 64 64 32) | 45.2, 45.3 | 45.6, 45.7 |
| City of Tears (32 64 64 32) | 39.7, 39.2 | 37.8, 37.8 |
| the Hall of Gods room of the boss save (64 32 32 64) | 56.1, 56.2 | 55.6, 55.3 |
| the release's install (64-bit): King's Pass, City of Tears, the boss save's room | - | 57 - 58.8 on a cool handheld, 44 - 46 warm; 37.0 - 38.1; 55.3 - 56.9 |

The handheld is warm in these (76 - 78 C in a run; the thermal governor holds the CPU at 1.0 - 1.2 GHz). King's
Pass and the boss room are within half a frame either way; City of Tears is 1.6 fps (4 %) behind on 64-bit, the
GL thread's driver time on the RK3326's small L2 (above). 18 launches in all (12 of a 64-bit build, 6 of the 32-bit one), every one left by the hotkey in 2 s with
EmulationStation back, no hang; the battery went 4.17 -> 4.12 - 4.14 V at 100 % over two hours on the Mac's USB
port (95 - 217 mA out while a game runs, 30 - 120 mA in between runs). The Knulli and muOS numbers of this page
need no such correction: Knulli's launcher sets no governor (schedutil in the menu and in a game), and the muOS
runs already apply muOS's own content governor as its launcher does.

### Fragments that cannot show (2026-11-21)

`UA_GL_TRACE_FILL=1` (runtime/host/glfill.c: every draw of a traced frame made a second time with a counting copy
of its program) on Knulli, 64-bit, the game's own shader sources (`UA_SHADER_DUMP` keeps Unity's binary cache
off), five to seven traced frames a scene. The scene's 720x450 pass:

| | draws counted | fragments a pixel | discarded | unchanged (alpha 0 under blending) | covered later (written, then wholly replaced) | opaque under blending (alpha 1: the ones that cover) |
|---|---|---|---|---|---|---|
| King's Pass | 46 | 6.9 | 7.3 % | 16.6 % | 7.4 % | 5.4 % |
| City of Tears | 45 | 7.4 | 6.1 % | 17.9 % | 8.7 % | 3.0 % |
| Hall of Gods boss (two frames of the fight) | 68, 85 | 14.8, 10.7 | 10.4, 16.8 % | 30.6, 38.7 % | 13.0, 4.7 % | 5.7, 3.3 % |

(27 of 87 draws at King's Pass, 85 of 156 in City of Tears and 51 - 55 of 142 - 160 in the fight were not
counted: the tool could not make a counting copy of their programs; their fragments are in none of the columns.
The window's own 66 - 71 % "covered later" is the letterbox quads under the final copy.)

Of the counted fragments 5 - 13 % are hidden behind a later opaque one, and the fragments that hide them are 3 -
6 % of the pass. A depth pre-pass would have to run the alpha of every fragment of the covering draws' programs
once more (those programs shade 8 - 18 % of the frame) to save that 5 - 13 %: nothing left over. Not built. What
the boss arena has instead is alpha-0 fragments: 31 - 39 % of its pass.

**Discarding the fragments that write nothing: exact, and slower.** A fragment whose output is exactly vec4(0)
leaves its pixel as it was under the scene's three blend states, so a shader may discard it. Measured where the
share is highest, by replay: the boss scene's GL stream recorded with its texture pixels (`UA_GL_REC_FILE`,
`UA_GL_REC_FILE_PIXELS=1`, 3,251 frames) and replayed against libmali on Knulli (`glreplay`, `glFinish` after
each frame, the last 300 frames, 114 draws a frame), plain and with `glreplay --zero-discard` (new, a
measurement: each fragment shader with the one output `SV_Target0` gets a `main()` that runs the game's and
discards on an all-zero output; 36 shaders), A B B A: 33.97, 34.63 ms a frame against 36.62, 36.82 ms with the
discards (+7 %), the calling thread's CPU the same (17.0 - 17.5 ms). The pictures are the same (`--hash`, the
last 150 frames, 2,551 readbacks at every pass's end: 0 differ). On the Mali-G31 a shader that can discard costs
every fragment it shades more than the blend and tile write its discards save. Dropped. (Of the census above:
why a third to half of the draws had no counting copy was not found; no failure is logged, so their programs
never passed the tool's `glShaderSource`.)

### Memory, by mapping (2026-11-21)

The same installs on muOS (no swap), the 64-bit one with the Mono the ports ship (8-byte granules, the managed
allocator: the handhelds' `lib-gG8` copies still carried the build before the allocator, which the frame-rate
census above ran with; it changes no frame rate), 1 MB allocator blocks, `UA_MEM_LEDGER=1`, the game's
`/proc/<pid>/smaps` taken after the plan's last step, one run each. Resident MB:

| | King's Pass 32 | King's Pass 64 | boss 32 | boss 64 |
|---|---|---|---|---|
| game resident at the end of the run / MemAvailable | 686 / 239 | 707 - 709 / 250 | 721 / 207 | 732 / 184 |
| anonymous + brk (cannot be reclaimed) | 476 | 460 | 486 | 506 |
| - Unity's allocators (libunity's mappings) | 293 | 231 | 297 | 271 |
| - Mono's collector heap (mapped; reachable after a full collection) | 112 (69) | 144 (93) | 112 (61) | 144 (82) |
| - malloc (in use in the arenas + mmapped chunks) | 51 + 22 | 71 + 3 | 55 + 22 | 75 + 4 |
| file pages (reclaimable): assemblies | (in malloc: read from the APK, 22) | 20 - 21 | (22) | 22 |
| file pages: libmali / libunity / libmono text, the rest | 11.5 / 18.6 / 6.8, 10 | 23.3 / 18.7 / 3.5, 15 | 11.5 / 18.6 / 6.8, 10 | 4.0 / 8.2 / 3.2, 7 |

(The ledger run of the 32-bit build holds a 16 MB table of its own, taken off. The driver's `/dev/mali0` mappings
are not in a process's resident count on either build. File pages resident at a snapshot depend on what the page
cache still holds: the 64-bit libmali's 23 against 4 MB are the same code in two runs.)

- **The gap is 11 - 22 MB resident, and at King's Pass none of it is memory the kernel cannot take back**: the
  64-bit build's anonymous memory is 16 MB *lower* there (Unity's allocators -62 MB with 1 MB blocks against
  Mono's heap +32 and malloc +20, the assemblies' 22 MB apart), and its assemblies are file pages where the 32-bit
  player holds them as heap blocks (it reads them out of the APK). MemAvailable follows: 250 against 239 MB. At the
  boss (two scenes later) Unity's allocators have grown 40 MB on 64-bit against 4 on 32-bit (larger blocks, more of
  them partly used) and the anonymous total is 20 MB higher.
- **Mono's heap, +32 MB**: the same 1.3 - 1.5 M objects take 82 - 93 MB instead of 61 - 69 (pointers), the heap
  grows in 16 MB steps and `UA_GC_BUDGET`'s 32 MB sits on top on both; the 64-bit build takes three budget
  collections on the way to King's Pass where the 32-bit one takes two. Smaller steps were measured before
  (140 instead of 144 MB: not taken).
- **malloc, +20 MB**: in the 32-bit player Mono's own blocks are 41.7 MB (`UA_MEM_LEDGER=2`), 13 MB of them
  assemblies, 19 MB mempool chunks (2,472 blocks of 4 - 16 KB), 4.6 MB in 263 K blocks of 64 bytes or less;
  libunity's 12 MB are assemblies but 1 MB. On 64-bit the same metadata is pointers for the most part (Mono's
  counters: `MonoClass` 3.2 -> 6.7 MB, inflated classes 1.8 -> 3.2, inflated methods 1.1 -> 1.7, `MonoMethod` 0.8
  -> 1.2): about 28 -> 48 MB by the arena totals (an estimate: the ledger does not follow our glibc-built Mono). glibc's settings are in effect process-wide (`mallopt` before any thread starts:
  2 arenas, 4 MB free in them, 2 - 4 MB in 3 - 7 mmapped chunks), so none of the +20 is freed memory kept.
- **`UA_MONO64_ASSEMBLIES=scripting`** (King's Pass, one run against one): resident 707 -> 703 MB, MemAvailable
  250 -> 258, the assemblies' mapping 33.8 -> 25.7 MB, malloc 73 -> 71 MB. Exact for a game that was a Mono player
  (it loads what Unity's Mono player loads); before it is a default each game has to be started, loaded and saved
  with it once, since code that looks a type up by name in a class library not yet loaded, or walks
  `AppDomain.GetAssemblies()`, sees the Mono player's list instead of IL2CPP's. Left opt-in: 4 - 8 MB.

Nothing here is worth a patch today: the two terms that are the 64-bit build's own (Mono's heap and metadata,
+50 MB) are pointer width, and Unity's 1 MB blocks already pay them back at King's Pass.

## The BattleXP G350 (RK3326): the min-spec handheld, 32-bit against 64-bit (2026-11-20)

The G350 (dArkOS, RK3326: four Cortex-A35 cores up to 1.512 GHz, Mali-G31 with r13p0 GBM libmali for each
userland, 895 MB, no swap) is the handheld this toolkit has to be fast enough on. Hollow Knight at King's
Pass, the port's 1 GB settings (`UA_PRESENT_THREAD=1`, `UA_GL_REC=auto`: Unity's GL replayed on ua_host's GL
thread, one job worker), the same 32-bit and 64-bit installs as on the RG34XX-SPs (the 64-bit one with
6466ebf's runtime: 8-byte granules and the managed allocator), EmulationStation stopped
(`systemctl stop emulationstation`: profbench's `STOP_ES` knows only the init scripts of Knulli and muOS),
`tools/device/profbench.sh` 25 s from a cool handheld, `HWCOUNT=1` on UnityMain, Unity's render thread and
the GL thread. Per-thread CPU from `/proc/<pid>/task/*/stat` (this 4.4 kernel has no schedstat).

| run | build | fps | UnityMain | render thread | GL thread | job worker | FMOD threads | lowest available | CPU clock (avg of samples) |
|---|---|---|---|---|---|---|---|---|---|
| y32a | 32-bit | 39.3 | 24.4 ms | 8.3 | 12.5 | 8.7 | 11.7 | 263 MB | 1377 MHz |
| y64a | 64-bit | 37.1 | 25.3 | 8.7 | 15.4 | 9.2 | 11.3 | 221 MB* | 1266 |
| y32b | 32-bit | 38.8 | 24.5 | 8.4 | 12.6 | 8.8 | 12.1 | 266 MB | 1338 |
| y64b | 64-bit | 36.6 | 25.8 | 8.9 | 16.4 | 9.4 | 11.8 | 225 MB* | 1293 |
| m32 | 32-bit (40 s) | 39.7 | 24.2 | 8.0 | 12.1 | 8.7 | 11.9 | 263 MB | 1271 |
| m64 | 64-bit (40 s) | 37.5 | 25.4 | 8.4 | 15.7 | 9.2 | 11.7 | 219 MB* | 1191 |
| b64a | 64-bit | 36.3 | 25.8 | 8.8 | 15.9 | 9.5 | 12.1 | 179 MB | 1219 |
| a64n | 64-bit | 38.4 | 24.6 | 8.4 | 15.4 | 9.1 | 10.6 | 179 MB | 1422 |

(ms of CPU a frame; \* runs in which the two CJK fonts of `UA_UNLOAD_ASSETS` stayed given back, about 40 -
45 MB: like for like, with the fonts held as every 32-bit run holds them, the 64-bit build leaves 179 -
180 MB against 263 - 266.) The gap: 2.0 - 2.2 fps (5 - 6 %) and about 85 MB.

**The counters** (a frame's worth, two runs of each build):

| a frame | UnityMain 32 / 64 | render thread 32 / 64 | GL thread 32 / 64 |
|---|---|---|---|
| cycles (user) | 28.1, 27.0 / 29.6, 28.9 M | 8.2 / 9.3 M | 13.5, 13.3 / 17.0, 17.1 M |
| instructions | 5.49, 5.10 / 4.62, 4.87 M | 1.98 / 2.10 M | 2.35, 2.33 / 2.27, 2.24 M |
| L1I accesses | 6.1 / 6.1 M | 2.2 / 2.7 M | 2.57 / 3.10 M |
| L2 refills | 130 / 148 K | 35 / 39 K | 66 / 91 K |
| cycles stalled on an i-cache miss | 7.2, 6.9 / 8.2, 7.9 M | 2.5 / 3.2 M | 5.5, 5.4 / 8.7, 8.8 M |
| cycles stalled on a load miss | 11.3, 11.1 / 12.5, 12.7 M | 2.7 / 2.8 M | 4.0, 3.8 / 4.7, 4.7 M |

- **The GL thread is most of the G350's gap, and all of it is instruction fetch.** The 64-bit libmali
  (40.9 MB file, r13p0 like the 32-bit one's 30.2 MB) runs the same frame in *fewer* instructions, but its
  code takes 22 % more L1I accesses and refills and 38 % more L2 refills (code from DRAM), and the
  i-cache stall cycles grow by 3.3 M of the 3.6 M extra cycles. The A35's L2 is a quarter of the H700's
  (RK3326: 256 KB shared by the four cores; the H700's is 1 MB), so a 64-bit driver whose sampled hot code
  spreads over 182 pages (32-bit 150) misses it far more than on the RG34XX-SP (where the render thread's
  i-cache stalls grew 4.7 -> 7.0 M). By GL call (`UA_GL_REC=time`, the same scene, a frame): every state call is
  as fast or faster on 64-bit (`glVertexAttribPointer` 2.7 -> 2.1 us, `glUniform4fv` 3.7 -> 2.9,
  `glMapBufferRange` 1.9 -> 1.2); the loss is in the calls that run the driver's big paths:
  `glDrawElements` 88.6 -> 104.8 us (87 a frame: +1.4 ms), `glBindFramebuffer` (a pass's flush) 249 -> 286 us
  (15: +0.6 ms), `glInvalidateFramebuffer`, `glClear`. The same 1,880 calls a frame on both builds.
- **UnityMain** +1.2 - 1.5 M cycles, half instruction fetch, half load misses (L2 refills 130 -> 148 K):
  the RG34XX-SP's picture on a smaller L2 (routes 1 - 3 above apply unchanged).
- **The clock.** dArkOS's thermal governor is `power_allocator` (sustainable 750 mW, control at 85 C), and
  25 s of King's Pass take the SoC to 76 - 80 C: the CPU's clock is whatever its share of the power budget
  buys, 1008 - 1512 MHz changing several times a second. The 64-bit runs average 6 - 8 % less (1191 - 1293
  against 1271 - 1377 MHz): the same budget spread over more busy cycles and more DRAM traffic. On this
  handheld a cycle saved on *any* thread (the GL thread, the mixer) is clock for UnityMain, not only a
  cycle saved on the frame's critical path; the per-second fps does not follow the per-second clock
  sample (the governor moves faster than a second), so run-to-run spread is 1 - 2 fps and A/B pairs need
  several runs.
- **Not it**: the profiles (`UA_PROF=200` on every busy thread, both builds) show the same shares by module
  except the driver's; the futex `syscall` samples on the job worker (32 - 35 % of its on-CPU samples) and the
  render thread (23 %) are threads woken and waiting for a core (their kernel time is 0.6 - 0.8 s of 25 s),
  the same on both builds. Leaving out repeated GL state (`UA_GL_STATE=1`): 36.7, 37.0 fps against 36.3
  (it acts on the render thread's calls only, not the GL thread's replay). FMOD's update on a worker
  (`UA_FMOD_ASYNC=1`, route 1): UnityMain -1.5 M cycles a frame, 38.3 fps against 38.4 in the same hold
  (the 25 s runs' spread). UnityMain on a core of its own (`UA_AFFINITY='UnityMain:3 *:0-2'`): 38.8; with
  both 38.3; `UnityMain:3 ua_glthread:2 *:0-1` 38.0 (another hold): within the spread.

**What is left for the G350**: the libmali code footprint, the one cost the RG34XX-SPs barely see; its
relayout was tried and stopped (above, "libmali's hot code relaid out"), so the GL thread's +3.3 M stall
cycles stay the driver's. Routes 1 - 3 on libunity apply as on the RG34XX-SPs (`UA_PREFETCH`, the relayout:
their G350 rows above). Memory: Unity's 1 MB allocator blocks (now the 64-bit default,
[mono-arm64.md](mono-arm64.md#unitys-allocator-blocks-the-largest-memory-lever-both-builds)) give the G350
+30 MB like for like (m64k 209 MB against b64a 179 MB, fonts held).

**A hang** (2026-11-20 20:34 device time): the G350 stopped answering (USB link up, no ARP) 30 s into a
plain 64-bit run, four seconds after the title screen's first frames on the GL thread (60 fps, 481 MB
available, nothing loading); the last log lines are `[glrec] thread: the GL thread replays the frames
...`, the first title frames and `[dump] UA_UNLOAD_ASSETS: none of them loaded at 21 s`. A power cycle
brought it back. The game's player prefs were being written at that moment and came back empty: that is
how the 64-bit install's prefs had lost `GameLangSet` earlier that day (its runs then stopped at the
language screen). The G350 has hung on the 32-bit build as well (2026-11-08 after a long series of runs,
2026-11-09 in City of Tears with 126 MB held), so this is not yet known to be the 64-bit build's.
A second hang the same evening came in a 64-bit room tour (1 MB allocator blocks, about 7 min in) run
with ua_host's GL flight recorder (`UA_GL_REC_BLACKBOX`, a 4 KB record rewritten and synced before each
submit and around each swap): its last record is frame 3067, stage `pass`, inside `glBindFramebuffer` (the
driver's submit of a pass), with the game at 665 MB resident (the stretch at 18 fps with 197 ms hitches
before it is the recorder's own: its synced writes at level 2 halve the G350's frame rate, King's Pass 40.5
fps on the 32-bit build against 19.4 on the 64-bit one with it on: a tool for hang hunting only, never for frame rates). So that hang was inside libmali / the
GPU's kernel side during a pass submit, not in ua_host's threads; the same build passed the tour's other
three runs (16 MB blocks twice, 1 MB once) and a 90 s King's Pass run with the recorder on.
