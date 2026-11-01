# New-M5SupportBundle.ps1 -- the package a user attaches to a support request (R-15).
#
# **One command, one file.**  It gathers what is needed to explain a failure without asking the
# user to go hunting: versions, OS, driver, service state, profiles, recent operations, the
# bundle manifests, and the tails of the logs.  Everything text is **redacted by default**
# (computer name, user profile paths, absolute paths); `-IncludePaths` keeps the absolute paths
# when a maintainer explicitly asks for them.
#
# The bundle is deliberately self-describing: `manifest.json` lists every file with its SHA-256
# and carries a `failure` section summarising the most recent failed operation, so a reader can
# start from one place instead of reading the whole archive.
#
# Run it **as administrator**: the durable log directory is management-only on purpose, and a
# standard user cannot read it.

[CmdletBinding()]
param(
    [string]$Output,
    [string]$InstallDir = 'C:\Program Files\MyRamDisk',
    [string]$DataRoot = 'C:\ProgramData\MyRamDisk',
    [string]$CliPath,
    [int]$LogTailKb = 256,
    [int]$RecentOperations = 20,
    [switch]$IncludePaths
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$text) { Write-Host "--- $text" }
function Fail([string]$text) {
    [Console]::Error.WriteLine("FAIL: $text")
    exit 1
}

$toolVersion = '1.0.0'
$createdUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path (Get-Location) ("MyRamDisk-support-" +
        (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') + '.zip')
}
$Output = [System.IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) { Fail "refusing to overwrite an existing bundle: $Output" }
if ($LogTailKb -lt 16) { Fail '-LogTailKb must be at least 16' }

if ([string]::IsNullOrWhiteSpace($CliPath)) {
    $CliPath = Join-Path $InstallDir 'myrd-cli.exe'
}

$staging = Join-Path ([System.IO.Path]::GetTempPath()) ('m5-support-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $staging | Out-Null
$collected = New-Object System.Collections.Generic.List[object]

function Get-Sha256([string]$path) {
    (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

# **Redaction happens on the way in.**  A text file is read, redacted, and only then written, so
# the raw bytes never reach the archive; the manifest hashes what was actually written.
function Protect-Text([string]$text) {
    if ([string]::IsNullOrEmpty($text)) { return $text }
    $result = $text
    $computer = $env:COMPUTERNAME
    if (-not [string]::IsNullOrWhiteSpace($computer)) {
        $result = $result -replace [regex]::Escape($computer), '%COMPUTERNAME%'
    }
    $profile = $env:USERPROFILE
    if (-not [string]::IsNullOrWhiteSpace($profile)) {
        $result = $result -replace [regex]::Escape($profile), '%USERPROFILE%'
        $userName = Split-Path $profile -Leaf
        if (-not [string]::IsNullOrWhiteSpace($userName)) {
            $result = $result -replace ('C:\\Users\\' + [regex]::Escape($userName)), '%USERPROFILE%'
            $result = $result -replace ('C:/Users/' + [regex]::Escape($userName)), '%USERPROFILE%'
        }
    }
    if (-not $IncludePaths) {
        # Absolute Windows paths become <path>.  This is what keeps bundle locations and share
        # names out of a package that travels by email.
        $result = [regex]::Replace($result, '[A-Za-z]:\\[^"''\r\n<>|?*]*', '<path>')
        $result = [regex]::Replace($result, '[A-Za-z]:/[^"''\r\n<>|?*]*', '<path>')
    }
    return $result
}

function Add-TextFile([string]$relative, [string]$text) {
    $target = Join-Path $staging $relative
    $dir = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($target, (Protect-Text $text), (New-Object System.Text.UTF8Encoding($false)))
    $collected.Add([pscustomobject]@{ path = $relative; bytes = (Get-Item -LiteralPath $target).Length;
        sha256 = (Get-Sha256 $target) })
}

function Add-JsonFile([string]$relative, $object) {
    Add-TextFile $relative ($object | ConvertTo-Json -Depth 8)
}

function Add-BinaryFile([string]$relative, [string]$source) {
    $target = Join-Path $staging $relative
    $dir = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Copy-Item -LiteralPath $source -Destination $target -Force
    $collected.Add([pscustomobject]@{ path = $relative; bytes = (Get-Item -LiteralPath $target).Length;
        sha256 = (Get-Sha256 $target) })
}

function Add-LogTail([string]$relative, [string]$source) {
    if (-not (Test-Path -LiteralPath $source)) {
        Add-TextFile $relative "(absent: the product never wrote this log)"
        return
    }
    $length = (Get-Item -LiteralPath $source).Length
    $want = [Math]::Min($length, [int64]$LogTailKb * 1024)
    $stream = [System.IO.File]::Open($source, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        [void]$stream.Seek(-$want, [System.IO.SeekOrigin]::End)
        $buffer = New-Object byte[] $want
        $read = $stream.Read($buffer, 0, $want)
        $text = [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
    } finally {
        $stream.Dispose()
    }
    Add-TextFile $relative ("(last $([math]::Round($want / 1KB)) KiB of a $([math]::Round($length / 1KB)) KiB log)`n" + $text)
}

function Invoke-Cli([string[]]$arguments) {
    if (-not (Test-Path -LiteralPath $CliPath)) {
        return [pscustomobject]@{ exit = -1; output = "myrd-cli not found at $CliPath" }
    }
    # **Native stderr is data here, not a fatal error.**  The CLI writes refusals to stderr and
    # the tool must record them; with $ErrorActionPreference='Stop' (set for the tool's own
    # failures) PS 5.1 turns that into a terminating RemoteException.  The CLI's failures are
    # exactly what a support bundle is for, so the preference is relaxed around the call and
    # the exit code is captured explicitly.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $text = (& $CliPath @arguments 2>&1 | Out-String)
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    return [pscustomobject]@{ exit = $code; output = $text }
}

try {
    Write-Step "collecting into $staging"

    # ---- environment --------------------------------------------------------
    $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction SilentlyContinue
    $environment = [ordered]@{
        collected_utc   = $createdUtc
        tool_version    = $toolVersion
        include_paths   = [bool]$IncludePaths
        powershell      = $PSVersionTable.PSVersion.ToString()
        is_admin        = ([Security.Principal.WindowsPrincipal](
                            [Security.Principal.WindowsIdentity]::GetCurrent())).
                            IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
        os_caption      = if ($os) { $os.Caption } else { 'unknown' }
        os_version      = if ($os) { $os.Version } else { 'unknown' }
        os_build        = if ($os) { $os.BuildNumber } else { 'unknown' }
        architecture    = $env:PROCESSOR_ARCHITECTURE
        computer        = $env:COMPUTERNAME
        user            = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    }
    Add-JsonFile 'environment.json' $environment

    # ---- product versions and hashes ---------------------------------------
    $binaries = @()
    foreach ($name in @('myrd-cli.exe', 'myrd-service.exe', 'myrd-m5-worker.exe', 'myrd-gui.exe')) {
        $path = Join-Path $InstallDir $name
        if (-not (Test-Path -LiteralPath $path)) {
            $binaries += [pscustomobject]@{ name = $name; present = $false }
            continue
        }
        $info = (Get-Item -LiteralPath $path).VersionInfo
        $binaries += [pscustomobject]@{
            name            = $name
            present         = $true
            file_version    = $info.FileVersion
            product_version = $info.ProductVersion
            bytes           = (Get-Item -LiteralPath $path).Length
            sha256          = (Get-Sha256 $path)
        }
    }
    $versionOutput = Invoke-Cli @('--version')
    Add-JsonFile 'versions.json' ([ordered]@{
        install_dir = $InstallDir
        cli         = $versionOutput.output.Trim()
        cli_exit    = $versionOutput.exit
        binaries    = $binaries
    })

    # ---- driver and service -------------------------------------------------
    $driverQuery = (& sc.exe query phdskmnt 2>&1 | Out-String)
    $driverFile = Join-Path $env:SystemRoot 'System32\drivers\phdskmnt.sys'
    $driverVersion = if (Test-Path -LiteralPath $driverFile) {
        (Get-Item -LiteralPath $driverFile).VersionInfo.FileVersion
    } else { 'absent' }
    Add-JsonFile 'driver.json' ([ordered]@{
        service_query = $driverQuery.Trim()
        driver_file_version = $driverVersion
        driver_file_sha256 = if (Test-Path -LiteralPath $driverFile) { Get-Sha256 $driverFile } else { 'absent' }
    })

    $services = @()
    foreach ($service in @(Get-Service -Name 'MyRamDisk*' -ErrorAction SilentlyContinue)) {
        $services += [pscustomobject]@{ name = $service.Name; status = $service.Status.ToString() }
    }
    Add-JsonFile 'service.json' ([ordered]@{ services = $services })

    # ---- the CLI's own view -------------------------------------------------
    #
    # **Only verbs that exist.**  The CLI has `disk` and `operation` nouns; there is no
    # `status`, no `driver status` and no `operation list` (measured: calling them returns
    # "unknown command" and, under $ErrorActionPreference='Stop', aborts the tool).  The disk
    # list is the entry point, and each disk is then shown individually.
    $diskList = Invoke-Cli @('disk', 'list')
    Add-TextFile 'commands/disk-list.txt' $diskList.output
    $profileIds = @()
    foreach ($match in [regex]::Matches($diskList.output,
            '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')) {
        if ($profileIds -notcontains $match.Value) { $profileIds += $match.Value }
    }
    foreach ($profileId in $profileIds) {
        Add-TextFile ("commands/disk-show-" + $profileId + ".txt") ((Invoke-Cli @('disk', 'show', $profileId)).output)
    }

    # ---- recent operations, and the failure that prompted the bundle --------
    #
    # The ids come from the durable records (one `<id>.myop` per operation), because there is
    # no list verb; `operation show` is the readable projection of each of them.
    $operationsRoot = Join-Path $DataRoot 'operations'
    $ids = @()
    if (Test-Path -LiteralPath $operationsRoot) {
        foreach ($file in @(Get-ChildItem -LiteralPath $operationsRoot -Filter '*.myop' -File -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending)) {
            $name = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
            if ($ids -notcontains $name) { $ids += $name }
        }
    }
    $failure = $null
    $shown = 0
    foreach ($id in $ids) {
        if ($shown -ge $RecentOperations) { break }
        $show = Invoke-Cli @('operation', 'show', $id)
        Add-TextFile "operations/$id.txt" $show.output
        $shown++
        if ($null -eq $failure -and $show.output -match ':\s+failed') {
            $failure = [pscustomobject]@{
                operation_id = $id
                state        = 'failed'
                detail       = ($show.output -replace "`r?`n", ' | ')
            }
        }
    }
    Add-JsonFile 'failure.json' ([ordered]@{ failure = $failure })

    # ---- profiles and their manifests --------------------------------------
    $profilesRoot = Join-Path $DataRoot 'profiles'
    $profileRows = @()
    if (Test-Path -LiteralPath $profilesRoot) {
        foreach ($dir in @(Get-ChildItem -LiteralPath $profilesRoot -Directory -ErrorAction SilentlyContinue)) {
            $manifest = Join-Path $dir.FullName 'disk.rdm'
            $row = [pscustomobject]@{
                profile_id   = $dir.Name
                has_manifest = (Test-Path -LiteralPath $manifest)
                manifest_bytes = if (Test-Path -LiteralPath $manifest) { (Get-Item -LiteralPath $manifest).Length } else { 0 }
            }
            $profileRows += $row
            if (Test-Path -LiteralPath $manifest) {
                Add-BinaryFile ("manifests/" + $dir.Name + ".disk.rdm") $manifest
            }
        }
    }
    Add-JsonFile 'profiles.json' ([ordered]@{ profiles_root = $profilesRoot; profiles = $profileRows })

    # ---- logs ---------------------------------------------------------------
    foreach ($log in @('service.log', 'worker.log')) {
        Add-LogTail ("logs/" + $log) (Join-Path $DataRoot ('logs\' + $log))
    }

    # ---- the bundle's own manifest -----------------------------------------
    Write-Step 'writing manifest.json'
    Add-JsonFile 'manifest.json' ([ordered]@{
        tool_version = $toolVersion
        created_utc  = $createdUtc
        redacted     = -not $IncludePaths
        failure      = $failure
        files        = $collected
    })

    Write-Step "compressing to $Output"
    $parent = Split-Path -Parent $Output
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $Output -CompressionLevel Optimal
    if (-not (Test-Path -LiteralPath $Output)) { Fail 'the archive was not created' }
    $size = (Get-Item -LiteralPath $Output).Length
    $failureId = if ($null -ne $failure) { $failure.operation_id } else { 'none' }
    Write-Host ("READING: support_bundle={0}" -f $Output)
    Write-Host ("READING: support_bundle_bytes={0}" -f $size)
    Write-Host ("READING: support_bundle_files={0}" -f $collected.Count)
    Write-Host ("READING: support_bundle_failure={0}" -f $failureId)
    Write-Host 'PASS New-M5SupportBundle'
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}
