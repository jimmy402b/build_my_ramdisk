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
    $realFunctionalPath = Join-Path $tempRoot 'm3-ntfs-report.json'
    $crashAggregatePath = Join-Path $tempRoot 'm3-real-crash-aggregate.json'
    [ordered]@{
        schema = 'myrd.m3.ntfs-workload.v1'; status = 'PASS'
        terminal_verdict = 'PASS'; source_commit = $source
    } | ConvertTo-Json | Set-Content -LiteralPath $realFunctionalPath -Encoding utf8
    [ordered]@{
        schema = 'myrd.m3.real-crash-aggregate.v1'; status = 'PASS'
        terminal_verdict = 'PASS'; task42_complete = $true
        milestone_eligible = $true; source_commit = $source
        guarantees = [ordered]@{
            latest_complete_confirmed_checkpoint = $true
            mixed_generation_rejected = $true
        }
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $crashAggregatePath -Encoding utf8
    $realFunctionalHash = (Get-FileHash -LiteralPath $realFunctionalPath -Algorithm SHA256).Hash
    $crashAggregateHash = (Get-FileHash -LiteralPath $crashAggregatePath -Algorithm SHA256).Hash
    $report = [ordered]@{
        schema = 'myrd.m3.soak-evidence.v1'
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
        thresholds = [ordered]@{
            startup_read_limit_bytes = 67108864
            startup_read_bytes = 0
            extra_committed_limit_bytes = 134217728
            extra_committed_bytes = 0
            checkpoint_payload_bytes = 268435456
            journal_growth_limit_bytes = 322122547
            journal_growth_bytes = 1
            mutation_pause_limit_ms = 250
            mutation_pause_max_ms = 1
            writer_stall_limit_ms = 250
            writer_stall_max_ms = 1
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
        cleanup = [ordered]@{ succeeded = $true; residuals = @() }
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
    $quick.dependencies = $null
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
        @{ Name = 'startup read threshold'; Apply = { param($r) $r.thresholds.startup_read_bytes = 67108865 } },
        @{ Name = 'committed memory threshold'; Apply = { param($r) $r.thresholds.extra_committed_bytes = 134217729 } },
        @{ Name = 'checkpoint payload'; Apply = { param($r) $r.thresholds.checkpoint_payload_bytes = 1 } },
        @{ Name = 'journal growth threshold'; Apply = { param($r) $r.thresholds.journal_growth_bytes = 322122548 } },
        @{ Name = 'mutation pause threshold'; Apply = { param($r) $r.thresholds.mutation_pause_max_ms = 251 } },
        @{ Name = 'writer stall threshold'; Apply = { param($r) $r.thresholds.writer_stall_max_ms = 251 } },
        @{ Name = 'cleanup result'; Apply = { param($r) $r.cleanup.succeeded = $false } },
        @{ Name = 'cleanup residual'; Apply = { param($r) $r.cleanup.residuals = @('mapping') } },
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
        @{ Name = 'Task41 report source'; Target = 'Task41'; Apply = { param($r) $r.source_commit = ('c' * 40) } },
        @{ Name = 'Task41 report verdict'; Target = 'Task41'; Apply = { param($r) $r.terminal_verdict = 'FAIL' } },
        @{ Name = 'Task42 report source'; Target = 'Task42'; Apply = { param($r) $r.source_commit = ('c' * 40) } },
        @{ Name = 'Task42 report verdict'; Target = 'Task42'; Apply = { param($r) $r.terminal_verdict = 'FAIL' } },
        @{ Name = 'Task42 report completion'; Target = 'Task42'; Apply = { param($r) $r.task42_complete = $false } },
        @{ Name = 'latest checkpoint guarantee'; Target = 'Task42'; Apply = { param($r) $r.guarantees.latest_complete_confirmed_checkpoint = $false } },
        @{ Name = 'mixed generation guarantee'; Target = 'Task42'; Apply = { param($r) $r.guarantees.mixed_generation_rejected = $false } }
    )
    $originalTask41 = Get-Content -Raw -LiteralPath $realFunctionalPath | ConvertFrom-Json
    $originalTask42 = Get-Content -Raw -LiteralPath $crashAggregatePath | ConvertFrom-Json
    foreach ($mutation in $dependencyMutations) {
        $candidate = Copy-JsonObject $report
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
