# integration.m5.cluster_size -- a create can choose the NTFS cluster size, and the volume it
# formats really has that size (R-17a).
#
# **Guest-only.**  It creates a real worker, AIM disk and NTFS volume, and reads the cluster
# size back from the formatted volume with `Get-Volume`.
#
# The option is create-only on purpose: a cluster size is a property of the formatting, so it
# rides the create intent into the worker's provisioning helper and never enters the durable
# record.  The accepted set (4 KiB..64 KiB, powers of two) is enforced in the CLI, the wire
# decoder, the helper launch and the provisioning script; this case proves the two ends that a
# user can see -- the refusal and the actual `AllocationUnitSize`.

param(
    [string]$Configuration = 'Release',
    [int]$ClusterSizeKiB = 64
)

$ErrorActionPreference = 'Stop'

trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.cluster_size creates a real worker, AIM disk and volume; it may only ' +
        'run on the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

$workRoot = 'C:\m5\cluster-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
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
        # **A failed operation makes `operation show` exit 1; that is the answer, not an
        # error.**  The state is still in the output, so it is parsed either way.
        $match = [regex]::Match($show.output,
            '(?m)^operation\s+\S+:\s+(succeeded|completed|failed|cancelled)\b')
        if ($match.Success) {
            if ($match.Groups[1].Value -eq 'completed') { return 'succeeded' }
            return $match.Groups[1].Value
        }
        Start-Sleep -Milliseconds 200
    }
    return 'timeout'
}

function Wait-ForRunning([string]$id, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $show = Invoke-Cli @('disk', 'show', $id)
        if ($show.output -match 'running') { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
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
    if ($null -eq (Wait-ForService 90)) { Fail 'the service never answered' }
    $mount = Select-FreeLetter
    if ($null -eq $mount) { Fail 'no free drive letter' }
    Write-Reading 'mount' $mount

    Write-Step 'an invalid cluster size is refused before any request'
    $invalid = Invoke-Cli @('disk', 'create', '--name', 'bad-cluster',
        '--bundle', (Join-Path $workRoot 'bad-bundle'), '--size', '64MiB',
        '--mount', 'T:', '--cluster-size', '10000')
    Write-Reading 'invalid.exit' ([string]$invalid.exit)
    Write-Reading 'invalid.output' (($invalid.output -replace "`n", ' | '))
    if ($invalid.exit -ne 2) { Fail "an invalid cluster size must be a usage failure; got $($invalid.exit)" }
    if ($invalid.output -notmatch 'cluster-size must be a power of two between 4KiB and 64KiB') {
        Fail "the refusal did not name the accepted set: $($invalid.output)"
    }

    Write-Step "create a 64 MiB disk with a $ClusterSizeKiB KiB cluster size"
    $create = Invoke-Cli @('disk', 'create', '--name', 'cluster',
        '--bundle', $bundlePath, '--size', '64MiB', '--mount', $mount,
        '--checkpoint-interval', '30m', '--cluster-size', "$($ClusterSizeKiB)KiB")
    if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the create carried no operation id' }
    $state = Wait-ForTerminal $createId 300
    Write-Reading 'create.state' $state
    if ($state -ne 'succeeded') { Fail "the create ended $state" }

    Write-Step 'the formatted volume really has that cluster size'
    $letter = $mount.TrimEnd(':')
    # The operation reaching `succeeded` is not the same instant the volume gets its letter
    # (the mount is one of the last worker actions), so the read is retried.
    $volume = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $volume = Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue
        if ($null -ne $volume) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($null -eq $volume) { Fail "no volume appeared at $mount" }
    Write-Reading 'volume.filesystem' ([string]$volume.FileSystem)
    Write-Reading 'volume.allocation_unit_bytes' ([string]$volume.AllocationUnitSize)
    if ($volume.FileSystem -ne 'NTFS') { Fail "expected NTFS, found $($volume.FileSystem)" }
    if ([uint32]$volume.AllocationUnitSize -ne [uint32]($ClusterSizeKiB * 1024)) {
        Fail ("the volume's allocation unit is $($volume.AllocationUnitSize), expected " +
            "$($ClusterSizeKiB * 1024)")
    }
    Write-Reading 'volume.cluster_size_match' 'yes'

    # **`fsutil` prints in the OS language, so its text is not parsed here.**  Measured on
    # this zh-CN guest: the label is `每簇字节数`, and a regex over it kept missing (the
    # child's output crosses two code pages).  `Win32_Volume.BlockSize` is the same number,
    # locale-independent, and comes from the volume itself rather than from the Storage
    # module's cached view, so the reading is still independent of `Get-Volume`.
    $cimVolume = Get-CimInstance -ClassName Win32_Volume `
        -Filter "DriveLetter='$($letter):'" -ErrorAction SilentlyContinue
    if ($null -eq $cimVolume) { Fail "Win32_Volume did not report the volume at $mount" }
    Write-Reading 'cim.block_size' ([string]$cimVolume.BlockSize)
    if ([uint32]$cimVolume.BlockSize -ne [uint32]($ClusterSizeKiB * 1024)) {
        Fail ("Win32_Volume.BlockSize is $($cimVolume.BlockSize), expected " +
            "$($ClusterSizeKiB * 1024)")
    }
    Write-Reading 'cim.cluster_size_match' 'yes'

    Write-Step 'a stop and start keeps the formatted cluster size (adopt, not reformat)'
    $line = Invoke-Cli @('disk', 'list')
    $profileId = Get-UuidFromText $line.output
    if ($null -eq $profileId) { Fail 'the created disk is not listed' }
    $stop = Invoke-Cli @('disk', 'stop', $profileId)
    if ($stop.exit -ne 0) { Fail "stop failed: $($stop.output)" }
    $stopId = Get-UuidFromText $stop.output
    if ($null -ne $stopId) {
        $stopState = Wait-ForTerminal $stopId 300
        if ($stopState -ne 'succeeded') { Fail "stop ended $stopState" }
    }
    $start = Invoke-Cli @('disk', 'start', $profileId)
    if ($start.exit -ne 0) { Fail "start failed: $($start.output)" }
    $startId = Get-UuidFromText $start.output
    if ($null -ne $startId) {
        $startState = Wait-ForTerminal $startId 300
        if ($startState -ne 'succeeded') { Fail "start ended $startState" }
    }
    # A stop immediately after a start can be admitted while the runtime is still coming up;
    # wait for the disk to report running before asking it to stop.
    if (-not (Wait-ForRunning $profileId 30)) { Fail 'the disk did not report running after start' }
    $restartedVolume = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $restartedVolume = Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue
        if ($null -ne $restartedVolume) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($null -eq $restartedVolume) { Fail "no volume at $mount after the restart" }
    Write-Reading 'restart.allocation_unit_bytes' ([string]$restartedVolume.AllocationUnitSize)
    if ([uint32]$restartedVolume.AllocationUnitSize -ne [uint32]($ClusterSizeKiB * 1024)) {
        Fail ("after a restart the allocation unit is $($restartedVolume.AllocationUnitSize), " +
            "expected $($ClusterSizeKiB * 1024)")
    }
    Write-Reading 'restart.cluster_size_match' 'yes'

    Write-Step 'a create without the option keeps the Windows default (4 KiB)'
    # Single-disk: the restarted disk has to be stopped before a second create is admitted.
    $stopFirst = Invoke-Cli @('disk', 'stop', $profileId)
    if ($stopFirst.exit -ne 0) { Fail "stopping the first disk failed: $($stopFirst.output)" }
    $stopFirstId = Get-UuidFromText $stopFirst.output
    if ($null -ne $stopFirstId) {
        $stopFirstState = Wait-ForTerminal $stopFirstId 300
        if ($stopFirstState -ne 'succeeded') { Fail "the first stop ended $stopFirstState" }
    }
    # **Not the first disk's letter.**  `Select-FreeLetter` legitimately returns it once the
    # volume is gone, but the stopped profile still owns that mount target and the
    # registration then refuses with "mount target is already registered to another disk".
    $defaultMount = $null
    foreach ($candidate in @('P:', 'Q:', 'U:', 'V:', 'W:', 'X:', 'Y:', 'Z:')) {
        if ($candidate -eq $mount) { continue }
        if (Test-Path "$candidate\") { continue }
        if ($null -ne (Get-Volume -DriveLetter $candidate.TrimEnd(':') -ErrorAction SilentlyContinue)) { continue }
        $defaultMount = $candidate
        break
    }
    if ($null -eq $defaultMount) { Fail 'no free drive letter for the default disk' }
    $defaultCreate = Invoke-Cli @('disk', 'create', '--name', 'cluster-default',
        '--bundle', (Join-Path $workRoot 'bundle-default'), '--size', '64MiB',
        '--mount', $defaultMount, '--checkpoint-interval', '30m')
    if ($defaultCreate.exit -ne 0) { Fail "the default create failed: $($defaultCreate.output)" }
    $defaultId = Get-UuidFromText $defaultCreate.output
    if ($null -eq $defaultId) { Fail 'the default create carried no operation id' }
    $defaultState = Wait-ForTerminal $defaultId 300
    if ($defaultState -ne 'succeeded') { Fail "the default create ended $defaultState" }
    $defaultLetter = $defaultMount.TrimEnd(':')
    $defaultVolume = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $defaultVolume = Get-Volume -DriveLetter $defaultLetter -ErrorAction SilentlyContinue
        if ($null -ne $defaultVolume) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($null -eq $defaultVolume) { Fail "no volume appeared at $defaultMount" }
    Write-Reading 'default.allocation_unit_bytes' ([string]$defaultVolume.AllocationUnitSize)
    if ([uint32]$defaultVolume.AllocationUnitSize -ne 4096) {
        Fail ("the default create formatted with $($defaultVolume.AllocationUnitSize), not 4096")
    }
    Write-Reading 'default.cluster_size_match' 'yes'

    Write-Step 'cleanup: stop and forget both disks'
    $forgotten = 0
    foreach ($row in @((Invoke-Cli @('disk', 'list')).output)) {
        foreach ($match in [regex]::Matches($row,
                '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')) {
            $id = $match.Value
            $stopOne = Invoke-Cli @('disk', 'stop', $id)
            if ($stopOne.exit -eq 0) {
                $opId = Get-UuidFromText $stopOne.output
                if ($null -ne $opId) { [void](Wait-ForTerminal $opId 300) }
            }
            $forget = Invoke-Cli @('disk', 'forget', $id, '--confirmation', $id)
            if ($forget.exit -eq 0) { $forgotten++ }
        }
    }
    Write-Reading 'cleanup_forget_count' ([string]$forgotten)

    Write-Host 'PASS integration.m5.cluster_size: the create formats with the requested cluster size'
    Write-Reading 'result' 'PASS'
} finally {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
}


