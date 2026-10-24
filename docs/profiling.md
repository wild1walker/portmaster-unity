# Profiling on the handheld

Where a game's frame goes, measured on the device itself. The tools, from the cheapest look to a
whole measured session:

| question | tool |
|---|---|
| how fast, how steady | `UA_FPS=<s>` (on by default every 5 s): fps, average / p99 / worst frame, frames over 25 and 50 ms, Mono collections ([runtime.md](runtime.md)) |
| what one slow frame did | `UA_HITCH_TRACE=<ms>` + `tools/analysis/hitchsum.py`: samples inside frames longer than `<ms>` |
| where the steady frame goes | `UA_PROF=<hz>` + `tools/analysis/profsum.py`: a sampling profile of UnityMain and the render thread (below) |
| the same, measured and compared across settings | `tools/device/profbench.sh` + `tools/analysis/benchsum.py`: one run per setting with frame rate, clocks, CPU, memory and the profile (below) |
| what a thread waits in right now | `python3 tools/device/stacks.py <pid> [thread]` on the device, or `kill -s RTMIN+7 <pid>` (every thread's stack into the log) |
| CPU per thread, clocks, temperatures while playing | `python3 tools/device/profcpu.py <pid>` on the device |
| what the GPU does, pass by pass, and what each pass costs | `UA_GL_TRACE=<s>` (+ `UA_GL_TRACE_DRAWS`, `UA_SHADER_DUMP`): below, [GPU, pass by pass](#gpu-pass-by-pass) |
| how many of the sprites' fragments sample only transparent texels | `tools/device/deadfrags.py` + `UA_SHADER_OVERRIDE`: [below](#sprites-transparent-texels-kings-pass-2026-10-23) |
| which of the GPU's units a frame or a shader keeps busy (arithmetic, texturing, varyings, tile writes, memory) | `runtime/host/gpucnt` (the Mali's hardware counters; `GPUCNT=1` in a bench run): [below](#gpu-counters-gpucnt) |
| what a pass's loads and stores cost the GPU itself | `runtime/host/passbench` on the device: [below](#what-a-pass-costs-on-the-mali-g31-passbench) |
| which passes, loads and stores a frame could do without | `tools/analysis/framegraph.py` on a `UA_GL_TRACE_DRAWS` log; the GL calls a frame makes, by entry point: `UA_GL_REC=census` ([holding the whole frame](#holding-the-whole-frame-ua_gl_rec)) |
| whether the GPU ever waits for work inside the frames | `UA_GPU_IDLE=<s>` ([runtime.md](runtime.md); below, [the handheld around the game](#the-handheld-around-the-game-2026-10-23)) |

## UA_PROF: the sampling profiler

`runtime/host/prof.c`, off unless `UA_PROF` is set:

| variable | effect |
|---|---|
| `UA_PROF=<hz>` | samples per second and thread while a recording runs (250 - 1000 is useful; 500 gives 10,000 samples in 20 s) |
| `UA_PROF_THREADS=a,b*` | threads by name, a trailing `*` matches a prefix, `@swap` the thread that presents the frames (default `UnityMain,UnityGfxDeviceW`: Unity's render thread is `UnityGfxDeviceWorker`, cut to 15 characters; its job threads are `Job.Worker <n>`: `Job.Worker*`; FMOD's mixer `FMOD*`). `UnityMain` is the thread that runs Unity's frames: the threads it starts carry its name until they set their own, and are left out |
| `UA_PROF_SECS=<s>` | length of a recording (20) |
| `UA_PROF_START=<s>` | one recording `<s>` s after the first frame (default: none until `prof.req`) |
| `touch <root>/prof.req` | start a recording now (looked for every second; a number in it is the length) |
| `UA_PROF_DEPTH=<n>`, `UA_PROF_SCAN=<bytes>` | return addresses kept per sample (32, at most 128) and how much of the stack is scanned for them (16384) |

Each recording becomes `<root>/prof-<n>.txt` and a log line such as `[prof] prof-1.txt: 20.0 s,
UnityMain 10000 samples (running 96%, waiting for a CPU 1%, blocked 3%), ..., 640 frames (32.0 fps)`.

How it samples. A sampler thread (`ua_prof`) wakes `<hz>` times a second and reads each thread's
state from `/proc/self/task/<tid>/stat`:

- **running**: a signal (SIGRTMIN+5) makes the thread record itself in its signal handler, into a
  buffer allocated for the recording: pc, lr and its callers. The callers are unwound exactly as far
  as the code has unwind tables: on 32-bit ARM the EHABI opcodes of `.ARM.exidx`/`.ARM.extab`
  (libunity, Mono's runtime, libc and the GLES driver have them), on AArch64 the frame-pointer
  chain. Mono's JIT code has no such tables: from the first managed frame on, the stack is
  scanned for words that are return addresses (the instruction before them is a call; ARM, Thumb
  and A64 forms), and on 32-bit ARM the unwind starts again, exactly, at the first return address
  into code with tables that was reached by an indirect call (`BLX <register>`: how Mono's runtime
  and Unity's scripting calls enter JIT code; the callee pushed lr at the top of its frame, so the
  caller's sp is the next word). The engine's frames below the scripts are thus unwound too: only
  the managed frames themselves come from the scan, which stops at the thread's start. The handler
  names nothing, takes no lock, allocates nothing, and reads the stack only inside the thread's
  stack mapping. A thread whose previous signal is still pending is counted as **Q**: runnable but
  not on a CPU, or in a long kernel path;
- **blocked**: the thread is left alone (it is not woken, its wait does not return early):
  `/proc/self/task/<tid>/syscall` gives the call it waits in with its sp and pc, and the sampler
  scans its stack from there. Under QEMU that file shows the emulator's registers, so blocked
  samples in the emulated rig carry only the state.

When the recording ends the sampler names the addresses, outside any signal handler: Android
libraries as `module+offset` by the loader's load bias (ELF addresses: the offline tools name them
from Unity's symbol files), glibc libraries (libc, the GLES driver) by `dladdr`'s nearest exported
symbol, and Mono's JIT code by method (`mono_jit_info_table_find` + `mono_method_full_name`, attached
to Mono's root domain meanwhile; the Android players' Mono exports both). It adds each thread's CPU
time over the recording (`schedstat`: on a CPU and waiting for one; utime, stime), the cores'
`/proc/stat` counters and the frame times (every `eglSwapBuffers` and every UnityMain frame), so the
profile can be read against the frame rate.

Cost. Memory: (9 + depth) words a sample, `hz x secs` samples a thread, allocated when a recording
starts and freed when it is written: 500 Hz, 20 s, depth 32 is 1.6 MB a thread. Time: a running
sample is a signal, an unwind through the tables and a scan of at most 16 KB of stack against a
table of the code mappings (a bitmap first, then a binary search); a blocked one is two `pread`s
on the sampler's own core. `runtime/host/test/prof_test.sh` measures what a sampled thread loses
(`test_prof spin`: on an arm64 PC, natively, nothing measurable at 500 or 1000 Hz, within the
2% run-to-run noise); on the device compare `UA_PROF` runs with runs without it (the bench does)
before reading small differences. Nothing runs, and nothing is allocated, while no recording is on.

Limits. The managed frames are a scan, so a stale return address can appear among them, and a
wrong re-start of the unwind is possible ("on the stack" is an upper bound; `profsum.py --exact`
counts only unwound callers, the profile marks scanned ones `~`, and the summary says how many
samples were unwound and how deep). ua_host and the fake JNI are built with `-funwind-tables`, so
the unwind goes on below Unity's `nativeRender` to the thread's start. A sample taken in a
function's prologue or epilogue can stop the unwind early (the tables describe the body). A sample
in a function without tables (`EXIDX_CANTUNWIND`: glibc's assembly, `syscall`, `memcpy`) is scanned
from its stack pointer, lr naming the caller (until 2026-10-23 such a sample was taken for the
thread's start and kept no callers: a fifth of Hollow Knight's UnityMain samples, every futex wake
among them; the restarts this brought found a stack check that a popped garbage stack pointer
could wrap, now closed). Blocked samples have
only sp and pc (from `/proc`): their callers are always a scan. Mono's trampolines and code it has
no information for are "anonymous code". Libraries the profile names only by exported symbols (a
stripped libc or GLES driver) are named by the nearest exported function, which is often not the
function itself.

Known issue (RG34XX-SP, Hollow Knight, 2026-10-22): with `UA_SWAP_INTERVAL=0 UA_FRAME_RATE=200`
and `UA_PROF` together, the game ended twice: once a scan read past the render thread's stack after
an unwind through Mali's driver (fixed: an unwound stack pointer off the thread's stack ends the
scan), once with glibc's heap check failing after the recording (fixed: the writer's address list
counted pc, lr and the callers but also copied the scanned-frame bitmap, and overran its buffer;
it showed with vsync on too, in the 64-bit build).

## profsum.py

```sh
python3 tools/analysis/profsum.py prof-1.txt --syms <Unity player>/Variations/mono/Release/Symbols/armeabi-v7a
```

(`pmunity player fetch <version> --symbols` downloads Unity's symbol files; `--lib runtime/host/out/ua_host`
names ua_host's own functions.) For each sampled thread:

- **schedstat** and **state**: the share of the recording on a CPU, waiting for one (a thread that
  waits a lot for a CPU competes with other threads or processes: the bench's "other processes"),
  blocked and in which call (`futex` from a libunity function: waiting for another thread; `ioctl`
  from the GLES driver: the GPU);
- **on a CPU**: how many of the samples were unwound (and the average number of unwound
  callers), then **by module**: Unity's engine, Mono's runtime (GC, JIT, the wrappers that call
  into the engine), managed code (JIT-compiled methods), libc, the GLES driver, ua_host;
- **self**: the function the pc was in; **on the stack**: every function in the sample, once (the
  callers of hot code add up here: the engine's update loop, a camera's render, Physics2D);
- **managed code itself** and **by innermost managed method**: the time of each JIT-compiled method,
  and that method plus the engine and runtime calls it makes (the innermost managed frame of each
  sample): the game's scripts by cost;
- **blocked: who waits**: the call and the first frame outside libc.

`--exact` uses only the unwound callers for "on the stack", the managed methods and `--callers`;
`--frames` gives UnityMain's time on a CPU per frame (a frame that needs more than 16.7 ms misses
the vsync), `--slow 20` limits the tables to samples in frames longer than 20 ms, `--callers
<text>` lists the callers of the functions matching it, `--offsets` keeps offsets in method names.
`--areas` adds UnityMain's frame by area of Unity's player loop, in ms a frame: rendering by camera
stage (offscreen cameras, the cameras' sorting, batching and draw calls, culling, image effects),
scripts by callback (Update, FixedUpdate, LateUpdate, coroutines, OnGUI), physics, animation,
particles, audio, UI; the waits (for the previous frame's present, i.e. the render thread or the
GPU, and for the job threads); and, across those areas, the futex and clock system calls, managed
code, Mono's runtime, memory management, the job system and Physics2D queries. A sample's area is
the first rule one of its unwound frames matches (`AREAS` in profsum.py; the scanned frames when
none does).

## The device bench

```sh
DEVICE=root@<handheld> SSH_OPTS="-o ControlPath=..." tools/device/profbench.sh <game folder on the handheld> <name> [VAR=value ...]
DEVICE=root@<handheld> tools/device/profbench.sh <game folder on the handheld> --list <file>
```

The game folder is one `tools/device/test.sh` sent before (lib/, APK, OBB, android_package.json,
saves in data/); only `<game folder>/bench/` is written: this checkout's ua_host and
`tools/device/profrun.sh`, and one folder per run. Each run, detached on the handheld:

1. stops EmulationStation (`STOP_ES=0`: leave it), starts the game silent (SDL's dummy audio
   driver, always; `UA_AUDIO` is dropped) with `UA_FPS=1`, and a memory watchdog as uatest.sh's;
2. follows `PLAN` to the point to measure: steps separated by `;`: `log:<regex>` (a new log line
   matches, after what the last step matched), `seen:<regex>` (the game logged such a line at any
   time), `steady:<n>` (n one-second windows in a row without a frame over 50 ms), `hitch:<ms>`
   (a frame of at least `<ms>`: a scene load), `sleep:<s>`, `press:<presses>` (UA_INPUT_SCRIPT's
   syntax, through `input.req`), `shot`;
3. measures for `MEASURE` seconds (30): the frame rate every second; raw copies of `/proc` and `/sys`
   counters at the start and the end (per-core and per-thread CPU time, memory and swap traffic,
   cpufreq `time_in_state`, devfreq `trans_stat` (the GPU's clock residency, and the memory
   controller's where it has one), governors, temperatures, cooling states, every process's CPU
   time); each core's and the GPU's clock, every thermal zone's temperature and each cooling
   device's state every second (`freq.log`; benchsum.py's `per second:` line: each zone's range and
   average, the seconds at each GPU clock); with `UA_PROF=<hz>` a profile of the same seconds;
4. stops the game, starts EmulationStation again and writes `result` (`ok`, or where it stopped).

Clocks, governors and thermal settings are read, never written. The results come back to
`$OUT/<name>/` (default `./profbench`), and `tools/analysis/benchsum.py` sums each run up and, with
several, compares them with the first. A list file holds one run per line, `<name> [VAR=value ...]`
in shell quoting, `#` for comments; `PLAN` and `MEASURE` can be set for all runs in the environment.

Reading A/B runs: the handheld drifts (temperature, zram, uptime: Cuphead's numbers fell ~10% over
a long night), so run the baseline first and last and compare runs next to each other; differences
of a few percent need repeats. Settings that change how the game looks or plays are not A/B
candidates for a default ([CONTRIBUTING.md](../CONTRIBUTING.md)); a run without vsync is a
measurement (what the frame costs when it is not paced), not a setting.

## Worked example: Hollow Knight, King's Pass (Unity 6000.0, Mono)

On the RG34XX-SP the game ran King's Pass at 32 - 34 fps (2026-10-22; 33 - 37 with the host's flips:
[what limits it](#what-limits-kings-pass-32-bit-2026-10-23)). The saved game continues from the title
screen with Start Game and the save slot (Switch layout: `b` confirms); the title and the save load
are frames of several seconds, and memory is tight while the save loads (`UA_MEM_MIN=20`):

```sh
export DEVICE=root@<handheld> UA_PAD_NAME='Nintendo Switch Pro Controller' UA_MEM_MIN=20 MEASURE=25
export PLAN='log:find GameCameras; hitch:2500; steady:3; sleep:2; press:0:b; sleep:6; press:0:b; sleep:6; press:0:b; hitch:2000; steady:5; shot'
tools/device/profbench.sh /userdata/uatest/hollowknight --list hk-session.txt
```

(the third press lands while the save loads when the second one already started it; the `shot`
step's screenshot shows that the measurement is King's Pass and not the menu).

`hk-session.txt`, about two and a half minutes a run:

```
# baseline, and the profiler's own cost
base
prof      UA_PROF=500 MEASURE=20
# where the frame goes, including the job threads
prof-all  UA_PROF=250 MEASURE=20 UA_PROF_THREADS='UnityMain,UnityGfxDeviceW,Job.Worker*,FMOD*'
# what the frame costs without the vsync wait (a measurement: tears). Mali's fbdev driver flips at
# the vblank whatever the swap interval: UA_FB_FLIP=mailbox there; UA_SWAP_INTERVAL=0 on SDL displays
novsync   UA_FB_FLIP=mailbox UA_FRAME_RATE=200
novsync-prof UA_FB_FLIP=mailbox UA_FRAME_RATE=200 UA_PROF=500 MEASURE=20
# Mono's JIT: optimizations that keep the program's meaning (no 'unsafe', float32 untouched)
mono-abcrem UA_MONO_OPTIONS=-O=abcrem
mono-ssapre UA_MONO_OPTIONS=-O=abcrem,ssapre
# Unity's job threads and render thread (boot.config switches as command-line arguments)
jobs-2    UA_UNITY_ARGS='-job-worker-count 2'
jobs-1    UA_UNITY_ARGS='-job-worker-count 1'
gfx-st    UA_UNITY_ARGS=-force-gfx-st
# Mono's collector: fewer, larger collections (hitches, not the steady frame rate)
gc-64     UA_GC_BUDGET=64
# FMOD's output reported with 1024 frames a buffer (Unity 6000.0's FMOD then takes its AudioTrack
# output at 24 kHz: another sound, a measurement)
audio-1024     UA_AUDIO_FRAMES=1024
base-end
```

What each answers:

| run | reading |
|---|---|
| `base`, `base-end` | the frame rate before and after; their difference is the drift to allow for |
| `prof` | the profile (`benchsum.py --syms ...` prints it); fps against `base` is the profiler's cost |
| `prof-all` | whether the job threads have work (the engine's culling, sorting, sprite and particle jobs) or idle while UnityMain works alone |
| `novsync` | the real frame cost: 40 fps unpaced means 25 ms of work, short of 16.7 by 8 ms; 55 means a little work away from 60 |
| `novsync-prof` | the same with the profile, and `--frames`: UnityMain's time per frame |
| `mono-*` | whether JIT code quality matters (only if managed code is a large share of the profile); the log's `[host] mono options` line confirms they were applied |
| `jobs-*`, `gfx-st` | whether fewer threads compete with UnityMain on the four cores (schedstat's wait for a CPU) and what the render thread's hand-off costs; `JobSystem: Creating JobQueue using job-worker-count value <n>` in the log confirms the job count was taken (it is, in the emulated rig: boot.config switches work as `UA_UNITY_ARGS`), and a run without a `UnityGfxDeviceW` thread confirms `-force-gfx-st` |
| `gc-64` | collections and their pauses (`gc` in the fps lines); memory: available MB at the end |
| `audio-1024` | what FMOD costs with another output: in Hollow Knight (6000.0) it is not a longer buffer but FMOD's AudioTrack output at 24 kHz (its `FMODAudioDevice` thread 38 % of a core against the mixer's 56 - 57 % at 48 kHz, 2026-10-24) |

Read the profile against what the scene holds: the number of active sprite renderers, cameras,
behaviours with `Update`/`FixedUpdate` (PlayMaker FSMs among them), 2D colliders and rigidbodies,
and the project's fixed timestep (a 50 Hz fixed update runs 1.6 times in a 32 fps frame and less
than once in a 60 fps one). UnityPy reads those from the game's own files (the level file of the
scene and globalgamemanagers); keep such numbers out of this repository like any other game data.

### What limits King's Pass (32-bit, 2026-10-23)

With today's runtime (`UA_GL_DSCLEAR`, host flips) King's Pass runs 36.6 fps on a cool handheld and
33.3 fps once it is warm, and the difference is the GPU's clock, not the main thread:

- **The frame rate follows the Mali's clock.** At 696 MHz (simple_ondemand keeps it there: the GPU
  is busy past the governor's threshold) the scene runs 36.6 fps; when the SoC reaches about 68 C the
  thermal governor caps the GPU at 600 MHz (`thermal-devfreq-0` cooling state 1, `freq.log`'s
  `gpu=600`), after about 20 s of play from a cool start, and the frame rate settles at 33.3 fps (a
  150 s run: 33.1 - 33.4 in every 15 s window, the GPU at 600 MHz 95 % of the time; the CPUs are
  capped at 1416 - 1512 MHz too). Bench runs of 25 s therefore drift with the session's heat: in one
  session the same setting measured 36.6 and 35.0 fps, the GPU at 696 MHz 100 % and 60 % of the
  time. Compare runs only with their `gpu=` residency, or as below without the GPU.
- **UnityMain waits for the GPU.** In the normal frame (30 ms at 33 fps) UnityMain waits 3.4 ms in
  `GfxDeviceClient::WaitForPendingPresent` (the render thread has not presented the previous frame),
  and the render thread spends about 11 ms of its frame blocked inside Mali's driver, in the draws
  of the grab passes' copies and the first draws after a framebuffer change.
- **Without the GPU's work the main thread makes 49 fps.** A measurement, not a setting: every
  fragment shader the scene compiles replaced by one that writes a constant colour (a run with
  `UA_SHADER_DUMP=<dir>`, then `tools/device/flatshaders.py <dir> <flat dir>` and runs with
  `UA_SHADER_OVERRIDE=<flat dir>`, a folder outside `bench/`: profrun.sh empties `bench/<run name>`;
  the picture is grey, the draws, passes, blending and every CPU-side step stay). The GPU then idles at 240 MHz and the
  same scene runs **49.1 - 49.6 fps** with UnityMain 95 % on a CPU, 19.3 ms of CPU a frame (24 - 25.5
  ms in the normal frame: the same work, slower; with more fixed steps a frame at 33 fps, the DRAM
  the GPU keeps busy and the warmer CPU). This is the way to measure a main-thread change on this
  scene: with the GPU in the loop its clock decides the result.

UnityMain's frame by area (`profsum.py --areas`, 500 Hz; the profiler costs about 4 %), normal and
with the GPU's work left out:

| ms a frame | normal (33.3 fps, GPU 600 MHz) | without the GPU's work (47.4 fps) |
|---|---|---|
| render: the cameras (sorting, batching, draw calls of the sprites) | 5.6 | 4.7 |
| render: offscreen camera (the blurred background) | 2.7 | 1.8 |
| render: culling | 1.2 | 1.1 |
| render: image effects (OnRenderImage blits), other | 1.2 | 1.0 |
| scripts: Update (PlayMaker FSMs most of it) | 3.9 | 3.1 |
| scripts: FixedUpdate, LateUpdate, coroutines, OnGUI | 1.9 | 1.1 |
| animation (Animator, Director) | 1.6 | 1.2 |
| physics: 2D step (50 Hz: 1.5 steps a frame at 33 fps, 1.06 at 47) | 1.2 | 0.8 |
| audio (FMOD's update on UnityMain) | 1.3 | 0.9 |
| particles, UI, input, transforms, other player loop | 2.4 | 1.5 |
| not attributed (unwind stopped) | 2.2 | 1.8 |
| wait: the previous frame's present (render thread, GPU) | 3.4 | 0 |
| wait: job threads, other | 0.6 | 0.5 |
| **frame** | **30.0** | **21.1** |
| across the areas: managed code (JIT) | 4.0 | 3.0 |
| job system (scheduling and waking the workers: 1.1 ms of futex wakes) | 3.3 | 2.8 |
| Mono's runtime | 1.7 | 1.3 |
| memory management (Unity's allocators) | 1.5 | 1.2 |
| futex system calls | 1.5 | 1.2 |
| Physics2D queries | 0.9 | 0.7 |
| clock_gettime / gettimeofday system calls | 0.2 | 0.3 |

No single area is large: rendering is a third, the scripts a quarter (managed code 3 - 4 ms, the
rest the engine and Mono calls they make), the job system's wake-ups of its mostly idle workers
about 1 ms. What the runtime and the converted data make Unity do that an Android device would not,
looked for and measured (A/B in one session, each value one run):

| change (layer) | result |
|---|---|
| `clock_gettime` without a system call: the kernel gives 32-bit processes no vDSO, about 6,000 calls a second at 1.1 us each (`UA_FAST_CLOCK=1`, runtime.md) | UnityMain's kernel time -35 to -40 %; without the GPU's work 49.1 / 49.2 fps, without the change 49.2 / 49.1: no gain that can be measured. Option, off |
| Unity's `Process.setThreadPriority` calls applied (render thread -2, background jobs 10: `UA_THREAD_PRIORITY=1`) | 49.4 against 49.1 - 49.6: noise. Option, off |
| Mono's inline limit 40 (`MONO_INLINELIMIT=40`, default 20) | 49.3: noise |
| Mali's submission thread `mali-cmar-backe` at nice -10 (it waits for a CPU as long as it runs, 17 % of the time) | its wait for a CPU 15 -> 6 %; 33.1 fps against 33.4 (GPU at 600 MHz): no gain |
| FMOD's threads on core 0 and every other thread on cores 1 - 3 (`UA_AFFINITY='FMOD*:0 SDLAudio*:0 *:1-3'`; FMOD's mixer runs at nice -19, 57 % of a core, and preempts whatever shares its core) | without the GPU's work 50.8 / 50.9 / 50.6 fps against 49.1 - 49.6 (**+3 %**); only UnityMain kept off core 0: +1.2 %; UnityMain, the render thread and Mali's thread: +2.8 %. In the game itself nothing: King's Pass 33.0 against 33.4, the main menu 43.2 / 43.0 against 42.8 / 43.1, both GPU-bound. Option, off |
| Unity's job threads: `-job-worker-count` 2 / 1 / 0 (`UA_UNITY_ARGS`; the default is 3 on four cores, and their wake-ups cost UnityMain about 1 ms a frame) | 49.5 / 50.0, 49.6 / 48.4 and 46.8 fps against 49.2 / 49.0: none pays (0 workers: every job on UnityMain, -5 %) |
| `gfx-threading-mode=4` in the converted boot.config (the Mac build's native graphics jobs) | none to make: libunity's `AdjustThreadingModeForAPI` turns any jobs mode into 2 (a render thread) for OpenGL ES, as an Android build gets |
| Earlier (2026-10-22, GPU-bound then too, so read with care): Mono `-O=abcrem`, `-O=abcrem,ssapre`, `UA_GC_BUDGET=64`, `UA_AUDIO_FRAMES=1024` | within noise |

The ceiling this leaves: King's Pass is held by the GPU at 33 fps warm and 37 cool; the main
thread alone would allow about 49 - 51 fps. 60 fps needs the GPU's frame cut by about 45 % at 600
MHz (the grab passes and the 4-pass background blur are its largest pieces: [GPU, pass by
pass](#gpu-pass-by-pass)) and the main thread's by about 15 % (19.3 -> 16.7 ms); neither has a
picture-identical change of that size in sight in the runtime. IL2CPP (the 64-bit build) replaces
the managed code and Mono's runtime (4 - 5.5 ms here) but runs the same engine and the same GPU work
(30.6 - 32.4 fps in King's Pass).

### The handheld around the game (2026-10-23)

What the system around the game costs in King's Pass, both builds (the plan above, 30 s measured,
the GPU at 600 MHz, the SoC at 69 - 73 C), from the bench's counters and `UA_GPU_IDLE`:

| question | measured | reading |
|---|---|---|
| does the GPU wait for work inside a frame (a sync, a readback, the swap, a late submission)? | `UA_GPU_IDLE=5`: 0.08 ms (32-bit) and 0.10 ms (64-bit) of a 31 ms frame, 0.2 - 0.3 %, in every 5 s window; 25 spans (framebuffer changes) a frame. At the title (60 fps) 11 ms of a 17 ms frame, during loads most of it. The same in two GPU-bound URP games: Chapéu de Papel's title 0.11 ms of 28.6 (4 spans a frame), The Third Eye's first scene 0.07 ms of 31.5 (84 spans); Cuphead's title (64-bit, held at 60 fps by vsync, the GPU at 504 MHz) 2.5 ms of 16.9 | no. The render thread's ~11 ms a frame blocked in Mali's driver is the driver holding it back while the GPU is behind, not the GPU waiting for it: there are no bubbles to fill |
| does the presentation add waits (a frame done just after a vblank)? | host flips: the render thread's wait for its next buffer (`gate`) 0.0 ms a frame, 2 frames queued; without vsync (`mailbox`) the same frame rate (runtime.md: Cell Machine 53.6 / 53.4, Chapéu de Papel 36.4 / 36.3) | no: the GPU always has the next frame |
| memory pressure | 64-bit: 48 MB available at the end, 240 MB of swap in use (zram, lz4); in the 44 s measured 28 swap-ins, 1157 swap-outs (26 a second), 36 major faults, kswapd 0.1 % of a core. 32-bit: 76 MB available, 0 swap-ins, 0 swap-outs, 7 major faults | the swapping happens while the save loads; a frame of play pays nothing measurable for it |
| other processes | about 7 % of a core: Mali's job-completion kworkers 6 %, the FUSE daemon of `/userdata` (exFAT) 1 %, schedutil's thread 0.5 %. The bench's own sampling script 2 % | nothing that a game's frame waits for |
| the host's own threads | the UI thread 3 % of a core (its 4 ms poll of SDL and the looper: 270 wake-ups a second), `ua_flip` 1.7 % (nearly all kernel time in the sunxi driver's pan) | small; see the power budget below |
| interrupts, context switches | 8,000 - 9,400 interrupts a second (the timer 1,500 - 2,800, the GPU's job interrupt 1,450, the vblank 60), 15,000 context switches a second (Mali's submission thread 2,200, Unity's three job workers 3,600: they sleep and wake for each job batch, and 50 - 84 % of their time on a CPU is that futex system call) | the job workers' sleep and wake is about 15 % of a core |
| the loader's shims (bionic's pthread, stdio, the softfp thunks), the host's GL wrappers | ua_host's code is 1.1 % of UnityMain's samples, 0.3 % of the render thread's | nothing to gain |

**The GPU's clock is a share of one power budget, and the CPU's busy time takes from it.** The
thermal governor of the RG34XX-SP's kernel is the power allocator (`power_allocator`, read from
`/sys/class/thermal`): on the CPU's thermal zone only, it starts at 60 C (trip 0) and holds the
zone at 70 C (trip 1) with a power budget of 1000 mW (`sustainable_power`) plus a proportional term
(`k_pu` 40, `k_po` 20, no integral or derivative: about 20 mW less per degree over 70 C). Both
cooling devices hang on that zone with the same weight (1024): `thermal-cpufreq-0`, the four
cores' clock, and `thermal-devfreq-0`, the Mali's. Each period the budget is split between them
in proportion to the power each one asks for: the CPU's from the busy time of its cores at their
clock (a nice 19 thread's included), the GPU's from kbase's power model (`mali-simple-power-model`:
dynamic coefficient 750, static 17000) at its utilization; each then gets the highest clock its
share pays for. The GPU's own thermal zone has no cooling device. So once the SoC is warm, a
GPU-bound game's frame rate depends on how busy the CPUs are, whatever they are busy with.

Measured (King's Pass, 32-bit, 300 s from a cool start, the CPU zone at 69 - 74 C once in the
level): with one extra busy loop at nice 19 for 60 s (it takes only the cores' idle time, about one
core of it), the Mali went from 600 to 504 MHz within about 5 s and the frame rate from 33.2 to
27.7 - 28.8 fps (-16 %); when it stopped, 600 MHz and 33.2 fps again within about 5 s. Outside
that minute the GPU stayed at 600 MHz once the first 10 s were over, 696 MHz in 2 of the samples
(696 MHz needs a larger share than the CPUs leave it; the CPUs ran at 1416 - 1512 of their 1704
MHz). A second run, the busy loop on during seconds 60 - 120 and 180 - 240 of 300: the first minute
on, while the SoC still warmed (68 -> 72 C), took the GPU from 696 MHz (38 of the 60 s before) to
600 and the CPUs to 1416 MHz, 35.9 -> 33.1 fps; the second, once warm, 600 -> 504 MHz (56 of its 60
s), 33.3 -> 28.8 fps (-13 %), and 33.2 fps at 600 MHz in the minute after it. The other way,
CPU time taken away (a measurement: FMOD mixing at 12 kHz, `UA_AUDIO_RATE=12000`, which halves its
mixer's CPU time, 56 -> 29 % of a core; the sound is not the game's), two runs of 120 s from 48 C:
the Mali at 696 MHz for all 120 s against 28 s at 48 kHz, **37.5 fps against 34.2 (+9.7 %)**, the
SoC as warm (the CPU zone 69.6 against 69.3 C on average). So CPU time a frame does not need is
GPU clock lost, on every GPU-bound game once warm (about a quarter of a core is the step from
600 to 696 MHz in King's Pass), and a run's frame rate depends on the bench's own CPU time too
(profrun.sh's per-second loops now read with bash's builtins: they started 14 processes a second,
about 3.5 % of a core). It also means a
bench run must start as cool as the one it is compared with (the device lock waits for 50 C):
King's Pass from 47 C ran 34.5 fps, the GPU at 696 MHz 22 of 60 s; the same run from a warm start
at 69 C, 32.3 fps at 600 MHz throughout.

Where King's Pass's 2.6 cores of CPU time go (32-bit, 60 s; % of a core): UnityMain 83, the render
thread 57, FMOD's mixer 56, Mali's submission thread 17 (+ its job-completion kworkers 6), Unity's
three job workers 21, the host's UI thread 3, `ua_flip` 1.7, Unity's choreographer and background
job threads 3. What of it a frame could do without, measured or read from the profiles:

| CPU time | measured | reading |
|---|---|---|
| Unity's job workers sleeping and waking for each job batch (3,600 wake-ups a second) | `-job-worker-count 2` (`UA_UNITY_ARGS`), two runs from 47 C: 34.9 fps (696 MHz 24 of 60 s) against 34.5 (22 s); the workers' CPU 19 % of a core against 21 %, every core as busy (64 - 68 %) | the job batches' wake-ups move to the remaining workers: about 2 % of a core, within noise. Not taken |
| the host's UI thread: a 4 ms poll of the looper and SDL's events | profiled (1000 Hz, 15 s): 2.9 % of a core, 272 wake-ups a second; 59 % of its CPU time is the sleep's own system call, 25 % SDL 2.32's event pump (udev monitor, the pads), 8 % `poll` | half of it would need half the wake-ups: a longer poll (input up to 4 ms later, not the game as it is) or the pads' events waking it, which SDL does not offer. About 1.5 % of a core, a sixth of the quarter of a core that is one GPU clock step: not done |
| `ua_flip`: the framebuffer pan | 1.7 % of a core, all of it kernel time in the sunxi driver's `FBIOPAN_DISPLAY` (0.5 ms a pan) | the kernel's, a pan a frame |
| FMOD's mixer decoding Vorbis | 40 % of the mixer's samples (2026-10-22 profile, `fmod_tremor_*`): about 22 % of a core. More than a third of it (8 points of a core) was the converter's doing: the 12 atmosphere loops the game keeps loaded from the title on, which it marks DecompressOnLoad and the audio step had switched to CompressedInMemory | measured, and the audio step changed: [below](#fmods-mixer-and-the-audio-step-2026-10-24) |
| FMOD mixing at 48 kHz (`UA_AUDIO_RATE`) for 44.1 kHz clips | the resamplers are 2 - 3 % of the mixer's samples; 44.1 kHz would mix 8 % fewer samples (12 kHz, a measurement: the mixer 56 -> 29 % of a core, +9.7 %, above) | not the same sound (the mix and its effects at another rate, resampled elsewhere): not a default |

### FMOD's mixer and the audio step (2026-10-24)

Unity decodes a clip marked DecompressOnLoad to 16-bit PCM once, when the clip loads, and keeps the
PCM while it stays loaded; a CompressedInMemory clip keeps its Vorbis data, and FMOD's mixer decodes
it every time it plays. The converter's audio step used to switch every DecompressOnLoad Vorbis clip
of 2 s or more to CompressedInMemory (for Cuphead's Devil fight: 53 MiB of PCM in what its scene loads):
the same Vorbis data, decoded while it plays instead of once, the sound unchanged, the mixer's CPU
time not.

**What Hollow Knight holds.** Its data has 1,999 AudioClips: 1,023 CompressedInMemory and 61
Streaming as the game ships them, 915 DecompressOnLoad (506 MiB of PCM), of which the step switched
321 (450 MiB of PCM, 60 MiB of Vorbis; read with UnityPy, every clip's load type, length, rate and
channels). What a scene keeps loaded is its own shared assets file (`sharedassets<n>.assets`: the
assets the build put there are the ones scene n uses) and the assets its PreloadData lists from
other files: King's Pass (`Tutorial_01`) 139 clips, 11.3 MiB of them DecompressOnLoad PCM; the title
132 clips, 41.4 MiB, most of it 12 loops (cave wind, Dirtmouth's winds, rain, a waterfall, the
crystal shimmer, Fog Canyon, the shade's music, Grimm's bat: 29.6 MiB of PCM, 4.1 MiB of Vorbis)
on 14 AudioSources of a prefab in `resources.assets` that the game creates at the title and keeps
from scene to scene.

**What King's Pass decodes** (32-bit, the bench's plan, runs from 48 C; the installed OBB against
copies of it whose AudioClips got their original load type back, the 4 bytes of `m_LoadType` and
the entries' CRCs patched in place, nothing else of the files moved; FMOD's mixer thread from the
bench's per-thread CPU time):

| load types in the OBB | FMOD's mixer, % of a core | available memory in King's Pass (MB, lowest / average over the run) | GPU at 696 MHz, first 45 s |
|---|---|---|---|
| the step's length rule (as installed), 6 runs | 56 - 57 | 55 - 71 / 61 - 76 | 25 - 31 s (45 from a 45 C start) |
| King's Pass's own 11 switched clips back (1 of the 12 resident loops among them) | 56 | 53 / 56 | 30 s |
| 4 of the 12 resident loops back | 55 | 67 / 70 | 31 s |
| 7 of the 12 | 53 | 63 / 64 | 33 s |
| the budgeted step at 16 MiB (7 of the 12) | 54 | 56 / 59 | 40 s |
| the budgeted step at 36 MiB (all 12) | 49 | 46 / 49 | 45 s |
| every clip the step switched back, 3 runs | 48 | 35 / 41 - 55, 60 - 95 MB more in swap | 38 s |

The decoding the step had added is the resident loops: all of them play all the time in every
scene (the game's AudioMixer snapshots set what is heard, so FMOD decodes each one whatever its
level), 8 points of a core, more than a third of the mixer's Vorbis decoding; King's Pass's own
clips are not playing there. The rest of the decoding is the game's own CompressedInMemory clips. The frame rate,
120 s runs from equal 48 C starts, every switched clip back against the length rule: 34.1 -> 34.7
and 34.1 -> 34.5 fps (+1.7 %, +1.1 %), the Mali at 696 MHz 50 and 49 s of the 120 against 28 and
28; that variant swapped while it ran (available memory down to 35 MB, 60 - 95 MB more swap), which
the 36 MiB budget, with the same mixer time, does not (46 MB at the lowest, no swap traffic): 36.9
fps over its first 45 s at 696 MHz throughout (35.0 - 35.5 as installed in the same sessions). The loads do not get longer: the title's 7.9 - 8.1 s
and the save's 3.7 + 5.6 s are the same in every variant (within 0.15 s).

The 64-bit build (`hk64`, two runs of each, 45 s, interleaved): FMOD's mixer 54 - 55 % of a core
as installed, 50 - 51 with the 16 MiB budget, 47 with the 36 MiB one; 34.7 / 34.5 fps (the Mali at
696 MHz 22 / 22 of 45 s), 35.1 / 34.9 (24 / 21 s) and 35.8 / 35.3 (32 / 28 s). Its memory in King's
Pass hardly moves (available 41 - 51 MB as installed, 43 - 47 MB with either budget: the kernel
keeps about 240 - 300 MB of the game in zram either way); the load from the save swaps for 11 - 107
s in every variant (79 and 11 s as installed, the first run of a session the shortest; grab_fetch's
runs of the same data: 11 - 91 s), nothing the variants can be told apart by.

The mixer's profile (32-bit, 250 Hz, 20 s each, `UA_PROF_THREADS='FMOD*'`), as installed and with
the 36 MiB budget: on a CPU 56 and 45 % of the time; Vorbis decoding (`fmod_tremor_*`, the codebook
lookups, `CodecVorbis`) 41 and 32 % of its samples, 23 -> 14 points of a core; Unity's AudioMixer
fader 15 / 17 %, the SFX reverb 12 / 16 %, `FMOD::DSPFilter::read` (the lowpass filters) 14 / 15 %,
flange 4 / 5 %, the resamplers 3 / 3 %, libc's copies 5 / 6 % (the same work, a larger share of
less). What is left to decode is the game's own CompressedInMemory clips (1,023 of them, the
Knight's and the enemies' sounds among them, which it decodes while they play on every platform;
which of them King's Pass plays was not looked up). The rest of the audio path, also measured:

| piece | measured | reading |
|---|---|---|
| ua_host's output: the OpenSL ES shim's buffer queue into SDL (the bench's dummy driver; on the handheld SDL's ALSA driver, into PipeWire's ALSA plugin) | the `SDLAudioP2` thread 0.4 % of a core in King's Pass (125 s); FMOD's stream thread 0.4 % | a copy of each 2 KB buffer, no gain stage at full volume: nothing to take away. PipeWire's own cost needs the card (the runs stay silent): not measured |
| the output rate | ua_host reports 48 kHz (`UA_AUDIO_RATE`), FMOD mixes at it, and Knulli's PipeWire runs its graph at its default 48 kHz (`default.clock.rate`, `allowed-rates` left at `[ 48000 ]`), the H700's codec too | the mix reaches the card with no second resampling; FMOD's resamplers (3 % of the mixer) take the game's 44.1 kHz clips (1,730 of its 1,999) to 48 kHz as on an Android phone with a 48 kHz output. 44.1 kHz would mix the 48 kHz clips and the effects at another rate and leave PipeWire to resample: not the same samples, not done |
| a longer DSP buffer (`UA_AUDIO_FRAMES=1024`) | Unity 6000.0's FMOD then leaves OpenSL ES for its AudioTrack output at 24 kHz (log: `FMODAudioDevice: rate 24000`): 38 % of a core, 37.0 fps over 45 s at 696 MHz throughout | another sound (half the rate), not a buffer: a measurement, and it says the earlier `audio-1024` runs measured that too. A real buffer change (AudioManager's DSP buffer size) is more latency, a feel change: opt-in at most, not built |
| the mixer thread's page faults | blocked in a page fault in 6 - 7 % of its samples | pages it reads that are not resident (swapped out, or file pages); which, not looked into |

**The step now** ([audio.py](../convert/pmunity/audio.py)): a DecompressOnLoad clip stays as the game
has it unless its scene's PCM would pass the port's budget (`audio.pcm_budget_mb`, default 16): per
scene, what it keeps loaded (as above) plus what the first two scenes keep (the start scene and the
title, where games create what they keep from scene to scene: Hollow Knight's loops), all formats
counted; a scene over the budget switches its own long clips first, the largest first, then the
resident ones. Only clips the length rule would switch (Vorbis, 2 s or more) are ever switched, so
the budget never keeps less of the game's choice than the old rule did, and clips no scene uses
(loaded by scripts) keep the length rule. Hollow Knight at 16 MiB: 82 clips switched (302 MiB of
PCM, the Godhome bosses' music and the cutscenes'), King's Pass holding 13.1 MiB; at 36 MiB, 51 clips,
King's Pass 34.7 MiB. Cuphead at 16 MiB: the Devil fight's short clips alone hold 18.9 MiB, so it
keeps the length rule there. `pcm_budget_mb: null` is the length rule alone.

Cuphead's Devil fight (the 64-bit build, the port's test hooks: `CUPHEAD_TEST_LEVEL=Devil`, god
mode, the homing weapon; 45 s from the fight's start, one run each): 59.4 fps, held by vsync with
the Mali at 504 MHz, FMOD's mixer 41 % of a core, 266 MB available; with every clip the step had
switched back (909 clips, 75 of them in the fight's set) 59.5 fps, the mixer 34 %, 243 MB available,
the level's load 2.05 s against 1.95. The fight is not held by the GPU, so the 7 points of a core
are power and heat, not frames; the memory is there for a port that raises the budget (at 32 MiB
the fight would hold 31.8 MiB, 39 of its long clips as the game has them).

## Reverse engineering libunity, and what an exact in-memory change can reach

Unity publishes symbol files for its Android players (`pmunity player fetch <version> --symbols`:
`Variations/<backend>/Release/Symbols/<abi>/libunity.sym.so`), which name libunity's functions by
`module+offset` in a profile ([profsum.py](#profsumpy)). With them the hot frames above were read
back to the engine code that spends the time, to look for work this platform makes Unity do that a
newer Android device would not and for an exact faster replacement. The finding: the pieces an
in-memory hook can replace *without changing a pixel* are small (about 1 % of UnityMain each), and
the frame the GPU holds does not move. The largest platform cost is the job system's own futex
traffic, 0.8 ms of UnityMain's frame in `FUTEX_WAKE` system calls; an exact hook for it was built and
measured (`UA_FUTEX_SPIN`, [runtime.md](runtime.md)) and did not pay on the four-core handheld, so it
stays an option, off. No decompiled Unity code is kept in this repository; functions are named by
their symbol and described in our own words.

### Where UnityMain's engine time goes (King's Pass, the GPU's work left out)

Read with the flat-shader method ([What limits King's Pass](#what-limits-kings-pass-32-bit-2026-10-23)),
so the GPU clock does not hide a main-thread change, the 32-bit King's Pass frame (47 fps, UnityMain
95 % on a CPU) breaks down inside the engine as (`profsum.py --exact`, as a call tree from
`PlayerRender` / `ExecutePlayerLoop`, running samples as a share of UnityMain's on-a-CPU time):

- **the job system's scheduling and wake-ups, ~1.3 ms a frame.** `JobQueue::ScheduleGroups` ->
  `ujob_schedule_job_internal` -> `wake_or_exhaust_lane` -> `lane_wake_buddies` ->
  `ujob_lane_t::set_steal_hint_and_wake` -> `UnityClassic::Baselib_SystemFutex_Notify` is 1.0 - 1.1 ms
  of UnityMain alone: a `FUTEX_WAKE` system call each time a job batch is scheduled (culling,
  sprite and particle geometry, the render queue). The three worker threads are mostly idle (a few
  percent of a core each) and spend essentially all of their time asleep in `lane_guts` ->
  `Baselib_SystemFutex_Wait` (in a `prof-all` recording every worker sample was blocked there); each
  batch wakes one. The render thread reaches the same wake when it schedules its geometry jobs.
- **rendering, ~8.6 ms**: `RenderManager::RenderCameras` 6.4, the offscreen (blur) camera 1.8, the
  HUD/IMGUI repaint 0.2. Inside a camera the time is the draw-call build (`ForwardRenderLoopJob`,
  `BatchRenderer::ApplyShaderPass` -> `ShaderLab::Pass::ApplyPass`), the culling-result teardown
  (`RenderNodeQueue::Reset`, a memset of the per-frame node queue), and waits on the geometry jobs
  the camera scheduled (`CompleteFenceInternal` -> `JobQueue::WaitForJobGroupID` -> `ujob_wait_for`,
  which runs a job itself while it waits).
- **scripts, animation, physics, audio** as the area table shows; `MonoBehaviour::CallUpdateMethod`
  calls `MonoBehaviour::IsStateMachineBehaviour` first for every behaviour every frame (0.5 ms,
  through `SerializableManagedRef::GetScriptCache` and a `std::shared_weak_count` lock), and
  `GetCameraTargetRect` asks the display manager for the display's rect on every camera-rect query.

### The job system's futex traffic (`UA_FUTEX_SPIN`, `UA_FUTEX_STATS`)

What the engine does that an Android phone would not is nothing in the arithmetic; it is the round
trips through the kernel. A Baselib worker that runs out of jobs tries ten times to steal one (a few
microseconds, `lane_guts`) and then sleeps in a `FUTEX_WAIT`; here the three workers are so lightly
loaded that they are nearly always asleep, so every batch's wake is a real `FUTEX_WAKE` system call.
(Unity 6000.0 also has a boot option, `job-system-wait-spin-time-ns`, default 20,000: how long a
thread waiting for a job handle spins in `ujob_sleep_state::try_sleep_until_handle_done` before it
sleeps; it applies to the waiting thread, not to the workers' ten tries.)

The engine reaches the kernel futex through one libc import, `syscall`, which the loader resolves
like any other; `runtime/host/futex.c` registers a wrapper for it before the player loads (the
player's own code untouched). `UA_FUTEX_STATS=<s>` counts the waits and wakes per thread.
`UA_FUTEX_SPIN=<us>` makes each `FUTEX_WAIT` watch its word in user space for up to `<us>`
microseconds before it sleeps, and skips a `FUTEX_WAKE` when no thread is asleep in the kernel on
that word; the same spin is given to bionic's `sem_wait` (`runtime/loader/hybris_compat.c`), where
Unity 2017.4's older job queue sleeps (its `PlatformSemaphore` is a POSIX semaphore, bionic's a
futex word).

**Exact by construction.** A futex wait may return early at any time, and every caller re-reads its
own word and loops, so returning from the user-space watch computes nothing different: only the
instant a waiting thread resumes moves (up to `<us>` sooner). A wake is dropped only when there is
provably no kernel waiter: a wait that is about to sleep increments a per-word count, issues a
seq_cst fence and re-reads its word before the system call (returning without entering the kernel if
it already changed); a wake issues a seq_cst fence after the caller's store to the word and then
reads the count. The waiter's word read and the waker's count read are a store-buffering pair that
the two fences forbid both sides of, so a wake is dropped only when the waiter will not sleep.
Operations the skip cannot reason about (bitset waits, PI locks, requeues) turn it off while they
are in the kernel. `runtime/host/test/test_futex.c` and the loader's `test_sem` run a ping-pong and
a producer/consumer semaphore with the spin on and off and assert no wake-up is lost (QEMU cannot
read the timer, so `test_futex` skips there; on the RG34XX-SP it passes with the spin at 0, 20 and
100 us).

**Which versions it covers.** Every Unity Android player imports libc `syscall` and bionic
`sem_post`/`sem_wait` (checked in the players of the handheld benches: 2017.3, 2017.4, 2018.2,
2019.3, 2021.2, 2022.2 and 6000.0, Mono and IL2CPP, armv7 and arm64). The hook sits at that libc/bionic boundary, not inside libunity, so it
needs no Unity symbol and no signature match and cannot mis-target a version it has not seen; on a
kernel or emulator where the ARM generic timer cannot be read from user space (QEMU) it turns
itself off (a probe at start, as `fastclock.c` does) and the calls pass straight through.

**On the handheld (2026-10-23).** King's Pass, 32-bit, the GPU's work left out (flat fragment
shaders, so the GPU idles at 240 MHz in every run: 97 - 100 % of the time; the CPUs at 1512 MHz
throughout), A B A B in one hold, 20 s measured:

| run | fps | frames over 25 ms | Job.Worker 0/1/2 (% of a core, waiting for one) | render thread waiting for a CPU |
|---|---|---|---|---|
| off, `UA_FUTEX_STATS=5` | 48.3 | 97 | 6/12/8 %, 11/8/14 % | 3 % |
| `UA_FUTEX_SPIN=50` (+ stats) | 47.2 | 200 | 13/16/14 %, 13/17/20 % | 9 % |
| off | 48.4 | 120 | 6/12/8 %, 12/7/14 % | 3 % |
| `UA_FUTEX_SPIN=50` | 47.6 | 194 | 13/16/14 %, 14/17/20 % | 10 % |

`UA_FUTEX_STATS` with it off: UnityMain makes about 3,100 `FUTEX_WAKE`s a second (2,500 of them wake
a thread), each 12 - 13 us in the kernel: 37 - 40 ms a second, 0.8 ms of its 21 ms frame, which is
what the profile showed. The render thread makes 1,900 a second (8 - 9 us each), and one worker
wakes the others (1,170 a second, 23 us each). Each worker waits 1,200 - 2,450 times a second and
is asleep 86 - 94 % of the time; its sleeps are spread out: about a third under 50 us, most under
1 ms. The stats themselves cost nothing measurable (48.3 against 48.4).

With `UA_FUTEX_SPIN=50` UnityMain's wake system calls fall to about 2,000 a second (700 - 980 skipped
because the worker was still watching), but each remaining wake takes longer (14 - 30 us: its
target core is busier), so UnityMain's time in wakes does not drop, and each worker spends 56 - 97 ms
a second watching. The four cores are already about 75 % busy (UnityMain, the render thread, FMOD's
mixer at 57 % of a core, Mali's submission thread), so the watching workers take CPU from the render
thread (its wait for a CPU 3 % -> 9 - 10 %) and from Mali's thread (16 % -> 21 - 22 %): **-2 %** and
twice the frames over 25 ms. The sleep histogram says no watch length helps here: one short enough
to cost little catches only the few sleeps under 10 us, and one long enough to catch the 50 us - 1
ms sleeps costs more CPU than the handheld has free. The 12 us each wake costs is spent in the
kernel's wake-up path (Linux 4.9 on the Cortex-A53), which a game process cannot change.
`UA_FUTEX_SPIN` therefore stays an option, off; `UA_FUTEX_STATS`
is the measuring tool. (In the game as it ships the GPU sets the rate, King's Pass at 33 fps warm,
so a main-thread change would not show there either. The 2017.4 `sem_wait` path passes the loader's
tests but has not been benched in a 2017.4 game.)

### What did not pay, or is not exact

| change (how it would hook) | result |
|---|---|
| `UA_FUTEX_SPIN=50`: workers watch their futex word 50 us before sleeping, wakes with nobody asleep skipped (the libc `syscall` import; exact) | 47.2 / 47.6 fps against 48.3 / 48.4 with the GPU's work left out (-2 %), frames over 25 ms doubled: the watching takes CPU from the render thread and Mali's thread (above). Option, off |
| `InvertMatrix4x4_Full` (self time ~1 % of UnityMain; called per camera from `Camera::ExtractCameraRenderingParams`, image-effect blits, `ScreenPointToRay`) replaced by a NEON inverse, or memoized (it reads only its 16 input floats and writes only its output, no globals or calls) | a rewritten 4x4 inverse reassociates the arithmetic, so it is not bit-identical and cannot be a default (the exactness rule forbids it); a last-inputs memo would be exact but the function is small next to the wake traffic |
| `ShaderVariantAnalytics::...::SumUpVariants*` (editor telemetry summed per draw inside `ShaderLab::Pass::ApplyPass`, ~0.4 % of UnityMain and of the render thread) | computes an internal analytics counter the game never reads; could be no-op'd, but the saving is below the run-to-run noise |
| hooking `GfxDeviceGLES` to drop redundant GL state or uniform re-uploads on the render thread | the render thread's time is spent blocked inside Mali's driver (the grab copies and the first draw after a framebuffer change), not in redundant state Unity sends; the driver's buffer handling, not Unity's call count, is the cost ([GPU, pass by pass](#gpu-pass-by-pass)) |

## GPU, pass by pass

`UA_GL_TRACE=<s>` logs one whole frame every `<s>` seconds ([runtime.md](runtime.md)): its passes
(each `glBindFramebuffer` of a draw framebuffer starts one), per pass the framebuffer and size,
whether it starts with a clear or an invalidate (else a tiler loads the old contents), draws and
vertices (index counts), blits and copies, and the GPU time of each pass (`UA_GL_TRACE_GPU`, default
3: the host waits for the GPU at each pass's end, so passes run one at a time and each carries
about 0.3 ms of submission and start-up on the RG34XX-SP; the frame's first pass also carries the
vsync wait). `UA_GL_TRACE_START=<s>` puts the first traced frame past the menus: a traced frame
is a long one, and a plan's `steady:` step waits for a second without long frames.
`UA_GL_TRACE_DRAWS=<n>` then lists the frame's draws one by one with their programs' source hashes
(the file names `UA_SHADER_DUMP` writes), samplers and textures, and depth, stencil and blend state:
which shader each full-screen pass is, and whether a pass's depth/stencil buffer ever matters. A
dumped shader's uniforms and samplers (`_GrabTexture`, `_Bloom`, `_RgbTex`, ...) name it; the
game's Shader assets (UnityPy: `m_ParsedForm.m_Name`, `m_PropInfo`, the passes' `m_Type`, 2 = a
grab pass) confirm which.

Two things to check before reading a trace: that the frame is the scene meant (a plan's `shot`
step, then look at `shot-<n>.ppm`: a Hollow Knight plan that stopped one press short measured the
main menu for a day), and that the timings come from the clock the game runs at (`freq.log`: the
Mali's governor moves between 600 and 696 MHz).

### Hollow Knight, King's Pass (6000.0, 64-bit)

Reaching it from a cold start takes the menu's own load, then Start Game and the save slot; the
64-bit build reaches the save with 40 - 50 MB of memory available and swaps while it loads (one
load took 30 s), so the bench's memory watchdog line is lowered:

```sh
export DEVICE=adb ARCH=arm64 UA_PAD_NAME='Nintendo Switch Pro Controller' UA_MEM_MIN=20 MEASURE=25
export PLAN='log:find GameCameras; hitch:2500; steady:3; sleep:2; press:0:b; sleep:6; press:0:b; sleep:6; press:0:b; hitch:2000; steady:5; shot'
tools/device/profbench.sh /userdata/uatest/hk64 trace UA_GL_TRACE=4 UA_GL_TRACE_START=90 MEASURE=40
```

(the third press lands while the save loads when the second one already started it). One frame:
24 passes, about 90 draws (`glDrawElements`, two `glDrawElementsBaseVertexOES`), 13,500 indices.
The GPU time per pass, median of 72 traced frames (serialized; each includes about 0.3 ms of
submission and start-up):

| passes | what | ms |
|---|---|---|
| 576x360 x6 | the background layer's camera (7 sprites) into its own target, a two-iteration separable blur (4 passes), the result back into its target | 2.4 + 4.7 + 0.6 |
| 720x480 | two black bars into the window (the picture is 720x450) | 1.0 |
| 720x450 x4 | the scene: clear, the blurred background, ~50 sprites and particles (the game's dynamic batches: 2970 and 3948 indices) | 3.8 + 3.2 + 6.0 + 5.7 |
| 720x450 x3 | between those: three grab passes (blend-mode sprites: screen, overlay, vivid light, each `GrabPass {}`), each a full-screen copy of the scene's colour texture by Unity's internal blit program into another framebuffer | 0.8 + 0.7 + 0.8 |
| 180x112 x5 | bloom: downsample, two blur iterations | 1.9 |
| 720x450 x2 | bloom composite, colour correction (`_RgbTex` curves) | 0.9 + 1.3 |
| 720x450 | brightness/contrast back into the scene's target, then the HUD (14 draws) | 1.5 |
| 720x450 | the result copied into the window | 1.2 |

About 36.6 ms serialized, about 29 ms of GPU work once the per-pass overhead is taken off; the
frame takes 32.7 ms (30.6 fps) and UnityMain is on a CPU 80 % of it: the GPU and the main thread
are both near their limits here. Each grab pass makes the camera leave its framebuffer and come
back: the Mali stores its colour and depth/stencil buffers and loads them again. The depth/stencil
buffer never changes after its clear (the sprites test depth with writes off, the stencil
operations keep), so `UA_GL_DSCLEAR` (runtime/host/glopt.c, on by default) clears it again
instead of loading it: 30.6 -> 32.4 fps (two runs each; the main menu, 6 grab passes a frame,
36.1 -> 38.7; the 32-bit build in King's Pass 31.8 -> 32.5, one run each). Exact: the buffer gets
the values it holds.

What did not pay, or is not exact:

| change | result |
|---|---|
| invalidate the grab copy's target before Unity's blit (it overwrites every pixel; exact) | 36.1 -> 36.2 (menu): the copy's load is not what costs |
| invalidate the depth/stencil buffer when the camera leaves its framebuffer (not exact if the depth texture is read later) | nothing over `UA_GL_DSCLEAR` (38.7 -> 38.6, menu): the stores are cheap, the loads were not |
| no grab copies at all (a measurement: the blend-mode sprites read a stale texture) | menu 36.1 -> 41.4 (+14 %), 41.7 with `UA_GL_DSCLEAR` |
| the blend-mode shaders read the framebuffer instead (`GL_ARM_shader_framebuffer_fetch`, `gl_LastFragColorARM`, through `UA_SHADER_OVERRIDE`) with the grab copies left out | menu 41.8 (+16 % over no change); King's Pass 32.4 -> 35.0 - 35.3 (+8 % over `UA_GL_DSCLEAR`), screenshots that differ no more than two of the same run a second apart. Not exact: sprites of one batch that overlap see each other (with the grab, the picture before the batch), and the fetched colour is mediump. As a data change: `shaders.grab_fetch` ([shaders.md](shaders.md#blend-modes-without-grabpass-copies-grab_fetch-opt-in); `"always"` for this device's ARM-only driver) |
| a window without depth/stencil (an EGL config with none; the two bars write the window's depth, nothing reads it; Android's "Disable Depth and Stencil" player setting does the same) | King's Pass 32.6 -> 32.8 (two runs each): noise |
| GL_EXT_disjoint_timer_query per pass | the driver's answers cannot be right (0 ms for most of the scene's passes): `UA_GL_TRACE_GPU=3` instead |

### Sprites' transparent texels (King's Pass, 2026-10-23)

A sprite is drawn as a mesh, and the mesh covers texels of alpha 0 too: their fragments are
rasterized, shaded, sample the texture and are blended like the others. Where the texture sample's
alpha is exactly 0 such a fragment cannot change the picture under the scene's blend states: the
`Sprites/Default`-style shaders premultiply in the shader and blend One/OneMinusSrcAlpha (rgb and
alpha 0: dst stays), `tk2d/BlendVertexColor` blends SrcAlpha/OneMinusSrcAlpha, the additive
particles SrcAlpha/One, the blend-mode sprites discard below 0.01; nothing in these passes writes
depth or stencil (the stencil operations keep). (Not every shader: the darkness overlay adds a dither and a cutout term whatever
the texel.) A mesh cut down to the texels that can show would skip those fragments. How many are
there?

**Counted on the handheld.** `tools/device/deadfrags.py shaders <dump> <trace run.log> <dir>` turns
every fragment shader of a `UA_GL_TRACE_DRAWS` frame that samples `_MainTex` at a varying into a
counter: each fragment adds one code to red when the sample (the same texture, sampler and UV as
the game's, so the GPU's own bilinear footprint decides) has alpha exactly 0, to green otherwise;
the blend state each is drawn with makes that an exact sum (`1,1-sa` adds; `sa,1-sa` with alpha
1/255 adds one code and keeps the others below 127; `sa,1` adds). The post effects become copies
(`--copy`), the background copy and the darkness overlay add to blue (`--mark`), the HUD's own
shaders write nothing (`--hide`); `deadfrags.py sum` adds a run's screenshots up. Four shots of the
scene, 64-bit build, the whole 720x450 picture (leaving the HUD's box out, 4 % of it, moves the
shares by 0.1 point):

| fragments of the scene passes | full-screen layers | sampling only alpha 0 |
|---|---|---|
| every sprite, particle and blend-mode sprite | 6.98 | 0.56 (8.0 %) |
| of them `Sprites/Lit` (the material of 2,645 of the level's 2,946 SpriteRenderers: its art) | 3.14 | 0.37 (11.6 %) |
| `tk2d/BlendVertexColor` (the two large batches: the floor's dark fill) | 0.27 | 0.00 |
| the rest: `Sprites/Default`, the knight, particles, the three blend-mode sprites | 3.57 | 0.19 (5.3 %) |
| marked: the blurred background's copy (opaque) and the darkness overlay (full screen) | 2.00 | - |

The background camera's 7 sprites (576x360, blurred afterwards), counted with the blur passes as
copies (`--copy`) and the background copy passing the counts through (hand-written), less the
counts above: 1.44 of its layers, 0.11 of them dead (7.5 %). The counters were in every pixel's
range (at most 6 dead and 17 live fragments a pixel), the four shots agree within 0.2 %.

**Offline, from the converted data** (UnityPy; the King's Pass level's SpriteRenderers use 203
distinct sprites, all of them Unity's Tight meshes, packed into SpriteAtlases; the ASTC pages decoded
exactly in both of the GPU's decode modes, FP16 and UNORM8, a texel counting as transparent only when
both give 0; one mip level, bilinear, clamped): 17.9 % of the meshes' texel area has a bilinear
footprint of four alpha-0 texels. Unity's tight outlines are coarse (a fan of a few large triangles
over the gaps between stalactites, grass blades and leaves), so that area lies inside the outline,
in the gaps. A cut mesh needs a margin (UV interpolation is not exact to the texel, and the hardware
quantizes bilinear weights): with one texel around every texel that can show 15.0 % is left, cutting
at 8x8-texel blocks (a mesh of a sensible vertex count) 7.9 %, dropping only whole triangles that lie
in the dead area 0.1 %. On screen the dead share is smaller than in texel area (11.6 % of the
`Sprites/Lit` fragments): the sprites that cover most of the picture are the large, mostly opaque
layers.

**What it could save.** With every one of those fragment shaders discarding at once (a measurement,
`UA_GL_TRACE=4`), the four scene passes take 6.2 ms instead of 17.7 ms, the background camera's
pass 0.8 instead of 2.3; that run's GPU was at 504 MHz against 696 (less work: the governor clocked
down; the blur passes take 1.37 times as long), so at the same clock about 4.7 and 0.6 ms. The
sprites' fragment work beyond rasterization is thus about 13 ms a frame, 1.85 ms per full-screen
layer in the scene (1.2 per layer of the 576x360 target), and the dead fragments' share of it about
1.05 + 0.13 = 1.2 ms: **about 4 % of the 29 ms GPU frame for a mesh that followed every texel with
no margin at no cost.** Scaled by the offline ratios, a one-texel margin leaves about 1.0 ms (3.4 %),
a mesh cut at 8x8-texel blocks about 0.5 ms (1.7 %); at the warm handheld's 600 MHz cap (33.3 fps)
that is 34.7 and 34.0 fps at best, before the extra triangle edges (along every edge a mesh gains inside
the sprite the 2x2 quads are shaded for both triangles) and vertices are paid for. Below the 5 % worth
building.

**Not built**, for three reasons besides the size:

- *It is not bit-exact.* Unity computes a sprite's UVs at load from its vertex positions (the stored
  UV channel is zeros; `SpriteRenderData::CalculateUVsIfNeeded` -> `RecalculateUVs`: `(pos.xy *
  uvTransform.xz + uvTransform.yw) / texture size`, mirrored inside the atlas rect for flipped
  packing), so new vertices get UVs of the same affine map; but a pixel inside a new, smaller triangle
  interpolates its UV from other vertices than before, which can round differently in the last bits,
  and where that moves a bilinear weight by one step the colour can change by one code (expected, not
  measured: no tightened data was made). Only dropping whole original triangles keeps every remaining
  pixel's interpolation, and that is 0.1 % of the area.
- *The sprites' other uses are safe, the material is not.* Sprite bounds (culling, the sort point)
  come from the sprite's rect and pivot, not its vertices (6000.0's player: `SpriteRenderer::
  GetSpriteBounds` -> `Sprite::GetBounds`, which reads scalar fields only), physics shapes are separate data; but which shader draws
  a sprite is decided at run time (Animator sprite swaps, scripts that set sprites or materials), and
  the rule above holds only for shaders whose output vanishes with the texel's alpha (the darkness
  overlay's does not). A converter step would have to prove that for every material a sprite can be
  drawn with.
- *Vertices cost the main thread.* Unity batches sprites on the CPU (every vertex transformed and
  copied each frame), and King's Pass's main thread alone allows only about 49 fps: a finer mesh moves
  work from the GPU to the next limit.

## GPU counters: gpucnt

`runtime/host/gpucnt` (built with the host programs, static, so the armhf binary runs on both
userlands) reads the Mali's hardware counters through the kernel driver's own reader (kbase's
vinstr interface, `KBASE_IOCTL_HWCNT_READER_SETUP`, present up to UK 11.32; the RG34XX-SP has
11.17): every counter of the front end, the tiler, the L2 / memory system and the shader core,
GPU-wide (every context's work: the game's, and EmulationStation's if it draws), summed over dumps
100 ms apart. Nothing is changed on the handheld.

```sh
gpucnt sample <secs> [ms] [label]      # what the GPU did for <secs>
gpucnt exec <label> <cmd> [args...]    # while <cmd> runs, e.g. one program's shadercheck -t
gpucnt probe                           # the kernel interface (below, "Mesa's Panfrost ...")
```

Each counter is printed with Arm's name for the Mali-G31 (the counter database of Arm's
libGPUCounters, MIT, as copied into Mesa), then the figures Arm derives from them for this GPU: the
share of the shader core's active cycles in which the arithmetic unit issued (`EXEC_INSTR_COUNT`),
the texture unit filtered (`TEX_FILT_NUM_OPERATIONS`), the varying unit interpolated (`VARY_SLOT_32`
+ `VARY_SLOT_16`), the load/store unit worked; per fragment warp (4 pixels on the G31) the core
cycles and those issues; tiles written and eliminated (transaction elimination, the CRC check that
skips a tile write whose contents did not change) and the bytes per written tile; external bus
traffic (beats of the bus width `L2_FEATURES` gives: 128 bits here). In a bench run `GPUCNT=1`
samples the measurement (`profbench.sh` sends `runtime/host/out/gpucnt`, `GPUCNT_BIN` another
build; `<run>/gpucnt.txt`, and `benchsum.py` prints the derived lines).

Reading the G31's counters (checked with programs of known code, below): `EXEC_INSTR_COUNT` counts
one issue per two tuples of a warp's code (a program of 110 tuples: 54.9 per warp, and 54.9 core
cycles per warp: the core issues two tuples a cycle), `FRAG_WARPS` is quads of pixels, a 32-bit
varying slot is two components (a highp `vec4` is 2 slots, a mediump one 1 16-bit slot).

**What a fragment costs on the G31, with Mali's driver** (`gpucnt exec` around `shadercheck -t` of
synthetic programs, 720x480 blended quads; per warp of 4 pixels; the driver's code read from its
program binary, below):

| fragment shader | driver's code (tuples) | core cycles per warp | arithmetic / varying / texture issue share |
|---|---|---|---|
| constant colour | 4 | 2.1 | 95 / 0 / 0 % |
| one varying: highp `float`, `vec2`, `vec4`, or mediump `vec4` | 2 - 4 (the varying preloaded) | 5.1 - 5.3 | 19 - 39 / 19 - 37 / 0 % |
| a texture at a `vec2` varying | 2 (both preloaded) | 5.2 | 19 / 19 / 38 % |
| two textures at one varying | 7 | 5.5 | 63 / 18 / 72 % |
| two `vec2` varyings | 5 | 9.0 | 28 / 22 / 0 % |
| a sprite: `vec2` uv, `vec4` colour, texture (or the uv in a `vec4`) | 8 | 9.4 (9.3) | 43 / 32 / 21 % |
| five varyings (2, 3, 3, 4, 3 components: Hollow Knight's lit sprites' shape) and a texture | 17 | 14.0 | 61 / 64 / 14 % |
| the same 15 components packed into four `vec4`s | 18 | 12.5 | 72 / 64 / 16 % |
| 16 x (vec4 multiply-add, fract) on one varying | 110 | 54.9 | 100 / 4 / 0 % |

A sprite's fragment is not limited by any one unit: each varying the shader reads costs about 3.5 -
4 cycles a warp, whatever its width and precision, preloaded or not (no unit is busy for that long:
it looks like each message's latency, with few warps in flight to hide it), a second texture at a uv already interpolated about
0.3, arithmetic one cycle per two tuples, and a fragment's fixed part (rasterization, the alpha test
and blend messages, the tile) about 2. That is shaders.md's table (a varying or a texture fetch ~0.3
ms per full-screen layer, precision barely matters) seen from inside: only fewer varyings would make
a sprite cheaper (packing the same values into fewer `vec4`s: -11 % for the lit-sprite shape above;
interpolation is per component, so the values should be the same; not built, not checked on pixels).

## Mesa's Panfrost instead of Mali's driver? (evaluated 2026-10-23: not pursued)

Every Unity game runs on the RG34XX-SP through Arm's 2019 GLES driver (libmali r20p0) and its shader
compiler. Other firmwares for the H700 run Mesa's Panfrost on the upstream `panfrost` kernel driver.
Could Mesa's user-space driver run on this kernel's own driver (Arm's kbase), shipped inside a port
with no kernel or system change, and would it be faster?

**The kernel side** (read on the handheld; `gpucnt probe`). No DRM device at all (`/dev/dri` does not
exist: the display is sunxi's fbdev). `/dev/mali0` is kbase `r20p0-01rel0`, UK interface 11.17 (the
version handshake answers 11.17; `KBASE_IOCTL_GET_DDK_VERSION` `K:r20p0-01rel0(GPL)`), a Job Manager
GPU: Mali-G31 r0p0 (`RAW_GPU_ID` 0x70930000), one shader core, one 64 KB L2 slice with 64-byte lines
and a 128-bit bus (`RAW_L2_FEATURES` 0x07100206), 3 job slots, 8 address spaces, 33-bit GPU VAs,
696 MHz at most. From a user process: the version check, `SET_FLAGS`, the GPU properties and
`JOB_SUBMIT` work as they are (one no-op atom in UK 11.17's 48-byte `base_jd_atom_v2`, its event read
back from the fd 0.03 - 0.04 ms later), the hardware counters too (above); memory once the context
has its tracking page mapped (`BASE_MEM_MAP_TRACKING_HANDLE`; before that `MEM_ALLOC` refuses):
a 32-bit process gets SAME_VA allocations (a cookie, then `mmap` places it, CPU and GPU address
alike); without SAME_VA the GPU address lies above 4 GB, out of a 32-bit `mmap` offset's reach;
`MEM_JIT_INIT` in 11.5's layout is accepted, `MEM_EXEC_INIT` with 1M pages refused (ENOMEM in a
32-bit context). The blob itself draws the window straight into the framebuffer (fb0 exported as a
dma-buf, `FBIOGET_DMABUF`, imported into kbase) and pans to it, which runtime/host/fbdev.c already
takes over.

**Past work.** Icecream95 (a Panfrost developer) wrote a kbase backend for Mesa in 2021-2022, never
merged (his fork on Mesa's GitLab, branches `kbase-shim`, `kbase-g52`, `kbase-t860`, `csf`):
`src/panfrost/base` drives three kbase ABIs from one source (the legacy UK 3 of Midgard kernels, the
JM UK 11 of this one, and CSF), with allocation, dma-buf import, job submission (vertex/tiler and
fragment chains as atoms with order dependencies from an emulated implicit sync), completion events
and a user-space emulation of the DRM sync objects, and Gallium Panfrost patched to open
`/dev/mali0` (the kbase layer itself is 1,570 lines, besides the imported kernel headers and the
Gallium and EGL changes), on Mesa 22.2-devel. Mesa has since gained `pan_kmod` (24.0+: backends
for the `panfrost` and `panthor` kernel drivers), which abstracts device properties, buffer objects
and VMs but not submission or sync: Gallium's JM path (`pan_jm.c`) still calls
`DRM_IOCTL_PANFROST_SUBMIT` and uses DRM sync objects in about 20 places.

**What a port of Mesa 26.2 would need**: a kbase `pan_kmod` backend (properties from
`GET_GPUPROPS`; buffers as kbase allocations behind a handle table; the tiler heap as a
grow-on-fault allocation; dma-buf import; Icecream95's code is the model, its headers are UK 11.35's
and need 11.17's atom), the JM submit and a sync-object emulation in Gallium, device probing without
DRM (the pipe loader and EGL's surfaceless platform look for render nodes), and a window: Mesa has
no fbdev EGL platform, so either Unity gets a pbuffer and the host blits it into an EGLImage of
fb0's dma-buf at each swap (one more full-screen copy pass, about 1 ms on this GPU: Hollow Knight's
own copy into the window is 1.2 ms) or a small fbdev platform in Mesa renders into fb0's buffers
directly. AFBC would be a guess (kbase r20 does not report `AFBC_FEATURES`; Icecream95 assumed it
works). Mesa's Panfrost has what Unity asks of the driver on this GPU (OpenGL ES 3.2 / ESSL 320 on
v6+, ASTC LDR and ETC2, coherent `EXT_shader_framebuffer_fetch`, fence sync, program binaries),
conformant on the Mali-G52 (the same v7 architecture), not certified on the G31. Building it needs
meson, ninja and Mako, which neither this checkout's Docker images nor the Mac have.

**Would it be faster?** Every game below 60 fps here is held by the GPU's fragment work, with the
GPU busy all the frame, so the driver's CPU cost does not decide the frame rate; the GPU's time does.
Which of the GPU's units hold it, with the blob (`GPUCNT=1`, one run each after the lock's cool-down,
2026-10-24):

| | King's Pass (64-bit), 33.7 fps, GPU 600 / 696 MHz 82 / 18 % | Phasing Gun, first scene, 11.7 fps, GPU 696 MHz |
|---|---|---|
| GPU active / shader core active | 99.9 % / 99.3 % of it | 100 % / 79.7 % (the rest: its 2048x2048 shadow map's depth-only pass) |
| issue shares of the core's cycles: arithmetic / texture / varying / load-store | **80.8** / 38.2 / 28.7 / 0.6 % | **67.5** / 11.9 / 2.7 / 7.4 % |
| per frame | 18.3 M core cycles, 2.0 M fragment warps (24 layers of 720x450), 29.5 M tuples | 47.3 M core cycles, 0.6 M warps, 64 M tuples |
| tiles | 20,100 a frame, 28 % eliminated, 991 bytes per written tile | 76,200 a frame (the shadow map's 16,384 among them), 10 % eliminated |
| external memory | 29 MB read, 15 MB written a frame (1.5 GB/s) | 24 MB read, 73 MB written a frame (1.1 GB/s) |

Both frames are arithmetic-heavy (King's Pass's lit and blend-mode sprites, text, bloom; Phasing
Gun's SSAO and lit passes): the arithmetic unit is the busiest unit, so the code a compiler makes
decides much of the frame, and the comparison that matters is Mesa's code against the blob's:

1. *The compilers, shader by shader.* Mesa 26.2.5's Bifrost compiler run offline on the GLSL the
   games compile (`UA_SHADER_DUMP`): Mesa's GLSL front end, NIR and Bifrost back end built without
   meson (the generators run with a small stand-in for Mako), driven the way st/mesa and Gallium
   Panfrost prepare a program for a Mali-G31, printing Mesa's shader-db statistics and code. Against
   it the blob's real code: `glGetProgramBinary` of the same programs on the handheld (its Bifrost
   code is in the binary's `OBJC` chunks; Mesa's own disassembler reads it), and the counters above
   for what the blob's code costs per warp. Hollow Knight's King's Pass, its 25 programs (tuples of
   the fragment shader; the blob's first variant, the one that runs):

   | program (King's Pass) | Mesa | blob | blob, cycles a warp (issue shares: arithmetic / texture / varying) |
   |---|---|---|---|
   | sprites (`Sprites/Default`-style: uv, colour, texture) | 10 tuples, 3 clauses | 8, 2 (varying and texture preloaded) | 8.8 - 9.0 (45 / 22 / 34 %) |
   | additive sprites, particles | 13, 3 | 11, 2 | 8.9 (62 / 22 / 34 %) |
   | `Sprites/Lit` (most of the level's art; both counts with its light-probe branch) | 57, 11 | 51, 9 | not measured (`shadercheck` drew nothing with it until its sampler fix) |
   | background blur passes (5 taps) | 25, 7 | 21, 8 | 20 - 21 |
   | blend-mode sprites (screen, overlay, vivid light) | 31, 25, 31 | 40, 31, 32 | 20.2, 15.8, 16.9 (94 - 99 % arithmetic) |
   | bloom blur (7 taps) | 37, 9 | 38, 12 | 19.5 (97 % arithmetic) |
   | colour correction (`_RgbTex` curves, dependent lookups) | 13, 6 | 9, 5 | 34.7 (13 % arithmetic: latency) |
   | TextMesh Pro text | 74, 14 | 63, 14 | 33.1 (95 % arithmetic) |
   | the other 12 (more sprite shaders, copies, HUD, black bars, brightness, the darkness overlay) | 2 - 25 | 2 - 26: the same or fewer in 11 | |

   The blob's code is as short as Mesa's or shorter for 19 of the 25 programs; it preloads varyings
   and the first texture fetch into registers before the shader starts (v7's message preload) more
   than Mesa does. Mesa's is shorter for six: by 9 and 6 tuples in two of the blend-mode sprites (of
   40 and 31), by one in the third, the two bloom passes and the darkness overlay. The blend-mode
   sprites are arithmetic-bound: if they covered the whole picture once each, Mesa's code would save
   about 0.6 ms of the 29 ms frame, against longer code in the sprites, the lit sprites, the blur and
   the text.
2. *The URP games' programs* (Phasing Gun's 14 and The Third Eye's 17 traced programs; fragment
   tuples, Mesa / blob):

   | program | Mesa | blob | Mesa's code |
   |---|---|---|---|
   | Phasing Gun: URP's SSAO (one full-screen draw, 40 ms of the 89 ms frame) | 698 (100 clauses, half the threads: 29 registers) | 574 (84 clauses) | +22 % |
   | Phasing Gun: SSAO's three blur passes (4.5 ms each; 92 % arithmetic) | 77, 77, 68 | 61, 61, 56 | +21 - 26 % |
   | Phasing Gun: URP's lit shader, two variants (the main pass, 14 ms) | 1,499, 1,480 (24 spills) | 1,192, 1,173 | +26 % |
   | The Third Eye: post-processing (uber pass, colour grading LUT) | 336, 955 | 298, 814 | +13 %, +17 % |
   | The Third Eye: lit shaders | 301, 497, 484 | 259, 388, 367 | +16 - 32 % |
   | the small ones (copies, blits, depth) of both | 5 - 52 | 3 - 43 | about the same |

   On top of the length, Gallium Panfrost pushes only the default uniform block (and its sysvals)
   into the shader core's uniform registers, so URP's uniform blocks (`UnityPerDraw`,
   `UnityPerMaterial`, the lights) are read with memory loads: Mesa's estimate for the lit shader is
   load/store-bound (129 normalized load/store cycles against 54 arithmetic); the blob's lit shader
   has 47 - 59 loads.
3. *Tiles and memory.* The blob writes render targets uncompressed (1,040 bytes per 16x16 RGBA8
   tile in `shadercheck`'s target: no AFBC) and eliminates unchanged tiles (transaction elimination
   is on for textures too). Mesa would use AFBC where the GPU has it; on the G31 that is unknown
   without the kernel's register (kbase r20 does not report it), and the passes' loads and stores
   are Unity's (the same in any driver).

**Verdict.** The kernel interface is reachable and Mesa could be made to run on it, but at the size of
a driver port (a kbase backend, JM submission and sync in Gallium, a window system path), on a GPU
Panfrost is not conformant on, with memory a game like Hollow Knight does not have to spare; and the
measurements point the other way: the GPU-bound frames are arithmetic-heavy (81 % and 68 % of the
shader core's cycles), and the blob's 2019 compiler makes shorter code than Mesa 26.2's for nearly
every program that matters (the sprites by 2 tuples of 8 - 11, the URP passes by 13 - 32 %; Mesa's is
shorter in 6 of Hollow Knight's 25, by more than one tuple in two blend-mode shaders); where a sprite
is not arithmetic-bound its cost is the per-varying cost of the hardware, the same under any driver.
Phasing Gun's SSAO pass, 40 ms of arithmetic-heavy work, would get 22 % more code. Step 3 of the plan
(a prototype on the handheld) was therefore not started.

How these were made (none of it is in this repository): Mesa 26.2.5 from Mesa's GitLab; its code
generators run with a ~300-line stand-in for Mako's template subset they use, bison 3 and flex for
the GLSL parser; about 490 sources compiled with a generated Makefile in the `ua-build-arm64` image;
a small driver program around Mesa's standalone GLSL compiler (with the Bifrost NIR options and
ES 3.x limits) that applies st/mesa's uniform, uniform-block and sampler lowering and Gallium
Panfrost's CSO and variant steps before `pan_shader_compile`, viewport and noperspective sysvals as
constants (Gallium pushes them: free on Bifrost). Its shader-db numbers are an approximation of
Gallium's (no shader keys, no sysval UBO); the counts compared are the code itself. The blob's
programs: `glGetProgramBinary` (format 0x8f61) on the handheld; its `OBJC` chunks hold the Bifrost
code; the first fragment chunk is the one that runs (the counters' issues per warp match its length).

Usable without Mesa: the counter tool above; the varying cost (packing a shader's varyings into
fewer `vec4`s saved 11 % in the synthetic lit-sprite shape; interpolation is per component, so it
should give the same values: not built, not checked on pixels); the blob's environment variables
(its strings) are debugging only in this release build (`MALI_SAVE_FRAMES_TO_FILE`,
`MALI_REPORT_MEM_USAGE`, `MALI_TIMELINE_*`, `MALI_HWC_DUMP_*`, `MALI_PREROTATE`,
`MALI_CROP_RECTANGLE`, `MALI_CONFIG` / `.mali_config` for its debug channels), but for
`MALI_SCHED_RT_THREAD_PRIORITY`, by its name and place a real-time priority for the back-end thread
(not tried: the nice -10 test above showed that thread's priority does not pay in a GPU-bound game).


### Passes left out (glpass.c, glrename.c)

What each candidate could gain was measured first, with a measurement build that leaves the work
out whether or not that is right (King's Pass, the program cache off in every run, the GPU at 600
MHz in all of them, one run each, interleaved):

| left out (a measurement: the picture is wrong) | fps |
|---|---|
| nothing (first and last run) | 32.4, 32.5 |
| the three grab copies (the sprites read a stale grab texture) | 34.6 (+6.7 %) |
| the same with a `glFlush` in their place: the camera still stores its picture and loads it again, as it does when the copies are renamed (below) | 34.8 (+7.4 %) |
| the blur's copy back into the background camera's target (its pass stays: Unity also clears depth/stencil in it) | 32.4 (0) |
| the clear of the 720x450 texture nothing draws into (a pass that only clears) | 32.2 (-0.7 %) |
| the two black bars drawn into the window (their pass) | 32.6 (+0.5 %) |

So a grab copy costs about what its own pass costs, not the camera's store and reload around it,
and the passes that only clear or draw the two bars cost nothing that shows. What each scheme does
per grab (720x450 RGBA8, 1.3 MB):

| | passes | written | read |
|---|---|---|---|
| Unity's copy: the camera stores A; the copy pass loads C, samples A, stores C; the camera's next pass loads A | 2 | 2.6 MB | 3.9 MB |
| renamed: the camera stores A; its next pass, on C's old storage, starts with a texel copy of A | 1 | 1.3 MB | 1.3 MB |

Built ([runtime.md](runtime.md)), both off by default:

- **`UA_GL_SAMECLEAR`** (glpass.c, glopt.c): a clear that writes what the buffer already holds is
  held back (colour) or left out (depth/stencil). Exact by construction; `UA_GL_SAMECLEAR=check` in
  the emulated rig (the 64-bit build to King's Pass): 21,430 such clears, in the menus and in King's
  Pass, made and their image read back before and after: none changed a pixel. On the handheld
  (two runs each, interleaved, each from 50 C; 1,638 clears left out in 2,400 frames): of the
  pixels static within each run, 553 - 1,112 of about 305,000 differ between a run with it and one
  without, as many as between two runs without it (700) or two with it (1,159): the particles and
  plants. Cuphead's title and Petty Puny Planet's planet (59.3 / 59.4 and 59.4 / 59.4 fps without
  / with it) have no such clear. It gains nothing in
  King's Pass (the clear-only pass, above), so it is opt-in; with the rename it also removes the
  blur's copy-back pass, whose only other command is a depth/stencil clear to the values that
  buffer holds.
- **`UA_GL_RENAME`** (glrename.c): the copy's texture name gets the source's storage, the source's
  name the copy's, which gets the picture in the next pass that draws into it (one texelFetch
  triangle in place of the tile load) or never if it is replaced whole first; a draw counts as a
  copy only once measured to be one, on random noise, per set of inputs. In the rig (Mesa's
  llvmpipe) Unity's copies are exact: King's Pass draws 19 passes instead of 24 (with
  `UA_GL_SAMECLEAR`; three grab copies, the copy back and the clear-only pass gone), 5 copies left
  out a frame, and `UA_GL_RENAME=check` compared 135,901 such copies with their source in the game:
  none differed. **On the Mali-G31 none of them is exact**, so nothing is left out (and nothing
  gained: 33.0 / 33.1 fps against 33.7 / 33.5, the GPU at 606 against 617 MHz on average). The probe
  on noise: Unity's grab blit (its fragment stage is `mediump`) differs on 323,978 of 324,000
  pixels, 2 % of the channels by more than 15; the image effects' blit (`highp`, times `_Color`)
  on 206,062 of 207,360, by 1 - 15 (bilinear weights a little off the texel centres). In the game
  the grab copy differs from the picture it copies on 869 - 1,470 of 324,000 pixels by 1 - 3 (and
  the blit gave its source unchanged): the vanilla picture on this device has that in it, so giving
  the sprites the source itself would not be exact. `UA_GL_RENAME=near` (opt-in, **not exact**)
  takes such near copies too. King's Pass with `UA_GL_RENAME=near UA_GL_SAMECLEAR=1`, two runs each,
  interleaved, each from 50 C: **33.3 / 33.9 -> 36.0 / 36.5 fps** (+8 - 10 %; the GPU at 612 / 631
  against 634 / 638 MHz on average, so +4 - 7 % per MHz), 5 copies left out a frame (the three grab
  copies, the copy back, and the brightness pass, which at the game's default brightness is close to
  a copy), every renamed picture given in a pass, no blit. The picture: of the pixels static within
  each run, 8,797 - 9,453 of about 303,000 differ from a run without it, 77 % of them by 1, most of
  the rest by 2 - 3, at most 12 (a blend mode amplifying the grab's difference); two runs without
  it differ on 1,874 (particles). Opt-in, like `shaders.grab_fetch` (+8 %, 1.6 % of the static pixels
  off by 1 - 6), and for the same reason.

Not built:

- **Memoising the background camera and its blur** (about 7.7 ms when its inputs repeat): the
  blurred background (576x360, after the copy back) read back every frame and hashed, in the rig: the
  same as the frame before in 0 of 1,200 frames standing in King's Pass, 0 of 1,200 walking, 4 of
  about 8,000 in the menus. The background layer has particles (its 60-quad particle batch has 354
  or 360 indices from frame to frame even standing): nothing repeats.
- **The black bars' pass deferred into the window's copy**: at most what leaving the bars out gives,
  +0.5 %, within noise; doing it exactly would mean recording two draws with all their state and
  vertex data for later.
- **The render thread's waits.** In the 32-bit build's 500 Hz profile (31 fps, symbols named) the
  render thread is blocked 9.6 ms a frame: 6.0 ms in `FrameTimingManager::RenderThreadWaitCallback`
  (Unity's render thread waiting for the GPU to finish an earlier frame) and 2.9 ms in futexes inside
  Mali's driver; on a CPU it spends 18 ms a frame, half of it in the driver. With the GPU at the
  thermal cap 99 - 100 % of the measured time and the per-pass GPU times adding up to the frame,
  these waits are the GPU's backpressure, not stalls that leave it idle: a change of the call
  pattern cannot give the frame more GPU time; only less GPU work can.

## Holding the whole frame (UA_GL_REC)

The question (2026-10-23): a tiler pays for every pass (a load of each attachment it does not clear
or invalidate first, a store of each at the pass's end) and the driver sees the frame one call at a
time. ua_host sits between Unity and the driver: holding a whole frame's calls until the swap, it
would know, for each pass, what comes after it, and could submit an equivalent stream that is
cheaper: passes whose results are overwritten before anything reads them left out, invalidates
where an attachment is not read again (no store) or not needed at a pass's start (no load), passes
on one framebuffer separated by independent ones submitted next to each other. Measured first from
the traces, then built (`UA_GL_REC`, [runtime.md](runtime.md)) and measured on the handheld. In
short: holding the frame costs nothing that can be measured; the frame graph finds little to change
in these games: White Mirror +2.2 % (two clear-only passes a frame made part of the passes after
them), Hollow Knight and the others nothing. Off by default.

### What the traced frames could give (framegraph.py)

`tools/analysis/framegraph.py <UA_GL_TRACE_DRAWS run.log> --gpu <UA_GL_TRACE_GPU run.log>` reads a
traced frame's passes and draws and lists what a replay could do without changing a pixel: dead
passes; stores nobody needs (an attachment not read before a later whole clear or invalidate in the
frame, or before the swap for the window's depth/stencil); depth/stencil loads a pass does not test
and nothing needs afterwards; passes on one framebuffer with no chain of dependencies through the
passes between them (textures sampled, blit and copy sources, attachments loaded and written). A
texture keeps its contents into the next frame, so only what is overwritten within the frame
counts; a colour load always counts as needed (whether a pass's draws cover every pixel is not in
the trace).

| game (scene, traced on the RG34XX-SP) | passes | what a replay could change |
|---|---|---|
| Hollow Knight (King's Pass, 64-bit) | 24 | the two window passes (the black bars, the final copy) could be one: nothing between them reads the window; the window's depth/stencil is written by the bars, never tested, and loaded by the final pass; one blur target's depth is stored and invalidated by the next pass on it. Nothing is dead: every texture written is read in the frame |
| White Mirror (in game, two cameras) | 22 | one dead pass a frame: a 360x120 copy of the camera's colour that nothing samples (URP's downsampled opaque texture), written again by the second camera before anything reads it (the second copy is read by nothing either, but only the next frame could show that); the camera's target cleared in a pass of its own, then a 4x4 clear on another framebuffer, then the drawing pass on the first: the clear can go into the drawing pass (twice a frame); the second window pass loads the window's depth/stencil without testing it |
| The Third Eye (first scene) | 35 | nothing: URP invalidates every target at its passes' starts; the two cameras' 1x1 depth targets are cleared and sampled (empty shadow maps) |
| Phasing Gun, Chapéu de Papel, Cell Machine, Arthur's Nightmare | 9, 3, 22, 7 | nothing (Chapéu's cleared colour target is sampled by the scene's draws); at most the window's depth/stencil at the frame's end, which the swap discards anyway |
| Cuphead (Forest Follies, 64-bit) | 4 | nothing: the scene, the two film effects, the copy into the window, each read by the next |

These frames' GPU time is shading (Hollow Knight's scene sub-passes, the URP games' SSAO, bloom and
uber passes, Phasing Gun's 2048x2048 shadow map and its screen-space pass), not what a replay can
leave out. The one merge of passes with draws (Hollow Knight's two window passes) is not done here:
it needs the GL state moved with the pass, and it is one game's case.

### What a pass costs on the Mali-G31 (passbench)

`runtime/host/passbench [seconds] [width height] [ballast ms]` (a device check, built with the host;
EmulationStation stopped, as the bench does): frames of 16 passes on offscreen targets of 720x450
(RGBA8, a D24S8 texture), alternating the target measured with a colour-only one cleared and
invalidated, one 16x16 quad each unless the case says otherwise, run back to back (a `glFinish`
every 8 frames) behind a ballast pass of about 4 ms that keeps the GPU busy, so its governor holds
696 MHz; each case's time a frame against the ballast's alone. Read the differences between cases:
the absolute times carry a constant that the frames' mix adds (about 22 ms with any case, not
understood), the same in every case. 2026-10-24, 696 MHz throughout:

| what (8 passes a frame of the target measured) | cost |
|---|---|
| a pass that clears and invalidates its attachments (16 a frame: 8 or 32 make no difference) | nothing measurable on the GPU (its submission, about 25 us of CPU) |
| a colour store of a 720x450 target whose tiles all changed | 0.18 ms (0.56 ns a pixel) |
| a colour load of it (the store then mostly skipped: unchanged tiles are not written) | 0.19 ms |
| a depth/stencil store (the buffer cleared, never written) / its load, tested or not | 0.035 ms / nothing measurable |
| eight quads in one pass instead of eight passes (each with a load and a store) | 1.25 ms less: 0.18 ms a pass |
| a full-screen copy into the target, stored / its output invalidated | 0.30 ms / nothing (the driver skips a pass whose results are all invalidated) |

What a replay can save is therefore colour traffic, about 0.2 ms per 720x450 load or store a frame,
and nearly nothing in depth/stencil (as invalidating Hollow Knight's depth/stencil when its camera
leaves the framebuffer gave nothing over `UA_GL_DSCLEAR`; that `UA_GL_DSCLEAR` itself gained 1.8 ms a
frame is not explained by these loads). A pass whose attachments are all invalidated already costs
nothing. By these costs `framegraph.py` puts White Mirror's changes at about 0.6 ms a frame (2.5 %),
Hollow Knight's at 0.5 ms (0.44 of it the two window passes made one), the others' at 0.03 ms.

### The recorder (UA_GL_REC)

What a frame asks of the driver, `UA_GL_REC=census` (Hollow Knight, King's Pass): about 2,000 GL calls
a frame, the most `glVertexAttribPointer` (282), `glBindTexture` (198), `glBindBuffer` (192),
`glUniform4fv` (153), and 144 each of `glMapBufferRange`, `glFlushMappedBufferRange` and
`glUnmapBuffer`: Unity writes its dynamic geometry through a map of its vertex buffer for nearly
every draw (90 draws; White Mirror: 44 maps for 25 draws). A call that returns something ends what a
recorder can hold, so one that made the maps at once held one draw at a time (150 submissions a
frame, measured: the frame graph then saw single passes); holding the maps too (the game writes host
memory, the flushed ranges go into the real mapping when it is submitted) leaves one held frame a
frame, submitted at the swap: 2,000 calls, 69 KB of commands and 136 KB of data in Hollow Knight,
720 calls in White Mirror. What else ends a held frame: one `glFenceSync` a frame (a wait on a fence
or its deletion needs nothing submitted: a fence is only made once what came before it is in the
driver), texture definitions, shader and program management.

On the RG34XX-SP (Hollow Knight 64-bit, runs one after another from a warm handheld, 2026-10-23; White
Mirror 32-bit, every run from 47 - 48 C, 2026-10-24):

| game, run | fps | GPU clock | render thread (% of a core) | frame graph |
|---|---|---|---|---|
| Hollow Knight, baseline (warm start) | 32.6, 32.8 | 600 MHz 96 - 99 % | 62, 61 | |
| census | 32.5 | 600 MHz 97 % | 63 | |
| recorder, maps made at once (150 submissions a frame) | 32.3 | 600 MHz 95 % | 65 | |
| frame graph, maps made at once | 31.8 | 600 MHz 87 % | 72 | the graph's work 150 times a frame |
| frame graph, maps held (one held frame a frame) | 32.8 | 600 MHz 94 % | 58 | 0.3 ms a frame |
| White Mirror, baseline (cool start) | 42.8, 42.7; 42.7, 42.8 | 696 MHz 99 - 100 % | 40, 41; 40, 40 | |
| frame graph, clear passes not yet sunk (a check too strict, since fixed) | 43.1, 43.0 | 696 MHz 100 % | 40, 40 | 0.17 ms a frame |
| frame graph | 43.7, 43.7 | 696 MHz 100 % | 39, 39 | 0.16 ms a frame |

Holding the whole frame and submitting it at the swap costs nothing that shows: the GPU starts each
frame later, but it is still busy with the one before. In Hollow Knight the frame graph finds every
frame what the trace analysis found: 24 passes, nothing dead, nothing to sink; the window's
depth/stencil invalidated at the end of the bars' pass and at the start and end of the final copy,
a blur target's depth at its end (two end and one start invalidates a frame). They save depth/stencil
traffic, which costs the Mali little (passbench above), and the frame rate does not move. In White
Mirror it sinks the two clear-only passes into the passes that draw on their framebuffer (each
camera's target: a 720x240 store and a load fewer, twice), invalidates four attachments at their
pass's end (three depth/stencil, and the colour of the dead opaque-texture copy, which the driver
then skips: passbench's last line) and two depth/stencil buffers at a pass's start: 42.7 - 42.8 ->
43.7 fps (+2.2 %, two runs each, interleaved, every run from 48 C at 696 MHz throughout); the
invalidates alone gave +0.7 %. The copy is not left out: the game binds a shader storage buffer or
an image, and after that the frame graph leaves no pass out (a draw could write through it); its
invalidated output costs nothing anyway.

Pictures. Of the pixels static in both runs compared (two screenshots a second apart in each run):
Hollow Knight, baseline against the frame graph 0.73 % differ (1 - 2 of 255 for 96 % of them, at most
43), two baseline runs 0.66 % (at most 67), the frame graph against the second baseline 0.55 %: the
same, at the edges of the scene's animated foliage and particles. White Mirror, two baseline runs: 0
and 22 pixels differ; the frame graph against a baseline: 1, 15, 45 and 143 pixels, in the game's
on-screen clock (its digits and their reflection) or within a few levels of 255 beside them: runs that
reach the screenshot at another moment of the game's clock.

Cuphead (Forest Follies, 64-bit, the port's test hooks): 4 passes (the scene, the two film effects,
the copy into the window), nothing to change, and the level runs 55.6 fps with UnityMain 97 % busy:
not GPU-bound.

What did not pay, or was not done:

| change | result |
|---|---|
| holding the frame (`UA_GL_REC=1`), maps made at once | Hollow Knight 32.3 against 32.6 fps, render thread +3 % of a core: no gain possible, a small cost |
| the frame graph's invalidates (depth/stencil at pass ends and starts, a dead colour at its end) | Hollow Knight 32.8 against 32.8, White Mirror 43.0 - 43.1 against 42.7 - 42.8: noise to +0.7 % (the clear-pass sinking is what gives White Mirror its +2.2 %) |
| merging two passes on one framebuffer that have draws (Hollow Knight's two window passes: about 0.4 ms by passbench's costs) | not built: the moved pass needs the GL state of its original place (bindings, uniforms of shared programs, vertex arrays), and it is one game's pattern |
| a default | none: off by default. Exact by construction (the fake driver's tests; pictures within the run-to-run floor), but of the seven games measured one gains (White Mirror +2.2 %), and a wrapper for every GL entry point is a large layer to put in front of every game: a candidate for a default only after the regression rig has run every game with it |
