param(
    [ValidateSet('V0','V1','V2','V3','V4','V5','V6')][string]$Tier = 'V2',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [switch]$ListCommands
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

function step($name, $script) {
    if ($ListCommands) { Write-Output "  [$Tier] $name"; return }
    Write-Output "--- $name ---"
    & $script
    if ($LASTEXITCODE -ne 0) { throw "$name FAILED (exit $LASTEXITCODE)" }
    Write-Output "--- $name PASS ---"
}

function assertTestSelection([string]$preset, [string]$pattern,
                             [int]$expectedMinimum) {
    $listing = @(& ctest --preset $preset -N -R $pattern 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "CTest discovery failed for $pattern (exit $LASTEXITCODE)"
    }
    $totalLine = @($listing | Where-Object {
        $_ -match '^Total Tests:\s+(\d+)$'
    })
    if ($totalLine.Count -ne 1) {
        throw "Could not determine CTest selection count for $pattern"
    }
    $count = [int]([regex]::Match(
        [string]$totalLine[0], '(\d+)$').Groups[1].Value)
    if ($count -lt $expectedMinimum) {
        throw "CTest selection for $pattern found $count test(s), expected at least $expectedMinimum"
    }
}

. "$PSScriptRoot\Enter-MsvcEnvironment.ps1"

# V0: documentation and protocol checks
$v0 = {
    step "V0: source commit" {
        $expectedCommit = 'd379caa027d21902e45fce8c9daf02fd116c2dda'
        $actualCommit = (& git -C out/reference/aim rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0) {
            throw "Could not read AIM source commit (exit $LASTEXITCODE)"
        }
        if ($actualCommit -ne $expectedCommit) {
            throw "AIM source commit mismatch: expected $expectedCommit, got $actualCommit"
        }
        Write-Output $actualCommit
    }
    step "V0: protocol contract" {
        $t = Get-Content docs/protocol/aim-m0-contract.md -Raw
        if ($t -notmatch 'd379caa') { throw 'commit missing' }
    }
    step "V0: git diff check" { git diff --check }
}

# V1: build + unit tests
$v1 = {
    & $v0
    step "V1: configure" { cmake --preset msvc }
    step "V1: build $Configuration" { cmake --build --preset $Configuration.ToLower() }
    step "V1: unit tests" {
        $preset = $Configuration.ToLower()
        assertTestSelection $preset '^unit\.' 1
        ctest --preset $preset -R '^unit\.'
    }
}

# V2: local integration
$v2 = {
    & $v1
    step "V2: integration SHM 50x" {
        $preset = $Configuration.ToLower()
        assertTestSelection $preset '^integration\.shm$' 1
        1..50 | ForEach-Object {
            ctest --preset $preset -R '^integration\.shm$' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "iteration $_ failed" }
        }
    }
}

# V3: read-only driver gate
$v3 = {
    & $v2
    $exe = "out/build/msvc/src/$Configuration/myrd-m0-poc.exe"
    step "V3: dependency audit" { & "$PSScriptRoot\Audit-M0Dependencies.ps1" -Executable $exe }
    step "V3: query-driver" {
        $reportPath = "reports/m0-v3-verify-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".json"
        New-Item -ItemType Directory -Path reports -Force | Out-Null
        & $exe query-driver --report $reportPath
        if ($LASTEXITCODE -ne 0) { throw "query-driver failed (exit $LASTEXITCODE)" }
        & "$PSScriptRoot\Verify-M0Json.ps1" -Path $reportPath
    }
}

# V4: single normal E2E lifecycle (Debug)
$v4 = {
    & $v3
    step "V4: fresh Debug build and tests" {
        cmake --build --preset debug
        if ($LASTEXITCODE -ne 0) { throw 'Debug build failed' }
        assertTestSelection 'debug' '^(unit\.|integration\.shm$)' 11
        ctest --preset debug
    }
    step "V4: single Debug E2E lifecycle" {
        & "$root\tests\e2e\run-m0-driver-only.ps1" `
            -Iterations 1 -Configuration Debug -ReportRoot reports/m0-v4
    }
}

# V5: fault injection + normal run
$v5 = {
    & $v4
    $faultPoints = @('create_returned','device_confirmed',
                      'physical_drive_confirmed','round_trip_verified')
    foreach ($fp in $faultPoints) {
        step "V5: fault injection $fp" {
            & "$root\tests\e2e\run-m0-driver-only.ps1" `
                -Iterations 1 -Configuration Debug `
                -FailurePoint $fp -ReportRoot "reports/m0-v5-$fp"
        }
    }
    step "V5: normal run after faults" {
        & "$root\tests\e2e\run-m0-driver-only.ps1" `
            -Iterations 1 -Configuration Debug -ReportRoot reports/m0-v5-normal
    }
}

# V6: Release dependency audit + 20 cycles
$v6 = {
    & $v5
    $exe = "out/build/msvc/src/Release/myrd-m0-poc.exe"
    step "V6: Release import audit" {
        cmake --build --preset release --target myrd-m0-poc
        & "$PSScriptRoot\Audit-M0Dependencies.ps1" -Executable $exe
    }
    step "V6: 20 Release E2E cycles" {
        $existingSummaries = @(Get-ChildItem reports/m0-v6 `
            -Filter 'summary-*.json' -ErrorAction SilentlyContinue |
            ForEach-Object FullName)
        & "$root\tests\e2e\run-m0-driver-only.ps1" `
            -Iterations 20 -Configuration Release -ReportRoot reports/m0-v6
        if ($LASTEXITCODE -ne 0) { throw 'V6 E2E harness failed' }
        $newSummaries = @(Get-ChildItem reports/m0-v6 `
            -Filter 'summary-*.json' |
            Where-Object { $existingSummaries -notcontains $_.FullName })
        if ($newSummaries.Count -ne 1) {
            throw "Expected one new V6 summary, found $($newSummaries.Count)"
        }
        & "$PSScriptRoot\Assert-M0V6Aggregate.ps1" `
            -SummaryPath $newSummaries[0].FullName -ExpectedIterations 20
    }
}

if ($ListCommands) {
    Write-Output "=== M0 Verification Commands for Tier $Tier ==="
    & (Get-Variable -Name $Tier.ToLower() -ValueOnly)
    Write-Output "=== End of commands ==="
    exit 0
}

& (Get-Variable -Name $Tier.ToLower() -ValueOnly)
Write-Output "$Tier PASS"
Pop-Location
