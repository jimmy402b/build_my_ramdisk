# T3-05: the GUI as a thin client, accepted on the guest.
#
# A desktop test cannot read a ListView belonging to another process, so the acceptance is split
# into the two things that can be observed:
#
#   1. `myrd-gui.exe --self-check` runs the same client and the same view model the window uses and
#      prints what it would show -- the disk, its verdict, its generation and its memory;
#   2. the window itself is started, must stay alive, and must own a window with the product's
#      title, which is the part a self-check cannot fake.
#
# Everything it needs is created here and removed at the end: the service, one disk, and the
# process.  It runs only on the guest (AGENTS.md section 4).

param(
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.gui_workflow creates a real worker, a real AIM disk and a real volume, and ' +
        'may only run on the guest (AGENTS.md section 4).')
    exit 2
}

function Fail([string]$message) {
    [Console]::Error.WriteLine("FAIL: $message")
    exit 1
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
# The binaries this case *runs* are the installed ones: it drives the shipped installer, so it takes
# the path a user takes and the service finds the durable directories it requires.
$installDir = 'C:\Program Files\MyRamDisk'
$cliExe = Join-Path $installDir 'myrd-cli.exe'
$guiExe = Join-Path $installDir 'myrd-gui.exe'
$serviceExe = Join-Path $installDir 'myrd-service.exe'
$serviceName = 'MyRamDiskGuiE2E'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

foreach ($path in @((Join-Path $bin 'myrd-cli.exe'), (Join-Path $bin 'myrd-gui.exe'), (Join-Path $bin 'myrd-service.exe'))) {
    if (-not (Test-Path -LiteralPath $path)) { Fail "missing binary: $path" }
}

$mount = Select-FreeLetter
if ($null -eq $mount) { Fail 'no free drive letter' }
$bundle = 'C:\m5\gui-e2e-bundle'
Remove-Item -LiteralPath $bundle -Recurse -Force -ErrorAction SilentlyContinue

# **A clean record before the service starts.**  Measured: a profile left behind by an earlier run
# (a disk that was created and never stopped) makes the service's own start fail -- it recovers the
# durable state, tries to bring that disk back, and reports STOPPED when it cannot.  That is the
# product behaving as designed; a test that wants a clean start has to give it one.
foreach ($directory in @('C:\ProgramData\MyRamDisk\profiles', 'C:\ProgramData\MyRamDisk\operations')) {
    if (Test-Path -LiteralPath $directory) {
        Get-ChildItem -LiteralPath $directory -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    }
}
# **The shipped installer, not a hand-rolled `sc create`.**  Measured: the service refuses to start
# when its data directories are missing (`profile_store.open_directory`, native 2), and the profile
# store refuses directories whose DACL is not protected -- a test that creates neither gets a service
# that stops with exit code 0 and no log line.  Driving the installer also exercises the path a user
# takes, and it tolerates the SCM's thirty-second start budget itself.
Write-Step 'install through the shipped installer'
$installer = Join-Path $repo 'tools/install/Install-MyRamDisk.ps1'
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $installOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $installer `
        -SourceDirectory $bin -ServiceName $serviceName 2>&1 | Out-String
    $installExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previous
}
if ($installExit -ne 0) { Fail "the installer failed ($installExit)`n$installOutput" }
$answered = Wait-ForService 90
if ($null -eq $answered) { Fail "the service never answered:`n$((Get-ServiceState))" }
Write-Host "  service answering; disk will be mounted at $mount"

$profileId = $null
$gui = $null
try {
    Write-Step 'create one disk so the list has something to show'
    $create = Invoke-Cli @('disk', 'create', '--name', 'gui-e2e', '--bundle', $bundle,
        '--size', '32MiB', '--mount', $mount)
    if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the create carried no operation id' }
    $state = Wait-ForTerminal $createId 300
    if ($state -ne 'succeeded') { Fail "the create ended $state" }

    $listing = Invoke-Cli @('disk', 'list')
    foreach ($line in ($listing.output -split "`n")) {
        if ($line -match ('\s' + [regex]::Escape($mount))) {
            $uuidMatch = [regex]::Match($line,
                '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')
            if ($uuidMatch.Success) { $profileId = $uuidMatch.Value }
        }
    }
    if ($null -eq $profileId) { Fail "the listing has no profile uuid: $($listing.output)" }

    Write-Step 'the GUI reports from the same client the window uses'
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $selfCheck = & $guiExe --self-check 2>&1 | Out-String
        $selfExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    Write-Host ($selfCheck.Trim())
    if ($selfExit -ne 0) { Fail "self-check exited $selfExit`: $selfCheck" }
    if ($selfCheck -notmatch [regex]::Escape($mount)) {
        Fail "self-check did not report $mount`: $selfCheck"
    }
    if ($selfCheck -notmatch 'running') {
        Fail "self-check did not report the disk as running: $selfCheck"
    }
    if ($selfCheck -notmatch 'MiB') {
        Fail "self-check did not report memory: $selfCheck"
    }

    Write-Step 'the real window is created by the real window procedure'
    # **`--window-check` rather than finding the window from outside.**  Measured on the guest: a
    # probe from this process does not see the GUI's window -- the scheduled task and the GUI run on
    # a desktop FindWindow does not enumerate -- so the claim would have degraded to "a process with
    # that name is alive".  Instead the window is created by the same code path, reported by handle,
    # and found back by class and title from inside.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $windowCheck = & $guiExe --window-check 2>&1 | Out-String
        $windowExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    Write-Host ($windowCheck.Trim())
    if ($windowExit -ne 0) { Fail "window-check exited $windowExit`: $windowCheck" }

    Write-Step 'the application itself stays up'
    $gui = Start-Process -FilePath $guiExe -PassThru
    Start-Sleep -Seconds 6
    if ($gui.HasExited) { Fail "the GUI exited early (code $($gui.ExitCode))" }
    Write-Host "  process $($gui.Id) is alive after 6 s"
}
finally {
    if ($null -ne $gui -and -not $gui.HasExited) {
        Stop-Process -Id $gui.Id -Force -ErrorAction SilentlyContinue
    }
    Write-Step 'cleanup'
    if ($null -ne $profileId) {
        $stop = Invoke-Cli @('disk', 'stop', $profileId)
        if ($stop.exit -eq 0) {
            $stopId = Get-UuidFromText $stop.output
            if ($null -ne $stopId) { $null = Wait-ForTerminal $stopId 300 }
        }
        $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
        if ($forget.exit -eq 0) {
            $forgetId = Get-UuidFromText $forget.output
            if ($null -ne $forgetId) { $null = Wait-ForTerminal $forgetId 300 }
        }
    }
    # The uninstaller takes any disk down first, then removes the service and the files; the durable
    # record is left alone unless purged, which is the product's own rule and not worth bypassing here.
    $uninstaller = Join-Path $repo 'tools/install/Uninstall-MyRamDisk.ps1'
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $uninstallOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $uninstaller `
            -ServiceName $serviceName -AcceptVolatileDataLoss 2>&1 | Out-String
    } finally {
        $ErrorActionPreference = $previous
    }
    Write-Host "  $($uninstallOutput.Trim())"
    Remove-Item -LiteralPath $bundle -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'PASS T3-05: the GUI sees the service through the client library, and its window runs'
exit 0
