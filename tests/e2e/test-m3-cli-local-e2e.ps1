[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3LocalE2E {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-M3LocalCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $output = @(& $Executable @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    Require-M3LocalE2E ($exitCode -eq 0) `
        "$Name failed with exit code ${exitCode}: $($output -join [Environment]::NewLine)"
    $line = [string](@($output | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } | Select-Object -Last 1))
    Require-M3LocalE2E (-not [string]::IsNullOrWhiteSpace($line)) "$Name returned no JSON"
    try { $report = $line | ConvertFrom-Json }
    catch { throw "$Name returned invalid JSON: $line" }
    Require-M3LocalE2E ([string]$report.command -eq $Name) "$Name JSON command mismatch"
    Require-M3LocalE2E ([bool]$report.ok) "$Name JSON reports ok=false"
    return $report
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$executable = [IO.Path]::GetFullPath((Join-Path $repoRoot "out\build\msvc\src\$Configuration\myrd_m3_e2e.exe"))
Require-M3LocalE2E ([IO.File]::Exists($executable)) "M3 CLI executable is missing: $executable"

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('myrd-m3-cli-ctest-' + [Guid]::NewGuid().ToString('N'))
$tempRoot = [IO.Path]::GetFullPath($tempRoot)
$tempVolumeRoot = [IO.Path]::GetPathRoot($tempRoot)
$tempPrefix = $tempVolumeRoot.TrimEnd('\') + '\'
Require-M3LocalE2E ($tempRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) `
    'generated local E2E directory escaped the temporary volume'
Require-M3LocalE2E (-not [IO.Directory]::Exists($tempRoot) -and
    -not [IO.File]::Exists($tempRoot)) `
    'generated local E2E directory already exists'

try {
    $bundle = Join-Path $tempRoot 'bundle'
    [IO.Directory]::CreateDirectory($bundle) | Out-Null
    $common = @(
        '--bundle-root', $bundle,
        '--manifest', 'disk.manifest',
        '--timeout-ms', '120000',
        '--max-virtual-bytes', '1073741824',
        '--max-journal-bytes', '134217728',
        '--max-journal-records', '1024',
        '--max-index-cache-bytes', '65536',
        '--max-index-cache-entries', '16',
        '--max-bat-cache-bytes', '65536',
        '--max-bat-cache-entries', '16',
        '--max-io-scratch-bytes', '65536',
        '--max-verify-batch-bytes', '65536')
    $store = @(
        '--store-soft-bytes', '134217728',
        '--store-hard-bytes', '268435456',
        '--store-low-watermark-bytes', '67108864',
        '--store-batch-bytes', '4194304',
        '--store-batch-records', '64',
        '--store-stripes', '64',
        '--snapshot-cow-bytes', '134217728',
        '--snapshot-stall-ms', '250')

    $create = @('create') + $common + @(
        '--virtual-bytes', '67108864',
        '--logical-sector-size', '512',
        '--physical-sector-size', '4096')
    $inspect = @('inspect') + $common
    $verify = @('verify') + $common + @('--full')
    $recover = @('recover-test') + $common + $store

    $null = Invoke-M3LocalCommand $executable 'create' $create
    $null = Invoke-M3LocalCommand $executable 'inspect' $inspect
    $null = Invoke-M3LocalCommand $executable 'verify' $verify
    $null = Invoke-M3LocalCommand $executable 'recover-test' $recover
    Write-Output "M3 local CLI E2E PASS: $Configuration"
    exit 0
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) {
        [IO.Directory]::Delete($tempRoot, $true)
    }
    Require-M3LocalE2E (-not [IO.Directory]::Exists($tempRoot)) `
        'local CLI E2E temporary directory remained after cleanup'
}
