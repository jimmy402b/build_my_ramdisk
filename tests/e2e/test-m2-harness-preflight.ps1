$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'M2Harness.Common.ps1')

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Fails([scriptblock]$Action, [string]$Expected) {
    $caught = $null
    try { & $Action } catch { $caught = [string]$_ }
    Require ($null -ne $caught) "Expected failure containing: $Expected"
    Require ($caught -match [regex]::Escape($Expected)) "Unexpected failure. expected=$Expected actual=$caught"
}

function New-StaticInput {
    [pscustomobject]@{
        IsAdministrator = $true
        ExecutableExists = $true
        InventoryProbeExists = $true
        Driver = [pscustomobject]@{
            Readable = $true
            ServiceName = 'phdskmnt'
            State = 'Running'
            Sha256 = ('A' * 64)
        }
        ServiceBaselineReadable = $true
        MissingCommands = @()
        Evidence = [pscustomobject]@{
            Exists = $true
            IsDirectory = $true
            Writable = $true
            WouldOverwrite = $false
            HasReparsePoint = $false
        }
        ForbiddenModules = @()
        ForbiddenProcesses = @()
    }
}

function New-Candidate {
    param([int]$Number = 42)
    [pscustomobject]@{
        Number = $Number
        Size = 68719476736
        BusType = 'SCSI'
        VendorId = 'MYRD'
        ProductId = 'DynamicDisk'
        DeviceIdRaw = '197121'
        ProviderSessionId = 'm2-session'
        SerialNumber = 'm2-serial'
        UniqueId = 'm2-unique'
        IsBoot = $false
        IsSystem = $false
        IsPageFile = $false
        IsClustered = $false
        IsOffline = $false
        IsReadOnly = $false
        PartitionCount = 0
    }
}

function New-ExpectedIdentity {
    [pscustomobject]@{
        DiskSizeBytes = 68719476736
        BusType = 'SCSI'
        VendorId = 'MYRD'
        ProductId = 'DynamicDisk'
        DeviceIdRaw = '197121'
        ProviderSessionId = 'm2-session'
    }
}

$passed = 0

$caseInput = New-StaticInput
$caseInput.IsAdministrator = $false
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'Administrator privileges'
$passed++

$caseInput = New-StaticInput
$caseInput.ExecutableExists = $false
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'executable'
$passed++

$caseInput = New-StaticInput
$caseInput.InventoryProbeExists = $false
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'inventory probe'
$passed++

$caseInput = New-StaticInput
$caseInput.Driver.Readable = $false
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'driver identity'
$passed++

$caseInput = New-StaticInput
$caseInput.Driver.State = 'Stopped'
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'driver is not Running'
$passed++

$caseInput = New-StaticInput
$caseInput.Driver.Sha256 = ('B' * 64)
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput -ExpectedDriverSha256 ('A' * 64) } 'driver SHA-256'
$passed++

$caseInput = New-StaticInput
$caseInput.ServiceBaselineReadable = $false
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'service baseline'
$passed++

$caseInput = New-StaticInput
$caseInput.MissingCommands = @('Format-Volume')
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'required Windows command'
$passed++

$caseInput = New-StaticInput
$caseInput.Evidence.Writable = $false
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'evidence directory'
$passed++

$caseInput = New-StaticInput
$caseInput.Evidence.WouldOverwrite = $true
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'overwrite'
$passed++

$caseInput = New-StaticInput
$caseInput.ForbiddenModules = @('aimapi.dll')
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'forbidden AIM'
$passed++

$caseInput = New-StaticInput
$caseInput.ForbiddenProcesses = @('RamDyn.exe')
Assert-Fails { Test-M2StaticPreflight -InputObject $caseInput } 'forbidden AIM'
$passed++

$identity = New-ExpectedIdentity
Assert-Fails { Select-M2SafeCandidate -BeforeDiskNumbers @(0, 1) -AfterDisks @() -ExpectedIdentity $identity } 'exactly one new disk'
$passed++

Assert-Fails { Select-M2SafeCandidate -BeforeDiskNumbers @(0, 1) -AfterDisks @((New-Candidate 41), (New-Candidate 42)) -ExpectedIdentity $identity } 'exactly one new disk'
$passed++

Assert-Fails { Select-M2SafeCandidate -BeforeDiskNumbers @(1) -AfterDisks @((New-Candidate 0)) -ExpectedIdentity $identity } 'Disk 0'
$passed++

foreach ($property in @('IsBoot', 'IsSystem', 'IsPageFile', 'IsClustered')) {
    $candidate = New-Candidate
    $candidate.$property = $true
    Assert-Fails { Select-M2SafeCandidate -BeforeDiskNumbers @(0, 1) -AfterDisks @($candidate) -ExpectedIdentity $identity } $property
    $passed++
}

$candidate = New-Candidate
$candidate.Size = 34359738368
Assert-Fails { Select-M2SafeCandidate -BeforeDiskNumbers @(0, 1) -AfterDisks @($candidate) -ExpectedIdentity $identity } '64 GiB'
$passed++

foreach ($property in @('BusType', 'VendorId', 'ProductId', 'DeviceIdRaw', 'ProviderSessionId')) {
    $candidate = New-Candidate
    $candidate.$property = 'unexpected'
    Assert-Fails { Select-M2SafeCandidate -BeforeDiskNumbers @(0, 1) -AfterDisks @($candidate) -ExpectedIdentity $identity } $property
    $passed++
}

$candidate = New-Candidate
$candidate.PartitionCount = 1
Assert-Fails { Select-M2SafeCandidate -BeforeDiskNumbers @(0, 1) -AfterDisks @($candidate) -ExpectedIdentity $identity } 'partition'
$passed++

Assert-Fails { Test-M2MountSafety -MountExists $true -MountEntryCount 1 -MountConflict $false -HasReparsePoint $false } 'empty'
$passed++

Assert-Fails { Test-M2MountSafety -MountExists $false -MountEntryCount 0 -MountConflict $true -HasReparsePoint $false } 'conflict'
$passed++

Assert-Fails { Test-M2MountSafety -MountExists $true -MountEntryCount 0 -MountConflict $false -HasReparsePoint $true } 'reparse point'
$passed++

$safeInput = New-StaticInput
Test-M2StaticPreflight -InputObject $safeInput -ExpectedDriverSha256 ('A' * 64)
$selected = Select-M2SafeCandidate -BeforeDiskNumbers @(0, 1) -AfterDisks @((New-Candidate)) -ExpectedIdentity $identity
Test-M2MountSafety -MountExists $true -MountEntryCount 0 -MountConflict $false -HasReparsePoint $false
Require ($selected.Number -eq 42) 'safe candidate was not returned'
$passed++

$current = New-Candidate
Test-M2ExactCurrentCandidate -Candidate $current -ExpectedIdentity $identity -ExpectedSerialNumber 'm2-serial' -ExpectedUniqueId 'm2-unique' -RequireNoPartitions
$passed++

foreach ($property in @('SerialNumber', 'UniqueId')) {
    $current = New-Candidate
    $current.$property = 'changed'
    Assert-Fails { Test-M2ExactCurrentCandidate -Candidate $current -ExpectedIdentity $identity -ExpectedSerialNumber 'm2-serial' -ExpectedUniqueId 'm2-unique' -RequireNoPartitions } $property
    $passed++
}

foreach ($property in @('IsOffline', 'IsReadOnly')) {
    $current = New-Candidate
    $current.$property = $true
    Assert-Fails { Test-M2ExactCurrentCandidate -Candidate $current -ExpectedIdentity $identity -ExpectedSerialNumber 'm2-serial' -ExpectedUniqueId 'm2-unique' -RequireNoPartitions } $property
    $passed++
}

$current = New-Candidate
$current.IsReadOnly = $true
Test-M2ExactCurrentCandidate -Candidate $current -ExpectedIdentity $identity -ExpectedSerialNumber 'm2-serial' -ExpectedUniqueId 'm2-unique' -RequireNoPartitions -AllowReadOnly
$passed++

Test-M2InitializedGptPartitionSet -Partitions @()
$passed++

$msr = [pscustomobject]@{
    DiskNumber = 42
    PartitionNumber = 1
    GptType = '{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}'
    Size = [uint64]((16MB) - (34 * 512))
    DriveLetter = [char]0
    AccessPaths = @('')
}
Test-M2InitializedGptPartitionSet -Partitions @($msr)
$passed++

$dataPartition = [pscustomobject]@{
    DiskNumber = 42
    PartitionNumber = 2
    GptType = '{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}'
    Size = [uint64](1GB)
    DriveLetter = 'R'
    AccessPaths = @('R:\')
}
Assert-Fails { Test-M2InitializedGptPartitionSet -Partitions @($dataPartition) } 'reserved partition'
$passed++

Assert-Fails { Test-M2InitializedGptPartitionSet -Partitions @($msr, $msr) } 'at most one'
$passed++

$owned = Test-M2OwnedDataPartitionSet -Partitions @($msr, $dataPartition) -DiskNumber 42 -OwnedPartitionNumber 2
Require ([int]$owned.PartitionNumber -eq 2) 'owned data partition was not returned'
$passed++

Assert-Fails { Test-M2OwnedDataPartitionSet -Partitions @($msr) -DiskNumber 42 -OwnedPartitionNumber 2 } 'exactly once'
$passed++

$otherData = $dataPartition.PSObject.Copy()
$otherData.PartitionNumber = 3
Assert-Fails { Test-M2OwnedDataPartitionSet -Partitions @($msr, $dataPartition, $otherData) -DiskNumber 42 -OwnedPartitionNumber 2 } 'at most one'
$passed++

$normalizedMount = ConvertTo-M2MountAccessPath 'C:\m2\mount'
Require ($normalizedMount -eq 'C:\m2\mount\') 'mount access path was not normalized'
$passed++

$volume = [pscustomobject]@{ UniqueId = '\\?\Volume{11111111-1111-1111-1111-111111111111}\' }
$selectedVolume = Select-M2ExactVolume -Volumes @($volume) -ExpectedUniqueId $volume.UniqueId
Require ($selectedVolume.UniqueId -eq $volume.UniqueId) 'exact volume was not returned'
$passed++

Assert-Fails { Select-M2ExactVolume -Volumes @($volume) -ExpectedUniqueId '\\?\Volume{22222222-2222-2222-2222-222222222222}\' } 'exactly one'
$passed++

Assert-Fails { Select-M2ExactVolume -Volumes @($volume, $volume) -ExpectedUniqueId $volume.UniqueId } 'exactly one'
$passed++

$whatIfRoot = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m2-whatif-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($whatIfRoot) | Out-Null
try {
    $inventoryPath = Join-Path $whatIfRoot 'inventory.json'
    $statePath = Join-Path $whatIfRoot 'state.json'
    $mountPath = Join-Path $whatIfRoot 'mount'
    [IO.Directory]::CreateDirectory($mountPath) | Out-Null
    [pscustomobject]@{
        BeforeDiskNumbers = @(0, 1)
        AfterDisks = @((New-Candidate))
        MountExists = $true
        MountEntryCount = 0
        MountConflict = $false
        MountHasReparsePoint = $false
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $inventoryPath -Encoding utf8
    $output = @(& (Join-Path $PSScriptRoot 'Invoke-M2NtfsWorkload.ps1') -Phase Preflight -DiskNumber 42 -DeviceIdRaw '197121' -ProviderSessionId 'm2-session' -ExpectedBusType 'SCSI' -ExpectedVendorId 'MYRD' -ExpectedProductId 'DynamicDisk' -MountDirectory $mountPath -StatePath $statePath -TestInventoryPath $inventoryPath -WhatIf 2>&1)
    Require ($null -ne $output) "WhatIf produced no output"
    Require (($output -join [Environment]::NewLine) -match 'WHATIF PASS') 'WhatIf did not emit PASS'
    Require (-not [IO.File]::Exists($statePath)) 'WhatIf created a workload state file'
    Require ([IO.Directory]::GetFileSystemEntries($mountPath).Count -eq 0) 'WhatIf mutated the mount directory'
    $passed++
}
finally {
    if ([IO.Directory]::Exists($whatIfRoot)) {
        Remove-Item -LiteralPath $whatIfRoot -Recurse -Force
    }
}

Require ($passed -eq 48) "Expected 48 checks, passed $passed"
Write-Output "M2 harness preflight: $passed/48 PASS"
