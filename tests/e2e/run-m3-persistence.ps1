[CmdletBinding()]
param(
    [ValidateSet('Preflight')][string]$Tier = 'Preflight',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$ScratchRoot,
    [Parameter(Mandatory = $true)][string]$ExpectedDiskIdentityJson,
    [string]$ExpectedDriverService = 'phdskmnt',
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'M3Harness.Common.ps1')

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$evidence = Get-M3FullPath $EvidenceDirectory
Assert-M3Condition ([IO.Directory]::Exists($evidence)) 'EvidenceDirectory must already exist'
Assert-M3NoReparsePath $evidence
Assert-M3Condition (-not (Test-M3PathUnderRoot $evidence $repoRoot -AllowRoot)) `
    'EvidenceDirectory cannot be the repository root'

$identity = Read-M3DiskIdentityJson $ExpectedDiskIdentityJson
$preflight = Invoke-M3Preflight `
    -Executable $Executable `
    -ScratchRoot $ScratchRoot `
    -WorkspaceRoot $repoRoot `
    -ExpectedDiskIdentity $identity `
    -ExpectedDriverService $ExpectedDriverService `
    -WhatIf:$WhatIf

$preflight | Add-Member -NotePropertyName operation -NotePropertyValue $Tier
$preflight | Add-Member -NotePropertyName configuration -NotePropertyValue $Configuration
$preflight | Add-Member -NotePropertyName source_commit -NotePropertyValue $null
try {
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -ne $git) {
        $preflight.source_commit = ((& git.exe -C $repoRoot rev-parse HEAD 2>$null) -join '').Trim()
    }
}
catch { $preflight.source_commit = $null }
$executableFull = [IO.Path]::GetFullPath($Executable)
$executableSha256 = if ([IO.File]::Exists($executableFull)) {
    (Get-FileHash -LiteralPath $executableFull -Algorithm SHA256).Hash
} else { $null }
$preflight | Add-Member -NotePropertyName executable -NotePropertyValue $executableFull
$preflight | Add-Member -NotePropertyName executable_sha256 -NotePropertyValue $executableSha256
$preflight | Add-Member -NotePropertyName started_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
$preflight | Add-Member -NotePropertyName finished_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))

$reportPath = Join-Path $evidence 'preflight.json'
Write-M3Json -InputObject $preflight -Path $reportPath
$summaryPath = Join-Path $evidence 'preflight.summary.txt'
$summary = @(
    "M3 $Tier $($preflight.status)"
    "WhatIf: $([bool]$WhatIf)"
    "Executable: $executableFull"
    "ScratchRoot: $([IO.Path]::GetFullPath($ScratchRoot))"
    "DiskIdentity: number=$($identity.Number), size=$($identity.SizeBytes), device=$($identity.DeviceId)"
    "Reason: $([string]$preflight.reason)"
)
[IO.File]::WriteAllLines((Get-M3FullPath $summaryPath), $summary, [Text.UTF8Encoding]::new($false))

Write-Output ($preflight | ConvertTo-Json -Depth 20 -Compress)
if ($preflight.status -eq 'PASS') {
    Write-Output "M3 $Tier WHATIF PASS: $reportPath"
    exit 0
}
Write-Output "M3 $Tier BLOCKED: $([string]$preflight.reason)"
exit 77
