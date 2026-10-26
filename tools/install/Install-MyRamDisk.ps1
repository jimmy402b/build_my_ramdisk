# Installs MyRamDisk: binaries, the service, its durable directories, and the one prerequisite it
# refuses to install for you.
#
# **`phdskmnt` is checked, never installed.**  It is the AIM driver that presents the volumes, it
# requires a signed driver package and a reboot, and a product installer that quietly installs a
# storage driver is a product installer nobody should run.  Missing it is a clear refusal with the
# command the operator can run themselves.
#
# The directories get an explicit DACL rather than inheritance: profiles and operations are the
# service's durable truth, and "whoever happened to create the folder" is not a security boundary.

[CmdletBinding()]
param(
    [string]$SourceDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) '..\out\build\msvc\src\Release'),
    [string]$InstallDirectory = 'C:\Program Files\MyRamDisk',
    [string]$DataDirectory = 'C:\ProgramData\MyRamDisk',
    [string]$ServiceName = 'MyRamDiskService',
    [switch]$WhatIfOnly
)

$ErrorActionPreference = 'Stop'

function Fail([string]$message) {
    [Console]::Error.WriteLine("install: $message")
    exit 1
}

function Step([string]$message) {
    Write-Host "--- $message"
}

$source = (Resolve-Path -LiteralPath $SourceDirectory -ErrorAction SilentlyContinue)
if ($null -eq $source) { Fail "the source directory does not exist: $SourceDirectory" }

$files = @('myrd-service.exe', 'myrd-cli.exe', 'myrd-gui.exe', 'myrd-m5-worker.exe')
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $file))) {
        Fail "the release is incomplete: $file is missing from $source"
    }
}

Step 'check the AIM driver, do not install it'
$driver = & sc.exe query phdskmnt 2>&1 | Out-String
if ($driver -match '1060') {
    Fail ("the AIM driver (phdskmnt) is not installed.  It is a signed storage driver and this " +
        "installer will not install it for you; see the M5 usage document for the driver package.")
}
Write-Host "  phdskmnt present"

if ($WhatIfOnly) {
    Write-Host "WOULD INSTALL from $source to $InstallDirectory"
    exit 0
}

Step "install the binaries to $InstallDirectory"
New-Item -ItemType Directory -Force -Path $InstallDirectory | Out-Null
foreach ($file in $files) {
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination $InstallDirectory -Force
}

Step "create the durable directories under $DataDirectory with an explicit DACL"
foreach ($child in @('profiles', 'operations', 'logs')) {
    $path = Join-Path $DataDirectory $child
    New-Item -ItemType Directory -Force -Path $path | Out-Null
}
# SYSTEM and Administrators full control, Users read: a user has to be able to see why a disk is
# blocked, and must not be able to rewrite the record of it.
$acl = @(
    '*S-1-5-18:(OI)(CI)F',      # SYSTEM
    '*S-1-5-32-544:(OI)(CI)F',  # Administrators
    '*S-1-5-32-545:(OI)(CI)RX'  # Users
)
& icacls.exe $DataDirectory /inheritance:r /grant:r @acl | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "icacls failed on $DataDirectory ($LASTEXITCODE)" }
foreach ($child in @('profiles', 'operations', 'logs')) {
    & icacls.exe (Join-Path $DataDirectory $child) /inheritance:r /grant:r @acl | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "icacls failed on $child ($LASTEXITCODE)" }
}
Write-Host "  DACL: SYSTEM/Administrators full, Users read"

Step "register the service $ServiceName"
$existing = & sc.exe query $ServiceName 2>&1 | Out-String
if ($existing -notmatch '1060') {
    & sc.exe stop $ServiceName 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    & sc.exe delete $ServiceName 2>&1 | Out-Null
    Start-Sleep -Seconds 1
}
$binary = Join-Path $InstallDirectory 'myrd-service.exe'
& sc.exe create $ServiceName 'binPath=' $binary 'type=' 'own' 'start=' 'auto' | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "sc create failed ($LASTEXITCODE)" }
& sc.exe description $ServiceName 'MyRamDisk: RAM-backed volumes with durable checkpoints' | Out-Null
# Restart on failure, because a service that dies once should not need a human to notice.
& sc.exe failure $ServiceName 'reset=' 86400 'actions=' 'restart/5000/restart/15000/none' | Out-Null

Step "start it"
& sc.exe start $ServiceName | Out-Null
$started = $false
for ($i = 0; $i -lt 60; $i++) {
    $state = & sc.exe query $ServiceName 2>&1 | Out-String
    if ($state -match 'RUNNING') { $started = $true; break }
    Start-Sleep -Milliseconds 500
}
if (-not $started) { Fail "the service did not reach RUNNING:`n$($state)" }

$cli = Join-Path $InstallDirectory 'myrd-cli.exe'
$listed = & $cli disk list 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { Fail "the installed service does not answer: $listed" }
Write-Host "  service answers: $($listed.Trim())"

Write-Host "INSTALLED $ServiceName from $source"
exit 0
