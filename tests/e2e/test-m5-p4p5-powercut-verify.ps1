# The second half of R-02: after the hard power cut, look at what came back.
#
# **Guest-only.**  It never starts the disk until it has read what the machine recovered to;
# the claim being measured is the product's recovery, not this script's repair.
#
# The acceptance, in the task list's words: every cut lands on the **last committed
# generation** or the **candidate generation** named by the P4 marker, and never on a third
# state -- no "neither committed nor rolled back", no unbounded `reconciling`, no disk that
# needs a manual repair to start.  The candidate is what a landed P4 must produce; the last
# committed generation is recorded as the honest losing branch if it happens.

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
        'integration.m5.p4p5_verify creates a real worker, a real AIM disk and a real ' +
        'Windows volume, and may only run on the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
$serviceName = 'MyRamDiskService'
$statePath = 'C:\m5\p4p5-state.json'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

foreach ($path in @($serviceExe, $cliExe)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "missing binary: $path" }
}
if (-not (Test-Path -LiteralPath $statePath)) { throw "the power-cut state is missing: $statePath" }

$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
$profileId = [string]$state.profile_id
$mount = [string]$state.mount
$bundlePath = [string]$state.bundle
$generationBefore = [uint64]$state.generation_before
$candidate = [uint64]$state.candidate_generation
$checkpointId = [string]$state.checkpoint_operation
$payload = [string]$state.marker_payload
# **Plain string, not `Join-Path`.**  `Join-Path 'T:\' ...` validates that the drive
# exists, and after the power cut it does not until recovery starts the disk -- measured as
# a DriveNotFoundException before the first reading was taken.
$markerPath = $mount.TrimEnd('\') + '\p4p5-marker.txt'
$workRoot = Split-Path -Parent $bundlePath
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

try {
    Write-Reading 'generation_before' ([string]$generationBefore)
    Write-Reading 'candidate_generation' ([string]$candidate)

    Write-Step 'the cut must have killed every M5 process'
    $survivors = @(Get-Process -Name 'myrd-service', 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    Write-Reading 'survivors_after_cut' ([string]$survivors.Count)
    if ($survivors.Count -ne 0) {
        Fail ("$($survivors.Count) M5 process(es) survived a hard power cut: " +
            (($survivors | ForEach-Object { "$($_.Name)/$($_.Id)" }) -join ', '))
    }

    Write-Step 'start the service exactly as the product would, with no test pause'
    $env:MYRD_TRACE_FILE = Join-Path $workRoot 'trace-verify.log'
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError (Join-Path $workRoot 'service-verify.log') `
        -RedirectStandardOutput (Join-Path $workRoot 'service-verify.out.log') `
        -WindowStyle Hidden -PassThru
    if ($null -eq (Wait-ForService 180)) {
        Fail ('the service did not come back after the power cut: ' +
            (Get-Content -LiteralPath (Join-Path $workRoot 'service-verify.log') -Raw -ErrorAction SilentlyContinue))
    }

    Write-Step 'wait for the recovered generation to settle on a legal state'
    $deadline = (Get-Date).AddSeconds(180)
    $generation = $null
    $runtime = $null
    while ((Get-Date) -lt $deadline) {
        $line = Get-DiskLine $mount
        if ($null -ne $line) {
            $value = Get-Field $line 'generation'
            if ($null -ne $value) { $generation = [uint64]$value }
            $runtime = Get-Field $line 'runtime'
            # PowerShell 5.1 cannot parse a line that *starts* with a binary operator;
            # the operator has to end the previous line.
            if ($null -ne $generation -and
                $generation -ge $generationBefore -and
                $runtime -ne 'Orphaned') { break }
        }
        Start-Sleep -Seconds 1
    }
    if ($null -eq $generation) { Fail 'the recovered profile never reported a generation' }
    Write-Reading 'recovered_generation' ([string]$generation)
    Write-Reading 'recovered_runtime' ([string]$runtime)
    if ($generation -eq $candidate) {
        Write-Reading 'recovery_outcome' 'candidate'
    } elseif ($generation -eq $generationBefore) {
        Write-Reading 'recovery_outcome' 'last_committed'
    } else {
        Fail ("the recovered generation $generation is neither the last committed " +
            "$generationBefore nor the P4 candidate $candidate (third state)")
    }

    # The checkpoint that was interrupted must be resolved, not left `reconciling` forever:
    # the recovery path is allowed to take a while, but not to be unbounded.
    $operationDeadline = (Get-Date).AddSeconds(180)
    $operationState = 'unknown'
    while ((Get-Date) -lt $operationDeadline) {
        $show = Invoke-Cli @('operation', 'show', $checkpointId)
        foreach ($word in @('succeeded', 'failed', 'cancelled')) {
            if ($show.output -match "operation $checkpointId`: $word") { $operationState = $word; break }
        }
        if ($operationState -ne 'unknown') { break }
        Start-Sleep -Seconds 1
    }
    Write-Reading 'checkpoint_operation_state' $operationState
    if ($operationState -eq 'unknown') {
        Fail 'the interrupted checkpoint never reached a terminal state after recovery'
    }

    Write-Step 'the recovered disk must start and serve the committed bytes'
    $start = Invoke-Cli @('disk', 'start', $profileId)
    Write-Reading 'start_exit' ([string]$start.exit)
    if ($start.exit -ne 0) { Fail "disk start after the power cut failed: $($start.output)" }
    $startId = Get-UuidFromText $start.output
    if ($null -ne $startId) {
        $startState = Wait-ForTerminal $startId 300
        Write-Reading 'start_state' $startState
        if ($startState -ne 'succeeded') { Fail "disk start after the power cut ended $startState" }
    }
    $runningDeadline = (Get-Date).AddSeconds(120)
    $running = $false
    while ((Get-Date) -lt $runningDeadline) {
        if ((Get-Field (Get-DiskLine $mount) 'runtime') -eq 'Running') { $running = $true; break }
        Start-Sleep -Seconds 1
    }
    Write-Reading 'disk_running' ([string]$running)
    if (-not $running) { Fail 'the recovered disk did not reach Running' }

    $present = Test-Path -LiteralPath $markerPath
    Write-Reading 'marker_present' ([string]$present)
    if ($generation -eq $candidate) {
        # The candidate generation was committed at P4, so the marker written before it must
        # be there, byte for byte.  This is what separates "recovered to the candidate" from
        # "recovered to a generation that merely has the same number".
        if (-not $present) { Fail 'the recovered candidate generation does not contain the pre-P4 marker file' }
        $text = Get-Content -LiteralPath $markerPath -Raw
        if ($text -ne $payload) { Fail 'the recovered marker bytes differ from the committed ones' }
        Write-Reading 'marker_bytes_intact' 'yes'
    } else {
        Write-Reading 'marker_bytes_intact' 'not-applicable'
    }

    Write-Step 'cleanup: stop and forget the disk'
    $stop = Invoke-Cli @('disk', 'stop', $profileId)
    if ($stop.exit -eq 0) {
        $stopId = Get-UuidFromText $stop.output
        if ($null -ne $stopId) { [void](Wait-ForTerminal $stopId 300) }
    }
    $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
    Write-Reading 'cleanup_forget' ([string]$forget.exit)

    Write-Host 'PASS integration.m5.p4p5_verify: the P4/P5 cut recovered to a legal generation'
    Write-Reading 'result' 'PASS'
} finally {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
