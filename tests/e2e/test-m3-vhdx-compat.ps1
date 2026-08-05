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

$jsonLines = @($lines | Where-Object {
    $_ -is [string] -and $_.TrimStart().StartsWith('{')
})
if ($jsonLines.Count -ne 1) {
    throw 'M3-V5 did not emit exactly one final JSON report'
}
$jsonLine = $jsonLines[0]

try {
    $report = $jsonLine | ConvertFrom-Json -ErrorAction Stop
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
if (-not [bool]$report.sandbox_cleanup_attempted -or
    -not [bool]$report.sandbox_cleanup_succeeded -or
    [uint32]$report.sandbox_cleanup_win32 -ne 0) {
    throw 'M3-V5 PASS requires successful compatibility sandbox cleanup'
}

$expectedCases = [ordered]@{
    empty512 = @{
        bytes = 4096
        sha256 = 'AD7FACB2586FC6E966C004D7D1D16B024F5805FF7CB47C7A85DABD8B48892CA7'
    }
    sparse512 = @{
        bytes = 4096
        sha256 = 'A011CA1DE9C53CE0D06A36BC20816677EF3B3701706D59D0F5601DDD409CB9A9'
    }
    boundary512 = @{
        bytes = 4096
        sha256 = 'AE3A920ECDAF0E25AB84AA88901A37FD65B10564D1F9ED402CE66850E86B90A2'
    }
    sector4096 = @{
        bytes = 4096
        sha256 = '686145051D9C823F0F1DB71E7C3581C60DF80EFF61FE60C7FAEEEFFECAFB42D1'
    }
    windows_native_reader = @{
        bytes = 65536
        sha256 = '5D649E6DEFAD516DF94ABF5B3943591FA31A96C970F6D253246C5F2CB90C7344'
    }
}
$actualCases = @($report.cases)
if ($actualCases.Count -ne $expectedCases.Count) {
    throw 'M3-V5 PASS must contain exactly the five required cases'
}
$actualNames = @($actualCases | ForEach-Object { [string]$_.name })
if (@($actualNames | Sort-Object -Unique).Count -ne $actualNames.Count) {
    throw 'M3-V5 PASS contains duplicate case names'
}
foreach ($caseName in $expectedCases.Keys) {
    $case = @($actualCases | Where-Object { $_.name -eq $caseName })
    if ($case.Count -ne 1) {
        throw "M3-V5 PASS is missing required case: $caseName"
    }
    $case = $case[0]
    if ($case.status -ne 'PASS') {
        throw "M3-V5 PASS contains a non-passing case: $($case.name)"
    }
    if (-not [bool]$case.cleanup_attempted -or
        -not [bool]$case.cleanup_succeeded -or
        [uint32]$case.cleanup_win32 -ne 0) {
        throw "M3-V5 case lacks successful finally-detach: $($case.name)"
    }
    if ([uint64]$case.data_bytes -ne
        [uint64]$expectedCases[$caseName].bytes -or
        [string]$case.data_sha256 -cne
        [string]$expectedCases[$caseName].sha256) {
        throw "M3-V5 sample evidence mismatch: $($case.name)"
    }
}

exit 0
