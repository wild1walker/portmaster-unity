# Unity PlayerPrefs on Linux, from the shell (sourced by launcher.sh.in; bash, sed, no Python).
# The Linux player keeps them in $HOME/.config/unity3d/<company>/<product>/prefs, an XML file:
#   <unity_prefs version_major="1" version_minor="1">
#   	<pref name="Screenmanager Resolution Width" type="int">640</pref> ...
# The data folder's app.info names <company> and <product> (two lines).
#
#   unity_prefs_file <data dir> <home>            the prefs file the player uses (printed)
#   unity_prefs_set <file> <name> <int|float|string> <value>   adds or replaces one key
#   unity_prefs_screen <data dir> <home> <w> <h>  the screen keys for a full-screen window of w x h
#
# Only Unity's own screen keys are written: the game's picture and its own settings stay as they
# are (the -screen-* arguments set the same values at each start; the keys cover games that read
# Screen.currentResolution or their resolution menu before the first frame).

unity_prefs_file() {
  local company="" product=""
  if [ -f "$1/app.info" ]; then
    { IFS= read -r company; IFS= read -r product; } < "$1/app.info"
  fi
  echo "$2/.config/unity3d/${company:-unknown}/${product:-unknown}/prefs"
}

unity_prefs_set() {   # file name type value (values of type string are base64 in Unity's file)
  local f="$1" name="$2" type="$3" value="$4" line
  [ "$type" = string ] && value=$(printf '%s' "$value" | base64 | tr -d '\n')
  line=$(printf '\t<pref name="%s" type="%s">%s</pref>' "$name" "$type" "$value")
  mkdir -p "$(dirname "$f")"
  [ -s "$f" ] || printf '<unity_prefs version_major="1" version_minor="1">\n</unity_prefs>\n' > "$f"
  if grep -qF "<pref name=\"$name\" " "$f"; then
    sed -i "s|^.*<pref name=\"$name\" .*$|$line|" "$f"
  else
    sed -i "s|^</unity_prefs>|$line\n</unity_prefs>|" "$f"
  fi
}

unity_prefs_screen() {   # data home w h
  local f
  for f in "$(unity_prefs_file "$1" "$2")" "$2"/.config/unity3d/*/*/prefs; do
    [ -e "$f" ] || [ "$f" = "$(unity_prefs_file "$1" "$2")" ] || continue
    unity_prefs_set "$f" "Screenmanager Resolution Width" int "$3"
    unity_prefs_set "$f" "Screenmanager Resolution Height" int "$4"
    unity_prefs_set "$f" "Screenmanager Fullscreen mode" int 1      # 1 = full-screen window
    unity_prefs_set "$f" "Screenmanager Is Fullscreen mode" int 1   # (Unity 2017 and older)
    unity_prefs_set "$f" "Screenmanager Resolution Use Native" int 0
  done
}
