@echo off
rem Removes XR Window Panels (asks for administrator permission)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
