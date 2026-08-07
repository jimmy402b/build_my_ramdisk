[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3RealCrashTest {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$runner = Join-Path $PSScriptRoot 'Invoke-M3RealCrashScenario.ps1'
$matrix = Join-Path $repoRoot 'tests\fixtures\m3\real-crash-matrix.json'
$scratch = Join-Path ([IO.Path]::GetTempPath()) `
    ('myrd-m3-real-crash-contract-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $runner -Action Validate -MatrixPath $matrix `
        -EvidenceDirectory $scratch 2>&1)
    Require-M3RealCrashTest ($LASTEXITCODE -eq 0) `
        "real crash contract validation failed: $($output -join [Environment]::NewLine)"
    $reportPath = Join-Path $scratch 'm3-real-crash-validation.json'
    Require-M3RealCrashTest ([IO.File]::Exists($reportPath)) `
        'real crash validation report is missing'
    $report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    Require-M3RealCrashTest `
        ([string]$report.schema -eq 'myrd.m3.real-crash-validation.v1') `
        'real crash validation schema is invalid'
    Require-M3RealCrashTest ([string]$report.status -eq 'PASS') `
        'real crash validation status is not PASS'
    Require-M3RealCrashTest ([int]$report.scenario_count -eq 10) `
        'real crash matrix must contain ten required scenarios'
    Require-M3RealCrashTest ([int]$report.process_kill_count -eq 8) `
        'real crash matrix must contain eight process-kill scenarios'
    Require-M3RealCrashTest ([int]$report.controlled_power_cut_count -eq 2) `
        'real crash matrix must contain two controlled-power-cut scenarios'
    Require-M3RealCrashTest ([int]$report.old_count -eq 4) `
        'real crash matrix old-generation coverage is incomplete'
    Require-M3RealCrashTest ([int]$report.old_or_new_count -eq 1) `
        'real crash matrix indeterminate-window coverage is incomplete'
    Require-M3RealCrashTest ([int]$report.new_count -eq 5) `
        'real crash matrix committed-generation coverage is incomplete'
    Require-M3RealCrashTest `
        ([string]$report.matrix_sha256 -match '^[0-9A-Fa-f]{64}$') `
        'real crash matrix hash is invalid'
    Write-Output 'M3 real crash contract test PASS'
}
finally {
    if ([IO.Directory]::Exists($scratch)) {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
}
