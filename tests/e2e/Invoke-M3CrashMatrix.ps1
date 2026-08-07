[CmdletBinding()]
param(
    [ValidateSet('Validate', 'Run')][string]$Phase = 'Run',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [Parameter(Mandatory = $true)][string]$CrashModelExecutable,
    [Parameter(Mandatory = $true)][string]$MatrixPath,
    [Parameter(Mandatory = $true)][string]$RegistryPath,
    [uint64]$ScheduleCount = 10000,
    [ValidateRange(1, 1800)][int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'M3Harness.Common.ps1')

function Assert-M3Crash {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Read-M3CrashJsonFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-M3FullPath $Path
    Assert-M3Crash ([IO.File]::Exists($full)) "JSON fixture is missing: $full"
    Assert-M3NoReparsePath $full
    try {
        $raw = [IO.File]::ReadAllText(
            $full, [Text.UTF8Encoding]::new($false, $true))
        return ($raw | ConvertFrom-Json)
    }
    catch { throw "JSON fixture is invalid: $full" }
}

function Assert-M3CrashProperty {
    param($Object, [Parameter(Mandatory = $true)][string]$Name)
    Assert-M3Crash ($null -ne $Object -and
        $null -ne $Object.PSObject.Properties[$Name]) `
        "required fixture property is missing: $Name"
    return $Object.PSObject.Properties[$Name].Value
}

function Assert-M3CrashBooleanProperty {
    param($Object, [Parameter(Mandatory = $true)][string]$Name)
    $value = Assert-M3CrashProperty $Object $Name
    Assert-M3Crash ($value -is [bool]) "$Name is not a JSON boolean"
    return [bool]$value
}

function Get-M3CrashFixtureSummary {
    param(
        [Parameter(Mandatory = $true)][string]$Matrix,
        [Parameter(Mandatory = $true)][string]$Registry
    )
    $matrixObject = Read-M3CrashJsonFile $Matrix
    $registryObject = Read-M3CrashJsonFile $Registry
    $matrixSchema = Assert-M3CrashProperty $matrixObject 'schema_version'
    Assert-M3Crash ([int]$matrixSchema -eq 1) 'crash matrix schema_version is not 1'
    $registrySchema = Assert-M3CrashProperty $registryObject 'schema'
    Assert-M3Crash ([string]$registrySchema -eq 'm3-fault-phase-registry-v1') `
        'fault phase registry schema is invalid'
    $scenarios = @(Assert-M3CrashProperty $matrixObject 'scenarios')
    $phases = @(Assert-M3CrashProperty $registryObject 'phases')
    Assert-M3Crash ($scenarios.Count -eq 32) 'crash matrix must contain exactly 32 scenarios'
    Assert-M3Crash ($phases.Count -eq 59) 'fault registry must contain exactly 59 phases'

    $phaseIds = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    [int]$killCount = 0
    [int]$shortCount = 0
    [int]$tornCount = 0
    foreach ($item in $phases) {
        foreach ($name in @(
                'id', 'boundary', 'short', 'torn', 'kill', 'gate',
                'state_disposition', 'error_disposition', 'kill_recovery')) {
            $null = Assert-M3CrashProperty $item $name
        }
        $id = [string]$item.id
        Assert-M3Crash (-not [string]::IsNullOrWhiteSpace($id)) `
            'fault phase ID is empty'
        Assert-M3Crash ($phaseIds.Add($id)) "duplicate fault phase ID: $id"
        if (Assert-M3CrashBooleanProperty $item 'kill') { ++$killCount }
        if (Assert-M3CrashBooleanProperty $item 'short') { ++$shortCount }
        if (Assert-M3CrashBooleanProperty $item 'torn') { ++$tornCount }
    }
    Assert-M3Crash ($killCount -eq 54) 'fault registry kill count is not 54'
    Assert-M3Crash ($shortCount -eq 10) 'fault registry short-write count is not 10'
    Assert-M3Crash ($tornCount -eq 1) 'fault registry torn-write count is not 1'

    $scenarioIds = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    [int]$notApplicable = 0
    foreach ($item in $scenarios) {
        foreach ($name in @(
                'id', 'event', 'allowed_recovery', 'survivor_disposition',
                'survivor_phase', 'tail_garbage_allowed')) {
            $null = Assert-M3CrashProperty $item $name
        }
        $id = [string]$item.id
        Assert-M3Crash (-not [string]::IsNullOrWhiteSpace($id)) `
            'crash scenario ID is empty'
        Assert-M3Crash ($scenarioIds.Add($id)) "duplicate crash scenario ID: $id"
        Assert-M3Crash ($phaseIds.Contains($id)) `
            "crash scenario is absent from the frozen registry: $id"
        Assert-M3CrashBooleanProperty $item 'tail_garbage_allowed' | Out-Null
        $allowed = [string]$item.allowed_recovery
        Assert-M3Crash ($allowed -in @('old', 'new', 'old_or_new', 'not_applicable')) `
            "crash scenario has an invalid allowed_recovery: $id"
        if ($allowed -eq 'not_applicable') { ++$notApplicable }
        Assert-M3Crash ([string]$item.survivor_disposition -in @(
                'not_committed', 'commit_indeterminate',
                'committed_needs_reconciliation', 'committed')) `
            "crash scenario has an invalid survivor_disposition: $id"
        Assert-M3Crash ([string]$item.survivor_phase -in @(
                'ready', 'resolving_commit', 'reconciling')) `
            "crash scenario has an invalid survivor_phase: $id"
    }
    Assert-M3Crash ($notApplicable -eq 5) `
        'crash matrix not-applicable count is not 5'

    return [pscustomobject]@{
        matrix_sha256 = (Get-FileHash -LiteralPath $Matrix -Algorithm SHA256).Hash
        registry_sha256 = (Get-FileHash -LiteralPath $Registry -Algorithm SHA256).Hash
        matrix_scenarios = $scenarios.Count
        registry_phases = $phases.Count
        kill_phases = $killCount
        short_write_phases = $shortCount
        torn_write_phases = $tornCount
        not_applicable_scenarios = $notApplicable
    }
}

function Write-M3CrashTextAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Text
    )
    $full = Get-M3FullPath $Path
    Assert-M3Crash (-not [IO.File]::Exists($full) -and
        -not [IO.Directory]::Exists($full)) `
        "evidence output already exists: $full"
    $directory = [IO.Path]::GetDirectoryName($full)
    $temporary = Join-Path $directory `
        ('.' + [IO.Path]::GetFileName($full) + '.tmp-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllText($temporary, $Text, [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $full)
    }
    finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Write-M3CrashJsonAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )
    $json = $Value | ConvertTo-Json -Depth 20
    $null = $json | ConvertFrom-Json
    Write-M3CrashTextAtomic -Path $Path -Text $json
}

function Get-M3CrashSourceCommit {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    Assert-M3Crash ($null -ne $git) 'git.exe is required for crash evidence provenance'
    $commit = ((& git.exe -C $RepoRoot rev-parse HEAD 2>$null) -join '').Trim()
    Assert-M3Crash ($commit -match '^[0-9A-Fa-f]{40}$') `
        'source commit is unavailable for crash evidence'
    return $commit
}

try {
    Assert-M3Crash ($ScheduleCount -ge 10000 -and $ScheduleCount -le 1000000) `
        'ScheduleCount must be between 10000 and 1000000'
    $repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
    $evidenceFull = Get-M3FullPath $EvidenceDirectory
    Assert-M3Crash ([IO.Directory]::Exists($evidenceFull)) `
        'EvidenceDirectory must already exist'
    Assert-M3NoReparsePath $evidenceFull
    Assert-M3Crash (-not (Test-M3PathUnderRoot $evidenceFull $repoRoot -AllowRoot)) `
        'EvidenceDirectory cannot be inside the repository'
    Assert-M3Crash ([IO.Directory]::GetFileSystemEntries($evidenceFull).Count -eq 0) `
        'EvidenceDirectory must be empty'

    $executableFull = Get-M3FullPath $CrashModelExecutable
    $matrixFull = Get-M3FullPath $MatrixPath
    $registryFull = Get-M3FullPath $RegistryPath
    Assert-M3Crash ([IO.File]::Exists($executableFull)) `
        'crash-model executable is missing'
    Assert-M3NoReparsePath $executableFull
    Assert-M3Crash ([string]::Equals(
        [IO.Path]::GetExtension($executableFull), '.exe',
        [StringComparison]::OrdinalIgnoreCase)) `
        'crash-model executable must be an .exe file'
    $fixtures = Get-M3CrashFixtureSummary $matrixFull $registryFull
    $sourceCommit = Get-M3CrashSourceCommit $repoRoot
    $executableHash = (Get-FileHash -LiteralPath $executableFull -Algorithm SHA256).Hash
    $startedUtc = [DateTime]::UtcNow.ToString('o')

    if ($Phase -eq 'Validate') {
        $validation = [ordered]@{
            schema = 'myrd.m3.crash-validation.v1'
            status = 'PASS'
            terminal_verdict = 'PASS'
            configuration = $Configuration
            process_started = $false
            source_commit = $sourceCommit
            executable_sha256 = $executableHash
            fixtures = $fixtures
            started_utc = $startedUtc
            finished_utc = [DateTime]::UtcNow.ToString('o')
        }
        Write-M3CrashJsonAtomic `
            (Join-Path $evidenceFull 'm3-crash-validation.json') $validation
        Write-Output ($validation | ConvertTo-Json -Depth 12 -Compress)
        exit 0
    }

    $process = [Diagnostics.Process]::new()
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $executableFull
    $startInfo.WorkingDirectory = [IO.Path]::GetDirectoryName($executableFull)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables['MYRD_M3_CRASH_MODEL_SCHEDULES'] = `
        [string]$ScheduleCount
    $process.StartInfo = $startInfo
    $processStarted = $false
    $processExited = $false
    $timedOut = $false
    $processId = $null
    $exitCode = $null
    $stdout = ''
    $stderr = ''
    $cleanupResiduals = [Collections.ArrayList]::new()
    try {
        Assert-M3Crash $process.Start() 'failed to start crash-model child process'
        $processStarted = $true
        $processId = $process.Id
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            try { $process.Kill() }
            catch { [void]$cleanupResiduals.Add('exact crash-model child could not be terminated') }
        }
        if (-not $process.HasExited) {
            if (-not $process.WaitForExit(30000)) {
                [void]$cleanupResiduals.Add('exact crash-model child remained after cleanup timeout')
            }
        }
        if ($process.HasExited) {
            $process.WaitForExit()
            $processExited = $true
            $exitCode = $process.ExitCode
        }
        if ($stdoutTask.IsCompleted) { $stdout = [string]$stdoutTask.Result }
        if ($stderrTask.IsCompleted) { $stderr = [string]$stderrTask.Result }
    }
    catch {
        $stderr = $stderr + [Environment]::NewLine + [string]$_.Exception.Message
        if ($processStarted -and -not $process.HasExited) {
            try { $process.Kill(); $process.WaitForExit(30000) | Out-Null }
            catch { [void]$cleanupResiduals.Add('exact crash-model child cleanup failed after exception') }
        }
        if ($processStarted -and $process.HasExited) {
            $processExited = $true
            $exitCode = $process.ExitCode
        }
    }
    finally { $process.Dispose() }

    $stdoutPath = Join-Path $evidenceFull 'm3-crash-model.stdout.txt'
    $stderrPath = Join-Path $evidenceFull 'm3-crash-model.stderr.txt'
    Write-M3CrashTextAtomic $stdoutPath $stdout
    Write-M3CrashTextAtomic $stderrPath $stderr
    $summaryMatches = [regex]::Matches($stdout, '(?m)^(\d+)/(\d+) passed\r?$')
    $summary = if ($summaryMatches.Count -eq 1) {
        $summaryMatches[0].Value.Trim()
    } else { '' }
    $runPassed = $processStarted -and $processExited -and -not $timedOut -and
        $null -ne $exitCode -and [int]$exitCode -eq 0 -and
        $summary -eq '6/6 passed' -and $cleanupResiduals.Count -eq 0
    $status = if ($runPassed) { 'PASS' } else { 'FAIL' }
    $report = [ordered]@{
        schema = 'myrd.m3.crash-evidence.v1'
        status = $status
        terminal_verdict = $status
        scope = 'simulated'
        milestone_eligible = $false
        task42_complete = $false
        source_commit = $sourceCommit
        configuration = $Configuration
        executable = [ordered]@{
            file_name = [IO.Path]::GetFileName($executableFull)
            sha256 = $executableHash
        }
        fixtures = $fixtures
        execution = [ordered]@{
            schedule_count = [uint64]$ScheduleCount
            process_started = [bool]$processStarted
            process_id = $processId
            process_exit_code = $exitCode
            timed_out = [bool]$timedOut
            summary = $summary
            stdout_sha256 = (Get-FileHash -LiteralPath $stdoutPath -Algorithm SHA256).Hash
            stderr_sha256 = (Get-FileHash -LiteralPath $stderrPath -Algorithm SHA256).Hash
        }
        coverage = [ordered]@{
            simulated_file_clone = [bool]$runPassed
            fresh_process = [bool]$processStarted
            real_process_kill = $false
            real_aim_ntfs = $false
            controlled_power_cut = $false
        }
        cleanup = [ordered]@{
            process_exited = [bool]$processExited
            succeeded = ($cleanupResiduals.Count -eq 0 -and $processExited)
            residuals = @($cleanupResiduals)
        }
        started_utc = $startedUtc
        finished_utc = [DateTime]::UtcNow.ToString('o')
    }
    Write-M3CrashJsonAtomic (Join-Path $evidenceFull 'm3-crash-report.json') $report
    Write-Output ($report | ConvertTo-Json -Depth 20 -Compress)
    if ($runPassed) { exit 0 }
    [Console]::Error.WriteLine('M3 simulated crash model FAIL; inspect retained evidence')
    exit 1
}
catch {
    [Console]::Error.WriteLine(
        "M3 simulated crash $Phase FAIL: $([string]$_.Exception.Message)")
    exit 1
}
