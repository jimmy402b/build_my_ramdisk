[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$CrashModelExecutable = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3CrashRunnerTest {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-M3CrashRunnerTestProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Runner,
        [Parameter(Mandatory = $true)][string]$Phase,
        [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string]$MatrixPath,
        [Parameter(Mandatory = $true)][string]$RegistryPath
    )
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File $Runner -Phase $Phase -Configuration $Configuration `
            -EvidenceDirectory $EvidenceDirectory `
            -CrashModelExecutable $Executable -MatrixPath $MatrixPath `
            -RegistryPath $RegistryPath -ScheduleCount 10000 2>&1)
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    }
    finally { $ErrorActionPreference = $oldPreference }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$runner = Join-Path $PSScriptRoot 'Invoke-M3CrashMatrix.ps1'
$verifier = Join-Path $repoRoot 'tools\Verify-M3CrashJson.ps1'
$matrixPath = Join-Path $repoRoot 'tests\fixtures\m3\crash-matrix.json'
$registryPath = Join-Path $repoRoot 'tests\fixtures\m3\fault-phase-registry.json'
if ([string]::IsNullOrWhiteSpace($CrashModelExecutable)) {
    $CrashModelExecutable = Join-Path $repoRoot `
        "out\build\msvc\tests\$Configuration\myrd_m3_checkpoint_crash_model_tests.exe"
}
$CrashModelExecutable = [IO.Path]::GetFullPath($CrashModelExecutable)
Require-M3CrashRunnerTest ([IO.File]::Exists($CrashModelExecutable)) `
    "crash-model executable is missing: $CrashModelExecutable"

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('myrd-m3-crash-runner-' + [Guid]::NewGuid().ToString('N'))
$tempRoot = [IO.Path]::GetFullPath($tempRoot)
Require-M3CrashRunnerTest (-not [IO.Directory]::Exists($tempRoot) -and
    -not [IO.File]::Exists($tempRoot)) 'generated crash-runner temp path already exists'

try {
    [IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    $validateEvidence = Join-Path $tempRoot 'validate'
    $runEvidence = Join-Path $tempRoot 'run'
    [IO.Directory]::CreateDirectory($validateEvidence) | Out-Null
    [IO.Directory]::CreateDirectory($runEvidence) | Out-Null

    $validated = Invoke-M3CrashRunnerTestProcess $runner 'Validate' `
        $validateEvidence $CrashModelExecutable $matrixPath $registryPath
    Require-M3CrashRunnerTest ($validated.ExitCode -eq 0) `
        "valid crash fixture validation failed: $($validated.Output -join [Environment]::NewLine)"
    $validationPath = Join-Path $validateEvidence 'm3-crash-validation.json'
    Require-M3CrashRunnerTest ([IO.File]::Exists($validationPath)) `
        'crash validation report is missing'
    $validation = Get-Content -Raw -LiteralPath $validationPath -Encoding utf8 | ConvertFrom-Json
    Require-M3CrashRunnerTest ([string]$validation.schema -eq 'myrd.m3.crash-validation.v1') `
        'crash validation schema mismatch'
    Require-M3CrashRunnerTest ([string]$validation.status -eq 'PASS') `
        'crash validation status is not PASS'
    Require-M3CrashRunnerTest (-not [bool]$validation.process_started) `
        'Validate phase started the crash model'
    Require-M3CrashRunnerTest ([int]$validation.fixtures.matrix_scenarios -eq 32 -and
        [int]$validation.fixtures.registry_phases -eq 59) `
        'Validate phase fixture counts are wrong'
    Require-M3CrashRunnerTest (-not [IO.File]::Exists(
        (Join-Path $validateEvidence 'm3-crash-model.stdout.txt'))) `
        'Validate phase created crash-model stdout'

    $ran = Invoke-M3CrashRunnerTestProcess $runner 'Run' $runEvidence `
        $CrashModelExecutable $matrixPath $registryPath
    Require-M3CrashRunnerTest ($ran.ExitCode -eq 0) `
        "simulated crash runner failed: $($ran.Output -join [Environment]::NewLine)"
    $reportPath = Join-Path $runEvidence 'm3-crash-report.json'
    $stdoutPath = Join-Path $runEvidence 'm3-crash-model.stdout.txt'
    $stderrPath = Join-Path $runEvidence 'm3-crash-model.stderr.txt'
    foreach ($path in @($reportPath, $stdoutPath, $stderrPath)) {
        Require-M3CrashRunnerTest ([IO.File]::Exists($path)) `
            "simulated crash evidence is missing: $path"
    }
    $sourceCommit = ((& git.exe -C $repoRoot rev-parse HEAD 2>$null) -join '').Trim()
    Require-M3CrashRunnerTest ($sourceCommit -match '^[0-9A-Fa-f]{40}$') `
        'current source commit is unavailable'
    $null = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verifier `
        -Path $reportPath -ExpectedSourceCommit $sourceCommit `
        -ExpectedExecutableSha256 (Get-FileHash -LiteralPath $CrashModelExecutable -Algorithm SHA256).Hash `
        -ExpectedMatrixSha256 (Get-FileHash -LiteralPath $matrixPath -Algorithm SHA256).Hash `
        -ExpectedRegistrySha256 (Get-FileHash -LiteralPath $registryPath -Algorithm SHA256).Hash `
        -StdoutPath $stdoutPath -StderrPath $stderrPath
    Require-M3CrashRunnerTest ($LASTEXITCODE -eq 0) `
        'runner evidence failed independent verification'

    $fixtureRoot = Join-Path $tempRoot 'mutated-fixtures'
    [IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
    $matrix = Get-Content -Raw -LiteralPath $matrixPath -Encoding utf8 | ConvertFrom-Json
    $matrix.scenarios[1].id = [string]$matrix.scenarios[0].id
    $duplicateMatrix = Join-Path $fixtureRoot 'duplicate-matrix.json'
    $matrix | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $duplicateMatrix -Encoding utf8
    $duplicateEvidence = Join-Path $tempRoot 'duplicate-evidence'
    [IO.Directory]::CreateDirectory($duplicateEvidence) | Out-Null
    $duplicate = Invoke-M3CrashRunnerTestProcess $runner 'Validate' `
        $duplicateEvidence $CrashModelExecutable $duplicateMatrix $registryPath
    Require-M3CrashRunnerTest ($duplicate.ExitCode -ne 0) `
        'duplicate crash scenario ID was accepted'
    Require-M3CrashRunnerTest (-not [IO.File]::Exists(
        (Join-Path $duplicateEvidence 'm3-crash-model.stdout.txt'))) `
        'invalid duplicate fixture started the crash model'

    $matrix = Get-Content -Raw -LiteralPath $matrixPath -Encoding utf8 | ConvertFrom-Json
    $matrix.scenarios[0].PSObject.Properties.Remove('id')
    $missingMatrix = Join-Path $fixtureRoot 'missing-id-matrix.json'
    $matrix | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $missingMatrix -Encoding utf8
    $missingEvidence = Join-Path $tempRoot 'missing-evidence'
    [IO.Directory]::CreateDirectory($missingEvidence) | Out-Null
    $missing = Invoke-M3CrashRunnerTestProcess $runner 'Validate' `
        $missingEvidence $CrashModelExecutable $missingMatrix $registryPath
    Require-M3CrashRunnerTest ($missing.ExitCode -ne 0) `
        'missing crash scenario ID was accepted'
    Require-M3CrashRunnerTest (-not [IO.File]::Exists(
        (Join-Path $missingEvidence 'm3-crash-model.stdout.txt'))) `
        'invalid missing-ID fixture started the crash model'

    Write-Output "M3 simulated crash runner tests PASS: $Configuration"
    exit 0
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) {
        [IO.Directory]::Delete($tempRoot, $true)
    }
    Require-M3CrashRunnerTest (-not [IO.Directory]::Exists($tempRoot)) `
        'crash-runner temp directory remained after cleanup'
}
