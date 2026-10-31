# integration.m5.stop_after_restore -- a disk that was started from a repaired bundle must
# still stop.
#
# **Guest-only.**  It creates a real worker, a real AIM disk and a real Windows volume.
#
# Why this is its own case: the bundle-corruption matrix (R-03) needs to stop the disk between
# damages, and during that matrix a stop after "repair the manifest, start the disk again"
# ended `failed` while the same stop succeeded on the first cycle.  A disk that cannot be
# stopped is a release-gate failure on its own ("行为可预测；不挂死、不残留"), so the shape is
# measured directly and the service log is dumped *before* the service is touched again.
#
# The script deliberately keeps the service process alive until the verdict is printed: the
# first time this failed, the harness's cleanup killed the console service before anyone read
# `kind=7`, and the log was empty by the time it mattered.

param(
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'

trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.stop_after_restore creates a real worker, a real AIM disk and a ' +
        'real Windows volume, and may only run on the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
$serviceName = 'MyRamDiskService'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

foreach ($path in @($serviceExe, $cliExe)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "missing binary: $path" }
}

$workRoot = 'C:\m5\stop-restore-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$bundlePath = Join-Path $workRoot 'bundle'
$profileName = 'stop-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$serviceLog = Join-Path $workRoot 'service.log'
$traceLog = Join-Path $workRoot 'trace.log'
$manifestBackup = Join-Path $workRoot 'disk.rdm.orig'
$serviceProcess = $null

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
}

function Get-DiskLine([string]$letter) {
    $listing = Invoke-Cli @('disk', 'list')
    if ($listing.exit -ne 0) { return $null }
    return (($listing.output -split "`n") |
        Where-Object { $_ -match ('\s' + [regex]::Escape($letter)) } |
        Select-Object -First 1)
}

function Get-Field([string]$line, [string]$name) {
    if ($null -eq $line) { return $null }
    $match = [regex]::Match($line, $name + '=([^\s]+)')
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value
}

function Dump-Diagnostics {
    if (Test-Path -LiteralPath $serviceLog) {
        Write-Host '--- service log: kind=7 and stop/checkpoint lines'
        Get-Content -LiteralPath $serviceLog -ErrorAction SilentlyContinue |
            Where-Object { $_ -match 'kind=7|disk.stop|checkpoint' } |
            Select-Object -Last 30 | ForEach-Object { Write-Host "  $_" }
    }
    $line = Get-DiskLine $script:mount
    Write-Host ("  disk list: " + [string]$line)
}

try {
    Write-Step 'start the service'
    Remove-ScService
    $running = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) { Fail "the guest already has $($running.Count) worker(s); reset it first" }
    $env:MYRD_TRACE_FILE = $traceLog
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog `
        -RedirectStandardOutput (Join-Path $workRoot 'service.out.log') `
        -WindowStyle Hidden -PassThru
    $listed = Wait-ForService 90
    if ($null -eq $listed) { Fail "the service never answered a list command: $((Get-Content -LiteralPath $serviceLog -Raw -ErrorAction SilentlyContinue))" }
    $existing = (($listed.output -split "`n") | Where-Object { $_ -match 'generation=' })
    if ($existing.Count -ne 0) { Fail "the guest already has $($existing.Count) profile(s); reset it first" }

    $script:mount = Select-FreeLetter
    if ($null -eq $script:mount) { Fail 'no free drive letter' }
    Write-Reading 'mount' $script:mount

    Write-Step "create a 64 MiB disk at $($script:mount)"
    $create = Invoke-Cli @('disk', 'create', '--name', $profileName,
        '--bundle', $bundlePath, '--size', '64MiB', '--mount', $script:mount,
        '--checkpoint-interval', '30m')
    if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the create carried no operation id' }
    if ((Wait-ForTerminal $createId 300) -ne 'succeeded') { Fail 'the create did not succeed' }
    $line = Get-DiskLine $script:mount
    $profileId = Get-UuidFromText $line
    Write-Reading 'profile_id' $profileId

    $markerPath = Join-Path ($script:mount + '\') 'stop-marker.txt'
    $payload = 'committed before the repair ' + [guid]::NewGuid().ToString('N')
    [System.IO.File]::WriteAllText($markerPath, $payload, [System.Text.Encoding]::UTF8)
    $stream = [System.IO.File]::Open($markerPath, 'Open', 'ReadWrite', 'Read')
    try { $stream.Flush($true) } finally { $stream.Dispose() }
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $profileId)
    if ($checkpoint.exit -ne 0) { Fail "checkpoint failed: $($checkpoint.output)" }
    $checkpointId = Get-UuidFromText $checkpoint.output
    if ($null -ne $checkpointId) { [void](Wait-ForTerminal $checkpointId 300) }

    Write-Step 'first stop (the control)'
    $stop1 = Invoke-Cli @('disk', 'stop', $profileId)
    $stop1State = 'refused'
    if ($stop1.exit -eq 0) {
        $id = Get-UuidFromText $stop1.output
        if ($null -ne $id) { $stop1State = Wait-ForTerminal $id 300 }
    }
    Write-Reading 'stop1_state' $stop1State
    if ($stop1State -ne 'succeeded') { Dump-Diagnostics; Fail "the control stop ended $stop1State" }

    Write-Step 'truncate the manifest, start (must fail), restore, start again'
    $manifest = Join-Path $bundlePath 'disk.rdm'
    if (-not (Test-Path -LiteralPath $manifest)) { Fail "missing manifest: $manifest" }
    Copy-Item -LiteralPath $manifest -Destination $manifestBackup -Force
    $bytes = [System.IO.File]::ReadAllBytes($manifest)
    [System.IO.File]::WriteAllBytes($manifest, $bytes[0..31])

    $badStart = Invoke-Cli @('disk', 'start', $profileId)
    $badStartState = 'refused'
    if ($badStart.exit -eq 0) {
        $id = Get-UuidFromText $badStart.output
        if ($null -ne $id) { $badStartState = Wait-ForTerminal $id 300 }
    }
    Write-Reading 'damaged_start_state' $badStartState
    if ($badStartState -ne 'failed') { Fail "a truncated manifest did not fail the start: $badStartState" }

    Copy-Item -LiteralPath $manifestBackup -Destination $manifest -Force
    $goodStart = Invoke-Cli @('disk', 'start', $profileId)
    if ($goodStart.exit -ne 0) { Dump-Diagnostics; Fail "the repaired start was refused: $($goodStart.output)" }
    $goodStartId = Get-UuidFromText $goodStart.output
    $goodStartState = 'unknown'
    if ($null -ne $goodStartId) { $goodStartState = Wait-ForTerminal $goodStartId 300 }
    Write-Reading 'repaired_start_state' $goodStartState
    if ($goodStartState -ne 'succeeded') { Dump-Diagnostics; Fail "the repaired start ended $goodStartState" }
    Write-Reading 'marker_intact' $(if ((Get-Content -LiteralPath $markerPath -Raw) -eq $payload) { 'yes' } else { 'NO' })

    Write-Step 'second stop (must succeed on a running, repaired disk)'
    $before = Get-DiskLine $script:mount
    Write-Host ("  before stop: " + [string]$before)
    $stop2 = Invoke-Cli @('disk', 'stop', $profileId)
    $stop2State = 'refused'
    $stop2Id = $null
    if ($stop2.exit -eq 0) {
        $stop2Id = Get-UuidFromText $stop2.output
        if ($null -ne $stop2Id) { $stop2State = Wait-ForTerminal $stop2Id 300 }
    }
    Write-Reading 'stop2_cli_exit' ([string]$stop2.exit)
    Write-Reading 'stop2_state' $stop2State
    Write-Reading 'stop2_output' (($stop2.output -replace "`n", ' | '))
    if ($stop2State -ne 'succeeded') {
        # The service is still alive here on purpose: dump before anything stops it.
        Dump-Diagnostics
        Fail "the stop after a repaired start ended $stop2State (forced stop not attempted)"
    }
    $after = Get-DiskLine $script:mount
    Write-Host ("  after stop: " + [string]$after)
    Write-Reading 'runtime_after_stop' ([string](Get-Field $after 'runtime'))
    Write-Reading 'persistence_after_stop' ([string](Get-Field $after 'persistence'))
    $workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count
    Write-Reading 'workers_after_stop' ([string]$workers)
    if ($workers -ne 0) { Fail "the stop left $workers worker(s) behind" }

    Write-Step 'cleanup: forget the disk'
    $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
    Write-Reading 'cleanup_forget' ([string]$forget.exit)

    Write-Host 'PASS integration.m5.stop_after_restore: a repaired disk starts and still stops'
    Write-Reading 'result' 'PASS'
} finally {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
