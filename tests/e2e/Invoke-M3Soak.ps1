[CmdletBinding()]
param(
    [ValidateSet('Validate','Run')][string]$Phase = 'Run',
    [ValidateSet('Quick','Formal')][string]$Profile = 'Quick',
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [Parameter(Mandatory = $true)][string]$SoakExecutable,
    [string]$RealFunctionalReport = '',
    [string]$CrashAggregateReport = '',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateRange(30,86400)][int]$TimeoutSeconds = 3600
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-M3Soak([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-M3SoakFullPath([string]$Path) {
    Assert-M3Soak (-not [string]::IsNullOrWhiteSpace($Path)) 'path is empty'
    return [IO.Path]::GetFullPath($Path)
}

function Test-M3SoakPathUnderRoot([string]$Path, [string]$Root) {
    $candidate = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    return $candidate.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase) -or
        $candidate.StartsWith($rootFull + '\',
            [StringComparison]::OrdinalIgnoreCase)
}

function Assert-M3SoakNoReparse([string]$Path) {
    $current = Get-Item -LiteralPath $Path -Force
    while ($null -ne $current) {
        Assert-M3Soak (($current.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "reparse path is forbidden: $($current.FullName)"
        if ($current -is [IO.FileInfo]) {
            $current = $current.Directory
        }
        else {
            $current = $current.Parent
        }
    }
}

function Write-M3SoakTextAtomic([string]$Path, [string]$Text) {
    $full = [IO.Path]::GetFullPath($Path)
    Assert-M3Soak (-not [IO.File]::Exists($full)) `
        "atomic output already exists: $full"
    $temporary = $full + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllText($temporary, $Text,
            [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $full)
    }
    finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Write-M3SoakJsonAtomic([string]$Path, $Value) {
    $json = $Value | ConvertTo-Json -Depth 30
    Write-M3SoakTextAtomic $Path $json
}

function Get-M3SoakTextHash([string]$Text) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
    }
    finally { $sha.Dispose() }
}

function Get-M3SoakPercentile {
    param(
        [Parameter(Mandatory = $true)]
        [Collections.Generic.List[uint64]]$SortedSamples,
        [ValidateRange(1,100)][int]$Percentile
    )
    Assert-M3Soak ($SortedSamples.Count -gt 0) `
        'latency percentile requires at least one sample'
    $rank = [Math]::Ceiling(
        ([double]$SortedSamples.Count * [double]$Percentile) / 100.0)
    $index = [int]((([Math]::Max($rank, 1.0))) - 1.0)
    return [uint64]$SortedSamples[$index]
}

function Get-M3SoakSourceCommit([string]$RepositoryRoot) {
    $commit = ((& git.exe -C $RepositoryRoot rev-parse HEAD 2>$null) -join '').Trim()
    Assert-M3Soak ($LASTEXITCODE -eq 0 -and
        $commit -match '^[0-9A-Fa-f]{40}$') 'source commit is unavailable'
    return $commit
}

function Read-M3SoakJson([string]$Path, [string]$Name) {
    $full = Get-M3SoakFullPath $Path
    Assert-M3Soak ([IO.File]::Exists($full)) "$Name does not exist"
    Assert-M3SoakNoReparse $full
    $raw = [IO.File]::ReadAllText(
        $full, [Text.UTF8Encoding]::new($false, $true))
    Assert-M3Soak (-not [string]::IsNullOrWhiteSpace($raw)) "$Name is empty"
    return [pscustomobject]@{
        Path = $full
        Json = ($raw | ConvertFrom-Json)
        Sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    }
}

function Invoke-M3SoakSeedProcess {
    param(
        [string]$Executable,
        [string]$RequestedProfile,
        [uint64]$Seed,
        [int]$Timeout
    )
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.WorkingDirectory = Split-Path -Parent $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['MYRD_M3_SOAK_PROFILE'] = $RequestedProfile
    $start.EnvironmentVariables['MYRD_M3_SOAK_SEED'] = [string]$Seed
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        Assert-M3Soak ($process.Start()) 'failed to start soak seed process'
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($Timeout * 1000)) {
            try { $process.Kill() } catch {}
            $process.WaitForExit(30000) | Out-Null
            return [pscustomobject]@{
                ExitCode = 124
                Stdout = $stdoutTask.Result
                Stderr = $stderrTask.Result + "seed process timed out`n"
                ProcessId = $process.Id
            }
        }
        $process.WaitForExit()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdoutTask.Result
            Stderr = $stderrTask.Result
            ProcessId = $process.Id
        }
    }
    finally { $process.Dispose() }
}

function ConvertFrom-M3SoakSeedOutput {
    param([string]$Stdout, [string]$RequestedProfile, [uint64]$RequestedSeed)
    Assert-M3Soak (-not [string]::IsNullOrWhiteSpace($Stdout)) `
        'soak seed stdout is empty'
    Assert-M3Soak ($Stdout -notmatch '(?i)([a-z]:\\|\\\\)') `
        'soak seed stdout leaked an absolute path'
    $lines = @($Stdout -split '\r?\n' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    Assert-M3Soak ($lines.Count -gt 0) 'soak seed stdout has no final line'
    $jsonLines = @()
    foreach ($line in $lines) {
        try {
            $candidate = $line | ConvertFrom-Json
            if ($null -ne $candidate) { $jsonLines += ,$candidate }
        }
        catch {}
    }
    Assert-M3Soak ($jsonLines.Count -eq 1) `
        'soak seed must emit exactly one JSON summary'
    $summary = $null
    try { $summary = $lines[-1] | ConvertFrom-Json }
    catch { throw 'soak seed final line is malformed JSON' }
    foreach ($name in @(
        'schema','seed','profile','operation_count','expected_sha256',
        'actual_sha256','complete_generations_only','mixed_generation',
        'resource','checkpoint','latency','snapshot','coverage','performance','cleanup',
        'cleanup_succeeded')) {
        Assert-M3Soak ($null -ne $summary.PSObject.Properties[$name]) `
            "soak seed property is missing: $name"
    }
    Assert-M3Soak ($summary.schema -eq 'myrd.m3.soak-seed.v2') `
        'soak seed schema mismatch'
    Assert-M3Soak ([uint64]$summary.seed -eq $RequestedSeed) `
        'soak seed identity mismatch'
    Assert-M3Soak ($summary.profile -eq $RequestedProfile) `
        'soak seed profile mismatch'
    Assert-M3Soak ([uint64]$summary.operation_count -gt 0) `
        'soak seed operation count is zero'
    Assert-M3Soak ([string]$summary.expected_sha256 -match
        '^[0-9A-Fa-f]{64}$') 'soak seed expected hash is invalid'
    Assert-M3Soak ([string]$summary.actual_sha256 -ieq
        [string]$summary.expected_sha256) 'soak seed model hash mismatch'
    Assert-M3Soak ([bool]$summary.complete_generations_only) `
        'soak seed used an incomplete generation'
    Assert-M3Soak (-not [bool]$summary.mixed_generation) `
        'soak seed accepted a mixed generation'
    foreach ($name in @(
        'logical_bytes','startup_read_bytes','extra_committed_bytes',
        'allocated_profile_bytes','allocated_profile_startup_read_bytes',
        'allocated_profile_extra_committed_bytes',
        'allocated_profile_structural_read_bytes',
        'allocated_profile_verified')) {
        Assert-M3Soak ($null -ne $summary.resource.PSObject.Properties[$name]) `
            "soak seed resource property is missing: $name"
    }
    Assert-M3Soak ([uint64]$summary.resource.logical_bytes -eq 107374182400) `
        'soak seed logical size is not 100 GiB'
    Assert-M3Soak ([uint64]$summary.resource.startup_read_bytes -le 67108864) `
        'soak seed startup read threshold exceeded'
    Assert-M3Soak ([uint64]$summary.resource.extra_committed_bytes -le 134217728) `
        'soak seed committed-memory threshold exceeded'
    $isAllocatedProfileSeed = $RequestedProfile -eq 'Formal' -and
        $RequestedSeed -eq 3221344269
    if ($isAllocatedProfileSeed) {
        Assert-M3Soak `
            ([uint64]$summary.resource.allocated_profile_bytes -eq 1073741824) `
            'Formal allocated profile is not exactly 1 GiB'
        Assert-M3Soak ([bool]$summary.resource.allocated_profile_verified) `
            'Formal allocated profile was not verified by the production reader'
        Assert-M3Soak `
            ([uint64]$summary.resource.allocated_profile_structural_read_bytes -gt 0) `
            'Formal allocated profile captured no structural reads'
        Assert-M3Soak `
            ([uint64]$summary.resource.allocated_profile_startup_read_bytes -le `
             67108864) 'Formal allocated profile exceeded startup read limit'
        Assert-M3Soak `
            ([uint64]$summary.resource.allocated_profile_extra_committed_bytes -le `
             134217728) 'Formal allocated profile exceeded memory limit'
    }
    else {
        foreach ($name in @(
            'allocated_profile_bytes','allocated_profile_startup_read_bytes',
            'allocated_profile_extra_committed_bytes',
            'allocated_profile_structural_read_bytes')) {
            Assert-M3Soak ([uint64]$summary.resource.$name -eq 0) `
                "non-resource seed claimed allocated profile data: $name"
        }
        Assert-M3Soak (-not [bool]$summary.resource.allocated_profile_verified) `
            'non-resource seed claimed allocated profile verification'
    }
    foreach ($name in @(
        'payload_bytes','journal_growth_bytes','production_payload_bytes',
        'production_journal_growth_bytes')) {
        Assert-M3Soak ($null -ne $summary.checkpoint.PSObject.Properties[$name]) `
            "soak seed checkpoint property is missing: $name"
    }
    foreach ($name in @('mutation_pause_max_ms','writer_stall_max_ms')) {
        Assert-M3Soak ($null -ne $summary.latency.PSObject.Properties[$name]) `
            "soak seed latency property is missing: $name"
    }
    Assert-M3Soak ([uint64]$summary.latency.mutation_pause_max_ms -le 250) `
        'soak seed mutation pause threshold exceeded'
    Assert-M3Soak ([uint64]$summary.latency.writer_stall_max_ms -le 250) `
        'soak seed writer stall threshold exceeded'
    foreach ($name in @('records','cow_peak_bytes','writer_stalls')) {
        Assert-M3Soak ($null -ne $summary.snapshot.PSObject.Properties[$name]) `
            "soak seed snapshot property is missing: $name"
    }
    Assert-M3Soak ([uint64]$summary.snapshot.records -gt 0) `
        'soak seed snapshot record count is zero'
    Assert-M3Soak ([uint64]$summary.snapshot.cow_peak_bytes -ge 65536) `
        'soak seed did not observe a COW pre-image'
    foreach ($name in @(
        'duration_ms','checkpoint_duration_ms','compaction_duration_ms',
        'latency_samples_us')) {
        Assert-M3Soak ($null -ne $summary.performance.PSObject.Properties[$name]) `
            "soak seed performance property is missing: $name"
    }
    Assert-M3Soak ([uint64]$summary.performance.duration_ms -gt 0) `
        'soak seed duration is zero'
    Assert-M3Soak ([uint64]$summary.performance.checkpoint_duration_ms -gt 0) `
        'soak seed checkpoint duration is zero'
    Assert-M3Soak (@($summary.performance.latency_samples_us).Count -gt 0) `
        'soak seed latency sample set is empty'
    foreach ($sample in @($summary.performance.latency_samples_us)) {
        Assert-M3Soak ([uint64]$sample -gt 0) `
            'soak seed latency sample is zero'
    }
    foreach ($name in @(
        'production_checkpoint','reopen_verified','compaction_verified',
        'concurrent_first_touch','cow_overlap','io_fault_recovered',
        'repeated_close')) {
        Assert-M3Soak ($null -ne $summary.coverage.PSObject.Properties[$name]) `
            "soak seed coverage property is missing: $name"
    }
    foreach ($name in @(
        'production_checkpoint','reopen_verified','concurrent_first_touch',
        'cow_overlap','repeated_close')) {
        Assert-M3Soak ([bool]$summary.coverage.$name) `
            "soak seed production coverage is false: $name"
    }
    foreach ($name in @(
        'loading_blocks','active_snapshot_id','snapshot_cow_current_bytes',
        'snapshot_cow_records','snapshot_blocked_writers')) {
        Assert-M3Soak ($null -ne $summary.cleanup.PSObject.Properties[$name]) `
            "soak seed cleanup gauge is missing: $name"
        Assert-M3Soak ([uint64]$summary.cleanup.$name -eq 0) `
            "soak seed cleanup gauge is nonzero: $name"
    }
    Assert-M3Soak `
        ([uint64]$summary.checkpoint.production_payload_bytes -gt 0) `
        'soak seed production checkpoint payload is zero'
    Assert-M3Soak `
        ([uint64]$summary.checkpoint.production_journal_growth_bytes -gt `
         [uint64]$summary.checkpoint.production_payload_bytes) `
        'soak seed production journal did not include framing bytes'
    Assert-M3Soak ([bool]$summary.cleanup_succeeded) `
        'soak seed cleanup failed'
    return $summary
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$evidenceFull = Get-M3SoakFullPath $EvidenceDirectory
Assert-M3Soak ([IO.Directory]::Exists($evidenceFull)) `
    'EvidenceDirectory must already exist'
Assert-M3SoakNoReparse $evidenceFull
Assert-M3Soak (-not (Test-M3SoakPathUnderRoot $evidenceFull $repoRoot)) `
    'EvidenceDirectory cannot be inside the repository'
$executableFull = Get-M3SoakFullPath $SoakExecutable
Assert-M3Soak ([IO.File]::Exists($executableFull)) `
    'SoakExecutable does not exist'
Assert-M3SoakNoReparse $executableFull
$sourceCommit = Get-M3SoakSourceCommit $repoRoot
$binaryHash = (Get-FileHash -LiteralPath $executableFull -Algorithm SHA256).Hash

$realFunctional = $null
$crashAggregate = $null
if ($Profile -eq 'Formal') {
    Assert-M3Soak ($Configuration -eq 'Release') `
        'Formal soak requires Release configuration'
    Assert-M3Soak (-not [string]::IsNullOrWhiteSpace($RealFunctionalReport)) `
        'Formal soak requires RealFunctionalReport'
    Assert-M3Soak (-not [string]::IsNullOrWhiteSpace($CrashAggregateReport)) `
        'Formal soak requires CrashAggregateReport'
    $realFunctional = Read-M3SoakJson $RealFunctionalReport `
        'Task41 RealFunctional report'
    $crashAggregate = Read-M3SoakJson $CrashAggregateReport `
        'Task42 Crash Full report'
}
else {
    Assert-M3Soak ([string]::IsNullOrWhiteSpace($RealFunctionalReport) -and
        [string]::IsNullOrWhiteSpace($CrashAggregateReport)) `
        'Quick soak cannot bind milestone dependencies'
}

if ($Phase -eq 'Validate') {
    Write-Output ([ordered]@{
        schema = 'myrd.m3.soak-validation.v1'
        status = 'PASS'
        profile = $Profile
        source_commit = $sourceCommit
        executable_sha256 = $binaryHash
    } | ConvertTo-Json -Compress)
    exit 0
}

$runDirectory = Join-Path $evidenceFull (
    'soak-' + $Profile.ToLowerInvariant() + '-' +
    [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff') + '-' +
    [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runDirectory) | Out-Null
Assert-M3SoakNoReparse $runDirectory

$seeds = if ($Profile -eq 'Formal') {
    @(0..99 | ForEach-Object { [uint64](3221344269 + $_ * 104729) })
}
else { @([uint64]42, [uint64]12648430, [uint64]9001, [uint64]3735928559) }
$summaries = @()
$failedSeeds = @()
$stdoutBuilder = [Text.StringBuilder]::new()
$stderrBuilder = [Text.StringBuilder]::new()
$artifacts = @()

for ($index = 0; $index -lt $seeds.Count; ++$index) {
    $seed = [uint64]$seeds[$index]
    $process = Invoke-M3SoakSeedProcess -Executable $executableFull `
        -RequestedProfile $Profile -Seed $seed -Timeout $TimeoutSeconds
    $seedName = 'seed-{0:d3}' -f $index
    $stdoutPath = Join-Path $runDirectory ($seedName + '.stdout.txt')
    $stderrPath = Join-Path $runDirectory ($seedName + '.stderr.txt')
    Write-M3SoakTextAtomic $stdoutPath ([string]$process.Stdout)
    Write-M3SoakTextAtomic $stderrPath ([string]$process.Stderr)
    $stdoutHash = (Get-FileHash -LiteralPath $stdoutPath -Algorithm SHA256).Hash
    $stderrHash = (Get-FileHash -LiteralPath $stderrPath -Algorithm SHA256).Hash
    $artifacts += [ordered]@{ role = "$seedName.stdout"; sha256 = $stdoutHash }
    $artifacts += [ordered]@{ role = "$seedName.stderr"; sha256 = $stderrHash }
    [void]$stdoutBuilder.Append("seed=$seed`n")
    [void]$stdoutBuilder.Append([string]$process.Stdout)
    if (-not [string]$process.Stdout.EndsWith("`n")) {
        [void]$stdoutBuilder.Append("`n")
    }
    [void]$stderrBuilder.Append("seed=$seed`n")
    [void]$stderrBuilder.Append([string]$process.Stderr)
    if (-not [string]$process.Stderr.EndsWith("`n")) {
        [void]$stderrBuilder.Append("`n")
    }
    if ([int]$process.ExitCode -ne 0) {
        $failedSeeds += $seed
        break
    }
    try {
        $summaries += ConvertFrom-M3SoakSeedOutput `
            -Stdout ([string]$process.Stdout) -RequestedProfile $Profile `
            -RequestedSeed $seed
    }
    catch {
        $failedSeeds += $seed
        [void]$stderrBuilder.Append("validation=$($_.Exception.Message)`n")
        break
    }
}

$stdoutAggregatePath = Join-Path $runDirectory 'soak.stdout.txt'
$stderrAggregatePath = Join-Path $runDirectory 'soak.stderr.txt'
Write-M3SoakTextAtomic $stdoutAggregatePath $stdoutBuilder.ToString()
Write-M3SoakTextAtomic $stderrAggregatePath $stderrBuilder.ToString()
$stdoutAggregateHash = (Get-FileHash -LiteralPath $stdoutAggregatePath `
    -Algorithm SHA256).Hash
$stderrAggregateHash = (Get-FileHash -LiteralPath $stderrAggregatePath `
    -Algorithm SHA256).Hash
$artifacts += [ordered]@{ role = 'soak.stdout'; sha256 = $stdoutAggregateHash }
$artifacts += [ordered]@{ role = 'soak.stderr'; sha256 = $stderrAggregateHash }

Assert-M3Soak ($failedSeeds.Count -eq 0 -and
    $summaries.Count -eq $seeds.Count) `
    "soak seeds failed: $($failedSeeds -join ',')"
$expectedMaterial = (($summaries | ForEach-Object {
    "$($_.seed):$($_.expected_sha256)" }) -join "`n") + "`n"
$actualMaterial = (($summaries | ForEach-Object {
    "$($_.seed):$($_.actual_sha256)" }) -join "`n") + "`n"
$expectedHash = Get-M3SoakTextHash $expectedMaterial
$actualHash = Get-M3SoakTextHash $actualMaterial
$operationCount = [uint64](($summaries | Measure-Object -Property `
    operation_count -Sum).Sum)
$startupRead = [uint64](@($summaries | ForEach-Object {
    [uint64]$_.resource.startup_read_bytes
} | Measure-Object -Maximum).Maximum)
$extraCommitted = [uint64](@($summaries | ForEach-Object {
    [uint64]$_.resource.extra_committed_bytes
} | Measure-Object -Maximum).Maximum)
$resourceCheckpoint = @($summaries | Sort-Object `
    { [uint64]$_.checkpoint.payload_bytes } -Descending)[0]
$checkpointPayload = [uint64]$resourceCheckpoint.checkpoint.payload_bytes
$journalGrowth = [uint64]$resourceCheckpoint.checkpoint.journal_growth_bytes
$mutationPause = [uint64](@($summaries | ForEach-Object {
    [uint64]$_.latency.mutation_pause_max_ms
} | Measure-Object -Maximum).Maximum)
$writerStall = [uint64](@($summaries | ForEach-Object {
    [uint64]$_.latency.writer_stall_max_ms
} | Measure-Object -Maximum).Maximum)
$productionCheckpointCount = @($summaries | Where-Object {
    [bool]$_.coverage.production_checkpoint }).Count
$reopenCount = @($summaries | Where-Object {
    [bool]$_.coverage.reopen_verified }).Count
$compactionCount = @($summaries | Where-Object {
    [bool]$_.coverage.compaction_verified }).Count
$firstTouchCount = @($summaries | Where-Object {
    [bool]$_.coverage.concurrent_first_touch }).Count
$cowOverlapCount = @($summaries | Where-Object {
    [bool]$_.coverage.cow_overlap }).Count
$ioFaultRecoveryCount = @($summaries | Where-Object {
    [bool]$_.coverage.io_fault_recovered }).Count
$repeatedCloseCount = @($summaries | Where-Object {
    [bool]$_.coverage.repeated_close }).Count
Assert-M3Soak ($productionCheckpointCount -eq $seeds.Count) `
    'not every seed executed a production checkpoint'
Assert-M3Soak ($reopenCount -eq $seeds.Count) `
    'not every seed verified reopen'
Assert-M3Soak ($firstTouchCount -eq $seeds.Count) `
    'not every seed verified concurrent first-touch'
Assert-M3Soak ($cowOverlapCount -eq $seeds.Count) `
    'not every seed observed COW overlap'
Assert-M3Soak ($repeatedCloseCount -eq $seeds.Count) `
    'not every seed verified repeated close'
Assert-M3Soak ($compactionCount -ge 1) `
    'soak set did not execute production compaction'
Assert-M3Soak ($ioFaultRecoveryCount -ge 1) `
    'soak set did not recover an injected product I/O fault'
$allocatedProfiles = @($summaries | Where-Object {
    [bool]$_.resource.allocated_profile_verified })
if ($Profile -eq 'Formal') {
    Assert-M3Soak ($allocatedProfiles.Count -eq 1 -and
        [uint64]$allocatedProfiles[0].seed -eq 3221344269 -and
        [uint64]$allocatedProfiles[0].resource.allocated_profile_bytes -eq
            1073741824) 'Formal allocated-image profile coverage is invalid'
}
else {
    Assert-M3Soak ($allocatedProfiles.Count -eq 0) `
        'Quick soak claimed Formal allocated-image coverage'
}
$allocatedProfileSeed = if ($allocatedProfiles.Count -eq 1) {
    [uint64]$allocatedProfiles[0].seed
}
else { [uint64]0 }
$allocatedProfileBytes = if ($allocatedProfiles.Count -eq 1) {
    [uint64]$allocatedProfiles[0].resource.allocated_profile_bytes
}
else { [uint64]0 }

$cleanupGauges = [ordered]@{}
foreach ($name in @(
    'loading_blocks','active_snapshot_id','snapshot_cow_current_bytes',
    'snapshot_cow_records','snapshot_blocked_writers')) {
    $cleanupGauges[$name] = [uint64](@($summaries | ForEach-Object {
        [uint64]$_.cleanup.$name
    } | Measure-Object -Maximum).Maximum)
}
$totalDurationMs = [uint64](@($summaries | ForEach-Object {
    [uint64]$_.performance.duration_ms
} | Measure-Object -Sum).Sum)
Assert-M3Soak ($totalDurationMs -gt 0) 'soak total duration is zero'
$latencySamples = [Collections.Generic.List[uint64]]::new()
foreach ($summary in $summaries) {
    foreach ($sample in @($summary.performance.latency_samples_us)) {
        $latencySamples.Add([uint64]$sample)
    }
}
$latencySamples.Sort()
$throughput = [uint64][Math]::Floor(
    ([double]$operationCount * 1000.0) / [double]$totalDurationMs)
$throughput = [uint64](@([uint64]1, [uint64]$throughput) |
    Measure-Object -Maximum).Maximum
$checkpointDuration = [uint64](@($summaries | ForEach-Object {
    [uint64]$_.performance.checkpoint_duration_ms
} | Measure-Object -Maximum).Maximum)
$compactionDuration = [uint64](@($summaries | ForEach-Object {
    [uint64]$_.performance.compaction_duration_ms
} | Measure-Object -Maximum).Maximum)
Assert-M3Soak ($checkpointDuration -gt 0) `
    'soak checkpoint duration observation is zero'
Assert-M3Soak ($compactionDuration -gt 0) `
    'soak compaction duration observation is zero'

$dependencies = if ($Profile -eq 'Formal') {
    [ordered]@{
        real_functional = [ordered]@{
            status = [string]$realFunctional.Json.status
            source_commit = [string]$realFunctional.Json.source_commit
            sha256 = $realFunctional.Sha256
        }
        crash_full = [ordered]@{
            status = [string]$crashAggregate.Json.status
            task42_complete = [bool]$crashAggregate.Json.task42_complete
            source_commit = [string]$crashAggregate.Json.source_commit
            sha256 = $crashAggregate.Sha256
        }
    }
}
else { $null }

$report = [ordered]@{
    schema = 'myrd.m3.soak-evidence.v2'
    status = 'PASS'
    terminal_verdict = 'PASS'
    profile = $Profile
    source_commit = $sourceCommit
    task43_complete = ($Profile -eq 'Formal')
    milestone_eligible = ($Profile -eq 'Formal')
    build = [ordered]@{
        build_type = $Configuration
        soak_executable_sha256 = $binaryHash
        stdout_sha256 = $stdoutAggregateHash
        stderr_sha256 = $stderrAggregateHash
    }
    execution = [ordered]@{
        seed_count = $seeds.Count
        failed_seeds = @()
        process_exit_code = 0
        operation_count = $operationCount
        stdout_sha256 = $stdoutAggregateHash
        stderr_sha256 = $stderrAggregateHash
    }
    correctness = [ordered]@{
        expected_sha256 = $expectedHash
        actual_sha256 = $actualHash
        hash_match = ($expectedHash -ieq $actualHash)
        complete_generations_only = -not (@($summaries |
            Where-Object { -not [bool]$_.complete_generations_only }).Count)
        mixed_generation = [bool](@($summaries |
            Where-Object { [bool]$_.mixed_generation }).Count)
    }
    coverage = [ordered]@{
        production_checkpoint_seeds = $productionCheckpointCount
        reopen_verified_seeds = $reopenCount
        compaction_verified_seeds = $compactionCount
        concurrent_first_touch_seeds = $firstTouchCount
        cow_overlap_seeds = $cowOverlapCount
        io_fault_recovered_seeds = $ioFaultRecoveryCount
        repeated_close_seeds = $repeatedCloseCount
        resource_checkpoint_seed = [uint64]$resourceCheckpoint.seed
        allocated_profile_seed = $allocatedProfileSeed
    }
    thresholds = [ordered]@{
        startup_read_limit_bytes = 67108864
        startup_read_bytes = $startupRead
        extra_committed_limit_bytes = 134217728
        extra_committed_bytes = $extraCommitted
        allocated_profile_bytes = $allocatedProfileBytes
        checkpoint_payload_bytes = $checkpointPayload
        journal_growth_limit_bytes = 322122547
        journal_growth_bytes = $journalGrowth
        mutation_pause_limit_ms = 250
        mutation_pause_max_ms = $mutationPause
        writer_stall_limit_ms = 250
        writer_stall_max_ms = $writerStall
    }
    performance = [ordered]@{
        duration_ms = $totalDurationMs
        throughput_operations_per_second = [uint64]$throughput
        latency_sample_count = [uint64]$latencySamples.Count
        latency_us_p50 = Get-M3SoakPercentile $latencySamples 50
        latency_us_p95 = Get-M3SoakPercentile $latencySamples 95
        latency_us_p99 = Get-M3SoakPercentile $latencySamples 99
        checkpoint_duration_ms = $checkpointDuration
        compaction_duration_ms = $compactionDuration
    }
    dependencies = $dependencies
    cleanup = [ordered]@{
        succeeded = $true
        residuals = @()
        loading_blocks = $cleanupGauges.loading_blocks
        active_snapshot_id = $cleanupGauges.active_snapshot_id
        snapshot_cow_current_bytes = $cleanupGauges.snapshot_cow_current_bytes
        snapshot_cow_records = $cleanupGauges.snapshot_cow_records
        snapshot_blocked_writers = $cleanupGauges.snapshot_blocked_writers
    }
    limitations = @(
        'Performance values are engineering observations on the audited machine, not a Primo parity or fixed-bandwidth claim.',
        'The 256 MiB amplification case measures the production JournalWriter stream; every seed separately exercises full M3Provider checkpoints.'
    )
    artifacts = $artifacts
    error = $null
}
$reportPath = Join-Path $runDirectory 'm3-soak-report.json'
Write-M3SoakJsonAtomic $reportPath $report

$verifier = Join-Path $repoRoot 'tools\Verify-M3Json.ps1'
$verifyArguments = @(
    '-Path', $reportPath,
    '-ExpectedSourceCommit', $sourceCommit,
    '-ExpectedBinarySha256', $binaryHash,
    '-StdoutPath', $stdoutAggregatePath,
    '-StderrPath', $stderrAggregatePath)
if ($Profile -eq 'Formal') {
    $verifyArguments += @(
        '-RealFunctionalReportPath', $realFunctional.Path,
        '-CrashAggregateReportPath', $crashAggregate.Path)
}
$oldPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $null = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $verifier @verifyArguments 2>&1
    $verifyExit = $LASTEXITCODE
}
finally { $ErrorActionPreference = $oldPreference }
Assert-M3Soak ($verifyExit -eq 0) `
    "independent soak verification failed with exit code $verifyExit"

Write-Output "M3 Soak $Profile PASS: $reportPath"
exit 0
