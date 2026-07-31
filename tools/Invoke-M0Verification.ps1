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

. "$PSScriptRoot\Enter-MsvcEnvironment.ps1"

# Build V0 commands
$v0 = {
    step "V0: source commit" { git -C out/reference/aim rev-parse HEAD }
    step "V0: protocol contract" {
        $t = Get-Content docs/protocol/aim-m0-contract.md -Raw
        if ($t -notmatch 'd379caa') { throw 'commit missing' }
    }
    step "V0: git diff check" { git diff --check }
}

# Build V1 commands
$v1 = {
    & $v0
    step "V1: configure" { cmake --preset msvc }
    step "V1: build $Configuration" { cmake --build --preset $Configuration.ToLower() }
    step "V1: unit tests" { ctest --preset $Configuration.ToLower() -R '^unit\.' }
}

# Build V2 commands
$v2 = {
    & $v1
    step "V2: integration SHM 50x" {
        1..50 | ForEach-Object {
            ctest --preset $Configuration.ToLower() -R '^integration\.shm$' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "iteration $_ failed" }
        }
    }
}

# V3 placeholder
$v3 = {
    & $v2
    step "V3: dependency audit" { & "$PSScriptRoot\Audit-M0Dependencies.ps1" -Executable "out/build/msvc/src/$Configuration/myrd-m0-poc.exe" }
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
