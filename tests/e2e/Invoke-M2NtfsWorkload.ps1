[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Preflight', 'SetupWrite', 'DeleteRetrim', 'RemoveMounts')]
    [string]$Phase,
    [Parameter(Mandatory = $true)][ValidateRange(1, 2147483647)]
    [int]$DiskNumber,
    [Parameter(Mandatory = $true)][string]$DeviceIdRaw,
    [Parameter(Mandatory = $true)][string]$ProviderSessionId,
    [Parameter(Mandatory = $true)][string]$ExpectedBusType,
    [Parameter(Mandatory = $true)][string]$ExpectedVendorId,
    [Parameter(Mandatory = $true)][string]$ExpectedProductId,
    [Parameter(Mandatory = $true)][string]$MountDirectory,
    [Parameter(Mandatory = $true)][string]$StatePath,
    [string]$BeforeDiskNumbersCsv = '',
    [string]$TestInventoryPath = '',
    [switch]$SafetyAcknowledged,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'M2Harness.Common.ps1')

$ExactDiskSize = [uint64]68719476736
$FileSize = [uint64]1073741824
$FileCount = 4
$BufferSize = 8MB

function Assert-AbsoluteSafePath {
    param([string]$Path, [string]$Label)
    Assert-M2Condition (-not [string]::IsNullOrWhiteSpace($Path)) "$Label is empty"
    Assert-M2Condition ([IO.Path]::IsPathRooted($Path)) "$Label must be absolute"
    $full = [IO.Path]::GetFullPath($Path)
    Assert-M2Condition (-not $full.StartsWith('\\.\', [StringComparison]::OrdinalIgnoreCase)) "$Label must not be a device path"
    Assert-M2Condition (-not $full.StartsWith('\\?\GLOBALROOT', [StringComparison]::OrdinalIgnoreCase)) "$Label must not use GLOBALROOT"
    return $full
}

function Get-PathReparseState {
    param([string]$Path)
    if (-not [IO.Directory]::Exists($Path)) { return $false }
    $item = Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            return $true
        }
        $item = $item.Parent
    }
    return $false
}

function Convert-RealDiskCandidate {
    param($Disk)
    $drive = Get-CimInstance Win32_DiskDrive -Filter "Index=$DiskNumber" -ErrorAction Stop
    Assert-M2Condition ($null -ne $drive) 'Win32 disk identity is unavailable'
    $physical = @(Get-PhysicalDisk -ErrorAction Stop | Where-Object { [string]$_.DeviceId -eq [string]$DiskNumber })
    Assert-M2Condition ($physical.Count -eq 1) 'exact physical-disk identity is unavailable or ambiguous'
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)
    $clustered = $false
    if ($null -ne $Disk.PSObject.Properties['IsClustered']) {
        $clustered = [bool]$Disk.IsClustered
    }
    [pscustomobject]@{
        Number = [int]$Disk.Number
        Size = [uint64]$Disk.Size
        BusType = [string]$Disk.BusType
        VendorId = ([string]$physical[0].Manufacturer).Trim()
        ProductId = ([string]$physical[0].Model).Trim()
        SerialNumber = if (-not [string]::IsNullOrWhiteSpace([string]$physical[0].SerialNumber)) { ([string]$physical[0].SerialNumber).Trim() } else { ([string]$Disk.UniqueId).Trim() }
        DeviceIdRaw = $DeviceIdRaw
        ProviderSessionId = $ProviderSessionId
        IsBoot = [bool]$Disk.IsBoot
        IsSystem = [bool]$Disk.IsSystem
        IsPageFile = $false
        IsClustered = $clustered
        IsOffline = [bool]$Disk.IsOffline
        IsReadOnly = [bool]$Disk.IsReadOnly
        PartitionCount = $partitions.Count
        UniqueId = [string]$Disk.UniqueId
    }
}

function Get-PreflightInventory {
    if (-not [string]::IsNullOrWhiteSpace($TestInventoryPath)) {
        Assert-M2Condition $WhatIf 'TestInventoryPath is allowed only with WhatIf'
        Assert-M2Condition ([IO.File]::Exists($TestInventoryPath)) 'test inventory file is missing'
        return Get-Content -Raw -LiteralPath $TestInventoryPath -Encoding utf8 | ConvertFrom-Json
    }

    Assert-M2Condition (-not [string]::IsNullOrWhiteSpace($BeforeDiskNumbersCsv)) 'BeforeDiskNumbersCsv is required for real preflight'
    $before = @($BeforeDiskNumbersCsv.Split(',', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object { [int]$_.Trim() })
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $candidate = Convert-RealDiskCandidate $disk
    $mountExists = [IO.Directory]::Exists($script:MountFull)
    $mountEntries = if ($mountExists) { [IO.Directory]::GetFileSystemEntries($script:MountFull).Count } else { 0 }
    [pscustomobject]@{
        BeforeDiskNumbers = $before
        AfterDisks = @($candidate)
        MountExists = $mountExists
        MountEntryCount = $mountEntries
        MountConflict = $false
        MountHasReparsePoint = Get-PathReparseState $script:MountFull
    }
}

function Assert-ExactCurrentDisk {
    param(
        [bool]$RequireNoPartitions,
        [string]$ExpectedSerialNumber = '',
        [string]$ExpectedUniqueId = '',
        [switch]$AllowReadOnly
    )
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $candidate = Convert-RealDiskCandidate $disk
    $expected = [pscustomobject]@{
        DiskSizeBytes = $ExactDiskSize
        BusType = $ExpectedBusType
        VendorId = $ExpectedVendorId
        ProductId = $ExpectedProductId
        DeviceIdRaw = $DeviceIdRaw
        ProviderSessionId = $ProviderSessionId
    }
    $null = Test-M2ExactCurrentCandidate -Candidate $candidate -ExpectedIdentity $expected -ExpectedSerialNumber $ExpectedSerialNumber -ExpectedUniqueId $ExpectedUniqueId -RequireNoPartitions:$RequireNoPartitions -AllowReadOnly:$AllowReadOnly
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)
    return [pscustomobject]@{ Disk = $disk; Candidate = $candidate; Partitions = $partitions }
}

function Assert-ExactCurrentPartition {
    param(
        [int]$PartitionNumber,
        [string]$ExpectedSerialNumber,
        [string]$ExpectedUniqueId
    )
    Assert-M2Condition ($PartitionNumber -gt 0) 'exact partition number is unavailable'
    $current = Assert-ExactCurrentDisk $false $ExpectedSerialNumber $ExpectedUniqueId
    return Test-M2OwnedDataPartitionSet -Partitions @($current.Partitions) -DiskNumber $DiskNumber -OwnedPartitionNumber $PartitionNumber
}

function Save-WorkloadState {
    param($State)
    Assert-M2Condition (-not $WhatIf) 'WhatIf must not publish workload state'
    $parent = [IO.Path]::GetDirectoryName($script:StateFull)
    Assert-M2Condition ([IO.Directory]::Exists($parent)) 'state directory must already exist'
    $temporary = $script:StateFull + '.tmp-' + [Guid]::NewGuid().ToString('N')
    try {
        $State | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding utf8
        $null = Get-Content -Raw -LiteralPath $temporary -Encoding utf8 | ConvertFrom-Json
        Move-Item -LiteralPath $temporary -Destination $script:StateFull -Force
    }
    finally {
        if ([IO.File]::Exists($temporary)) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Load-WorkloadState {
    Assert-M2Condition ([IO.File]::Exists($script:StateFull)) 'workload state file is missing'
    $state = Get-Content -Raw -LiteralPath $script:StateFull -Encoding utf8 | ConvertFrom-Json
    Assert-M2Condition ([int]$state.schema_version -eq 1) 'workload state schema is invalid'
    Assert-M2Condition ([int]$state.disk_number -eq $DiskNumber) 'workload state disk number differs'
    Assert-M2Condition ([string]$state.device_id_raw -eq $DeviceIdRaw) 'workload state device identity differs'
    Assert-M2Condition ([string]$state.provider_session_id -eq $ProviderSessionId) 'workload state provider session differs'
    Assert-M2Condition ([IO.Path]::GetFullPath([string]$state.mount_directory) -eq $script:MountFull) 'workload state mount directory differs'
    return $state
}

function Get-HexDigest {
    param([byte[]]$Bytes)
    return ([BitConverter]::ToString($Bytes)).Replace('-', '')
}

function Write-DeterministicFile {
    param([string]$Path, [uint64]$Length, [int]$Seed)
    $buffer = New-Object byte[] $BufferSize
    $random = [Random]::new($Seed)
    $random.NextBytes($buffer)
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $expectedHash = ''
    try {
        $remaining = $Length
        while ($remaining -gt 0) {
            $count = [int][Math]::Min([uint64]$buffer.Length, $remaining)
            $stream.Write($buffer, 0, $count)
            $null = $sha.TransformBlock($buffer, 0, $count, $null, 0)
            $remaining -= [uint64]$count
        }
        $stream.Flush($true)
        $null = $sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
        $expectedHash = Get-HexDigest $sha.Hash
    }
    finally {
        $stream.Dispose()
        $sha.Dispose()
    }
    $readHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    Assert-M2Condition ($readHash -eq $expectedHash) "deterministic file readback hash mismatch: $Path"
    return [pscustomobject]@{
        path = $Path
        bytes = $Length
        write_sha256 = $expectedHash
        read_sha256 = $readHash
    }
}

function Set-DeterministicPrefix {
    param([string]$Path, [uint64]$Length, [int]$Seed)
    $buffer = New-Object byte[] $BufferSize
    $random = [Random]::new($Seed)
    $random.NextBytes($buffer)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $remaining = $Length
        while ($remaining -gt 0) {
            $count = [int][Math]::Min([uint64]$buffer.Length, $remaining)
            $stream.Write($buffer, 0, $count)
            $remaining -= [uint64]$count
        }
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
}

function Get-ManifestDigest {
    param([object[]]$Files)
    $manifest = (@($Files | Sort-Object path | ForEach-Object { "$($_.path)|$($_.bytes)|$($_.read_sha256)" }) -join [Environment]::NewLine)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return Get-HexDigest ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($manifest)))
    }
    finally {
        $sha.Dispose()
    }
}

function Invoke-SetupWrite {
    Assert-M2Condition $SafetyAcknowledged 'destructive safety acknowledgement is required'
    $inventory = Get-PreflightInventory
    $expected = [pscustomobject]@{
        DiskSizeBytes = $ExactDiskSize
        BusType = $ExpectedBusType
        VendorId = $ExpectedVendorId
        ProductId = $ExpectedProductId
        DeviceIdRaw = $DeviceIdRaw
        ProviderSessionId = $ProviderSessionId
    }
    $candidate = Select-M2SafeCandidate -BeforeDiskNumbers @($inventory.BeforeDiskNumbers) -AfterDisks @($inventory.AfterDisks) -ExpectedIdentity $expected
    Assert-M2Condition (-not [string]::IsNullOrWhiteSpace([string]$candidate.SerialNumber)) 'candidate serial identity is empty'
    Assert-M2Condition (-not [string]::IsNullOrWhiteSpace([string]$candidate.UniqueId)) 'candidate unique identity is empty'
    Test-M2MountSafety -MountExists ([bool]$inventory.MountExists) -MountEntryCount ([int]$inventory.MountEntryCount) -MountConflict ([bool]$inventory.MountConflict) -HasReparsePoint ([bool]$inventory.MountHasReparsePoint)
    Assert-M2Condition ([int]$candidate.Number -eq $DiskNumber) 'selected candidate differs from exact DiskNumber'
    if ($WhatIf) {
        Write-Output "M2 NTFS WHATIF PASS: exact Disk $DiskNumber is safe; no mutation executed"
        return
    }

    $initial = Assert-ExactCurrentDisk $true ([string]$candidate.SerialNumber) ([string]$candidate.UniqueId) -AllowReadOnly
    if ([bool]$initial.Candidate.IsReadOnly) {
        Set-Disk -Number $DiskNumber -IsReadOnly $false -ErrorAction Stop
    }
    $null = Assert-ExactCurrentDisk $true ([string]$candidate.SerialNumber) ([string]$candidate.UniqueId)
    if (-not [IO.Directory]::Exists($script:MountFull)) {
        [IO.Directory]::CreateDirectory($script:MountFull) | Out-Null
    }
    Test-M2MountSafety -MountExists $true -MountEntryCount ([IO.Directory]::GetFileSystemEntries($script:MountFull).Count) -MountConflict $false -HasReparsePoint (Get-PathReparseState $script:MountFull)

    $state = [ordered]@{
        schema_version = 1
        phase = 'pre_mutation'
        disk_number = $DiskNumber
        device_id_raw = $DeviceIdRaw
        provider_session_id = $ProviderSessionId
        mount_directory = $script:MountFull
        partition_number = 0
        volume_unique_id = ''
        bus_type = [string]$candidate.BusType
        vendor_id = [string]$candidate.VendorId
        product_id = [string]$candidate.ProductId
        serial_number = [string]$candidate.SerialNumber
        disk_unique_id = [string]$candidate.UniqueId
        bytes_written = [uint64]0
        bytes_read = [uint64]0
        bytes_deleted = [uint64]0
        write_sha256 = ''
        read_sha256 = ''
        delete_notify_enabled = $false
        retrim_succeeded = $false
        files = @()
    }
    Save-WorkloadState $state

    $disk = Initialize-Disk -Number $DiskNumber -PartitionStyle GPT -PassThru -ErrorAction Stop
    Assert-M2Condition ([int]$disk.Number -eq $DiskNumber) 'Initialize-Disk returned a different disk'
    $state.phase = 'initialized'
    Save-WorkloadState $state

    $initialized = Assert-ExactCurrentDisk $false ([string]$state.serial_number) ([string]$state.disk_unique_id)
    Test-M2InitializedGptPartitionSet -Partitions @($initialized.Partitions)
    Assert-M2Condition ([string]$initialized.Disk.PartitionStyle -eq 'GPT') 'initialized disk is not GPT'
    $partition = New-Partition -DiskNumber $DiskNumber -UseMaximumSize -AssignDriveLetter:$false -ErrorAction Stop
    Assert-M2Condition ([int]$partition.DiskNumber -eq $DiskNumber) 'New-Partition returned a different disk'
    $state.partition_number = [int]$partition.PartitionNumber
    $state.phase = 'partition_created'
    Save-WorkloadState $state

    $partition = Assert-ExactCurrentPartition ([int]$state.partition_number) ([string]$state.serial_number) ([string]$state.disk_unique_id)
    $volume = Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel 'MYRD-M2-TEST' -Confirm:$false -Force -ErrorAction Stop
    Assert-M2Condition ([string]$volume.FileSystem -eq 'NTFS') 'Format-Volume did not produce NTFS'
    $state.volume_unique_id = [string]$volume.UniqueId
    $state.phase = 'formatted'
    Save-WorkloadState $state

    $null = Assert-ExactCurrentPartition ([int]$state.partition_number) ([string]$state.serial_number) ([string]$state.disk_unique_id)
    Add-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $state.partition_number -AccessPath $script:MountAccessPath -ErrorAction Stop
    $state.phase = 'mounted'
    Save-WorkloadState $state

    $dataDirectory = Join-Path $script:MountFull 'data'
    [IO.Directory]::CreateDirectory($dataDirectory) | Out-Null
    $files = @()
    for ($index = 0; $index -lt $FileCount; $index++) {
        $path = Join-Path $dataDirectory ('file-{0:D3}.bin' -f $index)
        $files += Write-DeterministicFile -Path $path -Length $FileSize -Seed (1000 + $index)
    }

    Set-DeterministicPrefix -Path $files[0].path -Length ([uint64](64MB)) -Seed 9001
    $files[0].write_sha256 = (Get-FileHash -LiteralPath $files[0].path -Algorithm SHA256).Hash
    $files[0].read_sha256 = (Get-FileHash -LiteralPath $files[0].path -Algorithm SHA256).Hash

    $renamedDirectory = Join-Path $script:MountFull 'renamed'
    [IO.Directory]::CreateDirectory($renamedDirectory) | Out-Null
    $renamedPath = Join-Path $renamedDirectory 'file-000-renamed.bin'
    Move-Item -LiteralPath $files[0].path -Destination $renamedPath
    $files[0].path = $renamedPath
    Assert-M2Condition ((Get-FileHash -LiteralPath $renamedPath -Algorithm SHA256).Hash -eq $files[0].read_sha256) 'renamed file hash mismatch'

    $archiveSource = Join-Path $script:MountFull 'archive-staging'
    [IO.Directory]::CreateDirectory($archiveSource) | Out-Null
    $archiveFile = Join-Path $archiveSource 'file-003.bin'
    Move-Item -LiteralPath $files[3].path -Destination $archiveFile
    $archiveFinal = Join-Path $script:MountFull 'archive'
    Move-Item -LiteralPath $archiveSource -Destination $archiveFinal
    $files[3].path = Join-Path $archiveFinal 'file-003.bin'
    Assert-M2Condition ((Get-FileHash -LiteralPath $files[3].path -Algorithm SHA256).Hash -eq $files[3].read_sha256) 'renamed directory file hash mismatch'

    $manifest = Get-ManifestDigest $files
    $state.phase = 'setup_complete'
    $state.bytes_written = [uint64]($FileSize * $FileCount)
    $state.bytes_read = [uint64]($FileSize * $FileCount)
    $state.write_sha256 = $manifest
    $state.read_sha256 = $manifest
    $state.files = $files
    Save-WorkloadState $state
    Write-Output "M2 NTFS setup/write PASS: disk=$DiskNumber bytes=$($state.bytes_written)"
}

function Invoke-DeleteRetrim {
    Assert-M2Condition $SafetyAcknowledged 'destructive safety acknowledgement is required'
    if ($WhatIf) {
        Write-Output 'M2 NTFS delete/retrim WHATIF PASS; no mutation executed'
        return
    }
    $state = Load-WorkloadState
    $null = Assert-ExactCurrentDisk $false ([string]$state.serial_number) ([string]$state.disk_unique_id)
    Assert-M2Condition ([string]$state.phase -eq 'setup_complete') 'delete/retrim requires setup_complete state'
    Assert-M2Condition ([string]$state.write_sha256 -eq [string]$state.read_sha256) 'setup hashes differ before delete'

    $deleteNotify = @(& fsutil.exe behavior query DisableDeleteNotify 2>&1)
    Assert-M2Condition ($LASTEXITCODE -eq 0) 'fsutil DeleteNotify query failed'
    $deleteText = $deleteNotify -join [Environment]::NewLine
    Assert-M2Condition ($deleteText -match 'NTFS\s+DisableDeleteNotify\s*=\s*0') 'NTFS DeleteNotify is not enabled'

    $files = @($state.files)
    Assert-M2Condition ($files.Count -eq $FileCount) 'workload file manifest is incomplete'
    [uint64]$deleted = 0
    foreach ($index in @(0, 2)) {
        $path = [IO.Path]::GetFullPath([string]$files[$index].path)
        $mountPrefix = $script:MountFull.TrimEnd('\') + '\'
        Assert-M2Condition ($path.StartsWith($mountPrefix, [StringComparison]::OrdinalIgnoreCase)) 'refusing to delete a file outside the exact mount'
        Assert-M2Condition ([IO.File]::Exists($path)) "delete target is missing: $path"
        $deleted += [uint64]$files[$index].bytes
        Remove-Item -LiteralPath $path -Force
    }
    Assert-M2Condition ($deleted -ge [uint64](1GB)) 'delete workload is too small to prove trim'

    $partition = Assert-ExactCurrentPartition ([int]$state.partition_number) ([string]$state.serial_number) ([string]$state.disk_unique_id)
    $volume = Select-M2ExactVolume -Volumes @(Get-Volume -Partition $partition -ErrorAction Stop) -ExpectedUniqueId ([string]$state.volume_unique_id)
    Optimize-Volume -InputObject $volume -ReTrim -Verbose -ErrorAction Stop | Out-Null
    $state.phase = 'retrim_complete'
    $state.bytes_deleted = $deleted
    $state.delete_notify_enabled = $true
    $state.retrim_succeeded = $true
    Save-WorkloadState $state
    Write-Output "M2 NTFS delete/retrim PASS: disk=$DiskNumber bytes_deleted=$deleted"
}

function Invoke-RemoveMounts {
    Assert-M2Condition $SafetyAcknowledged 'destructive safety acknowledgement is required'
    if ($WhatIf) {
        Write-Output 'M2 NTFS mount cleanup WHATIF PASS; no mutation executed'
        return
    }
    if (-not [IO.File]::Exists($script:StateFull)) {
        $null = Assert-ExactCurrentDisk $false
        if ([IO.Directory]::Exists($script:MountFull)) {
            Assert-M2Condition ([IO.Directory]::GetFileSystemEntries($script:MountFull).Count -eq 0) 'unowned mount directory is non-empty'
            Remove-Item -LiteralPath $script:MountFull -Force
        }
        Write-Output "M2 NTFS mount cleanup PASS: no workload state was published for disk=$DiskNumber"
        return
    }
    $state = Load-WorkloadState
    $null = Assert-ExactCurrentDisk $false ([string]$state.serial_number) ([string]$state.disk_unique_id)
    $partitionNumber = [int]$state.partition_number
    if ($partitionNumber -gt 0) {
        $partition = Assert-ExactCurrentPartition $partitionNumber ([string]$state.serial_number) ([string]$state.disk_unique_id)
        if (-not [string]::IsNullOrWhiteSpace([string]$state.volume_unique_id)) {
            $null = Select-M2ExactVolume -Volumes @(Get-Volume -Partition $partition -ErrorAction Stop) -ExpectedUniqueId ([string]$state.volume_unique_id)
        }
        $matchingPaths = @($partition.AccessPaths | Where-Object {
            -not [string]::IsNullOrWhiteSpace([string]$_) -and
            [string]::Equals(([string]$_).TrimEnd('\'), $script:MountFull.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)
        })
        Assert-M2Condition ($matchingPaths.Count -le 1) 'exact mount path appears more than once on the owned partition'
        if ($matchingPaths.Count -eq 1) {
            Remove-PartitionAccessPath -InputObject $partition -AccessPath ([string]$matchingPaths[0]) -ErrorAction Stop
        }
    }
    $mountItem = Get-Item -LiteralPath $script:MountFull -Force -ErrorAction SilentlyContinue
    if ($null -ne $mountItem) {
        if (($mountItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Remove-Item -LiteralPath $script:MountFull -Force
        }
        else {
            Assert-M2Condition ([IO.Directory]::GetFileSystemEntries($script:MountFull).Count -eq 0) 'mount directory remained non-empty after access-path removal'
            Remove-Item -LiteralPath $script:MountFull -Force
        }
    }
    $state.phase = 'mounts_removed'
    Save-WorkloadState $state
    Write-Output "M2 NTFS mount cleanup PASS: disk=$DiskNumber"
}

$script:MountFull = Assert-AbsoluteSafePath $MountDirectory 'MountDirectory'
$script:MountAccessPath = ConvertTo-M2MountAccessPath $script:MountFull
$script:StateFull = Assert-AbsoluteSafePath $StatePath 'StatePath'
Assert-M2Condition ($DiskNumber -ne 0) 'refusing Disk 0'
Assert-M2Condition ($DeviceIdRaw -match '^\d+$' -and $DeviceIdRaw -ne '16777215') 'unsafe DeviceIdRaw'
Assert-M2Condition ($ProviderSessionId -match '^[A-Za-z0-9._-]+$') 'unsafe ProviderSessionId'

switch ($Phase) {
    'Preflight' {
        $inventory = Get-PreflightInventory
        $expected = [pscustomobject]@{
            DiskSizeBytes = $ExactDiskSize
            BusType = $ExpectedBusType
            VendorId = $ExpectedVendorId
            ProductId = $ExpectedProductId
            DeviceIdRaw = $DeviceIdRaw
            ProviderSessionId = $ProviderSessionId
        }
        $candidate = Select-M2SafeCandidate -BeforeDiskNumbers @($inventory.BeforeDiskNumbers) -AfterDisks @($inventory.AfterDisks) -ExpectedIdentity $expected
        Assert-M2Condition (-not [string]::IsNullOrWhiteSpace([string]$candidate.SerialNumber)) 'candidate serial identity is empty'
        Assert-M2Condition (-not [string]::IsNullOrWhiteSpace([string]$candidate.UniqueId)) 'candidate unique identity is empty'
        Test-M2MountSafety -MountExists ([bool]$inventory.MountExists) -MountEntryCount ([int]$inventory.MountEntryCount) -MountConflict ([bool]$inventory.MountConflict) -HasReparsePoint ([bool]$inventory.MountHasReparsePoint)
        Assert-M2Condition ([int]$candidate.Number -eq $DiskNumber) 'preflight selected a different disk'
        if ($WhatIf) {
            Write-Output "M2 NTFS WHATIF PASS: exact Disk $DiskNumber is safe; no mutation executed"
        }
        else {
            Write-Output "M2 NTFS preflight PASS: exact Disk $DiskNumber is safe"
        }
    }
    'SetupWrite' { Invoke-SetupWrite }
    'DeleteRetrim' { Invoke-DeleteRetrim }
    'RemoveMounts' { Invoke-RemoveMounts }
}
