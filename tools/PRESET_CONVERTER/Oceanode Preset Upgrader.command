#!/bin/zsh

set -u

SCRIPT_DIR="${0:A:h}"
CONVERTER="$SCRIPT_DIR/oceanode_preset_converter.py"
if [[ -x "/opt/homebrew/bin/python3" ]]; then
  PYTHON="/opt/homebrew/bin/python3"
elif [[ -x "/usr/local/bin/python3" ]]; then
  PYTHON="/usr/local/bin/python3"
else
  PYTHON="/usr/bin/python3"
fi

if [[ ! -f "$CONVERTER" ]]; then
  osascript -e 'display dialog "Could not find oceanode_preset_converter.py next to the launcher." with title "Oceanode Preset Upgrader" buttons {"OK"} default button "OK"'
  exit 1
fi

echo "Launching Oceanode Preset Upgrader GUI..."
echo "Launcher folder: $SCRIPT_DIR"
echo
OCEANODE_USE_TK=0 "$PYTHON" "$CONVERTER" --gui
STATUS=$?

echo
echo "Oceanode Preset Upgrader finished with exit code $STATUS."
echo "You can close this window."
exit "$STATUS"
