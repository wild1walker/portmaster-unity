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

# @@TITLE@@ on Unity's own Android player, run natively on the handheld's 32-bit userland by
# portmaster-unity's ua_host. On the first launch the setup builds the game from your copy in
# ports/@@PORTDIR@@/gamedata/ (see README.md there). Made with `pmunity port new` (@@ROUTE@@ route).

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

# Rockchip handhelds on vendor kernels (dArkOS, ArkOS): the video decoder's nodes are root-only while
# ports run as a user, and the port's libpmhwdec (pmhwdec/) needs them for the videos. They are opened to
# the video group for the game and put back exactly as they were when it ends (Knifethrower's fix;
# PortMaster's Moonlight opens the same nodes). Nothing changes where they are usable already.
pmhwdec_nodes=()
if [ -d "$GAMEDIR/pmhwdec" ]; then
  for n in /dev/vpu_service /dev/hevc_service; do
    { [ -e "$n" ] && ! { [ -r "$n" ] && [ -w "$n" ]; }; } || continue
    o=$(stat -c '%u:%g:%a' "$n") || continue
    $ESUDO chgrp video "$n" && $ESUDO chmod 660 "$n" && pmhwdec_nodes+=("$n:$o") && echo "$n: opened to the video group (was $o)"
  done
fi
pmhwdec_restore() {
  local e n o
  for e in "${pmhwdec_nodes[@]}"; do
    n=${e%%:*}; o=${e#*:}
    $ESUDO chown "${o%:*}" "$n"; $ESUDO chmod "${o##*:}" "$n"; echo "$n: put back ($o)"
  done
  pmhwdec_nodes=()
}
trap pmhwdec_restore EXIT

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

# ---- a game set up by an earlier release: what this release's setup does that it has not had yet
# (setup/update, once: the setup folder comes with each release and goes when it is done)
if [ -f "$GAMEDIR/setup/update" ] && [ -f "$controlfolder/utils/patcher.txt" ]; then
  export GAMEDIR DEVICE_ARCH controlfolder
  chmod +x "$GAMEDIR/setup/update"
  export PATCHER_FILE="$GAMEDIR/setup/update"
  export PATCHER_GAME="@@TITLE@@"
  export PATCHER_TIME="a few minutes, once"
  unset PATCHER_QUESTIONS
  source "$controlfolder/utils/patcher.txt"
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
# ua_host's layers that leave the picture and the play as they are (docs/runtime.md; port.env or the
# environment wins, an empty value turns one off): the methods a run compiled, compiled at the next launch
# over the title and the first load; dynamic geometry in ranges of one buffer; Unity's GLES buffer pool in
# O(1) lists (its own player versions); the main thread waiting for the last present at its frame's end
[ -n "${UA_JIT_WARM+x}" ] || export UA_JIT_WARM=1
[ -n "${UA_GL_ARENA+x}" ] || export UA_GL_ARENA=1
[ -n "${UA_GLES_POOL+x}" ] || export UA_GLES_POOL=1
[ -n "${UA_PRESENT_SYNC+x}" ] || export UA_PRESENT_SYNC=end
# handhelds without analog sticks whose pad mapping still names a left stick: where PortMaster says there are
# none (device_info.txt: ANALOG_STICKS=0), auto means a pad without sticks
if [ "${ANALOG_STICKS:-2}" = 0 ]; then
  case "${UA_DPAD_STICK:-}" in auto|auto,both) export UA_DPAD_STICK=${UA_DPAD_STICK/auto/1} ;; esac
fi
# a Wwise game: ua_host's own Wwise player plays the banks the setup converted (set UA_WWISE_DATA= empty for none)
[ -d "$GAMEDIR/wwise" ] && export UA_WWISE_DATA="${UA_WWISE_DATA-$GAMEDIR/wwise}"
chmod +x "$GAMEDIR/ua_host"
[ -f "$GAMEDIR/pmhwdec/pmhwdec-cedard" ] && chmod +x "$GAMEDIR/pmhwdec/pmhwdec-cedard"   # libpmhwdec's Allwinner decoder, where the port ships it

$GPTOKEYB2 "ua_host" -c "$GAMEDIR/@@PORTDIR@@.ini" &   # exit hotkey only
pm_platform_helper "$GAMEDIR/ua_host"

# the 32-bit libraries first (Knulli: /lib32, /usr/lib32)
LD_LIBRARY_PATH=/lib32:/usr/lib32${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} ./ua_host "$GAMEDIR"
echo "game exited ($?)"

pm_finish
