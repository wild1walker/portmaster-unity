# @@TITLE@@ (PortMaster port)

Made with portmaster-unity's `pmunity port new` (@@ROUTE@@ route: @@ROUTE_TEXT@@). Porter:
@@PORTER@@.

| file | what |
|---|---|
| `port.toml` | the port's settings: edit, then `python -m pmunity port new @@PORTDIR@@ --data <game>` again |
| `port.build.json` | everything resolved from port.toml and the game check (written by `port new`) |
| `@@LAUNCHER@@` | the launcher (PortMaster conventions; the first launch runs the setup) |
| `@@PORTDIR@@/` | the folder installed as `ports/@@PORTDIR@@/`: `setup/patchscript` (the setup), `setup/setup.env`, `setup/convert.json`, `port.env` (ua_host's options), `port.json`, READMEs |
| `package.sh` | builds the release zip (`out/@@ZIP@@`) with portmaster-unity's packager |

## Notes

The player provides @@GAME_WHAT@@; nothing of the game or of Unity is in
this port. The first launch builds the game on the handheld (@@SETUP_TIME@@).

## Controls

The handheld's pad reaches the game as an Android gamepad. Port-specific options (pointer mode
for mouse-driven menus, pad buttons that press keys) are in `@@PORTDIR@@/port.env`.

## Building

`./package.sh` (needs a portmaster-unity checkout with its runtime built: `PMUNITY=<checkout>`).
Test the setup on a device, or in an arm64 container as docs/porting.md in portmaster-unity
describes.
