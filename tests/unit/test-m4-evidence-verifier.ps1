[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Verifier
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Write-JsonFile($Value, [string]$Path) {
    $Value | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Invoke-Verifier([string]$Root, [string]$ExpectedCommit, [string]$ReleaseHash, [string]$DebugHash) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Verifier `
            -EvidenceRoot $Root -ExpectedSourceCommit $ExpectedCommit `
            -ExpectedReleaseSha256 $ReleaseHash -ExpectedDebugSha256 $DebugHash 2>&1)
        $code = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $saved }
    [pscustomobject]@{ Output = ($output -join [Environment]::NewLine); Code = $code }
}

function Expect-Failure($Root, [string]$Expected, [string]$Commit, [string]$ReleaseHash, [string]$DebugHash) {
    $run = Invoke-Verifier $Root $Commit $ReleaseHash $DebugHash
    Require ($run.Code -ne 0) "invalid fixture passed: $Expected"
    Require ($run.Output -match [regex]::Escape($Expected)) `
        "failure did not name '$Expected': $($run.Output)"
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-evidence-' + [Guid]::NewGuid().ToString('N'))
$commit = '0123456789abcdef0123456789abcdef01234567'
try {
    $verifierText = Get-Content -Raw -LiteralPath $Verifier
    foreach ($forbidden in @('Start-Process', 'Stop-Process', 'Restart-Computer', 'Stop-Computer', 'pnputil', 'Get-Disk', 'Get-Volume', 'Get-CimInstance')) {
        Require ($verifierText -notmatch [regex]::Escape($forbidden)) "verifier is not read-only; found $forbidden"
    }
    Require ($verifierText -match 'read_only\s*=\s*\$true') 'verifier does not declare read-only result'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $artifactDir = New-Item -ItemType Directory -Path (Join-Path $root 'artifacts')
    $captureDir = New-Item -ItemType Directory -Path (Join-Path $root 'captures')
    $releasePath = Join-Path $artifactDir.FullName 'myrd-release.exe'
    $debugPath = Join-Path $artifactDir.FullName 'myrd-debug.exe'
    [IO.File]::WriteAllBytes($releasePath, [byte[]](1, 2, 3, 4))
    [IO.File]::WriteAllBytes($debugPath, [byte[]](5, 6, 7, 8))
    $releaseHash = (Get-FileHash -LiteralPath $releasePath -Algorithm SHA256).Hash
    $debugHash = (Get-FileHash -LiteralPath $debugPath -Algorithm SHA256).Hash
    $markerPath = Join-Path $captureDir.FullName 'checkpoint-marker.txt'
    Set-Content -LiteralPath $markerPath -Value 'committed marker' -Encoding UTF8
    $markerHash = (Get-FileHash -LiteralPath $markerPath -Algorithm SHA256).Hash

    $manifest = [ordered]@{
        schema = 'myrd.m4.vm.qualification.v1'
        source_commit = $commit
        artifacts = [ordered]@{
            release = [ordered]@{ relative_path = 'artifacts/myrd-release.exe'; sha256 = $releaseHash }
            debug = [ordered]@{ relative_path = 'artifacts/myrd-debug.exe'; sha256 = $debugHash }
        }
        cases = @('VM-1', 'VM-2', 'VM-3', 'VM-4', 'VM-5')
        driver_pre_experiment = 'driver-pre-experiment/result.json'
    }
    Write-JsonFile $manifest (Join-Path $root 'qualification.json')

    $checkpoint = @([ordered]@{
        generation = 7
        files = @([ordered]@{ relative_path = 'captures/checkpoint-marker.txt'; sha256 = $markerHash })
    })
    foreach ($caseName in $manifest.cases) {
        $caseDir = New-Item -ItemType Directory -Path (Join-Path $root $caseName)
        Write-JsonFile ([ordered]@{
            schema = 'myrd.m4.vm.case-result.v1'
            case_id = $caseName
            classification = 'PASS'
            source_commit = $commit
            artifacts = [ordered]@{ release_sha256 = $releaseHash; debug_sha256 = $debugHash }
            checkpoint_evidence = $checkpoint
            cleanup = [ordered]@{ zero_residuals = $true; processes = @(); drive_letters = @(); aim_devices = @(); owned_resources = @() }
        }) (Join-Path $caseDir.FullName 'result.json')
    }
    $driverDir = New-Item -ItemType Directory -Path (Join-Path $root 'driver-pre-experiment')
    Write-JsonFile ([ordered]@{
        schema = 'myrd.m4.vm.driver-pre-experiment.v1'
        classification = 'PASS'
        source_commit = $commit
        commands = @('sc.exe config phdskmnt start= demand', 'sc.exe start phdskmnt')
        adapter_query_available = $true
        recovery_discovered_adapter = $true
        restored_configuration = $true
    }) (Join-Path $driverDir.FullName 'result.json')

    $valid = Invoke-Verifier $root $commit $releaseHash $debugHash
    Require ($valid.Code -eq 0) "valid evidence failed: $($valid.Output)"
    Require ($valid.Output -match 'M4 VM EVIDENCE PASS') 'PASS marker missing'

    $casePath = Join-Path $root 'VM-3\result.json'
    $case = Get-Content -Raw -LiteralPath $casePath | ConvertFrom-Json
    $case.classification = 'PRODUCT_FAIL'
    Write-JsonFile $case $casePath
    Expect-Failure $root 'VM-3 classification is not PASS' $commit $releaseHash $debugHash
    $case.classification = 'PASS'
    Write-JsonFile $case $casePath

    Remove-Item -LiteralPath (Join-Path $root 'VM-5\result.json')
    Expect-Failure $root 'VM-5 result.json is missing' $commit $releaseHash $debugHash
    Write-JsonFile ([ordered]@{
        schema = 'myrd.m4.vm.case-result.v1'; case_id = 'VM-5'; classification = 'PASS'; source_commit = $commit
        artifacts = [ordered]@{ release_sha256 = $releaseHash; debug_sha256 = $debugHash }
        checkpoint_evidence = $checkpoint
        cleanup = [ordered]@{ zero_residuals = $true; processes = @(); drive_letters = @(); aim_devices = @(); owned_resources = @() }
    }) (Join-Path $root 'VM-5\result.json')

    [IO.File]::WriteAllBytes($releasePath, [byte[]](9, 9, 9, 9))
    Expect-Failure $root 'qualification.artifacts.release hash mismatch' $commit $releaseHash $debugHash
}
finally {
    if ([IO.Directory]::Exists($root)) { Remove-Item -LiteralPath $root -Recurse -Force }
}

Write-Output 'M4 evidence verifier tests PASS'
