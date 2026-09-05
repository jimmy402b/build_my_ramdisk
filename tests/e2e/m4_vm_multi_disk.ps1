[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Preflight', 'PostRun')]
    [string]$Phase,

    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$BundleA,

    [Parameter(Mandatory = $true)]
    [string]$BundleB,

    [ValidatePattern('^[A-Za-z]:$')]
    [string]$DriveA = 'R:',

    [ValidatePattern('^[A-Za-z]:$')]
    [string]$DriveB = 'S:',

    [Parameter(Mandatory = $true)]
    [string]$EvidenceDirectory,

    [string]$CommandLineA,
    [string]$CommandStdoutA,
    [string]$CommandStderrA,
    [int]$CommandExitCodeA,

    [string]$CommandLineB,
    [string]$CommandStdoutB,
    [string]$CommandStderrB,
    [int]$CommandExitCodeB,

    [string]$SameBundleCollisionCommandLine,
    [string]$SameBundleCollisionStdout,
    [string]$SameBundleCollisionStderr,
    [int]$SameBundleCollisionExitCode,

    [string]$DriveCollisionCommandLine,
    [string]$DriveCollisionStdout,
    [string]$DriveCollisionStderr,
    [int]$DriveCollisionExitCode,

    [switch]$RequireClean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function New-ObservationDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    if ([IO.Directory]::Exists($full) -or [IO.File]::Exists($full)) {
        throw "Evidence directory already exists: $full"
    }
    $parent = [IO.Directory]::GetParent($full)
    if ($null -eq $parent -or -not [IO.Directory]::Exists($parent.FullName)) {
        throw "Evidence parent must already exist: $($parent.FullName)"
    }
    [IO.Directory]::CreateDirectory($full) | Out-Null
    return $full
}

function Write-Observation {
    param([Parameter(Mandatory = $true)]$Value, [Parameter(Mandatory = $true)][string]$Path)
    $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Get-DriveObservation {
    param([Parameter(Mandatory = $true)][string]$Letter)
    $name = $Letter.TrimEnd(':')
    $volume = @(Get-Volume -DriveLetter $name -ErrorAction SilentlyContinue)
    $partition = @(Get-Partition -DriveLetter $name -ErrorAction SilentlyContinue)
    [ordered]@{
        drive_letter = $Letter
        present = ($volume.Count -eq 1)
        filesystem = if ($volume.Count -eq 1) { [string]$volume[0].FileSystem } else { $null }
        volume_unique_id = if ($volume.Count -eq 1) { [string]$volume[0].UniqueId } else { $null }
        filesystem_label = if ($volume.Count -eq 1) { [string]$volume[0].FileSystemLabel } else { $null }
        partition_number = if ($partition.Count -eq 1) { [int]$partition[0].PartitionNumber } else { $null }
        disk_number = if ($partition.Count -eq 1) { [int]$partition[0].DiskNumber } else { $null }
        size = if ($volume.Count -eq 1) { [UInt64]$volume[0].Size } else { $null }
    }
}

function Get-AimObservation {
    $disks = @(Get-Disk -ErrorAction SilentlyContinue | Where-Object {
        ([string]$_.FriendlyName -match '(?i)Arsenal|AIM') -or
        ([string]$_.Manufacturer -match '(?i)Arsenal|AIM')
    })
    @($disks | ForEach-Object {
        [ordered]@{
            number = [int]$_.Number
            friendly_name = [string]$_.FriendlyName
            manufacturer = [string]$_.Manufacturer
            serial = [string]$_.SerialNumber
            size = [UInt64]$_.Size
            unique_id = [string]$_.UniqueId
            operational_status = @($_.OperationalStatus | ForEach-Object { [string]$_ })
        }
    })
}

function Get-ProcessObservation {
    @(Get-Process -Name myrd -ErrorAction SilentlyContinue | ForEach-Object {
        [ordered]@{ pid = [int]$_.Id; path = [string]$_.Path }
    })
}

function Get-BundleObservation {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    $exists = [IO.Directory]::Exists($full)
    $files = @()
    if ($exists) {
        $files = @(Get-ChildItem -LiteralPath $full -File -Recurse -ErrorAction SilentlyContinue |
            Sort-Object FullName |
            ForEach-Object {
                [ordered]@{
                    relative = $_.FullName.Substring($full.Length).TrimStart('\')
                    length = [UInt64]$_.Length
                    # A locked/unreadable bundle file is a harness failure; do
                    # not emit a null hash that could be mistaken for evidence.
                    sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256 -ErrorAction Stop).Hash
                }
            })
    }
    [ordered]@{
        path = $full
        exists = $exists
        file_count = $files.Count
        files = $files
        generation_files = @($files | Where-Object { $_.relative -match '(?i)manifest|journal|base-.*\.vhdx' } | ForEach-Object { $_.relative })
    }
}

function Get-GenerationObservations {
    param([Parameter(Mandatory = $true)][string]$Text)
    $values = @()
    foreach ($match in [regex]::Matches($Text, '(?i)generation\s*[:=]?\s*(\d+)')) {
        $values += [UInt64]$match.Groups[1].Value
    }
    @($values | Select-Object -Unique)
}

function Copy-CapturedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$CommandLine,
        [Parameter(Mandatory = $true)][string]$Stdout,
        [Parameter(Mandatory = $true)][string]$Stderr,
        [Parameter(Mandatory = $true)][int]$ExitCode,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    if (-not [IO.File]::Exists($Stdout)) { throw "$Name stdout capture is missing: $Stdout" }
    if (-not [IO.File]::Exists($Stderr)) { throw "$Name stderr capture is missing: $Stderr" }
    $dir = Join-Path $Destination $Name
    [IO.Directory]::CreateDirectory($dir) | Out-Null
    $outPath = Join-Path $dir 'stdout.log'
    $errPath = Join-Path $dir 'stderr.log'
    Copy-Item -LiteralPath $Stdout -Destination $outPath
    Copy-Item -LiteralPath $Stderr -Destination $errPath
    $outText = Get-Content -LiteralPath $outPath -Raw
    $errText = Get-Content -LiteralPath $errPath -Raw
    $record = [ordered]@{
        name = $Name
        command_line = $CommandLine
        exit_code = $ExitCode
        stdout = $outPath
        stderr = $errPath
        generations = @(Get-GenerationObservations -Text ($outText + "`n" + $errText))
    }
    Write-Observation $record (Join-Path $dir 'command.json')
    return $record
}

function Assert-CommandShape {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$CommandLine,
        [Parameter(Mandatory = $true)][string]$ExpectedBundle,
        [Parameter(Mandatory = $true)][string]$ExpectedDrive
    )
    if ($CommandLine -notmatch [regex]::Escape($ExpectedBundle)) {
        throw "$Name command line does not contain expected bundle path: $ExpectedBundle"
    }
    if ($CommandLine -notmatch [regex]::Escape($ExpectedDrive)) {
        throw "$Name command line does not contain expected drive letter: $ExpectedDrive"
    }
}

function Test-CompleteCommandSet {
    param([Parameter(Mandatory = $true)][hashtable]$Values)
    foreach ($name in @('CommandLineA', 'CommandStdoutA', 'CommandStderrA', 'CommandLineB', 'CommandStdoutB', 'CommandStderrB', 'SameBundleCollisionCommandLine', 'SameBundleCollisionStdout', 'SameBundleCollisionStderr', 'DriveCollisionCommandLine', 'DriveCollisionStdout', 'DriveCollisionStderr')) {
        if (-not $Values.ContainsKey($name) -or [string]::IsNullOrWhiteSpace([string]$Values[$name])) {
            throw "PostRun requires $name and its corresponding exit code."
        }
    }
    foreach ($name in @('CommandExitCodeA', 'CommandExitCodeB', 'SameBundleCollisionExitCode', 'DriveCollisionExitCode')) {
        if (-not $Values.ContainsKey($name)) { throw "PostRun requires $name." }
    }
}

$evidence = $null
$classification = 'HARNESS_FAIL'
$message = $null
try {
    $evidence = New-ObservationDirectory -Path $EvidenceDirectory
    $exe = [IO.Path]::GetFullPath($Executable)
    $bundleA = [IO.Path]::GetFullPath($BundleA)
    $bundleB = [IO.Path]::GetFullPath($BundleB)
    if (-not [IO.File]::Exists($exe)) { throw "Executable not found: $exe" }
    if ($bundleA -eq $bundleB) { throw 'Bundle A and bundle B must be different paths.' }
    if ($DriveA -eq $DriveB) { throw 'Drive A and drive B must be different letters.' }
    $parentA = [IO.Directory]::GetParent($bundleA)
    $parentB = [IO.Directory]::GetParent($bundleB)
    if ($null -eq $parentA -or -not [IO.Directory]::Exists($parentA.FullName)) { throw 'Bundle A parent does not exist.' }
    if ($null -eq $parentB -or -not [IO.Directory]::Exists($parentB.FullName)) { throw 'Bundle B parent does not exist.' }
    $volumeA = Get-Volume -DriveLetter ([IO.Path]::GetPathRoot($parentA.FullName).Substring(0, 1)) -ErrorAction Stop
    $volumeB = Get-Volume -DriveLetter ([IO.Path]::GetPathRoot($parentB.FullName).Substring(0, 1)) -ErrorAction Stop
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $identity = Get-CimInstance Win32_ComputerSystemProduct | Select-Object -First 1
    $os = Get-CimInstance Win32_OperatingSystem | Select-Object -First 1
    $macs = @(Get-NetAdapter -Physical -ErrorAction SilentlyContinue | Where-Object Status -eq 'Up' | ForEach-Object MacAddress)
    $initial = [ordered]@{
        drives = [ordered]@{ A = Get-DriveObservation $DriveA; B = Get-DriveObservation $DriveB }
        aim_devices = @(Get-AimObservation)
        processes = @(Get-ProcessObservation)
    }
    $preconditions = [ordered]@{
        administrator = $admin
        bundle_a_parent_ntfs = ([string]$volumeA.FileSystem -eq 'NTFS')
        bundle_b_parent_ntfs = ([string]$volumeB.FileSystem -eq 'NTFS')
        drive_a_free = -not [bool]$initial.drives.A.present
        drive_b_free = -not [bool]$initial.drives.B.present
        no_myrd_processes = ($initial.processes.Count -eq 0)
        no_aim_devices = ($initial.aim_devices.Count -eq 0)
        bundles_distinct = ($bundleA -ne $bundleB)
        drives_distinct = ($DriveA -ne $DriveB)
    }
    Write-Observation ([ordered]@{
        schema = 'myrd.m4.vm.multi-disk.preflight.v1'
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        vm = [ordered]@{ uuid = [string]$identity.UUID; vendor = [string]$identity.Vendor; product = [string]$identity.Name; mac_addresses = $macs }
        os = [ordered]@{ caption = [string]$os.Caption; version = [string]$os.Version; build = [string]$os.BuildNumber }
        admin_token = $admin
        executable = $exe
        bundles = [ordered]@{ A = $bundleA; B = $bundleB }
        drives = [ordered]@{ A = $DriveA; B = $DriveB }
        initial = $initial
        preconditions = $preconditions
    }) (Join-Path $evidence 'preflight.json')
    if ($Phase -eq 'Preflight') {
        $classification = if ($preconditions.Values -contains $false) { 'ENVIRONMENT_BLOCKED' } else { 'PASS' }
        $message = 'Preflight observation recorded; no product command was invoked.'
    } else {
        foreach ($name in @('CommandLineA', 'CommandStdoutA', 'CommandStderrA', 'CommandExitCodeA', 'CommandLineB', 'CommandStdoutB', 'CommandStderrB', 'CommandExitCodeB', 'SameBundleCollisionCommandLine', 'SameBundleCollisionStdout', 'SameBundleCollisionStderr', 'SameBundleCollisionExitCode', 'DriveCollisionCommandLine', 'DriveCollisionStdout', 'DriveCollisionStderr', 'DriveCollisionExitCode')) {
            if (-not $PSBoundParameters.ContainsKey($name)) { throw "PostRun requires parameter $name." }
        }
        Test-CompleteCommandSet -Values @{
            CommandLineA = $CommandLineA; CommandStdoutA = $CommandStdoutA; CommandStderrA = $CommandStderrA
            CommandLineB = $CommandLineB; CommandStdoutB = $CommandStdoutB; CommandStderrB = $CommandStderrB
            SameBundleCollisionCommandLine = $SameBundleCollisionCommandLine; SameBundleCollisionStdout = $SameBundleCollisionStdout; SameBundleCollisionStderr = $SameBundleCollisionStderr
            DriveCollisionCommandLine = $DriveCollisionCommandLine; DriveCollisionStdout = $DriveCollisionStdout; DriveCollisionStderr = $DriveCollisionStderr
            CommandExitCodeA = $CommandExitCodeA; CommandExitCodeB = $CommandExitCodeB
            SameBundleCollisionExitCode = $SameBundleCollisionExitCode; DriveCollisionExitCode = $DriveCollisionExitCode
        }
        Assert-CommandShape 'disk-a' $CommandLineA $bundleA $DriveA
        Assert-CommandShape 'disk-b' $CommandLineB $bundleB $DriveB
        Assert-CommandShape 'same-bundle-collision' $SameBundleCollisionCommandLine $bundleA $DriveA
        # The drive-collision case must use a third, disposable bundle; B is
        # intentionally still running and therefore would be classified as a
        # writer-lock collision before the occupied-letter check.
        if ($DriveCollisionCommandLine -notmatch [regex]::Escape($DriveA)) {
            throw "drive-collision command line does not contain expected drive letter: $DriveA"
        }
        $commands = [ordered]@{}
        $commands.A = Copy-CapturedCommand 'disk-a' $CommandLineA $CommandStdoutA $CommandStderrA $CommandExitCodeA $evidence
        $commands.B = Copy-CapturedCommand 'disk-b' $CommandLineB $CommandStdoutB $CommandStderrB $CommandExitCodeB $evidence
        $commands.same_bundle_collision = Copy-CapturedCommand 'same-bundle-collision' $SameBundleCollisionCommandLine $SameBundleCollisionStdout $SameBundleCollisionStderr $SameBundleCollisionExitCode $evidence
        $commands.drive_collision = Copy-CapturedCommand 'drive-collision' $DriveCollisionCommandLine $DriveCollisionStdout $DriveCollisionStderr $DriveCollisionExitCode $evidence
        $collisionText = (Get-Content -LiteralPath $commands.same_bundle_collision.stdout -Raw) + (Get-Content -LiteralPath $commands.same_bundle_collision.stderr -Raw)
        $driveCollisionText = (Get-Content -LiteralPath $commands.drive_collision.stdout -Raw) + (Get-Content -LiteralPath $commands.drive_collision.stderr -Raw)
        $lockEvidence = ($commands.same_bundle_collision.exit_code -ne 0) -and ($collisionText -match '(?i)lock|already running|in use|busy')
        $driveEvidence = ($commands.drive_collision.exit_code -ne 0) -and ($driveCollisionText -match '(?i)drive|letter|volume|occupied|in use|already')
        $final = [ordered]@{
            drives = [ordered]@{ A = Get-DriveObservation $DriveA; B = Get-DriveObservation $DriveB }
            aim_devices = @(Get-AimObservation)
            processes = @(Get-ProcessObservation)
            bundles = [ordered]@{ A = Get-BundleObservation $bundleA; B = Get-BundleObservation $bundleB }
        }
        $cleanup = [ordered]@{
            no_myrd_processes = ($final.processes.Count -eq 0)
            drive_a_absent = -not [bool]$final.drives.A.present
            drive_b_absent = -not [bool]$final.drives.B.present
            no_aim_devices = ($final.aim_devices.Count -eq 0)
        }
        Write-Observation ([ordered]@{
            schema = 'myrd.m4.vm.multi-disk.postrun.v1'
            timestamp_utc = [DateTime]::UtcNow.ToString('o')
            commands = $commands
            collision_checks = [ordered]@{ same_bundle_lock = $lockEvidence; drive_letter = $driveEvidence }
            final = $final
            cleanup = $cleanup
        }) (Join-Path $evidence 'postrun.json')
        # PostRun is intentionally invoked while A/B are alive, so the
        # dynamic baseline fields (free letters, no processes, no AIM disks)
        # must not be treated as failures here.  They are evidence of the
        # expected running state; static safety checks remain mandatory.
        $staticPreconditions = @(
            $preconditions.administrator,
            $preconditions.bundle_a_parent_ntfs,
            $preconditions.bundle_b_parent_ntfs,
            $preconditions.bundles_distinct,
            $preconditions.drives_distinct)
        $required = @($staticPreconditions) + @($commands.A.exit_code -eq 0, $commands.B.exit_code -eq 0, $lockEvidence, $driveEvidence)
        if ($RequireClean) { $required += @($cleanup.Values) }
        $classification = if ($required -contains $false) { 'PRODUCT_FAIL' } else { 'PASS' }
        $message = 'Two-disk observations and collision evidence recorded.'
    }
} catch {
    $classification = 'HARNESS_FAIL'
    $message = $_.Exception.Message
    if ($null -ne $evidence) { Set-Content -LiteralPath (Join-Path $evidence 'error.txt') -Value $message -Encoding UTF8 }
}

$summary = [ordered]@{ schema = 'myrd.m4.vm.multi-disk.result.v1'; classification = $classification; message = $message; evidence_directory = $evidence }
if ($null -ne $evidence) { Write-Observation $summary (Join-Path $evidence 'result.json') }
Write-Output ("{0}: {1}" -f $classification, $message)
if ($classification -eq 'PRODUCT_FAIL') { exit 1 }
if ($classification -eq 'ENVIRONMENT_BLOCKED') { exit 3 }
if ($classification -eq 'HARNESS_FAIL') { exit 2 }
