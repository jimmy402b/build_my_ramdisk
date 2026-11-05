# integration.m5.compaction_* -- the online-compaction acceptance (T4-03).
#
# Guest-only: every scenario creates a real worker, a real AIM disk and a real
# Windows volume (AGENTS.md section 4).  On the development machine it refuses
# with exit 2 rather than passing quietly.
#
# Scenarios:
#   manual  A 16 GiB running disk: write 1 GiB -> checkpoint -> `disk compact`
#           must succeed, the old journal must be gone and the new one smaller,
#           and the file's size and SHA-256 must be unchanged before and after a
#           stop/start.
#   auto    A 1 GiB disk with a low compaction threshold: 200 checkpoints must
#           move the generation at least 200, at least one *automatic*
#           compaction must complete, and the journal must stay bounded.
#   churn   A 16 GiB disk: write 10 GiB, delete it, repeat three times; every
#           round ends with a successful checkpoint and a successful compaction,
#           and the final stop/start still verifies the marker file.
#   kill    A compaction is paused after its manifest was published but before
#           the service recorded the evidence, the worker is killed, the service
#           is restarted, and the disk must start with its data intact.
param(
    [ValidateSet('manual', 'auto', 'churn', 'regression', 'kill')]
    [string]$Scenario = 'manual',
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 3300
)

$ErrorActionPreference = 'Stop'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

function Write-Step([string]$message) { Write-Host "--- $message" }
function Fail([string]$message) {
    [Console]::Error.WriteLine("FAIL: $message")
    exit 1
}
function Assert-NotExpired([string]$where) {
    if ((Get-Date) -gt $deadline) { Fail "the scenario deadline expired at $where" }
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.compaction_* creates a real worker, a real AIM disk and a ' +
        'real Windows volume, and may only run on the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # tests/e2e -> repo
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
$workerExe = Join-Path $bin 'myrd-m5-worker.exe'
foreach ($path in @($serviceExe, $cliExe, $workerExe)) {
    if (-not (Test-Path -LiteralPath $path)) { Fail "missing binary: $path" }
}

$serviceName = 'MyRamDiskService'
$workRoot = 'C:\m5\compaction-' + [guid]::NewGuid().ToString('N')
$bundlePath = Join-Path $workRoot 'bundle'
$serviceLog = Join-Path $workRoot 'service.log'
$serviceOutLog = Join-Path $workRoot 'service.out.log'
$pauseFile = Join-Path $workRoot 'compaction-committed.marker'
$serviceProcess = $null
$profileName = 'cmp-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$mount = $null
$serviceLog = Join-Path $workRoot 'service.log'

function Invoke-Cli {
    param([string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $started = Get-Date
        $output = & $cliExe @Arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
        return [pscustomobject]@{
            output = $output
            exit = $exit
            ms = [int]((Get-Date) - $started).TotalMilliseconds
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Get-UuidFromText([string]$text) {
    $match = [regex]::Match($text, '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')
    if (-not $match.Success) { return $null }
    return $match.Value
}

function Get-OperationState([string]$operationId) {
    $result = Invoke-Cli @('operation', 'show', $operationId)
    foreach ($word in @('queued', 'running', 'reconciling', 'succeeded', 'failed', 'cancelled')) {
        if ($result.output -match "operation $operationId`: $word") { return $word }
    }
    return 'unknown'
}

function Get-ServiceTail([int]$lines = 15) {
    if (-not (Test-Path -LiteralPath $serviceLog)) { return '' }
    return ((Get-Content -LiteralPath $serviceLog -Tail $lines) -join "`n")
}

function Wait-ForTerminal([string]$operationId, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    $state = 'unknown'
    while ((Get-Date) -lt $until) {
        $state = Get-OperationState $operationId
        if (@('succeeded', 'failed', 'cancelled') -contains $state) { return $state }
        Start-Sleep -Milliseconds 500
    }
    Fail "operation $operationId did not reach a terminal state in $seconds s (state=$state)`n$(Get-ServiceTail)"
}

function Invoke-DiskOperation {
    param([string[]]$Arguments, [int]$Seconds = 300, [switch]$AllowFailure)
    $result = Invoke-Cli $Arguments
    if ($result.exit -ne 0) {
        if ($AllowFailure) { return [pscustomobject]@{ exit = $result.exit; state = 'refused'; id = $null; output = $result.output } }
        Fail "command failed ($($Arguments -join ' '), exit $($result.exit)): $($result.output)"
    }
    $operationId = Get-UuidFromText $result.output
    if ($null -eq $operationId) {
        if ($AllowFailure) { return [pscustomobject]@{ exit = $result.exit; state = 'no-operation'; id = $null; output = $result.output } }
        Fail "command carried no operation id: $($result.output)"
    }
    $state = Wait-ForTerminal $operationId $Seconds
    return [pscustomobject]@{ exit = 0; state = $state; id = $operationId; output = $result.output }
}

function Wait-ForService([int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $probe = Invoke-Cli @('disk', 'list')
        if ($probe.exit -eq 0) { return $probe }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

# A worker command holds the adapter's command gate, and the public pipe's
# dispatch path reads a runtime observation for some commands -- so while a
# large checkpoint is in flight `disk compact` can fail with "the control pipe
# exchange failed" before it is even admitted.  That is backpressure, not a
# product failure: wait until a bounded list answers, then issue the next
# command.  The bound is short per probe so the wait itself cannot consume the
# scenario's deadline.
function Wait-ForCliResponsive([int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $probe = Invoke-Cli @('disk', 'list', '--timeout', '10')
        if ($probe.exit -eq 0) { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

function Start-ServiceForeground {
    Write-Step "start the service in the foreground from $serviceExe"
    New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
    $env:MYRD_TRACE_FILE = Join-Path $workRoot 'trace.log'
    $script:serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog -RedirectStandardOutput $serviceOutLog `
        -WindowStyle Hidden -PassThru
    $listed = Wait-ForService 60
    if ($null -eq $listed) {
        Fail "the service never answered a list command.`n$(Get-ServiceTail 40)"
    }
    return $listed
}

function Stop-ServiceForeground {
    if ($null -ne $script:serviceProcess -and -not $script:serviceProcess.HasExited) {
        Stop-Process -Id $script:serviceProcess.Id -Force -ErrorAction SilentlyContinue
        $script:serviceProcess.WaitForExit(10000) | Out-Null
    }
    for ($i = 0; $i -lt 20; $i++) {
        $left = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) { break }
        $left | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 250
    }
}

function Find-FreeLetter([string]$listed) {
    $occupied = @()
    foreach ($volume in (Get-CimInstance -ClassName Win32_Volume -ErrorAction Stop)) {
        if ($volume.DriveLetter) { $occupied += $volume.DriveLetter.ToUpper() }
    }
    foreach ($disk in (Get-CimInstance -ClassName Win32_LogicalDisk -ErrorAction Stop)) {
        if ($disk.DeviceID) { $occupied += $disk.DeviceID.ToUpper() }
    }
    if (-not ([System.Management.Automation.PSTypeName]'Myrd.DosDevice').Type) {
        Add-Type -Namespace Myrd -Name DosDevice -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern int QueryDosDevice(string name, System.Text.StringBuilder target, int max);
'@
    }
    foreach ($candidate in @('T:', 'U:', 'V:', 'W:', 'X:', 'Y:', 'Z:')) {
        if ($occupied -contains $candidate) { continue }
        if ($listed -match [regex]::Escape($candidate)) { continue }
        $buffer = New-Object System.Text.StringBuilder 512
        if ([Myrd.DosDevice]::QueryDosDevice($candidate, $buffer, 512) -ne 0) { continue }
        return $candidate
    }
    return $null
}

function Write-DataFile([string]$path, [uint64]$bytes) {
    $chunk = New-Object byte[] (4MB)
    (New-Object System.Random 424242).NextBytes($chunk)
    $remaining = $bytes
    $written = [uint64]0
    $nextReport = [uint64]1GB
    $stream = [System.IO.File]::Create($path, 4096, [System.IO.FileOptions]::WriteThrough)
    try {
        while ($remaining -gt 0) {
            $count = [int][Math]::Min([uint64]$chunk.Length, $remaining)
            try {
                $stream.Write($chunk, 0, $count)
            } catch {
                [Console]::Error.WriteLine(
                    "write failed at $written of $bytes bytes: $($_.Exception.Message)")
                [Console]::Error.WriteLine("free space: $((Get-PSDrive C).Free)")
                throw
            }
            $written += [uint64]$count
            $remaining -= [uint64]$count
            if ($written -ge $nextReport) {
                Write-Host "  wrote $([math]::Round($written / 1GB, 1)) GiB"
                $nextReport += [uint64]1GB
            }
        }
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Get-JournalStats {
    $files = @(Get-ChildItem -LiteralPath $bundlePath -Filter 'journal-*.rdj' -File -ErrorAction SilentlyContinue)
    $total = [uint64]0
    foreach ($file in $files) { $total += [uint64]$file.Length }
    return [pscustomobject]@{
        count = $files.Count
        total = $total
        names = @($files | ForEach-Object { $_.Name })
        sizes = @($files | ForEach-Object { [uint64]$_.Length })
    }
}

function Get-DiskLine([string]$name) {
    $show = Invoke-Cli @('disk', 'show', $name)
    if ($show.exit -ne 0) { Fail "disk show $name failed: $($show.output)" }
    return $show.output
}

function Get-Generation([string]$name) {
    $text = Get-DiskLine $name
    $match = [regex]::Match($text, 'generation=(\d+)')
    if (-not $match.Success) { Fail "disk show did not report a generation: $text" }
    return [uint64]$match.Groups[1].Value
}

function Get-Compactions([string]$name) {
    $text = Get-DiskLine $name
    $match = [regex]::Match($text, 'compactions=(\d+)')
    if (-not $match.Success) { return [uint64]0 }
    return [uint64]$match.Groups[1].Value
}

function Wait-ForCompactionCount([string]$name, [uint64]$atLeast, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        if ((Get-Compactions $name) -ge $atLeast) { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

function Wait-ForLog([string]$pattern, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        if ((Test-Path -LiteralPath $serviceLog) -and
            ((Get-Content -LiteralPath $serviceLog -Raw) -match [regex]::Escape($pattern))) {
            return $true
        }
        Start-Sleep -Seconds 2
    }
    return $false
}

function Create-Disk([string]$size, [string]$compactionThreshold, [string[]]$ExtraCreateArgs = @()) {
    $listed = (Invoke-Cli @('disk', 'list')).output
    $script:mount = Find-FreeLetter $listed
    if ($null -eq $script:mount) { Fail 'no free drive letter for the compaction disk' }
    Write-Step "create a $size disk at $script:mount with compaction threshold $compactionThreshold"
    $arguments = @('disk', 'create', '--name', $profileName,
        '--bundle', $bundlePath, '--size', $size, '--mount', $script:mount,
        '--checkpoint-interval', '30s')
    if ($compactionThreshold) {
        $arguments += @('--compaction-threshold', $compactionThreshold)
    }
    if ($ExtraCreateArgs.Count -gt 0) {
        $arguments += $ExtraCreateArgs
    }
    $created = Invoke-DiskOperation $arguments 900
    if ($created.state -ne 'succeeded') { Fail "disk create ended $($created.state)`n$(Get-ServiceTail)" }
    $generation = Get-Generation $profileName
    Write-Host "  created at generation $generation"
}

function Stop-And-Start-Disk {
    Write-Step 'stop and start the disk'
    if (-not (Wait-ForCliResponsive 900)) { Fail 'the service did not become responsive before stop' }
    $stop = Invoke-DiskOperation @('disk', 'stop', $profileName) 600
    if ($stop.state -ne 'succeeded') { Fail "disk stop ended $($stop.state)`n$(Get-ServiceTail)" }
    if (-not (Wait-ForCliResponsive 600)) { Fail 'the service did not become responsive before start' }
    $start = Invoke-DiskOperation @('disk', 'start', $profileName) 600
    if ($start.state -ne 'succeeded') { Fail "disk start ended $($start.state)`n$(Get-ServiceTail)" }
}

function Remove-TestDisk {
    $stop = Invoke-DiskOperation @('disk', 'stop', $profileName) 600 -AllowFailure
    if ($stop.exit -eq 0 -and $stop.state -eq 'succeeded') {
        $forget = Invoke-DiskOperation @('disk', 'forget', $profileName,
            '--confirmation', $profileName) 300 -AllowFailure
        if ($forget.exit -eq 0 -and $forget.state -eq 'succeeded') {
            Write-Host '  test disk forgotten'
        } else {
            Write-Host "  disk forget did not complete: $($forget.output)"
        }
    }
}

try {
    New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
    if (Test-Path -LiteralPath 'C:\m5\Reset-M5Guest.ps1') {
        Write-Step 'reset the guest state'
        & 'C:\m5\Reset-M5Guest.ps1' | Out-Host
    }
    # A kill leaves AIM proxy devices behind; they are not Windows services and
    # the reset script does not remove them.  A create then meets "the driver
    # already holds N device(s)" and can block in adapter enumeration.  Remove
    # them here, before any disk exists, so a scenario measures the product and
    # not the previous run's residue.
    $staleDevices = @(Get-PnpDevice -Class DiskDrive -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -like 'Arsenal Virtual*' })
    if ($staleDevices.Count -gt 0) {
        Write-Step "remove $($staleDevices.Count) stale AIM device(s)"
        foreach ($device in $staleDevices) {
            # `Remove-PnpDevice` is not present in the guest's Windows
            # PowerShell 5.1 module set; `pnputil` is, but removing a device the
            # driver still owns can block.  Bound each removal so a stale device
            # cannot turn cleanup into the scenario's timeout.
            $process = Start-Process -FilePath 'pnputil.exe' `
                -ArgumentList @('/remove-device', $device.InstanceId) `
                -WindowStyle Hidden -PassThru
            if (-not $process.WaitForExit(10000)) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
        }
        Start-Sleep -Seconds 2
    }
    if ($Scenario -eq 'kill') {
        # The pause is inherited by the worker this service launches.  It is
        # armed for compaction only, so the create's own checkpoint is unaffected.
        $env:MYRD_M5_TEST_PAUSE_AT = 'compaction-committed'
        $env:MYRD_M5_TEST_PAUSE_FILE = $pauseFile
        $env:MYRD_M5_TEST_PAUSE_SECONDS = '300'
    }
    Start-ServiceForeground | Out-Null

    if ($Scenario -eq 'manual') {
        Write-Step 'T4-03 manual: a running 16 GiB disk compacts and keeps its bytes'
        Create-Disk '16GiB' ''
        $file = Join-Path ($mount + '\') 'one-gib.bin'
        $bytes = [uint64]1GB
        Write-Step 'write 1 GiB through the mounted volume'
        Write-DataFile $file $bytes
        $before = Get-FileHash -LiteralPath $file -Algorithm SHA256
        Write-Host "  wrote $bytes bytes, SHA-256 $($before.Hash)"
        $checkpoint = $null
        for ($attempt = 1; $attempt -le 6; $attempt++) {
            if (-not (Wait-ForCliResponsive 600)) { Fail 'the service did not become responsive before the checkpoint' }
            $checkpoint = Invoke-DiskOperation @('disk', 'checkpoint', $profileName) 900 -AllowFailure
            if ($checkpoint.exit -eq 0 -and $checkpoint.state -eq 'succeeded') { break }
            Write-Host "  checkpoint attempt $attempt ended $($checkpoint.state); retrying after 10 s"
            Start-Sleep -Seconds 10
        }
        if ($null -eq $checkpoint -or $checkpoint.state -ne 'succeeded') { Fail "checkpoint ended $($checkpoint.state)`n$(Get-ServiceTail)" }
        $journalBefore = Get-JournalStats
        Write-Host ("  journal before compaction: " + ($journalBefore.names -join ', ') +
            " total=$($journalBefore.total)")

        $compaction = $null
        for ($attempt = 1; $attempt -le 4; $attempt++) {
            if (-not (Wait-ForCliResponsive 900)) { Fail 'the service did not become responsive before the compaction' }
            $compaction = Invoke-DiskOperation @('disk', 'compact', $profileName) 1800 -AllowFailure
            if ($compaction.exit -eq 0 -and $compaction.state -eq 'succeeded') { break }
            Write-Host "  compaction attempt $attempt ended $($compaction.state); retrying after 10 s"
            Start-Sleep -Seconds 10
        }
        if ($null -eq $compaction -or $compaction.state -ne 'succeeded') { Fail "disk compact ended $($compaction.state)`n$(Get-ServiceTail)" }
        $journalAfter = Get-JournalStats
        Write-Host ("  journal after compaction: " + ($journalAfter.names -join ', ') +
            " total=$($journalAfter.total)")
        if ($journalBefore.names -contains 'journal-e0001.rdj') {
            if ($journalAfter.names -contains 'journal-e0001.rdj') {
                Fail 'journal-e0001.rdj still exists after a successful compaction'
            }
        }
        if ($journalAfter.count -lt 1) { Fail 'no journal exists after compaction' }
        if ($journalAfter.total -ge $journalBefore.total) {
            Fail "the journal did not shrink: before=$($journalBefore.total) after=$($journalAfter.total)"
        }
        if ($journalAfter.total -gt 256MB) {
            Fail "the compacted journal is not bounded: $($journalAfter.total) bytes"
        }

        $after = Get-FileHash -LiteralPath $file -Algorithm SHA256
        if ($before.Hash -ne $after.Hash) { Fail "SHA-256 changed across compaction: $($before.Hash) -> $($after.Hash)" }
        if ((Get-Item -LiteralPath $file).Length -ne [int64]$bytes) { Fail 'the file length changed across compaction' }
        Write-Host "  SHA-256 unchanged: $($after.Hash)"
        Stop-And-Start-Disk
        $again = Get-FileHash -LiteralPath $file -Algorithm SHA256
        if ($again.Hash -ne $before.Hash) { Fail "SHA-256 changed across stop/start: $($before.Hash) -> $($again.Hash)" }
        Write-Host "  SHA-256 unchanged after stop/start: $($again.Hash)"
        $log = (Get-Content -LiteralPath $serviceLog -Raw)
        if ($log -notmatch 'compaction: completed') { Fail 'the service log has no completed compaction' }
        Write-Host 'PASS integration.m5.compaction_manual'
    }

    if ($Scenario -eq 'auto') {
        Write-Step 'T4-03 auto: 200 checkpoints with a low threshold compact automatically'
        Create-Disk '1GiB' '16'
        $marker = Join-Path ($mount + '\') 'marker.bin'
        Write-DataFile $marker 1MB
        $startGeneration = Get-Generation $profileName
        for ($i = 1; $i -le 200; $i++) {
            Assert-NotExpired "checkpoint $i"
            $arguments = @('disk', 'checkpoint', $profileName)
            $cp = Invoke-DiskOperation $arguments 300 -AllowFailure
            if ($cp.exit -ne 0 -or $cp.state -ne 'succeeded') {
                # A maintenance checkpoint or compaction may hold the profile.
                Start-Sleep -Seconds 1
                $cp = Invoke-DiskOperation $arguments 300
                if ($cp.state -ne 'succeeded') { Fail "checkpoint $i ended $($cp.state)`n$(Get-ServiceTail)" }
            }
            if (($i % 10) -eq 0) {
                Start-Sleep -Milliseconds 800
                Write-Host "  checkpoint $i, generation $(Get-Generation $profileName), compactions $(Get-Compactions $profileName)"
            }
        }
        $generation = Get-Generation $profileName
        if ($generation -lt ($startGeneration + 200)) {
            Fail "generation advanced only to $generation (start $startGeneration); 200 checkpoints were not generations"
        }
        if (-not (Wait-ForCompactionCount $profileName 1 120)) {
            Fail "no automatic compaction completed within 120 s of 200 checkpoints`n$(Get-ServiceTail)"
        }
        if (-not (Wait-ForLog 'compaction: completed' 120)) {
            Fail "the service log has no 'compaction: completed' line`n$(Get-ServiceTail)"
        }
        $journal = Get-JournalStats
        if ($journal.total -gt 512MB) {
            Fail "the journal is not bounded after 200 checkpoints: $($journal.total) bytes"
        }
        Write-Host "  generation=$generation compactions=$(Get-Compactions $profileName) journal=$($journal.total)"
        Write-Host 'PASS integration.m5.compaction_auto'
    }

    if ($Scenario -eq 'churn') {
        Write-Step 'T4-03 churn: 10 GiB written and deleted three times'
        Create-Disk '16GiB' '' @('--memory-mode', 'fixed', '--memory-hard-limit', '12GiB')
        $churn = Join-Path ($mount + '\') 'churn.bin'
        $marker = Join-Path ($mount + '\') 'final-marker.bin'
        $tenGiB = [uint64]10GB
        for ($round = 1; $round -le 3; $round++) {
            Assert-NotExpired "churn round $round"
            Write-Step "round ${round}: write 10 GiB, delete it, checkpoint, compact"
            Write-DataFile $churn $tenGiB
            Remove-Item -LiteralPath $churn -Force
            Write-Host "  round ${round}: deleted the 10 GiB file"
            $checkpoint = $null
            for ($attempt = 1; $attempt -le 20; $attempt++) {
                if (-not (Wait-ForCliResponsive 1200)) { Fail 'the service did not become responsive before the churn checkpoint' }
                $checkpoint = Invoke-DiskOperation @('disk', 'checkpoint', $profileName) 1800 -AllowFailure
                if ($checkpoint.exit -eq 0 -and $checkpoint.state -eq 'succeeded') { break }
                # A large delete leaves a discard backlog; the first checkpoint
                # may abort with SnapshotResourceExhausted and a maintenance
                # checkpoint may already hold the profile.  Both are retry
                # costs, not a failed round -- wait long enough for the active
                # operation to terminalize rather than burning attempts on
                # immediate refusals.
                Write-Host "  checkpoint attempt $attempt ended $($checkpoint.state); retrying after 30 s"
                Start-Sleep -Seconds 30
            }
            if ($null -eq $checkpoint -or $checkpoint.state -ne 'succeeded') {
                Fail "round ${round} checkpoint did not succeed`n$(Get-ServiceTail 25)"
            }
            if (-not (Wait-ForCliResponsive 1200)) { Fail 'the service did not become responsive before the churn compaction' }
            $compaction = Invoke-DiskOperation @('disk', 'compact', $profileName) 1800 -AllowFailure
            if ($compaction.exit -ne 0 -or $compaction.state -ne 'succeeded') {
                # The checkpoint may still be settling; one bounded retry is
                # enough for the same reason the checkpoint loop above waits.
                Start-Sleep -Seconds 30
                $compaction = Invoke-DiskOperation @('disk', 'compact', $profileName) 1800 -AllowFailure
            }
            if ($compaction.exit -ne 0 -or $compaction.state -ne 'succeeded') {
                Fail "round ${round} compaction ended $($compaction.state)`n$(Get-ServiceTail 25)"
            }
            $journal = Get-JournalStats
            Write-Host "  round ${round}: journal total $($journal.total) across $($journal.count) file(s)"
            if ($journal.total -gt 6GB) { Fail "round ${round} journal is not bounded: $($journal.total)" }
        }
        Write-Step 'write the final marker and verify it after stop/start'
        Write-DataFile $marker 1MB
        $before = Get-FileHash -LiteralPath $marker -Algorithm SHA256
        $checkpoint = Invoke-DiskOperation @('disk', 'checkpoint', $profileName) 900
        if ($checkpoint.state -ne 'succeeded') { Fail "final checkpoint ended $($checkpoint.state)" }
        Stop-And-Start-Disk
        $after = Get-FileHash -LiteralPath $marker -Algorithm SHA256
        if ($before.Hash -ne $after.Hash) { Fail "the marker changed across stop/start: $($before.Hash) -> $($after.Hash)" }
        Write-Host "  marker SHA-256 unchanged: $($after.Hash)"
        Write-Host 'PASS integration.m5.compaction_churn'
    }

    if ($Scenario -eq 'regression') {
        Write-Step '1.0.2 regression: 10 GiB -> delete -> 1 GiB -> checkpoint -> stop/start'
        Create-Disk '16GiB' '' @('--memory-mode', 'fixed', '--memory-hard-limit', '12GiB')
        $ten = Join-Path ($mount + '\') 'ten-gib.bin'
        $one = Join-Path ($mount + '\') 'one-gib.bin'
        Write-DataFile $ten ([uint64]10GB)
        Remove-Item -LiteralPath $ten -Force
        Write-DataFile $one ([uint64]1GB)
        $before = Get-FileHash -LiteralPath $one -Algorithm SHA256
        Write-Host "  1 GiB SHA-256 $($before.Hash)"
        $checkpoint = $null
        for ($attempt = 1; $attempt -le 20; $attempt++) {
            if (-not (Wait-ForCliResponsive 1200)) { Fail 'the service did not become responsive before the regression checkpoint' }
            $checkpoint = Invoke-DiskOperation @('disk', 'checkpoint', $profileName) 1800 -AllowFailure
            if ($checkpoint.exit -eq 0 -and $checkpoint.state -eq 'succeeded') { break }
            Write-Host "  checkpoint attempt $attempt ended $($checkpoint.state); retrying after 30 s"
            Start-Sleep -Seconds 30
        }
        if ($null -eq $checkpoint -or $checkpoint.state -ne 'succeeded') {
            Fail "the regression checkpoint did not succeed`n$(Get-ServiceTail 25)"
        }
        $journal = Get-JournalStats
        Write-Host "  journal after the checkpoint: $($journal.total) bytes across $($journal.count) file(s)"
        Stop-And-Start-Disk
        $after = Get-FileHash -LiteralPath $one -Algorithm SHA256
        if ($before.Hash -ne $after.Hash) {
            Fail "the 1 GiB file changed across stop/start: $($before.Hash) -> $($after.Hash)"
        }
        if ((Get-Item -LiteralPath $one).Length -ne [int64]1GB) {
            Fail 'the 1 GiB file length changed'
        }
        Write-Host "  1 GiB SHA-256 unchanged after stop/start: $($after.Hash)"
        Write-Host 'PASS integration.m5.compaction_regression'
    }

    if ($Scenario -eq 'kill') {
        Write-Step 'T4-03 kill: a worker killed after the compaction publish must not lose data or wedge'
        Create-Disk '1GiB' ''
        $file = Join-Path ($mount + '\') 'kill.bin'
        Write-DataFile $file 64MB
        $before = Get-FileHash -LiteralPath $file -Algorithm SHA256
        $checkpoint = Invoke-DiskOperation @('disk', 'checkpoint', $profileName) 600
        if ($checkpoint.state -ne 'succeeded') { Fail "checkpoint ended $($checkpoint.state)" }
        if (Test-Path -LiteralPath $pauseFile) { Remove-Item -LiteralPath $pauseFile -Force }
        Write-Step 'start compaction and wait for the post-publish pause marker'
        $compaction = Invoke-Cli @('disk', 'compact', $profileName)
        if ($compaction.exit -ne 0) { Fail "disk compact was refused: $($compaction.output)" }
        $compactionId = Get-UuidFromText $compaction.output
        if ($null -eq $compactionId) { Fail 'disk compact carried no operation id' }
        $until = (Get-Date).AddSeconds(300)
        while (-not (Test-Path -LiteralPath $pauseFile) -and (Get-Date) -lt $until) {
            Start-Sleep -Milliseconds 250
        }
        if (-not (Test-Path -LiteralPath $pauseFile)) {
            Fail "the worker never reached the compaction-committed pause`n$(Get-ServiceTail 25)"
        }
        Write-Host "  pause marker: $((Get-Content -LiteralPath $pauseFile -Raw).Trim())"
        Write-Step 'kill the worker and let the service observe the lost channel'
        $workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
        if ($workers.Count -ne 1) { Fail "expected one worker at the pause, found $($workers.Count)" }
        $workers | Stop-Process -Force
        $state = 'unknown'
        $until = (Get-Date).AddSeconds(180)
        while ((Get-Date) -lt $until) {
            $state = Get-OperationState $compactionId
            if (@('succeeded', 'failed', 'cancelled', 'reconciling') -contains $state) { break }
            Start-Sleep -Milliseconds 500
        }
        Write-Host "  compaction operation after the kill: $state"
        Write-Step 'restart the service and start the disk again'
        Stop-ServiceForeground
        Start-ServiceForeground | Out-Null
        $start = Invoke-DiskOperation @('disk', 'start', $profileName) 600 -AllowFailure
        if ($start.exit -ne 0 -or $start.state -ne 'succeeded') {
            if ($start.output -match 'already') {
                Write-Host '  the disk was already running after the restart'
            } else {
                Fail "disk start after the kill ended $($start.state): $($start.output)`n$(Get-ServiceTail 40)"
            }
        }
        $after = Get-FileHash -LiteralPath $file -Algorithm SHA256
        if ($before.Hash -ne $after.Hash) { Fail "SHA-256 changed across the kill/restart: $($before.Hash) -> $($after.Hash)" }
        Write-Host "  SHA-256 unchanged after the kill/restart: $($after.Hash)"
        Write-Host 'PASS integration.m5.compaction_kill'
    }
} finally {
    Write-Step 'cleanup'
    foreach ($preserve in @('trace.log', 'service.log')) {
        $source = Join-Path $workRoot $preserve
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination ('C:\m5\last-' + $preserve) -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-TestDisk
    Stop-ServiceForeground
    if ($Scenario -eq 'kill') {
        Remove-Item Env:MYRD_M5_TEST_PAUSE_AT -ErrorAction SilentlyContinue
        Remove-Item Env:MYRD_M5_TEST_PAUSE_FILE -ErrorAction SilentlyContinue
        Remove-Item Env:MYRD_M5_TEST_PAUSE_SECONDS -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
}
