#!/bin/bash
# Double-click this file (macOS) to start the WHAM operator console. A browser page opens; leave this window open while you work.
cd "$(dirname "$0")" || exit 1
if ! command -v python3 >/dev/null 2>&1; then
  echo "Python 3 is not installed. Install it from https://www.python.org/downloads/ and double-click this file again."
  read -r -p "Press Enter to close." _; exit 1
fi
python3 wham_ui.py "$@"
echo; read -r -p "The console has stopped. Press Enter to close this window." _
