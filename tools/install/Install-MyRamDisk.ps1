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
# **Present is not enough, and running is not enough (R-06).**  The product was built and
# frozen against phdskmnt 1.2.28.93 (the M0 evidence), so an older signed driver is refused
# by version rather than failing later inside an AIM call.  The floor lives here, next to
# the presence check, because this is the only place that can stop the install cleanly.
$driverFloor = [version]'1.2.28.93'
$driverFile = Join-Path $env:SystemRoot 'System32\drivers\phdskmnt.sys'
if (-not (Test-Path -LiteralPath $driverFile)) {
    Fail ("the AIM driver service exists but $driverFile is missing; the driver installation " +
        "is incomplete and will be refused.")
}
$driverVersion = (Get-Item -LiteralPath $driverFile).VersionInfo.FileVersion
$parsed = $null
if (-not [version]::TryParse(($driverVersion -replace '[^0-9.]', ''), [ref]$parsed)) {
    Fail ("the AIM driver version could not be read (FileVersion='$driverVersion'); " +
        "this release requires phdskmnt $driverFloor or newer.")
}
if ($parsed -lt $driverFloor) {
    Fail ("the AIM driver is phdskmnt $parsed, and this release requires $driverFloor or newer. " +
        "Update the driver package before installing; this installer will not replace it.")
}
if ($driver -notmatch 'RUNNING') {
    Fail ("the AIM driver (phdskmnt) is installed ($parsed) but not running. Start it with " +
        "`sc start phdskmnt` and run this installer again.")
}
Write-Host "  phdskmnt $parsed present and running (floor $driverFloor)"

if ($WhatIfOnly) {
    Write-Host "WOULD INSTALL from $source to $InstallDirectory"
    exit 0
}

# **An overwrite install has to stop the old service before it copies (R-08).**  Measured: with
# the service running, Copy-Item failed with "the file is in use" on myrd-service.exe.  The old
# service is stopped and removed here, before the binaries are replaced; the registration block
# further down re-creates and starts it.  Stopping it is also what takes any running disk down
# through the product's own stop path rather than leaving a worker on a replaced binary.
$existingService = & sc.exe query $ServiceName 2>&1 | Out-String
if ($existingService -notmatch '1060') {
    Step "stop and remove the existing service $ServiceName before replacing its binaries"
    & sc.exe stop $ServiceName 2>&1 | Out-Null
    $state = $existingService
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline) {
        $state = & sc.exe query $ServiceName 2>&1 | Out-String
        if ($state -match 'STOPPED' -or $state -match '1060') { break }
        Start-Sleep -Milliseconds 500
    }
    if ($state -notmatch 'STOPPED' -and $state -notmatch '1060') {
        Fail ("the existing service $ServiceName did not stop, so its binaries are still in use: " +
            ($state -replace "`n", ' '))
    }
    & sc.exe delete $ServiceName 2>&1 | Out-Null
    Start-Sleep -Seconds 1
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
# **The durable directories are for the service alone, and their DACL must be *protected*.**
#
# The profile store refuses to operate on a directory whose ACL has any entry that is not SYSTEM or
# Administrators with FILE_ALL_ACCESS, and it also refuses a DACL that is still inheriting.  Both
# were measured: a `Users: RX` grant produced "A management ACL entry lacks required access", and a
# directory whose inheritance had only been disabled in place produced "A profile directory ACL is
# not protected".  `icacls` cannot express "protected" reliably, so the ACL is built with
# `SetAccessRuleProtection($true, $false)` -- protection on, inherited rules removed -- and applied
# with `Set-Acl`.  A record anyone can read, or whose permissions can change by moving its parent, is
# a record whose disclosure cannot be controlled.
function Set-ManagementAcl([string]$path, [bool]$readableByUsers) {
    $security = New-Object System.Security.AccessControl.DirectorySecurity
    $security.SetAccessRuleProtection($true, $false)
    $inheritance = [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor `
        [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
    $propagation = [System.Security.AccessControl.PropagationFlags]::None
    $allow = [System.Security.AccessControl.AccessControlType]::Allow
    $full = [System.Security.AccessControl.FileSystemRights]::FullControl
    foreach ($sidText in @('S-1-5-18', 'S-1-5-32-544')) {
        $sid = New-Object System.Security.Principal.SecurityIdentifier($sidText)
        $security.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
            $sid, $full, $inheritance, $propagation, $allow)))
    }
    if ($readableByUsers) {
        $users = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-545')
        $read = [System.Security.AccessControl.FileSystemRights]::ReadAndExecute
        $security.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
            $users, $read, $inheritance, $propagation, $allow)))
    }
    Set-Acl -LiteralPath $path -AclObject $security
    # **The owner is set explicitly too (B2).**  An owner implicitly holds WRITE_DAC, so a
    # directory planted by a standard user (C:\ProgramData grants Users create-child and
    # CREATOR OWNER full control) would keep passing a DACL-only check while its owner could
    # re-DACL it later.  Setting the owner to the Administrators group, and the product
    # refusing any other owner, closes that chain.
    $owners = Get-Acl -LiteralPath $path
    $owners.SetOwner((New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')))
    Set-Acl -LiteralPath $path -AclObject $owners
}

# **The data root is checked too.**  Measured: with profiles/operations/logs protected but the
# MyRamDisk root still inheriting, the service refused to build its state ("A profile directory ACL
# is not protected"); protecting the root as well is what let it start.  So the root carries the same
# protected management-only ACL as the two record directories.
Set-ManagementAcl $DataDirectory $false
foreach ($child in @('profiles', 'operations')) {
    Set-ManagementAcl (Join-Path $DataDirectory $child) $false
}
# **The log directory is management-only too (B4).**  It was deliberately Users-readable so an
# operator could read failures without elevation; the audit showed the service writes failure
# messages that contain full bundle paths (and a PID), which disclosed one user's paths to
# every local user.  Reading the log is an administrator/operator action, so the directory now
# carries the same DACL and owner as the record.
Set-ManagementAcl (Join-Path $DataDirectory 'logs') $false
Write-Host "  DACL: profiles/operations/logs management-only (owner: Administrators)"

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
