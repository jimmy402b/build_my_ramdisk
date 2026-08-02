param(
    [ValidateSet('M1-V0','M1-V1','M1-V2','M1-V3','M1-V4','M1-V5','M1-V6')]
    [string]$Tier = 'M1-V2',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [switch]$ListCommands
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot

function Step([string]$Name, [scriptblock]$Action) {
    if ($ListCommands) {
        Write-Output "[$Tier] $Name"
        return
    }
    Write-Output "--- $Name ---"
    & $Action
    Write-Output "--- $Name PASS ---"
}
function Invoke-Native([scriptblock]$Action, [string]$Label) {
    & $Action
    if ($LASTEXITCODE -ne 0) { throw "$Label failed (exit=$LASTEXITCODE)" }
}

function Assert-TestSelection([string]$Pattern, [int]$Minimum) {
    $listing = @(& ctest --test-dir out/build/msvc -C $Configuration `
        -N -R $Pattern 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "CTest discovery failed: $Pattern" }
    $line = @($listing | Where-Object { $_ -match '^Total Tests:\s+(\d+)$' })
    Require ($line.Count -eq 1) "cannot parse CTest count for $Pattern"
    $count = [int]([regex]::Match([string]$line[0], '(\d+)$').Groups[1].Value)
    Require ($count -ge $Minimum) `
        "CTest selected $count test(s) for $Pattern; expected at least $Minimum"
}

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

if (-not $ListCommands) {
    . "$PSScriptRoot\Enter-MsvcEnvironment.ps1"
}

$v0 = {
    Step 'M1-V0 pinned AIM provenance' {
        $expected = 'd379caa027d21902e45fce8c9daf02fd116c2dda'
        $actual = (& git -C out/reference/aim rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or $actual -ne $expected) {
            throw "AIM source mismatch: expected=$expected actual=$actual"
        }
        $contract = Get-Content -Raw docs/protocol/aim-m0-contract.md
        Require ($contract -match 'd379caa') 'protocol contract lacks pinned commit'
    }
    Step 'M1-V0 M0 accepted ancestor and clean diff syntax' {
        & git merge-base --is-ancestor 0e2b3b3 HEAD
        if ($LASTEXITCODE -ne 0) { throw 'accepted M0 commit is not an ancestor' }
        Invoke-Native { git diff --check } 'git diff --check'
    }
}

$v1 = {
    & $v0
    Step "M1-V1 configure and build $Configuration" {
        Invoke-Native { cmake --preset msvc } 'CMake configure'
        Invoke-Native { cmake --build --preset $Configuration.ToLower() } `
            'M1 build'
    }
    Step 'M1-V1 all unit.m1 tests' {
        Assert-TestSelection '^unit\.m1\.' 12
        Invoke-Native { ctest --test-dir out/build/msvc -C $Configuration `
            -R '^unit\.m1\.' --output-on-failure } 'M1 unit tests'
    }
}

$v2 = {
    & $v1
    Step 'M1-V2 deterministic 10000-case protocol fuzz' {
        Assert-TestSelection '^unit\.m1\.protocol_fuzz$' 1
        Invoke-Native { ctest --test-dir out/build/msvc -C $Configuration `
            -R '^unit\.m1\.protocol_fuzz$' --output-on-failure } 'M1 fuzz'
    }
    Step 'M1-V2 local M1 SHM integration exactly 50 times' {
        Assert-TestSelection '^integration\.m1\.shm$' 1
        for ($index = 1; $index -le 50; $index++) {
            & ctest --test-dir out/build/msvc -C $Configuration `
                -R '^integration\.m1\.shm$' --output-on-failure | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "M1 SHM repetition $index failed"
            }
        }
    }
}

$v3 = {
    & $v2
    Step 'M1-V3 Release dependency audit' {
        Invoke-Native { cmake --build --preset release --target `
            myrd-m1-e2e myrd-m0-poc } 'Release verifier build'
        & "$PSScriptRoot\Audit-M0Dependencies.ps1" `
            -Executable 'out\build\msvc\src\Release\myrd-m1-e2e.exe'
    }
    Step 'M1-V3 M0 direct read-only driver query' {
        $path = Join-Path $repoRoot (
            'reports\m1-v3-query-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fffffff') +
            '.json')
        & 'out\build\msvc\src\Release\myrd-m0-poc.exe' `
            query-driver --report $path | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'M0 read-only driver query failed' }
        & "$PSScriptRoot\Verify-M0Json.ps1" -Path $path | Out-Null
    }
}

$v4 = {
    & $v3
    Step 'M1-V4 fresh Debug build and harness preflight tests' {
        Invoke-Native { cmake --build --preset debug --target `
            myrd-m1-e2e myrd-m0-poc } 'Debug verifier build'
        & "$repoRoot\tests\e2e\test-m1-harness-preflight.ps1"
    }
    Step 'M1-V4 one real Debug lifecycle' {
        & "$repoRoot\tests\e2e\run-m1-proxy.ps1" -Iterations 1 `
            -Configuration Debug -ReportRoot 'reports\m1-v4'
    }
}

$v5 = {
    & $v4
    foreach ($point in @('provider_ready','create_returned','device_confirmed',
                          'physical_drive_confirmed','write_verified',
                          'zero_verified','unmap_verified','device_removed')) {
        Step "M1-V5 failure injection $point" {
            & "$repoRoot\tests\e2e\run-m1-proxy.ps1" -Iterations 1 `
                -Configuration Debug -FailurePoint $point `
                -ReportRoot "reports\m1-v5-$point"
        }
    }
    Step 'M1-V5 normal lifecycle after all injected failures' {
        & "$repoRoot\tests\e2e\run-m1-proxy.ps1" -Iterations 1 `
            -Configuration Debug -ReportRoot 'reports\m1-v5-normal'
    }
}

$v6 = {
    & $v5
    Step 'M1-V6 pinned Release executable and import audit' {
        Invoke-Native { cmake --build --preset release --target `
            myrd-m1-e2e myrd-m0-poc } 'Release verifier build'
        & "$PSScriptRoot\Audit-M0Dependencies.ps1" `
            -Executable 'out\build\msvc\src\Release\myrd-m1-e2e.exe'
    }
    Step 'M1-V6 exactly 20 Release lifecycles' {
        $directory = Join-Path $repoRoot 'reports\m1-v6'
        $before = @(Get-ChildItem -LiteralPath $directory `
            -Filter 'summary-*.json' -ErrorAction SilentlyContinue |
            ForEach-Object FullName)
        & "$repoRoot\tests\e2e\run-m1-proxy.ps1" -Iterations 20 `
            -Configuration Release -ReportRoot $directory
        $after = @(Get-ChildItem -LiteralPath $directory `
            -Filter 'summary-*.json' | Where-Object {
                $before -notcontains $_.FullName
            })
        Require ($after.Count -eq 1) `
            "expected exactly one new M1-V6 aggregate; found $($after.Count)"
        $summary = Get-Content -Raw -LiteralPath $after[0].FullName |
            ConvertFrom-Json
        Require ($summary.attempted -eq 20 -and $summary.passed -eq 20) `
            'M1-V6 aggregate is not 20/20'
        Require ($summary.executable_sha256 -match '^[0-9A-F]{64}$') `
            'M1-V6 executable hash is invalid'
        Require $summary.service_state_restored `
            'M1-V6 deviosvc state was not restored'
        Require $summary.service_start_mode_unchanged `
            'M1-V6 deviosvc StartMode changed'
    }
}

try {
    if ($ListCommands) {
        Write-Output "=== M1 Verification Commands for $Tier ==="
    }
    switch ($Tier) {
        'M1-V0' { & $v0 }
        'M1-V1' { & $v1 }
        'M1-V2' { & $v2 }
        'M1-V3' { & $v3 }
        'M1-V4' { & $v4 }
        'M1-V5' { & $v5 }
        'M1-V6' { & $v6 }
    }
    if ($ListCommands) {
        Write-Output '=== End of commands ==='
    } else {
        Write-Output "$Tier PASS"
    }
} finally {
    Pop-Location
}
