@echo off
rem Windows: double-click to start the WHAM operator console. A browser page opens; leave this window open while you work.
cd /d "%~dp0"
where py >nul 2>nul && (py -3 wham_ui.py %* & goto done)
where python >nul 2>nul && (python wham_ui.py %* & goto done)
echo Python 3 is not installed. Install it from https://www.python.org/downloads/ and double-click this file again.
:done
echo.
pause
