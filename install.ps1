# Installs the XR Window Panels OpenXR layer.
# Copies the DLL + manifest to %LOCALAPPDATA%\XRWindowPanels\bin and registers it as an
# implicit OpenXR API layer.
#   .\install.ps1              register for the current user (HKCU)
#   .\install.ps1 -AllUsers    register machine-wide (HKLM, asks for admin). Needed by apps
#                              whose built-in OpenXR loader ignores HKCU (e.g. iRacing).
#
# NOTE: run this from a normal PowerShell / double-click Instalar.cmd. Do not run it from
# inside a packaged (MSIX) app such as Claude Desktop: Windows redirects AppData and
# registry writes from those apps into a private copy that games cannot see.
param(
  [string]$From = (Join-Path $PSScriptRoot 'dist'),
  [switch]$AllUsers,
  [string]$Root = (Join-Path $env:LOCALAPPDATA 'XRWindowPanels')
)

$ErrorActionPreference = 'Stop'
$manifestName = 'XR_APILAYER_NOVENDOR_window_panels.json'
$Dest = Join-Path $Root 'bin'
$manifestPath = Join-Path $Dest $manifestName

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($AllUsers -and -not $isAdmin) {
  # Re-run elevated, keeping this user's paths.
  $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -AllUsers -From `"$From`" -Root `"$Root`""
  $p = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
  exit $p.ExitCode
}

try {
  foreach ($f in 'XRWindowPanels.dll', $manifestName) {
    if (-not (Test-Path (Join-Path $From $f))) { throw "Missing $f in $From" }
  }

  New-Item -ItemType Directory -Force $Dest | Out-Null
  Copy-Item (Join-Path $From 'XRWindowPanels.dll') $Dest -Force
  Copy-Item (Join-Path $From $manifestName) $Dest -Force

  # Control app + desktop shortcut
  $control = Join-Path $From 'XRWindowPanelsControl.exe'
  if (Test-Path $control) {
    Copy-Item $control $Dest -Force
    $desktop = [Environment]::GetFolderPath('Desktop')
    $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $desktop 'XR Window Panels.lnk'))
    $lnk.TargetPath = Join-Path $Dest 'XRWindowPanelsControl.exe'
    $lnk.WorkingDirectory = $Dest
    $lnk.Description = 'Controle dos paineis de janelas no VR'
    $lnk.Save()
  }

  # Starter config (only if there is none yet)
  $cfgSrc = Join-Path $PSScriptRoot 'config.ini'
  $cfgDst = Join-Path $Root 'config.ini'
  if ((Test-Path $cfgSrc) -and -not (Test-Path $cfgDst)) { Copy-Item $cfgSrc $cfgDst }

  $hive = if ($AllUsers) { 'HKLM:' } else { 'HKCU:' }
  $key = "$hive\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
  if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
  New-ItemProperty -Path $key -Name $manifestPath -Value 0 -PropertyType DWord -Force | Out-Null

  # Avoid double registration (HKCU + HKLM would load the layer twice).
  $otherKey = "$(if ($AllUsers) { 'HKCU:' } else { 'HKLM:' })\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
  if ((Test-Path $otherKey) -and ($AllUsers -or $isAdmin)) {
    Remove-ItemProperty -Path $otherKey -Name $manifestPath -ErrorAction SilentlyContinue
  }

  Write-Host ""
  Write-Host "OK - XR Window Panels instalado" -ForegroundColor Green
  Write-Host "  DLL:      $Dest"
  Write-Host "  Registro: $key"
  Write-Host "  Config:   $cfgDst"
  Write-Host "  Log:      $(Join-Path $Root 'layer.log')"
} catch {
  Write-Host "ERRO: $_" -ForegroundColor Red
  if ($AllUsers) { Read-Host "Enter para fechar" }
  exit 1
}
if ($AllUsers) { Read-Host "Enter para fechar" }
