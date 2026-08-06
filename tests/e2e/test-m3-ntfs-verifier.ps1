[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3VerifierTest {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m3-ntfs-verifier-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$reportPath = Join-Path $root 'report.json'
$badPath = Join-Path $root 'bad.json'
$scriptPath = Join-Path $PSScriptRoot '..\..\tools\Verify-M3NtfsJson.ps1'
$commit = '0123456789abcdef0123456789abcdef01234567'
$serverHash = ('A' * 64)
$imageHash = ('B' * 64)

try {
    $report = [ordered]@{
        schema = 'myrd.m3.ntfs-workload.v1'
        phase = 'All'
        status = 'PASS'
        terminal_verdict = 'PASS'
        what_if = $false
        server_started = $true
        destructive_operations_emitted = $true
        source_commit = $commit
        executable_sha256 = [ordered]@{ server = $serverHash; image = $imageHash }
        cleanup = [ordered]@{ succeeded = $true; residuals = @(); entries = @() }
        workload = [ordered]@{
            first_checkpoint_generation = 1
            second_checkpoint_generation = 2
            compact_generation = 2
            server_reopen_verified = $true
            workload = [ordered]@{
                files = @(
                    [ordered]@{ path = 'X:\\myrd-mount\\data\\file-000.bin'; bytes = 4096; sha256 = ('C' * 64) },
                    [ordered]@{ path = 'X:\\myrd-mount\\renamed\\file-001.bin'; bytes = 4096; sha256 = ('D' * 64) }
                )
                sparse = [ordered]@{ path = 'X:\\myrd-mount\\data\\sparse.bin'; bytes = 67108864; sha256 = ('E' * 64) }
            }
            lazy_evidence = [ordered]@{
                bounded_sample = $true
                full_corpus_verified = $true
                startup_status = [ordered]@{ storage = [ordered]@{ backing_reads_total = 10; uncached_reads_total = 2 } }
                sample_status = [ordered]@{ storage = [ordered]@{ backing_reads_total = 11; uncached_reads_total = 2 } }
            }
            native_vhdx = [ordered]@{ filesystem = 'NTFS'; disk_number = 42; generation = 2; path = 'X:\\base.vhdx' }
        }
    }
    $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $reportPath -Encoding utf8
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath `
        -Path $reportPath -ExpectedSourceCommit $commit `
        -ExpectedServerSha256 $serverHash -ExpectedImageSha256 $imageHash 2>&1)
    Require-M3VerifierTest ($LASTEXITCODE -eq 0) `
        "valid report was rejected: $($output -join [Environment]::NewLine)"

    $bad = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    $bad.what_if = $true
    $bad | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $badPath -Encoding utf8
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $badOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath `
        -Path $badPath -ExpectedSourceCommit $commit `
        -ExpectedServerSha256 $serverHash -ExpectedImageSha256 $imageHash 2>&1)
    $badExit = $LASTEXITCODE
    $ErrorActionPreference = $saved
    Require-M3VerifierTest ($badExit -ne 0) `
        "invalid WhatIf report was accepted: $($badOutput -join [Environment]::NewLine)"
    Write-Output 'M3 NTFS verifier: PASS'
}
finally {
    if ([IO.Directory]::Exists($root)) { Remove-Item -LiteralPath $root -Recurse -Force }
}
