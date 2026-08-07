[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [Parameter(Mandatory = $true)][string]$ExpectedExecutableSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedMatrixSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedRegistrySha256,
    [Parameter(Mandatory = $true)][string]$StdoutPath,
    [Parameter(Mandatory = $true)][string]$StderrPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3CrashEvidence {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Require-M3CrashProperty {
    param($Object, [Parameter(Mandatory = $true)][string]$Name)
    Require-M3CrashEvidence ($null -ne $Object -and
        $null -ne $Object.PSObject.Properties[$Name]) `
        "required crash evidence property is missing: $Name"
    return $Object.PSObject.Properties[$Name].Value
}

function Require-M3CrashBoolean {
    param(
        $Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [bool]$Expected
    )
    $value = Require-M3CrashProperty $Object $Name
    Require-M3CrashEvidence ($value -is [bool]) "$Name is not a JSON boolean"
    Require-M3CrashEvidence ([bool]$value -eq $Expected) "$Name has an unsafe value"
}

function Require-M3CrashHash {
    param($Value, [Parameter(Mandatory = $true)][string]$Name)
    Require-M3CrashEvidence ([string]$Value -match '^[0-9A-Fa-f]{64}$') `
        "$Name is not SHA-256"
}

function Require-M3CrashExactCount {
    param(
        $Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [uint64]$Expected
    )
    $value = Require-M3CrashProperty $Object $Name
    try { $count = [uint64]$value }
    catch { throw "$Name is not an unsigned count" }
    Require-M3CrashEvidence ($count -eq $Expected) "$Name does not match the frozen count"
}

function Read-M3CrashUtf8 {
    param([Parameter(Mandatory = $true)][string]$InputPath)
    $full = [IO.Path]::GetFullPath($InputPath)
    Require-M3CrashEvidence ([IO.File]::Exists($full)) "evidence file is missing: $full"
    return [IO.File]::ReadAllText($full, [Text.UTF8Encoding]::new($false, $true))
}

$status = 'FAIL'
try {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $raw = Read-M3CrashUtf8 $fullPath
    $report = $raw | ConvertFrom-Json
    foreach ($name in @(
            'schema', 'status', 'terminal_verdict', 'scope',
            'milestone_eligible', 'task42_complete', 'source_commit',
            'executable', 'fixtures', 'execution', 'coverage', 'cleanup')) {
        $null = Require-M3CrashProperty $report $name
    }

    Require-M3CrashEvidence ([string]$report.schema -eq 'myrd.m3.crash-evidence.v1') `
        'crash evidence schema is invalid'
    Require-M3CrashEvidence ([string]$report.status -eq 'PASS' -and
        [string]$report.terminal_verdict -eq 'PASS') `
        'crash evidence terminal verdict is not PASS'
    Require-M3CrashEvidence ([string]$report.scope -eq 'simulated') `
        'crash evidence is not simulated scope'
    Require-M3CrashBoolean $report 'milestone_eligible' $false
    Require-M3CrashBoolean $report 'task42_complete' $false

    Require-M3CrashEvidence ([string]$ExpectedSourceCommit -match '^[0-9A-Fa-f]{40}$') `
        'expected source commit is invalid'
    Require-M3CrashEvidence ([string]$report.source_commit -match '^[0-9A-Fa-f]{40}$') `
        'report source commit is invalid'
    Require-M3CrashEvidence ([string]$report.source_commit -ieq $ExpectedSourceCommit) `
        'source commit mismatch'

    $reportExecutableHash = Require-M3CrashProperty $report.executable 'sha256'
    $reportMatrixHash = Require-M3CrashProperty $report.fixtures 'matrix_sha256'
    $reportRegistryHash = Require-M3CrashProperty $report.fixtures 'registry_sha256'
    foreach ($hash in @(
            @{ Value = $ExpectedExecutableSha256; Name = 'expected executable' },
            @{ Value = $ExpectedMatrixSha256; Name = 'expected matrix' },
            @{ Value = $ExpectedRegistrySha256; Name = 'expected registry' },
            @{ Value = $reportExecutableHash; Name = 'report executable' },
            @{ Value = $reportMatrixHash; Name = 'report matrix' },
            @{ Value = $reportRegistryHash; Name = 'report registry' })) {
        Require-M3CrashHash $hash.Value $hash.Name
    }
    Require-M3CrashEvidence ([string]$reportExecutableHash -ieq $ExpectedExecutableSha256) `
        'crash-model executable hash mismatch'
    Require-M3CrashEvidence ([string]$reportMatrixHash -ieq $ExpectedMatrixSha256) `
        'crash matrix hash mismatch'
    Require-M3CrashEvidence ([string]$reportRegistryHash -ieq $ExpectedRegistrySha256) `
        'fault registry hash mismatch'

    Require-M3CrashExactCount $report.fixtures 'matrix_scenarios' 32
    Require-M3CrashExactCount $report.fixtures 'registry_phases' 59
    Require-M3CrashExactCount $report.fixtures 'kill_phases' 54
    Require-M3CrashExactCount $report.fixtures 'short_write_phases' 10
    Require-M3CrashExactCount $report.fixtures 'torn_write_phases' 1

    $scheduleValue = Require-M3CrashProperty $report.execution 'schedule_count'
    try { $scheduleCount = [uint64]$scheduleValue }
    catch { throw 'schedule_count is not an unsigned count' }
    Require-M3CrashEvidence ($scheduleCount -ge 10000) `
        'simulated crash evidence ran fewer than 10000 schedules'
    Require-M3CrashBoolean $report.execution 'process_started' $true
    $exitValue = Require-M3CrashProperty $report.execution 'process_exit_code'
    Require-M3CrashEvidence ([int]$exitValue -eq 0) 'crash-model child exit code is nonzero'
    $summary = [string](Require-M3CrashProperty $report.execution 'summary')
    Require-M3CrashEvidence ($summary -eq '6/6 passed') `
        'crash-model summary is incomplete'

    Require-M3CrashBoolean $report.coverage 'simulated_file_clone' $true
    Require-M3CrashBoolean $report.coverage 'fresh_process' $true
    Require-M3CrashBoolean $report.coverage 'real_process_kill' $false
    Require-M3CrashBoolean $report.coverage 'real_aim_ntfs' $false
    Require-M3CrashBoolean $report.coverage 'controlled_power_cut' $false
    Require-M3CrashBoolean $report.cleanup 'process_exited' $true
    Require-M3CrashBoolean $report.cleanup 'succeeded' $true
    $residuals = @(Require-M3CrashProperty $report.cleanup 'residuals')
    Require-M3CrashEvidence ($residuals.Count -eq 0) 'crash evidence cleanup has residuals'

    $stdoutFull = [IO.Path]::GetFullPath($StdoutPath)
    $stderrFull = [IO.Path]::GetFullPath($StderrPath)
    $stdout = Read-M3CrashUtf8 $stdoutFull
    $null = Read-M3CrashUtf8 $stderrFull
    Require-M3CrashEvidence ($stdout -match '(?m)^6/6 passed\r?$') `
        'raw crash-model stdout lacks the complete summary'
    $actualStdoutHash = (Get-FileHash -LiteralPath $stdoutFull -Algorithm SHA256).Hash
    $actualStderrHash = (Get-FileHash -LiteralPath $stderrFull -Algorithm SHA256).Hash
    $reportStdoutHash = Require-M3CrashProperty $report.execution 'stdout_sha256'
    $reportStderrHash = Require-M3CrashProperty $report.execution 'stderr_sha256'
    Require-M3CrashHash $reportStdoutHash 'report stdout'
    Require-M3CrashHash $reportStderrHash 'report stderr'
    Require-M3CrashEvidence ([string]$reportStdoutHash -ieq $actualStdoutHash) `
        'raw stdout hash mismatch'
    Require-M3CrashEvidence ([string]$reportStderrHash -ieq $actualStderrHash) `
        'raw stderr hash mismatch'

    Write-Output "M3 simulated crash JSON validation PASS: $fullPath"
    $status = 'PASS'
    exit 0
}
catch {
    [Console]::Error.WriteLine(
        "M3 simulated crash JSON validation ${status}: $([string]$_.Exception.Message)")
    exit 1
}
