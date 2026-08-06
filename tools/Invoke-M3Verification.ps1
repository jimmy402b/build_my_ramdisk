[CmdletBinding()]
param(
    [ValidateSet('Local', 'Preflight', 'RealFunctional', 'Crash', 'Soak')]
    [string]$Tier = 'Local',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$EvidenceBase = '',
    [string]$Executable = '',
    [string]$ServerExecutable = '',
    [string]$BundleRoot = '',
    [string]$ManifestName = '',
    [string]$MountDirectory = '',
    [string]$ScratchRoot = '',
    [string]$ExpectedDiskIdentityJson = '',
    [string]$ExpectedDriverService = 'phdskmnt',
    [switch]$SafetyAcknowledged,
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
    Write-Output 'RealFunctional: real AIM/NTFS/checkpoint/reopen/compact/native-VHDX acceptance; requires approved scratch identity'
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

if ($Tier -eq 'RealFunctional') {
    if ($WhatIf) {
        Write-Output ([ordered]@{
            schema = 'myrd.m3.verification-route.v1'
            tier = $Tier
            status = 'BLOCKED'
            reason = 'WhatIf is a safety probe, not functional acceptance'
            required_skip = $true
        } | ConvertTo-Json -Depth 5 -Compress)
        exit 77
    }
    $required = @{
        ImageExecutable = $Executable
        ServerExecutable = $ServerExecutable
        BundleRoot = $BundleRoot
        ManifestName = $ManifestName
        ScratchRoot = $ScratchRoot
        MountDirectory = $MountDirectory
        ExpectedScratchDiskIdentityJson = $ExpectedDiskIdentityJson
    }
    foreach ($entry in $required.GetEnumerator()) {
        Require-M3Route (-not [string]::IsNullOrWhiteSpace([string]$entry.Value)) `
            "RealFunctional requires $($entry.Key)"
    }
    Require-M3Route $SafetyAcknowledged 'RealFunctional requires explicit SafetyAcknowledged'
    Require-M3Route (-not [string]::IsNullOrWhiteSpace($EvidenceBase)) 'EvidenceBase is required for RealFunctional'
    $evidenceBaseFull = [IO.Path]::GetFullPath($EvidenceBase)
    Require-M3Route ([IO.Directory]::Exists($evidenceBaseFull)) 'EvidenceBase must already exist'
    $repoFull = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
    Require-M3Route (-not $evidenceBaseFull.TrimEnd('\').StartsWith($repoFull, [StringComparison]::OrdinalIgnoreCase)) `
        'EvidenceBase cannot be inside the repository'
    $run = Join-Path $evidenceBaseFull ('real-functional-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff') + '-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($run) | Out-Null
    $scriptPath = Join-Path $repoRoot 'tests\e2e\Invoke-M3NtfsWorkload.ps1'
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scriptPath,
        '-Phase', 'All', '-Configuration', $Configuration,
        '-EvidenceDirectory', $run,
        '-ServerExecutable', ([IO.Path]::GetFullPath($ServerExecutable)),
        '-ImageExecutable', ([IO.Path]::GetFullPath($Executable)),
        '-BundleRoot', ([IO.Path]::GetFullPath($BundleRoot)),
        '-ManifestName', $ManifestName,
        '-ScratchRoot', ([IO.Path]::GetFullPath($ScratchRoot)),
        '-MountDirectory', ([IO.Path]::GetFullPath($MountDirectory)),
        '-ExpectedScratchDiskIdentityJson', ([IO.Path]::GetFullPath($ExpectedDiskIdentityJson)),
        '-SafetyAcknowledged')
    & powershell.exe @arguments
    $childExit = $LASTEXITCODE
    if ($childExit -ne 0) { exit $childExit }
    $reportPath = Join-Path $run 'm3-ntfs-report.json'
    $verifier = Join-Path $repoRoot 'tools\Verify-M3NtfsJson.ps1'
    $sourceCommit = ((& git.exe -C $repoRoot rev-parse HEAD 2>$null) -join '').Trim()
    Require-M3Route ($sourceCommit -match '^[0-9A-Fa-f]{40}$') 'source commit is unavailable for RealFunctional evidence'
    $serverFull = [IO.Path]::GetFullPath($ServerExecutable)
    $imageFull = [IO.Path]::GetFullPath($Executable)
    $serverHash = (Get-FileHash -LiteralPath $serverFull -Algorithm SHA256).Hash
    $imageHash = (Get-FileHash -LiteralPath $imageFull -Algorithm SHA256).Hash
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verifier `
        -Path $reportPath -ExpectedSourceCommit $sourceCommit `
        -ExpectedServerSha256 $serverHash -ExpectedImageSha256 $imageHash
    $verifierExit = $LASTEXITCODE
    if ($verifierExit -ne 0) { exit $verifierExit }
    Write-Output "M3 RealFunctional PASS: $reportPath"
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
