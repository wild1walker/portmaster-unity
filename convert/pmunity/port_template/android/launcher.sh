#!/bin/bash
# PORTMASTER: @@ZIP@@, @@LAUNCHER@@
#
# @@TITLE@@ on Unity's own Android player, run natively on the handheld's 32-bit userland by
# portmaster-unity's ua_host. On the first launch the setup builds the game from your copy in
# ports/@@PORTDIR@@/gamedata/ (see README.md there). Made with `pmunity port new` (@@ROUTE@@ route).

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
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/@@PORTDIR@@"
cd "$GAMEDIR" || exit 1
mv -f "$GAMEDIR/log.txt" "$GAMEDIR/log.prev.txt" 2>/dev/null
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
echo "@@TITLE@@: $CFW_NAME on $DEVICE_NAME ($DEVICE_ARCH), ${DISPLAY_WIDTH}x${DISPLAY_HEIGHT}, RAM $(awk '/MemTotal/{print int($2/1024)" MB"}' /proc/meminfo)"

# ua_host is a 32-bit (armhf) program: the system needs a 32-bit userland (e.g. Knulli's /lib32)
if [ ! -e /lib/ld-linux-armhf.so.3 ]; then
  pm_message "@@TITLE@@ needs a 32-bit (armhf) userland, which this system does not have (no /lib/ld-linux-armhf.so.3)."
  sleep 8
  pm_finish
  exit 1
fi

# ---- first launch: build the game from your copy (PortMaster's patcher shows the progress)
if [ ! -f "$GAMEDIR/.setup_done" ]; then
  if [ -f "$GAMEDIR/setup/patchscript" ] && [ -f "$controlfolder/utils/patcher.txt" ]; then
    export GAMEDIR DEVICE_ARCH controlfolder
    chmod +x "$GAMEDIR/setup/patchscript"
    export PATCHER_FILE="$GAMEDIR/setup/patchscript"
    export PATCHER_GAME="@@TITLE@@"
    export PATCHER_TIME="@@SETUP_TIME@@, once"
    [ -f "$GAMEDIR/setup/questions.lua" ] && export PATCHER_QUESTIONS="$GAMEDIR/setup/questions.lua"
    source "$controlfolder/utils/patcher.txt"
    $ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
  fi
  if [ ! -f "$GAMEDIR/.setup_done" ]; then
    pm_message "@@TITLE@@ is not set up yet: put @@GAME_WHAT@@ in ports/@@PORTDIR@@/gamedata and launch again (ports/@@PORTDIR@@/README.md; log: ports/@@PORTDIR@@/patchlog.txt)."
    sleep 8
    pm_finish
    exit 1
  fi
fi

# the built-in pad reaches the game as an Android gamepad (ua_host: SDL GameController ->
# KeyEvent and MotionEvent)
export SDL_GAMECONTROLLERCONFIG="${SDL_GAMECONTROLLERCONFIG:-$sdl_controllerconfig}"
# audio: the system's default ALSA device, else (a sound server holding the card and no 32-bit
# client libraries) raw PCM piped into pw-cat / pacat
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-alsa}
export UA_FPS=${UA_FPS:-10}                     # a line of frame statistics every 10 s in log.txt
# the game's options (port.env), and the setup's answers that apply when the game runs
[ -f "$GAMEDIR/port.env" ] && source "$GAMEDIR/port.env"
[ -f "$GAMEDIR/answers.env" ] && source "$GAMEDIR/answers.env"
chmod +x "$GAMEDIR/ua_host"

$GPTOKEYB "ua_host" -c "$GAMEDIR/@@PORTDIR@@.gptk" &   # exit hotkey only
pm_platform_helper "$GAMEDIR/ua_host"

# the 32-bit libraries first (Knulli: /lib32, /usr/lib32)
LD_LIBRARY_PATH=/lib32:/usr/lib32${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} ./ua_host "$GAMEDIR"
echo "game exited ($?)"

$ESUDO kill -9 $(pidof gptokeyb gptokeyb2) 2>/dev/null
pm_finish
