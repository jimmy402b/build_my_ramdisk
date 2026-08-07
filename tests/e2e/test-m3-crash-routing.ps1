[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$CrashModelExecutable = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3CrashRouteTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$route = Join-Path $repoRoot 'tools\Invoke-M3Verification.ps1'
if ([string]::IsNullOrWhiteSpace($CrashModelExecutable)) {
    $CrashModelExecutable = Join-Path $repoRoot `
        "out\build\msvc\tests\$Configuration\myrd_m3_checkpoint_crash_model_tests.exe"
}
$CrashModelExecutable = [IO.Path]::GetFullPath($CrashModelExecutable)
Require-M3CrashRouteTest ([IO.File]::Exists($CrashModelExecutable)) `
    "crash-model executable is missing: $CrashModelExecutable"

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('myrd-m3-crash-route-' + [Guid]::NewGuid().ToString('N'))
Require-M3CrashRouteTest (-not [IO.Directory]::Exists($tempRoot)) `
    "temporary root unexpectedly exists: $tempRoot"
[IO.Directory]::CreateDirectory($tempRoot) | Out-Null

try {
    $blocked = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $route `
        -Tier Crash -Configuration $Configuration 2>&1)
    $blockedExit = $LASTEXITCODE
    Require-M3CrashRouteTest ($blockedExit -eq 77) `
        "default Crash route must remain BLOCKED/77, got $blockedExit"
    $blockedJson = ($blocked -join [Environment]::NewLine) | ConvertFrom-Json
    Require-M3CrashRouteTest ([string]$blockedJson.status -eq 'BLOCKED') `
        'default Crash route did not emit BLOCKED JSON'
    Require-M3CrashRouteTest ([bool]$blockedJson.required_skip) `
        'default Crash route did not set required_skip'

    $evidenceBase = Join-Path $tempRoot 'evidence'
    [IO.Directory]::CreateDirectory($evidenceBase) | Out-Null
    $simulated = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $route `
        -Tier Crash -CrashMode Simulated -Configuration $Configuration `
        -EvidenceBase $evidenceBase -CrashExecutable $CrashModelExecutable 2>&1)
    $simulatedExit = $LASTEXITCODE
    Require-M3CrashRouteTest ($simulatedExit -eq 0) `
        "simulated Crash route failed with ${simulatedExit}: $($simulated -join [Environment]::NewLine)"
    $simulatedText = $simulated -join [Environment]::NewLine
    Require-M3CrashRouteTest ($simulatedText -match 'M3 Crash Simulated PASS') `
        'simulated Crash route did not emit PASS summary'
    $runs = @(Get-ChildItem -LiteralPath $evidenceBase -Directory)
    Require-M3CrashRouteTest ($runs.Count -eq 1) `
        "expected one simulated evidence run, found $($runs.Count)"
    $reportPath = Join-Path $runs[0].FullName 'm3-crash-report.json'
    Require-M3CrashRouteTest ([IO.File]::Exists($reportPath)) `
        'simulated Crash route did not publish crash report'
    $report = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    Require-M3CrashRouteTest ([string]$report.status -eq 'PASS') `
        'simulated Crash report is not PASS'
    Require-M3CrashRouteTest (-not [bool]$report.task42_complete -and `
        -not [bool]$report.milestone_eligible) `
        'simulated Crash route overstated Task42 or milestone completion'
}
finally {
    $fullRoot = [IO.Path]::GetFullPath($tempRoot)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    Require-M3CrashRouteTest ($fullRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) `
        'test cleanup escaped the system temporary directory'
    if ([IO.Directory]::Exists($fullRoot)) {
        [IO.Directory]::Delete($fullRoot, $true)
    }
}

Write-Output "M3 crash routing tests PASS: $Configuration"
