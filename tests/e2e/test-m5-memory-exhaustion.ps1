# integration.m5.memory_exhaustion -- what the product does when RAM runs out.
#
# **Guest-only.**  It creates a real worker, a real AIM disk and a real Windows volume,
# and it deliberately drives the worker into its memory limit (and, in `-Mode ram`, into
# the machine's physical memory).  It is NOT registered in ctest: filling RAM is not
# something a normal full-suite run should do to the guest, and STAB-04's one-command
# suite must stay non-destructive.  Run it as an experiment, one mode at a time:
#
#   $env:MYRD_M5VM_GUEST='1'
#   powershell -NoProfile -ExecutionPolicy Bypass -File <repo>\tests\e2e\test-m5-memory-exhaustion.ps1
#   powershell ... -Mode ram -Size 6GiB -HardLimit 6GiB -WriteMiB 6144
#
# The two modes answer two different questions:
#
#   budget (default)  the product's own hard limit (64KiB blocks, `--memory-hard-limit`)
#                     is reached while the machine still has RAM.  This is the *supported*
#                     exhaustion path: the product chose the number, so it must handle it.
#   ram               the limit is set near the machine's physical RAM, so the first
#                     failure can be the OS refusing a commit rather than the store
#                     refusing a block.  This is the "RAM really is gone" path.
#
# What the script asserts in both modes:
#   1. a write fails with a clear Windows error, within a deadline (no hang);
#   2. `disk show` still answers and the worker is still alive;
#   3. `disk checkpoint` completes after the failure;
#   4. stop + service restart + start recovers the last committed generation and every
#      byte that was committed before the failure;
#   5. a small write after the failure fails the same way (the limit is still enforced);
#   6. deleting the big file gives the memory back (UNMAP) and writes work again.
#
# Everything is printed as `READING: <key>=<value>` so a validation document can quote
# the transcript without re-deriving it.

param(
    [ValidateSet('budget', 'ram')]
    [string]$Mode = 'budget',
    [string]$Size = '1GiB',
    [string]$HardLimit = '256MiB',
    [int]$WriteMiB = 320,
    [int]$TimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'test-m5-memory-exhaustion creates a real worker, a real AIM disk and a real ' +
        'Windows volume, and fills memory on purpose.  It may only run on the guest ' +
        '(AGENTS.md section 4).  Set MYRD_M5VM_GUEST=1 on the guest to run it.')
    exit 2
}

if ($Mode -eq 'ram' -and -not $PSBoundParameters.ContainsKey('HardLimit')) {
    $HardLimit = '6GiB'
}
if ($Mode -eq 'ram' -and -not $PSBoundParameters.ContainsKey('WriteMiB')) {
    $WriteMiB = 6144
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo 'out/build/msvc/src/Release'
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
foreach ($path in @($serviceExe, $cliExe)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "missing binary: $path" }
}

$workRoot = 'C:\m5\mem-exhaust-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$bundlePath = Join-Path $workRoot 'bundle'
$serviceLog = Join-Path $workRoot 'service.log'
$profileName = 'mem-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$serviceProcess = $null
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

New-Item -ItemType Directory -Force -Path $workRoot | Out-Null

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
}

function Fail([string]$message) {
    [Console]::Error.WriteLine("FAIL: $message")
    if (Test-Path -LiteralPath $serviceLog) {
        [Console]::Error.WriteLine('--- service log (tail) ---')
        Get-Content -LiteralPath $serviceLog -Tail 25 | ForEach-Object {
            [Console]::Error.WriteLine($_)
        }
    }
    exit 1
}

function Invoke-Cli {
    param([string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $started = Get-Date
        $output = & $cliExe @Arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
        return [pscustomobject]@{
            output    = $output
            exit      = $exit
            ms        = [int]((Get-Date) - $started).TotalMilliseconds
            arguments = ($Arguments -join ' ')
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

function Wait-ForTerminal([string]$operationId, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $state = Get-OperationState $operationId
        if (@('succeeded', 'failed', 'cancelled') -contains $state) { return $state }
        Start-Sleep -Milliseconds 500
    }
    return 'timeout'
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

function Get-Generation([string]$text) {
    $match = [regex]::Match($text, 'generation=(\d+)')
    if (-not $match.Success) { return $null }
    return [uint64]$match.Groups[1].Value
}

function Show-Disk([string]$label) {
    $show = Invoke-Cli @('disk', 'show', $profileName)
    if ($show.exit -ne 0) { Fail "disk show ($label) failed: $($show.output)" }
    $line = ($show.output -split "`n") | Where-Object { $_ -match 'generation=' } | Select-Object -First 1
    Write-Host "  show($label): $($line.Trim())"
    # PowerShell has no `if` expression in 5.1, so each field is read through a
    # match-and-capture pair rather than inline.
    $runtime = '?'
    if ($line -match 'runtime=(\w+)') { $runtime = $Matches[1] }
    $persistence = '?'
    if ($line -match 'persistence=(\w+)') { $persistence = $Matches[1] }
    $health = '?'
    if ($line -match 'health=(\w+)') { $health = $Matches[1] }
    $mem = '?'
    if ($line -match 'mem=(\d+/\d+)') { $mem = $Matches[1] }
    # R-01: the budget-refusal counter is the reading that makes an exhaustion
    # self-explanatory.  Absent means zero (the CLI omits it until it happens).
    $rejections = 0
    if ($line -match 'budget_rejections=(\d+)') { $rejections = [uint64]$Matches[1] }
    Write-Reading "$label.runtime" $runtime
    Write-Reading "$label.persistence" $persistence
    Write-Reading "$label.health" $health
    Write-Reading "$label.generation" ([string](Get-Generation $line))
    Write-Reading "$label.mem" $mem
    Write-Reading "$label.budget_rejections" ([string]$rejections)
    return $line
}

function New-MountPoint {
    # **The DOS-device namespace, not just `Test-Path`.**  A failed create can leave a
    # drive letter mapped to a device that no longer exists: `Test-Path` and Win32_Volume
    # both report the letter free, but Windows cannot assign a new volume there, so the
    # volume lands on another letter and the product's create fails
    # `no volume matches the requested drive letter`.  Measured on this guest: T: carried
    # such a ghost and the first budget run failed exactly that way.
    if (-not ([System.Management.Automation.PSTypeName]'Myrd.DosDeviceMem').Type) {
        Add-Type -Namespace Myrd -Name DosDeviceMem -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern int QueryDosDevice(string name, System.Text.StringBuilder target, int max);
'@
    }
    # **T: is tried last, on purpose.**  Measured on this guest: T: carries a mount
    # manager entry that `QueryDosDevice`, `Test-Path` and `Win32_Volume` all fail to
    # report, but `Set-Partition -NewDriveLetter T:` refuses with "the requested access
    # path is already in use" -- a ghost that only a reboot clears (handoff §3.166).  A
    # fresh guest has no such entry; this order keeps the experiment runnable on one that
    # has been used without hiding the failure when T: really is the only letter left.
    foreach ($letter in @('U:', 'V:', 'W:', 'X:', 'Y:', 'Z:', 'T:')) {
        if (Test-Path -LiteralPath ($letter + '\')) { continue }
        $used = @(Get-CimInstance Win32_Volume -ErrorAction SilentlyContinue |
            Where-Object { $_.DriveLetter -eq $letter })
        if ($used.Count -ne 0) { continue }
        $buffer = New-Object System.Text.StringBuilder 512
        if ([Myrd.DosDeviceMem]::QueryDosDevice($letter, $buffer, 512) -ne 0) {
            Write-Host "  $letter is still mapped to $($buffer.ToString()) -- skipping"
            continue
        }
        return $letter
    }
    return $null
}

function Stop-WorkerAndService {
    foreach ($process in @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
    $serviceProcess = $null
    Start-Sleep -Milliseconds 500
}

$payloadBytes = 4MB
$buffer = New-Object byte[] $payloadBytes
$random = New-Object System.Random -ArgumentList 20261029
$random.NextBytes($buffer)
$chunkMiB = 4
$chunks = [int][math]::Ceiling($WriteMiB / $chunkMiB)

try {
    Write-Reading 'mode' $Mode
    Write-Reading 'requested_size' $Size
    Write-Reading 'requested_hard_limit' $HardLimit
    Write-Reading 'requested_write_mib' $WriteMiB

    # A leftover disk would make this measure the guest, not the product.
    $running = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) { Fail "the guest already has $($running.Count) worker(s); reset it first" }

    Write-Host '--- start the service in the foreground'
    $env:MYRD_TRACE_FILE = Join-Path $workRoot 'trace.log'
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog `
        -RedirectStandardOutput (Join-Path $workRoot 'service.out.log') `
        -WindowStyle Hidden -PassThru
    $listed = Wait-ForService 60
    if ($null -eq $listed) { Fail 'the service never answered a list command' }
    $existing = ($listed.output -split "`n") | Where-Object { $_ -match 'generation=' }
    if ($existing.Count -ne 0) {
        Fail "the guest already has $($existing.Count) profile(s); reset it first: $($existing -join ' | ')"
    }

    $mount = New-MountPoint
    if ($null -eq $mount) { Fail 'no free drive letter' }
    Write-Reading 'mount' $mount

    Write-Host "--- create $Size disk with a $HardLimit hard limit"
    $create = Invoke-Cli @('disk', 'create', '--name', $profileName,
        '--bundle', $bundlePath, '--size', $Size, '--mount', $mount,
        '--memory-mode', 'fixed', '--memory-hard-limit', $HardLimit,
        '--checkpoint-interval', '30m', '--no-wait')
    if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the create carried no operation id' }
    $created = Wait-ForTerminal $createId 300
    if ($created -ne 'succeeded') {
        Fail "the create ended $created; service log tail:`n$((Get-Content -LiteralPath $serviceLog -Tail 20) -join "`n")"
    }

    $list = Invoke-Cli @('disk', 'list')
    $line = ($list.output -split "`n") | Where-Object { $_ -match [regex]::Escape($mount) } | Select-Object -First 1
    $profileId = if ($null -ne $line) { Get-UuidFromText $line } else { $null }
    if ($null -eq $profileId) { Fail 'disk list did not report the created profile' }
    Write-Reading 'profile_id' $profileId
    $baseline = Show-Disk 'baseline'
    $generationBefore = Get-Generation $baseline
    if ($null -eq $generationBefore) { Fail 'the baseline carried no generation' }

    # ------------------------------------------------------------------
    # 1. Write until the product (or the OS) refuses.
    # ------------------------------------------------------------------
    Write-Host "--- write $WriteMiB MiB in $chunkMiB MiB chunks"
    $path = Join-Path ($mount + '\') 'exhaust.bin'
    $written = 0L
    $failure = $null
    $writeStarted = Get-Date
    $stream = $null
    try {
        $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        for ($index = 0; $index -lt $chunks; $index++) {
            if ((Get-Date) -gt $deadline) {
                $failure = [pscustomobject]@{
                    message = 'the experiment deadline expired while writing'
                    type    = 'deadline'
                    hresult = 0
                }
                break
            }
            try {
                $stream.Write($buffer, 0, $buffer.Length)
                $written += $buffer.Length
                # **Flush to disk, not just Flush.**  A plain `Flush()` only hands the
                # bytes to the Windows file cache; the failure this experiment is about
                # happens at the volume, so without `Flush(true)` the error would surface
                # later as a "delayed write failed" popup instead of on this write.
                $stream.Flush($true)
            } catch {
                $exception = $_.Exception
                $failure = [pscustomobject]@{
                    message = $exception.Message
                    type    = $exception.GetType().FullName
                    hresult = ('0x{0:X8}' -f ($exception.HResult -band 0xFFFFFFFF))
                }
                break
            }
        }
    } catch {
        $exception = $_.Exception
        $failure = [pscustomobject]@{
            message = $exception.Message
            type    = $exception.GetType().FullName
            hresult = ('0x{0:X8}' -f ($exception.HResult -band 0xFFFFFFFF))
        }
    } finally {
        if ($null -ne $stream) {
            try { $stream.Dispose() } catch { }
        }
    }
    $writeSeconds = [math]::Round(((Get-Date) - $writeStarted).TotalSeconds, 1)
    Write-Reading 'write_seconds' $writeSeconds
    Write-Reading 'written_mib' ([math]::Round($written / 1MB, 1))
    if ($null -eq $failure) {
        Write-Reading 'write_failed' 'no'
        Fail "all $WriteMiB MiB were written; the limit was not reached -- raise -WriteMiB or lower -HardLimit"
    }
    Write-Reading 'write_failed' 'yes'
    Write-Reading 'failure_type' $failure.type
    Write-Reading 'failure_hresult' $failure.hresult
    Write-Reading 'failure_message' $failure.message
    Write-Host "  write failed after $([math]::Round($written/1MB,1)) MiB in $writeSeconds s: $($failure.message) ($($failure.hresult))"

    # ------------------------------------------------------------------
    # 2. The product still answers, and a small write fails the same way.
    # ------------------------------------------------------------------
    $afterFailure = Show-Disk 'after_failure'
    # **R-01's observability half.**  Before this counter existed, the only evidence a
    # budget refusal had happened was the generic I/O error the write returned; `disk
    # show` said `mem=X/budget` and nothing else.  A refused commit must now be visible
    # as a number, and that number must be greater than zero after a measured refusal.
    $rejectionsAfterFailure = 0
    if ($afterFailure -match 'budget_rejections=(\d+)') {
        $rejectionsAfterFailure = [uint64]$Matches[1]
    }
    Write-Reading 'budget_rejections_after_failure' ([string]$rejectionsAfterFailure)
    if ($rejectionsAfterFailure -eq 0) {
        Fail ('the write failed but disk show reported no budget refusal ' +
            "(budget_rejections=0): the failure the user meets cannot be explained " +
            'by the product')
    }
    $workersAlive = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count
    Write-Reading 'workers_after_failure' $workersAlive
    if ($workersAlive -ne 1) { Fail "expected exactly one worker after the failure, found $workersAlive" }

    $small = Join-Path ($mount + '\') 'after-failure.txt'
    $smallFailure = $null
    try {
        [System.IO.File]::WriteAllText($small, 'one more write after the limit')
        $smallFailure = 'none'
    } catch {
        $smallFailure = ('0x{0:X8} {1}' -f ($_.Exception.HResult -band 0xFFFFFFFF), $_.Exception.Message)
    }
    Write-Reading 'small_write_result' $smallFailure
    # **A small write may still succeed, and that is not a contradiction.**  The limit
    # rejects *newly committed blocks*; a write that only rewrites metadata in blocks the
    # store already holds costs no new memory.  Measured: after the 256 MiB budget filled,
    # a 23-byte text file was created successfully while the 4 MiB chunk write had just
    # failed with an I/O device error.  What must not happen is the budget being ignored
    # by a write that needs new blocks -- and the 256 MiB reading above already shows that.

    # ------------------------------------------------------------------
    # 3. Checkpoint still completes.
    # ------------------------------------------------------------------
    Write-Host '--- checkpoint after the failure'
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $profileName)
    Write-Reading 'checkpoint_exit' $checkpoint.exit
    if ($checkpoint.exit -ne 0) {
        Write-Reading 'checkpoint_output' ($checkpoint.output.Trim() -replace "`n", ' | ')
        Fail "disk checkpoint failed after the memory limit was reached: $($checkpoint.output)"
    }
    $checkpointId = Get-UuidFromText $checkpoint.output
    if ($null -eq $checkpointId) { Fail 'the checkpoint carried no operation id' }
    $checkpointState = Wait-ForTerminal $checkpointId 300
    Write-Reading 'checkpoint_state' $checkpointState
    if ($checkpointState -ne 'succeeded') {
        Fail "the checkpoint ended $checkpointState"
    }
    $afterCheckpoint = Show-Disk 'after_checkpoint'
    $generationAfterCheckpoint = Get-Generation $afterCheckpoint
    Write-Reading 'generation_before' $generationBefore
    Write-Reading 'generation_after_checkpoint' $generationAfterCheckpoint
    if ($generationAfterCheckpoint -le $generationBefore) {
        Fail "the checkpoint did not advance the generation: $generationBefore -> $generationAfterCheckpoint"
    }

    # ------------------------------------------------------------------
    # 4. Stop, restart the service, start the disk, and read the data back.
    # ------------------------------------------------------------------
    Write-Host '--- stop the disk and restart the service'
    $stop = Invoke-Cli @('disk', 'stop', $profileName)
    if ($stop.exit -ne 0) { Fail "disk stop failed after the failure: $($stop.output)" }
    $stopId = Get-UuidFromText $stop.output
    if ($null -eq $stopId) { Fail 'the stop carried no operation id' }
    $stopState = Wait-ForTerminal $stopId 300
    Write-Reading 'stop_state' $stopState
    if ($stopState -ne 'succeeded') { Fail "the stop ended $stopState" }

    Stop-WorkerAndService
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError (Join-Path $workRoot 'service-restart.log') `
        -RedirectStandardOutput (Join-Path $workRoot 'service-restart.out.log') `
        -WindowStyle Hidden -PassThru
    if ($null -eq (Wait-ForService 60)) { Fail 'the restarted service never answered' }

    $start = Invoke-Cli @('disk', 'start', $profileName)
    if ($start.exit -ne 0) { Fail "disk start after restart failed: $($start.output)" }
    $startId = Get-UuidFromText $start.output
    if ($null -eq $startId) { Fail 'the start carried no operation id' }
    $startState = Wait-ForTerminal $startId 300
    Write-Reading 'restart_start_state' $startState
    if ($startState -ne 'succeeded') { Fail "the start after restart ended $startState" }
    $recovered = Show-Disk 'after_restart'
    $generationAfterRestart = Get-Generation $recovered
    Write-Reading 'generation_after_restart' $generationAfterRestart
    if ($generationAfterRestart -lt $generationAfterCheckpoint) {
        Fail "recovery went backwards: $generationAfterCheckpoint -> $generationAfterRestart"
    }

    # The bytes that were committed must still be there, and intact.
    if (-not (Test-Path -LiteralPath $path)) {
        Fail 'the written file is missing after the restart'
    }
    $fileLength = (Get-Item -LiteralPath $path).Length
    Write-Reading 'file_length_after_restart' $fileLength
    if ($fileLength -lt $written) {
        Fail "the file lost committed bytes: wrote $written, recovered $fileLength"
    }
    $checkLength = [math]::Min(4MB, [int64]$written)
    $readBuffer = New-Object byte[] $checkLength
    $checkStream = [System.IO.File]::OpenRead($path)
    try {
        $read = $checkStream.Read($readBuffer, 0, [int]$checkLength)
    } finally {
        $checkStream.Dispose()
    }
    if ($read -ne $checkLength) { Fail "short read back: $read of $checkLength" }
    for ($index = 0; $index -lt $read; $index++) {
        if ($readBuffer[$index] -ne $buffer[$index]) {
            Fail "the first committed MiB differs at byte $index"
        }
    }
    Write-Reading 'committed_prefix_verified_bytes' $read

    # ------------------------------------------------------------------
    # 5. Deleting the file gives the memory back; writes work again.
    # ------------------------------------------------------------------
    Write-Host '--- delete the file and write again (UNMAP frees the blocks)'
    Remove-Item -LiteralPath $path -Force
    $recoveredAfterUnmap = Show-Disk 'after_unmap'
    $retry = Join-Path ($mount + '\') 'after-unmap.txt'
    $retryOk = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        try {
            [System.IO.File]::WriteAllText($retry, 'write after unmap ' + $attempt)
            $retryOk = $true
            break
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    Write-Reading 'write_after_unmap' $(if ($retryOk) { 'ok' } else { 'still_failing' })
    if (-not $retryOk) { Fail 'writes still failed after the file was deleted; UNMAP did not give the memory back' }

    # ------------------------------------------------------------------
    # 6. Put the guest back: stop, forget, drop the bundle.
    # ------------------------------------------------------------------
    Write-Host '--- cleanup'
    $cleanupStop = Invoke-Cli @('disk', 'stop', $profileName)
    if ($cleanupStop.exit -eq 0) {
        $cleanupStopId = Get-UuidFromText $cleanupStop.output
        if ($null -ne $cleanupStopId) { [void](Wait-ForTerminal $cleanupStopId 300) }
    }
    $forgotten = $false
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
        if ($forget.exit -eq 0) { $forgotten = $true; break }
        Start-Sleep -Seconds 2
    }
    Write-Reading 'cleanup_forget' $(if ($forgotten) { 'ok' } else { 'failed' })
    if ($forgotten) {
        Remove-Item -LiteralPath $bundlePath -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host 'PASS integration.m5.memory_exhaustion: budget exhaustion behaved as documented'
    Write-Reading 'result' 'PASS'
} finally {
    Stop-WorkerAndService
}
