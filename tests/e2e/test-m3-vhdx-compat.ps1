[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "M3-V5 executable is missing: $Executable"
}

$lines = @(& $Executable 2>&1)
$nativeExit = $LASTEXITCODE
$lines | ForEach-Object { Write-Output $_ }

$jsonLine = @($lines | Where-Object {
    $_ -is [string] -and $_.TrimStart().StartsWith('{')
} | Select-Object -Last 1)
if ($jsonLine.Count -ne 1) {
    throw 'M3-V5 did not emit exactly one final JSON report'
}

try {
    $report = $jsonLine[0] | ConvertFrom-Json -ErrorAction Stop
} catch {
    throw "M3-V5 emitted invalid JSON: $($_.Exception.Message)"
}

if ($report.schema -ne 'myrd.m3.vhdx_compat.v1') {
    throw "M3-V5 schema mismatch: $($report.schema)"
}
if (@('PASS', 'FAIL', 'BLOCKED') -notcontains $report.status) {
    throw "M3-V5 status is invalid: $($report.status)"
}

if ($nativeExit -eq 77) {
    if ($report.status -ne 'BLOCKED' -or
        [string]::IsNullOrWhiteSpace([string]$report.reason)) {
        throw 'M3-V5 exit 77 requires a machine-readable BLOCKED reason'
    }
    exit 77
}

if ($nativeExit -ne 0) {
    if ($report.status -ne 'FAIL') {
        throw "M3-V5 nonzero exit requires FAIL, got $($report.status)"
    }
    exit $nativeExit
}

if ($report.status -ne 'PASS') {
    throw "M3-V5 exit 0 requires PASS, got $($report.status)"
}
if (-not [bool]$report.elevated) {
    throw 'M3-V5 PASS requires an elevated native host'
}
if ([int]$report.aim_devices_online -ne 0) {
    throw 'M3-V5 PASS requires zero pre-existing AIM devices'
}
if (@($report.cases).Count -lt 5) {
    throw 'M3-V5 PASS is missing required compatibility cases'
}
foreach ($case in @($report.cases)) {
    if ($case.status -ne 'PASS') {
        throw "M3-V5 PASS contains a non-passing case: $($case.name)"
    }
    if (-not [bool]$case.cleanup_attempted -or
        -not [bool]$case.cleanup_succeeded -or
        [uint32]$case.cleanup_win32 -ne 0) {
        throw "M3-V5 case lacks successful finally-detach: $($case.name)"
    }
}

exit 0
