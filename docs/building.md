# Building, and converting a game by hand

Most porters only need `pmu port` ([porting.md](porting.md), and the five steps in the
[README](../README.md#port-a-game-in-five-steps)). This page lists what building needs and the
same steps by hand.

## What you need

- **A handheld** with an AArch64 userland with GLES 3 / EGL and SDL2. `ua_host` (`runtime/build.sh`)
  runs the 64-bit routes ([arm64.md](arm64.md), [mono-arm64.md](mono-arm64.md), [apk.md](apk.md),
  [il2cpp.md](il2cpp.md)). (The 32-bit armhf build and route were removed on 2026-11-28; the last tree
  with them is the tag `last-armhf`.)
- **To build:** Docker (the Dockerfiles hold the arm64 build image, and `ua-rust`, where `pmu` builds and runs: `setup-rs/tools/pmu.sh <command>`, e.g. as
  `alias pmu="$PWD/setup-rs/tools/pmu.sh"`), 7-Zip (to unpack Unity's installer
  by hand), and the .NET 8 SDK for asmpatch (`tools/asmpatch/build.sh`; `port check` runs it when
  asmpatch is missing). The SDK is installed on the computer itself, not in Docker: Microsoft's
  installer or `dotnet-install.sh --channel 8.0` (it goes to `~/.dotnet`; `DOTNET=<dir>/dotnet` names
  another). Its packages (Mono.Cecil, and for the handheld's build the .NET runtime pack and
  ILLink, about 200 MB) come from NuGet into `~/.nuget/packages` (`NUGET_PACKAGES=<dir>` puts them
  elsewhere).
- **Unity's Android player for the game's exact Unity version**, from a donor: a small game built
  with that Unity version (`tools/donor/build.sh`; the editor from `tools/donor/get-editor.sh`:
  [porting.md](porting.md#5-donors-build-unitys-player-for-the-port)), unpacked with `pmu player
  donor <version> <dir> --apk <donor.apk>`. A port's setup does exactly this with the
  donors its release ships: the handheld never downloads Unity's player. (On the computer, `pmu player
  fetch <version> <dir>` also reads the files from Unity's installer, about 10 MB.) The
  folder with `Variations/` and `Data/` is the "player package" below. Unity's licence terms apply
  to it; it is never redistributed here.
- **The game:** a build you own. A Mono desktop build of Unity 5.6 - 6000.6 for the main route
  ([compat/README.md](compat/README.md): what each line needs and what has run). The macOS or
  Linux build is the usual choice (its OpenGL Core shaders convert to GLES 3); a Windows build
  carries only Direct3D 11 shaders, which `pmu convert --steps shaders --set shaders.from=d3d11` translates with Unity's
  HLSLcc (`tools/hlslcc/build.sh`; for a port's setup on the handheld `ARCH=arm64
  tools/hlslcc/build.sh`; [shaders.md](shaders.md)). Other builds: the routes in
  [porting.md](porting.md#2-routes).

## The runtime

```sh
git clone --recurse-submodules https://github.com/wild1walker/portmaster-unity
cd portmaster-unity
bash runtime/build.sh               # -> runtime/host/out/arm64/ua_host (+ device checks)
```

`OPENH264=0 runtime/build.sh` builds the video core without OpenH264 (runtime/media/README.md).

**Offline, or without Docker Hub.** Each part builds its Docker image once (`docker build`, which
pulls Debian or Ubuntu base images from Docker Hub) and uses it as it is afterwards: every build
script checks for its image first (`docker image inspect`). With the images there
(`ua-build-arm64`, `ua-media-arm64`, `ua-port-rehearse`, `ua-rust`; `docker image ls`),
nothing is pulled. To rebuild an image after its Dockerfile changed, remove it (`docker image rm
<name>`).

**pmu** (the setup's tool): `setup-rs/tools/build_pmu.sh <out>` (`port package` runs it) builds in
the `ua-rust` image ([rust-setup.md](rust-setup.md#building-and-testing)) from a copy of setup-rs in
`$TMPDIR` (default `/tmp`; removed afterwards). Its build output and the crate cache stay in two
Docker volumes, shared by every checkout: `pmu-target` (`RS_TARGET_VOLUME`, a few GB) and
`pmu-cargo` (`RS_CARGO_VOLUME`); `docker volume rm pmu-target` gives the space back (the next build
starts from scratch).

## Converting and running a game by hand

```sh
alias pmu="$PWD/setup-rs/tools/pmu.sh"     # pmu in Docker
pmu convert "<Game>.app/Contents/Resources/Data" work/data --config game.json
pmu player donor <version> work/player --backend il2cpp --abi arm64-v8a --apk <donors>/donor-<version>-il2cpp-arm64.apk
pmu android-data --src work/data --pkg work/player --out work/android --pack all
```

Then the game's code on Unity's Mono for aarch64 (the mono-arm64 route: [mono-arm64.md](mono-arm64.md)),
or compiled with IL2CPP ([il2cpp.md](il2cpp.md)), and on the handheld, in one folder: `ua_host`, the
player's libraries in `lib/`, and the contents of `work/android/zip/` (APK, OBB,
`android_package.json`). Start it with `./ua_host "$PWD"` (see `tools/device/run.sh` and
[runtime.md](runtime.md)). [porting.md](porting.md#9-by-hand) has
the details of each step, and what a game may need beyond them (assembly patches, a helper DLL).

## Tests

How to run the unit tests and the rig: [CONTRIBUTING.md](../CONTRIBUTING.md).
