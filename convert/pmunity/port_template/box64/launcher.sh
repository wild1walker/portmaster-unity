#!/bin/bash
# PORTMASTER: @@ZIP@@, @@LAUNCHER@@
#
# @@TITLE@@: the game's own Linux x86-64 build, unmodified, under box64 (portmaster-unity's
# @@ROUTE@@ route, docs/box64-route.md). The first launch runs the setup: it finds your copy in
# ports/@@PORTDIR@@/gamedata/ and, for the OpenGL ES mode, adds GLES 3 shaders to it. Options:
# ports/@@PORTDIR@@/port.env (PMU_GL, PMU_BOX64, PMU_MAXCPU, PMU_ARGS, PMU_DYNACACHE).

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

# ---- first launch: find the game and prepare it (PortMaster's patcher shows the progress)
if [ ! -f "$GAMEDIR/@@READY@@" ]; then
  if [ -f "$GAMEDIR/setup/patchscript" ] && [ -f "$controlfolder/utils/patcher.txt" ]; then
    export GAMEDIR DEVICE_ARCH controlfolder
    chmod +x "$GAMEDIR/setup/patchscript"
    export PATCHER_FILE="$GAMEDIR/setup/patchscript"
    export PATCHER_GAME="@@TITLE@@"
    export PATCHER_TIME="@@SETUP_TIME@@, once"
    source "$controlfolder/utils/patcher.txt"
    $ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
  fi
  if [ ! -f "$GAMEDIR/@@READY@@" ]; then
    pm_message "@@TITLE@@ is not set up yet: put @@GAME_WHAT@@ in ports/@@PORTDIR@@/gamedata and launch again (ports/@@PORTDIR@@/README.md; log: ports/@@PORTDIR@@/patchlog.txt)."
    sleep 8
    pm_finish
    exit 1
  fi
fi

# the game the setup found (game.env: GAME_DIR under gamedata/, EXE, DATA, GL_MODE), the options
source "$GAMEDIR/game.env"
[ -f "$GAMEDIR/port.env" ] && source "$GAMEDIR/port.env"
GD="$GAMEDIR/gamedata/$GAME_DIR"
chmod +x "$GAMEDIR/box64/box64" "$GD/$EXE"

# GL: gles  = Unity's OpenGL ES mode (-force-gles) through glxes on the firmware's GLES driver (the
#             setup added GLES 3 shaders to the data)
#     mesa  = Unity's OpenGL Core renderer on a desktop-GL driver (Panfrost, Turnip): data as it is
#     gl4es = Unity's OpenGL 2 renderer (Unity 5.4 and older) through gl4es
GL=${PMU_GL:-$GL_MODE}
ARGS=(-screen-fullscreen 1 -screen-width "$DISPLAY_WIDTH" -screen-height "$DISPLAY_HEIGHT" -logFile "$GAMEDIR/player.log")
case "$GL" in
  gles)  ARGS+=(-force-gles); GLENV=(BOX64_LIBGL="$GAMEDIR/glxes/libGL.so.1"); WESTON=(headless noop kiosk crusty_x11egl) ;;
  gl4es) ARGS+=(-force-opengl); GLENV=(LIBGL_NOBANNER=1); WESTON=(headless noop kiosk crusty_glx_gl4es) ;;
  mesa)  ARGS+=(-force-glcore); GLENV=(); WESTON=(drm gl kiosk system) ;;
  *)     pm_message "PMU_GL=$GL: gles, mesa or gl4es"; sleep 5; pm_finish; exit 1 ;;
esac
# shellcheck disable=SC2206
ARGS+=(${PMU_ARGS:-})

# box64: settings for Unity's players (box64.env), the x86-64 libraries box64 brings (x64lib)
source "$GAMEDIR/box64/box64.env"
B64=($(box64_profile "${PMU_BOX64:-default}") BOX64_LOG=1 BOX64_LD_LIBRARY_PATH="$GAMEDIR/box64/x64lib")
[ -n "${PMU_MAXCPU:-}" ] && B64+=(BOX64_MAXCPU="$PMU_MAXCPU")
# DynaCache (box64 0.4: on by default) keeps translated code between starts, in the port's folder
B64+=(BOX64_DYNACACHE="${PMU_DYNACACHE:-1}" BOX64_DYNACACHE_FOLDER="$GAMEDIR/conf/dynacache" BOX64_DYNACACHE_LIMIT=512)

# Saves and settings: Unity keeps them under $HOME/.config/unity3d/<company>/<product>
mkdir -p "$GAMEDIR/conf/dynacache"
source "$GAMEDIR/box64/unityprefs.sh"
unity_prefs_screen "$GD/$DATA" "$GAMEDIR/conf" "$DISPLAY_WIDTH" "$DISPLAY_HEIGHT"

# X11 for the player: PortMaster's Westonpack (Xwayland), as the firmwares run no X server
weston_dir=/tmp/weston
weston_runtime="weston_pkg_0.2"
if [ ! -f "$controlfolder/libs/${weston_runtime}.squashfs" ]; then
  if [ ! -f "$controlfolder/harbourmaster" ]; then
    pm_message "This port requires the latest PortMaster to run, please go to https://portmaster.games/ for more info."
    sleep 5
    exit 1
  fi
  $ESUDO $controlfolder/harbourmaster --quiet --no-check runtime_check "${weston_runtime}.squashfs"
fi
$ESUDO mkdir -p "$weston_dir"
[[ "$PM_CAN_MOUNT" != "N" ]] && $ESUDO umount "$weston_dir" 2>/dev/null
$ESUDO mount "$controlfolder/libs/${weston_runtime}.squashfs" "$weston_dir"

# the pad: the player's own SDL2 reads it; keys and mouse for games that want them, and the exit
# hotkey: ports/@@PORTDIR@@/@@PORTDIR@@.gptk
$GPTOKEYB "$EXE" -c "$GAMEDIR/@@PORTDIR@@.gptk" &
pm_platform_helper "$GAMEDIR/box64/box64"
# The game's command as a script (westonwrap.sh evaluates its arguments again: names with spaces);
# box64's and the GL settings go on westonwrap's command line, as it reloads PortMaster's settings
# before it starts the game and some firmwares (ROCKNIX) export their own BOX64_LD_LIBRARY_PATH
{ echo '#!/bin/bash'; printf 'cd %q || exit 1\nexec' "$GD"; printf ' %q' "$GAMEDIR/box64/box64" "./$EXE" "${ARGS[@]}"; echo; } > "$GAMEDIR/conf/run.sh"
chmod +x "$GAMEDIR/conf/run.sh"
$ESUDO env $weston_dir/westonwrap.sh "${WESTON[@]}" \
  "${B64[@]}" "${GLENV[@]}" HOME="$GAMEDIR/conf" XDG_CONFIG_HOME="$GAMEDIR/conf/.config" "$GAMEDIR/conf/run.sh"
echo "game exited ($?)"

$ESUDO $weston_dir/westonwrap.sh cleanup
[[ "$PM_CAN_MOUNT" != "N" ]] && $ESUDO umount "$weston_dir"
pm_finish
