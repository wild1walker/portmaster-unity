## Notes

@@THANKS@@

This port does not include the game: you need @@GAME_WHAT@@. Copy it into
`ports/@@PORTDIR@@/gamedata/`, then start @@TITLE@@ from the Ports menu. The first launch installs
the game's own Android build from your copy (@@SETUP_TIME@@, once); nothing is downloaded and
nothing of the game is converted. @@ROUTE_NEEDS@@

The game runs on the Unity player inside its own APK, through
[portmaster-unity](https://github.com/wild1walker/portmaster-unity)'s `ua_host` (its @@ROUTE@@
route): the APK's 32-bit (armeabi-v7a) player with `ua_host` where the handheld has a 32-bit
userland, its 64-bit (arm64-v8a) player with `ua_host64` elsewhere. The game's Java code (Google
Play services, ads, purchases) does not run. Saves and settings are in `ports/@@PORTDIR@@/data/`;
the game's options are in `ports/@@PORTDIR@@/port.env`.

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
