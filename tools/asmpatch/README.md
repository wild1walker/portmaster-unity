# asmpatch

Generic patches for a Unity game's managed assemblies, for Unity's Android player
(portmaster-unity). A desktop build compiled some decisions in for its platform; on the Android
player they must come out the way the game's Android build has them. Each patch looks for its
target in the game's own assemblies and rewrites the game's own IL there with
[Mono.Cecil](https://github.com/jbevain/cecil): nothing is shipped into the game, nothing is
replaced by outside code, and a patch that does not find exactly what it expects changes nothing.

```sh
asmpatch [--only=a,b] [--skip=a,b] <Managed in> <out dir>   # patch: writes only changed assemblies
asmpatch --list [<Managed in>]                              # the patches; what each would change there
asmpatch --survey <Managed in>                              # the game's input stacks and what they need
```

`<Managed in>` is the game's `<Game>_Data/Managed` folder (read only). Every .NET `.dll` in it is
read; references between them resolve to the same copies, so a patch can follow a base class into
a library. Output, one line per patch:

| line | meaning |
|---|---|
| `ok   <patch>: <file>: <what changed>` | applied |
| `would <patch>: ...` | `--list`: would apply |
| `done <patch>: ...` | already applied (e.g. a second run on patched assemblies) |
| `n/a  <patch>: <why>` | the game has nothing for it |
| `skip <patch>` | left out by `--only` / `--skip` |
| `FAIL <patch>: <why>` | its target is there but not as expected: nothing at all is written, exit status 1 |
| `wrote <file>` | an assembly written to `<out dir>` |

Exit status 0 (also when nothing applies), 1 when a patch failed, 2 for a usage error. A patched
assembly that would reference the patcher's own runtime (`System.Private.CoreLib`,
`System.Runtime`, `netstandard`) where the original did not is a failure too: games run on Mono.

## Patches

| patch | what |
|---|---|
| `rewired-android` | [Rewired](https://guavaman.com/projects/rewired/)'s `InputManager.DetectPlatform` reports Android. A desktop build compiled it to say Windows, OSX or Linux, and on the Android player Rewired then looks for that platform's input APIs (Linux: libudev) and the pad's buttons do nothing for the game. As Android, Rewired reads pads through Unity's input with its Android controller definitions, as the game's own Android builds do. `platform = Android`, `editorPlatform = None`, `webplayerPlatform = None` and `isEditor = false` are stored right before each return of the method as compiled; each value is the enum constant found by name in the game's Rewired (its numbers differ between versions). Works on the source edition (`InputManager` in `Assembly-CSharp`) and on DLL editions (any class deriving from `Rewired.InputManager_Base` with a `DetectPlatform` body, in whichever assembly). `pmunity android-data` adds the `Joy<j>Axis<a>` input axes Rewired's Unity input reads (`--rewired-joysticks`). |

Adding a patch: a class deriving from `Patch` (`src/Patch.cs`) in `src/Patches/`, listed in
`Patch.All`; its `Run(game, apply)` finds the targets, reports them, and with `apply` changes them
and marks their assemblies changed. Look things up by name (types, members, enum constants), fail
rather than guess, and keep the game's look and feel: a patch belongs here only if it restores
what the game's own Android build does.

## With pmunity android-data

```sh
python -m pmunity android-data --src <Game>_Data --pkg <player> --out work/android --pack all \
  --asmpatch "/path/to/asmpatch --only=rewired-android"
```

`--asmpatch COMMAND` runs `COMMAND <Managed> <work dir>` on `--game-managed` (default
`<src>/Managed`) and ships each assembly it wrote instead of the game's copy (like
`--managed-file`); its `ok`/`done`/`FAIL` lines go into the log and `android_data_report.json`.
With .NET installed instead of the self-contained build: `--asmpatch "dotnet /path/to/asmpatch.dll"`.
An assembly that asmpatch changes and that a port's own `--assembly-patch` or `--managed-file`
also provides is an error: run asmpatch inside that patch command instead (on its output).

## --survey

Which input stacks the game's own assemblies use (not Unity's or the class libraries): Rewired
(edition, compiled platform), InControl, the Input System package, the legacy `UnityEngine.Input`
calls by kind (axes/buttons, keys, mouse, touch), `OnMouse*` messages, UI pointer handlers, and
whether code selects UI elements (pad/keyboard menu navigation). The verdict line says whether the
menus are likely pointer-only (then the port needs ua_host's pointer mode, `UA_POINTER=1`, see
docs/input.md).

## Building

`./build.sh` (needs the .NET 8 SDK; packages from NuGet: Mono.Cecil 0.11.6, and for linux-arm64
the runtime pack and ILLink):

- `out/any/asmpatch.dll`: any machine with .NET 8 or newer (`dotnet out/any/asmpatch.dll ...`; on
  the building machine also `out/any/asmpatch`).
- `out/linux-arm64/asmpatch`: self-contained, single file, trimmed, for an aarch64 handheld
  without .NET (built as portmaster-cuphead builds its code patcher). Distribute it with the .NET
  runtime's licence and third-party notices (in the NuGet runtime pack) and Mono.Cecil's MIT
  licence (THIRD_PARTY.md).

`./build.sh test` also runs the tests (`test/`): fake Rewired assemblies are built with Cecil (a
`Rewired_Core` whose enums are numbered unlike any real Rewired, a source-edition game and a DLL
edition, a `DetectPlatform` with early returns and a short branch the patch pushes out of range),
patched, loaded into the test process and called; plus the not-applicable, already-applied,
failing and command-line cases.
