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
| [Shiver](https://aquiluslupus.itch.io/shiver) | 2017.2.0f3 | starts (Adventure Creator's text and PlayerPrefs, Steam's failed init handled as on a PC without Steam), but black, 64 frames in 300 s | the missing CSteamworks entry point is fixed; the black picture is not looked into yet (2026-10-21) |
| [Chassis Chase](https://frog626267.itch.io/chassis-chase) | 2023.2.20f1 | main menu | (2026-10-20) |
| [Lo-Fi Room](https://bearmask.itch.io/lofi-room) | 6000.0.58f2 | logo, title | (2026-10-21) |
| [DIG UP (test)](https://asestr.itch.io/dig-up-test) | 6000.3.11f1 | main menu | (2026-10-21) |
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
| [ROOTS](https://pierre-vandermaesen.itch.io/roots) | 2019.4.18f1, Mono armv7 | title | | stalls for minutes at times under QEMU (2026-10-20) |
| [Calculords](https://richjoslin.itch.io/calculords) | 2022.3.23f1, Mono armv7, GLES2 | title; as a port: the setup with a 32-bit userland (armeabi-v7a only, so the release ships only `ua_host` and port.json asks for armhf), then "Tap anywhere to start" in the armhf rig | | the check suggests `UA_POINTER=1` (UI pointer handlers, no pad navigation) (2026-10-22) |

## 64-bit IL2CPP route (desktop Mono games compiled with `pmunity il2cpp`)

| game | Unity | emulated rig (arm64) | handheld (64-bit userland) | last checked |
|---|---|---|---|---|
| Loco Motive (Game Jam) | 2018.4.36f1 | title menu; as a port made with `--route il2cpp-arm64`, its setup (the IL2CPP build on the device included) rehearsed in an arm64 container with 1 GB, then the title menu | title 59.4 - 59.6 fps | 2026-10-21 |
| buggish | 2019.4.8f1 | title | title 59.4 - 59.6 fps | 2026-10-20 |
| Tom City | 2020.3.49f1 | title | title 59.4 - 59.6 fps | 2026-10-20 |
| [Unsung Warriors - Prologue](https://unsungwarriors.itch.io/unsung-warriors-prologue) | 2017.4.40f1, .NET 3.5, Rewired | both intro videos, title menu (about 45 fps in the native arm64 rig); with `--asmpatch rewired-android` the pad skips the intro and opens the main menu. Sound: 2017.4's arm64 FMOD lost its mixer thread to glibc's larger minimum stack until the loader raised small stack sizes. The armv7 IL2CPP build reaches its title in the armhf rig, with sound | not yet | 2026-10-21 |

The IL2CPP build itself can also run on the handheld (`pmunity il2cpp --on-device`): Loco Motive
built in an arm64 container limited like the RG34XX-SP (1 GB, 4 CPUs, no swap), with the same
`libil2cpp.so` as the PC route; not yet on a handheld (2026-10-21).

## Input

| game | Unity, input | emulated rig | notes (last checked) |
|---|---|---|---|
| [Forsaken Castle Demo](https://duckblockgames.itch.io/forsaken-castle-demo) | 2018.2.7f1, InControl | title, file select, new game, gameplay | needs `UA_PAD_NAME="Xbox Wireless Controller"` (2026-10-21) |
| [IBUKI 伊吹](https://markli.itch.io/ibuki) | 2020.2.4f1, Input System, compressed | the pad passes "Press Enter", Start Game loads the level | about 0.05 fps in the rig (2026-10-21) |
| [Butterflies Episode 1: Rudies](https://le-capitaine.itch.io/butterflies-episode-1) | 2020.3.25f1, Input System | the patched Input System creates the Gamepad | stays on "now loading": its realtime GI data is SSE-only (2026-10-21) |
| [Black Resin](https://pies-dariusz.itch.io/black-resin) | 2020.1.4f1, Input System | runs, renders black | not looked into (2026-10-21) |
| [Sort the Court!](https://graebor.itch.io/sort-the-court) | 5.3.2p3, old InControl | survey only; box64 BOX32: menu | (2026-10-21) |

## Native plugins and services

| game | Unity, plugins | emulated rig | notes (last checked) |
|---|---|---|---|
| [Nainai's Recipe](https://houkanshan.itch.io/nainais-recipe) | 2021.1.28f1, FMOD, SQLite, Rewired | FMOD test double: init, banks, events; SQLite through the system's library | compute kernels missing: black frames (2026-10-20) |
| [Level UP - Mario's Minigames Mayhem](https://bookwormkevin.itch.io/lummm) | 2022.3.62f2, Discord SDK, Addressables | language screen; with the StreamingAssets fixes: title menu | (2026-10-21) |
| [SPELLSWORN](https://xk.itch.io/spellsworn) | 2022.1.10f1, SQLite, GameAnalytics | SQLite through the system's library | (2026-10-20) |
| [ADRORIUM](https://beholdernx.itch.io/adrorium) | 2018.1.0f1, Steam, Discord, Photon | main menu with its name prompt (New Game, Continue, Join Server, news) | crashed in a script's Awake before the Steam and Discord stubs; Photon's native plugins are missing (online play) and its server lookup fails without a network, as on a PC offline. At the rig's 300 s stop, a pause that timed out at 0.2 fps ended in glibc's "unaligned fastbin chunk" abort (shutdown only, not looked into) (2026-10-21) |

## StreamingAssets read as files

| game | Unity | emulated rig | notes (last checked) |
|---|---|---|---|
| [MineRalph](https://chopchopgames.itch.io/mineralph) | 2018.3.2f1 | title menu | `streaming_assets = "both"` (2026-10-21) |
| [Baldi's Basics Classic Remastered](https://basically-games.itch.io/baldis-basics-classic-remastered) | 2020.3.38f1, Rewired | launcher, then its title screen: the pad's Start (Rewired's "Pause") starts it | a pointer click or touch on the launcher's Play does not: Rewired's UI module receives the touch but no click lands on the button (not looked into further) (2026-10-21) |

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

**Total: 44 games** (42 free games, Cuphead and Hollow Knight); 11 have run on the handheld.
