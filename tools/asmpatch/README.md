# asmpatch

Generic patches for a Unity game's managed assemblies, for Unity's Android player
(portmaster-unity). A desktop build compiled some decisions in for its platform; on the Android
player they must come out the way the game's Android build has them. Each patch looks for its
target in the game's own assemblies and rewrites the game's own IL there with
[Mono.Cecil](https://github.com/jbevain/cecil): nothing is shipped into the game, nothing is
replaced by outside code, and a patch that does not find exactly what it expects changes nothing.

The patches come in three kinds, each with its own rule (`--list` groups them):

| kind | what it may change | selected |
|---|---|---|
| android ([Patches](#patches)) | what the game does on the Android player, to what its own Android build does | by default where it applies; the opt-in ones by name |
| optimize ([Optimizations](#optimizations)) | how much work the game's code does, never what it does: a default-on one leaves every value the game can observe as it was, shown for the code it patches | by default where it applies (`--no-optimize` leaves them out); the not exact ones only by name |
| reduce ([Reductions](#reductions)) | the picture, for small handhelds: never the game play (timing, physics, collisions, scripts) | only by name (`--with`), and then chosen at run time by an environment variable |

```sh
asmpatch [--only=a,b] [--skip=a,b] [--with=c] [--no-optimize] <Managed in> <out dir>   # patch: writes only changed assemblies
asmpatch --list [<Managed in>]                    # the patches; what each would change there
asmpatch --survey <Managed in>                    # the game's input stacks and file access, and what they need
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
| `skip <patch>` | left out by `--only` / `--skip`, or an opt-in patch not named (`skip <patch> (opt-in: --with=<patch>)`) |
| `FAIL <patch>: <why>` | its target is there but not as expected: nothing at all is written, exit status 1 |
| `wrote <file>` | an assembly written to `<out dir>` |

Opt-in patches (below) run only when named, by `--only` or `--with` (`--with` adds to the default
set or to `--only`'s list; `--only=` with an empty list runs nothing but the `--with` ones); `--list`
reports them like the others. An option given twice adds to its list.

Exit status 0 (also when nothing applies), 1 when a patch failed, 2 for a usage error. A patched
assembly that would reference the patcher's own runtime (`System.Private.CoreLib`,
`System.Runtime`, `netstandard`) where the original did not is a failure too: games run on Mono.

## Patches

| patch | what |
|---|---|
| `rewired-android` | [Rewired](https://guavaman.com/projects/rewired/)'s `InputManager.DetectPlatform` reports Android. A desktop build compiled it to say Windows, OSX or Linux, and on the Android player Rewired then looks for that platform's input APIs (Linux: libudev) and the pad's buttons do nothing for the game. As Android, Rewired reads pads through Unity's input with its Android controller definitions, as the game's own Android builds do. `platform = Android`, `editorPlatform = None`, `webplayerPlatform = None` and `isEditor = false` are stored right before each return of the method as compiled; each value is the enum constant found by name in the game's Rewired (its numbers differ between versions). Works on the source edition (`InputManager` in `Assembly-CSharp`) and on DLL editions (any class deriving from `Rewired.InputManager_Base` with a `DetectPlatform` body, in whichever assembly). `pmunity android-data` adds the `Joy<j>Axis<a>` input axes Rewired's Unity input reads (`--rewired-joysticks`). |
| `streaming-assets` (opt-in) | Reads of `Application.streamingAssetsPath` whose value reaches System.IO get the folder beside the APK, `Game_Data/StreamingAssets/` (`pmunity android-data --streaming-assets both` puts the files there and in the OBB). A desktop build reads StreamingAssets as files (`File.ReadAllText`, `Directory.GetFiles`, `FileStream`, XML and SQLite files by path, a native library's path argument); on Android the getter is `jar:file://<apk or obb>!/assets`, which only Unity's own APIs open. Each such `call Application::get_streamingAssetsPath()` becomes a call of a static method the patch writes into the same assembly (`<asmpatch>Paths.StreamingAssets`): it reads the getter, and only for a `jar:file://<dir>/<archive>!/…` value returns `<dir>/Game_Data/StreamingAssets`, so on a desktop player the patched assembly behaves as the original. Reads that reach only Unity's APIs (AssetBundle, UnityWebRequest, WWW, VideoPlayer) keep the jar path, which they read from the OBB, as in the game's Android build. Which reads reach what comes from a data-flow analysis of the IL (below); it covers the game's assemblies and Unity's packages that ship with it (Addressables, Unity Services). |
| `data-path` (opt-in) | Reads of `Application.dataPath` whose value reaches System.IO get `Game_Data/` beside the APK (`pmunity android-data --data-path`: a writable folder with the data folder's own files). On the Android player `dataPath` is the APK or OBB file, so `dataPath + "/config.ini"` and writes next to the data fail; the helper (`<asmpatch>Paths.DataPath`) returns `<folder>/Game_Data` for a value ending in `.apk` or `.obb` and the value itself otherwise. Reads that reach only Unity's APIs (an Android branch's `"jar:file://" + dataPath + "!/assets/"`) keep the archive. |
| `streaming-assets-all` (opt-in) | Every read of the getter gets the folder (`android-data --streaming-assets loose`: the files are only beside the APK, so Unity's APIs read them from there too, as on the desktop, where the getter is a folder for them as well). For big StreamingAssets, which `both` would keep twice. Code that runs only on Android (a runtime `Application.platform` check, compiled into the desktop build too) then gets a folder where it expects the jar path: `UnityWebRequest.Get(path)` without `file://` turns a bare path into `http://localhost/…`. |

| `inputsystem-android` | Unity's [Input System](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.7/manual/index.html) package compiles its Android device support only into Android builds; on the Android player the engine reports the pad as an Android game controller (interface `Android`, class `AndroidGameController`, state `'AGC '`), which a desktop build's `Unity.InputSystem.dll` has no layout for, so the game sees no gamepad. The patch registers layouts for it with the package's own public `InputSystem.RegisterLayout(json)`, at the end of `InputSystem.PerformDefaultPluginInitialization` (where the package registers its platform support). The layouts are JSON written here from the player's state format (Android key codes as bits from byte 0, MotionEvent axes as floats from byte 28), extend the package's `Gamepad` and place each of its controls; a pad with Microsoft's vendor id becomes an `XInputController` where the build has that class. Not applicable without `Unity.InputSystem.dll` or when it has the Android support (an Android build); fails if those two methods are missing. Keyboards, mice and touchscreens need nothing (common formats). docs/input.md has the mapping. |

Adding a patch: a class deriving from `Patch` (`src/Patch.cs`) in `src/Patches/`, listed in
`Patch.All`; its `Run(game, apply)` finds the targets, reports them, and with `apply` changes them
and marks their assemblies changed. Look things up by name (types, members, enum constants), fail
rather than guess, and keep the game's look and feel: an android patch belongs here only if it
restores what the game's own Android build does; an optimization only if it is exact (below) or
opt-in; a reduction only if the game plays the same.

## Optimizations

Work the game's own code does every frame that changes nothing, left out where the patch can show
that. The rule for a default-on optimization: every value the game can observe (fields, return
values, exceptions, the order of events, what the engine is told) is what it would have been; the
patch proves its preconditions in the game's assemblies at patch time and is `n/a` (with the reason)
when it cannot, never a guess. They run by default like the android patches (`--no-optimize`
leaves out those not named; a port's `android_data.asmpatch = "auto"` runs them, and `port check`
chooses `auto` for a game with PlayMaker). The ideas come from Clazex's
[HollowKnight.AlreadyEnoughPlayMaker](https://github.com/Clazex/HollowKnight.AlreadyEnoughPlayMaker)
mod (MIT); the code here is written for asmpatch and, unlike the mod's, keeps PlayMaker's behaviour
exactly ([docs/profiling.md](../../docs/profiling.md#playmakers-per-frame-work-asmpatch-optimizations), which also has the
mod's other rewrites and why they are not here).

| patch | what |
|---|---|
| `playmaker-loopcount` | PlayMaker's `Fsm.UpdateStateChanges` runs after every update of every FSM (Update, FixedUpdate, LateUpdate) and after each transition, and sets `loopCount = 0` on every state of the FSM (the count behind PlayMaker's guard against an FSM entering states `MaxLoopCount` times in one update); in Hollow Knight's King's Pass that loop was the main thread's largest managed cost. Only `FsmState.OnEnter` makes a count other than 0, so when no state anywhere was entered since an FSM's last reset and its state array is unchanged, the loop would store 0 over 0. The patch adds a program-wide change counter (`<asmpatch>PlayMaker.Changes`, a long) that goes up after every write of a loop count other than a constant 0 and after every store into a state array or into `Fsm.states`, and to each `Fsm` a non-serialized field with the counter's value at its last whole reset (plus one); `UpdateStateChanges` skips the loop while they agree. Every count, the guard and its error stay as they were. The state arrays are followed through every assembly of the game: the field, its getter, locals, methods that return them, new `FsmState[]` arrays; a use the patch cannot follow (an array passed to a method, kept in another field, an element's or the field's address taken, a delegate of the getter, a store outside PlayMaker.dll) leaves the game unpatched, with the place named. Not covered: writes by reflection or through native code into a live FSM (Unity's serializer only fills new objects, whose counts are 0 and whose mark is 0, so their first reset runs whole). |
| `playmaker-delayedevents` | PlayMaker's `Fsm.UpdateDelayedEvents` (every Update of every FSM) clears its two private work lists, copies the FSM's delayed events into one of them, updates them and removes the finished ones. With no delayed events and both work lists empty, all that changes are the lists' version numbers, which only an enumerator of those lists could read: the patch checks that nothing but this method and the constructors (which set them to new lists) ever touches them, and the method then returns at once. A missing `delayedEvents` list takes the original path (the same exception). |

Tested on a stand-in PlayMaker written for the tests (`test/fakes/PlayMaker`, built by `build.sh
test`): 60 seeded random sequences of 400 steps (state changes, states of other FSMs entered,
states put into other FSMs' arrays, new arrays, copies, the loop guard tripping, delayed events that
add, clear or change state while they are updated, a missing list) and a directed one show the
same counts, events and exceptions after every step patched and unpatched; game assemblies that
write into a state array, pass it on, keep it in a field or write through their own wrapper of the
getter leave it unpatched. The patches find the same shapes in Hollow Knight's PlayMaker.dll and in
the free corpus's other PlayMaker game. On the RG34XX-SP, Hollow Knight's King's Pass with the GPU's
work left out: +3.1 % (32-bit), +3.9 % (64-bit, Mono route); in game it is GPU-bound and runs the same
per MHz of the GPU's clock, with the same pictures ([docs/profiling.md](../../docs/profiling.md#playmakers-per-frame-work-asmpatch-optimizations)).

## Reductions

For handhelds with little memory and a slow GPU (the RG34XX-SP: 1 GB, a Mali-G31), a smaller picture
where the game's own options do not reach as far, never other game play: no timing, physics,
collision or script change. Always opt-in (`--with=<patch>`), and even then the game is as it was
until an environment variable asks for the reduction, so a port can turn it on per handheld (port.toml
`runtime_lowmem`, docs/porting.md) and a player can turn it off in `port.env`.

| patch | what |
|---|---|
| `hk-blur` | Hollow Knight's far background: the game draws it with a camera of its own (`LightBlurredBackground`) into a 576x360 texture, blurs it in four passes (`LightBlur`; two with the game's own Shader Quality "Low") and shows it on the blur planes. `ASMPATCH_HK_BLUR` (comma-separated words): `low`, two passes, as the game's own Low; `half`, the texture at half the height and width (288x180: the same passes over texels twice as large, blurrier, the same layout); `half,low`, both; `off`, no background camera and no blur, the blur planes' renderers off, the scene's camera draws the background itself, sharp (the look of Destalta's [NoBlur](https://github.com/Destalta/NoBlur) mod; here `LightBlurredBackground` does nothing instead of GameObjects being deactivated by name). Anything else, or unset: the game as it is. Read once, when the camera rig starts (`LightBlurredBackground.Awake`, `BlurPlane.Awake`, the texture's size at each aspect change, `LightBlur.BlurPassCount`). Measured in [docs/profiling.md](../../docs/profiling.md#hollow-knights-blur-and-particles-on-1-gb-handhelds). Tested on a stand-in for those classes (`test/fakes/HollowKnight`): each word and a few that are not, patched and unpatched. |

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

### The path analysis

`PathFlow` (src/Patches/StreamingAssets.cs) labels every read of the folder getters a desktop game
uses (`Application.streamingAssetsPath`, `dataPath`, `persistentDataPath`, `temporaryCachePath`,
the working directory, `AppDomain.BaseDirectory`, `Assembly.Location`) and follows the labels
forward through the IL of every assembly but the class libraries and Unity's engine modules: the
stack, locals and arguments, fields (static and instance alike, by name), return values (also of
virtual methods, through the overrides in the analysed code, and of methods made into delegates),
array elements, collections and builders a labelled value was added to, `out` arguments, and every
call outside the analysed code that returns a string, a path object, a collection or an object
(`string.Concat`, `Path.Combine`, `Directory.GetFiles`, ...). A sink is a call with a labelled
argument (or a labelled `FileInfo`/`DirectoryInfo`): System.IO's file and directory types, XML
(`XmlDocument`, `XmlReader`, `XDocument`) and SQLite connections by path, any P/Invoke; Unity's
AssetBundle, UnityWebRequest, WWW and VideoPlayer calls are recorded apart. It over-approximates
(context-insensitive, fields by name), so a read may be redirected where it did not need to be;
reaching a sink it does not see (reflection, a delegate's arguments) leaves a read on the jar path.

## --survey

Which input stacks the game's own assemblies use (not Unity's or the class libraries): Rewired
(edition, compiled platform), InControl, the Input System package, the legacy `UnityEngine.Input`
calls by kind (axes/buttons, keys, mouse, touch), `OnMouse*` messages, UI pointer handlers,
whether code selects UI elements (pad/keyboard menu navigation), InControl's Android device
profiles and the pad names they match (so a port can set `UA_PAD_NAME` to one), and text entry (`InputField`,
`TMP_InputField`, `TouchScreenKeyboard`: the soft keyboard, which ua_host's on-screen keyboard
answers), pointer locks (`Cursor.lockState = Locked`) and UI hit tests of the game's own
(`BaseRaycaster.Raycast`, `EventSystem.RaycastAll` outside an input module, in a method that does
not read the mouse's or a touch's position). Library code compiled into the game's assemblies is
counted apart, as it says nothing about how the game's menus work: Rewired's source edition
(namespace `Rewired`: its UI input module, demos, control mapper) and the scripts assets ship in
`Demo(s)`/`Example(s)`/`Sample(s)` namespaces; a Rewired UI input module in the build gets a note. The verdict line says whether the menus are likely pointer-only (then the port needs
ua_host's pointer mode, `UA_POINTER=1`, see docs/input.md), or whether the game likely has a
cursor of its own (a pointer lock and a UI hit test: it moves the cursor by mouse motion, so
pointer mode's position and touches do not steer it; docs/input.md, "Games with their own
cursor").

Then the file access section: for each folder getter the game reads, how many reads there are, how
many reach System.IO or a native library (and whether they write), the APIs they reach, Unity's
APIs they reach, and where the reads are. `-> StreamingAssets read with System.IO` means the port
wants `android_data.streaming_assets` (docs/android-data.md, "StreamingAssets"); reads of
`dataPath` reaching System.IO are worth a look (on Android that is the APK or OBB file).

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
patched, loaded into the test process and called; a fake `Unity.InputSystem` (an
`InputSystem` whose plugin initialization returns early or not, a logging `RegisterLayout` and a
decoy overload; no package code) is patched and called, and the layouts it registers are parsed
and checked control by control; plus the not-applicable, already-applied, failing and
command-line cases. The survey runs on fake games (references to Unity's types only): one with a
cursor of its own beside Rewired's UI input module, Rewired's demos, an asset's examples, an input
module of its own and a hit test at the mouse's position (none of which counts), one that locks
the pointer for mouse look and hit-tests at the mouse's position, and a pointer-driven one whose
only UI selection calls are Rewired's. For streaming-assets and data-path: a fake
`UnityEngine.CoreModule` (Application getters that return settable values, an AssetBundle and a UnityWebRequest that record
the path they get) and a game whose paths go straight into System.IO, through a property and a
helper, a static field, an array, a virtual method, a delegate, into Unity's APIs only, and into
both; the patched game runs against a `jar:file://` path (and an OBB dataPath) whose folder has the
files and against desktop paths (same results as unpatched), and the helpers against odd paths.
The optimizations and reductions are tested on stand-ins written in C# for the tests (`test/fakes/`,
built by `build.sh test` with the .NET SDK, no game or library code): a PlayMaker whose patched and
unpatched copies are driven through the same seeded and directed sequences and must agree after
every step (["Optimizations"](#optimizations)), and Hollow Knight's blur classes (["Reductions"](#reductions)).
