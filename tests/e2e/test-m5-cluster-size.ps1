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
        if ($show.exit -ne 0) { Fail "operation show failed: $($show.output)" }
        # **Match the operation line, not any `: word` in the output.**  The show output also
        # prints a disposition, and "disposition: completed" was read as a successful state
        # while the operation had failed -- which is how this case reported success for a
        # create whose provisioning had thrown.
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

    Write-Step 'cleanup: stop and forget'
    $line = Invoke-Cli @('disk', 'list')
    $profileId = Get-UuidFromText $line.output
    if ($null -ne $profileId) {
        $stop = Invoke-Cli @('disk', 'stop', $profileId)
        if ($stop.exit -eq 0) {
            $id = Get-UuidFromText $stop.output
            if ($null -ne $id) { [void](Wait-ForTerminal $id 300) }
        }
        $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
        Write-Reading 'cleanup_forget' ([string]$forget.exit)
    }

    Write-Host 'PASS integration.m5.cluster_size: the create formats with the requested cluster size'
    Write-Reading 'result' 'PASS'
} finally {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
}


