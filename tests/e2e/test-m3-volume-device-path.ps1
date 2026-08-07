[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'M3Harness.Common.ps1')

$path = Resolve-M3VolumeDevicePath -FilePath $PSScriptRoot
if ($path -notmatch '^\\\\\?\\Volume\{[0-9A-Fa-f-]+\}$') {
    throw "resolved volume device path is invalid: $path"
}
if ($path -match '^\\\\\.\\[A-Za-z]:$') {
    throw "resolved a drive-letter device instead of a stable volume GUID: $path"
}
Write-Output "M3 volume device path test PASS: $path"
