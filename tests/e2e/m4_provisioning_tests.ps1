[CmdletBinding()]
param(
    [switch]$AllowDestructiveProvisioning,
    [string]$TestBinary,
    [string]$EvidenceDirectory,
    [string]$Confirmation,
    [UInt32]$PhysicalDriveNumber,
    [UInt64]$DiskLengthBytes,
    [UInt32]$ScsiPortNumber,
    [Byte]$ScsiPathId,
    [Byte]$ScsiTargetId,
    [Byte]$ScsiLun,
    [UInt32]$AimDeviceId,
    [string]$ProfileUuid,
    [string]$ImageUuid,
    [string]$OperationUuid,
    [UInt64]$ProfileRevision = 1,
    [UInt64]$SessionGeneration = 1
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if (-not $AllowDestructiveProvisioning) {
    Write-Output 'SKIPPED: destructive provisioning requires -AllowDestructiveProvisioning and an exact disposable target tuple.'
    return
}

$required = @(
    'TestBinary',
    'EvidenceDirectory',
    'Confirmation',
    'PhysicalDriveNumber',
    'DiskLengthBytes',
    'ScsiPortNumber',
    'ScsiPathId',
    'ScsiTargetId',
    'ScsiLun',
    'AimDeviceId',
    'ProfileUuid',
    'ImageUuid',
    'OperationUuid'
)
foreach ($name in $required) {
    if (-not $PSBoundParameters.ContainsKey($name)) {
        throw "Missing destructive-test parameter: $name"
    }
}
if ($Confirmation -cne 'FORMAT_ONLY_THE_EXACT_VERIFIED_DISPOSABLE_DISK') {
    throw 'The exact destructive confirmation phrase was not supplied.'
}
if ($DiskLengthBytes -eq 0 -or $ProfileRevision -eq 0 -or
    $SessionGeneration -eq 0) {
    throw 'Size, profile revision, and session generation must be non-zero.'
}

$expectedAimDeviceId = [UInt32]((([UInt32]$ScsiLun) -shl 16) -bor
    (([UInt32]$ScsiTargetId) -shl 8) -bor ([UInt32]$ScsiPathId))
if ($AimDeviceId -ne $expectedAimDeviceId) {
    throw 'AIM device ID does not encode the supplied path/target/LUN tuple.'
}
foreach ($uuid in @($ProfileUuid, $ImageUuid, $OperationUuid)) {
    $parsed = [Guid]::Empty
    if (-not [Guid]::TryParseExact($uuid, 'D', [ref]$parsed) -or
        $parsed -eq [Guid]::Empty -or
        $parsed.ToString('D') -cne $uuid.ToLowerInvariant()) {
        throw "UUID is not canonical lowercase D form: $uuid"
    }
}

$binaryPath = (Resolve-Path -LiteralPath $TestBinary).Path
$evidenceItem = New-Item -ItemType Directory -Path $EvidenceDirectory -Force
$evidencePath = $evidenceItem.FullName
$evidenceRoot = [IO.Path]::GetPathRoot($evidencePath)
if ($evidenceRoot -match '^(?<letter>[A-Za-z]):\\$') {
    $evidencePartition = Get-Partition -DriveLetter $Matches['letter'] `
        -ErrorAction Stop
    if ([UInt32]$evidencePartition.DiskNumber -eq $PhysicalDriveNumber) {
        throw 'Evidence directory is located on the destructive target disk.'
    }
}

$disks = @(Get-Disk -Number $PhysicalDriveNumber -ErrorAction Stop)
if ($disks.Count -ne 1) { throw 'The exact disk number is not unique.' }
$disk = $disks[0]
if ([UInt32]$disk.Number -ne $PhysicalDriveNumber -or
    [UInt64]$disk.Size -ne $DiskLengthBytes) {
    throw 'Get-Disk number or byte length does not match the approved tuple.'
}
if ([string]$disk.PartitionStyle -ne 'RAW' -or
    [UInt32]$disk.NumberOfPartitions -ne 0 -or
    @(Get-Partition -DiskNumber $PhysicalDriveNumber `
        -ErrorAction SilentlyContinue).Count -ne 0) {
    throw 'The destructive target is not a RAW empty disk.'
}
if ($disk.IsSystem -or $disk.IsBoot -or $disk.BootFromDisk -or
    $disk.IsClustered -or $disk.IsHighlyAvailable -or
    $disk.IsReadOnly -or $disk.IsOffline) {
    throw 'The destructive target has a protected, read-only, or offline role.'
}

$cimDisks = @(Get-CimInstance -ClassName Win32_DiskDrive `
    -Filter "Index = $PhysicalDriveNumber")
if ($cimDisks.Count -ne 1) {
    throw 'Win32_DiskDrive did not return one exact target.'
}
$cimDisk = $cimDisks[0]
if ([UInt32]$cimDisk.SCSIPort -ne $ScsiPortNumber -or
    [Byte]$cimDisk.SCSIBus -ne $ScsiPathId -or
    [Byte]$cimDisk.SCSITargetId -ne $ScsiTargetId -or
    [Byte]$cimDisk.SCSILogicalUnit -ne $ScsiLun) {
    throw 'The CIM SCSI tuple does not match the approved tuple.'
}

$preflight = [ordered]@{
    schema = 'myrd.m4.provisioning.preflight.v1'
    physical_drive_number = $PhysicalDriveNumber
    disk_length_bytes = $DiskLengthBytes
    scsi_port_number = $ScsiPortNumber
    scsi_path_id = $ScsiPathId
    scsi_target_id = $ScsiTargetId
    scsi_lun = $ScsiLun
    partition_style = [string]$disk.PartitionStyle
    partition_count = [UInt32]$disk.NumberOfPartitions
    protected = $false
}
$preflight | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath `
    (Join-Path $evidencePath 'preflight.json') -Encoding UTF8

$environmentNames = @(
    'MYRD_M4_ALLOW_DESTRUCTIVE_PROVISIONING',
    'MYRD_M4_PROVISIONING_CONFIRM',
    'MYRD_M4_PROVISIONING_PHYSICAL_DRIVE_NUMBER',
    'MYRD_M4_PROVISIONING_DISK_LENGTH_BYTES',
    'MYRD_M4_PROVISIONING_SCSI_PORT_NUMBER',
    'MYRD_M4_PROVISIONING_SCSI_PATH_ID',
    'MYRD_M4_PROVISIONING_SCSI_TARGET_ID',
    'MYRD_M4_PROVISIONING_SCSI_LUN',
    'MYRD_M4_PROVISIONING_AIM_DEVICE_ID',
    'MYRD_M4_PROVISIONING_PROFILE_UUID',
    'MYRD_M4_PROVISIONING_IMAGE_UUID',
    'MYRD_M4_PROVISIONING_OPERATION_UUID',
    'MYRD_M4_PROVISIONING_PROFILE_REVISION',
    'MYRD_M4_PROVISIONING_SESSION_GENERATION'
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable(
        $name, 'Process')
}
try {
    $env:MYRD_M4_ALLOW_DESTRUCTIVE_PROVISIONING = '1'
    $env:MYRD_M4_PROVISIONING_CONFIRM = $Confirmation
    $env:MYRD_M4_PROVISIONING_PHYSICAL_DRIVE_NUMBER = [string]$PhysicalDriveNumber
    $env:MYRD_M4_PROVISIONING_DISK_LENGTH_BYTES = [string]$DiskLengthBytes
    $env:MYRD_M4_PROVISIONING_SCSI_PORT_NUMBER = [string]$ScsiPortNumber
    $env:MYRD_M4_PROVISIONING_SCSI_PATH_ID = [string]$ScsiPathId
    $env:MYRD_M4_PROVISIONING_SCSI_TARGET_ID = [string]$ScsiTargetId
    $env:MYRD_M4_PROVISIONING_SCSI_LUN = [string]$ScsiLun
    $env:MYRD_M4_PROVISIONING_AIM_DEVICE_ID = [string]$AimDeviceId
    $env:MYRD_M4_PROVISIONING_PROFILE_UUID = $ProfileUuid
    $env:MYRD_M4_PROVISIONING_IMAGE_UUID = $ImageUuid
    $env:MYRD_M4_PROVISIONING_OPERATION_UUID = $OperationUuid
    $env:MYRD_M4_PROVISIONING_PROFILE_REVISION = [string]$ProfileRevision
    $env:MYRD_M4_PROVISIONING_SESSION_GENERATION = [string]$SessionGeneration

    $savedErrorPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5 surfaces a native process' redirected stderr as
        # ErrorRecord objects. Do not let the harness' Stop preference abort
        # before the exit code and complete diagnostics are captured.
        $ErrorActionPreference = 'Continue'
        $testOutput = @(& $binaryPath 2>&1 | ForEach-Object {
            $_.ToString()
        })
        $testExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorPreference
    }
    $testOutput | Set-Content -LiteralPath `
        (Join-Path $evidencePath 'test-output.log') -Encoding UTF8
    if ($testExitCode -ne 0) {
        throw "Provisioning test binary failed with exit code $testExitCode."
    }
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable(
            $name, $savedEnvironment[$name], 'Process')
    }
}

$postDisk = Get-Disk -Number $PhysicalDriveNumber -ErrorAction Stop
$partitions = @(Get-Partition -DiskNumber $PhysicalDriveNumber `
    -ErrorAction Stop)
if ([string]$postDisk.PartitionStyle -ne 'GPT' -or
    $partitions.Count -ne 1) {
    throw 'Postcondition is not one GPT partition on the exact disk.'
}
$partition = $partitions[0]
$volume = Get-Volume -Partition $partition -ErrorAction Stop
if ([string]$volume.FileSystem -ne 'NTFS') {
    throw 'Postcondition filesystem is not NTFS.'
}
$driveLetterAssigned = [Int32][Char]$partition.DriveLetter -ne 0
$noDefaultDriveLetter = [bool]$partition.NoDefaultDriveLetter
if ($driveLetterAssigned -or -not $noDefaultDriveLetter) {
    throw 'Postcondition assigned a drive letter or lost the no-default-drive-letter attribute.'
}
$postcondition = [ordered]@{
    schema = 'myrd.m4.provisioning.postcondition.v1'
    physical_drive_number = [UInt32]$postDisk.Number
    disk_length_bytes = [UInt64]$postDisk.Size
    disk_guid = ([Guid]$postDisk.Guid).ToString('D').ToLowerInvariant()
    partition_guid = ([Guid]$partition.Guid).ToString('D').ToLowerInvariant()
    volume_unique_id = [string]$volume.UniqueId
    filesystem = [string]$volume.FileSystem
    drive_letter_assigned = $driveLetterAssigned
    no_default_drive_letter = $noDefaultDriveLetter
}
$postcondition | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath `
    (Join-Path $evidencePath 'postcondition.json') -Encoding UTF8

Write-Output "PASS: exact disposable PhysicalDrive$PhysicalDriveNumber was provisioned as one GPT/NTFS volume."
