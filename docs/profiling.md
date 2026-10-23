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
| the blend-mode shaders read the framebuffer instead (`GL_ARM_shader_framebuffer_fetch`, `gl_LastFragColorARM`, through `UA_SHADER_OVERRIDE`) with the grab copies left out | menu 41.8 (+16 % over no change); King's Pass 32.4 -> 35.0 - 35.3 (+8 % over `UA_GL_DSCLEAR`), screenshots that differ no more than two of the same run a second apart. Not exact: sprites of one batch that overlap see each other (with the grab, the picture before the batch), and the fetched colour is mediump; a data change (the shader's `GrabPass` removed, its GLSL rewritten) would be needed for a real option |
| a window without depth/stencil (an EGL config with none; the two bars write the window's depth, nothing reads it; Android's "Disable Depth and Stencil" player setting does the same) | King's Pass 32.6 -> 32.8 (two runs each): noise |
| GL_EXT_disjoint_timer_query per pass | the driver's answers cannot be right (0 ms for most of the scene's passes): `UA_GL_TRACE_GPU=3` instead |

