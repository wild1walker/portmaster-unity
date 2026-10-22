# portmaster-unity

Tools for porting **Unity games** to ARM Linux handhelds that run
[PortMaster](https://portmaster.games/).

Unity makes an Android version of its game player. portmaster-unity runs that player directly on
the handheld's Linux: no Android, no emulator, no x86 translation, so games run at close to full
speed. A converter turns a desktop game (its Linux, macOS or Windows version) into the files that
player reads, on the handheld itself, the first time the game starts.

- **Porters** use it to make PortMaster ports of Unity games: see [For porters](#for-porters).
- **Players** do not need this repository: they install a port made with it, such as
  [Cuphead](https://github.com/wild1walker/portmaster-cuphead). See [For players](#for-players).

**No files from Unity or from any game are in this repository, and none may ever be added.**
Players bring their own copy of the game; Unity's Android player is fetched from Unity on the
player's own handheld or computer. The tools convert them there.

The first finished port is Cuphead (Unity 2017.4, Mono) on an Anbernic RG34XX-SP with Knulli: most
of the game runs at 55 to 60 frames per second, against roughly half that with the earlier x86
route (box64).

## Contents

- [For players](#for-players)
- [For porters](#for-porters)
  - [What it can port today](#what-it-can-port-today)
  - [What is missing](#what-is-missing)
  - [Port a game in five steps](#port-a-game-in-five-steps)
  - [Documentation](#documentation)
- [Words used here](#words-used-here)
- [Licence and credits](#licence-and-credits)

## For players

### What you need

| | |
|---|---|
| **A handheld with PortMaster** | Most ports made with these tools need a 32-bit (armhf) userland, the 32-bit ARM libraries with OpenGL ES 3, next to the usual 64-bit system: Knulli on Allwinner H700 handhelds has both. The port's own page says what it needs. |
| **Tested on** | Anbernic RG34XX-SP (Allwinner H700, Mali-G31, 1 GB RAM) with Knulli. |
| **Your own copy of the game** | The version the port asks for, usually the Linux or macOS version. |
| **Free space** | About three times the game's size while the first-launch setup runs. |
| **Wi-Fi** | On the first launch: the setup fetches Unity's Android player for the game's Unity version from Unity (about 10 MB). |

### Install a port

PortMaster keeps its games in a folder called `ports` (on Knulli: `roms/ports` on the card with
your games). For a port named `mygame`:

1. Download the port's zip from where its porter publishes it, and unzip it into `ports/`. You get
   `ports/<Title>.sh` and the folder `ports/mygame/`.
2. Copy your game into `ports/mygame/gamedata/`: its folder, or the archive it came in (`.zip`,
   `.7z`, `.rar`, `.tar.*`).
3. Start the game from the Ports menu. If it does not show up, refresh your game list or restart
   the handheld.
4. **The first launch runs the setup.** PortMaster shows its progress, and may ask questions first
   (for example a texture quality). The setup fetches Unity's player, converts the game for the
   handheld and packs it. If it is interrupted (battery, power button), start the game again: it
   continues where it stopped. Its log is `ports/mygame/patchlog.txt`.
5. When the setup is done, the game starts. The game's folder in `gamedata/` is used up by the
   setup (its files move into the converted game); archives you put there can be deleted.

Saves and settings are in `ports/mygame/data/`. The log of the last run is `ports/mygame/log.txt`.

### Controls

- **The handheld's buttons** reach the game as an Android gamepad with the usual layout of an Xbox
  pad (buttons by position: the bottom face button is the Xbox pad's A).
- **PortMaster's exit hotkey** (Select + Start) quits.
- **Games with mouse-driven menus:** a port can turn on *pointer mode*. The stick or d-pad moves a
  cursor; by default A is the left mouse button and B the right one.
- **Text fields** (a name, a save slot): an on-screen keyboard opens. D-pad moves, A types a key,
  B deletes, X is space, Y is shift, L1/R1 move the cursor, Start is done, Select cancels.
- A port's own options are in `ports/mygame/port.env`.

### Known issues

- **The first launch is long**: the game is converted on the handheld. Ports that convert textures
  take the longest.
- **Only Cuphead has been played level by level on a handheld.** The other games tried so far
  were tested up to their title or first screen ([what has been tested](#what-it-can-port-today);
  every game by name: [docs/compat/games.md](docs/compat/games.md)).
- **1 GB of RAM is tight** for games with many large textures; ports of such games convert their
  textures (`pmunity port check` tells the porter when).
- **Online services** (Steam, Discord, Epic) answer as if they were not running. Games whose sound
  uses FMOD or Wwise are silent unless the porter supplies ARM versions of those libraries.
- **Copying big files to the card while you play** makes games stutter.

### Getting help and reporting a bug

- A problem with **one game's port**: report it on that port's page first (Cuphead:
  [portmaster-cuphead issues](https://github.com/wild1walker/portmaster-cuphead/issues)).
- A problem with **these tools**, or a game you ported or tried to port: open an
  [issue here](https://github.com/wild1walker/portmaster-unity/issues/new/choose) and pick a form:
  **Bug report**, **Game porting report** or **Feature request**.
- Attach `ports/<name>/patchlog.txt` (the setup) and `ports/<name>/log.txt` (the game).
  **Never attach game files or Unity's files.**

## For porters

### What it can port today

Each column is one kind of game build and the way it runs on the handheld (a *route*):

| Column | The build you have | How it runs on the handheld | Made with |
|---|---|---|---|
| **A** | Mono build for Linux or macOS | Unity's 32-bit ARM Android player, on the handheld's 32-bit userland; the game's data is converted on the handheld | `pmunity port new` (route `android`) |
| **B** | Mono build for Windows only | the same; its Direct3D 11 shaders are translated to OpenGL ES | route `android` with `shaders.from = ["d3d11"]` (build `tools/hlslcc/build.sh` first) |
| **C** | Mono build (tested with Linux builds) | Unity's 64-bit ARM Android player, for handhelds without 32-bit libraries; the game's code is compiled with IL2CPP on the porter's computer, or by the port's setup on the handheld (`--on-device`) | `pmunity il2cpp`, by hand ([docs/il2cpp.md](docs/il2cpp.md)) |
| **D** | The game's own Android build (APK, XAPK, AAB) | as it is, on the 32-bit or 64-bit runtime; nothing converted | `pmunity apk`, by hand ([docs/apk.md](docs/apk.md)) |
| **E** | Linux x86-64 build, Mono or IL2CPP | the game's own Linux player, translated by box64; about half the frame rate of A | `pmunity port new --route box64` ([docs/box64-route.md](docs/box64-route.md)) |

What has run, by Unity version (the version numbers in the cells are the builds that were tried):

| Unity | A: Mono, Linux/macOS, 32-bit | B: Mono, Windows only, 32-bit | C: Mono to 64-bit (IL2CPP) | D: Android build (APK) | E: Linux x86-64 under box64 |
|---|---|---|---|---|---|
| 5.0 - 5.5 | Not yet (5.0 - 5.4 store shaders differently; 5.5 not checked) | Not yet | No (Unity made no 64-bit ARM player) | Not yet | Emulator: 5.5.1 menu. 32-bit-only builds: 5.3.2 menu with box64's experimental 32-bit mode; 5.0.0 crashes |
| 5.6 - 2017.2 | Emulator: 5.6.0 main menu; 2017.2.0 stops at an old Steamworks plugin | Not yet (the Direct3D path covers 2017.3 - 2022.3) | No (Unity made no 64-bit ARM player) | Not yet | Not yet |
| 2017.3 - 2017.4 | **Handheld**: Cuphead (2017.4.9), every level, 55 - 60 fps in most; 2017.4.40: title, 59.6 fps, intro video plays | Not yet (only Unity's built-in shaders checked) | Emulator: 2017.4.40 (.NET 3.5) intro videos and title menu, no sound yet | Not yet | Emulator: 2017.4.40 title menu |
| 2018 | **Handheld**: 2018.4 title, 59.6 fps | Emulator: 2018.3.7 starts its first level | **Handheld**: 2018.4 title, 59.4 - 59.6 fps | Not yet | Emulator: 2018.4.36 title menu |
| 2019 | **Handheld**: 2019.4 title, 59.6 fps | Emulator: 2019.2.5 title; 2019.3.14 (URP) title menu | **Handheld**: 2019.4 title, 59.4 - 59.6 fps | Emulator: 2019.4.18 Mono, 32-bit: title | Emulator: 2019.4.8 title |
| 2020 | **Handheld**: 2020.3 title, 59.6 fps | Emulator: 2020.3.44 first screen | **Handheld**: 2020.3 title, 59.4 - 59.6 fps | **Handheld**: 2020.3 IL2CPP, 64-bit: title | Emulator: 2020.3.49 Mono title menu; 2020.3.28 IL2CPP main menu (desktop GL only) |
| 2021 | **Handheld**: 2021.3 (URP) title, 59.6 fps | Emulator: 2021.3.28 title menu (with the class library swap, `pmunity.winbcl`) | Not yet | **Handheld**: 2021.3 IL2CPP, 64-bit: title (its 32-bit libraries: emulator) | Emulator: 2021.3.16 (URP) main menu |
| 2022 | **Handheld**: 2022.3 (URP) title, 59.6 fps (with the desktop's frame pacing: Unity's 2021.1+ Android players default to 30) | Emulator: 2022.3.62 (URP) title menu | Not yet | **Handheld**: 2022.3 IL2CPP, 64-bit: title. 2022.3.23 Mono, 32-bit: emulator | Emulator: 2022.3.36 (URP) title menu; 2022.3.4 IL2CPP opening scene |
| 2023.1 - 2023.2 | Emulator: 2023.2.20 main menu (2023.1 not tried) | Not yet | Not yet | Not yet | Not yet |
| 6000.0 - 6000.6 (Unity 6) | **Handheld**: Hollow Knight (6000.0.61) from first launch to gameplay (King's Pass, 32 - 34 fps; the 1 GB is tight). Emulator: Hollow Knight from first launch to gameplay; 6000.0.58, 6000.3.11, 6000.5.7 and 6000.6.2 reach their menus or first scenes; 6000.6.3 gets as far as generating its world | Not yet | Not yet | Not yet | Emulator, desktop GL only: 6000.5.9 IL2CPP sign-in screen, 6000.6.4 Mono in game. GLES mode, which most handhelds need: not yet |
| 6000.7 and later | Not yet (betas are refused) | Not yet | Not yet | Not yet | Not yet |

| Mark | Meaning |
|---|---|
| **Handheld** | Ran on a real handheld: an Anbernic RG34XX-SP (Allwinner H700, Mali-G31, 1 GB RAM) with Knulli. |
| Emulator | Ran only in the test rig on a computer: ARM emulated with QEMU, or native ARM64 in Docker, with software graphics (Mesa). Frame rates there say nothing about a handheld. |
| Not yet | Not tried yet, or the tools do not handle it yet. |
| No | This route cannot take this kind of build. |

Notes:

- **"Title" means the game started, reached its title or first screen and drew it correctly.**
  Only Cuphead has been played level by level on a handheld. On the handheld, the column D builds ran at
  59.6 fps, or 30 fps where the game keeps Unity's Android default frame rate.
- A **Windows-only or macOS-only IL2CPP game** has no route: its code is compiled for that desktop
  system and its processor. It can only be ported from its Linux build (E) or its Android build (D).
- Games with a lot of **BC/DXT textures** need the textures step on a 1 GB handheld: the GPU cannot
  read them, so Unity unpacks them in memory. On the handheld, one game's 57 MB of such textures
  grew to 830 MB and froze it; converted to ASTC it ran at 60 fps in 385 MB. `port check` warns
  above 128 MB.
- Every game tested, by name, and how far it got: [docs/compat/games.md](docs/compat/games.md).
- The full record, per version and per feature: [docs/compat/README.md](docs/compat/README.md).

**What that covers.** In a survey of 65 free Linux Unity games (classified by their Unity version
and build type), 50 were Mono builds of Unity 2017.3 - 2022.3, the range the tools supported at the
time (it is 5.6 - 6000.6 now): column A, which has run on the handheld with a game of every year
from 2017 to 2022. 8 were IL2CPP builds (column E, emulator only so far), 5 used a Unity
version outside that range, and for 2 the survey could not tell the build type.

### What is missing

Most impact for the most games first:

1. **Playing games on a handheld.** Apart from Cuphead, games have only been started to their
   title screens on a handheld. Controls, sound, saves, memory over a long session and frame rates
   in play are still to be checked, game by game. One 2022.3 game shows its title at 30 fps, and
   why is not known yet.
2. **Handhelds without 32-bit libraries** (ROCKNIX and other 64-bit-only firmwares). Column C runs
   2018.4 - 2020.3 games on a handheld, but only as a porter's manual build: IL2CPP has to run on a
   computer, not on the handheld, `port new` has no template for it, and `port check` does not
   offer it yet. Unity 2021 - 2022, sound with 2017.4's 64-bit player, 2017.4 .NET 4.x games,
   native plugins, Burst and the Input System package are not done or not tested on this route
   (2017.4 .NET 3.5 games reach their titles in the emulator).
3. **Windows-only games on a handheld.** Six reached their first screens in the emulator; none has
   run on a handheld yet. No Windows game of 2017.3 - 2017.4 has been tried (only Unity's own
   shaders of 2017.4), Windows builds of 2023 and Unity 6 are not covered yet, and games with BC7,
   BC4 or BC5 textures have not been checked.
4. **The box64 route on a handheld** (IL2CPP desktop games, versions or x86-only plugins the other
   routes cannot take). It works in the emulator but has not run on a handheld: its GLES mode
   through PortMaster's Westonpack, sound and pads are untested there. GLES mode has not run with
   Unity 6 games yet, 32-bit x86 games need box64's experimental 32-bit mode (not in the launcher
   yet), and precompiling a game's code (Mono AOT) against first-use hitches is not part of the
   tools.
5. **Ports from Android builds.** `pmunity apk` works by hand, but `port check` and `port new` do
   not make ports from APKs yet. No 2017.4 - 2019.4 IL2CPP 64-bit build has been tried. The games'
   own Java parts (Google Play services, ads, billing) do not run; most games carry on without
   them. Touch-only games need pointer mode.
6. **Video.** Intro videos (Unity's VideoPlayer) have played in a 2017.4.40 game. The same code
   path for 2018.4 - 2021.3 has not run with a video yet, 2022.3's path is only unit-tested, a
   Unity 6 game's background video stalls, and the Vulkan path is missing.
7. **Native plugins.** FMOD and Wwise need ARM libraries of the game's exact version, supplied by
   the porter (FMOD needs an account to download). Other x86-only plugins fail; Burst code falls
   back to its slower managed version. An old Steamworks.NET (through CSteamworks) calls entry
   points the Steam stub does not have (a 2017.2 game stops there).
8. **Files the game reads itself.** StreamingAssets as loose files (`loose`) and `data_path` are
   only unit-tested; binary Addressables catalogs are not handled, nor moving saves (PlayerPrefs)
   from the Windows or macOS version.
9. **Versions and platforms not covered:** Unity 5.0 - 5.5 and 6000.7 on the Android player,
   Vulkan, Unity's GameActivity variant (not needed so far: every player package also has the
   usual Activity one), and kernels with 16 KiB pages.
10. **A stall seen only in the emulator:** a 2019.4 Mono Android build sometimes stops drawing
    under QEMU (Unity's job workers miss a wakeup). Only a handheld run can tell whether it
    happens on real hardware.

The plan behind these: [docs/roadmap.md](docs/roadmap.md). The detailed gap list:
[docs/compat/README.md](docs/compat/README.md#gaps-most-impact-for-the-most-games-first).

### Port a game in five steps

You need a computer with Docker and Python 3.10 or newer (macOS or Linux; the commands below are
for a shell; write `python3` where `python` is missing), and the game you own.

Once:

```sh
git clone --recurse-submodules https://github.com/wild1walker/portmaster-unity
cd portmaster-unity
pip install -r convert/requirements.txt
runtime/build.sh                # builds ua_host in Docker (port package also does it when needed)
cd convert
```

1. **Check the game.** `port check` says what the game is (Unity version, Mono or IL2CPP, Linux,
   macOS or Windows, shaders, input, video, plugins, textures) and which route fits. Its exit
   status is 0 when a route fits.
   ```sh
   python -m pmunity version "<Game>_Data"          # Unity version, supported or not
   python -m pmunity port check "<game folder>"
   ```
2. **Make the port.** `port new` writes `mygame/` with a short settings file, `port.toml` (title,
   pointer mode, keys, setup questions, ...). Edit it, then run `port new` again to regenerate the
   rest.
   ```sh
   python -m pmunity port new mygame --data "<game folder>"
   ```
3. **Package it.** This makes the release zip, `mygame/out/mygame.zip`: the launcher, the setup,
   `ua_host`, the converter, its Python and the licences. No game files, nothing of Unity.
   ```sh
   python -m pmunity port package mygame              # or: mygame/package.sh
   ```
4. **Test it.** First the rehearsal: it runs the port's first launch in a container that looks
   like a handheld, then the game in the emulator, and saves screenshots to `work/mygame/shots.png`
   (`PM_7ZZ` is PortMaster's own `7zzs.aarch64`, from its release). Then on a handheld: unzip the
   release into `ports/`, copy the game into `ports/mygame/gamedata/`, start it, and check the
   setup screen, the pad, sound, saves, the exit hotkey and the frame rate (`UA_FPS=5` writes it
   to `log.txt`). [docs/porting.md](docs/porting.md) has the full checklist.
   ```sh
   PM_7ZZ=<path to 7zzs.aarch64> ../tools/port/rehearse.sh mygame/out/mygame.zip "<game folder>" work/mygame 300
   ```
5. **Tell us how it went.** Open a
   [Game porting report](https://github.com/wild1walker/portmaster-unity/issues/new/choose) with the
   output of `pmunity version` and `port check`, what worked and where it failed, your handheld and
   firmware, and `patchlog.txt` and `log.txt`. Remove personal folder names from the output if
   you like, and never attach game files. Reports of games that work are as useful as failures.

`port new` makes ports for the `android` (A, B) and `box64` (E) routes. Columns C and D are done
by hand for now ([docs/il2cpp.md](docs/il2cpp.md), [docs/apk.md](docs/apk.md)). Publish only the
port's zip: never the game, a converted game, or Unity's files.

### Documentation

| Page | What is in it |
|---|---|
| [docs/porting.md](docs/porting.md) | the porter's guide: `port check`, `port.toml`, the setup, packaging, the rehearsal, testing on a device, doing it by hand, typical first problems |
| [docs/compat/README.md](docs/compat/README.md) | what each Unity version needs and what has run, on the handheld and in the emulator |
| [docs/compat/games.md](docs/compat/games.md) | every game tested, by name, and how far each got |
| [docs/roadmap.md](docs/roadmap.md) | where this is going |
| [docs/input.md](docs/input.md) | pads, Rewired, InControl, the Input System, pointer mode, the on-screen keyboard |
| [docs/plugins.md](docs/plugins.md) | native plugins, FMOD, Steam and other services, HTTPS |
| [docs/il2cpp.md](docs/il2cpp.md), [docs/apk.md](docs/apk.md), [docs/box64-route.md](docs/box64-route.md) | the 64-bit IL2CPP route, Android builds, the box64 route |
| [docs/README.md](docs/README.md) | every other page (runtime switches, shaders, data format, design) and the repository layout |
| [docs/building.md](docs/building.md) | what building needs, and converting a game by hand |
| [CONTRIBUTING.md](CONTRIBUTING.md) | running the tests, and the rules for changes |

## Words used here

| Word | Meaning |
|---|---|
| **Mono** / **IL2CPP** | Unity's two ways of running a game's code. A Mono build ships the code as .NET assemblies (`Managed/*.dll`), which Unity's ARM player can run. An IL2CPP build ships it compiled for one processor (`GameAssembly`), so its desktop version cannot run on ARM. |
| **Android player** | Unity's engine as built for Android phones (`libunity.so`). It is the same engine as on the desktop, compiled for ARM. |
| **ua_host** | this project's runtime: it loads the Android player on the handheld's Linux and stands in for the parts of Android the player uses. |
| **32-bit userland** (armhf) | 32-bit ARM system libraries next to the usual 64-bit ones. Unity's Mono Android players are 32-bit, so routes A and B need them. |
| **APK, OBB** | the files of an Android app: the package and its extra data. The converter writes both for the player. |
| **box64** | a program that runs x86-64 Linux programs on ARM by translating them while they run. |
| **GLES** | OpenGL ES, the graphics interface of handheld GPUs. Desktop shaders are converted to it. |
| **Emulator** (test rig) | `tools/emu` and `tools/box64/rig.sh`: the handheld's software running on a computer, for testing without a device. |

## Licence and credits

MIT (see [LICENSE](LICENSE)) for everything in this repository except the third-party parts listed
in [THIRD_PARTY.md](THIRD_PARTY.md): libhybris (a git submodule, Apache 2.0) and AOSP's `jni.h`
(Apache 2.0). `ua_host` also links the video decoders of Debian's packages (libvpx, libvorbis,
libogg, libopus: BSD-3-Clause; OpenH264: BSD-2-Clause), whose licence texts go with it.

Unity is a trademark of Unity Technologies. This project is not affiliated with or endorsed by
Unity Technologies or by any game's publisher.

Credits: the loader's ideas come from the PS Vita so_loader ports, FalsoJNI, PortMaster's gmloader
and [libhybris](https://github.com/libhybris/libhybris), whose bionic hooks it compiles in (all
code here is this project's own). The converter reads and writes Unity files with
[UnityPy](https://github.com/K0lb3/UnityPy), encodes textures with ARM's astc-encoder and
translates Direct3D shaders with Unity's open-source
[HLSLcc](https://github.com/Unity-Technologies/HLSLcc). The box64 route uses
[box64](https://github.com/ptitSeb/box64) and gl4es. Assembly patches use
[Mono.Cecil](https://github.com/jbevain/cecil).
