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

function Get-ProfileId([string]$name) {
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        $listing = Invoke-Cli @('disk', 'list')
        foreach ($row in @($listing.output)) {
            if ($row -match [regex]::Escape($name)) {
                $id = Get-UuidFromText $row
                if ($null -ne $id) { return $id }
            }
        }
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
    Write-Host 'PASS integration.m5.nonadmin_prepare (cleanup)'
    exit 0
}

Write-Step 'start the service (and leave it running for the host)'
$probe = Invoke-Cli @('disk', 'list')
if ($probe.exit -ne 0) {
    Remove-ScService
    Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog `
        -RedirectStandardOutput (Join-Path $workRoot 'service.out.log') `
        -WindowStyle Hidden | Out-Null
    if ($null -eq (Wait-ForService 90)) { Fail 'the service never answered' }
}
$sidFile = Join-Path $nonadmin 'sid.txt'
if (-not (Test-Path -LiteralPath $sidFile)) { Fail 'the host did not write sid.txt' }
$limitedSid = (Get-Content -LiteralPath $sidFile -Raw).Trim()
Write-Reading 'nonadmin.sid' $limitedSid

Write-Step 'create one profile the standard user owns and one the administrator owns'
$owned = Invoke-Operation 'owned' @('disk', 'create', '--name', 'nonadmin-owned',
    '--bundle', (Join-Path $nonadmin 'bundle-owned'), '--size', '64MiB',
    '--mount', 'T:', '--owner', $limitedSid, '--no-start')
if ($owned.state -ne 'succeeded') { Fail "the owned create ended $($owned.state): $($owned.output)" }
$admin = Invoke-Operation 'admin' @('disk', 'create', '--name', 'nonadmin-admin',
    '--bundle', (Join-Path $nonadmin 'bundle-admin'), '--size', '64MiB',
    '--mount', 'S:', '--no-start')
if ($admin.state -ne 'succeeded') { Fail "the admin create ended $($admin.state): $($admin.output)" }

$ownedId = Get-ProfileId 'nonadmin-owned'
$adminId = Get-ProfileId 'nonadmin-admin'
if ($null -eq $ownedId -or $null -eq $adminId) { Fail 'the prepared profiles are not listed' }
Write-Reading 'nonadmin.owned' $ownedId
Write-Reading 'nonadmin.admin' $adminId
[ordered]@{ owned = $ownedId; admin = $adminId } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $nonadmin 'profiles.json') -Encoding ASCII

Write-Host 'PASS integration.m5.nonadmin_prepare (prepare)'
Write-Reading 'result' 'PASS'
