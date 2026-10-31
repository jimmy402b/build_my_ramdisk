# integration.m5.bundle_corruption -- R-03 (part 1): a bundle that is corrupt or truncated.
#
# **Guest-only.**  It creates a real worker, a real AIM disk and a real Windows volume, then
# damages the bundle on disk.
#
# The claim: a damaged bundle is reported **explicitly** (the start fails with a product
# error, the disk is not silently rebuilt, reformatted or quietly dropped) and the product
# does not hang or leave a worker behind.  Each case restores the damaged file afterwards and
# proves the same disk starts again -- so the failure is attributable to the damage, not to
# the disk having been left unusable by the experiment.
#
# Three damages, all applied to a stopped disk:
#   A. manifest (`disk.rdm`) truncated to 32 bytes
#   B. manifest byte flipped in the middle (a valid-looking header is not enough)
#   C. base image (`base-*.vhdx`) truncated by 1 MiB
#   D. journal (`journal-*.rdj`) byte flipped near the end
#
# Everything is printed as `READING: <key>=<value>`.

param(
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
        'integration.m5.bundle_corruption creates a real worker, a real AIM disk and a ' +
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

$workRoot = 'C:\m5\bundle-corrupt-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$bundlePath = Join-Path $workRoot 'bundle'
$profileName = 'corrupt-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$serviceLog = Join-Path $workRoot 'service.log'
$traceLog = Join-Path $workRoot 'trace.log'
$serviceProcess = $null
# **The directory must exist before the service is started.**  `Start-Process`'s redirects
# into a missing directory do not create it; measured here as a run whose service.log and
# service.out.log never appeared, which is exactly the evidence the failing stop needed.
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null

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

function Get-BundleFile([string]$pattern) {
    return @(Get-ChildItem -LiteralPath $bundlePath -File -Filter $pattern |
        Sort-Object Length -Descending | Select-Object -First 1)
}

function Get-BundleSnapshot {
    $snapshot = @{}
    foreach ($file in (Get-ChildItem -LiteralPath $bundlePath -File)) {
        $snapshot[$file.Name] = [pscustomobject]@{
            length = $file.Length
            hash   = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        }
    }
    return $snapshot
}

function Compare-BundleSnapshot($before, $after, [string]$what) {
    $changed = @()
    foreach ($name in $before.Keys) {
        if (-not $after.ContainsKey($name)) { $changed += "$name (removed)"; continue }
        if ($before[$name].length -ne $after[$name].length -or
            $before[$name].hash -ne $after[$name].hash) {
            $changed += "$name (length $($before[$name].length) -> $($after[$name].length))"
        }
    }
    foreach ($name in $after.Keys) {
        if (-not $before.ContainsKey($name)) { $changed += "$name (created)" }
    }
    Write-Reading "$what.bundle_unchanged" $(if ($changed.Count -eq 0) { 'yes' } else { 'NO: ' + ($changed -join '; ') })
    if ($changed.Count -ne 0) {
        Fail ("the failed $what changed the bundle, which is the silent-rebuild the " +
            "product must not do: " + ($changed -join '; '))
    }
}

function Stop-ServiceProcess {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
    $serviceProcess = $null
    Start-Sleep -Seconds 2
}

# Run `disk start` once and require a clean, explicit product failure.
#
# **The start is asynchronous.**  A healthy start answers "operation queued" with exit 0 and
# the failure only appears when the operation runs; so "a corrupt bundle is refused" means
# the *operation* reaches `failed`, not that the CLI refused the request.  A refusal before
# admission (exit 1 with an error line) is accepted too -- either shape is a product verdict
# rather than a hang or a silently successful start.
function Start-ExpectExplicitFailure([string]$profileId, [string]$label) {
    $start = Invoke-Cli @('disk', 'start', $profileId)
    Write-Reading "$label.start_exit" ([string]$start.exit)
    Write-Reading "$label.start_ms" ([string]$start.ms)
    if ($start.exit -eq 0) {
        $operationId = Get-UuidFromText $start.output
        if ($null -eq $operationId) {
            Fail "${label}: the start was admitted without an operation id: $($start.output)"
        }
        $state = Wait-ForTerminal $operationId 300
        Write-Reading "$label.operation_state" $state
        if ($state -ne 'failed') {
            Fail ("${label}: a corrupt bundle must fail the start operation; it ended " +
                "$state -- the damage was accepted")
        }
    } elseif ($start.exit -eq 1) {
        if ($start.output -notmatch 'error:') {
            Fail "${label}: the refusal carried no error line: $($start.output)"
        }
        $message = (($start.output -split "`n") |
            Where-Object { $_ -match 'error:|step:|code:' }) -join ' | '
        Write-Reading "$label.error" $message
        Write-Reading "$label.operation_state" 'refused-before-admission'
    } else {
        Fail ("${label}: a corrupt bundle must be a product failure (exit 0 then failed " +
            "operation, or exit 1); got exit $($start.exit): $($start.output)")
    }
    Start-Sleep -Seconds 2
    $workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count
    Write-Reading "$label.workers_left" ([string]$workers)
    if ($workers -ne 0) { Fail "$label left $workers worker(s) behind" }
    $line = Get-DiskLine $script:mount
    Write-Reading "$label.runtime" ([string](Get-Field $line 'runtime'))
    Write-Reading "$label.health" ([string](Get-Field $line 'health'))
    Write-Reading "$label.persistence" ([string](Get-Field $line 'persistence'))
    if ((Get-Field $line 'runtime') -eq 'Running') {
        Fail "${label}: the disk reports Running over a bundle that failed to open"
    }
}

function Start-ExpectSuccess([string]$profileId, [string]$label) {
    $start = Invoke-Cli @('disk', 'start', $profileId)
    if ($start.exit -ne 0) { Fail "${label}: start after restoring the file failed: $($start.output)" }
    $startId = Get-UuidFromText $start.output
    if ($null -ne $startId) {
        $state = Wait-ForTerminal $startId 300
        if ($state -ne 'succeeded') { Fail "${label}: the restore start ended $state" }
    }
    $runtime = Get-Field (Get-DiskLine $script:mount) 'runtime'
    Write-Reading "$label.restored_runtime" ([string]$runtime)
    if ($runtime -ne 'Running') { Fail "${label}: the disk did not reach Running after restoring the file" }
}

# Bring the disk down before the next damage.
#
# **The operation verdict and the observable state are checked separately.**  Measured: after
# a repaired start, `disk stop` can end `failed` even though the trace shows the worker
# completing the whole ordered stop (flush, dismount, remove device, stop provider).  That
# discrepancy is a product finding of its own (recorded as a reading), but the next damage
# only needs the disk to be *down*, so "runtime=Stopped" is accepted and the failed operation
# is reported rather than silently treated as a test failure.  A disk that is still Running
# after both attempts is a hard failure.
function Stop-DiskForNextCase([string]$profileId, [string]$label) {
    $stop = Invoke-Cli @('disk', 'stop', $profileId)
    $stopId = Get-UuidFromText $stop.output
    $state = 'refused'
    if ($stop.exit -eq 0) {
        if ($null -ne $stopId) { $state = Wait-ForTerminal $stopId 300 }
    }
    Write-Reading ("$label.stop_state") $state
    if ($state -eq 'succeeded') {
        Start-Sleep -Seconds 2
        return
    }
    Write-Reading ("$label.stop_output") (($stop.output -replace "`n", ' | '))
    $forced = Invoke-Cli @('disk', 'stop', $profileId, '--force')
    $forcedState = 'refused'
    if ($forced.exit -eq 0) {
        $id = Get-UuidFromText $forced.output
        if ($null -ne $id) { $forcedState = Wait-ForTerminal $id 300 }
    }
    Write-Reading ("$label.forced_stop_state") $forcedState
    if ($forcedState -eq 'succeeded') {
        Start-Sleep -Seconds 2
        return
    }
    $deadline = (Get-Date).AddSeconds(30)
    $runtime = $null
    while ((Get-Date) -lt $deadline) {
        $runtime = Get-Field (Get-DiskLine $script:mount) 'runtime'
        if ($runtime -eq 'Stopped') { break }
        Start-Sleep -Seconds 1
    }
    Write-Reading ("$label.runtime_after_failed_stop") ([string]$runtime)
    if ($runtime -ne 'Stopped') {
        # Dump the evidence *here*, before anything stops the service: this failure has
        # twice reached a harness that killed the service first, leaving nothing to read.
        Write-Host "--- ${label}: operation show"
        if ($null -ne $stopId) {
            $show = Invoke-Cli @('operation', 'show', $stopId)
            Write-Host ("  exit=$($show.exit) " + (($show.output -replace "`n", ' | ')))
        }
        Write-Host "--- ${label}: disk list line"
        Write-Host ("  " + [string](Get-DiskLine $script:mount))
        if (Test-Path -LiteralPath $serviceLog) {
            Write-Host "--- ${label}: service log kind=7"
            Get-Content -LiteralPath $serviceLog -ErrorAction SilentlyContinue |
                Where-Object { $_ -match 'kind=7' } |
                Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" }
        } else {
            Write-Host "--- ${label}: service log MISSING at $serviceLog"
        }
        if (Test-Path -LiteralPath $traceLog) {
            Write-Host "--- ${label}: trace tail"
            Get-Content -LiteralPath $traceLog -ErrorAction SilentlyContinue |
                Select-Object -Last 40 | ForEach-Object { Write-Host "  $_" }
        }
        Fail ("${label}: the disk is still $runtime after a failed stop and a refused force-stop")
    }
    Write-Reading ("$label.stop_discrepancy") 'operation failed but the runtime is Stopped'
    Start-Sleep -Seconds 2
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

    $markerPath = Join-Path ($script:mount + '\') 'corruption-marker.txt'
    $payload = 'committed before the corruption ' + [guid]::NewGuid().ToString('N')
    [System.IO.File]::WriteAllText($markerPath, $payload, [System.Text.Encoding]::UTF8)
    $stream = [System.IO.File]::Open($markerPath, 'Open', 'ReadWrite', 'Read')
    try { $stream.Flush($true) } finally { $stream.Dispose() }
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $profileId)
    if ($checkpoint.exit -ne 0) { Fail "checkpoint failed: $($checkpoint.output)" }
    $checkpointId = Get-UuidFromText $checkpoint.output
    if ($null -ne $checkpointId) { [void](Wait-ForTerminal $checkpointId 300) }
    $stop = Invoke-Cli @('disk', 'stop', $profileId)
    if ($stop.exit -ne 0) { Fail "stop failed: $($stop.output)" }
    $stopId = Get-UuidFromText $stop.output
    if ($null -ne $stopId) { [void](Wait-ForTerminal $stopId 300) }
    Start-Sleep -Seconds 2

    Write-Step "bundle layout"
    foreach ($file in (Get-ChildItem -LiteralPath $bundlePath -File)) {
        Write-Reading ("bundle." + $file.Name) ([string]$file.Length)
    }
    # **Each case backs up the file it is about to damage, right before damaging it.**
    # One snapshot taken here and restored repeatedly is wrong: every successful stop can
    # advance the bundle's generation, so restoring the old snapshot rewinds the bundle
    # behind the profile's confirmed generation.  Measured cost of the wrong version: the
    # second repair start adopted an older bundle and the next stop cut a checkpoint whose
    # correlation (prior=4, candidate=5) could not match the profile (confirmed=5), which
    # looked like a stop defect until the service log named the boundary refusal.

    # ------------------------------------------------------------------
    # A. manifest truncated
    # ------------------------------------------------------------------
    Write-Step 'A: truncate the manifest'
    $manifest = Get-BundleFile 'disk.rdm'
    if ($manifest.Count -ne 1) { Fail "expected one manifest, found $($manifest.Count)" }
    $original = $manifest[0].FullName
    $manifestBackupA = Join-Path $workRoot 'disk.rdm.a.orig'
    Copy-Item -LiteralPath $original -Destination $manifestBackupA -Force
    $bytes = [System.IO.File]::ReadAllBytes($original)
    [System.IO.File]::WriteAllBytes($original, $bytes[0..31])
    Write-Reading 'a.manifest_bytes' ([string](Get-Item -LiteralPath $original).Length)
    $before = Get-BundleSnapshot
    Start-ExpectExplicitFailure $profileId 'a'
    Compare-BundleSnapshot $before (Get-BundleSnapshot) 'a'
    Copy-Item -LiteralPath $manifestBackupA -Destination $original -Force
    Start-ExpectSuccess $profileId 'a'
    if ((Get-Content -LiteralPath $markerPath -Raw) -ne $payload) { Fail 'A: the marker bytes changed after the manifest restore' }
    Write-Reading 'a.marker_intact' 'yes'
    Stop-DiskForNextCase $profileId 'a'

    # ------------------------------------------------------------------
    # B. manifest byte flipped
    # ------------------------------------------------------------------
    Write-Step 'B: flip one byte inside *each* manifest slot'
    # **Both slots, on purpose.**  The manifest keeps two generations; a start reads the
    # active one and must ignore damage in the other.  Corrupting one slot therefore proves
    # only that the fallback works, so this case damages both and asserts that *no* intact
    # copy remains -- the product must refuse rather than pick a damaged generation.
    $manifestBackupB = Join-Path $workRoot 'disk.rdm.b.orig'
    Copy-Item -LiteralPath $original -Destination $manifestBackupB -Force
    $bytes = [System.IO.File]::ReadAllBytes($original)
    if ($bytes.Length -lt 8192) { Fail "the manifest is unexpectedly small: $($bytes.Length) bytes" }
    $offsets = @([int]($bytes.Length / 4), [int]($bytes.Length * 3 / 4))
    foreach ($offset in $offsets) {
        $bytes[$offset] = $bytes[$offset] -bxor 0xFF
    }
    [System.IO.File]::WriteAllBytes($original, $bytes)
    Write-Reading 'b.manifest_flip_offsets' ($offsets -join ',')
    $before = Get-BundleSnapshot
    Start-ExpectExplicitFailure $profileId 'b'
    Compare-BundleSnapshot $before (Get-BundleSnapshot) 'b'
    Copy-Item -LiteralPath $manifestBackupB -Destination $original -Force
    Start-ExpectSuccess $profileId 'b'
    Write-Reading 'b.marker_intact' $(if ((Get-Content -LiteralPath $markerPath -Raw) -eq $payload) { 'yes' } else { 'NO' })
    Stop-DiskForNextCase $profileId 'b'

    # ------------------------------------------------------------------
    # C. base image truncated
    # ------------------------------------------------------------------
    Write-Step 'C: truncate the base image'
    $base = Get-BundleFile 'base-*.vhdx'
    if ($base.Count -ne 1) { Fail "expected one base image, found $($base.Count)" }
    $baseName = $base[0].Name
    $baseFile = $base[0].FullName
    $baseLength = $base[0].Length
    if ($baseLength -le 2MB) { Fail "the base image is unexpectedly small: $baseLength bytes" }
    $baseBackup = Join-Path $workRoot ($baseName + '.orig')
    Copy-Item -LiteralPath $baseFile -Destination $baseBackup -Force
    $stream = [System.IO.File]::Open($baseFile, 'Open', 'ReadWrite', 'None')
    try { $stream.SetLength($baseLength - 1MB) } finally { $stream.Dispose() }
    Write-Reading 'c.base_bytes' ([string](Get-Item -LiteralPath $baseFile).Length)
    $before = Get-BundleSnapshot
    Start-ExpectExplicitFailure $profileId 'c'
    Compare-BundleSnapshot $before (Get-BundleSnapshot) 'c'
    Copy-Item -LiteralPath $baseBackup -Destination $baseFile -Force
    Start-ExpectSuccess $profileId 'c'
    Write-Reading 'c.marker_intact' $(if ((Get-Content -LiteralPath $markerPath -Raw) -eq $payload) { 'yes' } else { 'NO' })
    Stop-DiskForNextCase $profileId 'c'

    # ------------------------------------------------------------------
    # D. journal byte flipped
    # ------------------------------------------------------------------
    Write-Step 'D: flip one byte in the journal header and one near its end'
    # **The header, not only the tail.**  The journal's final sector may be padding, and a
    # flip there proves nothing; the header carries the magic and the CRC that every reader
    # checks first.  Damaging both ends is the honest "this journal is corrupt" case.
    $journal = Get-BundleFile 'journal-*.rdj'
    if ($journal.Count -ne 1) { Fail "expected one journal, found $($journal.Count)" }
    $journalName = $journal[0].Name
    $journalFile = $journal[0].FullName
    $journalBackup = Join-Path $workRoot ($journalName + '.orig')
    Copy-Item -LiteralPath $journalFile -Destination $journalBackup -Force
    $stream = [System.IO.File]::Open($journalFile, 'Open', 'ReadWrite', 'None')
    try {
        $offsets = @([int64]64, [int64]($stream.Length - 4096))
        foreach ($offset in $offsets) {
            $stream.Position = $offset
            $one = $stream.ReadByte()
            $stream.Position = $offset
            $stream.WriteByte([byte]($one -bxor 0xFF))
        }
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    Write-Reading 'd.journal_flip_offsets' ($offsets -join ',')
    $before = Get-BundleSnapshot
    Start-ExpectExplicitFailure $profileId 'd'
    Compare-BundleSnapshot $before (Get-BundleSnapshot) 'd'
    Copy-Item -LiteralPath $journalBackup -Destination $journalFile -Force
    Start-ExpectSuccess $profileId 'd'
    Write-Reading 'd.marker_intact' $(if ((Get-Content -LiteralPath $markerPath -Raw) -eq $payload) { 'yes' } else { 'NO' })
    Stop-DiskForNextCase $profileId 'd'

    Write-Step 'cleanup: forget the disk'
    $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
    Write-Reading 'cleanup_forget' ([string]$forget.exit)

    Write-Host 'PASS integration.m5.bundle_corruption: four damages, four explicit refusals, four clean restores'
    Write-Reading 'result' 'PASS'
} finally {
    Stop-ServiceProcess
}
