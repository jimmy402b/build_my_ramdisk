# integration.m5.bigdisk -- create a large disk end to end and, when it stalls, keep the scene.
#
# **Why this case exists.**  1.0.0 was validated on 64 MiB-1 GiB disks.  Real use found that a
# 4 GiB create succeeds while an 8 GiB create mounts its volume and then sits in
# `create-bundle` forever: worker CPU 0, no I/O, `base-0000.vhdx` frozen at 4 MiB, and the
# service's `operation wait` exchange times out.  A 16 GiB automatic create fails earlier with
# `M3Provider::open provider limits are invalid`.  None of the release cases covered > 1 GiB,
# which is why this got out.  This case is the reproduction and the regression: it drives the
# same command a user drives, samples the operation every 5 s, and -- when the create does not
# finish -- prints the readings and exits non-zero *without cleaning up*, so the stuck worker,
# the bundle and the logs can be inspected.
#
# **Guest-only.**  It installs the service and creates a real worker, AIM disk and NTFS volume.
#
# Configuration comes from files, because the harness runs a case with a filter and no
# arguments (`C:\m5\bigdisk\...`):
#   size.txt     e.g. 8GiB / 16GiB                      (default 8GiB)
#   memory.txt   `automatic` or `fixed:12GiB`           (default automatic)
#   mode.txt     `run` (default) or `cleanup`
#   mount.txt    drive letter to mount                  (default Z:)
#   observe.txt  seconds to sample before declaring a stall (default 120)
#   reuse.txt    `1` keeps an existing bundle directory (default: delete it first)
#
# Modes:
#   run      - install the service if needed, create the disk, sample it, verify the volume on
#              success.  On a stall it prints the evidence and exits 3 with the scene intact.
#   cleanup  - stop and forget the disk, uninstall the service, delete C:\m5\bigdisk.  A stuck
#              worker usually makes this fail too; that failure is itself reported, because a
#              `disk forget` that cannot answer is part of the defect's observed behaviour.
#
# Exit codes: 0 pass, 1 fail, 2 wrong host, 3 stall (scene preserved).

param(
    [string]$Configuration = 'Release',
    [string]$Root = 'C:\m5\bigdisk'
)

$ErrorActionPreference = 'Stop'

trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.bigdisk creates a real service, worker, AIM disk and volume; it may ' +
        'only run on the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

$serviceName = 'MyRamDiskService'
$installedRoot = 'C:\Program Files\MyRamDisk'
$installedCli = Join-Path $installedRoot 'myrd-cli.exe'
$dataRoot = 'C:\ProgramData\MyRamDisk'
$serviceLog = Join-Path $dataRoot 'logs\service.log'
$workerTrace = Join-Path $dataRoot 'logs\worker-trace.log'
$bundle = Join-Path $Root 'bundle'

New-Item -ItemType Directory -Force -Path $Root | Out-Null

function Read-Config([string]$name, [string]$default) {
    $path = Join-Path $Root "$name.txt"
    if (-not (Test-Path -LiteralPath $path)) { return $default }
    $value = (Get-Content -LiteralPath $path -Raw) -replace "`0", ''
    $value = $value.Trim()
    if ([string]::IsNullOrWhiteSpace($value)) { return $default }
    return $value
}

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
}

# The exit code arrives in a file written by cmd (`echo %ERRORLEVEL% > file`).  The value is
# parsed rather than cast: cmd writes a localised "ECHO is off." when the variable expanded to
# nothing, and a cast of that threw away a whole run at 4 GiB instead of reporting a reading.
function Get-ExitCode([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return -1 }
    $raw = Get-Content -LiteralPath $path -Raw -ErrorAction SilentlyContinue
    if ($raw -match '(\d+)') { return [int]$Matches[1] }
    return -1
}

function Get-ServiceQuery {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        return (& sc.exe query $serviceName 2>&1 | Out-String)
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Ensure-Service {
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        if ((Get-ServiceQuery) -match 'RUNNING') { return }
        Start-Sleep -Milliseconds 500
    }
    $install = Join-Path $repo 'tools\install\Install-MyRamDisk.ps1'
    if (-not (Test-Path -LiteralPath $install)) { Fail "the installer is missing: $install" }
    Write-Step "install the service from $bin"
    $output = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $install `
        -SourceDirectory $bin -ServiceName $serviceName 2>&1 | Out-String)
    Write-Host $output
    $deadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $deadline) {
        if ((Get-ServiceQuery) -match 'RUNNING') { return }
        Start-Sleep -Milliseconds 500
    }
    Fail ("the installed service did not start:`n" + $output)
}

function Get-ProductCli {
    if (Test-Path -LiteralPath $installedCli) { return $installedCli }
    return $cliExe
}

function Get-BundleSnapshot {
    if (-not (Test-Path -LiteralPath $bundle)) { return 'absent' }
    $parts = @()
    foreach ($file in (Get-ChildItem -LiteralPath $bundle -Recurse -File -ErrorAction SilentlyContinue |
            Sort-Object Name)) {
        $parts += ("{0}={1}" -f $file.Name, $file.Length)
    }
    if ($parts.Count -eq 0) { return 'empty' }
    return ($parts -join ',')
}

function Get-WorkerSample {
    $worker = Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $worker) { return $null }
    $io = $null
    try {
        $io = Get-CimInstance Win32_PerfRawData_PerfProc_Process `
            -Filter "IDProcess=$($worker.Id)" -ErrorAction Stop
    } catch { $io = $null }
    [pscustomobject]@{
        id         = $worker.Id
        cpu        = [double]$worker.CPU
        workingSet = [int64]$worker.WorkingSet64
        ioRead     = if ($io) { [int64]$io.IOReadBytesPersec } else { -1 }
        ioWrite    = if ($io) { [int64]$io.IOWriteBytesPersec } else { -1 }
    }
}

function Get-WorkerThreads {
    $worker = Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $worker) { return 'no worker process' }
    $parts = @()
    foreach ($thread in $worker.Threads) {
        $parts += ("{0}:{1}/{2}" -f $thread.Id, $thread.ThreadState, $thread.WaitReason)
    }
    return ($parts -join ' ')
}

function Get-OperationReading([string]$operationId) {
    # `operation show` is a short command and answers while the long operation is dispatched.
    # A failure here is data too: it is what the CLI reports while the service is stuck.
    $show = Invoke-Cli @('operation', 'show', $operationId, '--timeout', '20')
    $state = 'unknown'
    $match = [regex]::Match($show.output,
        "(?m)^operation\s+$([regex]::Escape($operationId)):\s+(\w+)")
    if ($match.Success) { $state = $match.Groups[1].Value }
    $phase = ''
    $phaseMatch = [regex]::Match($show.output, '\(phase ([^)]*)\)')
    if ($phaseMatch.Success) { $phase = $phaseMatch.Groups[1].Value }
    [pscustomobject]@{ state = $state; phase = $phase; exit = $show.exit; text = $show.output }
}

function Show-Scene([string]$operationId) {
    Write-Step 'scene'
    Write-Reading 'bundle.files' (Get-BundleSnapshot)
    Write-Reading 'worker.process' (Get-WorkerThreads)
    $workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    Write-Reading 'worker.count' ([string]$workers.Count)
    if (Test-Path -LiteralPath $serviceLog) {
        Write-Host '--- service.log (tail 30) ---'
        Get-Content -LiteralPath $serviceLog -Tail 30 -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host $_ }
    } else {
        Write-Host "service.log absent: $serviceLog"
    }
    if (Test-Path -LiteralPath $workerTrace) {
        Write-Host '--- worker-trace.log (tail 40) ---'
        Get-Content -LiteralPath $workerTrace -Tail 40 -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host $_ }
    }
    $listing = Invoke-Cli @('disk', 'list', '--timeout', '20')
    Write-Reading 'disk.list.exit' ([string]$listing.exit)
    Write-Host ($listing.output -replace "`r?`n", ' | ')
    if ($operationId) {
        $show = Invoke-Cli @('operation', 'show', $operationId, '--timeout', '20')
        Write-Reading 'operation.show.exit' ([string]$show.exit)
        Write-Host ($show.output -replace "`r?`n", ' | ')
    }
}

# ------------------------------------------------------------------ pressure / verify modes
#
# These are the second half of the large-disk acceptance: a 16 GiB disk opened with a 12 GiB
# budget has to *work* under pressure, not just be created.  `pressure` writes more than the
# soft limit (half the budget) so the store must evict, checkpoints, and records the file's
# digest; the host then cuts power; `verify` starts the disk again and compares the digest, and
# checks that the single-disk rule still refuses a second disk while this one is up.

$mode = Read-Config 'mode' 'run'
$mount = Read-Config 'mount' 'Z:'
Write-Reading 'bigdisk.mode' $mode
Write-Reading 'bigdisk.size' (Read-Config 'size' '8GiB')
Write-Reading 'bigdisk.memory' (Read-Config 'memory' 'automatic')

function Get-DiskId {
    $listing = Invoke-Cli @('disk', 'list', '--timeout', '30')
    $match = [regex]::Match($listing.output,
        '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')
    if ($match.Success) { return $match.Value }
    return $null
}

function ConvertTo-Bytes([string]$text) {
    if ($text -match '^(\d+)(KiB|MiB|GiB)$') {
        $value = [uint64]$Matches[1]
        switch ($Matches[2]) {
            'KiB' { return $value * 1KB }
            'MiB' { return $value * 1MB }
            'GiB' { return $value * 1GB }
        }
    }
    if ($text -match '^(\d+)$') { return [uint64]$Matches[1] }
    throw "not a size: $text"
}

function Wait-Operation([string]$operationId, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $state = 'unknown'
    while ((Get-Date) -lt $deadline) {
        $show = Invoke-Cli @('operation', 'show', $operationId, '--timeout', '30')
        $match = [regex]::Match($show.output,
            "(?m)^operation\s+$([regex]::Escape($operationId)):\s+(\w+)")
        if ($match.Success) { $state = $match.Groups[1].Value }
        if (@('succeeded', 'failed', 'cancelled') -contains $state) { return $state }
        Start-Sleep -Seconds 2
    }
    return $state
}

$expectedFile = Join-Path $Root 'expected.sha256'
$dataFile = Join-Path $Root 'bigdisk.bin'

if ($mode -eq 'probe') {
    # Read-only apart from an optional start: the diagnostic entry point used when a phase
    # failed and the scene has to be described before the guest is reset.
    Ensure-Service
    $id = Get-DiskId
    Write-Reading 'probe.id' ([string]$id)
    $listing = Invoke-Cli @('disk', 'list', '--timeout', '60')
    Write-Reading 'probe.list_exit' ([string]$listing.exit)
    Write-Host ($listing.output -replace "`r?`n", ' | ')
    if ($id) {
        $show = Invoke-Cli @('disk', 'show', $id, '--timeout', '60')
        Write-Host ($show.output -replace "`r?`n", ' | ')
    }
    if ((Read-Config 'start' '0') -eq '1' -and $id) {
        $started = Invoke-Cli @('disk', 'start', $id, '--timeout', '60')
        Write-Reading 'probe.start_exit' ([string]$started.exit)
        $startId = Get-UuidFromText $started.output
        if ($startId) { Write-Reading 'probe.start_state' (Wait-Operation $startId 600) }
    }
    Write-Reading 'probe.result' 'PASS'
    exit 0
}

if ($mode -eq 'write') {
    # The isolated write experiment: no checkpoint, no assertions, one size from `write.txt`.
    # It exists because the pressure phase stalled and the question "does any file write to this
    # volume complete?" has to be answered before "does eviction work?" can be.
    Ensure-Service
    $volumeLetter = $mount.Substring(0, 1).ToUpperInvariant()
    $id = Get-DiskId
    if ($null -eq $id) { Fail 'no registered disk to write to' }
    $writeBytes = ConvertTo-Bytes (Read-Config 'write' '256MiB')
    $target = Join-Path "$($volumeLetter):\" 'write-probe.bin'
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    if ($os) {
        Write-Reading 'write.free_mb' ([string][int]($os.FreePhysicalMemory / 1KB))
        Write-Reading 'write.total_mb' ([string][int]($os.TotalVisibleMemorySize / 1KB))
    }
    $block = New-Object byte[] (1MB)
    [System.Random]::new(7).NextBytes($block)
    Write-Step "write $writeBytes bytes to $target"
    $startedWrite = Get-Date
    $stream = New-Object System.IO.FileStream($target,
        [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None, 4MB, [System.IO.FileOptions]::WriteThrough)
    try {
        [uint64]$written = 0
        while ($written -lt $writeBytes) {
            $chunk = [int][Math]::Min([uint64]$block.Length, $writeBytes - $written)
            $stream.Write($block, 0, $chunk)
            $written += [uint64]$chunk
            if (($written % 64MB) -lt $block.Length) {
                $stream.Flush($true)
                Write-Reading 'write.progress' ("{0}B in {1}s" -f $written,
                    [int]((Get-Date) - $startedWrite).TotalSeconds)
            }
        }
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
    Write-Reading 'write.elapsed_seconds' ([string][int]((Get-Date) - $startedWrite).TotalSeconds)
    Write-Reading 'write.file_bytes' ([string](Get-Item -LiteralPath $target).Length)
    $show = Invoke-Cli @('disk', 'show', $id, '--timeout', '60')
    Write-Reading 'write.show' (($show.output -replace "`r?`n", ' | '))
    Write-Reading 'write.result' 'PASS'
    Write-Host 'PASS integration.m5.bigdisk (write)'
    exit 0
}

if ($mode -eq 'pressure') {
    Ensure-Service
    $volumeLetter = $mount.Substring(0, 1).ToUpperInvariant()
    if ($null -eq (Get-Volume -DriveLetter $volumeLetter -ErrorAction SilentlyContinue)) {
        Fail "no volume at $mount; run the create first"
    }
    $id = Get-DiskId
    if ($null -eq $id) { Fail 'no registered disk to press' }
    Write-Reading 'pressure.id' $id
    $writeBytes = ConvertTo-Bytes (Read-Config 'write' '7GiB')
    Write-Reading 'pressure.write_bytes' ([string]$writeBytes)
    $target = Join-Path "$($volumeLetter):\" 'bigdisk.bin'
    Write-Step "write $writeBytes bytes to $target"
    # A deterministic 1 MiB block repeated is enough for a byte-for-byte digest check and lets
    # the write loop stay in native code: filling gigabytes from PowerShell would take longer
    # than the test's whole budget.
    $block = New-Object byte[] (1MB)
    $random = [System.Random]::new(20261104)
    $random.NextBytes($block)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    # **Write-through, because the volume is a RAM disk and the writer is the memory budget.**
    # Measured: with a cached FileStream the file reached 1 GiB in one second and then blocked
    # forever -- the bytes were sitting dirty in the Windows file cache, the worker's store had
    # not received any of them (its working set stayed at 16 MB), and the flush could not get
    # through.  A cached write measures the cache, not the store's pressure path; write-through
    # sends each block to the volume, which is what the budget is supposed to govern.
    $stream = New-Object System.IO.FileStream($target,
        [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None, 4MB, [System.IO.FileOptions]::WriteThrough)
    $startedWrite = Get-Date
    try {
        [uint64]$written = 0
        while ($written -lt $writeBytes) {
            $chunk = [int][Math]::Min([uint64]$block.Length, $writeBytes - $written)
            [void]$hasher.TransformBlock($block, 0, $chunk, $null, 0)
            $stream.Write($block, 0, $chunk)
            $written += [uint64]$chunk
            if (($written % 256MB) -lt $block.Length) {
                $stream.Flush($true)
                Write-Reading 'pressure.progress' ("{0}B in {1}s" -f $written,
                    [int]((Get-Date) - $startedWrite).TotalSeconds)
            }
        }
        $stream.Flush($true)
        [void]$hasher.TransformFinalBlock([byte[]]::new(0), 0, 0)
    } finally {
        $stream.Dispose()
    }
    $digest = ([System.BitConverter]::ToString($hasher.Hash)).Replace('-', '')
    $elapsedWrite = [int]((Get-Date) - $startedWrite).TotalSeconds
    Set-Content -LiteralPath $expectedFile -Value ("{0} {1}" -f $digest, $writeBytes) -Encoding ASCII
    Write-Reading 'pressure.digest' $digest
    Write-Reading 'pressure.write_seconds' ([string]$elapsedWrite)

    $show = Invoke-Cli @('disk', 'show', $id, '--timeout', '30')
    Write-Reading 'pressure.show' (($show.output -replace "`r?`n", ' | '))
    $memMatch = [regex]::Match($show.output, 'mem=(\d+)/(\d+)')
    if (-not $memMatch.Success) { Fail 'disk show carries no memory reading' }
    [uint64]$resident = $memMatch.Groups[1].Value
    [uint64]$budget = $memMatch.Groups[2].Value
    Write-Reading 'pressure.resident_bytes' ([string]$resident)
    Write-Reading 'pressure.budget_bytes' ([string]$budget)
    if ($budget -eq 0) { Fail 'the running disk reports a zero budget' }
    $soft = [uint64]($budget / 2)
    Write-Reading 'pressure.soft_bytes' ([string]$soft)
    if ($writeBytes -gt $soft -and $resident -ge $writeBytes) {
        Fail ("the store kept every byte resident ({0} >= {1}); no eviction happened" -f $resident, $writeBytes)
    }
    if ($writeBytes -le $soft) {
        Write-Reading 'pressure.eviction_expected' 'no (write is below the soft limit)'
    }
    if ($show.output -match 'budget_rejections=') {
        Fail 'the hard limit rejected writes during the pressure phase'
    }
    $journal = Get-ChildItem -LiteralPath $bundle -Filter 'journal-*.rdj' -ErrorAction SilentlyContinue |
        Sort-Object Length -Descending | Select-Object -First 1
    if ($journal) { Write-Reading 'pressure.journal_bytes' ([string]$journal.Length) }

    Write-Step 'checkpoint under memory pressure'
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $id, '--timeout', '60')
    Write-Reading 'pressure.checkpoint_exit' ([string]$checkpoint.exit)
    $checkpointId = Get-UuidFromText $checkpoint.output
    if (-not $checkpointId) { Fail ("checkpoint carried no operation id: " + $checkpoint.output) }
    $state = Wait-Operation $checkpointId 900
    Write-Reading 'pressure.checkpoint_state' $state
    if ($state -ne 'succeeded') { Fail "the checkpoint ended $state" }
    Write-Reading 'pressure.result' 'PASS'
    Write-Host 'PASS integration.m5.bigdisk (pressure)'
    exit 0
}

if ($mode -eq 'verify') {
    Ensure-Service
    $volumeLetter = $mount.Substring(0, 1).ToUpperInvariant()
    Write-Step 'verify: start the recorded disk and check the data after the cut'
    $id = Get-DiskId
    if ($null -eq $id) { Fail 'no profile survived the power cut' }
    Write-Reading 'verify.id' $id
    $started = Invoke-Cli @('disk', 'start', $id, '--timeout', '60')
    if ($started.exit -ne 0) { Fail ("start failed: " + $started.output) }
    $startId = Get-UuidFromText $started.output
    if (-not $startId) { Fail "start carried no operation id: $($started.output)" }
    $state = Wait-Operation $startId 900
    Write-Reading 'verify.start_state' $state
    if ($state -ne 'succeeded') { Fail "start ended $state" }
    if ($null -eq (Get-Volume -DriveLetter $volumeLetter -ErrorAction SilentlyContinue)) {
        Fail "no volume at $mount after the restart"
    }
    $show = Invoke-Cli @('disk', 'show', $id, '--timeout', '30')
    Write-Reading 'verify.show' (($show.output -replace "`r?`n", ' | '))

    Write-Step 'verify: compare the file digest recorded before the cut'
    if (-not (Test-Path -LiteralPath $expectedFile)) { Fail "no expected digest: $expectedFile" }
    $expected = ((Get-Content -LiteralPath $expectedFile -Raw).Trim()) -split '\s+'
    $target = Join-Path "$($volumeLetter):\" 'bigdisk.bin'
    if (-not (Test-Path -LiteralPath $target)) { Fail "the written file is gone: $target" }
    $actualSize = (Get-Item -LiteralPath $target).Length
    Write-Reading 'verify.file_bytes' ([string]$actualSize)
    if ([string]$actualSize -ne $expected[1]) {
        Fail ("the file length changed: {0} != {1}" -f $actualSize, $expected[1])
    }
    $actual = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    Write-Reading 'verify.digest' $actual
    if ($actual -ne $expected[0]) { Fail 'the file digest does not match the one recorded before the cut' }

    Write-Step 'the single-disk rule still refuses a second disk while this one is up'
    $second = Invoke-Cli @('disk', 'create', '--name', 'second', '--bundle',
        (Join-Path $Root 'bundle2'), '--size', '256MiB', '--mount', 'Y:', '--no-start',
        '--timeout', '60')
    Write-Reading 'verify.second_create_exit' ([string]$second.exit)
    Write-Reading 'verify.second_create_output' (($second.output -replace "`r?`n", ' | '))
    if ($second.exit -eq 0) { Fail 'a second disk was admitted while the first was up' }
    if ($second.output -notmatch 'single-disk|only one disk') {
        Fail ("the refusal did not name the single-disk rule: " + $second.output)
    }
    if (Test-Path -LiteralPath (Join-Path $Root 'bundle2')) {
        Fail 'the refused create still left a bundle behind'
    }
    Write-Reading 'verify.single_disk_refused' 'yes'
    Write-Reading 'verify.result' 'PASS'
    Write-Host 'PASS integration.m5.bigdisk (verify)'
    exit 0
}

# ------------------------------------------------------------------ cleanup mode

if ($mode -eq 'cleanup') {
    Write-Step 'cleanup: stop and forget whatever the guest still holds'
    $id = $null
    $listing = Invoke-Cli @('disk', 'list', '--timeout', '20')
    $match = [regex]::Match($listing.output,
        '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')
    if ($match.Success) { $id = $match.Value }
    Write-Reading 'cleanup.id' ([string]$id)
    if ($id) {
        $idMatch = [regex]::Match($listing.output, '(?m)^\s*(\S+)\s+.*?\s' + [regex]::Escape($id))
        $stopped = Invoke-Cli @('disk', 'stop', $id, '--timeout', '60')
        Write-Reading 'cleanup.stop_exit' ([string]$stopped.exit)
        $forgot = Invoke-Cli @('disk', 'forget', $id, '--confirmation', $id, '--timeout', '60')
        Write-Reading 'cleanup.forget_exit' ([string]$forgot.exit)
        Write-Host ($forgot.output -replace "`r?`n", ' | ')
    }
    $uninstall = Join-Path $repo 'tools\install\Uninstall-MyRamDisk.ps1'
    if (Test-Path -LiteralPath $uninstall) {
        $uninstallOutput = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $uninstall `
            -ServiceName $serviceName 2>&1 | Out-String)
        Write-Reading 'cleanup.uninstall_exit' ([string]$LASTEXITCODE)
        Write-Host $uninstallOutput
    }
    if (Test-Path -LiteralPath $Root) {
        Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Reading 'cleanup.done' 'yes'
    Write-Host 'PASS integration.m5.bigdisk (cleanup)'
    exit 0
}

# ------------------------------------------------------------------ run mode

$size = Read-Config 'size' '8GiB'
$memory = Read-Config 'memory' 'automatic'
$mount = Read-Config 'mount' 'Z:'
$observeSeconds = [int](Read-Config 'observe' '120')
$reuseBundle = (Read-Config 'reuse' '0') -eq '1'

Write-Step 'preflight: a clean guest and a running service'
Ensure-Service
$workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
if ($workers.Count -ne 0) { Fail "the guest already has $($workers.Count) worker(s); reset it first" }
$profiles = Invoke-Cli @('disk', 'list', '--timeout', '30')
if ($profiles.output -match '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}') {
    Fail ("the guest already has a registered disk; reset it first:`n" + $profiles.output)
}
$productCli = Get-ProductCli
Write-Reading 'preflight.cli' $productCli

if (-not $reuseBundle -and (Test-Path -LiteralPath $bundle)) {
    Write-Step 'remove the previous bundle directory'
    Remove-Item -LiteralPath $bundle -Recurse -Force
}
if (Test-Path -LiteralPath $workerTrace) {
    $archive = Join-Path $Root ('worker-trace.' + (Get-Date).ToString('HHmmss') + '.log')
    Move-Item -LiteralPath $workerTrace -Destination $archive -Force
    Write-Reading 'preflight.trace_archive' $archive
}

$arguments = @('disk', 'create', '--name', 'bigdisk', '--bundle', $bundle,
    '--size', $size, '--mount', $mount, '--wait', '--timeout', '1800')
if ($memory -ne 'automatic') {
    if ($memory -notmatch '^fixed:(.+)$') { Fail "memory.txt must be 'automatic' or 'fixed:<size>': $memory" }
    $arguments += @('--memory-mode', 'fixed', '--memory-hard-limit', $Matches[1])
} else {
    $arguments += @('--memory-mode', 'automatic')
}
Write-Step ("create {0} ({1}) at {2}" -f $size, $memory, $mount)
Write-Reading 'create.arguments' ($arguments -join ' ')

$stdoutLog = Join-Path $Root 'create.out.log'
$stderrLog = Join-Path $Root 'create.err.log'
$exitLog = Join-Path $Root 'create.exit.txt'
$cmdFile = Join-Path $Root 'create.cmd'
Remove-Item -LiteralPath $stdoutLog, $stderrLog, $exitLog, $cmdFile -Force -ErrorAction SilentlyContinue
# **The exit code is written by cmd, not read from the Process object.**  Measured: a
# Start-Process handle whose redirected child has exited reports `ExitCode` as null, which
# turned "the CLI failed" into an empty reading.  A one-line wrapper makes the code a file.
$quoted = foreach ($argument in $arguments) {
    if ($argument -match '\s') { '"' + $argument + '"' } else { $argument }
}
$cmdText = "@echo off`r`n`"$productCli`" $($quoted -join ' ') > `"$stdoutLog`" 2> `"$stderrLog`"`r`n" +
    "echo %ERRORLEVEL% > `"$exitLog`"`r`n"
[System.IO.File]::WriteAllText($cmdFile, $cmdText, [System.Text.Encoding]::ASCII)
$started = Get-Date
$create = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" `
    -ArgumentList '/c', $cmdFile -WindowStyle Hidden -PassThru

$operationId = $null
$lastState = 'unknown'
$lastPhase = ''
$stall = $true
$cliExited = $false
$cliExit = $null
$deadline = $started.AddSeconds($observeSeconds)
$previousSample = $null
while ((Get-Date) -lt $deadline) {
    # **The CLI's `--wait` is not the operation.**  Measured on the guest at 4 GiB: the client
    # reported `the control pipe exchange failed` after ~30 s while the create kept running and
    # the bundle journal was still growing.  So the sampling continues after the client exits;
    # what decides pass/stall is the *operation* state, and a client failure on a successful
    # operation is reported separately (it is still a failure: the user cannot see the result).
    Start-Sleep -Seconds 5
    if (-not $cliExited -and (Test-Path -LiteralPath $exitLog)) {
        Start-Sleep -Milliseconds 200
        $cliExited = $true
        $cliExit = Get-ExitCode $exitLog
        Write-Reading 'cli.exit' ([string]$cliExit)
        if (Test-Path -LiteralPath $stderrLog) {
            $early = Get-Content -LiteralPath $stderrLog -ErrorAction SilentlyContinue
            if ($early) { Write-Reading 'cli.stderr' (($early -join ' ') -replace "`r?`n", ' ') }
        }
    }
    if (Test-Path -LiteralPath $stdoutLog) {
        $text = Get-Content -LiteralPath $stdoutLog -Raw -ErrorAction SilentlyContinue
        if (-not $operationId -and $text) {
            $idMatch = [regex]::Match($text,
                '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')
            if ($idMatch.Success) { $operationId = $idMatch.Value }
        }
    }
    if ($operationId) {
        $reading = Get-OperationReading $operationId
        $lastState = $reading.state
        $lastPhase = $reading.phase
        $elapsed = [int]((Get-Date) - $started).TotalSeconds
        $sample = Get-WorkerSample
        $cpuDelta = ''
        $readDelta = ''
        $writeDelta = ''
        if ($sample -and $previousSample) {
            $cpuDelta = ('{0:N2}' -f ($sample.cpu - $previousSample.cpu))
            if ($sample.ioRead -ge 0 -and $previousSample.ioRead -ge 0) {
                $readDelta = [string]($sample.ioRead - $previousSample.ioRead)
                $writeDelta = [string]($sample.ioWrite - $previousSample.ioWrite)
            }
        }
        $previousSample = $sample
        Write-Reading 'sample' ("t={0}s state={1} phase={2} op_exit={3} bundle=[{4}] worker_cpu_delta={5} worker_io_delta={6}/{7}" -f `
            $elapsed, $reading.state, $reading.phase, $reading.exit, (Get-BundleSnapshot),
            $cpuDelta, $readDelta, $writeDelta)
        if (@('succeeded', 'failed', 'cancelled') -contains $reading.state) { $stall = $false; break }
    } else {
        $elapsed = [int]((Get-Date) - $started).TotalSeconds
        Write-Reading 'sample' ("t={0}s no operation id yet; bundle=[{1}]" -f $elapsed, (Get-BundleSnapshot))
        # Nothing was ever admitted and the client is gone: there is no operation to watch.
        if ($cliExited) { break }
    }
}

if ($stall) {
    Write-Step 'STALLED: the create has not reached a terminal state'
    Write-Reading 'stall.elapsed_seconds' ([string][int]((Get-Date) - $started).TotalSeconds)
    Write-Reading 'stall.operation' ([string]$operationId)
    Write-Reading 'stall.last_state' $lastState
    Write-Reading 'stall.last_phase' $lastPhase
    Write-Reading 'stall.cli_exited' ([string]$cliExited)
    if ($cliExited) { Write-Reading 'stall.cli_exit' ([string]$cliExit) }
    if (Test-Path -LiteralPath $stdoutLog) {
        Write-Host '--- create stdout ---'
        Get-Content -LiteralPath $stdoutLog -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    }
    if (Test-Path -LiteralPath $stderrLog) {
        Write-Host '--- create stderr ---'
        Get-Content -LiteralPath $stderrLog -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    }
    Show-Scene $operationId
    Write-Host 'STALL integration.m5.bigdisk (scene preserved; guest must be hard-reset before the next run)'
    exit 3
}

$exitCode = if (Test-Path -LiteralPath $exitLog) {
    Get-ExitCode $exitLog
} else {
    # The operation is terminal; `--wait` needs one more exchange to notice and return.
    $waitUntil = (Get-Date).AddSeconds(60)
    while (-not (Test-Path -LiteralPath $exitLog) -and (Get-Date) -lt $waitUntil) {
        Start-Sleep -Milliseconds 250
    }
    if (Test-Path -LiteralPath $exitLog) {
        Get-ExitCode $exitLog
    } else { -1 }
}
Write-Reading 'create.exit' ([string]$exitCode)
if (Test-Path -LiteralPath $stdoutLog) {
    Write-Host '--- create stdout ---'
    Get-Content -LiteralPath $stdoutLog -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
}
if (Test-Path -LiteralPath $stderrLog) {
    $err = Get-Content -LiteralPath $stderrLog -ErrorAction SilentlyContinue
    if ($err) { Write-Host '--- create stderr ---'; $err | ForEach-Object { Write-Host $_ } }
}
if ($lastState -ne 'succeeded') { Fail "the create ended $lastState" }
if ($exitCode -ne 0) {
    Fail ("the operation succeeded but `disk create --wait` exited $exitCode; a caller cannot " +
          "see the result (the exchange budget is 30 s, see the defect report)")
}

Write-Step 'the operation succeeded; verify the volume and the listing'
$letterChar = $mount.Substring(0, 1).ToUpperInvariant()
$volume = Get-Volume -DriveLetter $letterChar -ErrorAction SilentlyContinue
if ($null -eq $volume) { Fail "no volume is mounted at $mount after a successful create" }
Write-Reading 'verify.volume_size' ([string]$volume.Size)
Write-Reading 'verify.volume_filesystem' ([string]$volume.FileSystem)
Write-Reading 'verify.volume_label' ([string]$volume.FileSystemLabel)
$expectedVolume = ConvertTo-Bytes $size
if ($volume.Size -lt [uint64]($expectedVolume * 0.9)) { Fail "the mounted volume is smaller than requested: $($volume.Size) vs $expectedVolume" }
$listing = Invoke-Cli @('disk', 'list', '--timeout', '30')
if ($listing.exit -ne 0) { Fail "disk list failed after the create: $($listing.output)" }
if ($listing.output -notmatch [regex]::Escape($mount)) {
    Fail ("disk list does not show $mount`: " + $listing.output)
}
Write-Reading 'verify.list' (($listing.output -replace "`r?`n", ' | '))

# **A write in the same run as the create, before anything can stop the service.**
#
# The first attempt at the memory-pressure acceptance wrote from a *separate* run, and every
# such write hung.  The service log of those runs shows a restart between the two runs and an
# auto-start that adopted the AIM device, so the hang may belong to the run boundary rather than
# to the product.  This writes immediately after the create, in the process that just created
# the disk -- the flow a user actually has -- and is the discriminating reading.
$thenWrite = Read-Config 'thenwrite' ''
if (-not [string]::IsNullOrWhiteSpace($thenWrite)) {
    $writeBytes = ConvertTo-Bytes $thenWrite
    $target = Join-Path "$($letterChar):\" 'bigdisk.bin'
    Write-Step "write $writeBytes bytes to $target in the same run"
    $block = New-Object byte[] (1MB)
    [System.Random]::new(20261104).NextBytes($block)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $stream = New-Object System.IO.FileStream($target,
        [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None, 4MB, [System.IO.FileOptions]::WriteThrough)
    $startedWrite = Get-Date
    try {
        [uint64]$written = 0
        while ($written -lt $writeBytes) {
            $chunk = [int][Math]::Min([uint64]$block.Length, $writeBytes - $written)
            [void]$hasher.TransformBlock($block, 0, $chunk, $null, 0)
            $stream.Write($block, 0, $chunk)
            $written += [uint64]$chunk
            if (($written % 64MB) -lt $block.Length) {
                $stream.Flush($true)
                Write-Reading 'thenwrite.progress' ("{0}B in {1}s" -f $written,
                    [int]((Get-Date) - $startedWrite).TotalSeconds)
            }
        }
        $stream.Flush($true)
        [void]$hasher.TransformFinalBlock([byte[]]::new(0), 0, 0)
    } finally {
        $stream.Dispose()
    }
    Write-Reading 'thenwrite.elapsed_seconds' ([string][int]((Get-Date) - $startedWrite).TotalSeconds)
    Write-Reading 'thenwrite.file_bytes' ([string](Get-Item -LiteralPath $target).Length)
    $digest = ([System.BitConverter]::ToString($hasher.Hash)).Replace('-', '')
    Set-Content -LiteralPath (Join-Path $Root 'expected.sha256') -Value ("{0} {1}" -f $digest, $writeBytes) -Encoding ASCII
    Write-Reading 'thenwrite.digest' $digest
    $id = Get-DiskId
    if ($id) {
        $show = Invoke-Cli @('disk', 'show', $id, '--timeout', '60')
        Write-Reading 'thenwrite.show' (($show.output -replace "`r?`n", ' | '))
        $memMatch = [regex]::Match($show.output, 'mem=(\d+)/(\d+)')
        if ($memMatch.Success) {
            Write-Reading 'thenwrite.resident_bytes' $memMatch.Groups[1].Value
            Write-Reading 'thenwrite.budget_bytes' $memMatch.Groups[2].Value
        }
        if ($show.output -match 'budget_rejections=') {
            Fail 'the hard limit rejected writes during the pressure write'
        }
    }
    # The checkpoint is what makes the written data durable before a power cut, and it is the
    # step the acceptance needs a reading for.
    #
    # **The write itself crosses the dirty threshold, so the service starts a maintenance
    # checkpoint on its own.**  Issuing a second one while that is in flight raced it (measured:
    # a `disk checkpoint` whose exchange the busy service never answered, exit 3, and a
    # hand-issued checkpoint that then found "no runtime bound").  So this waits for the disk to
    # come back to Idle/Clean and records the generation the checkpoint produced; it only issues
    # a checkpoint itself if none ran.
    if ((Read-Config 'thencheckpoint' '0') -eq '1' -and $id) {
        Write-Step 'wait for the checkpoint to finish'
        $generationBefore = 2
        $checkpointDeadline = (Get-Date).AddSeconds(1800)
        $state = 'unknown'
        while ((Get-Date) -lt $checkpointDeadline) {
            $show = Invoke-Cli @('disk', 'show', $id, '--timeout', '60')
            if ($show.output -match 'activity=Idle' -and $show.output -match 'persistence=Clean') {
                $state = 'idle-clean'
                Write-Reading 'thencheckpoint.show' (($show.output -replace "`r?`n", ' | '))
                $genMatch = [regex]::Match($show.output, 'generation=(\d+)')
                if ($genMatch.Success) { Write-Reading 'thencheckpoint.generation' $genMatch.Groups[1].Value }
                break
            }
            Write-Reading 'thencheckpoint.waiting' (($show.output -replace "`r?`n", ' | '))
            Start-Sleep -Seconds 15
        }
        Write-Reading 'thencheckpoint.state' $state
        if ($state -ne 'idle-clean') { Fail 'the disk never returned to Idle/Clean after the write' }
        if ((Read-Config 'thencheckpoint.force' '0') -eq '1') {
            $checkpoint = Invoke-Cli @('disk', 'checkpoint', $id, '--timeout', '60')
            Write-Reading 'thencheckpoint.exit' ([string]$checkpoint.exit)
            $checkpointId = Get-UuidFromText $checkpoint.output
            if (-not $checkpointId) { Fail ("checkpoint carried no operation id: " + $checkpoint.output) }
            $forced = Wait-Operation $checkpointId 1800
            Write-Reading 'thencheckpoint.forced_state' $forced
            if ($forced -ne 'succeeded') { Fail "the checkpoint ended $forced" }
        }
    }
    Write-Reading 'thenwrite.result' 'PASS'
}

Write-Reading 'bigdisk.result' 'PASS'
Write-Host ("PASS integration.m5.bigdisk ({0}, {1})" -f $size, $memory)
exit 0
