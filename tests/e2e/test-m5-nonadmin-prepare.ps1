# integration.m5.nonadmin_prepare -- prepares the state the host-side non-admin run needs (R-11).
#
# **Guest-only, host-driven.**  It is not part of the release suite; `tools/m5vm/Test-M5NonAdmin.ps1`
# invokes it twice:
#   * `prepare` (default): start one console service (and leave it running), create two
#     `--no-start` profiles -- one owned by the standard user whose SID the host wrote to
#     `C:\m5\nonadmin\sid.txt`, one owned by this administrator -- and record their ids in
#     `C:\m5\nonadmin\profiles.json`;
#   * `cleanup`: forget both profiles and stop the service.
#
# The host then runs the GUI and the CLI as that standard user through `vmrun
# runProgramInGuest` (a non-elevated token in the guest's session) and reads the captured
# output.  Why this shape: `Start-Process -Credential`, a Limited scheduled task and
# `CreateProcessAsUser` all failed to launch the process on this guest; `vmrun` guest
# operations do work and are host-controlled.

param(
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.nonadmin_prepare creates real workers and volumes; it may only run on ' +
        'the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

$nonadmin = 'C:\m5\nonadmin'
$serviceName = 'MyRamDiskService'
New-Item -ItemType Directory -Force -Path $nonadmin | Out-Null
$modeFile = Join-Path $nonadmin 'mode.txt'
$mode = if (Test-Path -LiteralPath $modeFile) {
    (Get-Content -LiteralPath $modeFile -Raw).Trim()
} else { 'prepare' }
Write-Host "READING: nonadmin.mode=$mode"

$workRoot = Join-Path $nonadmin 'run'
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$serviceLog = Join-Path $workRoot 'service.log'

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
}

function Get-UuidFromText([string]$text) {
    $match = [regex]::Match($text,
        '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')
    if ($match.Success) { return $match.Value }
    return $null
}

function Wait-ForTerminal([string]$operationId, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $show = Invoke-Cli @('operation', 'show', $operationId)
        $match = [regex]::Match($show.output,
            '(?m)^operation\s+\S+:\s+(succeeded|completed|failed|cancelled)\b')
        if ($match.Success) {
            if ($match.Groups[1].Value -eq 'completed') { return 'succeeded' }
            return $match.Groups[1].Value
        }
        Start-Sleep -Milliseconds 200
    }
    return 'timeout'
}

function Invoke-Operation([string]$label, [string[]]$arguments, [int]$seconds = 300) {
    $result = Invoke-Cli $arguments
    if ($result.exit -ne 0) { return @{ state = 'refused'; output = $result.output } }
    $id = Get-UuidFromText $result.output
    if ($null -eq $id) { return @{ state = 'no-id'; output = $result.output } }
    return @{ state = (Wait-ForTerminal $id $seconds); output = $result.output }
}

# **The listing carries no profile name.**  Measured: `disk list` and `disk show` print id,
# mount, state, generation and memory only -- matching by name found nothing while both creates
# had succeeded.  The ids are therefore derived from the difference between two listings around
# each create, which is unambiguous and needs no name.
function Get-ListedIds {
    $ids = @()
    $listing = Invoke-Cli @('disk', 'list')
    foreach ($row in @($listing.output)) {
        foreach ($match in [regex]::Matches($row,
                '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')) {
            if ($ids -notcontains $match.Value) { $ids += $match.Value }
        }
    }
    return ,$ids
}

function Wait-ForNewId([string[]]$before, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $now = Get-ListedIds
        foreach ($id in $now) { if ($before -notcontains $id) { return $id } }
        Start-Sleep -Milliseconds 300
    }
    return $null
}
if ($mode -eq 'cleanup') {
    Write-Step 'cleanup: forget the prepared profiles and stop the service'
    $ids = @()
    $profilesFile = Join-Path $nonadmin 'profiles.json'
    if (Test-Path -LiteralPath $profilesFile) {
        $profiles = Get-Content -LiteralPath $profilesFile -Raw | ConvertFrom-Json
        foreach ($id in @($profiles.owned, $profiles.admin)) {
            if (-not [string]::IsNullOrWhiteSpace($id)) { $ids += $id }
        }
    }
    foreach ($id in $ids) {
        $forget = Invoke-Operation 'forget' @('disk', 'forget', $id, '--confirmation', $id) 300
        Write-Reading 'cleanup.forget' ("$id=" + $forget.state)
    }
    Get-Process -Name 'myrd-service' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    # Take the installed service away again: the non-admin case is the only consumer of an
    # installed service, and leaving it behind would change what the next case measures.
    $uninstall = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'tools\install\Uninstall-MyRamDisk.ps1'
    if (Test-Path -LiteralPath $uninstall) {
        $uninstallOutput = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $uninstall -ServiceName $serviceName 2>&1 | Out-String)
        Write-Reading 'cleanup.uninstalled' ([string](-not ($uninstallOutput -match 'error')))
    }
    Write-Host 'PASS integration.m5.nonadmin_prepare (cleanup)'
    exit 0
}

# **The service has to outlive this case.**  Measured: a console service started here is gone by
# the time the host runs the next step (the agent's job object takes its children with it), so
# every standard-user step answered "the service is not running".  The installed service runs
# under the SCM and persists, which is also the configuration a user actually has.
Write-Step 'ensure the installed service is running'
$state = (& sc.exe query $serviceName 2>&1 | Out-String)
if ($state -notmatch 'RUNNING') {
    Get-Process -Name 'myrd-service' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    $install = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'tools\install\Install-MyRamDisk.ps1'
    if (-not (Test-Path -LiteralPath $install)) { Fail "the installer is missing: $install" }
    $installOutput = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $install -SourceDirectory $bin -ServiceName $serviceName 2>&1 | Out-String)
    $deadline = (Get-Date).AddSeconds(120)
    $running = $false
    while ((Get-Date) -lt $deadline) {
        if ((& sc.exe query $serviceName 2>&1 | Out-String) -match 'RUNNING') { $running = $true; break }
        Start-Sleep -Milliseconds 300
    }
    if (-not $running) { Fail ("the installed service did not start:" + $installOutput) }
}
Write-Reading 'nonadmin.service' 'installed-running'
$sidFile = Join-Path $nonadmin 'sid.txt'
if (-not (Test-Path -LiteralPath $sidFile)) { Fail 'the host did not write sid.txt' }
$limitedSid = (Get-Content -LiteralPath $sidFile -Raw).Trim()
Write-Reading 'nonadmin.sid' $limitedSid

Write-Step 'create one profile the standard user owns and one the administrator owns'
$beforeAll = Get-ListedIds
$owned = Invoke-Operation 'owned' @('disk', 'create', '--name', 'nonadmin-owned',
    '--bundle', (Join-Path $nonadmin 'bundle-owned'), '--size', '64MiB',
    '--mount', 'T:', '--owner', $limitedSid, '--no-start')
if ($owned.state -ne 'succeeded') { Fail "the owned create ended $($owned.state): $($owned.output)" }
$ownedId = Wait-ForNewId $beforeAll 60
if ($null -eq $ownedId) { Fail 'the owned profile does not appear in the listing' }
$beforeAdmin = Get-ListedIds
$admin = Invoke-Operation 'admin' @('disk', 'create', '--name', 'nonadmin-admin',
    '--bundle', (Join-Path $nonadmin 'bundle-admin'), '--size', '64MiB',
    '--mount', 'S:', '--no-start')
if ($admin.state -ne 'succeeded') { Fail "the admin create ended $($admin.state): $($admin.output)" }
$adminId = Wait-ForNewId $beforeAdmin 60
if ($null -eq $adminId) { Fail 'the admin profile does not appear in the listing' }
Write-Reading 'nonadmin.owned' $ownedId
Write-Reading 'nonadmin.admin' $adminId
[ordered]@{ owned = $ownedId; admin = $adminId } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $nonadmin 'profiles.json') -Encoding ASCII

Write-Host 'PASS integration.m5.nonadmin_prepare (prepare)'
Write-Reading 'result' 'PASS'
