[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3NtfsTest {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m3-ntfs-test-' + [Guid]::NewGuid().ToString('N'))
$workspace = Join-Path $root 'workspace'
$scratch = Join-Path $root 'scratch'
$evidence = Join-Path $root 'evidence'
$bundle = Join-Path $scratch 'bundle'
$mount = Join-Path $scratch 'mount'
foreach ($path in @($workspace, $scratch, $evidence, $bundle, $mount)) {
    [IO.Directory]::CreateDirectory($path) | Out-Null
}

try {
    $scriptPath = Join-Path $PSScriptRoot 'Invoke-M3NtfsWorkload.ps1'
    $identityPath = Join-Path $root 'approved-scratch-identity.json'
    [pscustomobject]@{
        Number = 42
        SizeBytes = [uint64]68719476736
        BusType = 'SCSI'
        UniqueId = 'myrd-test-unique'
        SerialNumber = 'myrd-test-serial'
        DeviceId = 'myrd-test-device'
        IsBoot = $false
        IsSystem = $false
        IsPageFile = $false
        IsClustered = $false
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $identityPath -Encoding utf8

    $before = @(Get-ChildItem -LiteralPath $root -Force | Select-Object -ExpandProperty Name)
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath `
        -Phase Preflight -Configuration Release `
        -EvidenceDirectory $evidence `
        -ServerExecutable $env:ComSpec `
        -ImageExecutable $env:ComSpec `
        -BundleRoot $bundle -ManifestName 'test.rdm' `
        -ScratchRoot $scratch -MountDirectory $mount `
        -ExpectedScratchDiskIdentityJson $identityPath `
        -WhatIf 2>&1)
    $childExit = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorAction
    if ($childExit -ne 0) {
        Write-Output ($output | ForEach-Object { $_.ToString() })
    }
    Require-M3NtfsTest ($childExit -eq 0) `
        "Preflight WhatIf failed: $($output -join [Environment]::NewLine)"

    $reportPath = Join-Path $evidence 'm3-ntfs-report.json'
    Require-M3NtfsTest ([IO.File]::Exists($reportPath)) 'NTFS preflight report missing'
    $report = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    Require-M3NtfsTest ($report.schema -eq 'myrd.m3.ntfs-workload.v1') `
        'NTFS workload schema is incorrect'
    Require-M3NtfsTest ($report.status -eq 'PASS') `
        'WhatIf preflight did not pass'
    Require-M3NtfsTest ([bool]$report.what_if) 'WhatIf was not recorded'
    Require-M3NtfsTest (-not [bool]$report.server_started) `
        'WhatIf started the AIM server'
    Require-M3NtfsTest (-not [bool]$report.destructive_operations_emitted) `
        'WhatIf emitted destructive operations'
    $after = @(Get-ChildItem -LiteralPath $root -Force | Select-Object -ExpandProperty Name)
    Require-M3NtfsTest (($before -join '|') -eq ($after -join '|')) `
        'WhatIf changed the test root'

    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $blockedOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath `
        -Phase All -Configuration Release `
        -EvidenceDirectory $evidence `
        -ServerExecutable $env:ComSpec `
        -ImageExecutable $env:ComSpec `
        -BundleRoot $bundle -ManifestName 'test.rdm' `
        -ScratchRoot $scratch -MountDirectory $mount `
        -ExpectedScratchDiskIdentityJson $identityPath `
        -SafetyAcknowledged 2>&1)
    $blockedExit = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorAction
    Require-M3NtfsTest ($blockedExit -eq 77) `
        "unsafe real preflight was not BLOCKED: $($blockedOutput -join [Environment]::NewLine)"
    $blockedReport = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    Require-M3NtfsTest ($blockedReport.status -eq 'BLOCKED') `
        'unsafe real preflight did not publish BLOCKED evidence'
    Require-M3NtfsTest (-not [bool]$blockedReport.server_started) `
        'unsafe real preflight started the AIM server'

    Write-Output 'M3 NTFS workload preflight: PASS'
}
finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
