## Notes

@@THANKS@@

This port does not include the game: you need @@GAME_WHAT@@. Copy the game's Linux files into
`ports/@@PORTDIR@@/gamedata/`, then start @@TITLE@@ from the Ports menu. The first launch finds the
game and, for its OpenGL ES mode, adds OpenGL ES shaders next to its own (@@SETUP_TIME@@, once); the
game's code and pictures stay as they are. @@ROUTE_NEEDS@@

The game runs as its own Linux build, unmodified, under [box64](https://github.com/ptitSeb/box64),
set up by [portmaster-unity](https://github.com/wild1walker/portmaster-unity)'s box64 route. Saves
and settings are in `ports/@@PORTDIR@@/conf/`; options (how the game draws, box64's settings) are
in `ports/@@PORTDIR@@/port.env`.

## Controls

@@CONTROLS@@

## Compile

The port folder is made by portmaster-unity's `pmu port new` from the port's settings,
`port.toml`. On Linux or macOS with git and Docker (pmu runs in Docker: setup-rs/tools/pmu.sh):

```sh
git clone --recurse-submodules https://github.com/wild1walker/portmaster-unity.git
cd portmaster-unity
@@BUILD_STEPS@@
setup-rs/tools/pmu.sh port new @@PORTDIR@@ --out <port folder>   # reads <port folder>/port.toml
setup-rs/tools/pmu.sh port package <port folder>   # -> <port folder>/out/@@ZIP@@
```
