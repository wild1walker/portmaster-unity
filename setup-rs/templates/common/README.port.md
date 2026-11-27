## Notes

@@THANKS@@

This port does not include the game: you need @@GAME_WHAT@@. Copy it into
`ports/@@PORTDIR@@/gamedata/`, then start @@TITLE@@ from the Ports menu. The first launch builds the
game on the handheld from your copy (@@SETUP_TIME@@, once). @@PLAYER_FROM@@ @@ROUTE_NEEDS@@

The game runs on Unity's own Android player, through
[portmaster-unity](https://github.com/wild1walker/portmaster-unity)'s `ua_host` (its @@ROUTE@@
route). Saves and settings are in `ports/@@PORTDIR@@/data/`; the game's options are in
`ports/@@PORTDIR@@/port.env`.

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
DONORS=<donor folder> setup-rs/tools/pmu.sh port package <port folder>   # -> <port folder>/out/@@ZIP@@
```
