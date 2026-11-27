# Removes the XR Window Panels OpenXR layer registration (HKCU and HKLM) and binaries.
# Keeps config.ini. Asks for admin to clean the machine-wide (HKLM) entry.
param([string]$Root = (Join-Path $env:LOCALAPPDATA 'XRWindowPanels'))

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  $p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Root `"$Root`""
  exit $p.ExitCode
}

$manifest = Join-Path $Root 'bin\XR_APILAYER_NOVENDOR_window_panels.json'
foreach ($hive in 'HKLM:', 'HKCU:') {
  $key = "$hive\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
  if (Test-Path $key) { Remove-ItemProperty -Path $key -Name $manifest -ErrorAction SilentlyContinue }
}
Remove-Item -Recurse -Force (Join-Path $Root 'bin') -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Desktop')) 'XR Window Panels.lnk') -ErrorAction SilentlyContinue
Write-Host 'Desinstalado (config.ini mantido).' -ForegroundColor Green
Read-Host 'Enter para fechar'
