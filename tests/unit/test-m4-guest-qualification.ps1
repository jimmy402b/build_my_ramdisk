[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath($SourceRoot)
$controller = Join-Path $root 'tools\Invoke-M4GuestQualification.ps1'
if (-not [IO.File]::Exists($controller)) {
    throw 'guest qualification controller is missing'
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $controller -SelfTest
if ($LASTEXITCODE -ne 0) {
    throw "guest qualification controller selftest failed with exit code $LASTEXITCODE"
}

Write-Output 'M4 guest qualification contract PASS'
