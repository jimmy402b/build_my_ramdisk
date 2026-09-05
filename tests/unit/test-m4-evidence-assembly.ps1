[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Assembler,
    [string]$Verifier = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Write-Json($Value, [string]$Path) { $Value | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding UTF8 }
function Invoke-Assembly([hashtable]$Arguments) {
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Assembler)
    foreach ($key in $Arguments.Keys) { $argv += @('-' + $key, [string]$Arguments[$key]) }
    $stdoutPath = Join-Path ([IO.Path]::GetTempPath()) ('asm-out-' + [Guid]::NewGuid().ToString('N'))
    $stderrPath = Join-Path ([IO.Path]::GetTempPath()) ('asm-err-' + [Guid]::NewGuid().ToString('N'))
    try {
        $process = Start-Process -FilePath powershell.exe -ArgumentList $argv -Wait -PassThru -NoNewWindow -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        [pscustomobject]@{ Code = $process.ExitCode; Output = ((Get-Content -Raw $stdoutPath -ErrorAction SilentlyContinue) + (Get-Content -Raw $stderrPath -ErrorAction SilentlyContinue)) }
    } finally {
        Remove-Item -LiteralPath $stdoutPath,$stderrPath -Force -ErrorAction SilentlyContinue
    }
}
function Copy-Arguments([hashtable]$Base, [hashtable]$Overrides) {
    $copy = @{}; foreach ($key in $Base.Keys) { $copy[$key] = $Base[$key] }
    foreach ($key in $Overrides.Keys) { $copy[$key] = $Overrides[$key] }
    return $copy
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-assembly-' + [Guid]::NewGuid().ToString('N'))
try {
    $evidence = New-Item -ItemType Directory -Path $root -Force
    $artifactSource = New-Item -ItemType Directory -Path (Join-Path $root 'sources')
    $release = Join-Path $artifactSource.FullName 'release.exe'
    $debug = Join-Path $artifactSource.FullName 'debug.exe'
    [IO.File]::WriteAllBytes($release, [byte[]](1, 2, 3))
    [IO.File]::WriteAllBytes($debug, [byte[]](4, 5, 6))
    $capture = New-Item -ItemType Directory -Path (Join-Path $root 'captures')
    $marker = Join-Path $capture.FullName 'marker.txt'
    Set-Content -LiteralPath $marker -Value 'committed' -Encoding UTF8
    $markerHash = (Get-FileHash -LiteralPath $marker -Algorithm SHA256).Hash
    $checkpoint = Join-Path $root 'checkpoint.json'
    Write-Json @([ordered]@{ generation = 4; files = @([ordered]@{ relative_path = 'captures/marker.txt'; sha256 = $markerHash }) }) $checkpoint
    $cleanup = Join-Path $root 'cleanup.json'
    Write-Json ([ordered]@{ zero_residuals = $true; processes = @(); drive_letters = @(); aim_devices = @(); owned_resources = @() }) $cleanup

    $args = @{
        EvidenceRoot = $root; CaseId = 'VM-1'; SourceCommit = ('a' * 40)
        ReleaseArtifact = $release; DebugArtifact = $debug
        CheckpointEvidence = $checkpoint; Cleanup = $cleanup
    }
    $valid = Invoke-Assembly $args
    Require ($valid.Code -eq 0) "valid assembly failed: $($valid.Output)"
    $resultPath = Join-Path $root 'VM-1/result.json'
    Require ([IO.File]::Exists($resultPath)) 'canonical result was not written'
    $result = Get-Content -Raw -LiteralPath $resultPath | ConvertFrom-Json
    Require ([string]$result.schema -eq 'myrd.m4.vm.case-result.v1') 'wrong canonical schema'
    Require ([string]$result.case_id -eq 'VM-1') 'wrong case id'
    Require ($null -ne $result.artifacts.release_sha256 -and $null -ne $result.artifacts.debug_sha256) 'artifact hashes missing'
    Require ($null -ne $result.artifacts.release.relative_path -and $null -ne $result.artifacts.debug.relative_path) 'artifact references missing'

    if (-not [string]::IsNullOrWhiteSpace($Verifier)) {
        Require ([IO.File]::Exists($Verifier)) "verifier is missing: $Verifier"
        foreach ($caseName in @('VM-2', 'VM-3', 'VM-4', 'VM-5')) {
            $caseRun = Invoke-Assembly (Copy-Arguments $args @{ CaseId = $caseName })
            Require ($caseRun.Code -eq 0) "assembly failed for $caseName`: $($caseRun.Output)"
        }
        $releaseHash = (Get-FileHash -LiteralPath (Join-Path $root 'artifacts/myrd-release.exe') -Algorithm SHA256).Hash
        $debugHash = (Get-FileHash -LiteralPath (Join-Path $root 'artifacts/myrd-debug.exe') -Algorithm SHA256).Hash
        Write-Json ([ordered]@{
            schema = 'myrd.m4.vm.qualification.v1'; source_commit = ('a' * 40)
            artifacts = [ordered]@{
                release = [ordered]@{ relative_path = 'artifacts/myrd-release.exe'; sha256 = $releaseHash }
                debug = [ordered]@{ relative_path = 'artifacts/myrd-debug.exe'; sha256 = $debugHash }
            }
            cases = @('VM-1', 'VM-2', 'VM-3', 'VM-4', 'VM-5')
            driver_pre_experiment = 'driver-pre-experiment/result.json'
        }) (Join-Path $root 'qualification.json')
        $driverDir = New-Item -ItemType Directory -Path (Join-Path $root 'driver-pre-experiment') -Force
        Write-Json ([ordered]@{
            schema = 'myrd.m4.vm.driver-pre-experiment.v1'; classification = 'PASS'; source_commit = ('a' * 40)
            commands = @('sc.exe config phdskmnt start= demand', 'sc.exe start phdskmnt')
            adapter_query_available = $true; recovery_discovered_adapter = $true; restored_configuration = $true
        }) (Join-Path $driverDir.FullName 'result.json')
        $verify = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Verifier -EvidenceRoot $root -ExpectedSourceCommit ('a' * 40) -ExpectedReleaseSha256 $releaseHash -ExpectedDebugSha256 $debugHash 2>&1)
        Require ($LASTEXITCODE -eq 0) "verifier rejected assembled cases: $($verify -join [Environment]::NewLine)"
        Require (($verify -join [Environment]::NewLine) -match 'M4 VM EVIDENCE PASS') 'verifier PASS marker missing'
    }

    $missing = Join-Path $root 'missing-cleanup.json'
    Write-Json ([ordered]@{ zero_residuals = $true; processes = @(); drive_letters = @(); aim_devices = @() }) $missing
    $bad = Invoke-Assembly (Copy-Arguments $args @{ CaseId = 'VM-2'; Cleanup = $missing })
    Require ($bad.Code -ne 0) 'missing cleanup field was accepted'
    Require ($bad.Output -match 'owned_resources') "missing field was not named: $($bad.Output)"

    $badCheckpoint = Join-Path $root 'bad-checkpoint.json'
    Write-Json @([ordered]@{ generation = 4; files = @([ordered]@{ relative_path = 'captures/marker.txt' }) }) $badCheckpoint
    $bad2 = Invoke-Assembly (Copy-Arguments $args @{ CaseId = 'VM-3'; CheckpointEvidence = $badCheckpoint })
    Require ($bad2.Code -ne 0) 'missing checkpoint hash was accepted'
    Require ($bad2.Output -match 'sha256') "missing hash was not named: $($bad2.Output)"
}
finally {
    if ([IO.Directory]::Exists($root)) { Remove-Item -LiteralPath $root -Recurse -Force }
}

Write-Output 'M4 evidence assembly tests PASS'
