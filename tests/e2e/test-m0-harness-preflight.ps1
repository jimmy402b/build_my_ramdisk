$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repoRoot

function require([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

function Invoke-ExpectedHarnessFailure([string]$ReportRoot,
                                       [string]$ExpectedMessage) {
    $caught = $null
    try {
        & .\tests\e2e\run-m0-driver-only.ps1 `
            -Iterations 1 -Configuration Debug -ReportRoot $ReportRoot `
            2>&1 | Out-Null
    } catch {
        $caught = $_
    }
    require ($null -ne $caught) 'harness unexpectedly succeeded'
    require ([string]$caught -match [regex]::Escape($ExpectedMessage)) `
        "unexpected harness failure: $caught"
}

# An existing junction below reports must be rejected before New-Item can
# create a child through it.
$reportsRoot = (Resolve-Path .\reports).Path
$testId = [guid]::NewGuid().ToString('N')
$outside = Join-Path $env:TEMP "myrd-m0-reparse-$testId"
$junction = Join-Path $reportsRoot "preflight-junction-$testId"
$outsideChild = Join-Path $outside 'must-not-be-created'
New-Item -ItemType Directory -Path $outside | Out-Null
New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
try {
    Invoke-ExpectedHarnessFailure `
        (Join-Path $junction 'must-not-be-created') 'reparse point'
    require (-not (Test-Path -LiteralPath $outsideChild)) `
        'harness wrote through a junction before rejecting ReportRoot'
} finally {
    require ([IO.Path]::GetFullPath($junction).StartsWith(
        $reportsRoot, [StringComparison]::OrdinalIgnoreCase)) `
        'unsafe junction cleanup target'
    require ([IO.Path]::GetFullPath($outside).StartsWith(
        [IO.Path]::GetFullPath($env:TEMP),
        [StringComparison]::OrdinalIgnoreCase)) `
        'unsafe temporary cleanup target'
    if (Test-Path -LiteralPath $junction) {
        Remove-Item -LiteralPath $junction -Force
    }
    if (Test-Path -LiteralPath $outside) {
        Remove-Item -LiteralPath $outside -Force
    }
}
Write-Output 'pre-create reparse rejection PASS'

# Keep one handle open while a child process exits owning the mutex. The next
# WaitOne reports abandonment but also grants ownership; the harness must
# continue and release that recovered ownership in its finally block.
$mutexName = 'Global\myrd-m0-e2e-mutex'
$keeper = New-Object Threading.Mutex($false, $mutexName)
$keeperOwned = $false
try {
    $childSource = @"
`$m = [Threading.Mutex]::OpenExisting('$mutexName')
`$null = `$m.WaitOne()
[Environment]::Exit(0)
"@
    $encoded = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($childSource))
    & powershell.exe -NoProfile -EncodedCommand $encoded
    require ($LASTEXITCODE -eq 0) 'mutex owner child failed'

    Invoke-ExpectedHarnessFailure $env:TEMP `
        'below the repository reports directory'

    try {
        $keeperOwned = $keeper.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $keeperOwned = $true
    }
    require $keeperOwned 'harness did not release recovered mutex ownership'
} finally {
    if ($keeperOwned) { $keeper.ReleaseMutex() }
    $keeper.Dispose()
}
Write-Output 'abandoned mutex recovery PASS'
