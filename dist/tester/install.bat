@echo off
rem Installs omd2mp into Orcs Must Die! 2. Optional argument: the game folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Game "%~1"
pause
