# Contributing

Thanks for helping! The most useful contributions, in order:

1. **Game porting reports**: you tried a game with these tools; it worked, or it did not. Use the
   [Game porting report](https://github.com/wild1walker/portmaster-unity/issues/new/choose) form.
2. **Bug reports** about the tools, with the command you ran and its log.
3. **Code and documentation**, as pull requests.

## Setting up

```sh
git clone --recurse-submodules https://github.com/wild1walker/portmaster-unity
cd portmaster-unity
pip install -r convert/requirements.txt     # Python 3.10+
```

Docker builds the runtime (`runtime/build.sh`) and runs the native tests. The .NET 8 SDK is
needed for `tools/asmpatch`. See [docs/building.md](docs/building.md).

## Running the tests

| What | Command | Notes |
|---|---|---|
| converter (Python) | `cd convert && python3 -m unittest discover -s tests` | synthetic data only, no Unity or game files needed. Tests that read real files are skipped unless you point their variables at your own copies (`PMUNITY_GAMES`, `PMUNITY_GLCORE_CORPUS`, `PMUNITY_D3D11_CORPUS`, `PMUNITY_APK`, ...: see each test's header) |
| loader | `runtime/loader/build.sh test` (and `ARCH=arm64 runtime/loader/build.sh test`) | synthetic Android libraries, in Docker |
| Android shims | `runtime/android/build.sh test` | in Docker, no GPU |
| fake JNI and UnityPlayer | `TESTS=1 runtime/build.sh` (or `runtime/jni/build.sh`) | armhf under QEMU; some QEMU builds crash on their threads: the test binaries also run on a handheld |
| video core | `runtime/media/build.sh test` | |
| ua_host parts | `runtime/host/test/pointer_test.sh` (pointer mode, on-screen keyboard, the 2017.x MSAA present fix), `display_test.sh`, `plugins_test.sh`, `prof_test.sh` (the sampling profiler, then `profsum.py` on its output); `runtime/jni/test/tls_test.sh` | `ARCH=arm64` where the script supports it |
| profiling tools | `python3 -m unittest discover -s tools/analysis`; `tools/device/test/profbench_test.sh` (the device bench against a fake handheld: ssh and the game are stand-ins) | synthetic profiles and bench snapshots |
| asmpatch | `tools/asmpatch/build.sh test` | fake assemblies built with Mono.Cecil |
| box64 route | `tools/box64/test/launcher_test.sh`, `tools/box64/test/port_test.sh` | need a Linux build of a game you own |
| a whole port | `tools/port/rehearse.sh` ([docs/porting.md](docs/porting.md#6-rehearsal)) | the first launch in a handheld-like container, then the game in the emulator |

If your change touches what a game sees, say in the pull request how you checked it: unit tests,
the emulator (`tools/emu`), or a handheld (model and firmware).

## Rules

- **No game files and no Unity files, ever.** Not in commits, tests, issues or releases. That
  includes anything made from them: converted data, APKs and OBBs, shader dumps, screenshots and
  videos of games, logs of game runs that quote game data, symbol lists, decompiled code. Tests use
  synthetic data written by the test itself; tests that need real files read them from the
  tester's own copies through environment variables. `.gitignore` blocks the usual file types;
  check `git status` before you commit.
- **No game names from test runs** in the repository: describe results by Unity version and build
  type ("a 2021.3 URP game"), as [docs/compat/README.md](docs/compat/README.md) does.
- **Keep the game's look and play.** Defaults must not change the picture or the game play. A
  change that does is an option, off by default.
- **Licence:** everything here is MIT (see [LICENSE](LICENSE)). By contributing you agree that your
  contribution is MIT licensed too. Third-party code needs a licence that allows this, and an
  entry in [THIRD_PARTY.md](THIRD_PARTY.md).
- **Commits:** one change per commit, with a message that says what changed and why. Keep personal
  details out of commits: no local paths of your machine (`/home/<you>/...`), IP addresses or host
  names in code, docs or logs. A GitHub "noreply" email address as the commit author is fine.
- **Documentation:** when a change adds or changes what the tools can do, update the page that
  describes it (and the support table in the README if a route or version changes), and say
  whether it ran on a handheld or only in the emulator.
