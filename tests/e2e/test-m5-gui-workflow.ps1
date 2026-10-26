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
$cliExe = Join-Path $bin 'myrd-cli.exe'
$guiExe = Join-Path $bin 'myrd-gui.exe'
$serviceExe = Join-Path $bin 'myrd-service.exe'
$serviceName = 'MyRamDiskGuiE2E'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

foreach ($path in @($cliExe, $guiExe, $serviceExe)) {
    if (-not (Test-Path -LiteralPath $path)) { Fail "missing binary: $path" }
}

$mount = Select-FreeLetter
if ($null -eq $mount) { Fail 'no free drive letter' }
$bundle = 'C:\m5\gui-e2e-bundle'
Remove-Item -LiteralPath $bundle -Recurse -Force -ErrorAction SilentlyContinue

Write-Step 'install and start the service'
Remove-ScService
Invoke-Sc @('create', $serviceName, 'binPath=', $serviceExe, 'type=', 'own', 'start=', 'demand')
Invoke-Sc @('start', $serviceName)
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

    Write-Step 'the window starts, stays up, and owns a window with the product title'
    $gui = Start-Process -FilePath $guiExe -PassThru
    Start-Sleep -Seconds 6
    if ($gui.HasExited) { Fail "the GUI exited early (code $($gui.ExitCode))" }
    if (-not ([System.Management.Automation.PSTypeName]'Myrd.WindowProbe').Type) {
        Add-Type -Namespace Myrd -Name WindowProbe -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern System.IntPtr FindWindow(string className, string windowName);
'@
    }
    $window = [Myrd.WindowProbe]::FindWindow($null, 'MyRamDisk')
    if ($window -eq [System.IntPtr]::Zero) { Fail 'no window titled MyRamDisk exists' }
    Write-Host "  window handle 0x$($window.ToString('X')); process $($gui.Id) is alive"
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
    Remove-ScService
    Remove-Item -LiteralPath $bundle -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'PASS T3-05: the GUI sees the service through the client library, and its window runs'
exit 0
