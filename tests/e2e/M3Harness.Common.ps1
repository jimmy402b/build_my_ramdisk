Set-StrictMode -Version Latest

function Assert-M3Condition {
    param(
        [bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) { throw $Message }
}

function Get-M3FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    Assert-M3Condition (-not [string]::IsNullOrWhiteSpace($Path)) 'path is empty'
    Assert-M3Condition ($Path.IndexOfAny([char[]]'*?[]') -lt 0) "wildcard path is forbidden: $Path"
    try { return [IO.Path]::GetFullPath($Path) }
    catch { throw "path is invalid: $Path" }
}

function Get-M3ExistingAncestor {
    param([Parameter(Mandatory = $true)][string]$Path)
    $current = Get-M3FullPath $Path
    while (-not [IO.File]::Exists($current) -and
           -not [IO.Directory]::Exists($current)) {
        $parent = [IO.Directory]::GetParent($current)
        Assert-M3Condition ($null -ne $parent) "no existing ancestor for path: $Path"
        $current = $parent.FullName
    }
    return $current
}

function Assert-M3NoReparsePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $current = Get-M3ExistingAncestor $Path
    while ($true) {
        $item = Get-Item -LiteralPath $current -Force
        Assert-M3Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "reparse point is forbidden in safety path: $current"
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent -or $parent.FullName -eq $current) { break }
        $current = $parent.FullName
    }
}

function Assert-M3ScratchDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$ScratchRoot,
        [Parameter(Mandatory = $true)][string]$WorkspaceRoot,
        [switch]$WhatIf
    )
    $scratch = Get-M3FullPath $ScratchRoot
    $workspace = Get-M3FullPath $WorkspaceRoot
    Assert-M3Condition ([IO.Directory]::Exists($scratch)) 'scratch root does not exist'
    Assert-M3NoReparsePath $scratch
    Assert-M3Condition (-not [string]::Equals(
        $scratch.TrimEnd('\'), $workspace.TrimEnd('\'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'scratch root cannot be the workspace root'
    Assert-M3Condition ($scratch.TrimEnd('\') -ne [IO.Path]::GetPathRoot($scratch).TrimEnd('\')) `
        'scratch root cannot be a volume root'
    if (-not $WhatIf) {
        $probe = Join-Path $scratch ('.myrd-write-probe-' + [Guid]::NewGuid().ToString('N'))
        try {
            [IO.File]::WriteAllText($probe, 'probe', [Text.UTF8Encoding]::new($false))
        }
        finally {
            if ([IO.File]::Exists($probe)) {
                Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
            }
        }
    }
    return $scratch
}

function Test-M3PathUnderRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root,
        [switch]$AllowRoot
    )
    $fullPath = Get-M3FullPath $Path
    $fullRoot = (Get-M3FullPath $Root).TrimEnd('\') + '\'
    if ($AllowRoot -and [string]::Equals(
            $fullPath.TrimEnd('\'), $fullRoot.TrimEnd('\'),
            [StringComparison]::OrdinalIgnoreCase)) { return $true }
    return $fullPath.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-M3SafeTargetPath {
    param(
        [Parameter(Mandatory = $true)][string]$TargetPath,
        [Parameter(Mandatory = $true)][string]$ScratchRoot,
        [Parameter(Mandatory = $true)][string]$WorkspaceRoot
    )
    $target = Get-M3FullPath $TargetPath
    $scratch = Get-M3FullPath $ScratchRoot
    $workspace = Get-M3FullPath $WorkspaceRoot
    Assert-M3Condition (Test-M3PathUnderRoot $target $scratch) `
        'target path is outside the explicit scratch root'
    Assert-M3Condition (-not [string]::Equals(
        $target.TrimEnd('\'), $workspace.TrimEnd('\'),
        [StringComparison]::OrdinalIgnoreCase)) `
        'workspace root is not a valid destructive target'
    Assert-M3NoReparsePath $target
    return $target
}

function ConvertTo-M3DiskIdentity {
    param([Parameter(Mandatory = $true)]$InputObject)
    $required = @(
        'Number', 'SizeBytes', 'BusType', 'UniqueId', 'SerialNumber', 'DeviceId',
        'IsBoot', 'IsSystem', 'IsPageFile', 'IsClustered')
    foreach ($name in $required) {
        Assert-M3Condition ($null -ne $InputObject.PSObject.Properties[$name]) `
            "disk identity is missing: $name"
    }
    $number = [int]$InputObject.Number
    Assert-M3Condition ($number -gt 0) 'Disk 0 or automatic disk selection is forbidden'
    $size = [uint64]$InputObject.SizeBytes
    Assert-M3Condition ($size -gt 0) 'disk identity size must be positive'
    $values = [ordered]@{}
    foreach ($name in @('BusType', 'UniqueId', 'SerialNumber', 'DeviceId')) {
        $value = [string]$InputObject.$name
        Assert-M3Condition (-not [string]::IsNullOrWhiteSpace($value)) "disk identity $name is empty"
        Assert-M3Condition ($value.IndexOfAny([char[]]'*?') -lt 0) "disk identity $name contains wildcard"
        $values[$name] = $value
    }
    foreach ($name in @('IsBoot', 'IsSystem', 'IsPageFile', 'IsClustered')) {
        Assert-M3Condition (-not [bool]$InputObject.$name) "disk identity is unsafe: $name"
    }
    return [pscustomobject]@{
        Number = $number
        SizeBytes = $size
        BusType = $values.BusType
        UniqueId = $values.UniqueId
        SerialNumber = $values.SerialNumber
        DeviceId = $values.DeviceId
        IsBoot = $false
        IsSystem = $false
        IsPageFile = $false
        IsClustered = $false
    }
}

function Read-M3DiskIdentityJson {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-M3FullPath $Path
    Assert-M3Condition ([IO.File]::Exists($full)) "disk identity JSON is missing: $full"
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    try { $object = [IO.File]::ReadAllText($full, $utf8) | ConvertFrom-Json }
    catch { throw "disk identity JSON is invalid: $full" }
    return ConvertTo-M3DiskIdentity $object
}

function Test-M3DiskIdentityMatch {
    param(
        [Parameter(Mandatory = $true)]$Candidate,
        [Parameter(Mandatory = $true)]$Expected
    )
    try {
        $candidateIdentity = ConvertTo-M3DiskIdentity $Candidate
        $expectedIdentity = ConvertTo-M3DiskIdentity $Expected
    }
    catch { return $false }
    foreach ($name in @('Number', 'SizeBytes', 'BusType', 'UniqueId', 'SerialNumber', 'DeviceId')) {
        if (-not [string]::Equals([string]$candidateIdentity.$name,
                [string]$expectedIdentity.$name,
                [StringComparison]::OrdinalIgnoreCase)) { return $false }
    }
    return $true
}

function Select-M3ExactDisk {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Candidates,
        [Parameter(Mandatory = $true)]$ExpectedIdentity
    )
    $expected = ConvertTo-M3DiskIdentity $ExpectedIdentity
    $matches = @($Candidates | Where-Object {
        Test-M3DiskIdentityMatch -Candidate $_ -Expected $expected
    })
    Assert-M3Condition ($matches.Count -eq 1) `
        "exact disk identity must match one candidate; found $($matches.Count)"
    return $matches[0]
}

function Get-M3AvailableCommands {
    param([string[]]$Names)
    $found = [ordered]@{}
    foreach ($name in $Names) {
        $found[$name] = $null -ne (Get-Command $name -ErrorAction SilentlyContinue)
    }
    return [pscustomobject]$found
}

function Get-M3ReadOnlyHostProbe {
    param(
        [Parameter(Mandatory = $true)][string]$ExpectedDriverService,
        [Parameter(Mandatory = $true)][string]$ScratchRoot
    )
    $windowsIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $windowsPrincipal = [Security.Principal.WindowsPrincipal]::new($windowsIdentity)
    $isAdministrator = $windowsPrincipal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    $service = Get-Service -Name $ExpectedDriverService -ErrorAction SilentlyContinue
    $driver = Get-CimInstance Win32_SystemDriver -Filter `
        "Name='$ExpectedDriverService'" -ErrorAction SilentlyContinue
    $pnp = @()
    if ($null -ne (Get-Command Get-PnpDevice -ErrorAction SilentlyContinue)) {
        $pnp = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { [string]$_.Class -match 'SCSI|Storage' })
    }
    $commands = Get-M3AvailableCommands @(
        'Get-Disk', 'Get-Partition', 'Get-Volume', 'Get-PnpDevice',
        'Get-DiskImage', 'Mount-DiskImage', 'Dismount-DiskImage', 'powercfg')
    $power = @(& powercfg.exe /getactivescheme 2>&1)
    $scratchFull = Get-M3FullPath $ScratchRoot
    $volumeRoot = [IO.Path]::GetPathRoot($scratchFull)
    $drive = [IO.DriveInfo]::new($volumeRoot)
    $freeBytes = $null
    $filesystemReady = $false
    try {
        $drive.Refresh()
        $freeBytes = [uint64]$drive.AvailableFreeSpace
        $filesystemReady = $drive.IsReady
    }
    catch { $filesystemReady = $false }
    return [pscustomobject]@{
        IsAdministrator = [bool]$isAdministrator
        Driver = [pscustomobject]@{
            ServiceName = $ExpectedDriverService
            ServiceState = if ($null -eq $service) { 'Absent' } else { [string]$service.Status }
            StartType = if ($null -eq $service) { $null } else { [string]$service.StartType }
            PathName = if ($null -eq $driver) { $null } else { [string]$driver.PathName }
            Readable = $null -ne $driver
        }
        AimInterface = [pscustomobject]@{
            PresentStoragePnpCount = $pnp.Count
            PresentStoragePnpNames = @($pnp | ForEach-Object { [string]$_.FriendlyName })
        }
        Commands = $commands
        PowerPolicy = [pscustomobject]@{ Readable = ($power.Count -gt 0); Output = @($power) }
        Filesystem = [pscustomobject]@{
            ScratchVolumeRoot = $volumeRoot
            Ready = [bool]$filesystemReady
            AvailableFreeBytes = $freeBytes
        }
    }
}

function Get-M3PageFileDiskNumbers {
    $numbers = [Collections.Generic.HashSet[int]]::new()
    $pageFiles = @(Get-CimInstance Win32_PageFileUsage -ErrorAction Stop)
    foreach ($pageFile in $pageFiles) {
        $name = [string]$pageFile.Name
        $root = [IO.Path]::GetPathRoot($name)
        Assert-M3Condition ($root -match '^[A-Za-z]:\\$') `
            "pagefile path cannot be mapped to a drive: $name"
        $partition = Get-Partition -DriveLetter $root.Substring(0, 1) `
            -ErrorAction Stop
        foreach ($item in @($partition)) {
            [void]$numbers.Add([int]$item.DiskNumber)
        }
    }
    return $numbers
}

function Get-M3ActualDiskCandidates {
    $getDisk = Get-Command Get-Disk -ErrorAction SilentlyContinue
    $getPhysicalDisk = Get-Command Get-PhysicalDisk -ErrorAction SilentlyContinue
    Assert-M3Condition ($null -ne $getDisk -and $null -ne $getPhysicalDisk) `
        'Get-Disk and Get-PhysicalDisk are required for exact disk identity'

    $pageFileDiskNumbers = Get-M3PageFileDiskNumbers
    $physicalDisks = @(Get-PhysicalDisk -ErrorAction Stop)
    $candidates = [Collections.ArrayList]::new()
    foreach ($disk in @(Get-Disk -ErrorAction Stop)) {
        $physical = @($physicalDisks | Where-Object {
            [string]$_.DeviceId -eq [string]$disk.Number
        })
        # A disk without a unique PhysicalDisk identity is not a safe target.
        if ($physical.Count -ne 1) { continue }
        [void]$candidates.Add([pscustomobject]@{
            Number = [int]$disk.Number
            SizeBytes = [uint64]$disk.Size
            BusType = [string]$disk.BusType
            UniqueId = [string]$disk.UniqueId
            SerialNumber = [string]$disk.SerialNumber
            DeviceId = [string]$physical[0].DeviceId
            IsBoot = [bool]$disk.IsBoot
            IsSystem = [bool]$disk.IsSystem
            IsPageFile = $pageFileDiskNumbers.Contains([int]$disk.Number)
            IsClustered = [bool]$disk.IsClustered
        })
    }
    return @($candidates)
}

function Assert-M3ExpectedDiskOwnsPath {
    param(
        [Parameter(Mandatory = $true)][string]$ScratchRoot,
        [Parameter(Mandatory = $true)]$ExpectedIdentity
    )
    $full = Get-M3FullPath $ScratchRoot
    $root = [IO.Path]::GetPathRoot($full)
    Assert-M3Condition ($root -match '^[A-Za-z]:\\$') `
        'scratch root must be on a drive volume for disk identity binding'
    $partitions = @(Get-Partition -DriveLetter $root.Substring(0, 1) -ErrorAction Stop)
    Assert-M3Condition ($partitions.Count -eq 1) `
        'scratch root must map to exactly one partition'
    $diskNumber = [int]$partitions[0].DiskNumber
    $selected = Select-M3ExactDisk `
        -Candidates (Get-M3ActualDiskCandidates) `
        -ExpectedIdentity $ExpectedIdentity
    Assert-M3Condition ([int]$selected.Number -eq $diskNumber) `
        'ScratchRoot is not located on the approved exact disk identity'
    return $selected
}

function New-M3CleanupLedger {
    return [pscustomobject]@{
        Entries = [Collections.ArrayList]::new()
        Finalized = $false
    }
}

function Add-M3CleanupEntry {
    param(
        [Parameter(Mandatory = $true)]$Ledger,
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$RecoveryCommand
    )
    Assert-M3Condition (-not $Ledger.Finalized) 'cleanup ledger is already finalized'
    [void]$Ledger.Entries.Add([pscustomobject]@{
        Kind = $Kind
        Description = $Description
        Action = $Action
        RecoveryCommand = $RecoveryCommand
        Status = 'pending'
        Error = $null
    })
}

function Invoke-M3CleanupLedger {
    param(
        [Parameter(Mandatory = $true)]$Ledger,
        [switch]$WhatIf
    )
    if ($Ledger.Finalized) { return @($Ledger.Entries) }
    foreach ($entry in @($Ledger.Entries)) {
        if ($WhatIf) {
            $entry.Status = 'whatif-not-run'
            continue
        }
        if ($entry.Status -eq 'succeeded') { continue }
        try {
            & $entry.Action
            $entry.Status = 'succeeded'
        }
        catch {
            $entry.Status = 'failed'
            $entry.Error = [string]$_.Exception.Message
        }
    }
    $Ledger.Finalized = $true
    return @($Ledger.Entries)
}

function Invoke-M3Preflight {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string]$ScratchRoot,
        [Parameter(Mandatory = $true)][string]$WorkspaceRoot,
        [Parameter(Mandatory = $true)]$ExpectedDiskIdentity,
        [string]$ExpectedDriverService = 'phdskmnt',
        [switch]$WhatIf
    )
    $reasons = [Collections.ArrayList]::new()
    $static = [ordered]@{
        executable_exists = $false
        scratch_root_safe = $false
        disk_identity_valid = $false
        workspace_root_rejected = $false
        actual_disk_identity_match = $false
        scratch_disk_identity_match = $false
        exact_disk_mutation_permitted = $false
    }
    try {
        $exe = Get-M3FullPath $Executable
        $static.executable_exists = [IO.File]::Exists($exe)
        Assert-M3Condition $static.executable_exists 'M3 executable is missing'
        $null = Assert-M3ScratchDirectory $ScratchRoot $WorkspaceRoot -WhatIf:$WhatIf
        $static.scratch_root_safe = $true
        $null = ConvertTo-M3DiskIdentity $ExpectedDiskIdentity
        $static.disk_identity_valid = $true
        $static.workspace_root_rejected = $true
    }
    catch { [void]$reasons.Add([string]$_.Exception.Message) }

    $hostProbe = $null
    $actualDisk = $null
    if (-not $WhatIf) {
        try { $hostProbe = Get-M3ReadOnlyHostProbe $ExpectedDriverService $ScratchRoot }
        catch { [void]$reasons.Add("read-only host probe failed: $($_.Exception.Message)") }
        if ($null -ne $hostProbe -and -not $hostProbe.IsAdministrator) {
            [void]$reasons.Add('Administrator privileges are required for real preflight')
        }
        if ($null -ne $hostProbe -and $hostProbe.Driver.ServiceState -ne 'Running') {
            [void]$reasons.Add('accepted AIM driver service is not Running')
        }
        try {
            $actualDisk = Select-M3ExactDisk `
                -Candidates (Get-M3ActualDiskCandidates) `
                -ExpectedIdentity $ExpectedDiskIdentity
            $static.actual_disk_identity_match = $true
            $null = Assert-M3ExpectedDiskOwnsPath `
                -ScratchRoot $ScratchRoot -ExpectedIdentity $ExpectedDiskIdentity
            $static.scratch_disk_identity_match = $true
        }
        catch {
            [void]$reasons.Add("exact disk identity probe failed: $($_.Exception.Message)")
        }
    }
    $status = if ($reasons.Count -eq 0) { 'PASS' } else { 'BLOCKED' }
    return [pscustomobject][ordered]@{
        schema = 'myrd.m3.preflight.v1'
        status = $status
        terminal_verdict = $status
        reason = if ($reasons.Count -eq 0) { $null } else { $reasons -join '; ' }
        what_if = [bool]$WhatIf
        actual_is_administrator = if ($null -eq $hostProbe) { $false } else { [bool]$hostProbe.IsAdministrator }
        exact_disk_mutation_permitted = $false
        mutation_commands = @()
        static_checks = [pscustomobject]$static
        host = $hostProbe
        actual_disk_identity = $actualDisk
        expected_disk_identity = ConvertTo-M3DiskIdentity $ExpectedDiskIdentity
        cleanup = [pscustomobject]@{
            attempted = $false
            succeeded = $true
            residuals = @()
        }
    }
}

function Write-M3Json {
    param([Parameter(Mandatory = $true)]$InputObject, [Parameter(Mandatory = $true)][string]$Path)
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText((Get-M3FullPath $Path),
        ($InputObject | ConvertTo-Json -Depth 20), $utf8)
}
