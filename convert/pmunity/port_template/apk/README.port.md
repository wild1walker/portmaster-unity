# @@TITLE@@ (PortMaster port)

Made with portmaster-unity's `pmunity port new` (@@ROUTE@@ route: @@ROUTE_TEXT@@). Porter:
@@PORTER@@.

| file | what |
|---|---|
| `port.toml` | the port's settings: edit, then `python -m pmunity port new @@PORTDIR@@ --data <the game's APK>` again |
| `port.build.json` | everything resolved from port.toml and the game check (written by `port new`) |
| `@@LAUNCHER@@` | the launcher (PortMaster conventions; the first launch runs the setup; then `ua_host` or `ua_host64`, by the player the setup installed) |
| `@@PORTDIR@@/` | the folder installed as `ports/@@PORTDIR@@/`: `setup/patchscript` (the setup: `pmunity apk`), `setup/setup.env`, `port.env` (ua_host's options), `port.json`, READMEs |
| `package.sh` | builds the release zip (`out/@@ZIP@@`) with portmaster-unity's packager: `ua_host` for armeabi-v7a, `ua_host64` for arm64-v8a, as `player.abis` in port.toml names them |

## Notes

The player provides @@GAME_WHAT@@; nothing of the game or of Unity is in
this port. The first launch installs the game's own Android build on the handheld
(@@SETUP_TIME@@; no download, no conversion).

## Controls

The handheld's pad reaches the game as an Android gamepad. Port-specific options (pointer mode
for touch or mouse menus, pad buttons that press keys) are in `@@PORTDIR@@/port.env`.

## Building

`./package.sh` (needs a portmaster-unity checkout with its runtime built, both architectures for a
build with both players: `PMUNITY=<checkout>`). Test the setup on a device, or in an arm64
container as docs/porting.md in portmaster-unity describes (`REHEARSE_64ONLY=1` for a 64-bit-only
system).
