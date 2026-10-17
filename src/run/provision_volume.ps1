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
    $survey = @(Get-Disk | ForEach-Object {
        "#$($_.Number)/$($_.PartitionStyle)/$($_.Size)/boot=$($_.IsBoot)/sys=$($_.IsSystem)"
    }) -join ' '
    throw ("disk is not RAW [inspected #$DiskNumber style=$($disk.PartitionStyle) " +
           "size=$($disk.Size) boot=$($disk.IsBoot) sys=$($disk.IsSystem)] all=[$survey]")
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
