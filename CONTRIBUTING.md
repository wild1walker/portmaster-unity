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
```

The tools are Rust (setup-rs: `pmu`, the setup's tool and the porter's; crates/devtools: `pmu-dev`, the
developer's), run in Docker through `setup-rs/tools/pmu.sh` and `setup-rs/tools/pmu-dev.sh`; nothing
here needs Python (the converter that was Python, convert/, was retired on 2026-11-27).

Docker builds the runtime (`runtime/build.sh`) and runs the native tests. The .NET 8 SDK is
needed for `tools/asmpatch`. See [docs/building.md](docs/building.md).

## Running the tests

| What | Command | Notes |
|---|---|---|
| the setup's tools (Rust: `pmu`, setup-rs) | `RS_ROOT=<a folder> setup-rs/tools/rs.sh . cargo test --locked` | in Docker, in the `ua-rust` image (its recipe: `setup-rs/docker/`, [docs/rust-setup.md](docs/rust-setup.md#building-and-testing)); its unit tests use made-up data (a test that reads a game skips without `PMU_TEST_DATA`); the golden files of `pmu/tests/golden/` freeze the packager's, the check's and the converter's outputs |
| loader | `runtime/loader/build.sh test` (and `ARCH=arm64 runtime/loader/build.sh test`) | synthetic Android libraries, in Docker |
| Android shims | `runtime/android/build.sh test` | in Docker, no GPU |
| fake JNI and UnityPlayer | `TESTS=1 runtime/build.sh` (or `runtime/jni/build.sh`) | natively in `ua-build-arm64`; the test binaries also run on a handheld |
| video core | `runtime/media/build.sh test` | |
| ua_host parts | `runtime/host/test/pointer_test.sh` (pointer mode, on-screen keyboard, the 2017.x MSAA present fix), `display_test.sh`, `fbdev_test.sh` (the fbdev path's flips against a fake Mali driver and display), `plugins_test.sh`, `prof_test.sh` (the sampling profiler, then `pmu-dev profsum` on its output), `threads_test.sh` (`UA_AFFINITY`/`UA_NICE`), `unityhooks_test.sh` (the jump into a libunity function and its trampoline, `UA_GFX_JOBS`); `runtime/jni/test/tls_test.sh` | `ARCH=arm64` where the script supports it |
| profiling and developer tools (`pmu-dev`) | `RS_ROOT=<a folder> setup-rs/tools/rs.sh . cargo test -p devtools`; `tools/device/test/profbench_test.sh` (the device bench against a fake handheld: ssh and the game are stand-ins) | synthetic profiles, bench snapshots, shaders and instructions |
| asmpatch | `tools/asmpatch/build.sh test` | fake assemblies built with Mono.Cecil, and C# stand-ins (`test/fakes/`) for the optimizations and reductions |
| box64 route | `tools/box64/test/launcher_test.sh`, `tools/box64/test/port_test.sh` | need a Linux build of a game you own |
| a whole port | `tools/port/rehearse.sh` ([docs/porting.md](docs/porting.md#7-rehearsal)) | the first launch in a handheld-like container, then the game in the emulator |
| the games (regression) | `tools/regress/regress.sh fast` (convert, pack and checks), `regress.sh rig` (the games in the emulator) | **before merging a branch that touches setup-rs's converter or `runtime/`**: a list of games per Unity line and route, compared with the recorded baseline ([tools/regress/README.md](tools/regress/README.md)); needs your own game corpus |

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
- **Keep the game's play, and its look unless a port asks.** The tools' defaults must not change the
  picture or the game play; a change that does is an option, off by default. A port's setup is the
  one place where the picture is lower: the game's own lowest graphics options and the converter's
  handheld profile, with no setup question to undo them
  ([porting.md](docs/porting.md#the-games-own-options-lowest-and-the-games-rules-kept)); never the game play.
- **Licence:** everything here is MIT (see [LICENSE](LICENSE)). By contributing you agree that your
  contribution is MIT licensed too. Third-party code needs a licence that allows this, and an
  entry in [THIRD_PARTY.md](THIRD_PARTY.md).
- **Commits:** one change per commit, with a message that says what changed and why. Keep personal
  details out of commits: no local paths of your machine (`/home/<you>/...`), IP addresses or host
  names in code, docs or logs. A GitHub "noreply" email address as the commit author is fine.
- **Documentation:** when a change adds or changes what the tools can do, update the page that
  describes it (and the support table in the README if a route or version changes), and say
  whether it ran on a handheld or only in the emulator.
