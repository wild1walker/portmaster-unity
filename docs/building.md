# Building, and converting a game by hand

Most porters only need `pmunity port` ([porting.md](porting.md), and the five steps in the
[README](../README.md#port-a-game-in-five-steps)). This page lists what building needs and the
same steps by hand.

## What you need

- **A handheld** with an AArch64 kernel that runs 32-bit programs and has a 32-bit userland with
  GLES 3 / EGL and SDL2 (e.g. Knulli on Allwinner H700 devices: `/lib32`, `/usr/lib32`). The
  64-bit `ua_host` (`ARCH=arm64 runtime/build.sh`) runs the 64-bit routes on a 64-bit userland
  ([arm64.md](arm64.md), [apk.md](apk.md), [il2cpp.md](il2cpp.md)).
- **To build:** Docker (the Dockerfiles hold Debian bookworm armhf cross toolchains and the arm64
  build image), Python 3.10+ with `convert/requirements.txt`, 7-Zip (to unpack Unity's installer
  by hand), the .NET SDK if a port patches a game's assemblies (`tools/asmpatch`).
- **Unity's Android player for the game's exact Unity version.** `python -m pmunity player fetch
  <version> <dir>` reads only the files it needs from Unity's installer (about 10 MB). Or install,
  or just download, the Unity editor's "Android Build Support" for that version from Unity's
  download archive and unpack it (`7z x UnitySetup-Android-Support-for-Editor-<version>.exe`). The
  folder with `Variations/` and `Data/` is the "player package" below. Unity's licence terms apply
  to it; it is never redistributed here.
- **The game:** a build you own. A Mono desktop build of Unity 5.6 - 6000.6 for the main route
  ([compat/README.md](compat/README.md): what each line needs and what has run). The macOS or
  Linux build is the usual choice (its OpenGL Core shaders convert to GLES 3); a Windows build
  carries only Direct3D 11 shaders, which `pmunity shaders --from d3d11` translates with Unity's
  HLSLcc (`tools/hlslcc/build.sh`; for a port's setup on the handheld `ARCH=arm64
  tools/hlslcc/build.sh`; [shaders.md](shaders.md)). Other builds: the routes in
  [porting.md](porting.md#2-routes).

## The runtime

```sh
git clone --recurse-submodules https://github.com/wild1walker/portmaster-unity
cd portmaster-unity
runtime/build.sh                    # -> runtime/host/out/ua_host (+ device checks); ARCH=arm64 for 64-bit
```

`OPENH264=0 runtime/build.sh` builds the video core without OpenH264 (runtime/media/README.md).

## Converting and running a game by hand

```sh
pip install -r convert/requirements.txt
cd convert
python -m pmunity convert "<Game>.app/Contents/Resources/Data" work/data --config game.json
python -m pmunity player fetch <version> work/player
python -m pmunity android-data --src work/data --pkg work/player --out work/android --pack all
```

Then copy to the handheld, into one folder: `ua_host`, the player's three libraries into `lib/`,
and the contents of `work/android/zip/` (APK, OBB, `android_package.json`,
`assets/bin/Data/Managed/etc`). Start it with `LD_LIBRARY_PATH=/lib32:/usr/lib32 ./ua_host "$PWD"`
(see `tools/device/run.sh` and [runtime.md](runtime.md)). [porting.md](porting.md#8-by-hand) has
the details of each step, and what a game may need beyond them (assembly patches, a helper DLL).

## Tests

How to run the unit tests and the rig: [CONTRIBUTING.md](../CONTRIBUTING.md).
