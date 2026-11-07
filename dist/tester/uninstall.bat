@echo off
rem Removes omd2mp from Orcs Must Die! 2. Optional argument: the game folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Game "%~1" -Uninstall
pause
