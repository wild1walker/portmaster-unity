# @@TITLE@@ on PortMaster

@@TITLE@@'s own Android build, run natively on the handheld by
[portmaster-unity](https://github.com/wild1walker/portmaster-unity)'s `ua_host`: Unity's Android
player inside the game's APK, without Android. No game files come with this port: you provide
your own copy, and the first launch installs it.

## What you need

- **@@GAME_WHAT@@.** The setup checks the game's package and the Unity version it was built with
  (this port is made for @@UNITY_VERSIONS@@).
- About as much free space on the card as the game takes, during the setup (on exFAT and FAT
  cards the files are copied; on ext4 they are linked).
- A 64-bit (aarch64) handheld with GLES 3. The game's 32-bit (armeabi-v7a) player runs where the
  system has a 32-bit (armhf) userland (e.g. Knulli), its 64-bit (arm64-v8a) player everywhere
  else (e.g. ROCKNIX), as far as the APK has them.

No Wi-Fi: nothing is downloaded and nothing of the game is converted.

## Setting it up

1. Copy the game into `ports/@@PORTDIR@@/gamedata/`: the `.apk` with the `.obb` files that came
   with it (from `Android/obb/<package>/` on a phone, if it has any), the split APKs of an install
   (`base.apk`, `split_config.*.apk`), or the `.xapk`, `.apks` or `.aab` file.
2. Launch @@TITLE@@ from the Ports menu. PortMaster's patcher shows the setup's progress: it checks
   the game, takes the player that fits the system and installs it (@@SETUP_TIME@@). If it is
   interrupted, launch again: it continues where it stopped. The log is
   `ports/@@PORTDIR@@/patchlog.txt`.
3. When it is done the game starts. The setup deletes the files you put in `gamedata/` once the
   game is installed (`keep_installers=1` in `ports/@@PORTDIR@@/setup.txt` keeps them).

Saves and settings: `ports/@@PORTDIR@@/data/`. PortMaster's exit hotkey quits. The game's options
(pointer mode for touch or mouse menus, keys, ...) are in `ports/@@PORTDIR@@/port.env`; `log.txt`
is the last run's log.

The game's Java code does not run: Google Play services, ads, purchases and other Java SDKs see a
phone without them. Games made only for touch need the pointer mode (`UA_POINTER=touch` in
`port.env`).

Moved the card to a handheld without a 32-bit userland after a 32-bit setup? Install the port
again, delete `ports/@@PORTDIR@@/@@READY@@` and launch: the setup takes the installed APK again,
with its 64-bit player.

## Controls

@@CONTROLS@@

## Licences

The port's code is MIT (`licenses/`). The setup tools are removed when the setup is done; their
licences are in `licenses/` too. Unity is a trademark of Unity Technologies; this port is not
affiliated with Unity Technologies or the game's makers.
