param([UInt32]$DiskNumber, [char]$DriveLetter)
$disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
if ($disk.PartitionStyle -ne 'RAW') { throw 'disk is not RAW' }
if ($disk.IsBoot -or $disk.IsSystem) { throw 'refusing boot/system disk' }
Set-Disk -Number $DiskNumber -IsOffline $false -ErrorAction Stop
Set-Disk -Number $DiskNumber -IsReadOnly $false -ErrorAction Stop
Initialize-Disk -Number $DiskNumber -PartitionStyle GPT -ErrorAction Stop
$partition = New-Partition -DiskNumber $DiskNumber -UseMaximumSize -DriveLetter $DriveLetter -ErrorAction Stop
Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel 'MyRamDisk' -Confirm:$false -Force -ErrorAction Stop
