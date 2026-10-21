# tools/box64: the box64 route

A game's own Linux x86_64 build under box64 ([docs/box64-route.md](../../docs/box64-route.md)).
`python -m pmunity route <game>` (convert/pmunity/route.py) says when this route is the one.

| file | what |
|---|---|
| `build.sh` | box64 (v0.4.4) and gl4es (v1.1.6) from their release tags, and glxes, for aarch64 in Docker (Debian 11) -> `out/` |
| `glxes/glxes.c` | GLX for OpenGL ES contexts on EGL: lets Unity's Linux player run its `-force-gles` mode on GLES-only drivers |
| `gles.json` | `pmunity convert` config of the GLES mode (GLES 3 shader programs, GLES 3 in the graphics API list) |
| `box64.env` | box64 settings for Unity's players: `box64_profile safe\|default\|fast` |
| `unityprefs.sh` | Unity's PlayerPrefs file from the shell (the screen keys) |
| `../../convert/pmunity/port_template/box64/` | the route's port templates: launcher, setup, READMEs (`pmunity port new --route box64`; `pmunity route --launcher`) |
| `rig.sh`, `rig_inside.sh`, `rigstats.py`, `docker/Dockerfile.rig` | emulated handheld: the game under box64 in an arm64 container with Xvfb and Mesa; screenshots, fps, CPU per thread |
| `test/port_test.sh`, `test/launcher_test.sh`, `test/mockpm/` | a release zip (setup and game) or a launcher run as on a handheld, with a stand-in PortMaster |
