# integration.m5.compatibility -- the bundle lives where users actually keep files (R-18).
#
# **Guest-only.**  It creates real workers, AIM disks and NTFS volumes, and it changes the
# attributes of real directories on the guest.
#
# The three environments the release matrix promises to answer for, plus the two properties of
# the AIM disk itself:
#
#   1. a **compressed** NTFS directory (Explorer's "Compress contents to save disk space");
#   2. an **EFS-encrypted** directory (`cipher /e`), which needs a user certificate -- if the
#      guest cannot make one the case records that and continues, because "the platform cannot
#      do it" is a different answer from "the product hangs";
#   3. **Defender real-time protection** running during the whole cycle;
#   4. the AIM disk is a **non-system disk**, and its sector sizes are recorded.
#
# The acceptance is not only "it works": where it cannot work, the failure has to be a
# definite, classified error rather than a hang.  Every sub-case has a bounded wait.

param(
    [string]$Configuration = 'Release',
    [int]$SizeMiB = 64
)

$ErrorActionPreference = 'Stop'

trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.compatibility creates real workers, AIM disks and volumes; it may ' +
        'only run on the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
# The dot-sourced helpers resolve `$cliExe` and `$serviceExe` from the caller's scope, so both
# have to exist here before the first helper call.
$cliExe = Join-Path $bin 'myrd-cli.exe'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

$workRoot = 'C:\m5\compat-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
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
        # A failed operation makes `operation show` exit 1; the state is still in the output.
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

function Stop-EveryDisk {
    foreach ($row in @((Invoke-Cli @('disk', 'list')).output)) {
        foreach ($match in [regex]::Matches($row,
                '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')) {
            $id = $match.Value
            $stop = Invoke-Cli @('disk', 'stop', $id)
            if ($stop.exit -eq 0) {
                $opId = Get-UuidFromText $stop.output
                if ($null -ne $opId) { [void](Wait-ForTerminal $opId 300) }
            }
            [void](Invoke-Cli @('disk', 'forget', $id, '--confirmation', $id))
        }
    }
}

function Select-Mount {
    foreach ($candidate in @('U:', 'V:', 'W:', 'X:', 'Y:', 'Z:', 'P:', 'Q:')) {
        if (Test-Path "$candidate\") { continue }
        if ($null -ne (Get-Volume -DriveLetter $candidate.TrimEnd(':') -ErrorAction SilentlyContinue)) { continue }
        return $candidate
    }
    return $null
}

# One full cycle against a bundle in `$BundleRoot`: create, verify the volume, write, checkpoint,
# stop.  Returns the terminal state of the create and the readings it produced.
function Invoke-BundleCycle([string]$Label, [string]$BundleRoot) {
    $mount = Select-Mount
    if ($null -eq $mount) { Fail "no free drive letter for $Label" }
    $create = Invoke-Cli @('disk', 'create', '--name', $Label,
        '--bundle', $BundleRoot, '--size', "$($SizeMiB)MiB", '--mount', $mount,
        '--checkpoint-interval', '30m')
    if ($create.exit -ne 0) {
        Write-Reading "$Label.create" 'refused'
        Write-Reading "$Label.create_output" (($create.output -replace "`n", ' | '))
        return @{ state = 'refused'; output = $create.output }
    }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail "the $Label create carried no operation id" }
    $state = Wait-ForTerminal $createId 300
    Write-Reading "$Label.create" $state
    if ($state -ne 'succeeded') {
        return @{ state = $state; output = $create.output }
    }
    $letter = $mount.TrimEnd(':')
    $volume = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $volume = Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue
        if ($null -ne $volume) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($null -eq $volume) { Fail "no volume appeared at $mount for $Label" }
    Write-Reading "$Label.volume" 'present'
    $probe = Join-Path "$($mount)\" 'compat-probe.bin'
    [System.IO.File]::WriteAllBytes($probe, [byte[]]::new(1MB))
    Write-Reading "$Label.write_bytes" ([string](Get-Item $probe).Length)
    $profileId = Get-UuidFromText (Invoke-Cli @('disk', 'list')).output
    if ($null -eq $profileId) { Fail "$Label is not listed" }
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $profileId)
    if ($checkpoint.exit -eq 0) {
        $checkpointId = Get-UuidFromText $checkpoint.output
        if ($null -ne $checkpointId) {
            Write-Reading "$Label.checkpoint" (Wait-ForTerminal $checkpointId 300)
        }
    } else {
        Write-Reading "$Label.checkpoint" 'refused'
    }
    $stop = Invoke-Cli @('disk', 'stop', $profileId)
    if ($stop.exit -eq 0) {
        $stopId = Get-UuidFromText $stop.output
        if ($null -ne $stopId) {
            Write-Reading "$Label.stop" (Wait-ForTerminal $stopId 300)
        }
    } else {
        Write-Reading "$Label.stop" 'refused'
    }
    [void](Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId))
    return @{ state = 'succeeded'; output = '' }
}

try {
    Write-Step 'record the Defender state the whole run happens under'
    $defender = Get-MpComputerStatus -ErrorAction SilentlyContinue
    if ($null -ne $defender) {
        Write-Reading 'defender.realtime' ([string]$defender.RealTimeProtectionEnabled)
        Write-Reading 'defender.antivirus' ([string]$defender.AntivirusEnabled)
    } else {
        Write-Reading 'defender.realtime' 'unavailable'
    }

    Write-Step 'start the service'
    Remove-ScService
    $running = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) { Fail "the guest already has $($running.Count) worker(s); reset it first" }
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog `
        -RedirectStandardOutput (Join-Path $workRoot 'service.out.log') `
        -WindowStyle Hidden -PassThru
    if ($null -eq (Wait-ForService 90)) { Fail 'the service never answered' }

    Write-Step 'a compressed NTFS directory holds the bundle'
    $compressedRoot = Join-Path $workRoot 'compressed'
    New-Item -ItemType Directory -Force -Path $compressedRoot | Out-Null
    & compact.exe /c /s:$compressedRoot /q 2>&1 | Out-Null
    $compressedAttribute = (Get-Item $compressedRoot).Attributes
    Write-Reading 'compressed.attribute' ([string]$compressedAttribute)
    if ($compressedAttribute -notmatch 'Compressed') {
        Fail "the compressed directory did not become compressed: $compressedAttribute"
    }
    $compressedResult = Invoke-BundleCycle 'compressed' $compressedRoot
    if ($compressedResult.state -ne 'succeeded') {
        Fail "a compressed bundle did not finish a cycle: $($compressedResult.state): $($compressedResult.output)"
    }
    Write-Reading 'compressed.result' 'full-cycle-ok'

    Write-Step 'an EFS-encrypted directory holds the bundle (or the platform refuses cleanly)'
    $encryptedRoot = Join-Path $workRoot 'encrypted'
    New-Item -ItemType Directory -Force -Path $encryptedRoot | Out-Null
    $cipher = & cipher.exe /e $encryptedRoot 2>&1 | Out-String
    Write-Reading 'efs.cipher_exit' ([string]$LASTEXITCODE)
    Write-Reading 'efs.cipher_output' (($cipher -replace "`n", ' | '))
    $encryptedAttribute = (Get-Item $encryptedRoot).Attributes
    Write-Reading 'efs.attribute' ([string]$encryptedAttribute)
    if ($encryptedAttribute -match 'Encrypted') {
        $encryptedResult = Invoke-BundleCycle 'encrypted' $encryptedRoot
        Write-Reading 'efs.result' $encryptedResult.state
        if ($encryptedResult.state -notin @('succeeded', 'refused', 'failed')) {
            Fail "an encrypted bundle ended in an unbounded state: $($encryptedResult.state)"
        }
        if ($encryptedResult.state -ne 'succeeded') {
            Write-Reading 'efs.classified_failure' 'yes'
        }
    } else {
        # No user certificate on this guest: the platform, not the product, said no.
        Write-Reading 'efs.result' 'platform-unavailable'
    }

    Write-Step 'record the AIM disk identity: non-system, sector sizes'
    $mount = Select-Mount
    $create = Invoke-Cli @('disk', 'create', '--name', 'sector-check',
        '--bundle', (Join-Path $workRoot 'sector-bundle'), '--size', "$($SizeMiB)MiB",
        '--mount', $mount, '--checkpoint-interval', '30m')
    if ($create.exit -ne 0) { Fail "the sector-check create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the sector-check create carried no operation id' }
    if ((Wait-ForTerminal $createId 300) -ne 'succeeded') { Fail 'the sector-check create did not finish' }
    $letter = $mount.TrimEnd(':')
    $partition = Get-Partition -DriveLetter $letter -ErrorAction SilentlyContinue
    if ($null -eq $partition) { Fail "no partition at $mount" }
    $aimDisk = Get-Disk -Number $partition.DiskNumber
    Write-Reading 'aim.disk_number' ([string]$aimDisk.Number)
    Write-Reading 'aim.is_system' ([string]$aimDisk.IsSystem)
    Write-Reading 'aim.is_boot' ([string]$aimDisk.IsBoot)
    Write-Reading 'aim.logical_sector_bytes' ([string]$aimDisk.LogicalSectorSize)
    Write-Reading 'aim.physical_sector_bytes' ([string]$aimDisk.PhysicalSectorSize)
    Write-Reading 'aim.bus_type' ([string]$aimDisk.BusType)
    if ([bool]$aimDisk.IsSystem) { Fail 'the AIM disk reports itself as the system disk' }
    $systemDisk = Get-Disk | Where-Object { $_.IsSystem } | Select-Object -First 1
    Write-Reading 'system.disk_number' ([string]$systemDisk.Number)
    if ($systemDisk.Number -eq $aimDisk.Number) { Fail 'the AIM disk is the system disk' }

    Write-Step 'cleanup'
    Stop-EveryDisk
    Write-Reading 'result' 'PASS'
    Write-Host 'PASS integration.m5.compatibility: compressed bundle works; EFS/Defender/non-system recorded'
} finally {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
