param(
    [Parameter(Mandatory = 1)]
    [UInt32]$DiskNumber,
    [Parameter(Mandatory = 1)]
[char]$DriveLetter
)

$ErrorActionPreference = 'Stop'

if ($DiskNumber -eq [UInt32]::MaxValue) { throw 'invalid disk number' }
if ($DriveLetter -notmatch '^[A-Za-z]$') { throw 'invalid drive letter' }
$DriveLetter = [char]::ToUpperInvariant($DriveLetter)

$diskResults = @(Get-Disk -Number $DiskNumber)
$disk = $diskResults[$diskResults.Count - 1]
if ($disk.PartitionStyle -ne 'RAW') {
    # **The message has to identify which disk was checked, not just that it was wrong.**
    #
    # "disk is not RAW" is true and useless: it cannot distinguish "we were handed a stale or foreign
    # disk number" from "the disk really is ours and carries an old partition table".  Those need opposite
    # fixes, so the report names the disk that was inspected and lists every disk that exists.
    #
    # Written with `foreach` statements rather than pipelines on purpose: this script is held to an
    # allowlist of storage commands, and `ForEach-Object` is not on it.  A `foreach` statement is language
    # syntax, not a command invocation, so it stays inside the guard while doing the same work.
    $surveyParts = @()
    foreach ($other in @(Get-Disk)) {
        $surveyParts += "#$($other.Number)/$($other.PartitionStyle)/$($other.Size)/boot=$($other.IsBoot)/sys=$($other.IsSystem)"
    }
    $survey = $surveyParts -join ' '

    # **And it reports the actual bytes, because that decides who wrote the partition table.**
    #
    # If sector 1 still holds the ASCII "EFI PART" signature, something wrote a real GPT and we are looking
    # at our own storage answering with content it should not have.  If the sectors are zero while Windows
    # still calls the disk GPT, then the table is not on the medium at all and the problem is above it --
    # a cached or stale view.  Those are different defects and one read separates them.
    $signature = 'unreadable'
    try {
        $stream = [System.IO.File]::Open(
            "\\.\PhysicalDrive$DiskNumber", 'Open', 'Read', 'ReadWrite')
        try {
            # `[byte[]]::new()` rather than `New-Object`, because the allowlist governs command
            # invocations and a static method call is not one.
            $first = [byte[]]::new(1024)
            $read = $stream.Read($first, 0, 1024)
            $sector0 = ''
            for ($i = 0; $i -lt 8; $i++) { $sector0 += $first[$i].ToString('x2') }
            $sector1 = ''
            for ($i = 512; $i -lt 520; $i++) { $sector1 += [char]$first[$i] }
            $anyNonZero = $false
            for ($i = 0; $i -lt $read; $i++) { if ($first[$i] -ne 0) { $anyNonZero = $true; break } }
            $signature = "sector0=$sector0 sector1='$sector1' nonzero=$anyNonZero read=$read"
        } finally { $stream.Close() }
    } catch { $signature = "unreadable: $($_.Exception.Message)" }

    throw ("disk is not RAW [inspected #$DiskNumber style=$($disk.PartitionStyle) " +
           "size=$($disk.Size) boot=$($disk.IsBoot) sys=$($disk.IsSystem)] " +
           "bytes=[$signature] all=[$survey]")
}
if ($disk.IsBoot -or $disk.IsSystem) { throw 'refusing boot/system disk' }
$partitionResults = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)
$partitions = @()
foreach ($item in $partitionResults) { if ($null -ne $item -and $null -ne $item.PSObject.Properties['Number']) { $partitions += $item } }
if ($partitions.Count -ne 0) { throw 'disk already has partitions' }
$volumeResults = @(Get-Volume -DriveLetter $DriveLetter -ErrorAction SilentlyContinue)
$existingVolume = $null
foreach ($item in $volumeResults) { if ($null -ne $item -and $null -ne $item.PSObject.Properties['DriveLetter']) { $existingVolume = $item } }
if ($null -ne $existingVolume) { throw 'requested drive letter is occupied' }
$driveResults = @(Get-PSDrive -Name $DriveLetter -ErrorAction SilentlyContinue)
$existingDrive = $null
foreach ($item in $driveResults) { if ($null -ne $item -and $null -ne $item.PSObject.Properties['Name']) { $existingDrive = $item } }
if ($null -ne $existingDrive) { throw 'requested drive letter is occupied' }

$partition = $null
$mutationStarted = $false
try {
    Set-Disk -Number $DiskNumber -IsOffline $false | Out-Null
    Set-Disk -Number $DiskNumber -IsReadOnly $false | Out-Null
    $mutationStarted = $true
    Initialize-Disk -Number $DiskNumber -PartitionStyle GPT | Out-Null
    $partition = New-Partition -DiskNumber $DiskNumber -UseMaximumSize
    Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel 'MyRamDisk' -Confirm:$false -Force | Out-Null
    Set-Partition -DiskNumber $DiskNumber -PartitionNumber 1 -NewDriveLetter $DriveLetter | Out-Null
}
catch {
    # The adapter owns this exact RAW disk after Initialize-Disk succeeds.
    # Roll back only the partition created on that disk, so a mid-provision
    # failure cannot strand a formatted volume that DiskRuntime cannot own.
    if ($mutationStarted) {
        try {
            if ($null -ne $partition) {
                Remove-Partition -InputObject $partition -Confirm:$false -ErrorAction SilentlyContinue
            }
            Clear-Disk -Number $DiskNumber -RemoveData -Confirm:$false -ErrorAction SilentlyContinue
        } catch { }
    }
    throw
}

$result = [ordered]@{
    schema = 'myrd.m4.provisioning.result.v1'
    disk_number = [UInt32]$DiskNumber
    drive_letter = [string]$DriveLetter
    partition_style = 'GPT'
    filesystem = 'NTFS'
    partition_count = 1
}
Write-Output ($result | ConvertTo-Json -Compress)
