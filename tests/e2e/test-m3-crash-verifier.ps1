[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3CrashVerifierTest {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Copy-M3CrashReport {
    param([Parameter(Mandatory = $true)]$Report)
    return (($Report | ConvertTo-Json -Depth 20 -Compress) | ConvertFrom-Json)
}

function Write-M3CrashReportFixture {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $Report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Invoke-M3CrashVerifierFixture {
    param(
        [Parameter(Mandatory = $true)][string]$Verifier,
        [Parameter(Mandatory = $true)][string]$ReportPath,
        [Parameter(Mandatory = $true)][string]$SourceCommit,
        [Parameter(Mandatory = $true)][string]$ExecutableHash,
        [Parameter(Mandatory = $true)][string]$MatrixHash,
        [Parameter(Mandatory = $true)][string]$RegistryHash,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath
    )
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $null = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Verifier `
            -Path $ReportPath -ExpectedSourceCommit $SourceCommit `
            -ExpectedExecutableSha256 $ExecutableHash `
            -ExpectedMatrixSha256 $MatrixHash `
            -ExpectedRegistrySha256 $RegistryHash `
            -StdoutPath $StdoutPath -StderrPath $StderrPath 2>&1
        return $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $oldPreference }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$verifier = Join-Path $repoRoot 'tools\Verify-M3CrashJson.ps1'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('myrd-m3-crash-verifier-' + [Guid]::NewGuid().ToString('N'))
$tempRoot = [IO.Path]::GetFullPath($tempRoot)
Require-M3CrashVerifierTest (-not [IO.Directory]::Exists($tempRoot) -and
    -not [IO.File]::Exists($tempRoot)) 'generated verifier temp path already exists'

try {
    [IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    $stdoutPath = Join-Path $tempRoot 'm3-crash-model.stdout.txt'
    $stderrPath = Join-Path $tempRoot 'm3-crash-model.stderr.txt'
    [IO.File]::WriteAllText($stdoutPath,
        "PASS checkpoint_crash_matrix_fixture_is_present_and_versioned`r`n6/6 passed`r`n",
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($stderrPath, '', [Text.UTF8Encoding]::new($false))

    $sourceCommit = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    $executableHash = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
    $matrixHash = 'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc'
    $registryHash = 'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd'
    $stdoutHash = (Get-FileHash -LiteralPath $stdoutPath -Algorithm SHA256).Hash
    $stderrHash = (Get-FileHash -LiteralPath $stderrPath -Algorithm SHA256).Hash

    $valid = [pscustomobject]@{
        schema = 'myrd.m3.crash-evidence.v1'
        status = 'PASS'
        terminal_verdict = 'PASS'
        scope = 'simulated'
        milestone_eligible = $false
        task42_complete = $false
        source_commit = $sourceCommit
        executable = [pscustomobject]@{ sha256 = $executableHash }
        fixtures = [pscustomobject]@{
            matrix_sha256 = $matrixHash
            registry_sha256 = $registryHash
            matrix_scenarios = 32
            registry_phases = 59
            kill_phases = 54
            short_write_phases = 10
            torn_write_phases = 1
        }
        execution = [pscustomobject]@{
            schedule_count = 10000
            process_started = $true
            process_exit_code = 0
            summary = '6/6 passed'
            stdout_sha256 = $stdoutHash
            stderr_sha256 = $stderrHash
        }
        coverage = [pscustomobject]@{
            simulated_file_clone = $true
            fresh_process = $true
            real_process_kill = $false
            real_aim_ntfs = $false
            controlled_power_cut = $false
        }
        cleanup = [pscustomobject]@{
            process_exited = $true
            succeeded = $true
            residuals = @()
        }
    }
    $reportPath = Join-Path $tempRoot 'm3-crash-report.json'
    Write-M3CrashReportFixture $valid $reportPath
    $validExit = Invoke-M3CrashVerifierFixture $verifier $reportPath `
        $sourceCommit $executableHash $matrixHash $registryHash `
        $stdoutPath $stderrPath
    Require-M3CrashVerifierTest ($validExit -eq 0) 'valid simulated evidence was rejected'

    $mutations = @(
        @{ Name = 'task42 complete'; Apply = { param($r) $r.task42_complete = $true } },
        @{ Name = 'milestone eligible'; Apply = { param($r) $r.milestone_eligible = $true } },
        @{ Name = 'real process kill'; Apply = { param($r) $r.coverage.real_process_kill = $true } },
        @{ Name = 'too few schedules'; Apply = { param($r) $r.execution.schedule_count = 9999 } },
        @{ Name = 'scenario count'; Apply = { param($r) $r.fixtures.matrix_scenarios = 31 } },
        @{ Name = 'process exit'; Apply = { param($r) $r.execution.process_exit_code = 1 } },
        @{ Name = 'summary'; Apply = { param($r) $r.execution.summary = '5/6 passed' } },
        @{ Name = 'source'; Apply = { param($r) $r.source_commit = ('e' * 40) } },
        @{ Name = 'executable hash'; Apply = { param($r) $r.executable.sha256 = ('e' * 64) } },
        @{ Name = 'matrix hash'; Apply = { param($r) $r.fixtures.matrix_sha256 = ('e' * 64) } },
        @{ Name = 'registry hash'; Apply = { param($r) $r.fixtures.registry_sha256 = ('e' * 64) } },
        @{ Name = 'raw log hash'; Apply = { param($r) $r.execution.stdout_sha256 = ('e' * 64) } },
        @{ Name = 'cleanup'; Apply = { param($r) $r.cleanup.succeeded = $false } }
    )
    foreach ($mutation in $mutations) {
        $invalid = Copy-M3CrashReport $valid
        & $mutation.Apply $invalid
        Write-M3CrashReportFixture $invalid $reportPath
        $exitCode = Invoke-M3CrashVerifierFixture $verifier $reportPath `
            $sourceCommit $executableHash $matrixHash $registryHash `
            $stdoutPath $stderrPath
        Require-M3CrashVerifierTest ($exitCode -ne 0) `
            "invalid fixture was accepted: $($mutation.Name)"
    }

    Write-Output 'M3 crash verifier tests PASS'
    exit 0
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) {
        [IO.Directory]::Delete($tempRoot, $true)
    }
    Require-M3CrashVerifierTest (-not [IO.Directory]::Exists($tempRoot)) `
        'verifier temp directory remained after cleanup'
}
