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

# @@TITLE@@ on Unity's 64-bit (arm64) IL2CPP Android player, run natively by portmaster-unity's
# ua_host, for systems without a 32-bit userland. On the first launch the setup builds the game from
# your copy in ports/@@PORTDIR@@/gamedata/, compiling its code with Unity's IL2CPP on the handheld
# (see README.md there). Made with `pmunity port new` (@@ROUTE@@ route).

GAMEDIR="/$directory/ports/@@PORTDIR@@"
cd "$GAMEDIR" || exit 1
mv -f "$GAMEDIR/log.txt" "$GAMEDIR/log.prev.txt" 2>/dev/null
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
echo "@@TITLE@@: $CFW_NAME on $DEVICE_NAME ($DEVICE_ARCH), ${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}, RAM $(awk '/MemTotal/{print int($2/1024)" MB"}' /proc/meminfo)"

# ua_host and the game are 64-bit ARM programs
if [ "$(uname -m)" != aarch64 ]; then
  pm_message "@@TITLE@@ needs a 64-bit ARM (aarch64) system (this one: $(uname -m))."
  sleep 8
  pm_finish
  exit 1
fi

# ---- first launch: build the game from your copy (PortMaster's patcher shows the progress)
if [ ! -f "$GAMEDIR/@@READY@@" ]; then
  if [ -f "$GAMEDIR/setup/patchscript" ] && [ -f "$controlfolder/utils/patcher.txt" ]; then
    export GAMEDIR DEVICE_ARCH controlfolder
    chmod +x "$GAMEDIR/setup/patchscript"
    export PATCHER_FILE="$GAMEDIR/setup/patchscript"
    export PATCHER_GAME="@@TITLE@@"
    export PATCHER_TIME="@@SETUP_TIME@@, once"
    [ -f "$GAMEDIR/setup/questions.lua" ] && export PATCHER_QUESTIONS="$GAMEDIR/setup/questions.lua"
    source "$controlfolder/utils/patcher.txt"
  fi
  if [ ! -f "$GAMEDIR/@@READY@@" ]; then
    pm_message "@@TITLE@@ is not set up yet: put @@GAME_WHAT@@ in ports/@@PORTDIR@@/gamedata and launch again (ports/@@PORTDIR@@/README.md; log: ports/@@PORTDIR@@/patchlog.txt)."
    sleep 8
    pm_finish
    exit 1
  fi
fi

# the built-in pad reaches the game as an Android gamepad (ua_host: SDL GameController ->
# KeyEvent and MotionEvent)
export SDL_GAMECONTROLLERCONFIG="${SDL_GAMECONTROLLERCONFIG:-$sdl_controllerconfig}"
# audio: SDL's own choice (a 64-bit program reaches the system's sound server or ALSA itself)
export UA_FPS=${UA_FPS:-10}                     # a line of frame statistics every 10 s in log.txt
# the game's options (port.env), and the setup's answers that apply when the game runs
[ -f "$GAMEDIR/port.env" ] && source "$GAMEDIR/port.env"
[ -f "$GAMEDIR/answers.env" ] && source "$GAMEDIR/answers.env"
chmod +x "$GAMEDIR/ua_host"

$GPTOKEYB2 "ua_host" -c "$GAMEDIR/@@PORTDIR@@.ini" &   # exit hotkey only
pm_platform_helper "$GAMEDIR/ua_host"

./ua_host "$GAMEDIR"
echo "game exited ($?)"

pm_finish
