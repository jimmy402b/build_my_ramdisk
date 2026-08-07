[CmdletBinding()]
param(
    [ValidateSet('Local', 'Preflight', 'RealFunctional', 'Crash', 'Soak')]
    [string]$Tier = 'Local',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$EvidenceBase = '',
    [string]$Executable = '',
    [string]$ServerExecutable = '',
    [string]$BundleRoot = '',
    [string]$ManifestName = '',
    [string]$MountDirectory = '',
    [string]$ScratchRoot = '',
    [string]$ExpectedDiskIdentityJson = '',
    [string]$ExpectedDriverService = 'phdskmnt',
    [ValidateSet('Full', 'Simulated')][string]$CrashMode = 'Full',
    [string]$CrashExecutable = '',
    [string]$RealCrashEvidenceRoot = '',
    [string]$SimulatedCrashEvidencePath = '',
    [ValidateSet('Quick', 'Formal')][string]$SoakMode = 'Formal',
    [string]$SoakExecutable = '',
    [string]$RealFunctionalReportPath = '',
    [string]$CrashAggregateReportPath = '',
    [switch]$SafetyAcknowledged,
    [switch]$WhatIf,
    [switch]$ListCommands
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot

function Require-M3Route {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

if ($ListCommands) {
    Write-Output 'Local: Debug/Release CTest regression only'
    Write-Output 'Preflight: read-only host/artifact/scratch/disk safety probe'
    Write-Output 'RealFunctional: real AIM/NTFS/checkpoint/reopen/compact/native-VHDX acceptance; requires approved scratch identity'
    Write-Output 'Crash: Full=aggregate explicit real-process/power-cut evidence; Simulated=file-clone/fresh-process oracle only'
    Write-Output 'Soak: Quick deterministic feedback or Formal same-commit milestone gate'
    exit 0
}

if ($Tier -eq 'Local') {
    $buildDirectory = Join-Path $repoRoot 'out\build\msvc'
    Require-M3Route ([IO.Directory]::Exists($buildDirectory)) "build directory is missing: $buildDirectory"
    $ctest = Get-Command ctest.exe -ErrorAction SilentlyContinue
    Require-M3Route ($null -ne $ctest) 'ctest.exe is not available'
    & $ctest.Source '--test-dir' $buildDirectory '-C' $Configuration '--output-on-failure'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Output "M3 Local PASS: $Configuration CTest"
    exit 0
}

if ($Tier -eq 'Crash') {
    if ($CrashMode -eq 'Full') {
        $missingFullInput = [string]::IsNullOrWhiteSpace($EvidenceBase) -or
            [string]::IsNullOrWhiteSpace($RealCrashEvidenceRoot) -or
            [string]::IsNullOrWhiteSpace($SimulatedCrashEvidencePath)
        if ($missingFullInput) {
            Write-Output ([ordered]@{
                schema = 'myrd.m3.verification-route.v1'
                tier = $Tier
                crash_mode = $CrashMode
                status = 'BLOCKED'
                reason = 'Full crash acceptance requires EvidenceBase, RealCrashEvidenceRoot, and SimulatedCrashEvidencePath'
                required_skip = $true
                task42_complete = $false
                milestone_eligible = $false
            } | ConvertTo-Json -Depth 5 -Compress)
            exit 77
        }

        $evidenceBaseFull = [IO.Path]::GetFullPath($EvidenceBase)
        Require-M3Route ([IO.Directory]::Exists($evidenceBaseFull)) `
            'EvidenceBase must already exist'
        $evidenceBaseItem = Get-Item -LiteralPath $evidenceBaseFull -Force
        Require-M3Route (($evidenceBaseItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            'EvidenceBase cannot be a reparse point'
        $repoFull = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
        Require-M3Route (-not $evidenceBaseFull.TrimEnd('\').StartsWith(
                $repoFull, [StringComparison]::OrdinalIgnoreCase)) `
            'EvidenceBase cannot be inside the repository'

        $realEvidenceFull = [IO.Path]::GetFullPath($RealCrashEvidenceRoot)
        Require-M3Route ([IO.Directory]::Exists($realEvidenceFull)) `
            'RealCrashEvidenceRoot must already exist'
        $realEvidenceItem = Get-Item -LiteralPath $realEvidenceFull -Force
        Require-M3Route (($realEvidenceItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            'RealCrashEvidenceRoot cannot be a reparse point'
        $simulatedEvidenceFull = [IO.Path]::GetFullPath(
            $SimulatedCrashEvidencePath)
        Require-M3Route ([IO.File]::Exists($simulatedEvidenceFull)) `
            'SimulatedCrashEvidencePath must already exist'
        $simulatedEvidenceItem = Get-Item -LiteralPath `
            $simulatedEvidenceFull -Force
        Require-M3Route (($simulatedEvidenceItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            'SimulatedCrashEvidencePath cannot be a reparse point'

        $run = Join-Path $evidenceBaseFull ('crash-full-' +
            [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff') + '-' +
            [Guid]::NewGuid().ToString('N'))
        [IO.Directory]::CreateDirectory($run) | Out-Null
        $runner = Join-Path $repoRoot `
            'tests\e2e\Invoke-M3RealCrashScenario.ps1'
        $matrix = Join-Path $repoRoot `
            'tests\fixtures\m3\real-crash-matrix.json'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner `
            -Action Aggregate -MatrixPath $matrix -EvidenceDirectory $run `
            -ScenarioEvidenceRoot $realEvidenceFull `
            -SimulatedEvidencePath $simulatedEvidenceFull
        $childExit = $LASTEXITCODE
        if ($childExit -ne 0) { exit $childExit }

        $aggregatePath = Join-Path $run 'm3-real-crash-aggregate.json'
        Require-M3Route ([IO.File]::Exists($aggregatePath)) `
            'Full crash aggregate report is missing'
        $aggregate = Get-Content -LiteralPath $aggregatePath -Raw `
            -Encoding UTF8 | ConvertFrom-Json
        Require-M3Route ([string]$aggregate.schema -eq
            'myrd.m3.real-crash-aggregate.v1' -and
            [string]$aggregate.status -eq 'PASS' -and
            [string]$aggregate.terminal_verdict -eq 'PASS' -and
            [bool]$aggregate.task42_complete -and
            [bool]$aggregate.milestone_eligible -and
            [int]$aggregate.real_scenario_count -eq 10 -and
            [int]$aggregate.process_kill_count -eq 8 -and
            [int]$aggregate.controlled_power_cut_count -eq 2 -and
            [bool]$aggregate.guarantees.manifest_slot_diagnostics_verified) `
            'Full crash aggregate did not certify the complete Task42 matrix'
        Write-Output "M3 Crash Full PASS: $aggregatePath"
        exit 0
    }

    Require-M3Route (-not [string]::IsNullOrWhiteSpace($EvidenceBase)) `
        'Crash Simulated requires EvidenceBase'
    Require-M3Route (-not [string]::IsNullOrWhiteSpace($CrashExecutable)) `
        'Crash Simulated requires CrashExecutable'
    $evidenceBaseFull = [IO.Path]::GetFullPath($EvidenceBase)
    Require-M3Route ([IO.Directory]::Exists($evidenceBaseFull)) `
        'EvidenceBase must already exist'
    $evidenceBaseItem = Get-Item -LiteralPath $evidenceBaseFull -Force
    Require-M3Route (($evidenceBaseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'EvidenceBase cannot be a reparse point'
    $repoFull = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
    Require-M3Route (-not $evidenceBaseFull.TrimEnd('\').StartsWith(
            $repoFull, [StringComparison]::OrdinalIgnoreCase)) `
        'EvidenceBase cannot be inside the repository'
    $crashExecutableFull = [IO.Path]::GetFullPath($CrashExecutable)
    Require-M3Route ([IO.File]::Exists($crashExecutableFull)) `
        "CrashExecutable does not exist: $crashExecutableFull"
    $run = Join-Path $evidenceBaseFull ('crash-simulated-' +
        [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff') + '-' +
        [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($run) | Out-Null
    $runner = Join-Path $repoRoot 'tests\e2e\Invoke-M3CrashMatrix.ps1'
    $matrix = Join-Path $repoRoot 'tests\fixtures\m3\crash-matrix.json'
    $registry = Join-Path $repoRoot 'tests\fixtures\m3\fault-phase-registry.json'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner `
        -Phase Run -Configuration $Configuration -EvidenceDirectory $run `
        -CrashModelExecutable $crashExecutableFull -MatrixPath $matrix `
        -RegistryPath $registry
    $childExit = $LASTEXITCODE
    if ($childExit -ne 0) { exit $childExit }

    $sourceCommit = ((& git.exe -C $repoRoot rev-parse HEAD 2>$null) -join '').Trim()
    Require-M3Route ($sourceCommit -match '^[0-9A-Fa-f]{40}$') `
        'source commit is unavailable for Crash Simulated evidence'
    $verifier = Join-Path $repoRoot 'tools\Verify-M3CrashJson.ps1'
    $matrixHash = (Get-FileHash -LiteralPath $matrix -Algorithm SHA256).Hash
    $registryHash = (Get-FileHash -LiteralPath $registry -Algorithm SHA256).Hash
    $crashHash = (Get-FileHash -LiteralPath $crashExecutableFull -Algorithm SHA256).Hash
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verifier `
        -Path (Join-Path $run 'm3-crash-report.json') `
        -ExpectedSourceCommit $sourceCommit `
        -ExpectedExecutableSha256 $crashHash `
        -ExpectedMatrixSha256 $matrixHash `
        -ExpectedRegistrySha256 $registryHash `
        -StdoutPath (Join-Path $run 'm3-crash-model.stdout.txt') `
        -StderrPath (Join-Path $run 'm3-crash-model.stderr.txt')
    $verifierExit = $LASTEXITCODE
    if ($verifierExit -ne 0) { exit $verifierExit }
    Write-Output "M3 Crash Simulated PASS: $(Join-Path $run 'm3-crash-report.json')"
    Write-Output 'Task42 remains incomplete: no real process-kill, AIM/NTFS, or power-cut evidence'
    exit 0
}

if ($Tier -eq 'Soak') {
    function Write-M3SoakRouteBlocked([string]$Reason) {
        Write-Output ([ordered]@{
            schema = 'myrd.m3.verification-route.v1'
            tier = 'Soak'
            soak_mode = $SoakMode
            status = 'BLOCKED'
            reason = $Reason
            required_skip = $true
            task43_complete = $false
            milestone_eligible = $false
        } | ConvertTo-Json -Depth 8 -Compress)
        exit 77
    }

    if ($WhatIf) {
        Write-M3SoakRouteBlocked 'WhatIf is a safety probe, not soak execution'
    }
    if ([string]::IsNullOrWhiteSpace($EvidenceBase) -or
        [string]::IsNullOrWhiteSpace($SoakExecutable)) {
        Write-M3SoakRouteBlocked `
            'Soak requires EvidenceBase and SoakExecutable'
    }
    if ($SoakMode -eq 'Formal' -and
        ([string]::IsNullOrWhiteSpace($RealFunctionalReportPath) -or
         [string]::IsNullOrWhiteSpace($CrashAggregateReportPath))) {
        Write-M3SoakRouteBlocked `
            'Formal Soak requires same-commit Task41 and Task42 reports'
    }
    if ($SoakMode -eq 'Quick' -and
        (-not [string]::IsNullOrWhiteSpace($RealFunctionalReportPath) -or
         -not [string]::IsNullOrWhiteSpace($CrashAggregateReportPath))) {
        Write-M3SoakRouteBlocked 'Quick Soak cannot bind milestone reports'
    }

    $evidenceBaseFull = [IO.Path]::GetFullPath($EvidenceBase)
    Require-M3Route ([IO.Directory]::Exists($evidenceBaseFull)) `
        'EvidenceBase must already exist'
    $evidenceBaseItem = Get-Item -LiteralPath $evidenceBaseFull -Force
    Require-M3Route (($evidenceBaseItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'EvidenceBase cannot be a reparse point'
    $repoFull = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
    Require-M3Route (-not $evidenceBaseFull.TrimEnd('\').StartsWith(
            $repoFull, [StringComparison]::OrdinalIgnoreCase)) `
        'EvidenceBase cannot be inside the repository'
    $soakExecutableFull = [IO.Path]::GetFullPath($SoakExecutable)
    Require-M3Route ([IO.File]::Exists($soakExecutableFull)) `
        'SoakExecutable does not exist'
    $soakExecutableItem = Get-Item -LiteralPath $soakExecutableFull -Force
    Require-M3Route (($soakExecutableItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'SoakExecutable cannot be a reparse point'
    $sourceCommit = ((& git.exe -C $repoRoot rev-parse HEAD 2>$null) -join '').Trim()
    Require-M3Route ($sourceCommit -match '^[0-9A-Fa-f]{40}$') `
        'source commit is unavailable for Soak'

    if ($SoakMode -eq 'Formal') {
        try {
            $realPathFull = [IO.Path]::GetFullPath($RealFunctionalReportPath)
            $crashPathFull = [IO.Path]::GetFullPath($CrashAggregateReportPath)
            foreach ($path in @($realPathFull, $crashPathFull)) {
                Require-M3Route ([IO.File]::Exists($path)) `
                    'Formal dependency report does not exist'
                $item = Get-Item -LiteralPath $path -Force
                Require-M3Route (($item.Attributes -band
                        [IO.FileAttributes]::ReparsePoint) -eq 0) `
                    'Formal dependency report cannot be a reparse point'
            }
            $real = Get-Content -LiteralPath $realPathFull -Raw -Encoding UTF8 |
                ConvertFrom-Json
            $crash = Get-Content -LiteralPath $crashPathFull -Raw -Encoding UTF8 |
                ConvertFrom-Json
            Require-M3Route ($real.schema -eq 'myrd.m3.ntfs-workload.v1' -and
                $real.status -eq 'PASS' -and
                $real.terminal_verdict -eq 'PASS' -and
                [string]$real.source_commit -ieq $sourceCommit) `
                'Task41 dependency is not a PASS from the current commit'
            Require-M3Route ($crash.schema -eq
                'myrd.m3.real-crash-aggregate.v1' -and
                $crash.status -eq 'PASS' -and
                $crash.terminal_verdict -eq 'PASS' -and
                [bool]$crash.task42_complete -and
                [bool]$crash.milestone_eligible -and
                [string]$crash.source_commit -ieq $sourceCommit -and
                [bool]$crash.guarantees.latest_complete_confirmed_checkpoint -and
                [bool]$crash.guarantees.mixed_generation_rejected) `
                'Task42 dependency is not a complete current-commit PASS'
        }
        catch {
            Write-M3SoakRouteBlocked $_.Exception.Message
        }
    }

    $runner = Join-Path $repoRoot 'tests\e2e\Invoke-M3Soak.ps1'
    $runnerArguments = @(
        '-Phase', 'Run', '-Profile', $SoakMode,
        '-EvidenceDirectory', $evidenceBaseFull,
        '-SoakExecutable', $soakExecutableFull,
        '-Configuration', $Configuration)
    if ($SoakMode -eq 'Formal') {
        $runnerArguments += @(
            '-RealFunctionalReport', ([IO.Path]::GetFullPath($RealFunctionalReportPath)),
            '-CrashAggregateReport', ([IO.Path]::GetFullPath($CrashAggregateReportPath)))
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner `
        @runnerArguments
    $soakExit = $LASTEXITCODE
    if ($soakExit -ne 0) { exit $soakExit }
    exit 0
}

if ($Tier -eq 'RealFunctional') {
    if ($WhatIf) {
        Write-Output ([ordered]@{
            schema = 'myrd.m3.verification-route.v1'
            tier = $Tier
            status = 'BLOCKED'
            reason = 'WhatIf is a safety probe, not functional acceptance'
            required_skip = $true
        } | ConvertTo-Json -Depth 5 -Compress)
        exit 77
    }
    $required = @{
        ImageExecutable = $Executable
        ServerExecutable = $ServerExecutable
        BundleRoot = $BundleRoot
        ManifestName = $ManifestName
        ScratchRoot = $ScratchRoot
        MountDirectory = $MountDirectory
        ExpectedScratchDiskIdentityJson = $ExpectedDiskIdentityJson
    }
    foreach ($entry in $required.GetEnumerator()) {
        Require-M3Route (-not [string]::IsNullOrWhiteSpace([string]$entry.Value)) `
            "RealFunctional requires $($entry.Key)"
    }
    Require-M3Route $SafetyAcknowledged 'RealFunctional requires explicit SafetyAcknowledged'
    Require-M3Route (-not [string]::IsNullOrWhiteSpace($EvidenceBase)) 'EvidenceBase is required for RealFunctional'
    $evidenceBaseFull = [IO.Path]::GetFullPath($EvidenceBase)
    Require-M3Route ([IO.Directory]::Exists($evidenceBaseFull)) 'EvidenceBase must already exist'
    $repoFull = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
    Require-M3Route (-not $evidenceBaseFull.TrimEnd('\').StartsWith($repoFull, [StringComparison]::OrdinalIgnoreCase)) `
        'EvidenceBase cannot be inside the repository'
    $run = Join-Path $evidenceBaseFull ('real-functional-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff') + '-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($run) | Out-Null
    $scriptPath = Join-Path $repoRoot 'tests\e2e\Invoke-M3NtfsWorkload.ps1'
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scriptPath,
        '-Phase', 'All', '-Configuration', $Configuration,
        '-EvidenceDirectory', $run,
        '-ServerExecutable', ([IO.Path]::GetFullPath($ServerExecutable)),
        '-ImageExecutable', ([IO.Path]::GetFullPath($Executable)),
        '-BundleRoot', ([IO.Path]::GetFullPath($BundleRoot)),
        '-ManifestName', $ManifestName,
        '-ScratchRoot', ([IO.Path]::GetFullPath($ScratchRoot)),
        '-MountDirectory', ([IO.Path]::GetFullPath($MountDirectory)),
        '-ExpectedScratchDiskIdentityJson', ([IO.Path]::GetFullPath($ExpectedDiskIdentityJson)),
        '-SafetyAcknowledged')
    & powershell.exe @arguments
    $childExit = $LASTEXITCODE
    if ($childExit -ne 0) { exit $childExit }
    $reportPath = Join-Path $run 'm3-ntfs-report.json'
    $verifier = Join-Path $repoRoot 'tools\Verify-M3NtfsJson.ps1'
    $sourceCommit = ((& git.exe -C $repoRoot rev-parse HEAD 2>$null) -join '').Trim()
    Require-M3Route ($sourceCommit -match '^[0-9A-Fa-f]{40}$') 'source commit is unavailable for RealFunctional evidence'
    $serverFull = [IO.Path]::GetFullPath($ServerExecutable)
    $imageFull = [IO.Path]::GetFullPath($Executable)
    $serverHash = (Get-FileHash -LiteralPath $serverFull -Algorithm SHA256).Hash
    $imageHash = (Get-FileHash -LiteralPath $imageFull -Algorithm SHA256).Hash
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verifier `
        -Path $reportPath -ExpectedSourceCommit $sourceCommit `
        -ExpectedServerSha256 $serverHash -ExpectedImageSha256 $imageHash
    $verifierExit = $LASTEXITCODE
    if ($verifierExit -ne 0) { exit $verifierExit }
    Write-Output "M3 RealFunctional PASS: $reportPath"
    exit 0
}

if ($Tier -ne 'Preflight') {
    Write-Output ([ordered]@{
        schema = 'myrd.m3.verification-route.v1'
        tier = $Tier
        status = 'BLOCKED'
        reason = "${Tier} implementation belongs to a later M3 task"
        required_skip = $true
    } | ConvertTo-Json -Depth 5 -Compress)
    exit 77
}

Require-M3Route (-not [string]::IsNullOrWhiteSpace($EvidenceBase)) 'EvidenceBase is required for Preflight'
Require-M3Route (-not [string]::IsNullOrWhiteSpace($Executable)) 'Executable is required for Preflight'
Require-M3Route (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) 'ScratchRoot is required for Preflight'
Require-M3Route (-not [string]::IsNullOrWhiteSpace($ExpectedDiskIdentityJson)) `
    'ExpectedDiskIdentityJson is required for Preflight'
$evidenceBaseFull = [IO.Path]::GetFullPath($EvidenceBase)
Require-M3Route ([IO.Directory]::Exists($evidenceBaseFull)) 'EvidenceBase must already exist'
$repoFull = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
Require-M3Route (-not $evidenceBaseFull.TrimEnd('\').StartsWith(
        $repoFull, [StringComparison]::OrdinalIgnoreCase)) `
    'EvidenceBase cannot be inside the repository'
$run = Join-Path $evidenceBaseFull ('preflight-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff') + '-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($run) | Out-Null

$scriptPath = Join-Path $repoRoot 'tests\e2e\run-m3-persistence.ps1'
$arguments = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scriptPath,
    '-Tier', 'Preflight', '-Configuration', $Configuration,
    '-EvidenceDirectory', $run, '-Executable', ([IO.Path]::GetFullPath($Executable)),
    '-ScratchRoot', ([IO.Path]::GetFullPath($ScratchRoot)),
    '-ExpectedDiskIdentityJson', ([IO.Path]::GetFullPath($ExpectedDiskIdentityJson)),
    '-ExpectedDriverService', $ExpectedDriverService
)
if ($WhatIf) { $arguments += '-WhatIf' }
& powershell.exe @arguments
$childExit = $LASTEXITCODE
if ($childExit -ne 0) { exit $childExit }
Write-Output "M3 Preflight PASS: $(Join-Path $run 'preflight.json')"
exit 0
