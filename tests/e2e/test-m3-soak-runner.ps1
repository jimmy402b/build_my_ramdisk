[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Runner,
    [Parameter(Mandatory = $true)][string]$Executable
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-SoakRunnerTest {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-Runner {
    param([string[]]$Arguments)
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File $Runner @Arguments 2>&1)
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    }
    finally { $ErrorActionPreference = $old }
}

function Invoke-FailureMode {
    param([string]$Mode, [string]$Root, [int]$Timeout = 30)
    [IO.Directory]::CreateDirectory($Root) | Out-Null
    $oldMode = $env:MYRD_M3_SOAK_TEST_MODE
    try {
        $env:MYRD_M3_SOAK_TEST_MODE = $Mode
        return Invoke-Runner @(
            '-Phase', 'Run', '-Profile', 'Quick',
            '-EvidenceDirectory', $Root, '-SoakExecutable', $Executable,
            '-TimeoutSeconds', [string]$Timeout)
    }
    finally { $env:MYRD_M3_SOAK_TEST_MODE = $oldMode }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$tempRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) `
    ('myrd-m3-soak-runner-' + [Guid]::NewGuid().ToString('N'))))
Require-SoakRunnerTest (-not [IO.Directory]::Exists($tempRoot)) `
    'soak runner temporary root unexpectedly exists'

try {
    [IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    $insideRepo = Join-Path $repoRoot 'tests'
    $unsafe = Invoke-Runner @(
        '-Phase', 'Validate', '-Profile', 'Quick',
        '-EvidenceDirectory', $insideRepo, '-SoakExecutable', $Executable)
    Require-SoakRunnerTest ($unsafe.ExitCode -ne 0) `
        'repository-contained evidence was accepted'

    $missing = Invoke-Runner @(
        '-Phase', 'Validate', '-Profile', 'Quick',
        '-EvidenceDirectory', $tempRoot,
        '-SoakExecutable', (Join-Path $tempRoot 'missing.exe'))
    Require-SoakRunnerTest ($missing.ExitCode -ne 0) `
        'missing soak executable was accepted'

    $quick = Invoke-Runner @(
        '-Phase', 'Run', '-Profile', 'Quick',
        '-EvidenceDirectory', $tempRoot, '-SoakExecutable', $Executable,
        '-TimeoutSeconds', '30')
    Require-SoakRunnerTest ($quick.ExitCode -eq 0) `
        'Quick soak runner failed'
    $reports = @(Get-ChildItem -LiteralPath $tempRoot -Recurse -Filter `
        'm3-soak-report.json' -File)
    Require-SoakRunnerTest ($reports.Count -eq 1) `
        'Quick run did not publish exactly one report'
    $reportPath = $reports[0].FullName
    $report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    Require-SoakRunnerTest ($report.schema -eq 'myrd.m3.soak-evidence.v2') `
        'runner report schema mismatch'
    Require-SoakRunnerTest ($report.status -eq 'PASS' -and
        $report.terminal_verdict -eq 'PASS') 'Quick report is not terminal PASS'
    Require-SoakRunnerTest ($report.profile -eq 'Quick') `
        'Quick report profile mismatch'
    Require-SoakRunnerTest ([uint64]$report.execution.seed_count -eq 4) `
        'Quick seed set is not fixed at four'
    Require-SoakRunnerTest (@($report.execution.failed_seeds).Count -eq 0) `
        'Quick report contains failed seeds'
    foreach ($name in @(
        'production_checkpoint_seeds','reopen_verified_seeds',
        'concurrent_first_touch_seeds','cow_overlap_seeds',
        'repeated_close_seeds')) {
        Require-SoakRunnerTest ([uint64]$report.coverage.$name -eq 4) `
            "Quick production coverage mismatch: $name"
    }
    Require-SoakRunnerTest `
        ([uint64]$report.coverage.compaction_verified_seeds -ge 1) `
        'Quick run missed compaction coverage'
    Require-SoakRunnerTest `
        ([uint64]$report.coverage.io_fault_recovered_seeds -ge 1) `
        'Quick run missed I/O fault recovery coverage'
    Require-SoakRunnerTest `
        ($null -ne $report.coverage.PSObject.Properties['allocated_profile_seed'] `
         -and [uint64]$report.coverage.allocated_profile_seed -eq 0) `
        'Quick run claimed or omitted the Formal allocated-image profile'
    Require-SoakRunnerTest `
        ($null -ne $report.thresholds.PSObject.Properties['allocated_profile_bytes'] `
         -and [uint64]$report.thresholds.allocated_profile_bytes -eq 0) `
        'Quick run claimed or omitted Formal allocated-image bytes'
    Require-SoakRunnerTest ($report.cleanup.succeeded -eq $true -and
        @($report.cleanup.residuals).Count -eq 0) 'Quick cleanup was not empty'
    foreach ($name in @(
        'loading_blocks','active_snapshot_id','snapshot_cow_current_bytes',
        'snapshot_cow_records','snapshot_blocked_writers')) {
        Require-SoakRunnerTest ($null -ne $report.cleanup.PSObject.Properties[$name]) `
            "Quick cleanup gauge is missing: $name"
        Require-SoakRunnerTest ([uint64]$report.cleanup.$name -eq 0) `
            "Quick cleanup gauge is nonzero: $name"
    }
    foreach ($name in @(
        'duration_ms','throughput_operations_per_second',
        'latency_sample_count','latency_us_p50','latency_us_p95',
        'latency_us_p99','checkpoint_duration_ms','compaction_duration_ms')) {
        Require-SoakRunnerTest `
            ($null -ne $report.performance.PSObject.Properties[$name]) `
            "Quick performance field is missing: $name"
    }
    Require-SoakRunnerTest `
        ([uint64]$report.performance.duration_ms -gt 0 -and
         [uint64]$report.performance.throughput_operations_per_second -gt 0 -and
         [uint64]$report.performance.latency_sample_count -gt 0 -and
         [uint64]$report.performance.checkpoint_duration_ms -gt 0 -and
         [uint64]$report.performance.compaction_duration_ms -gt 0) `
        'Quick performance observations contain required zero values'
    Require-SoakRunnerTest `
        (@($report.limitations).Count -gt 0) `
        'Quick report did not state performance limitations'
    foreach ($role in @('soak.stdout', 'soak.stderr')) {
        Require-SoakRunnerTest ($null -ne @($report.artifacts |
            Where-Object { $_.role -eq $role })[0]) `
            "runner artifact is missing: $role"
    }
    $runDirectory = $reports[0].Directory.FullName
    Require-SoakRunnerTest ([IO.File]::Exists(
        (Join-Path $runDirectory 'soak.stdout.txt'))) 'aggregate stdout missing'
    Require-SoakRunnerTest ([IO.File]::Exists(
        (Join-Path $runDirectory 'soak.stderr.txt'))) 'aggregate stderr missing'
    Require-SoakRunnerTest (@(Get-ChildItem -LiteralPath $runDirectory -Filter `
        '*.tmp' -File).Count -eq 0) 'temporary report was not atomically removed'

    foreach ($mode in @('nonzero','malformed','multiple')) {
        $failureRoot = Join-Path $tempRoot $mode
        $failure = Invoke-FailureMode $mode $failureRoot
        Require-SoakRunnerTest ($failure.ExitCode -ne 0) `
            "runner accepted child failure mode: $mode"
        Require-SoakRunnerTest (@(Get-ChildItem -LiteralPath $failureRoot `
            -Recurse -Filter 'm3-soak-report.json' -File).Count -eq 0) `
            "failure mode published a passing report: $mode"
    }

    $beforeTimeout = @(Get-Process -Name 'myrd_m3_soak_tests' `
        -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
    $timeoutRoot = Join-Path $tempRoot 'timeout'
    $timeout = Invoke-FailureMode 'timeout' $timeoutRoot 30
    Require-SoakRunnerTest ($timeout.ExitCode -ne 0) `
        'runner accepted a timed-out child'
    Start-Sleep -Milliseconds 100
    $afterTimeout = @(Get-Process -Name 'myrd_m3_soak_tests' `
        -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
    $newProcesses = @($afterTimeout | Where-Object { $beforeTimeout -notcontains $_ })
    Require-SoakRunnerTest ($newProcesses.Count -eq 0) `
        'timed-out soak child remained running'

    $formalMissing = Invoke-Runner @(
        '-Phase', 'Run', '-Profile', 'Formal',
        '-EvidenceDirectory', $tempRoot, '-SoakExecutable', $Executable,
        '-TimeoutSeconds', '30')
    Require-SoakRunnerTest ($formalMissing.ExitCode -ne 0) `
        'Formal run without Task41/Task42 dependencies was accepted'

    Write-Output 'M3 soak runner contract PASS'
    exit 0
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) {
        [IO.Directory]::Delete($tempRoot, $true)
    }
    Require-SoakRunnerTest (-not [IO.Directory]::Exists($tempRoot)) `
        'soak runner temporary directory remained after cleanup'
}
