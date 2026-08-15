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
            $lengthValue = $record.length
            Need (($lengthValue -is [int]) -or ($lengthValue -is [long])) "qualification kit manifest length type is invalid for $name"
            [int64]$length = $lengthValue
            Need ($length -ge 0) "qualification kit manifest length is invalid for $name"
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
    $bad = & $newRecords; $bad[0].length = [string]$bad[0].length; & $writeManifest $commit $bad; & $reject 'qualification kit manifest length type is invalid'
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

function Assert-InitialGuestFacts {
    param([Parameter(Mandatory = $true)]$Facts)
    try {
        $expected = @('admin','boot_utc','model','bios','mac','root_ntfs','service_start','service_state','service_pid','driver_start','driver_state','processes','aim_ids','R','S','physical_disks')
        $actual = Get-ExactPropertyNames -Value $Facts
        Need (($actual -join '|') -ceq ($expected -join '|')) 'guest facts property schema is invalid'
        foreach ($name in $expected) { Need ($actual -ccontains $name) "guest facts is missing $name" }
        Need ($Facts.admin -is [bool] -and $Facts.admin) 'guest administrator privilege is required'
        Need ($Facts.boot_utc -is [string]) 'guest boot_utc type is invalid'
        $bootText = [string]$Facts.boot_utc; [DateTimeOffset]$boot = [DateTimeOffset]::MinValue
        Need ($bootText -cmatch '^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d{1,7})?Z$' -and [DateTimeOffset]::TryParse($bootText, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind, [ref]$boot)) 'guest boot_utc must be UTC'
        Need ($Facts.model -is [string] -and $Facts.model -ceq 'VMware20,1') 'guest model is not VMware20,1'
        Need ($Facts.bios -is [string] -and $Facts.bios -ceq 'T_VIRTUAL') 'guest BIOS is not T_VIRTUAL'
        Need ($Facts.mac -is [string] -and $Facts.mac -ceq '00:0C:29:02:90:C1') 'guest MAC is not the contracted adapter'
        Need ($Facts.root_ntfs -is [bool] -and $Facts.root_ntfs) 'guest root volume must be NTFS'
        Need ($Facts.service_start -is [string] -and $Facts.service_start -ceq 'Disabled') 'guest service start must be Disabled'
        Need ($Facts.service_state -is [string] -and $Facts.service_state -ceq 'Stopped') 'guest service state must be Stopped'
        Need (($Facts.service_pid -is [int]) -or ($Facts.service_pid -is [long])) 'guest service pid type is invalid'
        Need ([int64]$Facts.service_pid -eq 0) 'guest service pid must be zero'
        Need ($Facts.driver_start -is [string] -and $Facts.driver_start -ceq 'DEMAND_START') 'guest driver start must be DEMAND_START'
        Need ($Facts.driver_state -is [string] -and $Facts.driver_state -ceq 'Running') 'guest driver state must be Running'
        Need ($null -ne $Facts.processes -and $Facts.processes -is [Array]) 'guest process inventory must be an array'
        foreach ($processId in @($Facts.processes)) { Need (($processId -is [int]) -or ($processId -is [long])) 'guest process inventory element type is invalid' }
        Need (@($Facts.processes).Count -eq 0) 'guest myrd process inventory must be empty'
        Need ($null -ne $Facts.aim_ids -and $Facts.aim_ids -is [Array]) 'guest AIM inventory must be an array'
        foreach ($aimId in @($Facts.aim_ids)) { Need (($aimId -is [int]) -or ($aimId -is [long])) 'guest AIM inventory element type is invalid' }
        Need (@($Facts.aim_ids).Count -eq 0) 'guest AIM inventory must be empty'
        Need ($Facts.R -is [bool] -and -not $Facts.R) 'guest R drive must be absent'
        Need ($Facts.S -is [bool] -and -not $Facts.S) 'guest S drive must be absent'
        $disks = @($Facts.physical_disks)
        $expectedDisks = @('0|68719476736|GPT|4','1|68719476736|GPT|2','2|8589934592|RAW|0')
        Need ($Facts.physical_disks -is [Array]) 'guest physical disk inventory must be an array'; foreach ($disk in $disks) { Need ($disk -is [string]) 'guest physical disk element type is invalid' }
        Need (($disks -join "`n") -ceq ($expectedDisks -join "`n")) 'guest physical disk inventory is invalid'
        return $Facts
    }
    catch [M4QualificationBlocked] { throw }
    catch { throw [M4QualificationBlocked]::new("guest facts are invalid: $($_.Exception.Message)") }
}

function ConvertTo-WindowsCommandLine {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string[]]$Arguments)
    $quoted = foreach ($argument in $Arguments) {
        if ($argument.Length -eq 0) { '""'; continue }
        if ($argument -notmatch '[\s"]') { $argument; continue }
        $builder = [Text.StringBuilder]::new(); [void]$builder.Append('"'); $slashes = 0
        foreach ($ch in $argument.ToCharArray()) {
            if ($ch -eq '\') { $slashes++; continue }
            if ($ch -eq '"') { [void]$builder.Append('\' * (($slashes * 2) + 1)); [void]$builder.Append('"'); $slashes = 0; continue }
            if ($slashes -gt 0) { [void]$builder.Append('\' * $slashes); $slashes = 0 }
            [void]$builder.Append($ch)
        }
        if ($slashes -gt 0) { [void]$builder.Append('\' * ($slashes * 2)) }
        [void]$builder.Append('"'); $builder.ToString()
    }
    return ($quoted -join ' ')
}

function Invoke-BoundedProcess {
    param([Parameter(Mandatory = $true)][string]$FilePath, [Parameter(Mandatory = $true)][AllowEmptyString()][string[]]$Arguments, [Parameter(Mandatory = $true)][int]$TimeoutMs)
    $process = $null
    try {
        Need ($TimeoutMs -gt 0) 'bounded process timeout must be a positive integer'
        $psi = [Diagnostics.ProcessStartInfo]::new(); $psi.FileName = $FilePath; $psi.Arguments = ConvertTo-WindowsCommandLine -Arguments $Arguments
        $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
        $process = [Diagnostics.Process]::new(); $process.StartInfo = $psi; $started = [DateTime]::UtcNow
        Need ($process.Start()) 'bounded process failed to start'
        $stdoutTask = $process.StandardOutput.ReadToEndAsync(); $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutMs)
        if ($timedOut -and -not $process.HasExited) {
            try { $process.Kill() }
            catch [InvalidOperationException] { if (-not $process.HasExited) { throw } }
        }
        $process.WaitForExit(); [Threading.Tasks.Task]::WaitAll(@($stdoutTask, $stderrTask))
        return [ordered]@{ pid = $process.Id; exit = $process.ExitCode; timed_out = $timedOut; stdout = $stdoutTask.Result; stderr = $stderrTask.Result; started_utc = $started.ToString('o'); ended_utc = [DateTime]::UtcNow.ToString('o') }
    }
    catch [M4QualificationBlocked] { throw }
    catch { throw [M4QualificationBlocked]::new("bounded process failed: $($_.Exception.Message)") }
    finally { if ($null -ne $process) { $process.Dispose() } }
}

function ConvertFrom-AimQueryJson {
    param([Parameter(Mandatory = $true)][string]$Json)
    try {
        $value = $Json | ConvertFrom-Json
        $expected = @('success','api_version','sub_version','ids'); $actual = Get-ExactPropertyNames -Value $value
        Need ($actual.Count -eq $expected.Count) 'AIM query JSON property count is invalid'
        foreach ($name in $expected) { Need ($actual -ccontains $name) "AIM query JSON is missing $name" }
        Need ($value.success -is [bool] -and $value.success) 'AIM query JSON success is invalid'
        foreach ($name in @('api_version','sub_version')) { Need (($value.$name -is [int]) -or ($value.$name -is [long])) "AIM query JSON $name type is invalid" }
        Need ($null -ne $value.ids -and $value.ids -is [Array]) 'AIM query JSON ids must be an array'
        foreach ($id in @($value.ids)) { Need (($id -is [int]) -or ($id -is [long])) 'AIM query JSON id type is invalid' }
        return @($value.ids)
    }
    catch [M4QualificationBlocked] { throw }
    catch { throw [M4QualificationBlocked]::new("AIM query JSON is invalid: $($_.Exception.Message)") }
}

function Select-RelevantGuestProcesses {
    param([Parameter(Mandatory = $true)][object[]]$Processes, [Parameter(Mandatory = $true)][int]$CurrentProcessId)
    $executables = @('myrd.exe','myrd_m4_aim_query.exe','MyRamDiskHost.exe')
    $scripts = @('Capture-M4Child.ps1','Invoke-M4RunLifecycle.ps1','Prepare-M4NegativeFixtures.ps1','Invoke-M4Verification.ps1','Invoke-M4GuestQualification.ps1')
    foreach ($process in $Processes) {
        if ($null -eq $process -or -not ($process.PSObject.Properties.Name -ccontains 'ProcessId') -or -not ($process.PSObject.Properties.Name -ccontains 'Name') -or -not ($process.PSObject.Properties.Name -ccontains 'CommandLine')) { continue }
        if ([int64]$process.ProcessId -eq $CurrentProcessId) { continue }
        $name = [string]$process.Name; $commandLine = [string]$process.CommandLine
        if ($executables -contains $name) { [int64]$process.ProcessId; continue }
        if (@('powershell.exe','pwsh.exe') -contains $name -and @($scripts | Where-Object { $commandLine -like "*$_*" }).Count -gt 0) { [int64]$process.ProcessId }
    }
}

function Get-GuestFacts {
    param([Parameter(Mandatory = $true)][string]$QualificationRoot, [Parameter(Mandatory = $true)][string]$Kit)
    try {
        $kitRoot = [IO.Path]::GetFullPath($Kit); $query = Join-Path $kitRoot 'myrd_m4_aim_query.exe'
        Need ([IO.File]::Exists($query)) 'guest AIM query helper is missing'
        $os = Get-CimInstance Win32_OperatingSystem; $computer = Get-CimInstance Win32_ComputerSystem
        $adapter = @(Get-CimInstance Win32_NetworkAdapterConfiguration | Where-Object { $_.MACAddress -ceq '00:0C:29:02:90:C1' })
        $queryResult = Invoke-BoundedProcess -FilePath $query -Arguments @() -TimeoutMs 30000
        Need (-not $queryResult.timed_out -and $queryResult.exit -eq 0 -and [string]::IsNullOrEmpty($queryResult.stderr)) 'guest AIM query helper failed'
        $aimIds = @(ConvertFrom-AimQueryJson -Json $queryResult.stdout)
        $fullRoot = [IO.Path]::GetFullPath($QualificationRoot); $rootInfo = [IO.DirectoryInfo]$fullRoot
        while (-not $rootInfo.Exists -and $null -ne $rootInfo.Parent) { $rootInfo = $rootInfo.Parent }; $drive = [IO.DriveInfo]::new($rootInfo.Root.FullName)
        Need ($drive.DriveType -eq [IO.DriveType]::Fixed -and ([IO.DriveInfo]::new($rootInfo.Root.FullName).DriveFormat -ceq 'NTFS')) 'qualification root must be on local fixed NTFS'
        Need ([string]$computer.Name -ceq [Environment]::MachineName) 'guest computer name and environment name disagree'
        $service = @(Get-CimInstance Win32_Service -Filter "Name='MyRamDiskService'"); $driver = @(Get-CimInstance Win32_SystemDriver -Filter "Name='phdskmnt'")
        Need ($service.Count -eq 1) 'guest service inventory is invalid'; Need ($driver.Count -eq 1) 'guest driver inventory is invalid'
        $disks = @(Get-Disk | Sort-Object Number | ForEach-Object { $partitions = if ($_.PartitionStyle -ceq 'RAW') { @() } else { @(Get-Partition -DiskNumber $_.Number) }; '{0}|{1}|{2}|{3}' -f $_.Number,$_.Size,$_.PartitionStyle,$partitions.Count })
        # Historical field name: the frozen "bios" value is ComputerSystem.Name (T_VIRTUAL), not SMBIOS text.
        [ordered]@{ admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator); boot_utc = ([Management.ManagementDateTimeConverter]::ToDateTime($os.LastBootUpTime).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffffffZ')); model = [string]$computer.Model; bios = [string]$computer.Name; mac = if ($adapter.Count -eq 1) { [string]$adapter[0].MACAddress } else { '' }; root_ntfs = $true; service_start = [string]$service[0].StartMode; service_state = [string]$service[0].State; service_pid = [int64]$service[0].ProcessId; driver_start = if ([string]$driver[0].StartMode -ceq 'Manual') { 'DEMAND_START' } else { [string]$driver[0].StartMode }; driver_state = [string]$driver[0].State; processes = @(Select-RelevantGuestProcesses -Processes @(Get-CimInstance Win32_Process) -CurrentProcessId $PID); aim_ids = $aimIds; R = [bool](Test-Path 'R:\'); S = [bool](Test-Path 'S:\'); physical_disks = $disks }
    }
    catch [M4QualificationBlocked] { throw }
    catch { throw [M4QualificationBlocked]::new("guest fact sampling failed: $($_.Exception.Message)") }
}

function Publish-QualificationRootStage {
    param([Parameter(Mandatory = $true)][string]$Stage, [Parameter(Mandatory = $true)][string]$Root)
    try {
        Need ([IO.Directory]::Exists($Stage)) 'qualification root stage is missing'
        Need (-not [IO.Directory]::Exists($Root) -and -not [IO.File]::Exists($Root)) 'qualification root already exists'
        Need (([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Stage))) -ceq ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Root)))) 'qualification root stage parent is invalid'
        [IO.Directory]::Move($Stage, $Root)
        return $Root
    }
    catch [M4QualificationBlocked] { throw }
    catch { throw [M4QualificationBlocked]::new("qualification root publish failed: $($_.Exception.Message)") }
}

function Assert-QualificationRootLayout {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$ExpectedCommit)
    Need (((@(Get-ChildItem -LiteralPath $Root -Force | ForEach-Object Name) | Sort-Object) -join '|') -ceq 'evidence|formal|kit|logs|powercut|temp') 'qualification root directory set is invalid'
    Need (((@(Get-ChildItem -LiteralPath (Join-Path $Root 'formal') -Force | ForEach-Object Name) | Sort-Object) -join '|') -ceq 'Release|tests|tools') 'qualification formal directory set is invalid'
    Assert-QualificationKit -Kit (Join-Path $Root 'kit') -ExpectedCommit $ExpectedCommit | Out-Null
    foreach ($relative in @('formal\Release\myrd.exe','formal\tests\e2e\Invoke-M4RunLifecycle.ps1','formal\tools\Invoke-M4Verification.ps1')) { Need ([IO.File]::Exists((Join-Path $Root $relative))) "qualification root missed $relative" }
}

function Initialize-QualificationRoot {
    param([Parameter(Mandatory = $true)][string]$QualificationRoot, [Parameter(Mandatory = $true)][string]$SourceKit, [Parameter(Mandatory = $true)][string]$ExpectedCommit, [Parameter(Mandatory = $true)]$Facts)
    Assert-InitialGuestFacts -Facts $Facts | Out-Null; Assert-QualificationKit -Kit $SourceKit -ExpectedCommit $ExpectedCommit | Out-Null
    Need ([IO.Path]::IsPathRooted($QualificationRoot)) 'qualification root must be absolute'
    $root = [IO.Path]::GetFullPath($QualificationRoot); $parent = [IO.Path]::GetDirectoryName($root)
    Need ([IO.Directory]::Exists($parent)) 'qualification root parent is missing'
    Need (-not [IO.Directory]::Exists($root) -and -not [IO.File]::Exists($root)) 'qualification root already exists'
    $stage = Join-Path $parent ('.' + [IO.Path]::GetFileName($root) + '.stage-' + [Guid]::NewGuid().ToString('N'))
    Need (-not [IO.Directory]::Exists($stage) -and -not [IO.File]::Exists($stage)) 'qualification root stage already exists'
    $stageCreated = $false; $published = $false
    try {
        [IO.Directory]::CreateDirectory($stage) | Out-Null; $stageCreated = $true
        foreach ($relative in @('formal\Release','formal\tests\e2e','formal\tools','powercut','evidence','logs','temp','kit')) { [IO.Directory]::CreateDirectory((Join-Path $stage $relative)) | Out-Null }
        foreach ($name in @('Capture-M4Child.ps1','Invoke-M4GuestQualification.ps1','Invoke-M4RunLifecycle.ps1','Invoke-M4Verification.ps1','Prepare-M4NegativeFixtures.ps1','myrd.exe','myrd_m4_aim_query.exe','qualification-kit.json')) { [IO.File]::Copy((Join-Path $SourceKit $name), (Join-Path $stage ('kit\' + $name)), $false) }
        Assert-QualificationKit -Kit (Join-Path $stage 'kit') -ExpectedCommit $ExpectedCommit | Out-Null
        [IO.File]::Copy((Join-Path $stage 'kit\myrd.exe'), (Join-Path $stage 'formal\Release\myrd.exe'), $false)
        [IO.File]::Copy((Join-Path $stage 'kit\Invoke-M4RunLifecycle.ps1'), (Join-Path $stage 'formal\tests\e2e\Invoke-M4RunLifecycle.ps1'), $false)
        [IO.File]::Copy((Join-Path $stage 'kit\Invoke-M4Verification.ps1'), (Join-Path $stage 'formal\tools\Invoke-M4Verification.ps1'), $false)
        Publish-QualificationRootStage -Stage $stage -Root $root | Out-Null; $stageCreated = $false; $published = $true
        Assert-QualificationRootLayout -Root $root -ExpectedCommit $ExpectedCommit
        return $root
    }
    catch {
        $primary = $_.Exception; $cleanup = $null
        $ownedPath = if ($published) { $root } elseif ($stageCreated) { $stage } else { $null }
        if ($null -ne $ownedPath -and [IO.Directory]::Exists($ownedPath)) {
            try { Remove-Item -LiteralPath $ownedPath -Recurse -ErrorAction Stop }
            catch { $cleanup = $_.Exception }
        }
        $message = "qualification root initialization failed: $($primary.Message)"
        if ($null -ne $cleanup) { $message += "; cleanup failed: $($cleanup.Message)" }
        throw [M4QualificationBlocked]::new($message)
    }
}

function Assert-UnexpectedReset {
    param([Parameter(Mandatory = $true)]$Armed, [Parameter(Mandatory = $true)]$Post)
    try {
        Need (((Get-ExactPropertyNames -Value $Armed) -join '|') -ceq 'boot_utc|armed_utc') 'armed reset facts schema is invalid'
        Need (((Get-ExactPropertyNames -Value $Post) -join '|') -ceq 'boot_utc|system_events') 'post reset facts schema is invalid'
        [DateTimeOffset]$before = [DateTimeOffset]::MinValue; [DateTimeOffset]$armedAt = [DateTimeOffset]::MinValue; [DateTimeOffset]$after = [DateTimeOffset]::MinValue
        Need ($Armed.boot_utc -is [string] -and $Armed.armed_utc -is [string] -and $Post.boot_utc -is [string]) 'reset boot time type is invalid'
        foreach ($entry in @(@([string]$Armed.boot_utc,[ref]$before),@([string]$Armed.armed_utc,[ref]$armedAt),@([string]$Post.boot_utc,[ref]$after))) { Need ($entry[0] -cmatch 'Z$' -and [DateTimeOffset]::TryParse($entry[0], [ref]$entry[1])) 'reset time must be UTC' }
        Need ($after -gt $before) 'reset boot time did not advance'
        Need ($null -ne $Post.system_events -and $Post.system_events -is [Array]) 'post reset events type is invalid'
        $events = @($Post.system_events); foreach ($event in $events) { $p=Get-ExactPropertyNames $event; Need (($p -join '|') -ceq 'id|time_utc') 'reset event schema is invalid'; Need (($event.id -is [int]) -or ($event.id -is [long])) 'reset event id type is invalid'; Need ($event.time_utc -is [string]) 'reset event time type is invalid'; Need (($event.id -eq 41) -or ($event.id -eq 6008)) 'reset event id is invalid'; [DateTimeOffset]$when=[DateTimeOffset]::MinValue; Need (([string]$event.time_utc -cmatch '^\d{4}-\d\d-\d\dT.*Z$') -and [DateTimeOffset]::TryParse([string]$event.time_utc,[ref]$when) -and $when -gt $armedAt) "reset event $($event.id) time is not after armed_utc" }
        Need (@($events | Where-Object { [string]$_.id -ceq '41' }).Count -ge 1) 'reset event 41 is missing'; Need (@($events | Where-Object { [string]$_.id -ceq '6008' }).Count -ge 1) 'reset event 6008 is missing'
    }
    catch [M4QualificationBlocked] { throw }
    catch { throw [M4QualificationBlocked]::new("reset facts are invalid: $($_.Exception.Message)") }
}

function Invoke-SelfTest {
    $scratch = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-guest-qualification-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($scratch) | Out-Null
    try {
        $goodFacts = [ordered]@{
            admin = $true; boot_utc = '2026-08-16T00:00:00Z'; model = 'VMware20,1'; bios = 'T_VIRTUAL'
            mac = '00:0C:29:02:90:C1'; root_ntfs = $true; service_start = 'Disabled'; service_state = 'Stopped'
            service_pid = 0; driver_start = 'DEMAND_START'; driver_state = 'Running'; processes = @(); aim_ids = @()
            R = $false; S = $false; physical_disks = @('0|68719476736|GPT|4', '1|68719476736|GPT|2', '2|8589934592|RAW|0')
        }
        Assert-InitialGuestFacts -Facts $goodFacts | Out-Null
        $copyFacts = {
            $copy = [ordered]@{}
            foreach ($property in $goodFacts.GetEnumerator()) { $copy[$property.Key] = $property.Value }
            return $copy
        }
        $badFacts = @(
            @('admin', $false, 'administrator privilege'), @('model', 'other', 'model'), @('bios', 'other', 'BIOS'), @('mac', '00:00:00:00:00:00', 'MAC'),
            @('root_ntfs', $false, 'root volume'), @('service_start', 'Auto', 'service start'), @('service_state', 'Running', 'service state'), @('service_pid', 4, 'service pid'),
            @('driver_start', 'Auto', 'driver start'), @('driver_state', 'Stopped', 'driver state'), @('processes', @(1), 'process inventory'), @('processes', $null, 'process inventory'), @('aim_ids', @('x'), 'AIM inventory'), @('aim_ids', $null, 'AIM inventory'),
            @('R', $true, 'R drive'), @('S', $true, 'S drive'), @('physical_disks', @('0|1|RAW|0'), 'physical disk'), @('boot_utc', 'not-a-time', 'boot_utc'), @('boot_utc', [DateTime]::UtcNow, 'boot_utc type')
        )
        foreach ($mutation in $badFacts) {
            $bad = & $copyFacts; $bad[$mutation[0]] = $mutation[1]; $blocked = $false; $message = ''
            try { Assert-InitialGuestFacts -Facts $bad | Out-Null } catch [M4QualificationBlocked] { $blocked = $true; $message = $_.Exception.Message }
            Need $blocked "guest fact $($mutation[0]) was not blocked by type"
            Need ($message -clike "*$($mutation[2])*") "guest fact $($mutation[0]) used the wrong reason"
        }
        $aimResult = ConvertFrom-AimQueryJson -Json '{"success":true,"api_version":1,"sub_version":2,"ids":[]}'
        Need (@($aimResult).Count -eq 0) 'AIM parser did not preserve an empty ids array'
        foreach ($badAim in @('{"success":true,"api_version":1,"sub_version":2}', '{"success":"true","api_version":1,"sub_version":2,"ids":[]}', '{"success":true,"api_version":"1","sub_version":2,"ids":[]}', '{"success":true,"api_version":1,"sub_version":2,"ids":["x"]}', '{"success":true,"api_version":1,"sub_version":2,"ids":null}')) {
            $blocked=$false; try { ConvertFrom-AimQueryJson -Json $badAim | Out-Null } catch [M4QualificationBlocked] { $blocked=$true }; Need $blocked 'invalid AIM query JSON was not blocked by type'
        }
        $syntheticProcesses = @(
            [pscustomobject]@{ ProcessId = 100; Name = 'powershell.exe'; CommandLine = 'powershell -File Capture-M4Child.ps1' },
            [pscustomobject]@{ ProcessId = 101; Name = 'pwsh.exe'; CommandLine = 'pwsh Invoke-M4RunLifecycle.ps1' },
            [pscustomobject]@{ ProcessId = 102; Name = 'powershell.exe'; CommandLine = 'powershell Prepare-M4NegativeFixtures.ps1' },
            [pscustomobject]@{ ProcessId = 103; Name = 'pwsh.exe'; CommandLine = 'pwsh Invoke-M4Verification.ps1' },
            [pscustomobject]@{ ProcessId = 104; Name = 'myrd.exe'; CommandLine = '' },
            [pscustomobject]@{ ProcessId = 105; Name = 'myrd_m4_aim_query.exe'; CommandLine = '' },
            [pscustomobject]@{ ProcessId = 106; Name = 'MyRamDiskHost.exe'; CommandLine = '' },
            [pscustomobject]@{ ProcessId = 107; Name = 'powershell.exe'; CommandLine = 'powershell -File unrelated.ps1' },
            [pscustomobject]@{ ProcessId = 108; Name = 'powershell.exe'; CommandLine = 'powershell -File Invoke-M4GuestQualification.ps1' },
            [pscustomobject]@{ ProcessId = $PID; Name = 'powershell.exe'; CommandLine = 'powershell -File Invoke-M4GuestQualification.ps1' }
        )
        Need (((Select-RelevantGuestProcesses -Processes $syntheticProcesses -CurrentProcessId $PID) -join '|') -ceq '100|101|102|103|104|105|106|108') 'guest process classifier is invalid'
        $child = Join-Path $scratch 'bounded-child.ps1'
        [IO.File]::WriteAllText($child, "param([Parameter(ValueFromRemainingArguments=`$true)][string[]]`$Rest)`n[Console]::Out.WriteLine((`$Rest -join '|'))`n[Console]::Error.WriteLine('stderr-line')`nexit 23", [Text.UTF8Encoding]::new($false))
        $argv = @('-NoProfile','-File',$child,'','two words','two words\','C:\tail\','slash\"quote','say"quote')
        $processResult = Invoke-BoundedProcess -FilePath (Join-Path $PSHOME 'powershell.exe') -Arguments $argv -TimeoutMs 30000
        Need ($processResult.exit -eq 23 -and -not $processResult.timed_out) 'bounded child exit result is invalid'
        Need ($processResult.stdout.Trim() -ceq '|two words|two words\|C:\tail\|slash\"quote|say"quote') 'bounded child argv round-trip is invalid'
        Need ($processResult.stderr.Trim() -ceq 'stderr-line') 'bounded child stderr capture is invalid'
        $timeoutResult = Invoke-BoundedProcess -FilePath (Join-Path $PSHOME 'powershell.exe') -Arguments @('-NoProfile','-Command','Start-Sleep -Seconds 5') -TimeoutMs 20
        Need $timeoutResult.timed_out 'bounded child timeout was not reported'
        Need ($null -eq (Get-Process -Id $timeoutResult.pid -ErrorAction SilentlyContinue)) 'bounded timeout process is still present'
        for ($attempt = 0; $attempt -lt 20; $attempt++) {
            $raceResult = Invoke-BoundedProcess -FilePath (Join-Path $PSHOME 'powershell.exe') -Arguments @('-NoProfile','-Command','') -TimeoutMs 1
            Need ((@($raceResult.Keys) -join '|') -ceq 'pid|exit|timed_out|stdout|stderr|started_utc|ended_utc') 'bounded process race result schema is invalid'
        }
        $armed = [ordered]@{ boot_utc = '2026-08-16T00:00:00Z'; armed_utc = '2026-08-16T00:01:00Z' }
        $post = [ordered]@{ boot_utc = '2026-08-16T00:02:00Z'; system_events = @([ordered]@{ id = 41; time_utc = '2026-08-16T00:01:01Z' }, [ordered]@{ id = 6008; time_utc = '2026-08-16T00:01:02Z' }) }
        Assert-UnexpectedReset -Armed $armed -Post $post | Out-Null
        $badResets = @(
            @([ordered]@{ boot_utc = '2026-08-16T00:02:00Z'; system_events = @([ordered]@{ id = 6008; time_utc = '2026-08-16T00:01:02Z' }) }, 'event 41 is missing'),
            @([ordered]@{ boot_utc = '2026-08-16T00:02:00Z'; system_events = @([ordered]@{ id = 41; time_utc = '2026-08-16T00:01:01Z' }) }, 'event 6008 is missing'),
            @([ordered]@{ boot_utc = '2026-08-16T00:00:00Z'; system_events = $post.system_events }, 'boot time did not advance'),
            @([ordered]@{ boot_utc = '2026-08-16T00:02:00Z'; system_events = @([ordered]@{ id = 41; time_utc = '2026-08-16T00:01:00Z' }, [ordered]@{ id = 6008; time_utc = '2026-08-16T00:01:02Z' }) }, 'event 41 time'),
            @([ordered]@{ boot_utc = '2026-08-16T00:02:00Z'; system_events = @([ordered]@{ id = 41; time_utc = '2026-08-16T00:01:01Z' }, [ordered]@{ id = 6008; time_utc = '2026-08-16T00:01:00Z' }) }, 'event 6008 time'),
            @([ordered]@{ boot_utc = '2026-08-16T00:02:00Z'; system_events = @([ordered]@{ id = '41'; time_utc = '2026-08-16T00:01:01Z' }, [ordered]@{ id = 6008; time_utc = '2026-08-16T00:01:02Z' }) }, 'id type'),
            @([ordered]@{ boot_utc = '2026-08-16T00:02:00Z'; system_events = @([ordered]@{ id = 41; time_utc = [DateTime]::UtcNow }, [ordered]@{ id = 6008; time_utc = '2026-08-16T00:01:02Z' }) }, 'time type'),
            @([ordered]@{ boot_utc = '2026-08-16T00:02:00Z'; system_events = @([ordered]@{ id = 41; utc = '2026-08-16T00:01:01Z' }, [ordered]@{ id = 6008; time_utc = '2026-08-16T00:01:02Z' }) }, 'event schema')
        )
        foreach ($case in $badResets) { $blocked=$false; $message=''; try { Assert-UnexpectedReset -Armed $armed -Post $case[0] } catch [M4QualificationBlocked] { $blocked=$true; $message=$_.Exception.Message }; Need ($blocked -and $message -clike "*$($case[1])*") 'invalid unexpected reset used the wrong reason' }
        $badArmed = [ordered]@{ boot_utc = [DateTime]::UtcNow; armed_utc = '2026-08-16T00:01:00Z' }; $blocked=$false; $message=''
        try { Assert-UnexpectedReset -Armed $badArmed -Post $post } catch [M4QualificationBlocked] { $blocked=$true; $message=$_.Exception.Message }
        Need ($blocked -and $message -clike '*boot time type*') 'non-string armed reset time used the wrong reason'
        $sourceKit = Join-Path $scratch 'source-kit'; [IO.Directory]::CreateDirectory($sourceKit) | Out-Null
        $kitNames = @('Capture-M4Child.ps1','Invoke-M4GuestQualification.ps1','Invoke-M4RunLifecycle.ps1','Invoke-M4Verification.ps1','Prepare-M4NegativeFixtures.ps1','myrd.exe','myrd_m4_aim_query.exe')
        foreach ($name in $kitNames) { [IO.File]::WriteAllText((Join-Path $sourceKit $name), $name, [Text.UTF8Encoding]::new($false)) }
        $kitCommit = 'c' * 40
        $kitManifest = [ordered]@{ schema = 'myrd.m4.qualification-kit.v1'; source_commit = $kitCommit; files = @($kitNames | ForEach-Object { $p=Join-Path $sourceKit $_; [ordered]@{name=$_;length=[int64]([IO.FileInfo]$p).Length;sha256=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToUpperInvariant()} }) }
        [IO.File]::WriteAllText((Join-Path $sourceKit 'qualification-kit.json'), ($kitManifest | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
        $publishStage = Join-Path $scratch 'publish-stage'; $publishRoot = Join-Path $scratch 'publish-root'; [IO.Directory]::CreateDirectory($publishStage) | Out-Null
        Publish-QualificationRootStage -Stage $publishStage -Root $publishRoot | Out-Null
        Need ([IO.Directory]::Exists($publishRoot) -and -not [IO.Directory]::Exists($publishStage)) 'qualification root stage publish did not move atomically'
        $collisionStage = Join-Path $scratch 'publish-collision-stage'; $collisionRoot = Join-Path $scratch 'publish-collision-root'
        [IO.Directory]::CreateDirectory($collisionStage) | Out-Null; [IO.File]::WriteAllText((Join-Path $collisionStage 'stage-sentinel'), 'stage')
        [IO.Directory]::CreateDirectory($collisionRoot) | Out-Null; $collisionSentinel = Join-Path $collisionRoot 'root-sentinel'; [IO.File]::WriteAllText($collisionSentinel, 'root')
        $beforeCollision = (Get-FileHash -LiteralPath $collisionSentinel -Algorithm SHA256).Hash; $blocked=$false
        try { Publish-QualificationRootStage -Stage $collisionStage -Root $collisionRoot | Out-Null } catch [M4QualificationBlocked] { $blocked=$true }
        Need ($blocked -and [IO.Directory]::Exists($collisionStage)) 'competition publish did not preserve the caller stage'
        Need ((Get-FileHash -LiteralPath $collisionSentinel -Algorithm SHA256).Hash -ceq $beforeCollision) 'competition publish changed the root'
        Remove-Item -LiteralPath $collisionStage -Recurse -ErrorAction Stop
        $initialRoot = Join-Path $scratch 'qualification-root'; $bad=$false; $invalidFacts=& $copyFacts; $invalidFacts.admin=$false
        try { Initialize-QualificationRoot -QualificationRoot $initialRoot -SourceKit $sourceKit -ExpectedCommit $kitCommit -Facts $invalidFacts | Out-Null } catch [M4QualificationBlocked] { $bad=$true }
        Need ($bad -and -not [IO.Directory]::Exists($initialRoot)) 'bad facts created a qualification root'
        $bad=$false; try { Initialize-QualificationRoot -QualificationRoot $initialRoot -SourceKit $sourceKit -ExpectedCommit ('d' * 40) -Facts $goodFacts | Out-Null } catch [M4QualificationBlocked] { $bad=$true }
        Need ($bad -and -not [IO.Directory]::Exists($initialRoot)) 'bad kit created a qualification root'
        [IO.Directory]::CreateDirectory($initialRoot) | Out-Null; [IO.File]::WriteAllText((Join-Path $initialRoot 'sentinel'), 'keep')
        $bad=$false; try { Initialize-QualificationRoot -QualificationRoot $initialRoot -SourceKit $sourceKit -ExpectedCommit $kitCommit -Facts $goodFacts | Out-Null } catch [M4QualificationBlocked] { $bad=$true }
        Need ($bad -and ([IO.File]::ReadAllText((Join-Path $initialRoot 'sentinel')) -ceq 'keep')) 'qualification root collision changed existing root'
        Remove-Item -LiteralPath $initialRoot -Recurse -Force
        Initialize-QualificationRoot -QualificationRoot $initialRoot -SourceKit $sourceKit -ExpectedCommit $kitCommit -Facts $goodFacts | Out-Null
        Need (((@(Get-ChildItem -LiteralPath $initialRoot -Force | ForEach-Object Name) -join '|')) -ceq 'evidence|formal|kit|logs|powercut|temp') 'qualification root directory set is invalid'
        Need (((@(Get-ChildItem -LiteralPath (Join-Path $initialRoot 'formal') -Force | ForEach-Object Name) -join '|')) -ceq 'Release|tests|tools') 'qualification formal directory set is invalid'
        Need (((@(Get-ChildItem -LiteralPath (Join-Path $initialRoot 'kit') -Force | ForEach-Object Name) | Sort-Object) -join '|') -ceq ((@('Capture-M4Child.ps1','Invoke-M4GuestQualification.ps1','Invoke-M4RunLifecycle.ps1','Invoke-M4Verification.ps1','Prepare-M4NegativeFixtures.ps1','myrd.exe','myrd_m4_aim_query.exe','qualification-kit.json') | Sort-Object) -join '|')) 'qualification kit snapshot set is invalid'
        foreach ($relative in @('formal\Release\myrd.exe','formal\tests\e2e\Invoke-M4RunLifecycle.ps1','formal\tools\Invoke-M4Verification.ps1','kit\qualification-kit.json')) { Need ([IO.File]::Exists((Join-Path $initialRoot $relative))) "qualification root missed $relative" }
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
