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

function M4QualificationBlocked {
    param([Parameter(Mandatory = $true)][string]$Message)
    throw "M4QualificationBlocked: $Message"
}

function Need {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
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
        Need ($actual -contains $name) "qualification state is missing $name"
    }
    Need ([string]$State.schema -ceq 'myrd.m4.guest-qualification.v1') 'qualification state schema is invalid'
    Need (-not [string]::IsNullOrWhiteSpace([string]$State.qualification_id)) 'qualification state id is invalid'
    Need ($script:QualificationStates -contains [string]$State.state) 'qualification state value is invalid'
    $sequenceText = [string]$State.sequence
    Need ($sequenceText -match '^[0-9]+$') 'qualification state sequence is invalid'
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
    }
    catch {
        throw "qualification state file is invalid: $($_.Exception.Message)"
    }
    return (Assert-QualificationStateSchema -State $state)
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
    $tempPath = Join-Path $root ('.state-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $backupPath = Join-Path $root ('.state-' + [Guid]::NewGuid().ToString('N') + '.bak')
    try {
        $json = $State | ConvertTo-Json -Compress -Depth 4
        Write-DurableUtf8 -Path $tempPath -Content ($json + [Environment]::NewLine)
        Read-QualificationStateFile -Path $tempPath | Out-Null
        if ([IO.File]::Exists($statePath)) {
            [IO.File]::Replace($tempPath, $statePath, $backupPath)
        }
        else {
            [IO.File]::Move($tempPath, $statePath)
        }
        Append-QualificationEvent -QualificationRoot $root -State $State
    }
    finally {
        if ([IO.File]::Exists($tempPath)) {
            [IO.File]::Delete($tempPath)
        }
        if ([IO.File]::Exists($backupPath)) {
            [IO.File]::Delete($backupPath)
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
        M4QualificationBlocked "invalid transition $From -> $To"
    }
}

function Invoke-SelfTest {
    $scratch = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-guest-qualification-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($scratch) | Out-Null
    try {
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
        $events = @(
            Get-Content -LiteralPath (Join-Path $scratch 'events.jsonl') -Encoding UTF8 |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
                ForEach-Object { $_ | ConvertFrom-Json }
        )
        Need ($events.Count -eq 2) 'event sequence count is invalid'
        Need ([int]$events[0].sequence -eq 0 -and [int]$events[1].sequence -eq 1) 'event sequence order is invalid'
        $blocked = $false
        try { Assert-Transition -From 'Initial' -To 'Complete' } catch { $blocked = $_.Exception.Message -like 'M4QualificationBlocked:*' }
        Need $blocked 'Initial -> Complete must be blocked'
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

M4QualificationBlocked 'Run and Status are not implemented in Task 1'
