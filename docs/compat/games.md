# Games tested

Every game the tools have been run with, and how far each got. Kept up to date as games are tested
or retested; the date says when a row was last checked. Free games from itch.io unless noted; the
games themselves, and anything made from them, are never in this repository.

**How far** means: *title* (the title screen or main menu draws and runs), *menus* (the pad or
pointer walks them), *in game* (a level or the first scene plays). The **emulated rig** is QEMU with
Mesa's software renderer (tools/emu: correct pictures and game logic, about 1 fps); **handheld** is
an Anbernic RG34XX-SP (Allwinner H700, Mali-G31, 1 GB RAM, Knulli, 720x480) unless noted.

Most of these games are also in the regression run's list ([tools/regress](../../tools/regress/README.md)):
`regress.py fast` takes them through the converter and android-data again before a branch that
touches `convert/` or `runtime/` is merged, `regress.py rig` runs 17 of them in the emulated rig,
and both compare with the recorded results.

Routes: **32-bit** = Unity's Android Mono player (armv7) on `ua_host`, the desktop data converted;
**64-bit IL2CPP** = the desktop game compiled with `pmunity il2cpp`, on the arm64 player; **APK** =
the game's own Android build (`pmunity apk`); **box64** = the game's x86-64 Linux player under box64
(tools/box64).

## Full ports

| game | Unity, build | route | emulated rig | handheld | notes (last checked) |
|---|---|---|---|---|---|
| Cuphead + The Delicious Last Course (GOG, macOS) | 2017.4.9f1, Mono .NET 3.5 | 32-bit | title, levels; the title screen at PortMaster's test sizes: 480x320 (3:2, the port's side crop), 640x480 and 720x720 (letterboxed), 1280x720 | **whole game**: setup from GOG's installers, title 59.6 fps, eight boss fights 59.0 - 59.5 fps; setup on muOS 2606.0 (an H700, 1 GB, Medium, with the DLC, from Cuphead.app, 2026-10-29): 1 h 20 min (textures 63 min with the ASTC guide, packing 13 min), the CPU held at full speed (muOS leaves it at powersave, 288 MHz); 2026-11-01, the setup's tools in Rust (`pmu`, no Python): muOS 2606.0 from Steam-style depot folders 59 min, Knulli from GOG's installers 62 min, every built file and APK/OBB entry the Python setup's byte for byte, the title with the DLC on both (Knulli 59.6 fps; muOS 76 fps at its game clocks: muOS drives this panel at 106.6 Hz); the texture step's peak 329 MB (Python 508 MB). Players (2026.10.31): muOS on an RG35XX-H and Knulli on a Miyoo Flip, setup, game and DLC; ROCKNIX on RK3566 handhelds (RG ARC-S, RGB30) restarted during the texture conversion, not retried with 2026.11.01 yet | its own port (portmaster-cuphead). 64-bit (IL2CPP) build: Cuphead's code compiled with IL2CPP for 2017.4.40f1's arm64 player (7 min on the PC, `libil2cpp.so` 50.8 MB), the 2017.4.9 data retargeted to it; in the native arm64 rig: title screen with the DLC, menus with the handheld's button letters, a new game through the intro book into Elder Kettle's house (Dialoguer's dialogue; the saves, Dialoguer's XmlSerializer data included, written with the port's link.xml), sound starting. The port's own 64-bit setup (IL2CPP build on the device included) ran through on a 64-bit-only stand-in (arm64 container, 1 GB, no 32-bit loader) and its install reaches the main menu (2026-10-21) |
| Hollow Knight (GOG, Linux; Steam's Linux files too) | 6000.0.61f1, Mono .NET 4.x | 32-bit; 64-bit (Mono on the arm64 player) where the system has no 32-bit userland | first launch to gameplay: language, menus, calibration, both opening cinematics, King's Pass; the port's setup rehearsed in an arm64 container (`tools/port/rehearse.sh`, 1 GB memory + 2 GB swap: 32 minutes, 3.9 GB installed) and the game it installed starts at its first-run language screen | **first launch to gameplay**: title 37 fps, prologue 59.6, opening cinematic 30 (the video's rate), King's Pass 32 - 34 fps (2026-10-21, before the host's flips); saving (Quit to Menu) and loading the save work; 30 minutes with steady memory (56 MB available at the lowest) | the game switches Unity's garbage collector off: without `UA_GC_BUDGET` (default 32 MB) Mono's heap grew from 64 to 192 MB in 28 minutes and memory ran out; with it, a collection every 4 - 5 minutes, a pause of about 0.6 s each; the desktop's frame pacing (`UA_FRAME_RATE`) lifted the 30 fps cap; the audio step keeps the game's DecompressOnLoad clips where a scene's PCM fits its budget (16 MiB by default; until 2026-10-24 every clip of 2 s or more played compressed, and the 12 atmosphere loops the game keeps from the title on cost FMOD's mixer 8 points of a core in King's Pass: [profiling.md](../profiling.md#fmods-mixer-and-the-audio-step-2026-10-24)); the 64-bit IL2CPP route builds it since 2026-10-22 (a mixed build: Unity 6's own il2cpp is a native program: "64-bit IL2CPP route" below); the buttons by position with the handheld's letters in the prompts (asmpatch's `hk-pad-letters`, `UA_PAD_NAME` an Xbox pad's; until 2026.11.08.2 a Switch pad's name made the game take them by letter: [input.md](../input.md#buttons-by-position)). King's Pass is GPU-bound: 33.3 fps warm (the Mali capped at 600 MHz), 36.6 cool, 49 fps with the GPU's work left out (a measurement; [profiling.md](../profiling.md#what-limits-kings-pass-32-bit-2026-10-23), 2026-10-23). GPU (2026-10-23): King's Pass's frame is 24 passes, about 90 draws and 13,500 indices; the camera leaves its framebuffer three times a frame for its blend-mode sprites' grab passes, and the Mali reloaded its depth/stencil buffer each time: `UA_GL_DSCLEAR` (on by default) clears it again instead, 31.8 -> 32.5 fps (one run each; [profiling.md](../profiling.md#gpu-pass-by-pass)) (2026-10-23). asmpatch's PlayMaker optimizations (exact, by default): the main thread 0.6 ms a frame less (+3.1 % with the GPU's work left out), in game the same per MHz of the GPU; title, menus, the save and King's Pass as before, the Knight walks and jumps (2026-10-24, below). Reported by Knifethrower (2026-11-08): playable on an Anbernic RG552 (RK3399, Mali-T860, 4 GB, a 1920x1152 screen) with AmberELEC, with long black screens while loading and a frame rate he called not the best. His log (with 4 GB none of the small-handheld options apply: the game drew at 1920x1152 with its full blur, from full-size textures, the cinematics at 1080p): title 44 - 50 fps, King's Pass 10.6 - 11.8 fps, the load into it a 5.4 s hitch (our handhelds' logs of that load: 3.6 - 15 s, median 4.7 s, 300 runs: the black is the port's load time everywhere), the first 30 s cinematic about 45 s long. Unity counted one core (`SystemInfo CPU = ... Cores = 1`, `big.LITTLE configuration: 1 big (mask: 0x1)`; 4 on every handheld here), so Unity 6's Android player probably held its main and render threads to CPU 0 (an A53 on the RK3399; `-platform-android-unitymain-affinity` and `-gfxdeviceworker-affinity` default to its big cores); why it counted one is not known yet. 2026-11-08: the port's picture options now on every handheld (port b36f131, `[runtime_low]`: no blur, at most about 960x576 through `UA_RENDER_PIXELS`, the game's own video options at their lowest; Knulli 1 GB, Greenpath 52.3 / 51.9 fps, City of Tears 50.9 / 50.9 before / after with Shader Quality Low and Noise off added: no change). VP8 on the hardware (#12, libpmhwdec 27e1bf1 on cedar): the 1080p intro decodes on the RG34XX-SP's video engine through Unity, in real time as with libvpx, the cores 73 -> 54 % busy, lowest second 20 -> 33 fps 2026-11-09 testers (test release 2026.11.09.3): working on an RG40XX-H with muOS (the D-pad at times late or stuck: open, log asked for), a Powkiddy X55 with ROCKNIX (libmali, Steam's Linux files), an RGB10 and an RG353V with dArkOS, a Retroid Pocket 5 (Steam's files); a 720x720 Knulli handheld drew at 360x360, hard to read (now 480x480: `UA_RENDER_SIZE="540x360 480x480"`, 1:1 stand-in Greenpath 53.6 -> 51.5 fps); an R36S with dArkOS-RE: black screen with sound, then a crash (dArkOS-RE is not supported); TrimUI: stopped at the launcher (no 32-bit userland). dArkOS keeps EmulationStation in memory while a port runs (64 - 126 MB on the G350): with that held, City of Tears down to 34 MB available (porting.md, RK3326). The 64-bit build (porting.md, "Both builds in one release"): its whole setup in a 64-bit-only 1 GB container without network, 14 min; RG34XX-SP with muOS, Greenpath 45.6 fps, 49.7 with the arm64 present-sync row (32-bit 50.0), at least 189 MB available |
| Enter the Gungeon (GOG, Linux) | 2017.4.27f1, Mono .NET 3.5, Wwise, InControl, PlayMaker | 32-bit | the port of the first blind port test (its own assembly patch stubbed Wwise then: silent): the setup rehearsed (`tools/port/rehearse.sh`, 640x480), title and the Breach draw, the pad's A leaves the title for the Breach; before 2026-11-02 the title's sky was magenta and the Breach's dust motes magenta specks (9 "Desired shader compiler platform 9 is not available in shader blob"), now the night sky, clouds and stars draw and the log has none of those errors (the same release with this branch's pmu, the same presses) | RG34XX-SP, Knulli and muOS (2026-11-08): the Keep's first fight (a fixed floor seed and no damage, both test-only; the port's patches, `UA_FUTEX_SPIN=50`, the templates' four, `UA_MONO_INVOKE` and `UA_MONO_TLS`; the frame recorder where there is swap) Knulli 45.6 - 46.0 fps, muOS 45.8 - 45.9, and with the port's HUD patch (`df-render-skip`) Knulli 49.4 - 49.9, muOS 48.7 - 49.3; with the game's lowest options (Lighting Low, Shaders Very Low, reflections off, Debris Low: the ports' rule, no setup question) and ua_host's Wwise fix Knulli 58.0 - 58.5, muOS 59.4; with the job system's waits at 400 us on the H700 Knulli 58.7 - 59.1, muOS 60.2 - 60.6; without dfGUI's 2,100 hidden kinematic bodies (the port's `df-no-body`) Knulli 59.2 - 59.4, muOS 62.2 - 62.4 ([profiling.md](../profiling.md#a-ui-librarys-hidden-physics-bodies-dfgui-in-enter-the-gungeon-2026-11-09)); heavier scenes at the port's settings (a test build that goes there: [profiling.md](../profiling.md#how-to-bench-a-games-heaviest-scenes-enter-the-gungeon-2026-11-09)): the Keep's boss 49.9 / 47.7, the Forge's busiest room 40.7 / 41.6 -> 46.2 / 45.9 with the patches below, the High Dragun 47.3 / 48.6 -> 47.0 / 53.2 (Knulli / muOS), Bullet Hell's busiest room 23 - 26 -> 31 - 35 with the goop patches (`goop-dims`, `goop-scan`) -> 32.5 - 35 with `unity-list-ref` and `UA_MONO_TLS`'s thread-local copies -> 36 - 39 with `physics-skip` and `half-rate`; the Breach 59 fps with the game's Low lighting and shaders ([profiling.md](../profiling.md#monos-runtime-invoke-and-thread-local-reads-unity-20174-ua_mono_invoke-ua_mono_tls)); BattleXP G350 (dArkOS, 2026-11-08): the port's setup on the handheld 19 min (offline, the donor's player), the fight with the port's lowest settings 40.1 / 39.1 fps, room entry 38.9 / 39.7, lowest MemAvailable 107 MB (below the table) | its title and Breach scenes are scene bundles (StreamingAssets/Assets/foyer_00*: `BuildPlayer-<scene>` and its `.sharedAssets`) whose 10 shaders the converter left desktop-only; pmu converts them since 2026-11-02 ([shaders.md](../shaders.md#the-files-the-step-reads-asset-bundles-and-scene-bundles)): the 8 serialized files of its 4 scene bundles the Python's conversion of the same files byte for byte, every other converted file the Python's; `port check` lists the scene bundles. Movement is on the left stick only: `port check` suggests `UA_DPAD_STICK = "auto"` and `UA_DPAD_SWAP = "l1"` (L1 is its second dodge-roll button; it gives the d-pad back for switching guns and items) and drafts `[controls]` from its 47 InControl bindings; aim is on the right stick only, which the RG34XX-SP lacks (`port check` notes it; the game's own aim options are still to be checked on the handheld). Sound plays through ua_host's own Wwise player from the banks the setup converts: a rehearsal from GOG's installer (2026-11-02, 600 s in the rig) reached the Breach with its music in the capture (`sound.wav`: sound in 465 of 533 s, peak -2.1 dBFS). Its bundles hold 869 MB of RGBA32 textures (`port check` warns for 1 GB handhelds) |

Hollow Knight on the RG34XX-SP, King's Pass, 32-bit, 2026-10-23 (`tools/device/profbench.sh`, the plan in
docs/profiling.md): 31.8 fps in the morning; 32.5 with `UA_GL_DSCLEAR`; 35.7 - 36.2 with the host's fbdev
flips (UnityMain 86 - 88 % busy). The game's own video settings
(PlayerPrefs, no saved value means Shader Quality 1): Shader Quality 0: 36.4; Shader Quality 2: 25.1;
particles off: 33.9; Shader Quality 0 + particles, dithering and camera noise off: 37.3. The 64-bit
(IL2CPP) build: 30.6 in King's Pass before the flips (UnityMain 80 %), main menu 42.6 with them.
Later on 2026-10-23 (a profile with every caller, and the scene measured without the GPU's work):
King's Pass is held by the GPU, not the main thread. 36.6 fps while the Mali runs at 696 MHz, **33.3
fps** once the handheld is warm and the thermal governor caps it at 600 MHz (150 s: 33.1 - 33.4,
the GPU at 600 MHz 95 % of the time); UnityMain waits 3.4 ms a frame for the previous frame's
present. With every fragment shader writing a constant (a measurement) the same scene runs 49.1 -
49.6 fps, UnityMain 95 % busy at 19.3 ms of CPU a frame. Runtime changes measured there (none is a
default): `clock_gettime` without system calls (`UA_FAST_CLOCK=1`) 0 %, Unity's thread priorities
(`UA_THREAD_PRIORITY=1`) 0 %, `MONO_INLINELIMIT=40` 0 %, 2 or 1 job workers 0 % (0: -5 %), FMOD's
threads on a core of their own (`UA_AFFINITY='FMOD*:0 SDLAudio*:0 *:1-3'`) +3 % there but 0 % in
King's Pass and the main menu (both GPU-bound: the menu 43 fps with UnityMain 45 % busy). Details: [profiling.md](../profiling.md#what-limits-kings-pass-32-bit-2026-10-23).
2026-10-24 (the system around the game): the GPU never waits for work in King's Pass (`UA_GPU_IDLE`:
0.08 ms of a 31 ms frame 32-bit, 0.10 ms 64-bit), and its clock is a share of the thermal governor's
power budget that the CPUs' busy time takes from: from a 47 C start 34.5 fps (696 MHz 22 of 60 s),
from a warm 69 C start 32.3 (600 MHz); one more busy CPU core took the Mali to 504 MHz and 28 fps
(-16 %), and half of FMOD's mixer taken away (a measurement: mixing at 12 kHz) kept it at 696 MHz,
37.5 against 34.2 fps. Unity's job workers 3 -> 2: 34.9 against 34.5, noise. Details:
[profiling.md](../profiling.md#the-handheld-around-the-game-2026-10-23).
2026-10-24 (the audio step): the converter's old rule (every DecompressOnLoad clip of 2 s or more
compressed in memory) had FMOD decode the game's 12 resident atmosphere loops all the time; with
them decoded at load as the game has them, FMOD's mixer 56 - 57 -> 48 - 49 % of a core, 120 s from
48 C 34.1 -> 34.7 and 34.1 -> 34.5 fps (the Mali at 696 MHz 49 - 50 s of 120 against 28), the
available memory in King's Pass 46 - 49 MB with the 36 MiB budget that keeps all of them, against
55 - 76 MB before (the default 16 MiB keeps 7 of them: the mixer 54 %, 56 - 59 MB). Details:
[profiling.md](../profiling.md#fmods-mixer-and-the-audio-step-2026-10-24).
2026-10-24 (asmpatch's PlayMaker optimizations, exact, on by default for PlayMaker games:
`playmaker-loopcount`, `playmaker-delayedevents`): with the GPU's work left out 49.3 / 48.9 -> 50.5 /
50.7 fps (+3.1 %, UnityMain's frame 19.5 -> 18.9 ms; the 64-bit build on its Mono route 46.3 / 46.3
-> 48.3 / 47.9, +3.9 %); in game, GPU-bound, 35.3 / 33.8 against 35.0 /
35.6 fps, per 100 MHz of the GPU's clock the same (5.31 / 5.41 against 5.35 / 5.33); pictures as two
base runs (2.0 % of the static pixels by 1 - 3). Picture reductions for 1 GB handhelds (asmpatch's
opt-in `hk-blur`, the 64-bit build on its Mono route, one run each, per 100 MHz): the game's own
Shader Quality Low +4.5 %, `ASMPATCH_HK_BLUR=low` (the same picture) +5.4 %, `half` +17 %, `off`
+14 % (40.0 fps, then held by the main thread); every particle system stopped (a measurement)
+0.4 %: particles cost King's Pass nothing measurable; memory within the runs' spread (resident 729 -
746 MB, 42 - 46 MB available). Details: [profiling.md](../profiling.md#playmakers-per-frame-work-asmpatch-optimizations),
[profiling.md](../profiling.md#hollow-knights-blur-and-particles-on-1-gb-handhelds).
2026-10-24 (a smaller window, scaled by the display engine: `UA_RENDER_SIZE`, opt-in, a softer
picture, the same framing and HUD): King's Pass 600x400 40.7 fps, 540x360 41.7, against 35.2 and
34.7 at 720x480 (the Mali at 600 MHz in the scaled runs); UnityMain 94 - 95 % of a core: the main
thread is the limit from 600x400 on. The 64-bit build: 40.0 and 41.1 against 33.9 and 33.7. GPU
memory 6 - 7 MB less. Details: [profiling.md](../profiling.md#rendering-smaller-the-display-engines-scaler-ua_render_size).
2026-10-24 (the 1 GB options together, each measured alone before: `tools/device/profbench.sh`, 25 s
after the save's load, every run from a cool handheld, `devlock.py cool`, two static-frame shots a
second apart; from run 3 on the APK also has asmpatch's PlayMaker optimizations and `hk-blur`):

| run | fps | GPU MHz (avg; share at 696) | UnityMain on a CPU | FMOD mixer | available MB |
|---|---|---|---|---|---|
| as shipped | 36.2 | 689 (94 %) | 90 % | 57 % | 72 |
| `UA_RENDER_SIZE=600x400` | 40.9 | 605 (6 %) | 95 % | 56 % | 59 |
| + `ASMPATCH_HK_BLUR=low` | 42.8 | 674 (78 %) | 95 % | 56 % | 64 |
| + data with `shaders.grab_fetch = "always"`, `audio.pcm_budget_mb = 36` | 46.7 | 683 (88 %) | 96 % | 47 % | 52 |
| + `UA_GFX_JOBS=legacy` (the whole 1 GB profile) | **51.0** | 657 (61 %) | 90 % | 47 % | 52 |
| as shipped, again | 34.9 | 661 (65 %) | 87 % | 58 % | 59 |

From 600x400 on the main thread holds the frame (UnityMain 95 - 96 % busy), so the GPU savings of
the blur and grab_fetch pay less than alone and the graphics jobs pay more (+9 %, UnityMain 96 ->
90 %, the job workers 7 - 12 -> 12 - 18 % each). Pictures: the full profile with and without
`UA_GFX_JOBS` differ on 1,431 static pixels (16 by more than 3 of 255, scattered single pixels,
largest 37) against 2,201 (41, largest 20) between the two shipped runs: the same picture, no split
sprite batch to see; the data variant against the same options on the shipped data 5,846 (78 by more
than 3; grab_fetch's blend sprites, as [shaders.md](../shaders.md#blend-modes-without-grabpass-copies-grab_fetch-opt-in)
measured); framing at 600x400 the same part of the scene (`tools/analysis/framecmp.py`: ratio 1.00,
not the 1.20 of a pixel-perfect camera). A 60 s walk-through with the whole profile (the Knight
walks right into the cave, jumps, walks back, three times): 42 - 53 fps a second while walking
(each jump one frame of 90 - 100 ms, as without the options), 51.2 fps over the 30 s standing after
it with the Mali at 600 MHz, 46 MB available and 103 MB of swap at the end, no watchdog line. The
Hollow Knight port ships this as its `[runtime_lowmem]` (portmaster-hollowknight port.toml) with
`grab_fetch = "always"` and `pcm_budget_mb = 36` in its convert config and `asmpatch = ["auto",
"hk-blur"]`.
2026-10-25 (the three post-processing passes at the end of the frame drawn as one: the converter's
`shaders.fused_post` + asmpatch's `hk-post`, `ASMPATCH_HK_POST=fused`, opt-in, not exact by a
rounding step): on the 1 GB profile's data 16 -> 14 passes a frame, the chain's GPU time 2.7 -> 2.2
ms serialized at 600x375, the render thread 70 -> 68 % of a core; fps the same (52.7 / 53.7 against
52.7 / 49.4 at 600x400, main-thread bound; 40.1 against 39.9 at 720x480 GPU-bound, all at 696 MHz
but one); pictures within a rounding step (4.2 % of the static pixels differ by 1 - 3 against 2.5 %
between two base runs). Did not pay in frames: not in the port's profile. Details:
[profiling.md](../profiling.md#hollow-knights-post-chain-as-one-pass-fused_post--hk-post).
2026-11-01 (King's Pass, the whole 1 GB profile, 30 s windows from a cool handheld; Knulli's own CPU cap is
1512 MHz, its default, the chip goes to 1704): 50.0 - 51.1 fps at 1512 MHz; **48.8 at 1704 MHz** (Knulli's CPU
setting, set for the run only): the thermal governor (power_allocator, one budget for the CPUs and the Mali)
then holds the Mali at 600 MHz the whole run, so a faster CPU makes the frame slower. The game's full blur
instead of `hk-blur` low: 46.3 - 46.9. The 64-bit builds with the same options: IL2CPP 43.9 - 44.9, Mono
41.6 (the Mali at 600 MHz). Thread placement and priorities, each alone (FMOD's mixer on a core of its own,
Mali's `mali-cmar-backe` at nice -10, both, `UA_THREAD_PRIORITY=1`, `UA_FAST_CLOCK=1`): 48.5 - 51.1 against
48.3 / 50.0 for two base runs, noise. UnityMain is on a CPU 86 % of the time and waits 11 % (futex,
2.2 ms a frame); its own system calls are 7.7 % of its CPU time. The same game set up on the handheld by
`pmu` (no Python, 47 minutes, the Python's APK and OBB byte for byte): 49.4; with `resources.assets`'
texture streams split into three `.resS` parts in the main OBB (portmaster-unity issue 7's fix): title,
menus and King's Pass render, 52.7.
Real play is heavier than the standing spot. A player's own session (2026-10-26, the 1 GB profile, a new
game): King's Pass at 29 - 47 fps a second, mostly 30 - 35; the opening cinematic (a 1920x1080 VP8 video,
decoded in software) stopped the game for ~20 s in all (1.6, 4.8 and 13.2 s frames); loads of 8 s (title)
and 11 s (King's Pass); in fights frames of 0.1 - 0.9 s, several as the game's object pools instantiate an
effect for the first time ("Slash Impact R", "Strike Nail R", "Run Effects"); not shader compiles: on a
launch with Unity's shader cache emptied (2026-11-02, `UA_GL_PROGLOG`) the game made all its 26 programs
inside the title's and King's Pass's loads (2.6 s of compiles and links), the walk and fight below none,
and the next launch loaded them from the cache in 0.1 s ([profiling.md](../profiling.md#shader-programs-compiles-links-and-unitys-binary-cache)). 2026-11-01, the same
profile with `UA_PROF=250` (both runs pay the profiler's ~15 % of a core): standing 49.2 fps; walking
right, attacking and jumping 39.0 (UnityMain 19.9 ms on a CPU a frame against 17.7, waiting for a free
core 2.7 ms against 0.9 with all four cores 80 - 84 % busy; FMOD's mixer 46 % of a core in both, the
silent run included; the power allocator took the Mali down to 420 - 504 MHz, sometimes 240 - 336, at
64 - 69 C, against 600 standing).
The profiler's own cost grows with the action: the same walk without `UA_PROF`, 2026-11-01 (the Knight walks
right, attacks every 0.6 s and jumps every 2.2 s; 40 s measured, ~50 s of frames; each run from a cool handheld):
the 1 GB profile 49.3 / 48.3 / 48.0 fps (195 - 262 frames over 25 ms); `ASMPATCH_HK_BLUR=off` 53.2; the game's
own lowest settings (Shader Quality 0, particles, dithering and noise off, through its PlayerPrefs) 48.9;
`UA_AUDIO_RATE=24000` 50.2 (FMOD's mixer 48 -> 31 % of a core); 540x360 48.7; 480x320 49.8; all of them at
540x360 56.4 and 54.4, at 480x320 57.2; **blur off + 24 kHz at 600x400: 56.4 (36 frames over 25 ms)**, the
same at 540x360 54.2. The CPU each option saves goes to the Mali's clock under the shared power budget,
so together they gain more than each alone; the render size and the game's own settings add nothing on top.
2026-11-02, a player's play test of the port's release 2026.11.02 (Knulli, sound on through PipeWire's
`pw-cat`): King's Pass at 45 - 51 fps a second with 100 - 250 ms frames every few seconds, an 854 ms frame at
the first hit (the game's pool instantiates "Screen Flash"), the sound glitching on hits, the opening
cinematics a slide show. Measured on the H700 since (muOS, over SSH, silent; that handheld has no swap,
and Hollow Knight's opening and King's Pass leave it 38 - 67 MB available, enough to thrash): the
cinematics are 1920x1080 VP8 with one token partition, and Debian's armhf libvpx, which the media core
linked, is plain C: 140 ms a frame at 1.5 GHz. Built from the same source with NEON, 31 ms at 1.8 GHz
alone and about 40 ms in the game; the prologue shows 16.8 pictures a second instead of 7.6, but still
in slow motion against its music, since Unity 6000's SurfaceTexture path keeps one picture in flight
and the game runs at the clip's 30 fps without skipping frames
([runtime/media/README.md](../../runtime/media/README.md#unity-6000-one-picture-in-flight)). The media
core no longer keeps up to 25 MB of finished 1080p pictures through the game after a cinematic. The
game's mixer never ran dry in these runs (the OpenSL shim now logs every underrun and can capture its
output, `ANDROID_SLES_CAPTURE`); the gaps a stand-in for `pw-cat` heard came in bursts of 0.2 - 0.4 s
where the whole handheld stalled, under memory pressure. The hits themselves are not yet measured with
the capture (a King's Pass save is on the Knulli). The cinematics then (muOS, the 1 GB profile): pmu's `video` step halves the game's
14 clips over 720 lines to 960x540 (84.2 -> 43.9 MB, 578 s on the handheld; on an installed game in
place with `pmu video`), and with ua_host's display rate while a video plays and VP8 decoded on one
thread (four spun on `sched_yield` and showed ~12 pictures a second) the prologue shows its 30
pictures a second in steady play (latch to latch 32 - 39 ms), fewer only in the game's own loading
frames for King's Pass during the cinematic; memory at the clip 94 - 99 MB available instead of
50 - 53 ([runtime/media/README.md](../../runtime/media/README.md#unity-6000-one-picture-in-flight)).
portmaster-hollowknight's `[runtime_lowmem]` is now blur off, 600x400, gfx jobs legacy and 24 kHz
(540x360 since 2026-11-04, below).
2026-11-03/04 (Knulli and muOS, silent, the 1 GB profile, walks with attacks and jumps; the map of it
all: [performance.md](../performance.md)): the hits' sound glitch was the pipe output replaying
unrefilled buffers of FMOD's polled output (20 - 80 % of them a second), fixed by a paced writer
(2d174c1: none replayed since); the game's memory 830 -> 670 MB with textures at half size on 1 GB
(9bf02c0, port bbd5f3b), no more swapping; first-use compiling ahead at launch (`UA_JIT_WARM`,
f02782b, port 155b9c9): King's Pass hitches over 100 ms 8 -> 4. Test saves starting in other areas
(the save's respawn scene and marker): Deepnest 54.7 fps, Mantis Village 53.3, the Colosseum 52.6,
Kingdom's Edge 52.1, the Watcher Knights 50.1, Dirtmouth 49.7, Crystal Peak 48.3, Queen's Gardens 48.0,
Soul Tyrant 47.4, City of Tears 42.4, Soul Master 41.6, Greenpath 36 - 38: Greenpath is held by the
render thread (~21 ms a frame, the Mali driver 40 % of it), not the GPU's pixels (480x320 the same).
The 64-bit IL2CPP build of the same data: Greenpath 35.4, Soul Master 41.8, King's Pass 51.7, ~140 MB
more memory: not taken ([il2cpp.md](../il2cpp.md)). The render thread's sprite chunks in
persistently mapped buffers and then in one buffer (`UA_GL_PERSIST`, `UA_GL_ARENA`, on in the
port 2026-11-04 - 2026-11-06, then off: they drew sprites as black slabs on the handheld, see
[performance.md](../performance.md)): Greenpath 37.5 - 37.8 -> ~41.2 fps on muOS, 36.5 - 36.6 -> 41.0 on Knulli. The survey again
with both (2026-11-04, steady fps: the walk's seconds at 20 fps or more; one run each, the same
devices as before): muOS Colosseum 53.6 -> 54.9, Deepnest 56.5 -> 56.5, Kingdom's Edge 52.1 -> 55.3,
Queen's Gardens 48.0 -> 47.8, Mantis Village 54.1 -> 57.7, Dirtmouth 50.5 -> 54.1, City of Tears
39.9 / 39.8 -> 40.8 / 41.0 (A B A B); Knulli Soul Master 42.7 -> 46.5, Soul Tyrant 48.0 -> 52.6,
the Watcher Knights 50.1 -> 52.4, Crystal Peak 48.3 -> 48.2, City of Tears 43.1 -> 42.0 - 41.3. The
scenes that did not move are GPU-bound (City: the GPU idle 0.1 - 0.3 ms of a 26 ms frame); at
540x360 instead of 600x400 (softer): City 47.3, Crystal Peak 52.9, Queen's Gardens
52.3 -> 54.0 (Knulli), Greenpath 43.6 (muOS) / 42.1 (Knulli, where the render thread holds it):
the port's 1 GB profile renders at 540x360 since (portmaster-hollowknight 0bbf591). Then one job
worker (`-job-worker-count 1`, port 6fc5fa9): Greenpath 45.6 (Knulli) / 45.9 (muOS), Soul Master
51.9 - 52.8 / 51.9, the Colosseum 59.1, King's Pass 56.1 (Knulli); and the game's own Particle Effects
Low and Dithering off by default on 1 GB (`UA_PREFS_DEFAULTS`, port 5fe4579): City of Tears 46.3 - 48.2.
The survey with the whole 1 GB profile (both GL layers, 540x360, `UA_JIT_WARM`, one job worker, the game's
Particle Effects Low and Dithering off; 2026-11-04, steady fps, one walk each, the same devices as the
first survey): muOS Colosseum 61.9, Deepnest 61.3, Mantis Village 61.3, Kingdom's Edge 61.2, Dirtmouth
57.1, Queen's Gardens 54.7; Knulli Soul Tyrant 57.9, the Watcher Knights 56.6, Crystal Peak 53.5, Soul
Master 51.8, City of Tears 50.2, Greenpath 46.0 (the first survey: 53.6, 56.5, 54.1, 52.1, 50.5, 48.0;
48.0, 50.1, 48.3, 42.7, 43.1, 36.5).
2026-11-07 (Knulli, silent, the 1 GB profile, steady fps, screenshots checked in each run): the
arena drawn with ranged draws (767e230, the slabs were the Mali driver's stale index ranges), the
buffer pool, `UA_PRESENT_SYNC=end`, `UA_GL_MEMO=0` and the lit sprite twin (`UA_GL_SPEC`, 50 draws
compared on the handheld, 0 differ) are the port's defaults (portmaster-hollowknight 8f8a113):
Greenpath 52.1, City of Tears 55.8, Crystal Peak 56.9, Queen's Gardens 57.0, Deepnest 58.3,
Dirtmouth 58.4 (without the twin: 51.6, 51.8, 54.8, 56.2, 58.3, 57.1). Boss fights from the Hall of
Gods (the GG save's bench, a walk steered by `UA_DUMP_HERO` to each statue, the fight's seconds only;
without the twin): False Knight 57.1, Vengefly King 57.1, Mantis Lords 54.7, Flukemarm 54.6, Nosk
54.1, God Tamer 52.5, Oblobbles 52.4, Hive Knight 52.2, Crystal Guardian 50.3, Hornet 49.9, The
Collector 49.8, Uumuu 49.4, Gruz Mother 49.3, Broken Vessel 47.8; Gruz Mother with the twin 54 - 56.
The Godhome arenas are GPU-bound: about 26 fragments a pixel in the scene (god-ray beams, haze),
the scene pass 19 ms of the Mali; thread priorities and cores changed nothing. The black band over
the top of the Soul Master and Soul Tyrant arenas in the earlier spot saves is the arena's own mask
over the lower room, which the fight fades when the floor breaks: those saves put the Knight in the
lower room (`UA_DUMP_RENDERERS` found it), not the port.
Later on 2026-11-07: the lit sprite twin went into test release 2026.11.07, and the background
flip-books (`UA_FLIPBOOK`: 125 controllers that only swap their own sprite, shown by a stand-in for
Unity's Animator) into 2026.11.07.2 (Knulli, the release's own files, screenshots checked): Broken
Vessel 47.5 - 48.8 -> 55.7, Gruz Mother 56.1, Hornet 56.1, City of Tears 57.3, Greenpath 52.6 - 53.3,
Crystal Peak 56.6. Thread priorities, one core per thread, one to three job workers and `UA_GL_DSINVAL`
gained nothing in the fights.
2026-11-08, the BattleXP G350 (dArkOS, RK3326: Cortex-A35 cores throttled to 1.0 - 1.5 GHz at ~80 C,
Mali-G31 at 400 MHz, r13p0 through SDL's KMSDRM; 640x480, the 1 GB profile rendering 480x360, the
picture filling the 4:3 screen with asmpatch's `hk-aspect`; average fps of a walk with attacks, the
bench's "steady" figure never settles there): the release 17.5 in Greenpath, where the render thread
waited in every swap for the GPU to finish the frame. With ua_host's present thread
(`UA_PRESENT_THREAD`, portmaster-hollowknight c384e92): Dirtmouth 50.1, Mantis Village 43.1,
Deepnest 41.7, Kingdom's Edge 41.7, the Colosseum 38.7, King's Pass 38.1, Crystal Peak 36.4, Queen's
Gardens 35.4, City of Tears 32.4, Greenpath 25.1 (screenshots checked; boss fights to be measured from the
Hall of Gods: the Soul Master and Soul Tyrant spot saves start in the arena's lower room, behind the
scene's own mask, so their numbers are not the fights'). Measured and
left out there: exact vertex ranges and base-vertex draws (no change), vsync off (none), 0 job workers
(+11 % without the present thread, nothing with it), incremental GC (Knulli: -1.5 fps, the pauses as
large). The Knulli also gets 18 rest poses in flipbooks.txt (grass and the like: Greenpath 52.6 - 52.9
-> 54.3 - 54.6) and `hk-aspect` (3:2: the same fps, Greenpath 52.6, Dirtmouth 58.3).
Test release 2026.11.08 (the present thread, the fill, the rest poses; its own files, screenshots
checked): Knulli with the picture filling the screen, steady fps, Colosseum 59.5, Mantis Village 59.5,
Dirtmouth 59.5, Kingdom's Edge 57.8, Deepnest 57.4, City of Tears 57.3, Crystal Peak 56.0, Queen's
Gardens 54.8, Greenpath 54.3; Hall of Gods fights Vengefly King 58.7, Nosk 57.2, Flukemarm 56.6,
Oblobbles 56.5, Mantis Lords 56.1, Hive Knight 55.7, Gruz Mother 54.8, The Collector 54.0, Uumuu
54.0, Broken Vessel 52.9, Crystal Guardian 52.1 (showing more of the room costs the GPU-heavy fights
1 - 3 fps against the bars); G350 Hall of Gods fights Gruz Mother 35.4, Mantis Lords 32.8.
2026-11-08 night, the G350 with ua_host's GL thread (`UA_GL_REC=auto`: the render thread records each
frame, a thread of the host's makes the driver calls and the swap; portmaster-hollowknight f51ce92),
against the present thread alone above: Greenpath 25.1 -> 29.8 - 33.1, Colosseum 38.7 -> 43.4,
Crystal Peak 36.4 -> 39.7, City of Tears 32.4 -> 35.3, Queen's Gardens 35.4 -> 38.2, Mantis Village
43.1 -> 45.1, Kingdom's Edge 41.7 -> 42.9, Deepnest 41.7 -> 41.4, King's Pass 38.1 -> 36.7, Dirtmouth
within its runs' spread (41 - 45 either way this time); Hall of Gods Mantis Lords 36.3. Greenpath's
GL thread is 27 - 29 ms of its frame there. Measured and left out the same night (exact, no gain that
could be told apart): asmpatch's playmaker-idle (Knulli Greenpath 52.3 both ways), the arena's
sub-data copies (muOS, G350), skipping binds of what is bound (G350), `UA_FAST_VERTS` (Knulli +0.3, G350
none), and `UA_GL_MERGE` (draws 238 -> 138 a frame, slower: profiling.md); later `UA_INV_MEMO` and
asmpatch's `hk-camerafade` (the fade texture's uploads 2.8 -> 0.3 a frame; Knulli, muOS and G350
within their runs' spread), both opt-in.
Test release 2026.11.08.2 (the GL thread on RK3326 handhelds, and the setup's update applying the
fill in place to a game set up by an earlier release; its own files): G350 Greenpath 32.4, Dirtmouth
41.4; Knulli Greenpath 52.5, City of Tears 53.3, Queen's Gardens 54.3.
2026-11-08, the buttons by position (`UA_PAD_NAME` an Xbox pad's, asmpatch's `hk-pad-letters`), checked on
the RG34XX-SP in Dirtmouth through the setup's one-time update (the release candidate's own asmpatch
under `pmu apk-asmpatch`): the bottom button confirmed through the title, the save and the menus and
jumped (the Knight from 11.4 to 17.0 and down), right walked; Options, Controller showed the Switch Pro
Controller with B jump, Y attack, A focus / cast, X Dream Nail, and Remap Controller B, Y, A, X, ZR, ZL,
L, R; `UA_PAD_LETTERS=xbox` the game's Xbox 360 picture, the same positions. Test release 2026.11.08.3
(2026.11.08.2's files with that fix and the new READMEs).

Enter the Gungeon on the BattleXP G350 (dArkOS, RK3326, 895 MB, no swap; 2026-11-08, the port's branch
`physics-active` with its G350 commit): the port's own setup on the handheld from GOG's installer, offline,
EmulationStation running: 1157 s, the tools' peak 331 MB, at least 424 MB available. As installed, 3 of 5
launches hung at the first load (PlayMaker's FSMs loaded on Unity's loading thread and the main thread at
once: asmpatch's `playmaker-loadlock`, 6 of 6 since) and the Keep's load ran out of memory (43 MB available,
the memory watchdog's stop: pmu's `textures.readable`, 156 readable atlases no longer held twice: about
105 MB available with release, 182 MB with convert, the port's choice since aaab07a; 75 MB with 110 MB held
by a stand-in for EmulationStation, which stays resident on dArkOS while a port runs). The picture fills the width (480x360 scaled to 640x480 by ua_host's present
thread: 640x360), with the GL thread (`UA_GL_REC=auto`). The Keep's first fight (fixed seed, no damage, the
same room and enemies as on the Knulli): 29.8 fps with the present thread, 31.1 with the GL thread too (Low
lighting and shaders), 39.1 / 40.1 with the port's lowest settings and its HUD patch (portmaster-gungeon 27aebd7
set up on the handheld: 1122 s); the main thread is the
limit (98.6 % on a CPU, the GPU at 400 MHz). One job worker gained nothing. 2026-11-09: ua_host's
`UA_GL_PROGCACHE` (Unity re-linked about 150 programs a launch) and the JIT warm-up on Mono 2.6 (`UA_JIT_WARM=2`):
the first room's entry 16 -> 6 frames over 50 ms, the launch to the Keep about 13 s shorter; `UA_MONO_INVOKE`/`TLS`
36.5 -> 40.4 fps in the fight. The port with all of them and its df-no-body patch (portmaster-gungeon c0da303,
a rehearsal of its zip copied to the G350, the second launch): the fight 42.3 fps, the room entry 42.8 (7 frames
over 50 ms, p99 44 ms), lowest MemAvailable 198 MB. Details:
[profiling.md](../profiling.md#enter-the-gungeon-on-the-battlexp-g350-rk3326-2026-11-08).


## Linux builds, one per Unity line (32-bit route)

| game | Unity, build | emulated rig | handheld | notes (last checked) |
|---|---|---|---|---|
| [Unsung Warriors - Prologue](https://unsungwarriors.itch.io/unsung-warriors-prologue) | 2017.4.40f1, Mono 2.x, Rewired | intro video plays, title, menus to profile select | title 59.6 fps (intro video plays) | needs asmpatch rewired-android (2026-10-21) |
| [Loco Motive (Game Jam)](https://robustgames.itch.io/loco-motive-jam) | 2018.4.36f1, Mono 4.x | title; a pointer click starts the game | title 59.6 fps | mouse-driven menu: `UA_POINTER=1` (2026-10-21) |
| [buggish](https://norrimo.itch.io/buggish) | 2019.4.8f1, Mono 4.x | splash, title; pointer starts the game | title 59.6 fps | mouse-driven menu (2026-10-21) |
| [Tom City](https://tomanddad.itch.io/tomcity) | 2020.3.49f1, Mono 4.x | splash, title; a click starts a new city | title 59.6 fps | its crunched DXT textures froze the handheld until converted to ASTC (textures step) (2026-10-21) |
| [Untitled Hook Game](https://bonionline.itch.io/untitled-hook-game) | 2021.3.16f1, URP | name prompt on first launch; on-screen keyboard types the name; main menu | title 59.6 fps | (2026-10-21) |
| [Tiny Kingdom](https://neltile.itch.io/tiny-kingdom) | 2022.3.36f1, URP, FMOD, Steam, Burst | title (play/settings/exit) | title **59.6 fps** with the desktop's pacing (`UA_FRAME_RATE=desktop`, the default for 2021.1+ conversions); 30 fps before | no ARM FMOD: silent; Steam stub; the 30 fps was the 2021.1+ Android player's default (2026-10-21) |

## More Unity versions

| game | Unity, build | emulated rig | notes (last checked) |
|---|---|---|---|
| [Petty Puny Planet](https://whalesandgames.itch.io/petty-puny-planet) | 5.6.0f3 | main menu | (2026-10-20) |
| [Shiver](https://aquiluslupus.itch.io/shiver) | 2017.2.0f3 | starts (Adventure Creator's text and PlayerPrefs, Steam's failed init handled as on a PC without Steam); title menu (New Game, Load, Options, Quit) in the rig, on its pbuffer and on SDL's window path (offscreen driver) | was black (0,0,0) in every frame: its quality level (Fantastic) uses 2x MSAA, which makes the 2017.2 player present through its blit path, but it went on drawing into the window and presenting its own, untouched MSAA framebuffer over it (docs/compat/unity-5.6-2017.2.md, Graphics). Fixed in the runtime (`UA_MSAA_FIX`, on for 2017.1 - 2017.3): the window is presented, single-sampled, without MSAA (2026-10-22) |
| [Arthur's Nightmare](https://varunramesh.itch.io/arthurs-nightmare) | 2017.3.1f1, .NET 3.5, two VP8 VideoPlayers | Unity splash, title (New Game) | the first 2017.3 game; its first start from the USB drive stayed on frame 1 for the whole 600 s (cold reads of the APK), the next one reached the title (2026-10-22) |
| [White Mirror](https://teogaillard.itch.io/white-mirror) | 2021.2.9f1, URP, FMOD 2.01.07, Input System, an anti-cheat package | title menu (Play / Highscore / Controls / Credits / Quit) | the first 2021.2 game; silent without FMOD's ARM libraries (DllNotFoundException: fmodstudio, as android-data warns); asmpatch inputsystem-android applied (2026-10-22) |
| [The Third Eye](https://wannabuh.itch.io/the-third-eye) | 2022.2.15f1, URP, Input System, Cinemachine | first scene (a dark first-person room with a window) | the first 2022.2 game; asmpatch inputsystem-android applied; 84 shaders translated (2 not); not compared with the desktop picture (2026-10-22) |
| [Hey, Listen!](https://andrecardoso.itch.io/hey-listen) | 2023.1.8f1, URP, Input System, Visual Scripting, DOTween | title (its logo over the first room, its own cursor), after its "Loading..." screen: 30 minutes, 73 frames in the rig | the first 2023.1 game (2023.2 runs). Needs pointer mode, `UA_POINTER=1 UA_POINTER_CURSOR=0` (it draws its cursor): its cursor, companion and clicks read the Input System's `Mouse.current` with no null check, which is null without a mouse, so it threw a NullReferenceException in CursorHandler.Update and CompanionVisual.Update every frame; with it, none (`port check` now suggests it: asmpatch's survey finds such reads). The long load is the rig's speed, not a fault: the game loads its 18 rooms one by one (`LoadSceneAsync`, additive, each awaited; all 18 started, then `LightProbes.Tetrahedralize` and the end of loading at 30 minutes), and every frame draws all the rooms loaded so far behind the overlay (8 rooms: 2137 draws, 1.9 M vertices, 1466 draws into the 2048x2048 shadow map), 15 - 45 s per frame on the software renderer. The title still draws all 18 (3840 draws, 3.9 M vertices a frame, most of them shadow casters): a load for the handheld's GPU too (2026-10-22). Before: It aborted at start ("TuningForkUtils: Can't find realtime_features.json in APK", then "std::length_error: basic_string"): 2023.1's player reads the Android Game SDK's Memory Advice models (realtime_features.json, realtime.tflite, ...) from the APK, which a desktop build lacks; `player fetch` now takes them from Unity's installer and android-data puts them in the APK's assets for players that read them ("Initialized TensorFlow Lite runtime") (2026-10-22) |
| [Chassis Chase](https://frog626267.itch.io/chassis-chase) | 2023.2.20f1 | main menu | (2026-10-20) |
| [Lo-Fi Room](https://bearmask.itch.io/lofi-room) | 6000.0.58f2 | logo, title | (2026-10-21) |
| [DEEP (Beta)](https://fog-icmc.itch.io/deep) | 6000.2.6f2, URP, FMOD + Resonance, Input System, Unity Services | **crashes at start** (Mono SIGSEGV in `mono_class_from_name`, before the first frame) | the first 6000.2 game. android-data first failed in its check of globalgamemanagers (UnityConnectSettings: UnityPy's tree for 6000.2.6 has a field the data lacks; fixed). The build ships no `netstandard.dll` (and 21 more of the assemblies its ScriptingAssemblies.json lists); the Android player's own UnityEngine.CoreModule references netstandard 2.1.0.0, so its types do not load ("Could not load file or assembly 'netstandard, Version=2.1.0.0'", then 306 "Unable to find type [UnityEngine.CoreModule.dll]...") and Mono crashes. android-data warns about it, and `pmunity winbcl --player` (which a port's setup runs) now adds Unity's own facade of the game's version (`unityjit-linux/Facades/netstandard.dll` 2.1.0.0 from the 6000.2.6f2 Windows editor installer, 1.1 MB read); netstandard is the only assembly the engine modules reference that the build lacks. With it those types load, and the next stop is "globalgamemanagers is corrupted" ("Position out of bounds"), "Desired shader compiler platform 9 is not available in shader blob", "TypeLoadException: VTable setup of type UnityEngine.Rendering.ShaderStrippingSetting failed" (URP's core package), SIGSEGV. The other 21 missing assemblies are package assemblies only the developer has (ProBuilder's Csg, KdTree, Poly2Tri and Stl, Splines, URP's Config and Shaders, Core.ShaderLibrary, LightTransport, parts of Unity Services, ...; no shipped assembly references them): an incomplete build, not faked; not fixed (2026-10-22) |
| [DIG UP (test)](https://asestr.itch.io/dig-up-test) | 6000.3.11f1 | main menu | (2026-10-21) |
| [Flowerist's Horror](https://trazam-dev.itch.io/flowerists-horror) | 6000.4.6f1, URP (HDRP's assembly shipped, not used), PlayMaker, Buto fog, FBX SDK, Input System | title menu (PLAY / CREDITS / SETTINGS / QUIT) | the first 6000.4 game; 115 shaders and 9 of 11 compute shaders translated (2026-10-22); asmpatch's PlayMaker optimizations (`playmaker-loopcount`, `playmaker-delayedevents`) find the same code in its PlayMaker.dll as in Hollow Knight's and apply (`--list`; not run on the handheld, 2026-10-24) |
| [Ashley's Wacky Abandoned Building](https://davadoo.itch.io/ashleys-wacky-abandoned-building) | 6000.5.7f1 | title menu | (2026-10-21); its Vulkan programs also translate (`--from vulkan`, checked in memory): 80 of 90 shaders, 2180 programs, interfaces as the GLCore twins', all 1673 distinct link on Mesa (2026-10-23) |
| [Phasing Gun](https://atalachy.itch.io/phasing-gun) | 6000.6.2f1 | first scene (in game) | (2026-10-21); its Vulkan programs (`--from vulkan`, checked in memory) gave no program until the converter read 6000.6's new parameter entries; now 97 of 107 shaders, 526 programs, interfaces as the GLCore twins', all 464 distinct link on Mesa (2026-10-23) |
| [IronQuiet](https://aeroforger.itch.io/ironquiet) | 6000.6.3f1 | title menu over its 3D scene | about a minute per frame in the rig (2026-10-21); Vulkan programs (`--from vulkan`, in memory): none before 6000.6's parameter entries were read, now 48 of 54 shaders, 300 programs, all 273 distinct link on Mesa (2026-10-23) |

## Windows-only builds (D3D11 shaders translated)

A port made with `pmunity port new` does all of it on the handheld: the release ships `pmhlslcc`
for aarch64, and the setup fetches a 2021.2+ build's Unix class libraries (`pmunity winbcl`;
docs/porting.md, "Routes"). The rows say which games were also made into a port and rehearsed.

| game | Unity, build | emulated rig | shaders converted (last checked) |
|---|---|---|---|
| [Red Handed](https://badpiggy.itch.io/red-handed) | 2018.3.7f1, 3D | its first level | 33 of 34 (2026-10-20) |
| [Out of Ctrl](https://miknugget.itch.io/out-of-ctrl) | 2019.2.5f1 | title | 42 of 44 (2026-10-20) |
| [Cell Machine](https://samhogan.itch.io/cell-machine) | 2019.3.14f1, URP | splash, title menu | 60 of 62 (2026-10-20) |
| [Little Lars Diary](https://eufrasi0.itch.io/little-lars) | 2020.3.44f1 | splash, click-to-start | 50 of 52 (2026-10-20) |
| [10 Second Mixtape (Jam Version)](https://scottts.itch.io/10-second-mixtape) | 2021.3.28f1, compressed, Input System | title menu, as the port installed it | 49 of 51; needs `pmunity winbcl`. **As a port** (the game's folder in `gamedata/`): the setup rehearsed in the arm64 container unpacked `data.unity3d`, read 8 Unix class libraries (4.5 MB, 69 s), translated the shaders with the release's pmhlslcc (5 s) and ran asmpatch's inputsystem-android patch: 408 s in all, 68 MB installed; then the title menu in the rig, no exceptions (2026-10-22) |
| [Memory Remains](https://pokeydem.itch.io/memory-remains) | 2022.3.62f3, URP, Burst | title menu, as the port installed it | 82 of 85. **As a port** (the game's folder in `gamedata/`): the setup rehearsed in the arm64 container read 22 Unix class libraries (5.8 MB, 31 s) and translated the 569 programs with the release's pmhlslcc (33 s): 564 s in all, 92 MB installed; then the title menu in the rig (2026-10-22) |
| [Tacticsweeper](https://moocowsgomoo.itch.io/tacticsweeper) | 2022.2.17f1, 32-bit build | title menu (New Game / Settings / Exit Game), as the port installed it | 45 of 47; the first 2022.2 Windows build. **As a port** (`pmunity port new`; the game's zip in `gamedata/`): the setup rehearsed in the arm64 container (`tools/port/rehearse.sh`): the release's pmhlslcc translated the shaders there (4 s), `pmunity winbcl` read the 22 Unix class libraries from Unity's Windows editor installer (5.8 MB, 40 s), 265 s in all, 74 MB installed; then the title menu in the rig (2026-10-22) |
| [Die to Play](https://mrkvinter.itch.io/die-to-play) | 6000.1.4f1, URP 2D, Localization, Addressables (binary catalog), SQLite, R3, VContainer, UniTask | its first scene: the dark first-person room with the retro computer showing START GAME, the W hint; no exceptions; 0.1 - 0.3 fps | was a full-screen green picture: the D3D11 programs of URP's full-screen passes kept Direct3D's texture-coordinate flip (`UNITY_UV_STARTS_AT_TOP` in the Core RP library's full-screen triangle and quad), so the colour-grading LUT came out upside down and UberPost mapped the dark picture to green (found with `UA_GL_TRACE_DRAWS`: the LUT's pixels `(0,189,66)` where a neutral one has `(0,66,66)`); fixed in the D3D11 path (`dxbc.gl_uv_origin`, docs/shaders.md), the LUT is neutral and the picture upright. It also reads the Input System's `Mouse.current` with no null check (SelectTargetState.Tick): `UA_POINTER=1`, which `port check` now suggests. The 8 shaders left out (VoxelizeShader, VrsVisualization, HDRDebugView, the VFX quad: buffers or UAVs; CoreCopy's MS array pass; DBufferClear without programs; MSAA depth blit, VR blit): its first scene shows no error-shader (magenta) draw. 135 of 143 (URP 2D's sprite shaders only after leaving out their GPU sprite skinning variants; 133 before, every sprite with the error shader). Three converter faults on the way, all fixed: winbcl replaced the game's NuGet System.ComponentModel.Annotations 5.0 with Unity's 4.2 facade (ReflectionTypeLoadException), the Addressables catalog's Windows paths ("Invalid path in AssetBundleProvider: …aa\\StandaloneWindows64\\…", so "SelectedLocale is null"), and its binary catalog's bundle CRCs (2026-10-22) |
| [US&THEM](https://derrickmoore.itch.io/not-halo) | 6000.3.16f1, URP, Entities, Addressables, Input System, websocket | Unity splash, its title screen over a 3D ship (news ticker, "EXCUSE OUR MESS" panel) | 112 of 130; the first Unity 6 Windows build. Its first run was black: `pmunity.winbcl` failed on 6000.3's Windows editor installer (a solid NSIS whose 3.4 MB header takes two LZMA chunks; fixed), so the game ran on the unityjit-win32 class libraries (Interop+Kernel32, "type initializer for 'UnityEngine.GUI' threw" every frame); with the 23 Unix libraries it starts. Entities' `liblz4` has no ARM build (android-data warns; nothing called it here). 1.4 GB resident in the rig (2026-10-22) |

The shaders left out everywhere are BlitToDepth_MSAA and the VR blit.

## Android builds (APK route)

As ports (`pmunity port new --route apk`): the release zip rehearsed with `tools/port/rehearse.sh`
(the setup in an arm64 container that stands in for the handheld, with its 32-bit loader or,
`REHEARSE_64ONLY=1`, without), then the installed game in the emulated rig.

| game | Unity, build | emulated rig | handheld | notes (last checked) |
|---|---|---|---|---|
| [Imperium Host](https://vectorinox.itch.io/imperium-host) | 2020.3.3f1, IL2CPP arm64 + armv7 | title menu; as a port: the setup takes the armeabi-v7a player where the system has a 32-bit userland and the arm64-v8a one on a 64-bit-only system, and the 64-bit install reaches the title menu in the arm64 rig | title 59.6 fps (64-bit, by hand) | the port's setup takes 40 s in the container; nothing downloaded (2026-10-21) |
| [Cat Cove Inn](https://loaflabs.itch.io/cat-cove-inn) | 2021.3.4f1, IL2CPP + Burst | intro, title (arm64 and armv7); as a port from an `.xapk`: the 64-bit-only setup, then intro and title menu in the arm64 rig at the build's own 30 fps | title 30 fps (64-bit, by hand) | leaves Unity's Android default frame rate, as the port keeps it (2026-10-21) |
| [The Ump Show (Demo)](https://azeemba.itch.io/the-ump-show) | 2022.3.14f1, IL2CPP + Burst, FMOD, 3D | title over its 3D scene | title 30 fps (64-bit) | leaves Unity's Android default frame rate (2026-10-20) |
| [ROOTS](https://pierre-vandermaesen.itch.io/roots) | 2019.4.18f1, Mono armv7 | title. 64-bit: `pmunity apk --il2cpp` compiled its 89 assemblies for 2019.4.18f1's arm64 IL2CPP player (15 min in Docker; `libil2cpp.so` 30.0 MiB, metadata 24); in the native arm64 rig the title at about 140 fps, and with `UA_POINTER=touch` PLAY starts a run (the root grows, then RECOLT), no exceptions | | stalls for minutes at times under QEMU on the 32-bit route (2026-10-22) |
| [Chapéu de Papel](https://carambo.itch.io/paper-hat) | 2022.3.62f3, IL2CPP arm64, Input System, 98 MB of H.264 clips | title menu at 59.6 fps in the arm64 rig; the pad (A) starts it and walks the first scene's dialogue | | the desktop builds are no use: its macOS build has Metal shaders only (below); the clips were not reached (2026-10-22) |
| [Calculords](https://richjoslin.itch.io/calculords) | 2022.3.23f1, Mono armv7, GLES2 | title; as a port: the setup with a 32-bit userland (armeabi-v7a only, so the release ships only `ua_host` and port.json asks for armhf), then "Tap anywhere to start" in the armhf rig. 64-bit: `pmunity apk --il2cpp` compiled its 109 assemblies for 2022.3.23f1's arm64 IL2CPP player (Bee graph, 607 commands, 31 min in Docker; `libil2cpp.so` 53.1 MiB, metadata 29; base.apk 18.7 MiB without the 32-bit player and the assemblies); in the arm64 rig the title at its own 30 fps, and pointer taps lead into its first battle's tutorial, no exceptions. As a port with `player.il2cpp = true` (the release then ships `ua_host64` too): its setup rehearsed on a 64-bit-only stand-in (`REHEARSE_64ONLY=1`, 1 GB + 1 GB swap) in 38 min (the tools' downloads, UnityLinker 0.4 min, the conversion 8.6 min with a 989 MB peak, the compile 11.6 min with 2 jobs, 573 MB peak), installed 106 MB; the installed game reaches the same battle in the arm64 rig | | the check suggests `UA_POINTER=1` (UI pointer handlers, no pad navigation) and, for 64-bit-only systems, `player.il2cpp = true` (2026-10-22) |

## 64-bit IL2CPP route (desktop Mono games compiled with `pmunity il2cpp`)

| game | Unity | emulated rig (arm64) | handheld (64-bit userland) | last checked |
|---|---|---|---|---|
| Cuphead + The Delicious Last Course (GOG, macOS) | 2017.4.9f1 data on the 2017.4.40f1 player | title, main menu; its setup rehearsed on a 64-bit-only stand-in | **title 59.6 fps** (59.3 average over 150 s, lowest window 57.2), 424 MB resident; built on the Mac (portmaster-cuphead's build64.sh), run with the arm64 ua_host on the RG34XX-SP's 64-bit system; the release's whole 64-bit setup on the RG34XX-SP too (CUPHEAD_ROUTE=64, the network through adb): converting 2 h 40 min, the IL2CPP build on the handheld 1 h (one compiler ran out of memory, and the build went on with one job at a time), packing 20 min, then the title at 59.6 fps. Since 2026-10-24 the IL2CPP compiles run up to four at a time as memory allows (37 of them on the handheld: 391 -> 276 s; the whole IL2CPP step estimated ~35 min instead of 61; the library the same), the encoder alone needs 84 min of the textures step at medium, 28 at fast ([setup-speed.md](../setup-speed.md#the-64-bit-setup-compiling-and-encoding-2026-10-24)); a whole setup with these not yet run on the handheld. The Devil fight (the port's test hooks: god mode, the homing weapon) 59.4 fps, held by vsync, the Mali at 504 MHz, FMOD's mixer 41 % of a core, 266 MB available; with every clip the audio step switched back to DecompressOnLoad 59.5 fps, the mixer 34 %, 243 MB available, the level's load 2.05 s against 1.95 (one run each, 2026-10-24; [profiling.md](../profiling.md#fmods-mixer-and-the-audio-step-2026-10-24)) | 2026-10-24 |
| Loco Motive (Game Jam) | 2018.4.36f1 | title menu; as a port made with `--route il2cpp-arm64`, its setup (the IL2CPP build on the device included) rehearsed in an arm64 container with 1 GB, then the title menu | title 59.4 - 59.6 fps | 2026-10-21 |
| buggish | 2019.4.8f1 | title | title 59.4 - 59.6 fps | 2026-10-20 |
| Tom City | 2020.3.49f1 | title | title 59.4 - 59.6 fps | 2026-10-20 |
| [Untitled Hook Game](https://bonionline.itch.io/untitled-hook-game) | 2021.3.16f1, URP | the first-launch name prompt, then (a name in PlayerPrefs) the main menu, about 30 fps in the native arm64 rig; the first 2021.x build of the route: il2cpp's Bee build graph run by pmunity (605 commands, `libil2cpp.so` 53.2 MiB, metadata 29). As a port made with `--route il2cpp-arm64`: its setup rehearsed on a 64-bit-only stand-in (`REHEARSE_64ONLY=1`, arm64 container, 1 GB + 1 GB swap) in 32 min (the IL2CPP conversion 10.6 min with a 987 MB peak, the compile 9 min with 3 jobs, 599 MB peak), installed 230 MB; the installed game reaches the name prompt and the main menu in the arm64 rig | not yet | 2026-10-22 |
| [Tiny Kingdom](https://neltile.itch.io/tiny-kingdom) | 2022.3.36f1, URP, FMOD, Steam, Burst | title menu at 60 fps in the native arm64 rig; the first 2022.x build of the route (NDK r23b, 780 Bee commands, `libil2cpp.so` 82.0 MiB, metadata 31); silent as on the Mono route until FMOD's ARM libraries are supplied, the Steam stub answers | not yet | 2026-10-22 |
| [MineRalph](https://chopchopgames.itch.io/mineralph) | 2018.3.2f1, localization XML in StreamingAssets, Steamworks.NET | title menu with its localized text, about 20 fps in the native arm64 rig: the first build of the route that reads StreamingAssets with System.IO (asmpatch's streaming-assets patch compiled in, the files beside the APK) and the first 2018.3 one (`libil2cpp.so` 34.5 MiB, metadata 24; 2018.3's il2cpp needed `--map-file-parser` as 2017.4, and its Plugins copies of I18N.dll are now kept as the game's). Steamworks.NET's callbacks throw under IL2CPP (delegates to generic methods), after the Steam stub's failed init. As a port made with `--route il2cpp-arm64` (the check's `streaming_assets = "both"`): its setup rehearsed on a 64-bit-only stand-in (`REHEARSE_64ONLY=1`, 1 GB + 1 GB swap) in 33 min (the tools' downloads; UnityLinker 0.2 min; the conversion on .NET 8 4.2 min, 626 MB peak; the compile 14.7 min, 692 MB peak), installed 381 MB; the installed game reaches the same title menu in the arm64 rig (`EMU_ONLY=1 EMU_ARCH=arm64`) | not yet | 2026-10-22 |
| [Unsung Warriors - Prologue](https://unsungwarriors.itch.io/unsung-warriors-prologue) | 2017.4.40f1, .NET 3.5, Rewired | both intro videos, title menu (about 45 fps in the native arm64 rig); with `--asmpatch rewired-android` the pad skips the intro and opens the main menu. Sound: 2017.4's arm64 FMOD lost its mixer thread to glibc's larger minimum stack until the loader raised small stack sizes. The armv7 IL2CPP build reaches its title in the armhf rig, with sound | not yet | 2026-10-21 |
| Hollow Knight (GOG, Linux) | 6000.0.61f1, Mono .NET 4.x | title (about 50 fps), Start Game, profile, prologue, the opening cinematic, King's Pass: the Knight walks and jumps, the tutorial's Switch prompt (about 40 fps in the native arm64 rig, Mesa's 8 render threads at 65 - 72 % each, UnityMain at 6 % of one core); no exception. A mixed build (2022.3.62f3's il2cpp for 6000.0.61f1's libil2cpp) with the port's asmpatch patches compiled in (inputsystem-android, streaming-assets): 763 Bee commands, `libil2cpp.so` 80.0 MiB, metadata 31; APK 1.93 GiB + OBB 1.84 GiB, and libunity copied `Managed/` out of the APK with the OBB mounted. the game turns the collector off: without `UA_GC_BUDGET` the rig's resident size grew 9 MB a minute in King's Pass; with it (IL2CPP's collector), a collection about every 4 minutes there (88 - 107 ms in the rig) and a flat resident size for 12 minutes; a save written by this build loads | **King's Pass 32.4 fps** (30.6 before `UA_GL_DSCLEAR`, two runs each; the 32-bit port there: 31.8 - 32.5), UnityMain on a CPU 80 % of the time and about 29 ms of GPU work a 32.7 ms frame: both near their limits; main menu 38.7 fps (36.1); 42.6 with host flips (38.2 in the same session, 2026-10-23). Memory is the tight part: 44 - 58 MB available in King's Pass with about 250 MB in swap, and the load from the save swaps (4 - 30 s; the bench's watchdog line needs `UA_MEM_MIN=20`). Leaving out the three grab copies a frame and having the blend-mode shaders read the framebuffer (`GL_ARM_shader_framebuffer_fetch`) measured 35.0 - 35.3 fps; not exact (overlapping sprites of one batch): now the opt-in `shaders.grab_fetch` ([shaders.md](../shaders.md#blend-modes-without-grabpass-copies-grab_fetch-opt-in); 34 of its blend-mode shaders; in the arm64 rig the scene became one pass, 18 passes a frame instead of 24, every static pixel the same as without it. On the handheld with `"always"`, three runs each, interleaved: **King's Pass 33.2 -> 37.0 fps** (33.4 / 32.7 / 33.4 -> 37.9 / 37.0 / 36.1; at the same GPU clock, 614 and 612 MHz on average, 33.4 -> 36.1), 18 passes a frame instead of 24, all 230 GLES3 programs compiled by the Mali (ARM's extension); about 1.6 % of the static pixels off by 1 - 6 of 255 (ARM's fetched colour is mediump), none where two runs without it differ by more than 1). An earlier "36 fps in King's Pass, GPU-bound" was the main menu: the bench plan stopped one press short. GPU counters in King's Pass (`GPUCNT=1`, 33.7 fps, the GPU at 600 MHz 82 % of the time, 2026-10-24): the arithmetic unit issues in 81 % of the shader core's cycles, texturing 38 %, varyings 29 %; 28 % of the tiles eliminated as unchanged ([profiling.md](../profiling.md#gpu-counters-gpucnt)). Unity's texture copies (the grab copies, the blur's copy back) are not exact copies on the Mali-G31 (its blit samples at mediump; [profiling.md](../profiling.md#passes-left-out-glpassc-glrenamec)), so leaving them out exactly (`UA_GL_RENAME`) finds nothing to leave out there; `UA_GL_RENAME=near` (opt-in, not exact: about 3 % of the static pixels off, mostly by 1) **33.3 / 33.9 -> 36.0 / 36.5 fps** (2026-10-24). The audio step's budget (the game's resident atmosphere loops decoded at load as it has them): FMOD's mixer 54 - 55 -> 50 - 51 % of a core at the default 16 MiB, 47 at 36 MiB; King's Pass 34.7 / 34.5 -> 35.1 / 34.9 and 35.8 / 35.3 fps (45 s runs; the Mali at 696 MHz 22 -> 21 - 24 and 28 - 32 of 45 s), available memory there as before (41 - 51 MB; [profiling.md](../profiling.md#fmods-mixer-and-the-audio-step-2026-10-24)) (2026-10-24); with `UA_RENDER_SIZE` (opt-in, softer) King's Pass 600x400 40.0 fps, 540x360 41.1 against 33.9 (2026-10-24). **The 1 GB options together** (the port's `[runtime_lowmem]`: `UA_RENDER_SIZE=600x400`, `ASMPATCH_HK_BLUR=low`, `UA_GFX_JOBS=legacy`, on data converted with `grab_fetch = "always"` and `pcm_budget_mb = 36`): **King's Pass 36.2 / 34.9 -> 51.0 fps** (the GPU at 696 MHz 94 / 65 -> 61 % of the time; 40.9 with the render size alone, 42.8 with the blur, 46.7 with the data variant, 51.0 with the graphics jobs: from 600x400 on the main thread is the limit), the picture with the graphics jobs as without them (two shipped runs differ more), the framing the same; a 60 s walk-through at 42 - 53 fps (the table in the Hollow Knight notes under "Full ports") (2026-10-24) | 2026-10-24 |
| [Lo-Fi Room](https://bearmask.itch.io/lofi-room) | 6000.0.58f2, Addressables, Localization, TextMeshPro, Newtonsoft.Json, Steamworks.NET, incremental GC | title at 60 fps in the native arm64 rig (17970 frames in 300 s, no exception): the first Unity 6 build of the route, a mixed build (2022.3.62f3's .NET il2cpp converting for 6000.0.58f2's libil2cpp, UnityLinker, class libraries and player; API 23), 663 Bee commands, `libil2cpp.so` 68.0 MiB, metadata 31, all 157 `il2cpp_*` lookups of its libunity exported. Run from the internal disk: from the USB drive, through Docker's file sharing while another job wrote to it, the same build took 70 s to its first log lines and 300 s to its first scene | not yet | 2026-10-22 |
| [Chassis Chase](https://frog626267.itch.io/chassis-chase) | 2023.2.20f1 | main menu at 60 fps in the native arm64 rig (no exception): the first 2023.x build of the route, mixed with 2022.3.23f1's il2cpp (2023.2's libil2cpp reads metadata 29, as that release writes), 707 Bee commands, `libil2cpp.so` 75.4 MiB, all 163 `il2cpp_*` lookups exported | not yet | 2026-10-22 |

The IL2CPP build itself can also run on the handheld (`pmunity il2cpp --on-device`): Loco Motive
built in an arm64 container limited like the RG34XX-SP (1 GB, 4 CPUs, no swap), with the same
`libil2cpp.so` as the PC route (2026-10-21), and Untitled Hook Game (2021.3, Bee) in its port's
setup on a 1 GB + 1 GB swap stand-in (2026-10-22); not yet on a handheld.

## 64-bit Mono route (desktop Mono games on the arm64 player with Unity's Mono, `mono-arm64`)

The game's own assemblies on Unity's Mono built for aarch64 Linux, behind a `libil2cpp.so` bridge in
Unity's arm64 IL2CPP player ([mono-arm64.md](../mono-arm64.md)); no IL2CPP compile.

| game | Unity | emulated rig (arm64) | handheld (64-bit userland) | last checked |
|---|---|---|---|---|
| buggish | 2019.4.8f1 | title (75 - 170 fps under llvmpipe), 92 assemblies, 7,626 internal calls, Mono up 31 - 104 ms after `il2cpp_init` | **title 59.5 - 59.7 fps** (each second after the first; 57.6 over 21 s with the start), resident 255 MB, 677 MB available; Mono up 123 ms after `il2cpp_init` | 2026-10-24 |
| Tom City | 2020.3.49f1 | title; as a port made with `--route mono-arm64`, its setup rehearsed on a 64-bit-only stand-in (1 GB) in 13 min (no compile), 324 MB installed, then the title | | 2026-10-23 |
| Untitled Hook Game | 2021.3.16f1, URP | the first-launch name prompt at about 28 fps, as the IL2CPP build (about 30) | | 2026-10-23 |
| Cuphead (GOG macOS, .NET 3.5) | 2017.4.9f1 data on 2017.4.40f1's player, Mono 5.0.1 with Mono's 4.x class libraries (`unityjit`) | title, slot select, Veggies, Frogs, Forest Follies, the win screen; no managed exception (as the 32-bit build); Mono up 54 - 84 ms, 64 assemblies, 6,027 internal calls; Veggies replayed deterministically as the IL2CPP build (frames identical or 74 - 105 pixels off by 1/255); saves byte-identical to the 32-bit build's but for the order of two namespace declarations in Dialoguer's XML ([mono-arm64.md](../mono-arm64.md#a-net-35-game-on-the-4x-class-libraries-cuphead)) | | 2026-10-26 |
| Hollow Knight (GOG, Linux) | 6000.0.61f1 | first launch's language screen at 60 fps, 116 assemblies (the port's asmpatch patches applied to its assemblies), resident 1,543 - 1,573 MB as the IL2CPP build's in the same rig | **King's Pass 34.0 fps** (IL2CPP 34.4, the 32-bit port 35.2 in the same session, at their GPU clocks: GPU-bound); with flat fragment shaders 46.1 fps (IL2CPP 48.8, 32-bit 48.9: the main thread 6 % slower); resident 720 - 731 MB, 47 - 62 MB available, as the IL2CPP build ([mono-arm64.md](../mono-arm64.md#hollow-knight-on-the-handheld-the-mono-route-against-il2cpp-and-the-32-bit-port)). With asmpatch's PlayMaker optimizations, flat shaders, A B A B: 46.3 / 46.3 -> 48.3 / 47.9 fps (+3.9 %, [profiling.md](../profiling.md#playmakers-per-frame-work-asmpatch-optimizations)); the `hk-blur` reductions measured on this route ([profiling.md](../profiling.md#hollow-knights-blur-and-particles-on-1-gb-handhelds)) | 2026-10-24 |

## Handheld benches (RG34XX-SP, 2026-10-23)

Folders packed with the current tools, run with `tools/device/profbench.sh` over USB (adb): a plan of
presses with no one at the handheld, then 20 - 30 s measured. "Limit" is what the measurement shows:
the GPU at its 696 MHz maximum, the main thread busy, or a frame-rate cap.

| game | Unity, route | where the plan got | fps | limit, notes |
|---|---|---|---|---|
| Petty Puny Planet | 5.6.0f3, 32-bit | the planet (in game) | 59.5 | - |
| Arthur's Nightmare | 2017.3.1f1, 32-bit | title 59.3; the 3D room after the cutscene 12.9 | 12.9 (2026-10-23 again; 12.9, 12.9 on 2026-10-24); **42.4, 42.3 with the handheld quality profile** (2026-10-24) | GPU (3D room): the forward pass, three light passes a pixel at 2x MSAA, 65 of its 82 ms ([profiling.md](../profiling.md#six-more-gpu-bound-games-pass-by-pass-2026-10-23)); the profile ([quality.md](../quality.md)): one pixel light (the point lights per vertex), hard shadows, no MSAA, Standard's LOD-150 SubShader; a smaller window scaled by the display engine (`UA_RENDER_SIZE`, opt-in, softer picture, the same framing): 600x400 17.8, 540x360 21.4 against 12.9 (2026-10-24, [profiling.md](../profiling.md#rendering-smaller-the-display-engines-scaler-ua_render_size)) |
| Forsaken Castle Demo | 2018.2.7f1, 32-bit, InControl | in game | 58.9 (30.6 before defeed7) | vsync off and no frame rate: Android's 30 fps default until the desktop's pacing became the default for every Unity version |
| Liberal Crime Squad | 2019.1.15f1 macOS, 32-bit | title menu | 59.4 (30.2 before defeed7) | as Forsaken |
| Cell Machine | 2019.3.14f1 Windows (D3D11), URP, 32-bit | level 1 | 59.6 with `UA_GL_MEMO` (53.6 before; 29.0 - 29.7 before host flips) | GPU (696 MHz): the uber post pass 14 of its 24 ms; `UA_GL_MEMO` leaves out the colour grading LUT ([profiling.md](../profiling.md#the-colour-grading-lut-drawn-again-ua_gl_memo)). Before: Mali's fbdev driver waited for each flip's vblank on the thread that feeds the GPU, so 18.6 ms frames were shown every second refresh with the GPU clocked down (420 - 504 MHz); ua_host makes the flips now (docs/runtime.md, "The fbdev path's flips"); without vsync 53.4 |
| White Mirror | 2021.2.9f1, URP, FMOD (silent), 32-bit | in game | 53.9 with `UA_GL_MEMO` (43.1 before; title 58); 42.7 - 42.8 from a 48 C start (2026-10-24) | GPU: `UA_GL_MEMO` leaves out both cameras' LUT and circle-of-confusion passes; the bokeh blur, 11 of the 30 ms that remain; host flips: frames over 25 ms 197 -> 12 in 25 s (43.0 before). With the frame recorder's frame graph (`UA_GL_REC=2`, opt-in) 43.7 (+2.2 %: its two clear-only passes a frame made part of the passes after them; [profiling.md](../profiling.md#holding-the-whole-frame-ua_gl_rec)) |
| The Third Eye | 2022.2.15f1, URP 3D, 32-bit | first scene | 34.5 with `UA_GL_MEMO` (31.9 before; 29.6 before host flips); 31.7 on 2026-10-24 from a 48 C start; 34.2, 34.2 and **50.3, 50.8 with the handheld quality profile** (2026-10-24; its main thread the limit then) | GPU: SSAO of two cameras 20 of its 38 ms, fragment arithmetic ([profiling.md](../profiling.md#six-more-gpu-bound-games-pass-by-pass-2026-10-23)); the GPU idle 0.07 ms of a 31.5 ms frame, 84 framebuffer changes a frame (`UA_GPU_IDLE`, 2026-10-24) |
| Tacticsweeper | 2022.2.17f1 Windows (D3D11), 32-bit | level 1 board | 59.5 | - |
| Chapéu de Papel | 2022.3.62f3 macOS (Metal), 32-bit | title menu | 37.2 - 39.5 with `UA_GL_MEMO` (33.4 - 36.7 before, the title animates; 20.2 - 29.6 before host flips); 34.9 on 2026-10-24 from a 46 C start | GPU (696 MHz): its UI's textured full-screen layers, 23 of its 26 ms; the GPU idle 0.11 ms of a 28.6 ms frame (`UA_GPU_IDLE`, 2026-10-24). Before: as Cell Machine, the GPU at 336 or 600 MHz from run to run (20 or 30 fps); without vsync 36.3; `UA_RENDER_SIZE` (opt-in, softer, the menu at the same place): 600x400 40.3, 540x360 50.3 against 33.8 - 38.4 (2026-10-24, [profiling.md](../profiling.md#rendering-smaller-the-display-engines-scaler-ua_render_size)) |
| Chassis Chase | 2023.2.20f1, 32-bit | the race (in game) | 59.5 | - |
| DIG UP (test) | 6000.3.11f1, shaders from Vulkan, 32-bit | main menu | 59.5 | - |
| Phasing Gun | 6000.6.2f1, URP 3D, 32-bit | first scene | 11.8 (2026-10-23 again; 11.7 on 2026-10-24, GPU at 696 MHz; four more runs 2026-10-24 at 696 MHz all the time: 11.8 each, with or without its programs through a SPIR-V round trip: [shaders.md](../shaders.md#exact-rewrites-for-the-mali-g31s-compiler-looked-for-2026-10-23)); 11.8, 11.7 and **31.9, 31.9 with the handheld quality profile** (2026-10-24) | the profile ([quality.md](../quality.md)): SSAO downsampled with one Kawase blur, the shadow atlas 2048, soft shadows Low; the picture nearly the same (no pixel off by more than 11/255 in the first scene). GPU before: SSAO 54 of its 89 ms (arithmetic), the 4096x4096 shadow atlas 17 (memory writes), no post-processing (no LUT); its GPU counters (`GPUCNT=1`) give the arithmetic unit 68 % of the shader core's cycles; URP's SSAO, one full-screen draw, is 40 ms of the 89 ms frame, a 2048x2048 shadow map 17 ms ([profiling.md](../profiling.md#mesas-panfrost-instead-of-malis-driver-evaluated-2026-10-23-not-pursued)); `UA_RENDER_SIZE` (opt-in, softer, the same framing): 600x400 15.5, 540x360 18.0 against 11.6 (2026-10-24, [profiling.md](../profiling.md#rendering-smaller-the-display-engines-scaler-ua_render_size)) |
| Calculords | 2022.3.23f1 APK (Mono armv7) | first battle | 30.2 | the build's own Android pacing (30 fps) |
| Chapéu de Papel (APK) | 2022.3.62f3 APK (IL2CPP arm64) | first scene's dialogue | 33.3 - 36.0 | GPU (host flips 33.4, the driver's 32.9) |
| ROOTS | 2019.4.18f1 Mono APK through `pmunity apk --il2cpp` (arm64) | a run | 59.4 | - |
| Unsung Warriors - Prologue | 2017.4.40f1, 64-bit IL2CPP | main menu | 59.4 | - |
| Lo-Fi Room | 6000.0.58f2, 64-bit IL2CPP (mixed build) | title | 59.0 | - |

## Input

| game | Unity, input | emulated rig | notes (last checked) |
|---|---|---|---|
| [Forsaken Castle Demo](https://duckblockgames.itch.io/forsaken-castle-demo) | 2018.2.7f1, InControl | title, file select, new game, gameplay | needs `UA_PAD_NAME="Xbox Wireless Controller"` (2026-10-21) |
| [IBUKI 伊吹](https://markli.itch.io/ibuki) | 2020.2.4f1, Input System, compressed | the pad passes "Press Enter", Start Game loads the level | about 0.05 fps in the rig (2026-10-21) |
| [Butterflies Episode 1: Rudies](https://le-capitaine.itch.io/butterflies-episode-1) | 2020.3.25f1, Input System | the patched Input System creates the Gamepad; loading screen, then the main menu (adventure / multiplayer / tutorial / options / quit) over its realtime-GI-lit scene, about 500 frames in | the "now loading" stall was its main menu's realtime GI (Enlighten): the ARM player opens `.neo` files and the build has `.sse` ones; android-data now ships them under both names (`--realtime-gi neon`, the default), and the player's Enlighten loads them without an error. Not compared with the desktop picture; menus not driven (2026-10-22) |
| [Black Resin](https://pies-dariusz.itch.io/black-resin) | 2020.1.4f1, URP, Input System | title menu (animated, START GAME / CONTROLS / ABOUT US / QUIT) after about 40 frames | the black picture was the game's own fade-in: its camera fader starts black and clears after 2 s + 2 s of game time, and Unity advances game time by at most `Time.maximumDeltaTime` (1/3 s) a frame, so at the rig's 5 - 50 s per frame the menu shows after about 7 minutes (earlier runs ended before); nothing to fix (2026-10-21) |
| [Sort the Court!](https://graebor.itch.io/sort-the-court) | 5.3.2p3, old InControl | survey only; box64 BOX32: menu | (2026-10-21) |

## Native plugins and services

| game | Unity, plugins | emulated rig | notes (last checked) |
|---|---|---|---|
| [Nainai's Recipe](https://houkanshan.itch.io/nainais-recipe) | 2021.1.28f1, PPv2, FMOD, SQLite, Rewired | FMOD test double: init, banks, events; SQLite through the system's library; title screen (fade-in, Start / 中文 / Exit) with the game's own cursor (a white dot: it hides the system cursor and draws its own) | the black frames were the Post-processing Stack v2: its `Uber` and `FinalPass` shaders had no GLES3 programs (their `STEREO_INSTANCING_ENABLED` VR variants did not translate); now left out as VR-only. Its compute kernels (a swarm, rain and oil-bubble particles) now get GLES 3.1 kernels (shaders step) and run with `min_sdk` 22 (Unity uses OpenGL ES 3.1 only from API 21; at ua_host's 19 every `FindKernel` failed); all 1818 programs and 29 kernels link on Mesa. Not reached: the cooking scene where the swarm draws (2026-10-22) |
| Enter the Gungeon (GOG, Linux) | 2017.4.27f1, Mono .NET 3.5, Wwise 2017.2.5 (1,004 entry points), InControl, PlayMaker, CSteamworks | without the port's own assembly patch: on the built-in Wwise the loading screen ends (Init, `Init.bnk`, `SFX.bnk` with its callback), the intro text, the title over the Breach; no stack trace in 13 minutes (35 KB of log; before: the loading screen for good, 54,351 stack-trace lines in 6.5 MB); the command line and `Screen.resolutions` answered (no `ArgumentNullException`, no out-of-range exception in its options code). The background is magenta (its scene bundles' shaders are not converted yet) | sound through ua_host's Wwise player (below); the d-pad as its left stick: `UA_DPAD_STICK = "auto"`, which `port check` suggests from its InControl bindings (2026-11-02). Its banks for ua_host's own Wwise player: `pmu wwise` converts all 1,821 media (273 Vorbis, 1,548 IMA ADPCM) and resolves its 1,196 events in 3.3 s and 22 MB (arm64 container; `port check` suggests `wwise.banks`), docs/wwise-player.md; ua_host's own player (wwplay) plays them since 2026-11-02: in the rig the title's and the Breach's music and the game's sounds reach the OpenSL ES output (a 600 s rehearsal's capture: sound in 465 of 533 s); 3D attenuation, panning and Wwise's effects are not in yet |
| [Level UP - Mario's Minigames Mayhem](https://bookwormkevin.itch.io/lummm) | 2022.3.62f2, Discord SDK, Addressables | language screen; with the StreamingAssets fixes: title menu | (2026-10-21) |
| [SPELLSWORN](https://xk.itch.io/spellsworn) | 2022.1.10f1, SQLite, GameAnalytics | SQLite through the system's library | (2026-10-20) |
| [ADRORIUM](https://beholdernx.itch.io/adrorium) | 2018.1.0f1, Steam, Discord, Photon | main menu with its name prompt (New Game, Continue, Join Server, news) | crashed in a script's Awake before the Steam and Discord stubs; Photon's native plugins are missing (online play) and its server lookup fails without a network, as on a PC offline. At the rig's 300 s stop, a pause that timed out at 0.2 fps ended in glibc's "unaligned fastbin chunk" abort (shutdown only, not looked into) (2026-10-21) |
| [Jeżmin](https://codevisoft.itch.io/jezmin) | 2022.3.46f1, URP (HDRP's assembly shipped), VFX Graph, Alembic (`abci`) | not run (the USB drive's space); converted and packed: 211 shaders, 5 of 103 compute shaders (VFX Graph's need image formats or buffer types GLSL ES lacks) | Alembic's native `abci` exists for desktops only: android-data warns that its calls fail (DllNotFoundException) where the game plays an Alembic stream (2026-10-22) |

## StreamingAssets read as files

| game | Unity | emulated rig | notes (last checked) |
|---|---|---|---|
| [MineRalph](https://chopchopgames.itch.io/mineralph) | 2018.3.2f1 | title menu; on the 64-bit IL2CPP route too (below) | `streaming_assets = "both"`; with IL2CPP the patch is compiled in (`pmunity il2cpp --streaming-assets both`) (2026-10-22) |
| [Baldi's Basics Classic Remastered](https://basically-games.itch.io/baldis-basics-classic-remastered) | 2020.3.38f1, Rewired | launcher, then its title screen: the pad's Start (Rewired's "Pause") starts it; the left stick moves the launcher's cursor and A on Play starts it | its own cursor (docs/input.md, "Games with their own cursor"): its buttons are not uGUI's, so pointer mode's position and touches do not click them; no `UA_POINTER` (the pad drives its cursor), or `UA_POINTER=1 UA_POINTER_CURSOR=0`: the pointer's first motion now moves its cursor by as much as the pointer moved (it jumped to the launcher's top right corner before: Unity dropped the start position, docs/input.md), and a click on Play starts it (2026-10-22) |

## macOS builds

The `android` route translates a macOS build's OpenGL Core programs, which Unity's macOS builds
listed next to Metal up to 2020.1 and dropped from 2020.3 on: of the free itch.io macOS builds
whose settings were read (range requests, 2026-10-22), all 14 games of 2017.3 - 2020.1 have
OpenGL Core, 17 of the 18 of 2020.3 - 6000.6 (ten of them macOS-only games) Metal only. A
Metal-only build is translated from its Metal programs (`shaders.from = ["metal"]`, which
`port check` sets; docs/shaders.md, "macOS builds: Metal programs").

| game | Unity, build | emulated rig | notes (last checked) |
|---|---|---|---|
| [Liberal Crime Squad](https://the-cheshire-cat.itch.io/lcs) | 2019.1.15f1, Metal + OpenGL Core, .NET 3.5; reads its game data (XML in StreamingAssets) with System.IO | Unity splash, title menu (New Game / Continue / High Scores / Exit) | aborted at start before the fix ("Runtime critical type System.RuntimeType not found": 2019.1 still has the .NET 3.5 runtime, android-data took every 2019.x game as .NET 4.x); `streaming_assets = "both"` (the check's suggestion) gives its XML reads the folder beside the APK. Its Metal programs translated instead (`--from metal`, in memory): 41 shaders, 208 programs, every one of the 223 variants compared declares what the same pass's OpenGL Core program does; 194 / 194 link on Mesa (2026-10-22) |
| [Chapéu de Papel](https://carambo.itch.io/paper-hat) | 2022.3.62f3 (URP), Metal only, Mono .NET 4.x, Input System | title menu, its backgrounds cycling (2004, 1987, 1954) as in its own Android build's run, the right way up | the 32-bit route with `shaders.from = ["metal"]` (the check's suggestion, with asmpatch and `streaming_assets = "both"`): 92 of 93 shaders, 1250 programs translated from Metal (URP's HDR debug view left out), the full-screen passes' coordinates given GL's origin (869 stages); its APK takes the `apk` route (above) (2026-10-22) |
| [Rhythm Quest (Demo)](https://ddrkirbyisq.itch.io/rhythm-quest) | 6000.2.6f2, Metal only; Localization, Addressables, Input System, InControl, Steam, Discord | - | its Metal programs translated in memory (`--from metal`): 113 of 114 shaders, 320 programs, 209 / 209 distinct link on Mesa; its Addressables bundles carry no Unity version in their headers, which stopped the shaders step (now given the data's). Not run; its Linux build is IL2CPP (`box64` route, not run) (2026-10-22) |

## Mach-O route (macOS IL2CPP builds' arm64 code)

| game | Unity, build | result | notes (last checked) |
|---|---|---|---|
| Dave the Diver (Steam, macOS v1.0.6.768) | 6000.0.52f1, IL2CPP, URP, Addressables, Burst, Input System, Steam, EOS | not run yet | every binary universal; GameAssembly's arm64 slice: 342 imports (libSystem 236, libc++ 95, CoreFoundation 11), 530 instructions beyond ARMv8.0 (489 LSE, 13 `ldapr`, 6 `eor3`, 22 `crc32`), all within a branch of a block before the image (macho-route.md; 2026-11-08). Data (2026-11-09): 4.7 GB, one scene, everything else in 3,967 Addressables bundles (2.6.0, JSON catalog, 15 bundles with a CRC); `pmu convert` (Metal): 529 of 558 shaders, 30,791 programs, the 12,024 distinct ones link on Mesa (shaders.md); left out VFX Graph's outputs (structured buffers in vertex stages); compute: 109 of 121 compute shaders from their Metal kernels (224 kernels: VFX Graph's simulations, the fish and boid flocking kernels, Obi's solver; the flocking kernels checked against Metal on the Mac's GPU; left out: Obi's append-buffer and texture kernels, shaders.md); on the handheld's Mali: 6,893 programs were over its 1,024 uniform vectors (URP's 256-light arrays: Spine and Shader Graph sprites, URP Lit), now declared for URP's mobile 32 lights (exact on the Android player); kernels with work groups over the Mali's 128 invocations (the fish 256, the boids 1024) run in groups of 128 (pmu's work-group split, with ua_host answering Unity's group-size checks; 9 of 17, the other 8 share memory: VFX Graph's sort and SDF scan, Obi's reduce and scan; Obi's 15 solvers use Burst, so those 2 never run; checked with Unity's own player on the handheld, shaders.md); quality step (types from the IL2CPP metadata, quality.md): its 8 URP assets read, 2x MSAA off in 2, soft shadows Low in all; textures 5.4 GB RGBA32 + 1 GB BC7 (about 4.3 GB as RGBA on a GPU without BC): the textures step is a must on 1 GB handhelds |

## Checked only (no route through the Android players)

| game | Unity, build | what `port check` says (last checked) |
|---|---|---|
| [Suvalf](https://cheesykirah.itch.io/suvalf) | 6000.3.5f1, HDRP, compressed (`data.unity3d`) | Vulkan shaders only (as every HDRP Linux build); `box64` too needs OpenGL. Its shaders translate from Vulkan (`shaders.from = ["vulkan"]`): 116 of 142, 718 programs, all 553 distinct link on Mesa; converted that way and run in the emulated rig anyway, the player logs "graphics API OpenGLES3 is not supported with HDRP" and HDRP's `Render` throws a NullReferenceException every frame (the loop runs at 57 fps, the screen stays black): the check blocks the route for HDRP. Before: the check could not read a compressed build's settings and passed it, the converter translated nothing and android-data failed in video.py; both now stop with the reason (2026-10-22) |
| [RTS Siege Up!](https://abuksigun.itch.io/siegeup) | 6000.3.16f1, IL2CPP, compressed, Firebase C++ SDK (analytics, auth, messaging, remote config) | `box64` (GLES mode); the Firebase libraries are x86-64 like the game (not run) (2026-10-22) |
| [Space Dash](https://jasdv.itch.io/space-dash) | 5.4.2f2, .NET 3.5 | below the converter's 5.6: `box64` with gl4es (not run) (2026-10-22) |

## box64 route (x86-64 Linux players)

| game | Unity, build | arm64 rig | last checked |
|---|---|---|---|
| [Techium Eclipse](https://kenney.itch.io/techium-eclipse) | 5.5.1f1, Mono | menu | 2026-10-21 |
| [C::Reactris](https://floatwoozy.itch.io/c4r) | 2020.3.28f1, IL2CPP | menu | 2026-10-21 |
| [Polynya](https://bulboka.itch.io/polynya) | 2022.3.4f1, IL2CPP | menu | 2026-10-21 |
| [Tigan](https://kotzie.itch.io/tigan) | 6000.5.9f1, IL2CPP | sign-in screen | 2026-10-21 |
| [Cold Apples](https://zohmer.itch.io/cold-apples) | 6000.6.4f1, Mono | starts in game | 2026-10-21 |
| [2:22AM](https://umbrella-isle.itch.io/222am) | 5.0.0f4, 32-bit | aborts | 2026-10-21 |

The six Linux Mono games of the 32-bit table also reach their menus under box64. Tigan's and Cold
Apples' Vulkan programs translate too (`--from vulkan`, checked in memory, 2026-10-23): 82 of 92
shaders, 871 programs, and 83 of 93, 426 programs (Cold Apples', 6000.6, none before the converter
read 6000.6's parameter entries); all their distinct programs (811, 361) link on Mesa.

**Total: 61 games** (58 free games, Cuphead, Hollow Knight and Enter the Gungeon); 28 have run on the handheld.
