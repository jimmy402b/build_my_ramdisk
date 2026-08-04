[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Verifier
)

$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-ExpectedFailure {
    param([string[]]$Arguments, [string]$Expected)
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Verifier @Arguments 2>&1)
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    Require ($code -ne 0) "Verifier unexpectedly succeeded: $($output -join [Environment]::NewLine)"
    Require (($output -join [Environment]::NewLine) -match [regex]::Escape($Expected)) "Unexpected verifier failure: $($output -join [Environment]::NewLine)"
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m2-json-verifier-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $output = @(& $Executable --local-fake --evidence-dir $root 2>&1)
    Require ($LASTEXITCODE -eq 0) "local fake failed: $($output -join [Environment]::NewLine)"
    $match = [regex]::Match(($output -join [Environment]::NewLine), 'M2 local fake report:\s*(.+\.json)\s*$')
    Require $match.Success 'local fake report path was not printed'
    $reportPath = $match.Groups[1].Value.Trim()

    $valid = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Verifier -Path $reportPath -Tier M2-V4 -Executable $Executable 2>&1)
    Require ($LASTEXITCODE -eq 0) "valid report was rejected: $($valid -join [Environment]::NewLine)"
    Require (($valid -join [Environment]::NewLine) -match 'M2 JSON validation PASS') 'verifier did not print PASS'

    $tamperedPath = Join-Path $root 'tampered.json'
    $tampered = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    $tampered.io.bytes_written = 1
    $tampered | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $tamperedPath -Encoding utf8
    Invoke-ExpectedFailure -Arguments @('-Path', $tamperedPath, '-Tier', 'M2-V4', '-Executable', $Executable) -Expected '4 GiB'

    $shortDeletePath = Join-Path $root 'short-delete.json'
    $shortDelete = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    $shortDelete.io.bytes_deleted = 1
    $shortDelete | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $shortDeletePath -Encoding utf8
    Invoke-ExpectedFailure -Arguments @('-Path', $shortDeletePath, '-Tier', 'M2-V4', '-Executable', $Executable) -Expected '1 GiB'

    $missingRetrimUnmapPath = Join-Path $root 'missing-retrim-unmap.json'
    $missingRetrimUnmap = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    $afterWriteUnmap = @($missingRetrimUnmap.store_stats_samples | Where-Object { $_.sample_point -eq 'AFTER-WRITE' })[0].stats.unmap_bytes_total
    @($missingRetrimUnmap.store_stats_samples | Where-Object { $_.sample_point -eq 'AFTER-RETRIM' })[0].stats.unmap_bytes_total = $afterWriteUnmap
    $missingRetrimUnmap | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $missingRetrimUnmapPath -Encoding utf8
    Invoke-ExpectedFailure -Arguments @('-Path', $missingRetrimUnmapPath, '-Tier', 'M2-V4', '-Executable', $Executable) -Expected 'UNMAP bytes did not increase during Delete/ReTrim'

    Invoke-ExpectedFailure -Arguments @('-Path', $reportPath, '-Tier', 'M2-V5', '-Executable', $Executable) -Expected 'REAL'
    Invoke-ExpectedFailure -Arguments @('-Path', $reportPath, '-Tier', 'M2-V6', '-Executable', $Executable) -Expected 'not implemented'
    Invoke-ExpectedFailure -Arguments @('-Path', $reportPath, '-Tier', 'M2-V7', '-Executable', $Executable) -Expected 'not implemented'
}
finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}

Write-Output 'M2 JSON verifier tests PASS'
