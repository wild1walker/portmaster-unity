#!/bin/bash
# PORTMASTER: @@ZIP@@, @@LAUNCHER@@

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
get_controls

# @@TITLE@@'s own Android build, run natively by portmaster-unity's ua_host: its 32-bit
# (armeabi-v7a) player with ua_host where the system has a 32-bit userland, its 64-bit (arm64-v8a)
# one with ua_host64 elsewhere. On the first launch the setup installs your copy from
# ports/@@PORTDIR@@/gamedata/ (see README.md there). Made with `pmunity port new` (@@ROUTE@@ route).

GAMEDIR="/$directory/ports/@@PORTDIR@@"
cd "$GAMEDIR" || exit 1
mv -f "$GAMEDIR/log.txt" "$GAMEDIR/log.prev.txt" 2>/dev/null
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
echo "@@TITLE@@: $CFW_NAME on $DEVICE_NAME ($DEVICE_ARCH), ${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}, RAM $(awk '/MemTotal/{print int($2/1024)" MB"}' /proc/meminfo)"

# the setup's tools and the 64-bit player are 64-bit ARM programs
if [ "$(uname -m)" != aarch64 ]; then
  pm_message "@@TITLE@@ needs a 64-bit ARM (aarch64) system (this one: $(uname -m))."
  sleep 8
  pm_finish
  exit 1
fi

# ---- first launch: install the game from your copy (PortMaster's patcher shows the progress)
if [ ! -f "$GAMEDIR/@@READY@@" ]; then
  if [ -f "$GAMEDIR/setup/patchscript" ] && [ -f "$controlfolder/utils/patcher.txt" ]; then
    export GAMEDIR DEVICE_ARCH controlfolder
    chmod +x "$GAMEDIR/setup/patchscript"
    export PATCHER_FILE="$GAMEDIR/setup/patchscript"
    export PATCHER_GAME="@@TITLE@@"
    export PATCHER_TIME="@@SETUP_TIME@@, once"
    [ -f "$GAMEDIR/setup/questions.lua" ] && export PATCHER_QUESTIONS="$GAMEDIR/setup/questions.lua"
    source "$controlfolder/utils/patcher.txt"
    $ESUDO kill -9 $(pidof gptokeyb gptokeyb2) 2>/dev/null
  fi
  if [ ! -f "$GAMEDIR/@@READY@@" ]; then
    pm_message "@@TITLE@@ is not set up yet: put @@GAME_WHAT@@ in ports/@@PORTDIR@@/gamedata and launch again (ports/@@PORTDIR@@/README.md; log: ports/@@PORTDIR@@/patchlog.txt)."
    sleep 8
    pm_finish
    exit 1
  fi
fi

# which of the game's players the setup installed (android_package.json: "abi"), and so which ua_host
ABI=$(sed -n 's/^ *"abi": *"\([^"]*\)".*/\1/p' "$GAMEDIR/android_package.json" 2>/dev/null | head -1)
if [ "$ABI" = arm64-v8a ]; then
  UA_HOST=ua_host64
else
  UA_HOST=ua_host
  # the 32-bit player needs the 32-bit userland (e.g. Knulli's /lib32): a card moved to a system
  # without one needs the setup again, for the game's 64-bit player (it takes the installed APK)
  if [ ! -e /lib/ld-linux-armhf.so.3 ]; then
    pm_message "@@TITLE@@ was set up for its 32-bit player, and this system has no 32-bit (armhf) userland. To set it up for this one: install the port again, delete ports/@@PORTDIR@@/@@READY@@ and launch."
    sleep 8
    pm_finish
    exit 1
  fi
fi
echo "the game's $ABI player, $UA_HOST"

# the built-in pad reaches the game as an Android gamepad (ua_host: SDL GameController ->
# KeyEvent and MotionEvent)
export SDL_GAMECONTROLLERCONFIG="${SDL_GAMECONTROLLERCONFIG:-$sdl_controllerconfig}"
# audio: SDL's own choice, else (no device SDL can open, e.g. a sound server without the client
# libraries of a 32-bit program) raw PCM piped into pw-cat / pacat / aplay (ANDROID_SLES_OUTPUT)
# frame pacing: the Android build's own (ua_host keeps UA_FRAME_RATE=android for imported builds)
export UA_FPS=${UA_FPS:-10}                     # a line of frame statistics every 10 s in log.txt
# the game's options (port.env), and the setup's answers that apply when the game runs
[ -f "$GAMEDIR/port.env" ] && source "$GAMEDIR/port.env"
[ -f "$GAMEDIR/answers.env" ] && source "$GAMEDIR/answers.env"
chmod +x "$GAMEDIR/$UA_HOST"

$GPTOKEYB2 "$UA_HOST" -c "$GAMEDIR/@@PORTDIR@@.ini" &   # exit hotkey only
pm_platform_helper "$GAMEDIR/$UA_HOST"

if [ "$UA_HOST" = ua_host ]; then
  # the 32-bit libraries first (Knulli: /lib32, /usr/lib32)
  LD_LIBRARY_PATH=/lib32:/usr/lib32${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} ./ua_host "$GAMEDIR"
else
  ./ua_host64 "$GAMEDIR"
fi
echo "game exited ($?)"

$ESUDO kill -9 $(pidof gptokeyb gptokeyb2) 2>/dev/null
pm_finish
