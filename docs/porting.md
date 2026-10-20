# Porting a Unity game

The runtime runs Unity's ARMv7 Android player; the converter makes a desktop build's data look
like an Android build's. What a game needs beyond that depends on the game. This is the order that
worked for Cuphead.

## 1. Check the game

- **Unity version**: the first line of `strings <Data>/globalgamemanagers | head` (e.g.
  `2017.4.9f1`). You need the Android Build Support installer of exactly that version. The runtime
  was built against 2017.4 (`libunity.so`'s imports, Java side and JNI calls); other versions may
  import or call things it does not provide yet: `tools/re/elfimports.py` lists a player's imports
  (compare with `runtime/android/README.md`), and running with `FJ_DUMP=1` lists the Java classes
  and methods Unity asked for and did not find (`UNKNOWN`).
- **Scripting backend**: Mono (`<Data>/Managed/*.dll` holds the game's code). IL2CPP games (a
  `GameAssembly` library, no `Managed/`) compile the game to native code for one platform; their
  desktop code cannot run in the Android player.
- **Graphics**: the macOS build's shaders include OpenGL Core programs, which `pmunity shaders`
  translates. A Linux build may carry GLCore too. Windows builds have only Direct3D bytecode.
- **Memory**: the handheld has little RAM (the RG34XX-SP: 1 GB with the system). The texture step
  can scale textures down (`textures.scale`, e.g. 0.375 for 1080p art on a 480-line screen).

## 2. Convert

A config file (JSON) for `python -m pmunity convert <Data> [<out>] --config game.json`:

```json
{
  "steps": ["shaders", "settings", "textures", "audio"],
  "shaders": {"precision": "highp", "bundles": "."},
  "settings": {"graphics_apis": [11], "mouse_events": true},
  "textures": {"scale": 1.0, "block": [4, 4], "quality": "medium"},
  "audio": {"compressed_min_s": 2.0}
}
```

Each step's settings are its module's `DEFAULTS` (convert/pmunity/textures.py, settings.py,
audio.py; shaders/step.py). Only turn `mouse_events` off for a game that has no `OnMouse*`
method (search its assemblies). A game's own conversion steps go in a plugin
(`--plugin steps.py` with `STEPS = {"name": function(job)}`; portmaster-cuphead's
`convert/animclips.py` is one).

Then check the shaders on the device: `python -m pmunity shaders-export <converted Data> glsl/`,
copy `glsl/` and `runtime/host/out/shadercheck` to the handheld, run
`LD_LIBRARY_PATH=/lib32:/usr/lib32 ./shadercheck glsl` (docs/shaders.md).

## 3. Package

```sh
python -m pmunity android-data --src <converted Data> --pkg <player package> --out android --pack all \
    [--name Game] [--package com.example.game] [--mono-etc <Game>.app/Contents/Mono/etc]
```

For a macOS build pass `--mono-etc` (Mono's config is outside the Data folder there; its `.dylib`
names are turned into Linux ones). The tool sets what an Android build would have had (platform,
graphics API, multithreaded-rendering flag, texture pixels inline, Rewired's Unity-input axes if
the game uses Rewired, an APK and an OBB tied by a build id); docs/android-data.md explains each.

## 4. Run

On the handheld, in one folder: `ua_host`, `lib/` with the player's `libmain.so`, `libunity.so`,
`libmono.so` (`Variations/mono/Release/...`), and the contents of `android/zip/`. Then
`tools/device/run.sh first.log 60` (silent, 60 s) and read the log: Unity's own messages
(`I/Unity`), `[host]` lines, the frame statistics (`UA_FPS=5`). The Development player
(`Variations/mono/Development`, as `devlib/`, `run.sh <log> <secs> dev`) logs much more.

Typical first problems and where to look:

| symptom | look at |
|---|---|
| exits during load with an unresolved symbol | the loader's `[so]` lines; add the function to `runtime/loader/hybris_compat.c` or an android shim |
| Java class or method missing | `FJ_DUMP=1` / `FJ_TRACE=1`; implement it in `runtime/jni/up_android.c` |
| black screen, shaders failing | `shadercheck`, `UA_SHADER_DUMP`, Unity's `GLSL` errors in the log |
| no pad | `SDL_GAMECONTROLLERCONFIG` (PortMaster's `get_controls`), `UA_PAD_NAME` (Rewired and InControl match Android pads by name; the survey lists InControl's names); `asmpatch --survey <Managed>` names the game's input stacks, `--asmpatch` (Rewired: `rewired-android`; the Input System package: `inputsystem-android`) and docs/input.md say what each needs |
| a name or text field waits for typing | nothing: the on-screen keyboard opens when the game asks for one; `UA_OSK=done` answers it at once (docs/input.md) |
| menus need a mouse, controls need keys | pointer mode (`UA_POINTER=1`) and `UA_KEYMAP` (docs/input.md) |
| no sound | `ANDROID_SLES_OUTPUT=pipe` when a sound server holds the card |
| hitches | `UA_HITCH_TRACE=50` + `tools/analysis/hitchsum.py`; `UA_GC_HEAP` for Mono's collector |

## 5. What may need a game's own code

Desktop builds sometimes assume a desktop: a store client (GOG Galaxy, Steamworks) the game waits
for, DLC found through the store, input set up for keyboards and desktop pads, a resolution or
aspect that does not fit the screen. Generic ones are in `tools/asmpatch` (`--asmpatch`: e.g.
Rewired on its Android platform); a game's own go in its port, as patches to its assemblies
(`--assembly-patch`, run on the original assembly at packaging time) and a helper assembly
(`--managed-file`), as portmaster-cuphead does with Mono.Cecil. Keep the game's look and feel:
changes that alter the picture or the game play belong behind an option, off by default.
