# @@TITLE@@ on PortMaster

@@TITLE@@ runs as the game's own Linux build, unmodified, under
[box64](https://github.com/ptitSeb/box64) (x86-64 code translated for the handheld's ARM CPU), set up
by [portmaster-unity](https://github.com/wild1walker/portmaster-unity)'s box64 route. No game files
come with this port: you provide your own copy.

## What you need

- **@@GAME_WHAT@@** (this port is made for Unity @@UNITY_VERSIONS@@).
- A 64-bit (aarch64) handheld with OpenGL ES 3 (or desktop OpenGL), and PortMaster's Westonpack
  runtime (PortMaster downloads it on the first launch).
- About @@SIZE_GB@@ GB free on the card.

## Setting it up

1. Copy the game's Linux files (the folder with the executable and its `_Data` folder, or the
   archive they came in) into `ports/@@PORTDIR@@/gamedata/`.
2. Launch @@TITLE@@ from the Ports menu. The first launch finds the game and adds OpenGL ES shaders
   next to its own (@@SETUP_TIME@@); the game's code and pictures stay as they are. If it is
   interrupted, launch again. The log is `ports/@@PORTDIR@@/patchlog.txt`.

Saves and settings: `ports/@@PORTDIR@@/conf/`. PortMaster's exit hotkey quits. Options
(`ports/@@PORTDIR@@/port.env`): `PMU_GL=gles|mesa|gl4es` (how the game draws), `PMU_BOX64=safe|default|fast`
(box64 settings; `fast` is quicker but can be unstable), `PMU_MAXCPU`, `PMU_ARGS` (more arguments for
the game), `PMU_DYNACACHE=0`. `log.txt` and `player.log` are the last run's logs.

## Licences

The port's code is MIT; box64 and gl4es are MIT, box64's x86-64 C++ runtime is GPL with the GCC
Runtime Library Exception (`licenses/`). Unity is a trademark of Unity Technologies; this port is
not affiliated with Unity Technologies or the game's makers.
