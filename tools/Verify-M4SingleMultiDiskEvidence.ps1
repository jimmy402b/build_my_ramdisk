[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot,

    [string]$ExpectedSourceCommit = '',
    [string]$ExpectedReleaseSha256 = '',
    [string]$ExpectedDebugSha256 = ''
)

# This verifier is deliberately offline.  It reads only the supplied evidence
# tree and never starts a process, queries a device, changes a service, or
# controls a VM.  A zero exit code is reserved for a complete qualification
# evidence set; missing or contradictory evidence fails closed.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Require-Property($Object, [string]$Name, [string]$Context) {
    Require ($null -ne $Object) "$Context is null"
    Require ($null -ne $Object.PSObject.Properties[$Name]) "$Context.$Name is missing"
}

function Require-String($Value, [string]$Name) {
    Require ($Value -is [string] -and -not [string]::IsNullOrWhiteSpace([string]$Value)) "$Name is missing or empty"
}

function Require-Hash($Value, [string]$Name) {
    Require ($Value -is [string] -and [string]$Value -match '^[0-9A-Fa-f]{64}$') "$Name is not a SHA-256 hash"
}

function Require-Commit($Value, [string]$Name) {
    Require ($Value -is [string] -and [string]$Value -match '^[0-9A-Fa-f]{40}$') "$Name is not a 40-character commit"
}

function Require-Boolean($Value, [string]$Name) {
    Require ($Value -is [bool]) "$Name must be boolean"
}

function Require-PositiveInteger($Value, [string]$Name) {
    $integral = $Value -is [byte] -or $Value -is [uint16] -or $Value -is [uint32] -or
        $Value -is [uint64] -or $Value -is [int16] -or $Value -is [int32] -or
        $Value -is [int64]
    Require $integral "$Name must be an integer"
    Require ([decimal]$Value -gt 0) "$Name must be greater than zero"
}

function Get-SafePath([string]$Root, [string]$Relative, [string]$Name) {
    Require-String $Relative $Name
    Require (-not [IO.Path]::IsPathRooted($Relative)) "$Name must be relative"
    Require ($Relative -notmatch '(^|[\\/])\.\.($|[\\/])') "$Name contains a parent traversal"
    Require ($Relative -notmatch '^[A-Za-z]:') "$Name contains a drive-qualified path"
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath((Join-Path $rootFull $Relative))
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    Require ($candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) "$Name escapes evidence root"

    $cursor = $candidate
    while ($null -ne $cursor -and $cursor.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        if ([IO.File]::Exists($cursor) -or [IO.Directory]::Exists($cursor)) {
            $attributes = [IO.File]::GetAttributes($cursor)
            Require (($attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "$Name traverses a reparse point"
        }
        $parent = [IO.Directory]::GetParent($cursor)
        $cursor = if ($null -eq $parent) { $null } else { $parent.FullName }
    }
    return $candidate
}

function Read-Json([string]$Root, [string]$Relative, [string]$Name) {
    $path = Get-SafePath $Root $Relative $Name
    Require ([IO.File]::Exists($path)) "$Name is missing: $Relative"
    $encoding = [Text.UTF8Encoding]::new($false, $true)
    try { $raw = [IO.File]::ReadAllText($path, $encoding) }
    catch { throw "$Name is not valid UTF-8: $Relative" }
    Require (-not [string]::IsNullOrWhiteSpace($raw)) "$Name is empty: $Relative"
    try { $json = $raw | ConvertFrom-Json }
    catch { throw "$Name is not valid JSON: $Relative ($($_.Exception.Message))" }
    return [pscustomobject]@{ Json = $json; Path = $path }
}

function Assert-RelativeFileHash([string]$Root, $Entry, [string]$Context) {
    Require-Property $Entry 'relative_path' $Context
    Require-Property $Entry 'sha256' $Context
    $path = Get-SafePath $Root ([string]$Entry.relative_path) "$Context.relative_path"
    Require ([IO.File]::Exists($path)) "$Context file is missing: $($Entry.relative_path)"
    Require-Hash $Entry.sha256 "$Context.sha256"
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    Require ($actual -ieq [string]$Entry.sha256) "$Context hash mismatch"
}

function Assert-Case($Root, [string]$CaseName, [string]$ExpectedCommit, [string]$ExpectedRelease, [string]$ExpectedDebug) {
    $relative = "$CaseName/result.json"
    $result = (Read-Json $Root $relative "$CaseName result.json").Json
    foreach ($name in @('schema','case_id','classification','source_commit','artifacts','checkpoint_evidence','cleanup')) {
        Require-Property $result $name $CaseName
    }
    Require ([string]$result.schema -eq 'myrd.m4.vm.case-result.v1') "$CaseName schema is invalid"
    Require ([string]$result.case_id -eq $CaseName) "$CaseName case_id mismatch"
    Require ([string]$result.classification -eq 'PASS') "$CaseName classification is not PASS"
    Require-Commit $result.source_commit "$CaseName.source_commit"
    Require ([string]$result.source_commit -ieq $ExpectedCommit) "$CaseName source commit mismatch"

    foreach ($name in @('release_sha256','debug_sha256')) {
        Require-Property $result.artifacts $name "$CaseName.artifacts"
        Require-Hash $result.artifacts.$name "$CaseName.artifacts.$name"
    }
    Require ([string]$result.artifacts.release_sha256 -ieq $ExpectedRelease) "$CaseName release artifact hash mismatch"
    Require ([string]$result.artifacts.debug_sha256 -ieq $ExpectedDebug) "$CaseName debug artifact hash mismatch"

    $checkpoints = @($result.checkpoint_evidence)
    Require ($checkpoints.Count -gt 0) "$CaseName checkpoint_evidence is empty"
    $generations = @()
    foreach ($checkpoint in $checkpoints) {
        Require-Property $checkpoint 'generation' "$CaseName checkpoint"
        Require-PositiveInteger $checkpoint.generation "$CaseName checkpoint generation"
        Require ($checkpoint.generation -notin $generations) "$CaseName contains duplicate checkpoint generation"
        $generations += [uint64]$checkpoint.generation
        Require-Property $checkpoint 'files' "$CaseName checkpoint generation $($checkpoint.generation)"
        $files = @($checkpoint.files)
        Require ($files.Count -gt 0) "$CaseName checkpoint generation $($checkpoint.generation) has no file hashes"
        foreach ($file in $files) { Assert-RelativeFileHash $Root $file "$CaseName generation $($checkpoint.generation) file" }
    }

    $cleanup = $result.cleanup
    Require-Property $cleanup 'zero_residuals' "$CaseName.cleanup"
    Require-Boolean $cleanup.zero_residuals "$CaseName.cleanup.zero_residuals"
    Require ([bool]$cleanup.zero_residuals) "$CaseName cleanup reports residual resources"
    foreach ($name in @('processes','drive_letters','aim_devices','owned_resources')) {
        Require-Property $cleanup $name "$CaseName.cleanup"
        Require (@($cleanup.$name).Count -eq 0) "$CaseName cleanup.$name is not empty"
    }
    return [ordered]@{ case_id = $CaseName; generations = @($generations | Sort-Object); file_hash_count = @($checkpoints | ForEach-Object { @($_.files).Count } | Measure-Object -Sum).Sum }
}

$exitCode = 1
try {
    $root = [IO.Path]::GetFullPath($EvidenceRoot)
    Require ([IO.Directory]::Exists($root)) "EvidenceRoot does not exist: $root"
    $rootItem = Get-Item -LiteralPath $root -Force
    Require (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'EvidenceRoot cannot be a reparse point'

    $manifest = (Read-Json $root 'qualification.json' 'qualification manifest').Json
    foreach ($name in @('schema','source_commit','artifacts','cases','driver_pre_experiment')) { Require-Property $manifest $name 'qualification' }
    Require ([string]$manifest.schema -eq 'myrd.m4.vm.qualification.v1') 'qualification schema is invalid'
    Require-Commit $manifest.source_commit 'qualification.source_commit'
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSourceCommit)) {
        Require-Commit $ExpectedSourceCommit 'ExpectedSourceCommit'
        Require ([string]$manifest.source_commit -ieq $ExpectedSourceCommit) 'qualification source commit mismatch'
    }
    $commit = [string]$manifest.source_commit

    foreach ($name in @('release','debug')) {
        Require-Property $manifest.artifacts $name 'qualification.artifacts'
        Require-Property $manifest.artifacts.$name 'relative_path' "qualification.artifacts.$name"
        Require-Property $manifest.artifacts.$name 'sha256' "qualification.artifacts.$name"
        Assert-RelativeFileHash $root $manifest.artifacts.$name "qualification.artifacts.$name"
    }
    $releaseHash = [string]$manifest.artifacts.release.sha256
    $debugHash = [string]$manifest.artifacts.debug.sha256
    Require-Hash $releaseHash 'qualification release artifact'
    Require-Hash $debugHash 'qualification debug artifact'
    if (-not [string]::IsNullOrWhiteSpace($ExpectedReleaseSha256)) {
        Require-Hash $ExpectedReleaseSha256 'ExpectedReleaseSha256'
        Require ($releaseHash -ieq $ExpectedReleaseSha256) 'qualification release artifact does not match expected hash'
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedDebugSha256)) {
        Require-Hash $ExpectedDebugSha256 'ExpectedDebugSha256'
        Require ($debugHash -ieq $ExpectedDebugSha256) 'qualification debug artifact does not match expected hash'
    }

    $caseNames = @($manifest.cases | ForEach-Object { [string]$_ })
    Require ($caseNames.Count -eq 5) 'qualification must name exactly five cases'
    $missingCases = @(@('VM-1','VM-2','VM-3','VM-4','VM-5') | Where-Object { $_ -notin $caseNames })
    Require ($missingCases.Count -eq 0) 'qualification cases must be VM-1 through VM-5'
    Require (@($caseNames | Select-Object -Unique).Count -eq 5) 'qualification cases contain duplicates'

    $caseSummaries = @()
    foreach ($caseName in @('VM-1','VM-2','VM-3','VM-4','VM-5')) { $caseSummaries += Assert-Case $root $caseName $commit $releaseHash $debugHash }

    $driverRelative = [string]$manifest.driver_pre_experiment
    if ($driverRelative -match '\.json$') { $driverResultRelative = $driverRelative } else { $driverResultRelative = "$driverRelative/result.json" }
    $driver = (Read-Json $root $driverResultRelative 'driver pre-experiment').Json
    foreach ($name in @('schema','classification','source_commit','commands','adapter_query_available','recovery_discovered_adapter','restored_configuration')) { Require-Property $driver $name 'driver pre-experiment' }
    Require ([string]$driver.schema -eq 'myrd.m4.vm.driver-pre-experiment.v1') 'driver pre-experiment schema is invalid'
    Require ([string]$driver.classification -eq 'PASS') 'driver pre-experiment classification is not PASS'
    Require-Commit $driver.source_commit 'driver pre-experiment source_commit'
    Require ([string]$driver.source_commit -ieq $commit) 'driver pre-experiment source commit mismatch'
    $commands = @($driver.commands)
    Require ($commands.Count -eq 2) 'driver pre-experiment must record exactly two commands'
    Require (([string]$commands[0] -eq 'sc.exe config phdskmnt start= demand') -and ([string]$commands[1] -eq 'sc.exe start phdskmnt')) 'driver pre-experiment commands are not the approved sequence'
    foreach ($name in @('adapter_query_available','recovery_discovered_adapter','restored_configuration')) {
        Require-Boolean $driver.$name "driver pre-experiment.$name"
        Require ([bool]$driver.$name) "driver pre-experiment.$name is false"
    }

    [ordered]@{
        schema = 'myrd.m4.vm.qualification-verification.v1'
        classification = 'PASS'
        source_commit = $commit
        cases = $caseSummaries
        driver_pre_experiment = $driverResultRelative
        artifacts = [ordered]@{ release_sha256 = $releaseHash; debug_sha256 = $debugHash }
        read_only = $true
    } | ConvertTo-Json -Depth 12 | Write-Output
    Write-Output 'M4 VM EVIDENCE PASS'
    $exitCode = 0
}
catch {
    Write-Error ("M4 VM EVIDENCE FAIL: " + $_.Exception.Message)
    $exitCode = 1
}
exit $exitCode
