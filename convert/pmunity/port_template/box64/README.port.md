# @@TITLE@@ (PortMaster port)

Made with portmaster-unity's `pmunity port new` (@@ROUTE@@ route: @@ROUTE_TEXT@@). Porter:
@@PORTER@@.

| file | what |
|---|---|
| `port.toml` | the port's settings: edit, then `python -m pmunity port new @@PORTDIR@@ --data <game> --route box64` again |
| `port.build.json` | everything resolved from port.toml and the game check (written by `port new`) |
| `@@LAUNCHER@@` | the launcher (PortMaster conventions; the first launch runs the setup) |
| `@@PORTDIR@@/` | the folder installed as `ports/@@PORTDIR@@/`: `setup/patchscript` (the setup), `setup/gles.json` (its shader conversion), `port.env` (the options), `port.json`, READMEs |
| `package.sh` | builds the release zip (`out/@@ZIP@@`): box64, glxes, gl4es (portmaster-unity's `tools/box64/build.sh` output), the converter and the setup's Python |

## Notes

The player provides @@GAME_WHAT@@; nothing of the game or of Unity is in
this port. The game runs unmodified under box64; in its OpenGL ES mode (the default) the setup adds
GLES 3 shader programs to the player's copy (docs/box64-route.md in portmaster-unity).

## Controls

The game's own SDL2 reads the pad (PortMaster's `SDL_GAMECONTROLLERCONFIG`); `@@PORTDIR@@.gptk`
(gptokeyb) can give keys and mouse to games that want them, and holds the exit hotkey.

## Building

`./package.sh` (needs a portmaster-unity checkout with `tools/box64/build.sh` run:
`PMUNITY=<checkout>`). Test the launcher in the box64 rig: `tools/box64/test/launcher_test.sh`.
