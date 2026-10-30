# integration.m5.process_kills -- R-04: a worker or the service killed inside the P4 window.
#
# **Guest-only.**  It creates a real worker, a real AIM disk and a real Windows volume.
#
# The interesting moment for a process kill is the one R-02 cuts the power in: the commit
# has landed durably (P4) and the service has not yet recorded the evidence or published the
# epoch (P5).  The worker offers a deterministic pause there through
# `MYRD_M5_TEST_PAUSE_AT=p4-commit` (*m5_worker_runtime.cpp*), so this script does not have
# to win a race: it waits for the marker file, then kills.
#
# The two kill targets are two invocations of this script (`-Part A` / `-Part B`), not two
# halves of one run: after either kill the guest's storage stack needs a hard reboot before
# another kill experiment can run, and a recovered volume must not be reused as the next
# experiment's subject.  The full suite gives each part its own group and reset.
#
# Part A: kill the worker inside P4.  The service must not hang, the checkpoint must settle
#         (an indeterminate commit is `reconciling` by design -- only the recovery path may
#         say whether it landed), and a service restart must resolve it and bring the disk
#         back at (or past) the committed generation with the marker bytes intact.
# Part B: kill the service inside P4.  The worker is a child of the service and must not
#         survive, and the restarted service's own recovery must produce a startable disk.
#
# Part C (three consecutive hard power cuts) is host-driven: a guest script cannot cut its
# own power.  That half lives in `tools/m5vm/Test-M5RepeatedPowerLoss.ps1`.
#
# Everything is printed as `READING: <key>=<value>` so the validation document can quote
# the transcript.

param(
    [ValidateSet('A', 'B')]
    [string]$Part = 'A',
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.process_kills creates a real worker, a real AIM disk and a real ' +
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

$workRoot = 'C:\m5\process-kill-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$bundlePath = Join-Path $workRoot 'bundle'
$profileName = 'kill-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$marker = Join-Path $workRoot 'p4-marker.txt'
$arm = Join-Path $workRoot 'p4-arm.txt'
$serviceLog = Join-Path $workRoot 'service.log'
$serviceProcess = $null
$mount = $null
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
}

function Invoke-GuestCli {
    param([string[]]$Arguments)
    return Invoke-Cli $Arguments
}

function Start-GuestService([string]$logName) {
    $previous = $env:MYRD_TRACE_FILE
    $env:MYRD_TRACE_FILE = Join-Path $workRoot $logName
    $process = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog `
        -RedirectStandardOutput (Join-Path $workRoot ($logName + '.out.log')) `
        -WindowStyle Hidden -PassThru
    $env:MYRD_TRACE_FILE = $previous
    return $process
}

function Get-DiskLine([string]$letter) {
    $listing = Invoke-GuestCli @('disk', 'list')
    if ($listing.exit -ne 0) { return $null }
    return (($listing.output -split "`n") |
        Where-Object { $_ -match ('\s' + [regex]::Escape($letter)) } |
        Select-Object -First 1)
}

# A letter that is free *and* not the one a stopped profile still names as its mount target.
# Two profiles may not share a letter in this test, because `disk list` would otherwise be
# ambiguous for every helper that looks a disk up by its mount.
function Select-OtherFreeLetter([string]$exclude) {
    if (-not ([System.Management.Automation.PSTypeName]'Myrd.DosDeviceKill').Type) {
        Add-Type -Namespace Myrd -Name DosDeviceKill -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern int QueryDosDevice(string name, System.Text.StringBuilder target, int max);
'@
    }
    $occupied = @()
    foreach ($volume in (Get-CimInstance -ClassName Win32_Volume -ErrorAction Stop)) {
        if ($volume.DriveLetter) { $occupied += $volume.DriveLetter.ToUpper() }
    }
    foreach ($disk in (Get-CimInstance -ClassName Win32_LogicalDisk -ErrorAction Stop)) {
        if ($disk.DeviceID) { $occupied += $disk.DeviceID.ToUpper() }
    }
    foreach ($letter in @('U:', 'V:', 'W:', 'X:', 'Y:', 'Z:', 'T:')) {
        if ($letter -eq $exclude) { continue }
        if ($occupied -contains $letter) { continue }
        $buffer = New-Object System.Text.StringBuilder 512
        if ([Myrd.DosDeviceKill]::QueryDosDevice($letter, $buffer, 512) -ne 0) { continue }
        return $letter
    }
    return $null
}

function Get-Field([string]$line, [string]$name) {
    if ($null -eq $line) { return $null }
    $match = [regex]::Match($line, $name + '=([^\s]+)')
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value
}

function Get-Generation([string]$letter) {
    $line = Get-DiskLine $letter
    if ($null -eq $line) { return $null }
    $value = Get-Field $line 'generation'
    if ($null -eq $value) { return $null }
    return [uint64]$value
}

function Wait-Helper([int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { Start-Sleep -Milliseconds 500 }
}

function Stop-ServiceProcess {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
    $serviceProcess = $null
    # Disarm before the next service starts.  The pause checks the arm file at the moment a
    # commit lands, not at process start, so removing it is what keeps the *recovery*
    # commits from pausing in the window this test is trying to measure.
    Remove-Item -LiteralPath $arm -Force -ErrorAction SilentlyContinue
    Wait-Helper 2
}

function Assert-NoWorkers([string]$when) {
    $until = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $until) {
        $count = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count
        if ($count -eq 0) {
            Write-Reading "$when.workers_left" '0'
            return
        }
        Start-Sleep -Milliseconds 500
    }
    $left = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count
    Fail "$when left $left worker(s) behind after 60 s"
}

# `reconciling` is the *designed* durable answer to an indeterminate commit: only the
# recovery path may say whether it landed, so counting it as "hung" would fail the product
# for being honest.  It is settled, not terminal.
function Wait-ForSettled([string]$operationId, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    $state = 'unknown'
    while ((Get-Date) -lt $until) {
        $state = Get-OperationState $operationId
        if (@('succeeded', 'failed', 'cancelled', 'reconciling') -contains $state) { return $state }
        Start-Sleep -Milliseconds 500
    }
    Fail "the operation did not settle within $seconds s (last state: $state)"
}

function Wait-ForTerminalOrFail([string]$operationId, [int]$seconds, [string]$what) {
    $until = (Get-Date).AddSeconds($seconds)
    $state = 'unknown'
    while ((Get-Date) -lt $until) {
        $state = Get-OperationState $operationId
        if (@('succeeded', 'failed', 'cancelled') -contains $state) { return $state }
        Start-Sleep -Milliseconds 500
    }
    Fail "$what did not reach a terminal state within $seconds s (last state: $state)"
}

function Wait-ForRuntime([string]$expected, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    $runtime = $null
    while ((Get-Date) -lt $until) {
        $runtime = Get-Field (Get-DiskLine $mount) 'runtime'
        if ($runtime -eq $expected) { return $true }
        Start-Sleep -Seconds 1
    }
    Write-Reading 'unexpected_runtime' ([string]$runtime)
    return $false
}

# The pause is installed at service start, because the service launches the worker and the
# child inherits the environment (null environment block, CREATE_UNICODE_ENVIRONMENT).
$env:MYRD_M5_TEST_PAUSE_AT = 'p4-commit'
$env:MYRD_M5_TEST_PAUSE_FILE = $marker
$env:MYRD_M5_TEST_PAUSE_ARM = $arm
$env:MYRD_M5_TEST_PAUSE_SECONDS = '300'

function Clear-PauseMarker {
    Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue
}

function Wait-ForPauseMarker([int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        if (Test-Path -LiteralPath $marker) {
            # The writer opens with CREATE_ALWAYS and writes once; a reader that catches
            # the empty file just retries.  The reading is taken once it has content.
            $text = Get-Content -LiteralPath $marker -Raw -ErrorAction SilentlyContinue
            if ($null -ne $text -and $text -match 'generation=(\d+)') {
                return $text.Trim()
            }
        }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

try {
    Write-Step 'start the service with the P4 pause installed'
    Remove-ScService
    $running = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) { Fail "the guest already has $($running.Count) worker(s); reset it first" }
    $serviceProcess = Start-GuestService 'trace-a.log'
    $listed = Wait-ForService 90
    if ($null -eq $listed) { Fail "the service never answered a list command: $((Get-Content -LiteralPath $serviceLog -Raw -ErrorAction SilentlyContinue))" }
    $existing = (($listed.output -split "`n") | Where-Object { $_ -match 'generation=' })
    if ($existing.Count -ne 0) { Fail "the guest already has $($existing.Count) profile(s); reset it first" }

    $mount = Select-FreeLetter
    if ($null -eq $mount) { Fail 'no free drive letter' }
    Write-Reading 'mount' $mount

    Write-Step "create a 64 MiB disk at $mount"
    $create = Invoke-GuestCli @('disk', 'create', '--name', $profileName,
        '--bundle', $bundlePath, '--size', '64MiB', '--mount', $mount,
        '--checkpoint-interval', '30m')
    if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the create carried no operation id' }
    if ((Wait-ForTerminalOrFail $createId 300 'the create') -ne 'succeeded') { Fail 'the create did not succeed' }
    $profileLine = Get-DiskLine $mount
    if ($null -eq $profileLine) { Fail "disk list did not report ${mount}" }
    $profileId = Get-UuidFromText $profileLine
    Write-Reading 'profile_id' $profileId
    $generationBefore = Get-Generation $mount
    Write-Reading 'generation_before' ([string]$generationBefore)

    # **Arm the window only now.**  `disk create` performs its own first checkpoint, so a
    # pause armed at service start would freeze the create (measured: `reconciling` for the
    # full pause).  Arming after the create is what makes the checkpoint the paused one.
    Set-Content -LiteralPath $arm -Value 'armed' -Encoding ascii

    $payload = 'committed before the p4 kill ' + [guid]::NewGuid().ToString('N')
    $filePath = Join-Path ($mount + '\') 'p4-marker.txt'
    [System.IO.File]::WriteAllText($filePath, $payload, [System.Text.Encoding]::UTF8)
    # Flush the file itself, so "the bytes are on the volume" is true before P4 is entered.
    $writeStream = [System.IO.File]::Open($filePath, 'Open', 'ReadWrite', 'Read')
    try { $writeStream.Flush($true) } finally { $writeStream.Dispose() }

    $killTarget = if ($Part -eq 'A') { 'worker' } else { 'service' }
    Write-Step "kill the $killTarget inside the P4 window"
    Clear-PauseMarker
    $checkpointA = Invoke-GuestCli @('disk', 'checkpoint', $profileId, '--no-wait')
    if ($checkpointA.exit -ne 0) { Fail "the checkpoint failed to start: $($checkpointA.output)" }
    $checkpointAId = Get-UuidFromText $checkpointA.output
    if ($null -eq $checkpointAId) { Fail 'the checkpoint carried no operation id' }
    $pauseA = Wait-ForPauseMarker 180
    if ($null -eq $pauseA) { Fail 'the worker never reached the P4 pause; no kill window' }
    Write-Reading 'pause_marker' $pauseA
    $pauseMatch = [regex]::Match($pauseA, 'generation=(\d+)')
    $candidateA = [uint64]$pauseMatch.Groups[1].Value
    Write-Reading 'candidate_generation' ([string]$candidateA)

    if ($Part -eq 'A') {
        $worker = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
        if ($worker.Count -ne 1) { Fail "expected exactly one worker to kill, found $($worker.Count)" }
        Stop-Process -Id $worker[0].Id -Force
        Write-Reading 'killed_worker_pid' ([string]$worker[0].Id)

        $settledA = Wait-ForSettled $checkpointAId 180
        Write-Reading 'operation_state_after_kill' $settledA
        $listAfterA = Invoke-GuestCli @('disk', 'list')
        Write-Reading 'list_exit' ([string]$listAfterA.exit)
        Write-Reading 'list_ms' ([string]$listAfterA.ms)
        if ($listAfterA.exit -ne 0) { Fail "the service did not answer disk list after the worker was killed: $($listAfterA.output)" }
        $lineAfterA = Get-DiskLine $mount
        Write-Reading 'runtime_after_kill' ([string](Get-Field $lineAfterA 'runtime'))
        Write-Reading 'persistence_after_kill' ([string](Get-Field $lineAfterA 'persistence'))
        Assert-NoWorkers 'a'
    } else {
        $serviceId = $serviceProcess.Id
        Stop-Process -Id $serviceId -Force
        Write-Reading 'killed_service_pid' ([string]$serviceId)
        $serviceProcess = $null
        # The worker is a child of the service's job object; losing the service must not leave
        # a worker that owns the bundle.
        Assert-NoWorkers 'b'
    }

    Write-Step "restart the service and let its recovery resolve the checkpoint"
    Stop-ServiceProcess
    $serviceProcess = Start-GuestService 'trace-recovery.log'
    if ($null -eq (Wait-ForService 120)) { Fail 'the restarted service never answered' }
    $resolvedA = Wait-ForTerminalOrFail $checkpointAId 300 'the checkpoint after the service restart'
    Write-Reading 'recovery.operation_state' $resolvedA
    # The disk may come back by itself (its create intent asked for auto-start) or need the
    # start command; either way a start on an already-running profile is idempotent.
    $startA = Invoke-GuestCli @('disk', 'start', $profileId)
    Write-Reading 'recovery.start_exit' ([string]$startA.exit)
    if ($startA.exit -ne 0) { Fail "disk start after the kill failed: $($startA.output)" }
    $startAId = Get-UuidFromText $startA.output
    if ($null -ne $startAId) { [void](Wait-ForTerminalOrFail $startAId 300 'the start after the kill') }
    if (-not (Wait-ForRuntime 'Running' 120)) { Fail 'the disk did not come back Running after the kill' }
    $generationAfterA = Get-Generation $mount
    Write-Reading 'recovery.generation' ([string]$generationAfterA)
    if ($generationAfterA -lt $candidateA) {
        Fail "recovery went below the candidate generation: candidate $candidateA, recovered $generationAfterA"
    }
    if (-not (Test-Path -LiteralPath $filePath)) { Fail 'the committed file is missing after the recovery' }
    if ((Get-Content -LiteralPath $filePath -Raw) -ne $payload) { Fail 'the committed file changed across the recovery' }
    Write-Reading 'recovery.data_intact' 'yes'

    Write-Step 'cleanup: stop and forget the disk'
    $stop = Invoke-GuestCli @('disk', 'stop', $profileId)
    if ($stop.exit -eq 0) {
        $stopId = Get-UuidFromText $stop.output
        if ($null -ne $stopId) { [void](Wait-ForTerminalOrFail $stopId 300 'the cleanup stop') }
    }
    $forget = Invoke-GuestCli @('disk', 'forget', $profileId, '--confirmation', $profileId)
    Write-Reading 'cleanup_forget' ([string]$forget.exit)

    Write-Host "PASS integration.m5.process_kills: the $killTarget was killed inside P4 and recovery reached the candidate generation"
    Write-Reading 'result' 'PASS'
} finally {
    Stop-ServiceProcess
    Remove-Item Env:MYRD_M5_TEST_PAUSE_AT -ErrorAction SilentlyContinue
    Remove-Item Env:MYRD_M5_TEST_PAUSE_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:MYRD_M5_TEST_PAUSE_ARM -ErrorAction SilentlyContinue
    Remove-Item Env:MYRD_M5_TEST_PAUSE_SECONDS -ErrorAction SilentlyContinue
}
