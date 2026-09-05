[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$Bundle,

    [ValidatePattern('^[A-Za-z]:$')]
    [string]$DriveLetter = 'R:',

    [ValidatePattern('^[A-Za-z]:$')]
    [string]$SpareDriveLetter = 'S:',

    [string]$EvidenceDirectory = (Join-Path (Get-Location) 'm4-vm-evidence'),

    [string]$CommandLine,

    [string]$CommandStdout,

    [string]$CommandStderr,

    [int]$CommandExitCode
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function New-ObservationDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    if ([IO.Directory]::Exists($full) -or [IO.File]::Exists($full)) {
        throw "Evidence directory already exists: $full"
    }
    $parent = [IO.Directory]::GetParent($full)
    if ($null -eq $parent -or -not [IO.Directory]::Exists($parent.FullName)) {
        throw "Evidence parent must already exist: $($parent.FullName)"
    }
    [IO.Directory]::CreateDirectory($full) | Out-Null
    return $full
}

function Write-JsonObservation {
    param([Parameter(Mandatory = $true)]$Value, [Parameter(Mandatory = $true)][string]$Path)
    $Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Get-DriveObservation {
    param([Parameter(Mandatory = $true)][string]$Letter)
    $name = $Letter.TrimEnd(':')
    $volume = @(Get-Volume -DriveLetter $name -ErrorAction SilentlyContinue)
    $partition = @(Get-Partition -DriveLetter $name -ErrorAction SilentlyContinue)
    [ordered]@{
        drive_letter = $Letter
        present = ($volume.Count -eq 1)
        filesystem = if ($volume.Count -eq 1) { [string]$volume[0].FileSystem } else { $null }
        volume_unique_id = if ($volume.Count -eq 1) { [string]$volume[0].UniqueId } else { $null }
        partition_number = if ($partition.Count -eq 1) { [int]$partition[0].PartitionNumber } else { $null }
        disk_number = if ($partition.Count -eq 1) { [int]$partition[0].DiskNumber } else { $null }
    }
}

function Get-AimObservation {
    $disks = @(Get-Disk -ErrorAction SilentlyContinue | Where-Object {
        ([string]$_.FriendlyName -match '(?i)Arsenal|AIM') -or
        ([string]$_.Manufacturer -match '(?i)Arsenal|AIM')
    })
    @($disks | ForEach-Object {
        [ordered]@{
            number = [int]$_.Number
            friendly_name = [string]$_.FriendlyName
            manufacturer = [string]$_.Manufacturer
            serial = [string]$_.SerialNumber
            size = [UInt64]$_.Size
            operational_status = @($_.OperationalStatus | ForEach-Object { [string]$_ })
        }
    })
}

function Get-DriverObservation {
    $path = Join-Path $env:WINDIR 'System32\drivers\phdskmnt.sys'
    $file = if (Test-Path -LiteralPath $path) { Get-Item -LiteralPath $path } else { $null }
    $service = @(Get-CimInstance Win32_SystemDriver -Filter "Name='phdskmnt'" -ErrorAction SilentlyContinue)
    [ordered]@{
        path = $path
        present = ($null -ne $file)
        file_version = if ($null -ne $file) { [string]$file.VersionInfo.FileVersion } else { $null }
        service_state = if ($service.Count -eq 1) { [string]$service[0].State } else { $null }
        service_start_mode = if ($service.Count -eq 1) { [string]$service[0].StartMode } else { $null }
    }
}

function Get-AimVersionObservation {
    $candidates = @()
    if ($env:ProgramFiles) { $candidates += (Join-Path $env:ProgramFiles 'Arsenal Image Mounter\aim_ll.exe') }
    if (${env:ProgramFiles(x86)}) { $candidates += (Join-Path ${env:ProgramFiles(x86)} 'Arsenal Image Mounter\aim_ll.exe') }
    $candidates = @($candidates | Where-Object { Test-Path -LiteralPath $_ })
    @($candidates | ForEach-Object {
        $item = Get-Item -LiteralPath $_
        [ordered]@{ path = $_; file_version = [string]$item.VersionInfo.FileVersion }
    })
}

$evidence = $null
$classification = 'HARNESS_FAIL'
$resultMessage = $null
try {
    $evidence = New-ObservationDirectory -Path $EvidenceDirectory
    $exe = [IO.Path]::GetFullPath($Executable)
    $bundlePath = [IO.Path]::GetFullPath($Bundle)
    if (-not [IO.File]::Exists($exe)) { throw "Executable not found: $exe" }
    $bundleParent = [IO.Directory]::GetParent($bundlePath)
    if ($null -eq $bundleParent -or -not [IO.Directory]::Exists($bundleParent.FullName)) { throw 'Bundle parent does not exist.' }
    $parentRoot = [IO.Path]::GetPathRoot($bundleParent.FullName)
    $parentVolume = Get-Volume -DriveLetter $parentRoot.Substring(0, 1) -ErrorAction Stop
    if ([string]$parentVolume.FileSystem -ne 'NTFS') { throw 'Bundle parent is not on NTFS.' }
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $identity = Get-CimInstance Win32_ComputerSystemProduct | Select-Object -First 1
    $os = Get-CimInstance Win32_OperatingSystem | Select-Object -First 1
    $macs = @(Get-NetAdapter -Physical -ErrorAction SilentlyContinue | Where-Object Status -eq 'Up' | ForEach-Object MacAddress)
    $processes = @(Get-Process -Name myrd -ErrorAction SilentlyContinue | ForEach-Object { [ordered]@{ pid = $_.Id; path = [string]$_.Path } })
    $drives = [ordered]@{ target = Get-DriveObservation $DriveLetter; spare = Get-DriveObservation $SpareDriveLetter }
    $aim = Get-AimObservation
    $driver = Get-DriverObservation
    $versions = [ordered]@{ windows_build = [string]$os.BuildNumber; windows_version = [string]$os.Version; phdskmnt = $driver; aim = Get-AimVersionObservation }
    $preconditions = [ordered]@{
        administrator = $admin
        bundle_parent_ntfs = ([string]$parentVolume.FileSystem -eq 'NTFS')
        target_drive_free = -not [bool]$drives.target.present
        spare_drive_free = -not [bool]$drives.spare.present
        no_myrd_process = ($processes.Count -eq 0)
        no_aim_devices = ($aim.Count -eq 0)
        driver_present = [bool]$driver.present
    }
    Write-JsonObservation ([ordered]@{
        schema = 'myrd.m4.vm.lifecycle.observation.v1'
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        vm = [ordered]@{ uuid = [string]$identity.UUID; vendor = [string]$identity.Vendor; product = [string]$identity.Name; mac_addresses = $macs }
        os = [ordered]@{ caption = [string]$os.Caption; version = [string]$os.Version; build = [string]$os.BuildNumber }
        admin_token = $admin
        bundle = [ordered]@{ path = $bundlePath; parent = $bundleParent.FullName; parent_filesystem = [string]$parentVolume.FileSystem }
        versions = $versions
        drives = $drives
        processes = $processes
        aim_devices = $aim
        preconditions = $preconditions
    }) (Join-Path $evidence 'preflight.json')
    if ($preconditions.Values -contains $false) {
        $classification = 'ENVIRONMENT_BLOCKED'
        $resultMessage = 'One or more VM preconditions are not satisfied; no product command was invoked.'
    } elseif ($CommandLine) {
        if (-not $PSBoundParameters.ContainsKey('CommandStdout') -or -not $PSBoundParameters.ContainsKey('CommandStderr') -or -not $PSBoundParameters.ContainsKey('CommandExitCode')) {
            throw 'CommandLine requires CommandStdout, CommandStderr, and CommandExitCode captured by the operator.'
        }
        if (-not (Test-Path -LiteralPath $CommandStdout) -or -not (Test-Path -LiteralPath $CommandStderr)) { throw 'Captured command stdout/stderr file is missing.' }
        Copy-Item -LiteralPath $CommandStdout -Destination (Join-Path $evidence 'command.stdout.log')
        Copy-Item -LiteralPath $CommandStderr -Destination (Join-Path $evidence 'command.stderr.log')
        $command = [ordered]@{ command_line = $CommandLine; exit_code = $CommandExitCode; stdout = (Join-Path $evidence 'command.stdout.log'); stderr = (Join-Path $evidence 'command.stderr.log'); invoked_by_observer = $false }
        Write-JsonObservation $command (Join-Path $evidence 'command.json')
        $classification = if ($CommandExitCode -eq 0) { 'PASS' } else { 'PRODUCT_FAIL' }
        $resultMessage = "Observed product command exit code $($command.exit_code)."
        $bundleFiles = @(Get-ChildItem -LiteralPath $bundlePath -File -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
            [ordered]@{ relative = $_.FullName.Substring($bundlePath.Length).TrimStart('\\'); length = [UInt64]$_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
        })
        $outText = Get-Content -LiteralPath (Join-Path $evidence 'command.stdout.log') -Raw -ErrorAction SilentlyContinue
        $generations = @()
        if ($outText -match '(?i)generation\s+(\d+)') { $generations += [UInt64]$Matches[1] }
        Write-JsonObservation ([ordered]@{ generations_observed = $generations; files = $bundleFiles; target_drive = Get-DriveObservation $DriveLetter; aim_devices = Get-AimObservation; processes = @(Get-Process -Name myrd -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id) }) (Join-Path $evidence 'postcommand.json')
    } else {
        $classification = 'PASS'
        $resultMessage = 'Preflight recorded; no product command was invoked.'
    }
} catch {
    $classification = 'HARNESS_FAIL'
    $resultMessage = $_.Exception.Message
    if ($null -ne $evidence) { Set-Content -LiteralPath (Join-Path $evidence 'error.txt') -Value $resultMessage -Encoding UTF8 }
}

$summary = [ordered]@{ schema = 'myrd.m4.vm.lifecycle.result.v1'; classification = $classification; message = $resultMessage; evidence_directory = $evidence }
if ($null -ne $evidence) { Write-JsonObservation $summary (Join-Path $evidence 'result.json') }
Write-Output ("{0}: {1}" -f $classification, $resultMessage)
if ($classification -eq 'PRODUCT_FAIL') { exit 1 }
if ($classification -eq 'ENVIRONMENT_BLOCKED') { exit 3 }
if ($classification -eq 'USER_ABORTED') { exit 4 }
if ($classification -eq 'HARNESS_FAIL') { exit 2 }
