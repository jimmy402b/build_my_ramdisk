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

function Resolve-AggregateFile {
    param(
        [Parameter(Mandatory = $true)][string]$AggregatePath,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )
    Require (-not [IO.Path]::IsPathRooted($RelativePath)) 'aggregate child path must be relative'
    $root = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($AggregatePath))
    $full = [IO.Path]::GetFullPath((Join-Path $root $RelativePath))
    $prefix = $root.TrimEnd('\') + '\'
    Require ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) 'aggregate child path escaped its evidence directory'
    Require ([IO.File]::Exists($full)) "aggregate child file is missing: $RelativePath"
    return $full
}

function Assert-FileHash {
    param([string]$Path, [string]$Expected)
    Require ($Expected -match '^[0-9A-Fa-f]{64}$') "invalid recorded SHA-256: $Path"
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    Require ($actual -ieq $Expected) "recorded SHA-256 does not match: $Path"
}

function Invoke-NativeM2Validation {
    param([string]$ExecutablePath, [string]$ReportPath)
    $output = @(& $ExecutablePath --validate-report $ReportPath 2>&1)
    Require ($LASTEXITCODE -eq 0) "native M2 semantic validation failed: $($output -join [Environment]::NewLine)"
}

function Invoke-V5ChildValidation {
    param([string]$ExecutablePath, [string]$ReportPath)
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Path $ReportPath -Tier M2-V5 -Executable $ExecutablePath 2>&1)
    Require ($LASTEXITCODE -eq 0) "aggregate V5 child validation failed: $($output -join [Environment]::NewLine)"
    Require (($output -join [Environment]::NewLine) -match 'M2 JSON validation PASS') 'aggregate V5 child verifier did not emit PASS'
}

function Get-AggregateExecutable {
    param($Aggregate)
    Require-Property $Aggregate 'configuration'
    Require ([string]$Aggregate.configuration -eq 'Release') 'M2-V6/V7 requires Release configuration'
    Require-Property $Aggregate 'candidate'
    Require-Property $Aggregate.candidate 'executable_sha256'
    $candidateHash = [string]$Aggregate.candidate.executable_sha256
    Require ($candidateHash -match '^[0-9A-Fa-f]{64}$') 'aggregate candidate SHA-256 is invalid'
    $selected = $Executable
    if ([string]::IsNullOrWhiteSpace($selected)) {
        $selected = Join-Path $repoRoot 'out\build\msvc\src\Release\myrd-m2-e2e.exe'
    }
    $full = [IO.Path]::GetFullPath($selected)
    Require ([IO.File]::Exists($full)) "M2 validator executable not found: $full"
    Require ((Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash -ieq $candidateHash) 'aggregate candidate does not match the selected executable'
    return $full
}

function Read-AggregateChildReport {
    param($Aggregate, [string]$AggregatePath, $Entry, [string]$ExecutablePath)
    foreach ($name in @('report_path', 'report_sha256', 'process_exit_code', 'outer_inventory_path', 'outer_inventory_sha256')) {
        Require-Property $Entry $name
    }
    $reportPath = Resolve-AggregateFile -AggregatePath $AggregatePath -RelativePath ([string]$Entry.report_path)
    Assert-FileHash -Path $reportPath -Expected ([string]$Entry.report_sha256)
    $outerPath = Resolve-AggregateFile -AggregatePath $AggregatePath -RelativePath ([string]$Entry.outer_inventory_path)
    Assert-FileHash -Path $outerPath -Expected ([string]$Entry.outer_inventory_sha256)
    $outer = Get-Content -Raw -LiteralPath $outerPath -Encoding utf8 | ConvertFrom-Json
    Require ([bool]$outer.structural_equal) "child outer inventory did not return to baseline: $reportPath"
    $child = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    Require ([string]$child.build_toolchain.configuration -eq 'Release') 'aggregate child is not a Release report'
    Require ([string]$child.build_toolchain.executable_sha256 -ieq [string]$Aggregate.candidate.executable_sha256) 'aggregate child candidate SHA-256 differs'
    return [pscustomobject]@{
        Path = $reportPath
        Report = $child
    }
}

function Assert-FaultCleanup {
    param($Report, [string]$FailurePoint)
    $expectedPhase = $FailurePoint.ToUpperInvariant()
    Require ([string]$Report.verdict -eq 'NO-GO') "fault report did not fail closed: $FailurePoint"
    $failures = @($Report.failures | Where-Object {
        [string]$_.failure_phase -eq $expectedPhase -and
        [string]$_.failure_message -match 'injected acceptance failure'
    })
    Require ($failures.Count -eq 1) "fault report does not prove the requested injection: $FailurePoint"
    $failedPhases = @($Report.phases | Where-Object {
        [string]$_.phase_id -eq $expectedPhase -and
        [string]$_.phase_status -eq 'FAIL'
    })
    Require ($failedPhases.Count -eq 1) "fault phase is not FAIL: $FailurePoint"
    $required = switch ($FailurePoint) {
        'capture-baseline' { @('RESTORE-SERVICE') }
        'construct-provider' { @('STOP-PROVIDER', 'RESTORE-SERVICE') }
        'start-provider' { @('STOP-PROVIDER', 'RESTORE-SERVICE') }
        'create-device' { @('REMOVE-DEVICE', 'STOP-PROVIDER', 'RESTORE-SERVICE') }
        'wait-for-disk' { @('REMOVE-DEVICE', 'STOP-PROVIDER', 'RESTORE-SERVICE') }
        default { @('REMOVE-MOUNTS', 'REMOVE-DEVICE', 'STOP-PROVIDER', 'RESTORE-SERVICE') }
    }
    foreach ($action in @($Report.cleanup_actions)) {
        $owned = $required -contains [string]$action.cleanup_action_id
        if ($owned) {
            Require ([bool]$action.cleanup_attempted) "owned cleanup was not attempted: $FailurePoint/$($action.cleanup_action_id)"
            Require ([string]$action.cleanup_result -eq 'SUCCEEDED') "owned cleanup failed: $FailurePoint/$($action.cleanup_action_id)"
        }
        else {
            Require (-not [bool]$action.cleanup_attempted) "unowned cleanup was attempted: $FailurePoint/$($action.cleanup_action_id)"
            Require ([string]$action.cleanup_result -eq 'NOT-ATTEMPTED') "unowned cleanup result is invalid: $FailurePoint/$($action.cleanup_action_id)"
        }
    }
    Require ([string]$Report.final_baseline_comparison.comparison -eq 'MATCH') "fault baseline is not MATCH: $FailurePoint"
    Require ([bool]$Report.final_baseline_comparison.exact_match) "fault baseline is not exact: $FailurePoint"
    Require ([bool]$Report.final_baseline_comparison.resources_within_tolerance) "fault resources exceed tolerance: $FailurePoint"
}

function Verify-M2V6Aggregate {
    param($Aggregate, [string]$AggregatePath, [string]$ExecutablePath)
    Require ([string]$Aggregate.evidence_kind -eq 'M2-V6-AGGREGATE') 'M2-V6 evidence_kind is invalid'
    Require ([string]$Aggregate.verdict -eq 'PASS') 'M2-V6 aggregate verdict is not PASS'
    Require-Property $Aggregate 'memory'
    $memory = $Aggregate.memory
    Require ([int]$memory.integration_exit_code -eq 0) 'M2-V6 virtual-memory integration failed'
    Require ([uint64]$memory.ready_committed_bytes -le [uint64]134217728) 'M2-V6 empty-provider committed memory exceeds 128 MiB'
    Require ([uint64]$memory.write_bytes -eq [uint64]1073741824) 'M2-V6 memory workload is not exactly 1 GiB'
    Require ([uint64]$memory.store_committed_after_write_bytes -eq [uint64]1073741824) 'M2-V6 store did not commit exactly 1 GiB'
    $growthCeiling = [uint64]$memory.fixed_metadata_bytes + [uint64]1181116006
    Require ([uint64]$memory.observed_total_growth_bytes -le $growthCeiling) 'M2-V6 1 GiB growth exceeds 1.10 GiB plus measured metadata'
    Require ([uint64]$memory.reclaimed_bytes -ge [uint64]966367641) 'M2-V6 reclaimed less than 90% of 1 GiB'
    Require ([uint64]$memory.reclaim_elapsed_ms -le [uint64]10000) 'M2-V6 reclaim exceeded 10 seconds'
    $memoryLog = Resolve-AggregateFile -AggregatePath $AggregatePath -RelativePath ([string]$memory.log_path)
    Assert-FileHash -Path $memoryLog -Expected ([string]$memory.log_sha256)

    Require-Property $Aggregate 'request_allocation'
    $allocation = $Aggregate.request_allocation
    Require ([int]$allocation.test_exit_code -eq 0) 'M2-V6 request-allocation tests failed'
    Require ([bool]$allocation.constant_space_range_descriptor) 'M2-V6 range descriptor is not constant-space'
    Require ([uint64]$allocation.maximum_request_allocation_bytes -le [uint64]8388608) 'M2-V6 request allocation exceeds the protocol maximum'
    Require ([uint64]$allocation.measured_private_growth_bytes -le [uint64]1048576) 'M2-V6 repeated request path shows unbounded private-byte growth'
    $allocationLog = Resolve-AggregateFile -AggregatePath $AggregatePath -RelativePath ([string]$allocation.log_path)
    Assert-FileHash -Path $allocationLog -Expected ([string]$allocation.log_sha256)

    Require-Property $Aggregate 'normal_run'
    $normal = Read-AggregateChildReport -Aggregate $Aggregate -AggregatePath $AggregatePath -Entry $Aggregate.normal_run -ExecutablePath $ExecutablePath
    Require ([int]$Aggregate.normal_run.process_exit_code -eq 0) 'M2-V6 normal run exit code is not zero'
    Invoke-V5ChildValidation -ExecutablePath $ExecutablePath -ReportPath $normal.Path
    Require ([uint64]$normal.Report.performance.comparison.sequential_ratio_basis_points -ge [uint64]7000) 'M2-V6 sequential performance is below 70%'
    Require ([uint64]$normal.Report.performance.comparison.random_ratio_basis_points -ge [uint64]6000) 'M2-V6 random QD1 performance is below 60%'

    $expectedFaults = @(
        'capture-baseline', 'construct-provider', 'start-provider',
        'create-device', 'wait-for-disk', 'setup-write',
        'sample-after-write', 'delete-retrim',
        'sample-after-retrim', 'performance')
    $faults = @($Aggregate.fault_runs)
    Require ($faults.Count -eq $expectedFaults.Count) 'M2-V6 fault matrix is incomplete'
    Require (Same-Set @($faults | ForEach-Object { [string]$_.failure_point }) $expectedFaults) 'M2-V6 fault point set is incorrect'
    foreach ($fault in $faults) {
        Require ([int]$fault.process_exit_code -eq 8) "fault child exit code is not 8: $($fault.failure_point)"
        $child = Read-AggregateChildReport -Aggregate $Aggregate -AggregatePath $AggregatePath -Entry $fault -ExecutablePath $ExecutablePath
        Invoke-NativeM2Validation -ExecutablePath $ExecutablePath -ReportPath $child.Path
        Assert-FaultCleanup -Report $child.Report -FailurePoint ([string]$fault.failure_point)
    }
}

function Verify-M2V7Aggregate {
    param($Aggregate, [string]$AggregatePath, [string]$ExecutablePath)
    Require ([string]$Aggregate.evidence_kind -eq 'M2-V7-AGGREGATE') 'M2-V7 evidence_kind is invalid'
    Require ([string]$Aggregate.verdict -eq 'PASS') 'M2-V7 aggregate verdict is not PASS'
    Require ([uint64]$Aggregate.cycle_count -eq [uint64]20) 'M2-V7 cycle_count is not 20'
    $cycles = @($Aggregate.cycles)
    Require ($cycles.Count -eq 20) 'M2-V7 does not contain exactly 20 cycles'
    $indices = @($cycles | ForEach-Object { [int]$_.cycle_index } | Sort-Object)
    Require (($indices -join ',') -eq ((1..20) -join ',')) 'M2-V7 cycle indices are incomplete or duplicated'
    foreach ($cycle in $cycles) {
        Require ([int]$cycle.process_exit_code -eq 0) "M2-V7 child exit code is not zero: $($cycle.cycle_index)"
        $child = Read-AggregateChildReport -Aggregate $Aggregate -AggregatePath $AggregatePath -Entry $cycle -ExecutablePath $ExecutablePath
        Invoke-V5ChildValidation -ExecutablePath $ExecutablePath -ReportPath $child.Path
        $ready = @($child.Report.store_stats_samples | Where-Object { $_.sample_point -eq 'READY' })
        $afterWrite = @($child.Report.store_stats_samples | Where-Object { $_.sample_point -eq 'AFTER-WRITE' })
        $afterRetrim = @($child.Report.store_stats_samples | Where-Object { $_.sample_point -eq 'AFTER-RETRIM' })
        Require ($ready.Count -eq 1 -and $afterWrite.Count -eq 1 -and $afterRetrim.Count -eq 1) "M2-V7 cycle samples are incomplete: $($cycle.cycle_index)"
        Require ([uint64]$afterWrite[0].stats.zero_bytes_total -gt [uint64]$ready[0].stats.zero_bytes_total) "M2-V7 ZERO was not observed: $($cycle.cycle_index)"
        Require ([uint64]$afterRetrim[0].stats.unmap_bytes_total -gt [uint64]$afterWrite[0].stats.unmap_bytes_total) "M2-V7 UNMAP was not observed: $($cycle.cycle_index)"
    }

    Require-Property $Aggregate 'regressions'
    foreach ($name in @('m2_v1', 'm2_v2', 'm2_v3_first', 'm2_v3_second', 'm2_v4', 'm1_v2')) {
        Require-Property $Aggregate.regressions $name
        $gate = $Aggregate.regressions.$name
        Require ([int]$gate.exit_code -eq 0) "final regression failed: $name"
        $log = Resolve-AggregateFile -AggregatePath $AggregatePath -RelativePath ([string]$gate.log_path)
        Assert-FileHash -Path $log -Expected ([string]$gate.log_sha256)
    }
}

if ($Tier -in @('M2-V6', 'M2-V7')) {
    $aggregatePath = [IO.Path]::GetFullPath($Path)
    Require ([IO.File]::Exists($aggregatePath)) "M2 aggregate not found: $aggregatePath"
    $aggregateRaw = Get-Content -Raw -LiteralPath $aggregatePath -Encoding utf8
    Require (-not [string]::IsNullOrWhiteSpace($aggregateRaw)) 'M2 aggregate is empty'
    $aggregate = $aggregateRaw | ConvertFrom-Json
    Require ([int]$aggregate.schema_version -eq 1) 'aggregate schema_version must be 1'
    $aggregateExecutable = Get-AggregateExecutable -Aggregate $aggregate
    if ($Tier -eq 'M2-V6') {
        Verify-M2V6Aggregate -Aggregate $aggregate -AggregatePath $aggregatePath -ExecutablePath $aggregateExecutable
    }
    else {
        Verify-M2V7Aggregate -Aggregate $aggregate -AggregatePath $aggregatePath -ExecutablePath $aggregateExecutable
    }
    Write-Output "M2 JSON validation PASS [$Tier]: $aggregatePath"
    exit 0
}

Require ($Tier -in @('M2-V4', 'M2-V5')) 'unsupported M2 verification tier'

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
