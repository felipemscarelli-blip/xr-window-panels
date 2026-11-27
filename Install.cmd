@echo off
rem Installs XR Window Panels for all OpenXR apps (asks for administrator permission)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -AllUsers
