[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'M3Harness.Common.ps1')

function Require-M3Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Expect-M3Failure {
    param([scriptblock]$Action, [string]$Message)
    $caught = $null
    try { & $Action } catch { $caught = [string]$_.Exception.Message }
    Require-M3Test ($null -ne $caught) "expected failure: $Message"
    Require-M3Test ($caught -match [regex]::Escape($Message)) `
        "wrong failure: expected=$Message actual=$caught"
}

function New-TestDiskIdentity {
    param([int]$Number = 42)
    return [pscustomobject]@{
        Number = $Number
        SizeBytes = [uint64]68719476736
        BusType = 'SCSI'
        UniqueId = 'myrd-unique'
        SerialNumber = 'myrd-serial'
        DeviceId = 'myrd-device'
        IsBoot = $false
        IsSystem = $false
        IsPageFile = $false
        IsClustered = $false
    }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m3-harness-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $workspace = Join-Path $root 'workspace'
    $scratch = Join-Path $root 'scratch'
    $evidence = Join-Path $root 'evidence'
    foreach ($path in @($workspace, $scratch, $evidence)) {
        [IO.Directory]::CreateDirectory($path) | Out-Null
    }
    $identityPath = Join-Path $root 'disk-identity.json'
    New-TestDiskIdentity | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $identityPath -Encoding utf8

    $identity = Read-M3DiskIdentityJson $identityPath
    Require-M3Test ($identity.Number -eq 42) 'identity parser changed disk number'
    Expect-M3Failure { ConvertTo-M3DiskIdentity (New-TestDiskIdentity 0) } 'Disk 0'
    $wildcard = New-TestDiskIdentity
    $wildcard.DeviceId = 'disk-*'
    Expect-M3Failure { ConvertTo-M3DiskIdentity $wildcard } 'wildcard'
    $systemDisk = New-TestDiskIdentity
    $systemDisk.IsSystem = $true
    Expect-M3Failure { ConvertTo-M3DiskIdentity $systemDisk } 'IsSystem'
    $incomplete = New-TestDiskIdentity
    $incomplete.PSObject.Properties.Remove('IsBoot')
    Expect-M3Failure { ConvertTo-M3DiskIdentity $incomplete } 'IsBoot'

    $candidate = New-TestDiskIdentity
    $selected = Select-M3ExactDisk -Candidates @($candidate) -ExpectedIdentity $identity
    Require-M3Test ($selected.Number -eq 42) 'exact disk selector returned wrong candidate'
    function Get-Partition {
        param([char]$DriveLetter)
        return [pscustomobject]@{ DiskNumber = 42 }
    }
    function Get-M3ActualDiskCandidates {
        return @(New-TestDiskIdentity 42)
    }
    $bound = Assert-M3ExpectedDiskOwnsPath -ScratchRoot 'C:\myrd-test\' `
        -ExpectedIdentity $identity
    Require-M3Test ($bound.Number -eq 42) 'scratch identity binding returned wrong disk'
    function Get-Partition {
        param([char]$DriveLetter)
        return [pscustomobject]@{ DiskNumber = 43 }
    }
    Expect-M3Failure {
        Assert-M3ExpectedDiskOwnsPath -ScratchRoot 'C:\myrd-test\' `
            -ExpectedIdentity $identity
    } 'ScratchRoot is not located on the approved exact disk identity'
    . (Join-Path $PSScriptRoot 'M3Harness.Common.ps1')
    Expect-M3Failure { Select-M3ExactDisk -Candidates @() -ExpectedIdentity $identity } 'found 0'
    Expect-M3Failure {
        Select-M3ExactDisk -Candidates @($candidate, $candidate) -ExpectedIdentity $identity
    } 'found 2'
    $mismatch = New-TestDiskIdentity
    $mismatch.DeviceId = 'other-device'
    Expect-M3Failure { Select-M3ExactDisk -Candidates @($mismatch) -ExpectedIdentity $identity } 'found 0'

    Expect-M3Failure {
        Assert-M3ScratchDirectory -ScratchRoot $workspace -WorkspaceRoot $workspace -WhatIf
    } 'workspace root'
    Expect-M3Failure {
        Assert-M3SafeTargetPath -TargetPath (Join-Path $root 'outside') `
            -ScratchRoot $scratch -WorkspaceRoot $workspace
    } 'outside the explicit scratch root'
    Expect-M3Failure {
        Assert-M3SafeTargetPath -TargetPath (Join-Path $scratch 'disk-*.vhdx') `
            -ScratchRoot $scratch -WorkspaceRoot $workspace
    } 'wildcard path'

    $beforeScratch = @([IO.Directory]::GetFileSystemEntries($scratch))
    $preflight = Invoke-M3Preflight -Executable $env:ComSpec `
        -ScratchRoot $scratch -WorkspaceRoot $workspace `
        -ExpectedDiskIdentity $identity -WhatIf
    Require-M3Test ($preflight.status -eq 'PASS') 'safe WhatIf preflight did not pass'
    Require-M3Test (-not $preflight.exact_disk_mutation_permitted) `
        'WhatIf permitted exact disk mutation'
    Require-M3Test (@($preflight.mutation_commands).Count -eq 0) `
        'WhatIf emitted a mutation command'
    $afterScratch = @([IO.Directory]::GetFileSystemEntries($scratch))
    Require-M3Test (($beforeScratch -join '|') -eq ($afterScratch -join '|')) `
        'WhatIf changed scratch contents'

    $realProbe = Invoke-M3Preflight -Executable $env:ComSpec `
        -ScratchRoot $scratch -WorkspaceRoot $workspace `
        -ExpectedDiskIdentity $identity
    Require-M3Test ($realProbe.status -eq 'BLOCKED') `
        'preflight accepted an identity that was not matched to a real disk'
    Require-M3Test (-not $realProbe.static_checks.actual_disk_identity_match) `
        'unmatched disk identity was reported as matched'
    Require-M3Test ($realProbe.reason -match 'exact disk identity probe failed') `
        'unmatched disk identity reason was not recorded'

    $ledger = New-M3CleanupLedger
    $count = 0
    Add-M3CleanupEntry -Ledger $ledger -Kind 'marker' -Description 'test marker' `
        -RecoveryCommand 'remove the test marker' -Action { $script:count++ }
    $firstCleanup = Invoke-M3CleanupLedger $ledger
    $secondCleanup = Invoke-M3CleanupLedger $ledger
    Require-M3Test ($count -eq 1) 'cleanup ledger was not idempotent'
    Require-M3Test ($firstCleanup[0].Status -eq 'succeeded') 'cleanup did not succeed'
    Require-M3Test ($secondCleanup[0].Status -eq 'succeeded') 'cleanup repeat changed status'

    $script:cleanupOrder = [Collections.ArrayList]::new()
    $orderedLedger = New-M3CleanupLedger
    Add-M3CleanupEntry -Ledger $orderedLedger -Kind 'provider' `
        -Description 'stop provider after dependents' -RecoveryCommand 'stop provider' `
        -Action { [void]$script:cleanupOrder.Add('provider') }
    Add-M3CleanupEntry -Ledger $orderedLedger -Kind 'mount' `
        -Description 'remove dependent mount first' -RecoveryCommand 'remove mount' `
        -Action { [void]$script:cleanupOrder.Add('mount') }
    $null = Invoke-M3CleanupLedger $orderedLedger
    Require-M3Test (($script:cleanupOrder -join '|') -eq 'mount|provider') `
        'cleanup ledger did not execute dependent resources in reverse registration order'

    $harness = Join-Path $PSScriptRoot 'run-m3-persistence.ps1'
    $harnessOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $harness `
        -Tier Preflight -Configuration Release -EvidenceDirectory $evidence `
        -Executable $env:ComSpec -ScratchRoot $scratch `
        -ExpectedDiskIdentityJson $identityPath -WhatIf 2>&1)
    Require-M3Test ($LASTEXITCODE -eq 0) `
        "harness WhatIf failed: $($harnessOutput -join [Environment]::NewLine)"
    $reportPath = Join-Path $evidence 'preflight.json'
    Require-M3Test ([IO.File]::Exists($reportPath)) 'preflight report was not written'
    $report = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    Require-M3Test ($report.terminal_verdict -eq 'PASS') 'harness report did not pass'
    Require-M3Test (-not [bool]$report.exact_disk_mutation_permitted) `
        'harness report permitted disk mutation'

    $route = Join-Path $PSScriptRoot '..\..\tools\Invoke-M3Verification.ps1'
    $listed = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $route -ListCommands 2>&1)
    foreach ($tierName in @('Local', 'Preflight', 'RealFunctional', 'Crash', 'Soak')) {
        Require-M3Test (($listed -join [Environment]::NewLine) -match $tierName) `
            "verification route did not list $tierName"
    }
    $blocked = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $route `
        -Tier RealFunctional -WhatIf 2>&1)
    Require-M3Test ($LASTEXITCODE -eq 77) 'unimplemented tier was not BLOCKED/nonzero'
    Require-M3Test (($blocked -join [Environment]::NewLine) -match 'BLOCKED') `
        'blocked tier did not emit machine-readable BLOCKED'

    $routeOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $route `
        -Tier Preflight -Configuration Release -EvidenceBase $evidence `
        -Executable $env:ComSpec -ScratchRoot $scratch `
        -ExpectedDiskIdentityJson $identityPath -WhatIf 2>&1)
    Require-M3Test ($LASTEXITCODE -eq 0) `
        "verification Preflight failed: $($routeOutput -join [Environment]::NewLine)"
    $runs = @(Get-ChildItem -LiteralPath $evidence -Directory)
    Require-M3Test ($runs.Count -eq 1) "expected one routed run, found $($runs.Count)"
}
finally {
    $fullRoot = Get-M3FullPath $root
    Assert-M3Condition ($fullRoot.StartsWith(
        [IO.Path]::GetFullPath([IO.Path]::GetTempPath()),
        [StringComparison]::OrdinalIgnoreCase)) 'test cleanup escaped temp root'
    if ([IO.Directory]::Exists($fullRoot)) {
        Remove-Item -LiteralPath $fullRoot -Recurse -Force
    }
}

Write-Output 'M3 harness preflight: PASS'
