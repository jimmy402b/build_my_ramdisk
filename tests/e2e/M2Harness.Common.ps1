Set-StrictMode -Version Latest

function Assert-M2Condition {
    param(
        [bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) { throw $Message }
}

function ConvertTo-M2NativeArgument {
    param([AllowEmptyString()][string]$Value)
    if ($Value -notmatch '[\s"]') { return $Value }
    $quoted = '"'
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $slashes++
            continue
        }
        if ($character -eq '"') {
            $quoted += ('\' * (($slashes * 2) + 1)) + '"'
            $slashes = 0
            continue
        }
        $quoted += ('\' * $slashes) + $character
        $slashes = 0
    }
    $quoted += ('\' * ($slashes * 2)) + '"'
    return $quoted
}

function Invoke-M2NativeProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @()
    )

    $fullPath = [IO.Path]::GetFullPath($FilePath)
    Assert-M2Condition ([IO.File]::Exists($fullPath)) "native executable is missing: $fullPath"
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $fullPath
    $startInfo.Arguments = (@($Arguments | ForEach-Object { ConvertTo-M2NativeArgument ([string]$_) }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        Assert-M2Condition $process.Start() "failed to start native process: $fullPath"
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        return [pscustomobject]@{
            ExitCode = [int]$process.ExitCode
            StandardOutput = [string]$stdout
            StandardError = [string]$stderr
        }
    }
    finally {
        $process.Dispose()
    }
}

function Test-M2StaticPreflight {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$InputObject,
        [string]$ExpectedDriverService = 'phdskmnt',
        [string]$ExpectedDriverSha256 = '',
        [switch]$AllowNonAdministratorForWhatIf
    )

    Assert-M2Condition ([bool]$InputObject.IsAdministrator -or $AllowNonAdministratorForWhatIf) 'Administrator privileges are required for real M2 mode'
    Assert-M2Condition ([bool]$InputObject.ExecutableExists) 'M2 executable is missing'
    Assert-M2Condition ([bool]$InputObject.InventoryProbeExists) 'read-only AIM inventory probe is missing'
    Assert-M2Condition ($null -ne $InputObject.Driver -and [bool]$InputObject.Driver.Readable) 'accepted driver identity could not be read'
    Assert-M2Condition ([string]::Equals([string]$InputObject.Driver.ServiceName, $ExpectedDriverService, [StringComparison]::OrdinalIgnoreCase)) 'driver service identity does not match'
    Assert-M2Condition ([string]$InputObject.Driver.State -eq 'Running') 'accepted driver is not Running'
    Assert-M2Condition ([string]$InputObject.Driver.Sha256 -match '^[0-9A-Fa-f]{64}$') 'driver SHA-256 evidence is invalid'
    if (-not [string]::IsNullOrWhiteSpace($ExpectedDriverSha256)) {
        Assert-M2Condition ([string]$InputObject.Driver.Sha256 -ieq $ExpectedDriverSha256) 'driver SHA-256 does not match the accepted identity'
    }
    Assert-M2Condition ([bool]$InputObject.ServiceBaselineReadable) 'service baseline could not be read'
    $missingCommands = @($InputObject.MissingCommands)
    Assert-M2Condition ($missingCommands.Count -eq 0) "required Windows command is missing: $($missingCommands -join ', ')"

    $evidence = $InputObject.Evidence
    Assert-M2Condition ($null -ne $evidence -and [bool]$evidence.Exists -and [bool]$evidence.IsDirectory -and [bool]$evidence.Writable -and -not [bool]$evidence.HasReparsePoint) 'evidence directory is missing, unsafe, or not writable'
    Assert-M2Condition (-not [bool]$evidence.WouldOverwrite) 'refusing to overwrite existing evidence'

    $forbiddenModules = @($InputObject.ForbiddenModules)
    $forbiddenProcesses = @($InputObject.ForbiddenProcesses)
    Assert-M2Condition ($forbiddenModules.Count -eq 0 -and $forbiddenProcesses.Count -eq 0) 'forbidden AIM user-mode module or process is present'
}

function Select-M2SafeCandidate {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object[]]$BeforeDiskNumbers,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$AfterDisks,
        [Parameter(Mandatory = $true)]$ExpectedIdentity
    )

    $before = @($BeforeDiskNumbers | ForEach-Object { [int]$_ })
    $candidates = @($AfterDisks | Where-Object { $before -notcontains [int]$_.Number })
    Assert-M2Condition ($candidates.Count -eq 1) "expected exactly one new disk; found $($candidates.Count)"
    $candidate = $candidates[0]

    Assert-M2Condition ([int]$candidate.Number -ne 0) 'refusing to select Disk 0'
    Assert-M2Condition (-not [bool]$candidate.IsBoot) 'candidate IsBoot must be false'
    Assert-M2Condition (-not [bool]$candidate.IsSystem) 'candidate IsSystem must be false'
    Assert-M2Condition (-not [bool]$candidate.IsPageFile) 'candidate IsPageFile must be false'
    Assert-M2Condition (-not [bool]$candidate.IsClustered) 'candidate IsClustered must be false'
    Assert-M2Condition ([uint64]$candidate.Size -eq [uint64]$ExpectedIdentity.DiskSizeBytes) 'candidate must be exactly 64 GiB'

    foreach ($property in @('BusType', 'VendorId', 'ProductId', 'DeviceIdRaw', 'ProviderSessionId')) {
        $actual = [string]$candidate.$property
        $expected = [string]$ExpectedIdentity.$property
        Assert-M2Condition ([string]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) "candidate $property does not match exact provider identity: expected='$expected' actual='$actual'"
    }
    Assert-M2Condition ([int]$candidate.PartitionCount -eq 0) 'candidate already contains a partition'
    return $candidate
}

function Test-M2ExactCurrentCandidate {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Candidate,
        [Parameter(Mandatory = $true)]$ExpectedIdentity,
        [string]$ExpectedSerialNumber = '',
        [string]$ExpectedUniqueId = '',
        [switch]$RequireNoPartitions,
        [switch]$AllowReadOnly
    )

    Assert-M2Condition ([int]$Candidate.Number -ne 0) 'refusing to mutate Disk 0'
    Assert-M2Condition ([uint64]$Candidate.Size -eq [uint64]$ExpectedIdentity.DiskSizeBytes) 'current disk is no longer exactly 64 GiB'
    foreach ($property in @('BusType', 'VendorId', 'ProductId', 'DeviceIdRaw', 'ProviderSessionId')) {
        $actual = [string]$Candidate.$property
        $expected = [string]$ExpectedIdentity.$property
        Assert-M2Condition ([string]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) "current disk $property changed: expected='$expected' actual='$actual'"
    }
    Assert-M2Condition (-not [bool]$Candidate.IsBoot) 'current disk IsBoot changed'
    Assert-M2Condition (-not [bool]$Candidate.IsSystem) 'current disk IsSystem changed'
    Assert-M2Condition (-not [bool]$Candidate.IsPageFile) 'current disk IsPageFile changed'
    Assert-M2Condition (-not [bool]$Candidate.IsClustered) 'current disk IsClustered changed'
    Assert-M2Condition (-not [bool]$Candidate.IsOffline) 'current disk IsOffline changed'
    if (-not $AllowReadOnly) {
        Assert-M2Condition (-not [bool]$Candidate.IsReadOnly) 'current disk IsReadOnly changed'
    }
    Assert-M2Condition (-not [string]::IsNullOrWhiteSpace([string]$Candidate.SerialNumber)) 'current disk SerialNumber is empty'
    Assert-M2Condition (-not [string]::IsNullOrWhiteSpace([string]$Candidate.UniqueId)) 'current disk UniqueId is empty'
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSerialNumber)) {
        Assert-M2Condition ([string]::Equals([string]$Candidate.SerialNumber, $ExpectedSerialNumber, [StringComparison]::OrdinalIgnoreCase)) 'current disk SerialNumber changed'
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedUniqueId)) {
        Assert-M2Condition ([string]::Equals([string]$Candidate.UniqueId, $ExpectedUniqueId, [StringComparison]::OrdinalIgnoreCase)) 'current disk UniqueId changed'
    }
    if ($RequireNoPartitions) {
        Assert-M2Condition ([int]$Candidate.PartitionCount -eq 0) 'current disk acquired a partition before initialization'
    }
    return $Candidate
}

function Test-M2InitializedGptPartitionSet {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Partitions)

    Assert-M2Condition ($Partitions.Count -le 1) 'initialized GPT disk may contain at most one reserved partition'
    if ($Partitions.Count -eq 0) { return }

    $partition = $Partitions[0]
    $msrGuid = '{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}'
    Assert-M2Condition ([string]::Equals([string]$partition.GptType, $msrGuid, [StringComparison]::OrdinalIgnoreCase)) 'initialized GPT disk contains a non-reserved partition'
    $size = [uint64]$partition.Size
    $expectedSize = [uint64]((16MB) - (34 * 512))
    Assert-M2Condition ($size -eq $expectedSize) "initialized GPT reserved partition has an unexpected size: expected=$expectedSize actual=$size"
    $driveLetter = [string]$partition.DriveLetter
    $driveLetterAbsent = [string]::IsNullOrEmpty($driveLetter) -or ($driveLetter.Length -eq 1 -and [int][char]$driveLetter[0] -eq 0)
    Assert-M2Condition $driveLetterAbsent 'initialized GPT reserved partition has a drive letter'
    $accessPaths = @($partition.AccessPaths | Where-Object {
        $text = [string]$_
        -not [string]::IsNullOrEmpty($text) -and -not ($text.Length -eq 1 -and [int][char]$text[0] -eq 0)
    })
    Assert-M2Condition ($accessPaths.Count -eq 0) 'initialized GPT reserved partition has access paths'
}

function Test-M2OwnedDataPartitionSet {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object[]]$Partitions,
        [Parameter(Mandatory = $true)][int]$DiskNumber,
        [Parameter(Mandatory = $true)][int]$OwnedPartitionNumber
    )

    Assert-M2Condition ($DiskNumber -ne 0 -and $OwnedPartitionNumber -gt 0) 'owned partition identity is unsafe'
    $owned = @($Partitions | Where-Object {
        [int]$_.DiskNumber -eq $DiskNumber -and [int]$_.PartitionNumber -eq $OwnedPartitionNumber
    })
    Assert-M2Condition ($owned.Count -eq 1) 'owned data partition must appear exactly once'
    $basicDataGuid = '{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}'
    Assert-M2Condition ([string]::Equals([string]$owned[0].GptType, $basicDataGuid, [StringComparison]::OrdinalIgnoreCase)) 'owned partition is not GPT basic data'
    $other = @($Partitions | Where-Object {
        -not ([int]$_.DiskNumber -eq $DiskNumber -and [int]$_.PartitionNumber -eq $OwnedPartitionNumber)
    })
    Test-M2InitializedGptPartitionSet -Partitions $other
    return $owned[0]
}

function ConvertTo-M2MountAccessPath {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    Assert-M2Condition ([IO.Path]::IsPathRooted($full)) 'mount access path must be absolute'
    return $full.TrimEnd('\') + '\'
}

function Select-M2ExactVolume {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object[]]$Volumes,
        [Parameter(Mandatory = $true)][string]$ExpectedUniqueId
    )

    Assert-M2Condition (-not [string]::IsNullOrWhiteSpace($ExpectedUniqueId)) 'expected volume UniqueId is empty'
    $matches = @($Volumes | Where-Object {
        [string]::Equals([string]$_.UniqueId, $ExpectedUniqueId, [StringComparison]::OrdinalIgnoreCase)
    })
    Assert-M2Condition ($matches.Count -eq 1) 'expected volume UniqueId must match exactly one partition-associated volume'
    return $matches[0]
}

function Test-M2MountSafety {
    [CmdletBinding()]
    param(
        [bool]$MountExists,
        [int]$MountEntryCount,
        [bool]$MountConflict,
        [bool]$HasReparsePoint
    )

    Assert-M2Condition (-not $MountConflict) 'mount path conflicts with an existing volume or run'
    Assert-M2Condition (-not $HasReparsePoint) 'mount path must not traverse a reparse point'
    if ($MountExists) {
        Assert-M2Condition ($MountEntryCount -eq 0) 'mount directory must be empty'
    }
}
