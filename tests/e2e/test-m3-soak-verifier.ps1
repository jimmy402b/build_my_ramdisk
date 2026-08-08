[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-SoakVerifierTest {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-SoakVerifier {
    param(
        [Parameter(Mandatory = $true)][string]$Verifier,
        [Parameter(Mandatory = $true)][string]$Report,
        [Parameter(Mandatory = $true)][string]$SourceCommit,
        [Parameter(Mandatory = $true)][string]$BinaryHash,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [Parameter(Mandatory = $true)][string]$RealFunctionalPath,
        [Parameter(Mandatory = $true)][string]$CrashAggregatePath
    )
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $null = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Verifier `
            -Path $Report -ExpectedSourceCommit $SourceCommit `
            -ExpectedBinarySha256 $BinaryHash `
            -StdoutPath $StdoutPath -StderrPath $StderrPath `
            -RealFunctionalReportPath $RealFunctionalPath `
            -CrashAggregateReportPath $CrashAggregatePath 2>&1
        return $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $old }
}

function Copy-JsonObject {
    param([Parameter(Mandatory = $true)]$Value)
    return ($Value | ConvertTo-Json -Depth 30 | ConvertFrom-Json)
}

function Write-TestJson {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $json = $Value | ConvertTo-Json -Depth 30
    [IO.File]::WriteAllText($Path, $json, [Text.UTF8Encoding]::new($false))
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$verifier = Join-Path $repoRoot 'tools\Verify-M3Json.ps1'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('myrd-m3-soak-verifier-' + [Guid]::NewGuid().ToString('N'))
$tempRoot = [IO.Path]::GetFullPath($tempRoot)
Require-SoakVerifierTest (-not [IO.Directory]::Exists($tempRoot)) `
    'soak verifier temporary root unexpectedly exists'

try {
    [IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    $source = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    $binary = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
    $stdoutPath = Join-Path $tempRoot 'soak.stdout.txt'
    $stderrPath = Join-Path $tempRoot 'soak.stderr.txt'
    [IO.File]::WriteAllText($stdoutPath, "soak output`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($stderrPath, '', [Text.UTF8Encoding]::new($false))
    $stdoutHash = (Get-FileHash -LiteralPath $stdoutPath -Algorithm SHA256).Hash
    $stderrHash = (Get-FileHash -LiteralPath $stderrPath -Algorithm SHA256).Hash
    $dataHash = 'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd'
    $serverHash = '1111111111111111111111111111111111111111111111111111111111111111'
    $imageHash = '2222222222222222222222222222222222222222222222222222222222222222'
    $realFunctionalPath = Join-Path $tempRoot 'm3-ntfs-report.json'
    $crashAggregatePath = Join-Path $tempRoot 'm3-real-crash-aggregate.json'
    [ordered]@{
        schema = 'myrd.m3.ntfs-workload.v1'; status = 'PASS'
        terminal_verdict = 'PASS'; source_commit = $source; phase = 'All'
        what_if = $false; server_started = $true
        destructive_operations_emitted = $true
        executable_sha256 = [ordered]@{ server = $serverHash; image = $imageHash }
        cleanup = [ordered]@{ succeeded = $true; residuals = @() }
        workload = [ordered]@{
            first_checkpoint_generation = 1
            second_checkpoint_generation = 2
            compact_generation = 3
            server_reopen_verified = $true
            workload = [ordered]@{
                files = @([ordered]@{ path = 'a.bin'; bytes = 1; sha256 = $dataHash })
                sparse = [ordered]@{ sha256 = $dataHash }
            }
            lazy_evidence = [ordered]@{
                bounded_sample = $true; full_corpus_verified = $true
                startup_status = [ordered]@{ storage = [ordered]@{
                    backing_reads_total = 0; uncached_reads_total = 0 } }
                sample_status = [ordered]@{ storage = [ordered]@{
                    backing_reads_total = 1; uncached_reads_total = 0 } }
            }
            native_vhdx = [ordered]@{ filesystem = 'NTFS'; disk_number = 1 }
        }
    } | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $realFunctionalPath -Encoding utf8
    $crashScenarios = @()
    for ($index = 0; $index -lt 10; ++$index) {
        $crashScenarios += [ordered]@{
            scenario_id = "scenario-$index"
            phase = "phase-$index"
            interruption = if ($index -lt 2) { 'controlled-power-cut' } else { 'process-kill' }
            allowed_recovery = if (($index % 2) -eq 0) { 'old' } else { 'new' }
            recovered_classification = if (($index % 2) -eq 0) { 'old' } else { 'new' }
            selected_generation = if (($index % 2) -eq 0) { 3 } else { 4 }
            selected_slot = if (($index % 2) -eq 0) { 1 } else { 0 }
            fallback = $false
            slot_diagnostics = @(
                [ordered]@{ slot_index = 0; status = 'passed'; generation = 4; selected = (($index % 2) -ne 0) },
                [ordered]@{ slot_index = 1; status = 'passed'; generation = 3; selected = (($index % 2) -eq 0) })
            report_sha256 = $dataHash
        }
    }
    [ordered]@{
        schema = 'myrd.m3.real-crash-aggregate.v1'; status = 'PASS'
        terminal_verdict = 'PASS'; task42_complete = $true
        milestone_eligible = $true; source_commit = $source
        server_sha256 = $serverHash; image_sha256 = $imageHash
        matrix_sha256 = $dataHash; simulated_evidence_sha256 = $dataHash
        simulated_schedule_count = 10000; real_scenario_count = 10
        process_kill_count = 8; controlled_power_cut_count = 2
        scenarios = $crashScenarios
        guarantees = [ordered]@{
            latest_complete_confirmed_checkpoint = $true
            mixed_generation_rejected = $true
            manifest_slot_diagnostics_verified = $true
            post_checkpoint_uncommitted_changes_may_be_lost = $true
            read_only_recovery_first = $true
            ntfs_reformat_forbidden = $true
            per_scenario_guest_reboot_isolation = $true
        }
    } | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $crashAggregatePath -Encoding utf8
    $realFunctionalHash = (Get-FileHash -LiteralPath $realFunctionalPath -Algorithm SHA256).Hash
    $crashAggregateHash = (Get-FileHash -LiteralPath $crashAggregatePath -Algorithm SHA256).Hash
    $report = [ordered]@{
        schema = 'myrd.m3.soak-evidence.v2'
        status = 'PASS'
        terminal_verdict = 'PASS'
        profile = 'Formal'
        source_commit = $source
        task43_complete = $true
        milestone_eligible = $true
        build = [ordered]@{
            build_type = 'Release'
            soak_executable_sha256 = $binary
            stdout_sha256 = $stdoutHash
            stderr_sha256 = $stderrHash
        }
        execution = [ordered]@{
            seed_count = 100
            failed_seeds = @()
            process_exit_code = 0
            operation_count = 100000
            stdout_sha256 = $stdoutHash
            stderr_sha256 = $stderrHash
        }
        correctness = [ordered]@{
            expected_sha256 = $dataHash
            actual_sha256 = $dataHash
            hash_match = $true
            complete_generations_only = $true
            mixed_generation = $false
        }
        coverage = [ordered]@{
            production_checkpoint_seeds = 100
            reopen_verified_seeds = 100
            compaction_verified_seeds = 1
            concurrent_first_touch_seeds = 100
            cow_overlap_seeds = 100
            io_fault_recovered_seeds = 1
            repeated_close_seeds = 100
            resource_checkpoint_seed = 3221344269
            allocated_profile_seed = 3221344269
        }
        thresholds = [ordered]@{
            startup_read_limit_bytes = 67108864
            startup_read_bytes = 0
            extra_committed_limit_bytes = 134217728
            extra_committed_bytes = 0
            allocated_profile_bytes = 1073741824
            checkpoint_payload_bytes = 268435456
            journal_growth_limit_bytes = 322122547
            journal_growth_bytes = 1
            mutation_pause_limit_ms = 250
            mutation_pause_max_ms = 1
            writer_stall_limit_ms = 250
            writer_stall_max_ms = 1
        }
        performance = [ordered]@{
            duration_ms = 1000
            throughput_operations_per_second = 100
            latency_sample_count = 100000
            latency_us_p50 = 10
            latency_us_p95 = 20
            latency_us_p99 = 30
            checkpoint_duration_ms = 40
            compaction_duration_ms = 50
        }
        dependencies = [ordered]@{
            real_functional = [ordered]@{
                status = 'PASS'; source_commit = $source; sha256 = $realFunctionalHash
            }
            crash_full = [ordered]@{
                status = 'PASS'; task42_complete = $true
                source_commit = $source; sha256 = $crashAggregateHash
            }
        }
        cleanup = [ordered]@{
            succeeded = $true
            residuals = @()
            loading_blocks = 0
            active_snapshot_id = 0
            snapshot_cow_current_bytes = 0
            snapshot_cow_records = 0
            snapshot_blocked_writers = 0
        }
        limitations = @(
            'Performance values are engineering observations, not a Primo parity claim.'
        )
        artifacts = @(
            [ordered]@{ role = 'soak.stdout'; sha256 = $stdoutHash },
            [ordered]@{ role = 'soak.stderr'; sha256 = $stderrHash }
        )
        error = $null
    }
    $reportPath = Join-Path $tempRoot 'm3-soak-report.json'
    Write-TestJson $report $reportPath
    $exitCode = Invoke-SoakVerifier $verifier $reportPath $source $binary `
        $stdoutPath $stderrPath $realFunctionalPath $crashAggregatePath
    Require-SoakVerifierTest ($exitCode -eq 0) `
        "valid Formal soak evidence was rejected with exit code $exitCode"

    $quick = Copy-JsonObject $report
    $quick.profile = 'Quick'
    $quick.task43_complete = $false
    $quick.milestone_eligible = $false
    $quick.execution.seed_count = 4
    $quick.coverage.production_checkpoint_seeds = 4
    $quick.coverage.reopen_verified_seeds = 4
    $quick.coverage.concurrent_first_touch_seeds = 4
    $quick.coverage.cow_overlap_seeds = 4
    $quick.coverage.repeated_close_seeds = 4
    $quick.coverage.allocated_profile_seed = 0
    $quick.dependencies = $null
    $quick.thresholds.checkpoint_payload_bytes = 1
    $quick.thresholds.allocated_profile_bytes = 0
    $quick.thresholds.journal_growth_bytes = 0
    Write-TestJson $quick $reportPath
    $exitCode = Invoke-SoakVerifier $verifier $reportPath $source $binary `
        $stdoutPath $stderrPath $realFunctionalPath $crashAggregatePath
    Require-SoakVerifierTest ($exitCode -eq 0) `
        "valid Quick soak evidence was rejected with exit code $exitCode"

    $mutations = @(
        @{ Name = 'source commit'; Apply = { param($r) $r.source_commit = ('c' * 40) } },
        @{ Name = 'executable hash'; Apply = { param($r) $r.build.soak_executable_sha256 = ('c' * 64) } },
        @{ Name = 'stdout hash'; Apply = { param($r) $r.build.stdout_sha256 = ('c' * 64) } },
        @{ Name = 'profile'; Apply = { param($r) $r.profile = 'Nightly' } },
        @{ Name = 'seed count'; Apply = { param($r) $r.execution.seed_count = 99 } },
        @{ Name = 'failed seed'; Apply = { param($r) $r.execution.failed_seeds = @(7) } },
        @{ Name = 'child exit'; Apply = { param($r) $r.execution.process_exit_code = 1 } },
        @{ Name = 'model hash'; Apply = { param($r) $r.correctness.actual_sha256 = ('e' * 64) } },
        @{ Name = 'hash match proof'; Apply = { param($r) $r.correctness.hash_match = $false } },
        @{ Name = 'complete generation proof'; Apply = { param($r) $r.correctness.complete_generations_only = $false } },
        @{ Name = 'mixed generation proof'; Apply = { param($r) $r.correctness.mixed_generation = $true } },
        @{ Name = 'production checkpoint coverage'; Apply = { param($r) $r.coverage.production_checkpoint_seeds = 99 } },
        @{ Name = 'reopen coverage'; Apply = { param($r) $r.coverage.reopen_verified_seeds = 99 } },
        @{ Name = 'compaction coverage'; Apply = { param($r) $r.coverage.compaction_verified_seeds = 0 } },
        @{ Name = 'first-touch coverage'; Apply = { param($r) $r.coverage.concurrent_first_touch_seeds = 99 } },
        @{ Name = 'COW coverage'; Apply = { param($r) $r.coverage.cow_overlap_seeds = 99 } },
        @{ Name = 'I/O fault coverage'; Apply = { param($r) $r.coverage.io_fault_recovered_seeds = 0 } },
        @{ Name = 'repeated close coverage'; Apply = { param($r) $r.coverage.repeated_close_seeds = 99 } },
        @{ Name = 'resource checkpoint seed'; Apply = { param($r) $r.coverage.resource_checkpoint_seed = 3221344270 } },
        @{ Name = 'allocated profile seed'; Apply = { param($r) $r.coverage.allocated_profile_seed = 3221344270 } },
        @{ Name = 'startup read threshold'; Apply = { param($r) $r.thresholds.startup_read_bytes = 67108865 } },
        @{ Name = 'committed memory threshold'; Apply = { param($r) $r.thresholds.extra_committed_bytes = 134217729 } },
        @{ Name = 'checkpoint payload'; Apply = { param($r) $r.thresholds.checkpoint_payload_bytes = 1 } },
        @{ Name = 'allocated profile bytes'; Apply = { param($r) $r.thresholds.allocated_profile_bytes = 1073741823 } },
        @{ Name = 'journal growth threshold'; Apply = { param($r) $r.thresholds.journal_growth_bytes = 322122548 } },
        @{ Name = 'mutation pause threshold'; Apply = { param($r) $r.thresholds.mutation_pause_max_ms = 251 } },
        @{ Name = 'writer stall threshold'; Apply = { param($r) $r.thresholds.writer_stall_max_ms = 251 } },
        @{ Name = 'performance duration'; Apply = { param($r) $r.performance.duration_ms = 0 } },
        @{ Name = 'performance throughput'; Apply = { param($r) $r.performance.throughput_operations_per_second = 0 } },
        @{ Name = 'latency sample count'; Apply = { param($r) $r.performance.latency_sample_count = 0 } },
        @{ Name = 'latency percentile order'; Apply = { param($r) $r.performance.latency_us_p95 = 5 } },
        @{ Name = 'checkpoint duration'; Apply = { param($r) $r.performance.checkpoint_duration_ms = 0 } },
        @{ Name = 'compaction duration'; Apply = { param($r) $r.performance.compaction_duration_ms = 0 } },
        @{ Name = 'cleanup result'; Apply = { param($r) $r.cleanup.succeeded = $false } },
        @{ Name = 'cleanup residual'; Apply = { param($r) $r.cleanup.residuals = @('mapping') } },
        @{ Name = 'loading block residual'; Apply = { param($r) $r.cleanup.loading_blocks = 1 } },
        @{ Name = 'active snapshot residual'; Apply = { param($r) $r.cleanup.active_snapshot_id = 1 } },
        @{ Name = 'COW byte residual'; Apply = { param($r) $r.cleanup.snapshot_cow_current_bytes = 65536 } },
        @{ Name = 'COW record residual'; Apply = { param($r) $r.cleanup.snapshot_cow_records = 1 } },
        @{ Name = 'blocked writer residual'; Apply = { param($r) $r.cleanup.snapshot_blocked_writers = 1 } },
        @{ Name = 'missing limitation'; Apply = { param($r) $r.limitations = @() } },
        @{ Name = 'Task43 completion'; Apply = { param($r) $r.task43_complete = $false } },
        @{ Name = 'milestone eligibility'; Apply = { param($r) $r.milestone_eligible = $false } },
        @{ Name = 'Task41 dependency status'; Apply = { param($r) $r.dependencies.real_functional.status = 'FAIL' } },
        @{ Name = 'Task41 dependency hash'; Apply = { param($r) $r.dependencies.real_functional.sha256 = ('c' * 64) } },
        @{ Name = 'Task42 dependency status'; Apply = { param($r) $r.dependencies.crash_full.status = 'FAIL' } },
        @{ Name = 'Task42 completion'; Apply = { param($r) $r.dependencies.crash_full.task42_complete = $false } },
        @{ Name = 'Task42 dependency hash'; Apply = { param($r) $r.dependencies.crash_full.sha256 = ('c' * 64) } }
    )
    foreach ($mutation in $mutations) {
        $candidate = Copy-JsonObject $report
        & $mutation.Apply $candidate
        Write-TestJson $candidate $reportPath
        $exitCode = Invoke-SoakVerifier $verifier $reportPath $source $binary `
            $stdoutPath $stderrPath $realFunctionalPath $crashAggregatePath
        Require-SoakVerifierTest ($exitCode -ne 0) `
            "Formal soak mutation was accepted: $($mutation.Name)"
    }

    $dependencyMutations = @(
        @{ Name = 'Task41 full-contract field'; Target = 'Task41'; Apply = { param($r) $r.PSObject.Properties.Remove('phase') } },
        @{ Name = 'Task41 WhatIf'; Target = 'Task41'; Apply = { param($r) $r.what_if = $true } },
        @{ Name = 'Task41 cleanup'; Target = 'Task41'; Apply = { param($r) $r.cleanup.succeeded = $false } },
        @{ Name = 'Task41 lazy corpus'; Target = 'Task41'; Apply = { param($r) $r.workload.lazy_evidence.full_corpus_verified = $false } },
        @{ Name = 'Task41 report source'; Target = 'Task41'; Apply = { param($r) $r.source_commit = ('c' * 40) } },
        @{ Name = 'Task41 report verdict'; Target = 'Task41'; Apply = { param($r) $r.terminal_verdict = 'FAIL' } },
        @{ Name = 'Task42 real scenario count'; Target = 'Task42'; Apply = { param($r) $r.real_scenario_count = 9 } },
        @{ Name = 'Task42 binary cross-binding'; Target = 'Task42'; Apply = { param($r) $r.server_sha256 = ('c' * 64) } },
        @{ Name = 'Task42 manifest diagnostics'; Target = 'Task42'; Apply = { param($r) $r.guarantees.manifest_slot_diagnostics_verified = $false } },
        @{ Name = 'Task42 report source'; Target = 'Task42'; Apply = { param($r) $r.source_commit = ('c' * 40) } },
        @{ Name = 'Task42 report verdict'; Target = 'Task42'; Apply = { param($r) $r.terminal_verdict = 'FAIL' } },
        @{ Name = 'Task42 report completion'; Target = 'Task42'; Apply = { param($r) $r.task42_complete = $false } },
        @{ Name = 'latest checkpoint guarantee'; Target = 'Task42'; Apply = { param($r) $r.guarantees.latest_complete_confirmed_checkpoint = $false } },
        @{ Name = 'mixed generation guarantee'; Target = 'Task42'; Apply = { param($r) $r.guarantees.mixed_generation_rejected = $false } }
    )
    $originalTask41 = Get-Content -Raw -LiteralPath $realFunctionalPath | ConvertFrom-Json
    $originalTask42 = Get-Content -Raw -LiteralPath $crashAggregatePath | ConvertFrom-Json
    Write-TestJson $originalTask41 $realFunctionalPath
    Write-TestJson $originalTask42 $crashAggregatePath
    $canonicalReport = Copy-JsonObject $report
    $canonicalReport.dependencies.real_functional.sha256 = `
        (Get-FileHash -LiteralPath $realFunctionalPath -Algorithm SHA256).Hash
    $canonicalReport.dependencies.crash_full.sha256 = `
        (Get-FileHash -LiteralPath $crashAggregatePath -Algorithm SHA256).Hash
    foreach ($mutation in $dependencyMutations) {
        $candidate = Copy-JsonObject $canonicalReport
        if ($mutation.Target -eq 'Task41') {
            $dependency = Copy-JsonObject $originalTask41
            & $mutation.Apply $dependency
            Write-TestJson $dependency $realFunctionalPath
            $candidate.dependencies.real_functional.sha256 = `
                (Get-FileHash -LiteralPath $realFunctionalPath -Algorithm SHA256).Hash
            Write-TestJson $originalTask42 $crashAggregatePath
        }
        else {
            $dependency = Copy-JsonObject $originalTask42
            & $mutation.Apply $dependency
            Write-TestJson $dependency $crashAggregatePath
            $candidate.dependencies.crash_full.sha256 = `
                (Get-FileHash -LiteralPath $crashAggregatePath -Algorithm SHA256).Hash
            Write-TestJson $originalTask41 $realFunctionalPath
        }
        Write-TestJson $candidate $reportPath
        $exitCode = Invoke-SoakVerifier $verifier $reportPath $source $binary `
            $stdoutPath $stderrPath $realFunctionalPath $crashAggregatePath
        Require-SoakVerifierTest ($exitCode -ne 0) `
            "Formal soak dependency mutation was accepted: $($mutation.Name)"
    }

    Write-Output 'M3 soak verifier contract tests PASS'
    exit 0
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) {
        [IO.Directory]::Delete($tempRoot, $true)
    }
    Require-SoakVerifierTest (-not [IO.Directory]::Exists($tempRoot)) `
        'soak verifier temporary directory remained after cleanup'
}
