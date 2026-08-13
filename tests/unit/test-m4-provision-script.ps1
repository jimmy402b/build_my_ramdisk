$ErrorActionPreference = 'Stop'
$script = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\src\run\provision_volume.ps1')
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($script, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'provision script does not parse' }
$commands = @($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] }, $true) | ForEach-Object { $_.GetCommandName() })
$expected = @('Get-Disk','Set-Disk','Set-Disk','Initialize-Disk','New-Partition','Format-Volume')
if (($commands -join ',') -ne ($expected -join ',')) { throw "frozen command sequence changed: $($commands -join ',')" }
foreach ($dynamic in $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and $null -eq $node.GetCommandName() }, $true)) { throw 'dynamic command invocation is forbidden' }
$commandText = @($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] }, $true) | ForEach-Object { $_.Extent.Text })
$parameterSets = @(
    '-Number $DiskNumber -ErrorAction Stop', '-Number $DiskNumber -IsOffline $false -ErrorAction Stop',
    '-Number $DiskNumber -IsReadOnly $false -ErrorAction Stop', '-Number $DiskNumber -PartitionStyle GPT -ErrorAction Stop',
    '-DiskNumber $DiskNumber -UseMaximumSize -DriveLetter $DriveLetter -ErrorAction Stop',
    "-Partition `$partition -FileSystem NTFS -NewFileSystemLabel 'MyRamDisk' -Confirm:`$false -Force -ErrorAction Stop")
for ($i=0; $i -lt $parameterSets.Count; ++$i) { if ($commandText[$i] -notmatch [regex]::Escape($parameterSets[$i])) { throw "frozen parameter set changed at $i" } }
$ifs=@($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.IfStatementAst] },$true))
$getDisk=($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Get-Disk' },$true))[0]
$setDisk=($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Set-Disk' },$true))[0]
if($ifs.Count -ne 2 -or $ifs[0].Extent.StartOffset -lt $getDisk.Extent.EndOffset -or $ifs[1].Extent.StartOffset -lt $getDisk.Extent.EndOffset -or $ifs[0].Extent.EndOffset -gt $setDisk.Extent.StartOffset -or $ifs[1].Extent.EndOffset -gt $setDisk.Extent.StartOffset) { throw 'RAW/boot guards are not exactly before mutation' }
if ($script -notmatch '^param\(\[UInt32\]\$DiskNumber, \[char\]\$DriveLetter\)') { throw 'frozen parameter contract changed' }
$allow = @('Get-Disk','Set-Disk','Initialize-Disk','New-Partition','Format-Volume')
$calls = [regex]::Matches($script, '(?m)^\s*([A-Za-z]+-[A-Za-z]+)\b') | ForEach-Object { $_.Groups[1].Value }
foreach ($call in $calls) { if ($call -notin $allow) { throw "disallowed cmdlet: $call" } }
foreach ($forbidden in @('Invoke-Expression','Start-Process','Invoke-WebRequest','Invoke-RestMethod','Remove-Item','Clear-','-Path','-LiteralPath')) { if ($script -match [regex]::Escape($forbidden)) { throw "forbidden token: $forbidden" } }
foreach ($required in @("PartitionStyle -ne 'RAW'", 'IsBoot -or $disk.IsSystem', 'Initialize-Disk -Number $DiskNumber -PartitionStyle GPT', 'New-Partition -DiskNumber $DiskNumber -UseMaximumSize -DriveLetter $DriveLetter', "Format-Volume -Partition `$partition -FileSystem NTFS -NewFileSystemLabel 'MyRamDisk'")) { if ($script -notmatch [regex]::Escape($required)) { throw "missing frozen statement: $required" } }

$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\src\run\windows_volume.cpp')
$main = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\src\run\main.cpp')
foreach ($required in @('IOCTL_STORAGE_GET_DEVICE_NUMBER', 'IOCTL_DISK_GET_DRIVE_GEOMETRY_EX', 'IOCTL_DISK_GET_DRIVE_LAYOUT_EX', 'IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS', 'GetVolumeInformationW', 'GetVolumePathNamesForVolumeNameW', 'kQueryCap', 'ERROR_MORE_DATA', 'stdout_read', 'stderr_read', 'std::thread stdout_thread', 'std::thread stderr_thread', 'TerminateProcess', 'DeleteFileW', 'BCryptGenRandom', 'CREATE_NEW')) { if ($source -notmatch [regex]::Escape($required)) { throw "missing native safety evidence: $required" } }
if ($source -match 'WaitForSingleObject\([^\r\n]+\)\s*;\s*std::array<std::byte, 4096> output') { throw 'helper waits before draining output and can deadlock' }
$classes = @{
  'sentinel-wrong-device-id' = @('info.disk.path == disk_path', 'IOCTL_STORAGE_GET_DEVICE_NUMBER', 'device.DeviceNumber != info.disk.physical_drive_number')
  'exact-size-and-sectors' = @('geometry.DiskSize.QuadPart != static_cast<LONGLONG>(info.virtual_bytes)', 'geometry.Geometry.BytesPerSector != info.sector_size')
  'raw-no-partition' = @('layout.PartitionStyle == PARTITION_STYLE_RAW', 'layout.PartitionCount == 0')
  'boot-system' = @("`$disk.IsBoot -or `$disk.IsSystem")
  'zero-multiple-mixed-extents' = @('value->NumberOfDiskExtents!=1', 'ExtentClass::Mixed', 'matches.size() != 1')
  'occupied-letter' = @('drive_mask & (1u << (letter - L''A''))')
  'ambiguous-identity' = @('volume identity is ambiguous')
}
foreach ($name in $classes.Keys) { foreach ($evidence in $classes[$name]) { if (($source + $script) -notmatch [regex]::Escape($evidence)) { throw "missing fail-closed class ${name}: $evidence" } } }
if ($source -notmatch 'kQueryCap' -or $source -notmatch 'ERROR_MORE_DATA' -or $source -notmatch 'needed>paths.size\(\)') { throw 'variable-size query cap/race rejection missing' }
if ($source -notmatch 'opening enumerated volume failed' -or $source -notmatch 'volume extent query failed' -or $source -notmatch 'truncated or ambiguous volume extents') { throw 'enumeration does not fail closed on every extent/open error' }
if ($source -notmatch 'rollback_exact_mount' -or $source -notmatch 'rollback_exact_mount\(info.disk.physical_drive_number,letter\)') { throw 'post-helper exact-mount rollback missing' }
if ($source -notmatch 'owned_mount' -or $source -notmatch 'refusing to remove foreign mount' -or $source -notmatch 'exact volume extent validation failed') { throw 'mount ownership or flush extent validation missing' }
if ($source -notmatch 'fixed_ioctl_sizes_ok' -or $source -notmatch 'device_returned' -or $source -notmatch 'geometry_returned' -or $source -notmatch 'alignment_returned' -or $source -notmatch 'layout_returned') { throw 'fixed IOCTL returned-byte validation missing' }
if ($source -notmatch 'write_error=GetLastError' -or $source -notmatch 'flush_error=GetLastError' -or $source -notmatch 'close_error=GetLastError' -or $source -notmatch 'temporary script retained') { throw 'helper primary-error or retained-script semantics missing' }
if ($source -notmatch 'attribute_initialized' -or $source -notmatch 'restricted child handle list initialization failed') { throw 'attribute-list failure cleanup missing' }
if ($source -notmatch 'flush_volume.*volume_device_path' -or $source -notmatch 'flush_volume.*GENERIC_READ\|GENERIC_WRITE' -or $source -notmatch 'attribute_list_size_ok\(first_attributes,attribute_error,attributes_size\)' -or $source -notmatch 'const DWORD query_error=GetLastError\(\); if \(query_error') { throw 'volume-open, attribute-sizing, or mount-query capture safety missing' }
if ($main -notmatch 'AllocateAndInitializeSid' -or $main -notmatch 'CheckTokenMembership' -or $main -notmatch 'FreeSid\(admin\)') { throw 'administrator group membership preflight missing' }
if ($main -notmatch 'DuplicateToken\(token,SecurityIdentification' -or $main.IndexOf('DuplicateToken(') -gt $main.IndexOf('CheckTokenMembership(')) { throw 'membership does not use duplicated primary token' }
if ($main -notmatch 'OpenProcessToken\(GetCurrentProcess\(\),TOKEN_QUERY\|TOKEN_DUPLICATE,&token\)') { throw 'primary token lacks TOKEN_DUPLICATE access' }
if ($source -notmatch 'SetLastError\(ERROR_SUCCESS\); const DWORD drive_mask=GetLogicalDrives' -or $source -match 'alignment_error == ERROR_MORE_DATA') { throw 'drive-mask or stale-error safety missing' }
if ($main -notmatch 'require_elevation\(\)' -or $main.IndexOf('require_elevation()') -gt $main.IndexOf('classify_bundle')) { throw 'elevation preflight is not before bundle mutation' }
if ($source -notmatch 'TerminateProcess\(' -or $source -notmatch 'WaitForSingleObject\(process.hProcess, 5000\)' -or $source -notmatch 'termination_failed' -or $source -notmatch 'PeekNamedPipe' -or $source -notmatch 'producer_done' -or $source -notmatch 'cancel_drains') { throw 'helper forced-termination bounded cleanup missing' }
if ($main -notmatch 'auto volume = recover' -or $main -notmatch '\? myrd::find_recovered_volume' -or $main -notmatch ': myrd::provision_new_volume') { throw 'manual create/recover call graph is not exclusive' }
if ($main -notmatch 'myrd::flush_volume\(volume.value\(\)\)' -or $main -notmatch 'VolumeBarrierKind::WindowsVolumeFlushed' -or $main -notmatch 'checkpoint.generation != 1') { throw 'first flush to committed generation-one sequence missing' }
if ($main -notmatch 'GetLogicalDrives\(' -or $main -notmatch 'options.drive_letter' -or $main -notmatch 'preflight_drive_letter') { throw 'create drive-letter preflight is missing' }
