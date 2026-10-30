#!/bin/sh
# Linux: start the WHAM operator console. A browser page opens; leave this terminal open while you work.
cd "$(dirname "$0")" || exit 1
exec python3 wham_ui.py "$@"
