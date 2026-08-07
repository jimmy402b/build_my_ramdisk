[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [string]$ExpectedBinarySha256 = '',
    [string]$Executable = '',
    [string]$StdoutPath = '',
    [string]$StderrPath = '',
    [string]$RealFunctionalReportPath = '',
    [string]$CrashAggregateReportPath = '',
    [switch]$AllowAbsolutePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Require-Property($Object, [string]$Name) {
    Require ($null -ne $Object) "object missing while checking $Name"
    Require ($null -ne $Object.PSObject.Properties[$Name]) "required property missing: $Name"
}

function Require-Hash($Value, [string]$Name) {
    Require ([string]$Value -match '^[0-9A-Fa-f]{64}$') "$Name SHA-256 is invalid"
}

function Require-Boolean($Value, [string]$Name) {
    Require ($Value -is [bool]) "$Name must be boolean"
}

function Require-UnsignedInteger($Value, [string]$Name) {
    $integral = $Value -is [byte] -or $Value -is [uint16] -or
        $Value -is [uint32] -or $Value -is [uint64] -or
        $Value -is [int16] -or $Value -is [int32] -or $Value -is [int64]
    Require ($integral) "$Name must be an integer"
    Require ([decimal]$Value -ge 0) "$Name must be nonnegative"
}

function Require-NonEmptyString($Value, [string]$Name) {
    Require ($Value -is [string]) "$Name must be a string"
    Require (-not [string]::IsNullOrWhiteSpace($Value)) "$Name is empty"
}

function Read-StrictJsonFile([string]$CandidatePath, [string]$Name) {
    Require-NonEmptyString $CandidatePath "$Name path"
    $candidateFullPath = [IO.Path]::GetFullPath($CandidatePath)
    Require ([IO.File]::Exists($candidateFullPath)) "$Name does not exist"
    $attributes = [IO.File]::GetAttributes($candidateFullPath)
    Require (($attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Name cannot be a reparse point"
    $encoding = [Text.UTF8Encoding]::new($false, $true)
    $candidateRaw = [IO.File]::ReadAllText($candidateFullPath, $encoding)
    Require (-not [string]::IsNullOrWhiteSpace($candidateRaw)) "$Name is empty"
    return [pscustomobject]@{
        Path = $candidateFullPath
        Json = ($candidateRaw | ConvertFrom-Json)
        Sha256 = (Get-FileHash -LiteralPath $candidateFullPath -Algorithm SHA256).Hash
    }
}

function Require-SoakLog(
    [string]$CandidatePath,
    [string]$ExpectedHash,
    [string]$Name
) {
    Require-NonEmptyString $CandidatePath "$Name path"
    $candidateFullPath = [IO.Path]::GetFullPath($CandidatePath)
    Require ([IO.File]::Exists($candidateFullPath)) "$Name does not exist"
    $attributes = [IO.File]::GetAttributes($candidateFullPath)
    Require (($attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Name cannot be a reparse point"
    Require-Hash $ExpectedHash $Name
    $actualHash = (Get-FileHash -LiteralPath $candidateFullPath -Algorithm SHA256).Hash
    Require ($actualHash -ieq [string]$ExpectedHash) "$Name SHA-256 mismatch"
    return $actualHash
}

function Verify-M3SoakReport($SoakReport) {
    foreach ($name in @(
        'schema','status','terminal_verdict','profile','source_commit',
        'task43_complete','milestone_eligible','build','execution',
        'correctness','thresholds','dependencies','cleanup','artifacts','error')) {
        Require-Property $SoakReport $name
    }
    Require ([string]$SoakReport.schema -eq 'myrd.m3.soak-evidence.v1') `
        'soak schema is invalid'
    Require ([string]$SoakReport.status -eq 'PASS') 'soak status is not PASS'
    Require ([string]$SoakReport.terminal_verdict -eq 'PASS') `
        'soak terminal verdict is not PASS'
    Require (@('Quick','Formal') -contains [string]$SoakReport.profile) `
        'soak profile is invalid'
    $isFormal = [string]$SoakReport.profile -eq 'Formal'

    Require ([string]$SoakReport.source_commit -match '^[0-9A-Fa-f]{40}$') `
        'soak source commit is invalid'
    Require ($ExpectedSourceCommit -match '^[0-9A-Fa-f]{40}$') `
        'expected source commit is invalid'
    Require ([string]$SoakReport.source_commit -ieq $ExpectedSourceCommit) `
        'soak source commit mismatch'
    Require-Boolean $SoakReport.task43_complete 'task43_complete'
    Require-Boolean $SoakReport.milestone_eligible 'milestone_eligible'
    if ($isFormal) {
        Require ([bool]$SoakReport.task43_complete) `
            'Formal soak must complete Task43'
        Require ([bool]$SoakReport.milestone_eligible) `
            'Formal soak must be milestone eligible'
    }
    else {
        Require (-not [bool]$SoakReport.task43_complete) `
            'Quick soak cannot complete Task43'
        Require (-not [bool]$SoakReport.milestone_eligible) `
            'Quick soak cannot be milestone eligible'
        Require ($null -eq $SoakReport.dependencies) `
            'Quick soak cannot bind milestone dependencies'
    }

    foreach ($name in @(
        'build_type','soak_executable_sha256','stdout_sha256','stderr_sha256')) {
        Require-Property $SoakReport.build $name
    }
    Require-NonEmptyString $SoakReport.build.build_type 'soak build type'
    if ($isFormal) {
        Require ([string]$SoakReport.build.build_type -eq 'Release') `
            'Formal soak requires a Release build'
    }
    Require-Hash $SoakReport.build.soak_executable_sha256 'soak executable'
    Require-Hash $SoakReport.build.stdout_sha256 'soak stdout'
    Require-Hash $SoakReport.build.stderr_sha256 'soak stderr'
    if (-not [string]::IsNullOrWhiteSpace($Executable)) {
        $binaryPath = [IO.Path]::GetFullPath($Executable)
        Require ([IO.File]::Exists($binaryPath)) 'selected soak executable does not exist'
        $actualBinaryHash = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash
        Require ($actualBinaryHash -ieq [string]$SoakReport.build.soak_executable_sha256) `
            'soak executable SHA-256 mismatch'
    }
    elseif (-not [string]::IsNullOrWhiteSpace($ExpectedBinarySha256)) {
        Require-Hash $ExpectedBinarySha256 'expected soak executable'
        Require ([string]$SoakReport.build.soak_executable_sha256 -ieq $ExpectedBinarySha256) `
            'soak executable SHA-256 mismatch'
    }
    else {
        throw 'a soak executable or expected binary SHA-256 is required'
    }

    $actualStdoutHash = Require-SoakLog $StdoutPath `
        ([string]$SoakReport.build.stdout_sha256) 'soak stdout'
    $actualStderrHash = Require-SoakLog $StderrPath `
        ([string]$SoakReport.build.stderr_sha256) 'soak stderr'

    foreach ($name in @(
        'seed_count','failed_seeds','process_exit_code','operation_count',
        'stdout_sha256','stderr_sha256')) {
        Require-Property $SoakReport.execution $name
    }
    Require-UnsignedInteger $SoakReport.execution.seed_count 'soak seed_count'
    $minimumSeeds = if ($isFormal) { 100 } else { 4 }
    Require ([uint64]$SoakReport.execution.seed_count -ge $minimumSeeds) `
        "soak seed_count is below the $($SoakReport.profile) minimum"
    Require (@($SoakReport.execution.failed_seeds).Count -eq 0) `
        'soak contains failed seeds'
    Require-UnsignedInteger $SoakReport.execution.process_exit_code `
        'soak process_exit_code'
    Require ([uint64]$SoakReport.execution.process_exit_code -eq 0) `
        'soak process did not exit successfully'
    Require-UnsignedInteger $SoakReport.execution.operation_count 'soak operation_count'
    Require ([uint64]$SoakReport.execution.operation_count -gt 0) `
        'soak operation_count is zero'
    Require-Hash $SoakReport.execution.stdout_sha256 'execution stdout'
    Require-Hash $SoakReport.execution.stderr_sha256 'execution stderr'
    Require ([string]$SoakReport.execution.stdout_sha256 -ieq $actualStdoutHash) `
        'execution stdout SHA-256 mismatch'
    Require ([string]$SoakReport.execution.stderr_sha256 -ieq $actualStderrHash) `
        'execution stderr SHA-256 mismatch'

    foreach ($name in @(
        'expected_sha256','actual_sha256','hash_match',
        'complete_generations_only','mixed_generation')) {
        Require-Property $SoakReport.correctness $name
    }
    Require-Hash $SoakReport.correctness.expected_sha256 'soak expected data'
    Require-Hash $SoakReport.correctness.actual_sha256 'soak actual data'
    Require ([string]$SoakReport.correctness.expected_sha256 -ieq `
        [string]$SoakReport.correctness.actual_sha256) 'soak model hash mismatch'
    Require-Boolean $SoakReport.correctness.hash_match 'soak hash_match'
    Require ([bool]$SoakReport.correctness.hash_match) `
        'soak hash_match proof is false'
    Require-Boolean $SoakReport.correctness.complete_generations_only `
        'soak complete_generations_only'
    Require ([bool]$SoakReport.correctness.complete_generations_only) `
        'soak used an incomplete generation'
    Require-Boolean $SoakReport.correctness.mixed_generation 'soak mixed_generation'
    Require (-not [bool]$SoakReport.correctness.mixed_generation) `
        'soak accepted a mixed generation'

    $thresholdNames = @(
        'startup_read_limit_bytes','startup_read_bytes',
        'extra_committed_limit_bytes','extra_committed_bytes',
        'checkpoint_payload_bytes','journal_growth_limit_bytes',
        'journal_growth_bytes','mutation_pause_limit_ms','mutation_pause_max_ms',
        'writer_stall_limit_ms','writer_stall_max_ms')
    foreach ($name in $thresholdNames) {
        Require-Property $SoakReport.thresholds $name
        Require-UnsignedInteger $SoakReport.thresholds.$name "soak threshold $name"
    }
    Require ([uint64]$SoakReport.thresholds.startup_read_limit_bytes -eq 67108864) `
        'startup read limit is not frozen at 64 MiB'
    Require ([uint64]$SoakReport.thresholds.extra_committed_limit_bytes -eq 134217728) `
        'extra committed limit is not frozen at 128 MiB'
    if ($isFormal) {
        Require ([uint64]$SoakReport.thresholds.checkpoint_payload_bytes -eq 268435456) `
            'Formal checkpoint payload is not frozen at 256 MiB'
    }
    else {
        Require ([uint64]$SoakReport.thresholds.checkpoint_payload_bytes -gt 0) `
            'Quick checkpoint payload is zero'
    }
    Require ([uint64]$SoakReport.thresholds.journal_growth_limit_bytes -eq 322122547) `
        'journal growth limit is not frozen at 120 percent'
    Require ([uint64]$SoakReport.thresholds.mutation_pause_limit_ms -eq 250) `
        'mutation pause limit is not frozen at 250 ms'
    Require ([uint64]$SoakReport.thresholds.writer_stall_limit_ms -eq 250) `
        'writer stall limit is not frozen at 250 ms'
    Require ([uint64]$SoakReport.thresholds.startup_read_bytes -le `
        [uint64]$SoakReport.thresholds.startup_read_limit_bytes) `
        'startup read threshold exceeded'
    Require ([uint64]$SoakReport.thresholds.extra_committed_bytes -le `
        [uint64]$SoakReport.thresholds.extra_committed_limit_bytes) `
        'extra committed threshold exceeded'
    Require ([uint64]$SoakReport.thresholds.journal_growth_bytes -le `
        [uint64]$SoakReport.thresholds.journal_growth_limit_bytes) `
        'journal growth threshold exceeded'
    Require ([uint64]$SoakReport.thresholds.mutation_pause_max_ms -le `
        [uint64]$SoakReport.thresholds.mutation_pause_limit_ms) `
        'mutation pause threshold exceeded'
    Require ([uint64]$SoakReport.thresholds.writer_stall_max_ms -le `
        [uint64]$SoakReport.thresholds.writer_stall_limit_ms) `
        'writer stall threshold exceeded'

    if ($isFormal) {
        Require ($null -ne $SoakReport.dependencies) `
            'Formal soak requires milestone dependencies'
        foreach ($name in @('real_functional','crash_full')) {
            Require-Property $SoakReport.dependencies $name
        }
        foreach ($name in @('status','source_commit','sha256')) {
            Require-Property $SoakReport.dependencies.real_functional $name
        }
        foreach ($name in @('status','task42_complete','source_commit','sha256')) {
            Require-Property $SoakReport.dependencies.crash_full $name
        }
        Require ([string]$SoakReport.dependencies.real_functional.status -eq 'PASS') `
            'Task41 dependency status is not PASS'
        Require ([string]$SoakReport.dependencies.crash_full.status -eq 'PASS') `
            'Task42 dependency status is not PASS'
        Require-Boolean $SoakReport.dependencies.crash_full.task42_complete `
            'Task42 dependency completion'
        Require ([bool]$SoakReport.dependencies.crash_full.task42_complete) `
            'Task42 dependency is incomplete'
        foreach ($dependency in @(
            $SoakReport.dependencies.real_functional,
            $SoakReport.dependencies.crash_full)) {
            Require ([string]$dependency.source_commit -ieq $ExpectedSourceCommit) `
                'soak dependency source commit mismatch'
            Require-Hash $dependency.sha256 'soak dependency'
        }

        $realFunctional = Read-StrictJsonFile $RealFunctionalReportPath `
            'Task41 RealFunctional report'
        Require ([string]$SoakReport.dependencies.real_functional.sha256 -ieq `
            $realFunctional.Sha256) 'Task41 dependency SHA-256 mismatch'
        foreach ($name in @('schema','status','terminal_verdict','source_commit')) {
            Require-Property $realFunctional.Json $name
        }
        Require ([string]$realFunctional.Json.schema -eq 'myrd.m3.ntfs-workload.v1') `
            'Task41 dependency schema is invalid'
        Require ([string]$realFunctional.Json.status -eq 'PASS') `
            'Task41 report status is not PASS'
        Require ([string]$realFunctional.Json.terminal_verdict -eq 'PASS') `
            'Task41 report terminal verdict is not PASS'
        Require ([string]$realFunctional.Json.source_commit -ieq $ExpectedSourceCommit) `
            'Task41 report source commit mismatch'
        foreach ($name in @('executable_sha256','cleanup','workload','phase',
            'what_if','server_started','destructive_operations_emitted')) {
            Require-Property $realFunctional.Json $name
        }
        foreach ($name in @('server','image')) {
            Require-Property $realFunctional.Json.executable_sha256 $name
            Require-Hash $realFunctional.Json.executable_sha256.$name `
                "Task41 $name executable"
        }
        $ntfsVerifier = Join-Path $PSScriptRoot 'Verify-M3NtfsJson.ps1'
        Require ([IO.File]::Exists($ntfsVerifier)) `
            'Task41 independent verifier is missing'
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $null = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
                -File $ntfsVerifier -Path $realFunctional.Path `
                -ExpectedSourceCommit $ExpectedSourceCommit `
                -ExpectedServerSha256 `
                    ([string]$realFunctional.Json.executable_sha256.server) `
                -ExpectedImageSha256 `
                    ([string]$realFunctional.Json.executable_sha256.image) 2>&1
            $ntfsVerifierExit = $LASTEXITCODE
        }
        finally { $ErrorActionPreference = $oldPreference }
        Require ($ntfsVerifierExit -eq 0) `
            'Task41 report failed independent NTFS verification'

        $crashFull = Read-StrictJsonFile $CrashAggregateReportPath `
            'Task42 Crash Full report'
        Require ([string]$SoakReport.dependencies.crash_full.sha256 -ieq `
            $crashFull.Sha256) 'Task42 dependency SHA-256 mismatch'
        foreach ($name in @(
            'schema','status','terminal_verdict','task42_complete',
            'milestone_eligible','source_commit','guarantees')) {
            Require-Property $crashFull.Json $name
        }
        Require ([string]$crashFull.Json.schema -eq `
            'myrd.m3.real-crash-aggregate.v1') 'Task42 dependency schema is invalid'
        Require ([string]$crashFull.Json.status -eq 'PASS') `
            'Task42 report status is not PASS'
        Require ([string]$crashFull.Json.terminal_verdict -eq 'PASS') `
            'Task42 report terminal verdict is not PASS'
        Require-Boolean $crashFull.Json.task42_complete 'Task42 report completion'
        Require ([bool]$crashFull.Json.task42_complete) `
            'Task42 report is incomplete'
        Require-Boolean $crashFull.Json.milestone_eligible `
            'Task42 milestone eligibility'
        Require ([bool]$crashFull.Json.milestone_eligible) `
            'Task42 report is not milestone eligible'
        Require ([string]$crashFull.Json.source_commit -ieq $ExpectedSourceCommit) `
            'Task42 report source commit mismatch'
        foreach ($name in @(
            'server_sha256','image_sha256','matrix_sha256',
            'simulated_evidence_sha256','simulated_schedule_count',
            'real_scenario_count','process_kill_count',
            'controlled_power_cut_count','scenarios')) {
            Require-Property $crashFull.Json $name
        }
        foreach ($name in @(
            'server_sha256','image_sha256','matrix_sha256',
            'simulated_evidence_sha256')) {
            Require-Hash $crashFull.Json.$name "Task42 $name"
        }
        Require ([string]$crashFull.Json.server_sha256 -ieq
            [string]$realFunctional.Json.executable_sha256.server) `
            'Task41/Task42 server executable hashes differ'
        Require ([string]$crashFull.Json.image_sha256 -ieq
            [string]$realFunctional.Json.executable_sha256.image) `
            'Task41/Task42 image executable hashes differ'
        foreach ($name in @(
            'simulated_schedule_count','real_scenario_count',
            'process_kill_count','controlled_power_cut_count')) {
            Require-UnsignedInteger $crashFull.Json.$name "Task42 $name"
        }
        Require ([uint64]$crashFull.Json.simulated_schedule_count -ge 10000) `
            'Task42 simulated schedule count is below 10000'
        Require ([uint64]$crashFull.Json.real_scenario_count -eq 10) `
            'Task42 real scenario count is not 10'
        Require ([uint64]$crashFull.Json.process_kill_count -eq 8) `
            'Task42 process-kill count is not 8'
        Require ([uint64]$crashFull.Json.controlled_power_cut_count -eq 2) `
            'Task42 controlled-power-cut count is not 2'
        $crashScenarios = @($crashFull.Json.scenarios)
        Require ($crashScenarios.Count -eq 10) `
            'Task42 scenario array does not contain 10 entries'
        $scenarioIds = @{}
        $processKillScenarios = 0
        $powerCutScenarios = 0
        foreach ($scenario in $crashScenarios) {
            foreach ($name in @(
                'scenario_id','phase','interruption','allowed_recovery',
                'recovered_classification','selected_generation',
                'selected_slot','fallback','slot_diagnostics','report_sha256')) {
                Require-Property $scenario $name
            }
            Require-NonEmptyString $scenario.scenario_id `
                'Task42 scenario_id'
            Require (-not $scenarioIds.ContainsKey([string]$scenario.scenario_id)) `
                'Task42 scenario_id is duplicated'
            $scenarioIds[[string]$scenario.scenario_id] = $true
            Require-NonEmptyString $scenario.phase 'Task42 phase'
            Require (@('process-kill','controlled-power-cut') -contains
                [string]$scenario.interruption) `
                'Task42 interruption is invalid'
            if ([string]$scenario.interruption -eq 'process-kill') {
                ++$processKillScenarios
            }
            else { ++$powerCutScenarios }
            Require (@('old','new','old_or_new') -contains
                [string]$scenario.allowed_recovery) `
                'Task42 allowed recovery is invalid'
            Require (@('old','new') -contains
                [string]$scenario.recovered_classification) `
                'Task42 recovered classification is invalid'
            if ([string]$scenario.allowed_recovery -ne 'old_or_new') {
                Require ([string]$scenario.allowed_recovery -eq
                    [string]$scenario.recovered_classification) `
                    'Task42 recovered a forbidden generation class'
            }
            Require-UnsignedInteger $scenario.selected_generation `
                'Task42 selected generation'
            Require-UnsignedInteger $scenario.selected_slot `
                'Task42 selected slot'
            Require ([uint64]$scenario.selected_slot -in 0,1) `
                'Task42 selected slot is invalid'
            Require-Boolean $scenario.fallback 'Task42 fallback'
            Require (-not [bool]$scenario.fallback) `
                'Task42 final evidence used fallback recovery'
            Require-Hash $scenario.report_sha256 'Task42 scenario report'
            $slotDiagnostics = @($scenario.slot_diagnostics)
            Require ($slotDiagnostics.Count -eq 2) `
                'Task42 scenario requires two slot diagnostics'
            Require (@($slotDiagnostics | Where-Object {
                [bool]$_.selected }).Count -eq 1) `
                'Task42 scenario must select exactly one slot'
            for ($slotIndex = 0; $slotIndex -lt 2; ++$slotIndex) {
                $diagnostic = $slotDiagnostics[$slotIndex]
                foreach ($name in @(
                    'slot_index','status','generation','selected')) {
                    Require-Property $diagnostic $name
                }
                Require ([uint64]$diagnostic.slot_index -eq $slotIndex) `
                    'Task42 slot diagnostics are out of order'
                Require (@('passed','rejected') -contains
                    [string]$diagnostic.status) `
                    'Task42 slot diagnostic status is invalid'
                Require-Boolean $diagnostic.selected `
                    'Task42 slot diagnostic selection'
            }
        }
        Require ($processKillScenarios -eq 8 -and $powerCutScenarios -eq 2) `
            'Task42 scenario interruption counts are inconsistent'
        foreach ($name in @(
            'latest_complete_confirmed_checkpoint','mixed_generation_rejected',
            'manifest_slot_diagnostics_verified',
            'post_checkpoint_uncommitted_changes_may_be_lost',
            'read_only_recovery_first','ntfs_reformat_forbidden',
            'per_scenario_guest_reboot_isolation')) {
            Require-Property $crashFull.Json.guarantees $name
            Require-Boolean $crashFull.Json.guarantees.$name "Task42 guarantee $name"
            Require ([bool]$crashFull.Json.guarantees.$name) `
                "Task42 guarantee is false: $name"
        }
    }

    foreach ($name in @('succeeded','residuals')) {
        Require-Property $SoakReport.cleanup $name
    }
    Require-Boolean $SoakReport.cleanup.succeeded 'soak cleanup succeeded'
    Require ([bool]$SoakReport.cleanup.succeeded) 'soak cleanup failed'
    Require (@($SoakReport.cleanup.residuals).Count -eq 0) `
        'soak cleanup left residual resources'

    $soakArtifacts = @($SoakReport.artifacts)
    Require ($soakArtifacts.Count -gt 0) 'soak artifact hashes are required'
    $soakRoles = @{}
    foreach ($artifact in $soakArtifacts) {
        foreach ($name in @('role','sha256')) { Require-Property $artifact $name }
        Require-NonEmptyString $artifact.role 'soak artifact role'
        Require (-not $soakRoles.ContainsKey([string]$artifact.role)) `
            'soak artifact role is duplicated'
        $soakRoles[[string]$artifact.role] = [string]$artifact.sha256
        Require-Hash $artifact.sha256 'soak artifact'
    }
    Require ($soakRoles.ContainsKey('soak.stdout')) 'soak stdout artifact is missing'
    Require ($soakRoles.ContainsKey('soak.stderr')) 'soak stderr artifact is missing'
    Require ([string]$soakRoles['soak.stdout'] -ieq $actualStdoutHash) `
        'soak stdout artifact SHA-256 mismatch'
    Require ([string]$soakRoles['soak.stderr'] -ieq $actualStderrHash) `
        'soak stderr artifact SHA-256 mismatch'
    Require ($null -eq $SoakReport.error) 'passing soak report contains an error'
}

$fullPath = [IO.Path]::GetFullPath($Path)
Require ([IO.File]::Exists($fullPath)) 'M3 report does not exist'
Require (-not $fullPath.StartsWith('\.\', [StringComparison]::OrdinalIgnoreCase)) 'M3 report path cannot be a device path'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$raw = [IO.File]::ReadAllText($fullPath, $strictUtf8)
Require (-not [string]::IsNullOrWhiteSpace($raw)) 'M3 report is empty'
$report = $raw | ConvertFrom-Json

if ($null -ne $report.PSObject.Properties['schema'] -and
    [string]$report.schema -eq 'myrd.m3.soak-evidence.v1') {
    Verify-M3SoakReport $report
    Write-Output "M3 soak JSON validation PASS: $fullPath"
    exit 0
}

foreach ($name in @(
    'schema_version','operation','status','operation_id','started_utc',
    'finished_utc','build','bundle_path_hash','bundle_path','image','slots',
    'selected','verification','cleanup_candidates','artifacts','error')) {
    Require-Property $report $name
}
Require-UnsignedInteger $report.schema_version 'schema_version'
Require ([int]$report.schema_version -eq 1) 'schema_version must be 1'
Require (@('inspect','verify_structural','verify_full') -contains [string]$report.operation) 'operation is unknown'
Require (@('passed','failed','blocked') -contains [string]$report.status) 'status is unknown'
Require ([string]$report.status -eq 'passed') 'required M3 evidence is not passed'
Require (-not [string]::IsNullOrWhiteSpace([string]$report.operation_id)) 'operation id is empty'
Require ([string]$report.started_utc -match '^\d{4}-\d{2}-\d{2}T') 'started timestamp is invalid'
Require ([string]$report.finished_utc -match '^\d{4}-\d{2}-\d{2}T') 'finished timestamp is invalid'

foreach ($name in @('source_commit','build_type','binary_sha256')) {
    Require-Property $report.build $name
}
Require ([string]$report.build.source_commit -match '^[0-9A-Fa-f]{40}$') 'source commit is invalid'
Require ($ExpectedSourceCommit -match '^[0-9A-Fa-f]{40}$') 'expected source commit is invalid'
Require ([string]$report.build.source_commit -ieq $ExpectedSourceCommit) 'source commit mismatch'
Require (-not [string]::IsNullOrWhiteSpace([string]$report.build.build_type)) 'build type is empty'
Require-Hash $report.build.binary_sha256 'binary'
if (-not [string]::IsNullOrWhiteSpace($Executable)) {
    $binaryPath = [IO.Path]::GetFullPath($Executable)
    Require ([IO.File]::Exists($binaryPath)) 'selected M3 executable does not exist'
    $actual = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash
    Require ($actual -ieq [string]$report.build.binary_sha256) 'binary SHA-256 mismatch'
}
elseif (-not [string]::IsNullOrWhiteSpace($ExpectedBinarySha256)) {
    Require-Hash $ExpectedBinarySha256 'expected binary'
    Require ([string]$report.build.binary_sha256 -ieq $ExpectedBinarySha256) 'binary SHA-256 mismatch'
}
else {
    throw 'an executable or expected binary SHA-256 is required'
}
Require-Hash $report.bundle_path_hash 'bundle path'
if ($null -ne $report.bundle_path) {
    Require ($AllowAbsolutePath.IsPresent) 'absolute path evidence requires explicit opt-in'
    Require ([IO.Path]::IsPathRooted([string]$report.bundle_path)) 'opted-in bundle path is not absolute'
}
Require ($null -ne $report.image) 'passing report has no image identity'
foreach ($name in @('image_uuid','virtual_bytes','logical_sector_size','physical_sector_size')) {
    Require-Property $report.image $name
}
Require ([string]$report.image.image_uuid -match '^[0-9A-Fa-f]{32}$') 'image UUID is invalid'
Require-UnsignedInteger $report.image.virtual_bytes 'image virtual_bytes'
Require-UnsignedInteger $report.image.logical_sector_size 'image logical_sector_size'
Require-UnsignedInteger $report.image.physical_sector_size 'image physical_sector_size'
Require ([uint64]$report.image.virtual_bytes -gt 0) 'image virtual size is zero'
Require (@(512,4096) -contains [int]$report.image.logical_sector_size) 'logical sector size is invalid'
Require ([int]$report.image.physical_sector_size -eq 4096) 'physical sector size is invalid'

$slots = @($report.slots)
Require ($slots.Count -eq 2) 'exactly two slot diagnostics are required'
$slotStatuses = @('empty','selected','valid_not_selected','fallback_rejected','rejected')
$checkStatuses = @('not_run','passed','missing','corrupt','unsupported','access_denied','io_error','cancelled','deadline_exceeded','conflict')
$checks = @('slot_decode','slot_support','slot_semantics','base_path','journal_path','base_presence','base_open','base_identity','base_expected_length','base_profile','base_layout','journal_presence','journal_open','journal_identity','journal_header','journal_committed_length','journal_commit','journal_root','journal_digest')
for ($index = 0; $index -lt 2; ++$index) {
    $slot = $slots[$index]
    foreach ($name in @('slot','status','generation','first_failed_check','check_status','selected')) {
        Require-Property $slot $name
    }
    $expectedSlot = if ($index -eq 0) { 'A' } else { 'B' }
    Require ([string]$slot.slot -eq $expectedSlot) 'slot order/name is invalid'
    Require ($slotStatuses -contains [string]$slot.status) 'slot status is unknown'
    Require ($checkStatuses -contains [string]$slot.check_status) 'slot check status is unknown'
    Require-Boolean $slot.selected "slot selected"
    if ($null -ne $slot.generation) { Require-UnsignedInteger $slot.generation 'slot generation' }
    if ($null -ne $slot.first_failed_check) {
        Require ($checks -contains [string]$slot.first_failed_check) 'slot failed check is unknown'
    }
}
Require (@($slots | Where-Object { [bool]$_.selected }).Count -eq 1) 'passing report must select exactly one slot'

Require ($null -ne $report.selected) 'passing report has no selected generation'
foreach ($name in @('slot_index','fallback','checkpoint_generation','base_generation','journal_epoch','base_windows_identity','base_format_identity','journal_windows_identity','journal_format_identity','journal_committed_length','root','commit')) {
    Require-Property $report.selected $name
}
Require-UnsignedInteger $report.selected.slot_index 'selected slot index'
Require ([int]$report.selected.slot_index -in 0,1) 'selected slot index is invalid'
Require-Boolean $report.selected.fallback 'selected fallback'
foreach ($name in @('checkpoint_generation','base_generation','journal_epoch','journal_committed_length')) {
    Require-UnsignedInteger $report.selected.$name "selected $name"
}
foreach ($name in @('base_windows_identity','base_format_identity','journal_windows_identity','journal_format_identity')) {
    Require-NonEmptyString $report.selected.$name "selected $name"
}
Require ($null -ne $report.selected.root) 'selected root is null'
Require ($null -ne $report.selected.commit) 'selected commit is null'
foreach ($name in @('offset','length','crc32c')) {
    Require-Property $report.selected.root $name
    Require-UnsignedInteger $report.selected.root.$name "root $name"
    Require-Property $report.selected.commit $name
    Require-UnsignedInteger $report.selected.commit.$name "commit $name"
}
Require-Property $report.selected.commit 'chain_sha256'
Require-Hash $report.selected.commit.chain_sha256 'commit chain'

if ([string]$report.operation -eq 'inspect') {
    Require ($null -eq $report.verification) 'inspect report cannot claim verification'
}
else {
    Require ($null -ne $report.verification) 'verify report has no verification section'
    foreach ($name in @('depth','verified','candidate_blocks','data_blocks','zero_blocks','data_bytes_hashed','logical_sha256')) {
        Require-Property $report.verification $name
    }
    Require (@('structural','full') -contains [string]$report.verification.depth) 'verification depth is unknown'
    $expectedDepth = if ([string]$report.operation -eq 'verify_full') { 'full' } else { 'structural' }
    Require ([string]$report.verification.depth -eq $expectedDepth) 'operation and verification depth differ'
    Require-Boolean $report.verification.verified 'verification verified'
    Require ([bool]$report.verification.verified) 'required verification did not pass'
    foreach ($name in @('candidate_blocks','data_blocks','zero_blocks','data_bytes_hashed')) {
        Require-UnsignedInteger $report.verification.$name "verification $name"
    }
    Require ([uint64]$report.verification.candidate_blocks -eq
        ([uint64]$report.verification.data_blocks + [uint64]$report.verification.zero_blocks)) 'verification block counts are inconsistent'
    if ($expectedDepth -eq 'full') {
        Require (-not [bool]$report.selected.fallback) 'full verification cannot use fallback generation'
        Require-Hash $report.verification.logical_sha256 'logical'
    }
}

$cleanupDecisions = @('Eligible','InvalidName','ReparsePoint','HardLinked','UnknownOwnership','ManifestReferenced','LiveViewReferenced','InFlightReferenced')
foreach ($candidate in @($report.cleanup_candidates)) {
    foreach ($name in @('kind','relative_name','decision','action')) { Require-Property $candidate $name }
    Require (@('base','journal') -contains [string]$candidate.kind) 'cleanup kind is unknown'
    Require ($cleanupDecisions -contains [string]$candidate.decision) 'cleanup decision is unknown'
    Require (@('Kept','Deleted') -contains [string]$candidate.action) 'cleanup action is unknown'
    $relativeName = [string]$candidate.relative_name
    Require (-not [IO.Path]::IsPathRooted($relativeName)) 'cleanup candidate leaked an absolute path'
    Require ([IO.Path]::GetFileName($relativeName) -eq $relativeName) 'cleanup candidate is not a single component'
}

$artifacts = @($report.artifacts)
Require ($artifacts.Count -gt 0) 'artifact hashes are required'
$roles = @{}
foreach ($artifact in $artifacts) {
    foreach ($name in @('role','sha256')) { Require-Property $artifact $name }
    Require (-not [string]::IsNullOrWhiteSpace([string]$artifact.role)) 'artifact role is empty'
    Require (-not $roles.ContainsKey([string]$artifact.role)) 'artifact role is duplicated'
    $roles[[string]$artifact.role] = $true
    Require-Hash $artifact.sha256 'artifact'
}
Require ($null -eq $report.error) 'passing report contains a primary error'

Write-Output "M3 JSON validation PASS: $fullPath"
exit 0
