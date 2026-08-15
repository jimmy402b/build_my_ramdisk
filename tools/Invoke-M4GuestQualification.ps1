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

        if ([IO.File]::Exists($backupPath)) {
            [IO.File]::Delete($backupPath)
        }
    }
    catch {
        $commitError = $_.Exception
        $rollbackError = $null
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
            }
            catch {
                $rollbackError = $_.Exception
            }
        }

        if ($null -ne $rollbackError) {
            throw [M4QualificationBlocked]::new("qualification commit failed: $($commitError.Message); rollback failed: $($rollbackError.Message)")
        }
        throw [M4QualificationBlocked]::new("qualification commit failed: $($commitError.Message)")
    }
    finally {
        if ([IO.File]::Exists($tempPath)) {
            [IO.File]::Delete($tempPath)
        }
        if ([IO.File]::Exists($backupPath)) {
            [IO.File]::Delete($backupPath)
        }
        if ([IO.File]::Exists($rollbackBackupPath)) {
            [IO.File]::Delete($rollbackBackupPath)
        }
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
        $expectedStates = @(
            'Initial', 'FormalCreateReady', 'AwaitingCrashReset', 'CrashResetObserved',
            'FormalRecovered', 'AwaitingLivePowerCut', 'LivePowerCutObserved',
            'LivePowerRecovered', 'FixturesPrepared', 'AwaitingMissingDriverReset',
            'MissingDriverResetObserved', 'MissingDriverCaptured', 'DriverRestored',
            'NegativeComplete', 'Complete'
        )
        Need (([string]::Join('|', $script:QualificationStates)) -ceq ([string]::Join('|', $expectedStates))) 'qualification state graph changed'
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
