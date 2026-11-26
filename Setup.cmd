@echo off
rem Opens the setup window: pick your disc image, and it builds everything.
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\setup\setup.ps1"
