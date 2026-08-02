$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repoRoot

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

foreach ($path in @('tools\Invoke-M1Verification.ps1',
                     'tools\Verify-M1Json.ps1',
                     'tests\e2e\run-m1-proxy.ps1',
                     'tests\e2e\test-m1-harness-preflight.ps1')) {
    $tokens = $null
    $errors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path $path), [ref]$tokens, [ref]$errors) | Out-Null
    Require ($errors.Count -eq 0) `
        "$path has PowerShell parse errors: $($errors.Message -join '; ')"
}

$v0 = @(& .\tools\Invoke-M1Verification.ps1 -Tier M1-V0 -ListCommands)
$v2 = @(& .\tools\Invoke-M1Verification.ps1 -Tier M1-V2 -ListCommands)
$v6 = @(& .\tools\Invoke-M1Verification.ps1 -Tier M1-V6 -ListCommands)
Require (($v0 -join "`n") -match 'M1-V0 pinned AIM provenance') `
    'M1-V0 command listing is incomplete'
Require (($v2 -join "`n") -match 'exactly 50 times') `
    'M1-V2 does not list the 50x M1 SHM gate'
Require (($v2 -join "`n") -notmatch 'all unit.*50') `
    'M1-V2 still repeats the entire slow unit suite'
Require (($v6 -join "`n") -match 'exactly 20 Release lifecycles') `
    'M1-V6 command listing is incomplete'
Require (($v6 -join "`n") -notmatch 'placeholder') `
    'verification tiers still contain placeholder gates'

$text = Get-Content -Raw .\tools\Invoke-M1Verification.ps1
foreach ($point in @('provider_ready','create_returned','device_confirmed',
                      'physical_drive_confirmed','write_verified',
                      'zero_verified','unmap_verified','device_removed')) {
    Require ($text -match [regex]::Escape($point)) `
        "M1-V5 omits failure point $point"
}

$jsonVerifier = Get-Content -Raw .\tools\Verify-M1Json.ps1
foreach ($field in @('file_version','authenticode_status','signer_subject',
                      'imported_modules','query_version_raw')) {
    Require ($jsonVerifier -match "'$field'") `
        "M1 JSON verifier omits driver evidence field $field"
}
Require ($jsonVerifier -match
    '\$report\.driver\.file_version\s+-match') `
    'M1 JSON verifier does not reject an empty driver file version'
Require ($jsonVerifier -match
    '\$report\.driver\.authenticode_status\s+-match') `
    'M1 JSON verifier does not reject an empty Authenticode status'

$mainText = Get-Content -Raw .\src\m1\main.cpp
Require ($mainText -match 'GetFileVersionInfoSizeW') `
    'M1 executable does not collect the driver file version'
Require ($mainText -match 'L"devio\.dll"') `
    'M1 runtime module audit omits devio.dll'

$auditText = Get-Content -Raw .\tools\Audit-M0Dependencies.ps1
foreach ($dependency in @('aimapi.dll','devio.dll','aim_ll.exe','AIMtk.exe')) {
    Require ($auditText -match [regex]::Escape($dependency)) `
        "Release import audit omits $dependency"
}
Write-Output 'M1 verification script tests PASS'
