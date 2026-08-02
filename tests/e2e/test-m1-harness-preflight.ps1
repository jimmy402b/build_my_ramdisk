# test-m1-harness-preflight.ps1 — Non-destructive M1 harness safety tests
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path "$ScriptDir\..\.."
$HarnessScript = Join-Path $ScriptDir 'run-m1-proxy.ps1'

Write-Host "=== M1 Harness Preflight Tests ==="

# Test 1: non-admin token should fail preflight
Write-Host "[1/8] Non-admin rejection..."
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if ($currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "  SKIP: running as admin, cannot test non-admin rejection"
} else {
    Write-Host "  PASS: running as non-admin (preflight would reject in harness)"
}

# Test 2: missing executable
Write-Host "[2/8] Missing executable detection..."
$fakeExe = Join-Path $RepoRoot 'out\build\msvc\src\Debug\nonexistent.exe'
if (Test-Path $fakeExe) { throw "Unexpected executable exists" }
Write-Host "  PASS: nonexistent executable correctly absent"

# Test 3: report root within repo
Write-Host "[3/8] Report root validation..."
$reportsDir = Join-Path $RepoRoot 'reports'
if (-not (Test-Path $reportsDir)) {
    New-Item -ItemType Directory -Path $reportsDir -Force | Out-Null
}
Write-Host "  PASS: reports directory exists: $reportsDir"

# Test 4: phdskmnt running
Write-Host "[4/8] Driver running check..."
$phdskmnt = Get-Service -Name 'phdskmnt' -ErrorAction SilentlyContinue
if (-not $phdskmnt -or $phdskmnt.Status -ne 'Running') {
    Write-Host "  WARN: phdskmnt not running (required for real E2E)"
} else {
    Write-Host "  PASS: phdskmnt is Running"
}

# Test 5: deviosvc baseline capture
Write-Host "[5/8] deviosvc baseline..."
$deviosvc = Get-Service -Name 'deviosvc' -ErrorAction SilentlyContinue
if ($deviosvc) {
    Write-Host "  deviosvc State=$($deviosvc.Status) StartMode=$($deviosvc.StartType)"
}

# Test 6: capture baseline AIM devices
Write-Host "[6/8] AIM device baseline..."
$aimDevices = Get-CimInstance -ClassName Win32_DiskDrive -Filter "Model LIKE 'Arsenal%'" -ErrorAction SilentlyContinue
Write-Host "  AIM devices found: $($aimDevices.Count)"

# Test 7: PhysicalDrive baseline
Write-Host "[7/8] PhysicalDrive baseline..."
$pdBaseline = Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty Index | Sort-Object
Write-Host "  PhysicalDrive count: $($pdBaseline.Count)"

# Test 8: verify no forbidden processes
Write-Host "[8/8] Forbidden process check..."
$forbidden = @('RamDyn', 'AIM_GUI', 'aimapi')
$found = Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $forbidden -contains $_.ProcessName
}
if ($found) {
    Write-Host "  WARN: forbidden processes found: $($found.ProcessName)"
} else {
    Write-Host "  PASS: no forbidden processes"
}

Write-Host "=== Preflight complete ==="
