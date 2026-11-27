@echo off
rem Remove o XR Window Panels (pede permissao de administrador)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
