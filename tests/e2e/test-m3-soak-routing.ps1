[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Route,
    [Parameter(Mandatory = $true)][string]$Executable
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-SoakRouteTest {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-Route {
    param([string[]]$Arguments)
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File $Route @Arguments 2>&1)
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    }
    finally { $ErrorActionPreference = $old }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$tempRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) `
    ('myrd-m3-soak-route-' + [Guid]::NewGuid().ToString('N'))))
try {
    [IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    $listed = Invoke-Route @('-ListCommands')
    Require-SoakRouteTest ($listed.ExitCode -eq 0 -and
        (@($listed.Output) -join "`n") -match 'Soak') `
        'Soak route is missing from command listing'

    $defaultBlocked = Invoke-Route @('-Tier', 'Soak',
        '-EvidenceBase', $tempRoot)
    Require-SoakRouteTest ($defaultBlocked.ExitCode -eq 77) `
        'default Formal Soak missing inputs did not return BLOCKED/77'
    $defaultJson = @($defaultBlocked.Output | ForEach-Object { [string]$_ } |
        Where-Object { $_ -match '^\s*\{' } | Select-Object -Last 1) | ConvertFrom-Json
    Require-SoakRouteTest ($defaultJson.status -eq 'BLOCKED' -and
        $null -ne $defaultJson.PSObject.Properties['milestone_eligible'] -and
        $defaultJson.milestone_eligible -eq $false) `
        'default Formal Soak did not publish a blocked route result'

    $quickEvidence = Join-Path $tempRoot 'quick'
    [IO.Directory]::CreateDirectory($quickEvidence) | Out-Null
    $quick = Invoke-Route @(
        '-Tier', 'Soak', '-SoakMode', 'Quick',
        '-EvidenceBase', $quickEvidence, '-SoakExecutable', $Executable,
        '-Configuration', 'Release')
    Require-SoakRouteTest ($quick.ExitCode -eq 0) `
        'Quick Soak route failed'
    $quickReports = @(Get-ChildItem -LiteralPath $quickEvidence -Recurse `
        -Filter 'm3-soak-report.json' -File)
    Require-SoakRouteTest ($quickReports.Count -eq 1) `
        'Quick route did not publish exactly one report'
    $quickReport = Get-Content -LiteralPath $quickReports[0].FullName -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    Require-SoakRouteTest ($quickReport.profile -eq 'Quick' -and
        $quickReport.task43_complete -eq $false -and
        $quickReport.milestone_eligible -eq $false) `
        'Quick route incorrectly claimed milestone completion'

    $fakeFormal = Invoke-Route @(
        '-Tier', 'Soak', '-SoakMode', 'Formal',
        '-EvidenceBase', $quickEvidence, '-SoakExecutable', $Executable,
        '-RealFunctionalReportPath', $quickReports[0].FullName,
        '-CrashAggregateReportPath', $quickReports[0].FullName,
        '-Configuration', 'Release')
    Require-SoakRouteTest ($fakeFormal.ExitCode -eq 77) `
        'Quick evidence was accepted as Formal dependency input'
    Require-SoakRouteTest ((@($fakeFormal.Output) -join "`n") -notmatch
        'Soak Formal PASS') 'fake Formal route printed PASS'

    $source = ((& git.exe -C $repoRoot rev-parse HEAD) -join '').Trim()
    $minimalRealPath = Join-Path $tempRoot 'minimal-real.json'
    $minimalCrashPath = Join-Path $tempRoot 'minimal-crash.json'
    [IO.File]::WriteAllText($minimalRealPath, ([ordered]@{
        schema = 'myrd.m3.ntfs-workload.v1'; status = 'PASS'
        terminal_verdict = 'PASS'; source_commit = $source
    } | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($minimalCrashPath, ([ordered]@{
        schema = 'myrd.m3.real-crash-aggregate.v1'; status = 'PASS'
        terminal_verdict = 'PASS'; task42_complete = $true
        milestone_eligible = $true; source_commit = $source
        guarantees = [ordered]@{
            latest_complete_confirmed_checkpoint = $true
            mixed_generation_rejected = $true
        }
    } | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    $minimalFormal = Invoke-Route @(
        '-Tier', 'Soak', '-SoakMode', 'Formal',
        '-EvidenceBase', $quickEvidence, '-SoakExecutable', $Executable,
        '-RealFunctionalReportPath', $minimalRealPath,
        '-CrashAggregateReportPath', $minimalCrashPath,
        '-Configuration', 'Release')
    Require-SoakRouteTest ($minimalFormal.ExitCode -eq 77) `
        'minimal synthetic Formal dependencies were not blocked before workload'

    $insideRepo = Invoke-Route @(
        '-Tier', 'Soak', '-SoakMode', 'Quick',
        '-EvidenceBase', (Join-Path $repoRoot 'tests'),
        '-SoakExecutable', $Executable)
    Require-SoakRouteTest ($insideRepo.ExitCode -ne 0) `
        'repository-contained route evidence was accepted'

    Write-Output 'M3 soak routing contract PASS'
    exit 0
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) {
        [IO.Directory]::Delete($tempRoot, $true)
    }
}
