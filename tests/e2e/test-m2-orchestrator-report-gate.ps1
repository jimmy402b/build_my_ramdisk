[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Common,
    [Parameter(Mandatory = $true)][string]$Verifier
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $Common

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Require-Failure([scriptblock]$Action, [string]$Expected) {
    try {
        & $Action
    }
    catch {
        Require ($_.Exception.Message -match [regex]::Escape($Expected)) "unexpected failure: $($_.Exception.Message)"
        return
    }
    throw 'expected report gate failure did not occur'
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m2-report-gate-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $output = @(& $Executable --local-fake --evidence-dir $root 2>&1)
    Require ($LASTEXITCODE -eq 0) "local fake failed: $($output -join [Environment]::NewLine)"

    $accepted = Confirm-M2PublishedReport -RunDirectory $root -Tier M2-V4 -Executable $Executable -Verifier $Verifier
    Require ([IO.File]::Exists($accepted)) 'accepted report path is missing'
    Require ([IO.File]::Exists((Join-Path $root 'verification.log'))) 'independent verification log is missing'

    $extra = Join-Path $root 'm2-report-extra.json'
    Copy-Item -LiteralPath $accepted -Destination $extra
    Require-Failure {
        Confirm-M2PublishedReport -RunDirectory $root -Tier M2-V4 -Executable $Executable -Verifier $Verifier
    } 'expected exactly one immutable M2 report'
    Remove-Item -LiteralPath $extra -Force

    $tampered = Get-Content -Raw -LiteralPath $accepted -Encoding utf8 | ConvertFrom-Json
    $tampered.io.bytes_written = 1
    $tampered | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $accepted -Encoding utf8
    Require-Failure {
        Confirm-M2PublishedReport -RunDirectory $root -Tier M2-V4 -Executable $Executable -Verifier $Verifier
    } 'independent M2 report verification failed'
}
finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}

Write-Output 'M2 orchestrator report gate test PASS'
