[CmdletBinding()]
param(
    [ValidateSet('Validate', 'Prepare', 'Verify', 'Aggregate')]
    [string]$Action = 'Validate',
    [Parameter(Mandatory = $true)][string]$MatrixPath,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [string]$ScenarioId = '',
    [string]$ServerExecutable = '',
    [string]$ImageExecutable = '',
    [string]$BundleRoot = '',
    [string]$ManifestName = '',
    [string]$BaselineReportPath = '',
    [string]$MountDirectory = '',
    [string]$ExpectedScratchDiskIdentityJson = '',
    [string]$RepositoryRoot = '',
    [string]$SourceCommit = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [uint64]$VirtualBytes = (16GB),
    [switch]$SafetyAcknowledged,
    [string[]]$ScenarioEvidenceDirectories = @(),
    [string]$ScenarioEvidenceRoot = '',
    [string]$SimulatedEvidencePath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'M3Harness.Common.ps1')

$script:RealServer = $null
$script:RealServerStdErr = $null
$script:RealMount = $null

function Assert-M3RealCrash {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-M3RealFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [IO.Path]::GetFullPath($Path)
}

function Read-M3RealJson {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-M3RealFullPath $Path
    Assert-M3RealCrash ([IO.File]::Exists($full)) "JSON file is missing: $full"
    try {
        return [IO.File]::ReadAllText($full, [Text.UTF8Encoding]::new($false, $true)) |
            ConvertFrom-Json
    }
    catch { throw "JSON file is invalid: $full" }
}

function Write-M3RealJsonAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )
    $full = Get-M3RealFullPath $Path
    $directory = [IO.Path]::GetDirectoryName($full)
    Assert-M3RealCrash ([IO.Directory]::Exists($directory)) `
        "output directory is missing: $directory"
    Assert-M3RealCrash (-not [IO.File]::Exists($full)) `
        "refusing to overwrite evidence: $full"
    $temporary = Join-Path $directory `
        ('.' + [IO.Path]::GetFileName($full) + '.tmp-' + [Guid]::NewGuid().ToString('N'))
    try {
        $json = $Value | ConvertTo-Json -Depth 30
        $null = $json | ConvertFrom-Json
        [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $full)
    }
    finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Write-M3RealJsonDurable {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )
    $full = Get-M3RealFullPath $Path
    Assert-M3RealCrash (-not [IO.File]::Exists($full)) `
        "refusing to overwrite durable evidence: $full"
    $json = $Value | ConvertTo-Json -Depth 30
    $null = $json | ConvertFrom-Json
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    $stream = [IO.FileStream]::new(
        $full, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
        [IO.FileShare]::Read, 4096, [IO.FileOptions]::WriteThrough)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally { $stream.Dispose() }
}

function Assert-M3RealExecutable {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-M3RealFullPath $Path
    Assert-M3RealCrash ([IO.File]::Exists($full)) "executable is missing: $full"
    Assert-M3NoReparsePath $full
    Assert-M3RealCrash ([string]::Equals(
            [IO.Path]::GetExtension($full), '.exe',
            [StringComparison]::OrdinalIgnoreCase)) `
        "executable does not have an .exe suffix: $full"
    return $full
}

function Assert-M3RealComponent {
    param([Parameter(Mandatory = $true)][string]$Name)
    Assert-M3RealCrash (-not [string]::IsNullOrWhiteSpace($Name)) `
        'manifest name is empty'
    Assert-M3RealCrash ($Name -notmatch '[\\/:*?"<>|]' -and
        $Name -notin @('.', '..')) 'manifest name is not a safe component'
}

function ConvertTo-M3RealProcessArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    Assert-M3RealCrash ($Value.IndexOf([char]0) -lt 0) `
        'process argument contains NUL'
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Wait-M3RealTaskLine {
    param(
        [Parameter(Mandatory = $true)]$Task,
        [Parameter(Mandatory = $true)][string]$Description,
        [int]$TimeoutMilliseconds = 45000
    )
    Assert-M3RealCrash ($Task.Wait($TimeoutMilliseconds)) `
        "$Description timed out"
    $line = [string]$Task.Result
    Assert-M3RealCrash (-not [string]::IsNullOrWhiteSpace($line)) `
        "$Description returned no output"
    return $line
}

function ConvertFrom-M3RealCrashSignal {
    param([Parameter(Mandatory = $true)][string]$Text)
    $phaseMatch = [regex]::Match(
        $Text, '(?m)^phase=([^\r\n]+)\r?$')
    $pidMatch = [regex]::Match($Text, '(?m)^pid=(\d+)\r?$')
    Assert-M3RealCrash ($phaseMatch.Success -and $pidMatch.Success) `
        'crash phase signal is invalid'
    return [pscustomobject]@{
        phase = [string]$phaseMatch.Groups[1].Value
        pid = [int]$pidMatch.Groups[1].Value
    }
}

function Get-M3RealImageArguments {
    param([Parameter(Mandatory = $true)][string]$Command)
    return @(
        $Command, '--bundle-root', $script:RealBundle,
        '--manifest', $script:RealManifest,
        '--timeout-ms', '300000',
        '--max-virtual-bytes', [string]$VirtualBytes,
        '--max-journal-bytes', [string](2GB),
        '--max-journal-records', '524287',
        '--max-index-cache-bytes', [string](64MB),
        '--max-index-cache-entries', '16384',
        '--max-bat-cache-bytes', [string](16MB),
        '--max-bat-cache-entries', '4096',
        '--max-io-scratch-bytes', [string](8MB),
        '--max-verify-batch-bytes', [string](8MB)
    )
}

function Invoke-M3RealImage {
    param([Parameter(Mandatory = $true)][string]$Command, [switch]$Full)
    $arguments = Get-M3RealImageArguments $Command
    if ($Full) { $arguments += '--full' }
    $output = @(& $script:RealImage @arguments 2>&1)
    $exitCode = $LASTEXITCODE
    Assert-M3RealCrash ($exitCode -eq 0) `
        "image $Command failed (${exitCode}): $($output -join [Environment]::NewLine)"
    $jsonLines = @($output | Where-Object { [string]$_ -match '^\s*\{' })
    Assert-M3RealCrash ($jsonLines.Count -eq 1) `
        "image $Command did not return exactly one JSON line"
    $report = $jsonLines[0] | ConvertFrom-Json
    Assert-M3RealCrash ([bool]$report.ok) "image $Command returned failure"
    return $report
}

function Start-M3RealServer {
    param([AllowEmptyString()][string]$CrashPhase = '')
    Assert-M3RealCrash ($null -eq $script:RealServer) `
        'a real crash server is already active'
    $arguments = @(
        'serve', '--bundle-root',
        (ConvertTo-M3RealProcessArgument $script:RealBundle),
        '--manifest', (ConvertTo-M3RealProcessArgument $script:RealManifest),
        '--virtual-bytes', [string]$VirtualBytes
    )
    if (-not [string]::IsNullOrWhiteSpace($CrashPhase)) {
        $arguments += @(
            '--test-crash-phase', (ConvertTo-M3RealProcessArgument $CrashPhase),
            '--test-crash-signal', (ConvertTo-M3RealProcessArgument $script:RealSignal),
            '--test-crash-release', (ConvertTo-M3RealProcessArgument $script:RealRelease),
            '--test-crash-timeout-ms', '3600000'
        )
    }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $script:RealServerExecutable
    $start.Arguments = $arguments -join ' '
    $start.WorkingDirectory = $script:RealEvidence
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    Assert-M3RealCrash $process.Start() 'failed to start real AIM server'
    $script:RealServer = $process
    $script:RealServerStdErr = $process.StandardError.ReadToEndAsync()
    $line = Wait-M3RealTaskLine $process.StandardOutput.ReadLineAsync() `
        'real AIM server ready line'
    try { $ready = $line | ConvertFrom-Json }
    catch { throw "real AIM server ready line is invalid JSON: $line" }
    Assert-M3RealCrash ([string]$ready.event -eq 'ready') `
        'real AIM server did not report ready'
    Assert-M3RealCrash ([int]$ready.physical_drive_number -gt 1) `
        'real AIM server returned an unsafe PhysicalDrive number'
    Assert-M3RealCrash ([uint64]$ready.virtual_bytes -eq $VirtualBytes) `
        'real AIM server geometry differs from the scenario'
    return $ready
}

function Invoke-M3RealServerCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [int]$TimeoutMilliseconds = 45000
    )
    Assert-M3RealCrash ($null -ne $script:RealServer -and
        -not $script:RealServer.HasExited) 'real AIM server is not active'
    $script:RealServer.StandardInput.WriteLine($Command)
    $script:RealServer.StandardInput.Flush()
    $line = Wait-M3RealTaskLine `
        $script:RealServer.StandardOutput.ReadLineAsync() `
        "real AIM command '$Command'" $TimeoutMilliseconds
    try { return ($line | ConvertFrom-Json) }
    catch { throw "real AIM command returned invalid JSON: $line" }
}

function Stop-M3RealServer {
    if ($null -eq $script:RealServer) { return }
    $process = $script:RealServer
    if (-not $process.HasExited) {
        $process.StandardInput.WriteLine('stop')
        $process.StandardInput.Flush()
        $line = Wait-M3RealTaskLine $process.StandardOutput.ReadLineAsync() `
            'real AIM stop line' 45000
        $stopped = $line | ConvertFrom-Json
        Assert-M3RealCrash ([string]$stopped.event -eq 'stopped' -and
            [bool]$stopped.ok) 'real AIM server did not confirm stop'
        Assert-M3RealCrash ($process.WaitForExit(45000)) `
            'real AIM server did not exit after stop'
    }
    $stderr = if ($null -ne $script:RealServerStdErr -and
        $script:RealServerStdErr.IsCompleted) {
        [string]$script:RealServerStdErr.Result
    } else { '' }
    Assert-M3RealCrash ($process.ExitCode -eq 0) `
        "real AIM server exited with $($process.ExitCode): $stderr"
    $process.Dispose()
    $script:RealServer = $null
    $script:RealServerStdErr = $null
}

function Get-M3RealPartition {
    param([Parameter(Mandatory = $true)][int]$DiskNumber)
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop)
    $selected = @($partitions | Where-Object {
        $volumes = @(Get-Volume -Partition $_ -ErrorAction SilentlyContinue)
        $volumes.Count -eq 1 -and [string]$volumes[0].FileSystem -eq 'NTFS'
    })
    Assert-M3RealCrash ($selected.Count -eq 1) `
        "Disk $DiskNumber does not contain exactly one NTFS partition"
    return $selected[0]
}

function Add-M3RealMount {
    param(
        [Parameter(Mandatory = $true)][int]$DiskNumber,
        [switch]$ReadOnly
    )
    Assert-M3RealCrash ([IO.Directory]::Exists($script:RealMountPath)) `
        'mount directory is missing'
    $item = Get-Item -LiteralPath $script:RealMountPath -Force
    Assert-M3RealCrash (($item.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'mount directory is already a reparse point'
    Assert-M3RealCrash ([IO.Directory]::GetFileSystemEntries(
            $script:RealMountPath).Count -eq 0) 'mount directory is not empty'
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    foreach ($name in @('IsBoot', 'IsSystem')) {
        Assert-M3RealCrash ($null -ne $disk.PSObject.Properties[$name] -and
            -not [bool]$disk.PSObject.Properties[$name].Value) `
            "refusing unsafe runtime disk: $name"
    }
    # Storage Provider fields differ across Win11 builds. Optional safety
    # fields still fail closed when present, but absence is not a property
    # lookup exception after boot/system identity has already been proven.
    foreach ($name in @('IsPageFile', 'IsClustered')) {
        $property = $disk.PSObject.Properties[$name]
        if ($null -ne $property) {
            Assert-M3RealCrash (-not [bool]$property.Value) `
                "refusing unsafe runtime disk: $name"
        }
    }
    Assert-M3RealCrash ([uint64]$disk.Size -eq $VirtualBytes) `
        'runtime disk geometry is incorrect'
    Set-Disk -Number $DiskNumber -IsReadOnly ([bool]$ReadOnly) -ErrorAction Stop
    $partition = Get-M3RealPartition $DiskNumber
    Add-PartitionAccessPath -DiskNumber $DiskNumber `
        -PartitionNumber ([int]$partition.PartitionNumber) `
        -AccessPath $script:RealMountPath -ErrorAction Stop
    $script:RealMount = [pscustomobject]@{
        disk_number = $DiskNumber
        partition_number = [int]$partition.PartitionNumber
        read_only = [bool]$ReadOnly
    }
    return $script:RealMount
}

function Remove-M3RealMount {
    if ($null -eq $script:RealMount) { return }
    $mount = $script:RealMount
    $partition = Get-Partition -DiskNumber $mount.disk_number `
        -PartitionNumber $mount.partition_number -ErrorAction SilentlyContinue
    if ($null -ne $partition) {
        $paths = @($partition.AccessPaths | Where-Object {
            [string]::Equals(([string]$_).TrimEnd('\'),
                $script:RealMountPath.TrimEnd('\'),
                [StringComparison]::OrdinalIgnoreCase)
        })
        Assert-M3RealCrash ($paths.Count -le 1) `
            'exact crash mount appears more than once'
        if ($paths.Count -eq 1) {
            Remove-PartitionAccessPath -InputObject $partition `
                -AccessPath ([string]$paths[0]) -ErrorAction Stop
        }
    }
    if ([IO.Directory]::Exists($script:RealMountPath)) {
        $item = Get-Item -LiteralPath $script:RealMountPath -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Remove-Item -LiteralPath $script:RealMountPath -Force
            [IO.Directory]::CreateDirectory($script:RealMountPath) | Out-Null
        }
    }
    $script:RealMount = $null
}

function Convert-M3RealBaselinePath {
    param(
        [Parameter(Mandatory = $true)][string]$OriginalPath,
        [Parameter(Mandatory = $true)][string]$OriginalMount
    )
    $original = (Get-M3RealFullPath $OriginalPath)
    $root = (Get-M3RealFullPath $OriginalMount).TrimEnd('\')
    Assert-M3RealCrash ($original.StartsWith(
            $root + '\', [StringComparison]::OrdinalIgnoreCase)) `
        "baseline corpus path is outside its recorded mount: $original"
    $relative = $original.Substring($root.Length + 1)
    Assert-M3RealCrash (-not [string]::IsNullOrWhiteSpace($relative) -and
        -not [IO.Path]::IsPathRooted($relative) -and
        $relative -notmatch '(^|[\\/])\.\.([\\/]|$)') `
        "baseline corpus path is unsafe: $relative"
    return $relative
}

function Get-M3RealBaselineCorpus {
    param([Parameter(Mandatory = $true)]$Report)
    Assert-M3RealCrash ([string]$Report.status -eq 'PASS' -and
        [string]$Report.terminal_verdict -eq 'PASS') `
        'baseline NTFS report is not PASS'
    $payload = $Report.workload.workload
    Assert-M3RealCrash ($null -ne $payload) `
        'baseline NTFS corpus is missing'
    $originalMount = [string]$Report.mount_directory
    $entries = @()
    foreach ($file in @($payload.files)) {
        $entries += [pscustomobject]@{
            relative_path = Convert-M3RealBaselinePath `
                ([string]$file.path) $originalMount
            bytes = [uint64]$file.bytes
            sha256 = ([string]$file.sha256).ToUpperInvariant()
            role = 'ordinary'
        }
    }
    $entries += [pscustomobject]@{
        relative_path = Convert-M3RealBaselinePath `
            ([string]$payload.sparse.path) $originalMount
        bytes = [uint64]$payload.sparse.bytes
        sha256 = ([string]$payload.sparse.sha256).ToUpperInvariant()
        role = 'sparse'
    }
    Assert-M3RealCrash ($entries.Count -eq 4) `
        'baseline corpus must contain three ordinary files and one sparse file'
    foreach ($entry in $entries) {
        Assert-M3RealCrash ($entry.sha256 -match '^[0-9A-F]{64}$') `
            "baseline SHA-256 is invalid: $($entry.relative_path)"
    }
    $deleted = Convert-M3RealBaselinePath `
        ([string]$payload.deleted_path) $originalMount
    $generation = [uint64]$Report.workload.compact_generation
    Assert-M3RealCrash ($generation -gt 0 -and $generation -lt [uint64]::MaxValue) `
        'baseline compact generation is invalid'
    return [pscustomobject]@{
        entries = $entries
        deleted_relative_path = $deleted
        generation = $generation
    }
}

function Get-M3RealMountedPath {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    Assert-M3RealCrash (-not [IO.Path]::IsPathRooted($RelativePath) -and
        $RelativePath -notmatch '(^|[\\/])\.\.([\\/]|$)') `
        "unsafe mounted relative path: $RelativePath"
    $full = Get-M3RealFullPath (Join-Path $script:RealMountPath $RelativePath)
    $root = $script:RealMountPath.TrimEnd('\') + '\'
    Assert-M3RealCrash ($full.StartsWith(
            $root, [StringComparison]::OrdinalIgnoreCase)) `
        "mounted corpus path escaped the mount: $full"
    return $full
}

function Get-M3RealHash {
    param([Parameter(Mandatory = $true)][string]$Path)
    Assert-M3RealCrash ([IO.File]::Exists($Path)) "corpus file is missing: $Path"
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Write-M3RealDeterministicFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][uint64]$Length,
        [Parameter(Mandatory = $true)][int]$Seed
    )
    $parent = [IO.Path]::GetDirectoryName($Path)
    Assert-M3RealCrash ([IO.Directory]::Exists($parent)) `
        "deterministic file parent is missing: $parent"
    $buffer = New-Object byte[] (1MB)
    [Random]::new($Seed).NextBytes($buffer)
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Create, [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try {
        [uint64]$remaining = $Length
        while ($remaining -gt 0) {
            $count = [int][Math]::Min([uint64]$buffer.Length, $remaining)
            $stream.Write($buffer, 0, $count)
            $remaining -= [uint64]$count
        }
        $stream.Flush($true)
    }
    finally { $stream.Dispose() }
    return [pscustomobject]@{
        path = $Path
        bytes = $Length
        sha256 = Get-M3RealHash $Path
        seed = $Seed
    }
}

function Flush-M3RealVolume {
    $volumeDevice = Resolve-M3VolumeDevicePath -FilePath $script:RealMountPath
    if (-not ('MyrdM3RealCrashNativeVolume' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class MyrdM3RealCrashNativeVolume {
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
    [MyrdM3RealCrashNativeVolume]::Flush($volumeDevice)
    return $volumeDevice
}

function Wait-M3RealWriteQuiescence {
    param(
        [int]$SampleIntervalSeconds = 5,
        [int]$StableSamplesRequired = 3,
        [int]$MaximumSamples = 24
    )
    [uint64]$previous = [uint64]::MaxValue
    $stable = 0
    for ($index = 0; $index -lt $MaximumSamples; ++$index) {
        Start-Sleep -Seconds $SampleIntervalSeconds
        $status = Invoke-M3RealServerCommand 'status'
        Assert-M3RealCrash ([uint64]$status.transport.write_errors -eq 0) `
            'AIM WRITE errors occurred before crash checkpoint'
        $current = [uint64]$status.transport.write
        if ($current -eq $previous) { ++$stable }
        else { $stable = 0; $previous = $current }
        if ($stable -ge $StableSamplesRequired) { return $status }
    }
    throw 'AIM/NTFS writes did not become quiescent before crash checkpoint'
}

function Get-M3RealCorpusHash {
    param([Parameter(Mandatory = $true)][object[]]$Entries)
    $lines = @($Entries | Sort-Object relative_path | ForEach-Object {
        ([string]$_.relative_path).Replace('\', '/').ToLowerInvariant() + '|' +
            [string]$_.state + '|' + [string]$_.sha256
    })
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($lines -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Get-M3RealCorpusObservation {
    param(
        [Parameter(Mandatory = $true)]$Baseline,
        [Parameter(Mandatory = $true)][string]$MarkerRelativePath
    )
    $entries = @()
    foreach ($expected in @($Baseline.entries)) {
        $path = Get-M3RealMountedPath ([string]$expected.relative_path)
        $present = [IO.File]::Exists($path)
        $entries += [pscustomobject]@{
            relative_path = [string]$expected.relative_path
            state = if ($present) { 'present' } else { 'absent' }
            sha256 = if ($present) { Get-M3RealHash $path } else { '' }
            bytes = if ($present) { [uint64](Get-Item -LiteralPath $path).Length } else { [uint64]0 }
        }
    }
    foreach ($relative in @($Baseline.deleted_relative_path, $MarkerRelativePath)) {
        $path = Get-M3RealMountedPath ([string]$relative)
        $present = [IO.File]::Exists($path)
        $entries += [pscustomobject]@{
            relative_path = [string]$relative
            state = if ($present) { 'present' } else { 'absent' }
            sha256 = if ($present) { Get-M3RealHash $path } else { '' }
            bytes = if ($present) { [uint64](Get-Item -LiteralPath $path).Length } else { [uint64]0 }
        }
    }
    return [pscustomobject]@{
        entries = $entries
        corpus_sha256 = Get-M3RealCorpusHash $entries
    }
}

function Assert-M3RealBaselineObservation {
    param(
        [Parameter(Mandatory = $true)]$Baseline,
        [Parameter(Mandatory = $true)]$Observation,
        [Parameter(Mandatory = $true)][string]$MarkerRelativePath
    )
    foreach ($expected in @($Baseline.entries)) {
        $actual = @($Observation.entries | Where-Object {
            [string]$_.relative_path -eq [string]$expected.relative_path })
        Assert-M3RealCrash ($actual.Count -eq 1 -and
            [string]$actual[0].state -eq 'present' -and
            [string]$actual[0].sha256 -eq [string]$expected.sha256 -and
            [uint64]$actual[0].bytes -eq [uint64]$expected.bytes) `
            "baseline corpus mismatch: $($expected.relative_path)"
    }
    foreach ($relative in @($Baseline.deleted_relative_path, $MarkerRelativePath)) {
        $actual = @($Observation.entries | Where-Object {
            [string]$_.relative_path -eq [string]$relative })
        Assert-M3RealCrash ($actual.Count -eq 1 -and
            [string]$actual[0].state -eq 'absent') `
            "baseline unexpected file exists: $relative"
    }
}

function Initialize-M3RealScenarioContext {
    param(
        [Parameter(Mandatory = $true)]$Matrix,
        [Parameter(Mandatory = $true)][string]$EvidenceFull
    )
    Assert-M3RealCrash $SafetyAcknowledged `
        "$Action requires explicit -SafetyAcknowledged"
    foreach ($entry in @(
            @{ Name = 'ScenarioId'; Value = $ScenarioId },
            @{ Name = 'ServerExecutable'; Value = $ServerExecutable },
            @{ Name = 'ImageExecutable'; Value = $ImageExecutable },
            @{ Name = 'BundleRoot'; Value = $BundleRoot },
            @{ Name = 'ManifestName'; Value = $ManifestName },
            @{ Name = 'BaselineReportPath'; Value = $BaselineReportPath },
            @{ Name = 'MountDirectory'; Value = $MountDirectory },
            @{ Name = 'ExpectedScratchDiskIdentityJson'; Value = $ExpectedScratchDiskIdentityJson })) {
        Assert-M3RealCrash (-not [string]::IsNullOrWhiteSpace(
                [string]$entry.Value)) `
            "$Action requires $($entry.Name)"
    }
    Assert-M3RealCrash ($VirtualBytes -gt 0) 'VirtualBytes must be positive'
    $scenario = Get-M3RealScenario $Matrix $ScenarioId
    $repo = if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
        Get-M3RealFullPath (Join-Path $PSScriptRoot '..\..')
    } else { Get-M3RealFullPath $RepositoryRoot }
    Assert-M3RealCrash ([IO.Directory]::Exists((Join-Path $repo '.git')) -or
        [IO.File]::Exists((Join-Path $repo '.git'))) `
        "repository root is not a Git worktree: $repo"
    $commit = ((& git.exe -C $repo rev-parse HEAD 2>$null) -join '').Trim()
    Assert-M3RealCrash ($LASTEXITCODE -eq 0 -and
        $commit -match '^[0-9A-Fa-f]{40}$') 'source commit is unavailable'
    if (-not [string]::IsNullOrWhiteSpace($SourceCommit)) {
        Assert-M3RealCrash ($commit -ieq $SourceCommit) `
            'source commit differs from the required scenario commit'
    }
    & git.exe -C $repo diff --quiet --ignore-submodules --
    Assert-M3RealCrash ($LASTEXITCODE -eq 0) `
        'repository has unstaged tracked changes'
    & git.exe -C $repo diff --cached --quiet --ignore-submodules --
    Assert-M3RealCrash ($LASTEXITCODE -eq 0) `
        'repository has staged changes'

    $server = Assert-M3RealExecutable $ServerExecutable
    $image = Assert-M3RealExecutable $ImageExecutable
    $bundle = Get-M3RealFullPath $BundleRoot
    $mount = Get-M3RealFullPath $MountDirectory
    $baselinePath = Get-M3RealFullPath $BaselineReportPath
    $identityPath = Get-M3RealFullPath $ExpectedScratchDiskIdentityJson
    Assert-M3RealCrash ([IO.Directory]::Exists($bundle)) `
        "bundle root is missing: $bundle"
    Assert-M3RealCrash ([IO.Directory]::Exists($mount)) `
        "mount directory is missing: $mount"
    Assert-M3NoReparsePath $bundle
    Assert-M3NoReparsePath $baselinePath
    Assert-M3NoReparsePath $identityPath
    Assert-M3RealComponent $ManifestName
    Assert-M3RealCrash ([IO.File]::Exists((Join-Path $bundle $ManifestName))) `
        'bundle manifest is missing'
    $identity = Read-M3DiskIdentityJson $identityPath
    $null = Assert-M3ExpectedDiskOwnsPath -ScratchRoot $bundle `
        -ExpectedIdentity $identity
    $null = Assert-M3ExpectedDiskOwnsPath -ScratchRoot $EvidenceFull `
        -ExpectedIdentity $identity
    $baselineReport = Read-M3RealJson $baselinePath
    $baseline = Get-M3RealBaselineCorpus $baselineReport

    $script:RealScenario = $scenario
    $script:RealRepository = $repo
    $script:RealSourceCommit = $commit
    $script:RealEvidence = $EvidenceFull
    $script:RealServerExecutable = $server
    $script:RealImage = $image
    $script:RealBundle = $bundle
    $script:RealManifest = $ManifestName
    $script:RealMountPath = $mount
    $script:RealBaselineReportPath = $baselinePath
    $script:RealBaseline = $baseline
    $script:RealSignal = Join-Path $EvidenceFull 'crash-signal.txt'
    $script:RealRelease = Join-Path $EvidenceFull 'crash-release.txt'
    $script:RealState = Join-Path $EvidenceFull 'scenario-state.json'

    return [pscustomobject]@{
        scenario = $scenario
        source_commit = $commit
        server_sha256 = (Get-FileHash -LiteralPath $server -Algorithm SHA256).Hash
        image_sha256 = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash
        baseline_report_sha256 = (Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash
        matrix_sha256 = $Matrix.matrix_sha256
        baseline = $baseline
    }
}

function Flush-M3RealEvidenceVolume {
    $device = Resolve-M3VolumeDevicePath -FilePath $script:RealEvidence
    Assert-M3RealCrash ($null -ne ('MyrdM3RealCrashNativeVolume' -as [type])) `
        'native volume flush helper is unavailable'
    [MyrdM3RealCrashNativeVolume]::Flush($device)
    return $device
}

function Invoke-M3RealPrepare {
    param([Parameter(Mandatory = $true)]$Context)
    Assert-M3RealCrash ([IO.Directory]::GetFileSystemEntries(
            $script:RealEvidence).Count -eq 0) `
        'Prepare evidence directory must be empty'
    $residualServers = @(Get-Process -Name 'myrd-m3-aim-server' `
        -ErrorAction SilentlyContinue)
    Assert-M3RealCrash ($residualServers.Count -eq 0) `
        'a residual M3 AIM server exists before Prepare'
    $residualAim = @(Get-Disk -ErrorAction Stop | Where-Object {
        [string]$_.FriendlyName -eq 'Arsenal Virtual' })
    Assert-M3RealCrash ($residualAim.Count -eq 0) `
        'a residual Arsenal Virtual disk exists before Prepare; reboot the guest'

    $inspect = Invoke-M3RealImage 'inspect'
    Assert-M3RealCrash ([uint64]$inspect.generation -eq
        [uint64]$Context.baseline.generation) `
        'scenario clone is not at the baseline generation'
    $ready = $null
    $armed = $false
    try {
        $ready = Start-M3RealServer ([string]$Context.scenario.phase)
        $null = Add-M3RealMount ([int]$ready.physical_drive_number)
        $markerRelative = 'data\crash-marker-' + $ScenarioId + '.bin'
        $oldObservation = Get-M3RealCorpusObservation `
            $Context.baseline $markerRelative
        Assert-M3RealBaselineObservation $Context.baseline `
            $oldObservation $markerRelative

        $fileZero = @($Context.baseline.entries | Where-Object {
            [string]$_.relative_path -ieq 'data\file-000.bin' })
        $archive = @($Context.baseline.entries | Where-Object {
            [string]$_.relative_path -ieq 'archive\file-003-archived.bin' })
        Assert-M3RealCrash ($fileZero.Count -eq 1 -and $archive.Count -eq 1) `
            'baseline corpus lacks the required generation sentinels'
        $newFileZero = Write-M3RealDeterministicFile `
            (Get-M3RealMountedPath $fileZero[0].relative_path) `
            ([uint64]$fileZero[0].bytes) 42042
        $marker = Write-M3RealDeterministicFile `
            (Get-M3RealMountedPath $markerRelative) ([uint64](4MB)) 42043
        $archivePath = Get-M3RealMountedPath $archive[0].relative_path
        Assert-M3RealCrash ([IO.File]::Exists($archivePath)) `
            'archive generation sentinel disappeared before mutation'
        Remove-Item -LiteralPath $archivePath -Force

        $volumeDevice = Flush-M3RealVolume
        $quiescence = Wait-M3RealWriteQuiescence
        $newObservation = Get-M3RealCorpusObservation `
            $Context.baseline $markerRelative
        $fileZeroAfter = @($newObservation.entries | Where-Object {
            [string]$_.relative_path -ieq 'data\file-000.bin' })
        $archiveAfter = @($newObservation.entries | Where-Object {
            [string]$_.relative_path -ieq 'archive\file-003-archived.bin' })
        $markerAfter = @($newObservation.entries | Where-Object {
            [string]$_.relative_path -ieq $markerRelative })
        Assert-M3RealCrash ($fileZeroAfter.Count -eq 1 -and
            [string]$fileZeroAfter[0].sha256 -eq [string]$newFileZero.sha256 -and
            $archiveAfter.Count -eq 1 -and
            [string]$archiveAfter[0].state -eq 'absent' -and
            $markerAfter.Count -eq 1 -and
            [string]$markerAfter[0].sha256 -eq [string]$marker.sha256) `
            'new-generation sentinel mutation is incomplete'

        $state = [ordered]@{
            schema = 'myrd.m3.real-crash-state.v1'
            scenario_id = $ScenarioId
            phase = [string]$Context.scenario.phase
            interruption = [string]$Context.scenario.interruption
            allowed_recovery = [string]$Context.scenario.allowed_recovery
            configuration = $Configuration
            source_commit = $Context.source_commit
            matrix_sha256 = $Context.matrix_sha256
            server_sha256 = $Context.server_sha256
            image_sha256 = $Context.image_sha256
            baseline_report_sha256 = $Context.baseline_report_sha256
            bundle_root = $script:RealBundle
            manifest_name = $script:RealManifest
            virtual_bytes = [uint64]$VirtualBytes
            old_generation = [uint64]$Context.baseline.generation
            new_generation = [uint64]$Context.baseline.generation + 1
            marker_relative_path = $markerRelative
            exact_volume_device_path = $volumeDevice
            server_ready = $ready
            write_quiescence_status = $quiescence
            old_corpus = $oldObservation
            new_corpus = $newObservation
            mutation = [ordered]@{
                overwritten = $newFileZero
                marker = $marker
                deleted_relative_path = [string]$archive[0].relative_path
            }
            prepared_utc = [DateTime]::UtcNow.ToString('o')
        }
        Write-M3RealJsonDurable $script:RealState $state
        $null = Flush-M3RealEvidenceVolume

        $script:RealServer.StandardInput.WriteLine('checkpoint volume-flushed')
        $script:RealServer.StandardInput.Flush()
        $deadline = [DateTime]::UtcNow.AddMinutes(6)
        while (-not [IO.File]::Exists($script:RealSignal)) {
            if ($script:RealServer.HasExited) {
                throw "server exited before crash phase signal: $($script:RealServer.ExitCode)"
            }
            Assert-M3RealCrash ([DateTime]::UtcNow -lt $deadline) `
                'crash phase signal timed out'
            Start-Sleep -Milliseconds 100
        }
        $signalText = [IO.File]::ReadAllText(
            $script:RealSignal, [Text.UTF8Encoding]::new($false, $true))
        $signal = ConvertFrom-M3RealCrashSignal $signalText
        Assert-M3RealCrash ([string]$signal.phase -eq
            [string]$Context.scenario.phase) 'crash phase signal differs'
        Assert-M3RealCrash ([int]$signal.pid -eq
            [int]$script:RealServer.Id) 'crash signal PID differs from server PID'
        $armed = $true
        $armedReport = [ordered]@{
            schema = 'myrd.m3.real-crash-armed.v1'
            status = 'ARMED'
            scenario_id = $ScenarioId
            phase = [string]$Context.scenario.phase
            interruption = [string]$Context.scenario.interruption
            server_pid = [int]$script:RealServer.Id
            signal_sha256 = (Get-FileHash -LiteralPath $script:RealSignal -Algorithm SHA256).Hash
            armed_utc = [DateTime]::UtcNow.ToString('o')
        }
        Write-M3RealJsonDurable `
            (Join-Path $script:RealEvidence 'm3-real-crash-armed.json') `
            $armedReport
        $null = Flush-M3RealEvidenceVolume
        Write-Output ($armedReport | ConvertTo-Json -Depth 8 -Compress)

        Assert-M3RealCrash ($script:RealServer.WaitForExit(3600000)) `
            'interruption was not delivered within one hour'
        $interrupted = [ordered]@{
            schema = 'myrd.m3.real-crash-interruption.v1'
            status = 'OBSERVED'
            scenario_id = $ScenarioId
            interruption = [string]$Context.scenario.interruption
            server_pid = [int]$signal.pid
            server_exit_code = [int]$script:RealServer.ExitCode
            orphan_device_expected_until_reboot = $true
            observed_utc = [DateTime]::UtcNow.ToString('o')
        }
        Write-M3RealJsonDurable `
            (Join-Path $script:RealEvidence 'm3-real-crash-interruption.json') `
            $interrupted
        $script:RealServer.Dispose()
        $script:RealServer = $null
        $script:RealServerStdErr = $null
        # Do not touch the stale mount or orphan AIM device. A guest reboot is
        # the mandatory isolation boundary before Verify.
        return $interrupted
    }
    finally {
        if (-not $armed) {
            try { Remove-M3RealMount } catch {}
            try { Stop-M3RealServer } catch {}
        }
    }
}

function Test-M3RealObservationEqual {
    param(
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)]$Expected
    )
    if ([string]$Actual.corpus_sha256 -ne [string]$Expected.corpus_sha256) {
        return $false
    }
    $actualEntries = @($Actual.entries)
    $expectedEntries = @($Expected.entries)
    if ($actualEntries.Count -ne $expectedEntries.Count) { return $false }
    foreach ($entry in $expectedEntries) {
        $match = @($actualEntries | Where-Object {
            [string]$_.relative_path -eq [string]$entry.relative_path })
        if ($match.Count -ne 1 -or
            [string]$match[0].state -ne [string]$entry.state -or
            [string]$match[0].sha256 -ne [string]$entry.sha256 -or
            [uint64]$match[0].bytes -ne [uint64]$entry.bytes) {
            return $false
        }
    }
    return $true
}

function Read-M3RealFilePrefixHash {
    param([Parameter(Mandatory = $true)][string]$Path, [int]$Bytes = 4096)
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    $buffer = New-Object byte[] $Bytes
    try { $count = $stream.Read($buffer, 0, $buffer.Length) }
    finally { $stream.Dispose() }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
            $sha.ComputeHash($buffer, 0, $count))).Replace('-', '')
    }
    finally { $sha.Dispose() }
}

function Invoke-M3RealVerify {
    param([Parameter(Mandatory = $true)]$Context)
    Assert-M3RealCrash ([IO.File]::Exists($script:RealState)) `
        'scenario state is missing after interruption'
    $armedPath = Join-Path $script:RealEvidence 'm3-real-crash-armed.json'
    Assert-M3RealCrash ([IO.File]::Exists($armedPath)) `
        'armed evidence is missing after interruption'
    $state = Read-M3RealJson $script:RealState
    $armed = Read-M3RealJson $armedPath
    foreach ($pair in @(
            @{ Name = 'scenario'; Actual = [string]$state.scenario_id; Expected = $ScenarioId },
            @{ Name = 'phase'; Actual = [string]$state.phase; Expected = [string]$Context.scenario.phase },
            @{ Name = 'interruption'; Actual = [string]$state.interruption; Expected = [string]$Context.scenario.interruption },
            @{ Name = 'allowed recovery'; Actual = [string]$state.allowed_recovery; Expected = [string]$Context.scenario.allowed_recovery },
            @{ Name = 'source commit'; Actual = [string]$state.source_commit; Expected = [string]$Context.source_commit },
            @{ Name = 'matrix hash'; Actual = [string]$state.matrix_sha256; Expected = [string]$Context.matrix_sha256 },
            @{ Name = 'server hash'; Actual = [string]$state.server_sha256; Expected = [string]$Context.server_sha256 },
            @{ Name = 'image hash'; Actual = [string]$state.image_sha256; Expected = [string]$Context.image_sha256 },
            @{ Name = 'baseline report hash'; Actual = [string]$state.baseline_report_sha256; Expected = [string]$Context.baseline_report_sha256 })) {
        Assert-M3RealCrash ([string]::Equals(
                [string]$pair.Actual, [string]$pair.Expected,
                [StringComparison]::OrdinalIgnoreCase)) `
            "scenario state $($pair.Name) differs"
    }
    Assert-M3RealCrash ([string]$armed.status -eq 'ARMED' -and
        [string]$armed.scenario_id -eq $ScenarioId -and
        [string]$armed.phase -eq [string]$Context.scenario.phase) `
        'armed evidence does not match scenario state'
    if ([string]$Context.scenario.interruption -eq 'process-kill') {
        $interruptionPath = Join-Path $script:RealEvidence `
            'm3-real-crash-interruption.json'
        Assert-M3RealCrash ([IO.File]::Exists($interruptionPath)) `
            'process-kill interruption evidence is missing'
        $interruption = Read-M3RealJson $interruptionPath
        Assert-M3RealCrash ([string]$interruption.status -eq 'OBSERVED' -and
            [string]$interruption.scenario_id -eq $ScenarioId -and
            [int]$interruption.server_pid -eq [int]$armed.server_pid) `
            'process-kill interruption evidence differs'
    } else {
        $witnessPath = Join-Path $script:RealEvidence `
            'm3-real-host-interruption.json'
        Assert-M3RealCrash ([IO.File]::Exists($witnessPath)) `
            'controlled-power-cut host witness is missing'
        $witness = Read-M3RealJson $witnessPath
        Assert-M3RealCrash ([string]$witness.schema -eq
            'myrd.m3.controlled-power-cut-witness.v1' -and
            [string]$witness.status -eq 'OBSERVED' -and
            [string]$witness.scenario_id -eq $ScenarioId -and
            [string]$witness.interruption -eq 'controlled-power-cut' -and
            [int]$witness.vmrun_stop_exit_code -eq 0 -and
            [int]$witness.vmrun_start_exit_code -eq 0 -and
            [string]$witness.boot_id_before -ne [string]$witness.boot_id_after) `
            'controlled-power-cut host witness is invalid'
    }

    Assert-M3RealCrash (@(Get-Process -Name 'myrd-m3-aim-server' `
            -ErrorAction SilentlyContinue).Count -eq 0) `
        'a residual M3 AIM server exists after reboot'
    Assert-M3RealCrash (@(Get-Disk -ErrorAction Stop | Where-Object {
            [string]$_.FriendlyName -eq 'Arsenal Virtual' }).Count -eq 0) `
        'an orphan Arsenal Virtual disk remains after reboot'

    $inspect = Invoke-M3RealImage 'inspect'
    $fullVerify = Invoke-M3RealImage 'verify' -Full
    $selectedGeneration = [uint64]$inspect.generation
    Assert-M3RealCrash ([uint64]$fullVerify.generation -eq $selectedGeneration) `
        'inspect and full verify selected different generations'
    $oldGeneration = [uint64]$state.old_generation
    $newGeneration = [uint64]$state.new_generation
    $allowedGenerations = switch ([string]$state.allowed_recovery) {
        'old' { @($oldGeneration) }
        'new' { @($newGeneration) }
        'old_or_new' { @($oldGeneration, $newGeneration) }
        default { throw 'scenario state has invalid recovery set' }
    }
    Assert-M3RealCrash ($allowedGenerations -contains $selectedGeneration) `
        "selected generation $selectedGeneration is outside the allowed recovery set"

    $ready = $null
    $cleanupErrors = [Collections.ArrayList]::new()
    try {
        $ready = Start-M3RealServer
        $startupStatus = Invoke-M3RealServerCommand 'status'
        Assert-M3RealCrash ([uint64]$startupStatus.storage.backing_generation -eq
            $selectedGeneration) 'runtime backing generation differs from image verify'
        Assert-M3RealCrash ([uint64]$startupStatus.storage.committed_bytes -lt
            [uint64](128MB)) 'startup committed memory exceeds the M3-V7 ceiling'

        $null = Add-M3RealMount ([int]$ready.physical_drive_number) -ReadOnly
        $preSampleStatus = Invoke-M3RealServerCommand 'status'
        $sampleRelative = 'renamed\file-001-renamed.bin'
        $samplePath = Get-M3RealMountedPath $sampleRelative
        Assert-M3RealCrash ([IO.File]::Exists($samplePath)) `
            'lazy-load sample file is missing'
        $sampleHash = Read-M3RealFilePrefixHash $samplePath
        $postSampleStatus = Invoke-M3RealServerCommand 'status'
        Assert-M3RealCrash (
            [uint64]$postSampleStatus.storage.backing_reads_total -gt
                [uint64]$preSampleStatus.storage.backing_reads_total -or
            [uint64]$postSampleStatus.storage.uncached_reads_total -gt
                [uint64]$preSampleStatus.storage.uncached_reads_total) `
            'first random read did not produce a backing-read counter delta'

        $observation = Get-M3RealCorpusObservation `
            $Context.baseline ([string]$state.marker_relative_path)
        $isOld = Test-M3RealObservationEqual $observation $state.old_corpus
        $isNew = Test-M3RealObservationEqual $observation $state.new_corpus
        Assert-M3RealCrash ($isOld -xor $isNew) `
            'recovered NTFS corpus is mixed, corrupt, or ambiguous'
        $classification = if ($isOld) { 'old' } else { 'new' }
        Assert-M3RealCrash (
            ($classification -eq 'old' -and $selectedGeneration -eq $oldGeneration) -or
            ($classification -eq 'new' -and $selectedGeneration -eq $newGeneration)) `
            'NTFS corpus classification differs from selected generation'

        Remove-M3RealMount
        $null = Add-M3RealMount ([int]$ready.physical_drive_number)
        $partition = Get-M3RealPartition ([int]$ready.physical_drive_number)
        $volumes = @(Get-Volume -Partition $partition -ErrorAction Stop)
        Assert-M3RealCrash ($volumes.Count -eq 1 -and
            [string]$volumes[0].FileSystem -eq 'NTFS') `
            'recovered filesystem is not exactly one NTFS volume'
        $repairOutput = @($volumes[0] | Repair-Volume -Scan `
            -Confirm:$false -ErrorAction Stop | ForEach-Object { [string]$_ })
        Remove-M3RealMount
        Stop-M3RealServer
        $ready = $null

        Start-Sleep -Seconds 2
        Assert-M3RealCrash (@(Get-Process -Name 'myrd-m3-aim-server' `
                -ErrorAction SilentlyContinue).Count -eq 0) `
            'M3 AIM server leaked after Verify cleanup'
        Assert-M3RealCrash (@(Get-Disk -ErrorAction Stop | Where-Object {
                [string]$_.FriendlyName -eq 'Arsenal Virtual' }).Count -eq 0) `
            'Arsenal Virtual disk leaked after Verify cleanup'

        $report = [ordered]@{
            schema = 'myrd.m3.real-crash-report.v1'
            status = 'PASS'
            terminal_verdict = 'PASS'
            scenario_id = $ScenarioId
            phase = [string]$state.phase
            interruption = [string]$state.interruption
            allowed_recovery = [string]$state.allowed_recovery
            recovered_classification = $classification
            selected_generation = $selectedGeneration
            old_generation = $oldGeneration
            new_generation = $newGeneration
            source_commit = $Context.source_commit
            matrix_sha256 = $Context.matrix_sha256
            server_sha256 = $Context.server_sha256
            image_sha256 = $Context.image_sha256
            image_inspect = $inspect
            image_full_verify = $fullVerify
            read_only_first = $true
            full_corpus_sha256 = [string]$observation.corpus_sha256
            full_corpus = $observation
            filesystem = [ordered]@{
                type = 'NTFS'
                scan_succeeded = $true
                repair_output = $repairOutput
                reformatted = $false
            }
            lazy_load = [ordered]@{
                startup_status = $startupStatus
                pre_sample_status = $preSampleStatus
                post_sample_status = $postSampleStatus
                sample_relative_path = $sampleRelative
                sample_prefix_sha256 = $sampleHash
            }
            cleanup = [ordered]@{
                reboot_isolation_required = $true
                residual_server_count = 0
                residual_aim_disk_count = 0
                succeeded = $true
                errors = @()
            }
            verified_utc = [DateTime]::UtcNow.ToString('o')
        }
        Write-M3RealJsonAtomic `
            (Join-Path $script:RealEvidence 'm3-real-crash-report.json') `
            $report
        return $report
    }
    finally {
        if ($null -ne $script:RealMount) {
            try { Remove-M3RealMount } catch { [void]$cleanupErrors.Add($_.Exception.Message) }
        }
        if ($null -ne $script:RealServer) {
            try { Stop-M3RealServer } catch { [void]$cleanupErrors.Add($_.Exception.Message) }
        }
        if ($cleanupErrors.Count -gt 0) {
            [Console]::Error.WriteLine(
                "real crash Verify cleanup errors: $($cleanupErrors -join '; ')")
        }
    }
}

function Invoke-M3RealAggregate {
    param(
        [Parameter(Mandatory = $true)]$Matrix,
        [Parameter(Mandatory = $true)][string]$EvidenceFull
    )
    Assert-M3RealCrash ([IO.Directory]::GetFileSystemEntries(
            $EvidenceFull).Count -eq 0) `
        'Aggregate evidence directory must be empty'
    $scenarioDirectories = @($ScenarioEvidenceDirectories)
    if ($scenarioDirectories.Count -eq 0 -and
        -not [string]::IsNullOrWhiteSpace($ScenarioEvidenceRoot)) {
        $scenarioRoot = Get-M3RealFullPath $ScenarioEvidenceRoot
        Assert-M3RealCrash ([IO.Directory]::Exists($scenarioRoot)) `
            'ScenarioEvidenceRoot is missing'
        $scenarioDirectories = @(Get-ChildItem -LiteralPath $scenarioRoot `
            -Directory -Force | Where-Object {
                [IO.File]::Exists((Join-Path $_.FullName `
                    'm3-real-crash-report.json'))
            } | ForEach-Object { $_.FullName })
    }
    Assert-M3RealCrash ($scenarioDirectories.Count -eq
        $Matrix.scenarios.Count) `
        'Aggregate requires exactly one directory per real crash scenario'
    Assert-M3RealCrash (-not [string]::IsNullOrWhiteSpace(
            $SimulatedEvidencePath)) `
        'Aggregate requires SimulatedEvidencePath'
    $simulatedFull = Get-M3RealFullPath $SimulatedEvidencePath
    $simulated = Read-M3RealJson $simulatedFull
    Assert-M3RealCrash ([string]$simulated.schema -eq
        'myrd.m3.crash-evidence.v1' -and
        [string]$simulated.status -eq 'PASS' -and
        [string]$simulated.terminal_verdict -eq 'PASS' -and
        [string]$simulated.scope -eq 'simulated' -and
        [uint64]$simulated.execution.schedule_count -ge 10000 -and
        [string]$simulated.execution.summary -eq '6/6 passed' -and
        [bool]$simulated.coverage.simulated_file_clone -and
        [bool]$simulated.coverage.fresh_process -and
        [bool]$simulated.cleanup.succeeded -and
        @($simulated.cleanup.residuals).Count -eq 0) `
        'simulated crash evidence is incomplete or invalid'

    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $summaries = @()
    $commonSource = $null
    $commonServerHash = $null
    $commonImageHash = $null
    foreach ($directoryValue in $scenarioDirectories) {
        $directory = Get-M3RealFullPath $directoryValue
        Assert-M3RealCrash ([IO.Directory]::Exists($directory)) `
            "scenario evidence directory is missing: $directory"
        Assert-M3NoReparsePath $directory
        $reportPath = Join-Path $directory 'm3-real-crash-report.json'
        $report = Read-M3RealJson $reportPath
        Assert-M3RealCrash ([string]$report.schema -eq
            'myrd.m3.real-crash-report.v1' -and
            [string]$report.status -eq 'PASS' -and
            [string]$report.terminal_verdict -eq 'PASS') `
            "real crash report is not PASS: $reportPath"
        $id = [string]$report.scenario_id
        Assert-M3RealCrash ($seen.Add($id)) `
            "duplicate real crash report: $id"
        $scenario = Get-M3RealScenario $Matrix $id
        foreach ($pair in @(
                @{ Name = 'phase'; Actual = [string]$report.phase; Expected = [string]$scenario.phase },
                @{ Name = 'interruption'; Actual = [string]$report.interruption; Expected = [string]$scenario.interruption },
                @{ Name = 'allowed recovery'; Actual = [string]$report.allowed_recovery; Expected = [string]$scenario.allowed_recovery },
                @{ Name = 'matrix hash'; Actual = [string]$report.matrix_sha256; Expected = [string]$Matrix.matrix_sha256 })) {
            Assert-M3RealCrash ([string]::Equals(
                    $pair.Actual, $pair.Expected,
                    [StringComparison]::OrdinalIgnoreCase)) `
                "real crash $id $($pair.Name) differs"
        }
        Assert-M3RealCrash ([string]$report.source_commit -match
            '^[0-9A-Fa-f]{40}$') "real crash source commit is invalid: $id"
        foreach ($hashName in @('server_sha256', 'image_sha256',
                'full_corpus_sha256')) {
            Assert-M3RealCrash ([string]$report.$hashName -match
                '^[0-9A-Fa-f]{64}$') `
                "real crash $hashName is invalid: $id"
        }
        if ($null -eq $commonSource) {
            $commonSource = [string]$report.source_commit
            $commonServerHash = [string]$report.server_sha256
            $commonImageHash = [string]$report.image_sha256
        } else {
            Assert-M3RealCrash ([string]$report.source_commit -ieq $commonSource -and
                [string]$report.server_sha256 -ieq $commonServerHash -and
                [string]$report.image_sha256 -ieq $commonImageHash) `
                "real crash binary provenance drifted: $id"
        }
        $classification = [string]$report.recovered_classification
        $selected = [uint64]$report.selected_generation
        $old = [uint64]$report.old_generation
        $new = [uint64]$report.new_generation
        Assert-M3RealCrash ($new -eq $old + 1 -and
            (($classification -eq 'old' -and $selected -eq $old) -or
             ($classification -eq 'new' -and $selected -eq $new))) `
            "real crash generation/corpus classification is invalid: $id"
        Assert-M3RealCrash (
            ([string]$scenario.allowed_recovery -eq 'old' -and
                $classification -eq 'old') -or
            ([string]$scenario.allowed_recovery -eq 'new' -and
                $classification -eq 'new') -or
            ([string]$scenario.allowed_recovery -eq 'old_or_new' -and
                $classification -in @('old', 'new'))) `
            "real crash recovered outside the allowed generation set: $id"
        Assert-M3RealCrash ([bool]$report.image_inspect.ok -and
            [uint64]$report.image_inspect.generation -eq $selected -and
            [bool]$report.image_full_verify.ok -and
            [uint64]$report.image_full_verify.generation -eq $selected) `
            "real crash image verification is incomplete: $id"
        Assert-M3RealCrash ([bool]$report.read_only_first -and
            [string]$report.filesystem.type -eq 'NTFS' -and
            [bool]$report.filesystem.scan_succeeded -and
            -not [bool]$report.filesystem.reformatted) `
            "real crash NTFS recovery gate is incomplete: $id"
        $pre = $report.lazy_load.pre_sample_status.storage
        $post = $report.lazy_load.post_sample_status.storage
        Assert-M3RealCrash ([uint64]$report.lazy_load.startup_status.storage.committed_bytes -lt
            [uint64](128MB) -and
            ([uint64]$post.backing_reads_total -gt [uint64]$pre.backing_reads_total -or
             [uint64]$post.uncached_reads_total -gt [uint64]$pre.uncached_reads_total)) `
            "real crash lazy-load evidence is incomplete: $id"
        Assert-M3RealCrash ([bool]$report.cleanup.reboot_isolation_required -and
            [bool]$report.cleanup.succeeded -and
            [int]$report.cleanup.residual_server_count -eq 0 -and
            [int]$report.cleanup.residual_aim_disk_count -eq 0 -and
            @($report.cleanup.errors).Count -eq 0) `
            "real crash cleanup evidence is incomplete: $id"
        $summaries += [pscustomobject]@{
            scenario_id = $id
            phase = [string]$report.phase
            interruption = [string]$report.interruption
            allowed_recovery = [string]$report.allowed_recovery
            recovered_classification = $classification
            selected_generation = $selected
            report_sha256 = (Get-FileHash -LiteralPath $reportPath -Algorithm SHA256).Hash
        }
    }
    foreach ($scenario in @($Matrix.scenarios)) {
        Assert-M3RealCrash ($seen.Contains([string]$scenario.id)) `
            "required real crash scenario is missing: $($scenario.id)"
    }
    Assert-M3RealCrash ([string]$simulated.source_commit -ieq $commonSource) `
        'simulated and real crash evidence source commits differ'
    $processKillCount = @($summaries | Where-Object {
        $_.interruption -eq 'process-kill' }).Count
    $powerCutCount = @($summaries | Where-Object {
        $_.interruption -eq 'controlled-power-cut' }).Count
    Assert-M3RealCrash ($processKillCount -eq 8 -and $powerCutCount -eq 2) `
        'real interruption coverage counts are incomplete'

    $aggregate = [ordered]@{
        schema = 'myrd.m3.real-crash-aggregate.v1'
        status = 'PASS'
        terminal_verdict = 'PASS'
        task42_complete = $true
        milestone_eligible = $true
        source_commit = $commonSource
        server_sha256 = $commonServerHash
        image_sha256 = $commonImageHash
        matrix_sha256 = $Matrix.matrix_sha256
        simulated_evidence_sha256 = (Get-FileHash -LiteralPath $simulatedFull -Algorithm SHA256).Hash
        simulated_schedule_count = [uint64]$simulated.execution.schedule_count
        real_scenario_count = $summaries.Count
        process_kill_count = $processKillCount
        controlled_power_cut_count = $powerCutCount
        scenarios = @($summaries | Sort-Object scenario_id)
        guarantees = [ordered]@{
            latest_complete_confirmed_checkpoint = $true
            mixed_generation_rejected = $true
            post_checkpoint_uncommitted_changes_may_be_lost = $true
            read_only_recovery_first = $true
            ntfs_reformat_forbidden = $true
            per_scenario_guest_reboot_isolation = $true
        }
        finished_utc = [DateTime]::UtcNow.ToString('o')
    }
    Write-M3RealJsonAtomic `
        (Join-Path $EvidenceFull 'm3-real-crash-aggregate.json') $aggregate
    return $aggregate
}

function Get-M3RealMatrix {
    param([Parameter(Mandatory = $true)][string]$Path)
    $matrix = Read-M3RealJson $Path
    Assert-M3RealCrash ([string]$matrix.schema -eq 'myrd.m3.real-crash-matrix.v1') `
        'real crash matrix schema is invalid'
    $scenarios = @($matrix.scenarios)
    Assert-M3RealCrash ($scenarios.Count -eq 10) `
        'real crash matrix must contain exactly ten scenarios'
    $ids = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $phaseInterruptionSet = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($scenario in $scenarios) {
        foreach ($name in @('id', 'phase', 'interruption', 'allowed_recovery')) {
            Assert-M3RealCrash ($null -ne $scenario.PSObject.Properties[$name]) `
                "real crash scenario property is missing: $name"
        }
        $id = [string]$scenario.id
        $phase = [string]$scenario.phase
        Assert-M3RealCrash ($ids.Add($id)) "duplicate real crash scenario: $id"
        $phaseInterruption = $phase + '|' + [string]$scenario.interruption
        Assert-M3RealCrash ($phaseInterruptionSet.Add($phaseInterruption)) `
            "duplicate real crash phase/interruption: $phaseInterruption"
        Assert-M3RealCrash ($id -match '^[a-z0-9-]+$') "unsafe scenario ID: $id"
        Assert-M3RealCrash ($phase -in @(
                'checkpoint.prepare.before',
                'journal.flush.before', 'journal.flush.after',
                'manifest.slot_write.before', 'manifest.slot_write.after',
                'manifest.flush.after', 'manifest.readback.after',
                'reconcile.before_recover_view',
                'reconcile.before_report_success')) `
            "real crash phase is not a supported test seam: $phase"
        Assert-M3RealCrash ([string]$scenario.interruption -in @(
                'process-kill', 'controlled-power-cut')) `
            "real crash interruption type is invalid: $id"
        Assert-M3RealCrash ([string]$scenario.allowed_recovery -in @(
                'old', 'new', 'old_or_new')) `
            "real crash recovery set is invalid: $id"
    }
    $old = @($scenarios | Where-Object { [string]$_.allowed_recovery -eq 'old' })
    $oldOrNew = @($scenarios | Where-Object { [string]$_.allowed_recovery -eq 'old_or_new' })
    $new = @($scenarios | Where-Object { [string]$_.allowed_recovery -eq 'new' })
    $processKill = @($scenarios | Where-Object { [string]$_.interruption -eq 'process-kill' })
    $powerCut = @($scenarios | Where-Object { [string]$_.interruption -eq 'controlled-power-cut' })
    Assert-M3RealCrash ($old.Count -eq 4) 'real crash old coverage must be four'
    Assert-M3RealCrash ($oldOrNew.Count -eq 1) 'real crash old-or-new coverage must be one'
    Assert-M3RealCrash ($new.Count -eq 5) 'real crash new coverage must be five'
    Assert-M3RealCrash ($processKill.Count -eq 8) 'real crash process-kill coverage must be eight'
    Assert-M3RealCrash ($powerCut.Count -eq 2) 'real crash power-cut coverage must be two'
    return [pscustomobject]@{
        schema = [string]$matrix.schema
        scenarios = $scenarios
        matrix_sha256 = (Get-FileHash -LiteralPath (Get-M3RealFullPath $Path) -Algorithm SHA256).Hash
    }
}

function Get-M3RealScenario {
    param(
        [Parameter(Mandatory = $true)]$Matrix,
        [Parameter(Mandatory = $true)][string]$Id
    )
    $matches = @($Matrix.scenarios | Where-Object { [string]$_.id -eq $Id })
    Assert-M3RealCrash ($matches.Count -eq 1) "unknown real crash scenario: $Id"
    return $matches[0]
}

try {
    $matrix = Get-M3RealMatrix $MatrixPath
    $evidenceFull = Get-M3RealFullPath $EvidenceDirectory
    Assert-M3RealCrash ([IO.Directory]::Exists($evidenceFull)) `
        "EvidenceDirectory must already exist: $evidenceFull"

    if ($Action -eq 'Validate') {
        Assert-M3RealCrash ([IO.Directory]::GetFileSystemEntries($evidenceFull).Count -eq 0) `
            'validation evidence directory must be empty'
        $processKillCount = @($matrix.scenarios | Where-Object {
            [string]$_.interruption -eq 'process-kill' }).Count
        $powerCutCount = @($matrix.scenarios | Where-Object {
            [string]$_.interruption -eq 'controlled-power-cut' }).Count
        $oldCount = @($matrix.scenarios | Where-Object {
            [string]$_.allowed_recovery -eq 'old' }).Count
        $oldOrNewCount = @($matrix.scenarios | Where-Object {
            [string]$_.allowed_recovery -eq 'old_or_new' }).Count
        $newCount = @($matrix.scenarios | Where-Object {
            [string]$_.allowed_recovery -eq 'new' }).Count
        $crlfSignal = ConvertFrom-M3RealCrashSignal `
            "phase=journal.flush.before`r`npid=1234`r`n"
        $lfSignal = ConvertFrom-M3RealCrashSignal `
            "phase=journal.flush.before`npid=1234`n"
        Assert-M3RealCrash ($crlfSignal.phase -eq $lfSignal.phase -and
            $crlfSignal.pid -eq $lfSignal.pid) `
            'crash signal CRLF/LF parsing differs'
        $report = [ordered]@{
            schema = 'myrd.m3.real-crash-validation.v1'
            status = 'PASS'
            terminal_verdict = 'PASS'
            scenario_count = $matrix.scenarios.Count
            process_kill_count = $processKillCount
            controlled_power_cut_count = $powerCutCount
            old_count = $oldCount
            old_or_new_count = $oldOrNewCount
            new_count = $newCount
            signal_formats_validated = @('CRLF', 'LF')
            matrix_sha256 = $matrix.matrix_sha256
            started_utc = [DateTime]::UtcNow.ToString('o')
            finished_utc = [DateTime]::UtcNow.ToString('o')
        }
        Write-M3RealJsonAtomic (Join-Path $evidenceFull 'm3-real-crash-validation.json') $report
        Write-Output ($report | ConvertTo-Json -Depth 12 -Compress)
        exit 0
    }

    if ($Action -eq 'Aggregate') {
        $aggregate = Invoke-M3RealAggregate $matrix $evidenceFull
        Write-Output ($aggregate | ConvertTo-Json -Depth 20 -Compress)
        exit 0
    }

    Assert-M3NoReparsePath $evidenceFull
    $context = Initialize-M3RealScenarioContext $matrix $evidenceFull
    if ($Action -eq 'Prepare') {
        $prepared = Invoke-M3RealPrepare $context
        Write-Output ($prepared | ConvertTo-Json -Depth 12 -Compress)
        exit 0
    }
    if ($Action -eq 'Verify') {
        $verified = Invoke-M3RealVerify $context
        Write-Output ($verified | ConvertTo-Json -Depth 20 -Compress)
        exit 0
    }

    throw "Action '$Action' is not implemented yet"
}
catch {
    [Console]::Error.WriteLine(
        "M3 real crash $Action FAIL: $([string]$_.Exception.Message)")
    exit 1
}
