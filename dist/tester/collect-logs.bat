@echo off
rem Zips omd2mp.log onto your Desktop so you can send it back.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Game "%~1" -CollectLogs
pause
