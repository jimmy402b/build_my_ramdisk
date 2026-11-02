# integration.m5.drive_letter -- the drive letter is specified at create and reused on every
# start, including after a service restart and a guest reboot (R-19 follow-up).
#
# **Host-driven, two modes.**  A guest case cannot survive a reboot, so
# `tools/m5vm/Test-M5DriveLetter.ps1` runs this case twice:
#   * `stability`: install the service, create a disk at a free letter, start it, assert the
#     volume carries that letter, stop/start again, restart the *service* and start again, then
#     leave the disk registered (stopped) for the reboot check.  (An occupied-letter probe
#     with `subst` was measured separately: a substitution is not a volume mount point, so
#     start still succeeds and the volume takes the letter -- recorded, not re-run here
#     because that combination wedged the guest's mount manager twice.)
#   * `after-reboot`: start the same disk after the host hard-reset the VM, assert it is still
#     the recorded letter, then clean up (stop, forget, uninstall).
#
# The mode comes from `C:\m5\drive-letter\mode.txt`, because the harness runs the case with a
# filter and no arguments.  Readings are printed and written to `readings.json` so the host can
# compare the two runs without parsing prose.

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
        'integration.m5.drive_letter creates real workers and volumes; it may only run on ' +
        'the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

$root = 'C:\m5\drive-letter'
New-Item -ItemType Directory -Force -Path $root | Out-Null
$modeFile = Join-Path $root 'mode.txt'
$mode = if (Test-Path -LiteralPath $modeFile) { (Get-Content $modeFile -Raw).Trim() } else { 'stability' }
$letterFile = Join-Path $root 'letter.txt'
$readingsFile = Join-Path $root 'readings.json'
$serviceName = 'MyRamDiskService'
$bundle = Join-Path $root ('bundle-' + (Get-Date).ToString('HHmmss'))
if (Test-Path -LiteralPath $bundle) { Remove-Item -LiteralPath $bundle -Recurse -Force }
Write-Host "READING: drive_letter.mode=$mode"

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
}

function Ensure-Service {
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline) {
        if ((& sc.exe query $serviceName 2>&1 | Out-String) -match 'RUNNING') { return }
        Start-Sleep -Milliseconds 500
    }
    $install = Join-Path $repo 'tools\install\Install-MyRamDisk.ps1'
    if (-not (Test-Path -LiteralPath $install)) { Fail "the installer is missing: $install" }
    $output = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $install `
        -SourceDirectory $bin -ServiceName $serviceName 2>&1 | Out-String)
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline) {
        if ((& sc.exe query $serviceName 2>&1 | Out-String) -match 'RUNNING') { return }
        Start-Sleep -Milliseconds 500
    }
    Fail ("the installed service did not start:`n" + $output)
}

function Get-LetterChar([string]$target) {
    return $target.Substring(0, 1).ToUpperInvariant()
}

function Test-LetterMounted([string]$target) {
    $char = Get-LetterChar $target
    $volume = Get-Volume -DriveLetter $char -ErrorAction SilentlyContinue
    if ($null -eq $volume) { return $false }
    return $true
}

function Get-ListedMounts {
    $mounts = @()
    $listing = Invoke-Cli @('disk', 'list')
    foreach ($row in @($listing.output)) {
        foreach ($match in [regex]::Matches($row, '\s([A-Z]):\s')) { $mounts += $match.Groups[1].Value }
    }
    return ,$mounts
}

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

function Start-Disk([string]$id) {
    $started = Invoke-Cli @('disk', 'start', $id)
    if ($started.exit -ne 0) { return $started }
    $state = Wait-ForTerminal (Get-UuidFromText $started.output) 300
    return [pscustomobject]@{ exit = 0; output = $started.output; state = $state }
}

function Stop-Disk([string]$id) {
    $stopped = Invoke-Cli @('disk', 'stop', $id)
    if ($stopped.exit -ne 0) { return $stopped }
    $state = Wait-ForTerminal (Get-UuidFromText $stopped.output) 300
    return [pscustomobject]@{ exit = 0; output = $stopped.output; state = $state }
}

if ($mode -eq 'after-reboot') {
    Write-Step 'after-reboot: start the recorded disk and assert the same drive letter'
    if (-not (Test-Path -LiteralPath $letterFile)) { Fail 'the host did not record a letter' }
    $target = (Get-Content -LiteralPath $letterFile -Raw).Trim()
    if ([string]::IsNullOrWhiteSpace($target) -or $target.Length -lt 2) {
        Fail ("the recorded letter is unusable: '" + $target + "'")
    }
    Ensure-Service
    $listing = Invoke-Cli @('disk', 'list')
    $id = Get-UuidFromText $listing.output
    if ($null -eq $id) { Fail ('no profile survived the reboot: ' + $listing.output) }
    Write-Reading 'reboot.id' $id
    # A previous attempt may have left the disk running without a usable letter; stop first so
    # this run always exercises "start assigns the recorded letter", not "already running".
    [void](Stop-Disk $id)
    $started = Start-Disk $id
    if ($started.exit -ne 0) { Fail ("start after reboot failed: " + $started.output) }
    if ($started.state -ne 'succeeded') { Fail "start after reboot ended $($started.state)" }
    $mounted = Test-LetterMounted $target
    Write-Reading 'reboot.letter' $target
    Write-Reading 'reboot.volume_present' $(if ($mounted) { 'yes' } else { 'no' })
    if (-not $mounted) { Fail "the volume is not at $target after the reboot" }
    $mounts = Get-ListedMounts
    Write-Reading 'reboot.list_mounts' ($mounts -join ',')
    if ($mounts -notcontains (Get-LetterChar $target)) { Fail "disk list does not show $target after the reboot" }
    # Cleanup: stop, forget, uninstall.
    [void](Stop-Disk $id)
    $forgot = Invoke-Cli @('disk', 'forget', $id, '--confirmation', $id)
    Write-Reading 'reboot.forget_exit' ([string]$forgot.exit)
    $uninstall = Join-Path $repo 'tools\install\Uninstall-MyRamDisk.ps1'
    if (Test-Path -LiteralPath $uninstall) {
        [void](& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $uninstall -ServiceName $serviceName 2>&1 | Out-String)
    }
    [ordered]@{ mode = 'after-reboot'; letter = $target; id = $id; volume_present = $mounted } |
        ConvertTo-Json | Set-Content -LiteralPath $readingsFile -Encoding ASCII
    Write-Reading 'drive_letter.reboot_result' 'PASS'
    exit 0
}

Write-Step 'stability: install the service and create a disk at a free letter'
Ensure-Service
$target = Select-FreeLetter
if ($null -eq $target) { Fail 'no free drive letter to test with' }
[System.IO.File]::WriteAllText($letterFile, $target, [System.Text.Encoding]::ASCII)
if (((Get-Content -LiteralPath $letterFile -Raw) -replace "`0", "").Trim() -ne $target) { Fail "the letter record did not round-trip: $target" }
Write-Reading 'stability.letter' $target
$before = Get-ListedIds
$created = Invoke-Cli @('disk', 'create', '--name', 'drive-letter', '--bundle', $bundle,
    '--size', '256MiB', '--mount', $target, '--no-start')
if ($created.exit -ne 0) { Fail ("create failed: " + $created.output) }
$createdState = Wait-ForTerminal (Get-UuidFromText $created.output) 300
if ($createdState -ne 'succeeded') {
    $show = Invoke-Cli @('operation', 'show', (Get-UuidFromText $created.output))
    Write-Reading 'stability.create_state' $createdState
    Write-Reading 'stability.create_show' (($show.output -replace "`r?`n", ' | '))
    Fail ("create did not succeed: " + $createdState)
}
$id = Wait-ForNewId $before 60
if ($null -eq $id) { Fail 'the created profile does not appear in the listing' }
Write-Reading 'stability.id' $id

Write-Step 'stability: first start'
$started = Start-Disk $id
if ($started.exit -ne 0 -or $started.state -ne 'succeeded') { Fail ("first start failed: " + $started.output) }
if (-not (Test-LetterMounted $target)) { Fail "the volume is not at $target after the first start" }
Write-Reading 'stability.first_start_letter' $target
Write-Reading 'stability.first_start_volume' 'yes'

Write-Step 'stability: stop and start again'
$stopped = Stop-Disk $id
if ($stopped.exit -ne 0 -or $stopped.state -ne 'succeeded') { Fail ("stop failed: " + $stopped.output) }
$started = Start-Disk $id
if ($started.exit -ne 0 -or $started.state -ne 'succeeded') { Fail ("second start failed: " + $started.output) }
if (-not (Test-LetterMounted $target)) { Fail "the volume is not at $target after stop/start" }
$mounts = Get-ListedMounts
if ($mounts -notcontains (Get-LetterChar $target)) { Fail "disk list does not show $target after stop/start" }
Write-Reading 'stability.restart_letter' $target
Write-Reading 'stability.restart_volume' 'yes'

Write-Step 'stability: restart the service and start again'
[void](Invoke-Sc @('stop', $serviceName))
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
    if ((& sc.exe query $serviceName 2>&1 | Out-String) -notmatch 'RUNNING') { break }
    Start-Sleep -Milliseconds 500
}
[void](Invoke-Sc @('start', $serviceName))
if ($null -eq (Wait-ForService 120)) { Fail 'the service did not answer after the restart' }
$listAfterService = Invoke-Cli @('disk', 'list')
if ($listAfterService.exit -ne 0) { Fail 'the service does not list disks after its restart' }
$started = Start-Disk $id
if ($started.exit -ne 0 -or $started.state -ne 'succeeded') { Fail ("start after service restart failed: " + $started.output) }
if (-not (Test-LetterMounted $target)) { Fail "the volume is not at $target after the service restart" }
Write-Reading 'stability.service_restart_letter' $target

[ordered]@{ mode = 'stability'; letter = $target; id = $id } |
    ConvertTo-Json | Set-Content -LiteralPath $readingsFile -Encoding ASCII
Write-Reading 'drive_letter.stability_result' 'PASS'
Write-Host 'PASS integration.m5.drive_letter (stability)'
exit 0
