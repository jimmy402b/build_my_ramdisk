[CmdletBinding()]
param(
    [ValidateSet('Local', 'Preflight', 'RealFunctional', 'Crash', 'Soak')]
    [string]$Tier = 'Local',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$EvidenceBase = '',
    [string]$Executable = '',
    [string]$ScratchRoot = '',
    [string]$ExpectedDiskIdentityJson = '',
    [string]$ExpectedDriverService = 'phdskmnt',
    [switch]$WhatIf,
    [switch]$ListCommands
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot

function Require-M3Route {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

if ($ListCommands) {
    Write-Output 'Local: Debug/Release CTest regression only'
    Write-Output 'Preflight: read-only host/artifact/scratch/disk safety probe'
    Write-Output 'RealFunctional: BLOCKED until Task 41 is complete'
    Write-Output 'Crash: BLOCKED until Task 42 is complete'
    Write-Output 'Soak: BLOCKED until Task 43 is complete'
    exit 0
}

if ($Tier -eq 'Local') {
    $buildDirectory = Join-Path $repoRoot 'out\build\msvc'
    Require-M3Route ([IO.Directory]::Exists($buildDirectory)) "build directory is missing: $buildDirectory"
    $ctest = Get-Command ctest.exe -ErrorAction SilentlyContinue
    Require-M3Route ($null -ne $ctest) 'ctest.exe is not available'
    & $ctest.Source '--test-dir' $buildDirectory '-C' $Configuration '--output-on-failure'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Output "M3 Local PASS: $Configuration CTest"
    exit 0
}

if ($Tier -ne 'Preflight') {
    Write-Output ([ordered]@{
        schema = 'myrd.m3.verification-route.v1'
        tier = $Tier
        status = 'BLOCKED'
        reason = "${Tier} implementation belongs to a later M3 task"
        required_skip = $true
    } | ConvertTo-Json -Depth 5 -Compress)
    exit 77
}

Require-M3Route (-not [string]::IsNullOrWhiteSpace($EvidenceBase)) 'EvidenceBase is required for Preflight'
Require-M3Route (-not [string]::IsNullOrWhiteSpace($Executable)) 'Executable is required for Preflight'
Require-M3Route (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) 'ScratchRoot is required for Preflight'
Require-M3Route (-not [string]::IsNullOrWhiteSpace($ExpectedDiskIdentityJson)) `
    'ExpectedDiskIdentityJson is required for Preflight'
$evidenceBaseFull = [IO.Path]::GetFullPath($EvidenceBase)
Require-M3Route ([IO.Directory]::Exists($evidenceBaseFull)) 'EvidenceBase must already exist'
$repoFull = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
Require-M3Route (-not $evidenceBaseFull.TrimEnd('\').StartsWith(
        $repoFull, [StringComparison]::OrdinalIgnoreCase)) `
    'EvidenceBase cannot be inside the repository'
$run = Join-Path $evidenceBaseFull ('preflight-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff') + '-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($run) | Out-Null

$scriptPath = Join-Path $repoRoot 'tests\e2e\run-m3-persistence.ps1'
$arguments = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scriptPath,
    '-Tier', 'Preflight', '-Configuration', $Configuration,
    '-EvidenceDirectory', $run, '-Executable', ([IO.Path]::GetFullPath($Executable)),
    '-ScratchRoot', ([IO.Path]::GetFullPath($ScratchRoot)),
    '-ExpectedDiskIdentityJson', ([IO.Path]::GetFullPath($ExpectedDiskIdentityJson)),
    '-ExpectedDriverService', $ExpectedDriverService
)
if ($WhatIf) { $arguments += '-WhatIf' }
& powershell.exe @arguments
$childExit = $LASTEXITCODE
if ($childExit -ne 0) { exit $childExit }
Write-Output "M3 Preflight PASS: $(Join-Path $run 'preflight.json')"
exit 0
