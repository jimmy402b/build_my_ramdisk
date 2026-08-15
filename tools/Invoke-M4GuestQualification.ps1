[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Run')]
    [Parameter(Mandatory = $true, ParameterSetName = 'Status')]
    [string]$QualificationRoot,

    [Parameter(Mandatory = $true, ParameterSetName = 'Status')]
    [switch]$Status,

    [Parameter(Mandatory = $true, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

class M4QualificationBlocked : System.Exception {
    M4QualificationBlocked([string]$message) : base($message) {}
}

function Need {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw [M4QualificationBlocked]::new($Message) }
}

function Get-ExactPropertyNames {
    param([Parameter(Mandatory = $true)]$Value)
    if ($Value -is [Collections.IDictionary]) { return @($Value.Keys | ForEach-Object { [string]$_ }) }
    return @($Value.PSObject.Properties | ForEach-Object { $_.Name })
}

function Assert-QualificationKit {
    param(
        [Parameter(Mandatory = $true)][string]$Kit,
        [Parameter(Mandatory = $true)][string]$ExpectedCommit
    )

    try {
        $expectedNames = @(
            'Capture-M4Child.ps1', 'Invoke-M4GuestQualification.ps1',
            'Invoke-M4RunLifecycle.ps1', 'Invoke-M4Verification.ps1',
            'Prepare-M4NegativeFixtures.ps1', 'myrd.exe', 'myrd_m4_aim_query.exe')
        Need ($ExpectedCommit -cmatch '^[0-9a-f]{40}$') 'expected qualification-kit commit is invalid'
        $root = [IO.Path]::GetFullPath($Kit)
        Need ([IO.Directory]::Exists($root)) 'qualification kit root is missing'
        Need (@([IO.Directory]::GetDirectories($root)).Count -eq 0) 'qualification kit contains a subdirectory'
        $rootFiles = @([IO.Directory]::GetFiles($root) | ForEach-Object { [IO.Path]::GetFileName($_) })
        $allNames = @($expectedNames + 'qualification-kit.json')
        Need ($rootFiles.Count -eq 8) 'qualification kit root file count is invalid'
        foreach ($name in $allNames) { Need ($rootFiles -ccontains $name) "qualification kit root is missing $name" }

        $manifestPath = Join-Path $root 'qualification-kit.json'
        $bytes = [IO.File]::ReadAllBytes($manifestPath)
        Need ($bytes.Length -lt 3 -or -not ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) 'qualification kit manifest has a UTF-8 BOM'
        $manifest = [Text.UTF8Encoding]::new($false, $true).GetString($bytes) | ConvertFrom-Json
        $manifestProperties = Get-ExactPropertyNames -Value $manifest
        Need ($manifestProperties.Count -eq 3) 'qualification kit manifest property count is invalid'
        foreach ($name in @('schema', 'source_commit', 'files')) { Need ($manifestProperties -ccontains $name) "qualification kit manifest is missing $name" }
        Need ([string]$manifest.schema -ceq 'myrd.m4.qualification-kit.v1') 'qualification kit manifest schema is invalid'
        Need ([string]$manifest.source_commit -ceq $ExpectedCommit) 'qualification kit manifest source commit is invalid'
        $records = @($manifest.files)
        Need ($records.Count -eq 7) 'qualification kit manifest file count is invalid'
        Need (([string]::Join('|', @($records | ForEach-Object { [string]$_.name }))) -ceq ([string]::Join('|', $expectedNames))) 'qualification kit manifest names are invalid'
        foreach ($record in $records) {
            $properties = Get-ExactPropertyNames -Value $record
            Need ($properties.Count -eq 3) 'qualification kit manifest file property count is invalid'
            foreach ($property in @('name', 'length', 'sha256')) { Need ($properties -ccontains $property) "qualification kit manifest file is missing $property" }
            $name = [string]$record.name
            Need ($expectedNames -ccontains $name) "qualification kit manifest names unknown file $name"
            $lengthText = [string]$record.length
            [int64]$length = 0
            Need ($lengthText -cmatch '^[0-9]+$' -and [Int64]::TryParse($lengthText, [ref]$length)) "qualification kit manifest length is invalid for $name"
            $filePath = Join-Path $root $name
            Need ($length -eq ([IO.FileInfo]$filePath).Length) "qualification kit manifest length does not match $name"
            $hash = [string]$record.sha256
            Need ($hash -cmatch '^[0-9A-F]{64}$') "qualification kit manifest hash format is invalid for $name"
            Need ($hash -ceq (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToUpperInvariant()) "qualification kit manifest hash does not match $name"
        }
        return $manifest
    }
    catch [M4QualificationBlocked] { throw }
    catch { throw [M4QualificationBlocked]::new("qualification kit is invalid: $($_.Exception.Message)") }
}

function Invoke-QualificationKitValidatorSelfTest {
    param([Parameter(Mandatory = $true)][string]$Scratch)

    $kit = Join-Path $Scratch 'qualification-kit-selftest'
    [IO.Directory]::CreateDirectory($kit) | Out-Null
    $names = @('Capture-M4Child.ps1', 'Invoke-M4GuestQualification.ps1', 'Invoke-M4RunLifecycle.ps1', 'Invoke-M4Verification.ps1', 'Prepare-M4NegativeFixtures.ps1', 'myrd.exe', 'myrd_m4_aim_query.exe')
    foreach ($name in $names) { [IO.File]::WriteAllText((Join-Path $kit $name), $name, [Text.UTF8Encoding]::new($false)) }
    $commit = ('a' * 40)
    $manifestPath = Join-Path $kit 'qualification-kit.json'
    $writeManifest = {
        param($SourceCommit, $Records)
        $manifest = [ordered]@{ schema = 'myrd.m4.qualification-kit.v1'; source_commit = $SourceCommit; files = @($Records) }
        [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    }
    $newRecords = {
        return @($names | ForEach-Object {
            $path = Join-Path $kit $_
            [ordered]@{ name = $_; length = [int64]([IO.FileInfo]$path).Length; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant() }
        })
    }
    & $writeManifest $commit (& $newRecords)
    Assert-QualificationKit -Kit $kit -ExpectedCommit $commit | Out-Null
    $reject = {
        param([Parameter(Mandatory = $true)][string]$ExpectedMessage)
        $blocked = $false
        $message = $null
        try { Assert-QualificationKit -Kit $kit -ExpectedCommit $commit | Out-Null } catch [M4QualificationBlocked] { $blocked = $true; $message = $_.Exception.Message }
        Need $blocked 'invalid qualification kit was not blocked by type'
        Need ($message -clike "*$ExpectedMessage*") "invalid qualification kit reported the wrong reason: $message"
    }
    $victim = Join-Path $kit $names[0]
    [IO.File]::Delete($victim); & $reject 'qualification kit root file count is invalid'; [IO.File]::WriteAllText($victim, $names[0], [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $kit 'extra.txt'), 'extra'); & $reject 'qualification kit root file count is invalid'; [IO.File]::Delete((Join-Path $kit 'extra.txt'))
    [IO.Directory]::CreateDirectory((Join-Path $kit 'nested')) | Out-Null; & $reject 'qualification kit contains a subdirectory'; [IO.Directory]::Delete((Join-Path $kit 'nested'))
    $bad = & $newRecords; $bad[0].length = [int64]999; & $writeManifest $commit $bad; & $reject 'qualification kit manifest length does not match'
    $bad = & $newRecords; $bad[0].sha256 = ('B' * 64); & $writeManifest $commit $bad; & $reject 'qualification kit manifest hash does not match'
    & $writeManifest ('b' * 40) (& $newRecords); & $reject 'qualification kit manifest source commit is invalid'
    $self = @((& $newRecords) + [ordered]@{ name = 'qualification-kit.json'; length = 1; sha256 = ('A' * 64) }); & $writeManifest $commit $self; & $reject 'qualification kit manifest file count is invalid'
    $few = & $newRecords; & $writeManifest $commit @($few[0..5]); & $reject 'qualification kit manifest file count is invalid'
    $extra = @((& $newRecords) + [ordered]@{ name = 'extra.txt'; length = 1; sha256 = ('A' * 64) }); & $writeManifest $commit $extra; & $reject 'qualification kit manifest file count is invalid'
    & $writeManifest $commit (& $newRecords)
    $cleanManifest = [IO.File]::ReadAllBytes($manifestPath)
    $bomManifest = [byte[]]::new($cleanManifest.Length + 3)
    $bomManifest[0] = 0xEF; $bomManifest[1] = 0xBB; $bomManifest[2] = 0xBF
    [Array]::Copy($cleanManifest, 0, $bomManifest, 3, $cleanManifest.Length)
    [IO.File]::WriteAllBytes($manifestPath, $bomManifest); & $reject 'qualification kit manifest has a UTF-8 BOM'
    [IO.File]::WriteAllBytes($manifestPath, $cleanManifest)
}

function Get-QualificationStatePath {
    param([Parameter(Mandatory = $true)][string]$QualificationRoot)
    return (Join-Path ([IO.Path]::GetFullPath($QualificationRoot)) 'state.json')
}

function Assert-QualificationStateSchema {
    param([Parameter(Mandatory = $true)]$State)

    $expected = @('schema', 'qualification_id', 'state', 'sequence')
    if ($State -is [Collections.IDictionary]) {
        $actual = @($State.Keys | ForEach-Object { [string]$_ })
    }
    else {
        $actual = @($State.PSObject.Properties | ForEach-Object { $_.Name })
    }
    Need ($actual.Count -eq $expected.Count) 'qualification state has an invalid property count'
    foreach ($name in $expected) {
        Need ($actual -ccontains $name) "qualification state is missing $name"
    }
    Need ([string]$State.schema -ceq 'myrd.m4.guest-qualification.v1') 'qualification state schema is invalid'
    Need (-not [string]::IsNullOrWhiteSpace([string]$State.qualification_id)) 'qualification state id is invalid'
    Need ($script:QualificationStates -ccontains [string]$State.state) 'qualification state value is invalid'
    $sequenceText = [string]$State.sequence
    Need ($sequenceText -match '^[0-9]+$') 'qualification state sequence is invalid'
    [uint64]$sequenceValue = 0
    Need ([UInt64]::TryParse($sequenceText, [ref]$sequenceValue)) 'qualification state sequence is out of range'
    return $State
}

function Write-DurableUtf8 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )

    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Content)
    $stream = [IO.FileStream]::new(
        $Path,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None,
        4096,
        [IO.FileOptions]::WriteThrough)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
}

function Read-QualificationStateFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    Need ([IO.File]::Exists($Path)) 'qualification state file is missing'
    try {
        $state = [IO.File]::ReadAllText($Path, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
        return (Assert-QualificationStateSchema -State $state)
    }
    catch [M4QualificationBlocked] {
        throw
    }
    catch {
        throw [M4QualificationBlocked]::new("qualification state file is invalid: $($_.Exception.Message)")
    }
}

function Read-QualificationState {
    param([Parameter(Mandatory = $true)][string]$QualificationRoot)
    return (Read-QualificationStateFile -Path (Get-QualificationStatePath -QualificationRoot $QualificationRoot))
}

function Append-QualificationEvent {
    param(
        [Parameter(Mandatory = $true)][string]$QualificationRoot,
        [Parameter(Mandatory = $true)]$State
    )

    $eventPath = Join-Path ([IO.Path]::GetFullPath($QualificationRoot)) 'events.jsonl'
    $line = (($State | ConvertTo-Json -Compress -Depth 4) + [Environment]::NewLine)
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($line)
    $stream = [IO.FileStream]::new(
        $eventPath,
        [IO.FileMode]::Append,
        [IO.FileAccess]::Write,
        [IO.FileShare]::Read,
        4096,
        [IO.FileOptions]::WriteThrough)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
}

function Complete-QualificationRecoveryArtifacts {
    param(
        [Parameter(Mandatory = $true)][string[]]$Paths,
        [Parameter(Mandatory = $true)]
        [ValidateSet('CommitSucceeded', 'CommitNotApplied', 'RollbackSucceeded', 'RollbackFailed')]
        [string]$Outcome
    )

    $cleanupErrors = @()
    $retainedPaths = @()
    foreach ($path in $Paths) {
        if (-not [IO.File]::Exists($path)) { continue }
        if ($Outcome -ceq 'RollbackFailed') {
            $retainedPaths += $path
            continue
        }
        try {
            [IO.File]::Delete($path)
        }
        catch {
            $cleanupErrors += "${path}: $($_.Exception.Message)"
        }
    }
    return [pscustomobject]@{
        cleanup_errors = @($cleanupErrors)
        retained_paths = @($retainedPaths)
    }
}

function Write-QualificationState {
    param(
        [Parameter(Mandatory = $true)][string]$QualificationRoot,
        [Parameter(Mandatory = $true)]$State
    )

    $root = [IO.Path]::GetFullPath($QualificationRoot)
    Need ([IO.Directory]::Exists($root)) 'qualification root is missing'
    Assert-QualificationStateSchema -State $State | Out-Null
    $statePath = Get-QualificationStatePath -QualificationRoot $root
    if ([IO.File]::Exists($statePath)) {
        $previousState = Read-QualificationStateFile -Path $statePath
        Need ([string]::Equals([string]$previousState.qualification_id, [string]$State.qualification_id, [StringComparison]::Ordinal)) 'qualification id cannot change'
        [uint64]$previousSequence = [uint64]$previousState.sequence
        Need ($previousSequence -lt [UInt64]::MaxValue) 'qualification state sequence cannot overflow'
        Need ([uint64]$State.sequence -eq ($previousSequence + 1)) 'qualification state sequence must advance by one'
        Assert-Transition -From ([string]$previousState.state) -To ([string]$State.state)
    }
    else {
        Need ([string]$State.state -ceq 'Initial') 'first qualification state must be Initial'
        Need ([uint64]$State.sequence -eq 0) 'first qualification state sequence must be zero'
    }
    $tempPath = Join-Path $root ('.state-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $backupPath = Join-Path $root ('.state-' + [Guid]::NewGuid().ToString('N') + '.bak')
    $rollbackBackupPath = Join-Path $root ('.state-' + [Guid]::NewGuid().ToString('N') + '.rollback.bak')
    $eventPath = Join-Path $root 'events.jsonl'
    $hadState = [IO.File]::Exists($statePath)
    $hadEventFile = [IO.File]::Exists($eventPath)
    [int64]$eventLength = if ($hadEventFile) { ([IO.FileInfo]$eventPath).Length } else { 0 }
    $stateCommitted = $false
    $transactionError = $null
    $rollbackError = $null
    $tempCleanupError = $null
    $outcome = 'CommitNotApplied'
    try {
        $json = $State | ConvertTo-Json -Compress -Depth 4
        Write-DurableUtf8 -Path $tempPath -Content ($json + [Environment]::NewLine)
        Read-QualificationStateFile -Path $tempPath | Out-Null
        if ($hadState) {
            [IO.File]::Replace($tempPath, $statePath, $backupPath)
        }
        else {
            [IO.File]::Move($tempPath, $statePath)
        }
        $stateCommitted = $true
        Append-QualificationEvent -QualificationRoot $root -State $State
        $outcome = 'CommitSucceeded'
    }
    catch {
        $transactionError = $_.Exception
        if ($stateCommitted) {
            try {
                if ($hadState) {
                    if (-not [IO.File]::Exists($backupPath)) { throw 'qualification rollback backup is missing' }
                    [IO.File]::Replace($backupPath, $statePath, $rollbackBackupPath)
                }
                elseif ([IO.File]::Exists($statePath)) {
                    [IO.File]::Delete($statePath)
                }

                if ($hadEventFile) {
                    if (-not [IO.File]::Exists($eventPath)) { throw 'qualification event file disappeared during rollback' }
                    $eventStream = [IO.FileStream]::new(
                        $eventPath,
                        [IO.FileMode]::Open,
                        [IO.FileAccess]::Write,
                        [IO.FileShare]::Read,
                        4096,
                        [IO.FileOptions]::WriteThrough)
                    try {
                        $eventStream.SetLength($eventLength)
                        $eventStream.Flush($true)
                    }
                    finally {
                        $eventStream.Dispose()
                    }
                }
                elseif ([IO.File]::Exists($eventPath)) {
                    [IO.File]::Delete($eventPath)
                }
                $outcome = 'RollbackSucceeded'
            }
            catch {
                $rollbackError = $_.Exception
                $outcome = 'RollbackFailed'
            }
        }
    }
    finally {
        if ([IO.File]::Exists($tempPath)) {
            try {
                [IO.File]::Delete($tempPath)
            }
            catch {
                $tempCleanupError = $_.Exception
            }
        }
    }
    $artifactResult = Complete-QualificationRecoveryArtifacts `
        -Paths @($backupPath, $rollbackBackupPath) -Outcome $outcome

    $cleanupDetails = @()
    if ($null -ne $tempCleanupError) {
        $cleanupDetails += "temp cleanup failed at ${tempPath}: $($tempCleanupError.Message)"
    }
    $cleanupDetails += @($artifactResult.cleanup_errors)

    if ($null -ne $transactionError) {
        $message = "qualification commit failed: $($transactionError.Message)"
        if ($null -ne $rollbackError) {
            $message += "; rollback failed: $($rollbackError.Message)"
            if ($artifactResult.retained_paths.Count -gt 0) {
                $message += "; recovery artifacts retained at: $($artifactResult.retained_paths -join '; ')"
            }
            else {
                $message += '; no recovery artifact remained; manual recovery may be required'
            }
        }
        if ($cleanupDetails.Count -gt 0) {
            $message += "; cleanup failed: $($cleanupDetails -join '; ')"
        }
        throw [M4QualificationBlocked]::new($message)
    }
    if ($cleanupDetails.Count -gt 0) {
        throw [M4QualificationBlocked]::new("qualification commit succeeded but cleanup failed: $($cleanupDetails -join '; ')")
    }
}

$script:QualificationStates = @(
    'Initial', 'FormalCreateReady', 'AwaitingCrashReset', 'CrashResetObserved',
    'FormalRecovered', 'AwaitingLivePowerCut', 'LivePowerCutObserved',
    'LivePowerRecovered', 'FixturesPrepared', 'AwaitingMissingDriverReset',
    'MissingDriverResetObserved', 'MissingDriverCaptured', 'DriverRestored',
    'NegativeComplete', 'Complete'
)

function Assert-Transition {
    param(
        [Parameter(Mandatory = $true)][string]$From,
        [Parameter(Mandatory = $true)][string]$To
    )

    $fromIndex = [array]::IndexOf($script:QualificationStates, $From)
    $toIndex = [array]::IndexOf($script:QualificationStates, $To)
    if ($fromIndex -lt 0 -or $toIndex -lt 0 -or $toIndex -ne ($fromIndex + 1)) {
        throw [M4QualificationBlocked]::new("invalid transition $From -> $To")
    }
}

function Invoke-SelfTest {
    $scratch = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-guest-qualification-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($scratch) | Out-Null
    try {
        Invoke-QualificationKitValidatorSelfTest -Scratch $scratch
        $expectedStates = @(
            'Initial', 'FormalCreateReady', 'AwaitingCrashReset', 'CrashResetObserved',
            'FormalRecovered', 'AwaitingLivePowerCut', 'LivePowerCutObserved',
            'LivePowerRecovered', 'FixturesPrepared', 'AwaitingMissingDriverReset',
            'MissingDriverResetObserved', 'MissingDriverCaptured', 'DriverRestored',
            'NegativeComplete', 'Complete'
        )
        Need (([string]::Join('|', $script:QualificationStates)) -ceq ([string]::Join('|', $expectedStates))) 'qualification state graph changed'

        $commitArtifact = Join-Path $scratch '.selftest-commit.bak'
        $rollbackArtifact = Join-Path $scratch '.selftest-rollback.bak'
        $retainedArtifact = Join-Path $scratch '.selftest-retained.bak'
        [IO.File]::WriteAllText($commitArtifact, 'commit')
        $artifactResult = Complete-QualificationRecoveryArtifacts -Paths @($commitArtifact) -Outcome CommitSucceeded
        Need (-not [IO.File]::Exists($commitArtifact)) 'successful commit retained a recovery artifact'
        Need ($artifactResult.cleanup_errors.Count -eq 0) 'successful commit recovery cleanup failed'
        [IO.File]::WriteAllText($rollbackArtifact, 'rollback')
        $artifactResult = Complete-QualificationRecoveryArtifacts -Paths @($rollbackArtifact) -Outcome RollbackSucceeded
        Need (-not [IO.File]::Exists($rollbackArtifact)) 'successful rollback retained a recovery artifact'
        Need ($artifactResult.cleanup_errors.Count -eq 0) 'successful rollback recovery cleanup failed'
        [IO.File]::WriteAllText($retainedArtifact, 'retained')
        $artifactResult = Complete-QualificationRecoveryArtifacts -Paths @($retainedArtifact) -Outcome RollbackFailed
        Need ([IO.File]::Exists($retainedArtifact)) 'failed rollback deleted its recovery artifact'
        Need ($artifactResult.retained_paths.Count -eq 1 -and $artifactResult.retained_paths[0] -ceq $retainedArtifact) 'failed rollback did not report its retained recovery artifact'
        [IO.File]::Delete($retainedArtifact)

        $state = [ordered]@{
            schema = 'myrd.m4.guest-qualification.v1'
            qualification_id = 'q1'
            state = 'Initial'
            sequence = 0
        }
        Write-QualificationState -QualificationRoot $scratch -State $state
        $roundTrip = Read-QualificationState -QualificationRoot $scratch
        Need ($roundTrip.schema -ceq 'myrd.m4.guest-qualification.v1') 'state schema did not round-trip'
        Need ($roundTrip.qualification_id -ceq 'q1') 'qualification id did not round-trip'
        Need ($roundTrip.state -ceq 'Initial') 'initial state did not round-trip'
        Need ([int]$roundTrip.sequence -eq 0) 'initial sequence did not round-trip'
        Assert-Transition -From 'Initial' -To 'FormalCreateReady'
        $next = [ordered]@{
            schema = 'myrd.m4.guest-qualification.v1'
            qualification_id = 'q1'
            state = 'FormalCreateReady'
            sequence = 1
        }
        Write-QualificationState -QualificationRoot $scratch -State $next
        $current = Read-QualificationState -QualificationRoot $scratch
        Need ($current.state -ceq 'FormalCreateReady' -and [uint64]$current.sequence -eq 1) 'legal transition did not persist'
        $events = @(
            Get-Content -LiteralPath (Join-Path $scratch 'events.jsonl') -Encoding UTF8 |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
                ForEach-Object { $_ | ConvertFrom-Json }
        )
        Need ($events.Count -eq 2) 'event sequence count is invalid'
        Need ([int]$events[0].sequence -eq 0 -and [int]$events[1].sequence -eq 1) 'event sequence order is invalid'

        $statePath = Join-Path $scratch 'state.json'
        $eventsPath = Join-Path $scratch 'events.jsonl'
        $beforeState = [IO.File]::ReadAllBytes($statePath)
        $beforeEvents = [IO.File]::ReadAllBytes($eventsPath)
        foreach ($invalid in @(
            [ordered]@{ schema = 'myrd.m4.guest-qualification.v1'; qualification_id = 'q1'; state = 'Initial'; sequence = 2 },
            [ordered]@{ schema = 'myrd.m4.guest-qualification.v1'; qualification_id = 'q1'; state = 'FormalRecovered'; sequence = 2 },
            [ordered]@{ schema = 'myrd.m4.guest-qualification.v1'; qualification_id = 'q2'; state = 'FormalCreateReady'; sequence = 2 },
            [ordered]@{ schema = 'myrd.m4.guest-qualification.v1'; qualification_id = 'q1'; state = 'FormalCreateReady'; sequence = 3 }
        )) {
            $blocked = $false
            try { Write-QualificationState -QualificationRoot $scratch -State $invalid } catch [M4QualificationBlocked] { $blocked = $true }
            Need $blocked 'invalid state write was not blocked by type'
            Need ([Collections.StructuralComparisons]::StructuralEqualityComparer.Equals($beforeState, [IO.File]::ReadAllBytes($statePath))) 'failed state write changed state.json'
            Need ([Collections.StructuralComparisons]::StructuralEqualityComparer.Equals($beforeEvents, [IO.File]::ReadAllBytes($eventsPath))) 'failed state write changed events.jsonl'
            Need (@([IO.Directory]::GetFiles($scratch, '.state-*.tmp')).Count -eq 0) 'failed state write left a temp file'
        }

        $appendFailureState = [ordered]@{
            schema = 'myrd.m4.guest-qualification.v1'
            qualification_id = 'q1'
            state = 'AwaitingCrashReset'
            sequence = 2
        }
        $eventsBeforeAppendFailure = [IO.File]::ReadAllBytes($eventsPath)
        Remove-Item -LiteralPath $eventsPath -Force
        [IO.Directory]::CreateDirectory($eventsPath) | Out-Null
        try {
            $blocked = $false
            try { Write-QualificationState -QualificationRoot $scratch -State $appendFailureState } catch [M4QualificationBlocked] { $blocked = $true }
            Need $blocked 'append failure was not blocked by type'
            Need ([Collections.StructuralComparisons]::StructuralEqualityComparer.Equals($beforeState, [IO.File]::ReadAllBytes($statePath))) 'append failure changed state.json'
            Need ([IO.Directory]::Exists($eventsPath)) 'append failure changed events.json shape'
            Need (@([IO.Directory]::GetFiles($scratch, '.state-*.tmp')).Count -eq 0) 'append failure left a temp file'
            Need (@([IO.Directory]::GetFiles($scratch, '.state-*.bak')).Count -eq 0) 'append failure left a backup file'
        }
        finally {
            if ([IO.Directory]::Exists($eventsPath)) { Remove-Item -LiteralPath $eventsPath -Recurse -Force }
            [IO.File]::WriteAllBytes($eventsPath, $eventsBeforeAppendFailure)
        }

        $blocked = $false
        try { Assert-Transition -From 'FormalCreateReady' -To 'Complete' } catch [M4QualificationBlocked] { $blocked = $true }
        Need $blocked 'FormalCreateReady -> Complete must be blocked'

        $corruptPayloads = [Collections.Generic.List[byte[]]]::new()
        $corruptPayloads.Add([byte[]](0xFF, 0xFE, 0xFD))
        $corruptPayloads.Add([Text.UTF8Encoding]::new($false).GetBytes('{not-json'))
        $corruptPayloads.Add([Text.UTF8Encoding]::new($false).GetBytes('{"schema":"invalid","qualification_id":"q1","state":"FormalCreateReady","sequence":1}'))
        $corruptPayloads.Add([Text.UTF8Encoding]::new($false).GetBytes('{"Schema":"myrd.m4.guest-qualification.v1","qualification_id":"q1","state":"FormalCreateReady","sequence":1}'))
        $corruptPayloads.Add([Text.UTF8Encoding]::new($false).GetBytes('{"schema":"myrd.m4.guest-qualification.v1","qualification_id":"q1","state":"formalcreateready","sequence":1}'))
        foreach ($corruptPayload in $corruptPayloads) {
            [IO.File]::WriteAllBytes($statePath, $corruptPayload)
            $blocked = $false
            try { Read-QualificationState -QualificationRoot $scratch | Out-Null } catch [M4QualificationBlocked] { $blocked = $true }
            Need $blocked 'corrupt state was not rejected by type'
            Need ([Collections.StructuralComparisons]::StructuralEqualityComparer.Equals($beforeEvents, [IO.File]::ReadAllBytes($eventsPath))) 'corrupt state read changed events.jsonl'
        }
    }
    finally {
        if ([IO.Directory]::Exists($scratch)) {
            Remove-Item -LiteralPath $scratch -Recurse -Force
        }
    }
}

if ($SelfTest) {
    Invoke-SelfTest
    Write-Output 'M4 guest qualification SelfTest PASS'
    exit 0
}

throw [M4QualificationBlocked]::new('Run and Status are not implemented in Task 1')
