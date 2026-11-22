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

The port folder is made by portmaster-unity's `pmunity port new` from the port's settings,
`port.toml`. On Linux or macOS with git, Docker and Python 3.11 or newer:

```sh
git clone --recurse-submodules https://github.com/wild1walker/portmaster-unity.git
cd portmaster-unity
@@BUILD_STEPS@@
python3 -m pip install -r convert/requirements.txt
cd convert
python3 -m pmunity port new @@PORTDIR@@ --out <port folder>   # reads <port folder>/port.toml
python3 -m pmunity port package <port folder>                  # -> <port folder>/out/@@ZIP@@
```
