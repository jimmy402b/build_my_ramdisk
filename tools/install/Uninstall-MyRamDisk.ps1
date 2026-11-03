# Removes MyRamDisk: the service first, then any disk it was serving, then the files.
#
# **Disks are taken down before the service is deleted**, and a disk whose checkpoint cannot be
# confirmed needs `-AcceptVolatileDataLoss`: uninstalling is not a reason to discard writes without
# saying so.  The data directory is *kept* unless `-PurgeData`, because it is the record of what the
# disks were and someone diagnosing a removal will want it.
#
# `phdskmnt` is not touched.  This product did not install it and does not remove it.

[CmdletBinding()]
param(
    [string]$InstallDirectory = 'C:\Program Files\MyRamDisk',
    [string]$DataDirectory = 'C:\ProgramData\MyRamDisk',
    [string]$ServiceName = 'MyRamDiskService',
    [switch]$AcceptVolatileDataLoss,
    [switch]$PurgeData
)

$ErrorActionPreference = 'Stop'

function Fail([string]$message) {
    [Console]::Error.WriteLine("uninstall: $message")
    exit 1
}

function Step([string]$message) {
    Write-Host "--- $message"
}

$cli = Join-Path $InstallDirectory 'myrd-cli.exe'
$servicePresent = ((& sc.exe query $ServiceName 2>&1 | Out-String) -notmatch '1060')

Step 'take the disks down while the service can still answer'
if ($servicePresent -and (Test-Path -LiteralPath $cli)) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $listing = & $cli disk list 2>&1 | Out-String
        $listExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($listExit -eq 0) {
        $ids = [regex]::Matches($listing,
            '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')
        foreach ($match in $ids) {
            $id = $match.Value
            Write-Host "  stopping $id"
            $arguments = @('disk', 'stop', $id)
            if ($AcceptVolatileDataLoss) { $arguments += @('--force', '--accept-volatile-data-loss') }
            $stop = & $cli @arguments 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0) {
                if (-not $AcceptVolatileDataLoss) {
                    Fail ("$id could not be stopped cleanly, so nothing was removed: $stop`n" +
                        "Re-run with -AcceptVolatileDataLoss to discard its uncommitted writes.")
                }
                Write-Host "  $id refused a clean stop; continuing because volatile data loss was accepted"
            }
        }
    } else {
        Write-Host "  the service did not answer; no disk could be stopped from here"
    }
}

Step "stop and delete the service $ServiceName"
if ($servicePresent) {
    & sc.exe stop $ServiceName 2>&1 | Out-Null
    for ($i = 0; $i -lt 120; $i++) {
        if (((& sc.exe query $ServiceName 2>&1 | Out-String) -match 'STOPPED')) { break }
        Start-Sleep -Milliseconds 500
    }
    & sc.exe delete $ServiceName 2>&1 | Out-Null
    Start-Sleep -Seconds 1
    if (((& sc.exe query $ServiceName 2>&1 | Out-String) -notmatch '1060')) {
        Fail "the service is still registered after sc delete"
    }
    Write-Host "  service removed"
} else {
    Write-Host "  the service was not registered"
}

# The installed-programs entry is removed even when the files are kept for diagnosis: an entry that
# points at a missing uninstaller is worse than no entry at all.
Step 'remove the installed-programs entry'
$uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\MyRamDisk'
if (Test-Path -LiteralPath $uninstallKey) {
    Remove-Item -LiteralPath $uninstallKey -Recurse -Force
}

Step "remove $InstallDirectory"
if (Test-Path -LiteralPath $InstallDirectory) {
    Remove-Item -LiteralPath $InstallDirectory -Recurse -Force
}

if ($PurgeData) {
    Step "purge $DataDirectory"
    if (Test-Path -LiteralPath $DataDirectory) {
        Remove-Item -LiteralPath $DataDirectory -Recurse -Force
    }
} else {
    Write-Host "  kept $DataDirectory (the record of what the disks were); -PurgeData removes it"
}

Write-Host 'UNINSTALLED'
exit 0
