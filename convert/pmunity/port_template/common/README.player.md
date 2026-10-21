# @@TITLE@@ on PortMaster

@@TITLE@@ runs on Unity's own Android player, run natively on the handheld's 32-bit userland by
[portmaster-unity](https://github.com/wild1walker/portmaster-unity)'s `ua_host`. No game files come
with this port: you provide your own copy, and the first launch builds the game from it on the
handheld.

## What you need

- **@@GAME_WHAT@@.** The setup checks the Unity version it was built with (this port is made
  for @@UNITY_VERSIONS@@).
- Wi-Fi on the first launch, to download Unity's Android player of that version from Unity (about
  10 MB of its installer). Or put `UnitySetup-Android-Support-for-Editor-<version>.exe` from Unity's
  download archive into `gamedata/` yourself.
- About @@SIZE_GB@@ GB free on the card during the setup.
- A handheld with a 32-bit (armhf) userland and GLES 3 (e.g. Knulli on Allwinner H700 devices),
  and a 64-bit (aarch64) system for the setup's tools.

## Setting it up

1. Copy the game (its folder, the archive it came in, or GOG's Linux installer `.sh`) into
   `ports/@@PORTDIR@@/gamedata/`.
2. Launch @@TITLE@@ from the Ports menu. PortMaster's patcher shows the setup's progress: it finds
   (or unpacks) the game, downloads Unity's player, converts the game for the handheld's GPU and
   packs it for the player (@@SETUP_TIME@@). If it is interrupted, launch again: it continues
   where it stopped. The log is `ports/@@PORTDIR@@/patchlog.txt`.
3. When it is done the game starts. The setup uses up the game's folder in `gamedata/` (its files
   are moved into the converted game); archives you put there can be deleted afterwards.

Saves and settings: `ports/@@PORTDIR@@/data/`. PortMaster's exit hotkey quits. The game's options
(pointer mode, keys, ...) are in `ports/@@PORTDIR@@/port.env`; `log.txt` is the last run's log.

## Licences

The port's code is MIT (`licenses/`). The setup tools are removed when the setup is done; their
licences are in `licenses/` too. Unity is a trademark of Unity Technologies; this port is not
affiliated with Unity Technologies or the game's makers.
