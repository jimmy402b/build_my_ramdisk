# Enter-MsvcEnvironment.ps1
# Dot-source this script to enter a deterministic MSVC x64 + Ninja environment.
# This script must NOT call exit.

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found' }
$installationPath = & $vswhere -latest -products * `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
if ([string]::IsNullOrWhiteSpace($installationPath)) { throw 'MSVC x64 tools not found' }
$module = Join-Path $installationPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $module -ErrorAction Stop
Enter-VsDevShell -VsInstallPath $installationPath -SkipAutomaticLocation `
  -DevCmdArguments '-arch=amd64 -host_arch=amd64' | Out-Null
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe unavailable' }
if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) { throw 'ninja.exe unavailable' }
