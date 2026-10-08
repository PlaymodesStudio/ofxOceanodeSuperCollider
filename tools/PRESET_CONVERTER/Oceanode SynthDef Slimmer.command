#!/bin/zsh

set -u

SCRIPT_DIR="${0:A:h}"
APP="$SCRIPT_DIR/oceanode_synthdef_slimmer.py"
if [[ -x "/opt/homebrew/bin/python3" ]]; then
  PYTHON="/opt/homebrew/bin/python3"
elif [[ -x "/usr/local/bin/python3" ]]; then
  PYTHON="/usr/local/bin/python3"
else
  PYTHON="/usr/bin/python3"
fi

if [[ ! -f "$APP" ]]; then
  osascript -e 'display dialog "Could not find oceanode_synthdef_slimmer.py next to the launcher." with title "Oceanode SynthDef Slimmer" buttons {"OK"} default button "OK"'
  exit 1
fi

echo "Launching Oceanode SynthDef Slimmer..."
echo "Launcher folder: $SCRIPT_DIR"
echo
"$PYTHON" "$APP" --gui
STATUS=$?

echo
echo "Oceanode SynthDef Slimmer finished with exit code $STATUS."
echo "You can close this window."
exit "$STATUS"
