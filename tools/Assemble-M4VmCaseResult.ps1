[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot,

    [Parameter(Mandatory = $true)]
    [ValidateSet('VM-1', 'VM-2', 'VM-3', 'VM-4', 'VM-5')]
    [string]$CaseId,

    [Parameter(Mandatory = $true)]
    [string]$SourceCommit,

    [Parameter(Mandatory = $true)]
    [string]$ReleaseArtifact,

    [Parameter(Mandatory = $true)]
    [string]$DebugArtifact,

    # A JSON array of { generation, files:[{ relative_path, sha256 }] }, or an
    # object containing that array under checkpoint_evidence.  Paths are
    # relative to EvidenceRoot and are never copied or interpreted as data.
    [Parameter(Mandatory = $true)]
    [string]$CheckpointEvidence,

    # A JSON object containing zero_residuals, processes, drive_letters,
    # aim_devices and owned_resources.  It is intentionally operator supplied
    # from the final observer state, not inferred by this offline tool.
    [Parameter(Mandatory = $true)]
    [string]$Cleanup,

    [ValidateSet('PASS', 'PRODUCT_FAIL', 'ENVIRONMENT_BLOCKED', 'HARNESS_FAIL', 'USER_ABORTED')]
    [string]$Classification = 'PASS',

    [string]$Message = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition, [string]$Text) {
    if (-not $Condition) { throw $Text }
}

function Require-String($Value, [string]$Name) {
    Require ($Value -is [string] -and -not [string]::IsNullOrWhiteSpace([string]$Value)) "$Name is missing or empty"
}

function Require-Property($Object, [string]$Name, [string]$Context) {
    Require ($null -ne $Object) "$Context is null"
    Require ($null -ne $Object.PSObject.Properties[$Name]) "$Context.$Name is missing"
}

function Get-SafePath([string]$Root, [string]$Relative, [string]$Name) {
    Require-String $Relative $Name
    Require (-not [IO.Path]::IsPathRooted($Relative)) "$Name must be relative"
    Require ($Relative -notmatch '(^|[\\/])\.\.($|[\\/])') "$Name contains parent traversal"
    Require ($Relative -notmatch '^[A-Za-z]:') "$Name contains a drive-qualified path"
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath((Join-Path $rootFull $Relative))
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    Require ($candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) "$Name escapes evidence root"
    return $candidate
}

function Read-Json([string]$Path, [string]$Name) {
    Require ([IO.File]::Exists($Path)) "$Name is missing: $Path"
    $encoding = [Text.UTF8Encoding]::new($false, $true)
    try { $raw = [IO.File]::ReadAllText($Path, $encoding) } catch { throw "$Name is not valid UTF-8" }
    try { return ($raw | ConvertFrom-Json) } catch { throw "$Name is not valid JSON: $($_.Exception.Message)" }
}

function Require-Hash($Value, [string]$Name) {
    Require ($Value -is [string] -and [string]$Value -match '^[0-9A-Fa-f]{64}$') "$Name is not a SHA-256 hash"
}

function Write-JsonNoBom($Value, [string]$Path) {
    $text = $Value | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($Path, $text, [Text.UTF8Encoding]::new($false))
}

try {
    $root = [IO.Path]::GetFullPath($EvidenceRoot)
    Require ([IO.Directory]::Exists($root)) "EvidenceRoot does not exist: $root"
    $caseDir = Get-SafePath $root $CaseId 'CaseId'
    if (-not [IO.Directory]::Exists($caseDir)) { [IO.Directory]::CreateDirectory($caseDir) | Out-Null }

    Require ($SourceCommit -match '^[0-9A-Fa-f]{40}$') 'SourceCommit is not a 40-character commit'
    $checkpointInput = Read-Json ([IO.Path]::GetFullPath($CheckpointEvidence)) 'CheckpointEvidence'
    $checkpoints = if ($null -ne $checkpointInput.PSObject.Properties['checkpoint_evidence']) {
        @($checkpointInput.checkpoint_evidence)
    } else { @($checkpointInput) }
    Require (@($checkpoints).Length -gt 0) 'checkpoint evidence is empty'
    foreach ($checkpoint in $checkpoints) {
        Require-Property $checkpoint 'generation' 'checkpoint'
        Require ([int64]$checkpoint.generation -gt 0) 'checkpoint generation must be positive'
        Require-Property $checkpoint 'files' 'checkpoint'
        $files = @($checkpoint.files)
        Require (@($files).Length -gt 0) 'checkpoint files are empty'
        foreach ($file in $files) {
            Require-Property $file 'relative_path' 'checkpoint file'
            Require-Property $file 'sha256' 'checkpoint file'
            Require-Hash $file.sha256 'checkpoint file sha256'
            $path = Get-SafePath $root ([string]$file.relative_path) 'checkpoint file relative_path'
            Require ([IO.File]::Exists($path)) "checkpoint file is missing: $($file.relative_path)"
            $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            Require ($actual -ieq [string]$file.sha256) "checkpoint file hash mismatch: $($file.relative_path)"
        }
    }

    $cleanupObject = Read-Json ([IO.Path]::GetFullPath($Cleanup)) 'Cleanup'
    foreach ($name in @('zero_residuals', 'processes', 'drive_letters', 'aim_devices', 'owned_resources')) {
        Require-Property $cleanupObject $name 'cleanup'
    }
    Require ($cleanupObject.zero_residuals -is [bool]) 'cleanup.zero_residuals must be boolean'
    foreach ($name in @('processes', 'drive_letters', 'aim_devices', 'owned_resources')) {
        Require (@($cleanupObject.$name).Length -eq 0) "cleanup.$name must be empty for a PASS case"
    }

    $artifactDir = Get-SafePath $root 'artifacts' 'artifact directory'
    if (-not [IO.Directory]::Exists($artifactDir)) { [IO.Directory]::CreateDirectory($artifactDir) | Out-Null }
    $artifactEntries = [ordered]@{}
    foreach ($kind in @('release', 'debug')) {
        $source = if ($kind -eq 'release') { $ReleaseArtifact } else { $DebugArtifact }
        $sourceFull = [IO.Path]::GetFullPath($source)
        Require ([IO.File]::Exists($sourceFull)) "$kind artifact is missing: $sourceFull"
        $destinationRelative = "artifacts/myrd-$kind.exe"
        $destination = Get-SafePath $root $destinationRelative "$kind artifact destination"
        $sourceHash = (Get-FileHash -LiteralPath $sourceFull -Algorithm SHA256).Hash
        if ([IO.File]::Exists($destination)) {
            $existingHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
            Require ($existingHash -ieq $sourceHash) "$kind artifact destination already contains a different file"
        } else { Copy-Item -LiteralPath $sourceFull -Destination $destination }
        $artifactEntries[$kind] = [ordered]@{ relative_path = $destinationRelative; sha256 = $sourceHash }
    }

    $result = [ordered]@{
        schema = 'myrd.m4.vm.case-result.v1'
        case_id = $CaseId
        classification = $Classification
        source_commit = $SourceCommit
        artifacts = [ordered]@{
            release_sha256 = $artifactEntries.release.sha256
            debug_sha256 = $artifactEntries.debug.sha256
            release = $artifactEntries.release
            debug = $artifactEntries.debug
        }
        checkpoint_evidence = $checkpoints
        cleanup = $cleanupObject
        message = $Message
    }
    Write-JsonNoBom $result (Join-Path $caseDir 'result.json')
    Write-Output (Join-Path $caseDir 'result.json')
    exit 0
} catch {
    Write-Error ('M4 case assembly failed: ' + $_.Exception.ToString())
    exit 1
}
