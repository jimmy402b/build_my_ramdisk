# integration.m5.performance -- the performance baseline R-12 asks for, measured on the guest
# rather than asserted from a spec.
#
# **Guest-only.**  It creates a real worker, a real AIM disk and a real Windows volume, writes
# 256 MiB through the volume and commits it.
#
# What is measured (all wall-clock, from this process, on the guest):
#   create    admission -> terminal
#   start     admission -> terminal, plus the worker process working set afterwards
#   write     256 MiB through T: with a 4 MiB buffer, MiB/s
#   checkpoint flush + commit of that dirty volume, milliseconds
#   stop      admission -> terminal
#
# The bounds are deliberately loose: this case exists to catch an order-of-magnitude regression
# and to publish a baseline, not to fail on a slow VM day.  The readings are the deliverable.

param(
    [string]$Configuration = 'Release',
    [int]$SizeMiB = 64,
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
        'integration.m5.performance creates a real worker, AIM disk and volume; it may only ' +
        'run on the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

$workRoot = 'C:\m5\perf-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$bundlePath = Join-Path $workRoot 'bundle'
$serviceLog = Join-Path $workRoot 'service.log'
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$serviceProcess = $null

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
        if ($show.exit -ne 0) { Fail "operation show failed: $($show.output)" }
        $match = [regex]::Match($show.output, ': (succeeded|completed|failed|cancelled)')
        if ($match.Success) {
            if ($match.Groups[1].Value -eq 'completed') { return 'succeeded' }
            return $match.Groups[1].Value
        }
        Start-Sleep -Milliseconds 200
    }
    return 'timeout'
}

# Runs one command that is admitted asynchronously and returns (state, elapsed ms).
function Invoke-TimedOperation([string[]]$Arguments) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $result = Invoke-Cli $Arguments
    if ($result.exit -ne 0) { Fail "command failed: $($result.output)" }
    $id = Get-UuidFromText $result.output
    if ($null -eq $id) { Fail "the command carried no operation id: $($result.output)" }
    $state = Wait-ForTerminal $id 300
    $watch.Stop()
    return [pscustomobject]@{ state = $state; ms = [int]$watch.Elapsed.TotalMilliseconds }
}

try {
    Write-Step 'start the service'
    Remove-ScService
    $running = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) { Fail "the guest already has $($running.Count) worker(s); reset it first" }
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog `
        -RedirectStandardOutput (Join-Path $workRoot 'service.out.log') `
        -WindowStyle Hidden -PassThru
    $listed = Wait-ForService 90
    if ($null -eq $listed) { Fail 'the service never answered' }
    $profiles = @(($listed.output -split "`n") | Where-Object { $_ -match 'generation=' })
    if ($profiles.Count -ne 0) { Fail "the guest already has $($profiles.Count) profile(s); reset it first" }

    $mount = Select-FreeLetter
    if ($null -eq $mount) { Fail 'no free drive letter' }
    Write-Reading 'mount' $mount
    Write-Reading 'size_mib' ([string]$SizeMiB)

    Write-Step 'create'
    $create = Invoke-TimedOperation @('disk', 'create', '--name', 'perf',
        '--bundle', $bundlePath, '--size', ([string]$SizeMiB + 'MiB'),
        '--mount', $mount, '--checkpoint-interval', '30m')
    Write-Reading 'create.state' $create.state
    Write-Reading 'create.ms' ([string]$create.ms)
    if ($create.state -ne 'succeeded') { Fail "create ended $($create.state)" }
    if ($create.ms -gt 60000) { Fail "create took $($create.ms) ms" }
    # **The list is retried.**  Measured: a create that reported `succeeded` was not in the very
    # next `disk list` on this VM, which left the profile id empty and the start failed with a
    # usage error.  The terminal state is the durable truth; the list is a projection and can lag.
    $profileId = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $listing = Invoke-Cli @('disk', 'list')
        $profileId = Get-UuidFromText $listing.output
        if ($null -ne $profileId) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($null -eq $profileId) { Fail "the created profile never appeared in disk list: $($listing.output)" }
    Write-Reading 'profile_id' $profileId

    Write-Step 'start'
    $start = Invoke-TimedOperation @('disk', 'start', $profileId)
    Write-Reading 'start.state' $start.state
    Write-Reading 'start.ms' ([string]$start.ms)
    if ($start.state -ne 'succeeded') { Fail "start ended $($start.state)" }
    if ($start.ms -gt 60000) { Fail "start took $($start.ms) ms" }
    $workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    Write-Reading 'worker.count' ([string]$workers.Count)
    if ($workers.Count -eq 0) { Fail 'start left no worker process' }
    $rss = 0L
    foreach ($worker in $workers) { $rss += $worker.WorkingSet64 }
    Write-Reading 'worker.working_set_mib' ([string][math]::Round($rss / 1MB, 1))
    if ($rss -gt 512MB) { Fail "the worker working set is $([math]::Round($rss/1MB)) MiB" }

    Write-Step "write $SizeMiB MiB through the volume"
    # **Leave room for the filesystem.**  Measured: writing the full logical size failed with
    # "insufficient disk space" -- NTFS metadata and the volume's own bookkeeping do not fit in
    # the last few MiB, so the payload is 32 MiB smaller than the disk.
    $writeMiB = [Math]::Max(1, $SizeMiB - 32)
    Write-Reading 'write.mib_requested' ([string]$writeMiB)
    $filePath = $mount.TrimEnd('\') + '\perf-payload.bin'
    $chunk = New-Object byte[] (4MB)
    $stream = [System.IO.File]::Open($filePath, 'Create', 'Write', 'None')
    $writeWatch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $remaining = [int64]$writeMiB * 1MB
        while ($remaining -gt 0) {
            $count = [int][Math]::Min($chunk.Length, $remaining)
            $stream.Write($chunk, 0, $count)
            $remaining -= $count
        }
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
    $writeWatch.Stop()
    $seconds = [Math]::Max($writeWatch.Elapsed.TotalSeconds, 0.001)
    $mibPerSecond = [math]::Round($writeMiB / $seconds, 1)
    Write-Reading 'write.ms' ([string][int]$writeWatch.Elapsed.TotalMilliseconds)
    Write-Reading 'write.mib_per_s' ([string]$mibPerSecond)
    if ($mibPerSecond -lt 50) { Fail "write throughput is $mibPerSecond MiB/s" }

    Write-Step 'checkpoint (flush + commit of a dirty volume)'
    $checkpoint = Invoke-TimedOperation @('disk', 'checkpoint', $profileId)
    Write-Reading 'checkpoint.state' $checkpoint.state
    Write-Reading 'checkpoint.ms' ([string]$checkpoint.ms)
    if ($checkpoint.state -ne 'succeeded') { Fail "checkpoint ended $($checkpoint.state)" }
    if ($checkpoint.ms -gt 120000) { Fail "checkpoint took $($checkpoint.ms) ms" }

    Write-Step 'stop'
    $stop = Invoke-TimedOperation @('disk', 'stop', $profileId)
    Write-Reading 'stop.state' $stop.state
    Write-Reading 'stop.ms' ([string]$stop.ms)
    if ($stop.state -ne 'succeeded') { Fail "stop ended $($stop.state)" }
    if ($stop.ms -gt 60000) { Fail "stop took $($stop.ms) ms" }

    $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
    Write-Reading 'cleanup_forget' ([string]$forget.exit)

    Write-Host 'PASS integration.m5.performance: baseline recorded'
    Write-Reading 'result' 'PASS'
} finally {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
}

