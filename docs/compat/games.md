# Games tested

Every game the tools have been run with, and how far each got. Kept up to date as games are tested
or retested; the date says when a row was last checked. Free games from itch.io unless noted; the
games themselves, and anything made from them, are never in this repository.

**How far** means: *title* (the title screen or main menu draws and runs), *menus* (the pad or
pointer walks them), *in game* (a level or the first scene plays). The **emulated rig** is QEMU with
Mesa's software renderer (tools/emu: correct pictures and game logic, about 1 fps); **handheld** is
an Anbernic RG34XX-SP (Allwinner H700, Mali-G31, 1 GB RAM, Knulli, 720x480) unless noted.

Routes: **32-bit** = Unity's Android Mono player (armv7) on `ua_host`, the desktop data converted;
**64-bit IL2CPP** = the desktop game compiled with `pmunity il2cpp`, on the arm64 player; **APK** =
the game's own Android build (`pmunity apk`); **box64** = the game's x86-64 Linux player under box64
(tools/box64).

## Full ports

| game | Unity, build | route | emulated rig | handheld | notes (last checked) |
|---|---|---|---|---|---|
| Cuphead + The Delicious Last Course (GOG, macOS) | 2017.4.9f1, Mono .NET 3.5 | 32-bit | title, levels; the title screen at PortMaster's test sizes: 480x320 (3:2, the port's side crop), 640x480 and 720x720 (letterboxed), 1280x720 | **whole game**: setup from GOG's installers, title 59.6 fps, eight boss fights 59.0 - 59.5 fps | its own port (portmaster-cuphead). 64-bit (IL2CPP) build: Cuphead's code compiled with IL2CPP for 2017.4.40f1's arm64 player (7 min on the PC, `libil2cpp.so` 50.8 MB), the 2017.4.9 data retargeted to it; in the native arm64 rig: title screen with the DLC, menus with the handheld's button letters, a new game through the intro book into Elder Kettle's house (Dialoguer's dialogue; the saves, Dialoguer's XmlSerializer data included, written with the port's link.xml), sound starting. The port's own 64-bit setup (IL2CPP build on the device included) ran through on a 64-bit-only stand-in (arm64 container, 1 GB, no 32-bit loader) and its install reaches the main menu (2026-10-21) |
| Hollow Knight (GOG, Linux) | 6000.0.61f1, Mono .NET 4.x | 32-bit | first launch to gameplay: language, menus, calibration, both opening cinematics, King's Pass; the port's setup rehearsed in an arm64 container (`tools/port/rehearse.sh`, 1 GB memory + 2 GB swap: 32 minutes, 3.9 GB installed) and the game it installed starts at its first-run language screen | **first launch to gameplay**: title 37 fps, prologue 59.6, opening cinematic 30 (the video's rate), King's Pass 32 - 34 fps (its main thread is busy all the time); saving (Quit to Menu) and loading the save work; 30 minutes with steady memory (56 MB available at the lowest) | the game switches Unity's garbage collector off: without `UA_GC_BUDGET` (default 32 MB) Mono's heap grew from 64 to 192 MB in 28 minutes and memory ran out; with it, a collection every 4 - 5 minutes, a pause of about 0.6 s each; the desktop's frame pacing (`UA_FRAME_RATE`) lifted the 30 fps cap; the audio step keeps long clips compressed; the 64-bit IL2CPP route is not open to it yet (Unity 6's il2cpp is a native Windows/macOS/Linux x86-64 program); Switch controls via `UA_PAD_NAME` (2026-10-21) |

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
| [Hey, Listen!](https://andrecardoso.itch.io/hey-listen) | 2023.1.8f1, URP, Input System, DOTween | **aborts at start**, before the first frame: libunity logs "TuningForkUtils: Can't find realtime_features.json in APK", then "terminating with uncaught exception of type std::length_error: basic_string" | the first 2023.1 game (2023.2 runs); `realtime_features.json` sits among the GL/Vulkan feature names of 2023.1's libunity (with `predictRealtime`; not in 6000.0's): a device-performance check of that line that reads a file a desktop build does not have. Not fixed (2026-10-22) |
| [Chassis Chase](https://frog626267.itch.io/chassis-chase) | 2023.2.20f1 | main menu | (2026-10-20) |
| [Lo-Fi Room](https://bearmask.itch.io/lofi-room) | 6000.0.58f2 | logo, title | (2026-10-21) |
| [DEEP (Beta)](https://fog-icmc.itch.io/deep) | 6000.2.6f2, URP, FMOD + Resonance, Input System, Unity Services | **crashes at start** (Mono SIGSEGV in `mono_class_from_name`, before the first frame) | the first 6000.2 game. android-data first failed in its check of globalgamemanagers (UnityConnectSettings: UnityPy's tree for 6000.2.6 has a field the data lacks; fixed). The build ships no `netstandard.dll` (and 21 more of the assemblies its ScriptingAssemblies.json lists); the Android player's own UnityEngine.CoreModule references netstandard 2.1.0.0, so its types do not load ("Could not load file or assembly 'netstandard, Version=2.1.0.0'", then 306 "Unable to find type [UnityEngine.CoreModule.dll]...") and Mono crashes. android-data now warns about it. With another Unity 6 build's netstandard.dll added by hand (an experiment) those types load, and the next stop is "TypeLoadException: VTable setup of type UnityEngine.Rendering.ShaderStrippingSetting failed" (URP's core package, whose build lacks Unity.RenderPipelines.Core.ShaderLibrary and Unity.Rendering.LightTransport.Runtime among the missing ones), "globalgamemanagers is corrupted", SIGSEGV: an incomplete build, not fixed (2026-10-22) |
| [DIG UP (test)](https://asestr.itch.io/dig-up-test) | 6000.3.11f1 | main menu | (2026-10-21) |
| [Flowerist's Horror](https://trazam-dev.itch.io/flowerists-horror) | 6000.4.6f1, URP (HDRP's assembly shipped, not used), PlayMaker, Buto fog, FBX SDK, Input System | title menu (PLAY / CREDITS / SETTINGS / QUIT) | the first 6000.4 game; 115 shaders and 9 of 11 compute shaders translated (2026-10-22) |
| [Ashley's Wacky Abandoned Building](https://davadoo.itch.io/ashleys-wacky-abandoned-building) | 6000.5.7f1 | title menu | (2026-10-21) |
| [Phasing Gun](https://atalachy.itch.io/phasing-gun) | 6000.6.2f1 | first scene (in game) | (2026-10-21) |
| [IronQuiet](https://aeroforger.itch.io/ironquiet) | 6000.6.3f1 | title menu over its 3D scene | about a minute per frame in the rig (2026-10-21) |

## Windows-only builds (D3D11 shaders translated)

| game | Unity, build | emulated rig | shaders converted (last checked) |
|---|---|---|---|
| [Red Handed](https://badpiggy.itch.io/red-handed) | 2018.3.7f1, 3D | its first level | 33 of 34 (2026-10-20) |
| [Out of Ctrl](https://miknugget.itch.io/out-of-ctrl) | 2019.2.5f1 | title | 42 of 44 (2026-10-20) |
| [Cell Machine](https://samhogan.itch.io/cell-machine) | 2019.3.14f1, URP | splash, title menu | 60 of 62 (2026-10-20) |
| [Little Lars Diary](https://eufrasi0.itch.io/little-lars) | 2020.3.44f1 | splash, click-to-start | 50 of 52 (2026-10-20) |
| [10 Second Mixtape (Jam Version)](https://scottts.itch.io/10-second-mixtape) | 2021.3.28f1, compressed | title menu | 49 of 51; needs `pmunity.winbcl` (2026-10-20) |
| [Memory Remains](https://pokeydem.itch.io/memory-remains) | 2022.3.62f3, URP, Burst | title menu | 82 of 85 (2026-10-20) |
| [Tacticsweeper](https://moocowsgomoo.itch.io/tacticsweeper) | 2022.2.17f1 | title menu (New Game / Settings / Exit Game) | 45 of 47; `pmunity.winbcl`: 22 class libraries; the first 2022.2 Windows build (2026-10-22) |
| [Die to Play](https://mrkvinter.itch.io/die-to-play) | 6000.1.4f1, URP 2D, Localization, Addressables (binary catalog), SQLite, R3, VContainer, UniTask | no exceptions left; its first screen is a full-screen green picture (an untranslated shader's?, not compared with the desktop), 0.1 - 0.3 fps | 135 of 143 (URP 2D's sprite shaders only after leaving out their GPU sprite skinning variants; 133 before, every sprite with the error shader). Three converter faults on the way, all fixed: winbcl replaced the game's NuGet System.ComponentModel.Annotations 5.0 with Unity's 4.2 facade (ReflectionTypeLoadException), the Addressables catalog's Windows paths ("Invalid path in AssetBundleProvider: …aa\\StandaloneWindows64\\…", so "SelectedLocale is null"), and its binary catalog's bundle CRCs (2026-10-22) |
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
| Loco Motive (Game Jam) | 2018.4.36f1 | title menu; as a port made with `--route il2cpp-arm64`, its setup (the IL2CPP build on the device included) rehearsed in an arm64 container with 1 GB, then the title menu | title 59.4 - 59.6 fps | 2026-10-21 |
| buggish | 2019.4.8f1 | title | title 59.4 - 59.6 fps | 2026-10-20 |
| Tom City | 2020.3.49f1 | title | title 59.4 - 59.6 fps | 2026-10-20 |
| [Untitled Hook Game](https://bonionline.itch.io/untitled-hook-game) | 2021.3.16f1, URP | the first-launch name prompt, then (a name in PlayerPrefs) the main menu, about 30 fps in the native arm64 rig; the first 2021.x build of the route: il2cpp's Bee build graph run by pmunity (605 commands, `libil2cpp.so` 53.2 MiB, metadata 29). As a port made with `--route il2cpp-arm64`: its setup rehearsed on a 64-bit-only stand-in (`REHEARSE_64ONLY=1`, arm64 container, 1 GB + 1 GB swap) in 32 min (the IL2CPP conversion 10.6 min with a 987 MB peak, the compile 9 min with 3 jobs, 599 MB peak), installed 230 MB; the installed game reaches the name prompt and the main menu in the arm64 rig | not yet | 2026-10-22 |
| [Tiny Kingdom](https://neltile.itch.io/tiny-kingdom) | 2022.3.36f1, URP, FMOD, Steam, Burst | title menu at 60 fps in the native arm64 rig; the first 2022.x build of the route (NDK r23b, 780 Bee commands, `libil2cpp.so` 82.0 MiB, metadata 31); silent as on the Mono route until FMOD's ARM libraries are supplied, the Steam stub answers | not yet | 2026-10-22 |
| [MineRalph](https://chopchopgames.itch.io/mineralph) | 2018.3.2f1, localization XML in StreamingAssets, Steamworks.NET | title menu with its localized text, about 20 fps in the native arm64 rig: the first build of the route that reads StreamingAssets with System.IO (asmpatch's streaming-assets patch compiled in, the files beside the APK) and the first 2018.3 one (`libil2cpp.so` 34.5 MiB, metadata 24; 2018.3's il2cpp needed `--map-file-parser` as 2017.4, and its Plugins copies of I18N.dll are now kept as the game's). Steamworks.NET's callbacks throw under IL2CPP (delegates to generic methods), after the Steam stub's failed init. As a port made with `--route il2cpp-arm64` (the check's `streaming_assets = "both"`): its setup rehearsed on a 64-bit-only stand-in (`REHEARSE_64ONLY=1`, 1 GB + 1 GB swap) in 33 min (the tools' downloads; UnityLinker 0.2 min; the conversion on .NET 8 4.2 min, 626 MB peak; the compile 14.7 min, 692 MB peak), installed 381 MB; the installed game reaches the same title menu in the arm64 rig (`EMU_ONLY=1 EMU_ARCH=arm64`) | not yet | 2026-10-22 |
| [Unsung Warriors - Prologue](https://unsungwarriors.itch.io/unsung-warriors-prologue) | 2017.4.40f1, .NET 3.5, Rewired | both intro videos, title menu (about 45 fps in the native arm64 rig); with `--asmpatch rewired-android` the pad skips the intro and opens the main menu. Sound: 2017.4's arm64 FMOD lost its mixer thread to glibc's larger minimum stack until the loader raised small stack sizes. The armv7 IL2CPP build reaches its title in the armhf rig, with sound | not yet | 2026-10-21 |

The IL2CPP build itself can also run on the handheld (`pmunity il2cpp --on-device`): Loco Motive
built in an arm64 container limited like the RG34XX-SP (1 GB, 4 CPUs, no swap), with the same
`libil2cpp.so` as the PC route (2026-10-21), and Untitled Hook Game (2021.3, Bee) in its port's
setup on a 1 GB + 1 GB swap stand-in (2026-10-22); not yet on a handheld.

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
OpenGL Core, 17 of the 18 of 2020.3 - 6000.6 (ten of them macOS-only games) Metal only. Such a
build needs the game's Linux or Windows build; `port check` and the converter say so.

| game | Unity, build | emulated rig | notes (last checked) |
|---|---|---|---|
| [Liberal Crime Squad](https://the-cheshire-cat.itch.io/lcs) | 2019.1.15f1, Metal + OpenGL Core, .NET 3.5; reads its game data (XML in StreamingAssets) with System.IO | Unity splash, title menu (New Game / Continue / High Scores / Exit) | aborted at start before the fix ("Runtime critical type System.RuntimeType not found": 2019.1 still has the .NET 3.5 runtime, android-data took every 2019.x game as .NET 4.x); `streaming_assets = "both"` (the check's suggestion) gives its XML reads the folder beside the APK (2026-10-22) |
| [Chapéu de Papel](https://carambo.itch.io/paper-hat) | 2022.3.62f3, Metal only | - | no programs to translate; its APK takes the `apk` route (above) (2026-10-22) |
| [Rhythm Quest (Demo)](https://ddrkirbyisq.itch.io/rhythm-quest) | 6000.2.6f2, Metal only; Localization, Addressables, Input System, InControl, Steam, Discord | - | no programs to translate; its Linux build is IL2CPP (`box64` route, not run) (2026-10-22) |

## Checked only (no route through the Android players)

| game | Unity, build | what `port check` says (last checked) |
|---|---|---|
| [Suvalf](https://cheesykirah.itch.io/suvalf) | 6000.3.5f1, HDRP, compressed (`data.unity3d`) | Vulkan shaders only (as every HDRP Linux build): no route; `box64` too needs OpenGL. Before: the check could not read a compressed build's settings and passed it, the converter translated nothing and android-data failed in video.py; both now stop with the reason (2026-10-22) |
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

The six Linux Mono games of the 32-bit table also reach their menus under box64.

**Total: 60 games** (58 free games, Cuphead and Hollow Knight); 11 have run on the handheld.
