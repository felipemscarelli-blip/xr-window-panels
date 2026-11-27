@echo off
rem Instala o XR Window Panels para todos os apps OpenXR (pede permissao de administrador)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -AllUsers
