# T4-01: install, run, and uninstall, on the guest.
#
# What this proves is the shipped path rather than the test harness's own `sc create`: the installer
# copies a release, checks the driver instead of installing it, gives the durable directories an
# explicit DACL, registers the service and waits for it to answer.  The uninstall then has to take
# the disks down before it removes the thing that serves them, and has to leave the durable record
# behind unless asked to purge it.

param(
    [string]$Configuration = 'Release',
    [string]$ServiceName = 'MyRamDiskService'
)

$ErrorActionPreference = 'Stop'
if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.install_uninstall creates a real service, worker and AIM disk, and may only ' +
        'run on the guest (AGENTS.md section 4).')
    exit 2
}

function Fail([string]$message) {
    [Console]::Error.WriteLine("FAIL: $message")
    exit 1
}

function Step([string]$message) {
    Write-Host "--- $message"
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$installScript = Join-Path $repo 'tools/install/Install-MyRamDisk.ps1'
$uninstallScript = Join-Path $repo 'tools/install/Uninstall-MyRamDisk.ps1'
$source = Join-Path $repo "out/build/msvc/src/$Configuration"
$installDir = 'C:\Program Files\MyRamDisk'
$dataDir = 'C:\ProgramData\MyRamDisk'

foreach ($path in @($installScript, $uninstallScript, $source)) {
    if (-not (Test-Path -LiteralPath $path)) { Fail "missing: $path" }
}

function Invoke-Script {
    param([string]$Path, [string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Path @Arguments 2>&1 |
            Out-String
        return [pscustomobject]@{ exit = $LASTEXITCODE; output = $output }
    } finally {
        $ErrorActionPreference = $previous
    }
}

# Leave nothing from an earlier run: the installer replaces a registration, but a stale install
# directory would make "did the install put the files there" unanswerable.
$null = Invoke-Script -Path $uninstallScript -Arguments @('-AcceptVolatileDataLoss', '-PurgeData')

$installed = $false
try {
    Step 'install'
    $install = Invoke-Script -Path $installScript -Arguments @('-SourceDirectory', $source)
    Write-Host $install.output.Trim()
    if ($install.exit -ne 0) { Fail "the installer failed ($($install.exit))" }
    $installed = $true

    foreach ($file in @('myrd-service.exe', 'myrd-cli.exe', 'myrd-gui.exe', 'myrd-m5-worker.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $installDir $file))) {
            Fail "the install directory has no $file"
        }
    }
    $state = & sc.exe query $ServiceName 2>&1 | Out-String
    if ($state -notmatch 'RUNNING') { Fail "the service is not running:`n$state" }
    Write-Host '  service installed and running'

    Step 'the durable directories have the DACL the installer gave them'
    $acl = & icacls.exe $dataDir 2>&1 | Out-String
    if ($acl -notmatch 'NT AUTHORITY\\SYSTEM' -and $acl -notmatch 'S-1-5-18') {
        Fail "the data directory does not grant SYSTEM:`n$acl"
    }
    if ($acl -notmatch 'BUILTIN\\Administrators' -and $acl -notmatch 'S-1-5-32-544') {
        Fail "the data directory does not grant Administrators:`n$acl"
    }
    # The profile store refuses a directory a user can even read (measured:
    # 'A management ACL entry lacks required access'), so the installer must not grant it.
    $profilesAcl = & icacls.exe (Join-Path $dataDir 'profiles') 2>&1 | Out-String
    if ($profilesAcl -match 'BUILTIN\\Users' -or $profilesAcl -match 'S-1-5-32-545') {
        Fail "the profiles directory is readable by Users, which the store refuses:`n$profilesAcl"
    }

    Step 'the installed service serves its own client'
    $cli = Join-Path $installDir 'myrd-cli.exe'
    $listing = & $cli disk list 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Fail "the installed client cannot list: $listing" }
    Write-Host "  $($listing.Trim())"

    # ---- R-07: one version, three binaries -------------------------------------------------
    # The version check rides on the install case because that is where the *shipped* binaries
    # exist: the service, the CLI and the GUI all print `product::kProductVersion`, and the
    # header they print is compared too, so "they agree" cannot be true merely because all three
    # were built from the same stale copy.
    Step 'the service, the CLI and the GUI report the same version (R-07)'
    $versionPattern = 'myrd-[a-z]+ (\d+\.\d+\.\d+)'
    $reported = @{}
    foreach ($piece in @(
            @{ name = 'cli'; file = 'myrd-cli.exe' },
            @{ name = 'service'; file = 'myrd-service.exe' },
            @{ name = 'gui'; file = 'myrd-gui.exe' })) {
        $exe = Join-Path $installDir $piece.file
        # **Start-Process for all three.**  The GUI is a Windows-subsystem binary, so PowerShell
        # cannot capture its stdout through `&`; redirecting to a file works for every subsystem,
        # which keeps the three measurements identical instead of special-casing one.
        $outFile = Join-Path $env:TEMP ("m5-version-" + $piece.name + ".txt")
        $errFile = "$outFile.err"
        $process = Start-Process -FilePath $exe -ArgumentList '--version' `
            -RedirectStandardOutput $outFile -RedirectStandardError $errFile `
            -NoNewWindow -Wait -PassThru
        $output = Get-Content -LiteralPath $outFile -Raw -ErrorAction SilentlyContinue
        if ($null -eq $output) { $output = '' }
        if ($process.ExitCode -ne 0) {
            Fail ("$($piece.name) --version exited $($process.ExitCode): " +
                (Get-Content -LiteralPath $errFile -Raw -ErrorAction SilentlyContinue))
        }
        $match = [regex]::Match($output, $versionPattern)
        if (-not $match.Success) {
            Fail "$($piece.name) --version printed no version (exit $($process.ExitCode)): $output"
        }
        $reported[$piece.name] = $match.Groups[1].Value
        Write-Host "  $($piece.name): $($reported[$piece.name])"
    }
    $distinct = @($reported.Values | Sort-Object -Unique)
    if ($distinct.Count -ne 1) {
        Fail ("the shipped binaries disagree on the version: " +
            (($reported.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', '))
    }
    $headerPath = Join-Path $repo 'src/product/product_version.h'
    $header = Get-Content -LiteralPath $headerPath -Raw
    $expected = [regex]::Match($header, 'kProductVersion = "([^"]+)"')
    if (-not $expected.Success) { Fail "could not read kProductVersion from $headerPath" }
    if ($distinct[0] -ne $expected.Groups[1].Value) {
        Fail ("the binaries report $($distinct[0]) but the source says " +
            "$($expected.Groups[1].Value)")
    }
    Write-Host ("  consistent version: {0} (matches product_version.h)" -f $distinct[0])

    Step 'uninstall removes the service and the files, and keeps the record'
    $uninstall = Invoke-Script -Path $uninstallScript -Arguments @()
    Write-Host $uninstall.output.Trim()
    if ($uninstall.exit -ne 0) { Fail "the uninstaller failed ($($uninstall.exit))" }
    $installed = $false

    if ((& sc.exe query $ServiceName 2>&1 | Out-String) -notmatch '1060') {
        Fail 'the service is still registered after uninstall'
    }
    if (Test-Path -LiteralPath $installDir) { Fail 'the install directory survived the uninstall' }
    if (-not (Test-Path -LiteralPath $dataDir)) {
        Fail 'the data directory was removed without -PurgeData'
    }
    $stray = @(Get-Process -Name 'myrd-*' -ErrorAction SilentlyContinue)
    if ($stray.Count -ne 0) { Fail "processes survived the uninstall: $($stray.Count)" }
    Write-Host '  service and files gone, data directory kept, no processes left'
}
finally {
    if ($installed) {
        $null = Invoke-Script -Path $uninstallScript -Arguments @('-AcceptVolatileDataLoss')
    }
}

$purged = Invoke-Script -Path $uninstallScript -Arguments @('-PurgeData')
if ($purged.exit -ne 0) { Fail "the second uninstall (acting as the purge) failed: $($purged.output)" }
if (Test-Path -LiteralPath $dataDir) { Fail 'the data directory survived -PurgeData' }

Write-Host 'PASS T4-01: install, serve, uninstall, keep the record, purge on request'
exit 0
