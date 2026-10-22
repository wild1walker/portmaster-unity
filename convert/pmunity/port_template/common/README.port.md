## Notes

@@THANKS@@

This port does not include the game: you need @@GAME_WHAT@@. Copy it into
`ports/@@PORTDIR@@/gamedata/`, then start @@TITLE@@ from the Ports menu. The first launch builds the
game on the handheld from your copy (@@SETUP_TIME@@, once) and downloads Unity's Android player for
it from Unity (about 10 MB, Wi-Fi). @@ROUTE_NEEDS@@

The game runs on Unity's own Android player, through
[portmaster-unity](https://github.com/wild1walker/portmaster-unity)'s `ua_host` (its @@ROUTE@@
route). Saves and settings are in `ports/@@PORTDIR@@/data/`; the game's options are in
`ports/@@PORTDIR@@/port.env`.

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
