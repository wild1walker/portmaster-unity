# Performance: the paths that paid, in the order to take them

What a port of a heavy Unity game to a 1 GB, 4 x Cortex-A53 handheld (the RG34XX-SP: H700,
Mali-G31, 720x480) should check and set first, learned on Hollow Knight (Unity 6000.0.61f1, Mono,
2D with many layered sprites) and Cuphead, with what each step bought and what did not pay. The
measurements and tools behind each line are in [profiling.md](profiling.md), [runtime.md](runtime.md),
[shaders.md](shaders.md) and [quality.md](quality.md); this page is the map.

## 1. Measure on the handheld, in the game's heaviest places

- **Benches walk, they do not stand.** A standing spot overstates real play. Drive the game with a
  plan (`tools/device/profbench.sh`, `PLAN=`): continue a save, walk with attacks and jumps
  (`UA_INPUT_SCRIPT` repeats, `0.7@0.6:y~0.1`: adb's shell takes at most ~4 KB of command, so long
  press lists fail to start), screenshot along the way (`shot`), and read the `[fps]` and `[hitch]`
  lines of the walk only.
- **Go where the game is heaviest.** One starting save is not enough: give the test copy saves
  that start in the dense areas and boss rooms. Most games keep their save as a serialized object
  with a "respawn scene" and a "respawn marker"; Hollow Knight's is AES-ECB with the key in its
  own `TeamCherry.SharedUtils.Encryption`, and any object tagged `RespawnPoint` in a scene can be the
  marker (benches, "Death Respawn Marker"). Hollow Knight's survey: open areas 48-55 fps, Greenpath
  37, City of Tears 42, Soul Master 42 - the averages of the first area hide the worst.
- **Every run starts cool.** At about 68 C the Mali drops from 696 to 600 MHz, hot muOS runs went to
  408-504; the shared device lock waits for 50 C (`devlock.py`, one lock per handheld:
  `DEVLOCK_DEVICE=muos` for the SSH one). Read each run's GPU clock from `freq.log`.
- **Know the firmware.** Knulli's CPU tops out at 1512 MHz, muOS's at 1800 (the same H700): CPU-bound
  scenes differ by that much. The thermal governor shares one power budget between CPU and GPU:
  CPU work lowers the GPU clock.
- **Name everything.** `pmu player fetch <version> <dir> --backend mono --abi armeabi-v7a
  --installer <Android support .exe> --symbols-only` takes Unity's own symbol files from the
  installer (the Windows one is the smaller: 501 MB for 6000.0.61f1);
  `tools/analysis/profsum.py prof-1.txt --syms <dir>/Variations/mono/Release/Symbols/armeabi-v7a`
  then names every libunity frame of a `UA_PROF` recording, Mono names the managed ones.

## 2. Memory first on 1 GB

- **Count the GPU too.** The `[fps]` line's `rss` (statm) includes the pages the Mali driver charges
  to the process; `/proc/<pid>/smaps` does not. Hollow Knight: 830 MB = ~520 MB CPU + ~300 MB GPU
  pages (`/sys/kernel/debug/mali0/gpu_memory`, 4 KB pages). With ~970 MB in the device, Knulli swapped
  at 40 MB free and muOS (no swap) ran out in a death's scene load; a player's session ended with the
  sound client killed, then the game (exit 137).
- **Textures at half size on 1 GB** (`[convert.textures] lowmem_scale = 0.5`, text atlases in
  `sharp_names`; [porting.md](porting.md#small-handhelds-1-gb)): a game drawn at 600x400 from 1080p
  art keeps more than a texel a pixel. Hollow Knight: 830 -> 670 MB, GPU pages 300 -> 142 MB, no more
  swapping, the same frame rate, the picture the same to the eye. Rehearse it with
  `REHEARSE_MEMINFO` (a 1 GB `/proc/meminfo` for the setup container).
- **Sound in memory**: the audio step's `pcm_budget_mb` decides how much a scene holds decoded.

## 3. Hitches

Sample them first (`UA_HITCH_TRACE=80 UA_HITCH_EVERY=10`): a frame with far fewer samples than its
length allows had its thread unable to take a signal (a garbage collection or a kernel wait), not
slow code.
- **Mono's compiler is most of the first-time hitches.** `UA_JIT_LOG=1` times it: Hollow Knight's
  first slash, first hit and first death compiled 50-240 ms of methods in 100-320 ms frames, 12-18 s
  in a session. `UA_JIT_WARM=1` lists what was compiled and compiles it ahead at the next launch over
  the title and loads (4 ms a frame): King's Pass hitches over 100 ms 8 -> 4. A port turns it on after
  checking that the game's static field initializers read nothing else ([runtime.md](runtime.md)).
- **Pooled objects made mid-fight** (Hollow Knight's "Object Pool ... has run out", ~100-200 ms
  each) and **a game's own saves** (compiling, reflection, encryption: ~0.5 s at the first death)
  are the next ones; `UA_JIT_WARM` takes most of their compiling (what is left is generic
  instances: Hollow Knight's death save 243 -> 129 ms).
- **Garbage collection at scene loads.** Games that keep Unity's collector off (Hollow Knight's
  `GCManager`) collect at each load: Boehm marks the whole heap, ~1.4 s for 128 MB on the A53. It
  lands on the black screen; `UA_GC_BUDGET` keeps a long scene's heap in check.

## 4. Sound

- **The pipe output must take the game's buffers on the clock.** Unity's FMOD (OpenSL output,
  `FMOD::OutputPolled`) fills a buffer from what its mixer thread, awake every 10 ms, has mixed; a
  writer that took buffers as fast as the pipe had room replayed unrefilled blocks (20-80 % of them
  each second in Hollow Knight on Knulli: the queue heard twice, loudest on hits). The runtime paces it
  now (2d174c1). To check a game's sound silently: `ANDROID_SLES_PIPE_CMD` into a pacing sink, then
  compare each 512-frame block with the block 2048 frames earlier ([profiling.md](profiling.md)).
- Mixing at 24 kHz (`UA_AUDIO_RATE=24000`) is a 1 GB trade-off that buys CPU.

## 5. Frame rate: GPU-bound or CPU-bound, per scene

Decide before changing anything: `UA_GPU_IDLE=5` gives the GPU's idle time a frame without
serializing anything, and a render-size A/B at equal GPU clocks tells how much of the GPU's time is
pixels.
- **Paid on the GPU side** (Hollow Knight's 1 GB profile, King's Pass 36 -> ~51): the background
  blur off (`hk-blur`), 600x400 scaled by the display engine (`UA_RENDER_SIZE`), framebuffer fetch
  instead of grab copies (`shaders.grab_fetch`), the host's own flips (`UA_FB_FLIP=host`), cheaper
  clears (`UA_GL_DSCLEAR`), Unity's shader cache.
- **Did not pay** where the scene was CPU-bound (Greenpath, 36-38 fps): 540x360 and 480x320 (the GPU
  then idles 9 ms a frame), graphics jobs off, the game's particles off, `UA_GL_MEMO=0` (it skips
  nothing in a built-in-pipeline game); nor the fused post chain (`hk-post`), nor stripping dead
  shader outputs (the driver does).
- **CPU-bound means the render thread.** Greenpath: Unity's render thread ~21 ms of a 26 ms frame
  (the Mali driver 40 % of it, Unity's GLES device 33 %), the main thread ~17 ms and waiting, the
  four cores only ~65 % busy. No Vulkan driver on these firmwares (Knulli has only an ICD file).
  Find the driver's share by GL entry point (`profsum.py --callers 'libmali.so:<function>'`) and
  count the calls a frame (`UA_GL_REC=census`) before writing a layer.
- **Paid on the render thread:** Unity's dynamic geometry in persistently mapped buffers
  (`UA_GL_PERSIST=1`; Unity maps, flushes and unmaps a pooled buffer for every sprite chunk, ~320 a
  frame in Greenpath, `glMapBufferRange` alone ~6 % of the render thread): Greenpath 37.8 / 37.5 ->
  39.2 / 39.2 fps, then 40.2 - 40.6 once the layer stopped reading the clock at every map (a system
  call for a 32-bit process on these kernels: keep per-call work in a GL layer free of system
  calls); Knulli 36.6 / 36.5 -> 39.5 / 39.6. Those buffers as ranges of one (`UA_GL_ARENA=1`): ~+2 %
  more (muOS ~41.2, Knulli 41.0). The same bytes drawn, both checked by fake-driver tests.
- **The Mali driver works in the draw call.** `runtime/host/drawbench` on the device: a sprite draw
  ~30 - 37 us, all its state calls together 2.5 - 2.9 us. So filtering state calls that change
  nothing (`UA_GL_STATE=1`: 133 of ~2,300 a frame in Greenpath), prebuilt vertex array objects,
  the fast clock (`UA_FAST_CLOCK=1`) and pinning the render thread to its own core
  (`UA_AFFINITY='UnityGfxDeviceW:3 *:0-2'`: muOS no change, Knulli +1.5 - 2 %) did not pay or not
  on both firmwares. What is left is the number of draws, ~200 in Greenpath, set by the game's
  sprites alternating between two atlases in depth order.
- **Then measure the GPU again.** With both layers Greenpath's GPU is idle 0.7 ms of a 26 ms frame
  (`UA_GPU_IDLE=5`, muOS): the render thread and the GPU now hold it at the same ~41 fps, the main
  scene pass (180 sprite draws, 600x375) 18 - 20 ms of GPU time (`UA_GL_TRACE`). Past this a scene
  needs both fewer draws and fewer pixels drawn.

## 6. Native code: 64-bit and IL2CPP

Everything already runs as native ARM code (no emulator); what is left is Mono's JIT (less
optimized code, managed-to-native transitions: ~23 % of Greenpath's main thread is managed code and
Mono's runtime) and 32-bit mode for the engine and the driver. [il2cpp.md](il2cpp.md) has the
64-bit IL2CPP route (on a PC or on the handheld) and [mono-arm64.md](mono-arm64.md) the 64-bit Mono
one.

**Measured on Hollow Knight, and not worth it there** (2026-11-04, Knulli RG34XX-SP, the same
converted data and 1 GB profile, every run from a cool handheld): the game's code built with IL2CPP
for arm64-v8a (`pmu il2cpp --on-device` in the arm64 container on an M3: 20 min, 35 min of CPU, peak
869 MB, `libil2cpp.so` 80 MiB; on an A53 the CPU time alone is hours) against the 32-bit Mono build:

| scene | 32-bit Mono | 64-bit IL2CPP |
|---|---|---|
| Greenpath (walk) | 36.4 fps | 35.4 fps |
| Soul Master's arena | 41.6 fps | 41.8 fps |
| King's Pass (walk) | 50.6 - 51.0 fps | 51.7 fps |
| the game's memory (`[fps]` rss) | 623 - 680 MB | 768 - 806 MB |

No gain where it counts: the heavy scenes' critical path is the render thread and the Mali driver,
which neither native script code nor 64-bit mode speeds up, while the 64-bit build holds ~140 MB
more, which a 1 GB handheld cannot spare. Compiling ahead (`UA_JIT_WARM`) takes the one thing
IL2CPP fixes that mattered (first-use compiling) without either cost. A game whose frames are spent
in its own scripts (a main thread busy with managed code, not waiting) is where IL2CPP would pay:
check `profsum.py`'s module split first.

Also did not pay: raising the render thread above FMOD's mixer (`UA_NICE='UnityGfxDeviceW:-19'`:
Greenpath 36.4 -> 32.9: the main thread and the mixer then wait).

## On the porter's machine

Rehearsals, conversions and IL2CPP builds write gigabytes (a copy of the game, 828 MB of generated
C++ for Hollow Knight). Docker volumes live in Docker Desktop's disk image on the Mac's own disk:
put those files on a big drive as bind mounts instead (slower, but a full system disk stops Docker
Desktop, and its "Reset to factory defaults" would delete every image and volume). Check `df -h /`
before a multi-GB job.

## Where to go next

- Each measured switch: [runtime.md](runtime.md); the device bench and its plans:
  [profiling.md](profiling.md); the converter's options: [porting.md](porting.md).
