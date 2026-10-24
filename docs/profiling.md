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
   time); each core's and the GPU's clock every second; with `UA_PROF=<hz>` a profile of the same
   seconds;
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
# FMOD's mixer wakes half as often (more output latency, the same sound)
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
| `audio-1024` | what FMOD's mixer thread costs (its CPU in the thread table) when it runs every 1024 frames instead of 512 |

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

