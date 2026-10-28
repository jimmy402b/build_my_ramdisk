# Puts the guest back into the state a test run expects: no worker, no disk, no service, no records.
#
# **This exists because a reading taken on a dirty guest measures the guest.**  Measured: a leftover
# profile at T: with generation 31 and dirty persistence made `integration.m5.cli_e2e` fail in three
# seconds and delayed the next case by thirty minutes -- a failure that looks exactly like a product
# defect and is not one.
#
# Run it on the guest, as the elevated agent or an elevated shell:
#   powershell -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1

[CmdletBinding()]
param(
    [string]$DataDirectory = 'C:\ProgramData\MyRamDisk',
    [string[]]$ServiceNames = @('MyRamDiskService', 'MyRamDiskGuiE2E', 'MyRamDiskPowerLoss'),
    [switch]$PurgeData
)

$ErrorActionPreference = 'Continue'

function Step([string]$message) {
    Write-Host "--- $message"
}

Step 'stop the services that could be holding a disk'
foreach ($name in $ServiceNames) {
    sc.exe query $name 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        sc.exe stop $name 2>&1 | Out-Null
        for ($i = 0; $i -lt 40; $i++) {
            if ((sc.exe query $name 2>&1 | Out-String) -match 'STOPPED') { break }
            Start-Sleep -Milliseconds 250
        }
        sc.exe delete $name 2>&1 | Out-Null
        Write-Host "  removed $name"
    }
}

Step 'stop workers and any surviving service process'
$workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
$services = @(Get-Process -Name 'myrd-service' -ErrorAction SilentlyContinue)
foreach ($process in ($workers + $services)) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Write-Host "  killed $($process.ProcessName) $($process.Id)"
}
# A worker that will not die still holds the device, so this is a refusal rather than a shrug.
Start-Sleep -Milliseconds 500
$left = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
if ($left.Count -ne 0) {
    [Console]::Error.WriteLine("reset: $($left.Count) worker(s) survived; a run now would measure them")
    exit 1
}

# **The store refuses a directory that is not protected.**  Measured: with the leaf created but its ACL inherited,
# `m5.startup_sweep` failed with `profile_store.directory_acl` -- "A profile directory is not protected" -- which is
# the third of the three rules in the T4-01 evidence.  A reset that leaves directories the product refuses is a reset
# that makes tests fail for a reason that is not the product's.
function Protect-ManagementDirectory([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    $acl = Get-Acl -LiteralPath $path
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($rule in @($acl.Access)) { [void]$acl.RemoveAccessRule($rule) }
    foreach ($sidText in @('S-1-5-32-544', 'S-1-5-18')) {
        $sid = New-Object System.Security.Principal.SecurityIdentifier($sidText)
        $rights = New-Object System.Security.AccessControl.FileSystemAccessRule(
            $sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        [void]$acl.AddAccessRule($rights)
    }
    Set-Acl -LiteralPath $path -AclObject $acl
    Write-Host "  protected $path"
}
Step 'clear the durable record so the service starts from nothing'
foreach ($child in @('profiles', 'operations')) {
    $directory = Join-Path $DataDirectory $child
    if (Test-Path -LiteralPath $directory) {
        Get-ChildItem -LiteralPath $directory -Force -ErrorAction SilentlyContinue |
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    }
    # **The directory has to exist whether or not it existed before the reset.**
    # Measured: an uninstall can remove `operations`, and the old code only recreated a
    # directory it had just emptied.  The next service start then failed
    # `ServiceOperationStore::open_existing` with native=2, which `sc start` reports as
    # 1053 and which looks like a product start failure.  The DACL belongs to the
    # installer; this creates the directory and nothing else.
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        Write-Host "  created $directory"
    }
    $remaining = @(Get-ChildItem -LiteralPath $directory -Force -ErrorAction SilentlyContinue)
    Write-Host ("  {0}: {1} entries left" -f $child, $remaining.Count)
    if ($remaining.Count -ne 0) {
        [Console]::Error.WriteLine("reset: $child is not empty; a run now would inherit a disk")
        exit 1
    }
}
# `WorkerBindingStore::over_protected_root` opens `MyRamDisk\profiles\workers`, and a guest that has never
# installed the service has neither.  Measured: with only `profiles` present the case still failed with
# `profile_store.open_directory` `native=2`, because the leaf it wants is one level deeper.
$workers = Join-Path (Join-Path $DataDirectory 'profiles') 'workers'
if (-not (Test-Path -LiteralPath $workers)) {
    New-Item -ItemType Directory -Force -Path $workers | Out-Null
    Write-Host "  created $workers"
}
if ($PurgeData -and (Test-Path -LiteralPath $DataDirectory)) {
    Remove-Item -LiteralPath $DataDirectory -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "  purged $DataDirectory"
}

Step 'report what is left'
$disks = @(Get-CimInstance Win32_LogicalDisk -ErrorAction SilentlyContinue |
    Where-Object { $_.DriveType -eq 3 } | ForEach-Object { $_.DeviceID })
Write-Host "  mounted volumes: $($disks -join ', ')"
$logs = Join-Path $DataDirectory 'logs'
if (Test-Path -LiteralPath $logs) {
    # The log is left alone on purpose: it is the record of what the previous run did, and the next
    # run's failures are read against it.
    Write-Host "  kept $logs"
}
# **The store refuses a directory that is not protected.**  Measured: with the leaf created but its ACL inherited,
# `m5.startup_sweep` failed at `profile_store.directory_acl` -- "A profile directory is not protected" -- the third of
# the three rules in the T4-01 evidence.  A reset that leaves directories the product refuses is a reset that makes
# tests fail for a reason that is not the product's.
function Protect-ManagementDirectory([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    $acl = Get-Acl -LiteralPath $path
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($rule in @($acl.Access)) { [void]$acl.RemoveAccessRule($rule) }
    foreach ($sidText in @('S-1-5-32-544', 'S-1-5-18')) {
        $sid = New-Object System.Security.Principal.SecurityIdentifier($sidText)
        $rights = New-Object System.Security.AccessControl.FileSystemAccessRule(
            $sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        [void]$acl.AddAccessRule($rights)
    }
    Set-Acl -LiteralPath $path -AclObject $acl
    Write-Host "  protected $path"
}

foreach ($target in @($DataDirectory,
                      (Join-Path $DataDirectory 'profiles'),
                      (Join-Path $DataDirectory 'operations'),
                      (Join-Path (Join-Path $DataDirectory 'profiles') 'workers'))) {
    Protect-ManagementDirectory $target
}

Write-Host 'RESET'
exit 0
