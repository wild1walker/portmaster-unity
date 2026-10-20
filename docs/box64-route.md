# The box64 route: a game's own Linux build under box64

The main route ([design.md](design.md)) runs Unity's ARM Android player natively and converts the
game's desktop data for it. It covers Mono builds of Unity 2017.3 - 2022.3. Some games cannot go
that way, so this second route runs **the game's own Linux x86_64 build, unmodified, under
[box64](https://github.com/ptitSeb/box64)**. box64 translates x86_64 code to ARM64 and runs the
system libraries the game opens (GL, X11, audio) as the handheld's own ARM64 libraries.

| | Android-player route | box64 route |
|---|---|---|
| game code | the game's IL on the Android player's Mono, native ARM | x86_64 machine code (IL2CPP) or Mono's JIT output, translated by box64 |
| Unity engine | Unity's ARM build | Unity's x86_64 Linux player, translated |
| frame rate | Cuphead: 55-60 fps on an H700 | Cuphead: about half (about 30 fps in its heavy levels): the main thread is emulation-bound |
| takes | Mono, 2017.3 - 2022.3, OpenGL shaders (or D3D11 for <= 2018.4) | any version with a Linux build, IL2CPP too, x86-only plugins |
| data | converted (GLES 3 shaders, textures, Android settings) | as it is (desktop GL), or GLES 3 shaders added (GLES mode, below) |

## When to use it

`pmunity route <game folder>` reads the build (ELF/PE/Mach-O headers, the data's serialized-file
headers, strings in the player; nothing is run) and says which route fits and why:

```
python -m pmunity route "<Game folder>"          # report
python -m pmunity route "<Game folder>" --json   # all facts (for tools such as port.py)
python -m pmunity route "<Game folder>" --launcher "Game.sh" --port game   # box64 launcher (below)
```

The box64 route is the one for:

- **IL2CPP builds** (`GameAssembly.so` next to the executable, `<Game>_Data/il2cpp_data/`): the
  game's code is compiled x86 machine code; there is no IL for the Android player's Mono. (The
  game's own Android build, if the player owns one, is the other way: apk-arm64 work.)
- **Unity versions the Android route does not cover** (5.x, 2017.1 - 2017.2, 2023.x, Unity 6).
- **Native plugins that exist only for x86** and that the game cannot do without: FMOD Studio and
  Wwise (sound), AVPro (video), a game's own plugins. The detector names each plugin and what it
  costs on the Android route.

When both routes fit, the Android route is the better port (about twice the frame rate). With
only a Windows or macOS build the box64 route needs the game's Linux build; a macOS **Mono** build's
data can also run on Unity's own Linux player of the same version (downloaded from Unity on the
device, as Cuphead's first port and other PortMaster ports did).

## What it costs

- **Performance**: everything x86 is translated. Cuphead (2017.4, Mono) ran its heavy levels at
  about 30 fps on an RG34XX-SP (H700, 4x Cortex-A53) with the main thread at 100% of a core, and
  boss fights at 42-45 fps after tuning (the `fast` box64 profile below, Mono AOT of the
  assemblies against the JIT's first-use hitches, game-specific patches). The Android route runs
  the same game at 55-60. IL2CPP games have no JIT, but all of their code is translated; Unity's
  engine code is translated in both cases.
- **Memory**: box64's translation cache and the x86 player cost tens of MB more than the Android
  route; a 1 GB handheld needs zram/swap for big games either way.
- **Legal**: the game's own build runs unmodified, from the player's own copy; the port ships
  scripts plus box64 (MIT), gl4es (MIT), glxes (ours, MIT) and box64's x86_64 GCC runtime
  libraries (GPL with the GCC Runtime Library Exception). In GLES mode the setup adds GLES 3
  shader programs to the player's copy of the data on the device, as the Android route's setup
  does. No Unity files are shipped: the Linux player is part of the game's build.

## GL: three ways to draw

Unity's Linux player draws with OpenGL Core (3.2+ core profile; 2017.x - Unity 6), Vulkan, or in
5.4 and older the legacy OpenGL 2 renderer. Most PortMaster handhelds have only OpenGL ES (Mali
blob drivers on muOS, Knulli, dArkOS, AmberELEC, ArkOS). The launcher picks one of three modes
(`PMU_GL`):

| mode | for | how |
|---|---|---|
| `gles` | GLES-only drivers (most handhelds); tested on Unity 5.5 and 2017.4 - 2022.3 | the player's own OpenGL ES mode: `-force-gles`, GLES 3 shader programs added to the data, `glxes` for GLX |
| `mesa` | desktop-GL drivers: Panfrost, Turnip (ROCKNIX) | the player's OpenGL Core renderer as on a PC; data as it is |
| `gl4es` | Unity 5.4 and older (OpenGL 2 renderer) | gl4es (OpenGL 2.1 on GLES 2), `-force-opengl` |

### GLES mode

Unity's Linux player has an OpenGL ES device built in, used with **`-force-gles`** (the
`-force-gles30/31/32` forms are not recognized; the switch is not in the player's strings, so
`pmunity route` recognizes the mode by `GLX_EXT_create_context_es2_profile`). Found in the test
rig, and it worked on every player it was tried with: 5.5 and 2017.4 - 2022.3 (results below). It
needs three things:

1. **OpenGL ES 3 in the build's graphics API list**: without it `-force-gles` is ignored. The player
   logs `Unknown renderer 11` for that entry and goes on. `pmunity convert`'s `settings` step does
   it (`graphics_apis: [11, 17]`: GLES 3, then OpenGL Core as before).
2. **GLES 3 shader programs** (shader platform 9): the ES device looks only for those ("Desired
   shader compiler platform 9 is not available in shader blob" and magenta otherwise). `pmunity
   convert`'s `shaders` step adds them next to the OpenGL Core ones, the same conversion as the
   Android route's (same numbers, GLSL ES 3.00). The player's own built-in shaders (`unity default
   resources`) already carry GLES 3 programs. One config does both steps:
   ```
   python -m pmunity convert "<Game>_Data" --config tools/box64/gles.json        # in place, resumable
   ```
3. **A GLX that gives OpenGL ES contexts.** The player makes its ES context through GLX
   (`GLX_EXT_create_context_es2_profile`) and first opens a desktop core-profile context to learn
   the GL level; without one it stops ("Unable to find a supported OpenGL core profile"), and the
   level it reads becomes the ES device's level: a 4.5 probe makes "OpenGL ES 4.5" and shaders that
   ask for `#version 450 es`. `tools/box64/glxes` (a `libGL.so.1`, ours) is GLX for ES contexts on
   EGL: every context is an EGL OpenGL ES context on the same X display, drawing to an EGL window
   surface of the GLX drawable; a context asked for without the ES profile answers as a 3.2 core
   profile (`GLXES_PROBE_VERSION`), which gives an OpenGL ES 3.2 device. box64 loads it in place of
   the system libGL (`BOX64_LIBGL`).

On the handheld the X display and EGL on X11 come from PortMaster's Westonpack in its
`crusty_x11egl` mode (EGL with X11 on top of SDL2, GLES 3.2; "near-native performance" in its
documentation). **Not yet run on a handheld**: glxes was tested only on Mesa's EGL in the rig (no
desktop GL library present at all), and Westonpack cannot be tested in the rig.

### mesa and gl4es modes

`mesa`: the player's own OpenGL Core renderer on a driver with desktop GL. Unity asks for a 3.2+
core profile; Panfrost's desktop GL version depends on the GPU and Mesa release (3.1 on older
Mesa for Mali-G31/G52, which Unity refuses; `MESA_GL_VERSION_OVERRIDE=3.3` is untested). On a PC
or a handheld with a full desktop GL this needs nothing from the port.

`gl4es`: gl4es gives OpenGL 2.1 on GLES 2. That is enough for Unity's legacy OpenGL 2 renderer
(Unity 4.x and 5.0 - 5.4; `pmunity route` lists `opengl2` among the player's renderers), not for
OpenGL Core: Unity reads its 2.1 version and refuses, or, with the version string faked, calls GL 3
functions gl4es does not have. gl4es is also what Westonpack's `crusty_glx_gl4es` mode bundles.

## box64

Built by `tools/box64/build.sh` from the official release tag (v0.4.4), generic ARM64 dynarec
(`-DARM64=ON -DARM_DYNAREC=ON`), on Debian 11: the binary needs glibc 2.29. Settings for Unity's
players are in `tools/box64/box64.env` (`box64_profile safe|default|fast`):

| profile | settings | why |
|---|---|---|
| `safe` | none: box64's own detection | box64 0.4.4 sets `BOX64_UNITY=1` (STRONGMEM=1) when `UnityPlayer.so` loads, and BIGBLOCK=0 + STRONGMEM=1 when `libmonobdwgc-2.0.so` (MonoBleedingEdge) loads |
| `default` | `BLEEDING_EDGE=0 STRONGMEM=1 BIGBLOCK=2` | the MonoBleedingEdge override off, strong memory order kept: a 2019.4 Mono game on an H700 ran 37-41 fps against 27-31 with box64's Mono profile, no crashes |
| `fast` | `default` + `UNITYPLAYER=0 STRONGMEM=0 SAFEFLAGS=0 CALLRET=1 FORWARD=1024` | Cuphead (2017.4, Mono) on an H700: ~28 -> 42-45 fps in its heaviest fights. STRONGMEM=0 can break lock-free code that relies on x86 ordering: play the whole game before making it a port's default |

Other switches: `BOX64_MAXCPU` (Unity sizes its job workers by the CPU count; Cuphead kept 2 on a
4-core H700), DynaCache (on by default in 0.4: translated code kept between starts; the launcher
keeps it in the port's `conf/dynacache`, 512 MB at most, `PMU_DYNACACHE=0` turns it off).
box64's `x64lib` brings x86_64 `libstdc++.so.6` and `libgcc_s.so.1`: Unity players up to 2018.4
link libstdc++ dynamically, all of them need libgcc_s.

**Mono AOT** helped Cuphead's first-use hitches a lot (every method JIT-compiled under emulation
costs a translation too): Unity's Mono can compile a game's assemblies ahead of time to `.so`
files it loads instead of JIT-compiling. That needs a per-game build step in x86_64 (Mono's AOT
compiler, x86_64 binutils) and a rebuild after every assembly change; not part of these tools yet.

## What the device needs

Under box64 the player's x86_64 glibc requirement is met by box64's own wrapped C library, not
by an x86 glibc. The handheld needs:

- glibc 2.29 or newer for box64 itself (Debian 11 build; glxes needs 2.17, gl4es 2.27);
- the libraries box64 runs natively for the player (all `dlopen`ed except libGL in 5.x): GL
  (`libGL.so.1`, or glxes in GLES mode; `libEGL.so.1`, `libGLESv2.so.2`), X11 (`libX11`, `libXext`,
  `libXcursor`, `libXrandr`, `libXi`, `libXinerama`, `libXss`, `libXxf86vm`; 2020.3+ also
  `libxkbcommon`, the Wayland client libraries and `libdbus-1`, all optional), `libasound.so.2`,
  `libpulse-simple.so.0` (optional), `libudev.so.1` (pads; optional);
- an X server: none of the handheld firmwares runs one, so the launcher uses Westonpack
  (`weston_pkg_0.2`, Xwayland), as PortMaster's other X11 ports do.

What the players ask for, as read by `pmunity route` from the test corpus:

| Unity | x86_64 glibc asked for (met by box64) | libstdc++ | libraries opened at run time |
|---|---|---|---|
| 5.3, 5.5 | 2.15 | dynamic (GLIBCXX_3.4.15) | libGL, libX11, libXcursor, libXrandr linked; ALSA, Pulse, udev |
| 2017.4, 2018.4 | 2.15 | dynamic | GL/EGL/GLES, X11 family, ALSA, Pulse, udev, Vulkan |
| 2019.4 | 2.15 | static | same |
| 2020.3 - 2022.3 | 2.15 - 2.17 | static | same, + xkbcommon, Wayland client, dbus |
| 6000.5 - 6000.6 (Unity 6) | 2.34 - 2.35 (+ `libanl.so.1`) | static | same |

Firmwares (glibc of their base system; GPU driver):

| firmware | glibc | GL | notes |
|---|---|---|---|
| Knulli (H700, ...) | 2.40 | libmali, GLES only (fbdev) | `gles` mode |
| muOS | 2.3x (not checked here) | libmali, GLES only | `gles` mode |
| dArkOS | 2.36 (Debian 12) | libmali (KMS/DRM) | `gles` mode |
| ArkOS | 2.30 (Ubuntu 19.10) | libmali | `gles` mode; box64 needs 2.29: fits |
| AmberELEC | 2.3x (not checked here) | libmali (legacy) | `gles` mode |
| ROCKNIX | 2.40+ | Panfrost (Mesa) or libmali, Sway/Wayland | `mesa` where the desktop GL version is 3.2+, else `gles` |

These glibc values are the firmwares' base systems as documented by them, not measured on devices
in this work; box64's 2.29 is measured (`tools/box64/out/versions.txt`).

### 32-bit x86 builds

Unity made 32-bit Linux players until 2019.1 (the target was deprecated in 2019.1 and removed in
2019.2); "Universal" builds ship both (`.x86` and `.x86_64`), so only x86-only builds are a
problem. Two ways, both untested here:

- **box86** on a 32-bit ARM userland (armhf `libc`, GL/GLES, SDL2, X11 client libraries). Knulli
  on H700 devices has one (`/lib32`, as the Android route uses); most other firmwares do not.
  box86 0.3.8 is the last release (box86 is no longer developed actively).
- **box64 with BOX32** (`BOX64_CMAKE=-DBOX32=ON tools/box64/build.sh`): 32-bit x86 on the 64-bit
  userland, with box64's own 32-bit wrappers; "experimental, do not use" in box64 0.4.4.

## Making a port

1. `python -m pmunity route "<Game folder>"`: the route, the GL mode, the plugins, the libraries.
2. `tools/box64/build.sh` -> `tools/box64/out/` (box64, gl4es, glxes, licences, versions).
3. Port folder (PortMaster layout):
   ```
   ports/<port>/
     box64/box64, box64/x64lib/, box64/box64.env, box64/unityprefs.sh   (tools/box64)
     glxes/libGL.so.1                                                   (gles mode)
     gl4es/libGL.so.1                                                   (gl4es mode, if not Westonpack's)
     licenses/LICENSE.box64.txt, LICENSE.gl4es.txt, LICENSE.gcc-runtime.txt, LICENSE.<port>.txt
     <port>.gptk                                                        (gptokeyb: keys, mouse, exit hotkey)
     gamedata/                                                          (the player copies the Linux build here)
   <Title>.sh                                                           (the launcher)
   ```
   `port.json`: `"runtime": ["weston_pkg_0.2.squashfs"]`, `"arch": ["aarch64"]`.
4. The launcher: `python -m pmunity route "<Game folder>" --launcher "<Title>.sh" --port <port>
   [--title "<Title>"]` fills `tools/box64/launcher.sh.in` (its `@FIELD@`s are listed at its top)
   with the executable, the data folder, the GL mode (`gles`; `gl4es` for a player with only the
   OpenGL 2 renderer or data the converter cannot take) and the box64 profile (`default`). It
   follows PortMaster's conventions: `control.txt`, `$ESUDO`, `$GPTOKEYB`, `pm_platform_helper`,
   Westonpack mounted and `westonwrap.sh` given box64's and the GL settings as `VAR=value`
   arguments (Westonpack reloads PortMaster's settings before it starts the game, and ROCKNIX
   exports its own `BOX64_LD_LIBRARY_PATH`), `pm_finish`. Unity's screen arguments come from
   `$DISPLAY_WIDTH`/`$DISPLAY_HEIGHT`; `unityprefs.sh` writes the same values as Unity's own screen
   keys in the game's prefs (resolution, full-screen window) and nothing else.
   `tools/box64/test/launcher_test.sh <launcher> <game folder> <out>` runs a launcher end to end in
   the test rig with a stand-in PortMaster (`test/mockpm/`: the names a launcher uses, a
   `westonwrap.sh` that only applies the `VAR=value`s).
5. GLES mode only: a first-start setup (PortMaster's patcher, as portmaster-cuphead's) that runs
   `pmunity convert "<Game>_Data" --config gles.json` in place on the device.
6. Input: Unity's Linux players have SDL2 built in and read pads with the firmware's
   `SDL_GAMECONTROLLERCONFIG` mapping (the player's legacy SDL numbering can differ from the
   firmware's; see the 20 Minutes Till Dawn port's launcher for a remap); gptokeyb gives keyboard and
   mouse to games that want them, and the exit hotkey.
7. Saves: `HOME=<port>/conf`, so Unity writes `conf/.config/unity3d/<company>/<product>/`.

User switches in `ports/<port>/options.txt`: `PMU_GL`, `PMU_BOX64`, `PMU_MAXCPU`, `PMU_ARGS`
(more player arguments, e.g. `-force-gfx-direct`), `PMU_DYNACACHE=0`.

## Test rig

`tools/box64/rig.sh <game folder> <out> [seconds]` runs a Linux build under box64 in an arm64
container (native on an Apple-silicon Mac): Xvfb, Mesa (llvmpipe),
4 CPUs, no network, the game folder read-only; no sound device (a sound server would double the
image). It writes screenshots, the player and box64 logs, frames per second (Mesa's HUD, written to
a file, not drawn; in GLES mode glxes's own swap count, `GLXES_FPS`) and CPU time per thread per
frame (`rigstats.py`, on the host). `GL=mesa` (desktop GL 4.5), `GL=gles` (no desktop GL library at all, EGL + GLES 3.2
only, as a Mali driver: for `-force-gles` with glxes), `GL=noop` (nothing rasterized: the CPU
side alone), `GL=gl4es`; `OVERLAY=<converted files>`, `KEYS`, `CLICKS`, `PROFILE`, `ARGS`
(header of the script).

Frame rates there are rough: the Mac's cores are several times faster than a Cortex-A53, llvmpipe
draws on the same CPUs, and other jobs share the machine. The per-thread CPU time per frame of the
player's main and render threads is the better number to scale: Cuphead's box64 work measured the
H700's A53 cores at about 3.7x slower than this container for the same translated code.

## Results (emulated rig)

Twelve free Linux builds (2026-10-20; box64 0.4.4 `default` profile, 640x480, 4 CPUs of an
Apple-silicon Mac shared with other jobs; frame rates measured under that load, so rough).
"GLES" = GLES mode in the rig's GLES-only container (no desktop GL library at all): converted
data, `-force-gles`, glxes on Mesa's EGL. "est." = the player's main/render thread CPU time per
frame times 3.7 (Cortex-A53 at 1.5 GHz against this container, from Cuphead's box64 work), capped
at 60: the CPU side only; a Mali-G31 adds its own limit for 3D scenes.

| Unity | backend | why box64 | desktop GL (Mesa) | GLES mode | main thread / frame | est. H700 (menus) |
|---|---|---|---|---|---|---|
| 5.5.1 | Mono (.NET 3.5) | version | menu | menu (`--any-unity-version` conversion) | 18 ms (3D planet on llvmpipe) | ~15-30 |
| 2017.4.40 | Mono (.NET 3.5), Steamworks | (both fit) | logo video, then title menu | title menu | 5-18 ms | ~15-50 |
| 2018.4.36 | bdwgc | (both fit) | title menu | title menu | 0.5-4 ms | 60 |
| 2019.4.8 | bdwgc | (both fit) | title | title | 0.7-7 ms | ~40-60 |
| 2020.3.49 | bdwgc | (both fit) | title menu | title menu (also through the launcher, below) | 0.4-7 ms | ~40-60 |
| 2021.3.16 | bdwgc, URP, Burst | (both fit) | main menu, then the game's name entry | name entry (the game's first-start flow) | 2-24 ms | ~10-25 (URP 3D) |
| 2022.3.36 | bdwgc, URP, FMOD Studio, Steam, Burst | x86-only FMOD | title menu | title menu | 1-14 ms | ~20-40 |
| 2020.3.28 | **IL2CPP** | backend | main menu | - (the converter's settings step fails on this build's globalgamemanagers) | 6 ms | ~25 |
| 2022.3.4 | **IL2CPP**, Burst | backend | opening scene | opening scene, same picture | 9-12 ms | ~20-30 |
| 6000.5.9 (Unity 6) | **IL2CPP**, Burst | backend, version | sign-in screen (an online game) | - (format 23 data: no converter yet) | 7 ms | ~40 |
| 6000.6.4 (Unity 6) | bdwgc, Burst | version | plays (it starts in the game) | - (format 23 data) | 9 ms | ~30 |
| 5.0.0, 5.3.2 | Mono | 32-bit x86 only | not run: needs box86 or BOX32 | | | |

- Every 64-bit build tried reached its menu (or its first screen) under box64 with no change to
  the game, and GLES mode drew the same pictures as desktop GL on all seven versions it could be
  converted for (5.5, 2017.4 - 2022.3, Mono and IL2CPP).
- Startup is slow under emulation: 20-60 s to the title in the loaded rig (the JIT and box64
  translating the engine); box64's DynaCache keeps translated code between starts on the device.
- On the same 3.2 probe, 5.5, 2017.4 and 2018.4 make an "OpenGL ES 3.1 AEP" device, 2019.4 and
  newer an "OpenGL ES 3.2" one.
- With nothing rasterized (`GL=noop`) the menus' main threads need 0.5-1.9 ms a frame here, so
  menus are not CPU-bound on an A53; gameplay is (Cuphead: ~30 fps in heavy levels).
- The launcher end to end (`test/launcher_test.sh`, the 2020.3 game, GLES mode, stand-in PortMaster):
  title menu at 640x480 full screen, prefs written. Xvfb has no window manager: without the input
  focus that game hides its menu, so the test gives the window focus as Westonpack's kiosk shell
  does.
- Not seen in the rig: sound (no sound device: FMOD's no-sound output), pads, a real Mali driver,
  Westonpack.
