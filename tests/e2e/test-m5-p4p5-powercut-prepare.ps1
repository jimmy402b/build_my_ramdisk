# The first half of R-02: stop the worker inside the P4/P5 window and leave the machine for
# a hard power cut.
#
# **Guest-only.**  It creates a real worker, a real AIM disk and a real Windows volume.
#
# P4 is "the commit landed durably"; P5 is "the service recorded the evidence and published
# the epoch".  The gap is milliseconds, so a cut at a guessed time proves nothing.  The
# worker offers a deterministic pause there (`MYRD_M5_TEST_PAUSE_AT=p4-commit`), writes the
# marker before it waits, and this script exits while the worker is still paused -- the host
# driver (`tools/m5vm/Test-M5P4P5PowerCut.ps1`) cuts the power and then runs
# `test-m5-p4p5-powercut-verify.ps1`.
#
# The state that must survive the cut is written to `C:\m5\p4p5-state.json` and flushed to
# the disk: profile id, mount, bundle, the last committed generation, the candidate
# generation named by the P4 marker, the checkpoint operation id, and the exact marker bytes.

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
        'integration.m5.p4p5_prepare creates a real worker, a real AIM disk and a real ' +
        'Windows volume, and may only run on the guest (AGENTS.md section 4).')
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

$statePath = 'C:\m5\p4p5-state.json'
$workRoot = 'C:\m5\p4p5-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$bundlePath = Join-Path $workRoot 'bundle'
$profileName = 'p4p5-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$marker = Join-Path $workRoot 'p4-marker.txt'
$arm = Join-Path $workRoot 'p4-arm.txt'
$serviceLog = Join-Path $workRoot 'service.log'
$serviceProcess = $null
$prepared = $false
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $arm -Force -ErrorAction SilentlyContinue

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

function Write-State($document) {
    $json = $document | ConvertTo-Json
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($json)
    $stream = [System.IO.FileStream]::new(
        [System.IO.Path]::GetFullPath($statePath), 'Create', 'Write', 'None')
    try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) }
    finally { $stream.Dispose() }
}

try {
    Write-Step 'start the service with the P4 pause installed'
    Remove-ScService
    $running = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) { Fail "the guest already has $($running.Count) worker(s); reset it first" }
    $env:MYRD_M5_TEST_PAUSE_AT = 'p4-commit'
    $env:MYRD_M5_TEST_PAUSE_FILE = $marker
    $env:MYRD_M5_TEST_PAUSE_ARM = $arm
    $env:MYRD_M5_TEST_PAUSE_SECONDS = '300'
    $env:MYRD_TRACE_FILE = Join-Path $workRoot 'trace-prepare.log'
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog `
        -RedirectStandardOutput (Join-Path $workRoot 'service.out.log') `
        -WindowStyle Hidden -PassThru
    $listed = Wait-ForService 120
    if ($null -eq $listed) { Fail "the service never answered a list command: $((Get-Content -LiteralPath $serviceLog -Raw -ErrorAction SilentlyContinue))" }
    $existing = (($listed.output -split "`n") | Where-Object { $_ -match 'generation=' })
    if ($existing.Count -ne 0) { Fail "the guest already has $($existing.Count) profile(s); reset it first" }

    $mount = Select-FreeLetter
    if ($null -eq $mount) { Fail 'no free drive letter' }
    Write-Reading 'mount' $mount

    Write-Step "create a 64 MiB disk at $mount"
    $create = Invoke-Cli @('disk', 'create', '--name', $profileName,
        '--bundle', $bundlePath, '--size', '64MiB', '--mount', $mount,
        '--checkpoint-interval', '30m')
    if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the create carried no operation id' }
    if ((Wait-ForTerminal $createId 300) -ne 'succeeded') { Fail 'the create did not succeed' }
    $profileLine = Get-DiskLine $mount
    if ($null -eq $profileLine) { Fail "disk list did not report ${mount}" }
    $profileId = Get-UuidFromText $profileLine
    $generationBefore = [uint64](Get-Field $profileLine 'generation')
    Write-Reading 'profile_id' $profileId
    Write-Reading 'generation_before' ([string]$generationBefore)

    # The marker is written before P4, flushed through the volume, and its exact bytes are
    # what a recovered candidate generation must contain.
    $payload = 'p4p5 marker ' + [guid]::NewGuid().ToString('N')
    $filePath = Join-Path ($mount + '\') 'p4p5-marker.txt'
    [System.IO.File]::WriteAllText($filePath, $payload, [System.Text.Encoding]::UTF8)
    $writeStream = [System.IO.File]::Open($filePath, 'Open', 'ReadWrite', 'Read')
    try { $writeStream.Flush($true) } finally { $writeStream.Dispose() }

    # Arm only after the create: the create performs its own first checkpoint and an armed
    # pause would freeze the create instead of the checkpoint we want to cut.
    Set-Content -LiteralPath $arm -Value 'armed' -Encoding ascii

    Write-Step 'start a checkpoint and wait for it to reach P4'
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $profileId, '--no-wait')
    if ($checkpoint.exit -ne 0) { Fail "checkpoint failed to start: $($checkpoint.output)" }
    $checkpointId = Get-UuidFromText $checkpoint.output
    if ($null -eq $checkpointId) { Fail 'the checkpoint carried no operation id' }

    $until = (Get-Date).AddSeconds(180)
    $pauseText = $null
    while ((Get-Date) -lt $until) {
        if (Test-Path -LiteralPath $marker) {
            $text = Get-Content -LiteralPath $marker -Raw -ErrorAction SilentlyContinue
            if ($null -ne $text -and $text -match 'generation=(\d+)') { $pauseText = $text.Trim(); break }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $pauseText) { Fail 'the worker never reached the P4 pause' }
    $candidate = [uint64]([regex]::Match($pauseText, 'generation=(\d+)').Groups[1].Value)
    Write-Reading 'pause_marker' $pauseText
    Write-Reading 'candidate_generation' ([string]$candidate)
    if ($candidate -le $generationBefore) {
        Fail "the P4 candidate generation ($candidate) did not advance past the last committed ($generationBefore)"
    }

    Write-State ([ordered]@{
        profile_id            = $profileId
        profile_name          = $profileName
        mount                 = $mount
        bundle                = $bundlePath
        generation_before     = $generationBefore
        candidate_generation  = $candidate
        checkpoint_operation  = $checkpointId
        marker_path           = 'p4p5-marker.txt'
        marker_payload        = $payload
        service_log           = $serviceLog
    })
    Write-Host "  state written to $statePath"
    $prepared = $true
    Write-Host 'P4_REACHED'
    Write-Reading 'result' 'PREPARED'
} finally {
    if (-not $prepared) {
        if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
            Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
        }
    }
    # On success the service and its paused worker are deliberately left running: the power
    # cut is the next step, and stopping them would destroy the window.
}
