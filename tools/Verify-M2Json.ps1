[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [ValidateSet('M2-V4', 'M2-V5', 'M2-V6', 'M2-V7')]
    [string]$Tier = 'M2-V5',
    [string]$Executable = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot

function Require {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Require-Property {
    param($Object, [string]$Name)
    Require ($null -ne $Object) "object missing while checking $Name"
    Require ($null -ne $Object.PSObject.Properties[$Name]) "required property missing: $Name"
}

function Same-Set {
    param($Left, $Right)
    $a = @($Left | ForEach-Object { [string]$_ } | Sort-Object)
    $b = @($Right | ForEach-Object { [string]$_ } | Sort-Object)
    return (($a -join [Environment]::NewLine) -eq ($b -join [Environment]::NewLine))
}

Require ($Tier -in @('M2-V4', 'M2-V5')) 'M2-V6 and M2-V7 are not implemented; Task 22 evidence is required'

$reportPath = [IO.Path]::GetFullPath($Path)
Require ([IO.File]::Exists($reportPath)) "M2 report not found: $reportPath"
Require (-not $reportPath.StartsWith('\\.\', [StringComparison]::OrdinalIgnoreCase)) 'M2 report path must not be a device path'
$raw = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8
Require (-not [string]::IsNullOrWhiteSpace($raw)) 'M2 report is empty'
$report = $raw | ConvertFrom-Json

foreach ($name in @('schema_version', 'run', 'build_toolchain', 'accepted_m1_baseline', 'machine_os', 'state_before', 'state_after', 'geometry_options', 'state', 'phases', 'io', 'store_stats_samples', 'os_memory_samples', 'performance', 'device_identity', 'cleanup_actions', 'final_baseline_comparison', 'failures', 'verdict')) {
    Require-Property $report $name
}
Require ([int]$report.schema_version -eq 1) 'schema_version must be 1'
Require ([string]$report.verdict -eq 'GO') 'M2 report verdict is not GO'
Require (@($report.failures).Count -eq 0) 'GO report contains failures'

$expectedMode = if ($Tier -eq 'M2-V4') { 'FAKE' } else { 'REAL' }
Require ([string]$report.run.mode -eq $expectedMode) "$Tier requires $expectedMode report mode"
Require (-not [string]::IsNullOrWhiteSpace([string]$report.run.run_id)) 'run_id is empty'
Require ([string]$report.run.started_utc -match '^\d{4}-\d{2}-\d{2}T') 'started_utc is invalid'
Require ([string]$report.run.ended_utc -match '^\d{4}-\d{2}-\d{2}T') 'ended_utc is invalid'

Require ([bool]$report.accepted_m1_baseline.accepted) 'accepted M1 baseline is missing'
Require ([string]$report.accepted_m1_baseline.m1_verdict -eq 'GO') 'accepted M1 baseline is not GO'
Require ([string]$report.state_before.driver_sha256 -match '^[0-9A-Fa-f]{64}$') 'before driver SHA-256 is invalid'
Require ([string]$report.state_after.driver_sha256 -match '^[0-9A-Fa-f]{64}$') 'after driver SHA-256 is invalid'

Require ([uint64]$report.geometry_options.disk_size_bytes -eq [uint64]68719476736) 'logical disk size is not exactly 64 GiB'
Require ([uint64]$report.geometry_options.sector_size_bytes -eq [uint64]512) 'sector size is not 512'
Require ([uint64]$report.geometry_options.block_size_bytes -eq [uint64]65536) 'block size is not 64 KiB'
Require (-not [bool]$report.state.durable_successor_installed) 'M2 must not claim a durable successor'
Require (-not [bool]$report.state.durable_successor_generation_present) 'M2 must not expose a durable successor generation'
Require ([uint64]$report.state.initial_backing_generation -eq [uint64]0) 'M2 initial backing generation is not zero'
Require ([uint64]$report.state.current_backing_generation -eq [uint64]0) 'M2 current backing generation is not zero'

Require ([uint64]$report.io.bytes_written -ge [uint64]4294967296) 'verified write evidence is below 4 GiB'
Require ([uint64]$report.io.bytes_read -ge [uint64]4294967296) 'verified read evidence is below 4 GiB'
Require ([uint64]$report.io.bytes_deleted -ge [uint64]1073741824) 'verified delete evidence is below 1 GiB'
Require ([string]$report.io.write_sha256 -match '^[0-9A-Fa-f]{64}$') 'write SHA-256 is invalid'
Require ([string]$report.io.write_sha256 -eq [string]$report.io.read_sha256) 'write/read SHA-256 values differ'

$storeSamples = @($report.store_stats_samples)
$memorySamples = @($report.os_memory_samples)
Require ($storeSamples.Count -eq 4) 'exactly four store samples are required'
Require ($memorySamples.Count -eq 4) 'exactly four OS memory samples are required'
$expectedPoints = @('READY', 'AFTER-WRITE', 'AFTER-RETRIM', 'FINAL')
Require (Same-Set @($storeSamples | ForEach-Object { $_.sample_point }) $expectedPoints) 'store sample points are incomplete'
Require (Same-Set @($memorySamples | ForEach-Object { $_.memory_sample_point }) $expectedPoints) 'OS memory sample points are incomplete'
$ready = @($storeSamples | Where-Object { $_.sample_point -eq 'READY' })
$afterWrite = @($storeSamples | Where-Object { $_.sample_point -eq 'AFTER-WRITE' })
$afterRetrim = @($storeSamples | Where-Object { $_.sample_point -eq 'AFTER-RETRIM' })
Require ($ready.Count -eq 1 -and $afterWrite.Count -eq 1 -and $afterRetrim.Count -eq 1) 'store samples are ambiguous'
Require ([uint64]$ready[0].stats.committed_bytes -le [uint64]134217728) 'empty-disk committed memory exceeds 128 MiB'
$verifiedWriteBytes = [uint64]$report.io.bytes_written
$postWriteCeiling = [uint64]($verifiedWriteBytes + [uint64]($verifiedWriteBytes / 10) + [uint64]134217728)
Require ([uint64]$afterWrite[0].stats.committed_bytes -le $postWriteCeiling) 'post-write committed memory exceeds verified bytes + 10% + 128 MiB'
Require ([uint64]$afterRetrim[0].stats.unmap_bytes_total -gt [uint64]$afterWrite[0].stats.unmap_bytes_total) 'UNMAP bytes did not increase during Delete/ReTrim'
Require ([uint64]$afterRetrim[0].stats.committed_bytes -lt [uint64]$afterWrite[0].stats.committed_bytes) 'committed memory did not decrease after ReTrim'

Require ([string]$report.performance.gate_status -eq 'PASS') 'performance gate did not pass'
Require ([bool]$report.performance.comparison.overall_pass) 'performance comparison did not pass'
Require ([uint64]$report.performance.comparison.sequential_threshold_basis_points -eq [uint64]7000) 'sequential threshold changed'
Require ([uint64]$report.performance.comparison.random_threshold_basis_points -eq [uint64]6000) 'random threshold changed'
Require ([uint64]$report.performance.comparison.sequential_ratio_basis_points -ge [uint64]7000) 'sequential performance is below 70%'
Require ([uint64]$report.performance.comparison.random_ratio_basis_points -ge [uint64]6000) 'random performance is below 60%'

Require ([bool]$report.device_identity.confirmed_by_query_device) 'device was not confirmed by QUERY_DEVICE'
Require ([uint64]$report.device_identity.disk_length_bytes -eq [uint64]68719476736) 'confirmed disk length is not 64 GiB'
Require ([string]$report.device_identity.device_id_raw -match '^\d+$') 'exact device id is invalid'
Require ([string]$report.device_identity.device_id_raw -ne '16777215') 'device id is the all-device sentinel'

$cleanup = @($report.cleanup_actions)
Require ($cleanup.Count -eq 4) 'exactly four cleanup actions are required'
foreach ($action in $cleanup) {
    Require ([bool]$action.cleanup_attempted) "cleanup was not attempted: $($action.cleanup_action_id)"
    Require ([string]$action.cleanup_result -eq 'SUCCEEDED') "cleanup failed: $($action.cleanup_action_id)"
}
$baseline = $report.final_baseline_comparison
Require ([string]$baseline.comparison -eq 'MATCH') 'final baseline comparison is not MATCH'
Require ([bool]$baseline.exact_match) 'final inventory is not an exact match'
Require ([bool]$baseline.resources_within_tolerance) 'final resources exceed tolerance'
Require (@($baseline.diff.added).Count -eq 0 -and @($baseline.diff.removed).Count -eq 0 -and @($baseline.diff.changed).Count -eq 0) 'final inventory diff is not empty'

if ([string]::IsNullOrWhiteSpace($Executable)) {
    $configuration = [string]$report.build_toolchain.configuration
    if ($configuration -notin @('Debug', 'Release')) { $configuration = 'Release' }
    $Executable = Join-Path $repoRoot "out\build\msvc\src\$configuration\myrd-m2-e2e.exe"
}
$executablePath = [IO.Path]::GetFullPath($Executable)
Require ([IO.File]::Exists($executablePath)) "M2 validator executable not found: $executablePath"
$validationOutput = @(& $executablePath --validate-report $reportPath 2>&1)
Require ($LASTEXITCODE -eq 0) "native M2 semantic validation failed: $($validationOutput -join [Environment]::NewLine)"

Write-Output "M2 JSON validation PASS [$Tier]: $reportPath"
