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
function's prologue or epilogue can stop the unwind early (the tables describe the body). Blocked samples have
only sp and pc (from `/proc`): their callers are always a scan. Mono's trampolines and code it has
no information for are "anonymous code". Libraries the profile names only by exported symbols (a
stripped libc or GLES driver) are named by the nearest exported function, which is often not the
function itself.

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

On the RG34XX-SP the game runs King's Pass at 32 - 34 fps with UnityMain busy the whole frame. The
saved game continues from the title screen with two presses (Switch layout: `b` confirms), and the
title and the save load are frames of several seconds, so the plan needs no log lines:

```sh
export DEVICE=root@<handheld> UA_PAD_NAME='Nintendo Switch Pro Controller'
export PLAN='hitch:5000; steady:3; press:0:b; sleep:6; press:0:b; hitch:2000; steady:5'
tools/device/profbench.sh /userdata/uatest/hollowknight --list hk-session.txt
```

`hk-session.txt`, about two and a half minutes a run:

```
# baseline, and the profiler's own cost
base
prof      UA_PROF=500 MEASURE=20
# where the frame goes, including the job threads
prof-all  UA_PROF=250 MEASURE=20 UA_PROF_THREADS='UnityMain,UnityGfxDeviceW,Job.Worker*,FMOD*'
# what the frame costs without the vsync wait (a measurement: tears on fbdev)
novsync   UA_SWAP_INTERVAL=0 UA_FRAME_RATE=200
novsync-prof UA_SWAP_INTERVAL=0 UA_FRAME_RATE=200 UA_PROF=500 MEASURE=20
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
