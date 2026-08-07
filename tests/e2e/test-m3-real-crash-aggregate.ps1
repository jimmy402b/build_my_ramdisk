[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3AggregateTest {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Write-TestJson {
    param([string]$Path, $Value)
    [IO.File]::WriteAllText(
        $Path, ($Value | ConvertTo-Json -Depth 20),
        [Text.UTF8Encoding]::new($false))
}

function Get-TestSha256 {
    param([byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace(
            '-', '')
    }
    finally { $sha.Dispose() }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$runner = Join-Path $PSScriptRoot 'Invoke-M3RealCrashScenario.ps1'
$route = Join-Path $repoRoot 'tools\Invoke-M3Verification.ps1'
$matrixPath = Join-Path $repoRoot 'tests\fixtures\m3\real-crash-matrix.json'
$matrix = Get-Content -LiteralPath $matrixPath -Raw -Encoding UTF8 | ConvertFrom-Json
$matrixText = [IO.File]::ReadAllText(
    $matrixPath, [Text.UTF8Encoding]::new($false, $true))
$matrixLf = $matrixText -replace "`r`n", "`n" -replace "`r", "`n"
$matrixCrLf = $matrixLf -replace "`n", "`r`n"
$matrixCrLfHash = Get-TestSha256 `
    ([Text.UTF8Encoding]::new($false).GetBytes($matrixCrLf))
$root = Join-Path ([IO.Path]::GetTempPath()) `
    ('myrd-m3-real-crash-aggregate-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $scenarioDirectories = @()
    foreach ($scenario in @($matrix.scenarios)) {
        $directory = Join-Path $root ([string]$scenario.id)
        [IO.Directory]::CreateDirectory($directory) | Out-Null
        $scenarioDirectories += $directory
        $old = [uint64]3
        $new = [uint64]4
        $classification = if ([string]$scenario.allowed_recovery -eq 'new') {
            'new'
        } else { 'old' }
        $selected = if ($classification -eq 'new') { $new } else { $old }
        $inspection = [ordered]@{
            ok = $true
            generation = $selected
            selected_slot = 0
            fallback = $false
            slots = @(
                [ordered]@{
                    slot_index = 0
                    status = 'passed'
                    generation = $selected
                    selected = $true
                },
                [ordered]@{
                    slot_index = 1
                    status = 'missing'
                    generation = $null
                    selected = $false
                }
            )
        }
        Write-TestJson (Join-Path $directory 'm3-real-crash-report.json') ([ordered]@{
            schema = 'myrd.m3.real-crash-report.v1'
            status = 'PASS'
            terminal_verdict = 'PASS'
            scenario_id = [string]$scenario.id
            phase = [string]$scenario.phase
            interruption = [string]$scenario.interruption
            allowed_recovery = [string]$scenario.allowed_recovery
            recovered_classification = $classification
            selected_generation = $selected
            old_generation = $old
            new_generation = $new
            source_commit = ('a' * 40)
            # Model evidence produced by a CRLF checkout and aggregate it from
            # an LF checkout. Semantic JSON provenance must remain portable.
            matrix_sha256 = $matrixCrLfHash
            server_sha256 = ('b' * 64)
            image_sha256 = ('c' * 64)
            image_inspect = $inspection
            image_full_verify = $inspection
            read_only_first = $true
            full_corpus_sha256 = ('d' * 64)
            filesystem = [ordered]@{ type = 'NTFS'; scan_succeeded = $true; reformatted = $false }
            lazy_load = [ordered]@{
                startup_status = [ordered]@{ storage = [ordered]@{ committed_bytes = 0 } }
                pre_sample_status = [ordered]@{ storage = [ordered]@{ backing_reads_total = 1; uncached_reads_total = 1 } }
                post_sample_status = [ordered]@{ storage = [ordered]@{ backing_reads_total = 2; uncached_reads_total = 1 } }
            }
            cleanup = [ordered]@{
                reboot_isolation_required = $true
                residual_server_count = 0
                residual_aim_disk_count = 0
                succeeded = $true
                errors = @()
            }
        })
    }
    $simulatedPath = Join-Path $root 'simulated.json'
    Write-TestJson $simulatedPath ([ordered]@{
        schema = 'myrd.m3.crash-evidence.v1'
        status = 'PASS'
        terminal_verdict = 'PASS'
        scope = 'simulated'
        milestone_eligible = $false
        task42_complete = $false
        source_commit = ('a' * 40)
        execution = [ordered]@{ schedule_count = 10000; summary = '6/6 passed' }
        coverage = [ordered]@{ simulated_file_clone = $true; fresh_process = $true }
        cleanup = [ordered]@{ succeeded = $true; residuals = @() }
    })
    $outputDirectory = Join-Path $root 'aggregate-pass'
    [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $runner -Action Aggregate -MatrixPath $matrixPath `
        -EvidenceDirectory $outputDirectory `
        -ScenarioEvidenceRoot $root `
        -SimulatedEvidencePath $simulatedPath 2>&1)
    Require-M3AggregateTest ($LASTEXITCODE -eq 0) `
        "aggregate rejected valid reports: $($output -join [Environment]::NewLine)"
    $aggregatePath = Join-Path $outputDirectory 'm3-real-crash-aggregate.json'
    Require-M3AggregateTest ([IO.File]::Exists($aggregatePath)) `
        'aggregate report is missing'
    $aggregate = Get-Content -LiteralPath $aggregatePath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    Require-M3AggregateTest ([string]$aggregate.status -eq 'PASS' -and
        [bool]$aggregate.task42_complete -and
        [bool]$aggregate.milestone_eligible -and
        [int]$aggregate.real_scenario_count -eq 10) `
        'aggregate did not certify the complete Task 42 matrix'

    $routeEvidenceBase = Join-Path $root 'route-evidence'
    [IO.Directory]::CreateDirectory($routeEvidenceBase) | Out-Null
    $routeOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $route -Tier Crash -CrashMode Full `
        -EvidenceBase $routeEvidenceBase `
        -RealCrashEvidenceRoot $root `
        -SimulatedCrashEvidencePath $simulatedPath 2>&1)
    $routeExit = $LASTEXITCODE
    Require-M3AggregateTest ($routeExit -eq 0) `
        "full Crash route rejected valid evidence: $($routeOutput -join [Environment]::NewLine)"
    $routeRuns = @(Get-ChildItem -LiteralPath $routeEvidenceBase -Directory)
    Require-M3AggregateTest ($routeRuns.Count -eq 1) `
        "full Crash route did not create exactly one run: $($routeRuns.Count)"
    $routeReportPath = Join-Path $routeRuns[0].FullName `
        'm3-real-crash-aggregate.json'
    Require-M3AggregateTest ([IO.File]::Exists($routeReportPath)) `
        'full Crash route aggregate report is missing'
    $routeReport = Get-Content -LiteralPath $routeReportPath -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    Require-M3AggregateTest ([string]$routeReport.status -eq 'PASS' -and
        [bool]$routeReport.task42_complete -and
        [bool]$routeReport.milestone_eligible) `
        'full Crash route overstated or omitted Task42 completion'

    $diagnosticDirectory = $scenarioDirectories[0]
    $diagnosticPath = Join-Path $diagnosticDirectory `
        'm3-real-crash-report.json'
    $diagnostic = Get-Content -LiteralPath $diagnosticPath -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    $diagnostic.image_inspect.fallback = $true
    Write-TestJson $diagnosticPath $diagnostic
    $diagnosticOutputDirectory = Join-Path $root 'aggregate-diagnostic-fail'
    [IO.Directory]::CreateDirectory($diagnosticOutputDirectory) | Out-Null
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $diagnosticOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File $runner -Action Aggregate `
            -MatrixPath $matrixPath `
            -EvidenceDirectory $diagnosticOutputDirectory `
            -ScenarioEvidenceRoot $root `
            -SimulatedEvidencePath $simulatedPath 2>&1)
        $diagnosticExit = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $savedPreference }
    Require-M3AggregateTest ($diagnosticExit -ne 0) `
        "aggregate accepted fallback diagnostics: $($diagnosticOutput -join [Environment]::NewLine)"
    $diagnostic.image_inspect.fallback = $false
    Write-TestJson $diagnosticPath $diagnostic

    $badDirectory = $scenarioDirectories[0]
    $badPath = Join-Path $badDirectory 'm3-real-crash-report.json'
    $bad = Get-Content -LiteralPath $badPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $bad.status = 'FAIL'
    Write-TestJson $badPath $bad
    $badOutputDirectory = Join-Path $root 'aggregate-fail'
    [IO.Directory]::CreateDirectory($badOutputDirectory) | Out-Null
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $badOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File $runner -Action Aggregate -MatrixPath $matrixPath `
            -EvidenceDirectory $badOutputDirectory `
            -ScenarioEvidenceRoot $root `
            -SimulatedEvidencePath $simulatedPath 2>&1)
        $badExitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $savedPreference }
    Require-M3AggregateTest ($badExitCode -ne 0) `
        "aggregate accepted a FAIL real scenario report: $($badOutput -join [Environment]::NewLine)"
    Write-Output 'M3 real crash aggregate test PASS'
}
finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
