[CmdletBinding()]
param(
    [ValidateSet('Preflight', 'All')][string]$Phase = 'Preflight',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [Parameter(Mandatory = $true)][string]$ServerExecutable,
    [Parameter(Mandatory = $true)][string]$ImageExecutable,
    [Parameter(Mandatory = $true)][string]$BundleRoot,
    [Parameter(Mandatory = $true)][string]$ManifestName,
    [Parameter(Mandatory = $true)][string]$ScratchRoot,
    [Parameter(Mandatory = $true)][string]$MountDirectory,
    [Parameter(Mandatory = $true)][string]$ExpectedScratchDiskIdentityJson,
    [int]$FileCount = 4,
    [uint64]$FileSizeBytes = (16MB),
    [uint64]$VirtualBytes = (16GB),
    [switch]$SafetyAcknowledged,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'M3Harness.Common.ps1')

$script:EvidenceFull = $null
$script:Server = $null
$script:ServerStdErrTask = $null
$script:Cleanup = New-M3CleanupLedger
$script:DestructiveOperationsEmitted = $false
$script:ServerEverStarted = $false
$script:FinalReport = $null

function Assert-M3Ntfs {
    param(
        [bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    Assert-M3Condition $Condition $Message
}

function Assert-M3Executable {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-M3FullPath $Path
    Assert-M3Ntfs ([IO.File]::Exists($full)) "executable is missing: $full"
    Assert-M3NoReparsePath $full
    return $full
}

function Assert-M3ExistingDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-M3FullPath $Path
    Assert-M3Ntfs ([IO.Directory]::Exists($full)) "directory is missing: $full"
    Assert-M3NoReparsePath $full
    return $full
}

function Assert-M3BundleComponent {
    param([Parameter(Mandatory = $true)][string]$Name)
    Assert-M3Ntfs (-not [string]::IsNullOrWhiteSpace($Name)) 'manifest name is empty'
    Assert-M3Ntfs ($Name -notmatch '[\\/:*?"<>|]') 'manifest name is not a safe component'
    Assert-M3Ntfs ($Name -notmatch '^(\\|\.|\.\.)$') 'manifest name is not a safe component'
    Assert-M3Ntfs ($Name.IndexOfAny([char[]]'*?[]') -lt 0) 'manifest name contains wildcard'
}

function Get-M3CommonImageArguments {
    param([Parameter(Mandatory = $true)][string]$Command)
    return @(
        $Command, '--bundle-root', $script:BundleFull, '--manifest', $ManifestName,
        '--timeout-ms', '300000',
        '--max-virtual-bytes', [string]$VirtualBytes,
        '--max-journal-bytes', [string](512MB),
        '--max-journal-records', '131072',
        '--max-index-cache-bytes', [string](64MB),
        '--max-index-cache-entries', '65536',
        '--max-bat-cache-bytes', [string](16MB),
        '--max-bat-cache-entries', '65536',
        '--max-io-scratch-bytes', [string](8MB),
        '--max-verify-batch-bytes', [string](8MB)
    )
}

function Get-M3CommonStoreArguments {
    return @(
        '--store-soft-bytes', [string](512MB),
        '--store-hard-bytes', [string](1GB),
        '--store-low-watermark-bytes', [string](256MB),
        '--store-batch-bytes', [string](8MB),
        '--store-batch-records', '8192',
        '--store-stripes', '256',
        '--snapshot-cow-bytes', [string](64MB),
        '--snapshot-stall-ms', '250'
    )
}

function Invoke-M3ImageCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Operation
    )
    Assert-M3Ntfs (-not $WhatIf) "$Operation is not executed during WhatIf"
    $script:DestructiveOperationsEmitted = $true
    $stdout = @(& $Executable @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Operation failed with exit code ${exitCode}: $($stdout -join [Environment]::NewLine)"
    }
    return @($stdout)
}

function ConvertTo-M3ProcessArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    Assert-M3Ntfs ($Value.IndexOf([char]0) -lt 0) 'process argument contains NUL'
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Wait-M3ProcessLine {
    param(
        [Parameter(Mandatory = $true)]$Task,
        [Parameter(Mandatory = $true)][string]$Description,
        [int]$TimeoutMilliseconds = 30000
    )
    if (-not $Task.Wait($TimeoutMilliseconds)) { throw "$Description timed out" }
    $line = [string]$Task.Result
    Assert-M3Ntfs (-not [string]::IsNullOrWhiteSpace($line)) "$Description returned no output"
    return $line
}

function Start-M3Server {
    Assert-M3Ntfs (-not $WhatIf) 'server cannot start in WhatIf'
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $script:ServerFull
    $startInfo.Arguments = @(
        'serve', '--bundle-root', (ConvertTo-M3ProcessArgument $script:BundleFull),
        '--manifest', (ConvertTo-M3ProcessArgument $ManifestName),
        '--virtual-bytes', [string]$VirtualBytes
    ) -join ' '
    $startInfo.WorkingDirectory = $script:ScratchFull
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    Assert-M3Ntfs $process.Start() 'failed to start AIM server'
    $script:Server = $process
    $script:ServerEverStarted = $true
    $script:ServerStdErrTask = $process.StandardError.ReadToEndAsync()
    Add-M3CleanupEntry -Ledger $script:Cleanup -Kind 'process' `
        -Description 'stop AIM server' -RecoveryCommand 'send stop to the exact AIM server process' `
        -Action { Stop-M3Server }
    $line = Wait-M3ProcessLine $process.StandardOutput.ReadLineAsync() 'AIM server ready line'
    try { $ready = $line | ConvertFrom-Json }
    catch { throw "AIM server ready line is invalid JSON: $line" }
    Assert-M3Ntfs ([string]$ready.event -eq 'ready') 'AIM server did not report ready'
    Assert-M3Ntfs ([int]$ready.physical_drive_number -gt 0) 'AIM server returned unsafe PhysicalDrive number'
    return $ready
}

function Invoke-M3ServerCommand {
    param([Parameter(Mandatory = $true)][string]$Command)
    Assert-M3Ntfs ($null -ne $script:Server -and -not $script:Server.HasExited) 'AIM server is not running'
    $script:Server.StandardInput.WriteLine($Command)
    $script:Server.StandardInput.Flush()
    $line = Wait-M3ProcessLine $script:Server.StandardOutput.ReadLineAsync() "AIM server command '$Command'"
    try { return ($line | ConvertFrom-Json) }
    catch { throw "AIM server command returned invalid JSON: $line" }
}

function Get-M3ServerStatus {
    return Invoke-M3ServerCommand 'status'
}

function Stop-M3Server {
    if ($null -eq $script:Server) { return }
    $process = $script:Server
    if (-not $process.HasExited) {
        $process.StandardInput.WriteLine('stop')
        $process.StandardInput.Flush()
        $line = Wait-M3ProcessLine $process.StandardOutput.ReadLineAsync() 'AIM server stopped line'
        $stopped = $line | ConvertFrom-Json
        Assert-M3Ntfs ([string]$stopped.event -eq 'stopped' -and [bool]$stopped.ok) `
            'AIM server did not confirm clean stop'
        if (-not $process.WaitForExit(30000)) { throw 'AIM server did not exit after stop' }
    }
    $stderr = if ($null -ne $script:ServerStdErrTask -and $script:ServerStdErrTask.IsCompleted) {
        [string]$script:ServerStdErrTask.Result
    } else { '' }
    Assert-M3Ntfs ($process.ExitCode -eq 0) "AIM server exited with $($process.ExitCode): $stderr"
    $process.Dispose()
    $script:Server = $null
    $script:ServerStdErrTask = $null
}

function Get-M3RuntimeDisk {
    param([Parameter(Mandatory = $true)]$Ready)
    $number = [int]$Ready.physical_drive_number
    Assert-M3Ntfs ($number -gt 0) 'refusing to operate on Disk 0 or an automatic disk'
    $candidates = @(Get-M3ActualDiskCandidates | Where-Object { [int]$_.Number -eq $number })
    Assert-M3Ntfs ($candidates.Count -eq 1) "runtime PhysicalDrive$number did not map to one disk identity"
    $identity = $candidates[0]
    Assert-M3Ntfs ([uint64]$identity.SizeBytes -eq [uint64]$Ready.virtual_bytes) 'runtime disk size differs from server geometry'
    foreach ($name in @('IsBoot', 'IsSystem', 'IsPageFile', 'IsClustered')) {
        Assert-M3Ntfs (-not [bool]$identity.$name) "runtime disk is unsafe: $name"
    }
    return $identity
}

function Assert-M3RuntimeDiskUnchanged {
    param(
        [Parameter(Mandatory = $true)]$Ready,
        [Parameter(Mandatory = $true)]$Expected
    )
    $current = Get-M3RuntimeDisk $Ready
    Assert-M3Ntfs (Test-M3DiskIdentityMatch -Candidate $current -Expected $Expected) `
        'runtime disk identity changed before a destructive operation'
    return $current
}

function Save-M3Identity {
    param(
        [Parameter(Mandatory = $true)]$Identity,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $path = Join-Path $script:EvidenceFull $Name
    $Identity | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8
    return $path
}

function Assert-M3EmptyMount {
    $item = Get-Item -LiteralPath $script:MountFull -Force -ErrorAction SilentlyContinue
    if ($null -ne $item) {
        Assert-M3Ntfs (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'mount directory is a reparse point'
        Assert-M3Ntfs ([IO.Directory]::GetFileSystemEntries($script:MountFull).Count -eq 0) 'mount directory is not empty'
    } else {
        Assert-M3Ntfs (-not $WhatIf) 'mount directory must already exist for WhatIf'
        [IO.Directory]::CreateDirectory($script:MountFull) | Out-Null
    }
}

function Assert-M3MountedChildPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-M3FullPath $Path
    $mountRoot = (Get-M3FullPath $script:MountFull).TrimEnd('\')
    Assert-M3Ntfs ($full.StartsWith($mountRoot + '\', [StringComparison]::OrdinalIgnoreCase)) `
        'workload target is outside the approved mount root'
    $current = $full
    while (-not [string]::Equals($current.TrimEnd('\'), $mountRoot, [StringComparison]::OrdinalIgnoreCase)) {
        if ([IO.File]::Exists($current) -or [IO.Directory]::Exists($current)) {
            $item = Get-Item -LiteralPath $current -Force
            Assert-M3Ntfs (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
                "reparse point is forbidden below the approved mount root: $current"
        }
        $parent = [IO.Directory]::GetParent($current)
        Assert-M3Ntfs ($null -ne $parent) 'mounted child path has no parent'
        $current = $parent.FullName
    }
    return $full
}

function Get-M3ExactPartition {
    param(
        [int]$DiskNumber,
        [int]$PartitionNumber = 0
    )
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop)
    Assert-M3Ntfs ($partitions.Count -gt 0) "runtime disk $DiskNumber has no partition"
    $selected = if ($PartitionNumber -gt 0) {
        @($partitions | Where-Object { [int]$_.PartitionNumber -eq $PartitionNumber })
    } else { @($partitions | Where-Object { -not [bool]$_.IsReserved }) }
    Assert-M3Ntfs ($selected.Count -eq 1) "runtime disk $DiskNumber partition selection is ambiguous"
    Assert-M3Ntfs ([int]$selected[0].DiskNumber -eq $DiskNumber) 'partition identity changed'
    return $selected[0]
}

function Add-M3Mount {
    param([int]$DiskNumber, [int]$PartitionNumber)
    Assert-M3EmptyMount
    $cleanupAction = {
        $part = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ErrorAction SilentlyContinue
        if ($null -eq $part) { return }
        $paths = @($part.AccessPaths | Where-Object {
            [string]::Equals(([string]$_).TrimEnd('\'), $script:MountFull.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)
        })
        Assert-M3Ntfs ($paths.Count -le 1) 'exact access path appears more than once'
        if ($paths.Count -eq 1) {
            Remove-PartitionAccessPath -InputObject $part -AccessPath ([string]$paths[0]) -ErrorAction Stop
        }
        if ([IO.Directory]::Exists($script:MountFull)) {
            Assert-M3Ntfs ([IO.Directory]::GetFileSystemEntries($script:MountFull).Count -eq 0) 'mount path is not empty after removal'
        }
    }.GetNewClosure()
    Add-M3CleanupEntry -Ledger $script:Cleanup -Kind 'mount' `
        -Description 'remove exact NTFS access path' `
        -RecoveryCommand "Remove-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -AccessPath '$($script:MountFull)'" `
        -Action $cleanupAction
    Add-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber `
        -AccessPath $script:MountFull -ErrorAction Stop
    return Get-M3ExactPartition $DiskNumber $PartitionNumber
}

function Get-M3FileHashExact {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-M3FullPath $Path
    $null = Assert-M3MountedChildPath $full
    Assert-M3Ntfs ([IO.File]::Exists($full)) "workload file is missing: $full"
    return (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
}

function Get-M3FilePrefixHash {
    param([Parameter(Mandatory = $true)][string]$Path, [int]$Bytes = 4096)
    $full = Get-M3FullPath $Path
    $null = Assert-M3MountedChildPath $full
    $stream = [IO.File]::Open($full, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $buffer = New-Object byte[] $Bytes
    try { $count = $stream.Read($buffer, 0, $buffer.Length) }
    finally { $stream.Dispose() }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($buffer, 0, $count))).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Write-M3DeterministicFile {
    param([string]$Path, [uint64]$Length, [int]$Seed)
    $buffer = New-Object byte[] (1MB)
    $random = [Random]::new($Seed)
    $random.NextBytes($buffer)
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        [uint64]$remaining = $Length
        while ($remaining -gt 0) {
            $count = [int][Math]::Min([uint64]$buffer.Length, $remaining)
            $stream.Write($buffer, 0, $count)
            $remaining -= [uint64]$count
        }
        $stream.Flush($true)
    }
    finally { $stream.Dispose(); $sha.Dispose() }
    return [pscustomobject]@{ path = $Path; bytes = $Length; sha256 = (Get-M3FileHashExact $Path) }
}

function Flush-M3Volume {
    param([Parameter(Mandatory = $true)][string]$MountPath)
    $root = [IO.Path]::GetPathRoot((Get-M3FullPath $MountPath))
    $drive = $root.Substring(0, 1)
    if (-not ('MyrdM3NativeVolume' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class MyrdM3NativeVolume {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool FlushFileBuffers(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError=true)] private static extern bool CloseHandle(IntPtr handle);
    public static void Flush(string path) {
        IntPtr handle = CreateFile(path, 0xC0000000, 0x7, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (handle == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        try { if (!FlushFileBuffers(handle)) throw new Win32Exception(Marshal.GetLastWin32Error()); }
        finally { CloseHandle(handle); }
    }
}
'@ -ErrorAction Stop
    }
    [MyrdM3NativeVolume]::Flush("\\.\$drive`:")
}

function Save-M3State {
    param([Parameter(Mandatory = $true)]$State)
    $path = Join-Path $script:EvidenceFull 'm3-ntfs-state.json'
    $temporary = $path + '.tmp-' + [Guid]::NewGuid().ToString('N')
    try {
        $State | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $temporary -Encoding utf8
        $null = Get-Content -Raw -LiteralPath $temporary -Encoding utf8 | ConvertFrom-Json
        Move-Item -LiteralPath $temporary -Destination $path -Force
    }
    finally {
        if ([IO.File]::Exists($temporary)) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Invoke-M3GenerationZero {
    $manifestPath = Join-Path $script:BundleFull $ManifestName
    if ([IO.File]::Exists($manifestPath)) {
        # Reuse is allowed only for the exact named manifest. Never replace an
        # existing image as an incidental side effect of an acceptance run.
        Assert-M3NoReparsePath $manifestPath
        return [pscustomobject]@{ created = $false; manifest = $manifestPath }
    }
    Assert-M3Ntfs ([IO.Directory]::GetFileSystemEntries($script:BundleFull).Count -eq 0) `
        'bundle root is non-empty but the exact manifest is absent'
    $arguments = Get-M3CommonImageArguments 'create'
    $arguments += @(
        '--virtual-bytes', [string]$VirtualBytes,
        '--logical-sector-size', '512',
        '--physical-sector-size', '4096'
    )
    $null = Invoke-M3ImageCommand -Executable $script:ImageFull `
        -Arguments $arguments -Operation 'generation-zero create'
    Assert-M3Ntfs ([IO.File]::Exists($manifestPath)) 'generation-zero did not create manifest'
    return [pscustomobject]@{ created = $true; manifest = $manifestPath }
}

function Set-M3DeterministicFile {
    param([string]$Path, [uint64]$Length, [int]$Seed)
    if ([IO.File]::Exists($Path)) { Remove-Item -LiteralPath $Path -Force }
    return Write-M3DeterministicFile $Path $Length $Seed
}

function Invoke-M3FilesystemWorkload {
    $data = Join-Path $script:MountFull 'data'
    $renamed = Join-Path $script:MountFull 'renamed'
    $archive = Join-Path $script:MountFull 'archive'
    foreach ($directory in @($data, $renamed, $archive)) {
        $null = Assert-M3MountedChildPath $directory
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
    $files = @()
    for ($index = 0; $index -lt $FileCount; ++$index) {
        $path = Join-Path $data ('file-{0:D3}.bin' -f $index)
        $files += Set-M3DeterministicFile $path $FileSizeBytes (1000 + $index)
    }

    # Overwrite after the initial tree is durable in the filesystem cache.
    $overwritten = Set-M3DeterministicFile $files[0].path $FileSizeBytes 9001
    $files[0] = $overwritten

    # A sparse file proves first-touch holes are read as zeroes without
    # requiring a huge host-side write.
    $sparsePath = Join-Path $data 'sparse.bin'
    $null = & fsutil.exe file SetSparse $sparsePath 2>&1
    Assert-M3Ntfs ($LASTEXITCODE -eq 0) 'fsutil SetSparse failed'
    $sparseStream = [IO.File]::Open($sparsePath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $sparseLength = [uint64](64MB)
        $sparseStream.SetLength($sparseLength)
        $prefix = New-Object byte[] (1MB)
        [Random]::new(7301).NextBytes($prefix)
        $sparseStream.Write($prefix, 0, $prefix.Length)
        $sparseStream.Seek(-1MB, [IO.SeekOrigin]::End) | Out-Null
        $sparseStream.Write($prefix, 0, $prefix.Length)
        $sparseStream.Flush($true)
    }
    finally { $sparseStream.Dispose() }
    $sparseHash = Get-M3FileHashExact $sparsePath

    $renamedPath = Join-Path $renamed 'file-001-renamed.bin'
    Move-Item -LiteralPath $files[1].path -Destination $renamedPath
    $files[1] = [pscustomobject]@{ path = $renamedPath; bytes = $FileSizeBytes; sha256 = Get-M3FileHashExact $renamedPath }

    $archivePath = Join-Path $archive 'file-003-archived.bin'
    Move-Item -LiteralPath $files[3].path -Destination $archivePath
    $files[3] = [pscustomobject]@{ path = $archivePath; bytes = $FileSizeBytes; sha256 = Get-M3FileHashExact $archivePath }

    $deletePath = Get-M3FullPath $files[2].path
    $null = Assert-M3MountedChildPath $deletePath
    Remove-Item -LiteralPath $deletePath -Force
    $files[2] = $null
    $liveFiles = @($files | Where-Object { $null -ne $_ })
    foreach ($file in $liveFiles) {
        Assert-M3Ntfs ((Get-M3FileHashExact $file.path) -eq [string]$file.sha256) "workload hash mismatch: $($file.path)"
    }
    return [pscustomobject]@{
        files = $liveFiles
        sparse = [pscustomobject]@{ path = $sparsePath; bytes = [uint64](64MB); sha256 = $sparseHash }
        deleted_path = $deletePath
        seed = 9001
    }
}

function Remove-M3ExactMount {
    param([int]$DiskNumber, [int]$PartitionNumber)
    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ErrorAction SilentlyContinue
    if ($null -eq $partition) { return }
    $paths = @($partition.AccessPaths | Where-Object {
        [string]::Equals(([string]$_).TrimEnd('\'), $script:MountFull.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)
    })
    Assert-M3Ntfs ($paths.Count -le 1) 'exact mount path appears more than once during cleanup'
    if ($paths.Count -eq 1) {
        Remove-PartitionAccessPath -InputObject $partition -AccessPath ([string]$paths[0]) -ErrorAction Stop
    }
}

function Get-M3BasePathAfterCompact {
    $arguments = Get-M3CommonImageArguments 'inspect'
    $output = Invoke-M3ImageCommand -Executable $script:ImageFull -Arguments $arguments -Operation 'image inspect'
    $jsonLine = @($output | Where-Object { [string]$_ -match '^\s*\{' } | Select-Object -Last 1)
    Assert-M3Ntfs ($jsonLine.Count -eq 1) 'image inspect did not return one JSON report'
    $report = $jsonLine[0] | ConvertFrom-Json
    Assert-M3Ntfs ($null -ne $report.selected) 'image inspect selected no durable generation'
    $generation = [uint64]$report.selected.base_generation
    $name = if ($generation -eq 0) { 'base-0000.vhdx' } else { "base-g0000-c${generation}.vhdx" }
    $path = Get-M3FullPath (Join-Path $script:BundleFull $name)
    Assert-M3NoReparsePath $path
    Assert-M3Ntfs ([IO.File]::Exists($path)) "selected compact base is missing: $name"
    return [pscustomobject]@{ path = $path; generation = $generation; report = $report }
}

function Invoke-M3NativeVhdxReadOnlyVerification {
    $base = Get-M3BasePathAfterCompact
    $already = @(Get-DiskImage -ImagePath $base -ErrorAction SilentlyContinue)
    Assert-M3Ntfs ($already.Count -eq 0) 'selected VHDX is already attached; refusing double attach'
    $before = @(Get-Disk | ForEach-Object { [int]$_.Number })
    $detachAction = {
        $attached = @(Get-DiskImage -ImagePath $base -ErrorAction SilentlyContinue)
        if ($attached.Count -eq 1) { Dismount-DiskImage -ImagePath $base -ErrorAction Stop }
    }.GetNewClosure()
    Add-M3CleanupEntry -Ledger $script:Cleanup -Kind 'vhdx' `
        -Description 'detach exact compact VHDX' `
        -RecoveryCommand "Dismount-DiskImage -ImagePath '$base'" `
        -Action $detachAction
    Mount-DiskImage -ImagePath $base -Access ReadOnly -NoDriveLetter -ErrorAction Stop | Out-Null
    $newDisks = @(Get-Disk | Where-Object { $before -notcontains [int]$_.Number })
    Assert-M3Ntfs ($newDisks.Count -eq 1) 'read-only VHDX attach did not create exactly one new disk'
    $disk = $newDisks[0]
    Assert-M3Ntfs (-not [bool]$disk.IsBoot -and -not [bool]$disk.IsSystem) 'attached VHDX is unsafe'
    $partitions = @(Get-Partition -DiskNumber ([int]$disk.Number) -ErrorAction Stop)
    Assert-M3Ntfs ($partitions.Count -gt 0) 'attached VHDX has no partition'
    $volume = @(Get-Volume -Partition $partitions[0] -ErrorAction Stop)
    Assert-M3Ntfs ($volume.Count -eq 1 -and [string]$volume[0].FileSystem -eq 'NTFS') 'attached VHDX is not an NTFS volume'
    return [pscustomobject]@{ path = $base.path; generation = $base.generation; disk_number = [int]$disk.Number; filesystem = [string]$volume[0].FileSystem }
}

function Invoke-M3AllWorkload {
    Assert-M3Ntfs $SafetyAcknowledged 'real M3 NTFS execution requires -SafetyAcknowledged'
    if (-not [IO.Directory]::Exists($script:BundleFull)) {
        [IO.Directory]::CreateDirectory($script:BundleFull) | Out-Null
    }
    Assert-M3NoReparsePath $script:BundleFull
    $null = Invoke-M3GenerationZero
    $firstReady = Start-M3Server
    $firstIdentity = Get-M3RuntimeDisk $firstReady
    $null = Save-M3Identity $firstIdentity 'aim-disk-first.json'
    $firstDisk = Get-Disk -Number ([int]$firstIdentity.Number) -ErrorAction Stop
    $existing = @(Get-Partition -DiskNumber ([int]$firstIdentity.Number) -ErrorAction SilentlyContinue)
    Assert-M3Ntfs ($existing.Count -eq 0) 'generation-zero AIM disk already has partitions; refusing format'
    $null = Assert-M3RuntimeDiskUnchanged $firstReady $firstIdentity
    if ([bool]$firstDisk.IsReadOnly) { Set-Disk -Number ([int]$firstIdentity.Number) -IsReadOnly $false -ErrorAction Stop }
    Initialize-Disk -Number ([int]$firstIdentity.Number) -PartitionStyle GPT -ErrorAction Stop | Out-Null
    $null = Assert-M3RuntimeDiskUnchanged $firstReady $firstIdentity
    $partition = New-Partition -DiskNumber ([int]$firstIdentity.Number) -UseMaximumSize -AssignDriveLetter:$false -ErrorAction Stop
    Assert-M3Ntfs ([int]$partition.DiskNumber -eq [int]$firstIdentity.Number) 'created partition identity changed'
    $null = Assert-M3RuntimeDiskUnchanged $firstReady $firstIdentity
    $volume = Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel 'MYRD-M3-TEST' -Confirm:$false -Force -ErrorAction Stop
    Assert-M3Ntfs ([string]$volume.FileSystem -eq 'NTFS') 'target volume did not format as NTFS'
    $null = Add-M3Mount ([int]$firstIdentity.Number) ([int]$partition.PartitionNumber)
    $workload = Invoke-M3FilesystemWorkload
    Flush-M3Volume $script:MountFull
    $checkpoint = Invoke-M3ServerCommand 'checkpoint volume-flushed'
    Assert-M3Ntfs ([bool]$checkpoint.ok -and [string]$checkpoint.disposition -eq 'Committed') 'first filesystem checkpoint did not commit'
    Remove-M3ExactMount ([int]$firstIdentity.Number) ([int]$partition.PartitionNumber)
    Stop-M3Server

    $secondReady = Start-M3Server
    $secondIdentity = Get-M3RuntimeDisk $secondReady
    $startupStatus = Get-M3ServerStatus
    $secondPartition = Get-M3ExactPartition ([int]$secondIdentity.Number)
    $secondVolume = @(Get-Volume -Partition $secondPartition -ErrorAction Stop)
    Assert-M3Ntfs ($secondVolume.Count -eq 1 -and [string]$secondVolume[0].FileSystem -eq 'NTFS') 'reopened volume is not NTFS'
    $null = Add-M3Mount ([int]$secondIdentity.Number) ([int]$secondPartition.PartitionNumber)
    $sampleHash = Get-M3FilePrefixHash $workload.files[0].path
    $sampleStatus = Get-M3ServerStatus
    Assert-M3Ntfs (
        [uint64]$sampleStatus.storage.backing_reads_total -gt
        [uint64]$startupStatus.storage.backing_reads_total -or
        [uint64]$sampleStatus.storage.uncached_reads_total -gt
        [uint64]$startupStatus.storage.uncached_reads_total) `
        'bounded first-touch sample did not produce a backing read counter delta'
    foreach ($file in @($workload.files)) {
        Assert-M3Ntfs ((Get-M3FileHashExact $file.path) -eq [string]$file.sha256) "reopen hash mismatch: $($file.path)"
    }
    Assert-M3Ntfs ((Get-M3FileHashExact $workload.sparse.path) -eq [string]$workload.sparse.sha256) 'reopen sparse hash mismatch'
    Flush-M3Volume $script:MountFull
    $secondCheckpoint = Invoke-M3ServerCommand 'checkpoint volume-flushed'
    Assert-M3Ntfs ([bool]$secondCheckpoint.ok -and [string]$secondCheckpoint.disposition -eq 'Committed') 'reopen checkpoint did not commit'
    Remove-M3ExactMount ([int]$secondIdentity.Number) ([int]$secondPartition.PartitionNumber)
    Stop-M3Server

    $compact = Get-M3CommonImageArguments 'compact'
    $compact += @('--barrier', 'raw-test')
    $compact += Get-M3CommonStoreArguments
    $null = Invoke-M3ImageCommand -Executable $script:ImageFull -Arguments $compact -Operation 'durable view compaction'
    $native = Invoke-M3NativeVhdxReadOnlyVerification
    $state = [ordered]@{
        schema = 'myrd.m3.ntfs-state.v1'
        virtual_bytes = [uint64]$VirtualBytes
        file_count = [int]$FileCount
        first_checkpoint_generation = [uint64]$checkpoint.generation
        second_checkpoint_generation = [uint64]$secondCheckpoint.generation
        compact_generation = [uint64]$native.generation
        workload = $workload
        native_vhdx = $native
        server_reopen_verified = $true
        lazy_evidence = [pscustomobject]@{
            bounded_sample = $true
            sample_sha256 = $sampleHash
            startup_status = $startupStatus
            sample_status = $sampleStatus
            full_corpus_verified = $true
        }
        seed = 9001
    }
    Save-M3State $state
    return $state
}

function Invoke-M3PreflightWorkload {
    $server = Assert-M3Executable $ServerExecutable
    $image = Assert-M3Executable $ImageExecutable
    $scratch = Assert-M3ExistingDirectory $ScratchRoot
    $evidence = Assert-M3ExistingDirectory $EvidenceDirectory
    Assert-M3NoReparsePath $evidence
    $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    Assert-M3Ntfs (-not (Test-M3PathUnderRoot $evidence $repoRoot -AllowRoot)) 'evidence cannot be in repository'
    $bundle = Assert-M3SafeTargetPath -TargetPath $BundleRoot -ScratchRoot $scratch -WorkspaceRoot $repoRoot
    $mount = Assert-M3SafeTargetPath -TargetPath $MountDirectory -ScratchRoot $scratch -WorkspaceRoot $repoRoot
    Assert-M3BundleComponent $ManifestName
    Assert-M3Ntfs ($FileCount -ge 3) 'FileCount must be at least 3'
    Assert-M3Ntfs ($FileSizeBytes -gt 0) 'FileSizeBytes must be positive'
    Assert-M3Ntfs ($VirtualBytes -gt 0) 'VirtualBytes must be positive'
    $identity = Read-M3DiskIdentityJson $ExpectedScratchDiskIdentityJson
    if (-not $WhatIf) {
        $null = Assert-M3ExpectedDiskOwnsPath -ScratchRoot $scratch `
            -ExpectedIdentity $identity
    }
    $probe = Invoke-M3Preflight -Executable $server -ScratchRoot $scratch `
        -WorkspaceRoot $repoRoot -ExpectedDiskIdentity $identity -WhatIf:$WhatIf
    if ($probe.status -ne 'PASS') {
        throw (if ([string]::IsNullOrWhiteSpace([string]$probe.reason)) {
            'M3 preflight returned BLOCKED without a reason'
        } else { [string]$probe.reason })
    }
    $script:ServerFull = $server
    $script:ImageFull = $image
    $script:ScratchFull = $scratch
    $script:EvidenceFull = $evidence
    $script:BundleFull = $bundle
    $script:MountFull = $mount
    Assert-M3EmptyMount
    $report = [ordered]@{
        schema = 'myrd.m3.ntfs-workload.v1'
        phase = $Phase
        configuration = $Configuration
        status = 'PASS'
        terminal_verdict = 'PASS'
        what_if = [bool]$WhatIf
        server_started = $false
        destructive_operations_emitted = $false
        scratch_root = $scratch
        bundle_root = $bundle
        mount_directory = $mount
        expected_scratch_disk_identity = $identity
        preflight = $probe
        source_commit = $null
        executable_sha256 = [ordered]@{
            server = (Get-FileHash -LiteralPath $server -Algorithm SHA256).Hash
            image = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash
        }
        cleanup = [pscustomobject]@{ attempted = $false; succeeded = $true; residuals = @() }
        started_utc = [DateTime]::UtcNow.ToString('o')
        finished_utc = $null
    }
    try {
        $git = Get-Command git.exe -ErrorAction SilentlyContinue
        if ($null -ne $git) { $report.source_commit = ((& git.exe -C $repoRoot rev-parse HEAD 2>$null) -join '').Trim() }
    } catch { $report.source_commit = $null }
    return [pscustomobject]$report
}

function Write-M3NtfsReport {
    param([Parameter(Mandatory = $true)]$Report)
    $Report.finished_utc = [DateTime]::UtcNow.ToString('o')
    Write-M3Json -InputObject $Report -Path (Join-Path $script:EvidenceFull 'm3-ntfs-report.json')
}

function Get-M3CleanupEvidence {
    param([Parameter(Mandatory = $true)][AllowNull()][AllowEmptyCollection()][object[]]$Entries)
    if ($null -eq $Entries) { $Entries = @() }
    $failed = @($Entries | Where-Object { $_.Status -eq 'failed' })
    return [pscustomobject]@{
        attempted = $true
        succeeded = ($failed.Count -eq 0)
        entries = @($Entries | ForEach-Object {
            [pscustomobject]@{
                kind = $_.Kind
                description = $_.Description
                status = $_.Status
                error = $_.Error
                recovery_command = $_.RecoveryCommand
            }
        })
        residuals = @($failed | ForEach-Object { $_.Description })
    }
}

try {
    $report = Invoke-M3PreflightWorkload
    if ($Phase -eq 'All' -and -not $WhatIf) {
        $report | Add-Member -NotePropertyName workload -NotePropertyValue (Invoke-M3AllWorkload)
    }
    $report.server_started = [bool]$script:ServerEverStarted
    $report.destructive_operations_emitted = [bool]$script:DestructiveOperationsEmitted
    $script:FinalReport = $report
    Write-M3NtfsReport $report
    Write-Output ($report | ConvertTo-Json -Depth 20 -Compress)
    exit 0
}
catch {
    $message = [string]$_.Exception.Message
    $status = if ($script:DestructiveOperationsEmitted) { 'FAIL' } else { 'BLOCKED' }
    $exitCode = if ($status -eq 'FAIL') { 1 } else { 77 }
    $evidence = if ($null -ne $script:EvidenceFull) { $script:EvidenceFull } else { Get-M3FullPath $EvidenceDirectory }
    if ([IO.Directory]::Exists($evidence)) {
        $report = [ordered]@{
            schema = 'myrd.m3.ntfs-workload.v1'
            phase = $Phase
            configuration = $Configuration
            status = $status
            terminal_verdict = $status
            what_if = [bool]$WhatIf
            server_started = [bool]$script:ServerEverStarted
            destructive_operations_emitted = [bool]$script:DestructiveOperationsEmitted
            reason = $message
            cleanup = [pscustomobject]@{ attempted = $false; succeeded = $true; residuals = @() }
            started_utc = [DateTime]::UtcNow.ToString('o')
            finished_utc = [DateTime]::UtcNow.ToString('o')
        }
        $script:FinalReport = [pscustomobject]$report
        try { Write-M3Json -InputObject $script:FinalReport -Path (Join-Path $evidence 'm3-ntfs-report.json') } catch {}
    }
    [Console]::Error.WriteLine("M3 NTFS $Phase ${status}: $message")
    exit $exitCode
}
finally {
    if ($null -ne $script:Cleanup) {
        $entries = Invoke-M3CleanupLedger -Ledger $script:Cleanup
        $cleanupEvidence = Get-M3CleanupEvidence $entries
        if ($null -ne $script:FinalReport) {
            $script:FinalReport.cleanup = $cleanupEvidence
            try { Write-M3NtfsReport $script:FinalReport } catch {}
        }
        if (-not [bool]$cleanupEvidence.succeeded) {
            [Console]::Error.WriteLine('M3 NTFS cleanup failed; recovery commands are in the cleanup ledger')
            exit 1
        }
    }
}
